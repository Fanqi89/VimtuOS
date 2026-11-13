/* peload_stubs.S - ★ W1：IAT 桩的 **Win64 -> SysV 跳板**
 *
 * ============================ 为什么必须有这个文件 ============================
 * PE 里的代码是 **Windows x64 调用约定**：第 1..4 个整数参数走 `rcx, rdx, r8, r9`，
 * 调用者还要在栈上留 32 字节"影子空间"（shadow store），并且在 `call` 前把 rsp 对齐到 16。
 * 而我们的用户态 C（clang，`x86_64-unknown-none-elf`）是 **SysV**：参数走 `rdi, rsi, rdx, rcx, r8, r9`，
 * 没有影子空间。
 *
 * 也就是说：**不能把一个用 C 写的函数地址直接写进 IAT** —— 哪怕只有一个参数。
 * `ExitProcess(7)` 会把 7 放在 `rcx` 里，而 SysV 的 `ExitProcess` 桩会去读 `rdi`（垃圾），
 * 于是"退出码 7"变成"退出码随机"，而这**看起来像桩生效了**（进程确实退出了）。这是最难查的一类假绿。
 *
 * 所以每条桩都是一个 3~4 条指令的汇编跳板：把 Win64 的寄存器搬进 SysV 的寄存器，
 * 再 call 真正的 C 实现（`pl_do_*`，见 user/win/peload.c）。
 *
 * ============================ 栈对齐怎么算 ============================
 * 进桩时 rsp % 16 == 8（`call` 刚压过返回地址）。
 * `subq $0x28, %rsp`：0x28 = 40，40 % 16 = 8，于是 rsp % 16 == 0 —— 满足 SysV "call 前 16 对齐"，
 * 同时这 40 字节顺带就是 Win64 要求的 32 字节影子空间 + 8 字节对齐填充。
 * 桩返回时 `addq $0x28, %rsp` 还原，再 `ret` 回 PE。
 */

    .section .text.peload_stubs,"ax",@progbits

/* long pl_do_exit_process(u64 code) —— Win64: rcx = code */
    .globl pl_stub_ExitProcess
    .type  pl_stub_ExitProcess, @function
pl_stub_ExitProcess:
    movq    %rcx, %rdi
    subq    $0x28, %rsp
    call    pl_do_exit_process
    addq    $0x28, %rsp
    ret
    .size pl_stub_ExitProcess, .-pl_stub_ExitProcess

/* void *pl_do_get_proc_address(void *h, const char *name) —— Win64: rcx = h, rdx = name */
    .globl pl_stub_GetProcAddress
    .type  pl_stub_GetProcAddress, @function
pl_stub_GetProcAddress:
    movq    %rcx, %rdi
    movq    %rdx, %rsi
    subq    $0x28, %rsp
    call    pl_do_get_proc_address
    addq    $0x28, %rsp
    ret
    .size pl_stub_GetProcAddress, .-pl_stub_GetProcAddress

/* void *pl_do_load_library_a(const char *name) —— Win64: rcx = name */
    .globl pl_stub_LoadLibraryA
    .type  pl_stub_LoadLibraryA, @function
pl_stub_LoadLibraryA:
    movq    %rcx, %rdi
    subq    $0x28, %rsp
    call    pl_do_load_library_a
    addq    $0x28, %rsp
    ret
    .size pl_stub_LoadLibraryA, .-pl_stub_LoadLibraryA

    .section .note.GNU-stack,"",@progbits
