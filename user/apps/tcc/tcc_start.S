/* tcc_start.S - ★ A4-2b：**tcc 自己**的入口桩（静态 musl 版的 _start）
 *
 * 为什么不用 musl 的 crt1.o：musl 的 crt/crt1.c 里有 `lea _DYNAMIC(%rip),%rsi`（非 PIC 的
 *   _DYNAMIC 引用），`-static` 下 _DYNAMIC 无处解析 -> 链接期就报
 *   `relocation R_X86_64_PC32 out of range`（同 user/apps/muslhello.c 的注释，A2 已实测）。
 *   所以这里自写 _start：把初始栈交给 musl 的 __libc_start_main（内部的 __init_libc/
 *   static_init_tls 会去读 argc/argv/envp/auxv —— 初始栈由内核摆好；tcc 被装载驱动
 *   接过来时，驱动已经把 auxv 的 AT_PHDR/AT_PHNUM/AT_PHENT/AT_ENTRY 改成 tcc 自己的）。
 *
 * tcc 的主函数名就是 `main`（tcc.c:246 `int main(int argc0, char **argv0)`），这里不改名：
 *   直接把 main 传给 __libc_start_main，与 musl 的 crt1.o 同口径。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi                  /* rdi = 初始栈（System V / musl 的 _start 约定） */
    andq    $-16, %rsp
    call    tcc_vimtu_start             /* user/apps/tcc/tcc_start.c：-> __libc_start_main(main,…) */
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
