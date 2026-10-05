// ehci64.cpp - USB 2.0 主机控制器（EHCI）驱动 + USB 存储（BOT/SCSI，★ 可写）
//
// ============================ 为什么这么做 ============================
// 1) 为什么要它：机器上只有 UHCI（usb64）与 xHCI（xhci64）两份实现。**只挂 EHCI 的机器**
//    （USB 2.0 时代的芯片组：ICH7/ICH9 一带，BIOS 把端口路由给 EHCI 时）此前插 U 盘什么都不会发生
//    —— 本文件把 EHCI 补上，存储接到 kernel/ata64.h 的 ATA64_USB_BASE（24..）那段统一驱动器号。
// 2) 寄存器与 DMA 全部是**物理地址**（与 usb64/xhci64 同一条纪律）：
//    * MMIO：BAR0（64 位 MMIO BAR）**恒等映射**（前 4GB），物理地址即指针；
//    * DMA 结构（QH/qTD 池、数据缓冲、DMA 暂存页）全部来自 page_alloc_64()（页池恒等映射），
//      **指针值就是物理地址**；
//    * 调用方传进来的缓冲区（drive64 的扇区缓冲在内核 .bss 高半区）走 pa 直映换算；认不出来就
//      退回 **DMA 暂存页**（拷贝进出），绝不把错地址交给硬件（usb64.cpp 的 usb_pa32 踩过这个坑）。
// 3) 异步调度：**一个自环 QH**（H 位 = Head of Reclamation，HLP 指向自己）+ 一条垂直的 qTD 链。
//    * 水平指针（QH.HLP）：异步表在这个 QH 上闭环（单个 QH 也必须闭环，否则控制器会走出表）；
//    * 垂直指针（qTD.next）：一条 qTD 链 = 一次传输（控制三段 / 批量 N 包）；
//    * qTD.alt_next：本驱动一律 TERM（不用"替代链"，短包由 BOT 的 CSW residue 判定）；
//    * QH 覆盖区（overlay：cur/next/alt/token/buf）是**硬件唯一写回的地方**（qTD 本身硬件只读），
//      所以完成状态从 overlay 读：token.ACTIVE 清掉 = 当前 qTD 不在飞；cur 前进到 TERM 或
//      最后一条 qTD = 链走完。**完成信号取 USBSTS.USBINT（最后一条 qTD 带 IOC）**，overlay 只作
//      兜底与状态读取 —— 两条信号都在同一轮询循环里看，有界超时，绝不依赖中断投递。
//    * ★ 修改 QH 前后按规范"安全窗口"做：**先 USBCMD.ASE=0 并等 USBSTS.ASS 读回 0** 再改覆盖区，
//      改完再 ASE=1 —— 避免硬件正在取 qTD 时被我们改写（这是异步表唯一一类难查的竞态）。
//    * ★ "每次轮询只处理一项传输"：一个 QH、一条链，串行同步传输（由传输锁保护），不做流水。
//    * Ping 位（QH Endpoint Capabilities bit30）本驱动**置 0**（不做 ping 流控，如实说明）：
//      ping 只对"高速 OUT 且设备可能长时间 NAK"的场景有意义，代价是每个 OUT 包多一次往返；
//      这里靠正常的 NAK 重试语义即可（QEMU 的 usb-storage 与真实 U 盘都工作在这条路径上）。
// 4) 端口策略：**只接管 High-speed 端口**（EHCI 规范：PED 只在高速设备上置起来）。
//    * PortOwner=1（伴随 UHCI/OHCI 的端口）**不抢**，只打点；
//    * 复位后 PED 没起来（FS/LS 设备）-> 写 PortOwner=1 交还伴随控制器（打点 owner=1 speed=full|low）；
//    * BIOS handoff：HCCPARAMS.EECP 指向扩展能力链，找 USBLEGSUP(cap id 0x01)，
//      置 OS 语义位（bit24）、等 BIOS 位（bit16）清、清 SMI 使能 —— 拿不到所有权也继续（如实打点）。
// 5) 传输全部**同步 + 有界超时**（g_ticks64 计时 + 硬自旋上界兜底），超时一律 abort 并如实返回失败。
//    运行期轮询（端口插拔）由 kusb 内核线程经 usb64_poll64() -> ehci64_poll64() 驱动，
//    与同步传输用**自旋锁**互斥（poll 侧 try-lock，拿不到就下次再来，绝不阻塞）。
// 6) ★ HID 键盘**没做**（EHCI 中断传输 / 高速中断端点 + S-mask 调度）：键盘由 UHCI 与 xHCI 覆盖，
//    本驱动只做 USB 存储；`[EHCI]` 打点里因此没有 hid 行。这条边界是**有意的**，不是漏做。
//
// 没做到的（如实记录）：
//   * 不做 hub（集线器后面的设备）、不做 FS/LS 设备（伴随端口不碰）、不做热插拔的地址回收
//     （地址只增不复用，到 0x7F 上限后只打点不枚举）；
//   * 不接 USB IRQ / MSI（全程轮询）、不做挂起/恢复（SUSPEND/FPR）、不做 EOF1/EOF2 的错误恢复；
//   * 不做 Split Transaction（EHCI 的 FS/LS 走 hub 时需要）、不做带宽/微帧调度（S-mask 恒 0x01）；
//   * 周期表（Periodic List）**完全不建**（PSE=0）：本驱动只需要异步调度（批量/控制），
//     中断传输没做，所以周期性骨架不需要 —— 这是刻意的取舍，不是漏配。
// ======================================================================
#include "ehci64.h"
#include "port.h"         // outl/inl（PCI 配置空间 0xCF8/0xCFC）
#include "debug64.h"      // 串口打点（行锁 begin/end）
#include "memlayout64.h"  // ★ 必须先于 mem_64.h（PAGE_SIZE_64 会撞）；ML64_KERNEL_VA_BASE/BASE
#include "mem_64.h"       // page_alloc_64 / memset_64 / memcpy_64 / memcmp_64
#include "x86_64.h"       // g_ticks64 / ms_to_ticks64 / nop_pause()
#include <stdint.h>
#include <stddef.h>

extern "C" char __bss_end[];               // 内核镜像高半区上界（判断指针是否落在内核镜像里）

// ==================== 寄存器偏移（EHCI 1.0 第 2/3/4 章）====================
// 能力寄存器（相对 BAR0）
#define EHCI_CAP_CAPLENGTH   0x00u     // 1 字节：操作寄存器的字节偏移
#define EHCI_CAP_HCSPARAMS   0x04u     // N_PORTS[3:0] PPC[4] N_CC[11:8] P_INDICATOR[16] DebugPort[27:20]
#define EHCI_CAP_HCCPARAMS   0x08u     // AC64[0] PFLF[1] ASP[2] IST[7:4] EECP[15:8]
#define EHCI_CAP_DBOFF       0x14u     // 调试端口寄存器偏移（本驱动只打点，不用）
// 操作寄存器（相对 CAPLENGTH）
#define EHCI_OP_USBCMD       0x00u
#define EHCI_OP_USBSTS       0x04u
#define EHCI_OP_USBINTR      0x08u
#define EHCI_OP_FRINDEX      0x0Cu
#define EHCI_OP_CTRLDSSEG    0x10u     // CTRLDSSEGMENT（64 位寻址；本驱动只支持前 4GB，写 0）
#define EHCI_OP_PERIODICBASE 0x14u     // 周期表基址（PSE=0，不建周期表）
#define EHCI_OP_ASYNCLISTADDR 0x18u    // ★ 异步表基址（QH 的物理地址）
#define EHCI_OP_CONFIGFLAG   0x40u     // =1：把所有端口路由给 EHCI（否则被伴随控制器占着）
#define EHCI_OP_PORTSC       0x44u     // 端口 1..N，步长 4
#define EHCI_PORT_STRIDE     4u

// USBCMD
#define EHCI_CMD_RS          (1u << 0)     // Run/Stop
#define EHCI_CMD_HCRESET     (1u << 1)     // Host Controller Reset（自清）
#define EHCI_CMD_PSE         (1u << 4)     // Periodic Schedule Enable（本驱动不建周期表）
#define EHCI_CMD_ASE         (1u << 5)     // ★ Asynchronous Schedule Enable
#define EHCI_CMD_ITC_1       (1u << 16)    // 中断阈值 = 1（8 微帧）—— 只影响中断合并，全程轮询
// USBSTS（写 1 清）
#define EHCI_STS_USBINT      (1u << 0)
#define EHCI_STS_USBERR      (1u << 1)
#define EHCI_STS_PCD         (1u << 2)
#define EHCI_STS_FLR         (1u << 3)
#define EHCI_STS_HSE         (1u << 4)
#define EHCI_STS_HCH         (1u << 12)    // HC Halted（RS=0 且真的停了）
#define EHCI_STS_ASS         (1u << 15)    // ★ Asynchronous Schedule Status（ASE 是否真的生效）
// PORTSC
#define EHCI_PSC_CCS         (1u << 0)     // Current Connect Status
#define EHCI_PSC_CSC         (1u << 1)     // Connect Status Change（W1C）
#define EHCI_PSC_PED         (1u << 2)     // Port Enabled（高速设备才置得起来）
#define EHCI_PSC_PEDC        (1u << 3)     // Port Enable Change（W1C）
#define EHCI_PSC_OCA         (1u << 4)     // Over-current Active
#define EHCI_PSC_OCC         (1u << 5)     // Over-current Change（W1C）
#define EHCI_PSC_FPR         (1u << 6)     // Force Port Resume（本驱动不碰）
#define EHCI_PSC_SUSP        (1u << 7)     // Suspend（本驱动不碰）
#define EHCI_PSC_PR          (1u << 8)     // Port Reset（写 1 开始复位，自清）
#define EHCI_PSC_LSC         (1u << 10)    // Line Status Change（W1C）
#define EHCI_PSC_PP          (1u << 12)    // Port Power
#define EHCI_PSC_PO          (1u << 13)    // ★ Port Owner（1 = 伴随控制器持有这个端口）
#define EHCI_PSC_IND_MASK    (3u << 14)
#define EHCI_PSC_WK_MASK     ((1u << 20) | (1u << 21) | (1u << 22))
#define EHCI_PSC_SPEED_MASK  (3u << 26)    // 复位后有效：0=full 1=low 2=high
#define EHCI_PSC_SPEED_SHIFT 26
// 读-改-写时"我们愿意保留的 RW 位"：PP / PO / 指示灯 / 唤醒使能。
// **绝不**把 PR / SUSP / FPR / 测试位带回写（那会真的复位/挂起端口）。
#define EHCI_PSC_KEEP        (EHCI_PSC_PP | EHCI_PSC_PO | EHCI_PSC_IND_MASK | EHCI_PSC_WK_MASK)
// W1C：写 1 清
#define EHCI_PSC_W1C         (EHCI_PSC_CSC | EHCI_PSC_PEDC | EHCI_PSC_OCC | EHCI_PSC_LSC)

// qTD Token（32 位；★ 低位是状态、高位是长度 —— 与 iTD 的布局不一样，别抄错）：
//   bit0 Ping  bit1 SplitX  bit2 MMF  bit3 XACTERR  bit4 BABBLE  bit5 DBE  bit6 HALTED  bit7 ACTIVE
//   bits9:8 PID（0=OUT 1=IN 2=SETUP）  bits11:10 CERR（重试次数）  bits14:12 Current Page
//   bit15 IOC（完成中断）  bits30:16 Total Bytes to Transfer（15 位）  bit31 Data Toggle
#define EHCI_QTD_ACTIVE      (1u << 7)
#define EHCI_QTD_HALTED      (1u << 6)
#define EHCI_QTD_DBE         (1u << 5)
#define EHCI_QTD_BABBLE      (1u << 4)
#define EHCI_QTD_XACT        (1u << 3)
#define EHCI_QTD_MMF         (1u << 2)
#define EHCI_QTD_PING        (1u << 0)
#define EHCI_QTD_PID_OUT     (0u << 8)
#define EHCI_QTD_PID_IN      (1u << 8)
#define EHCI_QTD_PID_SETUP   (2u << 8)
#define EHCI_QTD_PID_MASK    (3u << 8)
#define EHCI_QTD_CERR_3      (3u << 10)
#define EHCI_QTD_IOC         (1u << 15)
#define EHCI_QTD_LEN_MASK    (0x7FFFu << 16)
#define EHCI_QTD_LEN_SHIFT   16
#define EHCI_QTD_DT          (1u << 31)     // Data Toggle（DTC=1 时由软件维护）
#define EHCI_QTD_STATUS_MASK (EHCI_QTD_ACTIVE | EHCI_QTD_HALTED | EHCI_QTD_DBE | EHCI_QTD_BABBLE | EHCI_QTD_XACT)
#define EHCI_QTD_TERM        (1u)           // next/alt_next 的 T 位

// QH
#define EHCI_QH_HLP_TERM     (1u)           // HLP 的 T 位
#define EHCI_QH_TYPE_QH      (1u)           // HLP 的 type=01（QH）
#define EHCI_QH_EC_H         (1u << 15)     // Head of Reclamation List
#define EHCI_QH_EC_DTC       (1u << 14)     // Data Toggle Control（1 = 软件维护 toggle）
#define EHCI_QH_EC_C         (1u << 27)     // Control Endpoint Flag（高速 + mps=64 的控制端点）
#define EHCI_QH_EC_SPEED_HIGH (2u << 12)
#define EHCI_QH_EC_MPS_SHIFT 16

// ==================== 数据结构 ====================
// qTD（32 字节，必须 32 字节对齐）
struct EhciQtd {
    volatile uint32_t next;        // 垂直指针：下一条 qTD（bit0 = T 终止）
    volatile uint32_t alt;         // 替代指针：短包/错误时的分支（本驱动恒 TERM）
    volatile uint32_t token;       // 状态:PID:CERR:页:IOC:长度:DT（见上面的位定义）
    volatile uint32_t buf[5];      // 缓冲区页指针（buf[0] = 精确地址，buf[1..] = 后续页）
} __attribute__((aligned(32)));

// QH（48 字节；后 32 字节是"qTD 覆盖区"，布局与 qTD 后半段一致）
struct EhciQhHead {
    volatile uint32_t hlp;         // 水平指针：下一个 QH（bit0 = T 终止；type=01）
    volatile uint32_t epchar;      // 端点特征：地址/端点/速度/DTC/H/mps/C
    volatile uint32_t epcap;       // 端点能力：S-mask/C-mask/hub/port/Chained/Ping
};
struct EhciQh {
    volatile uint32_t hlp;
    volatile uint32_t epchar;
    volatile uint32_t epcap;
    volatile uint32_t cur;         // overlay：当前 qTD（★ 发布点：最后写）
    volatile uint32_t next;        // overlay：下一条 qTD
    volatile uint32_t alt;         // overlay：替代 qTD
    volatile uint32_t token;       // overlay：状态/长度（硬件写回的唯一位置）
    volatile uint32_t buf[5];      // overlay：缓冲区指针
} __attribute__((aligned(32)));

// qTD 池：一个页里放 32 条（0x40 + 32*0x20 = 0x440 < 0x1000）
#define EHCI_QTD_COUNT 32
// 单条 qTD 的长度上限（4KB = 两页内必定装得下：buf[0] 精确地址 + buf[1] 下一页）
#define EHCI_QTD_MAX_BYTES 4096u
// 一次批量调用的字节上限（与 usb64 的 USB64_MSC_MAX_DATA 一致：8 个 512B 扇区）
#define EHCI_MSC_MAX_SECTORS 8
#define EHCI_MSC_MAX_DATA    (EHCI_MSC_MAX_SECTORS * 512u)
// 超时（有界）
#define EHCI_CTL_TIMEOUT_MS  300u
// 超时（有界）
#define EHCI_CTL_TIMEOUT_MS  300u
#define EHCI_BULK_TIMEOUT_MS 800u
#define EHCI_RESET_SPINS     2000000u
// ★ 复位后等 PED 的**自旋次数上界**（除 100ms 时间上界外的第二道保险：时间基准万一看不到进展，
//   也绝不挂死 —— 5 万次 MMIO 读约等于 1~2 s，够用且不会把启动链拖住）。
#define EHCI_PED_SPINS       50000u
#define EHCI_CTL_BUF_BYTES   256u
#define EHCI_MAX_PORTS       16

#define EHCI_ST_INIT        0u
#define EHCI_ST_NOT_FOUND   1u
#define EHCI_ST_NO_DEVICE   2u
#define EHCI_ST_ENUM_FAILED 3u
#define EHCI_ST_READY       4u

// ==================== 全局状态 ====================
static bool      g_inited = false;
static bool      g_found  = false;             // 找到 EHCI 主控
static bool      g_ready  = false;             // 异步表在跑（RS+ASE 已配好）
static uint32_t  g_state  = EHCI_ST_INIT;

static uint8_t   g_bus = 0, g_pcidev = 0, g_fn = 0;
static uint64_t  g_mmio = 0;                   // BAR0（MMIO 基址，恒等映射）
static uint32_t  g_caplen = 0, g_dboff = 0, g_ports = 0, g_ppc = 0;
static uint32_t  g_hcs = 0, g_hcc = 0, g_eecp = 0;
static volatile uint32_t* g_reg = nullptr;     // MMIO 32 位视图（= g_mmio）

// DMA 结构（page_alloc_64 给的页：恒等映射，指针值 = 物理地址）
static uint8_t*  g_p_async = nullptr;          // QH + qTD 池
static uint8_t*  g_p_data  = nullptr;          // 控制/描述符/CBW/CSW/扇区缓冲
static uint8_t*  g_p_bounce= nullptr;          // 4KB DMA 暂存页（调用方缓冲区认不出物理地址时）
static EhciQh*   g_qh      = nullptr;
static uint8_t*  g_setup   = nullptr;          // 8 字节 SETUP 包
static uint8_t*  g_desc    = nullptr;          // 256 字节描述符缓冲
static uint8_t*  g_cbw     = nullptr;          // 32 字节 CBW
static uint8_t*  g_csw     = nullptr;          // 16 字节 CSW
static uint8_t*  g_scratch = nullptr;          // 64 字节（INQUIRY / SENSE / CAPACITY）
static uint8_t*  g_sector  = nullptr;          // 512 字节（自检读的那一块）

// 统计/自检
static uint32_t  g_devices = 0;
static uint32_t  g_link_ok = 0;                // 复位后 PED=1 的端口数
static uint32_t  g_selftest_mask = 0;
static uint32_t  g_conn_seen = 0;              // 启动期"有设备"的端口数
static uint32_t  g_ports_seen = 0;
static uint64_t  g_xfers = 0;

// 存储设备（最多一台）
struct EhciMsc {
    bool     present;              // 枚举到了 BOT 存储接口
    bool     supported;            // 探测全过（暴露成块设备）
    uint8_t  addr;                 // USB 地址
    uint8_t  ep_in, ep_out;        // 批量端点号（含方向位）
    uint16_t mps_in, mps_out;      // 批量端点最大包
    uint32_t blocks;               // 总块数（READ CAPACITY(10) 报的"最后 LBA + 1"）
    uint32_t block_size;
    uint32_t tag;                  // BOT 的 CBW Tag
    uint8_t  csw_status;           // 最近一次 CSW 的 bCSWStatus
    uint32_t csw_residue;          // 最近一次 CSW 的 dCSWDataResidue
    uint32_t last_reason;          // 见 ehci64_msc_last_reason64()
    char     vendor[9];
    char     product[17];
    uint32_t reads_ok, reads_fail, writes_ok, writes_fail, wverify_ok, wverify_fail;
    uint32_t selftest_mask;
};
static EhciMsc g_msc;
static bool    g_msc_probe_done = false;       // 探测只做一次（热插拔重枚举时会重做）
// 打点上限（防刷屏；与 usb64 同款）
static uint32_t g_usbst_read_logs = 0, g_usbst_fail_logs = 0, g_usbst_wlogs = 0, g_usbst_wfail_logs = 0;
#define EHCI_READ_LOG_MAX 128u
#define EHCI_FAIL_LOG_MAX 64u
#define EHCI_WRITE_LOG_MAX 64u

// ★ P8b 同款纪律：初始化收尾之前一律不比对端口（否则把刚枚举好的设备当成"新插入"）
static bool     g_hp_armed = false;
static uint32_t g_port_ccs = 0;                // 上一轮各根端口的 CCS 位
static uint32_t g_hp_retry_fail_logs = 0;

// 传输互斥（与 usb64 同款：传输侧自旋拿锁，poll 侧 try-lock）
static volatile uint32_t g_ehci_lock = 0;
static inline bool ehci_lock_try() {
    uint32_t v = 1;
    __asm__ volatile("xchgl %0, %1" : "+r"(v) : "m"(g_ehci_lock) : "memory");
    return v == 0;
}
static void ehci_lock_acquire() {
    uint64_t spins = 0;
    while (!ehci_lock_try() && spins < 400000000ull) { spins++; nop_pause(); }
}
static inline void ehci_lock_release() {
    __asm__ volatile("" ::: "memory");
    g_ehci_lock = 0;
}

// ==================== 小工具 ====================
static void elog_begin() { dbg64_line_begin64(); }
static void elog_end()   { dbg64_nl(); dbg64_line_end64(); }

static void ehex(uint32_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    char buf[9];
    if (digits > 8) digits = 8;
    for (int i = digits - 1; i >= 0; i--) { buf[i] = H[v & 0xFu]; v >>= 4; }
    for (int i = 0; i < digits; i++) dbg64_putc(buf[i]);
}

// ★ 物理地址换算（与 usb64.cpp 的 usb_pa32 / xhci64.cpp 的 xhci_pa64 同款约定）：
//   低内存（< 4GB，含页池里的 DMA 结构）：恒等映射；内核镜像高半区对象（.bss/.data）直映；
//   认不出来返回 0，调用方退回 DMA 暂存页（绝不把错地址交给硬件）。
static inline uint32_t ehci_pa64(const void* ptr) {
    const uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v == 0) return 0;
    if (v < 0x100000000ULL) return (uint32_t)v;
    if (v >= ML64_KERNEL_VA_BASE && v < (uint64_t)(uintptr_t)__bss_end + 0x10000ULL)
        return (uint32_t)(v - (ML64_KERNEL_VA_BASE - (uint64_t)ML64_KERNEL_BASE));
    return 0;
}

static void edelay_ms(uint32_t ms) {
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(ms ? ms : 1);
    uint64_t spin = 0;
    while ((g_ticks64 - t0) < want && spin < 400000000ull) { spin++; nop_pause(); }
}

static inline uint32_t mm_rd32(uint32_t off) { return g_reg[off >> 2]; }
static inline void     mm_wr32(uint32_t off, uint32_t v) {
    g_reg[off >> 2] = v;
    __asm__ volatile("" ::: "memory");
}
static inline uint32_t op_rd32(uint32_t off) { return mm_rd32(g_caplen + off); }
static inline void     op_wr32(uint32_t off, uint32_t v) { mm_wr32(g_caplen + off, v); }
static inline uint32_t port_off(uint32_t n) { return EHCI_OP_PORTSC + (n - 1u) * EHCI_PORT_STRIDE; }
static inline uint32_t port_rd(uint32_t n) { return op_rd32(port_off(n)); }
// 写 PORTSC：只回写"我们愿意保留的 RW 位" + 指定的 W1C 位；RO 位一律写 0。
// ★ 注意 PORTSC 的 **PED（bit2）是 R/W**：写 0 = **关闭端口**。所以"清变化位"这类写入**必须**把
//   当前 PED 原样写回，否则刚复位好的高速端口会被自己关掉（下面 port_clear_changes 专门做这件事）。
static inline void port_wr(uint32_t n, uint32_t v) {
    const uint32_t cur = port_rd(n);
    op_wr32(port_off(n), (cur & EHCI_PSC_KEEP) | (v & ~EHCI_PSC_KEEP));
}

// 清 PORTSC 的 W1C 变化位（CSC/PEDC/OCC/LSC），**保留 PED**（参上：写 0 会关端口）。
// 热插拔轮询与启动期"清掉上电时的变化位"都用它 —— 不动正在工作的那个已使能端口。
static inline void port_clear_changes(uint32_t n) {
    const uint32_t cur = port_rd(n);
    port_wr(n, (cur & EHCI_PSC_PED) | EHCI_PSC_CSC | EHCI_PSC_PEDC | EHCI_PSC_OCC | EHCI_PSC_LSC);
}

// ★ 把端口交还伴随控制器：写 PO=1（PO 在 port_wr 的 KEEP 里会被换成当前值，所以这里直接写寄存器）。
//   保留 PP / 中断指示 / 唤醒位；PED 写 0（本驱动没使能过它，规范也要求交还前不用它）。
static inline void port_give_to_companion(uint32_t n) {
    const uint32_t cur = port_rd(n);
    op_wr32(port_off(n), (cur & (EHCI_PSC_PP | EHCI_PSC_IND_MASK | EHCI_PSC_WK_MASK)) | EHCI_PSC_PO);
}


// ==================== PCI（0xCF8/0xCFC；与 usb64/xhci64 同一套做法）====================
static uint32_t epci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void epci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, v);
}

// 找 EHCI：**class 0x0C / subclass 0x03 / prog-if 0x20**（PCI 规范里 USB 2.0 EHCI 的 prog-if）。
// 只认标准 prog-if —— 别的 prog-if（0x00 UHCI / 0x10 OHCI / 0x30 xHCI）**绝不能**被当成 EHCI，
// 那不是"兼容"，而是会把另一台控制器当自家主控乱配（真变砖的路子）。
static bool ehci_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = epci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = epci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = epci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint32_t cc = epci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                if ((cc >> 24) != 0x0Cu || ((cc >> 16) & 0xFFu) != 0x03u) continue;
                if (((cc >> 8) & 0xFFu) != 0x20u) continue;
                g_bus = (uint8_t)bus; g_pcidev = (uint8_t)dev; g_fn = (uint8_t)fn;
                return true;
            }
        }
        if (bus_has) empty_run = 0;
        else if (++empty_run >= 4u) break;
    }
    return false;
}

// ==================== BIOS handoff（USBLEGSUP，EECP 链里的 cap id 0x01）====================
// 规范流程（EHCI 1.0 §2.3.4.1 "OS/BIOS Handoff"）：
//   ① 置 USBLEGSUP 的 **OS Owned Semaphore（bit24）**；
//   ② 等 BIOS Owned Semaphore（bit16）清（BIOS 让出所有权）；1 秒还不清可视为 BIOS 已死；
//   ③ 清 USBLEGCTLSTS（offset+4）低半字的 SMI 使能位，别让固件继续 SMI 抢寄存器。
// 拿不到/没有 EECP 都**不阻塞**后续初始化（如实打点即可）—— 端口能不能用最终看 PORTSC。
static bool ehci_bios_handoff() {
    if (g_eecp == 0) return false;
    uint8_t off = (uint8_t)g_eecp;
    for (int i = 0; i < 8 && off >= 0x40; i++) {
        const uint32_t v = epci_rd32(g_bus, g_pcidev, g_fn, off);
        const uint8_t cap_id = (uint8_t)(v & 0xFFu);
        const uint8_t next   = (uint8_t)((v >> 8) & 0xFFu);
        if (cap_id == 0x01u) {                              // USBLEGSUP
            uint32_t sup = v;
            if (sup & 0x01000000u) {                        // 已经被 OS 拿走（例如引导链/固件）
                elog_begin();
                dbg64_str("[EHCI] bios handoff ee=");
                ehex((uint32_t)off, 2);
                dbg64_str(" took=1 (os already owned)");
                elog_end();
                return true;
            }
            epci_wr32(g_bus, g_pcidev, g_fn, off, (sup & ~0x00010000u) | 0x01000000u);
            bool took = false;
            for (int t = 0; t < 100; t++) {                 // ≤ 1s（10ms 一步）
                sup = epci_rd32(g_bus, g_pcidev, g_fn, off);
                if (!(sup & 0x00010000u)) { took = true; break; }
                edelay_ms(10);
            }
            if (!took) {
                // BIOS 位一直不清：规范允许 OS 认为 BIOS 已死并继续 —— 如实打点，继续初始化。
                epci_wr32(g_bus, g_pcidev, g_fn, off, (sup | 0x01000000u));
            }
            const uint32_t ctl = epci_rd32(g_bus, g_pcidev, g_fn, (uint8_t)(off + 4u));
            epci_wr32(g_bus, g_pcidev, g_fn, (uint8_t)(off + 4u), ctl & 0xFFFF0000u);   // 清 SMI 使能
            elog_begin();
            dbg64_str("[EHCI] bios handoff ee=");
            ehex((uint32_t)off, 2);
            dbg64_str(" took=");
            dbg64_dec(took ? 1u : 0u);
            dbg64_str(took ? "" : " (bios bit still set; continuing)");
            elog_end();
            return took;
        }
        if (next == 0) break;
        off = next;
    }
    return false;
}

// ==================== qTD / QH ====================
static volatile EhciQtd* qtd_slot(int i) {
    return (volatile EhciQtd*)(void*)(g_p_async + 0x40 + (size_t)i * 32u);
}

// 填一条 qTD：next/alt 用物理地址（bit0=1 表示终止），token/buf 见上面的位定义。
//   buf  = 缓冲区**物理**地址；len ≤ 4096（buf[0] 精确地址 + buf[1] 下一页就够）
static void qtd_fill(volatile EhciQtd* q, uint32_t next_pa, bool next_term, uint32_t token, uint32_t buf_pa, uint32_t len) {
    q->next = next_term ? EHCI_QTD_TERM : (next_pa & ~0x1Fu);
    q->alt  = EHCI_QTD_TERM;                                // 本驱动不用替代链（短包由 CSW residue 判）
    q->token = token | ((len & 0x7FFFu) << EHCI_QTD_LEN_SHIFT) | EHCI_QTD_ACTIVE;
    q->buf[0] = buf_pa;
    q->buf[1] = (buf_pa & ~0xFFFu) + 0x1000u;               // 下一页（≤4KB 时最多用到两页）
    q->buf[2] = 0; q->buf[3] = 0; q->buf[4] = 0;
    __asm__ volatile("" ::: "memory");
}

// --- 异步表开关（改 QH 前的"安全窗口"）---
static void async_sched_off() {
    const uint32_t cmd = op_rd32(EHCI_OP_USBCMD);
    if (cmd & EHCI_CMD_ASE) op_wr32(EHCI_OP_USBCMD, cmd & ~EHCI_CMD_ASE);
    for (uint32_t i = 0; i < 20000u; i++) {                 // 等 USBSTS.ASS 读回 0（有界）
        if (!(op_rd32(EHCI_OP_USBSTS) & EHCI_STS_ASS)) return;
        nop_pause();
    }
    // 超时不阻塞：只是少了一次"安全确认"，下面的写回仍按发布顺序做。
}

// 一条 qTD 链的执行：返回 0 = 完成（可用 *status 判断错误位），-1 = 超时或控制器没跑。
// 完成判据（两条信号，同一轮循环里看，见文件头第 3 节）：
//   ① USBSTS.USBINT（最后一条 qTD 带 IOC）被硬件置起；
//   ② overlay：token.ACTIVE 清掉 **且** cur 已前进到 TERM 或最后一条 qTD（兜底，避免假死）。
static int ehci_exec_chain(volatile EhciQtd* first, volatile EhciQtd* last,
                           uint32_t epchar, uint32_t epcap, uint32_t timeout_ms,
                           uint32_t* out_token, uint32_t* out_sts) {
    if (out_token) *out_token = 0;
    if (out_sts) *out_sts = 0;
    if (!g_qh || !first) return -1;
    if (!(op_rd32(EHCI_OP_USBCMD) & EHCI_CMD_RS)) return -1;   // 控制器没在跑

    async_sched_off();

    // ---- 组装 QH：水平自环（单个 QH 也必须闭环）+ 端点特征；覆盖区最后发布 ----
    const uint32_t qh_pa = (uint32_t)(uintptr_t)g_qh;
    g_qh->hlp    = qh_pa | EHCI_QH_TYPE_QH;                    // 自环：异步表就这一个 QH
    g_qh->epchar = epchar | EHCI_QH_EC_H;                      // ★ H 位：Head of Reclamation List
    g_qh->epcap  = epcap;
    // 清覆盖区（顺序：先 alt/buf/token，**最后**发布 next = 第一条 qTD —— 这才是"发布点"）。
    // ★ 布局（与 EHCI 1.0 §3.6 / Linux struct ehci_qh_hw **逐字段一致**，本轮修）：
    //     0x00 hlp 0x04 epchar 0x08 epcap
    //     0x0C cur   = **Current qTD Pointer（硬件拥有：控制器写它，软件不写）**
    //     0x10 next  = **overlay Next qTD Pointer（软件把第一条 qTD 写在这里）** ← 发布点
    //     0x14 alt 0x18 token 0x1C buf[5]
    //   旧代码把第一条 qTD 写进 `cur`（硬件拥有字段）、把 `next` 留在 TERM —— 控制器于是认为
    //   "这条链没有 qTD 可执行"：QH 被取到（USBSTS.ASS=1）但一条 qTD 都没跑，软件等超时
    //   （实测串口：`[EHCI] control req=06 FAILED (timeout) tok=00000000 sts=00008000`）。
    g_qh->alt   = EHCI_QTD_TERM;
    g_qh->token = 0;
    for (int i = 0; i < 5; i++) g_qh->buf[i] = 0;
    g_qh->cur   = 0;                                           // 硬件拥有字段：软件只清 0（不发布 qTD）
    __asm__ volatile("" ::: "memory");
    g_qh->next  = (uint32_t)(uintptr_t)first;                 // ★ 发布：overlay 的 Next qTD Pointer
    __asm__ volatile("" ::: "memory");

    op_wr32(EHCI_OP_ASYNCLISTADDR, qh_pa);
    const uint32_t sts_clear = EHCI_STS_USBINT | EHCI_STS_USBERR | EHCI_STS_PCD | EHCI_STS_FLR | EHCI_STS_HSE;
    op_wr32(EHCI_OP_USBSTS, sts_clear);                        // 清挂起位（写 1 清）
    const uint32_t cmd0 = op_rd32(EHCI_OP_USBCMD);
    op_wr32(EHCI_OP_USBCMD, cmd0 | EHCI_CMD_ASE);              // 开始跑这一条链

    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(timeout_ms ? timeout_ms : 1);
    uint64_t spin = 0;
    bool done = false;
    for (;;) {
        const uint32_t sts = op_rd32(EHCI_OP_USBSTS);
        const uint32_t tok = g_qh->token;
        const uint32_t cur = g_qh->cur;
        const bool nothing_in_flight = !(tok & EHCI_QTD_ACTIVE) && !(last->token & EHCI_QTD_ACTIVE);
        if ((sts & EHCI_STS_USBINT) && nothing_in_flight) { done = true; break; }
        if (nothing_in_flight && (cur == EHCI_QTD_TERM || cur == (uint32_t)(uintptr_t)last ||
                                  cur == (uint32_t)(uintptr_t)first)) { done = true; break; }
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) break;
        nop_pause();
    }

    // ★ 结果读**最后一条 qTD 本体**（硬件把状态/residue 写回 qTD，Linux 也是读这里）；
    //   只有在"链根本没走到最后一条"（它还 ACTIVE）时才退回读 overlay 的 token。
    const uint32_t tok = (last->token & EHCI_QTD_ACTIVE) ? g_qh->token : last->token;
    const uint32_t sts = op_rd32(EHCI_OP_USBSTS);
    op_wr32(EHCI_OP_USBSTS, sts & (EHCI_STS_USBINT | EHCI_STS_USBERR | EHCI_STS_PCD | EHCI_STS_FLR | EHCI_STS_HSE));
    async_sched_off();
    // 收尾：把整条链标成不活跃（免得下次误把硬件半成品当成本次结果）+ 覆盖区清空
    for (volatile EhciQtd* q = first; q && (uint32_t)(uintptr_t)q != EHCI_QTD_TERM; ) {
        q->token &= ~EHCI_QTD_ACTIVE;
        const uint32_t nx = q->next;
        if (nx & EHCI_QTD_TERM) break;
        if (nx == 0) break;
        if (nx == (uint32_t)(uintptr_t)q) break;
        q = (volatile EhciQtd*)(uintptr_t)(nx & ~0x1Fu);
    }
    g_qh->cur   = EHCI_QTD_TERM;
    g_qh->next  = EHCI_QTD_TERM;
    g_qh->token = 0;
    __asm__ volatile("" ::: "memory");
    g_xfers++;
    if (out_token) *out_token = tok;
    if (out_sts) *out_sts = sts;
    if (!done) return -1;
    if (tok & (EHCI_QTD_HALTED | EHCI_QTD_DBE | EHCI_QTD_BABBLE | EHCI_QTD_XACT)) return -1;
    return 0;
}

// ==================== 控制传输（SETUP / DATA / STATUS）====================
// 语义与 UHCI 一致：返回 0 = 成功；-1 = 超时/硬件错；-2 = 设备 STALL。
// 高速设备的 EP0：mps 8/16/32/64（从设备描述符读）；DTC=0（toggle 由控制器管），
// 控制端点高速 + mps=64 时置 C 位（EHCI 规范的硬要求，见 qh epchar 定义）。
static int ehci_control(uint8_t addr, uint8_t mps, uint8_t rt, uint8_t req, uint16_t val,
                        uint16_t idx, uint16_t len, uint8_t* data, uint16_t* out_len) {
    if (out_len) *out_len = 0;
    if (!g_ready || !g_qh || !g_setup || !g_desc) return -1;
    if (len > EHCI_CTL_BUF_BYTES) return -1;

    const uint32_t setup_pa = ehci_pa64(g_setup);
    uint32_t data_pa = 0;
    if (len > 0) {
        if (!data) return -1;
        data_pa = ehci_pa64(data);
        if (data_pa == 0 && data != g_desc) return -1;         // 控制数据只走页池/镜像缓冲
    }

    uint8_t* su = g_setup;
    su[0] = rt; su[1] = req;
    su[2] = (uint8_t)(val & 0xFFu); su[3] = (uint8_t)(val >> 8);
    su[4] = (uint8_t)(idx & 0xFFu); su[5] = (uint8_t)(idx >> 8);
    su[6] = (uint8_t)(len & 0xFFu); su[7] = (uint8_t)(len >> 8);
    __asm__ volatile("" ::: "memory");

    const bool in_dir = (rt & 0x80u) != 0;
    const uint32_t epchar = (uint32_t)(addr & 0x7Fu) | ((uint32_t)mps << EHCI_QH_EC_MPS_SHIFT) |
                            EHCI_QH_EC_SPEED_HIGH |
                            ((mps == 64u) ? EHCI_QH_EC_C : 0u);   // DTC=0：toggle 归控制器
    const uint32_t epcap = 0x01u;                             // S-mask=1、C-mask=0（高速，T/TT 无关）

    // ---- 组装 qTD 链：SETUP -> [DATA...] -> STATUS ----
    int slot = 0;
    volatile EhciQtd* q = qtd_slot(slot);
    volatile EhciQtd* first = q;
    volatile EhciQtd* last = q;
    // SETUP：PID=SETUP，8 字节，ACTIVE，IOC=0
    {
        uint32_t tok = EHCI_QTD_PID_SETUP | EHCI_QTD_CERR_3;
        // 占位：next 稍后回填
        qtd_fill(q, 0, true, tok, setup_pa, 8);
        last = q;
    }
    // DATA：按 mps 分包（≤4KB 一条 qTD；控制缓冲只有 256B，最多 4 包）
    uint32_t sent = 0;
    const uint32_t mps_u = (mps ? mps : 8u);
    while (sent < len) {
        uint32_t n = len - sent;
        if (n > mps_u) n = mps_u;
        if (n > EHCI_QTD_MAX_BYTES) n = EHCI_QTD_MAX_BYTES;
        if (++slot >= EHCI_QTD_COUNT) return -1;
        volatile EhciQtd* nx = qtd_slot(slot);
        const uint32_t tok = (in_dir ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) | EHCI_QTD_CERR_3;
        qtd_fill(nx, 0, true, tok, data_pa + sent, n);
        // 前一条的 next 指向这一条（发布顺序：先填好新 qTD，再链上前一条）
        q->next = (uint32_t)(uintptr_t)nx & ~0x1Fu;
        __asm__ volatile("" ::: "memory");
        q = nx; last = q;
        sent += n;
    }
    // STATUS：方向与数据阶段相反；零长度；IOC=1（完成信号）
    if (++slot >= EHCI_QTD_COUNT) return -1;
    {
        volatile EhciQtd* nx = qtd_slot(slot);
        const uint32_t pid = in_dir ? EHCI_QTD_PID_OUT : EHCI_QTD_PID_IN;
        // 零长度：缓冲区指针仍要给一个合法物理地址（页池里的哨兵）
        qtd_fill(nx, 0, true, pid | EHCI_QTD_CERR_3 | EHCI_QTD_IOC, (uint32_t)(uintptr_t)g_sector, 0);
        q->next = (uint32_t)(uintptr_t)nx & ~0x1Fu;
        __asm__ volatile("" ::: "memory");
        last = nx;
    }

    uint32_t tok = 0, sts = 0;
    const int rc = ehci_exec_chain(first, last, epchar, epcap, EHCI_CTL_TIMEOUT_MS, &tok, &sts);
    if (rc != 0) {
        elog_begin();
        dbg64_str("[EHCI] control req=");
        ehex((uint32_t)req, 2);
        dbg64_str(" FAILED (");
        dbg64_str((tok & EHCI_QTD_HALTED) ? "halted" : "timeout");
        dbg64_str(") tok=");  ehex(tok, 8);
        dbg64_str(" sts=");   ehex(sts, 8);
        elog_end();
        return -1;
    }
    if (tok & EHCI_QTD_HALTED) return -2;                     // 设备 STALL（或端点 halt）
    if (out_len) {
        const uint32_t left = (tok & EHCI_QTD_LEN_MASK) >> EHCI_QTD_LEN_SHIFT;
        *out_len = (left >= len) ? 0 : (uint16_t)(len - left);
    }
    return 0;
}

// ==================== 批量传输 =====================
// 一次调用 = 一条 qTD 链；toggle 由调用方维护（与 usb64 的 usb_bulk64 语义一致）。
// 返回 0 = 成功；1 = 超时；2 = 设备 NAK/停顿（这里用 halted 表示）；3 = 其他硬件错。
// *got = 实际搬了多少字节（按最后一条 qTD 的剩余长度反推；短包只在最后一条上被看到）。
static int ehci_bulk(uint8_t addr, uint8_t ep, uint16_t mps, bool in, uint8_t* toggle,
                     uint8_t* buf, uint32_t len, uint32_t* got) {
    if (got) *got = 0;
    if (!g_ready || !g_qh) return 3;
    if (len == 0) return 0;
    const uint32_t mps_u = (mps ? mps : 64u);
    const uint32_t pa = ehci_pa64(buf);
    if (pa == 0) return 3;                                    // 调用方必须保证 DMA 可达（BOT 层已兜底）

    const uint32_t epchar = (uint32_t)(addr & 0x7Fu) | ((uint32_t)(ep & 0x0Fu) << 8) |
                            ((uint32_t)mps_u << EHCI_QH_EC_MPS_SHIFT) | EHCI_QH_EC_SPEED_HIGH |
                            EHCI_QH_EC_DTC;                            // DTC=1：toggle 由软件维护
    // DTC=1：toggle 由软件维护（见文件头"Bulk-Only Transport 的 toggle 语义"）
    const uint32_t epcap = 0x01u;
    const bool dt1 = (toggle && *toggle);

    int slot = 0;
    volatile EhciQtd* first = nullptr;
    volatile EhciQtd* q = nullptr;
    uint32_t sent = 0;
    while (sent < len) {
        uint32_t n = len - sent;
        if (n > mps_u) n = mps_u;
        if (n > EHCI_QTD_MAX_BYTES) n = EHCI_QTD_MAX_BYTES;
        if (slot >= EHCI_QTD_COUNT) return 3;
        volatile EhciQtd* nx = qtd_slot(slot);
        // 每包的 DATA0/DATA1 按"包序号"翻转：n 是包大小，只有 mps 整数包才翻转；
        // 最后一包若不是整数包，toggle 停在它用的那一位（BOT 的 CSW 恒 DATA1 由调用方另行指定）
        const bool dt = dt1 ^ ((sent / mps_u) & 1u);
        uint32_t tok = (in ? EHCI_QTD_PID_IN : EHCI_QTD_PID_OUT) | EHCI_QTD_CERR_3;
        if (dt) tok |= EHCI_QTD_DT;
        qtd_fill(nx, 0, true, tok, pa + sent, n);
        if (!first) first = nx;
        if (q) { q->next = (uint32_t)(uintptr_t)nx & ~0x1Fu; }
        __asm__ volatile("" ::: "memory");
        q = nx;
        sent += n;
        slot++;
    }
    if (!first || !q) return 3;
    q->token |= EHCI_QTD_IOC;                                 // 最后一条带 IOC = 完成信号
    __asm__ volatile("" ::: "memory");

    uint32_t tok = 0, sts = 0;
    const int rc = ehci_exec_chain(first, q, epchar, epcap, EHCI_BULK_TIMEOUT_MS, &tok, &sts);
    const uint32_t left = (tok & EHCI_QTD_LEN_MASK) >> EHCI_QTD_LEN_SHIFT;
    if (got) *got = (left >= len) ? 0u : (len - left);
    if (rc != 0) return (tok & EHCI_QTD_HALTED) ? 2 : 1;
    // 更新 toggle：整数包的个数决定翻转次数（BOT 里每包 512B，len 是 512 的整数倍）
    if (toggle) {
        const uint32_t packets = (len + mps_u - 1u) / mps_u;
        *toggle = (uint8_t)(dt1 ^ (packets & 1u));
    }
    return 0;
}

// ==================== 端口 =====================
static const char* psc_speed_name(uint32_t v) {
    switch ((v & EHCI_PSC_SPEED_MASK) >> EHCI_PSC_SPEED_SHIFT) {
    case 0: return "full";
    case 1: return "low";
    case 2: return "high";
    default: return "?";
    }
}

// 打印一行 `[EHCI] port <n> owner=<0|1> speed=<..> ccs=<0|1>`（启动期与热插拔都用同一行格式）
static void port_log(uint32_t n, uint32_t v) {
    elog_begin();
    dbg64_str("[EHCI] port ");
    dbg64_dec((uint64_t)n);
    dbg64_str(" owner=");
    dbg64_dec((v & EHCI_PSC_PO) ? 1u : 0u);
    dbg64_str(" speed=");
    dbg64_str((v & EHCI_PSC_PED) ? psc_speed_name(v) : "full");
    dbg64_str(" ccs=");
    dbg64_dec((v & EHCI_PSC_CCS) ? 1u : 0u);
    elog_end();
}

// 复位一个端口（只在 owner=0 且 CCS=1 时调用）。返回 true = PED 置起来（高速链路就绪）。
// 流程（EHCI 1.0 §4.2.4 + Linux ehci-hub.c / ehci_reset_port 同款，**这一步本轮才修对**）：
//   1) 端口电源：PP=0 就先打开（**不看 PPC** —— PPC=0 表示 PP 恒为 1，读 0 就补写一次，不吃亏）；
//   2) PR=1 **保持 50ms**（规范要求 ≥10ms）—— 不是"写一下等它自清"；
//   3) **显式写 PR=0**（复位结束；高速设备这一步之后由控制器置 PED）。
//      ★ 实测（QEMU 11.1 usb-ehci + PORTSC trace）：**控制器不会自清 PR** —— 旧代码"等 PR 自清"
//      的循环（200 万次 MMIO 读）永远等不到，整条启动链就卡在 ehci64_init64() 里
//      （串口停在 `[EHCI] port 1 owner=0 speed=full ccs=1`，其它线程照常跑，无 PANIC）。
//      规范的顺序是软件**自己**把 PR 写回 0，SeaBIOS/Linux 都这么做。
//   4) 等 PED（高速设备复位后才被使能）：**双上界** —— 时间 100ms + 自旋次数上界（有界，绝不挂死）；
//      旧代码固定 10 万次 nop_pause（≈ 几毫秒）会误判成"不是高速设备"，把 U 盘交还伴随控制器。
//   5) 清变化位时**保留 PED**（见 port_clear_changes —— 写 0 会把刚使能的端口关掉）。
// **不碰 FPR/SUSP**。
static bool port_reset(uint32_t n, uint32_t* out_v) {
    uint32_t v = port_rd(n);
    if (out_v) *out_v = v;
    if ((v & EHCI_PSC_PO) || !(v & EHCI_PSC_CCS)) return false;
    if (!(v & EHCI_PSC_PP)) {                                // 端口电源（PPC=0 时按规范 PP 恒为 1，读 0 就补写一次）
        port_wr(n, EHCI_PSC_PP);
        edelay_ms(20);
    }
    port_wr(n, EHCI_PSC_PR | EHCI_PSC_CSC | EHCI_PSC_PEDC);  // PR=1 开始复位（PED 写 0：复位前先关端口）
    edelay_ms(50);                                           // 规范要求 PR 至少保持 10ms；与 Linux 同款取 50ms
    port_wr(n, EHCI_PSC_CSC | EHCI_PSC_PEDC);                // ★ 显式写 PR=0（不等自清）
    {                                                        // 等 PED（高速设备才会置起来）
        const uint64_t t0 = g_ticks64;
        const uint64_t want = ms_to_ticks64(100);
        uint32_t spins = 0;
        for (;;) {
            v = port_rd(n);
            if (v & EHCI_PSC_PED) break;
            if ((g_ticks64 - t0) >= want || ++spins > EHCI_PED_SPINS) break;   // ★ 双上界
            nop_pause();
        }
    }
    port_clear_changes(n);                                   // 清变化位（★ PED 原样保留）
    if (out_v) *out_v = v;
    return (v & EHCI_PSC_PED) != 0;
}

// ==================== 枚举（一台 BOT 存储）====================
static int enum_fail(const char* stage, int rc) {
    elog_begin();
    dbg64_str("[EHCI] enum FAILED stage=");
    dbg64_str(stage);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(rc < 0 ? (uint32_t)(-rc) : (uint32_t)rc));
    elog_end();
    return rc;
}

static void e_log_str(const char* s, int n) {
    int last = n - 1;
    while (last >= 0 && (s[last] == ' ' || s[last] == 0)) last--;
    for (int i = 0; i <= last; i++) {
        const char ch = s[i];
        dbg64_putc((ch >= 0x20 && ch < 0x7F) ? ch : '.');
    }
}

// 枚举一台设备（地址 addr 由调用方分配；mps 从设备描述符读）。返回 0 = 存储接口就绪。
static int ehci_enum_storage(uint8_t addr) {
    uint8_t* buf = g_desc;
    uint16_t got = 0;

    // ---- 1) 设备描述符前 8 字节：拿 EP0 最大包 ----
    memset_64(buf, 0, 64);
    if (ehci_control(0, 8, 0x80, 6 /*GET_DESCRIPTOR*/, 0x0100, 0, 8, buf, &got) != 0) return enum_fail("get-device-8", -1);
    if (got < 8) return enum_fail("get-device-8-short", -2);
    uint8_t mps = buf[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;
    elog_begin();
    dbg64_str("[EHCI] control setup 06 ok");                  // bRequest=0x06 GET_DESCRIPTOR
    elog_end();

    // ---- 2) SET_ADDRESS ----
    if (ehci_control(0, mps, 0x00, 5 /*SET_ADDRESS*/, addr, 0, 0, nullptr, nullptr) != 0) return enum_fail("set-address", -3);
    edelay_ms(2);
    elog_begin();
    dbg64_str("[EHCI] control setup 05 ok");
    elog_end();
    elog_begin();
    dbg64_str("[EHCI] set address=");
    dbg64_dec((uint64_t)addr);
    dbg64_str(" ok");
    elog_end();

    // ---- 3) 设备描述符 18 字节（厂商/产品）----
    memset_64(buf, 0, 64);
    if (ehci_control(addr, mps, 0x80, 6, 0x0100, 0, 18, buf, &got) != 0 || got < 18) return enum_fail("get-device-18", -4);
    const uint16_t vendor  = (uint16_t)(buf[8] | ((uint16_t)buf[9] << 8));
    const uint16_t product = (uint16_t)(buf[10] | ((uint16_t)buf[11] << 8));
    elog_begin();
    dbg64_str("[EHCI] device addr=");
    dbg64_dec((uint64_t)addr);
    dbg64_str(" mps=");
    dbg64_dec((uint64_t)mps);
    dbg64_str(" vendor=");
    ehex(vendor, 4);
    dbg64_str(" product=");
    ehex(product, 4);
    elog_end();

    // ---- 4) 配置描述符前 9 字节 -> 总长 ----
    memset_64(buf, 0, 64);
    if (ehci_control(addr, mps, 0x80, 6, 0x0200, 0, 9, buf, &got) != 0 || got < 9) return enum_fail("get-config-9", -5);
    uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    if (total < 9 || total > 256) return enum_fail("config-len", -6);

    // ---- 5) 整份配置描述符，遍历找 BOT 存储接口 ----
    memset_64(buf, 0, 256);
    if (ehci_control(addr, mps, 0x80, 6, 0x0200, 0, total, buf, &got) != 0 || got < 9) return enum_fail("get-config-full", -7);
    uint32_t n_ifaces = 0;
    uint8_t  ep_in = 0, ep_out = 0;
    uint16_t mps_in = 0, mps_out = 0;
    bool msc_iface = false;
    {
        uint32_t o = 0;
        while (o + 2u <= (uint32_t)got) {
            const uint8_t blen = buf[o];
            const uint8_t btype = buf[o + 1];
            if (blen < 2u) break;
            if (btype == 4u) {                                  // INTERFACE
                n_ifaces++;
                if (o + 9u <= (uint32_t)got && buf[o + 5] == 0x08u && buf[o + 6] == 0x06u && buf[o + 7] == 0x50u)
                    msc_iface = true;
                else
                    msc_iface = false;
            } else if (btype == 5u && msc_iface && o + 7u <= (uint32_t)got) {   // ENDPOINT
                const uint8_t epa = buf[o + 2];
                const uint16_t mx = (uint16_t)((buf[o + 4] | ((uint16_t)buf[o + 5] << 8)) & 0x7FFu);
                const uint8_t attr = buf[o + 3] & 0x03u;
                if (attr == 0x02u) {                            // BULK
                    if (epa & 0x80u) { ep_in = epa; mps_in = mx; }
                    else             { ep_out = epa; mps_out = mx; }
                }
            }
            o += blen;
        }
    }
    (void)vendor; (void)product;
    if (!msc_iface || ep_in == 0 || ep_out == 0) return enum_fail("no-msc-interface", -8);
    if (mps_in == 0) mps_in = 512;
    if (mps_out == 0) mps_out = 512;

    // ---- 6) SET_CONFIGURATION(1) ----
    if (ehci_control(addr, mps, 0x00, 9 /*SET_CONFIG*/, 1, 0, 0, nullptr, nullptr) != 0) return enum_fail("set-config", -9);
    elog_begin();
    dbg64_str("[EHCI] control setup 09 ok");
    elog_end();

    elog_begin();
    dbg64_str("[USBST] iface found class=08 sub=06 proto=50 ep_in=");
    ehex(ep_in, 2);
    dbg64_str(" ep_out=");
    ehex(ep_out, 2);
    elog_end();
    elog_begin();
    dbg64_str("[EHCI] config set value=1 ifaces=");
    dbg64_dec((uint64_t)n_ifaces);
    dbg64_str(" msc=1 ep_in=");
    ehex(ep_in, 2);
    dbg64_str(" ep_out=");
    ehex(ep_out, 2);
    dbg64_str(" mps=");
    dbg64_dec((uint64_t)mps_in);
    elog_end();

    g_msc.present  = true;
    g_msc.addr     = addr;
    g_msc.ep_in    = ep_in;
    g_msc.ep_out   = ep_out;
    g_msc.mps_in   = mps_in;
    g_msc.mps_out  = mps_out;
    g_msc.tag      = 0;
    g_msc.block_size = 0;
    g_msc.blocks   = 0;
    return 0;
}

// ==================== BOT（Bulk-Only Transport）+ SCSI 子集 ====================
#define BOT_CBW_SIG   0x43425355u
#define BOT_CSW_SIG   0x53425355u
#define BOT_CBW_LEN   31u
#define BOT_CSW_LEN   13u
#define BOT_DIR_IN    0x80u

static uint32_t rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// 前置声明：sense 自己也是一条 BOT 命令（定义在下面，见 ehci_bot）
static int ehci_bot(uint8_t* cdb, uint8_t cdb_len, bool dir_in, uint32_t data_len, uint8_t* data);
// REQUEST SENSE(0x03)：出错时读 18 字节 SENSE 并把 key/asc/ascq 打进串口（定位用）。
// ★ 递归保护：sense 自己失败时**不再**套一层 sense（否则会互相递归）。
static bool g_in_sense = false;
static void msc_log_sense(const char* what) {
    if (g_in_sense) return;
    g_in_sense = true;
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x03;
    cdb[4] = 18;
    memset_64(g_scratch, 0, 18);
    const int r = ehci_bot(cdb, 6, true, 18, g_scratch);
    g_in_sense = false;
    elog_begin();
    dbg64_str("[USBST] sense after=");
    dbg64_str(what);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(r < 0 ? 99u : (uint32_t)r));
    if (r == 0 && g_scratch[0] == 0x70u) {                    // 固定格式 SENSE 数据
        dbg64_str(" key=");
        ehex((uint32_t)(g_scratch[2] & 0x0Fu), 1);
        dbg64_str(" asc=");
        ehex(g_scratch[12], 2);
        dbg64_str(" ascq=");
        ehex(g_scratch[13], 2);
    }
    elog_end();
}

// 一条 BOT 命令：CBW -> [数据阶段] -> CSW。返回 0 = 通过；1/2 = CSW 报的命令失败/阶段错；
// <0 = 传输层失败（见 g_msc.last_reason）。语义与 usb64 的 usb_bot64 **完全一致**：
//   * CBW 恒 DATA0、数据阶段从 DATA0 起逐包翻转、CSW 恒 DATA1；
//   * CSW 签名/Tag 不对 = 阶段错；bCSWStatus != 0 = 命令失败；dCSWDataResidue != 0 = 数据没搬完 -> 失败。
static int ehci_bot(uint8_t* cdb, uint8_t cdb_len, bool dir_in, uint32_t data_len, uint8_t* data) {
    EhciMsc& m = g_msc;
    if (!m.present) { m.last_reason = 5; return -1; }
    if (data_len > EHCI_MSC_MAX_DATA) { m.last_reason = 4; return -1; }

    // ---- CBW（31 字节，DATA0）----
    uint8_t* cbw = g_cbw;
    for (int i = 0; i < (int)BOT_CBW_LEN; i++) cbw[i] = 0;
    wr32le(cbw + 0, BOT_CBW_SIG);
    m.tag++;
    wr32le(cbw + 4, m.tag);
    wr32le(cbw + 8, data_len);
    cbw[12] = dir_in ? (uint8_t)BOT_DIR_IN : 0;
    cbw[13] = 0;                                              // LUN 0
    cbw[14] = (uint8_t)(cdb_len & 0x1Fu);
    for (int i = 0; i < 16; i++) cbw[15 + i] = (i < (int)cdb_len) ? cdb[i] : 0;

    uint8_t  tg = 0;
    uint32_t got = 0;
    int r = ehci_bulk(m.addr, m.ep_out, m.mps_out, false, &tg, cbw, BOT_CBW_LEN, &got);
    if (r != 0 || got != BOT_CBW_LEN) {
        m.last_reason = (r == 1) ? 2 : 3;
        return -1;
    }

    // ---- 数据阶段（toggle 从 DATA0 起；短包由 CSW 的 residue 判定）----
    if (data_len > 0 && data) {
        tg = 0;
        got = 0;
        r = ehci_bulk(m.addr, dir_in ? m.ep_in : m.ep_out, dir_in ? m.mps_in : m.mps_out,
                      dir_in, &tg, data, data_len, &got);
        if (r != 0) { m.last_reason = (r == 1) ? 2 : 3; return -1; }
        if (got == 0) { m.last_reason = 1; return -1; }
    }

    // ---- CSW（13 字节，DATA1）----
    tg = 1;
    got = 0;
    r = ehci_bulk(m.addr, m.ep_in, m.mps_in, true, &tg, g_csw, BOT_CSW_LEN, &got);
    if (r != 0 || got != BOT_CSW_LEN) {
        m.last_reason = (r == 1) ? 2 : 3;
        return -1;
    }
    const uint32_t sig = rd32le(g_csw);
    const uint32_t tag = rd32le(g_csw + 4);
    m.csw_status  = g_csw[12];
    m.csw_residue = rd32le(g_csw + 8);
    if (sig != BOT_CSW_SIG || tag != m.tag) { m.last_reason = 1; return -1; }
    if (m.csw_status != 0) { m.last_reason = 1; return (int)m.csw_status; }
    if (m.csw_residue != 0) { m.last_reason = 6; return -2; }
    m.last_reason = 0;
    return 0;
}

// INQUIRY(0x12)：36 字节标准查询数据
static int msc_inquiry() {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x12;
    cdb[4] = 36;
    memset_64(g_scratch, 0, 36);
    const int r = ehci_bot(cdb, 6, true, 36, g_scratch);
    if (r != 0) { msc_log_sense("inquiry"); return -1; }
    for (int i = 0; i < 8; i++)  g_msc.vendor[i]  = (char)g_scratch[8 + i];
    g_msc.vendor[8] = 0;
    for (int i = 0; i < 16; i++) g_msc.product[i] = (char)g_scratch[16 + i];
    g_msc.product[16] = 0;
    elog_begin();
    dbg64_str("[USBST] inquiry vendor=");
    e_log_str(g_msc.vendor, 8);
    dbg64_str(" product=");
    e_log_str(g_msc.product, 16);
    dbg64_str((g_scratch[1] & 0x80u) ? " rmb=1" : " rmb=0");
    elog_end();
    return 0;
}

// READ CAPACITY(10)(0x25)：8 字节（最后 LBA(4, 大端) + 块大小(4, 大端)）
static int msc_capacity() {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x25;
    memset_64(g_scratch, 0, 8);
    if (ehci_bot(cdb, 10, true, 8, g_scratch) != 0) { msc_log_sense("read-capacity"); return -1; }
    const uint32_t last = rd32be(g_scratch);
    const uint32_t bs   = rd32be(g_scratch + 4);
    g_msc.blocks = last + 1u;
    g_msc.block_size = bs;
    const uint64_t bytes = (uint64_t)g_msc.blocks * (uint64_t)bs;
    elog_begin();
    dbg64_str("[USBST] capacity blocks=");
    dbg64_dec((uint64_t)g_msc.blocks);
    dbg64_str(" block_size=");
    dbg64_dec((uint64_t)bs);
    dbg64_str(" bytes=");
    dbg64_dec(bytes);
    dbg64_str(" cap_mb=");
    dbg64_dec(bytes / (1024ull * 1024ull));
    if (bytes >= (1024ull * 1024ull * 1024ull)) {
        const uint64_t gb100 = (bytes * 100ull) / (1024ull * 1024ull * 1024ull);
        dbg64_str(" cap_gb=");
        dbg64_dec(gb100 / 100ull);
        dbg64_putc('.');
        const uint64_t frac = gb100 % 100ull;
        dbg64_putc((char)('0' + (frac / 10ull)));
        dbg64_putc((char)('0' + (frac % 10ull)));
    }
    dbg64_str(" (EHCI)");
    elog_end();
    return 0;
}

// 越界判定（读/写共用）：lba + count > blocks 一律先拒绝（发任何 SCSI 命令之前）。
static bool msc_range_ok(uint32_t lba, uint32_t count, bool log_read) {
    if (g_msc.blocks == 0) return false;
    if (lba > g_msc.blocks || count > g_msc.blocks - lba) {
        if (log_read) {
            g_msc.reads_fail++;
            if (g_usbst_fail_logs < EHCI_FAIL_LOG_MAX) {
                g_usbst_fail_logs++;
                elog_begin();
                dbg64_str("[USBST] read FAILED lba=");
                dbg64_dec((uint64_t)lba);
                dbg64_str(" count=");
                dbg64_dec((uint64_t)count);
                dbg64_str(" reason=range (out of capacity blocks=");
                dbg64_dec((uint64_t)g_msc.blocks);
                dbg64_str(")");
                elog_end();
            }
        }
        return false;
    }
    return true;
}

// READ(10)(0x28)：lba/count 单位是 512 字节扇区
static int msc_read10(uint32_t lba, uint16_t count, uint8_t* buf) {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    wr16be(cdb + 7, count);
    const uint32_t bytes = (uint32_t)count * 512u;
    const int r = ehci_bot(cdb, 10, true, bytes, buf);
    if (r != 0) {
        g_msc.reads_fail++;
        if (g_usbst_fail_logs < EHCI_FAIL_LOG_MAX) {
            g_usbst_fail_logs++;
            elog_begin();
            dbg64_str("[USBST] read FAILED lba=");
            dbg64_dec((uint64_t)lba);
            dbg64_str(" count=");
            dbg64_dec((uint64_t)count);
            dbg64_str(" reason=");
            dbg64_str(r > 0 ? "csw status" : ehci64_msc_last_reason64());
            if (r > 0) { dbg64_str(" csw="); dbg64_dec((uint64_t)g_msc.csw_status); }
            elog_end();
        }
        return -1;
    }
    g_msc.reads_ok++;
    if (g_usbst_read_logs < EHCI_READ_LOG_MAX) {
        g_usbst_read_logs++;
        elog_begin();
        dbg64_str("[USBST] read lba=");
        dbg64_dec((uint64_t)lba);
        dbg64_str(" count=");
        dbg64_dec((uint64_t)count);
        dbg64_str(" ok (EHCI)");
        elog_end();
    }
    return 0;
}

// WRITE(10)(0x2A)：数据阶段是**主机 -> 设备**
static int msc_write10(uint32_t lba, uint16_t count, const uint8_t* buf) {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x2A;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    wr16be(cdb + 7, count);
    const uint32_t bytes = (uint32_t)count * 512u;
    const int r = ehci_bot(cdb, 10, false, bytes, (uint8_t*)(uintptr_t)buf);
    if (r != 0) {
        g_msc.writes_fail++;
        if (g_usbst_wfail_logs < EHCI_FAIL_LOG_MAX) {
            g_usbst_wfail_logs++;
            elog_begin();
            dbg64_str("[USBST] write FAILED lba=");
            dbg64_dec((uint64_t)lba);
            dbg64_str(" count=");
            dbg64_dec((uint64_t)count);
            dbg64_str(" reason=");
            dbg64_str(r > 0 ? "csw status" : ehci64_msc_last_reason64());
            dbg64_str(" residue=");
            dbg64_dec((uint64_t)g_msc.csw_residue);
            dbg64_str(" dir=out");
            elog_end();
        }
        return -1;
    }
    g_msc.writes_ok++;
    if (g_usbst_wlogs < EHCI_WRITE_LOG_MAX) {
        g_usbst_wlogs++;
        elog_begin();
        dbg64_str("[USBST] write lba=");
        dbg64_dec((uint64_t)lba);
        dbg64_str(" count=");
        dbg64_dec((uint64_t)count);
        dbg64_str(" ok (EHCI)");
        elog_end();
    }
    return 0;
}

// ==================== 探测（INQUIRY / TUR / READ CAPACITY / READ(10) / 边界探针）====================
// 自检位（[EHCI] selftest mask= 与 [USBST] selftest 共用一套）：
//   bit0 INQUIRY 失败   bit1 READ CAPACITY 失败   bit2 TUR 失败   bit3 块大小不是 512
//   bit4 READ(10) LBA0 失败   bit5 LBA0 读回全 0（可疑）
//   bit6 越界读的边界探针没被拒（读路径守卫坏了才置位）
static void msc_probe() {
    uint32_t mask = 0;
    if (!g_msc.present) return;
    g_msc.supported = false;
    g_msc_probe_done = true;

    if (msc_inquiry() != 0) mask |= 1u;
    {
        uint8_t cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x00;                                        // TEST UNIT READY
        int r = -1;
        for (int t = 0; t < 3 && r != 0; t++) {
            r = ehci_bot(cdb, 6, false, 0, nullptr);
            if (r != 0) edelay_ms(60);
        }
        if (r != 0) { msc_log_sense("test-unit-ready"); mask |= 4u; }
    }
    if (msc_capacity() != 0) mask |= 2u;
    if (g_msc.present && g_msc.block_size != 512u) {
        mask |= 8u;
        elog_begin();
        dbg64_str("[USBST] block_size=");
        dbg64_dec((uint64_t)g_msc.block_size);
        dbg64_str(" != 512 -> not exposed as a block device (this batch only moves 512-byte blocks)");
        elog_end();
    }
    if (mask == 0) {
        memset_64(g_sector, 0, 512);
        if (msc_read10(0, 1, g_sector) != 0) {
            mask |= 16u;
        } else {
            bool all0 = true;
            for (uint32_t i = 0; i < 512u; i++) if (g_sector[i] != 0) { all0 = false; break; }
            if (all0) mask |= 32u;
        }
    }
    // ★ 越界读的**边界探针**（与 UHCI 的 write-bounds probe 同一条纪律）：
    //   这两次调用走的是与上层读**完全同一条** ehci64_msc_read64()（同一份范围判定），
    //   lba 落在盘外 -> 在发任何 SCSI 命令之前就返回 false，不碰介质、不改盘上任何字节。
    if (mask == 0) {
        const bool r1 = ehci64_msc_read64(0, g_msc.blocks, 1, g_sector);
        const bool r2 = ehci64_msc_read64(0, g_msc.blocks - 1u, 2u, g_sector);
        elog_begin();
        dbg64_str("[USBST] read-bounds probe blocks=");
        dbg64_dec((uint64_t)g_msc.blocks);
        dbg64_str(" lba=blocks rejected=");
        dbg64_dec(r1 ? 0u : 1u);
        dbg64_str(" lba=blocks-1 count=2 rejected=");
        dbg64_dec(r2 ? 0u : 1u);
        elog_end();
        if (r1 || r2) mask |= 64u;                            // 守卫坏了（越界读被放行）
    }
    // ★ 越界写的**边界探针**（与 UHCI 的 write-bounds probe 完全同一条纪律，位号也一致 = bit7）：
    //   两次调用走的是与上层写**同一条** ehci64_msc_write64()（同一份范围判定），lba 落在盘外 ->
    //   在发任何 SCSI 命令之前就返回 false：不写介质、不改盘上任何字节（场景 ③ 的"盘镜像 CRC 不变"靠它）。
    //   注意：必须在 supported 置 true 之前调用 —— ehci64_msc_write64() 里带 supported 守卫，
    //   所以这里先临时把 supported 置 true（探针自己的范围判定与 supported 无关；下面会按 mask 重设）。
    if (mask == 0) {
        g_msc.supported = true;                               // 只为让探针走进"范围判定"那一层
        const bool w1 = ehci64_msc_write64(0, g_msc.blocks, 1, g_sector);
        const bool w2 = ehci64_msc_write64(0, g_msc.blocks - 1u, 2u, g_sector);
        g_msc.supported = false;
        elog_begin();
        dbg64_str("[USBST] write-bounds probe blocks=");
        dbg64_dec((uint64_t)g_msc.blocks);
        dbg64_str(" lba=blocks rejected=");
        dbg64_dec(w1 ? 0u : 1u);
        dbg64_str(" lba=blocks-1 count=2 rejected=");
        dbg64_dec(w2 ? 0u : 1u);
        elog_end();
        if (w1 || w2) mask |= 128u;                           // 守卫坏了（越界写被放行）
    }
    g_msc.supported = (mask == 0);
    g_msc.selftest_mask = mask;
    elog_begin();
    if (mask == 0) {
        dbg64_str("[USBST] selftest PASS mask=0 (EHCI)");
    } else {
        dbg64_str("[USBST] selftest FAIL mask=");
        dbg64_dec((uint64_t)mask);
        dbg64_str(" (EHCI)");
    }
    elog_end();
}

// ==================== 端口扫描（启动期）====================
static void ehci_scan_ports_boot() {
    uint8_t addr = 1;
    g_conn_seen = 0;
    g_ports_seen = 0;
    g_link_ok = 0;
    for (uint32_t n = 1; n <= g_ports && n <= EHCI_MAX_PORTS; n++) {
        uint32_t v = port_rd(n);
        port_log(n, v);
        if (v & EHCI_PSC_PO) continue;                        // 伴随控制器的端口：不抢
        if (!(v & EHCI_PSC_CCS)) {
            port_clear_changes(n);                            // 清变化位（不留挂起；★ 保留 PED —— 别关掉正在用的端口）
            continue;
        }
        g_conn_seen++;
        uint32_t rv = 0;
        const bool ped = port_reset(n, &rv);
        if (!ped) {
            // 复位后 PED 没起来 = 设备不是高速：写 PortOwner=1 交还伴随控制器（本驱动只做高速）。
            port_give_to_companion(n);                        // ★ 真正交还（见 port_give_to_companion）
            elog_begin();
            dbg64_str("[EHCI] port ");
            dbg64_dec((uint64_t)n);
            dbg64_str(" owner=1 speed=");
            dbg64_str(psc_speed_name(rv));
            dbg64_str(" ccs=1 (full/low speed: handed to the companion controller)");
            elog_end();
            continue;
        }
        g_link_ok++;
        elog_begin();
        dbg64_str("[EHCI] port ");
        dbg64_dec((uint64_t)n);
        dbg64_str(" reset ok speed=high ped=1");
        elog_end();
        if (g_msc.present || addr > 0x7Fu) {
            elog_begin();
            dbg64_str("[EHCI] port ");
            dbg64_dec((uint64_t)n);
            dbg64_str(" device skipped (this driver handles one USB storage device)");
            elog_end();
            continue;
        }
        g_ports_seen++;
        const int rc = ehci_enum_storage(addr);
        if (rc == 0) {
            addr++;
            g_devices++;
            msc_probe();
        } else {
            g_selftest_mask |= 8u;                            // 有设备但没就绪
        }
    }
}

// ==================== init ====================
static void ehci_selftest_log() {
    elog_begin();
    if (!g_found) {
        dbg64_str("[EHCI] selftest skipped (no controller)");
    } else if (g_selftest_mask) {
        dbg64_str("[EHCI] selftest FAIL mask=");
        dbg64_dec((uint64_t)g_selftest_mask);
    } else {
        dbg64_str("[EHCI] selftest PASS mask=0");
    }
    elog_end();
}

static int ehci_boot_fail(const char* why) {
    elog_begin();
    dbg64_str("[EHCI] not found (");
    dbg64_str(why);
    dbg64_str(")");
    elog_end();
    ehci_selftest_log();
    return -1;
}

int ehci64_init64() {
    if (g_inited) return g_ready ? 0 : -1;
    g_inited = true;
    memset_64(&g_msc, 0, sizeof(g_msc));

    // ---- 1) PCI + BAR0（MMIO）----
    if (!ehci_pci_find()) {
        g_state = EHCI_ST_NOT_FOUND;
        elog_begin();
        dbg64_str("[EHCI] not found");
        elog_end();
        ehci_selftest_log();
        return -1;
    }
    const uint32_t cmd = epci_rd32(g_bus, g_pcidev, g_fn, 0x04);
    epci_wr32(g_bus, g_pcidev, g_fn, 0x04, (cmd & 0xFFFF0000u) | 0x0006u);   // MEM + BUS MASTER
    const uint32_t b0 = epci_rd32(g_bus, g_pcidev, g_fn, 0x10);
    if (b0 & 0x1u) { g_state = EHCI_ST_NOT_FOUND; return ehci_boot_fail("BAR0 is IO (EHCI 规范要求 MMIO)"); }
    uint64_t bar = (uint64_t)(b0 & 0xFFFFFFF0u);
    if ((b0 & 0x6u) == 0x4u) {                                // 64 位 MMIO BAR
        const uint32_t b1 = epci_rd32(g_bus, g_pcidev, g_fn, 0x14);
        bar |= ((uint64_t)b1) << 32;
    }
    if (bar == 0 || bar >= 0x100000000ull) { g_state = EHCI_ST_NOT_FOUND; return ehci_boot_fail("bad BAR0"); }
    g_mmio = bar;
    g_reg  = (volatile uint32_t*)(uintptr_t)g_mmio;

    // ---- 2) 能力寄存器 ----
    const uint32_t cap0 = mm_rd32(EHCI_CAP_CAPLENGTH);
    g_caplen = cap0 & 0xFFu;
    g_hcs = mm_rd32(EHCI_CAP_HCSPARAMS);
    g_hcc = mm_rd32(EHCI_CAP_HCCPARAMS);
    g_dboff = mm_rd32(EHCI_CAP_DBOFF) & 0xFFFFu;
    g_ports = g_hcs & 0x0Fu;
    g_ppc = (g_hcs >> 4) & 0x1u;
    g_eecp = (g_hcc >> 8) & 0xFFu;
    elog_begin();
    dbg64_str("[EHCI] pci ");
    dbg64_dec((uint64_t)g_bus); dbg64_putc(':'); dbg64_dec((uint64_t)g_pcidev); dbg64_putc('.');
    dbg64_dec((uint64_t)g_fn);
    dbg64_str(" mmio=");
    ehex((uint32_t)g_mmio, 8);
    dbg64_str(" ports=");
    dbg64_dec((uint64_t)g_ports);
    dbg64_str(" caplen=");
    dbg64_dec((uint64_t)g_caplen);
    elog_end();
    if (g_caplen < 0x10u || g_caplen > 0x40u) { g_state = EHCI_ST_NOT_FOUND; return ehci_boot_fail("sanity: caplen"); }
    if (g_ports == 0 || g_ports > EHCI_MAX_PORTS) { g_state = EHCI_ST_NOT_FOUND; return ehci_boot_fail("sanity: n_ports"); }
    g_found = true;

    // ---- 3) BIOS handoff（有 EECP 才做）----
    (void)ehci_bios_handoff();

    // ---- 4) 软复位：USBCMD=0 -> 等 HCH -> HCRESET -> 等自清 ----
    op_wr32(EHCI_OP_USBINTR, 0);                              // 全程轮询：不开任何中断
    op_wr32(EHCI_OP_USBCMD, 0);
    {
        uint32_t spins = 0;
        while (!(op_rd32(EHCI_OP_USBSTS) & EHCI_STS_HCH) && spins < EHCI_RESET_SPINS) { spins++; nop_pause(); }
    }
    op_wr32(EHCI_OP_USBCMD, EHCI_CMD_HCRESET);
    {
        uint32_t spins = 0;
        while ((op_rd32(EHCI_OP_USBCMD) & EHCI_CMD_HCRESET) && spins < EHCI_RESET_SPINS) { spins++; nop_pause(); }
        if (op_rd32(EHCI_OP_USBCMD) & EHCI_CMD_HCRESET) {
            g_state = EHCI_ST_NOT_FOUND;
            g_selftest_mask |= 1u;
            return ehci_boot_fail("HCRESET did not self-clear");
        }
    }
    op_wr32(EHCI_OP_USBSTS, 0x3Fu);                           // 清状态位（写 1 清）
    op_wr32(EHCI_OP_CTRLDSSEG, 0);                            // 只支持前 4GB（DMA 结构全在页池/镜像里）
    op_wr32(EHCI_OP_CONFIGFLAG, 1);                            // ★ 所有端口路由给 EHCI
    elog_begin();
    dbg64_str("[EHCI] reset ok hcs=");
    ehex(g_hcs, 8);
    dbg64_str(" hcc=");
    ehex(g_hcc, 8);
    dbg64_str(" dboff=");
    dbg64_dec((uint64_t)g_dboff);
    elog_end();

    // ---- 5) DMA 结构（3 个页：异步表 + 数据 + 暂存）----
    g_p_async  = (uint8_t*)page_alloc_64();
    g_p_data   = (uint8_t*)page_alloc_64();
    g_p_bounce = (uint8_t*)page_alloc_64();
    if (!g_p_async || !g_p_data || !g_p_bounce) {
        g_state = EHCI_ST_NOT_FOUND;
        g_selftest_mask |= 1u;
        return ehci_boot_fail("out of pages");
    }
    memset_64(g_p_async, 0, PAGE_SIZE_64);
    memset_64(g_p_data, 0, PAGE_SIZE_64);
    memset_64(g_p_bounce, 0, PAGE_SIZE_64);
    g_qh      = (EhciQh*)(void*)g_p_async;
    g_setup   = g_p_data + 0x000;
    g_desc    = g_p_data + 0x010;
    g_cbw     = g_p_data + 0x110;
    g_csw     = g_p_data + 0x150;
    g_scratch = g_p_data + 0x160;
    g_sector  = g_p_data + 0x200;

    // ---- 6) 异步表：QH 自环 + USBCMD.RS|ASE ----
    const uint32_t qh_pa = (uint32_t)(uintptr_t)g_qh;
    g_qh->hlp    = qh_pa | EHCI_QH_TYPE_QH;
    g_qh->epchar = EHCI_QH_EC_H;
    g_qh->epcap  = 0x01u;
    g_qh->cur    = EHCI_QTD_TERM;
    g_qh->next   = EHCI_QTD_TERM;
    g_qh->alt    = EHCI_QTD_TERM;
    g_qh->token  = 0;
    for (int i = 0; i < 5; i++) g_qh->buf[i] = 0;
    __asm__ volatile("" ::: "memory");
    op_wr32(EHCI_OP_PERIODICBASE, 0);                         // 不建周期表（PSE=0）
    op_wr32(EHCI_OP_ASYNCLISTADDR, qh_pa);
    op_wr32(EHCI_OP_USBCMD, EHCI_CMD_RS | EHCI_CMD_ASE | EHCI_CMD_ITC_1);
    const uint32_t cmd_rd = op_rd32(EHCI_OP_USBCMD);
    const uint32_t sts_rd = op_rd32(EHCI_OP_USBSTS);
    const uint32_t asy_rd = op_rd32(EHCI_OP_ASYNCLISTADDR);
    elog_begin();
    dbg64_str("[EHCI] async qh=");
    ehex(qh_pa, 8);
    dbg64_str(" qtd=");
    ehex((uint32_t)(uintptr_t)qtd_slot(0), 8);
    dbg64_str(" qtds=");
    dbg64_dec((uint64_t)EHCI_QTD_COUNT);
    elog_end();
    if (!(cmd_rd & EHCI_CMD_RS)) g_selftest_mask |= 2u;
    if (asy_rd != qh_pa)         g_selftest_mask |= 4u;
    (void)sts_rd;
    g_ready = true;
    g_state = EHCI_ST_NO_DEVICE;

    // ---- 7) 扫所有根端口：只接管高速端口，枚举一台 BOT 存储 ----
    ehci_scan_ports_boot();

    // ---- 8) 自检位 + 收尾（★ 初始化全部完成之后才 arm 热插拔基线）----
    if (g_msc.present && g_msc.selftest_mask) g_selftest_mask |= 16u;    // U 盘枚举到了但探测没过
    if (g_msc.present && !g_msc.supported)    g_selftest_mask |= 16u;
    if (g_msc.supported && (g_msc.selftest_mask & 64u)) g_selftest_mask |= 32u;   // 越界读探针没被拒
    if (g_conn_seen > 0 && g_devices == 0 && !g_msc.present) g_selftest_mask |= 8u;
    g_hp_armed = false;                                       // 基线在下面建立
    g_port_ccs = 0;
    for (uint32_t n = 1; n <= g_ports && n <= EHCI_MAX_PORTS; n++) {
        const uint32_t v = port_rd(n);
        if (v & EHCI_PSC_CCS) g_port_ccs |= (1u << (n - 1u));
        if (v & (EHCI_PSC_CSC | EHCI_PSC_PEDC | EHCI_PSC_OCC | EHCI_PSC_LSC)) port_clear_changes(n);
    }
    g_hp_armed = true;                                        // ★ P8b：到这之后端口变化才算"新插入"
    g_state = g_msc.supported ? EHCI_ST_READY : (g_conn_seen ? EHCI_ST_ENUM_FAILED : EHCI_ST_NO_DEVICE);
    ehci_selftest_log();
    return g_msc.supported ? 0 : -1;
}

// ==================== 热插拔（根端口 CCS 变化 -> 重枚举）====================
void ehci64_poll64() {
    if (!g_found || !g_ready || !g_hp_armed) return;
    if (!ehci_lock_try()) return;                             // 有同步传输在跑：下次再来
    uint32_t mask = 0;
    for (uint32_t n = 1; n <= g_ports && n <= EHCI_MAX_PORTS; n++) {
        const uint32_t v = port_rd(n);
        if (v & EHCI_PSC_CCS) mask |= (1u << (n - 1u));
        if (v & (EHCI_PSC_CSC | EHCI_PSC_PEDC | EHCI_PSC_OCC | EHCI_PSC_LSC)) port_clear_changes(n);
    }
    if (mask == g_port_ccs) { ehci_lock_release(); return; }
    const uint32_t chg = mask ^ g_port_ccs;
    g_port_ccs = mask;
    for (uint32_t n = 1; n <= g_ports && n <= EHCI_MAX_PORTS; n++) {
        if (!(chg & (1u << (n - 1u)))) continue;
        const bool now = (mask & (1u << (n - 1u))) != 0;
        if (!now) {
            elog_begin();
            dbg64_str("[EHCI] port ");
            dbg64_dec((uint64_t)n);
            dbg64_str(" detached (usb storage");
            dbg64_str(g_msc.present ? " removed" : " was not used by this driver");
            dbg64_str(")");
            elog_end();
            if (g_msc.present) {
                memset_64(&g_msc, 0, sizeof(g_msc));          // 清掉：盘符重扫由上层做
                g_devices = 0;
            }
            continue;
        }
        // ★ P8b：端口上还是本驱动**已经在用**的那台设备（g_msc.present = 存储在线、配置还在生效）
        //   —— **不复位、不重枚举**（多一次复位会让正在工作的设备短暂离开总线：BOT 传输刚要开始时被
        //   复位，重枚举的第一个控制传输就会失败）。本驱动只处理一台存储，所以直接跳过；
        //   盘符由门面（usb64_poll64 的差分）看到"数量没变"而不重扫 —— 幂等，不需要额外动作。
        if (g_msc.present) {
            elog_begin();
            dbg64_str("[EHCI] port ");
            dbg64_dec((uint64_t)n);
            dbg64_str(" attached (already-configured usb storage kept as-is; no re-reset/re-enum)");
            elog_end();
            continue;
        }
        // 插入：与启动期同一条 复位 -> 枚举 -> 探测 路径；**失败不冒充整机枚举失败**
        uint32_t rv = 0;
        const bool ped = port_reset(n, &rv);
        if (!ped) {
            port_give_to_companion(n);                        // ★ 真正交还（见 port_give_to_companion）
            elog_begin();
            dbg64_str("[EHCI] port ");
            dbg64_dec((uint64_t)n);
            dbg64_str(" attached speed=");
            dbg64_str(psc_speed_name(rv));
            dbg64_str(" -> handed to the companion controller (not high speed)");
            elog_end();
            continue;
        }
        elog_begin();
        dbg64_str("[EHCI] port ");
        dbg64_dec((uint64_t)n);
        dbg64_str(" attached speed=high reset ok");
        elog_end();
        const int rc = ehci_enum_storage(1);
        if (rc != 0) {
            if (g_hp_retry_fail_logs < 8u) {
                g_hp_retry_fail_logs++;
                elog_begin();
                dbg64_str("[EHCI] hotplug port=");
                dbg64_dec((uint64_t)n);
                dbg64_str(" attach failed (retry allowed on next attach; this is NOT a boot-time enum failure)");
                elog_end();
            }
            continue;
        }
        g_devices++;
        msc_probe();
    }
    ehci_lock_release();
}

// ==================== 只读接口 ====================
const char* ehci64_state_str64() {
    switch (g_state) {
    case EHCI_ST_NOT_FOUND:   return "not found";
    case EHCI_ST_NO_DEVICE:   return "no device";
    case EHCI_ST_ENUM_FAILED: return "enum failed";
    case EHCI_ST_READY:       return "ready";
    default:                  return "init";
    }
}
int ehci64_ports64()    { return g_found ? (int)g_ports : 0; }
int ehci64_devices64()  { return (int)g_devices; }
uint32_t ehci64_link_ok64() { return g_link_ok; }

int ehci64_selftest64() {
    if (!g_found) return (int)g_selftest_mask;                // 没主控 = 合法降级（正常是 0）
    return (int)g_selftest_mask;
}

// ★ 给门面（kernel/usb64.cpp）用：热插拔基线是否已经建立（= ehci64_init64() 全部收尾完成）。
//   语义：返回 false 时门面**不比对** EHCI 的存储数量 —— 启动期（init 还没跑完 / 还没 arm）一律不当事件。
bool ehci64_hotplug_ready64() { return g_hp_armed; }

// ==================== USB 存储：对外接口（给 usb64 门面 / ata64 分派用）====================
int ehci64_msc_count64() { return (g_msc.present && g_msc.supported) ? 1 : 0; }

bool ehci64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512) {
    if (idx != 0 || !g_msc.present || !g_msc.supported) return false;
    if (model && model_cap > 0) {
        int o = 0;
        for (int i = 0; i < 8 && g_msc.vendor[i] && o < model_cap - 1; i++) model[o++] = g_msc.vendor[i];
        if (o < model_cap - 1) model[o++] = ' ';
        for (int i = 0; i < 16 && g_msc.product[i] && o < model_cap - 1; i++) model[o++] = g_msc.product[i];
        model[o] = 0;
    }
    if (sectors_512) {
        const uint64_t bytes = (uint64_t)g_msc.blocks * (uint64_t)g_msc.block_size;
        *sectors_512 = bytes / 512u;
    }
    return true;
}

// 按 512B 扇区读（ata64 的语义）；一条 READ(10) 最多 8 个扇区（4KB），多了自动分块。
// 缓冲区认不出物理地址时走 4KB DMA 暂存页（拷贝进出），与写路径同一套兜底。
bool ehci64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf) {
    if (idx != 0 || !g_msc.present || !g_msc.supported) return false;
    if (count == 0) return true;
    if (!buf) return false;
    if (g_msc.block_size != 512u) return false;
    if (!msc_range_ok(lba, count, true)) return false;        // ★ 越界先拒绝（不发命令、不碰介质）
    uint8_t* p = (uint8_t*)buf;
    ehci_lock_acquire();
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > EHCI_MSC_MAX_SECTORS) n = EHCI_MSC_MAX_SECTORS;
        const uint32_t bytes = n * 512u;
        const uint32_t pa = ehci_pa64(p);
        if (pa != 0) {
            if (msc_read10(lba + done, (uint16_t)n, p) != 0) { ehci_lock_release(); return false; }
        } else if (g_p_bounce) {
            if (bytes > PAGE_SIZE_64) { ehci_lock_release(); return false; }
            if (msc_read10(lba + done, (uint16_t)n, g_p_bounce) != 0) { ehci_lock_release(); return false; }
            memcpy_64(p, g_p_bounce, bytes);
        } else {
            ehci_lock_release();
            return false;
        }
        done += n;
        p += bytes;
    }
    ehci_lock_release();
    return true;
}

// ★ 按 512B 扇区**写**（与 usb64_msc_write64 完全同一条语义）：
//   1) 越界/超容量先拒绝（reason=range，碰不到介质）；
//   2) 写后读回逐字节校验（每块写完立刻 READ(10) 回同一 LBA 段）；不一致 -> [USBST] write verify FAILED。
bool ehci64_msc_write64(int idx, uint32_t lba, uint32_t count, const void* buf) {
    if (idx != 0 || !g_msc.present || !g_msc.supported) return false;
    if (count == 0) return true;
    if (!buf) return false;
    const uint8_t* p = (const uint8_t*)buf;
    if (g_msc.block_size != 512u) {
        elog_begin();
        dbg64_str("[USBST] write FAILED reason=block-size (only 512B blocks are supported)");
        elog_end();
        g_msc.writes_fail++;
        return false;
    }
    if (!msc_range_ok(lba, count, false)) {
        g_msc.writes_fail++;
        if (g_usbst_wfail_logs < EHCI_FAIL_LOG_MAX) {
            g_usbst_wfail_logs++;
            elog_begin();
            dbg64_str("[USBST] write FAILED lba=");
            dbg64_dec((uint64_t)lba);
            dbg64_str(" count=");
            dbg64_dec((uint64_t)count);
            dbg64_str(" reason=range (out of capacity blocks=");
            dbg64_dec((uint64_t)g_msc.blocks);
            dbg64_str(")");
            elog_end();
        }
        return false;
    }
    if (!g_p_bounce) return false;                            // 读回缓冲必须能 DMA
    ehci_lock_acquire();
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > EHCI_MSC_MAX_SECTORS) n = EHCI_MSC_MAX_SECTORS;
        const uint32_t bytes = n * 512u;
        const uint32_t pa = ehci_pa64(p);
        if (pa == 0) {
            if (bytes > PAGE_SIZE_64) { ehci_lock_release(); return false; }
            memcpy_64(g_p_bounce, p, bytes);
            if (msc_write10(lba + done, (uint16_t)n, g_p_bounce) != 0) { ehci_lock_release(); return false; }
        } else {
            if (msc_write10(lba + done, (uint16_t)n, p) != 0) { ehci_lock_release(); return false; }
        }
        if (msc_read10(lba + done, (uint16_t)n, g_p_bounce) != 0) {
            g_msc.wverify_fail++;
            elog_begin();
            dbg64_str("[USBST] write verify FAILED lba=");
            dbg64_dec((uint64_t)(lba + done));
            dbg64_str(" reason=readback");
            elog_end();
            ehci_lock_release();
            return false;
        }
        if (memcmp_64(g_p_bounce, p, bytes) != 0) {
            uint32_t bad = 0;
            while (bad < bytes && g_p_bounce[bad] == p[bad]) bad++;
            g_msc.wverify_fail++;
            elog_begin();
            dbg64_str("[USBST] write verify FAILED lba=");
            dbg64_dec((uint64_t)(lba + done));
            dbg64_str(" count=");
            dbg64_dec((uint64_t)n);
            dbg64_str(" at=");
            dbg64_dec((uint64_t)bad);
            dbg64_str(" want=");
            ehex((uint32_t)p[bad], 2);
            dbg64_str(" got=");
            ehex((uint32_t)g_p_bounce[bad], 2);
            elog_end();
            ehci_lock_release();
            return false;
        }
        g_msc.wverify_ok++;
        if (g_usbst_wlogs < EHCI_WRITE_LOG_MAX) {
            g_usbst_wlogs++;
            elog_begin();
            dbg64_str("[USBST] write verify lba=");
            dbg64_dec((uint64_t)(lba + done));
            dbg64_str(" count=");
            dbg64_dec((uint64_t)n);
            dbg64_str(" ok (read back, byte-for-byte)");
            elog_end();
        }
        done += n;
        p += bytes;
    }
    ehci_lock_release();
    return true;
}

const char* ehci64_msc_last_reason64() {
    switch (g_msc.last_reason) {
    case 0:  return "ok";
    case 1:  return "csw status";
    case 2:  return "timeout";
    case 3:  return "nak";
    case 4:  return "block-size";
    case 6:  return "residue";
    default: return "no-device";
    }
}

// 一次性自检（供 kernel64.cpp 在 init 之后调用；语义与 usb64 一致：没插盘 = 合法降级）
int ehci64_msc_selftest64() {
    if (!g_msc.present) return 0;
    if (!g_msc_probe_done) msc_probe();
    return (int)g_msc.selftest_mask;
}
