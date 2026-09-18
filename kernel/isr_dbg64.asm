bits 64
section .text

; ---------------------------------------------------------------------------
; isr_dbg_handler64 - 全透明中断诊断处理函数（不用栈、不调函数）
;
; 进入时（由 isr_stubs64.asm 的 isr64_common 调用）：
;   rdi = 帧指针 = 指向 frame.rax 槽
;   帧布局：[rdi+0x00]=rax [rcx] [rdx] [rbx] [rbp] [rsi] [rdi] [r8..r15] [ds] [es] [fs] [gs]
;           [rdi+0x78]=int_no  [rdi+0x80]=err_code
;           [rdi+0x88]=rip [rdi+0x90]=cs [rdi+0x98]=rflags [rdi+0xA0]=rsp [rdi+0xA8]=ss
;
; 输出："Q" + int_no(4 hex) + " E" + err(4 hex) + " R" + rip(16 hex) + "\n"
;       + "S" + rsp(16 hex) + " C" + cs(4 hex) + " F" + rflags(16 hex) + "\n"
; ---------------------------------------------------------------------------
extern isr_dbg_u64
extern isr_dbg_u32
extern isr_dbg_puts

global isr_dbg_handler64
isr_dbg_handler64:
    mov rbx, [rdi + 0x78]          ; int_no
    mov r12, [rdi + 0x80]          ; err_code
    mov r13, [rdi + 0x88]          ; rip
    mov r14, [rdi + 0x90]          ; cs
    mov r15, [rdi + 0x98]          ; rflags
    mov rbp, [rdi + 0xA0]          ; rsp
    mov r11, [rdi + 0x00]          ; rax（顺便看看）

    mov rdi, msg1
    call isr_dbg_puts
    mov rdi, rbx
    call isr_dbg_u32
    mov rdi, msg2
    call isr_dbg_puts
    mov rdi, r12
    call isr_dbg_u32
    mov rdi, msg3
    call isr_dbg_puts
    mov rdi, r13
    call isr_dbg_u64
    mov rdi, msg4
    call isr_dbg_puts
    mov rdi, rbp
    call isr_dbg_u64
    mov rdi, msg5
    call isr_dbg_puts
    mov rdi, r14
    call isr_dbg_u32
    mov rdi, msg6
    call isr_dbg_puts
    mov rdi, r15
    call isr_dbg_u64
    mov rdi, msg7
    call isr_dbg_puts
    mov rdi, r11
    call isr_dbg_u64
    mov rdi, msg_nl
    call isr_dbg_puts

.hang:
    cli
    hlt
    jmp .hang

section .rodata
msg1:   db "Q int_no=", 0
msg2:   db " err=", 0
msg3:   db " rip=0x", 0
msg4:   db " rsp=0x", 0
msg5:   db " cs=", 0
msg6:   db " rflags=0x", 0
msg7:   db " rax=0x", 0
msg_nl: db 13, 10, 0
