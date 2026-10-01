/* crt0.S - 用户程序入口 _start（Vimtu64 用户态 C 运行时）
 *
 * 谁跳到这里：kernel/usermode64.cpp 的 blob 加载器（user64_run_blob64 / user64_run_capp64）
 *   把本程序拷进用户代码页（VA = USER64_CODE_VA64 = 4GiB）并 iretq 进 ring3，
 *   rip = 4GiB（本文件必须排在 blob 的**第一个字节**，靠 .text.start 段 + user64.ld 保证），
 *   rsp = 用户栈顶（4GiB + 64KiB - 16，16 字节对齐），其余寄存器清零。
 *
 * ★ 参数从哪来（如实说明，★ 本批修）：两条入口的栈布局不同，_start 必须自己分：
 *   ① **ELF64 装载器路径**（kernel/elf64.cpp 的 e64_build_stack*64）：内核已经在用户栈顶摆好
 *      [argc][argv0..][NULL][envp..][auxv..] 初始帧，rsp 16 字节对齐、[rsp] = argc。
 *      `run /bin/shell.bin -c "…"`、GNU make 的 `/bin/sh -c '<recipe>'`（fork+execve）全走这条路
 *      —— 这里**必须**把真 argc/argv 交给 main，否则 `-c` 永远看不到（本批实测踩到）。
 *   ② **blob 路径**（kernel/usermode64.cpp 的 user64_run_blob64 / user64_run_capp64）：
 *      入口 ABI 不传参数（没有 argc/argv/envp 寄存器约定，也没有初始帧），栈页是**清零**的。
 *      判据：[rsp] 是不是一个合法 argc（1..16）。是 -> 用 ①；否则（0 = 清零栈）走老行为：
 *      argc = 1、argv[0] = "user64"、argv[1] = NULL、envp[0] = NULL
 *      （tests/userlib64_test.py 的 `[HELLO] argc=1 argv0=user64` 就是这条口径，**必须不变**）。
 *   exit 不该返回；万一返回就死循环停住（绝不落到没有映射的地址上乱跑）。 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    andq    $-16, %rsp              /* 栈顶（内核给的是 16 对齐，这里再保险一次） */
    /* ★ 本批修：见 file header 的两条入口。ELF64 装载器路径的初始帧第一个字就是 argc
       （1..16）；blob 路径的栈是清零的（[rsp] == 0）-> 走老行为（argc=1/"user64"）。 */
    movq    (%rsp), %rax
    cmpq    $1, %rax
    jb      .Lv64_blob
    cmpq    $16, %rax
    ja      .Lv64_blob
    movq    %rax, %rdi              /* argc（内核摆的真值） */
    leaq    8(%rsp), %rsi           /* argv（argv[0] 在 [rsp+8]） */
    leaq    16(%rsp,%rax,8), %rdx   /* envp = argv + argc + 1 个指针 */
    call    main                    /* SysV：此处 rsp 16 对齐 -> main 入口 rsp%16 == 8（正确） */
    jmp     .Lv64_exit

.Lv64_blob:
    subq    $32, %rsp               /* 现搭 argc/argv/envp 三件套（栈顶向下 32 字节） */
    movabsq $v64_prog64, %rax
    movq    %rax, 0(%rsp)           /* argv[0] = "user64" */
    movq    $0,   8(%rsp)           /* argv[1] = NULL */
    movq    $0,   16(%rsp)          /* envp[0] = NULL（本内核没有环境变量表） */
    movl    $1, %edi                /* argc = 1 */
    movq    %rsp, %rsi              /* argv */
    leaq    16(%rsp), %rdx          /* envp */
    call    main                    /* SysV：此处 rsp 16 对齐 -> main 入口 rsp%16 == 8（正确） */

.Lv64_exit:
    movl    %eax, %edi              /* main 的返回值 = 退出码 */
    call    exit                    /* 不返回 */
1:  jmp     1b

    .size  _start, .-_start

    .section .rodata
v64_prog64:
    .asciz  "user64"

    .section .note.GNU-stack,"",@progbits
