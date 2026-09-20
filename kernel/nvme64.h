// nvme64.h - VimtuOS 最小 NVMe 驱动（轮询式）—— 真机可用性套件 item 6
//
// 定位与边界（先读，避免误期待）：
//   * **单控制器、单 I/O 队列对、单命名空间（NSID 1）**、**全程轮询**（INTMS 屏蔽所有中断，
//     与 AHCI 同款做法）。多队列 / 中断 / MSI-X / PRP 列表 > 18 项 / 多命名空间都不做。
//   * 传输上限：一条命令 ≤ 128 扇区（64KB），PRP1 +（两页时 PRP2 直接指针 / 三页以上 PRP 列表）搬运；
//     更大的请求由驱动**内部按 128 扇区分块**，上层（part64/fat64/vfs64/store64）不用管。
//   * 只支持 **512B 逻辑块**的命名空间（此时 NSZE 就是 512B 扇区数，与 DiskInfo 口径一致）；
//     别的 LBA 尺寸（如 4KB）如实打点并**不给驱动器号**，绝不假装能用。
//   * 与 PATA/AHCI 的关系：驱动器号 **16..** 分派到这里（见 ata64.h 的 ATA64_NVME_BASE），
//     上层（setup64 / part64 / vfs64 / store64 / fat64）一行都不用改。
//
// 串口打点（验收按行 grep，改前先想清楚）：
//   [NVME64] not found                                        （没控制器：优雅降级，不崩）
//   [NVME64] pci <b>:<d>.<f> bar0=<hex> cap=<hex> vs=<hex>
//   [NVME64] cc=<hex> csts=<hex> rdy=1
//   [NVME64] ctrl model=<...> sn=<...>
//   [NVME64] nsid=1 lba_bytes=512 sectors=<n>
//   [NVME64] queue sq=<hex> cq=<hex> qd=<n>
//   [NVME64] read lba=<n> count=<n> ok                       （失败：... failed sc=<n>）
//   [NVME64] write lba=<n> count=<n> ok
//   [NVME64] timeout stage=<...> qid=<n>
//   [NVME64] selftest PASS / FAIL mask=<n> / skipped (...)
// selftest FAIL mask：1=控制器未就绪 2=IDENTIFY NAMESPACE/容量非法 4=I/O 队列创建失败
//                     8=读回失败 16=控制器型号非法
#pragma once
#include <stdint.h>
#include "ata64.h"      // DiskInfo（与 PATA/AHCI 共用同一份结构）

// 最多登记几个命名空间：驱动器号 = ATA64_NVME_BASE + idx。
// 必须与 ata64.h 的 ATA64_MAX_DRIVE64（= ATA64_NVME_BASE + 8）一致。
#define NVME64_MAX_NS   8
// 队列深度（admin 与 I/O 各一组同深度）：8 项 = SQ 8×64B + CQ 8×16B，各占一页足够
#define NVME64_QD       8

// 控制器/队列/命名空间摘要（硬件检查报告页与诊断用；未初始化时 found=0）
struct Nvme64CtrlInfo {
    int      found;              // 1 = PCI 找到控制器且 BAR0 有效
    int      ready;              // 1 = 控制器已使能（CSTS.RDY=1）且 admin 队列可用
    int      io_ready;           // 1 = I/O 队列对（qid=1）已创建
    uint8_t  bus, dev, fn;       // PCI 位置
    uint64_t bar0;               // BAR0（64 位 MMIO 控制器寄存器基址）
    uint64_t cap;                // CAP
    uint32_t vs;                 // VS（控制器版本）
    uint32_t cc, csts;           // 使能后的 CC / CSTS 快照
    uint32_t dstrd;              // CAP.DSTRD（门铃步长 = 4 << DSTRD）
    uint32_t qd;                 // 队列深度（项数）
    char     model[41];          // 控制器型号（Identify Controller 的 MN，已去尾空格）
    char     serial[21];         // 控制器序列号（SN）
    int      ns_count;           // 可用命名空间数（= nvme64_count64()）
    uint32_t nsid[NVME64_MAX_NS];
    uint32_t lba_bytes[NVME64_MAX_NS];
    uint64_t ns_sectors[NVME64_MAX_NS];
    uint32_t timeouts;           // 累计超时次数
    const char* last_timeout;    // 最后一次超时的阶段名（0 = 从未超时）
};

// 初始化（幂等）：PCI 找 class 0x01 / subclass 0x08 / prog-if 0x02 -> BAR0(64 位 MMIO)
//   -> CSTS.RDY=0 -> INTMS 全屏蔽 -> AQA -> ASQ/ACQ -> CC（CSS=0/MPS=0/IOSQES=6/IOCQES=4/EN=1）
//   -> CSTS.RDY=1 -> Identify Controller / Identify Namespace(1) / Create I/O CQ+SQ。
// 没控制器时只打一行 "[NVME64] not found"（优雅降级，不影响任何既有测试）。
// 所有等待都有界（g_ticks64 + 自旋上限）：超时打点并如实失败返回，绝不挂死。
void nvme64_init64();

// 可用命名空间数（0 = 没控制器 / 控制器没就绪 / 没有可用的 512B 命名空间）
int  nvme64_count64();

// idx = 命名空间序号 0..count64()-1，对应**统一驱动器号** ATA64_NVME_BASE + idx（见 ata64.h）
bool nvme64_info64(int idx, DiskInfo* out);

// 读/写：lba = 512B 扇区号（uint32 上限与 PATA/AHCI 侧一致），count 任意（内部按 128 扇区分块）
bool nvme64_read64 (int idx, uint32_t lba, uint32_t count, void* buf);
bool nvme64_write64(int idx, uint32_t lba, uint32_t count, const void* buf);

// 只读自检：对第一个命名空间读 LBA 0（**不写盘**）+ 校验型号/容量。打 [NVME64] 行。
// 没控制器/没就绪/没命名空间时返回 true 但打 "skipped (...)"（与 ahci64/usb/e1000 的 skipped 口径一致）。
bool nvme64_selftest64();

// 控制器摘要（报告页只读；永远非空）
const Nvme64CtrlInfo* nvme64_ctrl64();
