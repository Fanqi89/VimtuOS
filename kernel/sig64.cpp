// sig64.cpp - ★ A4-5：信号投递的真实实现（投递决策 / 用户栈信号帧 / rt_sigreturn / 用户态异常）
//
// 设计、帧布局、打点格式、如实边界：**全部写在 kernel/sig64.h 的文件头**（唯一真源）。
// 本文件只实现。三件事在这里，别混：
//   1) **决策**：未决位 + 屏蔽字 + handler/SIG_IGN/默认动作 -> 投递 or 记未决 or 杀掉；
//   2) **投递**：在用户栈上构造 Sig64Frame64 并改写即将返回用户态的 pt_regs64 帧；
//   3) **恢复**：rt_sigreturn(15) 按帧恢复被信号打断的现场（含原屏蔽字）。
// 进程表（pgid / 谁是谁 / 杀哪个进程）在 proc64.cpp 里 —— 本文件只通过下面这些**弱引用**
// 访问它（安装介质内核不链 proc64.cpp：那时这些符号是 0，本模块只做参数校验与如实返回）。
#include "sig64.h"
#include "usermode64.h"     // user64_range_ok64 / user64_exit_to_kernel64
#include "debug64.h"        // dbg64_*（串口打点）

// ==================== proc64 侧的弱引用（安装介质内核里为 0）====================
// ★ 弱符号：这些函数是 C++ 名字，声明必须与 kernel/proc64.h 逐字一致（否则名字对不上，
//   弱引用永远解析不到、静默变成 0 —— task64.h 里记过同一个坑）。
struct Sig64State64;
Sig64State64* proc64_sig_state_of64(int pid)                    __attribute__((weak));
Sig64State64* proc64_sig_state_current64()                      __attribute__((weak));
int  proc64_sig_die_current64(int sig, pt_regs64* r)             __attribute__((weak));
int  proc64_sig_die_other64(int pid, uint32_t code, int sig)     __attribute__((weak));
int  proc64_pgid_of64(int pid)                                   __attribute__((weak));
int  proc64_proc_pid_at64(int idx)                               __attribute__((weak));
int  proc64_current_pid64()                                      __attribute__((weak));

// syscall 指令路径的"本帧已 exit"开关（定义在 syscall64.cpp；两份内核都链它）。
// 为什么这里要设它：把 syscall 帧改成 ring0 蹦床之后，syscall 入口**不能再 sysret**
// （CS=0x08 的帧走 sysret 会按 STAR 强行回 CPL=3 -> #GP）。int 0x80 路径不需要它（iretq 自己处理）。
extern "C" uint64_t g_syscall64_exit_to_kernel64;
static const int64_t  SIG64_EINVAL = 22, SIG64_EFAULT = 14, SIG64_ESRCH = 3;

// ==================== 打点（有预算，防刷屏）====================
static int      g_sig64_budget64    = 256;    // 明细行预算
static uint32_t g_sig64_suppressed64 = 0;
static uint32_t g_sig64_poll_budget64 = 32;   // pending/procmask 行的预算（更少，更容易刷屏）
static bool sig64_log_ok64() {
    if (g_sig64_budget64 > 0) { g_sig64_budget64--; return true; }
    g_sig64_suppressed64++;
    return false;
}
// 两对 K/V + 尾巴字符串（K/V 的数值分别是十进制与十六进制两种形态）
static void sig64_log2u_s64(const char* a, uint64_t x, const char* b, uint64_t y, const char* tail) {
    if (!sig64_log_ok64()) return;
    dbg64_line_begin64();
    dbg64_str(a); dbg64_dec(x);
    dbg64_str(b); dbg64_dec(y);
    if (tail) dbg64_str(tail);
    dbg64_nl();
    dbg64_line_end64();
}
static void sig64_log2u_h64(const char* a, uint64_t x, const char* b, uint64_t hex, const char* tail) {
    if (!sig64_log_ok64()) return;
    dbg64_line_begin64();
    dbg64_str(a); dbg64_dec(x);
    dbg64_str(b); dbg64_hex64(hex);
    if (tail) dbg64_str(tail);
    dbg64_nl();
    dbg64_line_end64();
}
// [SIG64] send pid=<n> sig=<n> src=<kill|kill-group|tty-int|fault|raise>
static void sig64_log_send64(int pid, int sig, const char* src) {
    if (!sig64_log_ok64()) return;
    dbg64_line_begin64();
    dbg64_str("[SIG64] send pid=");
    dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_str(" src=");
    dbg64_str(src ? src : "?");
    dbg64_nl();
    dbg64_line_end64();
}
uint32_t sig64_suppressed64() { return g_sig64_suppressed64; }
int      sig64_budget_left64() { return g_sig64_budget64; }

// ==================== 默认动作表 ====================
// 1 = 终止、2 = 忽略、3 = 停止（本内核不实现作业控制，收到只打一行如实说明）
static const uint8_t g_sig64_default64[SIG64_NSIG] = {
    /* 0  */ 2,
    /* 1  HUP  */ 1, /* 2  INT  */ 1, /* 3  QUIT */ 1, /* 4  ILL  */ 1,
    /* 5  TRAP */ 1, /* 6  ABRT */ 1, /* 7  BUS  */ 1, /* 8  FPE  */ 1,
    /* 9  KILL */ 1, /* 10 USR1 */ 1, /* 11 SEGV */ 1, /* 12 USR2 */ 1,
    /* 13 PIPE */ 1, /* 14 ALRM */ 1, /* 15 TERM */ 1, /* 16 STKFLT */ 1,
    /* 17 CHLD */ 2, /* 18 CONT */ 2, /* 19 STOP */ 3, /* 20 TSTP */ 3,
    /* 21 TTIN */ 3, /* 22 TTOU */ 3, /* 23 URG  */ 2, /* 24 XCPU */ 1,
    /* 25 XFSZ */ 1, /* 26 VTALRM */ 1, /* 27 PROF */ 1, /* 28 WINCH */ 2,
    /* 29 IO   */ 1, /* 30 PWR  */ 1, /* 31 SYS  */ 1
};
int sig64_default_class64(int sig) {
    if (sig <= 0 || sig >= SIG64_NSIG) return 1;
    return (int)g_sig64_default64[sig];
}
uint32_t sig64_term_code64(int sig) { return (uint32_t)(128 + sig); }

// ==================== 每进程状态操作 ====================
void sig64_state_init64(Sig64State64* st) {
    if (!st) return;
    uint8_t* p = (uint8_t*)st;
    for (uint32_t i = 0; i < sizeof(Sig64State64); i++) p[i] = 0;
}
int sig64_state_copy64(Sig64State64* dst, const Sig64State64* src) {
    if (!dst || !src) return -1;
    // fork 继承：handler/flags/restorer/act_mask/blocked 全抄；**未决位清 0**（子进程不是收到信号的那个）。
    for (uint32_t i = 0; i < sizeof(Sig64State64); i++) ((uint8_t*)dst)[i] = ((const uint8_t*)src)[i];
    dst->pending  = 0;
    dst->delivered = 0;
    dst->faults    = 0;
    return 0;
}
int64_t sig64_state_action64(Sig64State64* st, int sig, uint64_t handler, uint64_t flags,
                             uint64_t restorer, uint64_t mask, uint64_t* old_handler_out, int pid) {
    if (!st) return -SIG64_EINVAL;
    if (sig <= 0 || sig >= SIG64_NSIG) return -SIG64_EINVAL;
    // SIGKILL/SIGSTOP：不可捕获、不可忽略（Linux 同；调用方已在更外层拦一次，这里再钉死一层）
    if (sig == SIG64_KILL || sig == SIG64_STOP) {
        sig64_log2u_s64("[SIG64] action deny pid=", (uint64_t)(uint32_t)pid, " sig=", (uint64_t)sig, " reason=uncatchable");
        return -SIG64_EINVAL;
    }
    if (old_handler_out) *old_handler_out = st->handler[sig];
    st->handler[sig]   = handler;
    st->flags[sig]     = flags;
    st->restorer[sig]  = restorer;
    st->act_mask[sig]  = mask;
    if (handler == SIG64_IGN64) st->pending &= ~(1ULL << sig);        // 忽略 -> 未决位也清（Linux 语义）
    {

        uint64_t x = st->handler[sig];
        if (!sig64_log_ok64()) { /* 预算用完 */ }
        else {
            dbg64_line_begin64();
            dbg64_str("[SIG64] action pid=");
            dbg64_dec((uint64_t)(uint32_t)pid);
            dbg64_str(" sig=");
            dbg64_dec((uint64_t)sig);
            dbg64_str(" handler=0x");
            dbg64_hex64(x);
            dbg64_str(" flags=0x");
            dbg64_hex64(flags);
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    return 0;
}
int64_t sig64_state_procmask64(Sig64State64* st, uint32_t how, uint64_t set, uint64_t* old_out, int pid) {
    if (!st) return 0;                                  // 没有进程上下文：只做参数校验
    const uint64_t old = st->blocked;
    if (old_out) *old_out = old;
    uint64_t mask = old;
    if (how == SIG64_SIG_BLOCK64)        mask |= set;
    else if (how == SIG64_SIG_UNBLOCK64) mask &= ~set;
    else if (how == SIG64_SIG_SETMASK64) mask = set;
    else return -SIG64_EINVAL;
    mask &= ~((1ULL << SIG64_KILL) | (1ULL << SIG64_STOP));   // 这两号永远不可阻塞
    st->blocked = mask;
    if (g_sig64_poll_budget64 > 0) {
        g_sig64_poll_budget64--;
        dbg64_line_begin64();
        dbg64_str("[SIG64] pending/procmask pid=");
        dbg64_dec((uint64_t)(uint32_t)pid);
        dbg64_str(" how=");
        dbg64_dec((uint64_t)how);
        dbg64_str(" mask=0x");
        dbg64_hex64(mask);
        dbg64_str(" pending=0x");
        dbg64_hex64(st->pending);
        dbg64_nl();
        dbg64_line_end64();
    }
    return 0;
}

// ==================== 系统调用：rt_sigaction / rt_sigprocmask ====================
// struct sigaction（x86_64）：handler(+0) flags(+8) restorer(+16) mask(+24) —— 32 字节。
static int64_t sig64_copy_to_user64(uint64_t va, const void* src, uint64_t n) {
    if (!user64_range_ok64(va, n)) return -SIG64_EFAULT;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) ((uint8_t*)(uintptr_t)va)[i] = s[i];
    return 0;
}
int64_t sig64_sys_rt_sigaction64(uint64_t sig, uint64_t act_va, uint64_t old_va, uint64_t sigsetsize) {
    (void)sigsetsize;                                   // Linux 要求 == 8；本内核不校验（如实）
    const int pid = proc64_current_pid64 ? proc64_current_pid64() : -1;
    if (sig == 0 || sig >= 64) return -SIG64_EINVAL;
    Sig64State64* st = proc64_sig_state_current64 ? proc64_sig_state_current64() : nullptr;
    uint64_t handler = 0, flags = 0, restorer = 0, mask = 0;
    if (act_va) {
        if (!user64_range_ok64(act_va, 32)) return -SIG64_EFAULT;
        const uint64_t* a = (const uint64_t*)(uintptr_t)act_va;
        handler = a[0]; flags = a[1]; restorer = a[2]; mask = a[3];
    }
    if (act_va && (sig == SIG64_KILL || sig == SIG64_STOP)) {       // 不可捕获/忽略
        sig64_log2u_s64("[SIG64] action deny pid=", (uint64_t)(uint32_t)pid, " sig=", sig, " reason=uncatchable");
        return -SIG64_EINVAL;
    }
    uint64_t old_handler = 0;
    if (st && act_va) {
        const int64_t rc = sig64_state_action64(st, (int)sig, handler, flags, restorer, mask, &old_handler, pid);
        if (rc < 0) return rc;
    }
    if (old_va) {
        if (!user64_range_ok64(old_va, 32)) return -SIG64_EFAULT;
        uint64_t o[4];
        // oldact 的语义：**改之前**的注册值（没有进程上下文就全 0，与老实现一致）
        if (st) { o[0] = old_handler; o[1] = st->flags[sig]; o[2] = st->restorer[sig]; o[3] = st->act_mask[sig]; }
        else    { o[0] = 0; o[1] = 0; o[2] = 0; o[3] = 0; }
        const int64_t cr = sig64_copy_to_user64(old_va, o, 32);
        if (cr < 0) return cr;
    }
    return 0;
}
int64_t sig64_sys_rt_sigprocmask64(uint64_t how, uint64_t set_va, uint64_t old_va, uint64_t sigsetsize) {
    (void)sigsetsize;
    const int pid = proc64_current_pid64 ? proc64_current_pid64() : -1;
    Sig64State64* st = proc64_sig_state_current64 ? proc64_sig_state_current64() : nullptr;
    uint64_t set = 0;
    if (set_va) {
        if (!user64_range_ok64(set_va, 8)) return -SIG64_EFAULT;
        set = *(const uint64_t*)(uintptr_t)set_va;
    }
    uint64_t old = st ? st->blocked : 0;
    if (st && set_va) {
        const int64_t rc = sig64_state_procmask64(st, (uint32_t)how, set, &old, pid);
        if (rc < 0) return rc;
    }
    if (old_va) {
        if (!user64_range_ok64(old_va, 8)) return -SIG64_EFAULT;
        const int64_t cr = sig64_copy_to_user64(old_va, &old, 8);
        if (cr < 0) return cr;
    }
    return 0;
}

// ==================== 投递：用户栈上构造信号帧 ====================
static int sig64_push_frame64(Sig64State64* st, int sig, pt_regs64* r, const char* where) {
    const uint64_t handler  = st->handler[sig];
    const uint64_t restorer = st->restorer[sig];
    if (handler <= 1) return 0;                          // 不是用户 handler
    // 没有 restorer 桩 = 程序没告诉我们"怎么回来" -> 不投递（按默认动作处理）。绝不自作主张猜地址。
    if (!restorer) {
        sig64_log2u_s64("[SIG64] deliver FAILED pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                        " sig=", (uint64_t)sig, " reason=no-restorer");
        return 0;
    }
    if (!user64_range_ok64(r->rsp, 8)) {                 // 用户栈不可写：不投递
        sig64_log2u_s64("[SIG64] deliver FAILED pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                        " sig=", (uint64_t)sig, " reason=bad-stack");
        return 0;
    }
    const uint64_t need = (uint64_t)sizeof(Sig64Frame64) + 8;
    const uint64_t fr = (r->rsp - (uint64_t)sizeof(Sig64Frame64)) & ~0xFULL;   // 帧基址 16 字节对齐
    const uint64_t sp = fr - 8;                                                // handler 入口 rsp（对齐到 rsp%16==8）
    if (fr >= r->rsp || !user64_range_ok64(sp, need)) {
        sig64_log2u_s64("[SIG64] deliver FAILED pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                        " sig=", (uint64_t)sig, " reason=stack-range");
        return 0;
    }
    Sig64Frame64 f;
    f.magic  = SIG64_FRAME_MAGIC64;
    f.sig    = (uint64_t)sig;
    f.r15 = r->r15; f.r14 = r->r14; f.r13 = r->r13; f.r12 = r->r12;
    f.r11 = r->r11; f.r10 = r->r10; f.r9  = r->r9;  f.r8  = r->r8;
    f.rdi = r->rdi; f.rsi = r->rsi; f.rbp = r->rbp; f.rbx = r->rbx;
    f.rdx = r->rdx; f.rcx = r->rcx; f.rax = r->rax;
    f.rip = r->rip; f.rflags = r->rflags; f.rsp = r->rsp;
    f.blocked = st->blocked;
    // 写用户栈：帧 + 返回地址（restorer 桩）
    volatile uint64_t* dst = (volatile uint64_t*)(uintptr_t)fr;
    const uint64_t* src = (const uint64_t*)&f;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(Sig64Frame64) / 8); i++) dst[i] = src[i];
    *(volatile uint64_t*)(uintptr_t)sp = restorer;
    // 改写帧：进 handler
    r->rip = handler;
    r->rsp = sp;
    r->rdi = (uint64_t)(uint32_t)sig;                    // handler(int sig)
    r->rax = 0;
    // handler 期间屏蔽该信号（SA_NODEFER 例外）+ 注册时给的 mask
    uint64_t nb = st->blocked;
    if (!(st->flags[sig] & SIG64_SA_NODEFER64)) nb |= (1ULL << sig);
    nb |= st->act_mask[sig];
    st->blocked = nb;
    st->delivered++;
    {
        dbg64_line_begin64();
        dbg64_str("[SIG64] deliver pid=");
        dbg64_dec((uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1));
        dbg64_str(" sig=");
        dbg64_dec((uint64_t)sig);
        dbg64_str(" handler=0x");
        dbg64_hex64(handler);
        dbg64_str(" frame=0x");
        dbg64_hex64(fr);
        dbg64_str(" where=");
        dbg64_str(where ? where : "?");
        dbg64_nl();
        dbg64_line_end64();
    }
    return 1;
}

// 默认动作：终止当前进程（退出码 128+sig；父进程 wait4 看得到）
static int sig64_kill_current64(int sig, pt_regs64* r, const char* why) {
    const int pid = proc64_current_pid64 ? proc64_current_pid64() : -1;
    dbg64_line_begin64();
    dbg64_str("[SIG64] default action pid=");
    dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_str(" exit=");
    dbg64_dec((uint64_t)sig64_term_code64(sig));
    dbg64_str(" why=");
    dbg64_str(why ? why : "default");
    dbg64_nl();
    dbg64_line_end64();
    if (!r) return 0;
    if (!proc64_sig_die_current64) return 0;
    return proc64_sig_die_current64(sig, r);
}

void sig64_deliver64(pt_regs64* r, const char* where) {
    if (!r || r->cs != SEL64_UCODE) return;              // 只投"即将回用户态"的帧
    if (!proc64_sig_state_current64) return;
    Sig64State64* st = proc64_sig_state_current64();
    if (!st) return;
    // 一次调用最多投一个信号（handler 里再来的信号由下一次投递点处理），循环上限防死循环。
    for (int guard = 0; guard < 4; guard++) {
        const uint64_t ready = st->pending & ~st->blocked;
        if (!ready) return;
        int sig = 0;
        for (int i = 1; i < SIG64_NSIG; i++) if (ready & (1ULL << i)) { sig = i; break; }
        if (!sig) return;
        st->pending &= ~(1ULL << sig);
        const uint64_t h = st->handler[sig];
        if (h == SIG64_IGN64) {
            sig64_log2u_s64("[SIG64] ignored pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                            " sig=", (uint64_t)sig, " src=ign");
            continue;
        }
        if (h > 1 && sig != SIG64_KILL) {                 // SIGKILL 永远走默认动作（不可捕获）
            if (sig64_push_frame64(st, sig, r, where)) return;
        }
        const int cls = (sig == SIG64_KILL) ? 1 : sig64_default_class64(sig);
        if (cls == 2) {                                   // 默认忽略（SIGCHLD/SIGURG/SIGWINCH/SIGCONT）
            continue;
        }
        if (cls == 3) {                                   // 停止类信号：本内核不实现作业控制
            sig64_log2u_s64("[SIG64] unsupported pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                            " sig=", (uint64_t)sig, " (no job control)");
            continue;
        }
        if (sig64_kill_current64(sig, r, "default")) return;
        return;                                           // 没有进程上下文（任务 0）：不改（不能杀内核）
    }
}

// ==================== rt_sigreturn(15)：按用户栈上的帧恢复 ====================
int64_t sig64_sys_rt_sigreturn64(pt_regs64* r) {
    if (!r) return -SIG64_EINVAL;
    Sig64State64* st = proc64_sig_state_current64 ? proc64_sig_state_current64() : nullptr;
    const uint64_t fr = r->rsp;                            // restorer 桩 `ret` 之后 rsp 正指向帧
    if (!user64_range_ok64(fr, (uint64_t)sizeof(Sig64Frame64))) {
        sig64_log2u_h64("[SIG64] sigreturn FAILED pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                        " rsp=0x", fr, " reason=range");
        return -SIG64_EFAULT;
    }
    Sig64Frame64 f;
    const uint64_t* src = (const uint64_t*)(uintptr_t)fr;
    uint64_t* dst = (uint64_t*)&f;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(Sig64Frame64) / 8); i++) dst[i] = src[i];
    if (f.magic != SIG64_FRAME_MAGIC64 || f.sig == 0 || f.sig >= (uint64_t)SIG64_NSIG) {
        sig64_log2u_h64("[SIG64] sigreturn FAILED pid=", (uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1),
                        " magic=0x", f.magic, " reason=bad-frame");
        return -SIG64_EFAULT;
    }
    // 恢复现场（15 个 GPR + rip/rflags/rsp + 原屏蔽字）
    r->r15 = f.r15; r->r14 = f.r14; r->r13 = f.r13; r->r12 = f.r12;
    r->r11 = f.r11; r->r10 = f.r10; r->r9  = f.r9;  r->r8  = f.r8;
    r->rdi = f.rdi; r->rsi = f.rsi; r->rbp = f.rbp; r->rbx = f.rbx;
    r->rdx = f.rdx; r->rcx = f.rcx; r->rax = f.rax;
    r->rip = f.rip; r->rflags = f.rflags; r->rsp = f.rsp;
    if (st) st->blocked = f.blocked;
    {
        dbg64_line_begin64();
        dbg64_str("[SIG64] sigreturn pid=");
        dbg64_dec((uint64_t)(uint32_t)(proc64_current_pid64 ? proc64_current_pid64() : -1));
        dbg64_str(" sig=");
        dbg64_dec(f.sig);
        dbg64_str(" rip=0x");
        dbg64_hex64(f.rip);
        dbg64_str(" mask=0x");
        dbg64_hex64(f.blocked);
        dbg64_nl();
        dbg64_line_end64();
    }
    return (int64_t)f.rax;                                // 出口写回 rax = 被打断处的 rax（见函数头说明）
}

// ==================== kill(62) / 投递决策 ====================
static int64_t sig64_send_one64(int tpid, int sig, const char* src, int group) {
    const int cur = proc64_current_pid64 ? proc64_current_pid64() : -1;
    Sig64State64* st = proc64_sig_state_of64 ? proc64_sig_state_of64(tpid) : nullptr;
    if (!st) return -SIG64_ESRCH;
    const uint64_t bit = 1ULL << (uint32_t)sig;
    sig64_log_send64(tpid, sig, group ? "kill-group" : src);
    // SIGKILL：不可捕获/忽略/阻塞 -> **立刻**终止（别的进程用既有"打死 + 收尸"路径；
    //   自己就挂未决，本系统调用返回用户态前的投递钩子会把它变成退出码 137）。
    if (sig == SIG64_KILL && tpid != cur) {
        if (proc64_sig_die_other64 && proc64_sig_die_other64(tpid, 137u, SIG64_KILL)) {
            sig64_log2u_s64("[SIG64] default action pid=", (uint64_t)(uint32_t)tpid, " sig=9 exit=", 137, " why=kill");
            return 0;
        }
        return -SIG64_ESRCH;
    }
    if (st->handler[sig] == SIG64_IGN64 && sig != SIG64_KILL) {
        sig64_log2u_s64("[SIG64] ignored pid=", (uint64_t)(uint32_t)tpid, " sig=", (uint64_t)sig, " src=ign");
        return 0;
    }
    st->pending |= bit;
    if (st->blocked & bit) {
        if (g_sig64_poll_budget64 > 0) {
            g_sig64_poll_budget64--;
            dbg64_line_begin64();
            dbg64_str("[SIG64] pending/procmask pid=");
            dbg64_dec((uint64_t)(uint32_t)tpid);
            dbg64_str(" blocked=0x");
            dbg64_hex64(st->blocked);
            dbg64_str(" pending=0x");
            dbg64_hex64(st->pending);
            dbg64_str(" (delivered when unblocked)\n");
            dbg64_line_end64();
        }
    }
    return 0;
}
int64_t sig64_sys_kill64(int pid, int sig) {
    if (sig < 0 || sig >= 64) return -SIG64_EINVAL;
    if (sig == 0) return 0;                               // kill(pid, 0)：存在性检查（不投递）
    const int cur = proc64_current_pid64 ? proc64_current_pid64() : -1;
    int target_pgid = -1;
    if (pid > 0) {
        const int64_t r = sig64_send_one64(pid, sig, "kill", 0);
        if (r < 0) sig64_log2u_s64("[SIG64] send FAILED pid=", (uint64_t)(uint32_t)pid, " sig=", (uint64_t)sig, " (no such process)");
        return r;
    }
    if (pid == 0) {                                       // 自己的进程组
        if (cur <= 0) return -SIG64_ESRCH;
        target_pgid = proc64_pgid_of64 ? proc64_pgid_of64(cur) : cur;
    } else {                                              // -pgid（-1 = 全部）
        target_pgid = (pid == -1) ? -1 : -pid;
    }
    if (!proc64_proc_pid_at64) return -38;                // 没有进程表（安装介质内核）
    int n = 0;
    for (int i = 0; i < 16; i++) {                        // PROC64_MAX
        const int p = proc64_proc_pid_at64(i);
        if (p <= 0) continue;
        const int pg = proc64_pgid_of64 ? proc64_pgid_of64(p) : p;
        if (pid != -1 && pg != target_pgid) continue;
        if (pid == -1 && p == cur) continue;              // kill(-1)：除自己以外（Linux 也不打自己）
        if (sig64_send_one64(p, sig, "kill-group", 1) == 0) n++;
    }
    dbg64_line_begin64();
    dbg64_str("[SIG64] send-group pgrp=");
    dbg64_dec((uint64_t)(uint32_t)(pid == -1 ? 0 : target_pgid));
    dbg64_str(" sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" src=kill-group");
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 用户态 CPU 异常 -> 只杀该进程 ====================
static const char* sig64_exc_name64(uint64_t no) {
    switch (no) {
    case 0:  return "#DE";
    case 1:  return "#DB";
    case 3:  return "#BP";
    case 4:  return "#OF";
    case 5:  return "#BR";
    case 6:  return "#UD";
    case 7:  return "#NM";
    case 8:  return "#DF";
    case 10: return "#TS";
    case 11: return "#NP";
    case 12: return "#SS";
    case 13: return "#GP";
    case 14: return "#PF";
    case 16: return "#MF";
    case 17: return "#AC";
    case 18: return "#MC";
    case 19: return "#XM";
    case 20: return "#VE";
    case 21: return "#CP";
    default: return "#EXC";
    }
}
// 异常号 -> 信号（0 = 本内核不转换，保持老行为 PANIC：不可恢复的 #DF/#TS/#MC 等）
static int sig64_exc_sig64(uint64_t no) {
    switch (no) {
    case 0:  return SIG64_FPE;    // #DE 除零
    case 1:  return SIG64_TRAP;   // #DB
    case 3:  return SIG64_TRAP;   // #BP（int3）
    case 4:  return SIG64_FPE;    // #OF（into）
    case 5:  return SIG64_SEGV;   // #BR（bound）
    case 6:  return SIG64_ILL;    // #UD 非法指令
    case 7:  return SIG64_FPE;    // #NM 设备不可用（FPU）
    case 9:  return SIG64_SEGV;   // 协处理器段越界（历史号）
    case 11: return SIG64_SEGV;   // #NP
    case 12: return SIG64_BUS;    // #SS
    case 13: return SIG64_SEGV;   // #GP
    case 14: return SIG64_SEGV;   // #PF 缺页（最常见的一条）
    case 16: return SIG64_FPE;    // #MF x87
    case 17: return SIG64_BUS;    // #AC 对齐检查
    case 19: return SIG64_FPE;    // #XM SIMD
    case 20: return SIG64_SEGV;   // #VE
    case 21: return SIG64_SEGV;   // #CP
    default: return 0;            // #DF/#TS/#MC/...：不可恢复，如实保持 PANIC
    }
}
extern "C" int sig64_user_fault64(uint64_t int_no, pt_regs64* r) {
    if (!r || int_no >= 32) return 0;
    if ((r->cs & 3u) != 3u) return 0;                     // 只处理 ring3 的异常（内核异常照旧 PANIC）
    const int sig = sig64_exc_sig64(int_no);
    if (!sig) return 0;                                   // 不转换：交回 isr_handler64（PANIC）
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    const int pid = proc64_current_pid64 ? proc64_current_pid64() : -1;
    Sig64State64* st = proc64_sig_state_current64 ? proc64_sig_state_current64() : nullptr;
    if (st) st->faults++;
    dbg64_line_begin64();
    dbg64_str("[SIG64] fault pid=");
    dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" no=");
    dbg64_dec(int_no);
    dbg64_str(" ");
    dbg64_str(sig64_exc_name64(int_no));
    dbg64_str(" err=0x");
    dbg64_hex64(r->err_code);
    dbg64_str(" cr2=0x");
    dbg64_hex64(cr2);
    dbg64_str(" rip=0x");
    dbg64_hex64(r->rip);
    dbg64_str(" -> sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_nl();
    dbg64_line_end64();
    // 注册了 handler 且没被阻塞 -> 交给 handler（Linux 同：handler 能接管 #PF/#GP/...）
    if (st && st->handler[sig] > 1 && !(st->blocked & (1ULL << (uint32_t)sig))) {
        st->pending &= ~(1ULL << (uint32_t)sig);
        if (sig64_push_frame64(st, sig, r, "fault")) return 1;
    }
    // 否则：默认动作 = **只杀这个进程**（不再 PANIC 整个内核）
    dbg64_line_begin64();
    dbg64_str("[SIG64] default action pid=");
    dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_str(" exit=");
    dbg64_dec((uint64_t)sig64_term_code64(sig));
    dbg64_str(" fault=");
    dbg64_str(sig64_exc_name64(int_no));
    dbg64_str(" err=0x");
    dbg64_hex64(r->err_code);
    dbg64_str(" cr2=0x");
    dbg64_hex64(cr2);
    dbg64_nl();
    dbg64_line_end64();
    if (!proc64_sig_die_current64) return 0;
    if (!st) return 0;                                    // 没有进程上下文（任务 0）：如实交回 PANIC
    return proc64_sig_die_current64(sig, r);
}

// ==================== 终端前台进程组 + Ctrl+C ====================
static int g_sig64_tty_fg64 = 0;
void sig64_tty_set_fg64(int pgid) {
    if (pgid <= 0) return;
    const int prev = g_sig64_tty_fg64;
    g_sig64_tty_fg64 = pgid;
    dbg64_line_begin64();
    dbg64_str("[SIG64] tty fg pgrp=");
    dbg64_dec((uint64_t)(uint32_t)pgid);
    dbg64_str(" prev=");
    dbg64_dec((uint64_t)(uint32_t)prev);
    dbg64_nl();
    dbg64_line_end64();
}
int sig64_tty_fg64() { return g_sig64_tty_fg64; }
int64_t sig64_tty_int64() {
    const int pg = g_sig64_tty_fg64;
    if (pg <= 0) {
        if (sig64_log_ok64()) {
            dbg64_line_begin64();
            dbg64_str("[SIG64] tty-int no-fg (Ctrl+C dropped; no foreground process group registered)\n");
            dbg64_line_end64();
        }
        return 0;
    }
    const int64_t r = sig64_sys_kill64(-pg, SIG64_INT);    // kill(-pgid, SIGINT)
    dbg64_line_begin64();
    dbg64_str("[SIG64] tty-int send pgrp=");
    dbg64_dec((uint64_t)(uint32_t)pg);
    dbg64_str(" sig=2 rc=");
    dbg64_dec((uint64_t)(r < 0 ? (uint64_t)(-r) : 0));
    dbg64_nl();
    dbg64_line_end64();
    return r;
}

// ==================== 启动期自检 ====================
// bit0 默认动作表（四个"必须终止" + SIGCHLD/SIGWINCH 忽略 + STOP 类不实现）
// bit1 退出码 128+sig（SIGINT=130 / SIGTERM=143 / SIGSEGV=139 / SIGKILL=137）
// bit2 屏蔽字运算（BLOCK/UNBLOCK/SETMASK + SIGKILL 不可阻塞）
// bit3 状态拷贝（fork 继承 handler/mask，未决位清 0）+ handler 超界被拒
int sig64_selftest64() {
    int fail = 0;
    if (sig64_default_class64(SIG64_TERM) != 1 || sig64_default_class64(SIG64_INT) != 1 ||
        sig64_default_class64(SIG64_QUIT) != 1 || sig64_default_class64(SIG64_HUP) != 1 ||
        sig64_default_class64(SIG64_SEGV) != 1 || sig64_default_class64(SIG64_CHLD) != 2 ||
        sig64_default_class64(SIG64_WINCH) != 2 || sig64_default_class64(SIG64_STOP) != 3) fail |= 1;
    if (sig64_term_code64(SIG64_INT) != 130 || sig64_term_code64(SIG64_TERM) != 143 ||
        sig64_term_code64(SIG64_SEGV) != 139 || sig64_term_code64(SIG64_KILL) != 137) fail |= 2;
    {
        Sig64State64 st;
        sig64_state_init64(&st);
        uint64_t old = 0;
        if (sig64_state_procmask64(&st, SIG64_SIG_BLOCK64, (1ULL << SIG64_INT), &old, 0) != 0) fail |= 4;
        if (st.blocked != (1ULL << SIG64_INT)) fail |= 4;
        if (sig64_state_procmask64(&st, SIG64_SIG_BLOCK64, (1ULL << SIG64_KILL), &old, 0) != 0) fail |= 4;
        if (st.blocked & (1ULL << SIG64_KILL)) fail |= 4;            // SIGKILL 不可阻塞
        if (sig64_state_procmask64(&st, SIG64_SIG_UNBLOCK64, (1ULL << SIG64_INT), &old, 0) != 0) fail |= 4;
        if (st.blocked != 0) fail |= 4;
        if (old != (1ULL << SIG64_INT)) fail |= 4;                   // old = 上一个屏蔽字
        if (sig64_state_procmask64(&st, 9, 0, &old, 0) != -SIG64_EINVAL) fail |= 4;   // how 非法
        if (sig64_state_action64(&st, SIG64_KILL, 0x1000, 0, 0x2000, 0, nullptr, 0) != -SIG64_EINVAL) fail |= 4;
        if (sig64_state_action64(&st, 40, 0x1000, 0, 0x2000, 0, nullptr, 0) != -SIG64_EINVAL) fail |= 4;
        if (sig64_state_action64(&st, SIG64_USR1, 0x1000, 0, 0x2000, 0, nullptr, 0) != 0) fail |= 8;
        Sig64State64 c2;
        sig64_state_init64(&c2);
        c2.pending = 0x3FFULL;
        if (sig64_state_copy64(&c2, &st) != 0) fail |= 8;
        if (c2.handler[SIG64_USR1] != 0x1000 || c2.restorer[SIG64_USR1] != 0x2000) fail |= 8;
        if (c2.pending != 0) fail |= 8;                              // 未决位不继承
    }
    if (sizeof(Sig64Frame64) != 168) fail |= 16;                     // 帧布局自洽（21 x 8）
    if (SIG64_FRAME_MAGIC64 != 0x5349473634465241ULL) fail |= 16;
    return fail;
}
