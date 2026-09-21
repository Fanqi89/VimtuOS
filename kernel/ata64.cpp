// ata64.cpp - Vimtu64 精简 ATA PIO 驱动（安装程序专用）
//
// 端口序列与 boot/loader64.asm 的保护模式读盘一致（那套已在两个模拟器上验过）：
//   * 只做 PIO；等 DRQ / 等写完成默认由 **IRQ14 中断唤醒**（hlt 循环 + 完成标志），
//     IRQ 不可用/超时（从通道、IF=0、平台不投递）自动回退到轮询交替状态寄存器 0x3F6
//     （注意：读主状态 0x1F7 会清掉 pending 中断，中断路径里正好用它清设备中断请求）
//   * 每个扇区等一次 DRQ，再搬 256 个字；读/写命令收尾都等一次"完成中断"
//   * 写盘用 0x30 (WRITE SECTORS) + 每个扇区等 DRQ 后写 256 个字，最后等 BSY/DRQ 清零
// ★ 驱动器号分派（见 ata64.h 的统一驱动器号）：8..15 = AHCI(SATA) 盘（ahci64.*）、
//   16.. = NVMe 命名空间（nvme64.*）—— 上层（setup64/part64/vfs64/store64）一行都不用改。
#include "ata64.h"
#include "ahci64.h"     // ★ item 5a：驱动器号 8..15 时把识别/读写分派到 AHCI(SATA)
#include "nvme64.h"     // ★ item 6：驱动器号 16.. 时把识别/读写分派到 NVMe
#include "port.h"
#include "debug64.h"
#include "x86_64.h"      // pic_unmask64 / g_ticks64：IRQ14 等待与超时计时
#include "hwinfo64.h"    // ★ 批次 B：IDENTIFY 成功后把型号/容量填进 hwinfo64 的磁盘表

// ---------------- 端口基址 ----------------
static inline uint16_t base_port(int drive) {
    return (drive & 2) ? 0x170 : 0x1F0;          // 2,3 = secondary
}
static inline uint16_t ctrl_port(int drive) {
    return (drive & 2) ? 0x376 : 0x3F6;          // 交替状态寄存器
}

// ==================== IRQ14（中断驱动等待 + 超时回退轮询）====================
// 机制：
//   1) 命令发出前 ata64_irq_arm()：关中断清"完成标志"，保证不会把上一条命令的 IRQ
//      当成这一条的完成信号；
//   2) IRQ14 到达：irq14_handler() 置完成标志 + 读主状态寄存器（清设备中断请求）留快照；
//   3) 等待方 ata_wait_drq_irq()：hlt 循环检查标志，等到后读状态判定 DRQ/ERR；
//      超时（IRQ 被屏蔽/设备不投递/从通道/IF=0）自动回退原 PIO 轮询，把这次等待计入
//      polled，并只打印一次告警 —— 绝不挂死、绝不误判成功。
#define ATA64_IRQ_TIMEOUT_TICKS 40                  // 40 * 4ms = 160ms（PIT 250Hz）
#define ATA64_IRQ_STATUS_PORT   0x1F7               // 主通道主状态寄存器（读它清设备中断请求）

volatile uint32_t ata64_irq14_irqs   = 0;           // IRQ14 到达次数
volatile uint32_t ata64_irq14_reads  = 0;           // 由中断唤醒完成的等待次数
volatile uint32_t ata64_irq14_polled = 0;           // 超时回退到轮询的等待次数

static volatile uint8_t g_ata64_irq_pending  = 0;   // 1 = 有一条主通道命令的完成中断
static volatile uint8_t g_ata64_irq_status   = 0;   // 中断时读到的状态寄存器快照（诊断用）
static volatile uint8_t g_ata64_irq_enabled  = 0;   // ata64_init64() 成功后为 1
static volatile uint8_t g_ata64_irq_timeout_logged = 0;

static inline bool ata64_irqs_on() {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200ULL) != 0;                    // RFLAGS.IF
}

// 命令发出前调用：清完成标志。关中断做，避免"清"与"中断置位"互相覆盖。
static inline void ata64_irq_arm(int drive) {
    if (!g_ata64_irq_enabled || (drive & 2)) return;
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    g_ata64_irq_pending = 0;
    if (fl & 0x200ULL) __asm__ volatile("sti" ::: "memory");
}

extern "C" void irq14_handler() {
    g_ata64_irq_status  = inb(ATA64_IRQ_STATUS_PORT);   // 读主状态：清设备中断请求 + 留快照
    g_ata64_irq_pending = 1;
    ata64_irq14_irqs++;
}

static inline uint8_t drive_select(int drive) {
    // 0xE0 = bit7(1) + bit6(LBA=1) + bit5(1) + bit4(DRV: 0=主盘/1=从盘)
    // ★ 踩坑记录：这里原来写的是 0xA0（bit6=0 → CHS 模式）。IDENTIFY 不看这一位，
    //   所以"识别正常"，但随后的 READ/WRITE 会被设备以 ABRT（err=0x04）中止。
    //   必须用 0xE0 才是 LBA 模式。
    return (uint8_t)(0xE0 | ((drive & 1) ? 0x10 : 0x00));
}

// 约 400ns 延时：连读 4 次交替状态
static void ata_delay(int drive) {
    for (int i = 0; i < 4; i++) (void)inb(ctrl_port(drive));
}

// 等 BSY=0：返回 false 表示超时（无设备）
static bool ata_wait_ready(int drive) {
    const uint16_t io = base_port(drive);
    for (uint32_t spin = 0; spin < 1000000; spin++) {
        const uint8_t st = inb((uint16_t)(io + 7));
        if (st == 0) return false;                    // 无设备：状态总线浮空
        if (!(st & 0x80)) return true;                 // BSY=0
    }
    return false;
}

// 等 DRQ（数据就绪）
static bool ata_wait_drq(int drive) {
    const uint16_t io = base_port(drive);
    ata64_dbg_polls = 0;
    for (uint32_t spin = 0; spin < 2000000; spin++) {
        const uint8_t st = inb((uint16_t)(io + 7));
        ata64_dbg_polls++;
        if (st & 0x01) { ata64_dbg_reason = 2; return false; }   // ERR
        if (st & 0x08) return true;                              // DRQ
    }
    ata64_dbg_reason = 3;                                        // DRQ 超时
    return false;
}

// 状态快查：1=DRQ 就绪、-1=ERR、0=还没好。读主状态寄存器会清设备中断请求。
static inline int ata64_check_drq64(int drive) {
    const uint8_t st = inb((uint16_t)(base_port(drive) + 7));
    ata64_dbg_polls = 0;
    if (st & 0x01) { ata64_dbg_reason = 2; return -1; }
    if (st & 0x08) return 1;
    return 0;
}

// 等 DRQ：优先由 IRQ14 唤醒；不适用（从通道/中断关闭/IF=0）或超时就回退到上面的轮询。
// polled 计数 + 只告警一次；回退后就是旧驱动逻辑，绝不挂死、绝不误判成功。
//
// ★ 快速路径：先看一眼状态。原因：上一条命令的完成中断可能与本条命令的中断"折叠"成
//   一次（完成标志只有一个），只看标志会白等一个 160ms 超时窗口。状态里已经有 DRQ/ERR
//   就直接收工 —— 这不是轮询循环，只是一次非阻塞检查。
static bool ata_wait_drq_irq(int drive) {
    if (!g_ata64_irq_enabled || (drive & 2) || !ata64_irqs_on()) {
        return ata_wait_drq(drive);                    // 老实的轮询：行为与改动前一致
    }
    {
        const int quick = ata64_check_drq64(drive);
        if (quick) {
            g_ata64_irq_pending = 0;
            if (quick > 0) { ata64_irq14_reads++; return true; }
            return false;
        }
    }
    const uint64_t t0 = g_ticks64;
    for (;;) {
        if (g_ata64_irq_pending) {
            g_ata64_irq_pending = 0;
            const int r = ata64_check_drq64(drive);    // 读主状态：清设备中断请求
            if (r) { if (r > 0) { ata64_irq14_reads++; return true; } return false; }
            // 既非 DRQ 也非 ERR（BSY 未落/伪中断）：继续等，超时后回退
        }
        if (g_ticks64 - t0 > ATA64_IRQ_TIMEOUT_TICKS) {
            ata64_irq14_polled++;
            if (!g_ata64_irq_timeout_logged) {
                g_ata64_irq_timeout_logged = 1;        // 只打印一次，避免刷屏
                dbg64_str("[ATA64] irq14 timeout -> fallback to polling");
                dbg64_nl();
            }
            return ata_wait_drq(drive);                // 回退：原 PIO 轮询
        }
        __asm__ volatile("hlt");                       // 让出 CPU，等 IRQ14/PIT 唤醒
    }
}

// 等命令完成（BSY=0 且 DRQ=0）：读/写两条路径的收尾都走它。同样带状态快查快速路径。
static bool ata_wait_done_irq(int drive) {
    const uint16_t io = base_port(drive);
    if (g_ata64_irq_enabled && !(drive & 2) && ata64_irqs_on()) {
        {
            const uint8_t st = inb((uint16_t)(io + 7));
            if (st & 0x01) return false;
            if (!(st & 0x80) && !(st & 0x08)) {
                g_ata64_irq_pending = 0; ata64_irq14_reads++; return true;
            }
        }
        const uint64_t t0 = g_ticks64;
        for (;;) {
            if (g_ata64_irq_pending) {
                g_ata64_irq_pending = 0;
                const uint8_t st = inb((uint16_t)(io + 7));
                if (st & 0x01) return false;
                if (!(st & 0x80) && !(st & 0x08)) { ata64_irq14_reads++; return true; }
            }
            if (g_ticks64 - t0 > ATA64_IRQ_TIMEOUT_TICKS) {
                ata64_irq14_polled++;
                if (!g_ata64_irq_timeout_logged) {
                    g_ata64_irq_timeout_logged = 1;
                    dbg64_str("[ATA64] irq14 timeout -> fallback to polling");
                    dbg64_nl();
                }
                break;                                 // 回退：原 PIO 轮询
            }
            __asm__ volatile("hlt");
        }
    }
    for (uint32_t spin = 0; spin < 2000000; spin++) {  // 原轮询逻辑（回退路径）
        const uint8_t st = inb((uint16_t)(io + 7));
        if (st & 0x01) return false;
        if (!(st & 0x80) && !(st & 0x08)) return true;
    }
    return false;
}

// ---------------- 枚举接口（统一驱动器号，见 ata64.h）----------------
// 上层（setup64 / part64）用它枚举，不要自己写 for (d=0; d<4; d++)：
//   槽 0..3 -> 驱动器号 0..3（PATA）；其后 -> 8,9,...（AHCI 盘）；再后 -> 16,17,...（NVMe 命名空间）
// ★ 顺序有讲究：先 PATA、再 AHCI、最后 NVMe —— 与"驱动器号从小往大"一致，界面行序稳定。
int ata64_drive_count64() { return 4 + ahci64_count64() + nvme64_count64(); }

int ata64_slot_to_drive64(int slot) {
    if (slot < 0) return -1;
    if (slot < 4) return slot;
    const int n = ahci64_count64();
    const int i = slot - 4;
    if (i < n) return ATA64_AHCI_BASE + i;      // AHCI 盘：驱动器号 8..
    const int m = nvme64_count64();
    const int j = i - n;
    if (j < m) return ATA64_NVME_BASE + j;      // NVMe 命名空间：驱动器号 16..
    return -1;                                  // 越界（槽数比实际盘多时）
}

// ---------------- IDENTIFY ----------------
// 分派：≥ ATA64_NVME_BASE 走 NVMe、≥ ATA64_AHCI_BASE 走 AHCI（两者都复用同一份 DiskInfo）。
bool ata64_identify(int drive, DiskInfo* out) {
    out->present = false;
    out->atapi = false;
    out->sectors = 0;
    out->model[0] = 0;
    if (drive >= ATA64_NVME_BASE) return nvme64_info64(drive - ATA64_NVME_BASE, out);
    if (drive >= ATA64_AHCI_BASE) return ahci64_info64(drive - ATA64_AHCI_BASE, out);
    if (drive < 0 || drive > 3) return false;   // 4..7 保留空洞：不映射任何设备
    const uint16_t io = base_port(drive);
    outb((uint16_t)(io + 6), drive_select(drive));
    ata_delay(drive);
    outb((uint16_t)(io + 2), 0);
    outb((uint16_t)(io + 3), 0);
    outb((uint16_t)(io + 4), 0);
    outb((uint16_t)(io + 5), 0);
    outb((uint16_t)(io + 7), 0xEC);                    // IDENTIFY DEVICE

    uint8_t st = inb((uint16_t)(io + 7));
    if (st == 0) return false;                         // 无设备
    if (!ata_wait_ready(drive)) return false;
    // ATAPI：IDENTIFY DEVICE 之后是 0x14 0xEB 签名（LBA mid/high 非零）
    const uint8_t mid = inb((uint16_t)(io + 4));
    const uint8_t hi  = inb((uint16_t)(io + 5));
    if (mid == 0x14 && hi == 0xEB) {
        // ★ 是 ATAPI：改走 IDENTIFY PACKET DEVICE(0xA1) 的正确路径。
        //   为什么不能"探测到签名就完事"：0xEC 对光驱是非法命令，设备会进入 ERR 状态，
        //   之后它可能**拒不接受 PACKET 命令**（实测：读载荷时卡在等 DRQ）。
        //   走 0xA1 既拿到型号，又把设备状态清干净。
        return ata64_atapi_identify(drive, out);
    }
    if (!ata_wait_drq(drive)) return false;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io);

    out->present = true;
    out->atapi = false;
    // 型号：word 27..46，按大端字节序
    for (int i = 0; i < 20; i++) {
        out->model[i * 2]     = (char)(id[27 + i] >> 8);
        out->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out->model[40] = 0;
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--) out->model[i] = 0;   // 去尾空格
    out->sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
    if (out->sectors == 0) {
        // 老盘用 word 57/58（CHS）；这里不换算，直接视为未知
        out->sectors = 0;
    }
    // ★ 批次 B：IDENTIFY 的**最终结果**填进 hwinfo64（任务管理器性能页/设置页的磁盘型号与容量
    //   就是从那里读的）。两点如实口径：
    //     * sectors==0（没给 LBA28 容量）时不填 —— 宁可不显示，也不写"0 容量"；
    //     * lba48 传 0：本驱动的读写路径只用 LBA28（见 ata64.h 的限制），不假装支持 LBA48。
    //   超时保护不在这里加：本函数每一步都走 ata_wait_ready/ata_wait_drq 的有界等待（带超时回退）。
    if (out->sectors > 0 && drive >= 0 && drive < (int)HW64_DISK_MAX) {
        hwinfo_set_disk64(drive, out->model, (uint64_t)out->sectors, 0);
    }
    return true;
}

// ==================== PATA PIO 分块（单条命令 ≤128 扇区）+ 每块重试 ====================
// ★ 为什么必须有这一层（批次 K 之后补的真缺陷，已在 128MB/64MB PATA 目标盘上复现）：
//   PATA 的扇区计数寄存器（io+2，0x1F2/0x172）只有 **8 位**：0..255，其中 0 表示 256。
//   fat64 写 48MB ESP 时 write_fats 一条命令要写 g_fatsz = 768 个扇区：768 & 0xFF = 0
//   -> 设备按"256 个扇区"执行，搬完就结束命令；主机却还在等第 257 个扇区的 DRQ ——
//   先撞 IRQ14 的 160ms 超时回退轮询，再轮询 200 万次后放弃。实测症状：
//   `[FAT64] FAIL fat-write` + `[ATA64] irq14 timeout -> fallback to polling`；
//   128MB 与 64MB PATA 目标盘**都**必失败，而 AHCI 目标盘全容量都正常（ahci64 内部
//   本来就按 128 扇区分块、CFIS 的计数字段是 16 位）→ 根因在**接口**，不在容量。
//   修法：把"一条 PATA 命令"封成 *_once（≤128 扇区），对外仍是 count 不限的接口：
//   自动分块 + 每块最多 ATA64_PIO_RETRY 次重试。128 扇区 = 64KB，与 ahci64 的分块、
//   part64 安装载荷的块大小一致（安装器在 PATA 盘上实测走通的正是这条 128 扇区路径）。
//   为什么这样不会再失败：每条命令的计数值都落在 8 位寄存器可表示的范围（1..128），
//   设备与主机的"还剩多少个扇区"始终一致；偶发失败也只重发当前这一块，语义不变。
static const uint32_t ATA64_PIO_MAX_SECTORS = 128;
static const int      ATA64_PIO_RETRY       = 3;

// ---------------- READ（单条 PATA PIO 命令；count 1..128）----------------
// ★ 不变量：本函数只允许 1..128 —— 由 ata64_read 的分块循环保证（见上面的总说明）。
static bool ata_pio_read_once(int drive, uint32_t lba, uint32_t count, void* buf) {
    const uint16_t io = base_port(drive);
    // ★ 踩坑记录：必须**先选盘再等状态**。状态寄存器反映的是"当前选中的驱动器"，
    //   而前一次操作（枚举循环/另一个驱动器的命令）会把别的驱动器留在选中状态：
    //   如果那个驱动器不存在，读到的状态是 0x00，会被误判成"这个驱动器没设备"。
    //   实测症状：光盘引导下目标盘是 0 号盘、1 号位空 → 新建分区时报"读 LBA0 失败"。
    outb((uint16_t)(io + 6), (uint8_t)(drive_select(drive) | ((lba >> 24) & 0x0F)));
    ata_delay(drive);
    if (!ata_wait_ready(drive)) { ata64_dbg_reason = 1; return false; }
    outb((uint16_t)(io + 1), 0);                       // 特性寄存器
    outb((uint16_t)(io + 2), (uint8_t)count);
    outb((uint16_t)(io + 3), (uint8_t)(lba & 0xFF));
    outb((uint16_t)(io + 4), (uint8_t)((lba >> 8) & 0xFF));
    outb((uint16_t)(io + 5), (uint8_t)((lba >> 16) & 0xFF));
    ata64_irq_arm(drive);                              // 清完成标志（避免 stale IRQ 误判）
    outb((uint16_t)(io + 7), 0x20);                    // READ SECTORS

    // 诊断：命令发出后立刻采 8 次状态，看设备到底进了哪个状态
    // （0x80=BSY, 0x40=RDY, 0x20=DF, 0x08=DRQ, 0x01=ERR）
    for (int i = 0; i < 8; i++) {
        ata64_dbg_st[i] = inb((uint16_t)(io + 7));
        for (volatile int k = 0; k < 2000; k++) { }
    }
    ata64_dbg_err = inb((uint16_t)(io + 1));           // 错误寄存器

    uint16_t* p = (uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq_irq(drive)) {
            ata64_dbg_err = inb((uint16_t)(io + 1));
            return false;
        }
        for (int i = 0; i < 256; i++) *p++ = inw(io);
    }

    // 读命令收尾：等完成中断（BSY=0 且 DRQ=0）再返回 —— 不把"完成 IRQ"留给下一条命令。
    // 否则两条命令的中断会被"完成标志只有一个"折叠成一次，白等一个超时窗口（实测踩过）。
    if (!ata_wait_done_irq(drive)) return false;
    return true;
}

// READ 对外入口：分派 + 分块 + 每块重试（语义与改动前一致；>128 扇区不再被 8 位寄存器截断）
bool ata64_read(int drive, uint32_t lba, uint32_t count, void* buf) {
    // ★ 分派：驱动器号 16.. -> NVMe 命名空间读（内部按 128 扇区分块）；
    //          8..15 -> AHCI(SATA) DMA 读（LBA48）
    if (drive >= ATA64_NVME_BASE) return nvme64_read64(drive - ATA64_NVME_BASE, lba, count, buf);
    if (drive >= ATA64_AHCI_BASE) return ahci64_read64(drive - ATA64_AHCI_BASE, lba, count, buf);
    if (drive < 0 || drive > 3) return false;   // 4..7 保留空洞
    if (count == 0) return true;
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > ATA64_PIO_MAX_SECTORS) n = ATA64_PIO_MAX_SECTORS;
        bool ok = false;
        for (int t = 0; t < ATA64_PIO_RETRY && !ok; t++) {
            ok = ata_pio_read_once(drive, lba + done, n, p + (uint32_t)done * 512u);
        }
        if (!ok) return false;
        done += n;
    }
    return true;
}

// ---------------- WRITE（单条 PATA PIO 命令；count 1..128）----------------
// ★ 不变量：本函数只允许 1..128 —— 由 ata64_write 的分块循环保证（见上面的总说明）。
static bool ata_pio_write_once(int drive, uint32_t lba, uint32_t count, const void* buf) {
    const uint16_t io = base_port(drive);
    // 同 ata64_read：先选盘再等状态
    outb((uint16_t)(io + 6), (uint8_t)(drive_select(drive) | ((lba >> 24) & 0x0F)));
    ata_delay(drive);
    if (!ata_wait_ready(drive)) { ata64_dbg_reason = 1; return false; }
    outb((uint16_t)(io + 1), 0);
    outb((uint16_t)(io + 2), (uint8_t)count);
    outb((uint16_t)(io + 3), (uint8_t)(lba & 0xFF));
    outb((uint16_t)(io + 4), (uint8_t)((lba >> 8) & 0xFF));
    outb((uint16_t)(io + 5), (uint8_t)((lba >> 16) & 0xFF));
    ata64_irq_arm(drive);                              // 清完成标志（避免 stale IRQ 误判）
    outb((uint16_t)(io + 7), 0x30);                    // WRITE SECTORS

    const uint16_t* p = (const uint16_t*)buf;
    for (uint32_t s = 0; s < count; s++) {
        if (!ata_wait_drq_irq(drive)) return false;
        for (int i = 0; i < 256; i++) outw(io, *p++);
        ata_delay(drive);
        // 最后一个扇区之后由下面统一等 flush
    }
    // 等写完成（BSY=0 且 DRQ=0）
    if (!ata_wait_done_irq(drive)) return false;   // 中断唤醒；超时自动回退轮询
    return true;
}

// WRITE 对外入口：分派 + 分块 + 每块重试（语义与改动前一致；>128 扇区不再被 8 位寄存器截断）
bool ata64_write(int drive, uint32_t lba, uint32_t count, const void* buf) {
    // ★ 分派同 ata64_read：16.. -> NVMe、8..15 -> AHCI(SATA) DMA 写
    if (drive >= ATA64_NVME_BASE) return nvme64_write64(drive - ATA64_NVME_BASE, lba, count, buf);
    if (drive >= ATA64_AHCI_BASE) return ahci64_write64(drive - ATA64_AHCI_BASE, lba, count, buf);
    if (drive < 0 || drive > 3) return false;   // 4..7 保留空洞
    if (count == 0) return true;
    const uint8_t* p = (const uint8_t*)buf;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > ATA64_PIO_MAX_SECTORS) n = ATA64_PIO_MAX_SECTORS;
        bool ok = false;
        for (int t = 0; t < ATA64_PIO_RETRY && !ok; t++) {
            ok = ata_pio_write_once(drive, lba + done, n, p + (uint32_t)done * 512u);
        }
        if (!ok) return false;
        done += n;
    }
    return true;
}

bool ata64_read_sector (int drive, uint32_t lba, void* buf)             { return ata64_read(drive, lba, 1, buf); }

// ==================== 读路径诊断记录 ====================
// 读失败时把"看到的状态序列"和错误寄存器留下来，供上层打印。
// （踩坑记录：不加这个只能看到"读失败"三个字，无法区分 ERR / 超时 / 状态不对。）
volatile uint8_t ata64_dbg_st[8];
volatile uint8_t ata64_dbg_err = 0;
volatile uint32_t ata64_dbg_polls = 0;
volatile uint8_t ata64_dbg_reason = 0;   // 1=wait_ready 失败 2=ERR 3=DRQ 超时
bool ata64_write_sector(int drive, uint32_t lba, const void* buf)        { return ata64_write(drive, lba, 1, buf); }

// ==================== 诊断：把 4 个驱动器的原始寄存器值打出来 ====================
// 用途：识别失败时区分"设备不存在" / "端口序列不对" / "轮询逻辑有问题"。
void ata64_diag() {
    for (int d = 0; d < 4; d++) {
        const uint16_t io = base_port(d);
        const uint8_t st_idle = inb((uint16_t)(io + 7));

        outb((uint16_t)(io + 6), drive_select(d));
        ata_delay(d);
        const uint8_t st_sel = inb((uint16_t)(io + 7));

        outb((uint16_t)(io + 2), 0);
        outb((uint16_t)(io + 3), 0);
        outb((uint16_t)(io + 4), 0);
        outb((uint16_t)(io + 5), 0);
        outb((uint16_t)(io + 7), 0xEC);
        const uint8_t st_cmd = inb((uint16_t)(io + 7));

        for (uint32_t i = 0; i < 200000; i++) {
            if (!(inb((uint16_t)(io + 7)) & 0x80)) break;
        }
        const uint8_t st_done = inb((uint16_t)(io + 7));
        const uint8_t mid = inb((uint16_t)(io + 4));
        const uint8_t hi  = inb((uint16_t)(io + 5));

        dbg64_str("[ATA] drive="); dbg64_dec(d);
        dbg64_str(" io=0x");      dbg64_hex64(io);
        dbg64_str(" st_idle=");   dbg64_hex64(st_idle);
        dbg64_str(" st_sel=");    dbg64_hex64(st_sel);
        dbg64_str(" st_cmd=");    dbg64_hex64(st_cmd);
        dbg64_str(" st_done=");   dbg64_hex64(st_done);
        dbg64_str(" mid=");       dbg64_hex64(mid);
        dbg64_str(" hi=");        dbg64_hex64(hi);
        dbg64_nl();
    }
}

// ==================== ATAPI（光驱）====================
// 现场记录（出错时打印）
volatile uint8_t  ata64_cd_stage = 0;   // 1=选盘 2=写字节计数 3=发 PACKET 4=写 CDB 5=读数据 6=收尾
volatile uint8_t  ata64_cd_err = 0;
volatile uint8_t  ata64_cd_st = 0;

// ATAPI 的驱动器选择字节用 0xA0/0xB0（bit6 那位对 ATAPI 无意义，
// 但实测设备对 0xE0 的接受度不统一，按规范用 0xA0/0xB0 最稳）
static inline uint8_t atapi_drive_select(int drive) {
    return (uint8_t)(0xA0 | ((drive & 1) ? 0x10 : 0x00));
}

// 等 BSY=0 且 DRQ=0（发 PACKET 之前的必检条件）
static bool atapi_wait_ready_no_drq(int drive) {
    const uint16_t io = base_port(drive);
    for (uint32_t spin = 0; spin < 2000000; spin++) {
        const uint8_t st = inb((uint16_t)(io + 7));
        ata64_cd_st = st;
        if (st & 0x01) { ata64_cd_stage = 0xA1; ata64_cd_err = inb((uint16_t)(io + 1)); return false; }
        if (!(st & 0x80) && !(st & 0x08)) return true;   // BSY=0 DRQ=0
    }
    ata64_cd_stage = 0xA2;
    return false;
}

// 发一个 PACKET 命令：cdb 12 字节，期望从数据口读 bytes 字节（PIO）
static bool atapi_packet(int drive, const uint8_t* cdb, void* buf, uint32_t bytes) {
    const uint16_t io = base_port(drive);

    // ★ 先选盘再等状态（状态寄存器反映"当前选中的驱动器"；枚举循环会把空的光驱
    //   从盘留在选中状态，此时读到的 0x00 会被误判成"没设备"——实测踩过）。
    outb((uint16_t)(io + 6), atapi_drive_select(drive));
    ata_delay(drive);
    ata64_cd_stage = 1;
    if (!ata_wait_ready(drive)) { ata64_cd_stage = 0xE1; return false; }

    // 字节计数上限（PIO 传输长度，低/高 8 位在 0x1F4/0x1F5）
    ata64_cd_stage = 2;
    outb((uint16_t)(io + 1), 0);                      // 特性寄存器清零（PACKET 的 overlap 位）
    outb((uint16_t)(io + 4), (uint8_t)(bytes & 0xFF));
    outb((uint16_t)(io + 5), (uint8_t)((bytes >> 8) & 0xFF));
    if (!atapi_wait_ready_no_drq(drive)) return false;

    // 发 PACKET 命令，并立刻采状态做诊断（能区分"当场被 ABRT"还是"慢慢超时"）
    ata64_cd_stage = 3;
    outb((uint16_t)(io + 7), 0xA0);
    const uint8_t s0 = inb((uint16_t)(io + 7));
    const uint8_t s1 = inb((uint16_t)(io + 7));
    const uint8_t s2 = inb((uint16_t)(io + 7));
    dbg64_str("[CD] PACKET 后 st0=0x");
    dbg64_hex64(s0);
    dbg64_str(" st1=0x");
    dbg64_hex64(s1);
    dbg64_str(" st2=0x");
    dbg64_hex64(s2);
    dbg64_str(" lba_lo=0x");
    dbg64_hex64(inb((uint16_t)(io + 3)));
    dbg64_nl();
    if (!ata_wait_drq(drive)) {
        ata64_cd_stage = 0xE3;
        ata64_cd_st = inb((uint16_t)(io + 7));        // 记下真实状态（bit0=ERR）
        ata64_cd_err = inb((uint16_t)(io + 1));
        dbg64_str("[CD] PACKET 后等 DRQ 失败 st=0x");
        dbg64_hex64(ata64_cd_st);
        dbg64_str(" err=0x");
        dbg64_hex64(ata64_cd_err);
        dbg64_str(" feat=0x");
        dbg64_hex64(inb((uint16_t)(io + 1)));
        dbg64_str(" cyllo=0x");
        dbg64_hex64(inb((uint16_t)(io + 4)));
        dbg64_str(" cylhi=0x");
        dbg64_hex64(inb((uint16_t)(io + 5)));
        dbg64_nl();
        return false;
    }

    // 写 12 字节 CDB（6 个字，小端）
    ata64_cd_stage = 4;
    for (int i = 0; i < 6; i++) {
        const uint16_t w = (uint16_t)cdb[i * 2] | (uint16_t)((uint16_t)cdb[i * 2 + 1] << 8);
        outw(io, w);
    }

    // 收数据：每轮 DRQ 搬 512 字节
    ata64_cd_stage = 5;
    uint16_t* p = (uint16_t*)buf;
    uint32_t left = bytes;
    while (left > 0) {
        if (!ata_wait_drq(drive)) { ata64_cd_stage = 0xE5; ata64_cd_err = inb((uint16_t)(io + 1)); return false; }
        uint32_t chunk = (left < 512) ? left : 512;
        for (uint32_t i = 0; i < chunk / 2; i++) *p++ = inw(io);
        left -= chunk;
        ata_delay(drive);
    }

    // 收尾：等 BSY=0 且 DRQ=0（命令真正结束）
    ata64_cd_stage = 6;
    for (uint32_t spin = 0; spin < 2000000; spin++) {
        const uint8_t st = inb((uint16_t)(io + 7));
        ata64_cd_st = st;
        if (st & 0x01) { ata64_cd_stage = 0xE6; ata64_cd_err = inb((uint16_t)(io + 1)); return false; }
        if (!(st & 0x80) && !(st & 0x08)) { ata64_cd_stage = 0; return true; }
    }
    ata64_cd_stage = 0xE7;
    return false;
}

bool ata64_atapi_read(int drive, uint32_t cd_lba, uint32_t sectors, void* buf) {
    uint8_t* out = (uint8_t*)buf;
    for (uint32_t s = 0; s < sectors; s++) {
        // READ(10)：CDB[0]=0x28, LBA 在 [2..5] 大端, 传输长度在 [7..8] 大端
        const uint32_t lba = cd_lba + s;
        uint8_t cdb[12];
        cdb[0] = 0x28;
        cdb[1] = 0;
        cdb[2] = (uint8_t)((lba >> 24) & 0xFF);
        cdb[3] = (uint8_t)((lba >> 16) & 0xFF);
        cdb[4] = (uint8_t)((lba >> 8) & 0xFF);
        cdb[5] = (uint8_t)(lba & 0xFF);
        cdb[6] = 0;
        cdb[7] = 0;            // 传输长度 1 个扇区（高字节）
        cdb[8] = 1;            // 低字节
        cdb[9] = 0; cdb[10] = 0; cdb[11] = 0;
        if (!atapi_packet(drive, cdb, out, 2048)) {
            // ★ 踩坑记录（QEMU 实测）：别的驱动器上的命令会把 IDE 状态机留在
            //   不利状态，随后 PACKET 可能被当场 ABRT（st=0x41 / err=0x04）。
            //   重发一次 IDENTIFY PACKET DEVICE 归位设备状态，再重试同一扇区。
            DiskInfo tmp;
            const bool reid = ata64_atapi_identify(drive, &tmp);
            if (reid && atapi_packet(drive, cdb, out, 2048)) {
                dbg64_str("[CD] 重新识别后恢复读取 lba=");
                dbg64_dec(lba);
                dbg64_nl();
                out += 2048;
                continue;
            }
            dbg64_str("[CD] ATAPI 读失败 lba=");
            dbg64_dec(lba);
            dbg64_str(" stage=0x");
            dbg64_hex64(ata64_cd_stage);
            dbg64_str(" err=0x");
            dbg64_hex64(ata64_cd_err);
            dbg64_str(" st=0x");
            dbg64_hex64(ata64_cd_st);
            dbg64_nl();
            return false;
        }
        out += 2048;
    }
    return true;
}

bool ata64_atapi_identify(int drive, DiskInfo* out) {
    out->present = false;
    out->atapi = false;
    out->sectors = 0;
    out->model[0] = 0;

    const uint16_t io = base_port(drive);
    // ★ 踩坑记录：必须**先选盘再看状态**。状态寄存器反映的是"当前选中的驱动器"，
    //   选盘前读到的 0x00（总线浮空）会被当成"这个驱动器号没有设备"，于是
    //   整个 ATAPI 识别在第一步就返回了 —— 实测就是这样错过光驱的。
    outb((uint16_t)(io + 6), atapi_drive_select(drive));
    ata_delay(drive);
    if (!ata_wait_ready(drive)) return false;

    outb((uint16_t)(io + 2), 0);
    outb((uint16_t)(io + 3), 0);
    outb((uint16_t)(io + 4), 0);
    outb((uint16_t)(io + 5), 0);
    outb((uint16_t)(io + 7), 0xA1);                 // IDENTIFY PACKET DEVICE

    if (!ata_wait_ready(drive)) return false;       // 等 BSY 落
    uint8_t st = inb((uint16_t)(io + 7));
    // 判据：硬盘收到 0xA1 会 ABRT（ERR=1，且没有数据）；ATAPI 会准备好 256 字数据。
    // （不要用 LBA mid/high 的 0x14/0xEB 签名判断：进入数据相位后那两个端口读的是数据。）
    if (st & 0x01) return false;
    if (!(st & 0x08) && !ata_wait_drq(drive)) return false;

    uint16_t id[256];
    for (int i = 0; i < 256; i++) id[i] = inw(io);
    // 数据必须全部读走（否则设备会认为传输未完成，下一条命令报 ABRT）

    out->present = true;
    out->atapi = true;
    for (int i = 0; i < 20; i++) {
        out->model[i * 2]     = (char)(id[27 + i] >> 8);
        out->model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    out->model[40] = 0;
    for (int i = 39; i >= 0 && out->model[i] == ' '; i--) out->model[i] = 0;
    out->sectors = 0;                                // 光驱容量靠 READ CAPACITY，这里不查
    return true;
}

bool ata64_atapi_selftest(int* out_drive) {
    static uint8_t sec[2048];
    for (int d = 0; d < 4; d++) {
        DiskInfo di;
        if (!ata64_atapi_identify(d, &di) || !di.atapi) continue;
        // ISO9660 主卷描述符在光盘 LBA 16，开头是 01 'C' 'D' '0' '0' '1'
        if (!ata64_atapi_read(d, 16, 1, sec)) {
            dbg64_str("[CD] ATAPI 存在但读 LBA16 失败 drive=");
            dbg64_dec(d);
            dbg64_nl();
            continue;
        }
        const bool ok = (sec[0] == 0x01 && sec[1] == 'C' && sec[2] == 'D' &&
                         sec[3] == '0' && sec[4] == '0' && sec[5] == '1');
        dbg64_str("[CD] atapi drive=");
        dbg64_dec(d);
        dbg64_str(" model=");
        dbg64_str(di.model);
        dbg64_str(" pvd=");
        for (int i = 0; i < 6; i++) {
            const char* H = "0123456789ABCDEF";
            char h[3] = { H[(sec[i] >> 4) & 0xF], H[sec[i] & 0xF], 0 };
            dbg64_str(h);
        }
        dbg64_str(ok ? " ok" : " BAD");
        dbg64_nl();
        if (ok) {
            if (out_drive) *out_drive = d;
            return true;
        }
    }
    dbg64_str("[CD] 没有可用的 ATAPI 光盘（下面是原始状态诊断）");
    dbg64_nl();
    for (int d = 0; d < 4; d++) ata64_atapi_diag(d);
    return false;
}

// ---- 诊断：把 ATAPI 选盘/识别每一步的原始状态打出来 ----
// 用途：自检失败时区分"设备压根不在这个驱动器号" / "0xA1 被拒绝" / "签名不对"。
void ata64_atapi_diag(int drive) {
    const uint16_t io = base_port(drive);
    const uint8_t st0 = inb((uint16_t)(io + 7));           // 选盘前
    outb((uint16_t)(io + 6), atapi_drive_select(drive));   // 选盘
    ata_delay(drive);
    const uint8_t st1 = inb((uint16_t)(io + 7));           // 选盘后
    outb((uint16_t)(io + 2), 0);
    outb((uint16_t)(io + 3), 0);
    outb((uint16_t)(io + 4), 0);
    outb((uint16_t)(io + 5), 0);
    outb((uint16_t)(io + 7), 0xA1);                        // IDENTIFY PACKET DEVICE
    const uint8_t st2 = inb((uint16_t)(io + 7));           // 命令发出后
    uint32_t spins = 0;
    while ((inb((uint16_t)(io + 7)) & 0x80) && spins < 2000000) spins++;
    const uint8_t st3 = inb((uint16_t)(io + 7));
    const uint8_t err = inb((uint16_t)(io + 1));
    const uint8_t mid = inb((uint16_t)(io + 4));
    const uint8_t hi  = inb((uint16_t)(io + 5));
    // 把可能的数据读掉，避免污染后续命令
    if (st3 & 0x08) for (int i = 0; i < 256; i++) (void)inw(io);
    dbg64_str("[CD] diag d="); dbg64_dec(drive);
    dbg64_str(" sel=0x");  dbg64_hex64(atapi_drive_select(drive));
    dbg64_str(" st0=0x");  dbg64_hex64(st0);
    dbg64_str(" st1=0x");  dbg64_hex64(st1);
    dbg64_str(" st2=0x");  dbg64_hex64(st2);
    dbg64_str(" st3=0x");  dbg64_hex64(st3);
    dbg64_str(" err=0x");  dbg64_hex64(err);
    dbg64_str(" mid=0x");  dbg64_hex64(mid);
    dbg64_str(" hi=0x");   dbg64_hex64(hi);
    dbg64_str(" spins=");  dbg64_dec(spins);
    dbg64_nl();
}

// ==================== 初始化（PATA IRQ14 + AHCI + NVMe）+ 自检 ====================
// 为什么放驱动里做：安装程序用 ATA，系统内核的 VFS/store 也用 ATA；两份内核的启动
// 路径各调用一次 ata64_init64()（kernel64.cpp 的 os_boot_path / setup64.cpp 的 setup64_run）。
//
// 设备控制寄存器（0x3F6/0x376）bit1 = nIEN：0 = 允许设备拉中断线。引导层只读过它、
// 没写过，这里显式清 0，保证 IRQ14 一定会被拉起来。
//
// ★ item 5a / item 6：这里同时把 AHCI(SATA) 与 NVMe 拉起来（两个 init 都幂等；没有控制器
//   只打一行 not found 就返回，PATA 路径完全不受影响）。**顺序有讲究**：
//   两个控制器都先初始化 —— 上层的磁盘枚举（setup64 的 enumerate_disks / part64 的形状探测）
//   靠 ata64_drive_count64() = 4 + ahci64_count64() + nvme64_count64() 决定要枚举几个槽，
//   枚举必须看到最终结果，不能"先枚举完再发现还有 SATA/NVMe 盘"。
void ata64_init64() {
    ahci64_init64();                                    // 幂等：没有控制器 -> not found（优雅降级）
    (void)ahci64_selftest64();                          // 只读：每块 AHCI 盘读 LBA0（没盘/没控制器 -> skipped）
    nvme64_init64();                                    // 幂等：没有控制器 -> [NVME64] not found
    (void)nvme64_selftest64();                          // 只读：第一个命名空间读 LBA0（没盘/没控制器 -> skipped）
    if (g_ata64_irq_enabled) return;                    // 幂等
    outb(ctrl_port(0), 0x00);                           // 主通道：nIEN=0
    outb(ctrl_port(2), 0x00);                           // 从通道同样放开（IRQ15 未挂入口，仅取一致）
    pic_unmask64(14);
    g_ata64_irq_enabled = 1;
    dbg64_str("[ATA64] irq14 enabled (irq-driven waits, polling fallback)");
    dbg64_nl();
    (void)ata64_irq_selftest64();                       // 只读：读 LBA0，验证中断确实到达
}

// 自检：主通道第一个 ATA 硬盘读 LBA 0（**只读，不写盘**）。
// PASS 条件 = 数据读回成功 且 irqs>0（IRQ14 确实到达）；拿不到主通道硬盘 -> skipped。
// FAIL mask：bit0=读失败、bit1=中断没到达（走了轮询回退）。
bool ata64_irq_selftest64() {
    int drive = -1;
    DiskInfo di;
    for (int d = 0; d < 2; d++) {                       // 只查主通道：IRQ14 覆盖 drive 0/1
        if (ata64_identify(d, &di) && di.present && !di.atapi) { drive = d; break; }
    }
    if (drive < 0) {
        dbg64_str("[ATA64] irq14 selftest skipped (no drive)");
        dbg64_nl();
        return false;
    }

    const uint32_t reads0  = ata64_irq14_reads;
    const uint32_t irqs0   = ata64_irq14_irqs;
    const uint32_t polled0 = ata64_irq14_polled;

    static uint8_t sec[512];
    const bool ok = ata64_read_sector(drive, 0, sec);   // 只读 LBA0

    const uint32_t reads  = ata64_irq14_reads  - reads0;
    const uint32_t irqs   = ata64_irq14_irqs   - irqs0;
    const uint32_t polled = ata64_irq14_polled - polled0;

    dbg64_str("[ATA64] irq14 selftest reads=");
    dbg64_dec(reads);
    dbg64_str(" irqs=");
    dbg64_dec(irqs);
    dbg64_str(" polled=");
    dbg64_dec(polled);
    const int mask = (ok ? 0 : 1) | (irqs > 0 ? 0 : 2);
    dbg64_str(mask == 0 ? " status=ok" : " status=fail");
    dbg64_nl();

    if (mask == 0) {
        dbg64_str("[ATA64] irq14 selftest PASS");
        dbg64_nl();
        return true;
    }
    dbg64_str("[ATA64] irq14 selftest FAIL mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_nl();
    return false;
}
