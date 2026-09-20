// fat64.h - 最小 FAT16 写入器（安装程序在**目标盘**上建 ESP 用）
//
// 为什么需要它：
//   安装盘（ISO）里的 ESP 是构建期由 tools/make_esp.py 用 Python 手写出来的；
//   而"装到硬盘上的系统"要能被 UEFI 固件启动，硬盘上也必须有一个**真正的
//   FAT16 EFI 系统分区**（ESP）—— 固件只认 FAT。所以把 make_esp.py 的卷格式
//   算法原样搬进内核（同样的卷参数），安装完成时写进目标盘。
//
// 卷参数（与 tools/make_esp.py **完全一致**，否则固件侧行为会变）：
//     * 512B 扇区、每簇 1 扇区（SPC=1）—— 这条是硬要求：簇数必须落在 FAT16 的
//       合法区间 [4085, 65525)，EDK2 的 FAT 驱动按**簇数**决定用 12/16 位读 FAT 表；
//       簇数 < 4085 会被当 FAT12 解析 -> 多簇文件读取报 EFI_VOLUME_CORRUPTED。
//     * 1 个保留扇区 + 2 份 FAT + 512 项固定根目录（32 扇区）+ 数据区
//     * BPB 里的 OEM/卷标/类型串与 make_esp.py 一致
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
static const uint32_t FAT64_SPC          = 1;      // 每簇扇区数（保证真 FAT16）
static const uint32_t FAT64_RESERVED     = 1;      // 保留扇区数
static const uint32_t FAT64_NUM_FATS     = 2;      // FAT 份数
static const uint32_t FAT64_ROOT_ENTRIES = 512;    // 根目录项数
static const uint32_t FAT64_ROOT_SECTORS = (FAT64_ROOT_ENTRIES * 32 + FAT64_SECTOR - 1) / FAT64_SECTOR;
static const uint8_t  FAT64_MEDIA        = 0xF8;   // 固定盘媒体描述符
static const uint32_t FAT64_CLUSTER_MIN  = 4085;   // FAT16 合法簇数下界
static const uint32_t FAT64_CLUSTER_MAX  = 65525;  // FAT16 合法簇数上界（不含）
static const uint32_t FAT64_MAX_FAT_SECTORS = 256; // 65525 簇 * 2B / 512B = 256 扇区
static const int      FAT64_MAX_DIRS     = 8;      // 根 + 最多 7 个子目录
static const int      FAT64_MAX_CHAINS   = 32;     // 最多 32 段连续簇链（本用途 ~6 段）

// 格式化一个 FAT16 卷（写 BPB + FAT1/FAT2 + 空根目录）。成功返回 0，失败 -1。
// 打点：[FAT64] format lba=<start> sectors=<n> clusters=<n> fat_sectors=<n> spc=1
int fat64_format64(int drive, uint32_t start_lba, uint32_t sectors);

// 在根（或已存在的子目录）下建一个子目录；path8_3 形如 "EFI" 或 "EFI/BOOT"。
// 成功 0 / 失败 -1。打点：[FAT64] mkdir path=<p> cluster=<c>
int fat64_mkdir64(int drive, const char* path8_3);

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
int fat64_selftest64();
