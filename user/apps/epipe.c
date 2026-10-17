/* epipe.c - ★ v0.4.9：SIGPIPE **端到端自证**程序（单进程，不需要 shell / fork / busybox）
 *
 * 为什么要单独一个程序：
 *   v0.4.8 内核已经在"管道读端全关的写路径"上置了 SIGPIPE 未决位（kernel/fd64.cpp），但
 *   验收一直卡在**造不出 EPIPE 现场**上 —— 串口里敲 `sh` 拉不起 ring3 shell（那份夹具盘
 *   system.img 里根本没有 VimtuFS2 卷，所以没有 /bin/shell.bin），busybox 的 `yes | head`
 *   也就跑不起来。其实"读者提前退出"这件事单进程就能做：**自己关掉自己的读端**再写。
 *
 * 三段证据（一份进程内自证，任何一段不合预期都会被 tests/sigpipe64_test.py 抓住）：
 *   ① baseline：读者在场 -> write 返回 2、read 拿回 "ok"（证明同一份写路径平时**不**投递信号）
 *   ② SIG_IGN：读端全关后写 -> 返回 -EPIPE(32)，进程**活着**（证明 ignore 分支不是"顺手杀掉"）
 *   ③ SIG_DFL：同样条件再写一次 -> 被默认动作终止（内核 exit=141 = 128+13），
 *      所以最后那行 "SURVIVED" **不该**出现在串口里 —— 它同时是反相对照（有牙齿的判据）。
 *
 * 交付方式（与 /sig64.elf、/pipe64.elf 同一条纪律）：build64.sh 用自研 user/lib 编成静态
 *   ELF64（链接脚本 user/apps/evshm_demo.ld），**不内嵌内核**，由 tools/demo_pack_win.py
 *   写进系统卷 /epipe.elf；终端里 `run /epipe.elf` 就能跑（`run` 是终端内建命令）。
 *
 * 系统调用口径（务实混用，两边都是本内核的真 ABI）：
 *   pipe/read/write/close 走 Linux x86_64 号段（22/0/1/3，`syscall` 指令）—— 自有 ABI 没有 pipe；
 *   signal() 走自有 ABI（int 0x80）的 user/lib 封装。两份 ABI 共用同一个进程的信号状态。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include "vimtu64.h"

#define LNR_READ   0
#define LNR_WRITE  1
#define LNR_CLOSE  3
#define LNR_PIPE  22

/* Linux ABI 裸系统调用：rax = 号，rdi/rsi/rdx = 参数。clang 在这套 -mcmodel=large 下支持。 */
static long lx3(long nr, long a, long b, long c) {
    long r;
    __asm__ __volatile__("syscall"
                         : "=a"(r)
                         : "a"(nr), "D"(a), "S"(b), "d"(c)
                         : "rcx", "r11", "memory");
    return r;
}

static void put(const char* s) { printf("%s", s); }

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;

    int fds[2];
    fds[0] = -1;
    fds[1] = -1;
    const long rc_pipe = lx3(LNR_PIPE, (long)(uintptr_t)fds, 0, 0);
    printf("epipe: pipe rc=%d rfd=%d wfd=%d\n", (int)rc_pipe, fds[0], fds[1]);
    if (rc_pipe < 0 || fds[0] < 0 || fds[1] < 0) {
        printf("epipe: ABORT (no pipe: kernel fd64 没有建出管道)\n");
        return 1;
    }

    /* ---- ① baseline：读者在场。写成功 + 读回原字节 ---- */
    const long w0 = lx3(LNR_WRITE, fds[1], (long)(uintptr_t)"ok", 2);
    char back[8];
    for (int i = 0; i < 8; i++) back[i] = 0;
    const long r0 = lx3(LNR_READ, fds[0], (long)(uintptr_t)back, 2);
    printf("epipe: baseline write=%d read=%d data=%s\n", (int)w0, (int)r0, back);

    /* ---- ② SIG_IGN：读端全关 -> write 返回 -EPIPE，但**不许**杀进程 ---- */
    signal(SIGPIPE, SIG_IGN);
    const long rc_close = lx3(LNR_CLOSE, fds[0], 0, 0);
    const long w1 = lx3(LNR_WRITE, fds[1], (long)(uintptr_t)"x", 1);
    printf("epipe: ign close=%d write=%d (expect -32) alive=1\n", (int)rc_close, (int)w1);

    /* ---- ③ SIG_DFL：同一条件再写 -> 默认动作终止（exit=141）。下面这行不该出现 ---- */
    signal(SIGPIPE, SIG_DFL);
    const long w2 = lx3(LNR_WRITE, fds[1], (long)(uintptr_t)"y", 1);
    put("epipe: SURVIVED");
    printf(" write=%d alive=1\n", (int)w2);
    return 0;
}
