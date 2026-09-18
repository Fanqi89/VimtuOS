; isr_out64.asm - 诊断用输出函数（只用寄存器，不碰栈，避免干扰被观察的帧）
;
; 约定：这些函数**不保存/不恢复寄存器**（只改 rax/rbx/rcx/rdx），也不使用栈，
;       调用方把要观察的值放在 r11..r15 / rbp 里即可。
bits 64
section .text

global isr_dbg_puts
global isr_dbg_u64
global isr_dbg_u32
global isr_dbg_putc

; rdi = 字符串
isr_dbg_puts:
    mov rbx, rdi
.next:
    movzx rdi, byte [rbx]
    test dil, dil
    jz .done
    call isr_dbg_putc
    inc rbx
    jmp .next
.done:
    ret

; rdi = 64 位值（打 16 个 nibble，从高到低）
isr_dbg_u64:
    mov rbx, rdi
    mov ecx, 16
.nib:
    rol rbx, 4
    mov eax, ebx
    and eax, 0xF
    cmp al, 10
    jb .dig
    add al, 'A' - 10
    jmp .emit
.dig:
    add al, '0'
.emit:
    movzx rdi, al
    call isr_dbg_putc
    dec ecx
    jnz .nib
    ret

; rdi = 32 位值（打低 8 个 nibble，从高到低）
isr_dbg_u32:
    mov ebx, edi
    rol ebx, 8
    mov ecx, 8
.nib:
    rol ebx, 4
    mov eax, ebx
    and eax, 0xF
    cmp al, 10
    jb .dig
    add al, 'A' - 10
    jmp .emit
.dig:
    add al, '0'
.emit:
    movzx rdi, al
    call isr_dbg_putc
    dec ecx
    jnz .nib
    ret

; rdi = 字符（低 8 位）-> 0x402 + COM1（等待方式与 loader 一致，带超时）
isr_dbg_putc:
    push rax
    push rcx
    push rdx
    mov eax, edi
    mov ah, al
    movzx eax, al
    mov ah, al
    mov dx, 0x402
    out dx, al
    mov al, ah
    mov dx, 0x3FD
    mov ecx, 0x10000
.wait:
    in al, dx
    test al, 0x20
    jnz .ok
    loop .wait
.ok:
    mov al, ah
    mov dx, 0x3F8
    out dx, al
    pop rdx
    pop rcx
    pop rax
    ret
