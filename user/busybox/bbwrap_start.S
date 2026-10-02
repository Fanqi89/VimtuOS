/* bbwrap_start.S - VimtuOS64：/bin/<applet> 包装程序的入口桩
 * 初始 rsp（argc/argv/envp/auxv）由内核 elf64.cpp 按 SysV ABI 摆好，原样交给 wrap_main。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    movq    %rsp, %rdi
    andq    $-16, %rsp
    call    wrap_main
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
