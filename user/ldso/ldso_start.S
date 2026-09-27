/* ldso_start.S - ★ A3 下半：动态链接器的入口桩（汇编部分，只有 8 条指令）
 *
 * 为什么必须用汇编写入口（为什么不能是 C 的 _dlstart）：
 *   ld.so 被内核直接"扔"进 ring3 时，rsp 指向内核建好的**初始栈**（argc/argv/envp/auxv，
 *   与主程序将来要看到的**完全同一份**）。C 函数入口会自己改 rsp（压返回地址/局部变量），
 *   所以这里：
 *     1) 先把初始 rsp 原样存进 ldso_saved_sp（C 侧读 auxv 用它）；
 *     2) 按 SysV 对齐后再 call ldso_main（参数 = 初始 rsp）；
 *     3) ldso_main 返回**主程序入口**地址（rax）后，把 rsp **原样恢复**成初始栈，
 *        再 jmp 过去 —— 于是主程序 _start 看到的 argc/argv/auxv 一点没变（Linux 的
 *        ld.so 也是这么做的：解释器与主程序共享同一份初始栈）。
 *
 * 注：ldso_main 不再返回（它做完重定位后返回入口地址），这里的 hlt 只是防御性的。
 */
    .text
    .globl _dlstart
    .type  _dlstart, @function
_dlstart:
    movq  %rsp, %rdi                    /* rdi = 初始栈（argc 所在处） */
    movq  %rsp, ldso_saved_sp(%rip)
    andq  $-16, %rsp                    /* System V：调用前 16 字节对齐 */
    call  ldso_main
    movq  ldso_saved_sp(%rip), %rsp     /* 恢复初始栈：主程序看到的就是内核给的那一份 */
    jmp   *%rax                         /* -> 主程序入口（AT_ENTRY） */
    hlt
    .size _dlstart, .-_dlstart
