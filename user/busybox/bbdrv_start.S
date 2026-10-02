/* bbdrv_start.S - VimtuOS64：/bin/busybox（装载驱动）的入口桩
 *
 * 与 user/lib/crt0.S 的区别（不要混用）：crt0.S 是给**平铺 blob** 用的（内核的 blob 入口
 *   ABI 不传参数）。本程序是**从系统卷装载的 ELF64**（shell 的 run -> fork + execve），
 *   内核 elf64.cpp 按 SysV ABI 摆好了初始栈：
 *     [rsp] = argc，[rsp+8..] = argv…（+ NULL + NULL envp + auxv…）
 *   所以这里把**初始 rsp 原样**交给 drv_main —— 它要读真 argv（用户敲的参数、argv[0] 决定
 *   applet 名）、也要在原地改 auxv，最后恢复同一份 rsp 再 jmp 进 busybox。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    movq    %rsp, %rdi                  /* rdi = 初始栈（argc 所在处） */
    andq    $-16, %rsp                  /* SysV：调用前 16 字节对齐 */
    call    drv_main
    hlt                                 /* 不该到这里 */
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
