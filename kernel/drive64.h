// drive64.h - 盘符 / 驱动器枚举层（"此电脑"的**数据来源**，不含 UI）
//
// 为什么要有这一层：
//   桌面上的"此电脑"要显示"有几块盘、每块盘上有什么分区、哪个是系统盘（C:）、
//   哪些能点进去浏览、容量多少"。这些信息散在 ata64（有哪些驱动器）/ MBR（分区表）/
//   vfs64（卷超级块+位图算容量）/ fat64（ESP 的指纹）里。本层把它们**汇总成一张盘符表**，
//   并只读探测（不挂载、不格式化、不写盘），给 UI 一个稳定的接口。
//
// ==================== 枚举范围（如实写清）====================
//   * 磁盘：走 kernel/ata64.h 的**统一驱动器号**接口 —— 0..3 = PATA、8.. = AHCI(SATA)、
//     16.. = NVMe 命名空间（ata64_drive_count64 / ata64_slot_to_drive64），
//     ATAPI（光驱）与不存在的槽位跳过。
//   * 分区：每块盘的 **MBR 分区表**（LBA 0 的 4 个分区项；签名 0x55AA 才认）。
//     本工程安装器同时写"混合 MBR + 盘尾备份 GPT"，两者内容一致；
//     **不重复解析盘尾备份 GPT**（主 GPT 头按规范应在 LBA 1，而 LBA 1 被 loader64 占着 ——
//     见 kernel/part64.h 的说明）。所以 GPT-only 的盘（没有 MBR 项）只会显示成"未分区"。
//   * 没有分区项的盘（例如安装介质盘、空盘）会作为**一条"整盘/未识别"记录**列出，
//     不分配盘符，打点 reason=no-fs。
//
// ==================== 文件系统识别 ====================
//   * VimtuFS2：分区首扇区 magic = "VIMTUFS2"，并且**过一遍 vfs64 的超级块校验**
//     （CRC32 + 几何自洽重算）→ 才算"可浏览的卷"。容量从超级块拿（总块数 / 空闲块数 × 512B）。
//   * FAT32/FAT16：分区首扇区是合法的 BPB（0x55AA + 偏移 82 "FAT32   " 或偏移 54 "FAT"）。
//     标成 "FAT32"、名字给"EFI 系统分区"（type = 0xEF 时）或"FAT 卷"；
//     **可浏览 = 否**（本内核只有 FAT32 写入器，没有 FAT 读取/列目录，见 kernel/fat64.h）。
//   * 其它：unknown（不分配盘符）。
//
// ==================== 盘符规则（依据写清）====================
//   * **C: = 运行中的系统所在分区**。判定依据（按优先级）：
//       1) vfs64 当前**真正挂载**的那个卷的 (drive, start_lba)：
//          系统内核 os_boot_path 用 app64_main_part_lba64(0)（= 读 drive 0 的 MBR、
//          取 type 0x07 的主数据分区项，读不到才退回常量 PART_MAIN_LBA = 8009）
//          作为挂载参数调用 vfs64_mount_system64 -> 挂载成功即"这就是系统盘/系统分区"；
//       2) 还没挂载时（例如枚举被更早调用）：drive 0 的 MBR 里第一个 type = 0x07 的项；
//       3) 都没有：枚举顺序里第一个可浏览的 VimtuFS2 卷。
//     也就是说 C: 一定是一个**已经能挂载的 VimtuFS2 卷**（挂载失败时宁可不给 C:）。
//   * 其余可浏览的 VimtuFS2 卷按枚举顺序（驱动器号升序、盘内分区项升序）依次拿 D:、E:、…（跳过 A:/B:）。
//   * ★ 多卷：每个**可浏览**的条目都会在扫描时占用一个 vfs64 卷槽（DriveInfo64.slot），
//     drive64_activate_letter64('D') = 把这个槽激活成"当前卷"，于是 D:/E:/… 真的能点进去浏览与读写。
//     槽不够（VFS64_SLOT_MAX = 4）时**如实拒绝**：该条目 browsable = false、skip = DRV64_SKIP_NOSLOT
//     （打点 reason=voltable-full），绝不悄悄覆盖已经挂载的卷。
//   * **被跳过的条目（ESP / 未知文件系统 / 未分区 / 卷槽满）不占字母**，但仍会在表里列出（letter = 0）。
//     UI 可以显示成"EFI 系统分区（不浏览）"。
//
// ==================== 打点（自动验收 grep；格式勿改）====================
//   [DRV64] scan disks=<n> parts=<n> fs=<n>
//   [DRV64] letter=C: disk=<n> part=<n> fs=VimtuFS2 total_kb=<n> free_kb=<n> slot=<n>
//   [DRV64] skip lba=<n> type=<0xEF|unknown|VimtuFS2> reason=<esp|no-fs|voltable-full>
//   [DRV64] activate letter=D: slot=<n> disk=<n> lba=<n> ok|FAILED reason=<...>
//   [DRV64] selftest PASS / [DRV64] selftest FAIL mask=<n>
//   （另有 [DRV64] no browsable volume / skipped (no disk) / skipped (VimtuFS2 not mounted) 等说明行）
#pragma once
#include <stdint.h>

#define DRV64_MAX_ENTRIES 24u      // 盘符表容量（条目 = 分区 / 整盘；超出只打点不崩）
#define DRV64_NAME_MAX    40u      // 显示名缓冲
#define DRV64_FS_MAX      16u      // 文件系统字符串缓冲

// 文件系统种类（DriveInfo64.fskind）
#define DRV64_FS_UNKNOWN     0u
#define DRV64_FS_VIMTUFS2    1u
#define DRV64_FS_FAT32       2u
#define DRV64_FS_FAT12_16    3u

// 跳过原因（DriveInfo64.skip）—— 被跳过的条目不分配盘符
#define DRV64_SKIP_NONE  0u
#define DRV64_SKIP_ESP   1u        // type 0xEF：EFI 系统分区（UEFI 引导卷；不浏览，免得碰坏引导）
#define DRV64_SKIP_NOFS  2u        // 没有认出文件系统（未格式化 / 未知 / 卷损坏）
#define DRV64_SKIP_NOSLOT 3u       // ★ 是合法 VimtuFS2 卷，但 vfs64 卷槽表已满（如实拒绝，绝不覆盖已有卷）
#define DRV64_SLOT_NONE  0xFFu     // DriveInfo64.slot：没有占用 vfs64 卷槽

struct DriveInfo64 {
    bool     present;              // 该条目是否有效（present = false 表示表槽为空）
    char     letter;               // 盘符 'C'..'Z'；0 = 不占字母（ESP / 未识别）
    char     name[DRV64_NAME_MAX]; // 显示名（"本地磁盘" / "EFI 系统分区" / "FAT 卷" / "未识别分区"）
    char     fs[DRV64_FS_MAX];     // 文件系统字符串（"VimtuFS2" / "FAT32" / "FAT16" / "unknown"）
    int      disk;                 // 驱动器号（ata64 统一编号）
    int      part;                 // MBR 分区项号 1..4；0 = 整盘（没有分区项）
    uint32_t start_lba;            // 分区起始绝对 LBA（part=0 时 = 0）
    uint32_t sectors;              // 分区扇区数（part=0 时 = 整盘扇区数）
    uint64_t total_kb;             // 总容量 KB（total_known = false 时无意义）
    uint64_t free_kb;              // 可用容量 KB（free_known = false 时无意义）
    bool     total_known;          // 容量已知（VimtuFS2：超级块；FAT：BPB；unknown：否）
    bool     free_known;           // 可用已知（只有 VimtuFS2 能从位图算）
    bool     browsable;            // 是否可浏览（= 校验通过的 VimtuFS2 卷 + **已经占用一个 vfs64 卷槽**）
    bool     system;               // 是否系统盘/系统分区（C:）
    uint32_t vol_version;          // VimtuFS2 卷版本（2 / 3）；非 VimtuFS2 = 0
    uint8_t  fskind;               // DRV64_FS_*
    uint8_t  skip;                 // DRV64_SKIP_*
    uint8_t  slot;                 // ★ vfs64 卷槽号（browsable = true 时有效）；DRV64_SLOT_NONE = 没占槽
};

// 扫描/刷新（**幂等**：重复调用结果一致，只是重新读一遍盘）。返回表里的条目数（>= 0）。
int  drive64_scan64();

// 当前表里的条目数（未扫描过 = 0）
int  drive64_count64();

// 取第 i 条（0-based）。返回 0 = 成功；-1 = 下标越界（已打点）。
int  drive64_info64(int i, DriveInfo64* out);

// 按盘符查条目下标（大写字母，'C'..）；找不到 / 该字母没分配返回 -1。
int  drive64_by_letter64(char letter);

// ★ 激活某个盘符对应的卷（= vfs64 的"当前卷"切过去）：0 = 成功；-1 = 没这个盘符 / 不可浏览 / 没占槽。
// 打点：[DRV64] activate letter=D: slot=1 disk=1 lba=8192 ok（失败打 FAILED reason=...）。
int  drive64_activate_letter64(char letter);

// 当前"活动盘符"（= vfs64 当前卷对应的字母）；没有可浏览卷时返回 0。
char drive64_current_letter64();

// 跳过原因的稳定字符串（"esp" / "no-fs" / "voltable-full" / "none"）。
const char* drive64_skip_reason64(uint8_t skip);

// 自检（位掩码，0 = 全过；打印 [DRV64] selftest PASS|FAIL mask=<n>）：
//   bit0(1)    C: 存在且是可浏览的 VimtuFS2 卷（完全没有可浏览卷时如实跳过，不算失败）
//   bit1(2)    每个可浏览条目的 total_kb > 0、free_kb <= total_kb、且两个值都已知
//   bit2(4)    盘符唯一且从 C: 起连续；被跳过的条目 letter = 0
//   bit3(8)    by_letter64 与表一致（下标 / 盘符 / 驱动器号 / 起始 LBA）
//   bit4(16)   幂等：再扫一遍条目数与 C: 的 (disk, part, start_lba, total_kb) 不变
//   bit5(32)   skip 条目自洽：skip != NONE 且 letter = 0 且 start_lba < 分区/整盘范围
//   bit6(64)   ★ 多卷槽一致：可浏览条目的 slot 有效且指向同一个 (disk,start_lba)；
//             activate_letter64('C') 成功、current_letter64 对得上；有 D: 时双向切换；
//             不存在的盘符 -> -1 且不改当前盘。
int  drive64_selftest64();
// 串口打印盘符表（每条一行 [DRV64] letter=... / [DRV64] skip ...；验收与排障用）
void drive64_dump64();

// kind（VFS64_KIND_*，见 kernel/vfs64.h）-> UI 用的稳定小写字符串；未知 -> "unknown"。
// 放在这里是为了让 UI 只 include 一个头就有"类型名字"。
const char* drive64_kind_name64(uint32_t vfs_kind);
