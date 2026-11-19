/* mp_start.S - ★ v0.11.0：MicroPython 自己的 _start（静态 musl 版入口桩）
 *
 * 与 user/lua/lua_start.S 逐条同构：只把内核摆好的初始 rsp 交给 C 侧的
 * mp_vimtu_start()，由它调 musl 的 __libc_start_main(main, argc, argv, 0,0,0)。
 * 不用 musl 的 crt1.o：crt1.c 里那句 `lea _DYNAMIC(%rip),%rsi` 在 -static 下
 * 链接期就报 R_X86_64_PC32 out of range（lua 那次已经实测过）。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi                  /* rdi = 初始栈（argc/argv/envp/auxv） */
    andq    $-16, %rsp
    call    mp_vimtu_start
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
