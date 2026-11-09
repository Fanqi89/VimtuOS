// ahci64.h - VimtuOS 最小 AHCI(SATA) 驱动（真机可用性套件 item 5a）
//
// 定位与边界（先读，避免误期待）：
//   * 只做**非队列 DMA**：每个端口一个命令槽（slot 0），一条命令一个或多个 PRDT 项，
//     一次最多 128 扇区（64KB，见 ahci64_xfer 的分块说明）。
//   * 读写用 READ/WRITE DMA EXT（0x25/0x35，**LBA48**）；识别用 IDENTIFY DEVICE（0xEC，
//     AHCI 下同样走 DMA 通路，一个 512B 的 PRDT 项）；ATAPI 端口用
//     IDENTIFY PACKET DEVICE（0xA1）拿型号。
//   * **全程轮询** PxCI 清零 / PxIS / PxTFD（不开端口中断、不 unmask IRQ）：
//     安装程序（纯 8259 PIC）与系统内核（LAPIC 可能已接管）两条路径行为完全一致，
//     也不需要跟中断路由耦合。
//   * 超时**双保险**：g_ticks64 计时（250Hz，默认 250 tick = 1s）+ 自旋计数上限
//     （IF=0 时 g_ticks64 根本不前进，只靠计时就会死循环）。绝不挂死；错误一律如实
//     返回 false 并留 PxTFD/PxIS/PxSERR 打点，绝不把失败当成功。
//   * 与 PATA 的关系：kernel/ata64.cpp 把**驱动器号 8..** 分派到这里（见 ata64.h 的
//     ATA64_AHCI_BASE），上层（setup64 / part64 / vfs64 / store64）完全不用改。
//   * ATAPI（PxSIG = 0xEB140101）：**只识别**（kind=atapi + 型号），不给驱动器号，
//     **不提供 ATAPI 读** —— 光盘读盘仍然走 ata64 的 PATA PACKET 路径。
//   * 单线程/串行化假设：与 PATA 路径一致（同一时刻只有一条命令在飞，没有锁）。
//
// 串口打点（验收按行 grep，改前先想清楚）：
//   [AHCI64] not found                                  （没控制器：优雅降级，不崩）
//   [AHCI64] pci <b>:<d>.<f> abar=<hex> cap=<hex> pi=<hex> ports=<n>
//   [AHCI64] hba reset <ok|timeout> bohc=<hex>
//   [AHCI64] port=<n> det=<d> sig=<hex> kind=ata|atapi|none
//   [AHCI64] drive <n> model=<...> sectors=<n>          （<n> = 统一驱动器号 8+idx）
//   [AHCI64] read lba=<n> count=<n> ok                  （失败：... failed tfd=<hex> is=<hex> serr=<hex>）
//   [AHCI64] write lba=<n> count=<n> ok
//   [AHCI64] selftest PASS / FAIL mask=<n> / skipped (no controller)
// selftest FAIL mask：1=控制器命令未完成 2=PxIS 错误位 4=PxTFD ERR/DF 8=IDENTIFY 失败
//                     16=读回失败 32=型号/容量非法
#pragma once
#include <stdint.h>
#include "ata64.h"      // DiskInfo（与 PATA 共用同一份结构）

// 最多登记多少个"端口上的设备"（ATA 盘 + ATAPI 光驱，按端口号升序）。
// 每台设备一份 1 页命令列表/FIS/命令表 + 一份 4KB×16 的 DMA 暂存（按需）。
#define AHCI64_MAX_DEVS  8

enum Ahci64Kind : uint8_t {
    AHCI64_KIND_NONE  = 0,
    AHCI64_KIND_ATA   = 1,
    AHCI64_KIND_ATAPI = 2,
};

// 一个端口的如实快照（未登记的端口不进这张表）
struct Ahci64PortInfo {
    uint8_t  port;          // 硬件端口号 0..31
    uint8_t  det;           // PxSSTS.DET（3 = 就绪）
    uint8_t  ipm;           // PxSSTS.IPM（1 = 活跃）
    uint8_t  kind;          // Ahci64Kind
    uint32_t sig;           // PxSIG
    char     model[41];     // kind=ata/atapi 时的型号（否则空串）
    uint64_t sectors;       // kind=ata 时的总扇区数（LBA48 优先，512B/扇区）
};

// 控制器摘要（给硬件检查报告页 / 诊断用；未初始化时 found=0）
struct Ahci64CtrlInfo {
    int      found;         // 1 = 找到控制器并初始化成功
    uint8_t  bus, dev, fn;  // PCI 位置
    uint64_t abar;          // BAR5（ABAR，MMIO 物理地址）
    uint32_t cap;           // CAP
    uint32_t pi;            // PI（端口位图）
    uint32_t vs;            // VS（版本）
    int      ports;         // CAP.NP + 1（实现端口数）
    int      list_count;    // 登记的端口数
    Ahci64PortInfo list[32];
};

// 初始化（幂等）：PCI 找 class 0x01/0x06/prog-if 0x01 -> ABAR -> 端口/设备探测/IDENTIFY。
// 没控制器时只打一行 "[AHCI64] not found" 并保持 count=0（优雅降级，不影响 PATA 路径）。
void ahci64_init64();

// AHCI 上可用的 **ATA 盘**数（ATAPI 不计入；0 = 没有控制器 / 没插盘）
int  ahci64_count64();

// idx = 盘序号 0..count64()-1，对应**统一驱动器号** 8+idx（见 ata64.h）
bool ahci64_info64(int idx, DiskInfo* out);

// 读/写：lba = 绝对 LBA（48 位），count ≤ 65536 扇区（内部按 128 扇区分块）
bool ahci64_read64 (int idx, uint32_t lba, uint32_t count, void* buf);
bool ahci64_write64(int idx, uint32_t lba, uint32_t count, const void* buf);

// 只读自检：对每块 ATA 盘读 LBA 0（**不写盘**）+ 校验 IDENTIFY 结果；打 [AHCI64] 行。
// 没有控制器时返回 true 但打 "skipped (no controller)"（与 e1000/usb 的 skipped 口径一致）。
bool ahci64_selftest64();

// 控制器/端口摘要（报告页只读；永远非空）
const Ahci64CtrlInfo* ahci64_ctrl64();

// ==================== ★ v0.9.26：AHCI 侧 ATAPI（SATA 光驱）读 ====================
// 到 v0.9.25 为止，本驱动对 PxSIG = 0xEB140101 的端口**只识别不读**（打点里也这么写）。
// 本批补上 PACKET(0xA0) + 12 字节 CDB 的读路径：数据走 DMA（一条命令一个 2048B 光盘块），
// 传输长度由 CDB 决定（H2D FIS 的 count 写 0、features bit0 = 1 = DMA）。
//   语义与 ata64 的光驱段**完全一致**：idx 是"第几个 AHCI 光驱"（0 起），
//   lba/sectors 单位 = 2048B 光盘块；上层拿到的驱动器号 = ATA64_CD_AHCI_BASE + idx。
//   没控制器 / 没 ATAPI 端口时 count = 0（调用方按"没有光驱"处理，不崩）。
int  ahci64_cd_count64();
bool ahci64_cd_info64(int idx, DiskInfo* out);
bool ahci64_cd_read64(int idx, uint32_t cd_lba, uint32_t sectors, void* buf);
bool ahci64_cd_capacity64(int idx, uint32_t* out_sectors, uint32_t* out_bytes);

// ==================== ★ v0.9.33：零拷贝散列读（给 ring3 驱动框架用）====================
// 把盘上数据**直接 DMA 到调用方给的物理帧**，全程不做 memcpy。这些帧通常来自
// proc64_shm_create64 建的共享对象（proc64_shm_view64 能取到帧号数组），
// 并已 proc64_shm_map64 映射进 ring3 驱动服务的地址空间 —— 这就是第 3 条通道的落点。
//   * idx   = AHCI 盘序号（0 起；驱动器号 = ATA64_AHCI_BASE + idx）
//   * lba   = 绝对 LBA；count = 扇区数（≤ nframes*8）
//   * frames[] = 物理帧号数组；一帧一项 PRDT，一条命令最多 16 帧（64 KiB = 128 扇区）
//   * out_bytes = 实际落地的字节数（失败时是已完成的部分，调用方据此判"半截"）
// 返回 true = 全部落地；false = 参数非法 / 目标帧不够 / 控制器报错（已打点）。
bool ahci64_read_frames64(int idx, uint32_t lba, uint32_t count,
                          const uint64_t* frames, uint32_t nframes,
                          uint64_t* out_bytes);
