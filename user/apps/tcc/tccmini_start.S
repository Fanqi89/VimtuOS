/* tccmini_start.S - ★ A4-2b：tcc 产物（可执行文件）的入口桩 = 系统卷里的 crt1.o
 *
 * 谁用它：`tcc hello.c -o hello` 的链接阶段。tcc 的默认运行时顺序（tccelf.c:tcc_add_runtime +
 *   libtcc.c 里的 tcc_add_crt）是：
 *       crt1.o -> crti.o -> <用户 .o> -> -lc（我们的 tccmini.o） -> libtcc1.a -> crtn.o
 *   并且 tcc 写 ELF 头时 `e_entry = text_section->sh_addr`（tccelf.c:1916）—— 也就是说**入口
 *   必须是 .text 的最前面那一份**，所以本文件的 .text 排第一（这正是 crt1.o 被最先加进来的
 *   原因）。crti.o / crtn.o 本平台是**空对象**（构造/析构不需要）。
 *
 * 初始栈：内核 elf64.cpp 按 SysV ABI 摆好（[rsp]=argc，接着 argv…/envp/auxv，rsp%16==0），
 *   所以这里直接取 argc/argv 交给 main —— 与 Linux 的 crt1.o 同口径（少的只是 __libc_csu_*
 *   与 _init/_fini，本平台的 tccmini.c 里没有那些东西）。
 */
    .text
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    (%rsp), %rdi                /* argc */
    leaq    8(%rsp), %rsi               /* argv */
    andq    $-16, %rsp                  /* SysV：调用前 16 字节对齐 */
    call    main
    movl    %eax, %edi                  /* main 的返回值 = 退出码 */
    call    exit                        /* tccmini.c：exit_group(231) */
    hlt                                 /* 不该到这里 */
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
