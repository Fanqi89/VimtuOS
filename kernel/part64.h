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

// ==================== v0.5.14：分区表来源 + 统一删除/格式化 ====================
// 为什么需要这一层（真缺陷，实测记录在 tests/partmgr64_test.py）：
//   界面上的"分区 N"这一行可能来自 **MBR**，也可能来自 **GPT**（带保护性 0xEE 项的盘
//   以 GPT 项为准，见 setup64.cpp 的 probe_disk）。v0.5.13 及以前的删除/格式化只按
//   **MBR 项号** 去写盘，在 GPT 盘上实测：
//     * 选中 GPT 分区 1（LBA 2048）按"删除" -> 清掉的是 MBR 第 2 项（用户没选它）；
//     * 按"格式化" -> 按 MBR 保护性 0xEE 项的 start=1/sectors=整盘 去清零 LBA1..64，
//       **把主 GPT 头和项数组抹了**，而用户选的那个分区一个字节都没动。
enum PartSrc { PART_SRC_MBR = 0, PART_SRC_GPT = 1 };

// 一次删除/格式化的目标（界面按行填好，内核按它写盘；几何以内核重读的表为准）
struct PartTarget {
    int      src;        // PART_SRC_MBR / PART_SRC_GPT
    int      index;      // 该表内的 0-based 项号
    uint32_t start;      // 分区起始 LBA（MBR 表才是权威，GPT 会重新读）
    uint32_t sectors;    // 分区扇区数
    uint8_t  mbr_type;   // 仅 MBR：类型码（0x07 主分区 / 0xEF 引导 / 0x0C FAT32 …）
    bool     gpt_esp;    // 仅 GPT：类型 = EFI 系统分区
    bool     gpt_basic;  // 仅 GPT：类型 = Microsoft Basic Data
};

// GPT 位置：主头在 LBA1，备份头在盘尾；本系统装好的盘 LBA1 是 loader，只有备份头
struct GptLoc {
    bool     valid;
    bool     primary;      // true = LBA1 主头；false = 盘尾备份头
    uint32_t hdr_lba;      // 头所在 LBA
    uint32_t ent_lba;      // 项数组起始 LBA
    int      ent_count;
    int      ent_size;
};
struct GptEntryInfo {
    bool     used;
    bool     is_esp;       // 类型 = EFI 系统分区
    bool     is_basic;     // 类型 = Microsoft Basic Data
    uint64_t first;        // 起始 LBA
    uint64_t last;         // 结束 LBA（含）
};

// 定位 GPT（主头优先且要过头 CRC 自检；否则退到盘尾备份头）
bool part_gpt_locate64(int drive, uint32_t disk_sectors, GptLoc* out);
// 读第 index 项
bool part_gpt_entry64(int drive, const GptLoc& loc, int index, GptEntryInfo* out);
// 清掉第 index 项：项数组重写 + 项数组 CRC + 头 CRC 全部重算；主/备两份都清
bool part_gpt_delete_entry64(int drive, uint32_t disk_sectors, int index);
// 抹掉盘上所有 GPT 头（新建分区表之前调用，避免"删了分区但盘尾 GPT 还描述着它"）
bool part_gpt_wipe64(int drive, uint32_t disk_sectors);

// ---- 统一动作（安装界面只调这两个）----
// 删除：按 src 清对应表的项；**另一张表里描述同一段**的项也一并清掉（我们自己的安装盘
//       是 MBR 项 + 盘尾 GPT 双份），最后抹掉该分区首扇区（删掉的卷不该再被当卷挂上）。
bool part_delete_partition64(int drive, uint32_t disk_sectors, const PartTarget& t);
// 格式化：按 src/类型选文件系统 —— 主数据分区 / GPT basic data -> 真 VimtuFS2；
//         EFI 系统分区（盘尾 ESP 或 GPT ESP）-> 拒绝（抹了 UEFI 就起不来）；
//         其余 -> 清首 64 扇区。
//   返回 0 = OK；-1 = 参数/读盘失败；-2 = 受保护（EFI 系统分区）。
//   fs_out 回填 "VimtuFS2" / "blank" / "refused-esp"。
int  part_format_partition64(int drive, uint32_t disk_sectors, const PartTarget& t,
                             char* fs_out, int fs_cap);

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
