/* sys/types.h - 基础类型别名（Vimtu64 用户态 C 运行时） */
#ifndef VIMTU64_SYS_TYPES_H
#define VIMTU64_SYS_TYPES_H

#include <stddef.h>

typedef long          ssize_t;
typedef long          off_t;
typedef int           pid_t;
typedef unsigned int  mode_t;
typedef unsigned int  uid_t;
typedef unsigned int  gid_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;
typedef long          time_t;
typedef long          blkcnt_t;
typedef long          blksize_t;
typedef long          suseconds_t;

#endif /* VIMTU64_SYS_TYPES_H */
