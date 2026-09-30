/* signal.h - ★ A4-5：用户态信号接口（Vimtu64 用户态 C 运行时）

 *
 *
 * 与内核的对应关系（**唯一真源**是 kernel/sig64.h，这里只是用户侧的一份拷贝）：
 *   signal(sig, handler)      -> rt_sigaction(13)：内核在用户栈上构造信号帧、handler 返回时经
 *                                restorer 桩执行 rt_sigreturn(15) 恢复现场；
 *   sigaction(sig, act, old)  -> 同上（本内核只读 handler/flags/restorer/mask 四个字段）；
 *   sigprocmask(how, set, o)  -> rt_sigprocmask(14)：真阻塞（阻塞期间挂未决，解除后投递）；
 *   raise(sig)                -> kill(62) 给**自己**（内核允许自杀：投递点在系统调用出口）；
 *   kill(pid, sig)            -> kill(62)：pid>0 单个进程；== 0 自己的进程组；< 0 = 进程组 |pid|。
 *
 * restorer 桩：内核**不**假设 handler 怎么返回，它只把 `act.restorer` 当返回地址压栈，所以每个
 * 用户程序都必须提供一个"执行 rt_sigreturn 的小桩"。本运行时的 sigaction() 包装会**自动**把
 * 它填成 __v64_sigreturn_stub（user/lib/syscall.S）—— 调用方不用管。没填 restorer 的
 * rt_sigaction（例如手写裸系统调用）内核会如实拒绝投递（`[SIG64] deliver FAILED reason=no-restorer`）。
 *
 * 如实边界（与内核一致，别当成 POSIX 全套）：
 *   * 没有 siginfo（handler 只有 int 参数）、没有 sigaltstack/SA_RESTART/SA_RESETHAND、
 *     没有实时信号排队、没有 SIGSTOP/作业控制；
 *   * 默认动作里 SIGTERM/SIGINT/SIGQUIT/SIGHUP/SIGSEGV/... 都会让本进程以 128+sig 退出。
 */
#ifndef VIMTU64_SIGNAL_H
#define VIMTU64_SIGNAL_H

#include <stdint.h>
#include <stdint.h>
#include <stddef.h>   /* size_t（mmap 的 len） */
/* 信号号（Linux x86_64 口径；与 kernel/sig64.h 的 SIG64_* 一一对应） */
#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGSTKFLT 16
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22
#define SIGURG   23
#define SIGXCPU  24
#define SIGXFSZ  25
#define SIGVTALRM 26
#define SIGPROF  27
#define SIGWINCH 28
#define SIGIO    29
#define SIGPWR   30
#define SIGSYS   31
#define NSIG     32

/* handler 的两个特殊值（与内核一致：0 = SIG_DFL、1 = SIG_IGN） */
#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)

/* rt_sigaction 的 flags（本内核认 SA_NODEFER 的作用，其余原样记录，见 kernel/sig64.h） */
#define SA_NOCLDSTOP 0x00000001u
#define SA_NOCLDWAIT 0x00000002u
#define SA_SIGINFO   0x00000004u
/* signal() 失败时的返回值（POSIX：SIG_ERR = (void (*)(int))-1） */
#define SIG_ERR  ((void (*)(int))-1)
#define SA_ONSTACK   0x08000000u
#define SA_RESTART   0x10000000u
#define SA_NODEFER   0x40000000u
#define SA_RESETHAND 0x80000000u

/* sigprocmask 的 how */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* x86_64 的 struct sigaction = 32 字节（handler/flags/restorer/mask；本内核只读这四项） */
struct sigaction {
    void (*handler)(int);       /* +0  SIG_DFL / SIG_IGN / 用户函数 */
    unsigned long flags;        /* +8  SA_* */
    void (*restorer)(void);     /* +16 返回桩（sigaction() 包装自动填） */
    unsigned long mask;         /* +24 handler 期间额外屏蔽的位图 */
};

typedef unsigned long sigset_t;

/* 返回 0 = 成功、-1 = 失败（errno 见 errno.h）；内核返回 -errno 原样进 errno */
int  sigaction(int sig, const struct sigaction* act, struct sigaction* oldact);
void (*signal(int sig, void (*handler)(int)))(int);
int  sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int  raise(int sig);
int  kill(int pid, int sig);
int  setpgid(int pid, int pgid);

/* 返回桩本体（汇编）：`mov $15,%rax; syscall` —— rt_sigreturn(15) 按用户栈上的帧恢复现场。
 * 为什么放在这里而不是"每个程序自己写"：内核把它当返回地址压栈，任何 handler 都用得上。 */
void __v64_sigreturn_stub(void);
/* 本运行时自动填进 restorer 的那份地址（= __v64_sigreturn_stub 的地址） */
void (*vimtu64_sigreturn_addr(void))(void);

#endif /* VIMTU64_SIGNAL_H */
