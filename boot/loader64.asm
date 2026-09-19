; loader.asm - Vimtu64 Loader
;
; 实模式阶段（BIOS 只在这个阶段用）:
;   1) 串口初始化 (COM1) + 开启 A20 地址线
;   2) E820 收集内存地图 -> 0x2000
;   3) VBE 探测并设置图形模式（按显示器 EDID/模式表自适应）+ 读 EDID
;   4) BootInfo -> 0x1000
;   5) 读内核。三条路（由 0x0F00 的介质描述符区分，参见 boot/cdiso.asm）：
;        * 无描述符 = **磁盘启动**（装好的系统盘 / 裸盘安装介质）
;          -> BIOS INT 13h 扩展读（AH=0x42 + DAP）：分块读进低内存暂存区 0x20000，
;             每批（≤13 块 × 32KB）进一次保护模式搬到 0x100000（见 int13_load_kernel）。
;          ★ 这是"SATA=AHCI 的机器也能启动已安装系统"的关键：固件负责认盘/读盘，
;            我们不再碰 IDE 兼容端口（旧版的 PATA PIO 只认那些端口，AHCI 下读不到内核）。
;        * kind=1 = 光盘启动 -> 进长模式后用 ATAPI(PACKET) 读（见 loader64_atapi.inc）
;        * kind=2 = RAM 源（hybrid ISO / U 盘）-> 引导桩 cdiso.asm 已用 INT 13h 搬好内核/载荷
;
; 保护模式阶段（此后不再返回实模式，也再不用任何 BIOS 中断）:
;   6) 远跳 0x08:0x100000 进入内核
;
; 为什么"BIOS 读盘"曾经被换掉，现在又换回来（两次都是实测驱动）:
;   旧写法是"每 pass 进出保护模式拷贝"，实测在 VMware 上，只要做过一次"进/出保护模式"的往返，
;   它的 BIOS 之后的中断服务（INT 13h 读盘、INT 10h VBE）就会**永久挂住**（还原 GDTR、
;   复位 FS/GS、重新开中断都无效，连一次只拷 0 字节的往返也足以触发）。SeaBIOS 不依赖这些
;   状态，所以旧写法在 QEMU 上一直正常。当时于是改成"自带 PATA PIO 读盘、全程不碰 BIOS"。
;   但 PIO 在 SATA=AHCI 的真机上根本读不到盘（装完系统起不来），所以本版本改回来：
;   **内核整块读完之前不进保护模式、进保护模式之后不再调 BIOS** —— 两个雷都避开。
;   （顺序细节见 int13_load_kernel 的注释。）
;
; loader 加载在 0x9000（避开 SeaBIOS trampoline 区 0x8000-0x9000），
; 整个 loader 必须 < 0x7000 字节，否则 16 位远跳偏移会越界。
; 体积上限：loader64.bin ≤ 4096 字节（build64.sh 会检查）。
;
; 实模式内存布局:
;   0x0500  A20 别名测试用（先存后恢复，别动别人的内存）
;   0x0600  DAP（boot/boot.asm 也用同一位置；loader 进长模式前用它给 BIOS 传读盘参数）
;   0x0F00  介质描述符（由引导桩 cdiso.asm 写；"VMMD" + kind + drive + …）
;   0x1000  BootInfo
;   0x2000  E820 内存条目 (最多 64 * 20B)
;   0x20000..0x87FFF  INT 13h 暂存区（416KB：磁盘启动时 BIOS 只能写低内存，见 int13_load_kernel）
;   0x0600  hybrid MBR（boot/hybrid_mbr.asm）把自己搬到这里再工作；loader 不用它
;   （loader 给 BIOS 传读盘参数用的 DAP 在 loader 自己的数据区里，见下面的 dap:）
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

KERNEL_SECTORS equ 8000             ; 内核区总扇区数（4MB）。与 build64.sh 的 KERNEL_SECTORS、
                                    ; kernel/memlayout64.h 三方一致，启动时**读满**这个数
KERNEL_LBA     equ 9                ; 内核起始 LBA（= build64.sh 的 KERNEL_LBA；LBA 布局不许改）

; ---- INT 13h 磁盘读的暂存区分块（见 int13_load_kernel 的长注释）----
STAGE_PHYS     equ 0x20000          ; 暂存区物理地址（低内存，实模式可达；loader 在 0x9000，
                                    ; RM 栈顶 0x7BFF，都在它下面）
STAGE_SEG0     equ STAGE_PHYS / 16  ; 段:偏移 = 0x2000:0x0000（0x2000 × 16 = 0x20000）
CHUNK_SECS     equ 64               ; 单次读 64 扇区 = 32KB（≤ 127 扇区上限，且不跨 64KB 边界）
CHUNKS_MAX     equ 13               ; 每批最多 13 块 = 416KB（暂存区 0x20000..0x87FFF；
                                    ; 再往上是 PM 阶段的栈 0x90000，别踩）
SEG_STEP       equ 0x800            ; 暂存窗口段步进 = 32KB / 16

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

    ; ---- 3. 读内核（三条路，由 0x0F00 的介质描述符区分；见 boot/cdiso.asm）----
    ;   a) 无描述符 = **磁盘启动**（装好的系统盘 / 裸盘安装介质）：走 BIOS INT 13h 扩展读，
    ;      在这里（还在实模式时）就把内核读进 0x100000。这条路的失败会自己打 [LM] 点并停机。
    ;      ★ 为什么在这一步用 BIOS：PIO 只认 IDE 兼容端口的盘，SATA=AHCI 的机器上读不到内核；
    ;        而 BIOS 认识所有自己能引导的盘（SATA/AHCI、固件映射过的 NVMe、USB）。
    ;   b) kind=1 = 光盘启动：内核在光盘上，交给 64 位阶段的 ATAPI(PACKET) 读（CD 不是磁盘，
    ;      INT 13h 的软盘/硬盘读法对 2048B 扇区不适用）。
    ;   c) kind=2 = RAM 源（hybrid ISO / U 盘）：引导桩 cdiso.asm 已经用 INT 13h 把内核与
    ;      载荷搬进内存了，这里什么都不用做。
    ;     ★ VMware 的 BIOS 在"进/出保护模式往返"之后就挂住（INT 13h/INT 10h 全废，实测）；
    ;       int13_load_kernel 内部为了搬高内存会做保护模式往返，但它把内核整块读完才返回，
    ;       返回后我们只进保护模式不再回头 —— 所以"BIOS 调用"与"保护模式往返"不交错踩雷。
    mov eax, [0x0F00]
    cmp eax, 0x444D4D56             ; 'VMMD' 小端
    jne .disk_boot                  ; 没有描述符 -> 磁盘启动
    cmp byte [0x0F04], 1
    jne .kernel_ready_rm            ; kind=2：桩已经把内核搬好了
    mov si, msg_lm_cd               ; kind=1：光盘 -> 打点后由 ATAPI 读
    call dbg_str
    jmp .kernel_ready_rm
.disk_boot:
    call int13_load_kernel          ; 失败时内部打 [LM] int13 read FAILED 并停机
.kernel_ready_rm:
    ; L:ata —— 既有验收脚本（tests/boot64_assert.py 的 must_debugcon）依赖这个标记。
    ; 语义已随本次改动变为"读内核阶段结束、准备切长模式"（读盘本身已由 BIOS INT 13h / ATAPI 完成）。
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
    ; 请求 VBE 2.0 控制器信息（原来这里把 AX 返回值同时打给屏幕和串口，已随体积预算删掉：
    ;   失败路径由 msg_vbe_err + best_mode 打点，成功路径由后面的 [LM] / [LM64] 打点覆盖）
    mov di, 0x7000
    mov dword [di], 'VBE2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .fail
    cmp dword [0x7000], 'VESA'
    jne .fail
    call read_edid                  ; 读显示器 EDID（供模式自适应）
    ; 模式列表指针 (offset:segment) 位于偏移 14
    mov ax, [0x7000 + 14]
    mov bx, [0x7000 + 16]
    mov es, bx
    mov di, ax
    ; ★ 体积预算：这里原来有一大段"列表指针 + 前 4 个模式号 + LIST seg,off="的调试打印
    ;   （还有配套的 msg_dbg2/msg_dbg3/msg_colon/msg_space/msg_nl/msg_d_list 字符串）。
    ;   loader64.bin 有 4096 字节硬上限，而模式是否可用已经由内核的
    ;   [LM64] LFB addr=… 与 [DISP64] 模式清单如实打出，所以整段删掉。
    mov word [best_mode], 0
    mov word [ml_count], 0         ; 已收集的可用模式数
    mov word [best_score], 0       ; score 均为正数，0 表示未选
.mode_loop:
    mov cx, [es:di]
    cmp cx, 0xFFFF
    je .done
    ; （原来这里把模式号打给串口，已删）
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
    ; （原来这里把 int 10h 的返回码打给串口，已删）
    cmp ax, 0x004F
    jne .next_mode
    ; 模式属性
    mov ax, [0x7200]                ; 模式属性（原来这里还把它打给串口，已随体积预算删掉）
    test ax, 0x0001
    jz .next_mode
    test ax, 0x0080            ; 需要 LFB
    jz .next_mode
    ; （这里原来打印 " ok(W H bpp" 三个十六进制 —— 已删；同样的值 427 行起就写进
    ;   m_width/m_height/m_bpp 并参与评分，内核启动后会打真实画面参数）
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
    ; （原来这里打印 "BEST=xxxx" —— 体积预算下删掉：模式选择结果由内核的
    ;   [LM64] LFB addr=… / [DISP64] 模式清单打，屏幕上的串口打点已经够定位问题）
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

; ============================================================================
; BIOS INT 13h 扩展读（AH=0x42 + DAP）：磁盘启动路径把内核读进 0x100000
; ============================================================================
; 为什么不再用自己的 PATA PIO（旧版本的写法，代码已删）：
;   PIO 只认 IDE 兼容端口（0x1F0/0x170）后面挂着的盘。现代主板 SATA 默认工作在 AHCI
;   模式，那些端口后面什么都没有 —— 于是"在 SATA 盘上装完系统，重启就起不来"。
;   改走 BIOS INT 13h 之后，**凡是固件能看见并能引导的盘**（SATA/AHCI、固件映射过的
;   NVMe、USB、老 PATA）都能读：控制器/端口细节交给固件，我们只给一个 DAP。
;
; 实模式的两条硬限制（决定了下面"分块 + 保护模式搬运"的写法）：
;   1) DAP 的目标是 16 位 段:偏移（物理 = 段×16 + 偏移，20 位寻址，最大 0x10FFEF），
;      **写不到 0x100000 以上的高内存**：0x100000 / 16 = 0x10000 已经放不进 16 位段寄存器。
;      （0x04000000 同理，载荷那条路在 cdiso.asm 里也是这么绕的。）
;      所以先把数据读进低内存暂存区，再自己搬到高内存 —— 暂存区的换算就是
;      物理 0x20000 = 段:偏移 0x2000:0x0000（0x2000 × 16 = 0x20000）。
;   2) 每次读的缓冲区**不能跨 64KB 边界**（BIOS 用段式拷贝/DMA，跨界会撕数据），
;      且多数实现要求 count ≤ 127 扇区（127 × 512 = 65024 < 64KB）。
;      这里固定每块 64 扇区 = 32KB，暂存窗口按 32KB 递增（段步进 0x800）、偏移恒为 0：
;      每块正好落在某个 64KB 对齐窗口的一半里，**永不跨界**。
;
; 搬运为什么必须进保护模式：实模式段:偏移到不了 0x100000（见上），32 位寻址必须 PE=1。
;   所以每读完一批就进一次保护模式、用 GDT 里的 4GB 平坦数据段（0x10）做一次
;   a32 rep movsd 搬到高内存，再回实模式继续读 —— 与 boot/cdiso.asm 的 copy_chunk_high
;   同一套路（那条路已在 QEMU 上跑通）。
;   ★ 顺序很关键：**一旦进过保护模式就不要再调 BIOS**（VMware 的 BIOS 在保护模式往返后
;     会挂住，见本文件开头）。所以这里把内核整块读完才进保护模式进长模式，之后永不回头；
;     批次内的"读 → 搬 → 再读"是"读在前、搬在后"，每次 BIOS 调用前刚回到实模式。
;
; 失败处理：每块重试 3 次（每次先复位磁盘 AH=0x00），仍失败 -> 打 [LM] 串口点并停机。
;   **绝不静默失败**：这条路上没有屏幕输出（图形模式已开），串口/debugcon 是唯一出口。
; ============================================================================
int13_load_kernel:
    ; ---- 驱动器号：用 BIOS 传进来的 DL（boot/boot.asm 存进 BOOT_DRIVE 透传过来）----
    ;   不猜 0x80：BIOS 的盘号与内核的驱动器号没有固定映射，而 DL 就是固件刚刚用来引导
    ;   我们的那块盘的号 —— 这是唯一可靠的来源。若 DL 不是硬盘号（< 0x80：软盘/CD 号），
    ;   说明固件没给我们可用的磁盘号，回退 0x80 并在打点里注明 (fallback)。
    mov si, msg_lm_i13
    call dbg_str
    mov al, [BOOT_DRIVE]
    test al, 0x80
    jnz .dl_ok
    mov al, 0x80
    mov [BOOT_DRIVE], al
    call dbg_hex8
    mov si, msg_dl_fb
    call dbg_str
    call dbg_nl
    jmp .dl_done
.dl_ok:
    call dbg_hex8
    call dbg_nl
.dl_done:
    ; ---- 起点：LBA 9（KERNEL_LBA）、目标物理 0x100000、共 8000 扇区（KERNEL_SECTORS）----
    mov dword [l13_lba], KERNEL_LBA
    mov dword [l13_dst], 0x00100000
    mov word [l13_left], KERNEL_SECTORS
.batch:
    mov word [l13_n], 0             ; 本批从暂存区头开始
    mov word [l13_seg], STAGE_SEG0
.chunk:
    mov ax, [l13_left]
    test ax, ax
    jz .flush                       ; 没有剩余 -> 收工
    mov bx, CHUNK_SECS              ; 本块扇区数（尾部可能更少）
    cmp ax, bx
    jae .cnt_ok
    mov bx, ax
.cnt_ok:
    mov [dap_count], bx             ; DAP.count（≤ 64）
    mov ax, [l13_seg]
    mov [dap_seg], ax               ; DAP.段（0x2000 + n*0x800 -> 物理 0x20000 + n*32KB）
    mov word [dap_off], 0           ; DAP.偏移（恒 0）
    mov eax, [l13_lba]
    mov [dap_lba], eax              ; DAP.LBA（32 位；高位在数据区里恒 0）
    mov byte [l13_retry], 3
.retry:
    mov dl, [BOOT_DRIVE]            ; BIOS 约定：DL = 驱动器号
    mov si, dap                     ; BIOS 约定：DS:SI = DAP
    mov ah, 0x42                    ; 扩展读（LBA + DAP）
    int 0x13
    call rm_flat                    ; BIOS 可能踩坏段寄存器（不改 FLAGS，可在 jc 前调用）
    jc .fail
    ; ---- 打点：只打整个加载的首尾各一条（4000 次读全打会刷屏，且拖慢 TCG）----
    mov ax, [l13_left]
    cmp ax, [dap_count]             ; 剩余 == 本次读：这次就是最后一块
    je .log
    cmp dword [l13_lba], KERNEL_LBA ; 第一块
    jne .adv
.log:
    mov si, msg_i13_ok
    call dbg_str
    mov ax, word [dap_lba]
    call dbg_hex16                  ; LBA（9..8009，16 位足够；已按低 16 位打印）
    mov si, msg_i13_cnt
    call dbg_str
    mov ax, [dap_count]
    call dbg_hex16
    mov si, msg_i13_tail
    call dbg_str
    call dbg_nl
.adv:
    movzx eax, word [dap_count]
    add [l13_lba], eax              ; LBA += 本次扇区数
    mov ax, [dap_count]
    sub [l13_left], ax              ; 剩余 -= 本次扇区数
    add word [l13_seg], SEG_STEP    ; 下一个 32KB 暂存窗口
    inc word [l13_n]
    cmp word [l13_n], CHUNKS_MAX
    jb .chunk                       ; 暂存区还没满 -> 继续读
.flush:
    ; ---- 本批读完：一次保护模式拷贝把暂存区搬到高内存（暂存区永远从 0x20000 起）----
    movzx eax, word [l13_n]
    test eax, eax
    jz .done                        ; 没有新数据（剩余为 0）-> 收工
    shl eax, 15                     ; 字节数 = n * 32KB
    push eax
    mov ecx, eax
    mov edi, [l13_dst]
    call copy_stage_high
    pop eax
    add [l13_dst], eax              ; 目标前进
    cmp word [l13_left], 0
    jnz .batch
.done:
    ret
.fail:
    ; CF=1：AH = BIOS 错误码（已经先存下来，后面的调用不会污染它）
    mov [l13_ah], ah
    mov si, msg_i13_fail
    call dbg_str
    mov al, [l13_ah]
    call dbg_hex8
    mov si, msg_i13_lba
    call dbg_str
    mov ax, word [dap_lba]
    call dbg_hex16
    mov si, msg_i13_retry
    call dbg_str
    mov al, [l13_retry]
    call dbg_hex8
    call dbg_nl
    dec byte [l13_retry]
    jz .dead
    xor ah, ah                      ; AH=0x00 复位磁盘（有些 BIOS 失败后不先复位就不认下一次读）
    mov dl, [BOOT_DRIVE]
    int 0x13
    call rm_flat
    jmp .retry
.dead:
    mov si, msg_i13_dead
    call dbg_str
    cli
    hlt
    jmp $

; ---- DS/ES 拉回实模式 0 段（BIOS 调用可能踩坏段寄存器）----
; ★ 全程用 mov（**不用 xor**）：本函数必须在 `jc` 之前调用，不能改 FLAGS（CF/ZF 都要留着）
rm_flat:
    push ax
    mov ax, 0
    mov ds, ax
    mov es, ax
    pop ax
    ret

; ---- 暂存区（低内存）-> 高内存的搬运：保护模式里做 32 位拷贝 ----
; 入：ecx = 字节数（32KB 的整数倍）、edi = 目标物理地址（0x100000；载荷路径另算）
; 段缓存说明：进 PE 时**不**远跳，CS 缓存仍是实模式的 16 位代码段，所以下面依然是 16 位
;   代码（配上 32 位操作数/地址前缀）—— 与 boot/cdiso.asm 的 copy_chunk_high 完全同路。
copy_stage_high:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 1                       ; PE=1
    mov cr0, eax
    jmp short $+2
    mov ax, 0x10                    ; 4GB 平坦数据段（gdt_data：G=1 / D=1 / limit=0xFFFFF）
    mov ds, ax
    mov es, ax
    mov esi, STAGE_PHYS             ; 源：0x20000（= 段:偏移 0x2000:0x0000）
    shr ecx, 2                      ; 按 dword 搬（字节数一定是 4 的倍数：32KB 的整数倍）
    a32 rep movsd
    mov eax, cr0
    and al, 0xFE                    ; PE=0（回实模式）
    mov cr0, eax
    xor ax, ax                      ; 回实模式后段缓存会按选择子重建 -> 显式拉回 0 段
    mov ds, ax
    mov es, ax
    sti
    ret

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

; 打印 al 的低 8 位为**两位**十六进制（dbg_hex16 是 4 位 + 一个只进 debugcon 的空格，
; 对 "dl=0x80" / "ah=0xC4" / "retry=03" 这种字段不合适：串口那侧空格不落地，会粘在一起）
dbg_hex8:
    push ax
    mov ah, al
    shr al, 4
    call .n
    mov al, ah
    and al, 0x0F
    call .n
    pop ax
    ret
.n:
    cmp al, 10
    jb .d
    add al, 'A' - 10
    jmp .o
.d:
    add al, '0'
.o:
    mov dx, 0x402
    out dx, al
    SPUTC16
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
; ---- 磁盘地址包（DAP，INT 13h AH=0x42 用）----
;   size=0x10 / count / offset:segment / LBA 低 32 位 / LBA 高 32 位
;   count 每次读前由 int13_load_kernel 填（≤ 64 扇区 = 32KB：既满足"单次 ≤ 127 扇区"，
;   又保证只落在一个 64KB 对齐窗口里的一半，绝不跨 64KB 边界 —— 见那边的注释）
align 4
dap:
    db 0x10
    db 0
dap_count:
    dw CHUNK_SECS
dap_off:
    dw 0
dap_seg:
    dw STAGE_SEG0
dap_lba:
    dd KERNEL_LBA                   ; 起始 LBA（= build64.sh 的 KERNEL_LBA / memlayout64.h 一致）
    dd 0                            ; LBA 高 32 位（内核在 9..8009，恒 0）

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

BOOT_DRIVE: db 0                ; BIOS 传进来的 DL（boot/boot.asm 保存后透传）—— 磁盘启动读盘用
l13_lba:   dd KERNEL_LBA        ; INT 13h 读内核：当前 LBA（32 位足够：9..8009）
l13_dst:   dd 0x00100000        ; 目标物理地址（高内存；实模式写不到，见 int13_load_kernel）
l13_left:  dw KERNEL_SECTORS    ; 还差多少扇区（8000 < 65536，16 位够）
l13_n:     dw 0                 ; 本批已读进暂存区的块数（每块 64 扇区 = 32KB）
l13_seg:   dw STAGE_SEG0        ; 本块暂存窗口的段值（0x2000 + n*0x800 = 物理 0x20000 + n*32KB）
l13_retry: db 3                 ; 本块剩余重试次数
l13_ah:    db 0                 ; 失败时 BIOS 返回的 AH 错误码（打点用）
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
msg_vbe_err:   db "VBE error! best_mode=0x", 0
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
; （VBE 模式枚举的调试字符串全部删除 —— 与之配套的打印点在 vbe_probe 里已随
;   4096 字节体积预算去掉。原来这里是：msg_dbg1/msg_dbg2/msg_dbg3/msg_colon/msg_space/
;   msg_nl/msg_d_begin/msg_d_ax/msg_d_sigok/msg_d_list/msg_d_mode/msg_d_ret/msg_d_attr/
;   msg_d_ok/msg_d_score/msg_d_sc2/msg_d_bpp2/msg_d_best。）

; ---- INT 13h 磁盘引导路径的打点（[LM] 前缀，与光盘路径的 [LM] cd boot via ATAPI 区分）----
; 体积预算：loader 有 4096 字节硬上限，所以把这些字符串拆成可复用的片段
;   （" lba=" / " count=" 两条打点共用），数字一律由 dbg_hex8 / dbg_hex16 打。
msg_lm_i13:    db "[LM] disk boot via INT 13h dl=0x", 0
msg_lm_cd:     db "[LM] cd boot via ATAPI", 13, 10, 0
msg_dl_fb:     db " (fallback)", 0
msg_i13_ok:    db "[LM] int13 read lba=", 0
msg_i13_cnt:   db " count=", 0
msg_i13_tail:  db " ok", 13, 10, 0
msg_i13_fail:  db "[LM] int13 read FAILED ah=0x", 0
msg_i13_lba:   db " lba=", 0
msg_i13_retry: db " retry=", 0
msg_i13_dead:  db "[LM] int13 read FAILED: kernel not loaded, halted", 13, 10, 0


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
    ;     kind = 1 光盘（用 ATAPI 从光盘读内核；实模式阶段已打 "[LM] cd boot via ATAPI"）
    ;     kind = 2 RAM（引导桩已经用 INT 13h 把内核与载荷搬进内存，这里直接进内核）
    ;     没有描述符 = **磁盘启动**（装好的系统盘 / 裸盘安装介质）：内核在实模式阶段已经由
    ;       BIOS INT 13h 扩展读送进 0x100000（见 int13_load_kernel），这里只管进内核。
    mov eax, dword [0x0F00]
    cmp eax, 0x444D4D56             ; 'VMMD' 小端
    jne .kernel_ready               ; 磁盘启动：INT 13h 已经把内核读好了
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
    ; 光盘路径：内核已经由上面的 atapi64_read 读进 0x100000，载荷也读好了，直接进 .kernel_ready。
    ; （磁盘启动那条路的内核读取**不在这里**：实模式阶段已经用 BIOS INT 13h 扩展读完成，
    ;   见 int13_load_kernel。）
    jmp .kernel_ready
.kernel_ready:
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

; ---------------------------------------------------------------------------
; （原 64 位 ATA PIO 读盘整段已删除：ata64_delay / ata64_wait_bsy /
;   ata64_wait_drq / ata64_read_lba28）
;
; 磁盘启动路径改走 BIOS INT 13h 扩展读：见 16 位阶段的 int13_load_kernel。
; 原因（真机可用性）：PIO 只认 IDE 兼容端口（0x1F0/0x170）后面挂着的盘。现代主板 SATA 默认
;   工作在 AHCI 模式，那些端口后面什么都没有 —— 用 PIO 读内核就等于"装完系统重启起不来"。
;   交给固件读之后，凡是 BIOS 能引导的盘（SATA/AHCI、固件映射过的 NVMe、USB、老 PATA）
;   都能启动，控制器细节由固件负责。
; 光盘路径不受影响：ATAPI(PACKET) 在 loader64_atapi.inc 里，光盘不是磁盘，也不需要 BIOS。
; ★ 这里**没有** PIO 回退：本版本只支持"BIOS 提供 INT 13h 扩展读(AH=0x42)"的机器
;   （1998 年后的固件基本都有；那之前的老机器也跑不了本系统的 64 位长模式）。
; ---------------------------------------------------------------------------

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
