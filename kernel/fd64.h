// fd64.h - Vimtu64 的 FD 层：**每进程** fd 表 + 引用计数的打开文件对象 + pipe
//
// 设计（批次 D：POSIX 语义补完；为什么长这样）：
//   * **表按进程**：每张 FdTable64 有 32 个槽（fd = 3 + 槽位；0/1/2 是标准流，由
//     syscall64/终端自己认）。进程结构（kernel/proc64.h 的 Proc64）持有一个
//     FdTable64*（fd64_table_alloc64 从池里取）；任务经 task_proc_of_current64()
//     找到自己的进程再拿到表；**没有进程上下文**时（任务 0 = 桌面/终端、安装介质内核、
//     启动早期）退回**内核表** fd64_kernel_table64()——“终端”就是这张表的拥有者。
//   * **打开文件对象 + 引用计数**：fd 槽指向一个 OpenFile64（路径、flags、**偏移游标**、
//     引用计数、目录缓存、pipe 端）。语义严格按 Linux：
//       - dup/dup2：两张槽指向**同一个** OpenFile64（共享游标）；
//       - fork：整张表**逐槽共享**（引用计数 +1），父子共享偏移；
//       - execve：**默认保留**所有 fd（本内核没有实现 O_CLOEXEC，如实注明）；
//       - close：引用计数 -1，归零才真正释放对象。
//   * **O_APPEND(0x400)** 真实现：每次 write 都先定位到文件末尾（写后游标 = 末尾）。
//   * **pipe(22)** 真实现：64 字节环形缓冲 + 两个 fd（读端/写端）。**没有阻塞语义**：
//     写满返回短写（一点空间都没有 -> -EAGAIN）；读空且写端还开着 -> -EAGAIN，
//     写端全关 -> 0（EOF）。fork 后父子各持一端即可通信。
//   * 底层只有 vfs64（VimtuFS2）：路径形如 "/dir/sub/name"（也接受 "name" = 从根开始），
//     大小写敏感、**支持多级路径**（v3 起；'.'/'..' 交给 vfs64 解析）、单文件 <= 67584 B
//     （FD64_FILE_MAX = VFS64_MAX_FILE_BYTES），没有权限、不能删非空目录。
//   * 读：vfs64 没有 read-at-offset 原语，所以每次读把整个文件读进**一块全局读缓冲**
//     （FD64_FILE_MAX），再按游标拷贝请求的片段；读/写期间关中断。
//   * 写：vfs64_write 是"整体覆盖"语义 —— 本层每次写做一次**整文件 read-modify-write**，
//     写完立刻整体落盘（不缓存脏页，也不留写暂存跨调用）。好处：多个 fd/多个进程同时写
//     同一个文件时语义仍然正确（O_APPEND 的真值就靠它）；代价：每次写都读写整个文件，
//     单文件 <= 67584 B，演示规模够用。
//   * 目录句柄：fd64_opendir64 + fd64_readdir64（线性枚举，第一次重列目录后缓存 16 条）。
//
// 边界（如实写在文档里，别把没做的说成做了）：
//   * 没有文件权限/属主；没有 O_CLOEXEC、fcntl(72)/F_DUPFD、非阻塞标志的完整语义；
//   * 没有 select/poll/epoll、没有文件的 mmap、没有硬链接/符号链接；
//   * pipe 没有阻塞/信号语义（见上），容量固定 64 B，最多 8 条同时在用；
//   * vfs64 没有 read-at-offset，读写都要把整个文件过一遍内存。
//
// 打点（自动验收 grep，格式勿改）：
//   [FD64] open path=<p> fd=<n> flags=<n>
//   [FD64] read fd=<n> n=<n>
//   [FD64] write fd=<n> n=<n> total=<n>
//   [FD64] close fd=<n> refs=<n>
//   [FD64] pipe read n=<n> data=<s>
//   [FD64] selftest PASS / [FD64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>

#define FD64_MAX        32u          // 每张 fd 表的槽位数（fd = 3 + 槽位）
#define FD64_TABLE_MAX  20u          // fd 表池（16 进程 + 内核表 + 余量）
#define FD64_OPEN_MAX   64u          // OpenFile64 对象池（引用计数，不按进程复制）
#define FD64_PIPE_MAX   8u           // pipe 对象池（每条 64 B 环形缓冲）
#define FD64_PIPE_BYTES 64u          // ★ pipe 容量（固定；没有阻塞语义，见文件头）
#define FD64_PATH_MAX   64u          // 路径缓冲：支持多级路径（"/apps/demo/file.txt" 这种）
#define FD64_NAME_MAX   32u          // 目录项名字缓冲（与 vfs64_ls 的 [][32] 对齐；v3 名字上限 31）
#define FD64_FILE_MAX   67584u       // 单文件上限（= VFS64_MAX_FILE_BYTES）

// open 标志（Linux 的一个子集；不认识的高位一律忽略）
#define FD64_O_RDONLY    0x0000u
#define FD64_O_WRONLY    0x0001u
#define FD64_O_RDWR      0x0002u
#define FD64_O_CREAT     0x0040u
#define FD64_O_TRUNC     0x0200u
#define FD64_O_APPEND    0x0400u     // ★ 批次 D：真实现（每次写定位到末尾）
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
#define FD64_EAGAIN  11

// 路径规范化：接受 "/dir/sub/name" 与 "dir/sub/name"（相对路径 = 从根开始）；折叠连续的 '/'、
// 去掉结尾 '/'、去前导空白；".." **原样保留**（父目录语义由 vfs64 负责）；拒绝空串、'/'、控制字符、超长。
// 成功返回 0 并把规范形式（含 '/' 前缀）写进 out；失败返回负错误码。
int fd64_norm_path64(const char* in, char* out, int cap);

// ==================== fd 表（每进程一张；有内核表兜底）====================
// 不透明类型：外部（proc64）只持有指针，槽位布局是 fd64.cpp 的实现细节。
struct FdTable64;

FdTable64* fd64_kernel_table64();                    // 内核/桌面（终端）用的那张表
FdTable64* fd64_current_table64();                   // 有进程上下文 = 进程表；否则内核表
FdTable64* fd64_table_alloc64();                     // 池满返回 nullptr
void       fd64_table_free64(FdTable64* t);          // 必须已 close_all
int        fd64_table_clone64(FdTable64* dst, const FdTable64* src);   // fork：逐槽引用 +1
void       fd64_table_close_all64(FdTable64* t);     // exit/destroy：逐槽 close（引用计数 -1）
uint32_t   fd64_table_used64(const FdTable64* t);    // 已占用槽位数
uint64_t   fd64_object_id64(int fd);                 // 该 fd 指向的 OpenFile64 标识（0 = 无效）

// 文件句柄（fd >= 3）。错误一律返回负 errno。
int fd64_open64(const char* path, uint32_t flags);
int fd64_close64(int fd);
int fd64_read64(int fd, void* buf, int len);
int fd64_write64(int fd, const void* buf, int len);
int fd64_lseek64(int fd, int64_t off, int whence);
int fd64_stat64(int fd, uint32_t* type_out, uint32_t* size_out);
int fd64_fsync64(int fd);
// dup：newfd >= 3 时占用该槽；newfd < 0 时自动分配一个新槽。返回新 fd 或负错误码。
// ★ Linux 语义：新旧 fd 指向**同一个 OpenFile64**（共享偏移/目录游标），引用计数 +1。
int fd64_dup64(int oldfd, int newfd);

// ==================== pipe（64 B 环形缓冲；无阻塞语义）====================
// 成功返回 0 并填入读端/写端两个 fd；失败返回负错误码（-EMFILE 池/fd 槽满）。
int fd64_pipe64(int* fd_r, int* fd_w);

// 目录句柄
int fd64_opendir64(const char* path);
// 返回 1 = 下一条（name_out 已 NUL 结尾）、0 = 枚举结束、<0 = -errno。type_out/size_out 可空。
int fd64_readdir64(int fd, char* name_out, int name_cap, uint32_t* type_out, uint32_t* size_out);

// FD 语义演示（终端 `fdtest` 命令）：独立游标 / dup 共享游标 / O_APPEND / pipe 环回。
// 打点前缀 [FD64] demo ...；返回 0 = 全过，非 0 = 失败位掩码。不碰任何已有文件：
// 只用 /fdtest.a、/fdtest.b（跑完删掉）。
int fd64_demo64();

// 自检（路径规范化 + fd 表分配/释放 + dup 共享对象 + 已挂载时的目录句柄）。0 = 全过。
// 内部打印 [FD64] selftest PASS / FAIL mask=<n> 与实测行 [FD64] selftest table=... vfs=...
int fd64_selftest64();

// 串口打点：当前 fd 表占用 + 每槽 path/off/size + 对象引用计数（验收/诊断用）。
void fd64_dump64();
