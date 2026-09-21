// fat64.h - 最小 FAT32 写入器（安装程序在**目标盘**上建 ESP 用）
//
// 为什么需要它：
//   安装盘（ISO）里的 ESP 是构建期由 tools/make_esp.py 用 Python 手写出来的；
//   而"装到硬盘上的系统"要能被 UEFI 固件启动，硬盘上也必须有一个**真正的
//   FAT32 EFI 系统分区**（ESP）—— 固件只认 FAT。所以把 make_esp.py 的卷格式
//   算法原样搬进内核（**同样的卷参数**），安装完成时写进目标盘。
//
// 卷参数（与 tools/make_esp.py **逐条一致**，否则"内核写的卷"和"构建期写的卷"不一样）：
//     * 512B 扇区、每簇 1 扇区（SPC=1）、1 簇 = 512B
//     * 32 个保留扇区（FAT32 规范要求 >= 32）：0 = 引导扇区、1 = FSInfo、
//       6 = 备份引导扇区（BPB_BkBootSec）、7 = FSInfo 备份
//     * 2 份 FAT（32 位项：FAT[0]=0x0FFFFFF8、FAT[1]=0x0FFFFFFF，链尾 0x0FFFFFFF）
//     * 根目录 = **簇链**，从簇 2 开始（FAT12/16 才是固定根目录区；RootEntCnt=0）
//     * BPB 里的 OEM/卷标/类型串（"FAT32   "）/卷序号与 make_esp.py 一致
//
// ★★ 硬约束（这才是"真 FAT32"，不是把类型字符串改掉就算）：
//     Microsoft FAT 规范按**簇数**判定类型：< 4085 -> FAT12，4085..65524 -> FAT16，
//     >= 65525 -> FAT32。EDK2（OVMF / VMware EFI）的 FAT 驱动**按簇数**决定用
//     12/16/32 位读 FAT 表：簇数不到 65525 却自称 FAT32 的卷会被按 FAT16 解析
//     （32 位 FAT 项被砍成 16 位）-> 簇链变垃圾 -> 读文件报 EFI_VOLUME_CORRUPTED。
//     这跟当初"名为 FAT16、簇数却落在 FAT12 区间"的坑是**镜像关系**，所以这里同样
//     加硬断言：cluster_count >= 65525（FAT64_CLUSTER_MIN），format_volume 越界直接拒绝。
//     推论：SPC=1/512B 扇区时数据区至少 65525 个扇区（32MB），加上保留扇区与两份
//     32 位 FAT，合法 FAT32 卷的下限约 33.5MB —— 所以 ESP 定 **48MB**（见 part64.h）。
//
// 支持范围（写清楚限制，别指望它是一般意义的 FAT 实现）：
//     * 目录：根目录 + 最多 2 层子目录（本项目只需要 EFI/BOOT 这一层）
//     * 文件：**簇链连续分配**（不做碎片合并），8.3 短名，无 LFN
//     * 单卷：一次只操作"最近一次 fat64_format64 的那个卷"
//     * 写：只支持"新建"（不覆盖同名文件）；删除/改名不支持
//
// 打点（自动验收 grep）：[FAT64] format / [FAT64] mkdir / [FAT64] file / [FAT64] selftest
#pragma once
#include <stdint.h>

// ---------------- 卷参数（与 tools/make_esp.py 对齐） ----------------
static const uint32_t FAT64_SECTOR       = 512;    // 扇区字节数
static const uint32_t FAT64_SPC          = 1;      // 每簇扇区数（簇 = 512B）
static const uint32_t FAT64_RESERVED     = 32;     // 保留扇区数（FAT32 必须 >= 32）
static const uint32_t FAT64_NUM_FATS     = 2;      // FAT 份数
static const uint32_t FAT64_ROOT_CLUSTER = 2;      // ★ 根目录起始簇（FAT32 根目录是簇链）
static const uint32_t FAT64_FSINFO_SEC   = 1;      // FSInfo 扇区号（BPB_FSInfo）
static const uint32_t FAT64_BKBOOT_SEC   = 6;      // 备份引导扇区号（BPB_BkBootSec）
static const uint8_t  FAT64_MEDIA        = 0xF8;   // 固定盘媒体描述符
static const uint32_t FAT64_EOF          = 0x0FFFFFFFu; // FAT32 链尾
static const uint32_t FAT64_CLUSTER_MIN  = 65525;  // ★ FAT32 合法簇数下界（硬断言）
static const uint32_t FAT64_MIN_SECTORS  = 66600;  // 能解出 >= 65525 簇的最小卷（~34MB）
static const uint32_t FAT64_MAX_FAT_SECTORS = 1024; // 96762 簇 * 4B / 512B = 756 扇区
static const int      FAT64_MAX_DIRS     = 8;      // 根 + 最多 7 个子目录
static const int      FAT64_MAX_CHAINS   = 32;     // 最多 32 段连续簇链（本用途 ~8 段）

// 自检用内存卷（**只在内存里**，绝不碰真盘）：34MB = 69632 扇区 -> 解出 68512 簇（>= 65525）。
// 为什么不用更小的：FAT32 的簇数硬下限决定了最小合法卷就是 ~34MB（见文件顶部说明）。
static const uint32_t FAT64_SELFTEST_SECTORS = 69632;
static const uint32_t FAT64_RAM_PAGE         = 4096;   // 内存卷按 4KB 页拼（页池分配，不要求连续）
static const uint32_t FAT64_RAM_MAX_PAGES    = FAT64_SELFTEST_SECTORS * FAT64_SECTOR / FAT64_RAM_PAGE;

// 格式化一个 FAT32 卷（写 BPB + FSInfo + 备份引导 + FAT1/FAT2 + 空根目录簇）。
// 成功返回 0，失败 -1。sectors 必须能解出 >= 65525 簇，否则拒绝（见文件顶部硬约束）。
// 打点：[FAT64] format lba=<start> sectors=<n> fs=FAT32 clusters=<n> free=<n> fat_sectors=<n> spc=1
int fat64_format64(int drive, uint32_t start_lba, uint32_t sectors);

// 在根（或已存在的子目录）下建一个子目录；path8_3 形如 "EFI" 或 "EFI/BOOT"。
// 成功 0 / 失败 -1。打点：[FAT64] mkdir path=<p> cluster=<c>
int fat64_mkdir64(int drive, const char* path8_3);

// 最近一次格式化出来的卷的簇数（0 = 还没有卷）。只给日志/验收用：
// [INSTALL] esp: lba=.. sectors=.. fs=FAT32 clusters=.. fat_ok=1
uint32_t fat64_last_clusters64();

// 写一个文件（数据在内存里）。path8_3 形如 "UEFI64.BIN" 或 "EFI/BOOT/BOOTX64.EFI"。
// 成功 0 / 失败 -1。打点：[FAT64] file path=<p> bytes=<n> clusters=<c>
int fat64_write_file64(int drive, const char* path8_3, const uint8_t* data, uint32_t len);

// 同 fat64_write_file64，但文件数据**从另一块盘的 LBA 流式拷进来**（数据不占内核内存）。
// 用途：KERNEL64.BIN 要写目标盘 LBA 9..8008 的 4MB 系统内核区，而安装程序内核
// 装不下这 4MB 的副本（体积硬约束）。
int fat64_write_file_from_disk64(int drive, const char* path8_3, int src_drive,
                                 uint32_t src_lba, uint32_t len);

// 离线自检：坏参数被拒 / 内存卷上的卷结构自洽 / 读写往返 / 目录项与簇链检查。
// 返回 0 = PASS，非 0 = 失败的位掩码。打点：[FAT64] selftest PASS|FAIL mask=<n>
// ★ 安全设计：自检**只在内存卷**上跑（页池临时分配），真盘的格式化只能走
//   fat64_format64（显式切后端）—— 曾经因为自检里误用真盘后端把安装介质格式化掉，
//   这条路径不能再退回去。
int fat64_selftest64();

// ==================== ★ 批次 K：读取器（只读浏览）====================
// 背景：FAT 卷以前只"识别"不"浏览"（本内核只有写入器）。本批给同一个文件加上**只读**读路径：
//   挂载/校验 BPB -> 列目录（8.3 + VFAT 长名 LFN）-> 按簇链读文件。
//
// 支持范围（如实写清，别指望它是一般意义的 FAT 实现）：
//   * **只读**：没有写/删/改名/建目录；上层（fs64/explorer/terminal/fd64）对 FAT 卷的写请求一律拒绝。
//   * 扇区固定 512B（BPB_BytsPerSec == 512，其它值拒绝）；每簇扇区数按 BPB 计算（1..128，含 U 盘常见的 8）；
//   * 卷类型按**簇数**判定：< 4085 -> FAT12、4085..65524 -> FAT16、>= 65525 -> FAT32；
//     本批**只挂载/浏览 FAT32**（FAT12/16 的簇链项是 12/16 位，本批不做；probe 仍会如实报出实际类型）。
//   * 目录：根目录簇链 + 任意层子目录簇链（"." / ".." 走真项）；跳过 0xE5 删除项、0x00 终止项、卷标项；
//     LFN（0x0F 项）：顺序位（0x40 = 最后逻辑项）/校验和（8.3 名字的 checksum）/UTF-16 拼接全按规范做，
//     非法 LFN 组回退成 8.3 短名；名字以 UTF-8 输出（LFN 上限 255 个 UTF-16 码元，缓冲 FAT64_NAME_MAX）。
//   * 时间：FAT 的 date/time（1980 基准）**转成与 vfs64 同一种打包编码**（见 kernel/vfs64.h），
//     这样文件管理器的"修改日期"列、属性面板不用为 FAT 再写一套格式化。
//   * 边界：簇号范围、簇链长度上限（FAT64_CHAIN_MAX，防坏链死循环）、单文件读取上限
//     （FAT64_READ_MAX_BYTES）、卷 LBA 范围全部先校验再用；坏 BPB / 坏链 / 越界一律打点 + 返回 -1。
//   * 不做：碎片整理、删除项复用、8.3 与 LFN 冲突的写回处理（读侧只看 LFN 是否自洽）。
//   * 可用空间：优先取 FSInfo 的 free 字段；FSInfo 无效时**不实时扫 FAT**（如实标为未知）。
//     所以容量数字是挂载那一刻的快照（见 fs64/drive64 的说明）。
//
// 打点（[FAT64] 前缀，行锁）：
//   [FAT64] probe lba=<n> fs=FAT32|FAT16|FAT12|none clusters=<n> spc=<n> fatsz=<n> [label=..]
//   [FAT64] mount vol=<n> lba=<n> clusters=<n> free=<n> fat_ok=1
//   [FAT64] list path=<p> entries=<n> / [FAT64] read path=<p> size=<n> bytes=<n>
//   [FAT64] reject <why>（坏 BPB / 坏链 / 越界 / 只支持 FAT32 等）
static const uint32_t FAT64_TYPE_12 = 12;    // 簇数 < 4085
static const uint32_t FAT64_TYPE_16 = 16;    // 4085..65524
static const uint32_t FAT64_TYPE_32 = 32;    // >= 65525
static const int      FAT64_VOL_MAX  = 4;    // 同时挂载的 FAT 卷上限（只读；与 vfs64 的 4 个卷槽同规格）
static const int      FAT64_VOL_NONE = -1;
static const uint32_t FAT64_NAME_MAX = 256;  // UTF-8 名字缓冲（LFN 255 UTF-16 码元解出来够用）
static const uint32_t FAT64_LFN_CHARS = 260; // LFN 组装缓冲（UTF-16 码元数；规范上限 255 + 余量）
static const uint32_t FAT64_CHAIN_MAX = 1u << 20;      // 单条簇链的簇数上限（坏链防护）
static const uint32_t FAT64_READ_MAX_BYTES = 16u * 1024u * 1024u;  // 单次读取上限（16MB）

// 卷信息（只读探测 / 挂载后查询）
struct Fat64Info64 {
    uint32_t bytes_per_sector;
    uint32_t spc;              // 每簇扇区数
    uint32_t reserved;
    uint32_t num_fats;
    uint32_t fatsz;            // 每份 FAT 的扇区数（FAT32 = BPB_FATSz32）
    uint32_t total_sectors;
    uint32_t root_cluster;
    uint32_t data_start;       // 卷内第一个数据扇区
    uint32_t clusters;         // 数据区簇数
    uint32_t cluster_bytes;    // spc * 512
    uint32_t fsinfo_sector;
    uint32_t free_clusters;    // FSInfo 的 free（0xFFFFFFFF = 未知）
    uint32_t fat_type;         // FAT64_TYPE_*
    uint8_t  mirr;             // 1 = 两份 FAT 互为镜像且逐字节一致
    uint8_t  fsinfo_ok;        // 1 = FSInfo 三个签名有效
    char     oem[9];           // BPB_OEMName（NUL 结尾）
    char     label[12];        // BPB 卷标（NUL 结尾）
};

// 目录项 / stat 结果
struct Fat64Entry64 {
    char     name[FAT64_NAME_MAX];   // UTF-8（LFN 长名或 "BASE.EXT" 短名）
    uint32_t attr;                   // FAT 属性位（0x10 = 目录、0x01 = 只读、0x02 = 隐藏、0x04 = 系统）
    uint32_t cluster;                // 首簇（0 = 空文件）
    uint32_t size;                   // 字节数（目录 = 0）
    uint32_t mtime;                  // 打包时间（vfs64 口径；0 = 未知）
    uint8_t  lfn;                    // 1 = 名字来自 VFAT 长名
};

// 只读探测：(drive, lba) 的首扇区必须是合法 BPB。成功 0 并把几何填进 *out；失败 -1（已打点）。
// **不改任何卷状态**（drive64 扫描所有分区时用）；out 可传 nullptr（只判真假）。
int fat64_probe64(int drive, uint32_t lba, Fat64Info64* out);

// 挂载一个 FAT32 卷（只读）到卷槽；同 (drive,lba) **幂等**（复用已有槽，不重复占）。成功 0 并填 *out_vol。
int fat64_mount64(int drive, uint32_t lba, int* out_vol);
// (drive,lba) 已挂载的卷号；-1 = 没挂载过。
int fat64_mount_find64(int drive, uint32_t lba);
// 卷号是否有效（合法且已挂载）。
int fat64_vol_used64(int vol);
// 卷信息（只读）。
int fat64_vol_info64(int vol, Fat64Info64* out);
// 卷的空闲簇数（挂载时从 FSInfo 缓存的快照；0xFFFFFFFF = 未知）。
uint32_t fat64_vol_free64(int vol);

// 列目录：path 形如 "/"、"EFI"、"EFI/BOOT"（大小写不敏感；"." / ".." 段支持）。
// 从 *cursor 开始最多填 max 条（跳过 "." / ".."）；把 *cursor 推到下一条；返回填充条数、0 = 结束、-1 = 错。
int fat64_list64(int vol, const char* path, Fat64Entry64* out, int max, uint32_t* cursor);

// 查属性（文件/目录都可以；根目录 "/" 返回 dir 条目）。成功 0；失败 -1（打点 path not found）。
int fat64_stat64(int vol, const char* path, Fat64Entry64* out);

// 读文件：最多 max 字节（同时受文件大小与 FAT64_READ_MAX_BYTES 约束），*out_len = 实际字节数。
// 目录 / 超上限 / 坏链 / 越界一律 -1。成功 0。
int fat64_read64(int vol, const char* path, void* buf, uint32_t max, uint32_t* out_len);

// 分块读：从文件偏移 off 读 len 字节（供大文件（如 4MB 的 KERNEL64.BIN）分块校验用）。
// *out_got = 实际读到的字节数（到文件末尾会短读）；成功 0。
int fat64_read_range64(int vol, const char* path, uint32_t off, void* buf, uint32_t len, uint32_t* out_got);
