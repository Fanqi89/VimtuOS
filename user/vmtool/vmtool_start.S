/* dpkg_start.S - ★ v0.7.5：用户态 dpkg/apt 的入口桩
 *
 * 与 gzip_start.S 的区别（有意）：gzip 把**初始 rsp**原样交给 gzip_main（它自己解 argv）；
 * 这里按标准 SysV 把 argc/argv 解好再调 main —— 这样 dpkg.c 里就是普通的
 * `int main(int argc, char** argv)`，便于后续维护。
 *   内核 elf64.cpp 按 SysV 摆好初始栈：[rsp]=argc、[rsp+8..]=argv…（+NULL +NULL envp +auxv）
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    movq    (%rsp), %rdi                /* rdi = argc */
    leaq    8(%rsp), %rsi               /* rsi = argv */
    andq    $-16, %rsp                  /* SysV：调用前 16 字节对齐 */
    call    main
    movq    %rax, %rdi                  /* 返回码 -> exit_group 参数 */
    movq    $231, %rax                  /* exit_group */
    syscall
    hlt                                 /* 不该到这里 */
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
