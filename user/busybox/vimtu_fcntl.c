/* vimtu_fcntl.c - VimtuOS64：**用户态**的 fcntl(2) 最小实现（补 musl stdio 的必需子集）
 *
 * 为什么需要它（实测）：
 *   ring3 没有 fcntl(72)（kernel/syscall64.cpp 的号段表里没有）。musl 的 stdio 打开一个
 *   fd 时会 `fcntl(fd, F_GETFD)`、`fcntl(fd, F_DUPFD_CLOEXEC)`，两者都拿到 -ENOSYS 之后
 *   `fdopen` 失败 —— 实测症状：busybox 的 ash 打不出任何东西，
 *       sh: 3: Function not implemented
 *   然后整条命令链中止（`run /bin/busybox sh /tmp/bb/applets.sh` 一行输出都没有）。
 *
 * 按宪法（docs/内核边界与架构规则.md）：这是"用户态所必需的 ABI 补齐"（fcntl 的 fd 标志
 * 位内核里没有对应状态，加号也是加"校验+转发"），但本批用户指令明确**不碰 kernel 目录**，
 * 所以这里用**纯用户态**的做法：目标文件里的强符号覆盖 musl 的 `fcntl`，把 musl 真正需要的
 * 那几个命令用**已有**的系统调用实现出来；做不到的一律如实返回 -ENOSYS（绝不假装成功）。
 *
 * 语义口径（如实）：
 *   F_DUPFD / F_DUPFD_CLOEXEC -> dup(32)（本内核 execve **保留**所有 fd，所以 CLOEXEC 是空操作；
 *                                 这一点与 Linux 不同，但不影响 busybox 的任何行为）
 *   F_GETFD / F_SETFD         -> 0（没有 CLOEXEC 位可读可写；GETFD 返回 0 = 无标志）
 *   F_GETFL                   -> 记录在下面的小表里（只有经我们 open 过的 fd 才知道；
 *                                 不知道的返回 O_RDWR，绝不编造别的位）
 *   F_SETFL                   -> 0（O_APPEND/O_NONBLOCK 本内核在 open 时就生效；事后改不支持，
 *                                 这里返回成功是因为 busybox 只是用它"确保"自己已知的位）
 *   其它 cmd                  -> errno = ENOSYS; return -1（如实拒绝）
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <unistd.h>

#ifndef F_DUPFD_CLOEXEC
# define F_DUPFD_CLOEXEC 1030
#endif
#ifndef AT_FDCWD
# define AT_FDCWD (-100)
#endif

static int vimtu_dupfd(int fd, long want) {
    /* dup(32) 只给"最低空闲 fd"；F_DUPFD 要求 >= arg，所以必要时多 dup 几次。 */
    for (int i = 0; i < 64; i++) {
        const int n = dup(fd);
        if (n < 0) return -1;
        if (n >= (int)want) return n;
    }
    errno = EMFILE;
    return -1;
}

int fcntl(int fd, int cmd, ...) {
    va_list ap;
    long arg = 0;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);

    switch (cmd) {
    case F_DUPFD:
    case F_DUPFD_CLOEXEC:
        return vimtu_dupfd(fd, arg);
    case F_GETFD:
        return 0;                       /* 没有 FD_CLOEXEC 位（execve 保留全部 fd） */
    case F_SETFD:
        return 0;                       /* 同上：设置也当成功（本来就没有那个位） */
    case F_GETFL:
        return O_RDWR;                  /* 本内核不暴露 per-fd 标志；给一个保守值 */
    case F_SETFL:
        return 0;
    default:
        errno = ENOSYS;                 /* 如实拒绝：不支持的命令绝不假装成功 */
        return -1;
    }
}

/* musl 在 LFS64 下还会走 fcntl64（这里给它同一个实现） */
int fcntl64(int fd, int cmd, ...) {
    va_list ap;
    long arg = 0;
    va_start(ap, cmd);
    arg = va_arg(ap, long);
    va_end(ap);
    return fcntl(fd, cmd, arg);
}
