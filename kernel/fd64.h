// fd64.h - Vimtu64 的 FD 层：**每进程** fd 表 + 引用计数的打开文件对象 + pipe
//
// ★ A4-2a：**标准流槽可替换**（dup2 到 0/1/2 真生效）+ 最小"控制台 tty"对象 + readdir 类型判定修正。
//   * **表按进程**：每张 FdTable64 有 32 个槽，**槽号就是 fd 号**（0..31）；槽 0/1/2 是**标准流槽**，
//     默认**空** —— 空 = 老语义（0 = 立刻 EOF、1 = 串口+屏幕、2 = 只串口，由 syscall64 认）；
//     一旦被 dup2 绑上真实对象（文件 / pipe / tty），该 fd 的 read/write/close/ioctl 就走本层对象，
//     与 fd >= 3 完全同一条路径。分配新 fd 时从槽 3 起（0/1/2 不会被自动占用）。
//     FdTable64*（fd64_table_alloc64 从池里取）；任务经 task_proc_of_current64()
//     找到自己的进程再拿到表；**没有进程上下文**时（任务 0 = 桌面/终端、安装介质内核、
//     启动早期）退回**内核表** fd64_kernel_table64()——“终端”就是这张表的拥有者。
//   * **打开文件对象 + 引用计数**：fd 槽指向一个 OpenFile64（路径、flags、**偏移游标**、
//     引用计数、目录缓存、pipe 端）。语义严格按 Linux：
//       - dup/dup2：两张槽指向**同一个** OpenFile64（共享游标）；dup2 的 newfd 可以是 0/1/2；
//       - **tty 对象**（FD64_TTY_PATH = "/dev/console"、"/dev/tty"）：write 直连控制台
//         （串口 + 屏幕），read 立刻返回 0（本内核没有"给用户程序的键盘输入流"，如实），
//         ioctl 认 TIOCGWINSZ/TCGETS/TCSETS（见 syscall64）；
//       - fork：整张表**逐槽共享**（引用计数 +1），父子共享偏移；
//       - execve：**默认保留**所有 fd（本内核没有实现 O_CLOEXEC，如实注明）；
//       - close：引用计数 -1，归零才真正释放对象。
//   * **O_APPEND(0x400)** 真实现：每次 write 都先定位到文件末尾（写后游标 = 末尾）。
//   * **pipe(22)** 真实现：64 字节环形缓冲 + 两个 fd（读端/写端）。★ 本批：**默认阻塞**
//     （POSIX 语义）—— 读空且写端还开着 -> 睡等（task_sleep64 -> task_yield64，不忙等，有界
//     FD64_PIPE_WAIT_MS）；写满 -> 睡等到能全写完。`fcntl(72) F_SETFL O_NONBLOCK(0x800)`
//     切回**非阻塞**（读空 -> -EAGAIN、写满 -> 短写/无空间 -> -EAGAIN）。写端全关 -> read 返回 0
//     （EOF）；读端全关后 write -> -EPIPE（**不投递 SIGPIPE**，见下面的边界段）。fork 后父子各持一端即可通信。
//   * 底层只有 vfs64（VimtuFS2）：路径形如 "/dir/sub/name"（也接受 "name" = 从根开始），
//     大小写敏感、**支持多级路径**（v3 起；'.'/'..' 交给 vfs64 解析）、单文件 <= **8 MiB**
//     （FD64_FILE_MAX = VFS64_MAX_FILE_BYTES；v2 旧卷仍是 67584 B，由 vfs64 按卷布局把关），
//     没有权限、不能删非空目录。
//   * ★ 批次 M：读/写都走 vfs64 的**按偏移分块**原语（fs64_read_range64 / fs64_write_at64），
//     **不再有 8MiB 级别的全局读写缓冲**：`read` 直接读进调用方的缓冲；`write` 只改被覆盖的块
//     （保留原内容、缺块按需分配、seek 过末尾的洞补零）。所以"写一个字节"不再需要把整个文件过一遍内存，
//     8 MiB 的文件也能用 64KB 的小缓冲分块读写（推荐每次 ≤ VFS64_READ_CHUNK_BYTES）。
//   * 偏移/长度：对外都是 **64 位**（fd64_lseek64 收 int64_t）。内部按单文件上限（8 MiB < 4GiB）
//     用 32 位保存，**超出上限一律 -EINVAL/-EFBIG，绝不回绕**。
//   * 写：vfs64_write_at64 是"部分写"，写到上限/空间不足时**先失败**（不会写一半）；
//     每次写后游标 = 写完的位置；O_APPEND 每次先定位到末尾（以盘上的当前大小为准）。
//   * 目录句柄：fd64_opendir64 + fd64_readdir64（线性枚举，第一次重列目录后缓存 16 条）。
//
// 边界（如实写在文档里，别把没做的说成做了）：
//   * 没有文件权限/属主；没有 O_CLOEXEC、F_DUPFD/F_GETFD/F_SETFD（fcntl 只做 F_GETFL/F_SETFL）；
//   * 没有 select/poll/epoll、没有文件的 mmap、没有硬链接/符号链接；
//   * pipe 容量固定 64 B，最多 8 条同时在用；等待**有界**（超时 -> 读 -EAGAIN / 写返回已写字节或
//     -EAGAIN，绝不无限挂死）；没有 select/poll、**不投递 SIGPIPE**（读端全关的 write 只回 -EPIPE）；
//   * 单文件上限 8 MiB（v2 卷 67584 B）：单次 read/write 的长度受调用方缓冲限制（推荐 ≤64KB/次）；
//     没有 O_SYNC/mmap/直接 I/O；写失败（空间不足/超上限）是**部分写也没有**（先失败）。
//   * ★ A4-2a 的 tty 对象**没有输入流**：read(tty) 立刻返回 0（本内核没有把键盘输入交给用户程序的
//     通道 —— 终端的行编辑是内核侧对象）。想要输入的程序只能从文件/pipe 读。**不假装有输入**。
//
// 打点（自动验收 grep，格式勿改）：
//   [FD64] open path=<p> fd=<n> flags=<n>
//   [FD64] read fd=<n> n=<n>
//   [FD64] write fd=<n> n=<n> total=<n>
//   [FD64] close fd=<n> refs=<n>
//   [FD64] pipe read n=<n> data=<s>
//   [FD64] dup old=<n> new=<n> refs=<n> path=<p>        （dup/dup2 共享对象；new <= 2 = 换标准流）
//   [FD64] tty open fd=<n> path=/dev/console            （最小控制台 tty 对象）
//   [FD64] pipe wait pid=<p> fd=<n> why=empty|full ticks=<t>   （发生阻塞等待时一行；t = 等待 tick 数）
//   [FD64] fcntl fd=<n> setfl=0x<hex> ok=<0|1>          （F_GETFL/F_SETFL；ok=0 = -EINVAL/无效 fd）
//   [FD64] selftest PASS / [FD64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>
// ★ A4-2a：**槽号 = fd 号**（0..31）。0/1/2 是标准流槽（默认空 = 由 syscall64 认的控制台语义）；
//   自动分配新 fd 时从 FD64_STDIO_MAX(3) 起，所以 0/1/2 只会被显式 dup2 占用。
#define FD64_MAX        32u          // 每张 fd 表的槽位数（= 最大 fd 号 + 1）
#define FD64_STDIO_MAX  3u           // 标准流槽数（0 = stdin / 1 = stdout / 2 = stderr）
#define FD64_TTY_PATH   "/dev/console"   // ★ 最小"控制台 tty"对象的路径（"/dev/tty" 也认）
#define FD64_TABLE_MAX  20u          // fd 表池（16 进程 + 内核表 + 余量）
#define FD64_OPEN_MAX   64u          // OpenFile64 对象池（引用计数，不按进程复制）
#define FD64_PIPE_MAX   8u           // pipe 对象池（每条 64 B 环形缓冲）
#define FD64_PIPE_BYTES 64u          // ★ pipe 容量（固定 64 B；读/写默认**阻塞**，见文件头）
// ★ 单次阻塞等待上限（毫秒；超时 -> 读 -EAGAIN / 写返回已写字节或 -EAGAIN）。走既有 task_sleep64
//   （task_yield64 让出，不忙等）；没有调度器（安装介质内核）时退回非阻塞，如实。
#define FD64_PIPE_WAIT_MS 5000u
#define FD64_PATH_MAX   64u          // 路径缓冲：支持多级路径（"/apps/demo/file.txt" 这种）
#define FD64_NAME_MAX   32u          // 目录项名字缓冲（与 vfs64_ls 的 [][32] 对齐；v3 名字上限 31）
#define FD64_FILE_MAX   VFS64_MAX_FILE_BYTES   // ★ 批次 M：单文件上限 8 MiB（= VFS64_MAX_FILE_BYTES；
                                               //   v2 旧卷由 vfs64 按卷布局把关成 67584 B）
#define FD64_IO_CHUNK   4096u                  // 单次 read/write 的**推荐**分块（缓冲由调用方给；
                                               //   一次更大的长度也支持，只是调用方要装得下）

// open 标志（Linux 的一个子集；不认识的高位一律忽略）
#define FD64_O_RDONLY    0x0000u
#define FD64_O_WRONLY    0x0001u
#define FD64_O_RDWR      0x0002u
#define FD64_O_CREAT     0x0040u
#define FD64_O_TRUNC     0x0200u
#define FD64_O_APPEND    0x0400u     // ★ 批次 D：真实现（每次写定位到末尾）
#define FD64_O_DIRECTORY 0x10000u
#define FD64_O_NONBLOCK  0x0800u     // ★ 本批：真实现（pipe 读空/写满立刻返回，不睡眠）

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
// 负错误码（= -errno；与 syscall64 的 LX64_* 同一口径）
#define FD64_EPERM   1
#define FD64_EAGAIN  11
#define FD64_EPIPE   32   // ★ 本批：读端全关后的 write（**不投递 SIGPIPE**，如实）
#define FD64_EROFS   30   // ★ 只读文件系统（FAT32 卷：打开写模式一律被拒）
#define FD64_EACCES  13   // ★ P4：权限不足（vfs64 的 -EACCES 透传；与 LX64_EACCES 同值）
// ★ 本批：fcntl(72) 的最小命令集（与 Linux 同值）与 F_SETFL 可接受的位。
#define FD64_F_GETFL 3
#define FD64_F_SETFL 4
#define FD64_F_SETFL_MASK64 ((uint32_t)(FD64_O_NONBLOCK | FD64_O_APPEND))
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
// ★ 多卷（批次 K 起统一走 fs64）：vol = **统一卷号**（0..3 = VimtuFS2 卷槽、>=4 = FAT 卷；
// -1 = 当前卷）。之后这个 fd 的读/写/stat/目录枚举都锁定在打开的卷上 —— 用户切盘不会
// 让已打开的 fd 读到另一块盘的同名文件。**只读卷（FAT32）上带写意图的打开直接
// 返回 -FD64_EROFS**（打点 [FD64] open FAILED ... ro=1），不会创建/截断任何文件。
// 默认 fd64_open64 就是 fd64_open_on64(fs64_current_vol64(), ...) 的包装。
int fd64_open_on64(int vol, const char* path, uint32_t flags);
int fd64_open64(const char* path, uint32_t flags);
int fd64_close64(int fd);
int fd64_read64(int fd, void* buf, int len);
int fd64_write64(int fd, const void* buf, int len);
int fd64_lseek64(int fd, int64_t off, int whence);
int fd64_stat64(int fd, uint32_t* type_out, uint32_t* size_out);
int fd64_fsync64(int fd);
// dup：newfd >= 0 时占用该槽（**含 0/1/2 = 换标准流**）；newfd < 0 时自动分配一个新槽（>= 3）。
// 返回新 fd 或负错误码（-EBADF：oldfd 无效 / newfd 越界 0..31）。
// ★ Linux 语义：新旧 fd 指向**同一个 OpenFile64**（共享偏移/目录游标），引用计数 +1。
int fd64_dup64(int oldfd, int newfd);

// ★ P4：fd -> (统一卷号, 规范化路径)。fchmod/fchown 用（内核没有 inode 句柄可直接改，按路径改）。
// 返回 0 = 已填入；-FD64_EBADF = fd 无效；-FD64_EINVAL = 缓冲太小。
int fd64_where64(int fd, int* out_vol, char* path_out, int cap);

// ★ 本批：fcntl(72) 的最小真实现（Linux 号段）：cmd = FD64_F_GETFL(3) -> 当前 flags（>= 0）；
//   cmd = FD64_F_SETFL(4) -> 用 arg 的位改 flags（只认 FD64_F_SETFL_MASK64，其它位 -> -EINVAL）。
// 错误：-FD64_EBADF（fd 无效）；-FD64_EINVAL（未知 cmd / 未知 flag）。状态在 OpenFile64.flags 里
//   —— 与 Linux 一致：dup/fork 共享同一份状态（同一打开文件描述）。
// 打点：[FD64] fcntl fd=<n> setfl=0x<hex> ok=<0|1>
int fd64_fcntl64(int fd, int cmd, uint64_t arg);

// ==================== pipe（64 B 环形缓冲；★ 本批：读/写默认阻塞）====================
// 成功返回 0 并填入读端/写端两个 fd；失败返回负错误码（-EMFILE 池/fd 槽满）。
// 语义见文件头：默认阻塞（有界等待，task_sleep64 让出）、F_SETFL O_NONBLOCK 切非阻塞、
// 写端全关 -> read 0（EOF）、读端全关 -> write -EPIPE（不投递 SIGPIPE）。
int fd64_pipe64(int* fd_r, int* fd_w);
// 从**当前表**把 srcfd 的对象绑到**另一张表** dst 的 newfd 槽（内核启动外部进程时把 fd 交给子进程用：
// 先在本表 open，再 dup 进子进程的表，然后 close 本表那份）。返回 newfd 或负错误码。
int fd64_dup_into64(FdTable64* dst, int srcfd, int newfd);

// ==================== 标准流槽（A4-2a）====================
// 1 = 当前表的槽 fd（0..31）绑着真实对象（则这个 fd 必须走本层，而不是 syscall64 的控制台语义）。
int fd64_slot_used64(int fd);
// 1 = 当前表的槽 fd 绑的是"控制台 tty"对象（ioctl 认 TIOCGWINSZ/TCGETS/TCSETS）。
int fd64_slot_is_tty64(int fd);
// 把 fd 的"是谁"写成一行串口证据（仅当槽已绑对象；用于 dup2 重定向的证据链）。
void fd64_log_slot64(int fd, const char* tag);

// 目录句柄
int fd64_opendir64(const char* path);
// 返回 1 = 下一条（name_out 已 NUL 结尾）、0 = 枚举结束、<0 = -errno。type_out/size_out 可空。
// ★ A4-2a 修正：**大小为 0 的条目**要按**父目录**（本句柄打开的路径）拼路径再 stat，
// 老代码拼的是相对卷根的 "/<name>"，于是子目录里的子目录被误判成普通文件（ls 显示 "sub 0 bytes"）。
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
