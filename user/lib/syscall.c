/* syscall.c - POSIX 名字的系统调用包装（Vimtu64 用户态 C 运行时）
 *
 * 分工（与 unistd.h 的表一致）：
 *   自有 ABI（int 0x80）：write / read / open(只读) / close / getpid / sleep_ms / ticks / exit
 *   Linux 兼容（syscall 指令）：open(带写标志) / lseek / stat / fstat / getcwd / unlink
 *   两条都没有：chdir -> -1 + errno = ENOSYS（如实，不假装换目录成功）
 *
 * errno 口径（见 errno.h）：
 *   * Linux 兼容路径的返回值就是 -errno -> 原样搬进 errno；
 *   * 自有 ABI 多数失败只给 -1（为什么：见 kernel/syscall64.h —— 老号段的错误码是粗的），
 *     这种情况置 EIO（具体原因内核在串口打了 [SYSCALL] deny / 对应模块的打点）；
 *     fb_map 的 -1..-4 是有意义的，按语义映射成 EPERM/EFAULT/ENODEV/ENOMEM。 */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "vimtu64.h"
#include <signal.h>   /* ★ A4-5：signal/sigaction/sigprocmask/kill/raise/fork/wait4/mmap/ioctl */

int errno = 0;          /* 单线程运行时的全局 errno（无 TLS，见 errno.h 的说明） */

/* 自有 ABI 的负返回值 -> errno（返回 -1，方便 `return v64_errno_own(r);` 的写法） */
static int v64_errno_own(long r) {
    switch (r) {
    case V64_FB_EPERM:  errno = EPERM;  break;
    case V64_FB_EFAULT: errno = EFAULT; break;
    case V64_FB_ENODEV: errno = ENODEV; break;
    case V64_FB_ENOMEM: errno = ENOMEM; break;
    default:            errno = EIO;    break;
    }
    return -1;
}
/* Linux 兼容路径：内核返回 -errno */
static int v64_errno_lx(long r) {
    errno = (int)(-r);
    return -1;
}

/* ---------------- 自有 ABI ---------------- */
ssize_t write(int fd, const void* buf, size_t len) {
    if (!buf && len != 0) { errno = EFAULT; return -1; }
    const unsigned char* p = (const unsigned char*)buf;
    size_t done = 0;
    /* 自有 ABI 的 write 单次上限 1024（内核 SYSCALL64_WRITE_MAX）：这里**自动分块**，
       超过也不会被内核判成参数错误（那会打 [SYSCALL] deny 并把整块丢掉）。 */
    while (done < len) {
        size_t n = len - done;
        if (n > V64_WRITE_MAX) n = V64_WRITE_MAX;
        const long r = __v64_int80(V64_NR_WRITE, (long)fd, (long)(uintptr_t)(p + done), (long)n, 0);
        if (r < 0) return v64_errno_own(r);
        if (r == 0) break;                 /* 内核说没写进去：别死循环 */
        done += (size_t)r;
        if ((size_t)r < n) break;          /* 短写：按实际写入返回 */
    }
    return (ssize_t)done;
}

ssize_t read(int fd, void* buf, size_t len) {
    if (!buf && len != 0) { errno = EFAULT; return -1; }
    unsigned char* p = (unsigned char*)buf;
    size_t done = 0;
    while (done < len) {
        size_t n = len - done;
        if (n > 4096) n = 4096;            /* 内核 read 单次上限（LX64_READ_MAX） */
        const long r = __v64_int80(V64_NR_READ, (long)fd, (long)(uintptr_t)(p + done), (long)n, 0);
        if (r < 0) return v64_errno_own(r);
        if (r == 0) break;
        done += (size_t)r;
        if ((size_t)r < n) break;          /* 短读：按实际读出返回（文件尾/管道空） */
    }
    return (ssize_t)done;
}

int close(int fd) {
    const long r = __v64_int80(V64_NR_CLOSE, (long)fd, 0, 0, 0);
    return (r < 0) ? v64_errno_own(r) : (int)r;
}

/* 只读 open 走自有 ABI（内核 fd64 的只读路径）；带写标志走 Linux 兼容路径
   （自有 ABI 的 open(6) **没有 flags 参数** —— 这是内核的能力边界，不是本运行时的裁剪）。 */
int open(const char* path, int flags, ...) {
    if (!path) { errno = EFAULT; return -1; }
    if ((flags & O_ACCMODE) == O_RDONLY && (flags & (O_CREAT | O_TRUNC | O_EXCL | O_APPEND)) == 0) {
        const long r = __v64_int80(V64_NR_OPEN, (long)(uintptr_t)path, 0, 0, 0);
        return (r < 0) ? v64_errno_own(r) : (int)r;
    }
    const long r = __v64_syscall(V64_LX_OPEN, (long)(uintptr_t)path, (long)flags, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : (int)r;
}

int getpid(void) {
    const long r = __v64_int80(V64_NR_GETPID, 0, 0, 0, 0);
    return (r < 0) ? 0 : (int)r;           /* 内核没有进程上下文时返回 0（真实值，不编） */
}

unsigned long vimtu64_ticks(void) {
    const long r = __v64_int80(V64_NR_TICKS, 0, 0, 0, 0);
    return (r < 0) ? 0ul : (unsigned long)r;
}

int vimtu64_sleep_ms(unsigned ms) {
    const long r = __v64_int80(V64_NR_SLEEP_MS, (long)ms, 0, 0, 0);
    return (r < 0) ? v64_errno_own(r) : 0;
}

unsigned sleep(unsigned seconds) {
    (void)vimtu64_sleep_ms(seconds * 1000u);
    return 0;                              /* POSIX：返回"没睡完的秒数"，本内核睡满了才算返回 -> 0 */
}

int usleep(unsigned long usec) {
    /* 内核睡眠粒度是 ms：向上取整（睡少了的语义比睡多了更容易出隐性缺陷） */
    const unsigned long ms = (usec + 999ul) / 1000ul;
    return vimtu64_sleep_ms((unsigned)ms);
}

void _exit(int code) {
    (void)__v64_int80(V64_NR_EXIT, (long)code, 0, 0, 0);
    for (;;) { }                           /* exit 不该返回；真返回就停住（不落到未映射地址） */
}

/* ---------------- Linux 兼容路径（自有 ABI 没有的号） ---------------- */
off_t lseek(int fd, off_t off, int whence) {
    const long r = __v64_syscall(V64_LX_LSEEK, (long)fd, (long)off, (long)whence, 0, 0);
    return (r < 0) ? (off_t)v64_errno_lx(r) : (off_t)r;
}

int stat(const char* path, struct stat* st) {
    if (!path || !st) { errno = EFAULT; return -1; }
    const long r = __v64_syscall(V64_LX_STAT, (long)(uintptr_t)path, (long)(uintptr_t)st, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}

int fstat(int fd, struct stat* st) {
    if (!st) { errno = EFAULT; return -1; }
    const long r = __v64_syscall(V64_LX_FSTAT, (long)fd, (long)(uintptr_t)st, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}

char* getcwd(char* buf, size_t size) {
    if (!buf || size == 0) { errno = EFAULT; return NULL; }
    const long r = __v64_syscall(V64_LX_GETCWD, (long)(uintptr_t)buf, (long)size, 0, 0, 0);
    return (r < 0) ? NULL : buf;
}

int unlink(const char* path) {
    if (!path) { errno = EFAULT; return -1; }
    const long r = __v64_syscall(V64_LX_UNLINK, (long)(uintptr_t)path, 0, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}

/* chdir：内核的自有 ABI 没有这个号，Linux 号段里的 80 也没实现（syscall64.cpp 的表里没有）——
   **如实**返回 -1 + ENOSYS，绝不假装换成功（"换目录后相对路径"在本内核里不存在）。 */
int chdir(const char* path) {
    (void)path;
    errno = ENOSYS;
    return -1;
}

/* ---------------- ★ A5 前置：输入事件投递 + 共享内存缓冲（自有 ABI 12/13/14） ----------------
   errno 口径：内核这三号返回的是**有语义的小负数**（-1..-5，见 vimtu64.h 的 V64_EV_ 与 V64_SHM_ 两组宏），
   与 fb_map 同一套分工 —— 原样返回给调用方（它比 errno 更具体），同时把 errno 映射成 POSIX 值，
   这样\"只用 errno 的代码\"也能拿到一个合理的分类。**绝不把负值改成 0**。 */
static int v64_errno_ev64(long r) {
    switch (r) {
    case V64_EV_EPERM:  errno = EPERM;  break;
    case V64_EV_EFAULT: errno = EFAULT; break;
    case V64_EV_ENODEV: errno = ENODEV; break;
    case V64_EV_ENOMEM: errno = ENOMEM; break;
    case V64_EV_EINVAL: errno = EINVAL; break;
    default:            errno = EIO;    break;
    }
    return (int)r;
}

int poll_event(struct Ev64Event* out, unsigned max, unsigned flags) {
    /* max == 0：只查询待取条数（内核不看 out 指针） */
    const long r = __v64_int80(V64_NR_INPUT_POLL, (long)(uintptr_t)out, (long)max, (long)flags, 0);
    if (r < 0) return v64_errno_ev64(r);
    return (int)r;
}

int shm_create(unsigned size) {
    const long r = __v64_int80(V64_NR_SHM_CREATE, (long)size, 0, 0, 0);
    if (r < 0) return v64_errno_ev64(r);
    return (int)r;                       /* 对象 id（>= 1），跨进程可传递 */
}

int shm_map(int id, unsigned offset, unsigned len, void** out_va) {
    if (!out_va) { errno = EFAULT; return -1; }
    const long r = __v64_int80(V64_NR_SHM_MAP, (long)id, (long)offset, (long)len,
                               (long)(uintptr_t)out_va);
    if (r < 0) return v64_errno_ev64(r);
    return (int)r;
}

/* ---------------- 时间 ---------------- */
time_t time(time_t* tloc) {
    const time_t t = (time_t)(vimtu64_ticks() / VIMTU64_TICKS_HZ);
    if (tloc) *tloc = t;
    return t;
}

/* ==================== ★ A4-5：信号（rt_sigaction 13 / rt_sigprocmask 14 / kill 62 / setpgid 109）====================
 * 口径：内核返回 -errno（syscall 指令路径），这里原样搬进 errno 并返回 -1。
 * sigaction() 包装做两件调用方不该操心的事：
 *   1) 自动把 act->restorer 填成 __v64_sigreturn_stub（内核拿它当 handler 的返回地址）；
 *   2) 若调用方给的是 NULL flags，保留 0（不偷偷加标志）。
 * 为什么 errno 口径是 -errno：这三个号走 **Linux 号段**（syscall 指令），与 4/5/6/8 等一致。 */
int sigaction(int sig, const struct sigaction* act, struct sigaction* oldact) {
    struct sigaction a;
    const struct sigaction* ap = act;
    if (act) {
        a = *act;                                   /* 结构体 32 字节：按值拷一份，改副本 */
        if (!a.restorer) a.restorer = __v64_sigreturn_stub;
        ap = &a;
    }
    const long r = __v64_syscall(13, (long)sig, (long)(uintptr_t)ap, (long)(uintptr_t)oldact, 8, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}
void (*signal(int sig, void (*handler)(int)))(int) {
    struct sigaction a;
    a.handler = handler; a.flags = 0; a.restorer = __v64_sigreturn_stub; a.mask = 0;
    struct sigaction old;
    if (sigaction(sig, &a, &old) != 0) return SIG_ERR;
    return old.handler;
}
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset) {
    const long r = __v64_syscall(14, (long)how, (long)(uintptr_t)set, (long)(uintptr_t)oldset, 8, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}
int kill(int pid, int sig) {
    const long r = __v64_syscall(62, (long)pid, (long)sig, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}
int raise(int sig) {
    /* 给**自己**发：投递点是本系统调用的出口（内核 syscall 出口钩子），返回时 handler 已经在跑了 */
    return kill(getpid(), sig);
}
int setpgid(int pid, int pgid) {
    const long r = __v64_syscall(109, (long)pid, (long)pgid, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}
void (*vimtu64_sigreturn_addr(void))(void) { return __v64_sigreturn_stub; }

/* ---- 进程（fork/wait4）：ring3 shell 的 run 用它们；信号演示也靠它们验证"父进程 wait4 看到 128+sig" ---- */
int fork(void) {
    const long r = __v64_syscall(57, 0, 0, 0, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : (int)r;
}
/* wait4 的 status 是 Linux 编码：正常退出 = (code & 0xFF) << 8；被信号打死 = 低 7 位放信号号
 * （本内核如实实现的是"退出码 = 128+sig"，所以 WEXITSTATUS(status) 就是 128+sig —— 见
 *  kernel/proc64.cpp 的 wait4 注释）。 */
int wait4(int pid, int* status, int options, void* rusage) {
    const long r = __v64_syscall(61, (long)pid, (long)(uintptr_t)status, (long)options,
                                (long)(uintptr_t)rusage, 0);
    return (r < 0) ? v64_errno_lx(r) : (int)r;
}
/* ---- ★ A4-5：mmap（9）—— 编辑器用它拿大块文本缓冲（1 MiB 文件不能用 4KB 的堆）---- */
void* mmap(void* addr, size_t len, int prot, int flags, int fd, long off) {
    (void)off;      /* 本内核的 mmap 是"每进程 bump 分配器"：没有文件后端，fd/off 忽略（如实） */
    /* Linux x86_64 的 mmap：rax=9, rdi=addr, rsi=len, rdx=prot, r10=flags, r8=fd, r9=off
     * 内核只认 len/prot/flags（arch 无关分派：lx64_mmap_disp64(a2,a4,a1)），fd/off 忽略并如实注明。 */
    const long r = __v64_syscall(9, (long)(uintptr_t)addr, (long)len, (long)prot, (long)flags, (long)fd);
    return (r < 0) ? (void*)0 : (void*)(uintptr_t)r;
}
/* ---- 终端 ioctl（16）：编辑器进入/退出 raw 模式用 TCGETS/TCSETS ---- */
int ioctl(int fd, unsigned long req, void* arg) {
    const long r = __v64_syscall(16, (long)fd, (long)req, (long)(uintptr_t)arg, 0, 0);
    return (r < 0) ? v64_errno_lx(r) : 0;
}
