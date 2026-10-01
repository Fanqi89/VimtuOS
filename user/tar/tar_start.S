/* tar_start.S - ★ B5：tar 的入口桩（静态 musl 版的 _start）
 *
 * 为什么不用 musl 的 crt1.o：musl 的 crt/crt1.c 里有 `lea _DYNAMIC(%rip),%rsi`（非 PIC 的
 *   _DYNAMIC 引用），`-static` 下 _DYNAMIC 无处解析 -> 链接期就报
 *   `relocation R_X86_64_PC32 out of range`（与 user/apps/tcc/tcc_start.S 同一条实测结论）。
 *   所以自写 _start：把初始栈交给 musl 的 __libc_start_main（内部的 __init_libc/static_init_tls
 *   会去读 argc/argv/envp/auxv —— 初始栈由内核摆好）。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi                  /* rdi = 初始栈 */
    andq    $-16, %rsp
    call    tar_vimtu_start             /* user/tar/tar_start.c：-> __libc_start_main(main,…) */
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
