/* unistd.h - POSIX 基础调用（Vimtu64 用户态 C 运行时）
 *
 * 实现分工（每条都写清了走哪条路径，因为两条路径的错误码口径不同）：
 *   write/read/open/close/exit/_exit/getpid/dup?  -> 自有 ABI（int 0x80）
 *   lseek/stat/getcwd/unlink                      -> Linux 兼容路径（syscall 指令）：
 *                                                    自有 ABI **没有**这些号，内核只在
 *                                                    Linux 号段实现了它们（返回 -errno）
 *   chdir                                         -> 内核两条路径都没有 -> -1 + errno=ENOSYS
 *   做不到的一律返回 -1 并置 errno（不假装成功，见 errno.h 的说明）。 */
#ifndef VIMTU64_UNISTD_H
#define VIMTU64_UNISTD_H

#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

ssize_t read(int fd, void* buf, size_t len);
ssize_t write(int fd, const void* buf, size_t len);
int     close(int fd);
int     unlink(const char* path);
int     chdir(const char* path);
char*   getcwd(char* buf, size_t size);
off_t   lseek(int fd, off_t off, int whence);

int     getpid(void);
unsigned sleep(unsigned seconds);          /* 秒 -> 内核 ms 睡眠（向上取整到 ms） */
int     usleep(unsigned long usec);        /* 微秒 -> 内核 ms 睡眠（向上取整到 ms） */
void    _exit(int code) __attribute__((noreturn));

/* 内核时钟（自有 ABI 4）：PIT tick 数，250Hz —— 供 time.h 用 */
unsigned long vimtu64_ticks(void);
int           vimtu64_sleep_ms(unsigned ms);   /* 自有 ABI 5 */

/* ---- ★ A4-5：fork/wait4（信号演示与 ring3 shell 的 run 用；Linux 号段 57/61，返回 -errno）---- */
int     fork(void);
/* wait4 的 status 是 Linux 编码：WEXITSTATUS(status) = (status>>8)&0xFF —— 被信号打死的进程
 * 退出码是 128+sig（本内核的如实实现，见 kernel/proc64.cpp 的 wait4 注释）。 */
int     wait4(int pid, int* status, int options, void* rusage);
/* ---- 内存映射（Linux 号段 9）：编辑器用它拿大块文本缓冲（user/lib 的堆只有 4 KiB）。
 *      prot: 1=READ 2=WRITE；flags: 0x22 = MAP_PRIVATE|MAP_ANONYMOUS（本内核只认 len/prot/flags）---- */
void*   mmap(void* addr, size_t len, int prot, int flags, int fd, long off);
/* ---- 终端 ioctl（Linux 号段 16）：TCGETS/TCSETS（raw 模式）、TIOCGWINSZ ---- */
int     ioctl(int fd, unsigned long req, void* arg);
#endif /* VIMTU64_UNISTD_H */
