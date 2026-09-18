; isr_stubs64.asm - Vimtu64 中断桩（64 位长模式）
;
; ============================ 中断帧布局（唯一权威定义）============================
; 低地址在前。**保存的 rsp（= 传给 C 的帧指针）指向最低地址（gs 槽）**。
; 这套布局必须与三处严格一致：
;   1) kernel/x86_64.h 的 struct pt_regs64   （字段顺序 = 内存升序）
;   2) kernel/switch64.asm 的帧构造顺序
;   3) 将来移植 kernel/task.cpp 时构造任务初始栈的顺序
;
; push 朝低地址走，所以"最后压的字段地址最低"。本文件的压栈顺序是：
;   宏里先 push err_code、再 push int_no  →  GPR×15（rax 先、r15 后）
;   →  4 个段占位槽（最后是 gs）。因此内存升序为：
;
;   偏移   字段        来源 / 说明
;   0x00   gs          桩压（占位 0x10；长模式下 push 段寄存器非法，故用占位值）
;   0x08   fs          桩压（占位 0x10）
;   0x10   es          桩压（占位 0x10）
;   0x18   ds          桩压（占位 0x10）
;   0x20   r15         桩压（push 顺序 rax..r15 → 升序即 r15..rax）
;   0x28   r14
;   0x30   r13
;   0x38   r12
;   0x40   r11
;   0x48   r10
;   0x50   r9
;   0x58   r8
;   0x60   rdi
;   0x68   rsi
;   0x70   rbp
;   0x78   rbx
;   0x80   rdx
;   0x88   rcx
;   0x90   rax
;   0x98   int_no      宏里 push（无错误码的异常/IRQ 前补 0 错误码）
;   0xA0   err_code    宏里 push
;   0xA8   rip         CPU 压入
;   0xB0   cs          CPU
;   0xB8   rflags      CPU
;   0xC0   rsp         CPU（仅特权级切换时有效）
;   0xC8   ss          CPU（同上）
;   总大小 0xD0 = 208 字节
;
; ★ 踩坑记录：如果把 CPU 压入的 rip/cs/rflags 当成"帧的开头"，int_no 就会被
;   读成某个 GPR 槽（实测读成 0，PIT 中断被误判成除零异常）。改结构体时务必
;   对照上面这张表。
; ==============================================================================
bits 64

extern isr_handler64
extern schedule64

section .text

; ---- 无错误码的中断/异常：补一个 0 当错误码 ----
%macro ISR_NOERR64 1
global isr%1_64
isr%1_64:
    push qword 0
    push qword %1
    jmp isr64_common
%endmacro

; ---- CPU 会压入错误码的异常：不要再补，否则帧错位 ----
%macro ISR_ERR64 1
global isr%1_64
isr%1_64:
    push qword %1
    jmp isr64_common
%endmacro

; ---- 异常 0..31（与 x86_64 架构一致：8/10/11/12/13/14/17/21 带错误码）----
ISR_NOERR64 0    ; 除零
ISR_NOERR64 1
ISR_NOERR64 2    ; NMI
ISR_NOERR64 3
ISR_NOERR64 4
ISR_NOERR64 5
ISR_NOERR64 6
ISR_NOERR64 7
ISR_ERR64   8    ; 双重故障
ISR_NOERR64 9
ISR_ERR64   10
ISR_ERR64   11
ISR_ERR64   12
ISR_ERR64   13
ISR_ERR64   14   ; 缺页
ISR_NOERR64 15
ISR_NOERR64 16
ISR_ERR64   17
ISR_NOERR64 18
ISR_NOERR64 19
ISR_NOERR64 20
ISR_ERR64   21
ISR_NOERR64 22
ISR_NOERR64 23
ISR_NOERR64 24
ISR_NOERR64 25
ISR_NOERR64 26
ISR_NOERR64 27
ISR_NOERR64 28
ISR_NOERR64 29
ISR_ERR64   30
ISR_NOERR64 31

; ---- IRQ 32..47（PIC 映射到 0x20..0x2F）----
%assign irq 32
%rep 16
ISR_NOERR64 irq
%assign irq irq+1
%endrep

; ---- 系统调用 0x80（int 0x80，DPL=3 的门）----
; 注：不要写成 `ISR_NOERR64 0x80` —— NASM 会把 0x80 当表达式算成十进制 128，
;     生成的符号名会变成 isr0x80_64。这里显式给出十进制 128，符号即 isr128_64。
ISR_NOERR64 128

; ============================ 公共入口/出口 ============================
isr64_common:
    ; 保存通用寄存器（顺序必须与帧布局一致：rax 在最低地址）
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

    ; 4 个段占位槽（长模式下 PUSH Sreg / MOVZX FROM Sreg 都是非法指令，
    ; 所以按帧布局填 0x10 占位；出口用 add rsp,32 跳过，不做段加载）。
    ; 内核对 ds/es/fs/gs 的选择子由 entry64.asm 一次性设为 0x10 后不再改变，
    ; 长模式下 ds/es 的基址对寻址无影响，所以这里不需要（也不能）重载段寄存器。
    push qword 0x10
    push qword 0x10
    push qword 0x10
    push qword 0x10

    ; 第一个参数 = 帧指针（rsp 现在正好指向 frame.gs）
    mov rdi, rsp
    call isr_handler64

    ; ---- 出口：严格逆序还原 ----
    ; 长模式下 POP Sreg 只允许 fs/gs，ds/es 会 #UD；而这四个槽在内核里恒为 0x10
    ; （基址在长模式下对 ds/es 无意义），所以直接丢弃 32 字节、不做段加载。
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

    add rsp, 16          ; 丢掉 int_no + err_code
    iretq                ; 恢复 rip/cs/rflags/rsp/ss

; ============================ 上下文切换 ============================
; 说明：任务切换不是"另写一套"上下文，而是**复用上面这套中断帧**：
;   调度器在 IRQ0 的 C 处理里改帧内容（或换成另一个任务的帧）再 iretq 回去。
;   下面的 task_switch_iret64 就是"直接换成目标任务的帧并返回用户态/内核态"。
;
; void task_switch_iret64(uint64_t next_frame)   -> 不返回，切到 next_frame
global task_switch_iret64
task_switch_iret64:
    mov rsp, rdi
    add rsp, 32                 ; 同上：不加载 ds/es/fs/gs（内核里恒为 0x10）
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
