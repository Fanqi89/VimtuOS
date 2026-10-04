/* sdk/software-template/include/vimtu/dir64.h - Ring 3 **实时目录枚举**包装（自有 ABI int 0x80 50/51/52）
 *
 * 为什么模板里带这个头：
 *   VimtuOS 的 `user/lib` 里有 stdio/stdlib/fb/wl/font64，但**没有**目录枚举的 C 包装
 *   （内核侧 ABI 是批次 dir64 才补的，见 kernel/syscall64.h 的"目录枚举 ABI"一节）。
 *   第三方程序要列目录，最方便的就是这个薄包装。它**只做转发**，不加任何策略：
 *   不排序、不过滤、不递归 —— 排序/过滤/递归留在你的程序里。
 *
 * ABI 真源：`kernel/syscall64.h`（SYSCALL64_DIR64_*_NR64 = 50/51/52 与 Dir64Ent64 布局）。
 *   int 50 dir_open(path)         -> >=3 目录句柄（就是本进程 fd 表里的一个 fd）/ 负 -errno
 *   int 51 dir_read(fd, buf, cap) -> >0 本次写入字节数（缓冲里 0..N 条记录）/ 0 = 枚举结束 / 负 -errno
 *   int 52 dir_close(fd)          -> 0 / 负 -EBADF
 *
 * 记录布局（紧凑：定长 12B 头 + 变长名字 + 结尾 NUL；记录长度 = 12 + name_len + 1，<= 44B）：
 *   uint16 name_len; uint8 kind; uint8 rsvd; uint32 size; uint32 mtime; char name[];
 *
 * 如实边界（与你必须知道的坑）：
 *   * path 必须是**绝对路径**（这个 ABI 不做 getcwd 拼接；要相对路径就先自己 getcwd 拼好）；
 *   * 句柄是 fd 表里的槽：**上限 8**（fd64 的目录游标表），用完必须 dir_close，否则会 EMFILE；
 *   * 权限由内核 vfs64 判（缺 r -> -EACCES(-13)，会打 [PERM64] deny）；
 *   * 兜底路径（老卷/旧内核）：构建期快照 `/etc/vimtu.dirs`（每行 "<目录>" TAB "<名字>"）——
 *     它是**上一次构建时的快照**，本次开机新建的文件看不到（user/busybox/vimtu_dirent.c 就是这个兜底）。
 */
#ifndef VIMTU_SDK_DIR64_H
#define VIMTU_SDK_DIR64_H

#include <stdint.h>

#define VIMTU_DIR64_OPEN_NR   50
#define VIMTU_DIR64_READ_NR   51
#define VIMTU_DIR64_CLOSE_NR  52

#define VIMTU_DIR_KIND_FILE   1u   /* = VFS64_TYPE_FILE */
#define VIMTU_DIR_KIND_DIR    2u   /* = VFS64_TYPE_DIR  */

#define VIMTU_DIR_ENT_HDR     12u  /* 定长头字节数（name 从 +12 起） */
#define VIMTU_DIR_ENT_MAX     44u  /* 12 + 31 + 1：一条记录的最大长度 */
#define VIMTU_DIR_BUF_MIN     64u  /* dir_read 的 cap 下限（内核要求） */

/* 一条目录项（解析后的视图：名字 + 类型 + 大小 + mtime）。 */
struct VimtuDirent {
    char     name[32];   /* NUL 结尾（内核名字上限 31 字节） */
    uint32_t kind;       /* VIMTU_DIR_KIND_FILE / VIMTU_DIR_KIND_DIR */
    uint32_t size;       /* 文件字节数；目录 = 0 */
    uint32_t mtime;      /* vfs64 打包时间戳；0 = 未知 */
};

struct VimtuDir;

/* 打开一个目录（**绝对路径**）。成功返回句柄，失败返回 NULL（errno 语义见下）。
 * 失败时 *err_out（可传 NULL）写内核返回的**负错误码**（-2 ENOENT / -13 EACCES / -20 ENOTDIR / -24 EMFILE）。 */
struct VimtuDir* vimtu_dir_open(const char* path, int* err_out);

/* 取一条目录项。返回 1 = 有一条；0 = 枚举结束；< 0 = 错误（负错误码）。
 * ★ 内部缓冲是**一批**记录（不是一条）：所以"取完这一批"才会再调一次 dir_read（少打点、快）。 */
int vimtu_dir_read(struct VimtuDir* d, struct VimtuDirent* out);

/* 关闭句柄（释放 fd 槽）。返回 0 / 负错误码。 */
int vimtu_dir_close(struct VimtuDir* d);

#endif /* VIMTU_SDK_DIR64_H */
