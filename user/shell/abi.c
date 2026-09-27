/* abi.c - ★ A4-1：ring3 shell 的 ABI 包装实现（每条都只有一行真正的 syscall，不夹带私货）
 *
 * 全都在**用户态**：这里是 shell 与内核之间唯一的系统调用落点，主逻辑（main.c）不直接碰
 * int 0x80 / syscall 指令，方便逐条核对"这条到底走的是哪个入口、错误码是什么口径"。
 */
#include "abi.h"

/* ==================== 自有 ABI（int 0x80）==================== */
long sh_intsys(long nr, long a1, long a2, long a3, long a4) {
    return __v64_int80(nr, a1, a2, a3, a4);
}

/* nr=5：真睡眠（内核走 task_sleep64；有调度器时让出 CPU 给别的任务）。
 * 为什么不用 musl 的 nanosleep/clock_nanosleep：那两条走 35/230，而 230 **内核没实现**
 * （会打一行 [SYSCALL] enosys）。自有 ABI 的 5 是本内核的真实现，且这里正好是自有 ABI。 */
int sh_sleep_ms(unsigned ms) {
    const long r = __v64_int80(5, (long)ms, 0, 0, 0);
    return (r < 0) ? -1 : 0;
}

/* ==================== Linux 兼容号段（syscall 指令；负值 = -errno）==================== */
long sh_sys(long nr, long a1, long a2, long a3, long a4, long a5) {
    return __v64_syscall(nr, a1, a2, a3, a4, a5);
}

/* nr=4：PIT tick（250Hz）。用途：shell 侧的有界超时（等内核抽干 out 环 / 等 ls 应答）。 */
unsigned long sh_ticks(void) {
    const long r = __v64_int80(4, 0, 0, 0, 0);
    return (r < 0) ? 0ul : (unsigned long)r;
}

int sh_open(const char* path, int flags) {
    return (int)sh_sys(2, (long)(uintptr_t)path, (long)flags, 0, 0, 0);
}

long sh_read(int fd, void* buf, unsigned long len) {
    return sh_sys(0, (long)fd, (long)(uintptr_t)buf, (long)len, 0, 0);
}

long sh_write(int fd, const void* buf, unsigned long len) {
    return sh_sys(1, (long)fd, (long)(uintptr_t)buf, (long)len, 0, 0);
}

int sh_close(int fd) {
    return (int)sh_sys(3, (long)fd, 0, 0, 0, 0);
}

int sh_stat(const char* path, void* st144) {
    const long r = sh_sys(4, (long)(uintptr_t)path, (long)(uintptr_t)st144, 0, 0, 0);
    return (r < 0) ? (int)r : 0;
}

int sh_fstat(int fd, void* st144) {
    const long r = sh_sys(5, (long)fd, (long)(uintptr_t)st144, 0, 0, 0);
    return (r < 0) ? (int)r : 0;
}

long sh_lseek(int fd, long off, int whence) {
    return sh_sys(8, (long)fd, off, (long)whence, 0, 0);
}

/* 32 = dup（自动分配）；33 = dup2（目标必须是 >=3 的 fd —— 目标 0/1/2 内核是 -ENOSYS，
 * 因为本内核的标准流不是一个可替换的 fd 对象）。shell 的重定向因此**不走 dup2**，
 * 而是把输出直接写进目标文件 fd（见 main.c 的 Out 抽象）。 */
int sh_dup(int oldfd, int newfd) {
    const long r = (newfd < 0) ? sh_sys(32, (long)oldfd, 0, 0, 0, 0)
                               : sh_sys(33, (long)oldfd, (long)newfd, 0, 0, 0);
    return (r < 0) ? (int)r : (int)r;
}

int sh_mkdir(const char* path, int mode) {
    const long r = sh_sys(83, (long)(uintptr_t)path, (long)mode, 0, 0, 0);
    return (r < 0) ? (int)r : 0;
}

int sh_unlink(const char* path) {
    const long r = sh_sys(87, (long)(uintptr_t)path, 0, 0, 0, 0);
    return (r < 0) ? (int)r : 0;
}

char* sh_getcwd(char* buf, unsigned long size) {
    const long r = sh_sys(79, (long)(uintptr_t)buf, (long)size, 0, 0, 0);
    return (r < 0) ? (char*)0 : buf;
}

/* ==================== 进程（外部命令的唯一通道：fork + execve + wait4）==================== */
int sh_fork(void) {
    return (int)sh_sys(57, 0, 0, 0, 0, 0);
}

int sh_execve(const char* path, char* const* argv, char* const* envp) {
    return (int)sh_sys(59, (long)(uintptr_t)path, (long)(uintptr_t)argv,
                       (long)(uintptr_t)envp, 0, 0);
}

/* nr=61：wait4(pid, status, options, rusage)。status 是 Linux 编码（退出码在 (st>>8)&0xFF）。 */
int sh_wait4(int pid, int* status, int options) {
    return (int)sh_sys(61, (long)pid, (long)(uintptr_t)status, (long)options, 0, 0);
}

void sh_exit_group(int code) {
    (void)sh_sys(231, (long)code, 0, 0, 0, 0);
    for (;;) { }                       /* 不该返回；真返回就停住，绝不落到未映射地址 */
}
