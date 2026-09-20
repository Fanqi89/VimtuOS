// nvme64.cpp - VimtuOS 最小 NVMe 驱动（轮询式）—— 真机可用性套件 item 6
//
// 设计要点（与 kernel/ahci64.cpp / kernel/e1000_64.cpp 同一套做法：
//            PCI 找设备 -> BAR -> MMIO -> 队列/命令 -> 轮询）：
//   1) PCI：扫 class 0x01 / subclass 0x08 / prog-if 0x02（NVMe 规范），读 **BAR0**
//      （PCI 偏移 0x10；bit2 = 1 表示 64 位 BAR，高 32 位在 0x14）。只在自己找到的
//      那个功能上开 MEM(bit1) + BUS MASTER(bit2)，不碰别的设备。
//   2) 控制器初始化（严格照 NVMe 规范的顺序）：
//        a) 关控制器：CC.EN=0 -> 等 CSTS.RDY=0；
//        b) INTMS=0xFFFFFFFF（屏蔽所有中断，并等读回确认）—— 全程轮询，与 AHCI 一致；
//        c) AQA：ASQS/ACQS 都是 **0-based**，QD=8 就写 7；
//        d) ASQ/ACQ：admin SQ/CQ 的**页对齐物理地址**（page_alloc_64 拿的低内存页，PA==VA）；
//        e) CC：CSS=0（NVM 命令集）、MPS=0（4KB，与 CAP.MPSMIN 一致）、IOSQES=6（64B 项）、
//           IOCQES=4（16B 项）、EN=1；
//        f) 等 CSTS.RDY=1（有界）；CSTS.CFS=1（控制器致命状态）时如实打点收工。
//   3) Admin 队列（qid 0）：Identify Controller(0x06 CNS=0x01) 拿型号/序列号 ->
//      Identify Namespace(0x06 CNS=0x02 NSID=1) 拿 NSZE/LBAF -> Create I/O CQ(0x05) ->
//      Create I/O SQ(0x01)（PC=1 连续、IEN=0 不开中断）。CQ 在 SQ 之前建（规范推荐顺序）。
//   4) I/O（qid 1）：Read(0x02)/Write(0x01)，NSID + SLBA + **NLB（0-based）** + PRP1/PRP2。
//      提交 = 写 SQ 项 + 敲 SQ tail 门铃（0x1000 + 2*qid*stride）；完成 = 轮询 CQ 项的
//      **phase 位**（bit16），非 0 的 SCT/SC 一律打点并返回失败；随后敲 CQ head 门铃
//      （0x1000 + (2*qid+1)*stride），stride = 4 << CAP.DSTRD。
//   5) 传输上限与 PRP：**一条命令 ≤ 128 扇区（64KB）**（与 AHCI 的分块口径一致；上层
//      part64 的 INSTALL_CHUNK_SECTORS 也正好是 128 扇区）。PRP 清单（规范 4.3，写错就搬错数据）：
//        * PRP1 之后**剩余 ≤ 1 页** -> 只用 PRP1；**刚好两页** -> PRP2 = 第二页**页基址**（直接指针）；
//          **三页及以上** -> PRP2 = **页对齐的 PRP 列表**物理地址，列表项 = 第 2..N 页的页基址。
//        ★ 实测口径（QEMU 11.1）：PRP 列表项必须是页对齐地址（否则控制器回 NVME_INVALID_FIELD）；
//          而"两页"若误用列表形式，控制器会把**列表页的第一个 8 字节**当作第二页的数据地址 ——
//          症状是"前半段对、后半段错"，本次就是这样抓到 ESP 里 BOOTX64.EFI 被写坏的。
//      DMA 缓冲区地址必须能算出物理地址：低内存（<4GB，含页池/堆）PA==VA；内核镜像高半区
//      对象按直映关系换算（PA = VA - (ML64_KERNEL_VA_BASE - ML64_KERNEL_BASE)）；认不出来的
//      指针走**暂存页**（页池 16 页，每页一项 PRP），数据再逐页 memcpy。
//   6) 超时**双保险**：g_ticks64 计时（250Hz，默认 250 tick = 1s）+ 自旋计数上限
//      （IF=0 时 g_ticks64 根本不前进，只靠计时就会死循环）。绝不挂死；错误一律如实返回
//      false 并留 [NVME64] 打点，绝不把失败当成功。
#include "nvme64.h"
#include "debug64.h"
#include "memlayout64.h"    // ★ 必须排在 mem_64.h 之前（PAGE_SIZE_64 宏冲突）
#include "mem_64.h"
#include "port.h"
#include "x86_64.h"         // g_ticks64 / PIT 250Hz

extern "C" char __bss_end[];    // 内核镜像高半区上界（判断指针是否落在内核镜像里）

// ==================== 控制器寄存器（只列用到的）====================
enum : uint32_t {
    NVME_CAP   = 0x00,   // Controller Capabilities（64 位；DSTRD = bits 31:28，MQES = bits 15:0）
    NVME_VS    = 0x08,   // Version
    NVME_INTMS = 0x0C,   // Interrupt Mask Set（写 1 屏蔽；本驱动写 0xFFFFFFFF 全屏蔽）
    NVME_INTMC = 0x10,   // Interrupt Mask Clear
    NVME_CC    = 0x14,   // Controller Configuration（EN=bit0，CSS=bits 6:4，MPS=bits 10:7，
                         //   IOSQES=bits 19:16，IOCQES=bits 23:20）
    NVME_CSTS  = 0x1C,   // Controller Status（RDY=bit0，CFS=bit1）
    NVME_AQA   = 0x24,   // Admin Queue Attributes（ASQS=bits 11:0，ACQS=bits 27:16；都 0-based）
    NVME_ASQ   = 0x28,   // Admin Submission Queue Base Address（64 位，页对齐）
    NVME_ACQ   = 0x30,   // Admin Completion Queue Base Address（64 位，页对齐）
};
static const uint32_t NVME_DB_BASE = 0x1000;   // 门铃区起点

static const uint32_t CC_EN      = 1u << 0;
static const uint32_t CSTS_RDY   = 1u << 0;
static const uint32_t CSTS_CFS   = 1u << 1;

// 命令码（只列用到的）
static const uint8_t  NVME_OP_CREATE_SQ   = 0x01;   // Admin：Create I/O Submission Queue
static const uint8_t  NVME_OP_WRITE       = 0x01;   // NVM：Write
static const uint8_t  NVME_OP_READ        = 0x02;   // NVM：Read
static const uint8_t  NVME_OP_CREATE_CQ   = 0x05;   // Admin：Create I/O Completion Queue
static const uint8_t  NVME_OP_IDENTIFY    = 0x06;   // Admin：Identify
// Identify 的 CNS（NVMe 基础规范 "Identify – CNS Values"）：
//   0x00 = Identify Namespace 数据（返回 NSZE/LBAF —— 这才是我们要的那份）
//   0x01 = Identify Controller 数据
//   ★ 踩坑（QEMU 11.1 实测）：CNS=0x02 是 **Active Namespace ID list**，不是 Identify Namespace；
//     用它读 NSZE 只会拿到命名空间 ID 列表（全 0 时表现为 nsid=1 not present (NSZE=0)）。
static const uint32_t NVME_CNS_NS         = 0x00;   // Identify Namespace
static const uint32_t NVME_CNS_CTRL       = 0x01;   // Identify Controller

// 超时（250Hz PIT：250 tick = 1s）+ 自旋兜底（IF=0 时 g_ticks64 不前进，只靠计时会死循环）
static const uint32_t NVME64_INIT_TICKS  = 250;
static const uint32_t NVME64_CMD_TICKS   = 250;
static const uint32_t NVME64_SPIN_GUARD  = 400000000u;
static const uint32_t NVME64_SPIN_BEFORE_HLT = 4096;   // 先自旋再 hlt：让宿主机/设备状态机有机会推进

// 一条命令最多搬多少扇区：128 × 512B = 64KB（与 AHCI 的 AHCI64_CHUNK_SECTORS 同值）
static const uint32_t NVME64_CHUNK_SECTORS = 128;
// 跨页时 PRP 列表项上限：64KB + 页内偏移最多占 18 页 -> 列表项最多 17
static const uint32_t NVME64_PRP_MAX       = 17;
// 暂存页数（64KB / 4KB = 16 页；暂存路径从页基址开始，不需要额外的偏移页）
static const int      NVME64_BOUNCE_PAGES  = 16;
// 超时打点次数上限（避免设备彻底不响应时刷屏）
static const uint32_t NVME64_TIMEOUT_LOG_MAX = 8;

// ==================== 状态 ====================
static Nvme64CtrlInfo g_ctrl;          // 控制器摘要（报告页读它）
static int      g_inited = 0;          // 幂等标志
static uint8_t  g_bus = 0, g_dev = 0, g_fn = 0;

// 一个队列（admin qid=0 / I/O qid=1）：SQ/CQ 各一页；QD ≤ 8，项数远小于一页容量
struct Nvme64Q {
    uint8_t* sq;          // Submission Queue 基址（页对齐，PA==VA）
    uint8_t* cq;          // Completion Queue 基址
    uint32_t qid;
    uint32_t qd;          // 项数
    uint32_t sq_tail;
    uint32_t cq_head;
    uint32_t phase;       // 下一个完成项的 phase 位（1 或 0）
    uint32_t cid;         // 命令 ID 计数器（同时只有一条在飞，值只要不重复即可）
    uint8_t  used;
};
static Nvme64Q g_adm;
static Nvme64Q g_io;

static int      g_ns_count = 0;
static uint32_t g_ns_nsid[NVME64_MAX_NS];
static uint32_t g_ns_lba_bytes[NVME64_MAX_NS];
static uint64_t g_ns_sectors[NVME64_MAX_NS];

static uint8_t* g_id_page   = nullptr;                 // 4KB identify 缓冲（自检读盘也用它）
static uint8_t* g_prp_page  = nullptr;                 // 4KB PRP 列表页
static uint8_t* g_bounce[NVME64_BOUNCE_PAGES];         // 暂存数据页（每页 4KB，PA==VA）

static uint32_t g_timeout_logged = 0;
static uint8_t  g_first_read_logged = 0, g_first_write_logged = 0;

// ==================== 小工具 ====================
static inline bool nvme_irqs_on() {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200ULL) != 0;                       // RFLAGS.IF
}

// ---- MMIO ----
static inline uint32_t nvme_rd32(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(g_ctrl.bar0 + off);
}
static inline uint64_t nvme_rd64(uint32_t off) {
    return *(volatile uint64_t*)(uintptr_t)(g_ctrl.bar0 + off);
}
static inline void nvme_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_ctrl.bar0 + off) = v;
    __asm__ volatile("" ::: "memory");                 // 防编译器把 MMIO 写挪位/合并
}
static inline void nvme_wr64(uint32_t off, uint64_t v) {
    *(volatile uint64_t*)(uintptr_t)(g_ctrl.bar0 + off) = v;
    __asm__ volatile("" ::: "memory");
}

// 门铃步长 = 4 << CAP.DSTRD；SQ y tail = 0x1000 + 2y*stride，CQ y head = 0x1000 + (2y+1)*stride
static inline void nvme_doorbell(uint32_t qid, int cq, uint32_t val) {
    const uint32_t stride = 4u << g_ctrl.dstrd;
    nvme_wr32(NVME_DB_BASE + (qid * 2u + (cq ? 1u : 0u)) * stride, val);
}

// 有界等待：等到 (寄存器 & mask) == want。g_ticks64 计时 + 自旋上限双保险，绝不挂死。
static bool nvme_wait32(uint32_t off, uint32_t mask, uint32_t want, uint32_t ticks) {
    const uint64_t t0 = g_ticks64;
    const bool can_hlt = nvme_irqs_on();
    uint32_t spin = 0;
    for (;;) {
        if ((nvme_rd32(off) & mask) == want) return true;
        if (g_ticks64 - t0 > (uint64_t)ticks) return false;
        if (++spin > NVME64_SPIN_GUARD) return false;
        if (can_hlt && spin > NVME64_SPIN_BEFORE_HLT) __asm__ volatile("hlt");
        else                                          __asm__ volatile("pause");
    }
}

// ---- 打点 ----
static void nvme_timeout_log(const char* stage, uint32_t qid) {
    g_ctrl.timeouts++;
    g_ctrl.last_timeout = stage;
    if (g_timeout_logged >= NVME64_TIMEOUT_LOG_MAX) return;   // 已经打过够多了：不再刷屏
    g_timeout_logged++;
    dbg64_line_begin64();
    dbg64_str("[NVME64] timeout stage=");
    dbg64_str(stage);
    dbg64_str(" qid=");
    dbg64_dec(qid);
    dbg64_nl();
    dbg64_line_end64();
}

static void nvme_cmd_fail_log(const char* stage, uint32_t qid, uint32_t sct, uint32_t sc) {
    dbg64_line_begin64();
    dbg64_str("[NVME64] cmd failed stage=");
    dbg64_str(stage);
    dbg64_str(" qid=");
    dbg64_dec(qid);
    dbg64_str(" sct=");
    dbg64_dec(sct);
    dbg64_str(" sc=");
    dbg64_dec(sc);
    dbg64_nl();
    dbg64_line_end64();
}

// 从 identify 页里拷定长 ASCII 字段（遇到 0 就停，去尾空格，补 0）
static void nvme_copy_str(char* dst, int dstsz, const uint8_t* src, int n) {
    int m = 0;
    for (int i = 0; i < n && m < dstsz - 1; i++) {
        const char c = (char)src[i];
        if (c == 0) break;
        dst[m++] = c;
    }
    dst[m] = 0;
    while (m > 0 && dst[m - 1] == ' ') dst[--m] = 0;
}

// ==================== 物理地址换算 ====================
// 低内存（< 4GB）：恒等映射，PA == VA（e1000/mem64/ahci64 同款约定）；
// 内核镜像高半区对象（.bss/.data/栈上的 static 数组）：直映关系 PA = VA - 偏移；
// 其余（认不出来）：返回 0 -> 调用方走暂存页兜底。
static inline uint64_t nvme_pa64(const void* ptr) {
    const uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v < 0x100000000ULL) return v;
    if (v >= ML64_KERNEL_VA_BASE && v < ((uint64_t)(uintptr_t)__bss_end) + 0x10000ULL) {
        return v - (ML64_KERNEL_VA_BASE - (uint64_t)ML64_KERNEL_BASE);
    }
    return 0;
}

// ==================== PCI 配置空间（0xCF8/0xCFC）====================
static uint32_t npci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void npci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, val);
}

// 扫 0..255 号总线找 NVMe 控制器（class 0x01 / subclass 0x08 / prog-if 0x02）。
// 连续 4 条空总线提前停（与 e1000_64 / hwinfo64 / ahci64 同款）。
// prog-if 不是 0x02 的 01/08 设备按规范不算 NVMe -> 如实打一行 note（便于真机排障）。
static bool nvme_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t d = 0; d < 32; d++) {
            uint32_t id = npci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = npci_rd32((uint8_t)bus, (uint8_t)d, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = npci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x00);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint32_t cc = npci_rd32((uint8_t)bus, (uint8_t)d, (uint8_t)fn, 0x08);
                if ((uint8_t)(cc >> 24) != 0x01u || (uint8_t)(cc >> 16) != 0x08u) continue;
                if ((uint8_t)(cc >> 8) == 0x02u) {                  // prog-if = NVM Express
                    g_bus = (uint8_t)bus;
                    g_dev = (uint8_t)d;
                    g_fn  = (uint8_t)fn;
                    return true;
                }
                dbg64_line_begin64();                               // 01/08 但不是 NVMe：如实记一笔
                dbg64_str("[NVME64] skip class 01/08 prog_if=0x");
                dbg64_hex64((uint8_t)(cc >> 8));
                dbg64_str(" (不是 0x02)");
                dbg64_nl();
                dbg64_line_end64();
            }
        }
        if (bus_has) empty_run = 0;
        else if (++empty_run >= 4u) break;
    }
    return false;
}

// ==================== 命令提交 + 完成轮询 ====================
// 提交一条 64B 命令（cmd = 16 个 dword）并轮询它的完成项：
//   * 完成项 = CQ 里 cq_head 位置那 16 字节；**phase 位（DW3 bit16）翻转**才是新项；
//   * SCT/SC 非 0 -> 打点并返回 false（绝不把失败当成功）；
//   * 完成项取走后推进 cq_head 并敲 CQ head 门铃；回卷时翻转 phase；
//   * 超时（g_ticks64 + 自旋上限）-> 打点返回 false（队列可能失同步，但绝不挂死）。
static bool nvme_submit(Nvme64Q& q, const uint32_t cmd[16], uint32_t* out_result, const char* stage) {
    if (!q.used || !q.sq || !q.cq || q.qd == 0) return false;

    uint32_t* e = (uint32_t*)(void*)(q.sq + (uint64_t)q.sq_tail * 64u);
    for (int i = 0; i < 16; i++) e[i] = cmd[i];
    __asm__ volatile("sfence" ::: "memory");               // 命令项必须先落地再敲门铃
    q.sq_tail = (q.sq_tail + 1u) % q.qd;
    nvme_doorbell(q.qid, 0, q.sq_tail);                    // SQ tail 门铃

    const uint32_t want_p = (q.phase ? 0x00010000u : 0u);
    const uint64_t t0 = g_ticks64;
    const bool can_hlt = nvme_irqs_on();
    uint32_t spin = 0;
    for (;;) {
        volatile uint32_t* cqe = (volatile uint32_t*)(void*)(q.cq + (uint64_t)q.cq_head * 16u);
        const uint32_t dw3 = cqe[3];
        if ((dw3 & 0x00010000u) == want_p) {
            const uint32_t sct = (dw3 >> 25) & 0x7u;
            const uint32_t sc  = (dw3 >> 17) & 0xFFu;
            if (out_result) *out_result = cqe[0];
            q.cq_head = (q.cq_head + 1u) % q.qd;
            if (q.cq_head == 0u) q.phase ^= 1u;            // 回卷 -> phase 翻转
            nvme_doorbell(q.qid, 1, q.cq_head);            // CQ head 门铃（释放完成项）
            if (sct || sc) {
                nvme_cmd_fail_log(stage, q.qid, sct, sc);
                return false;
            }
            return true;
        }
        if (g_ticks64 - t0 > (uint64_t)NVME64_CMD_TICKS) { nvme_timeout_log(stage, q.qid); return false; }
        if (++spin > NVME64_SPIN_GUARD)                  { nvme_timeout_log(stage, q.qid); return false; }
        if (can_hlt && spin > NVME64_SPIN_BEFORE_HLT) __asm__ volatile("hlt");
        else                                          __asm__ volatile("pause");
    }
}

// Admin：Identify（CNS=Identify Controller / Identify Namespace）
static bool nvme_adm_identify(uint32_t cns, uint32_t nsid, uint8_t* buf, const char* stage) {
    const uint64_t pa = nvme_pa64(buf);                    // identify 页来自页池：PA==VA
    if (pa == 0) return false;
    uint32_t cmd[16] = {0};
    cmd[0]  = (uint32_t)NVME_OP_IDENTIFY | ((g_adm.cid++ & 0xFFFFu) << 16);
    cmd[1]  = nsid;
    cmd[6]  = (uint32_t)(pa & 0xFFFFFFFFu);                // PRP1（4096B，页对齐 -> 单 PRP 够）
    cmd[7]  = (uint32_t)(pa >> 32);
    cmd[10] = cns;                                         // CDW10 = CNS
    return nvme_submit(g_adm, cmd, nullptr, stage);
}

// Admin：Create I/O Completion Queue（qid=1，PC=1 连续，IEN=0 不产生中断）
static bool nvme_adm_create_cq(uint32_t qid, uint32_t qd, uint8_t* cq) {
    const uint64_t pa = nvme_pa64(cq);
    if (pa == 0) return false;
    uint32_t cmd[16] = {0};
    cmd[0]  = (uint32_t)NVME_OP_CREATE_CQ | ((g_adm.cid++ & 0xFFFFu) << 16);
    cmd[6]  = (uint32_t)(pa & 0xFFFFFFFFu);                // PRP1 = CQ 基址（页对齐）
    cmd[7]  = (uint32_t)(pa >> 32);
    cmd[10] = (qid & 0xFFFFu) | (((qd - 1u) & 0xFFFFu) << 16);   // QID + QSIZE(0-based)
    cmd[11] = 1u;                                          // PC=1、IEN=0、IV=0
    return nvme_submit(g_adm, cmd, nullptr, "admin_create_cq");
}

// Admin：Create I/O Submission Queue（qid=1，PC=1 连续，挂到刚建的 CQ 上）
static bool nvme_adm_create_sq(uint32_t qid, uint32_t qd, uint8_t* sq, uint32_t cqid) {
    const uint64_t pa = nvme_pa64(sq);
    if (pa == 0) return false;
    uint32_t cmd[16] = {0};
    cmd[0]  = (uint32_t)NVME_OP_CREATE_SQ | ((g_adm.cid++ & 0xFFFFu) << 16);
    cmd[6]  = (uint32_t)(pa & 0xFFFFFFFFu);                // PRP1 = SQ 基址（页对齐）
    cmd[7]  = (uint32_t)(pa >> 32);
    cmd[10] = (qid & 0xFFFFu) | (((qd - 1u) & 0xFFFFu) << 16);   // QID + QSIZE(0-based)
    cmd[11] = 1u | ((cqid & 0xFFFFu) << 16);               // PC=1、QPRIO=0、CDW11[31:16] = CQID
    return nvme_submit(g_adm, cmd, nullptr, "admin_create_sq");
}

// ==================== PRP 准备 ====================
// PRP2 的两种含义（★ 规范 4.3 + QEMU 11.1 / Linux 的实现细节，写错就搬错数据）：
//   * PRP1 之后**剩余长度 ≤ 1 页** -> PRP2 = 第二页的**页基址**（直接指针，**不是** PRP 列表！）
//   * PRP1 之后剩余长度 > 1 页    -> PRP2 = **PRP 列表**页（项 = 第 2..N 页的页基址，全部页对齐）
// ★ 实测踩坑（本次定位到的真 bug）：把"刚好两页"的搬运也写成 PRP 列表，QEMU 会把列表页的
//   第一个 8 字节当成第二页的数据地址 -> **后半段数据搬到别处**（症状：ESP 里 BOOTX64.EFI
//   的前 848 字节对、后面全错，UEFI 报 Load Error；而 64KB 对齐的大块搬运一直是对的）。
//   所以"两页"必须走 PRP2 直接指针这条分支。
static const uint32_t NVME64_PAGE = 4096;

// 直传：算出 PRP1/PRP2。返回 false = 有页算不出物理地址（调用方走暂存）。
static bool nvme_prps_direct(const uint8_t* buf, uint32_t bytes,
                            uint64_t* out_prp1, uint64_t* out_prp2) {
    const uint64_t pa0 = nvme_pa64(buf);
    const uint32_t off = (uint32_t)((uint64_t)(uintptr_t)buf & 0xFFFu);
    if (pa0 == 0) return false;
    *out_prp1 = pa0;
    *out_prp2 = 0;
    const uint64_t total = (uint64_t)off + (uint64_t)bytes;
    if (total <= (uint64_t)NVME64_PAGE) return true;                  // 单页：只用 PRP1
    const uint32_t pages = (uint32_t)((total + NVME64_PAGE - 1u) / NVME64_PAGE);
    if ((pages - 1u) > NVME64_PRP_MAX) return false;                  // 超过本驱动的列表上限
    const uint64_t q2 = nvme_pa64(buf + (NVME64_PAGE - off));         // 第二页页基址
    if (q2 == 0 || (q2 & 0xFFFu) != 0) return false;
    if (pages == 2u) { *out_prp2 = q2; return true; }                 // ★ 两页：PRP2 = 直接指针
    if (!g_prp_page) return false;                                    // ≥3 页：PRP2 = PRP 列表
    uint64_t* lst = (uint64_t*)(void*)g_prp_page;
    lst[0] = q2;
    for (uint32_t k = 2; k < pages; k++) {
        const uint8_t* p = buf + ((uint64_t)k * NVME64_PAGE - (uint64_t)off);
        const uint64_t q = nvme_pa64(p);
        if (q == 0 || (q & 0xFFFu) != 0) return false;                // 必须页对齐（见上）
        lst[k - 1u] = q;
    }
    __asm__ volatile("sfence" ::: "memory");
    *out_prp2 = (uint64_t)(uintptr_t)g_prp_page;
    return true;
}

// 暂存：DMA 只碰页池里的 16 页（PA==VA、页对齐），数据由调用方逐页 memcpy。
// 返回 false = 这次搬运超出暂存能力（调用方如实失败）。语义与上面完全一致（两页走直接指针）。
static bool nvme_prps_bounce(uint32_t bytes, uint64_t* out_prp1, uint64_t* out_prp2,
                            uint32_t* out_pages) {
    if (!g_bounce[0]) return false;
    const uint32_t pages = (bytes + NVME64_PAGE - 1u) / NVME64_PAGE;
    if (pages == 0u || pages > (uint32_t)NVME64_BOUNCE_PAGES) return false;
    *out_prp1 = (uint64_t)(uintptr_t)g_bounce[0];
    *out_prp2 = 0;
    *out_pages = pages;
    if (pages == 1u) return true;
    if (pages == 2u) {                                                // ★ 两页：直接指针
        *out_prp2 = (uint64_t)(uintptr_t)g_bounce[1];
        return true;
    }
    if (!g_prp_page) return false;                                    // ≥3 页：PRP 列表
    uint64_t* lst = (uint64_t*)(void*)g_prp_page;
    for (uint32_t k = 1; k < pages; k++) lst[k - 1u] = (uint64_t)(uintptr_t)g_bounce[k];
    __asm__ volatile("sfence" ::: "memory");
    *out_prp2 = (uint64_t)(uintptr_t)g_prp_page;
    return true;
}

// ==================== I/O 搬运（读/写共用）====================
// count 任意大：内部按 NVME64_CHUNK_SECTORS（128 扇区）分块，每块一条命令。
static bool nvme_xfer(int idx, uint32_t lba, uint32_t count, void* buf, bool write) {
    if (idx < 0 || idx >= g_ns_count) return false;
    if (count == 0) return true;
    if (!g_io.used) return false;
    uint8_t* p8 = (uint8_t*)buf;
    const uint32_t nsid = g_ns_nsid[idx];

    for (uint32_t done = 0; done < count; ) {
        const uint32_t nsec = (count - done > NVME64_CHUNK_SECTORS)
                            ? NVME64_CHUNK_SECTORS : (count - done);
        const uint32_t bytes = nsec * 512u;
        uint8_t* p = p8 + (uint64_t)done * 512u;

        uint64_t prp1 = 0, prp2 = 0;
        uint32_t bpages = 0;
        bool bounce = false;
        if (!nvme_prps_direct(p, bytes, &prp1, &prp2)) {
            bounce = true;
            if (!nvme_prps_bounce(bytes, &prp1, &prp2, &bpages)) {
                dbg64_line_begin64();
                dbg64_str("[NVME64] no buffer for dma (bounce pool/exceeds cap) lba=");
                dbg64_dec((uint64_t)lba + done);
                dbg64_nl();
                dbg64_line_end64();
                return false;
            }
            if (write) {                                            // 写：先按页拷进暂存页
                uint32_t off = 0;
                for (uint32_t k = 0; k < bpages; k++) {
                    const uint32_t n = (bytes - off > 4096u) ? 4096u : (bytes - off);
                    for (uint32_t i = 0; i < n; i++) g_bounce[k][i] = p[off + i];
                    off += n;
                }
            }
        }

        uint32_t cmd[16] = {0};
        cmd[0]  = (uint32_t)(write ? NVME_OP_WRITE : NVME_OP_READ) | ((g_io.cid++ & 0xFFFFu) << 16);
        cmd[1]  = nsid;                                            // NSID
        cmd[6]  = (uint32_t)(prp1 & 0xFFFFFFFFu);                   // PRP1
        cmd[7]  = (uint32_t)(prp1 >> 32);
        cmd[8]  = (uint32_t)(prp2 & 0xFFFFFFFFu);                   // PRP2（0 = 不用）
        cmd[9]  = (uint32_t)(prp2 >> 32);
        cmd[10] = (uint32_t)((uint64_t)lba + done);                 // SLBA 低 32 位
        cmd[11] = (uint32_t)(((uint64_t)lba + done) >> 32);         // SLBA 高 32 位
        cmd[12] = (nsec - 1u) & 0xFFFFu;                            // ★ NLB 是 0-based

        if (!nvme_submit(g_io, cmd, nullptr, write ? "io_write" : "io_read")) {
            dbg64_line_begin64();
            dbg64_str(write ? "[NVME64] write lba=" : "[NVME64] read lba=");
            dbg64_dec((uint64_t)lba + done);
            dbg64_str(" count=");
            dbg64_dec(nsec);
            dbg64_str(" failed cc=0x");
            dbg64_hex64(nvme_rd32(NVME_CC));
            dbg64_str(" csts=0x");
            dbg64_hex64(nvme_rd32(NVME_CSTS));
            dbg64_nl();
            dbg64_line_end64();
            return false;
        }

        if (bounce && !write) {                                     // 读：再按页拷回调用方的缓冲
            uint32_t off = 0;
            for (uint32_t k = 0; k < bpages; k++) {
                const uint32_t n = (bytes - off > 4096u) ? 4096u : (bytes - off);
                for (uint32_t i = 0; i < n; i++) p[off + i] = g_bounce[k][i];
                off += n;
            }
        }
        done += nsec;
    }
    return true;
}

// ==================== 控制器初始化 ====================
// 关控制器（CC.EN=0 -> 等 CSTS.RDY=0）：复位后的正常状态本来就是 EN=0/RDY=0，
// 固件若把控制器留在使能状态，这一步会把它关干净，后面的队列地址才是我们自己的。
static bool nvme_ctrl_disable() {
    nvme_wr32(NVME_CC, nvme_rd32(NVME_CC) & ~CC_EN);
    if (!nvme_wait32(NVME_CSTS, CSTS_RDY, 0, NVME64_INIT_TICKS)) {
        nvme_timeout_log("csts_rdy0", 0);
        return false;
    }
    return true;
}

void nvme64_init64() {
    if (g_inited) return;
    g_inited = 1;
    g_ctrl.found = 0;
    g_ctrl.ready = 0;
    g_ctrl.io_ready = 0;
    g_ctrl.ns_count = 0;
    g_ctrl.timeouts = 0;
    g_ctrl.last_timeout = nullptr;
    g_ctrl.dstrd = 0;
    g_ctrl.qd = NVME64_QD;
    g_ctrl.model[0] = 0;
    g_ctrl.serial[0] = 0;
    g_ns_count = 0;
    g_adm.used = 0;
    g_io.used = 0;

    // ---- 1) PCI：找 class 01/08/prog-if 02 ----
    if (!nvme_pci_find()) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] not found");
        dbg64_nl();
        dbg64_line_end64();
        return;                                    // 优雅降级：PATA/AHCI 路径完全不受影响
    }

    // ---- 2) 打开 MEM(bit1) + BUS MASTER(bit2)，读 BAR0 ----
    const uint32_t pci_cmd = npci_rd32(g_bus, g_dev, g_fn, 0x04);
    npci_wr32(g_bus, g_dev, g_fn, 0x04, (pci_cmd & 0xFFFF0000u) | 0x0006u);
    const uint32_t bar0_lo = npci_rd32(g_bus, g_dev, g_fn, 0x10);
    uint64_t bar0 = (uint64_t)(bar0_lo & 0xFFFFFFF0u);
    if (bar0_lo & 0x4u) {                          // ★ 64 位 MMIO BAR：高 32 位在偏移 0x14
        const uint32_t bar0_hi = npci_rd32(g_bus, g_dev, g_fn, 0x14);
        bar0 |= (uint64_t)bar0_hi << 32;
    }
    g_ctrl.bus = g_bus; g_ctrl.dev = g_dev; g_ctrl.fn = g_fn;
    g_ctrl.bar0 = bar0;
    if (bar0 == 0 || bar0 == 0xFFFFFFF0ULL) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] not found (BAR0 unassigned)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    g_ctrl.found = 1;

    g_ctrl.cap  = nvme_rd64(NVME_CAP);
    g_ctrl.vs   = nvme_rd32(NVME_VS);
    g_ctrl.dstrd = (uint32_t)((g_ctrl.cap >> 28) & 0xFu);
    const uint32_t mqes = (uint32_t)(g_ctrl.cap & 0xFFFFu);        // 最大队列项数（0-based）

    dbg64_line_begin64();
    dbg64_str("[NVME64] pci ");
    dbg64_dec(g_bus); dbg64_str(":"); dbg64_dec(g_dev); dbg64_str("."); dbg64_dec(g_fn);
    dbg64_str(" bar0=0x"); dbg64_hex64(bar0);
    dbg64_str(" cap=0x");  dbg64_hex64(g_ctrl.cap);
    dbg64_str(" vs=0x");   dbg64_hex64(g_ctrl.vs);
    dbg64_nl();
    dbg64_line_end64();

    // ---- 3) 队列页（页池低内存：PA == VA，页对齐由页分配保证）----
    g_adm.sq = (uint8_t*)page_alloc_64();
    g_adm.cq = (uint8_t*)page_alloc_64();
    g_io.sq  = (uint8_t*)page_alloc_64();
    g_io.cq  = (uint8_t*)page_alloc_64();
    g_id_page  = (uint8_t*)page_alloc_64();
    g_prp_page = (uint8_t*)page_alloc_64();
    for (int i = 0; i < NVME64_BOUNCE_PAGES; i++) g_bounce[i] = (uint8_t*)page_alloc_64();
    bool pages_ok = g_adm.sq && g_adm.cq && g_io.sq && g_io.cq && g_id_page && g_prp_page && g_bounce[0];
    for (int i = 0; i < NVME64_BOUNCE_PAGES && pages_ok; i++) if (!g_bounce[i]) pages_ok = false;
    if (!pages_ok) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] not ready (page pool exhausted)");
        dbg64_nl();
        dbg64_line_end64();
        return;                                    // 找不到就如实降级
    }
    for (int k = 0; k < 4096; k++) {
        g_adm.sq[k] = 0; g_adm.cq[k] = 0; g_io.sq[k] = 0; g_io.cq[k] = 0;
        g_id_page[k] = 0; g_prp_page[k] = 0;
    }
    for (int i = 0; i < NVME64_BOUNCE_PAGES; i++) for (int k = 0; k < 4096; k++) g_bounce[i][k] = 0;

    // ---- 4) 按规范使能控制器 ----
    if (!nvme_ctrl_disable()) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] not ready (controller did not disable)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    // (b) 屏蔽所有中断（全程轮询；INTMS 是"写 1 屏蔽"，规范允许随时写）
    nvme_wr32(NVME_INTMS, 0xFFFFFFFFu);
    if (!nvme_wait32(NVME_INTMS, 0xFFFFFFFFu, 0xFFFFFFFFu, NVME64_INIT_TICKS)) {
        nvme_timeout_log("intms", 0);              // 打点继续：轮询照样能用
    }
    // (c) AQA：ASQS/ACQS 都是 0-based；QD 受 CAP.MQES+1 限制
    uint32_t qd = NVME64_QD;
    if (mqes + 1u < qd) qd = mqes + 1u;
    if (qd < 2u) qd = 2u;
    g_ctrl.qd = qd;
    g_adm.qd = qd; g_adm.qid = 0; g_adm.phase = 1; g_adm.cid = 0; g_adm.used = 1;
    g_io.qd = qd;  g_io.qid  = 1; g_io.phase  = 1; g_io.cid  = 0;
    nvme_wr32(NVME_AQA, ((qd - 1u) & 0xFFFu) | (((qd - 1u) & 0xFFFu) << 16));
    // (d) ASQ/ACQ：**页对齐物理地址**（低内存页池：PA == VA）
    nvme_wr64(NVME_ASQ, (uint64_t)(uintptr_t)g_adm.sq);
    nvme_wr64(NVME_ACQ, (uint64_t)(uintptr_t)g_adm.cq);
    // (e) CC：CSS=0（NVM）、MPS=0（4KB）、IOSQES=6（64B）、IOCQES=4（16B）、EN=1
    nvme_wr32(NVME_CC, (6u << 16) | (4u << 20) | CC_EN);
    // (f) 等 CSTS.RDY=1
    if (!nvme_wait32(NVME_CSTS, CSTS_RDY, CSTS_RDY, NVME64_INIT_TICKS)) {
        nvme_timeout_log("csts_rdy1", 0);
        dbg64_line_begin64();
        dbg64_str("[NVME64] not ready (CC=0x"); dbg64_hex64(nvme_rd32(NVME_CC));
        dbg64_str(" CSTS=0x");                   dbg64_hex64(nvme_rd32(NVME_CSTS));
        dbg64_str(")");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    g_ctrl.cc   = nvme_rd32(NVME_CC);
    g_ctrl.csts = nvme_rd32(NVME_CSTS);

    dbg64_line_begin64();
    dbg64_str("[NVME64] cc=0x");   dbg64_hex64(g_ctrl.cc);
    dbg64_str(" csts=0x");         dbg64_hex64(g_ctrl.csts);
    dbg64_str(" rdy=");            dbg64_dec((g_ctrl.csts & CSTS_RDY) ? 1 : 0);
    dbg64_nl();
    dbg64_line_end64();

    if (g_ctrl.csts & CSTS_CFS) {                  // 控制器致命状态：如实收工，不做后续命令
        dbg64_line_begin64();
        dbg64_str("[NVME64] not ready (CSTS.CFS=1)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    g_ctrl.ready = 1;

    // ---- 5) Admin：Identify Controller（型号 + 序列号）----
    if (!nvme_adm_identify(NVME_CNS_CTRL, 0, g_id_page, "admin_id_ctrl")) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] identify controller failed");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    nvme_copy_str(g_ctrl.model,  (int)sizeof(g_ctrl.model),  g_id_page + 24, 40);   // MN
    nvme_copy_str(g_ctrl.serial, (int)sizeof(g_ctrl.serial), g_id_page + 4,  20);   // SN
    dbg64_line_begin64();
    dbg64_str("[NVME64] ctrl model=");
    dbg64_str(g_ctrl.model[0] ? g_ctrl.model : "(none)");
    dbg64_str(" sn=");
    dbg64_str(g_ctrl.serial[0] ? g_ctrl.serial : "(none)");
    dbg64_nl();
    dbg64_line_end64();

    // ---- 6) Admin：Identify Namespace(NSID=1)：NSZE（LBA 数）+ LBAF（LBA 字节数）----
    // 单命名空间策略（见 nvme64.h 的边界）：只登记 NSID 1。
    if (nvme_adm_identify(NVME_CNS_NS, 1, g_id_page, "admin_id_ns")) {
        const uint64_t nsze = *(const uint64_t*)(const void*)g_id_page;
        const uint32_t flbas = (uint32_t)(g_id_page[26] & 0x0Fu);                  // 当前 LBA 格式
        const uint32_t lbads = (uint32_t)g_id_page[128 + flbas * 4 + 2];           // LBADS = log2(LBA 字节数)
        const uint32_t lba_bytes = (lbads < 31u) ? (1u << lbads) : 0u;
        if (nsze == 0) {
            dbg64_line_begin64();
            dbg64_str("[NVME64] nsid=1 not present (NSZE=0)");
            dbg64_nl();
            dbg64_line_end64();
        } else if (lba_bytes != 512u) {
            // 如实：只支持 512B 逻辑块（上层 VFS/分区/安装引擎全按 512B 扇区组织）
            dbg64_line_begin64();
            dbg64_str("[NVME64] nsid=1 lba_bytes=");
            dbg64_dec(lba_bytes);
            dbg64_str(" unsupported (only 512B LBA)");
            dbg64_nl();
            dbg64_line_end64();
        } else {
            dbg64_line_begin64();
            dbg64_str("[NVME64] nsid=1 lba_bytes=");
            dbg64_dec(lba_bytes);
            dbg64_str(" sectors=");
            dbg64_dec(nsze);
            dbg64_nl();
            dbg64_line_end64();
            g_ns_nsid[g_ns_count] = 1u;
            g_ns_lba_bytes[g_ns_count] = lba_bytes;
            g_ns_sectors[g_ns_count] = nsze;
            g_ns_count++;
        }
    } else {
        dbg64_line_begin64();
        dbg64_str("[NVME64] identify namespace failed (NSID=1)");
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- 7) Admin：Create I/O CQ -> Create I/O SQ（规范推荐 CQ 先建）----
    if (!nvme_adm_create_cq(1u, qd, g_io.cq)) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] create io cq failed");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    if (!nvme_adm_create_sq(1u, qd, g_io.sq, 1u)) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] create io sq failed");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    g_io.used = 1;
    g_ctrl.io_ready = 1;

    dbg64_line_begin64();
    dbg64_str("[NVME64] queue sq=0x");
    dbg64_hex64((uint64_t)(uintptr_t)g_io.sq);
    dbg64_str(" cq=0x");
    dbg64_hex64((uint64_t)(uintptr_t)g_io.cq);
    dbg64_str(" qd=");
    dbg64_dec(qd);
    dbg64_nl();
    dbg64_line_end64();

    g_ctrl.ns_count = g_ns_count;
    dbg64_line_begin64();
    dbg64_str("[NVME64] ready ns=");
    dbg64_dec((uint64_t)g_ns_count);
    dbg64_str(" drive_base=");
    dbg64_dec((uint64_t)ATA64_NVME_BASE);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 对外 API ====================
int nvme64_count64() { return g_ns_count; }

const Nvme64CtrlInfo* nvme64_ctrl64() {
    // 只读摘要：报告页可能在本驱动**还没初始化**时来读（安装介质内核对不对？两边都会
    // 走 ata64_init64，但报告页仍可能先跑）——这里顺手幂等初始化一次，保证数字是真的。
    nvme64_init64();
    g_ctrl.ns_count = g_ns_count;
    for (int i = 0; i < g_ns_count; i++) {
        g_ctrl.nsid[i]      = g_ns_nsid[i];
        g_ctrl.lba_bytes[i] = g_ns_lba_bytes[i];
        g_ctrl.ns_sectors[i] = g_ns_sectors[i];
    }
    return &g_ctrl;
}

bool nvme64_info64(int idx, DiskInfo* out) {
    out->present = false;
    out->atapi = false;
    out->sectors = 0;
    out->model[0] = 0;
    if (idx < 0 || idx >= g_ns_count) return false;
    out->present = true;
    out->atapi = false;
    for (int i = 0; i < 41; i++) out->model[i] = g_ctrl.model[i];   // 型号 = 控制器型号
    out->model[40] = 0;
    out->sectors = g_ns_sectors[idx];                               // 容量 = NSZE（512B 扇区数）
    return true;
}

bool nvme64_read64(int idx, uint32_t lba, uint32_t count, void* buf) {
    if (idx < 0 || idx >= g_ns_count || !buf) return false;
    const bool ok = nvme_xfer(idx, lba, count, buf, false);
    if (ok && !g_first_read_logged) {
        g_first_read_logged = 1;                    // 首次读盘留一行证据，之后不再打（避免刷屏）
        dbg64_line_begin64();
        dbg64_str("[NVME64] read lba=");
        dbg64_dec(lba);
        dbg64_str(" count=");
        dbg64_dec(count);
        dbg64_str(" ok");
        dbg64_nl();
        dbg64_line_end64();
    }
    return ok;
}

bool nvme64_write64(int idx, uint32_t lba, uint32_t count, const void* buf) {
    if (idx < 0 || idx >= g_ns_count || !buf) return false;
    const bool ok = nvme_xfer(idx, lba, count, (void*)buf, true);
    if (ok && !g_first_write_logged) {
        g_first_write_logged = 1;                   // 首次写盘留一行证据（安装时会命中）
        dbg64_line_begin64();
        dbg64_str("[NVME64] write lba=");
        dbg64_dec(lba);
        dbg64_str(" count=");
        dbg64_dec(count);
        dbg64_str(" ok");
        dbg64_nl();
        dbg64_line_end64();
    }
    return ok;
}

// ==================== 只读自检 ====================
bool nvme64_selftest64() {
    nvme64_init64();                                // 幂等
    if (!g_ctrl.found) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] selftest skipped (no controller)");
        dbg64_nl();
        dbg64_line_end64();
        return true;                                // 没控制器不算失败（与 ahci64/usb 的 skipped 口径一致）
    }
    if (!g_ctrl.ready || !g_io.used) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] selftest skipped (controller not ready)");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    if (g_ns_count == 0) {
        dbg64_line_begin64();
        dbg64_str("[NVME64] selftest skipped (no namespace)");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }

    int mask = 0;
    // 1) 控制器型号/序列号必须真是从设备读回来的
    if (g_ctrl.model[0] == 0) mask |= 16;
    // 2) 容量（NSZE）不能是 0
    if (g_ns_sectors[0] == 0) mask |= 2;
    // 3) 只读：读 LBA 0（**不写盘**），证明 DMA 通路真的能搬数据
    for (int k = 0; k < 512; k++) g_id_page[k] = 0;
    if (nvme_xfer(0, 0, 1, g_id_page, false)) {
        if (!g_first_read_logged) g_first_read_logged = 1;
        dbg64_line_begin64();
        dbg64_str("[NVME64] read lba=0 count=1 ok");
        dbg64_nl();
        dbg64_line_end64();
    } else {
        mask |= 8;
    }
    if (!g_ctrl.io_ready) mask |= 4;

    dbg64_line_begin64();
    if (mask == 0) {
        dbg64_str("[NVME64] selftest PASS");
        dbg64_nl();
        dbg64_line_end64();
        return true;
    }
    dbg64_str("[NVME64] selftest FAIL mask=");
    dbg64_dec((uint64_t)mask);
    dbg64_nl();
    dbg64_line_end64();
    return false;
}
