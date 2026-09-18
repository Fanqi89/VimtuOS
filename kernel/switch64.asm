; switch64.asm - Vimtu64 任务上下文切换（64 位长模式）
;
; 与 32 位版（kernel/switch.asm）的差别：
;   * 寄存器是 8 字节（64 位），中断帧比 32 位多 8 个寄存器字段；
;   * pushfd/popa/iret -> pushfq / 逐寄存器保存 / iretq；
;   * TCB.esp 与函数参数都是 64 位（uint64_t）。
;
; 帧布局与 kernel/isr_stubs64.asm 完全一致（内存升序，低地址在前）：
;   [gs][fs][es][ds][r15..r8][rdi][rsi][rbp][rbx][rdx][rcx][rax][int_no][err][rip][cs][rflags][rsp][ss]
;   共 0xD0 = 208 字节。保存的 rsp 指向 frame 的**最低地址**（gs 槽）。
;   ★ 偏移必须与 kernel/x86_64.h 的 pt_regs64 表格一致（踩过坑：整表写反过一次）。
bits 64
section .text

; ---------------------------------------------------------------------------
; void task_switch_yield64(uint64_t* prev_slot, uint64_t next_frame)
;   协程式让出（非中断路径）：把当前执行点保存成同格式帧，再切到 next_frame。
;   保存的 rip = 本函数的返回地址，所以目标任务恢复后从调用点之后继续执行。
;
;   进入时（System V）：
;       [rsp]   = 返回地址（= 要保存的 rip）
;       [rsp+8] = prev_slot 参数（rdi）
;       [rsp+16]= next_frame 参数（rsi）
;
;   实现要点：**不能**用 push 寄存器占位当"通道槽"——恢复时会 pop 回来，
;   把 rax/rcx/rdx 变成垃圾值。必须先把真实寄存器值存到临时沙坑，
;   帧搭好后再用 mov 写进对应的槽位（通道槽偏移恒定，见下面的 EQU）。
; ---------------------------------------------------------------------------
%define OFF_RDI   0x60
%define OFF_RSI   0x68
%define OFF_RBP   0x70
%define OFF_RBX   0x78
%define OFF_RDX   0x80
%define OFF_RCX   0x88
%define OFF_RAX   0x90

global task_switch_yield64
task_switch_yield64:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    pushfq                      ; 再算出 rflags（此时 CPU 已关中断由调用方保证）

    mov r12, rsp                ; r12 = 沙坑指针（保存区基址）
    mov r15, [rsp + 0x38]       ; 原 [rsp]=返回地址 → r15 = 保存的 rip
                                ; （0x38 = 6 个 push + pushfq = 56 字节）
    mov r14, rdi                ; prev_slot
    mov r13, rsi                ; next_frame

    ; ---- 构造帧（从高地址往低地址压）----
    push qword 0                ; ss     （同特权级 iretq 不弹出，占位）
    push qword 0                ; rsp    （同上，占位）
    push qword [r12]            ; rflags（沙坑里 pushfq 的值）
    push qword 0x08             ; cs     （内核代码段，与 GDT/IDT 一致）
    push r15                    ; rip
    push qword 0                ; err_code
    push qword 0                ; int_no
    push qword 0                ; rax 槽
    push qword 0                ; rcx 槽
    push qword 0                ; rdx 槽
    push qword 0                ; rbx 槽
    push qword 0                ; rbp 槽
    push qword 0                ; rsi 槽
    push qword 0                ; rdi 槽
    push qword 0                ; r8
    push qword 0                ; r9
    push qword 0                ; r10
    push qword 0                ; r11
    push qword 0                ; r12
    push qword 0                ; r13
    push qword 0                ; r14
    push qword 0                ; r15
    push qword 0                ; ds
    push qword 0                ; es
    push qword 0                ; fs
    push qword 0                ; gs

    ; ---- 把真实寄存器值写进对应槽位（rsp 就是帧基址）----
    ; 注意：帧里 r12/r13/r14/r15 的槽位保持 0 —— 恢复时那 4 个寄存器由
    ;       本函数的返回路径处理（恢复后不需要原值），保持 0 是安全的。
    mov [rsp + OFF_RAX], rax
    mov [rsp + OFF_RCX], rcx
    mov [rsp + OFF_RDX], rdx
    mov [rsp + OFF_RBX], rbx
    mov [rsp + OFF_RBP], rbp
    mov [rsp + OFF_RSI], rsi
    mov [rsp + OFF_RDI], rdi

    ; ---- 保存当前帧指针，切到目标任务 ----
    mov [r14], rsp              ; *prev_slot = 当前帧
    mov rsp, r13                ; rsp = next_frame

    ; 长模式下 POP Sreg 只允许 fs/gs，且内核里这四个槽恒为 0x10 -> 直接跳过 32 字节
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
    add rsp, 16                 ; 丢掉 int_no + err_code
    iretq                       ; -> 目标任务的 rip/cs/rflags

; ---------------------------------------------------------------------------
; void task_switch_iret64(uint64_t next_frame)
;   "抬栈返回"：不保存当前上下文（当前任务的帧由中断路径或调用方保存），
;   直接把栈换成 next_frame 并 iretq。用于抢占切换、任务退出、恢复 idle。
;
;   注：该符号定义在 kernel/isr_stubs64.asm（与中断出口共用同一段抬栈代码），
;       这里不再重复定义，避免重复符号。
; ---------------------------------------------------------------------------
