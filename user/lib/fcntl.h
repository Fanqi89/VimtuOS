/* fcntl.h - open 的标志位（Vimtu64 用户态 C 运行时）
 *
 * 取值与内核 fd64/fs64 用的 Linux 子集一致（kernel/fd64.h）。
 * ★ 能力边界：自有 ABI 的 open(6) **只读**；带写标志时 open() 走内核的 Linux 兼容路径
 *   （syscall 指令 nr=2），内核不支持就如实返回 -1 + errno。 */
#ifndef VIMTU64_FCNTL_H
#define VIMTU64_FCNTL_H

#include <sys/types.h>

#define O_RDONLY   00000000
#define O_WRONLY   00000001
#define O_RDWR     00000002
#define O_ACCMODE  00000003
#define O_CREAT    00000100
#define O_EXCL     00000200
#define O_TRUNC    00001000
#define O_APPEND   00002000
#define O_NONBLOCK 00004000

int open(const char* path, int flags, ...);

#endif /* VIMTU64_FCNTL_H */
