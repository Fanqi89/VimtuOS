/* r3drv_start.S - ★ v0.9.33：ring3 驱动服务（ext4svc / drvwatch）的入口桩
 *
 * 与 user/vmtool/vmtool_start.S 同一套约定（内核 elf64.cpp 按 SysV 摆好初始栈：
 * [rsp]=argc、[rsp+8..]=argv…），所以 ext4svc.c 里就是普通的 `int main(int argc, char** argv)`。
 *
 * 为什么 argv 对本批特别重要：内核 proc64_start_elf64_argv64 会传
 *   argv[0]=ELF 路径  argv[1]=驱动器号  argv[2]=卷起始 LBA
 * 服务靠后两个参数知道自己该服务哪块盘的哪个分区（而不是靠"猜"）。
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
