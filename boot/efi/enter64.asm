; boot/efi/enter64.asm - 从 UEFI 应用跳进 Vimtu64 内核
;
; 这是整条 UEFI 引导路径里唯一一段汇编，而且**全程 64 位**：
; 载入我们自己的 GDT（长模式下 0x08=64 位代码 / 0x10=数据）→ 换 CR3 到我们建好的
; 恒等映射页表 → jmp 到内核入口 0x100000（kernel/entry64.asm 的 lm64_entry）。
;
; 为什么要自己载 GDT：ExitBootServices 之后固件还留着它的 GDT，但选择子编号/属性是固件的
; 私有约定；内核 entry64.asm 明确要求 CS=0x08 / DS=0x10，所以这里必须换成我们自己的表。
;
; 汇编：nasm -f win64（COFF，供 lld-link 链成 PE32+ 的 efi_application）
; 参数（Win64 ABI）：rcx = 内核入口，rdx = 页表物理地址（CR3），r8 = 保留

bits 64
default rel

section .text
global efi_enter_kernel

efi_enter_kernel:
    cli                             ; 进内核前关中断（内核自己会建 IDT）
    cld

    ; ---- 载入我们自己的 GDT ----
    lgdt [gdt_desc]

    ; ---- 数据段选择子（0x10 = 我们的数据描述符）----
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ---- 栈（内核 entry64 会立即重设为 0x7C000，这里先给个安全值）----
    mov rsp, 0x7C000
    xor rbp, rbp

    ; ---- 换 CR3 到我们建的恒等映射页表（0..512GB，1GB 大页）----
    mov cr3, rdx

    ; ---- 跳进内核（rdi/rsi 按 entry64 的约定置 0）----
    xor rdi, rdi
    xor rsi, rsi
    jmp rcx

.hang:
    cli
    hlt
    jmp .hang

section .data
align 16
gdt_start:
    dq 0x0000000000000000            ; 0x00 空描述符
    dq 0x00AF9A000000FFFF            ; 0x08 64 位代码（L=1, P=1, DPL=0, type=0xA）
    dq 0x00AF92000000FFFF            ; 0x10 数据（P=1, type=0x2；长模式下基址/限长被忽略）
gdt_end:
gdt_desc:
    dw gdt_end - gdt_start - 1       ; 限长
    dq gdt_start                     ; 基址（64 位）
