/* edit_start.S - ★ A4-5：/bin/edit 的入口桩
 *
 * 与 user/lib/crt0.S 的区别（不要混用，理由与 user/gzip/gzip_start.S 相同）：
 *   crt0.S 是给**平铺 blob** 用的：内核的 blob 入口 ABI 不传参数，它自己合成
 *   argc=1 / argv[0]="user64"。而 /bin/edit 是**从系统卷装载的 ELF64**（shell 的
 *   `run /bin/edit <文件>` -> fork + execve），内核 elf64.cpp 按 SysV ABI 摆好了初始栈：
 *     [rsp] = argc，[rsp+8..] = argv…（+ NULL + NULL envp + auxv…）
 *   编辑器必须能拿到**真 argv**（要打开的文件名在里面），所以这里把初始 rsp 原样交给
 *   edit_main（它自己解析 argc/argv）。userlib64_test 断言的是 crt0.S 那条 blob 路径
 *   （argc=1 argv0=user64），所以这里**不动 crt0.S**。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    movq    %rsp, %rdi                  /* rdi = 初始栈（argc 在 [rdi]） */
    andq    $-16, %rsp                  /* SysV：调用前 16 字节对齐 */
    call    edit_main
    movl    %eax, %edi                  /* 返回值 = 退出码（走 user/lib 的 exit -> 自有 ABI exit(2)） */
    call    exit
    hlt                                 /* 不该到这里 */
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
