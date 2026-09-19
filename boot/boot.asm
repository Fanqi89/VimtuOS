; boot.asm - Vimtu32 主引导记录 (MBR)
; 功能: 用 int 13h 扩展读取 LBA1 起的 8 个扇区 (loader) 到 0x9000，然后跳转
; 注: loader 必须避开 SeaBIOS trampoline 区 (0x8000-0x9000)，故加载到 0x9000 之后
[bits 16]
[org 0x7C00]

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [BOOT_DRIVE], dl

    ; 提示字符
    mov si, msg_boot
    call print_string

    ; 读取 loader: LBA=1, count=8 -> 0x9000
    mov si, dap
    mov byte [dap_count], 8
    mov dword [dap_lba], 1
    mov word [dap_off], 0x9000
    mov word [dap_seg], 0
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error
    ; 跳转 loader（DL 必须带着：loader64 的磁盘读盘路径要用 BIOS 传进来的盘号，
    ;   它不再自己去猜 0x80 —— 猜错就会去读另一块盘）。int 13h 一般会保留 DL，但这里显式再装一次。
    mov dl, [BOOT_DRIVE]
    jmp 0x0000:0x9000

disk_error:
    mov si, msg_err
    call print_string
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

; ---- 磁盘地址包 (DAP) ----
dap:
    db 0x10          ; DAP 大小
    db 0             ; 保留
dap_count:
    dw 8             ; 扇区数
dap_off:
    dw 0x9000
dap_seg:
    dw 0
dap_lba:
    dd 1             ; 起始 LBA
    dd 0

BOOT_DRIVE: db 0
msg_boot: db "Aurora32 boot...", 0x0D, 0x0A, 0
msg_err:  db "Disk error!", 0x0D, 0x0A, 0

times 510 - ($ - $$) db 0
dw 0xAA55
