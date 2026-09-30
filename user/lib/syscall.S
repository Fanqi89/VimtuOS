/* syscall.S - 两个原始系统调用桩（Vimtu64 用户态 C 运行时）
 *
 * 为什么必须用汇编写：两条入口的寄存器约定都是**内核定死的 ABI**，C 编译器不会替你摆好
 * （例：自有 ABI 的第 4 个参数在 r10，而不是 rcx；syscall 指令会把 rcx/r11 覆盖）。
 *
 * 入口 1：自有 ABI（int 0x80）—— kernel/syscall64.h
 *   long __v64_int80(long nr, long a1, long a2, long a3, long a4);
 *     rax = nr; rdi/rsi/rdx/r10 = a1..a4; 返回 rax（负数 = 错误）。
 *
 * 入口 2：Linux 兼容号段（syscall 指令）—— kernel/syscall_entry64.asm
 *   long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);
 *     rax = nr; rdi/rsi/rdx/r10/r8 = a1..a5; 返回 rax（负值 = -errno）。
 *   只给自有 ABI 没有的号用（lseek/stat/getcwd/unlink），见 unistd.h 的分工表。
 *
 * 两个桩都不碰 rbx/rbp/r12..r15（内核的中断帧会完整还原所有寄存器，这里只是保持 SysV 约定）。 */
    .text

    .globl __v64_int80
    .type  __v64_int80, @function
__v64_int80:
    movq    %rdi, %rax          /* nr */
    movq    %rsi, %rdi          /* a1 */
    movq    %rdx, %rsi          /* a2 */
    movq    %rcx, %rdx          /* a3 */
    movq    %r8,  %r10          /* a4 -> r10（自有 ABI 的第 4 参数） */
    int     $0x80
    ret
    .size  __v64_int80, .-__v64_int80

    .globl __v64_syscall
    .type  __v64_syscall, @function
__v64_syscall:
    movq    %rdi, %rax          /* nr */
    movq    %rsi, %rdi          /* a1 */
    movq    %rdx, %rsi          /* a2 */
    movq    %rcx, %rdx          /* a3 */
    movq    %r8,  %r10          /* a4 -> r10（Linux 的 arg4） */
    movq    %r9,  %r8           /* a5 -> r8（Linux 的 arg5） */
    syscall
    ret
    .size  __v64_syscall, .-__v64_syscall

    .section .note.GNU-stack,"",@progbits

/* ★ A4-5：信号返回桩（rt_sigreturn）。
 * 内核在信号帧里把这个地址当**返回地址**压在用户栈上，所以 handler 的 `ret` 会落到这里。
 * 这里只做一件事：`mov $15,%rax; syscall` —— 内核按用户栈上的帧（magic/sig/全部 GPR/rip/rflags/rsp/
 * 原屏蔽字）精确恢复被信号打断的现场。返回之后用户代码从"被打断的那条指令"继续（就像没被打断过）。
 * 不该返回：真返回了就说明内核没恢复帧（例如帧被踩坏 -> -EFAULT）—— 那就停住，绝不落到未映射地址。
 * 为什么不在 C 里写：syscall 指令的约定（rax=号）必须由汇编保证，C 编译器不会替你摆好。 */
    .text
    .globl __v64_sigreturn_stub
    .type  __v64_sigreturn_stub, @function
__v64_sigreturn_stub:
    movq    $15, %rax           /* rt_sigreturn */
    syscall
.Lsigret_hang:
    jmp     .Lsigret_hang       /* 不该到这里 */
    .size  __v64_sigreturn_stub, .-__v64_sigreturn_stub

    .section .note.GNU-stack,"",@progbits
