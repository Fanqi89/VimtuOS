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
