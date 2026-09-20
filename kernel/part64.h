// part64.h - VimtuOS 安装程序的分区表与安装引擎
//
// 职责：
//   * 把"标准布局"写进目标硬盘的主引导记录（MBR 分区表）：
//       P1 引导分区（type 0xEF，活动）  LBA 9 .. 8008      ← 放 loader + 内核
//       P2 主分区  （type 0x07）        LBA 8009 .. ESP 之前（无 ESP 时到盘尾）
//       P3 EFI 系统分区（type 0xEF）    盘尾、GPT 备份表之前（只有"够大的盘"才有）
//   * 删除 / 格式化指定分区
//   * 把安装介质上的载荷（system.img）整盘复制到目标硬盘，并汇报真实进度
//   * 复制完成后写 **GPT（盘尾备份头 + 项数组）+ 混合 MBR + FAT32 ESP**
//     （EFI/BOOT/BOOTX64.EFI + UEFI64.BIN + KERNEL64.BIN），让装好的盘 UEFI/BIOS 双启动
//
// 为什么主 GPT 头不在 LBA 1：引导链是"MBR + 固定 LBA"（boot.bin 读 LBA 1..8 的 loader，
//   loader 读 LBA 9 起的内核），**LBA 1 被 loader 占着**。所以只写盘尾的备份 GPT：
//   UEFI 规范要求"主头无效时应使用备份头"，EDK2（OVMF / VMware EFI 都是它）实测在
//   主头不是 "EFI PART" 时会回退到备份头 —— tests/esp_install_test.py 用 OVMF 实测验证。
#pragma once
#include <stdint.h>

// 标准布局（必须与 boot/loader64.asm、kernel/memlayout64.h 的三方约定一致）
static const uint32_t PART_BOOT_LBA  = 9;        // 引导分区起点
static const uint32_t PART_BOOT_SECS = 8000;     // 引导分区扇区数（loader 读盘上限）
static const uint32_t PART_MAIN_LBA  = PART_BOOT_LBA + PART_BOOT_SECS;   // 8009
static const uint32_t PART_MAIN_MIN_SECS = 1024; // 主分区至少留 512KB，否则视为"磁盘太小"

// ★ ESP / GPT（安装完成时写：让**装好的盘**在 UEFI 固件下也能启动）
//   为什么 ESP 在**盘尾**：引导链三处硬约定占住了盘头 ——
//     LBA 0 MBR、LBA 1..8 loader（**主 GPT 头必须放 LBA 1，与 loader 冲突，所以只写盘尾的
//     备份 GPT**，EDK2 在主头无效时会用备份头，见 part64.cpp 的说明）、LBA 9..8008 内核。
//   ESP 尺寸 = **48MB**（98304 扇区）：FAT32 有**簇数硬下限 65525**（少于这个数就应被当作
//     FAT16），SPC=1/512B 扇区时数据区至少 65525 扇区（32MB），加上 32 个保留扇区与两份
//     32 位 FAT，最小合法卷 ≈ 33.5MB。这里取 48MB 留余量（解出 96736 簇）；
//     ESP 里只放 3 个文件（4MB 内核块 + 30KB 引导器；构建期那份还含 8MB 载荷），
//     48MB 的卷完全装得下。
//   为什么目标盘要 >= ~60MB：ESP 要在主分区**之外**（不与 VimtuFS2 重叠），
//     最小盘 = 8009（引导区起点）+ 16384（主分区至少 8MB）+ 98304（ESP）+ 33（备份 GPT）
//            = 122730 扇区 ≈ 59.9MiB。更小的盘（例如 16MB 的回归目标盘）放不下 ESP
//     -> 只写老 MBR 布局（BIOS-only），串口打 "[INSTALL] esp skipped (disk too small)"。
static const uint32_t PART_ESP_SECTORS       = 98304;   // ESP 大小 = 48MB（真 FAT32 的容量下限 ~33.5MB）
static const uint32_t PART_ESP_MIN_MAIN_SECS = 16384;   // 建 ESP 时主分区至少保留 8MB
static const uint32_t PART_GPT_BACKUP_SECTORS = 33;     // 盘尾备份 GPT：32 扇区项数组 + 1 扇区头
static const uint32_t PART_GPT_ENTRIES = 128;           // GPT 分区项数组项数

// ESP 几何（盘尾、GPT 备份表之前）。返回 false = 目标盘太小，不建 ESP。
bool part_esp_geometry64(uint32_t disk_sectors, uint32_t* out_start, uint32_t* out_sectors,
                         uint32_t* out_main_sectors);
// 一个分区项（从 MBR 里读出来的）
struct PartInfo {
    bool     used;
    uint8_t  type;
    bool     bootable;
    uint32_t start;
    uint32_t sectors;
};

// 载荷头（构建脚本写在安装介质的 VIMTU_PAYLOAD_LBA 处）
//   magic = "VIMTUPAY"，随后是载荷扇区数与载荷 LBA
struct PayloadHeader {
    char     magic[8];
    uint32_t sectors;
    uint32_t lba;
    uint32_t reserved;
};

// ---- 读取与查询 ----
bool part_read_mbr(int drive, uint8_t* sec512);                 // 读 LBA0
void part_parse_mbr(const uint8_t* sec512, PartInfo out[4]);    // 解析 4 个分区项
uint32_t part_unallocated_sectors(const uint8_t* sec512, uint32_t disk_sectors);

// ---- 写盘动作（安装界面上的"新建 / 删除 / 格式化"）----
// 新建：写入标准布局（引导分区 + 主分区）。若已有分区则拒绝（返回 false），
//       避免把别人已经分好区的盘直接覆盖掉。
bool part_create_standard(int drive, uint32_t disk_sectors);
// 删除：清掉第 index 个分区项（1-based，与界面上的"分区 N"对应）
bool part_delete_entry(int drive, int index);
// 格式化：把分区首部若干扇区清零；主分区还会写入一个占位超级块 "VIMTUFS1"
bool part_format_partition(int drive, const PartInfo& p, int index);

// ---- 安装：把载荷整盘复制到目标盘 ----
// 分块进行，便于主循环按块推进进度（不是阻塞式一把梭，界面才不会假死）
struct InstallJob {
    bool     active;
    bool     done;
    bool     failed;
    int      src_drive;         // 安装介质所在驱动器
    int      dst_drive;         // 目标驱动器
    uint32_t payload_lba;       // 载荷起始 LBA（含头）
    uint32_t payload_sectors;   // 载荷扇区数
    uint32_t copied;            // 已写扇区
    uint32_t write_table_at;    // 复制完成后重写分区表的目标磁盘扇区总数
    uint8_t  src_kind;          // INSTALL_SRC_DISK / _CD / _RAM
    uint8_t  src_cd;            // 光盘驱动器号（src_kind = CD）
    uint32_t src_lba;           // CD：载荷起始光盘扇区（2048B）；RAM：载荷物理地址
};

// ---- 介质描述符（引导桩 boot/cdiso.asm 写在物理地址 0x0F00，内核启动后仍可读）----
// ISO 形态的安装介质上，"内核/载荷在哪"只有 ISO9660 目录知道，所以由引导桩把
// 位置写进这个固定地址；loader64 与安装程序都按它取。
//   0x0F00 "VMMD" | 0x04 kind | 0x05 drive | 0x06 rsv | 0x08 kernel_lba
//   0x0C payload_lba | 0x10 payload_secs | 0x14 image_bytes
// kind: 1 = 光盘（走 ATAPI），2 = RAM（引导桩已把内容读进内存）
#define MEDIUM_DESC_ADDR   0x0F00u
static const uint32_t MEDIUM_MAGIC = 0x444D4D56u;   // 'VMMD'
static const uint8_t  MEDIUM_CD    = 1;
static const uint8_t  MEDIUM_RAM   = 2;

struct MediumDesc {
    uint32_t magic;
    uint8_t  kind;
    uint8_t  drive;
    uint16_t rsv;
    uint32_t kernel_lba;
    uint32_t payload_lba;
    uint32_t payload_secs;
    uint32_t image_bytes;
};
// 读取介质描述符；返回 false 表示没有（老路径：安装介质是裸盘/U 盘，载荷在某个硬盘上）
bool part_read_medium_desc(MediumDesc* out);
// 安装源种类
enum InstallSrcKind { INSTALL_SRC_DISK = 0, INSTALL_SRC_CD = 1, INSTALL_SRC_RAM = 2 };

// 用"介质描述符"初始化安装任务（光盘 = 走 ATAPI 读；RAM = 直接从内存拷）
bool part_install_begin_medium(InstallJob* job, const MediumDesc& md, int dst_drive,
                               uint32_t dst_sectors);

// 在 4 个驱动器里找"带载荷的安装介质"，返回驱动器号，找不到返回 -1
int  part_find_payload_drive(uint32_t payload_lba, uint32_t* out_sectors);
   // 找出"安装介质本身所在的那块盘"（U 盘 / hybrid ISO 场景）：ISO9660 主卷描述符固定落在
   // LBA 16，其偏移 +1..+5 是 "CD001"。这个指纹与 BIOS 磁盘号、ATA 通道顺序都无关，
   // 比"猜 BIOS 的 0x80 对应哪块 ATA 盘"可靠。找不到返回 -1。
int  part_find_iso_medium_drive(void);

// 初始化一次安装任务；返回 false 表示找不到载荷/参数不对
bool part_install_begin(InstallJob* job, uint32_t payload_lba, int dst_drive, uint32_t dst_sectors);

// 推进一次安装（每调用一次写一块）。返回值：0 进行中 / 1 完成 / -1 失败
int  part_install_step(InstallJob* job);
