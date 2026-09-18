; loader.asm - Vimtu64 Loader
;
; 实模式阶段（BIOS 只在这个阶段用）:
;   1) 串口初始化 (COM1) + 开启 A20 地址线
;   2) E820 收集内存地图 -> 0x2000
;   3) VBE 探测并设置图形模式（按显示器 EDID/模式表自适应）+ 读 EDID
;   4) BootInfo -> 0x1000
;
; 保护模式阶段（此后不再返回实模式，也再不用任何 BIOS 中断）:
;   5) 用自带的 ATA PIO (LBA28) 驱动，把内核从 LBA 9 起
;      **直接读进 0x100000**（每块 128 扇区 = 64KB）
;   6) 远跳 0x08:0x100000 进入内核
;
; 为什么改成"进 PM 一次不返回 + 自己读盘"（旧写法是"BIOS 读盘 + 每 pass 进出 PM 拷贝"）:
;   实测在 VMware 上，只要自己做过一次"进/出保护模式"的往返，它的 BIOS 之后的中断服务
;   （INT 13h 读盘、INT 10h VBE）就会**永久挂住**；还原 GDTR、复位 FS/GS、重新开中断
;   全都无效，连一次只拷 0 字节的往返也足以触发。SeaBIOS 不依赖这些状态，
;   所以旧写法在 QEMU 上一直正常 —— 这是它长期没被发现的原因。
;   现在 BIOS 只在实模式阶段用（E820/VBE/EDID），进 PM 后不再调用任何 BIOS 中断，
;   两个虚拟机上都成立。
;
; loader 加载在 0x9000（避开 SeaBIOS trampoline 区 0x8000-0x9000），
; 整个 loader 必须 < 0x7000 字节，否则 16 位远跳偏移会越界。
;
; 实模式内存布局:
;   0x0600  DAP（仅 disk_error 诊断打印用，现在已不再用 BIOS 读盘）
;   0x1000  BootInfo
;   0x2000  E820 内存条目 (最多 64 * 20B)
;   ★ 内核已搬高半区：引导层建立 **VA = 0xFFFFFFFF80000000 + PA** 的线性直映
;     （覆盖 PA 0..1GB，复用下面的 PD0），内核装载在 PA 0x100000 -> 链接在
;     0xFFFFFFFF80100000（见 kernel/linker64.ld 与 kernel/memlayout64.h）。
;   0x7600  EDID 缓冲 (128B)
;   0x7800  RSDP 传递槽 (8B)：BIOS 路径显式写 0 = 内核走 legacy 扫描（见 memlayout64.h）
;   0x7BFF  栈顶（向下生长）
;   0x9000  loader 自身

%macro SPUTC16 0
    push ax
    push dx
    push cx
    mov ah, al
    mov dx, 0x3FD
    mov cx, 0xFFFF
%%w:
    in al, dx
    test al, 0x20
    jnz %%d
    loop %%w
%%d:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop cx
    pop dx
    pop ax
%endmacro

%macro SPUTC32 0
    push eax
    push edx
    push ecx
    mov ah, al
    mov dx, 0x3FD
    mov ecx, 0xFFFF
%%w:
    in al, dx
    test al, 0x20
    jnz %%d
    loop %%w
%%d:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop ecx
    pop edx
    pop eax
%endmacro

KERNEL_SECTORS equ 8000             ; Vimtu64 64-bit kernel area (4MB)+字体+logo+图标 总扇区数（4000*512 = 2,048,000 B ≤ 2.048MB）
                                    ; PM 的 ATA 驱动按 128 扇区一块读，默认读满这个数

[bits 16]
[org 0x9000]

start:
    mov [BOOT_DRIVE], dl
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7BFF
    call ser_init
    call enable_a20


    mov si, msg_loader
    call print_string

    ; ---- 1. E820 内存地图 ----
    call detect_memory
    mov si, msg_mem
    call print_string
    mov ax, word [mem_count]
    call print_hex16
    mov si, msg_entries
    call print_string

    ; ---- 2. VBE 探测 ----
    call vbe_probe
    jc vbe_error
    ; 注意：图形模式已设置，int 0x10 文本输出会挂起，改用 debugcon
    mov si, msg_vbe_ok
    call dbg_str
    mov ax, word [best_mode]
    call dbg_hex16
    mov si, msg_mode_done
    call dbg_str

    ; 写 BootInfo
    call write_bootinfo
    mov si, msg_l_bootinfo
    call dbg_str

    ; ---- 3. 读内核：进保护模式一次，用 loader 自带的 ATA PIO 驱动直接读进 0x100000 ----
    ; 为什么不再用 "BIOS INT 13h 读盘 + 多 pass 保护模式拷贝"：
    ;   实测在 VMware 上，只要"进/出保护模式"往返一次，它的 BIOS 之后的中断服务
    ;   （INT 13h 读盘、INT 10h VBE）就会永久挂住；还原 GDTR、复位 FS/GS、重新开中断
    ;   全都无效，连一次只拷 0 字节的往返也足以触发。QEMU 的 SeaBIOS 不依赖这些状态，
    ;   所以旧写法在 QEMU 上一直正常 —— 这正是它长期没被发现的原因。
    ;   现在改成：BIOS 只负责 E820 / VBE / EDID（上面都已完成），此后进保护模式不再返回，
    ;   内核由我们自己的 ATA PIO 直接读入 0x100000，全程不再调用任何 BIOS 中断。
    mov si, msg_l_ata
    call dbg_str
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    db 0xEA
    dw pm_ata_load
    dw 0x0008
    cli
    hlt

; ================= 子程序（全部 16 位实模式） =================

; ---- 开启 A20 地址线 ----
; 内核要被拷贝到 0x100000（1MB 以上）。若 A20 未开启，0x100000 会回绕到 0x000000，
; 那次 512KB 拷贝的目标区间就变成 0x00000-0x7D000 —— 而 loader 自身位于 0x9000，
; 正落在这个区间内：拷贝写到一半就把正在执行的 loader 代码覆盖成垃圾 -> 三重故障。
; SeaBIOS（QEMU）默认已替我们开好 A20，所以在 QEMU 上一直"侥幸正常"；
; VMware 的 BIOS 没有这么做，于是一拷就崩。下面三种方法依次尝试，并用
; 0x000500 / 0x100500 的别名测试验证是否真的生效。
enable_a20:
    pusha
    ; 方法 1：BIOS 功能 INT 15h, AX=2401h（开启 A20）
    mov ax, 0x2401
    int 0x15
    call a20_check
    jz .ok                      ; ZF=1 -> 已开启
    ; 方法 2：快速 A20（芯片组端口 0x92 的 bit1）
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    call a20_check
    jnz .ok
    ; 方法 3：键盘控制器（0x64 命令 0xD1 -> 0x60 数据 0xDF）
    call a20_kbd
    call a20_check
.ok:
    jnz .off                    ; ZF=0 -> 仍未开启
    mov si, msg_a20_on
    jmp .log
.off:
    mov si, msg_a20_off
.log:
    call dbg_str
    popa
    ret

; ZF=1 表示 A20 已开启（别名测试：0x000500 与 0x100500 在 A20 关闭时会互相覆盖）
a20_check:
    pusha
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov al, [0x0500]
    push ax                     ; 保存原字节（低地址 0x500）
    mov ax, 0xFFFF
    mov es, ax
    ; ★ 别名测试的另一半位于 0xFFFF:0x0510 = 物理 0x100500，也就是**内核镜像的第一个 32KB 内**。
    ;   光盘/裸盘路径下内核是稍后才读进去的，所以这个写入被随后的读覆盖、看不出问题；
    ;   但 U 盘/hybrid 路径下内核在引导桩阶段就已经躺在 0x100000 了 —— 那次 0xFF 写入会把
    ;   内核代码改成非法指令（实测正是 0x100500 处 0xEE -> 0xFF，一进内核就 #UD 三重故障）。
    ;   所以这里必须先存后恢复：测试归测试，别动别人的内存。
    mov al, [es:0x0510]
    push ax                     ; ★ 保存原字节（高地址 0x100500）
    mov byte [0x0500], 0x00
    mov byte [es:0x0510], 0xFF
    mov al, [0x0500]
    cmp al, 0xFF                ; 被别名改写成 0xFF -> A20 仍是关闭的
    pop bx                      ; ★ 先恢复高地址原字节（pop/mov 不改 ZF）
    mov [es:0x0510], bl
    pop ax
    mov [0x0500], al            ; 恢复低地址原字节
    pop es
    pop ds
    popa
    ret

a20_kbd:
    pusha
    call .wait_in
    mov al, 0xD1
    out 0x64, al
    call .wait_in
    mov al, 0xDF
    out 0x60, al
    call .wait_in
    popa
    ret
.wait_in:
    mov cx, 0xFFFF
.loop:
    in al, 0x64
    test al, 0x02
    jz .done
    loop .loop
.done:
    ret

detect_memory:
    mov di, 0x2000
    xor ebx, ebx
    mov dword [mem_count], 0
.loop:
    mov eax, 0xE820
    mov ecx, 20
    mov edx, 0x534D4150        ; 'SMAP'
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    add di, 20
    inc dword [mem_count]
    test ebx, ebx
    jz .done
    cmp dword [mem_count], 64
    jb .loop
.done:
    ret

vbe_probe:
    mov si, msg_d_begin
    call dbg_str
    ; 请求 VBE 2.0 控制器信息
    mov di, 0x7000
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    push ax
    mov si, msg_dbg1
    call print_string
    pop ax
    call print_hex16
    push ax
    mov si, msg_d_ax
    call dbg_str
    pop ax
    call dbg_hex16
    call dbg_nl
    cmp ax, 0x004F
    jne .fail
    cmp dword [0x7000], 'VESA'
    jne .fail
    mov si, msg_d_sigok
    call dbg_str
    call dbg_nl
    call read_edid                  ; 读显示器 EDID（供模式自适应）
    ; 模式列表指针 (offset:segment) 位于偏移 14
    mov ax, [0x7000 + 14]
    mov bx, [0x7000 + 16]
    mov es, bx
    mov di, ax
    ; 调试: 打印列表指针
    push ax
    push bx
    mov si, msg_dbg2
    call print_string
    pop bx
    mov ax, bx
    call print_hex16
    mov si, msg_colon
    call print_string
    pop ax
    call print_hex16
    mov si, msg_nl
    call print_string
    ; 打印前 4 个模式号
    push di
    mov cx, 0
.dbg_modes:
    cmp cx, 4
    jge .dbg_done
    push cx
    mov si, msg_dbg3
    call print_string
    pop cx
    push cx
    mov ax, [es:di]
    call print_hex16
    pop cx
    push cx
    mov si, msg_space
    call print_string
    pop cx
    add di, 2
    inc cx
    jmp .dbg_modes
.dbg_done:
    pop di
    mov si, msg_nl
    call print_string
    mov si, msg_d_list
    call dbg_str
    ; 打印列表指针（已存在 best 数据区临时用）
    push di
    mov ax, [0x7000 + 16]
    call dbg_hex16
    mov ax, [0x7000 + 14]
    call dbg_hex16
    call dbg_nl
    pop di
    mov word [best_mode], 0
    mov word [ml_count], 0         ; 已收集的可用模式数
    mov word [best_score], 0       ; score 均为正数，0 表示未选
.mode_loop:
    mov cx, [es:di]
    cmp cx, 0xFFFF
    je .done
    push cx
    mov si, msg_d_mode
    call dbg_str
    pop cx
    push cx
    mov ax, cx                     ; dbg_hex16 打印 ax
    call dbg_hex16
    pop cx
    push es
    push di
    push cx
    ; 查询模式信息到 0x0000:0x7200
    xor ax, ax
    mov es, ax
    mov di, 0x7200
    mov ax, 0x4F01
    int 0x10
    pop cx
    push cx
    push ax
    mov si, msg_d_ret
    call dbg_str
    pop ax
    call dbg_hex16
    pop cx
    cmp ax, 0x004F
    jne .next_mode
    ; 模式属性
    mov ax, [0x7200]
    push ax
    mov si, msg_d_attr
    call dbg_str
    pop ax
    call dbg_hex16
    test ax, 0x0001
    jz .next_mode
    test ax, 0x0080            ; 需要 LFB
    jz .next_mode
    mov si, msg_d_ok
    call dbg_str
    mov ax, [0x7200 + 0x12]
    call dbg_hex16
    mov ax, [0x7200 + 0x14]
    call dbg_hex16
    mov al, [0x7200 + 0x1A]
    call dbg_hex16
    mov ax, [0x7200 + 0x12]
    mov [m_width], ax
    mov ax, [0x7200 + 0x14]
    mov [m_height], ax
    mov al, [0x7200 + 0x19]        ; bits_per_pixel（VBE 2.0 偏移 0x19）
    mov [m_bpp], al
    ; ---- 收集模式清单：内核要显示"显示器实际支持的模式"（真实探测值） ----
    ; 内核整套渲染都是 32bpp LFB，所以只收集 32bpp 模式：这样模式表恰好就是
    ; "本适配器在 32bpp 下真实支持的显示模式"，设置页的"分辨率"下拉直接用它。
    ; （VBE 模式表里前面的 0x100..0x11x 都是 8/15/16/24bpp 老模式，若不过滤会占满
    ;   16 项上限，把真正可用的 32bpp 模式挤掉。）
    cmp byte [m_bpp], 32
    jne .ml_skip_pop            ; 非 32bpp：不入表（此时还没 push，不能走 .ml_skip）
    push bx
    cmp word [ml_count], 16
    jae .ml_skip
    mov bx, [ml_count]
    shl bx, 3                      ; 每项 8 字节
    add bx, 0x7400
    mov [bx], cx                   ; VBE 模式号
    mov ax, [m_width]
    mov [bx + 2], ax
    mov ax, [m_height]
    mov [bx + 4], ax
    mov al, [m_bpp]
    mov [bx + 6], al
    mov byte [bx + 7], 0
    inc word [ml_count]
.ml_skip:
    pop bx
.ml_skip_pop:
    call mode_score
    mov [sc_tmp], ax            ; 保存分数
    cmp ax, [best_score]
    jbe .next_mode
    mov [best_score], ax
    mov [best_mode], cx
    mov eax, [0x7200 + 0x28]
    mov [best_lfb], eax
    mov ax, [0x7200 + 0x10]
    mov [best_pitch], ax
    mov ax, [0x7200 + 0x12]
    mov [best_w], ax
    mov ax, [0x7200 + 0x14]
    mov [best_h], ax
    mov al, [0x7200 + 0x19]
    mov [best_bpp], al
.next_mode:
    pop di
    pop es
    add di, 2
    jmp .mode_loop
.done:
    mov si, msg_d_best
    call dbg_str
    mov ax, [best_mode]
    call dbg_hex16
    call dbg_nl
    cmp word [best_mode], 0
    je .fail
    ; 设置模式 (启用 LFB)
    mov ax, 0x4F02
    mov bx, [best_mode]
    or bx, 0x4000
    int 0x10
    cmp ax, 0x004F
    jne .fail
    clc
    ret
.fail:
    stc
    ret

; 返回 ax = 模式评分
mode_score:
    ; ---- 有 EDID 时按显示器原生分辨率自适应 ----
    ; 只考虑 32bpp、且分辨率不超过原生：分数 = 宽*高/1024（越大越接近原生越优）；
    ; 其它情况给最低分 1，保证仍能选出一个可用模式（不会出现选不出模式的情况）。
    mov bx, [edid_w]
    test bx, bx
    jz .table_path
    mov al, [m_bpp]
    cmp al, 32
    jne .edid_low
    mov dx, [m_width]
    cmp dx, bx
    ja .edid_low
    mov dx, [m_height]
    cmp dx, [edid_h]
    ja .edid_low
    mov ax, [m_width]
    mov dx, [m_height]
    mul dx                           ; dx:ax = 宽*高
    push cx                          ; cx 是调用方的模式号，必须保护
    mov cx, 10
.edid_sh:
    shr dx, 1
    rcr ax, 1
    loop .edid_sh                     ; /1024
    pop cx
    ret
.edid_low:
    mov ax, 1
    ret
.table_path:
    cmp word [m_width], 1024
    jne .w800
    cmp word [m_height], 768
    jne .w800
    mov al, [m_bpp]
    cmp al, 32
    je .s100
    cmp al, 24
    je .s75
    cmp al, 16
    je .s50
    jmp .s10
.w800:
    cmp word [m_width], 800
    jne .w640
    cmp word [m_height], 600
    jne .w640
    mov al, [m_bpp]
    cmp al, 32
    je .s90
    cmp al, 24
    je .s70
    cmp al, 16
    je .s45
    jmp .s10
.w640:
    cmp word [m_width], 640
    jne .w480
    cmp word [m_height], 480
    jne .w480
    mov al, [m_bpp]
    cmp al, 32
    je .s80
    cmp al, 24
    je .s60
    cmp al, 16
    je .s40
    jmp .s10
.w480:
    cmp word [m_height], 400
    jne .s10
    mov al, [m_bpp]
    cmp al, 32
    je .s30
    jmp .s10
.s100:
    mov ax, 100
    ret
.s90:
    mov ax, 90
    ret
.s80:
    mov ax, 80
    ret
.s75:
    mov ax, 75
    ret
.s70:
    mov ax, 70
    ret
.s60:
    mov ax, 60
    ret
.s50:
    mov ax, 50
    ret
.s45:
    mov ax, 45
    ret
.s40:
    mov ax, 40
    ret
.s30:
    mov ax, 30
    ret
.s10:
    mov ax, 10
    ret

; ---- 读显示器 EDID（VBE/DDC 4F15 BL=01），解析首选详细时序的 hact/vact ----
; 结果：edid_ok = 1/0；edid_w/edid_h = 显示器原生分辨率（无 EDID 时为 0）
read_edid:
    push cx
    mov byte [edid_ok], 0
    mov word [edid_w], 0
    mov word [edid_h], 0
    push es
    xor ax, ax
    mov es, ax
    mov di, 0x7600
    mov ax, 0x4F15
    mov bx, 0x0001
    xor cx, cx
    xor dx, dx
    int 0x10
    pop es
    cmp ax, 0x004F
    jne .none
    cmp byte [0x7600], 0x00          ; EDID 头 00 FF FF FF FF FF FF 00
    jne .none
    cmp byte [0x7601], 0xFF
    jne .none
    cmp byte [0x7607], 0x00
    jne .none
    mov al, [0x7600 + 56]            ; d[2] = hact 低 8 位
    mov ah, 0
    mov cl, [0x7600 + 58]            ; d[4] 高 4 位 = hact 高 4 位
    shr cl, 4
    mov ch, 0
    shl cx, 8
    or ax, cx
    mov [edid_w], ax
    mov al, [0x7600 + 59]            ; d[5] = vact 低 8 位
    mov ah, 0
    mov cl, [0x7600 + 61]            ; d[7] 高 4 位 = vact 高 4 位
    shr cl, 4
    mov ch, 0
    shl cx, 8
    or ax, cx
    mov [edid_h], ax
    cmp word [edid_w], 640           ; 合理性下限
    jb .none
    cmp word [edid_h], 480
    jb .none
    mov byte [edid_ok], 1
    mov si, msg_edid_ok
    call dbg_str
    mov ax, [edid_w]
    call dbg_hex16
    mov ax, [edid_h]
    call dbg_hex16
    call dbg_nl
    pop cx
    ret
.none:
    mov si, msg_edid_no
    call dbg_str
    pop cx
    ret

write_bootinfo:
    mov dword [0x1000], 0x41555231    ; 'AUR1'
    mov eax, [best_lfb]
    mov [0x1004], eax
    mov ax, [best_w]
    mov [0x1008], ax
    mov ax, [best_h]
    mov [0x100A], ax
    mov al, [best_bpp]
    mov [0x100C], al
    mov byte [0x100D], 0
    mov ax, [best_pitch]
    mov [0x100E], ax
    mov eax, [mem_count]
    mov [0x1010], eax
    mov dword [0x1014], 0x2000
    mov eax, KERNEL_SECTORS        ; 始终读满 KERNEL_SECTORS 个扇区（与旧行为一致）
    shl eax, 9
    mov [0x1018], eax              ; BootInfo.kernel_size
    mov ax, [best_mode]
    mov [0x101C], ax               ; 当前 VBE 模式号
    mov ax, [ml_count]
    mov [0x101E], ax               ; 可用模式数
    mov dword [0x1020], 0x7400     ; 模式表地址
    mov dword [0x1024], 0x7600     ; EDID 缓冲地址
    mov word [0x1028], 128         ; EDID 长度
    mov al, [edid_ok]
    mov ah, 0
    mov [0x102A], ax               ; EDID 有效标志
    ret

disk_error:
    ; 重要：此时已处于图形模式，int 0x10 文本输出会挂起（本文件前面已有此教训），
    ; 所以错误必须走 dbg_str（0x402 debugcon + COM1 串口双出口），否则表现为"静默死机"。
    ; 同时打印失败时的读盘参数，便于定位是哪一次读取出问题。
    mov si, msg_disk_err
    call dbg_str
    mov ax, word [dap_lba]          ; LBA 低 16 位
    call dbg_hex16
    mov ax, word [dap_lba + 2]      ; LBA 高 16 位
    call dbg_hex16
    mov ax, word [dap_count]        ; 本次请求扇区数
    call dbg_hex16
    mov ax, word [dap_seg]          ; 目标段
    call dbg_hex16
    call dbg_nl
    cli
    hlt
    jmp $

vbe_error:
    mov si, msg_vbe_err
    call dbg_str
    mov ax, [best_mode]
    call dbg_hex16
    call dbg_nl
    cli
    hlt
    jmp $

print_string:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp print_string
.done:
    ret

; ---- COM1 (0x3F8) 串口镜像输出 ----
; 与 0x402(debugcon) 并行输出：QEMU 有 0x402 设备，VMware 没有，但两者都能把串口
; 写进文件（QEMU: -serial file:... ；VMware: serial0.fileType="file"）。
; 这样"loader 卡在哪一步"在两个虚拟机上都可见，排障不再有盲区。
ser_init:
    pusha
    push dx
    mov dx, 0x3F9
    xor al, al
    out dx, al                 ; 关中断
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al                 ; DLAB 置位
    mov dx, 0x3F8
    mov al, 1
    out dx, al                 ; 波特率除数低字节（115200）
    mov dx, 0x3F9
    xor al, al
    out dx, al                 ; 高字节
    mov dx, 0x3FB
    mov al, 3
    out dx, al                 ; 8 位 / 无校验 / 1 停止位
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al                 ; 开 FIFO 并清空
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al                 ; DTR / RTS
    pop dx
    popa
    ret

; 输出 al 中的字符到 COM1（有界自旋：串口不存在也不会挂死）
ser_putc:
    pusha
    push dx
    mov ah, al
    mov dx, 0x3FD
    mov cx, 0xFFFF
.wait:
    in al, dx
    test al, 0x20
    jnz .ok
    loop .wait
.ok:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop dx
    popa
    ret

; ---- QEMU debugcon 调试输出 (0x402) + 串口镜像 ----
dbg_str:
    pusha
.loop:
    lodsb
    or al, al
    jz .done
    mov dx, 0x402
    out dx, al
    SPUTC16
    jmp .loop
.done:
    popa
    ret

dbg_hex16:
    pusha
    mov cx, 4
.loop:
    rol ax, 4
    push ax
    and al, 0x0F
    cmp al, 10
    jb .num
    add al, 'A' - 10
    jmp .put
.num:
    add al, '0'
.put:
    mov dx, 0x402
    out dx, al
    SPUTC16
    pop ax
    loop .loop
    mov al, ' '
    out dx, al
    popa
    ret

dbg_nl:
    pusha
    mov al, 0x0D
    mov dx, 0x402
    out dx, al
    SPUTC16
    mov al, 0x0A
    out dx, al
    popa
    ret

; 打印 ax 的十六进制
print_hex16:
    pusha
    mov cx, 4
.loop:
    rol ax, 4
    push ax
    and al, 0x0F
    cmp al, 10
    jb .num
    add al, 'A' - 10
    jmp .put
.num:
    add al, '0'
.put:
    mov ah, 0x0E
    int 0x10
    pop ax
    loop .loop
    popa
    ret

; ================= 数据 =================
align 4
dap:
    db 0x10
    db 0
dap_count:
    dw 127
dap_off:
    dw 0
dap_seg:
    dw 0
dap_lba:
    dd 9
    dd 0

align 4
gdt_start:
    dq 0x0000000000000000
gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0xCF
    db 0x00
gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x92
    db 0xCF
    db 0x00
gdt_code16:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 0x9A
    db 0x00
    db 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

BOOT_DRIVE: db 0
ata_lba:   dd 9             ; 保护模式 ATA 读：当前 LBA
ata_dst:   dd 0x100000      ; 当前目标物理地址
ata_left:  dd 0             ; 还差多少扇区
ata_chunk: dd 0             ; 本块读多少扇区
mem_count: dd 0
best_mode: dw 0
best_score: dw 0
best_lfb: dd 0
best_pitch: dw 0
best_w: dw 0
best_h: dw 0
best_bpp: db 0
ml_count: dw 0
edid_ok:  db 0
edid_w:   dw 0
edid_h:   dw 0
m_width: dw 0
m_height: dw 0
m_bpp: db 0
sc_tmp: dw 0

msg_loader:    db "Vimtu64 Loader", 0x0D, 0x0A, 0
msg_mem:       db "E820 entries: ", 0
msg_entries:   db 0x0D, 0x0A, 0
msg_vbe_ok:    db "VBE mode: 0x", 0
msg_mode_done: db 0x0D, 0x0A, 0
msg_kernel_ok: db "Kernel loaded, jumping to protected mode...", 0x0D, 0x0A, 0
msg_vbe_err:   db "VBE error! best_mode=0x", 0
msg_disk_err:  db "Kernel disk read error!", 0x0D, 0x0A, 0
msg_l_bootinfo: db "L:bootinfo", 13, 10, 0
msg_l_ata:      db "L:ata", 13, 10, 0
msg_edid_ok:    db "L:edid ok", 13, 10, 0
msg_edid_no:    db "L:edid none", 13, 10, 0
m32_pm_ok:      db "L:pm ok", 13, 10, 0
m32_enter:      db "L:enter long mode", 13, 10, 0
m32_cpu_ok:     db "L:cpuid ok", 13, 10, 0
m32_pg_ok:      db "L:paging ok", 13, 10, 0
m32_no_lm:      db "L:FATAL no long mode: CPUID.80000001h:EDX.LM=0 (check VM guestOS=64bit)", 13, 10, 0
m32_no_pae:     db "L:FATAL CPUID.01h:EDX.PAE=0 (PAE missing -> cannot enter long mode)", 13, 10, 0
m32_no_leaf:    db "L:FATAL no extended CPUID leaf 80000001h (LM not reportable)", 13, 10, 0
m32_edx_pre:    db "  edx=0x", 0
m32_maxleaf_pre: db "  max_ext_leaf=0x", 0
m32_nl:         db 13, 10, 0
cpuid_edx:      dd 0
cpuid_maxleaf:  dd 0
msg_a20_on:     db "A20=on", 13, 10, 0
msg_a20_off:    db "A20=OFF", 13, 10, 0
msg_dbg1:      db "VBE probe AX=0x", 0
msg_dbg2:      db "Mode list seg:off=", 0
msg_dbg3:      db "mode=0x", 0
msg_colon:     db ":", 0
msg_space:     db " ", 0
msg_nl:        db 0x0D, 0x0A, 0
msg_d_begin:   db "[VBE]", 0
msg_d_ax:      db "AX=", 0
msg_d_sigok:   db "SIG=OK", 0
msg_d_list:    db "LIST seg,off=", 0
msg_d_mode:    db "M=", 0
msg_d_ret:     db " ret=", 0
msg_d_attr:    db " attr=", 0
msg_d_ok:      db " ok(", 0
msg_d_score:   db ") sc=", 0
msg_d_sc2:     db " sc2=", 0
msg_d_bpp2:    db " bpp=", 0
msg_d_best:    db "BEST=", 0


; ============================================================================
; Vimtu64 引导第二段：32 位代码被压缩到"进入长模式所必需的最少指令"
;
;   [bits 32] 这一段只做 CPU 规定必须先做的事（无法用 64 位代码完成）：
;       * 建立 4GB 恒等映射页表（PML4/PDPT/PD）
;       * CR4.PAE = 1
;       * IA32_EFER.LME = 1
;       * CR0.PG = 1 并远跳到 64 位代码段
;   [bits 64] 其余全部在 64 位长模式里做：
;       * 初始化串口、打印阶段标记
;       * 用 ATA PIO 把内核从 LBA 9 读进 0x100000（原来这段是 32 位的，现已搬到 64 位）
;       * 跳转到内核入口 0x100000
;
; 结论：引导链里**没有 32 位业务代码**（读盘与输出都是 64 位）。
; ============================================================================
[bits 32]

; 16 位阶段用远跳（CS=0x08, PE=1）进入这里
pm_ata_load:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov al, 'P'                     ; 标记：已进入保护模式（唯一目的就是进长模式）
    mov dx, 0x402
    out dx, al
    mov esi, m32_pm_ok              ; 串口也留一份（VMware 没有 0x402 设备）
    call ser32_str
    jmp lm64_enter_long

; ---- 32 位阶段的串口输出（32 位代码不能调用 16 位的 dbg_str）----
ser32_putc:                         ; al = 字符
    push eax
    push edx
    push ecx
    mov ah, al
    mov dx, 0x3FD
    mov ecx, 0xFFFF
.wait:
    in al, dx
    test al, 0x20
    jnz .ok
    loop .wait
.ok:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop ecx
    pop edx
    pop eax
    ret

ser32_str:                          ; esi = 以 0 结尾的字符串（ds 已是 0x10 平坦段）
    push eax
    push esi
.loop:
    lodsb
    test al, al
    jz .done
    call ser32_putc
    jmp .loop
.done:
    pop esi
    pop eax
    ret

; ---- CPUID 检测：PAE 与长模式 ----
; ★ 踩坑（VMware 上引导链静默停机，屏幕全黑）：
;   PAE 位必须读 **CPUID.01h:EDX bit6**。AMD 在 80000001h:EDX bit6 也报 PAE，
;   但 Intel **不报**（本机 Intel i3-6100 实测 80000001h:EDX=0x2C100800，bit6=0）。
;   旧代码把 PAE 读在 80000001h，于是在 QEMU 的 qemu64（AMD 风格 CPU）上一直通过，
;   一到 VMware（直通 Intel 主机 CPU）就被判成"不支持长模式"并静默 hlt。
;   失败时把原因字符串指针放进 esi，并记录 edx / 扩展叶最大值供 lm64_fatal 打印。
lm64_cpuid:
    push ebx
    push ecx
    push edx
    push eax
    mov eax, 1
    cpuid
    mov [cpuid_edx], edx
    test edx, 1 << 6                 ; CPUID.01h:EDX.PAE
    jz .fail_pae
    mov eax, 0x80000000
    cpuid
    mov [cpuid_maxleaf], eax
    cmp eax, 0x80000001
    jb .fail_leaf
    mov eax, 0x80000001
    cpuid
    mov [cpuid_edx], edx
    test edx, 1 << 29                ; CPUID.80000001h:EDX.LM
    jz .fail_lm
    pop eax
    pop edx
    pop ecx
    pop ebx
    clc
    ret
.fail_pae:
    mov esi, m32_no_pae
    jmp .fail_common
.fail_leaf:
    mov esi, m32_no_leaf
    jmp .fail_common
.fail_lm:
    mov esi, m32_no_lm
.fail_common:
    pop eax
    pop edx
    pop ecx
    pop ebx
    stc
    ret

; ---- 32 位十六进制输出：eax = 值（8 位十六进制）----
ser32_hex32:
    push eax
    push ebx
    push ecx
    mov ebx, eax
    mov ecx, 8
.hexloop:
    rol ebx, 4
    mov eax, ebx
    and eax, 0x0F
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp .out
.digit:
    add al, '0'
.out:
    call ser32_putc
    dec ecx
    jnz .hexloop
    pop ecx
    pop ebx
    pop eax
    ret

; ---- 构建恒等映射页表：0..4GB，用 2MB 大页（PML4 -> 4*PDPT 项 -> 4*PD）----
; 页表物理位置必须与 kernel/memlayout64.h 里的 ML64_PML4/... 常量一致。
lm64_build_paging:
    ; ---- 先把页表区整体清零：PML4(0x40000) + PDPT(0x41000) + 4×PD(0x42000..0x46000)
    ;      + 高半区 PDPT(0x46000) + 高半区 PD(0x47000) = 0x40000..0x48000 ----
    ;   ★ 必须显式清零。早期版本只写了 PD 项的低 32 位、且没有清 PD 区，靠"上电后
    ;     RAM 恰好是 0"才成立；2MB 大页项的**高 32 位**是物理地址高位，残留非 0
    ;     就会映射到错误的地方（或保留位非 0 -> #PF）。这类 bug 只在真实环境暴露。
    mov edi, 0x40000
    mov cr3, edi                    ; ★ 必须先把 CR3 指向 PML4！
                                    ;   漏了这一行的话：CR3=0，等 CR0.PG=1 一开分页，
                                    ;   CPU 会去物理 0 找页表 -> #PF -> 没有 IDT -> 三重故障
                                    ;   表现为"L:paging ok 之后立刻复位"（搬高半区时踩过）
    xor eax, eax
    mov ecx, (0x48000 - 0x40000) / 4
    rep stosd

    ; PML4[0] -> PDPT(0x41000)：0..4GB 恒等映射（LFB/载荷/页池都按物理地址访问）
    mov dword [0x40000 + 0*8], 0x41000 | 3

    ; PML4[511] -> 高半区 PDPT(0x46000)
    mov dword [0x40000 + 511*8], 0x46000 | 3

    ; PDPT：4 项，各覆盖 1GB（0..4GB）
    mov dword [0x41000 + 0*8], 0x42000 | 3
    mov dword [0x41000 + 1*8], 0x43000 | 3
    mov dword [0x41000 + 2*8], 0x44000 | 3
    mov dword [0x41000 + 3*8], 0x45000 | 3

    ; 4 张 PD：每张 512 项 × 2MB = 1GB 恒等映射
    mov edi, 0x42000
    mov eax, 0x83                   ; P|RW|PS(2MB 页)
    mov ecx, 512 * 4
.fill_pd:
    mov [edi], eax
    add edi, 8
    add eax, 0x200000
    dec ecx
    jnz .fill_pd

    ; ---- 高半区映射（内核搬高半区，为将来用户态让出低地址）----
    ;   PDPT_hi[510] **直接复用已有的 PD0(0x42000)** —— PD0 覆盖物理 0..1GB，
    ;   而内核镜像在 0x100000、堆在 0x05000000、页池起点 0x08000000，全在里面。
    ;   这样一条存储就够，省掉"再建一张 PD + 填 24 项"的代码（loader64 有 4096 字节
    ;   上限，实测多出来的代码会把镜像顶到 4106 字节而被构建脚本拒绝）。
    ;   公式：VA = 0xFFFFFFFF80000000 + PA（因为 PD0 从物理 0 开始）
    mov dword [0x40000 + 511*8], 0x46000 | 3     ; PML4[511] -> PDPT_hi
    mov dword [0x46000 + 510*8], 0x42000 | 3     ; PDPT_hi[510] -> PD0（物理 0..1GB）
    ret

; ---- 进入长模式（32 位段代码到此为止）----
; ---- 进入长模式（32 位段代码到此为止）----
; 每一步都在串口留标记：VMware 上没有 0x402 debugcon，一旦在 32 位段卡住，
; 没有标记就只能看到"屏幕没反应"。实测踩过：VMware 若未把 CPUID 的长模式位
; 报出来（guestOS 选了 32 位类型），旧代码会静默 hlt，完全看不出原因。
lm64_enter_long:
    mov esi, m32_enter
    call ser32_str
    call lm64_cpuid
    jc lm64_fatal
    mov esi, m32_cpu_ok
    call ser32_str
    call lm64_build_paging
    mov esi, m32_pg_ok
    call ser32_str

    lgdt [gdt64_descriptor]
    mov eax, cr4
    or eax, 1 << 5                  ; CR4.PAE = 1
    mov cr4, eax

    mov ecx, 0xC0000080             ; IA32_EFER
    rdmsr
    or eax, 1 << 8                  ; EFER.LME = 1
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | 1           ; CR0.PG = 1（开分页 -> 进入长模式）| PE
    mov cr0, eax

    jmp 0x08:lm64_in64              ; 远跳 64 位代码段

; CPUID 检测失败：必须"说话"再停机，否则只剩屏幕上没反应这个线索
lm64_fatal:
    call ser32_str                 ; esi = lm64_cpuid 设好的失败原因字符串
    mov esi, m32_edx_pre
    call ser32_str
    mov eax, [cpuid_edx]
    call ser32_hex32
    mov esi, m32_maxleaf_pre
    call ser32_str
    mov eax, [cpuid_maxleaf]
    call ser32_hex32
    mov esi, m32_nl
    call ser32_str
    mov al, '!'
    mov dx, 0x402
    out dx, al
    cli
    hlt
    jmp $

; ============================================================================
; 以下全部是 64 位代码
; ============================================================================
[bits 64]

lm64_in64:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov rsp, 0x7C000                ; 临时栈（内核入口会立刻换成同一个值）

    call ser64_init
    mov rsi, msg_lm64
    call dbg64_puts                 ; 阶段标记（debugcon + 串口）

    ; ---- 介质描述符（0x0F00，由引导桩 boot/cdiso.asm 写入）决定内核从哪读 ----
    ;   布局："VMMD" + kind(1) + drive(1) + rsv(2) + kernel_lba(4) + payload_lba(4)
    ;          + payload_secs(4) + image_bytes(4)
    ;     kind = 1 光盘（用 ATAPI 从光盘读内核）
    ;     kind = 2 RAM（引导桩已经把内核与载荷搬进内存，这里直接进内核）
    ;     没有描述符 = 老路径：从硬盘 LBA 9 读（安装介质是裸盘/U 盘时）
    mov eax, dword [0x0F00]
    cmp eax, 0x444D4D56             ; 'VMMD' 小端
    jne .from_disk
    movzx ecx, byte [0x0F04]
    cmp ecx, 1
    je .from_cd
    mov rsi, msg_medium_ram
    call dbg64_puts
    jmp .kernel_ready
.from_cd:
    mov rsi, msg_medium_cd
    call dbg64_puts
    ; 描述符里的 drive 是 BIOS 传的 DL（0xE0/0x9F…），不是 ATA 通道号，
    ; 所以这里依次试 2、3、0、1 号驱动器（光盘通常挂在从通道）。
    lea r14, [cd_try_tbl]
    mov r13d, 4
.cd_retry:
    movzx r15d, byte [r14]
    mov esi, dword [0x0F08]         ; 内核在光盘上的起始扇区（每次都要复位）
    mov rdi, 0x100000
    mov ebx, KERNEL_SECTORS / 4     ; 2000 个光盘扇区 = 4MB
    call atapi64_read
    jnc .cd_ok
    mov al, 'x'
    call dbg64_putc                 ; 这个驱动器号不行，标一个 x
    inc r14
    dec r13d
    jnz .cd_retry
    jmp lm64_fatal64
.cd_ok:
    mov al, 0x0A
    call dbg64_putc
    ; ---- 顺便把载荷（system.img）也读进内存：安装程序就不用再碰 ATAPI 了 ----
    ; 为什么这么做：QEMU 的 IDE 状态机在"枚举时向光驱发过 0xEC"之后，可能对后续
    ; PACKET 命令当场回 ABRT（实测 st=0x41 / err=0x04）。loader 里的 ATAPI 通路
    ; 已经证明能连续读 4MB，所以载荷也走它，安装程序只做"内存 -> 目标盘"。
    ; （loader 有 4096 字节硬上限，这里不打印额外消息；进度点由 atapi64_read 打）
    movzx eax, word [0x0F10]        ; 载荷扇区数（512B 单位）
    add eax, 3
    shr eax, 2                      ; 换算成光盘扇区（2048B）并向上取整
    mov ebx, eax
    mov esi, dword [0x0F0C]         ; 载荷在光盘上的起始扇区
    mov rdi, 0x04000000             ; ★ 放 64MB：必须**高于内核 BSS 末尾**（实测 BSS
                                    ;   一直延伸到 ~36.4MB，内核启动清 BSS 会把载荷抹成 0）
    call atapi64_read
    jc lm64_fatal64
    mov byte [0x0F04], 2            ; 描述符改成 RAM 模式
    mov dword [0x0F0C], 0x04000000  ; ★ 载荷地址改成 **RAM 物理地址**（原来是光盘扇区号 2037）。
                                    ;   漏了这一行的话：安装程序按 kind=2 + payload=2037 去读
                                    ;   "0x000007F5 + 偏移" 那片低内存 → 拷过去全是 0/垃圾，
                                    ;   表现为"安装报告写了 8073 扇区，但目标盘内容区全 0"。
                                    ;   （搬高半区时误删过，靠对比目标盘字节才发现）
    ; 光盘路径：内核已经由上面的 atapi64_read 读进 0x100000，载荷也读好了，
    ; 直接进 .kernel_ready（**不要**再走磁盘那条 ATA 读取）。
    jmp .kernel_ready
.from_disk:
    ; 老路径：安装介质是裸盘 / U 盘时（没有介质描述符），内核在硬盘 LBA 9
    mov rsi, msg_medium_disk
    call dbg64_puts
    mov rdi, 0x100000               ; 目标
    mov rsi, 9                      ; 起始 LBA
    mov ebx, KERNEL_SECTORS         ; 扇区数（build64.sh 与 memlayout64.h 一致：8000）
    call ata64_read_lba28
    jc lm64_fatal64
.kernel_ready:
    mov al, 'K'
    call dbg64_putc
    mov al, 0x0A
    call dbg64_putc
    ; ---- RSDP 传递槽：显式清 0（物理 0x7800，8 字节 u64，约定见 kernel/memlayout64.h 的
    ;      ML64_RSDP_PTR_PHYS）。BIOS 路径没有 EFI 配置表可查，写 0 = 告诉内核"自己扫
    ;      EBDA/0xE0000 legacy 窗口"；不清则可能残留固件/上次运行的垃圾值被误当 RSDP 地址。
    xor eax, eax
    mov [0x7800], rax
    ; ★ 跳**虚拟**地址：内核已搬高半区（链接在 0xFFFFFFFF80000000，物理装载仍是 0x100000，
    ;   lm64_build_paging 已建好这段映射）。
    ; ★ 必须用**寄存器间接跳**：`jmp 0xFFFFFFFF80000000` 这种 64 位绝对地址会被 NASM
    ;   按 rel32 编码，目标截断成 0x80000000（反汇编实测 `e9 a9 63 ff 7f`）→ 跳到 2GB
    ;   空白处执行垃圾 → 静默死机。搬高半区时踩过。
    mov rax, 0xFFFFFFFF80100000
    jmp rax                         ; -> kernel/entry64.asm 的 lm64_entry

lm64_fatal64:
    mov al, '!'
    call dbg64_putc
.hang:
    cli
    hlt
    jmp .hang

; ---------------------------------------------------------------------------
; 64 位辅助代码
; ---------------------------------------------------------------------------

; 串口初始化（COM1，115200 8N1）
ser64_init:
    push rax
    push rdx
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8
    mov al, 1
    out dx, al
    mov dx, 0x3F9
    xor al, al
    out dx, al
    mov dx, 0x3FB
    mov al, 3
    out dx, al
    mov dx, 0x3FA
    mov al, 0xC7
    out dx, al
    mov dx, 0x3FC
    mov al, 0x0B
    out dx, al
    pop rdx
    pop rax
    ret

; al = 字符 -> 0x402（debugcon）与 COM1
dbg64_putc:
    push rax
    push rdx
    push rcx
    mov ah, al
    mov dx, 0x402
    out dx, al
    mov al, ah
    mov dx, 0x3FD
    mov ecx, 0x10000
.w:
    in al, dx
    test al, 0x20
    jnz .ok
    loop .w
.ok:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop rcx
    pop rdx
    pop rax
    ret

; rsi = 以 0 结尾的字符串
dbg64_puts:
    push rax
    push rsi
.l:
    mov al, [rsi]
    test al, al
    jz .d
    call dbg64_putc
    inc rsi
    jmp .l
.d:
    pop rsi
    pop rax
    ret

; 约 400ns 延时：连读 4 次交替状态寄存器
ata64_delay:
    push rax
    push rdx
    mov dx, 0x3F6
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop rdx
    pop rax
    ret

; 等 BSY=0；CF=0 成功 / CF=1 超时
ata64_wait_bsy:
    push rax
    push rcx
    push rdx
    mov ecx, 0x2000000
.w:
    mov dx, 0x1F7
    in al, dx
    test al, 0x80
    jz .ok
    loop .w
    pop rdx
    pop rcx
    pop rax
    stc
    ret
.ok:
    pop rdx
    pop rcx
    pop rax
    clc
    ret

; 等 DRQ=1（或 ERR）；CF=0 成功 / CF=1 失败
ata64_wait_drq:
    push rax
    push rcx
    push rdx
    mov ecx, 0x4000000
.w:
    mov dx, 0x1F7
    in al, dx
    test al, 0x01                   ; ERR
    jnz .fail
    test al, 0x08                   ; DRQ
    jnz .ok
    loop .w
.fail:
    pop rdx
    pop rcx
    pop rax
    stc
    ret
.ok:
    pop rdx
    pop rcx
    pop rax
    clc
    ret

; ---------------------------------------------------------------------------
; 64 位 ATA PIO 读盘（主通道 0x1F0，LBA28，轮询）
;   rdi = 目标物理地址, rsi = 起始 LBA, rbx = 扇区数
;   CF=0 成功 / CF=1 失败
; 端口序列与 kernel/ata64.cpp 一致（主盘选择字节必须是 0xE0 = LBA 模式）。
; ---------------------------------------------------------------------------
ata64_read_lba28:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi

.chunk:
    test rbx, rbx
    jz .done
    mov r10, rbx
    cmp r10, 128
    jbe .cnt_ok
    mov r10, 128                    ; 每块最多 128 扇区（LBA28 计数寄存器 8 位）
.cnt_ok:
    call ata64_wait_bsy
    jc .fail

    mov rax, rsi
    shr rax, 24
    and al, 0x0F
    or al, 0xE0                     ; ★ LBA 模式（0xE0），不能写成 0xA0（CHS 会被 ABRT）
    mov dx, 0x1F6
    out dx, al
    call ata64_delay

    mov dx, 0x1F1                  ; 特性
    xor al, al
    out dx, al
    mov rax, r10                   ; 扇区数
    mov dx, 0x1F2
    out dx, al
    mov rax, rsi                   ; LBA[7:0]
    mov dx, 0x1F3
    out dx, al
    mov rax, rsi
    shr rax, 8                     ; LBA[15:8]
    mov dx, 0x1F4
    out dx, al
    mov rax, rsi
    shr rax, 16                    ; LBA[23:16]
    mov dx, 0x1F5
    out dx, al

    mov dx, 0x1F7
    mov al, 0x20                   ; READ SECTORS
    out dx, al

.sector:
    call ata64_wait_drq
    jc .fail
    mov dx, 0x1F0
    mov r9d, 256
.word:
    in ax, dx
    mov [rdi], ax
    add rdi, 2
    dec r9d
    jnz .word
    inc rsi
    dec rbx
    jz .done
    dec r10
    jnz .sector
    mov al, '.'
    call dbg64_putc                ; 每块一个点（进度可见）
    jmp .chunk

.done:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    clc
    ret

.fail:
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    stc
    ret

%include "loader64_atapi.inc"
; ---------------------------------------------------------------------------
; 数据
; ---------------------------------------------------------------------------
align 4
cd_try_tbl:  db 2, 3, 0, 1
msg_lm64: db "L:lm64", 13, 10, 0

; 64 位 GDT（长模式描述子：代码段 L=1，数据段只用于访问权限）
align 16
gdt64_start:
    dq 0x0000000000000000           ; 0x00 空描述符
gdt64_code:
    dq 0x00209A0000000000           ; 0x08 64 位代码
gdt64_data:
    dq 0x0000920000000000           ; 0x10 数据
    dq 0x0000000000000000           ; 0x18 保留（将来放 TSS）
gdt64_end:

gdt64_descriptor:
    dw gdt64_end - gdt64_start - 1
    dq gdt64_start                  ; 长模式下 GDT 基址必须是 64 位线性的
