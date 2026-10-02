/* bbstart.S - VimtuOS64：busybox 本体（/lib/busybox.bin）的 _start
 *
 * 为什么不用 musl 的 crt1.o：musl 的 crt/crt1.c 里有 `lea _DYNAMIC(%rip),%rsi`，
 * `-static` 下 _DYNAMIC 无处解析（与 user/make/make_start.S 同一条实测结论）。
 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    xorl    %ebp, %ebp
    movq    %rsp, %rdi                  /* rdi = 初始栈（SysV / musl 的 _start 约定） */
    andq    $-16, %rsp
    call    bb_vimtu_start
    hlt
    .size _start, .-_start

    .section .note.GNU-stack,"",@progbits
