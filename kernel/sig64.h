// sig64.h - ★ A4-5：**信号投递**（ring3 进程真正收到信号 + 用户栈上构造返回帧 + rt_sigreturn 恢复）
//
// ============================ 本文件解决什么 ============================
// 批次 C 之前，内核里的"信号"只是**记录**：rt_sigaction 把 handler 抄进进程表、rt_sigprocmask
// 把屏蔽字抄下来、kill(9/15) 直接打死 —— 没有任何一条路径让目标进程**真的收到**信号。
// 本模块补的就是这一层（make / 交互式编辑器这类程序的前提）：
//   1) kill(pid)/kill(-pgid)/终端 Ctrl+C  -> 目标进程**未决位**置位，并在"回到用户态"的时刻投递；
//   2) rt_sigaction 注册的 handler -> 在**用户栈**上压一个 Sig64Frame64 帧、rip 指向 handler
//      （用户 libc 的 restorer 桩做返回地址）；handler 里的 `rt_sigreturn(15)` 按帧恢复被打断的现场；
//   3) 默认动作：可终止信号 -> 该进程死亡，退出码 = 128+sig（父进程 wait4 看得到）；
//      SIGKILL/SIGSTOP 不可捕获/不可忽略/不可阻塞；
//   4) rt_sigprocmask 真生效（阻塞期间只挂未决，解除后投递）；SIG_IGN 真忽略；
//   5) **用户态异常只杀该进程**：ring3 的 #PF/#GP/#UD/#DE/... 走这里变成 SIGSEGV/SIGILL/SIGFPE…
//      （以前是 [PANIC] cpu exception + 停机，整个内核跟着死）。
//
// ============================ 投递时机（三条路径，缺一不可）============================
//   1) **系统调用出口**（syscall64.cpp：syscall 指令路径与 int 0x80 路径各一处）：
//      阻塞在 sleep/wait4 里的进程醒来时立刻投递；
//   2) **IRQ0 抢占出口**（task64.cpp 的 schedule64）：被抢占的 ring3 现场（r->cs == 0x2B）
//      与"即将切进去的任务帧"各投一次 —— 纯用户态死循环（不调系统调用）的程序因此
//      也能在 ~4ms 内收到信号；
//   3) **CPU 异常**（x86_64.cpp 的 isr_handler64，weak 钩子）：只杀当前进程 / 或交给 handler。
//   所有投递点都在"已经站在目标进程的地址空间里"（CR3 正确）时调用 —— 帧要写进用户栈，
//   校验页表靠的就是 user64_range_ok64()。
//
// ============================ 帧布局（用户栈）============================
//   handler 入口 rsp -> [restorer 返回地址]（16 字节对齐后 -8：SysV 要求 entry 时 rsp%16==8）
//                      [Sig64Frame64]（rip/rflags/rsp + 15 个 GPR + 原屏蔽字 + 魔数）
//   handler 用 `ret` 回到 restorer（user/lib 的 __v64_sigreturn_stub：`mov $15,%rax; syscall`），
//   那时 rsp 正好指向 Sig64Frame64 —— rt_sigreturn 就按它恢复（magic 不对 = -EFAULT，不猜）。
//
// ============================ 如实边界（没做的，别当做了）============================
//   * 没有 SA_SIGINFO/siginfo（handler 只有 int sig 一个参数）、没有 sigaltstack/SA_ONSTACK、
//     没有 SA_RESTART（被信号打断的系统调用**不自动重启**）、没有 SA_RESETHAND、没有实时信号排队
//     （同一号只挂一位）、没有 SIGSTOP/SIGTSTP 作业控制（收到只打一行 unsupported）；
//   * SIGCHLD 沿用既有语义（父进程 sigchld 标志 + wait4），不产生投递；
//   * 进程组只有一层（pgid），没有会话（session）、没有 tcsetpgrp：终端前台进程组 =
//     "最近一次 setpgid(0,0)（自立进程组）的进程" —— 见下面 sig64_tty_set_fg64 的说明。
//
// 串口打点（自动验收 tests/sig64_test.py grep，格式勿改；全部有预算，防刷屏）：
//   [SIG64] selftest PASS / [SIG64] selftest FAIL mask=<n>
//   [SIG64] action pid=<n> sig=<n> handler=0x<hex> flags=0x<hex>
//   [SIG64] send pid=<n> sig=<n> src=<kill|kill-group|tty-int|fault|raise>
//   [SIG64] deliver pid=<n> sig=<n> handler=0x<hex> frame=0x<hex> where=<syscall|irq|fault>
//   [SIG64] default action pid=<n> sig=<n> exit=<128+sig> [fault=#PF err=0x<hex> cr2=0x<hex>]
//   [SIG64] pending/procmask pid=<n> mask=0x<hex> pending=0x<hex> how=<n>
//   [SIG64] ignored pid=<n> sig=<n> src=<...>            （SIG_IGN）
//   [SIG64] tty fg pgrp=<n> prev=<n>                     （setpgid(0,0) 登记前台进程组）
//   [SIG64] tty-int send pgrp=<n> sig=2 n=<n>            （终端 Ctrl+C -> 前台进程组）
//   [SIG64] tty-int no-fg                                （没有登记过前台进程组：Ctrl+C 丢弃）
//   [SIG64] exit pid=<n> sig=<n> code=<n> by_signal=1
#pragma once
#include <stdint.h>
#include "x86_64.h"     // pt_regs64 / SEL64_UCODE

// ==================== 号段与语义常量（Linux x86_64 口径）====================
static const int SIG64_NSIG = 32;                   // 位图宽度（既有 PROC64_SIG_MAX 同口径）

enum Sig64Num : int {
    SIG64_HUP = 1,  SIG64_INT = 2,   SIG64_QUIT = 3,  SIG64_ILL = 4,  SIG64_TRAP = 5,
    SIG64_ABRT = 6, SIG64_BUS = 7,   SIG64_FPE = 8,   SIG64_KILL = 9, SIG64_USR1 = 10,
    SIG64_SEGV = 11, SIG64_USR2 = 12, SIG64_PIPE = 13, SIG64_ALRM = 14, SIG64_TERM = 15,
    SIG64_STKFLT = 16, SIG64_CHLD = 17, SIG64_CONT = 18, SIG64_STOP = 19, SIG64_TSTP = 20,
    SIG64_TTIN = 21, SIG64_TTOU = 22, SIG64_URG = 23, SIG64_XCPU = 24, SIG64_XFSZ = 25,
    SIG64_VTALRM = 26, SIG64_PROF = 27, SIG64_WINCH = 28, SIG64_IO = 29, SIG64_PWR = 30,
    SIG64_SYS = 31
};

static const uint64_t SIG64_DFL64 = 0;              // SIG_DFL
static const uint64_t SIG64_IGN64 = 1;              // SIG_IGN

// rt_sigaction 的 flags（Linux 值；本内核只认 SA_NODEFER 的作用，其余记录并如实标注）
static const uint64_t SIG64_SA_NOCLDSTOP64 = 0x00000001ULL;
static const uint64_t SIG64_SA_NOCLDWAIT64 = 0x00000002ULL;
static const uint64_t SIG64_SA_SIGINFO64   = 0x00000004ULL;
static const uint64_t SIG64_SA_ONSTACK64   = 0x08000000ULL;
static const uint64_t SIG64_SA_RESTART64   = 0x10000000ULL;
static const uint64_t SIG64_SA_NODEFER64   = 0x40000000ULL;
static const uint64_t SIG64_SA_RESETHAND64 = 0x80000000ULL;

// rt_sigprocmask 的 how（Linux 值）
static const uint32_t SIG64_SIG_BLOCK64   = 0;
static const uint32_t SIG64_SIG_UNBLOCK64 = 1;
static const uint32_t SIG64_SIG_SETMASK64 = 2;

// 用户栈上的帧（POD；handler 的 restorer 返回后 rsp 正好指向它）
static const uint64_t SIG64_FRAME_MAGIC64 = 0x5349473634465241ULL;   // "SIG64FRA"
struct Sig64Frame64 {
    uint64_t magic;                 // +0  固定魔数（rt_sigreturn 的判据，不猜）
    uint64_t sig;                   // +8  信号号
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;      // +16 .. +72
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;         // +80 .. +128
    uint64_t rip, rflags, rsp;      // +136 被信号打断处（rt_sigreturn 回到这里）
    uint64_t blocked;               // +160 被信号打断时的屏蔽字（rt_sigreturn 恢复它）
};

// ==================== 每进程信号状态（内嵌在 Proc64 里）====================
// 为什么放在 Proc64 里而不是本模块自己开表：信号的未决/屏蔽/handler 都是**进程属性**
// （fork 继承 handler/mask、execve 保留 mask/重置 handler），和进程表同一生命周期最省事。
struct Sig64State64 {
    uint64_t pending;                       // 未决位图（bit = 信号号）
    uint64_t blocked;                       // 阻塞位图（rt_sigprocmask 的真值）
    uint64_t handler[SIG64_NSIG];           // SIG_DFL / SIG_IGN / 用户 handler 地址
    uint64_t flags[SIG64_NSIG];             // 注册时的 flags（原样记录）
    uint64_t restorer[SIG64_NSIG];          // 注册时的 restorer（handler 的返回地址）
    uint64_t act_mask[SIG64_NSIG];          // 注册时给的 mask（交付时并入 blocked）
    uint32_t delivered;                     // 已投递（进 handler）次数
    uint32_t faults;                        // 由用户态 CPU 异常触发的次数
};

// ==================== 每进程状态操作（纯本地，不看进程表）====================
void sig64_state_init64(Sig64State64* st);
int  sig64_state_copy64(Sig64State64* dst, const Sig64State64* src);   // fork 继承（handler/mask 一起）

// rt_sigaction 的落地：sig <= 0/>= NSIG 或 SIGKILL/SIGSTOP -> -EINVAL(22)；成功 0。
// old_out != nullptr 时写回旧的 handler（Linux 的 oldact；本内核只给 handler 一项）。
int64_t sig64_state_action64(Sig64State64* st, int sig, uint64_t handler, uint64_t flags,
                            uint64_t restorer, uint64_t mask, uint64_t* old_handler_out, int pid);

// rt_sigprocmask 的落地：how = SIG_BLOCK/UNBLOCK/SETMASK。*old_out 写回旧屏蔽字。
// SIGKILL/SIGSTOP 永远进不了屏蔽字（Linux 同）。返回 0；how 非法 -EINVAL(22)。
int64_t sig64_state_procmask64(Sig64State64* st, uint32_t how, uint64_t set, uint64_t* old_out, int pid);

// 默认动作分类：1 = 终止进程、2 = 忽略（SIGCHLD/SIGURG/SIGWINCH/SIGCONT）、3 = 停止（未实现）
int sig64_default_class64(int sig);
// 默认动作的退出码（128+sig；供打点与 [PROC64] exit 用）
uint32_t sig64_term_code64(int sig);

// ==================== 系统调用落点（syscall64.cpp 直接转调）====================
int64_t sig64_sys_rt_sigaction64(uint64_t sig, uint64_t act_va, uint64_t old_va, uint64_t sigsetsize);
int64_t sig64_sys_rt_sigprocmask64(uint64_t how, uint64_t set_va, uint64_t old_va, uint64_t sigsetsize);
int64_t sig64_sys_rt_sigreturn64(pt_regs64* r);      // case 15：按用户栈上的帧恢复现场
int64_t sig64_sys_kill64(int pid, int sig);          // case 62：解析单个/进程组/全部

// ==================== 投递点（由 syscall64 / task64 调用）====================
// r 必须是"即将返回用户态"的帧：r->cs == SEL64_UCODE 才会投递（内核帧直接返回）。
// where = "syscall" / "irq" / "fault"（打点用）。
void sig64_deliver64(pt_regs64* r, const char* where);
// ★ 必须是 extern "C"：x86_64.cpp 的异常路径用**弱引用**取它（安装介质内核不链 sig64.cpp），
//   弱符号按 C 名解析 —— 若这里是 C++ 名字（_Z19sig64_user_fault64mP9pt_regs64），弱引用永远
//   解析不到、静默变成 0（症状：用户态 #PF/#GP 照旧 [PANIC]，本模块一行都进不来 —— 实测踩过）。
extern "C" int sig64_user_fault64(uint64_t int_no, pt_regs64* r);

// ==================== 终端前台进程组（最小作业控制替代）====================
// 为什么是这套语义（如实说明）：本内核没有会话（session）/tcsetpgrp/setpgid 全家桶，
// 所以"前台进程组"用一条最简可行的规则定义：**哪个进程自立了进程组（setpgid(0,0)），
// 它就是终端会话的前台作业**。交互式编辑器启动时做这一步 -> 终端 Ctrl+C 只打它，
// 不会连累 shell（否则 shell 与子进程同组，Ctrl+C 会把 shell 一起打死）。
void sig64_tty_set_fg64(int pgid);
int  sig64_tty_fg64();
int64_t sig64_tty_int64();                            // 终端 Ctrl+C：给前台进程组发 SIGINT

// ==================== 启动期自检 / 演示 ====================
int sig64_selftest64();                               // 位掩码自检（0 = 全过）
// 打点统计（启动期演示收尾打一行：被预算节流掉多少行，绝不静默丢证据）
uint32_t sig64_suppressed64();
