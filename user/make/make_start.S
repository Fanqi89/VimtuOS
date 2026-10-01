/* make_start.S - ★ B5：**GNU make 自身**的入口桩（静态 musl 版的 _start）
 *
 * 为什么不用 musl 的 crt1.o：musl 的 crt/crt1.c 里有 `lea _DYNAMIC(%rip),%rsi`（非 PIC 的
 *   _DYNAMIC 引用），`-static` 下 _DYNAMIC 无处解析 -> 链接期就报
 *   `relocation R_X86_64_PC32 out of range`（与 user/apps/tcc/tcc_start.S 同一条实测结论）。
 *   所以这里自写 _start：把初始栈交给 musl 的 __libc_start_main（内部的 __init_libc/
 *   static_init_tls 会去读 argc/argv/envp/auxv —— 初始栈由内核摆好，/bin/make 驱动接手时
 *   已经把 auxv 的 AT_PHDR/AT_PHNUM/AT_PHENT/AT_ENTRY 改成 make 自己的）。
 *
 * make 的主函数名是 `main`（third_party/make/make-4.4.1/src/main.c），这里不改名：直接把 main
 * 传给 __libc_start_main，与 musl 的 crt1.o 同口径。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi                  /* rdi = 初始栈（System V / musl 的 _start 约定） */
    andq    $-16, %rsp
    call    make_vimtu_start            /* user/make/make_start.c：-> __libc_start_main(main,…) */
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
