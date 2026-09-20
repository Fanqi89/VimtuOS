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
