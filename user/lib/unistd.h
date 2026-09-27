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

#endif /* VIMTU64_UNISTD_H */
