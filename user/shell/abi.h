/* abi.h - ★ A4-1：ring3 shell 用到的**内核 ABI 包装**（自有 int 0x80 号段 + Linux 兼容号段）
 *
 * 为什么自己在 user/shell/ 里再包一层（而不是直接用 user/lib 的 open/read/write）：
 *   shell 要用的号里有一半是 user/lib 的 POSIX 名字没覆盖的（fork/execve/wait4/mkdir/rmdir/
 *   getcwd），而**每一条都必须明确走哪条入口**（两条入口的错误码口径不同：自有 ABI 多数只给
 *   -1，Linux 号段给 -errno）。这里逐条写清走哪个入口、返回什么，绝不"猜一个成功"。
 *
 * 号段来源（唯一定义点在内核，别在这里发明新号）：
 *   int 0x80（自有 ABI）：kernel/syscall64.h 的号段（这里只用到 sleep_ms(5)）
 *   syscall 指令（Linux x86_64）：kernel/syscall64.cpp 顶部的映射表 —— 只调用表里
 *   标了"真"的号；表里没有的（chdir(80)、getdents(217)、rename(82)、rmdir(84)）**不用**，
 *   由 shell 自己用等价语义绕开（cwd 由 shell 自己维护，见 main.c 的 cwd 段）。
 */
#ifndef VIMTU64_SH_ABI_H
#define VIMTU64_SH_ABI_H

#include <stdint.h>

/* ---- 自有 ABI（int 0x80）---- */
long sh_intsys(long nr, long a1, long a2, long a3, long a4);   /* __v64_int80 的薄包装 */
int  sh_sleep_ms(unsigned ms);                                 /* nr=5（真睡眠，走 task_sleep64） */

/* ---- Linux 兼容号段（syscall 指令；返回负 = -errno）---- */
long sh_sys(long nr, long a1, long a2, long a3, long a4, long a5);

unsigned long sh_ticks(void);                                  /* nr=4（PIT tick，250 Hz；自有 ABI） */
int  sh_open(const char* path, int flags);                     /* nr=2  */
long sh_read(int fd, void* buf, unsigned long len);            /* nr=0  */
long sh_write(int fd, const void* buf, unsigned long len);     /* nr=1  */
int  sh_close(int fd);                                         /* nr=3  */
int  sh_stat(const char* path, void* st144);                   /* nr=4（Linux 144B struct stat） */
int  sh_fstat(int fd, void* st144);                            /* nr=5  */
long sh_lseek(int fd, long off, int whence);                   /* nr=8  */
int  sh_dup(int oldfd, int newfd);                             /* nr=32/33（只用 >=3 的目标） */
int  sh_mkdir(const char* path, int mode);                     /* nr=83 */
int  sh_unlink(const char* path);                              /* nr=87 */
char* sh_getcwd(char* buf, unsigned long size);                /* nr=79（本内核恒返回 "/"） */
int  sh_fork(void);                                            /* nr=57 */
int  sh_execve(const char* path, char* const* argv, char* const* envp);  /* nr=59 */
int  sh_wait4(int pid, int* status, int options);              /* nr=61 */
void sh_exit_group(int code) __attribute__((noreturn));        /* nr=231 */

/* 原始桩（user/lib/syscall.S）—— 上面每条都建立在它们之上 */
long __v64_int80(long nr, long a1, long a2, long a3, long a4);
long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);

#endif /* VIMTU64_SH_ABI_H */
