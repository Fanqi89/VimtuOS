; entry64.asm - Vimtu64 长模式内核入口（真正进入 64 位代码的第一条指令）
;
; 调用来源：boot/loader64.asm 的 lm64_enter，在 32 位保护模式里完成
;           （PAE + IA32_EFER.LME + CR0.PG + 远跳）之后 `jmp 0x08:lm64_entry`。
;
; 为什么入口必须用汇编：
;   1) 进长模式那一刻，通用寄存器任意、栈还没建立，C++ 代码无法直接接手；
;   2) 段选择子必须是 loader 的 GDT 里定义的值（CS=0x08 / DS 等=0x10），
;      这两条必须与 linker64.ld 的链接顺序配合 —— 入口函数用 section(".text.entry")
;      固定放在内核镜像的最前面，因为 loader 是**裸平铺加载**，只会跳到 0x100000。
;   3) 栈必须放在内核镜像（BSS）之外的安全区，不能覆盖已加载的内核或页表区。
;
; 栈的选择：0x7C000（约 496KB），位于 loader/内核加载区之上、EBDA(0x9FC00) 之下的
;   低端内存空闲区，不受内核镜像体积增长影响，且不会与 0x40000 页表区（最高 0x46000）冲突。

bits 64
section .text.entry
global lm64_entry
extern kmain64

lm64_entry:
    cli                             ; 中断在 IDT 建立之前一律关闭
    ; ---- 串口打点：'E' 进入内核入口 / 's' 栈就绪 / 'K' 即将 call kmain64 ----
    ;   用途：VMware EFI 下曾"进内核即静默复位"，无任何日志；靠这几个字节把
    ;   重启点夹到具体一条指令。COM1 由 UEFI 引导器（或 BIOS loader）初始化过，可直接写。
    mov dx, 0x3F8
    mov al, 'E'
    out dx, al

    ; ---- 数据段（长模式下基址被忽略，仅用于访问权限）----
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 建立内核栈 ----
    mov rsp, 0x7C000
    xor rbp, rbp

    mov al, 's'
    out dx, al
    ; ---- 传给 kmain64 的启动参数（System V AMD64 ABI）----
    ; rdi = 0 -> kmain64 从固定地址 0x1000 自行读取 BootInfo
    xor rdi, rdi
    xor rsi, rsi

    mov al, 'K'
    out dx, al
    call kmain64

.hang:
    cli
    hlt
    jmp .hang
