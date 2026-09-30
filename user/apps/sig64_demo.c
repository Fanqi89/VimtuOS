/* sig64_demo.c - ★ A4-5 启动期演示：**信号真投递**（ring3 真进程；fork + signal + wait4）
 *
 * 交付方式（与 /evshm.elf、/proc64.elf 同一套路，**不内嵌进内核镜像之外的地方**）：
 *   build64.sh 用自研 user/lib 把它编成静态 ELF64（链接脚本 user/apps/evshm_demo.ld），
 *   objcopy 平铺字节嵌进**系统内核**，启动期由 kernel64.cpp 幂等地装进系统卷 /sig64.elf，
 *   再以**真进程**跑（fork 需要进程上下文，blob 路径没有）。
 *
 * 这个程序与内核打点一起构成的验收链（tests/sig64_test.py 逐条断言）：
 *   ① handler 投递 + rt_sigreturn 之后继续执行：handler 里计数/打印，返回后主流程照常往下走；
 *   ② 默认动作终止：子进程收 SIGTERM -> 退出码 143，父进程 wait4 看得到；
 *   ③ SIGKILL 不可捕获：sigaction(SIGKILL) -> EINVAL；再真 SIGKILL 一个子进程 -> 137；
 *   ④ rt_sigprocmask 真生效：阻塞期间只挂未决（handler 计数不变），解除后在**系统调用出口**投递；
 *   ⑤ 用户态错误地址访问只杀该进程：子进程写未映射地址 -> 内核 [SIG64] fault ... #PF -> SIGSEGV/139，
 *      父进程照常跑（内核没有 PANIC）；顺带 #GP（wrmsr）与 #DE（除零）；
 *   ⑥ kill(-pgid) 广播：父 + 两个子都注册了同号 handler，父 kill(-pgid) 后三边都收到；
 *   ⑦ 终端 Ctrl+C（前台进程组）：本程序一开始就 setpgid(0,0) 把自己登记成终端前台作业，
 *      内核演示看到 SIGINT handler 装好后调 sig64_tty_int64() 模拟终端 Ctrl+C。
 *
 * 输出约定（tests/sig64_test.py grep 这些前缀；串口上每行会被内核的 [SYSCALL] 打点穿插，
 * 所以断言按**片段**匹配）：
 *   sig64: ...       本程序的证据行
 */
#include <stdint.h>
#include <stddef.h>

#include "vimtu64.h"      /* 自有 ABI（write/getpid/sleep_ms/ticks） */
#include <signal.h>       /* ★ A4-5：signal/sigaction/sigprocmask/kill/raise/fork/wait4/setpgid */
#include <stdio.h>        /* printf（行缓冲 -> write(1) -> 内核控制台：串口 + 屏幕） */
#include <unistd.h>       /* write/getpid/_exit */

/* ---- handler 里**绝不能**用 printf（实测踩过）：printf 是 128 字节行缓冲，主流程被信号打断时
 *      它的半行还在缓冲里，handler 的 printf 会把那半行和 handler 的行一起刷出去 —— 串口日志里
 *      证据行被撕成两半（`sig64: sigaction(SIGKILL) rc=sig64: handler sig=2 ...`），断言就抓不住了。
 *      这里用"自己拼一行 + 一次 write()"（单次 write 在核心里是原子的：全程 IF=0，信号只能在
 *      系统调用**出口**投递，所以一行不会被劈开）。 ---- */
static void hraw(const char* s, int n) {
    (void)__v64_syscall(1, 1, (long)(uintptr_t)s, (long)n, 0, 0);
}
static void hline(const char* tag, int sig, int count) {
    char b[120];
    int n = 0;
    while (tag[n] && n < 80) { b[n] = tag[n]; n++; }
    if (sig >= 10) b[n++] = (char)('0' + (sig / 10) % 10);
    b[n++] = (char)('0' + sig % 10);
    const char* mid = " count=";
    for (int i = 0; mid[i]; i++) b[n++] = mid[i];
    char t[12]; int m = 0;
    if (count == 0) t[m++] = '0';
    while (count > 0 && m < 11) { t[m++] = (char)('0' + (count % 10)); count /= 10; }
    while (m > 0) b[n++] = t[--m];
    b[n++] = '\n';
    hraw(b, n);
}


/* 计数（volatile：会被信号打断/恢复，编译器不许把它缓存在寄存器里） */
static volatile int g_usr1_seen = 0;      /* kill(-pgid) 广播 */
static volatile int g_usr2_seen = 0;      /* 阻塞/未决 */
static volatile int g_int_seen  = 0;      /* 终端 Ctrl+C（tty-int） */
static volatile int g_term_seen = 0;      /* 默认动作演示里那个"没注册 handler"的信号（不该被本进程收到） */

/* handler 本体：只置标志 + 一次原子 write 打证据行（**不用 printf**，理由见上面那段） */
static void on_usr1(int sig) { g_usr1_seen++; hline("sig64: handler sig=", sig, g_usr1_seen); }
static void on_usr2(int sig) { g_usr2_seen++; hline("sig64: handler sig=", sig, g_usr2_seen); }
static void on_int(int sig)  { g_int_seen++;  hline("sig64: handler sig=", sig, g_int_seen); }

/* 子进程睡一会儿再退出（父进程在它睡着时打死它 -> 覆盖"阻塞在系统调用里也能收到"这条） */
static void child_sleep_forever(void) {
    for (int i = 0; i < 60; i++) vimtu64_sleep_ms(50);
    _exit(0);
}
/* 子进程：写一个**未映射**的用户窗口地址（4GiB+8MiB，窗口内、没有映射）-> ring3 #PF */
static void child_bad_pointer(void) {
    printf("sig64: child bad-pointer (expect SIGSEGV/139)\n");
    volatile uint64_t* p = (volatile uint64_t*)(uintptr_t)0x0000000100800000ULL;
    *p = 0xDEADBEEFULL;                        /* 这一句就是 #PF */
    _exit(0);                                  /* 到不了 */
}
/* 子进程：ring3 执行特权指令 wrmsr -> #GP */
static void child_bad_insn(void) {
    printf("sig64: child privileged-insn wrmsr (expect SIGSEGV/139)\n");
    __asm__ volatile("wrmsr" :: "c"(0u), "a"(0u), "d"(0u));
    _exit(0);
}
/* 子进程：除零 -> #DE */
static void child_div0(void) {
    printf("sig64: child div-by-zero (expect SIGFPE/136)\n");
    volatile int z = 0;
    volatile int r = 100 / z;
    (void)r;
    _exit(0);
}

/* 收一个子进程并打印它的退出状态（Linux 编码：WEXITSTATUS = (st>>8)&0xFF） */
static int reap(const char* what, int pid) {
    int st = 0;
    const int r = wait4(pid, &st, 0, NULL);
    printf("sig64: wait4 %s pid=%d ret=%d status=%d WEXITSTATUS=%d\n",
           what, pid, r, st, (st >> 8) & 0xFF);
    return (st >> 8) & 0xFF;
}

int main(void) {
    struct sigaction a;
    printf("sig64: demo start pid=%d\n", getpid());

    /* ★ 终端前台进程组：setpgid(0,0) 自立进程组 -> 内核登记它为会话前台作业（见 kernel/sig64.h）。
     *   放在最前面：后面内核演示模拟的"终端 Ctrl+C"就只打这一组（不连累 shell）。 */
    if (setpgid(0, 0) == 0) printf("sig64: setpgid(0,0) ok\n");
    else                    printf("sig64: setpgid(0,0) FAILED\n");

    /* ---- ① 注册三个 handler：USR1（广播）/ USR2（阻塞）/ INT（终端 Ctrl+C） ---- */
    a.handler = on_usr1; a.flags = 0; a.restorer = 0; a.mask = 0;
    if (sigaction(SIGUSR1, &a, NULL) != 0) { printf("sig64: sigaction(USR1) FAILED\n"); return 1; }
    a.handler = on_usr2;
    if (sigaction(SIGUSR2, &a, NULL) != 0) { printf("sig64: sigaction(USR2) FAILED\n"); return 1; }
    a.handler = on_int;
    if (sigaction(SIGINT, &a, NULL) != 0) { printf("sig64: sigaction(INT) FAILED\n"); return 1; }
    printf("sig64: handlers installed (usr1/usr2/int)\n");

    /* ---- ③ SIGKILL 不可捕获/不可忽略 ---- */
    a.handler = on_usr1;
    const int kk = sigaction(SIGKILL, &a, NULL);
    printf("sig64: sigaction(SIGKILL) rc=%d (expect -1/EINVAL)\n", kk);
    /* SIGTERM 的默认动作：先确认它**没有** handler（SIG_DFL） */
    a.handler = SIG_DFL;
    if (sigaction(SIGTERM, &a, NULL) != 0) printf("sig64: sigaction(TERM,DFL) FAILED\n");

    /* ---- ④ 阻塞期间不投递、解除后投递（时序证据） ---- */
    sigset_t blk = (sigset_t)1 << SIGUSR2;
    if (sigprocmask(SIG_BLOCK, &blk, NULL) != 0) printf("sig64: sigprocmask(BLOCK) FAILED\n");
    raise(SIGUSR2);
    /* 这一行 printf 是**系统调用**：内核在出口的投递钩子里看到未决 & 被阻塞 -> 只挂未决，不投 */
    printf("sig64: blocked: seen=%d (expect 0)\n", g_usr2_seen);
    if (sigprocmask(SIG_UNBLOCK, &blk, NULL) != 0) printf("sig64: sigprocmask(UNBLOCK) FAILED\n");
    /* 本系统调用的出口就会投递：handler 已经跑完了，这里必然看到 1 */
    printf("sig64: unblocked: seen=%d (expect 1)\n", g_usr2_seen);

    /* ---- ⑥ kill(-pgid) 广播：两个子进程 + 自己都在同一个进程组 ---- */
    {
        const int c1 = fork();
        if (c1 == 0) { printf("sig64: child1 up (in group)\n"); child_sleep_forever(); _exit(0); }
        const int c2 = fork();
        if (c2 == 0) { printf("sig64: child2 up (in group)\n"); child_sleep_forever(); _exit(0); }
        printf("sig64: fork children pid1=%d pid2=%d\n", c1, c2);
        vimtu64_sleep_ms(60);                       /* 让两个子进程把 handler 装好、进入睡眠 */
        const int pg = -getpid();                    /* 进程组 = 自己的 pid（setpgid(0,0) 之后） */
        printf("sig64: kill(-%d, SIGUSR1) broadcast\n", getpid());
        const int kr = kill(pg, SIGUSR1);
        printf("sig64: kill rc=%d parent_seen=%d (expect 1)\n", kr, g_usr1_seen);
        vimtu64_sleep_ms(80);
        /* 子进程收到广播后应该已经退出（handler 里计数、返回后继续 sleep -> 这里结束它们） */
        printf("sig64: children after broadcast parent_seen=%d\n", g_usr1_seen);
        kill(c1, SIGKILL);                           /* 收尾：SIGKILL 不可捕获（子进程没有 handler 也没用） */
        kill(c2, SIGKILL);
        const int e1 = reap("child1(kill9)", c1);
        const int e2 = reap("child2(kill9)", c2);
        printf("sig64: children exit codes=%d,%d (expect 137,137)\n", e1, e2);
    }

    /* ---- ② 默认动作终止：SIGTERM -> 143 ---- */
    {
        const int c = fork();
        if (c == 0) { child_sleep_forever(); _exit(0); }
        vimtu64_sleep_ms(60);
        printf("sig64: kill(pid=%d, SIGTERM) default action\n", c);
        kill(c, SIGTERM);
        const int e = reap("child(SIGTERM)", c);
        printf("sig64: default-action exit=%d (expect 143)\n", e);
    }

    /* ---- ⑤ 用户态异常只杀该进程（内核继续跑）---- */
    {
        const int c = fork();
        if (c == 0) child_bad_pointer();
        const int e = reap("child(#PF)", c);
        printf("sig64: bad-pointer exit=%d (expect 139)\n", e);
    }
    {
        const int c = fork();
        if (c == 0) child_bad_insn();
        const int e = reap("child(#GP)", c);
        printf("sig64: bad-insn exit=%d (expect 139)\n", e);
    }
    {
        const int c = fork();
        if (c == 0) child_div0();
        const int e = reap("child(#DE)", c);
        printf("sig64: div-zero exit=%d (expect 136)\n", e);
    }

    /* ---- ⑦ 等内核演示模拟的"终端 Ctrl+C"（前台进程组 -> SIGINT）：有界等待 ---- */
    {
        const unsigned long t0 = vimtu64_ticks();
        while (g_int_seen == 0 && (vimtu64_ticks() - t0) < 750u) vimtu64_sleep_ms(10);  /* ~3 秒 */
        printf("sig64: tty-int (Ctrl+C) seen=%d\n", g_int_seen);
    }

    printf("sig64: demo done usr1=%d usr2=%d int=%d term=%d\n",
           g_usr1_seen, g_usr2_seen, g_int_seen, g_term_seen);
    return 0;
}
