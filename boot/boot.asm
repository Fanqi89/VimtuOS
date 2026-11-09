; boot.asm - Vimtu64 主引导记录（MBR）= 两阶段引导的 **Stage 1**
;
; 职责（只有这一件事）：用 INT 13h 扩展读把 Stage 2（loader64.bin，容量 32 KB）
; 从 LBA 1 起读进 0x9000，然后跳过去。分区表在 446..510，由构建期/安装器写入。
;
; ★ v0.9.32：Stage 2 窗口从 **8 扇区（4 KB）放大到 64 扇区（32 KB）**。
;   旧限制的来源：MBR 只读 LBA 1..8，于是 loader64.bin 被 build64.sh 断言 ≤ 4,096 B，
;   本批开工时它已经用到 4,090 B（只剩 6 字节）—— 再加一行代码就构建失败。
;   为什么分两次读：BIOS 的 DAP 传输**不许跨 64 KB 物理边界**（用段式拷贝/内部 DMA）。
;       0x9000 + 28 KB = 0x10000 正好落在边界上 -> 第一块只读 56 扇区；
;       剩下的 8 扇区放到 0x1000:0x0000（= 物理 0x10000..0x10FFF），仍在同一 64 KB 块内。
;   为什么不改 UEFI：**UEFI 那条链没有 4 KB 限制**（BOOTX64.EFI 把 UEFI64.BIN 读到
;   0x06000000，上限 256 KB；UEFI64.BIN 目前 40 KB）。4 KB 是**纯 BIOS/INT 13h 的窗口**，
;   所以只在 BIOS 侧扩容，UEFI 侧一个字都不动。
;   注: loader 必须避开 SeaBIOS trampoline 区 (0x8000-0x9000)，故加载到 0x9000
[bits 16]
[org 0x7C00]

LOADER_SECTORS equ 64          ; Stage 2 容量（扇区）= 32 KB；与 build64.sh 的 LOADER_SECTORS 一致
LOADER_CHUNK1  equ 56          ; 第一块：0x9000..0xFFFF（28 KB，不跨 64 KB 边界）

start:
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    mov [BOOT_DRIVE], dl

    mov si, msg_boot
    call print_string

    ; ---- 读 Stage 2 第一块：LBA 1，56 扇区 -> 0x0000:0x9000 ----
    mov si, dap
    mov word [dap_count], LOADER_CHUNK1
    mov dword [dap_lba], 1
    mov word [dap_off], 0x9000
    mov word [dap_seg], 0
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    ; ---- 读 Stage 2 第二块：LBA 1+56，8 扇区 -> 0x1000:0x0000（物理 0x10000）----
    mov si, dap
    mov word [dap_count], LOADER_SECTORS - LOADER_CHUNK1
    mov dword [dap_lba], 1 + LOADER_CHUNK1
    mov word [dap_off], 0
    mov word [dap_seg], 0x1000
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    ; 跳转 Stage 2（DL 必须带着：loader64 的读盘路径要用 BIOS 传进来的盘号，
    ;   它不再自己去猜 0x80 —— 猜错就会去读另一块盘）。int 13h 一般会保留 DL，这里显式再装一次。
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
    dw LOADER_SECTORS   ; 扇区数（每次读前覆盖；远小于 255）
dap_off:
    dw 0x9000
dap_seg:
    dw 0
dap_lba:
    dd 1                ; 起始 LBA
    dd 0

BOOT_DRIVE: db 0
msg_boot: db "Aurora32 boot...", 0x0D, 0x0A, 0
msg_err:  db "Disk error!", 0x0D, 0x0A, 0

times 510 - ($ - $$) db 0
dw 0xAA55
