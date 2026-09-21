// fs64.h - 统一文件系统分派层（VimtuFS2 读写 / FAT32 只读）
//
// 为什么要有这一层：
//   explorer/terminal/fd64 需要的是"列目录 / 查属性 / 读文件 / 写文件 / 卷信息"这一小组操作，
//   而底层有两套实现（kernel/vfs64.cpp 的 VimtuFS2、kernel/fat64.cpp 的 FAT32 只读）。
//   如果让每个上层调用点自己 if (fat) ... else ...，同一段逻辑会散落在文件管理器、终端、
//   FD 层、状态栏里 —— 每加一种文件系统就要再抄一遍。所以本层把"按卷类型分派"收在一处：
//     上层只拿一个**统一卷号**调 fs64_*，VimtuFS2 转 vfs64_*_on64、FAT32 转 fat64_*。
//
// ==================== 统一卷号（vol）====================
//   vol 0 .. VFS64_SLOT_MAX-1        = VimtuFS2 的卷槽（与 vfs64 槽号**一一对应**）
//   vol VFS64_SLOT_MAX .. +FAT64_VOL_MAX-1 = FAT 卷（= FS64_VOL_FAT_BASE + fat64 卷号）
//   任何 fs64_* 的 vol 传 -1 表示"当前卷"（= fs64_current_vol64()）。
//   卷表（g_vols）由 drive64 扫描时登记：VimtuFS2 用 fs64_init64()/槽挂载后自动登记，
//   FAT 用 fs64_mount_fat64()（幂等：同 (disk,lba) 复用同一个 FAT 卷号）。
//
// ==================== FAT 卷一律只读 ====================
//   写操作（write/create/mkdir/unlink/rmdir/rename）在 FAT 卷上**一律**返回 -FS64_EROFS，
//   并打一行 [FS64] reject ...（绝不假装成功，也不写盘）。上层用 fs64_is_readonly64()
//   先把按钮/菜单置灰，把错误路径只当兜底。
//
// 打点（行锁）：[FS64] vol=<n> kind=VimtuFS2|FAT32 ro=<0|1> disk=<n> lba=<n> total_kb=<n> free_kb=<n>
//              [FS64] mount fat vol=<n> disk=<n> lba=<n> ... / [FS64] reject ...
#pragma once
#include <stdint.h>
#include "vfs64.h"     // VFS64_SLOT_MAX / VFS64_TYPE_* / VFS64_KIND_* / VFS64_* 限制
#include "fat64.h"     // Fat64Info64 / Fat64Entry64 / FAT64_VOL_MAX

#define FS64_VOL_FAT_BASE ((int)VFS64_SLOT_MAX)                  // 4：统一卷号里 FAT 卷的起点
#define FS64_VOL_MAX      (FS64_VOL_FAT_BASE + FAT64_VOL_MAX)    // 8
#define FS64_VOL_NONE     (-1)
#define FS64_NAME_MAX     FAT64_NAME_MAX                         // 名字缓冲（UTF-8；FAT 长名也够）
#define FS64_EROFS        30                                     // -errno：只读文件系统

// 卷类型（Fs64Vol64.kind）
#define FS64_KIND_NONE     0u
#define FS64_KIND_VIMTUFS2 1u
#define FS64_KIND_FAT32    2u

// 统一的 stat / 目录项（字段与 vfs64 的对应结构对齐，外加 FAT 的属性位）
struct Fs64Stat64 {
    uint32_t type;      // VFS64_TYPE_FILE / VFS64_TYPE_DIR
    uint32_t size;      // 字节数（目录 = 0）
    uint32_t mtime;     // 打包时间（vfs64 口径；FAT 的 date/time 也转成同一编码）
    uint32_t kind;      // VFS64_KIND_*（FAT 按名字后缀保守判定）
    uint32_t attr;      // FAT 属性位（VimtuFS2 = 0）
};
struct Fs64Dirent64 {
    char     name[FS64_NAME_MAX];
    uint32_t index;     // VimtuFS2 inode 号；FAT = 0
    uint32_t type;
    uint32_t size;
    uint32_t mtime;
    uint32_t kind;
    uint32_t attr;
};
// 统一卷信息（只读）
struct Fs64Vol64 {
    uint8_t  kind;            // FS64_KIND_*
    uint8_t  readonly;        // 1 = 只读（FAT32）
    uint8_t  total_known;
    uint8_t  free_known;
    int      disk;
    uint32_t start_lba;
    int      vfs_slot;        // kind = VIMTUFS2 时的 vfs64 槽号（否则 -1）
    int      fat_slot;        // kind = FAT32 时的 fat64 卷号（否则 -1）
    uint64_t total_kb;
    uint64_t free_kb;
    char     fs[16];          // "VimtuFS2" / "FAT32"
};

// 幂等初始化：把 vfs64 已挂载的槽登记进统一卷表，并把 g_cur 对齐"当前卷"。
int fs64_init64();
// 卷表里的卷数（不含空槽）。
int fs64_vol_count64();
// 统一卷号是否有效（已登记）。
int fs64_vol_used64(int vol);
// 统一卷信息（只读；返回 0 / -1）。
int fs64_vol_info64(int vol, Fs64Vol64* out);
// vfs64 槽 -> 统一卷号（0..3，就是槽号本身；该槽没挂载 = -1）。
int fs64_find_vfs64(int slot);
// (disk,lba) 的 FAT 卷是否已挂载 -> 统一卷号；-1 = 没有。
int fs64_find_fat64(int disk, uint32_t lba);
// 探测 + 挂载 + 登记一个 FAT32 卷（幂等）。成功返回统一卷号（>= FS64_VOL_FAT_BASE），失败 -1。
int fs64_mount_fat64(int disk, uint32_t lba, Fs64Vol64* out);

// 激活某个统一卷（VimtuFS2 会顺带切 vfs64 当前槽）。0 = 成功 / -1 = 无效。
int fs64_activate64(int vol);
// 当前统一卷号（-1 = 没有任何可用的卷）。会与 vfs64 的当前槽保持一致。
int fs64_current_vol64();
// vol 是否只读（FAT32 = 1）；无效卷也返回 1（宁可不写）。
int fs64_is_readonly64(int vol);
// vol 的类型（FS64_KIND_*）。
uint32_t fs64_vol_kind64(int vol);

// ---- 统一文件操作（vol = -1 时作用在当前卷；语义与 vfs64_* 逐条对齐）----
// 列目录（游标分页）：返回填充条数、0 = 结束、-1 = 错。
int fs64_list64(int vol, const char* path, Fs64Dirent64* out, int max, uint32_t* cursor);
// 兼容版列目录（[][32] 名字 + 大小数组；FAT 长名超过 31B 会截断，与 vfs64_ls 同口径）。
int fs64_ls64(int vol, const char* path, char names[][VFS64_LS_NAME_BUF], int max, uint32_t* sizes);
int fs64_stat64(int vol, const char* path, Fs64Stat64* out);
// 读文件：最多 max 字节，返回实际字节数 / 负错误码。
int fs64_read64(int vol, const char* path, void* buf, int max);
// 分块读（大文件校验用）：*out_got = 实际读到的字节数；0 = 成功。
// 分块读（大文件校验用）：*out_got = 实际读到的字节数；0 = 成功。缓冲区由调用方给（建议 ≤64KB/次）。
int fs64_read_range64(int vol, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);
// ★ 批次 M：分块写（按偏移；保留原有字节；off > 当前大小 = 空洞**补零**；**8 MiB 上限**由 vfs64 把关，
//   超上限/空间不足一律先失败、不写一半）。只读卷（FAT32）返回 -FS64_EROFS。
int fs64_write_at64(int vol, const char* path, uint32_t off, const void* buf, uint32_t len);
// 写操作：FAT 卷上一律返回 -FS64_EROFS（打点 [FS64] reject ... readonly）。
int fs64_write64(int vol, const char* path, const void* buf, int len);
int fs64_create64(int vol, const char* path);
int fs64_mkdir64(int vol, const char* path);
int fs64_unlink64(int vol, const char* path);
int fs64_rmdir64(int vol, const char* path);
int fs64_rename64(int vol, const char* old_path, const char* new_name);
// 空间查询：*free_blocks/*total_blocks 以 512B 块计（与 vfs64_free64 同口径）。
// FAT 的空闲字节由挂载时的 FSInfo 快照折算；FSInfo 无效 -> -1（如实说"未知"）。
int fs64_free64(int vol, uint32_t* free_blocks, uint32_t* free_bytes, uint32_t* total_blocks);

// 只读校验：把文件按块读一遍算 CRC32（IEEE，zlib 同多项式）。成功返回 CRC，*out_size = 文件大小；
// 失败返回 0（CRC 恰好为 0 的概率可忽略；调用方同时看 *out_size）。供终端 `fatcheck` 逐字节核对 ESP 文件。
uint32_t fs64_crc32_file64(int vol, const char* path, uint32_t* out_size, uint32_t max_bytes);

// 自检（0 = 全过；打点 [FS64] selftest PASS|FAIL mask=<n>）：
//   bit0 卷表自洽（kind/fs/readonly/容量字段；有当前卷时 stat("/") 必须成功）
//   bit1 FAT 卷只读语义（write/create/mkdir/unlink/rmdir/rename 全部 -FS64_EROFS，没有写盘副作用）
//   bit2 无效 vol / 越界 path 被如实拒绝（不崩）
int fs64_selftest64();
