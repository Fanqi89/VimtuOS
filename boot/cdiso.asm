; cdiso.asm - 安装介质的引导桩（ISO 的 El Torito 引导镜像；同一份代码也负责 U 盘路径）
;
; 两个入口共用一个桩：
;   A) 光盘引导（BIOS 把 ISO 当光驱）：DL = 光盘驱动器号（0xE0/0x9F…）
;      → 只把 LOADER64.BIN 读进 0x9000（低内存，INT 13h 光盘读 = 2048B 单位），
;        写"介质描述符 kind=1"，剩下的（读内核与载荷）由 loader64 用 ATAPI 干。
;   B) 硬盘/U 盘引导（把 ISO 写进 U 盘、BIOS 把它当硬盘；或直接拿 ISO 当硬盘挂）：
;      DL = 0x80..0x8F
;      → 这里把 LOADER64.BIN 读进 0x9000，再把 **KERNEL64.BIN（4MB）与 SYSTEM.IMG（4.13MB）**
;        搬进高内存（0x100000 / 0x04000000），写"介质描述符 kind=2"（RAM 源），跳 loader64。
;      U 盘在 USB 控制器后面，我们的 ATA PIO 驱动够不着，所以只能靠 BIOS INT 13h；
;      而 BIOS 的读只能写到 1MB 以下，于是这里进一次保护模式把 ds/es/fs/gs 的段限设成
;      4GB（unreal mode），再用 32 位寻址把数据从 32KB 暂存区搬到高内存。
;      （实测：QEMU 的 SeaBIOS 可用；VMware 的 BIOS 在保护模式往返后会挂住 —— 所以
;        VMware 那条路仍走光盘/虚拟光驱，见 docs/ISO安装介质说明.md。）
;
; 介质描述符（0x0F00，loader64 与安装程序都读它）：
;   0x0F00  char[4] "VMMD"
;   0x0F04  u8 kind        1 = 光盘（loader64 用 ATAPI 读内核与载荷）
;                          2 = RAM（内核已在 0x100000、载荷已在 0x04000000）
;   0x0F05  u8 drive       BIOS 传进来的 DL（光盘号或硬盘号）
;   0x0F06  u16 rsv
;   0x0F08  u32 kernel_lba 光盘：内核起始扇区（2048B）；RAM：内核物理地址
;   0x0F0C  u32 payload_lba 光盘：载荷起始扇区；RAM：载荷物理地址
;   0x0F10  u32 payload_secs 载荷扇区数（512B 单位，8073）
;   0x0F14  u32 image_bytes 备用
;
; 构建期回填（tools/make_iso64.py 按魔数写 dword）：
;   光盘路径（2048B 单位）：VMLD=桩读 loader 的 LBA、VMLB=loader 字节数、
;                            VMKN=内核 LBA、VMSY=载荷 LBA、VMSS=载荷扇区数
;   硬盘路径（512B 单位）： VML9=loader LBA、VMK9=内核 LBA、VMKB=内核字节数、
;                            VMS9=载荷 LBA、VMYB=载荷字节数
[bits 16]
[org 0x7C00]

%define HIGH_KERNEL    0x00100000    ; 内核目标（与 linker64.ld / memlayout64.h 一致）
%define HIGH_PAYLOAD   0x04000000    ; 载荷目标：必须**高于内核 BSS 末尾**（实测 ~36.4MB），
                                     ; 否则内核启动清 BSS 时会把载荷抹成 0
%define STAGE_SEG      0x2000        ; 32KB 暂存区 = 0x20000
%define CHUNK_SECS     64            ; 每次 INT 13h 读 64 扇区 = 32KB

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    cld
    mov [boot_drive], dl
    call ser_init

    mov al, [boot_drive]
    mov si, m_banner
    call puts
    mov al, [boot_drive]
    call put_hex
    call crlf

    mov al, [boot_drive]
    cmp al, 0x80
    jb .cd_path                     ; < 0x80：光盘（0xE0/0x9F…）
    cmp al, 0x90
    jb .hdd_path                    ; 0x80..0x8F：硬盘/U 盘
    jmp .cd_path

; ==================== A) 光盘路径 ====================
.cd_path:
    mov si, m_cd
    call puts
    mov eax, [vmld_val]
    mov [dap_lba], eax
    mov eax, [vmlb_val]
    add eax, 2047
    shr eax, 11                     ; 需要几个 2048B 光盘扇区
    cmp eax, 2
    jae .cnt_ok
    mov eax, 2                      ; 至少 2 个（loader ~4KB）
.cnt_ok:
    mov [dap_count], ax
    mov word [dap_off], 0x4000
    mov word [dap_seg], 0x0000
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    jc .read_fail
    mov cx, [vmlb_val]
    mov si, 0x4000
    mov di, 0x9000
    cld
    rep movsb
    ; 描述符：kind=1（光盘），地点交给 loader64 用 ATAPI 读
    mov word [0x0F00], 0x4D56
    mov word [0x0F02], 0x444D
    mov byte [0x0F04], 1
    mov al, [boot_drive]
    mov [0x0F05], al
    mov word [0x0F06], 0
    mov eax, [vmkn_val]
    mov [0x0F08], eax
    mov eax, [vmsy_val]
    mov [0x0F0C], eax
    mov eax, [vmss_val]
    mov [0x0F10], eax
    mov dword [0x0F14], 0
    mov si, m_jump
    call puts
    jmp 0x0000:0x9000

; ==================== B) 硬盘/U 盘路径 ====================
.hdd_path:
    mov si, m_hdd
    call puts

    ; ---- 关键前提（实测出来的，不是理论）----
    ;   * 全程在**实模式平坦段 DS=0/ES=0** 下跑：此时本文件的标签（org 0x7C00）正好等于
    ;     线性地址，读 vml9_val/vmkb_val 这类回填值才拿到正确内容。
    ;   * int 13h 也必须在这个状态下调用：BIOS 按"段值 × 16 + 偏移"解释 DAP 指针，它不认
    ;     保护模式段缓存；DS=0x18 会让它去 0x180+0x8018 读 DAP —— 结果"读 0 个扇区"却返回成功。
    ;   * ★ 4GB 平坦段只在 **PE=1 期间**存在：实模式下 `mov ds, 0x18` 只会得到 base = 0x180
    ;     （选择子 × 16）的普通段，而且 QEMU 在 PE 1->0 时还会把段缓存按选择子重建。
    ;     所以"进一次保护模式之后一直用 4GB 段搬高内存"这条路不成立 —— 往 0x100000/0x04000000
    ;     搬数据必须**每块重新进一次保护模式**（copy_chunk_high 干这个）。
    lgdt [gdt_desc]                 ; 保护模式拷贝要用（copy_chunk_high 里也会再设一次）

    ; ---- 1) LOADER64.BIN → 0x9000（512B 单位；纯低内存，不需要保护模式）----
    mov eax, [vml9_val]
    mov [dap_lba], eax
    mov eax, [vmlb_val]
    add eax, 511
    shr eax, 9
    mov [dap_count], ax
    mov word [dap_off], 0x4000
    mov word [dap_seg], 0x0000
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    call seg_flat                   ; BIOS 可能踩坏段寄存器（本函数不改 FLAGS，可在 jc 前调用）
    jc .read_fail
    mov cx, [vmlb_val]
    mov si, 0x4000
    mov di, 0x9000
    cld
    rep movsb
    ; loader 已到 0x9000（下面开始把内核/载荷搬进高内存）

    ; ---- 2) 内核 4MB → 0x100000 ----
    mov eax, [vmk9_val]
    mov [big_src], eax
    mov eax, [vmkb_val]
    mov [big_left], eax
    mov dword [big_dst], HIGH_KERNEL
    mov al, 'k'                     ; 串口进度：开始搬内核（4MB）
    call ser_putc
    call copy_big

    ; ---- 3) 载荷 4.13MB → 0x04000000 ----
    mov eax, [vms9_val]
    mov [big_src], eax
    mov eax, [vmyb_val]
    mov [big_left], eax
    mov dword [big_dst], HIGH_PAYLOAD
    mov al, 'p'                     ; 串口进度：开始搬载荷（4.13MB）
    call ser_putc
    call copy_big

    ; ---- 4) 写描述符：kind=2（RAM 源）----
    mov word [0x0F00], 0x4D56
    mov word [0x0F02], 0x444D
    mov byte [0x0F04], 2
    mov al, [boot_drive]
    mov [0x0F05], al
    mov word [0x0F06], 0
    mov dword [0x0F08], HIGH_KERNEL
    mov dword [0x0F0C], HIGH_PAYLOAD
    mov ax, [vmss_val]
    movzx eax, ax
    mov [0x0F10], eax
    mov dword [0x0F14], 0

    mov si, m_jump
    call puts
    jmp 0x0000:0x9000

.read_fail:
    xor ax, ax                      ; 失败时 BIOS 可能刚踩过段寄存器，先拉回实模式平坦段，保证能打印
    mov ds, ax
    mov si, m_readerr
    call puts
    jmp halt

; ---- 把 DS/ES 拉回实模式 0 段（每次 BIOS 调用之后、每次回实模式之后都要做）----
;   为什么不是"加载 0x18 恢复 4GB 段"：实模式下段基址 = 选择子 × 16，0x18 只会得到
;   base = 0x180 的普通段。4GB 平坦段只在 PE=1 期间有效（见 .hdd_path 的说明）。
;   本函数**不改 FLAGS**，所以调用者可以在 `jc .fail` 之前就调用它。
seg_flat:
    push ax
    mov ax, 0
    mov ds, ax
    mov es, ax
    pop ax
    ret

; ---- 进保护模式，把低内存 0x20000 处的一块拷到 [big_dst]，然后回实模式 ----
;   实模式下没有 4GB 段可用（选择子 × 16 才是基址），所以每一块都必须单独走一次
;   "PE=1 -> 用 0x18 平坦段做 32 位拷贝 -> PE=0"。PE 往返只花几百个周期，可以忽略。
;   返回时 DS/ES 已拉回 0 段，调用者可以继续用标签访问。
copy_chunk_high:
    cli
    lgdt [gdt_desc]
    mov eax, cr0
    or al, 1
    mov cr0, eax                    ; PE=1
    jmp short $+2                   ; CS 缓存仍是实模式的 D=0：下面依然是 16 位代码
    mov ax, 0x18                    ; ★ 只有现在（PE=1）加载才是真正的 4GB 平坦段
    mov ds, ax
    mov es, ax
    mov esi, STAGE_SEG * 16         ; 0x20000
    mov edi, [big_dst]
    movzx ecx, word [dap_count]
    shl ecx, 9                      ; 本次字节数
    shr ecx, 2                      ; 按 dword 搬（512B 的整数倍 -> 一定是 4 的倍数）
    a32 rep movsd                   ; ★ 这一条就是搬运本体（别删：清理诊断打点时误删过一次，
                                    ;   症状是内核/载荷根本没被搬进高内存、loader 一跳就静默死机）
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax                    ; PE=0 回实模式
    mov ax, 0
    mov ds, ax                      ; 回实模式后段缓存会被按选择子重建 -> 显式拉回 0 段
    mov es, ax
    sti
    ret

; ---- 大块搬运：每次读 CHUNK_SECS 个 512B 扇区到低内存 0x20000，再拷到 [big_dst]（保护模式）----
;   入：big_src = 源 LBA（512B 单位）、big_left = 剩余字节数、big_dst = 目标物理地址
copy_big:
.loop:
    mov eax, [big_left]
    test eax, eax
    jz .done
    mov bx, CHUNK_SECS
    cmp eax, CHUNK_SECS * 512
    jae .cnt_ok
    add eax, 511
    shr eax, 9
    mov bx, ax
.cnt_ok:
    mov [dap_count], bx
    mov word [dap_off], 0x0000
    mov word [dap_seg], STAGE_SEG
    mov eax, [big_src]
    mov [dap_lba], eax
    mov si, dap
    mov ah, 0x42
    mov dl, [boot_drive]
    int 0x13
    call seg_flat                   ; BIOS 可能踩坏段寄存器（不改 FLAGS -> 可在 jc 前调用）
    jc .fail
    call copy_chunk_high            ; 0x20000 -> [big_dst]（保护模式里做）

    ; ---- 推进指针：源按扇区数、目标/剩余按字节数（DS=0，标签就是绝对地址）----
    movzx ebx, word [dap_count]
    movzx ecx, bx
    shl ecx, 9                      ; 本次字节数（不要用被 rep 消耗过的寄存器）
    mov eax, [big_dst]
    add eax, ecx
    mov [big_dst], eax
    mov eax, [big_left]
    sub eax, ecx
    mov [big_left], eax
    mov eax, [big_src]
    add eax, ebx
    mov [big_src], eax

    inc word [dot_cnt]
    mov ax, [dot_cnt]
    and ax, 7                       ; 每 8 个 32KB 块打一个点（4 个点 = 1MB）
    jnz .loop
    mov al, '.'
    call ser_putc
    jmp .loop
.done:
    ret
.fail:
    call seg_flat
    mov si, m_readerr
    call puts
    jmp halt
halt:
    cli
    hlt
    jmp halt

; ==================== 串口 ====================
ser_init:
    pusha
    push dx
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
    pop dx
    popa
    ret

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

puts:
    pusha
.l:
    lodsb
    test al, al
    jz .d
    call ser_putc
    jmp .l
.d:
    popa
    ret

crlf:
    mov al, 13
    call ser_putc
    mov al, 10
    call ser_putc
    ret

put_hex:
    pusha
    mov ah, al
    shr al, 4
    call .n
    mov al, ah
    and al, 0x0F
    call .n
    popa
    ret
.n:
    cmp al, 10
    jb .dd
    add al, 'A' - 10
    jmp .o
.dd:
    add al, '0'
.o:
    call ser_putc
    ret

put_hex_dword:
    pusha
    mov ebx, eax
    mov ecx, 8
.loop:
    rol ebx, 4
    mov al, bl
    and al, 0x0F
    cmp al, 10
    jb .d
    add al, 'A' - 10
    jmp .o
.d:
    add al, '0'
.o:
    call ser_putc
    dec ecx
    jnz .loop
    popa
    ret

; ==================== 数据 ====================
boot_drive: db 0
m_banner:  db "I:boot dl=", 0
m_cd:      db "I:cd mode", 13, 10, 0
m_hdd:     db "I:hdd mode (USB/disk)", 13, 10, 0
m_jump:    db "I:jump loader64", 13, 10, 0
m_readerr: db "I:read failed", 13, 10, 0

align 4
; ---- 构建期回填（魔数 + 值分开，代码只读 *_val）----
vmld_lba:   db "VMLD"               ; 光盘：loader 起始扇区（2048B 单位）
vmld_val:   dd 0
vmlb_bytes: db "VMLB"               ; loader 字节数（两个路径共用）
vmlb_val:   dd 4096
vmkn_lba:   db "VMKN"               ; 光盘：内核起始扇区
vmkn_val:   dd 0
vmsy_lba:   db "VMSY"               ; 光盘：载荷起始扇区
vmsy_val:   dd 0
vmss_secs:  db "VMSS"               ; 载荷扇区数（512B 单位）
vmss_val:   dd 8073
vml9_lba:   db "VML9"               ; 硬盘：loader 起始扇区（512B 单位）
vml9_val:   dd 0
vmk9_lba:   db "VMK9"               ; 硬盘：内核起始扇区（512B 单位）
vmk9_val:   dd 0
vmkb_bytes: db "VMKB"               ; 内核字节数（补零后 4MB）
vmkb_val:   dd 8000 * 512
vms9_lba:   db "VMS9"               ; 硬盘：载荷起始扇区（512B 单位）
vms9_val:   dd 0
vmyb_bytes: db "VMYB"               ; 载荷字节数
vmyb_val:   dd 8073 * 512

align 4
big_src:    dd 0
big_left:   dd 0
big_dst:    dd 0
dot_cnt:    dw 0

align 16
gdt_start:
    dq 0x0000000000000000
gdt_code16:
    dq 0x00009A000000FFFF            ; 0x08：16 位代码
gdt_data16:
    dq 0x000092000000FFFF            ; 0x10：16 位数据（未用）
gdt_data_big:
    dq 0x00CF92000000FFFF            ; 0x18：4GB 平坦数据（G=1, limit=0xFFFFF, D/B=1）
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1
    dd gdt_start

align 4
dap:        db 0x10, 0
dap_count:  dw 0
dap_off:    dw 0
dap_seg:    dw 0
dap_lba:    dd 0
            dd 0

times (512 - ($ - $$) % 512) % 512 db 0
