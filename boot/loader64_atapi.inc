; loader64_atapi.inc - 64 位 ATAPI（光驱）PIO 读，供 boot/loader64.asm 的 64 位阶段 %include
;
; 适用范围：**只用于光盘路径**（介质描述符 kind=1，loader64 打 "[LM] cd boot via ATAPI"）。
;   磁盘路径（装好的系统盘 / 裸盘介质，无描述符）不在这里 —— 那条路的内核读取在实模式阶段
;   由 BIOS INT 13h 扩展读完成（见 loader64.asm 的 int13_load_kernel），因为 PIO 在
;   SATA=AHCI 的机器上读不到盘。
;
; 为什么光盘还用 ATAPI（而不是 BIOS）：
;   ISO 形态的安装介质是光盘 —— 光盘不认识 READ SECTORS(LBA28)，必须用 SCSI 风格的
;   PACKET 命令（12 字节 CDB），而且扇区是 2048 字节。
;   另一个关键是：这里**不借 BIOS**。loader 进保护模式后不再调用任何 BIOS 中断
;   （VMware 的 BIOS 在保护模式往返后会挂住 —— boot/cdprobe.asm 探针实测），
;   而 ATAPI 是设备级命令，只碰端口，与 BIOS 无关，QEMU/VMware/真机都成立。
;
; 约定（调用前）：r8d 由下面 atapi64_read 自己算，调用方只需给：
;   r15d = 光盘驱动器号（0/1=主通道主/从盘，2/3=从通道主/从盘）
;   esi  = 光盘起始扇区号（2048 字节/扇区）
;   rdi  = 目标物理地址
;   rbx  = 光盘扇区数
; 返回：CF=0 成功，CF=1 失败
;
; 寄存器使用（均为调用方保存）：r8d=端口基址，r9b=选盘字节，r10d=每扇区收包轮数
bits 64

atapi64_read:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push rbx
    push r8
    push r9

    mov r8d, 0x1F0
    test r15d, 2
    jz .base_ok
    mov r8d, 0x170                  ; 从通道（secondary）
.base_ok:
    mov r9d, 0xA0                   ; ATAPI 选盘字节：0xA0 / 0xB0
    test r15d, 1
    jz .sel_ok
    or r9d, 0x10
.sel_ok:

.cdloop:
    test rbx, rbx
    jz .ok
    call atapi64_one
    jc .fail
    ; 注意：目标指针由内层 rep insw 自己推进（每次 512 字节 × 4 轮 = 2048），
    ;       这里**不能**再 add rdi, 2048 —— 踩过这个坑：镜像被摊成每 2KB 一个空洞，
    ;       内核跑起来但字符串全是 0。
    inc esi
    dec rbx
    mov eax, ebx
    and eax, 63
    jnz .cdloop
    push rax
    mov al, '.'
    call dbg64_putc                 ; 每 64 个光盘扇区（128KB）一个点
    pop rax
    jmp .cdloop

.ok:
    pop r9
    pop r8
    pop rbx
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    clc
    ret
.fail:
    pop r9
    pop r8
    pop rbx
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    stc
    ret

; ---- 读一个光盘扇区：esi = LBA，rdi = 目标 2048B，r8d = 基址，r9b = 选盘字节 ----
atapi64_one:
    push rax
    push rcx
    push rdx
    push r10

    mov dx, r8w
    add dx, 6
    mov al, r9b
    out dx, al
    call atapi64_delay

    call atapi64_wait_bsy
    jc .err

    mov dx, r8w
    add dx, 4
    xor al, al
    out dx, al
    mov dx, r8w
    add dx, 5
    mov al, 0x08                    ; 字节计数上限 = 2048
    out dx, al

    call atapi64_wait_nodrq
    jc .err

    mov dx, r8w
    add dx, 7
    mov al, 0xA0                    ; PACKET
    out dx, al

    call atapi64_wait_drq
    jc .err

    mov eax, esi
    bswap eax                       ; LBA 大端
    mov [atapi_cdb + 2], eax
    mov dx, r8w
    xor ecx, ecx
.cdb_word:
    mov ax, [atapi_cdb + rcx * 2]
    out dx, ax
    inc rcx
    cmp rcx, 6
    jb .cdb_word

    mov r10d, 4                     ; 4 × 512 字节 = 2048
.rd_chunk:
    call atapi64_wait_drq
    jc .err
    mov dx, r8w
    mov ecx, 256
    rep insw
    call atapi64_delay
    dec r10d
    jnz .rd_chunk

    call atapi64_wait_nodrq
    jc .err
    pop r10
    pop rdx
    pop rcx
    pop rax
    clc
    ret
.err:
    pop r10
    pop rdx
    pop rcx
    pop rax
    stc
    ret

; ---- 约 400ns 延时：交替状态 = 基址 + 0x206（0x1F0→0x3F6，0x170→0x376）----
atapi64_delay:
    push rax
    push rdx
    mov dx, r8w
    add dx, 0x206
    in al, dx
    in al, dx
    in al, dx
    in al, dx
    pop rdx
    pop rax
    ret

; ---- 等 BSY=0 ----
atapi64_wait_bsy:
    push rax
    push rcx
    push rdx
    mov ecx, 0x4000000
    mov dx, r8w
    add dx, 7
.w:
    in al, dx
    test al, 0x01
    jnz .err
    test al, 0x80
    jz .ok
    loop .w
.err:
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

; ---- 等 BSY=0 且 DRQ=0（发命令前 / 收尾）----
atapi64_wait_nodrq:
    push rax
    push rcx
    push rdx
    mov ecx, 0x4000000
    mov dx, r8w
    add dx, 7
.w:
    in al, dx
    test al, 0x01
    jnz .err
    test al, 0x80
    jnz .next
    test al, 0x08
    jz .ok
.next:
    loop .w
.err:
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

; ---- 等 DRQ=1 ----
atapi64_wait_drq:
    push rax
    push rcx
    push rdx
    mov ecx, 0x4000000
    mov dx, r8w
    add dx, 7
.w:
    in al, dx
    test al, 0x01
    jnz .err
    test al, 0x08
    jnz .ok
    loop .w
.err:
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

; ---- READ(10) 命令缓冲：0x28 00 <LBA 大端 4 字节> 00 00 01 00 00 00 ----
align 4
atapi_cdb:  db 0x28, 0x00, 0, 0, 0, 0, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00

msg_medium_cd:   db "L:media=cd", 13, 10, 0
msg_medium_ram:  db "L:media=ram", 13, 10, 0
