; fbdemo.asm - A1：**用户态绘图演示**（真实 ring3 程序自己把画面画到屏幕上）
;
; 构建方式（见 build64.sh）：
;   nasm -f bin user/fbdemo.asm -> build64/user_fbdemo64.bin
;   -> objcopy 变 elf64 目标文件嵌进内核（符号 _binary_build64_user_fbdemo64_bin_*）
;   -> kernel/kernel64.cpp 的 os_boot_path 调 user64_run_fbdemo64()：blob 拷到用户代码页
;      （VA 4GiB，**只读**）后 iretq 进 ring3 跑。平铺二进制、org 0：只做 RIP 相对寻址 +
;      用用户栈（可写），不依赖加载地址。
;
; 与内核的接口 = int 0x80（Vimtu64 自有 ABI，见 kernel/syscall64.h）：
;   rax=9  fb_map(out_va**, out_info**)     rdi / rsi           返回 0 = 成功（负 = 错误码）
;   rax=10 fb_flip(x, y, w, h)              rdi / rsi / rdx / r10
;                                          返回 0 = 已提交（含夹取）/ 1 = 完全越界被拒
;   rax=11 fb_present()                                         整屏提交
;   rax=1  write(1, buf, len) / 2 exit(code) / 3 getpid() / 5 sleep_ms(ms)
;
; 本程序做的事（自动验收 tests/fbmap64_test.py 的判据）：
;   1) fb_map：拿后备缓冲的用户 VA + 几何（宽/高/pitch/格式）——**显存由内核映射，程序自己画**；
;   2) 每帧往"横贯全宽的色带"里画：渐变底色 + 从左到右移动的实心矩形 + 固定位置的状态像素条，
;      再 fb_flip(0, band_y, width, band_h) **局部提交**到屏幕；
;   3) 第 3 帧故意喂一次**完全越界**的 flip（x = width+100）→ 内核拒绝并打点，本程序继续跑；
;      第 4 帧喂一次**部分越界**的 flip（x = -40）→ 内核夹取后提交，程序继续跑；
;   4) 每帧打印 [FBDEMO] frame=.. flip=.. va=0x.. pid=..，跑满 FRAMES 帧后 exit(0)。
;
; ★ 帧数/帧时长别随便拉长：本演示跑在**进桌面之前**的启动路径上，演示越久桌面前面的垃圾时间越长。
;   实测（TCG、宿主机还有别的 QEMU 在跑时）：12 帧 × 1600ms 会把 desktop64_test 的看门狗顶爆
;   （[WD64] watchdog fire stale>5000ms -> [PANIC64] stop=WATCHDOG_TIMEOUT，而 desktop64_test
;   断言"不得出现 PANIC"）；5 帧 × 700ms（≈2s）时 desktop64_test 与 fbmap64_test 都稳定绿。
;   验收测试（tests/fbmap64_test.py）按 [FBDEMO] frame= 同步抓屏 —— 抓屏期间 guest 基本停住，
;   所以**短演示也够抓够 3 帧**（实测 5 帧全抓到）。
;
; ★ 打印方式：整行先在栈上拼好再**一次** write(1,...) —— 每个 write 都会让内核打一行
;   `[SYSCALL] nr=1 ...`，逐段打印会把证据行切断（本文件早期版本踩过）。
; ★ 寄存器约定：r12=VA、r13d=width、r14d=band_y、r15d=band_h、rbp=width/8、rbx=帧号；
;   r8..r11/rax/rcx/rdx/rsi/rdi 是临时（打印助手也会用 r9，所以跨调用要存栈）。
;
; 打点格式（验收 grep，勿改）：
;   [FBDEMO] map va=0x.. w=.. h=.. pitch=.. fmt=..
;   [FBDEMO] frame=<n> flip=<ret> va=0x.. pid=<n>
;   [FBDEMO] oob ret=<ret>        （越界 flip 的返回码，1 = 被拒）
;   [FBDEMO] clamp ret=<ret>      （部分越界 flip 的返回码，0 = 已夹取提交）
;   [FBDEMO] done frames=<n>
;   [FBDEMO] fb_map FAILED ret=<负错误码>
bits 64
org 0

%define NR_WRITE    1
%define NR_EXIT     2
%define NR_GETPID   3
%define NR_SLEEP_MS 5
%define NR_FB_MAP   9
%define NR_FB_FLIP  10
%define NR_FB_PRESENT 11

%define FRAMES      5               ; 总帧数（每帧 FRAME_MS 请求值；别拉长：启动期演示会推迟桌面，见文件头说明）
%define FRAME_MS    700             ; 单帧停留（实测 ~0.45s；验收测试按帧抓屏）
%define RECT_W      240             ; 移动矩形宽
%define RECT_H      40              ; 移动矩形高

; 用户栈上的临时区（栈顶 16 字节对齐；进来先 sub rsp,ST_ALLOC 保持对齐）
%define ST_OUTVA    0               ; 8B：fb_map 的 out_va
%define ST_INFO     8               ; 32B：Fb64Info（kernel/syscall64.h：w/h/pitch/fmt/size/va）
%define ST_LINE     40              ; 128B：整行输出缓冲（拼好一次 write 出去）
%define ST_RET      168             ; 8B：本帧 fb_flip 的返回码（打印助手会用 r9，不能拿 r9 跨调用）
%define ST_ALLOC    176             ; 40 + 128 + 8 = 176（16 的整数倍）

_start:
    sub     rsp, ST_ALLOC

    ; ---- 1) fb_map(&out_va, &info)：内核把后备缓冲映射进本进程，返回用户 VA + 几何 ----
    mov     eax, NR_FB_MAP
    lea     rdi, [rsp + ST_OUTVA]
    lea     rsi, [rsp + ST_INFO]
    int     0x80
    test    rax, rax
    js      .map_failed

    mov     r12, [rsp + ST_OUTVA]        ; r12 = 后备缓冲用户 VA
    mov     r13d, [rsp + ST_INFO + 0]    ; r13d = width（后面一直用它算行偏移）
    mov     r14d, [rsp + ST_INFO + 4]    ; r14d = height（打印完换成 band_y）
    mov     r15d, [rsp + ST_INFO + 8]    ; r15d = pitch（打印完换成 band_h）
    mov     ebp, [rsp + ST_INFO + 12]    ; ebp = format（打印完换成 width/8）

    ; ---- 打印 [FBDEMO] map va=0x.. w=.. h=.. pitch=.. fmt=.. ----
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_map]
    mov     ecx, msg_map_len
    call    append_str
    mov     rax, r12
    call    append_hex
    lea     rsi, [rel msg_w]
    mov     ecx, msg_w_len
    call    append_str
    mov     eax, r13d
    call    append_dec
    lea     rsi, [rel msg_h]
    mov     ecx, msg_h_len
    call    append_str
    mov     eax, r14d
    call    append_dec
    lea     rsi, [rel msg_pitch]
    mov     ecx, msg_pitch_len
    call    append_str
    mov     eax, r15d
    call    append_dec
    lea     rsi, [rel msg_fmt]
    mov     ecx, msg_fmt_len
    call    append_str
    mov     eax, ebp
    call    append_dec
    call    flush_line

    ; ---- 色带几何：y = height/5，高 = height/6；rbp = width/8（背景色带的段数）----
    mov     eax, r14d
    xor     edx, edx
    mov     ecx, 5
    div     ecx
    mov     r14d, eax                    ; r14d = band_y
    mov     eax, [rsp + ST_INFO + 4]     ; height
    xor     edx, edx
    mov     ecx, 6
    div     ecx
    mov     r15d, eax                    ; r15d = band_h
    mov     eax, r13d
    xor     edx, edx
    mov     ecx, 8
    div     ecx
    mov     ebp, eax                     ; rbp = width/8

    xor     ebx, ebx                     ; rbx = 帧号

.frame_loop:
    ; ================= 画一帧（全部由用户程序写后备缓冲）=================
    ; ---- (a) 背景：整条色带按 8 像素一段填渐变（段的颜色随帧平移 17）----
    mov     r10d, r14d                   ; y = band_y
    mov     r8d, r15d                    ; rows = band_h
.bg_row:
    mov     eax, r10d
    imul    eax, r13d                    ; y * width
    shl     eax, 2                       ; * 4 = y * pitch
    lea     rdi, [r12 + rax]
    xor     r9d, r9d                     ; 段号 g
.bg_col:
    mov     eax, r9d
    shl     eax, 3                       ; g*8 = x
    mov     ecx, ebx
    imul    ecx, 17                      ; frame*17
    add     eax, ecx
    and     eax, 0xFF                    ; t = (x + frame*17) & 0xFF
    ; color = 0xFF000000 | (t<<16) | ((t^0x55)<<8) | (255-t)
    mov     edx, eax
    shl     edx, 16
    or      edx, 0xFF000000
    mov     ecx, eax
    xor     ecx, 0x55
    shl     ecx, 8
    or      edx, ecx
    mov     ecx, 255
    sub     ecx, eax
    or      edx, ecx
    mov     eax, edx                     ; rep stosd 用 eax = 颜色
    mov     ecx, 8                       ; 8 像素一段
    rep stosd
    inc     r9d
    cmp     r9d, ebp
    jb      .bg_col
    inc     r10d
    dec     r8d
    jnz     .bg_row

    ; ---- (b) 移动矩形：240x40，x = frame*(width-240)/(FRAMES-1)，y = band_y + 16 ----
    mov     eax, r13d
    sub     eax, RECT_W
    imul    eax, ebx
    xor     edx, edx
    mov     ecx, FRAMES - 1
    div     ecx
    mov     r9d, eax                     ; rx
    ; 颜色随帧变：R=(frame*29+80)&0xFF、G=(200-frame*11)&0xFF、B=(frame*53)&0xFF
    mov     eax, ebx
    imul    eax, 29
    add     eax, 80
    and     eax, 0xFF
    shl     eax, 16
    or      eax, 0xFF000000
    mov     ecx, ebx
    imul    ecx, 11
    mov     edx, 200
    sub     edx, ecx
    and     edx, 0xFF
    shl     edx, 8
    or      eax, edx
    mov     ecx, ebx
    imul    ecx, 53
    and     ecx, 0xFF
    or      eax, ecx
    mov     r11d, eax                    ; 矩形颜色
    mov     r10d, r14d
    add     r10d, 16                     ; y = band_y + 16
    mov     r8d, RECT_H
.rect_row:
    mov     eax, r10d
    imul    eax, r13d
    shl     eax, 2
    mov     ecx, r9d
    shl     ecx, 2                       ; rx*4
    add     eax, ecx
    lea     rdi, [r12 + rax]
    mov     ecx, RECT_W
    mov     eax, r11d
    rep stosd
    inc     r10d
    dec     r8d
    jnz     .rect_row

    ; ---- (c) 状态像素条：固定位置，宽度 = (frame+1)*(width/10)，高 10 ----
    mov     eax, r13d
    xor     edx, edx
    mov     ecx, 10
    div     ecx
    mov     ecx, ebx
    inc     ecx
    imul    eax, ecx
    mov     r11d, eax                    ; 条宽
    mov     r10d, r14d
    add     r10d, r15d
    sub     r10d, 12                     ; y = band_y + band_h - 12
    mov     r8d, 10
.bar_row:
    mov     eax, r10d
    imul    eax, r13d
    shl     eax, 2
    add     eax, 32                      ; x = 8（字节偏移 = 8*4）
    lea     rdi, [r12 + rax]
    mov     ecx, r11d
    mov     eax, 0xFFFFFFFF
    rep stosd
    inc     r10d
    dec     r8d
    jnz     .bar_row

    ; ================= 提交区域（fb_flip：只有这一块上屏）=================
    mov     eax, NR_FB_FLIP
    xor     edi, edi                     ; x = 0
    mov     esi, r14d                    ; y = band_y
    mov     edx, r13d                    ; w = width
    mov     r10d, r15d                   ; h = band_h
    int     0x80
    mov     [rsp + ST_RET], rax          ; 0 = 已提交（含夹取）；1 = 完全越界被拒

    ; ---- 第 3 帧：故意喂**完全越界**的区域（内核必须拒绝且不崩）----
    cmp     ebx, 2
    jne     .no_oob
    mov     eax, NR_FB_FLIP
    mov     edi, r13d
    add     edi, 100                     ; x = width + 100（完全在屏幕右边外）
    mov     esi, r14d                    ; y = band_y（在屏幕内，但 x 已越界）
    mov     edx, 32
    mov     r10d, 32
    int     0x80                         ; 期望 rax = 1
    mov     r11, rax
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_oob]
    mov     ecx, msg_oob_len
    call    append_str
    mov     rax, r11
    call    append_dec
    call    flush_line
.no_oob:

    ; ---- 第 4 帧：**部分越界**（x = -40）——内核夹取后提交，返回 0 ----
    cmp     ebx, 3
    jne     .no_clamp
    mov     eax, NR_FB_FLIP
    mov     rdi, -40                     ; x = -40（64 位负数：不能用 mov edi，会零扩展）
    mov     esi, r14d
    add     esi, 8                       ; y = band_y + 8（竖向保持在色带内）
    mov     edx, 80
    mov     r10d, RECT_H
    int     0x80                         ; 期望 rax = 0（夹取后提交）
    mov     r11, rax
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_clamp]
    mov     ecx, msg_clamp_len
    call    append_str
    mov     rax, r11
    call    append_dec
    call    flush_line
.no_clamp:

    ; ================= 打印本帧状态（整行一次 write）=================
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_frame]
    mov     ecx, msg_frame_len
    call    append_str
    mov     rax, rbx
    call    append_dec
    lea     rsi, [rel msg_flip]
    mov     ecx, msg_flip_len
    call    append_str
    mov     rax, [rsp + ST_RET]          ; fb_flip 的返回码
    call    append_dec
    lea     rsi, [rel msg_va]
    mov     ecx, msg_va_len
    call    append_str
    mov     rax, r12
    call    append_hex
    lea     rsi, [rel msg_pid]
    mov     ecx, msg_pid_len
    call    append_str
    mov     eax, NR_GETPID
    int     0x80
    call    append_dec
    call    flush_line

    ; ================= 停到下一帧（测试要按帧抓屏）=================
    mov     eax, NR_SLEEP_MS
    mov     edi, FRAME_MS
    int     0x80

    inc     ebx
    cmp     ebx, FRAMES
    jb      .frame_loop

    ; ---- 全屏提交一次（fb_present：验证 11 号调用）；然后退出 ----
    mov     eax, NR_FB_PRESENT
    int     0x80
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_done]
    mov     ecx, msg_done_len
    call    append_str
    mov     rax, FRAMES
    call    append_dec
    call    flush_line

    mov     eax, NR_EXIT
    xor     edi, edi
    int     0x80

.hang:
    jmp     .hang

.map_failed:
    ; fb_map 失败：如实打印错误码（负值），别再往下画（绝不假装成功）
    mov     r11, rax
    lea     rdi, [rsp + ST_LINE]
    lea     rsi, [rel msg_mapfail]
    mov     ecx, msg_mapfail_len
    call    append_str
    mov     rax, r11
    call    append_dec
    call    flush_line
    mov     eax, NR_EXIT
    mov     edi, 1
    int     0x80
    jmp     .hang

; ---------------------------------------------------------------------------
; 行缓冲助手（rdi = 写指针，返回时 rdi 指向行尾；只碰 r8/r9/rcx/rdx/rsi/rax）
; ---------------------------------------------------------------------------
append_str:                              ; rsi = src, rcx = len
    test    rcx, rcx
    jz      .done
.loop:
    mov     al, [rsi]
    mov     [rdi], al
    inc     rsi
    inc     rdi
    dec     rcx
    jnz     .loop
.done:
    ret

append_dec:                              ; rax = 值（十进制）
    sub     rsp, 40
    lea     r8, [rsp + 32]
    mov     r9, rdi                      ; 保存 dest
    mov     rcx, 10
.loop:
    xor     edx, edx
    div     rcx
    add     dl, '0'
    dec     r8
    mov     [r8], dl
    test    rax, rax
    jnz     .loop
    lea     rcx, [rsp + 32]
    sub     rcx, r8
    mov     rsi, r8
    mov     rdi, r9
.copy:
    mov     al, [rsi]
    mov     [rdi], al
    inc     rsi
    inc     rdi
    dec     rcx
    jnz     .copy
    add     rsp, 40
    ret

append_hex:                              ; rax = 值（十六进制，小写、无前缀）
    sub     rsp, 40
    lea     r8, [rsp + 32]
    mov     r9, rdi
    mov     rcx, 16
.loop:
    xor     edx, edx
    div     rcx
    cmp     dl, 10
    jb      .digit
    add     dl, 'a' - 10
    jmp     .store
.digit:
    add     dl, '0'
.store:
    dec     r8
    mov     [r8], dl
    test    rax, rax
    jnz     .loop
    lea     rcx, [rsp + 32]
    sub     rcx, r8
    mov     rsi, r8
    mov     rdi, r9
.copy:
    mov     al, [rsi]
    mov     [rdi], al
    inc     rsi
    inc     rdi
    dec     rcx
    jnz     .copy
    add     rsp, 40
    ret

flush_line:                              ; 补 '\n' 并 write(1, 行首, 长度)
    mov     byte [rdi], 10
    inc     rdi
    ; 本函数是 call 进来的：返回地址占了 [rsp]，所以行首相对当前 rsp 要 +8
    lea     rsi, [rsp + 8 + ST_LINE]
    mov     rdx, rdi
    sub     rdx, rsi
    mov     eax, NR_WRITE
    mov     edi, 1
    int     0x80
    ret

; ---- 只读数据（与代码同页：可读可执行、不可写）----
msg_map:        db "[FBDEMO] map va=0x"
msg_map_len     equ $ - msg_map
msg_frame:      db "[FBDEMO] frame="
msg_frame_len   equ $ - msg_frame
msg_flip:       db " flip="
msg_flip_len    equ $ - msg_flip
msg_va:         db " va=0x"
msg_va_len      equ $ - msg_va
msg_pid:        db " pid="
msg_pid_len     equ $ - msg_pid
msg_w:          db " w="
msg_w_len       equ $ - msg_w
msg_h:          db " h="
msg_h_len       equ $ - msg_h
msg_pitch:      db " pitch="
msg_pitch_len   equ $ - msg_pitch
msg_fmt:        db " fmt="
msg_fmt_len     equ $ - msg_fmt
msg_oob:        db "[FBDEMO] oob ret="
msg_oob_len     equ $ - msg_oob
msg_clamp:      db "[FBDEMO] clamp ret="
msg_clamp_len   equ $ - msg_clamp
msg_done:       db "[FBDEMO] done frames="
msg_done_len    equ $ - msg_done
msg_mapfail:    db "[FBDEMO] fb_map FAILED ret="
msg_mapfail_len equ $ - msg_mapfail
