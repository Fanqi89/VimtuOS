; isr_probe64.asm - 中断桩"保存/恢复"路径探针（诊断专用，验证通过后可删）
;
; 目的：把完整中断桩（isr_stubs64.asm）的 prologue + epilogue 原样走一遍，
;       但 C 侧只打印、不改变控制流，最后 iretq 回到被中断处。
;       若日志出现 "probe: RETURNED OK"，说明保存/恢复/iretq 全对，
;       问题只可能在业务处理函数；否则问题就在桩本身。
bits 64
section .text
extern isr_probe64_c

global isr_probe64_entry
isr_probe64_entry:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    push qword 0x10
    push qword 0x10
    push qword 0x10
    push qword 0x10

    mov rdi, rsp
    call isr_probe64_c

    add rsp, 32
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    add rsp, 16
    iretq
