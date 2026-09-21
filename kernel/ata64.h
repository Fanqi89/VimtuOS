#pragma once
#include <stdint.h>

// ata64.h - Vimtu64 精简 ATA PIO 驱动（安装程序专用）
//
// 为什么不直接用 32 位的 kernel/ata.cpp：
//   那份依赖 sysstate（重启状态机）与 32 位内联汇编（pushfl; popl），且面向"系统盘"，
//   而安装程序需要的是"枚举所有磁盘 + 读写任意 LBA"这种更小、更硬的需求。
//   这里独立实现，避免把安装程序绑到桌面系统的模块图上。
//
// 支持：PATA 4 个驱动器（primary/secondary × master/slave，LBA28 PIO）**+ AHCI(SATA)**。
// 限制：PATA 侧只支持 LBA28（≤128GB）、不做 DMA；AHCI 侧走 DMA + LBA48（见 kernel/ahci64.h），
//   两者都默认由中断/轮询等待，超时/从通道/调用点 IF=0 时自动回退 PIO 轮询（见 ata64_init64 注释）。
//
// ==================== 统一驱动器号（item 5a：AHCI 接入后**保持语义一致**）====================
//   0..3          = PATA（0=primary master / 1=primary slave / 2=secondary master / 3=secondary slave）
//   8..(8+N-1)    = AHCI 上第 1..N 块 **ATA 盘**（N = ahci64_count64()；AHCI 端口按端口号升序编号）
//   4..7          = 保留空洞（不映射任何设备）—— 旧代码里的"0..3 循环"必须改成下面的槽位接口
//   16..(16+M-1)  = NVMe 第 1..M 个**命名空间**（M = nvme64_count64()；见 kernel/nvme64.h）
// 上层（setup64 / part64 / vfs64 / store64）拿到的驱动器号就是上面这套编号，读写/识别全部由
// 本文件的 ata64_* 内部**分派**：≥ ATA64_NVME_BASE 转 nvme64_*，≥ ATA64_AHCI_BASE 转 ahci64_*，
// 其余走 PATA。
// 为什么要留空洞而不是紧接着 4 号：AHCI 盘的编号一旦与 PATA 混在一起，将来加第三类控制器
// （NVMe 之类）就会漂；留一段固定基址，编号=接口类型，分区表/安装逻辑里写下的号永远可解释。
static const int ATA64_AHCI_BASE = 8;
static const int ATA64_NVME_BASE = 16;
// 按驱动器号开数组的调用点（setup64.cpp 的磁盘表）用这个"最大驱动器号 + 1"：
//   NVMe 侧上限= 8 个命名空间（与 kernel/nvme64.h 的 NVME64_MAX_NS 一致）。
static const int ATA64_MAX_DRIVE64 = ATA64_NVME_BASE + 8;

// 枚举接口（**新代码用它，不要自己写 for (d=0; d<4; d++)**）：
//   ata64_drive_count64()   = 4 + AHCI 盘数 + NVMe 命名空间数（即"有几个可枚举的槽"）
//   ata64_slot_to_drive64(i)= 第 i 个槽的驱动器号（0,1,2,3,8,9,...,16,...；i 越界返回 -1）
int ata64_drive_count64();
int ata64_slot_to_drive64(int slot);

struct DiskInfo {
    bool     present;
    bool     atapi;          // true = ATAPI（光驱），安装程序不把它当安装目标
    char     model[41];      // 型号字符串（已去尾空格）
    uint64_t sectors;        // 总扇区数（512B/扇区）
};

// drive: 0=primary master, 1=primary slave, 2=secondary master, 3=secondary slave,
//        8..=AHCI 盘、16..=NVMe 命名空间（见上面的统一驱动器号说明）；4..7 是保留空洞，调用必失败。
bool ata64_identify(int drive, DiskInfo* out);

// 读写：PATA 侧 LBA28、count **任意**（内部按 ≤128 扇区分块 + 每块最多 3 次重试 —— ATA 的
//   扇区计数寄存器只有 8 位，一条命令 >255 个扇区会被设备静默截断；见 ata64.cpp 的总说明）；
//   AHCI 侧 LBA48、count ≤ 65536（按 128 扇区分块）；NVMe 侧 count 任意（按 128 扇区分块）。
//   驱动器号分派：≥ ATA64_NVME_BASE 走 nvme64_*，≥ ATA64_AHCI_BASE 走 ahci64_*。返回 false 表示出错。
bool ata64_read (int drive, uint32_t lba, uint32_t count, void* buf);
bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf);

// ---------------- ATAPI（光驱）：PACKET 命令 + PIO 读 ----------------
// 为什么需要它：64 位安装介质的正确形态是 **ISO**（光盘/U 盘/虚拟机光驱），
// 而光盘不能像硬盘那样用 READ SECTORS(LBA28) 读 —— 必须发 SCSI 风格的
// PACKET(12 字节 CDB) 命令。ISO9660 的扇区是 2048 字节（硬盘是 512），
// 所以这里按"光盘扇区"计数。
//
// 读法（PIO）：
//   1) 选驱动器（ATAPI 用 0xA0/0xB0）→ 写字节计数上限 = 本次要读的字节数
//   2) 发 0xA0 (PACKET) → 等 DRQ → 把 12 字节 CDB 当 6 个字写进数据口
//   3) 每轮 DRQ 搬 512 字节，直到字节数搬完 → 等 BSY=0/DRQ=0 收尾
// 目录用 READ(10)（CDB[0]=0x28），一次读一个光盘扇区（最稳，避免
// "byte count 与设备实际给的数据量不一致"导致的 ABRT）。
bool ata64_atapi_read(int drive, uint32_t cd_lba, uint32_t sectors, void* buf);
void ata64_atapi_diag(int drive);

// 用 IDENTIFY PACKET DEVICE(0xA1) 读光驱型号（比 0xEC 的签名探测更完整）
bool ata64_atapi_identify(int drive, DiskInfo* out);

// 自检：找一个 ATAPI 设备，读它的光盘 LBA 16（ISO9660 主卷描述符，开头必定
// 是 01 'C' 'D' '0' '0' '1'），校验通过说明 ATAPI 通路真的可用。
// 结果打到串口（[CD] 行），供自动验收断言。成功时 *out_drive 给出驱动器号。
bool ata64_atapi_selftest(int* out_drive);

// 诊断：ATAPI 出错时留下的现场（阶段 / 状态 / 错误寄存器）
extern volatile uint8_t  ata64_cd_stage;
extern volatile uint8_t  ata64_cd_err;
extern volatile uint8_t  ata64_cd_st;
// 便捷：读/写一个扇区
bool ata64_read_sector (int drive, uint32_t lba, void* buf);

// 诊断（排障用）：打印 4 个驱动器的原始寄存器值，识别失败时用它定位
void ata64_diag();

// 读路径诊断（失败时打印，见 setup64.cpp）：状态序列 / 错误寄存器 / 轮询次数 / 失败原因
extern volatile uint8_t  ata64_dbg_st[8];
extern volatile uint8_t  ata64_dbg_err;
extern volatile uint32_t ata64_dbg_polls;
extern volatile uint8_t  ata64_dbg_reason;   // 1=wait_ready 失败 2=ERR 3=DRQ 超时
bool ata64_write_sector(int drive, uint32_t lba, const void* buf);

// ==================== IRQ14（中断驱动等待 + 超时回退轮询）====================
// ata64_init64()：先把 AHCI(SATA) 与 NVMe 拉起来（幂等；没有控制器只打一行 not found），
//   再放开主通道 IRQ14、清设备控制寄存器的 nIEN、打印启用行，并跑一次只读自检。
//   幂等；系统内核与安装内核的启动路径各调用一次（kernel64.cpp / setup64.cpp）。
// 语义（如实）：默认有 IRQ14 就走"中断唤醒"；以下情况自动回退到原 PIO 轮询（打一次
//   "[ATA64] irq14 timeout -> fallback to polling"）：
//     * IRQ14 在超时窗口内没来（中断被屏蔽 / 设备/平台不投递）；
//     * 从通道（drive 2/3，IRQ15 没有处理入口）；
//     * 调用点 IF=0（在中断/关键区里），中断等待根本不可能生效。
//   回退后行为与旧驱动完全一致：绝不挂死、也不把失败当成功。
void ata64_init64();
bool ata64_irq_selftest64();

// 统计（自动验收 grep）：irqs=IRQ14 到达次数、reads=由中断唤醒完成的等待次数、
// polled=超时回退到轮询的等待次数。自检行格式：
//   [ATA64] irq14 selftest reads=<n> irqs=<n> polled=<n> status=ok
//   [ATA64] irq14 selftest PASS / FAIL mask=<n>
extern volatile uint32_t ata64_irq14_irqs;
extern volatile uint32_t ata64_irq14_reads;
extern volatile uint32_t ata64_irq14_polled;
