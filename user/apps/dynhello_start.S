/* dynhello_start.S - ★ A3 下半：动态主程序（dynhello）的入口桩
 *
 * 为什么必须是汇编（为什么不能"在 C 里读 %rsp"）：
 *   ld.so 做完重定位后 `jmp *AT_ENTRY`，此时 rsp **原样**指向内核建好的初始栈
 *   （[rsp]=argc、[rsp+8]=argv[0]、再往后 envp/auxv）。也就是说入口函数继承的是"栈顶布局"，
 *   不是 SysV 的函数调用 ABI：
 *     * 任何会 push 的 C 函数序言都会把 rsp 挪到 argc 槽**下面**，之后读到的就是垃圾；
 *       实测症状（未修前）：clang 给 `void _start(void)` 生成了 7 条 push，然后
 *       `movq %rsp, sp` —— argc 读成一个野值（0x100001BF74 量级），程序拿它算 argv 越界，
 *       #PF cr2=0x90001EA20（err=0x4，用户态读未映射页）。
 *   * 所以这里只用汇编写 4 条指令：把初始 rsp 当参数交给 C 主体，再按 ABI 对齐后 call。
 *
 * 约定（与 user/apps/dynhello.c 的 dh_main 配对，别各改一半）：
 *   入口符号 `_start`（lld 的默认 -e 就是它）；C 侧 `void dh_main(unsigned long* sp)`。
 *
 * 为什么还有个 hlt：dh_main 正常路径是 exit(60) 不返回；万一它返回了，绝不能让 CPU 继续
 * 往下执行（.text 后面是别的函数）——halt 比乱跑好，且 `-no-reboot` 下现场还在。
 */
    .text
    .globl _start
    .type  _start, @function
_start:
    movq  %rsp, %rdi                /* rdi = 初始栈指针（argc 所在处） */
    andq  $-16, %rsp                /* SysV：call 之前 16 字节对齐 */
    call  dh_main
    hlt
    .size _start, .-_start
