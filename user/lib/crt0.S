/* crt0.S - 用户程序入口 _start（Vimtu64 用户态 C 运行时）
 *
 * 谁跳到这里：kernel/usermode64.cpp 的 blob 加载器（user64_run_blob64 / user64_run_capp64）
 *   把本程序拷进用户代码页（VA = USER64_CODE_VA64 = 4GiB）并 iretq 进 ring3，
 *   rip = 4GiB（本文件必须排在 blob 的**第一个字节**，靠 .text.start 段 + user64.ld 保证），
 *   rsp = 用户栈顶（4GiB + 64KiB - 16，16 字节对齐），其余寄存器清零。
 *
 * ★ 参数从哪来（如实说明）：blob 入口 ABI **不传参数**（没有 argc/argv/envp 寄存器约定、
 *   也没有内核帮我们摆栈的初始帧 —— 那是 ELF64 加载器路径才有的）。所以 crt0 自己合成一个
 *   最小三件套：argc = 1、argv[0] = "user64"、argv[1] = NULL、envp[0] = NULL，
 *   让 `int main(int argc, char** argv)` 的写法有确定语义（不崩、不读到垃圾）。
 *
 * 出口：main 的返回值 -> exit(code)（stdlib.c：先刷 stdout，再自有 ABI exit(2)）。
 *   exit 不该返回；万一返回就死循环停住（绝不落到没有映射的地址上乱跑）。 */
    .section .text.start,"ax",@progbits
    .globl _start
    .type  _start, @function
_start:
    andq    $-16, %rsp              /* 栈顶（内核给的是 16 对齐，这里再保险一次） */
    subq    $32, %rsp               /* 现搭 argc/argv/envp 三件套（栈顶向下 32 字节） */
    movabsq $v64_prog64, %rax
    movq    %rax, 0(%rsp)           /* argv[0] = "user64" */
    movq    $0,   8(%rsp)           /* argv[1] = NULL */
    movq    $0,   16(%rsp)          /* envp[0] = NULL（本内核没有环境变量表） */
    movl    $1, %edi                /* argc = 1 */
    movq    %rsp, %rsi              /* argv */
    leaq    16(%rsp), %rdx          /* envp */
    call    main                    /* SysV：此处 rsp 16 对齐 -> main 入口 rsp%16 == 8（正确） */
    movl    %eax, %edi              /* main 的返回值 = 退出码 */
    call    exit                    /* 不返回 */
1:  jmp     1b

    .size  _start, .-_start

    .section .rodata
v64_prog64:
    .asciz  "user64"

    .section .note.GNU-stack,"",@progbits
