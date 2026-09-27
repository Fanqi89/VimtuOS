/* errno.h - 错误码（Vimtu64 用户态 C 运行时）
 *
 * 语义边界（如实标注，别把没做的说成做了）：
 *   * `errno` 是**一个全局 int**（本运行时单线程、无 TLS；内核也不投递信号），定义在 syscall.c；
 *   * 自有 ABI（int 0x80）的错误码很粗（多数失败就是 -1，详见 kernel/syscall64.h）——
 *     包装函数在这种情况把 errno 置成 EIO 并注明"具体原因看内核串口 [SYSCALL] deny 打点"；
 *   * Linux 兼容路径（syscall 指令）返回 -errno，包装函数**原样**搬进 errno（不猜）。 */
#ifndef VIMTU64_ERRNO_H
#define VIMTU64_ERRNO_H

extern int errno;

#define EPERM    1   /* Operation not permitted */
#define ENOENT   2   /* No such file or directory */
#define ESRCH    3
#define EINTR    4
#define EIO      5   /* I/O error（自有 ABI 只有 -1 时用它兜底） */
#define EBADF    9   /* Bad file descriptor */
#define EAGAIN  11
#define ENOMEM  12   /* Out of memory（fb_map 的 -4 / 堆耗尽） */
#define EACCES  13
#define EFAULT  14   /* Bad address（fb_map 的 -2） */
#define EBUSY   16
#define EEXIST  17
#define ENODEV  19   /* No such device（fb_map 的 -3：没有帧缓冲） */
#define ENOTDIR 20
#define EISDIR  21
#define EINVAL  22
#define ENOSPC  28
#define ESPIPE  29
#define EROFS   30
#define EPIPE   32
#define ERANGE  34
#define ENOSYS  38   /* Function not implemented（内核两条路径都没有的号） */
#define ENOTEMPTY 39

#define VIMTU64_ERRNO_OK 0

#endif /* VIMTU64_ERRNO_H */
