; hybrid_mbr.asm - hybrid ISO 的 MBR（写进 ISO 第 0 扇区）
;
; 用途：把 ISO 用 Rufus/balenaEtcher/dd 写进 **U 盘** 之后，BIOS 会把 U 盘当**硬盘**引导
;       （而不是光盘）。这时 El Torito 引导镜像不会被加载，BIOS 直接执行本扇区（LBA 0）。
;       本 MBR 只做一件事：把 ISO 里的引导桩（CDISO.BIN）读进 0x7C00 并跳过去 ——
;       剩下的事（unreal mode 搬内核/载荷进高内存）由引导桩的"硬盘分支"处理。
;
; 之所以要自己写 MBR 而不是用 syslinux 的 isohdpfx.bin：本项目不依赖外部引导代码，
; 而且我们只需要"读一个已知 LBA 的桩"这一件事，512 字节足够。
;
; 构建期回填（tools/make_iso64.py 按魔数写 dword）：
;   'VMHY' + dword = 引导桩在介质上的 **512 字节单位** LBA
;   'VMHZ' + dword = 引导桩的字节数
; 分区表（偏移 446 起 64 字节）与 55AA 签名也由构建脚本写入：
;   一项：活动 + 类型 0x17（隐藏 ISO9660）+ 起始 LBA 0 + 覆盖整张镜像
;   （isohybrid 的经典做法：分区从 LBA 0 开始，只是让 fdisk/BIOS 满意，真正执行的是 MBR）
[bits 16]
[org 0x7C00]

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

    ; ---- 把自己挪到 0x0600（马上要把桩读进 0x7C00，不能待在那儿）----
    mov si, 0x7C00
    mov di, 0x0600
    mov cx, 256
    rep movsw
    ; 注意：org 0x7C00 让所有标签都是**绝对**地址，副本在 0x0600，
    ;       所以这里要显式算副本地址 = 原偏移 + 0x0600（不能直接 jmp reloc）
    jmp 0x0000:(reloc - $$ + 0x0600)

reloc:
    ; ---- DL 先取好并压栈：桩被读进 0x7C00 之后，[boot_drive]（在 0x7Cxx）就被覆盖了 ----
    mov dl, [boot_drive]
    push dx

    ; ---- 取回填值（此时 0x7Cxx 还没被覆盖）----
    mov eax, [vmhy_val]             ; 桩的 512B 单位 LBA
    mov ebx, [vmhz_val]             ; 桩字节数
    add ebx, 511
    shr ebx, 9                      ; 扇区数

    ; ---- 在低内存 0x0500 搭 DAP（不能放在 0x7C00：马上要往那儿读数据）----
    mov word [0x0500], 0x0010
    mov [0x0502], bx
    mov word [0x0504], 0x7C00
    mov word [0x0506], 0x0000
    mov [0x0508], eax
    mov dword [0x050C], 0

    mov si, 0x0500
    mov ah, 0x42
    int 0x13
    jc fail

    pop dx                          ; DL 继续往下传（桩要用它判断"硬盘还是光盘"）
    jmp 0x0000:0x7C00

fail:
    ; 读桩失败：往 0x402(debugcon) 与 COM1 各写一个 '!'，然后停机（不放静默死循环）
    push ax
    push dx
    mov al, '!'
    mov dx, 0x402
    out dx, al
    mov dx, 0x3FD
    mov cx, 0xFFFF
.fw:
    in al, dx
    test al, 0x20
    jnz .fok
    loop .fw
.fok:
    mov al, '!'
    mov dx, 0x3F8
    out dx, al
    pop dx
    pop ax
hang:
    cli
    hlt
    jmp hang

; ==================== 数据 ====================
boot_drive: db 0

align 4
vmhy_lba:   db "VMHY"
vmhy_val:   dd 0
vmhz_cnt:   db "VMHZ"
vmhz_val:   dd 2048

; 分区表与签名由构建脚本写入（这里留空）
times 446 - ($ - $$) db 0
times 64 db 0                       ; 446..509 分区表（构建脚本回填）
dw 0xAA55                           ; 510..511 引导签名
