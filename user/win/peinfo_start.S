/* peinfo_start.S - ★ v0.9.43（W0）：/bin/peinfo 的入口桩
 *
 * 与 user/gzip/gzip_start.S 完全同一条纪律（**不要混用 user/lib/crt0.S**：
 *   那是给内核内嵌的"平铺 blob"用的，内核按 blob 入口 ABI 进 ring3、不传参数）。
 * 本程序是**从系统卷装载的 ELF64**（shell 的 run -> fork + execve），内核 elf64.cpp 按 SysV ABI
 * 摆好了初始栈：[rsp] = argc，[rsp+8..] = argv…（+ NULL + NULL envp + auxv…）。
 * 这里把**初始 rsp 原样**交给 peinfo_entry（它要读 argv[1] = 文件路径），
 * 退出走 exit_group(231)，所以不需要 __libc_start_main。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    movq    %rsp, %rdi                  /* rdi = 初始栈（argc 所在处） */
    andq    $-16, %rsp                  /* SysV：调用前 16 字节对齐 */
    call    peinfo_entry
    hlt                                 /* 不该到这里（peinfo_entry 以 exit_group 结束） */
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
