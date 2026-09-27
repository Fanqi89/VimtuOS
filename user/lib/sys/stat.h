/* sys/stat.h - 文件状态（Vimtu64 用户态 C 运行时）
 *
 * ★ ABI 对齐：`struct stat` 必须与内核 syscall64.cpp 的 lx64_fill_stat64_ex() 写的
 *   **Linux x86_64 144 字节布局**逐字段一致（内核按固定偏移写 144 字节，用户侧小一点
 *   就会越界写坏栈）。本结构体显式补位到 144 字节。
 *   只声明内核真的会填的字段（其余按 Linux 布局留空），见 kernel/syscall64.cpp。 */
#ifndef VIMTU64_SYS_STAT_H
#define VIMTU64_SYS_STAT_H

#include <sys/types.h>

struct stat {
    dev_t     st_dev;        /* +0 */
    ino_t     st_ino;        /* +8 */
    unsigned long st_nlink;  /* +16 */
    mode_t    st_mode;       /* +24（低 16 位 = 类型 + 权限） */
    uid_t     st_uid;        /* +28 */
    gid_t     st_gid;        /* +32 */
    unsigned int __pad0;     /* +36 */
    dev_t     st_rdev;       /* +40 */
    off_t     st_size;       /* +48 */
    blksize_t st_blksize;    /* +56 */
    blkcnt_t  st_blocks;     /* +64 */
    /* +72..+143：Linux 的 st_atim/st_mtim/st_ctim/__unused 槽位 —— 本内核不填，
       这里保留成同名数组以保证 sizeof(struct stat) == 144。 */
    long      __reserved[9]; /* +72..+143 */
};

#define S_IFMT   0170000
#define S_IFDIR  0040000
#define S_IFREG  0100000
#define S_IFCHR  0020000
#define S_ISDIR(m)  (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m)  (((m) & S_IFMT) == S_IFREG)

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);

#endif /* VIMTU64_SYS_STAT_H */
