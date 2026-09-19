// fd64.h - Vimtu64 的小 FD 层：终端命令与 ring3 系统调用共用的"文件句柄"
//
// 设计（为什么长这样）：
//   * fd 表是**全局**的静态表（FD64_MAX = 32 项），不按进程隔离 —— 本内核的进程模型只到
//     "每进程地址空间"，还没有每进程 fd 表/文件描述符继承。系统调用与终端命令看到的是同一张表；
//     这一条是**边界**，如实写在文档里（没有 CLONE_FILES/execve fd 继承语义）。
//   * fd 0/1/2 是标准流，由 syscall64/终端自己认；本层只管 fd >= 3（fd = 3 + 槽位下标）。
//   * 底层只有 vfs64（VimtuFS2 单层目录）：路径形如 "/name"（也接受 "name"），大小写敏感，
//     单文件 <= 67584 B（FD64_FILE_MAX = VFS64_MAX_FILE_BYTES），没有子目录树、没有权限、
//     不能删目录。这些限制在终端 help / 文档里如实写清。
//   * 读：vfs64 没有 read-at-offset 原语，所以每次读把整个文件读进**一块全局读缓冲**
//     （FD64_FILE_MAX），再按游标拷贝请求的片段；读/写期间关中断（单 CPU 上防止别的任务
//     在两个步骤之间穿插把共享缓冲改掉）。写与读共用同一套口径：
//   * 写：vfs64_write 是"整体覆盖"语义 —— 本层用**一块全局写暂存缓冲**做 read-modify-write，
//     写完立刻整体落盘（不缓存脏页）。同一时刻只支持一个 fd 处于写模式（共用暂存），
//     这是如实标注的边界；本内核的单文件场景（终端 write/echo、ring3 写)不会撞上。
//   * 目录句柄：fd64_opendir64 + fd64_readdir64（线性枚举，每次重列目录后取第 cursor 条）。
//
// 打点（自动验收 grep，格式勿改）：
//   [FD64] open path=<p> fd=<n> flags=<n>
//   [FD64] read fd=<n> n=<n>
//   [FD64] write fd=<n> n=<n>
//   [FD64] close fd=<n>
//   [FD64] selftest PASS / [FD64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

#define FD64_MAX        32u          // fd 表项数（fd = 3 + 槽位）
#define FD64_PATH_MAX   32u          // 路径缓冲：VFS64_NAME_MAX(27) + '/' + 2 兜底
#define FD64_NAME_MAX   32u          // 目录项名字缓冲（与 vfs64_ls 的 [][32] 对齐）
#define FD64_FILE_MAX   67584u       // 单文件上限（= VFS64_MAX_FILE_BYTES）

// open 标志（Linux 的一个子集；不认识的高位一律忽略）
#define FD64_O_RDONLY    0x0000u
#define FD64_O_WRONLY    0x0001u
#define FD64_O_RDWR      0x0002u
#define FD64_O_CREAT     0x0040u
#define FD64_O_TRUNC     0x0200u
#define FD64_O_DIRECTORY 0x10000u

// seek whence（与 Linux 对齐）
#define FD64_SEEK_SET 0
#define FD64_SEEK_CUR 1
#define FD64_SEEK_END 2

// 负错误码（= -errno；与 syscall64 的 LX64_* 同一口径）
#define FD64_EBADF   9
#define FD64_ENOENT  2
#define FD64_EMFILE  24
#define FD64_EFAULT  14
#define FD64_EINVAL  22
#define FD64_EFBIG   27
#define FD64_EISDIR  21
#define FD64_ENOTDIR 20
#define FD64_ESPIPE  29
#define FD64_ENOSPC  28
#define FD64_EPERM   1

// 路径规范化：接受 "/name" 与 "name"（等价）；拒绝空串、'/'-only、多级路径、含空白/控制字符。
// 成功返回 0 并把规范形式（含 '/' 前缀）写进 out；失败返回负错误码。单层路径限制就在这里把关。
int fd64_norm_path64(const char* in, char* out, int cap);

// 文件句柄（fd >= 3）。错误一律返回负 errno。
int fd64_open64(const char* path, uint32_t flags);
int fd64_close64(int fd);
int fd64_read64(int fd, void* buf, int len);
int fd64_write64(int fd, const void* buf, int len);
int fd64_lseek64(int fd, int64_t off, int whence);
int fd64_stat64(int fd, uint32_t* type_out, uint32_t* size_out);
int fd64_fsync64(int fd);
// dup：newfd >= 3 时占用该槽；newfd < 0 时自动分配一个新槽。返回新 fd 或负错误码。
int fd64_dup64(int oldfd, int newfd);

// 目录句柄
int fd64_opendir64(const char* path);
// 返回 1 = 下一条（name_out 已 NUL 结尾）、0 = 枚举结束、<0 = -errno。type_out/size_out 可空。
int fd64_readdir64(int fd, char* name_out, int name_cap, uint32_t* type_out, uint32_t* size_out);

// 自检（路径规范化逻辑 + 已挂载时的"打开目录句柄并读第一条"）。返回 0 = 全过，非 0 = 失败位掩码。
// 内部打印 [FD64] selftest PASS / FAIL mask=<n> 与实测行 [FD64] selftest table=... vfs=...
int fd64_selftest64();

// 串口打点：fd 表占用 + 每槽 path/off/size（验收/诊断用）。
void fd64_dump64();
