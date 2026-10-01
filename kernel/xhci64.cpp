// xhci64.cpp - USB 3.x 主机控制器（xHCI 1.0/1.1）：HID 引导键盘 + USB 存储（BOT/SCSI 只读）
//
// ============================ 为什么这么做 ============================
// 1) 为什么要它：机器上早就只有 UHCI 的支持（kernel/usb64.cpp），而 2012 年以后的主板基本只剩
//    xHCI（USB 3.x）；只有 UHCI 时插 U 盘/键盘什么都不会发生。本文件把 xHCI 补上，**键盘接到
//    和 UHCI 完全相同的那一条注入路径**（HID 用法码 -> PS/2 集 1 扫描码 -> input.cpp），
//    存储接到 kernel/ata64.h 的 ATA64_USB_BASE（24..）那一段统一驱动器号。
// 2) 寄存器与 DMA 全部是"物理地址"（踩过的坑，见 usb64.cpp 的 usb_pa32 说明）：
//    * MMIO：BAR0 是 64 位 MMIO BAR，**恒等映射**（前 4GB），所以物理地址即指针；
//    * DMA 结构（命令/事件环、ERST、DCBAA、设备/输入上下文、EP 环、数据缓冲）全部来自
//      page_alloc_64()（页池 [128MB, mem_high) 恒等映射），**指针值就是物理地址**；
//    * 调用方传进来的缓冲区（例如 drive64 的扇区缓冲在内核 .bss 高半区）走 xhci_pa64()
//      直映换算；认不出来就退回 **DMA 暂存页**（拷贝进出），绝不把错地址交给硬件。
// 3) 传输全部**同步 + 有界超时**（没有中断、没有 kusb 之外的并发驱动）：
//    * 一次只下发一个"传输"（控制传输 = Setup/Data/Status 一条序列；批量/中断 = 一个 Normal TRB），
//      等它的事件回来再下发下一个 —— xHCI 的环是生产/消费式环，"绝不越过消费指针"这条纪律
//      因此天然成立（不会把还没被硬件取走的 TRB 覆盖掉）；
//    * 等待用 g_ticks64（PIT 250Hz）计时 + 硬自旋上界兜底，超时一律 abort 并如实返回失败；
//    * 运行期轮询（HID 报告）由 kusb 内核线程经 usb64_poll64() -> xhci64_poll64() 驱动，
//      和同步传输用**自旋锁**互斥（poll 侧 try-lock，拿不到就下次再来，绝不阻塞）。
// 4) 事件环消费者游标 + ERDP.EHB：取一个事件就把 dequeue 前移并写 ERDP，**bit3（EHB）写 1**
//    —— 这是规范里"事件已处理完、清掉挂起标志"的语义（写 1 清，不是写 0 清）。第一次取到
//    事件时打一行 [XHCI] erdp ehb cleared 作为证据。
// 5) ★ 门铃（Doorbell）的 DB Target 位序在资料上有分歧，本驱动**自探**（以"第一条控制传输能不能
//    完成"为判据，探到的模式打进 [XHCI] doorbell mode=<n>）：
//      mode 0 = 值就是 DCI（**低字节**；QEMU 11.1 与 BIOS 自带 xHCI 驱动实测，SeaBIOS 就是 writel(DB+slot*4, ep)）；
//      mode 1 = DCI 在 bits31:16（xHCI 规范的 DB Target 位）；
//      mode 2 = 值恒为 1（只要求"写非 0"的实现）。寄存器地址恒为 DBOFF + slot*4。
//    ★ 三者对 EP0（DCI=1）可区分（1 / 0x10000 / 1），所以探出来的位序对批量/中断端点一样成立。
//    踩过：原先写 `1 | (dci<<8)`，EP0 恰好等效、键盘靠 QEMU 对 NAK 的自重试也能跑，但 U 盘的
//    EP4 门铃被当成 EP1 —— 批量传输永远不启动，INQUIRY 超时（这是存储那条链路的真因）。
// 6) 速率协商：复位后读 PORTSC 的 Port Speed 字段（bit13:10 -> 1=low 2=full 3=high 4=super
//    5=superplus），据此填 Slot Context 的 Speed 字段与 EP0 默认最大包（FS/LS=8、HS=64、SS=512），
//    再按"Address Device(BSR=1) -> 读 8 字节设备描述符 -> Address Device(BSR=0)"两步定址。
//    PORTSC.PR -> 等 PORTSC.PRC 的握手用 MFINDEX 采样证明时间真的在走。
//
// 没验证到的点（如实记录）：
//   * USB3 SuperSpeed：现在**有实测**了 —— QEMU 的 usb-storage 就是 SuperSpeed 设备（挂在 USB3 端口，
//     PORTSC 速度码 4、bMaxPacketSize0 报 9 是指数编码=512），存储全程（INQUIRY/CAPACITY/READ(10)）
//     走的就是 SS 链路。没验证的只剩：SS 的 **Streams / Max Burst / 链路状态切换（U1/U2/U3）**。
//   * 集线器（hub）、热插拔运行期枚举、多配置/多接口切换、USB 鼠标、带宽协商（xHCI 的
//     Configure Endpoint 带宽请求）都没做：只认直接插在根端口的设备，拔出只打点不重新枚举。
//   * 没有接 MSI/MSI-X/中断（全程轮询），没有挂起/恢复/休眠（U3 等链路状态）。
//   * 存储**只读**（没有 WRITE(10)），块大小 ≠ 512 的盘如实拒绝（打点后不暴露成块设备）。
// ======================================================================
#include "xhci64.h"
#include "usb64.h"        // ★ 复用 UHCI 那套 HID 报告解析（同一张用法码表 + 同一条按键队列）
#include "port.h"         // outl/inl（PCI 配置空间 0xCF8/0xCFC）
#include "debug64.h"      // 串口打点（行锁 begin/end）
#include "memlayout64.h"  // ★ 必须先于 mem_64.h（PAGE_SIZE_64 会撞）；ML64_KERNEL_VA_BASE/BASE
#include "mem_64.h"       // page_alloc_64 / memset_64 / memcpy_64
#include "x86_64.h"       // g_ticks64 / ms_to_ticks64 / nop_pause()
#include <stdint.h>
#include <stddef.h>

extern "C" char __bss_end[];               // 内核镜像高半区上界（判断指针是否落在内核镜像里）

// ==================== 寄存器偏移（xHCI 1.1 第 5 章）====================
#define XHCI_CAP_CAPLENGTH   0x00u     // 1 字节（低 8 位）+ HCIVERSION（高 16 位）
#define XHCI_CAP_HCSPARAMS1  0x04u     // MaxSlots[7:0] MaxIntrs[18:8] MaxPorts[31:24]
#define XHCI_CAP_HCSPARAMS2  0x08u     // ERST max / Max Scratchpad Buffers
#define XHCI_CAP_HCCPARAMS1  0x10u     // AC64[0] CSZ[2] xECP[31:16]
#define XHCI_CAP_DBOFF       0x14u
#define XHCI_CAP_RTSOFF      0x18u

#define XHCI_OP_USBCMD    0x00u
#define XHCI_OP_USBSTS    0x04u
#define XHCI_OP_PAGESIZE  0x08u
#define XHCI_OP_DNCTRL    0x14u
#define XHCI_OP_CRCR      0x18u
#define XHCI_OP_DCBAAP    0x30u
#define XHCI_OP_CONFIG    0x38u
#define XHCI_OP_PORTS     0x400u       // 端口寄存器组基址：第 n 个在 +0x10*(n-1)
#define XHCI_PORT_STRIDE  0x10u

#define XHCI_IR_BASE      0x20u        // 运行寄存器里"中断器 0"的起始偏移
#define XHCI_RT_MFINDEX   0x00u
// ★ 中断器寄存器组 0 **从 RTSOFF + 0x20 开始**（RTSOFF+0x00 是 MFINDEX，+0x04..0x1F 保留）——
//   踩过：把 IMAN/ERSTSZ/ERSTBA/ERDP 写到 RTSOFF+0x00..0x18（= MFINDEX + 保留区）时，
//   ERST/事件环等于**从没被编过程序**，现象是"命令永远等不到完成事件、端口变化也没事件"。
#define XHCI_IR_IMAN      (XHCI_IR_BASE + 0x00u)
#define XHCI_IR_ERSTSZ    (XHCI_IR_BASE + 0x08u)
#define XHCI_IR_ERSTBA    (XHCI_IR_BASE + 0x10u)
#define XHCI_IR_ERDP      (XHCI_IR_BASE + 0x18u)
#define XHCI_IR_STRIDE    0x20u        // 中断器寄存器组之间 0x20 字节

#define XHCI_USBCMD_RS    0x00000001u
#define XHCI_USBCMD_HCRST 0x00000002u
#define XHCI_USBCMD_INTE  0x00000004u
#define XHCI_USBSTS_HCH   0x00000001u
#define XHCI_USBSTS_EINT  0x00000008u
#define XHCI_USBSTS_PCD   0x00000010u
#define XHCI_USBSTS_CNR   0x00000800u
#define XHCI_USBSTS_HCE   0x00001000u

                     // 位 0..23 = 状态变化位（RW1C，写 1 清）
#define XHCI_PORTSC_CCS   0x00000001u     // 设备已连接
#define XHCI_PORTSC_PED   0x00000002u     // 端口使能
#define XHCI_PORTSC_OCA   0x00000008u     // 过流
#define XHCI_PORTSC_PR    0x00000010u     // 端口复位（RW1S）
#define XHCI_PORTSC_PLS_M 0x000001E0u     // 链路状态（bit8:5）
#define XHCI_PORTSC_PP    0x00000200u     // 端口上电
#define XHCI_PORTSC_SPEED_M 0x00003C00u   // Port Speed（bit13:10）
#define XHCI_PORTSC_SPEED_S 10
#define XHCI_PORTSC_PIC   0x0000C000u
#define XHCI_PORTSC_LWS   0x00010000u
#define XHCI_PORTSC_CSC   0x00020000u     // 连接状态变化（RW1C）
#define XHCI_PORTSC_PEC   0x00040000u     // 端口使能变化（RW1C）
#define XHCI_PORTSC_WRC   0x00080000u
#define XHCI_PORTSC_OCC   0x00100000u
#define XHCI_PORTSC_PRC   0x00200000u     // 端口复位完成（RW1C）
#define XHCI_PORTSC_PLC   0x00400000u
#define XHCI_PORTSC_CEC   0x00800000u
#define XHCI_PORTSC_CHANGE (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | \
                            XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)

// ==================== TRB ====================
// 16 字节：param(8) + status(4) + control(4)。★ Cycle 位是 **control 的最低位**，TRB Type 在
// control 的 15:10（Linux 的 TRB_CYCLE / TRB_TYPE 就是这两个位置）。
struct XhciTrb { uint64_t param; uint32_t status; uint32_t control; } __attribute__((aligned(16)));

#define XHCI_TRB_CYCLE   0x00000001u
#define XHCI_TRB_ENT     0x00000002u
#define XHCI_TRB_ISP     0x00000004u
#define XHCI_TRB_CH      0x00000010u
#define XHCI_TRB_IOC     0x00000020u
#define XHCI_TRB_TC      0x00000002u      // Link TRB 的 Toggle Cycle（bit1）
#define XHCI_TRB_TYPE(t) ((uint32_t)(t) << 10)
// Setup Stage：8 字节 Setup 包**内联**在 TRB 里（规范 6.4.1.1）——
//   TRT（bit17:16）= 0/2/3 决定有没有数据阶段；**IDT（bit6）必须置 1**（否则 xHC 会把 TRB Pointer
//   当"指向 Setup 包的内存地址"，而它其实是 Setup 包的位模式 → 控制传输永不完成、也不报错）。
#define XHCI_TRB_TRT(t)  ((uint32_t)(t) << 16)
#define XHCI_TRB_IDT     0x00000040u
#define XHCI_TRB_DIR_IN  0x00010000u      // Data/Status Stage：方向位（bit16）
#define XHCI_TRB_DIR_IN  0x00010000u      // Data/Status Stage：IN 方向（bit16）
#define XHCI_TRB_BSR     0x00000200u      // Address Device：Block Set Address（bit9）
#define XHCI_TRB_SLOT(s) ((uint32_t)(s) << 24)

#define TRB_NORMAL        1u
#define TRB_SETUP         2u
#define TRB_DATA          3u
#define TRB_STATUS        4u
#define TRB_LINK          6u
#define TRB_NOOP          8u
#define TRB_ENABLE_SLOT   9u
#define TRB_ADDRESS_DEV  11u
#define TRB_CONFIGURE_EP 12u

#define EVT_TRANSFER      32u
#define EVT_CMD_COMPLETE  33u
#define EVT_PORT_CHANGE   34u
#define EVT_MFINDEX_WRAP  39u

#define XHCI_CC_SUCCESS    1u             // Successful Completion
#define XHCI_CC_SHORT_PKT 13u             // Short Packet（数据阶段短包，算成功）
#define XHCI_CC_NO_SLOTS  9u

// 上下文里的 EP Type（bit5:3）与 DCI 规则：DCI = 端点号*2 + (IN ? 1 : 0)
#define EPT_CONTROL      4u
#define EPT_BULK_OUT     2u
#define EPT_BULK_IN      6u
#define EPT_INT_IN       7u

// ==================== 上界（都是有界的，绝不无限等）====================
#define XHCI_MAX_DEV      2               // 1 键盘 + 1 存储
#define XHCI_RING_TRBS    64              // 环大小（TRB 数；命令/传输环含 1 个 Link TRB）
#define XHCI_EVT_TRBS     64              // 事件环（无 Link TRB，段末尾自动回卷，故必须是 2 的幂）
#define XHCI_CTX_BYTES    4096u           // 设备/输入上下文各占一页（CSZ=1 时 2112 字节足够）
#define XHCI_DCI_MAX      32u
#define XHCI_CTL_BYTES    256u            // 控制传输数据阶段缓冲（描述符）
#define XHCI_CTL_TRBS     (1u + 32u + 1u) // Setup + 最多 32 个数据包（256/8）+ Status
#define XHCI_CTL_TIMEOUT  400u            // 单次控制传输的等待上限（ms）
#define XHCI_CMD_TIMEOUT  400u            // 单条命令的等待上限（ms）
#define XHCI_BULK_TIMEOUT 800u            // 单次批量传输的等待上限（ms）
#define XHCI_XFER_CHUNK   4096u           // 一次批量传输的字节上限（= MSC 一条 READ(10) 的数据）
#define XHCI_MSC_SECTORS  8u              // 一条 READ(10) 最多 8 个 512B 扇区（4KB）
#define XHCI_SCRATCH_MAX  16u             // scratchpad 缓冲上限（QEMU 是 0；真机常见 0/1）

// 数据页里的偏移（都在恒等映射的低内存页里，物理地址 = 指针）
//   ★ Setup 包不再占内存：它**内联**在 Setup Stage TRB 里（见 xhci_control）
#define XHCI_OFF_DESC     0x010u
#define XHCI_OFF_REP0     0x110u
#define XHCI_OFF_REP1     0x150u
#define XHCI_OFF_SECTOR   0x200u
#define XHCI_OFF_CBW      0x400u
#define XHCI_OFF_CSW      0x420u
#define XHCI_OFF_SCRATCH  0x440u

// ==================== 全局状态 ====================
enum Xhci64State : uint32_t {
    XHCI_ST_INIT = 0,
    XHCI_ST_NOT_FOUND,
    XHCI_ST_NO_DEVICE,
    XHCI_ST_ENUM_FAILED,
    XHCI_ST_READY
};

static bool     g_inited = false;
static bool     g_found  = false;
static bool     g_ready  = false;
static uint32_t g_state  = XHCI_ST_INIT;

static uint8_t  g_bus = 0, g_pcidev = 0, g_fn = 0, g_progif = 0;   // PCI 位置（g_dev[] 是设备表，别混）
static uint64_t g_cap = 0;                 // BAR0（MMIO 基址，恒等映射）
static uint32_t g_caplen = 0, g_rtsoff = 0, g_dboff = 0;
static uint32_t g_max_slots = 0, g_max_ports = 0;
static uint32_t g_csz = 0, g_ac64 = 0, g_ctx_bytes = 64;
// 事件环是否已经编好程序（poll 的门槛：**不依赖"枚举成功"** —— 端口变化事件在没有设备时
// 也要能被看到，否则"插拔/复位"这类事实永远打不出来）
static bool     g_evt_ready = false;
static uint32_t g_max_scratch = 0, g_scratch = 0;
static uint32_t g_db_mode = 0;             // 门铃位序自探结果（0 = (id<<8)|1，1 = 1<<id，2 = 1）
static uint32_t g_selftest_mask = 0;
static uint32_t g_ports_seen = 0;
static int      g_devices = 0;             // 已枚举成功的设备数
static uint32_t g_probe_slot = 0;          // 门铃自探时那条 Enable Slot 拿到的槽（给第一台设备复用，不浪费）
static uint64_t g_evt_seen = 0;

// DMA 页（page_alloc_64：低内存恒等映射，指针值即物理地址）
static uint8_t*  g_p_rings = nullptr;      // 命令环 + 事件环 + ERST
static uint8_t*  g_p_dcbaa = nullptr;      // DCBAA（MaxSlots+1 项）+ scratchpad 指针数组
static uint8_t*  g_p_inctx = nullptr;      // 输入上下文
static uint8_t*  g_p_data  = nullptr;      // 小数据缓冲（SETUP/描述符/报告/CBW/CSW/扇区）
static uint8_t*  g_p_bounce= nullptr;      // 4KB DMA 暂存页（调用方缓冲区认不出物理地址时）
static uint8_t*  g_p_scratch[XHCI_SCRATCH_MAX] = { nullptr };

static volatile XhciTrb* g_cmd_trbs = nullptr;
static volatile XhciTrb* g_evt_trbs = nullptr;
static uint32_t g_evt_idx = 0, g_evt_cycle = 1;

static uint8_t*  g_desc  = nullptr;        // 256 字节描述符缓冲
static uint8_t*  g_sector= nullptr;        // 512 字节扇区缓冲
static uint8_t*  g_cbw   = nullptr;        // 32 字节 CBW
static uint8_t*  g_csw   = nullptr;        // 16 字节 CSW
static uint8_t*  g_msc_scratch = nullptr;  // 64 字节（INQUIRY / CAPACITY / SENSE）

struct XhciRing { volatile XhciTrb* t; uint32_t idx; uint32_t cycle; };

struct XhciEvt { uint64_t param; uint32_t status; uint32_t control; };

struct XhciDev {
    bool     used;
    bool     is_kbd, is_msc;
    uint8_t  slot, port, speed, mps;       // speed = PORTSC 的 Port Speed 码
    uint16_t vendor, product;
    uint8_t  ifaces;
    // 键盘（HID 引导）
    uint8_t  hid_iface, kbd_dci, kbd_mps, kbd_interval;
    uint8_t* report;                       // 报告缓冲（每台一份，TRB 挂起期间不能被写）
    uint64_t int_trb;                      // 已武装的中断 TRB 物理地址（0 = 没武装）
    Usb64Hid64 hid;
    uint64_t reports, keys;
    // 存储（BOT）
    uint8_t  in_dci, out_dci;
    uint16_t in_mps, out_mps;
    bool     msc_ok;                       // 探测全过（INQUIRY + CAPACITY + READ(10)）
    uint32_t blocks, block_size, tag;
    uint8_t  csw_status;
    uint8_t  vendor_s[9], product_s[17];
    // 上下文 + 4 个环（EP0 / IN / OUT / 中断）
    uint8_t* ctx;
    XhciRing ep0, ep_in, ep_out, ep_int;
};
static XhciDev g_dev[XHCI_MAX_DEV];

// 传输/poll 互斥：批量传输是同步自旋等待的（由文件管理器/终端线程调用），kusb 线程同时会
// 进来处理事件；临界区里会动事件环游标，所以必须互斥（poll 侧 try-lock，绝不阻塞）。
static volatile uint32_t g_xhci_lock = 0;
static inline bool xhci_lock_try() {
    uint32_t v = 1;
    __asm__ volatile("xchgl %0, %1" : "+r"(v) : "m"(g_xhci_lock) : "memory");
    return v == 0;
}
static void xhci_lock_acquire() {
    uint64_t spins = 0;
    while (!xhci_lock_try() && spins < 400000000ull) { spins++; nop_pause(); }
}
static inline void xhci_lock_release() {
    __asm__ volatile("" ::: "memory");
    g_xhci_lock = 0;
}

// ==================== 小工具 ====================
static void xlog_begin() { dbg64_line_begin64(); }
static void xlog_end()   { dbg64_nl(); dbg64_line_end64(); }

static void xhex(uint32_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    char buf[9];
    if (digits > 8) digits = 8;
    for (int i = digits - 1; i >= 0; i--) { buf[i] = H[v & 0xFu]; v >>= 4; }
    for (int i = 0; i < digits; i++) dbg64_putc(buf[i]);
}

// ★ 物理地址换算（与 usb64.cpp 的 usb_pa32 / ahci64 / nvme64 同款约定）：
//   低内存（< 4GB，含页池里的 DMA 结构）：恒等映射；内核镜像高半区对象（.bss/.data）直映；
//   认不出来返回 0，调用方退回 DMA 暂存页（绝不把错地址交给硬件）。
static inline uint32_t xhci_pa64(const void* ptr) {
    const uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v == 0) return 0;
    if (v < 0x100000000ULL) return (uint32_t)v;
    if (v >= ML64_KERNEL_VA_BASE && v < (uint64_t)(uintptr_t)__bss_end + 0x10000ULL)
        return (uint32_t)(v - (ML64_KERNEL_VA_BASE - (uint64_t)ML64_KERNEL_BASE));
    return 0;
}

static void xdelay_ms(uint32_t ms) {
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(ms ? ms : 1);
    uint64_t spin = 0;
    while ((g_ticks64 - t0) < want && spin < 400000000ull) { spin++; nop_pause(); }
}

static uint32_t xcrc32(const uint8_t* p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
    return c ^ 0xFFFFFFFFu;
}

static const char* xspeed_name(uint32_t s) {
    switch (s) {
    case 1: return "low";
    case 2: return "full";
    case 3: return "high";
    case 4: return "super";
    case 5: return "superplus";
    default: return "?";
    }
}

// ==================== MMIO 访问 ====================
static inline uint32_t mmio_rd32(uint32_t off) {
    return *(volatile uint32_t*)(uintptr_t)(g_cap + off);
}
static inline void mmio_wr32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_cap + off) = v;
}
static inline uint64_t mmio_rd64(uint32_t off) {
    return *(volatile uint64_t*)(uintptr_t)(g_cap + off);
}
static inline void mmio_wr64(uint32_t off, uint64_t v) {
    *(volatile uint64_t*)(uintptr_t)(g_cap + off) = v;
}
static inline uint32_t op_rd32(uint32_t off) { return mmio_rd32(g_caplen + off); }
static inline uint64_t op_rd64(uint32_t off) { return mmio_rd64(g_caplen + off); }
static inline void     op_wr32(uint32_t off, uint32_t v) { mmio_wr32(g_caplen + off, v); }
static inline void     op_wr64(uint32_t off, uint64_t v) { mmio_wr64(g_caplen + off, v); }
static inline uint32_t rt_rd32(uint32_t off) { return mmio_rd32(g_rtsoff + off); }
static inline void     rt_wr32(uint32_t off, uint32_t v) { mmio_wr32(g_rtsoff + off, v); }
static inline uint64_t rt_rd64(uint32_t off) { return mmio_rd64(g_rtsoff + off); }
static inline void     rt_wr64(uint32_t off, uint64_t v) { mmio_wr64(g_rtsoff + off, v); }
static inline uint32_t port_off(uint32_t n) { return XHCI_OP_PORTS + (n - 1u) * XHCI_PORT_STRIDE; }
static inline uint32_t port_rd(uint32_t n) { return op_rd32(port_off(n)); }
static inline void     port_wr(uint32_t n, uint32_t v) { op_wr32(port_off(n), v); }

// 门铃：DB0 = 命令环（值 0）；DB[slot] = 设备槽。★ 值里"DB Target（端点号 DCI）"的位序**实测过**：
//   QEMU 11.1（`-trace usb_xhci_doorbell_write/ep_kick`）与 BIOS 自带的 xHCI 驱动都把 DCI 写在
//   **低字节**（SeaBIOS 就是 `writel(DB + slot*4, ep)`；val=0x401 时 QEMU 踢的是 epid 1，说明它读低字节）。
//   规范版写法是 DB Target 在 bits31:16。自探按"控制传输能不能完成"选：EP0 的 DCI=1，低字节写法=1、
//   规范写法=0x10000 —— 两者对 EP0 可区分，所以探到的位序对非 EP0 端点（批量/中断）同样成立。
//   （踩过：原先写的是 `1 | (dci<<8)`，EP0 恰好等效、键盘靠 QEMU 的 NAK 重试也能跑，但 U 盘的
//    EP4 门铃被当成 EP1 —— 批量传输永远不启动，INQUIRY 超时。）
static void xhci_db(uint32_t slot, uint32_t dci) {
    uint32_t v;
    switch (g_db_mode) {
    case 1:  v = dci << 16; break;                 // 规范：DB Target = bits31:16
    case 2:  v = 1u; break;                        // 只要求"写非 0"的实现
    default: v = dci; break;                       // QEMU/SeaBIOS 实测：目标在低字节
    }
    mmio_wr32(g_dboff + slot * 4u, v);
}

// ==================== PCI（0xCF8/0xCFC；与 usb64/hda64 同一套做法）====================
static uint32_t xpci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void xpci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, v);
}

// 找 xHCI：**class 0x0C / subclass 0x03 / prog-if 0x30**（PCI 规范里 USB 3.0 xHCI 的 prog-if）。
// 第一遍只看 prog-if=0x30（标准值）；找不到再放一遍"class/subclass 对但 prog-if 不是 30"
// 的候选（有些固件/桥会把 prog-if 报成别的值）——**但候选必须靠 xHCI 的能力寄存器复核**，
// 否则会把 EHCI(0x20)/OHCI(0x10)/UHCI(0x00) 误认成自家主控（那才是真的变砖）。
static bool xhci_pci_find() {
    for (int pass = 0; pass < 2; pass++) {
        uint32_t empty_run = 0;
        for (uint32_t bus = 0; bus < 256; bus++) {
            bool bus_has = false;
            for (uint32_t dev = 0; dev < 32; dev++) {
                uint32_t id = xpci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
                uint16_t vendor = (uint16_t)(id & 0xFFFFu);
                if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                bus_has = true;
                const uint32_t hdr = xpci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
                const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
                for (uint32_t fn = 0; fn < nfn; fn++) {
                    if (fn != 0) {
                        id = xpci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                        vendor = (uint16_t)(id & 0xFFFFu);
                        if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                    }
                    const uint32_t cc = xpci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x08);
                    if ((cc >> 24) != 0x0Cu || ((cc >> 16) & 0xFFu) != 0x03u) continue;
                    const uint8_t progif = (uint8_t)((cc >> 8) & 0xFFu);
                    if (pass == 0 && progif != 0x30u) continue;
                    if (pass == 1 && progif == 0x30u) continue;
                    g_bus = (uint8_t)bus; g_pcidev = (uint8_t)dev; g_fn = (uint8_t)fn;
                    g_progif = progif;
                    return true;
                }
            }
            if (bus_has) empty_run = 0;
            else if (++empty_run >= 4u) break;
        }
    }
    return false;
}

// ==================== 环 ====================
static void ring_init(XhciRing* r, volatile XhciTrb* mem) {
    r->t = mem;
    r->idx = 0;
    r->cycle = 1;
    memset_64((void*)(uintptr_t)mem, 0, XHCI_RING_TRBS * 16u);
    // Link TRB：指回环首；★ TC=1 —— 单段环每绕一圈都要翻转 Cycle（硬件按 TC 翻转自己的消费 cycle）
    volatile XhciTrb* l = &mem[XHCI_RING_TRBS - 1u];
    l->param = (uint64_t)(uintptr_t)mem;
    l->status = 0;
    l->control = XHCI_TRB_CYCLE | XHCI_TRB_TC | XHCI_TRB_TYPE(TRB_LINK);
}

// 入队一个 TRB，返回它的物理地址（事件里的 TRB Pointer 就是它，用来配对完成事件）
static uint64_t ring_put(XhciRing* r, uint64_t param, uint32_t status, uint32_t control) {
    volatile XhciTrb* t = &r->t[r->idx];
    const uint64_t pa = (uint64_t)(uintptr_t)t;
    t->param = param;
    t->status = status;
    __asm__ volatile("" ::: "memory");
    t->control = control | r->cycle;          // ★ 发布点：Cycle 位一次 32 位写进去
    r->idx++;
    if (r->idx >= XHCI_RING_TRBS - 1u) {
        volatile XhciTrb* l = &r->t[XHCI_RING_TRBS - 1u];
        l->param = (uint64_t)(uintptr_t)r->t;
        l->status = 0;
        __asm__ volatile("" ::: "memory");
        l->control = XHCI_TRB_TC | XHCI_TRB_TYPE(TRB_LINK) | r->cycle;
        r->idx = 0;
        r->cycle ^= 1u;
    }
    return pa;
}

// ==================== 事件环 ====================
static uint32_t ev_cc(uint32_t status) { return (status >> 24) & 0xFFu; }

static bool evt_next(XhciEvt* ev) {
    if (!g_evt_trbs) return false;
    volatile XhciTrb* t = &g_evt_trbs[g_evt_idx];
    if ((t->control & 1u) != g_evt_cycle) return false;   // 没有新事件（消费 cycle 不匹配）
    ev->param = t->param;
    ev->status = t->status;
    ev->control = t->control;
    __asm__ volatile("" ::: "memory");
    g_evt_idx = (g_evt_idx + 1u) & (XHCI_EVT_TRBS - 1u);  // 事件环无 Link TRB：2 的幂掩码回卷
    // ERDP 更新（见下面的说明：这里只在真的需要清 EHB 时才写 1）
    {
        const uint64_t erdp = rt_rd64(XHCI_IR_ERDP);
        const uint64_t next = (uint64_t)(uintptr_t)&g_evt_trbs[g_evt_idx];
        rt_wr64(XHCI_IR_ERDP, (erdp & 8u) ? (next | 8u) : next);
    }
    if (++g_evt_seen == 1u) {                              // 一次性证据：EHB 写过之后读回是 0
        xlog_begin();
        dbg64_str("[XHCI] erdp ehb cleared erdp=0x");
        dbg64_hex64(rt_rd64(XHCI_IR_ERDP));
        xlog_end();
    }
    return true;
}

static XhciDev* dev_by_slot(uint32_t slot) {
    for (int i = 0; i < XHCI_MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].slot == (uint8_t)slot) return &g_dev[i];
    return nullptr;
}

static void xhci_arm_int(XhciDev* d);

// 事件统一分派（等待循环与 kusb 轮询都走这里）：
//   * Transfer Event：HID 中断端点的报告 -> 解析 + 注入 + 重新武装；
//   * Port Status Change：打点（并清状态变化位）；本驱动不做运行期热插拔枚举。
static void xhci_dispatch(const XhciEvt* ev) {
    const uint32_t type = (ev->control >> 10) & 0x3Fu;
    const uint32_t slot = (ev->control >> 24) & 0xFFu;
    if (type == EVT_TRANSFER) {
        XhciDev* d = dev_by_slot(slot);
        // 对不上"HID 中断端点已武装的那个 TRB"的 Transfer Event：**如实打点**（有界）。
        // 为什么重要：这类事件如果被静默丢掉，现象就是"同步等待永远超时"（踩过：kusb 轮询
        // 提前把启动期枚举的完成事件消费掉了，硬件侧其实全都成功）。有这行就能一眼看出来。
        if (!d || !d->int_trb || ev->param != d->int_trb) {
            static uint32_t stray_logs = 0;
            if (stray_logs < 8u) {
                stray_logs++;
                xlog_begin();
                dbg64_str("[XHCI] stray transfer evt slot=");
                dbg64_dec(slot);
                dbg64_str(" ep=");
                dbg64_dec((ev->control >> 16) & 0x1Fu);
                dbg64_str(" p=0x");
                dbg64_hex64(ev->param);
                dbg64_str(" cc=");
                dbg64_dec(ev_cc(ev->status));
                xlog_end();
            }
            return;
        }
        const uint32_t res = ev->status & 0xFFFFFFu;
        uint32_t got = (d->kbd_mps > res) ? (uint32_t)(d->kbd_mps - res) : 0u;
        d->int_trb = 0;
        if (got > 0) {
            if (got < 8u) for (uint32_t i = got; i < 8u; i++) d->report[i] = 0;   // 短包：补零后照常解析
            d->reports++;
            (void)usb64_hid_report64(d->report, &d->hid, 1, &d->keys);
        }
        xhci_arm_int(d);                                                          // 重新武装
        return;
    }
    if (type == EVT_PORT_CHANGE) {
        const uint32_t port = (uint32_t)((ev->param >> 24) & 0xFFu);
        const uint32_t sc = (port >= 1u && port <= g_max_ports) ? port_rd(port) : 0u;
        xlog_begin();
        dbg64_str("[XHCI] port status change port=");
        dbg64_dec(port);
        dbg64_str(" portsc=0x");
        xhex(sc, 8);
        xlog_end();
        if (port >= 1u && port <= g_max_ports) port_wr(port, XHCI_PORTSC_CHANGE);  // 写 1 清
        return;
    }
    // Command Completion / MFINDEX Wrap 等：等待命令完成的循环自己配对，这里不做事
}

// ==================== 命令 ====================
// 提交一条命令并等它的完成事件（有界）。返回完成码（1 = 成功），*slot_out 拿 Slot ID。
static uint32_t xhci_cmd_wait(uint64_t pa, uint32_t timeout_ms, uint32_t* slot_out,
                              uint32_t* other_slot) {
    // ★ 命令环门铃（DB0 = Host Controller Doorbell）：值必须是 **0**（DB Target = 0 = "命令环有新 TRB"）。
    //   踩过：这里随设备门铃一起写成 1 时，xHC 会把 DB0 当成"空目标"直接不处理命令 ——
    //   现象是 Enable Slot 永远等不到完成事件（三个门铃约定全超时）。
    mmio_wr32(g_dboff, 0);
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(timeout_ms);
    uint64_t spin = 0;
    for (;;) {
        XhciEvt ev;
        while (evt_next(&ev)) {
            const uint32_t type = (ev.control >> 10) & 0x3Fu;
            if (type == EVT_CMD_COMPLETE) {
                if (ev.param == pa) {
                    if (slot_out) *slot_out = (ev.control >> 24) & 0xFFu;
                    return ev_cc(ev.status);
                }
                if (other_slot) *other_slot = (ev.control >> 24) & 0xFFu;   // 探门铃时的"陈旧"完成
                continue;
            }
            xhci_dispatch(&ev);
        }
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) return 0xFFFFFFFFu;
        nop_pause();
    }
}

// 等某个（或某几个）TRB 的 Transfer Event（有界）。返回完成码；*hit 拿命中的 TRB 地址。
static uint32_t xhci_xfer_wait(const uint64_t* pa, int n, uint32_t timeout_ms,
                               uint64_t* hit, uint32_t* res) {
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(timeout_ms);
    uint64_t spin = 0;
    for (;;) {
        XhciEvt ev;
        while (evt_next(&ev)) {
            const uint32_t type = (ev.control >> 10) & 0x3Fu;
            if (type == EVT_TRANSFER) {
                for (int i = 0; i < n; i++) {
                    if (pa[i] && ev.param == pa[i]) {
                        if (hit) *hit = pa[i];
                        if (res) *res = ev.status & 0xFFFFFFu;
                        return ev_cc(ev.status);
                    }
                }
            }
            xhci_dispatch(&ev);
        }
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) return 0xFFFFFFFFu;
        nop_pause();
    }
}

// 前置声明：第一条控制传输的门铃自探要在 xhci_control 定义之前（它自己定义在"传输"一节）
static int xhci_control(XhciDev* d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                        uint16_t len, uint8_t* buf, uint32_t* got);

// 第一台设备的**第一条控制传输**兼做"设备门铃（DB Target）位序"的判据：
// 命令门铃（DB0）恒为 0、与位序无关，所以位序只能靠"设备端点上的传输能不能完成"来定。
// 三种约定挨个试（每次换一种位序后重发同一条传输；环上旧 TRB 的事件按地址不匹配被忽略，
// 而 8 字节设备描述符读的是同一个缓冲，重发不会引入脏状态），谁先把数据取回来就用谁。
static int xhci_control_first(XhciDev* d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                              uint16_t len, uint8_t* buf) {
    int last = -9;
    for (uint32_t m = 0; m < 3u; m++) {
        last = xhci_control(d, rt, req, val, idx, len, buf, nullptr);
        if (last == 0) {
            if (m) {                                  // 换过位序：如实打点（QEMU/真机常见 0 时不打）
                xlog_begin();
                dbg64_str("[XHCI] device doorbell mode=");
                dbg64_dec(g_db_mode);
                xlog_end();
            }
            return 0;
        }
        g_db_mode = (g_db_mode + 1u) % 3u;
    }
    // 三次都失败：把"我给 xHC 的 EP0 环" vs "xHCI 实际用的（设备上下文里读回的）"打出来 —— 这两
    // 个不一样就说明上下文/上下文大小（CSZ）被理解错了，一样就说明问题在事件环那侧。
    {
        uint32_t* ep0in = (uint32_t*)(void*)(g_p_inctx + (size_t)2u * g_ctx_bytes);
        uint32_t* ep0dv = (uint32_t*)(void*)(d->ctx + (size_t)1u * g_ctx_bytes);
        const uint64_t in_pa = ((uint64_t)ep0in[3] << 32) | (ep0in[2] & 0xFFFFFFF0u);
        const uint64_t dv_pa = ((uint64_t)ep0dv[3] << 32) | (ep0dv[2] & 0xFFFFFFF0u);
        xlog_begin();
        dbg64_str("[XHCI] ep0 ring mine=0x");
        dbg64_hex64((uint64_t)(uintptr_t)d->ep0.t);
        dbg64_str(" inctx=0x");
        dbg64_hex64(in_pa);
        dbg64_str(" devctx=0x");
        dbg64_hex64(dv_pa);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(uint32_t)(-last));
        xlog_end();
    }
    return -1;
}

// ==================== 设备/输入上下文 ====================
// entry：设备上下文里 0 = Slot、d+0 = EP（DCI=d）；输入上下文里 0 = Input Control、1 = Slot、d+1 = EP
static inline uint32_t* inctx_dw(uint8_t* base, int entry, int dw) {
    return (uint32_t*)(base + (size_t)entry * g_ctx_bytes + (size_t)dw * 4u);
}

static uint32_t xspeed_code(uint32_t speed) {        // Slot Context 的 Speed 字段值
    switch (speed) {
    case 1: return 2;      // low
    case 2: return 1;      // full
    case 3: return 3;      // high
    case 4: return 4;      // super
    case 5: return 5;      // superplus
    default: return 0;
    }
}
static uint32_t xdefault_mps(uint32_t speed) {       // EP0 默认最大包
    switch (speed) {
    case 1: case 2: return 8;     // low / full（USB2 规范：默认 8）
    case 3: return 64;            // high
    case 4: case 5: return 512;   // super / superplus
    default: return 8;
    }
}

// 环里"下一个要执行的 TRB"的物理地址 + DCS（= 环当前的消费 cycle）。
//   ★ Address Device / Configure Endpoint 会把 xHC 的 dequeue **重置**成输入上下文里写的值，
//     所以这里必须写"当前该执行的那一个"而不是环首：写环首会让硬件把已经执行过的旧 TRB 再执行
//     一遍（踩过：Configure/Address 之后多出一个 "stray transfer evt"，旧控制传输被重放）。
static uint64_t ring_deq(const XhciRing* r) {
    return (uint64_t)(uintptr_t)&r->t[r->idx] | (uint64_t)(r->cycle & 1u);
}

static void fill_ep_ctx(uint8_t* base, int entry, uint32_t mps, uint32_t type,
                        uint64_t ring_deq_pa, uint32_t interval, uint32_t esit) {
    uint32_t* e = inctx_dw(base, entry, 0);
    // DP0 = Interval(7:0)；DP1 = MPS(31:16) | EP Type(5:3) | CErr(2:1，bit0 是保留位必须 0)
    //   ★ 原来写的是 "| 3u"：CErr 落成 1、还把保留位 bit0 置了 1。
    e[0] = (interval & 0xFFu) << 16;
    e[1] = (mps << 16) | (type << 3) | (3u << 1);
    e[2] = ((uint32_t)ring_deq_pa & 0xFFFFFFF0u) | ((uint32_t)ring_deq_pa & 1u);   // TR Dequeue + DCS
    e[3] = (uint32_t)(ring_deq_pa >> 32);
    e[4] = (esit << 16) | 8u;                             // Max ESIT Payload(31:16，只对 SS 有意义) | Average TRB Length(15:0)
}
// 命令环（全局一份：命令是串行的，一次只提交一条并等它的完成事件）
static XhciRing g_cmd_ring = { nullptr, 0, 1 };



static uint32_t xhci_cmd_addr_dev(XhciDev* d, uint32_t mps, bool bsr) {
    memset_64(g_p_inctx, 0, XHCI_CTX_BYTES);
    uint32_t* ic = inctx_dw(g_p_inctx, 0, 0);
    ic[1] = 0x3u;
    uint32_t* s = inctx_dw(g_p_inctx, 1, 0);
    s[0] = (xspeed_code(d->speed) << 20) | (1u << 27);
    s[1] = (uint32_t)d->port << 16;
    fill_ep_ctx(g_p_inctx, 2, mps ? mps : xdefault_mps(d->speed), EPT_CONTROL,
                ring_deq(&d->ep0), 0, 0);
    const uint64_t trb = ring_put(&g_cmd_ring, (uint64_t)(uintptr_t)g_p_inctx, 0,
                                  XHCI_TRB_TYPE(TRB_ADDRESS_DEV) | XHCI_TRB_SLOT(d->slot) |
                                  (bsr ? XHCI_TRB_BSR : 0u));
    return xhci_cmd_wait(trb, XHCI_CMD_TIMEOUT, nullptr, nullptr);
}

// Configure Endpoint 的一次尝试：Slot（Context Entries = 最高的 DCI）+ EP0（可选）+ 该设备的 EP。
//   with_ep0 = 把 EP0 也放进 Add Context 列表（规范/Linux 的写法：EP0 上下文跟着更新，此时必须
//   给一份**有效**的 EP0 上下文 —— 全 0 的 EP0 上下文会被 xHC 当 TRB Error）；
//   with_ep0 = false 是 BIOS 自带 xHCI 驱动的写法（不动 EP0，只配新端点）。
static uint32_t xhci_cmd_config_ep_try(XhciDev* d, bool with_ep0) {
    memset_64(g_p_inctx, 0, XHCI_CTX_BYTES);
    uint32_t* ic = inctx_dw(g_p_inctx, 0, 0);
    ic[7] = 1u;                                          // Configuration Value（SET_CONFIGURATION 的值）
    uint32_t add = with_ep0 ? 0x3u : 0x1u;               // Slot(A0) [+ EP0(A1)]
    uint32_t last = 1;
    uint32_t* s = inctx_dw(g_p_inctx, 1, 0);
    s[1] = (uint32_t)d->port << 16;
    // ★ EP0 进了 Add Context 列表就必须**给一份有效上下文**：全 0 的 EP0 上下文（MPS=0/类型=0/
    //   dequeue=0）是非法参数。MPS 用设备描述符里的值（与 Address Device 写进设备上下文的一致）。
    if (with_ep0) {
        fill_ep_ctx(g_p_inctx, 2, d->mps ? d->mps : 8u, EPT_CONTROL, ring_deq(&d->ep0), 0, 0);
    }
    if (d->is_kbd && d->kbd_dci) {
        const uint32_t ivl = d->kbd_interval ? (uint32_t)(d->kbd_interval - 1u) : 0u;
        // Max ESIT Payload 只对 SuperSpeed 端点有意义：HS/FS 端点写 0（与 QEMU 自带 xHCI 驱动一致）
        fill_ep_ctx(g_p_inctx, (int)d->kbd_dci + 1, d->kbd_mps, EPT_INT_IN,
                    ring_deq(&d->ep_int), ivl, 0);
        add |= 1u << d->kbd_dci;
        if (d->kbd_dci > last) last = d->kbd_dci;
    }
    if (d->is_msc) {
        if (d->in_dci) {
            fill_ep_ctx(g_p_inctx, (int)d->in_dci + 1, d->in_mps, EPT_BULK_IN,
                        ring_deq(&d->ep_in), 0, 0);
            add |= 1u << d->in_dci;
            if (d->in_dci > last) last = d->in_dci;
        }
        if (d->out_dci) {
            fill_ep_ctx(g_p_inctx, (int)d->out_dci + 1, d->out_mps, EPT_BULK_OUT,
                        ring_deq(&d->ep_out), 0, 0);
            add |= 1u << d->out_dci;
            if (d->out_dci > last) last = d->out_dci;
        }
    }
    ic[1] = add;
    s[0] = (xspeed_code(d->speed) << 20) | (last << 27);  // Context Entries = 最后一个有效的 DCI
    const uint64_t trb = ring_put(&g_cmd_ring, (uint64_t)(uintptr_t)g_p_inctx, 0,
                                  XHCI_TRB_TYPE(TRB_CONFIGURE_EP) | XHCI_TRB_SLOT(d->slot));
    return xhci_cmd_wait(trb, XHCI_CMD_TIMEOUT, nullptr, nullptr);
}

// 两条写法都试一遍（有界、最多两次命令）：先按规范带 EP0，cc 不是成功就退到 BIOS 的写法。
// 打点只在"退到第二条"时出现（正常路径不多一行）。
static uint32_t xhci_cmd_config_ep(XhciDev* d) {
    const uint32_t cc = xhci_cmd_config_ep_try(d, true);
    if (cc == XHCI_CC_SUCCESS) return cc;
    const uint32_t cc2 = xhci_cmd_config_ep_try(d, false);
    xlog_begin();
    dbg64_str("[XHCI] config ep retry without EP0: first cc=");
    dbg64_dec(cc);
    dbg64_str(" second cc=");
    dbg64_dec(cc2);
    xlog_end();
    return cc2;
}

// ==================== 传输 ====================
// 控制传输（Setup + Data* + Status）。返回 0 = 成功；*got 拿数据阶段实际字节数。
static int xhci_control(XhciDev* d, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                        uint16_t len, uint8_t* buf, uint32_t* got) {
    if (got) *got = 0;
    if (len > XHCI_CTL_BYTES) return -1;
    const bool in = (rt & 0x80u) != 0;
    const uint32_t trt = (len == 0) ? 0u : (in ? 3u : 2u);
    // ★ Setup Stage TRB：8 字节 Setup 包**内联**在 TRB 的 param 字段里，并且 **IDT(bit6) 必须置 1**
    //   （规范 6.4.1.1：Setup 包不来自内存；IDT=0 时 xHC 会把 param 当"指向 Setup 包的内存地址"）。
    //   踩过（这是原来记成"Transfer Event 不落事件环"那个 GAP 的真因）：老代码把 g_setup 的**地址**
    //   填进去、IDT 也没置 —— xHC 于是把 0x0008000001000680（其实是被当成地址的 Setup 包位模式）
    //   当内存地址去读，读不到东西也**不报错**：TRB 被取走、EP0 dequeue 照常前进、**事件永远不来**，
    //   控制传输全部超时 → 描述符读不回来 → 枚举断在这一步。事件环本身一直是好的。
    const uint64_t setup = (uint64_t)rt | ((uint64_t)req << 8) | ((uint64_t)val << 16) |
                           ((uint64_t)idx << 32) | ((uint64_t)len << 48);
    ring_put(&d->ep0, setup, 8u, XHCI_TRB_TYPE(TRB_SETUP) | XHCI_TRB_IDT | XHCI_TRB_TRT(trt));
    // 数据阶段：按 EP0 最大包切分（规范要求每个数据 TRB ≤ Max Packet Size）；多包用 CH 串成一条 TD。
    // ★ 数据阶段**不置 IOC**：一条控制传输只在 Status 阶段报一个事件（QEMU 自带的 xHCI 驱动/BIOS
    //   就是这么写的，实测 QEMU 按 IOC 逐 TRB 报事件）；事件里的残留量报的是整条传输没传完的
    //   字节数，短包照样算得出来（见下面 got 的算法）。
    uint8_t* dp = buf;
    uint32_t remain = len, last_len = 0;
    uint64_t pa_dlast = 0;
    while (remain > 0) {
        const uint32_t mps = d->mps ? d->mps : 8u;
        uint32_t chunk = (remain < mps) ? remain : mps;
        uint32_t ctl = XHCI_TRB_TYPE(TRB_DATA) | (in ? XHCI_TRB_DIR_IN : 0u);
        if (chunk != remain) ctl |= XHCI_TRB_CH;          // 多包数据阶段：串成一条 TD
        pa_dlast = ring_put(&d->ep0, (uint64_t)(uintptr_t)dp, chunk, ctl);
        last_len = chunk;
        dp += chunk; remain -= chunk;
    }
    // STATUS：方向与数据阶段相反；零长度；IOC 一定要（我们等它）
    const uint32_t dirst = (len > 0 && in) ? 0u : XHCI_TRB_DIR_IN;
    const uint64_t pa_status = ring_put(&d->ep0, 0, 0,
                                        XHCI_TRB_TYPE(TRB_STATUS) | dirst | XHCI_TRB_IOC);
    xhci_db(d->slot, 1);                                  // EP0 的 DCI = 1

    // 等事件：状态阶段（必需）+ 最后一个数据 TRB（拿实际字节数）。
    // 两个事件在同一次 drain 里就能收全（硬件按 TRB 顺序产生事件：数据阶段先、状态阶段后）。
    bool hit_st = false, hit_dt = false;
    uint32_t cc_st = 0, cc_dt = 0, res_dt = 0;
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(XHCI_CTL_TIMEOUT);
    uint64_t spin = 0;
    uint32_t n_ev = 0, last_cc = 0xFFFFu, last_st = 0, res_st = 0;
    uint64_t last_pa = 0;
    for (;;) {
        XhciEvt ev;
        while (evt_next(&ev)) {
            const uint32_t type = (ev.control >> 10) & 0x3Fu;
            if (type == EVT_TRANSFER) {
                n_ev++; last_pa = ev.param; last_st = ev.status; last_cc = ev_cc(ev.status);
                if (ev.param == pa_status) { hit_st = true; cc_st = ev_cc(ev.status); res_st = ev.status & 0xFFFFFFu; }
                else if (pa_dlast && ev.param == pa_dlast) {
                    hit_dt = true; cc_dt = ev_cc(ev.status); res_dt = ev.status & 0xFFFFFFu;
                } else xhci_dispatch(&ev);
            } else xhci_dispatch(&ev);
        }
        if (hit_st || hit_dt) {
            // ★ 一次性证据（自动验收断言的就是这一行）：Transfer Event **真的落进了事件环**、而且被
            //   认出来配对上了 —— 这正是原来那个"Transfer Event 不落事件环"GAP 的判据。
            static bool xfer_evt_logged = false;
            if (!xfer_evt_logged) {
                xfer_evt_logged = true;
                xlog_begin();
                dbg64_str("[XHCI] transfer evt ok idx=");
                dbg64_dec(g_evt_idx);
                dbg64_str(" seen=");
                dbg64_dec(g_evt_seen);
                xlog_end();
            }
            break;
        }
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) {
            // 排障：超时时把"我们等的是谁 / 收到过哪些 Transfer Event"打出来（有界，只在失败路径）
            xlog_begin();
            dbg64_str("[XHCI] ctl TIMEOUT want=0x");
            dbg64_hex64(pa_status);
            dbg64_str(" seen=");
            dbg64_dec(n_ev);
            dbg64_str(" last=0x");
            dbg64_hex64(last_pa);
            dbg64_str(" cc=");
            dbg64_dec(last_cc);
            dbg64_str(" st=0x");
            xhex(last_st, 8);
            xlog_end();
            return -2;
        }
        nop_pause();
    }
    const uint32_t cc = hit_st ? cc_st : cc_dt;
    if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PKT) return -3;
    if (len > 0) {
        // 状态阶段事件里的残留量 = **整条传输**没传完的字节数（规范 6.4.1.4 的 Residual 语义）；
        // 数据阶段的事件（现在不会产生，保留兜底）报的是数据阶段的残留。优先用状态阶段。
        uint32_t out = len;
        if (hit_st)      out = (res_st < len) ? (uint32_t)(len - res_st) : 0u;
        else if (hit_dt) out = (uint32_t)((len - last_len) + ((last_len > res_dt) ? (last_len - res_dt) : 0u));
        if (got) *got = out;
    }
    return 0;
}

// 一次批量传输（Bulk IN/OUT）：一个 Normal TRB 一个 TD（IOC 单独置位），短包即结束本次传输，
// 后续字节不再下发 —— 等价于"链"但不会被硬件提前取走后续 TRB（见文件头第 3 点）。
static int xhci_bulk(XhciDev* d, uint8_t dci, uint16_t mps, bool in, uint8_t* buf,
                     uint32_t len, uint32_t* got) {
    if (got) *got = 0;
    if (len == 0) return 0;
    if (len > XHCI_XFER_CHUNK) return -1;
    uint8_t* dma = buf;
    bool bounce = false;
    if (xhci_pa64(buf) == 0) {
        if (!g_p_bounce || len > 4096u) return -2;
        dma = g_p_bounce;
        bounce = true;
        if (!in) memcpy_64(dma, buf, len);
    }
    XhciRing* r = in ? &d->ep_in : &d->ep_out;
    uint32_t total = 0, remain = len;
    uint8_t* p = dma;
    const uint32_t chunk_max = (mps ? (uint32_t)mps * 15u : 64u * 15u);   // TD Size 语义：≤15 包
    while (remain > 0) {
        uint32_t chunk = (remain < XHCI_XFER_CHUNK) ? remain : XHCI_XFER_CHUNK;
        if (chunk > chunk_max) chunk = chunk_max;
        const uint32_t pa = xhci_pa64(p);
        if (pa == 0) return -2;
        const uint64_t trb = ring_put(r, (uint64_t)pa, chunk, XHCI_TRB_TYPE(TRB_NORMAL) | XHCI_TRB_IOC);
        xhci_db(d->slot, dci);
        uint32_t res = 0;
        const uint32_t cc = xhci_xfer_wait(&trb, 1, XHCI_BULK_TIMEOUT, nullptr, &res);
        if (cc != XHCI_CC_SUCCESS && cc != XHCI_CC_SHORT_PKT) {
            if (bounce && in && total > 0) memcpy_64(buf, dma, total);
            if (got) *got = total;
            return -3;
        }
        const uint32_t act = (chunk > res) ? (chunk - res) : 0u;
        total += act;
        p += chunk; remain -= chunk;
        if (act < chunk) break;                                  // 短包 = 本次传输结束
    }
    if (bounce && in && total > 0) memcpy_64(buf, dma, total);
    if (got) *got = total;
    return 0;
}

// 中断 IN（HID 键盘）：一个 Normal TRB；事件回来 -> 解析报告 -> 重新武装
static void xhci_arm_int(XhciDev* d) {
    if (!d->kbd_dci || !d->report) return;
    const uint32_t len = d->kbd_mps ? d->kbd_mps : 8u;
    d->int_trb = ring_put(&d->ep_int, (uint64_t)(uintptr_t)d->report, len,
                          XHCI_TRB_TYPE(TRB_NORMAL) | XHCI_TRB_IOC);
    xhci_db(d->slot, d->kbd_dci);
}

// ==================== 端口复位（PORTSC.PR -> 等 PRC）====================
// 先给端口上电（PPC=1 时 PP 必须为 1）-> 清状态变化位 -> 置 PR -> 等 PRC（等的时候采 MFINDEX
// 证明时间在走）-> 写 1 清 PRC/CSC 等 -> 读 Port Speed。USB2/USB3 走同一套（区别只在读回的速度码）。
static bool xhci_port_reset(uint32_t n, uint32_t* speed_out) {
    uint32_t v = port_rd(n);
    if (!(v & XHCI_PORTSC_CCS)) return false;               // 没插设备
    if (!(v & XHCI_PORTSC_PP)) {
        port_wr(n, XHCI_PORTSC_PP);                          // 上电（PPC=1 的端口必须；PPC=0 也无害）
        xdelay_ms(20);
        v = port_rd(n);
    }
    port_wr(n, XHCI_PORTSC_CHANGE);                          // 写 1 清所有状态变化位
    xdelay_ms(1);

    const uint32_t mf0 = rt_rd32(XHCI_RT_MFINDEX);
    port_wr(n, XHCI_PORTSC_PP | XHCI_PORTSC_PR | XHCI_PORTSC_CHANGE);   // ★ 置 PR = 开始复位
    uint32_t prc = 0;
    uint64_t spin = 0;
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(500);                 // 复位上限 500ms（有界）
    for (;;) {
        v = port_rd(n);
        if (v & XHCI_PORTSC_PRC) { prc = 1; break; }
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) break;
        nop_pause();
    }
    uint32_t mf1 = rt_rd32(XHCI_RT_MFINDEX);
    if (!prc) return false;
    // ★ QEMU 不模拟"复位 10ms"，PRC 可能在同一 MFINDEX tick 内就置起来（delta 读到 0 是**采样
    //   相位问题**，不是时间没走）。要证明"时间真的在走"，就在需要时等到下一个 MFINDEX tick
    //   再采样（MFINDEX 是 125us 一跳的自由计数器，等一拍开销 ≤125us；有界）。
    if (mf1 == mf0) {
        uint64_t s2 = 0;
        while (rt_rd32(XHCI_RT_MFINDEX) == mf0 && s2 < 20000000ull) { s2++; nop_pause(); }
        mf1 = rt_rd32(XHCI_RT_MFINDEX);
    }
    port_wr(n, XHCI_PORTSC_CHANGE);                           // 写 1 清 PRC/CSC/WRC/OCC/PLC/CEC
    xdelay_ms(2);
    if (g_ports_seen == 0) {                                  // 一次性证据：MFINDEX 在走
        xlog_begin();
        dbg64_str("[XHCI] mfindex=0x");
        xhex(mf0, 4);
        dbg64_str(" delta=0x");
        xhex((mf1 - mf0) & 0x1FFFu, 4);
        xlog_end();
    }
    v = port_rd(n);
    *speed_out = (v & XHCI_PORTSC_SPEED_M) >> XHCI_PORTSC_SPEED_S;
    return (v & XHCI_PORTSC_CCS) != 0;
}

// ==================== 描述符解析 ====================
// 只看两类：HID 引导键盘（class 3 / sub 1 / proto 1 + 中断 IN）与 BOT 存储（class 8 / sub 6 /
// proto 0x50 + 批量 IN/OUT）。其余接口（鼠标/集线器…）如实忽略。
static void xhci_parse_config(XhciDev* d, const uint8_t* cd, uint32_t len) {
    uint32_t off = 0;
    while (off + 2u <= len) {
        const uint8_t blen = cd[off], btype = cd[off + 1u];
        if (blen < 2u || off + blen > len) break;
        if (btype == 4u && blen >= 9u) {                     // Interface
            d->ifaces++;
            const uint8_t cls = cd[off + 5u], sub = cd[off + 6u], proto = cd[off + 7u];
            if (cls == 3u && sub == 1u && proto == 1u && !d->is_kbd) {
                d->is_kbd = true;
                d->hid_iface = cd[off + 2u];
            } else if (cls == 8u && sub == 6u && proto == 0x50u && !d->is_msc) {
                d->is_msc = true;
            }
        } else if (btype == 5u && blen >= 7u) {              // Endpoint
            const uint8_t addr = cd[off + 2u];
            const uint8_t attr = (uint8_t)(cd[off + 3u] & 0x03u);
            uint16_t mps = (uint16_t)(cd[off + 4u] | ((uint16_t)cd[off + 5u] << 8));
            mps = (uint16_t)(mps & 0x7FFu);
            const uint8_t ivl = cd[off + 6u];
            const uint8_t epn = (uint8_t)(addr & 0x0Fu);
            const bool in = (addr & 0x80u) != 0;
            if (epn != 0) {
                if (attr == 3u && in && d->is_kbd && !d->kbd_dci) {
                    d->kbd_dci = (uint8_t)(epn * 2u + 1u);
                    d->kbd_mps = (uint8_t)(mps > 64u ? 64u : mps);
                    d->kbd_interval = ivl;
                } else if (attr == 2u && d->is_msc) {
                    if (in && !d->in_dci) { d->in_dci = (uint8_t)(epn * 2u + 1u); d->in_mps = mps; }
                    else if (!in && !d->out_dci) { d->out_dci = (uint8_t)(epn * 2u); d->out_mps = mps; }
                }
            }
        }
        off += blen;
    }
}

// ==================== 枚举 ====================
static int xhci_enum_port(uint32_t port, uint32_t speed) {
    XhciDev* d = nullptr;
    for (int i = 0; i < XHCI_MAX_DEV; i++) if (!g_dev[i].used) { d = &g_dev[i]; break; }
    if (!d) return -1;

    // 1) Enable Slot：第一台设备复用门铃自探那条命令拿到的槽（g_probe_slot），其余自己发命令
    uint32_t slot = 0;
    uint32_t cc;
    if (g_probe_slot) { slot = g_probe_slot; cc = XHCI_CC_SUCCESS; }
    else {
        const uint64_t trb = ring_put(&g_cmd_ring, 0, 0, XHCI_TRB_TYPE(TRB_ENABLE_SLOT));
        cc = xhci_cmd_wait(trb, XHCI_CMD_TIMEOUT, &slot, nullptr);
    }
    if (cc != XHCI_CC_SUCCESS || slot == 0 || slot > g_max_slots) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=enable-slot rc=");
        dbg64_dec(cc);
        xlog_end();
        return -1;
    }
    d->used = true;
    d->slot = (uint8_t)slot;
    d->port = (uint8_t)port;
    d->speed = (uint8_t)speed;
    d->mps = (uint8_t)xdefault_mps(speed);
    d->ctx = (uint8_t*)page_alloc_64();
    uint8_t* rings = (uint8_t*)page_alloc_64();
    if (!d->ctx || !rings) { d->used = false; return -1; }
    memset_64(d->ctx, 0, PAGE_SIZE_64);
    memset_64(rings, 0, PAGE_SIZE_64);
    ring_init(&d->ep0,    (volatile XhciTrb*)(void*)(rings + 0x000u));
    ring_init(&d->ep_in,  (volatile XhciTrb*)(void*)(rings + 0x400u));
    ring_init(&d->ep_out, (volatile XhciTrb*)(void*)(rings + 0x800u));
    ring_init(&d->ep_int, (volatile XhciTrb*)(void*)(rings + 0xC00u));
    ((uint64_t*)(void*)g_p_dcbaa)[slot] = (uint64_t)(uintptr_t)d->ctx;   // DCBAA[slot] = 设备上下文

    // 2) Address Device（BSR=1：先把上下文建好、暂不定址）-> 读设备描述符前 8 字节
    uint32_t mps = xdefault_mps(speed);
    cc = xhci_cmd_addr_dev(d, mps, true);
    xlog_begin();
    dbg64_str("[XHCI] addr-dev bsr=1 cc=");
    dbg64_dec(cc);
    dbg64_str(" mps=");
    dbg64_dec(mps);
    xlog_end();
    if (cc == XHCI_CC_SUCCESS) {
        uint8_t dd[18];
        memset_64(dd, 0, sizeof(dd));
        // ★ 第一条控制传输兼做设备门铃位序自探（见 xhci_control_first），后面就固定用探到的位序
        const int r8 = xhci_control_first(d, 0x80u, 6u, 0x0100u, 0, 8, dd);
        xlog_begin();
        dbg64_str("[XHCI] desc8 rc=");
        dbg64_dec((uint64_t)(uint32_t)(-r8));
        dbg64_str(" bmaxpkt0=");
        dbg64_dec(dd[7]);
        xlog_end();
        if (r8 == 0 && dd[7] != 0) {
            // ★ SuperSpeed 设备的 bMaxPacketSize0 是**指数**编码（9 = 512 字节），不能直接当 EP0 的
            //   MPS 用 —— 踩过：U 盘报 9，EP0 上下文被写成 MPS=9，后面的传输全失败（INQUIRY 超时）。
            mps = (speed >= 4u) ? 512u : dd[7];
            d->vendor  = (uint16_t)(dd[8] | ((uint16_t)dd[9] << 8));
            d->product = (uint16_t)(dd[10] | ((uint16_t)dd[11] << 8));
        }
    }

    // 3) Address Device（BSR=0：真正定址，地址 = Slot ID）
    cc = xhci_cmd_addr_dev(d, mps, false);
    if (cc != XHCI_CC_SUCCESS) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=address-device rc=");
        dbg64_dec(cc);
        xlog_end();
        d->ep0.t = nullptr;
        d->used = false;
        ((uint64_t*)(void*)g_p_dcbaa)[slot] = 0;
        return -1;
    }

    // 4) 完整的设备描述符 + 配置描述符（先读 9 字节拿 wTotalLength，再读全）
    uint8_t dd[18];
    memset_64(dd, 0, sizeof(dd));
    if (xhci_control(d, 0x80u, 6u, 0x0100u, 0, 18, dd, nullptr) != 0) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=device-desc");
        xlog_end();
        d->used = false;
        return -1;
    }
    d->vendor  = (uint16_t)(dd[8] | ((uint16_t)dd[9] << 8));
    d->product = (uint16_t)(dd[10] | ((uint16_t)dd[11] << 8));
    // 设备描述符里的 bMaxPacketSize0 与 EP0 上下文不一致时用 Evaluate Context 更新？——
    // 不用：上面已经用 BSR=0 的 Address Device 把 mps 定成描述符里的值（那是唯一可靠来源）。
    uint8_t* cd = g_desc;
    memset_64(cd, 0, XHCI_CTL_BYTES);
    uint32_t got = 0;
    if (xhci_control(d, 0x80u, 6u, 0x0200u, 0, 9, cd, &got) != 0 || got < 9u) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=config-desc");
        xlog_end();
        d->used = false;
        return -1;
    }
    uint32_t total = (uint32_t)(cd[2] | ((uint32_t)cd[3] << 8));
    if (total > XHCI_CTL_BYTES) total = XHCI_CTL_BYTES;
    if (total < 9u) total = 9u;
    memset_64(cd, 0, XHCI_CTL_BYTES);
    if (xhci_control(d, 0x80u, 6u, 0x0200u, 0, (uint16_t)total, cd, &got) != 0) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=config-desc-full");
        xlog_end();
        d->used = false;
        return -1;
    }
    xhci_parse_config(d, cd, (got && got < total) ? got : total);

    // 5) SET_CONFIGURATION(1)
    if (xhci_control(d, 0x00u, 9u, 1u, 0, 0, nullptr, nullptr) != 0) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=set-config");
        xlog_end();
        d->used = false;
        return -1;
    }
    if (!d->is_kbd && !d->is_msc) {                      // 认不出来的设备：如实打点并放掉
        xlog_begin();
        dbg64_str("[XHCI] device addr=");
        dbg64_dec(slot);
        dbg64_str(" unsupported (not HID boot keyboard / BOT storage)");
        xlog_end();
        d->used = false;
        return -1;
    }

    // 6) 键盘：SET_PROTOCOL(boot) + SET_IDLE(0) -> Configure Endpoint -> 武装中断 TRB
    if (d->is_kbd && d->kbd_dci) {
        (void)xhci_control(d, 0x21u, 0x0Bu, 0, d->hid_iface, 0, nullptr, nullptr);   // SET_PROTOCOL
        (void)xhci_control(d, 0x21u, 0x0Au, 0, d->hid_iface, 0, nullptr, nullptr);   // SET_IDLE
    }
    cc = xhci_cmd_config_ep(d);
    if (cc != XHCI_CC_SUCCESS) {
        xlog_begin();
        dbg64_str("[XHCI] enum FAILED stage=configure-endpoint rc=");
        dbg64_dec(cc);
        xlog_end();
        d->used = false;
        return -1;
    }

    xlog_begin();
    dbg64_str("[XHCI] device addr=");
    dbg64_dec(slot);
    dbg64_str(" speed=");
    dbg64_str(xspeed_name(speed));
    dbg64_str(" mps=");
    dbg64_dec(mps);
    dbg64_str(" vendor=");
    xhex(d->vendor, 4);
    dbg64_str(" product=");
    xhex(d->product, 4);
    xlog_end();

    if (d->is_kbd && d->kbd_dci) {
        xlog_begin();
        dbg64_str("[XHCI] config set value=1 ifaces=");
        dbg64_dec(d->ifaces);
        dbg64_str(" hid=1 ep_in=");
        xhex(d->kbd_dci, 2);
        dbg64_str(" mps=");
        dbg64_dec(d->kbd_mps);
        dbg64_str(" interval=");
        dbg64_dec(d->kbd_interval);
        xlog_end();
        xlog_begin();
        dbg64_str("[XHCI] hid boot protocol set (8-byte reports)");
        xlog_end();
        xhci_arm_int(d);
    }
    if (d->is_msc && d->in_dci && d->out_dci) {
        xlog_begin();
        dbg64_str("[XHCI] config set value=1 ifaces=");
        dbg64_dec(d->ifaces);
        dbg64_str(" msc=1 ep_in=");
        xhex(d->in_dci, 2);
        dbg64_str(" mps=");
        dbg64_dec(d->in_mps);
        dbg64_str(" ep_out=");
        xhex(d->out_dci, 2);
        dbg64_str(" mps=");
        dbg64_dec(d->out_mps);
        xlog_end();
    }
    return 0;
}

// ==================== 存储：BOT + SCSI 只读 ====================
static uint32_t msc_rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint32_t msc_rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static void msc_wr32be(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}
static void msc_wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
// INQUIRY 的厂商/型号是空格填充的定长字段：去掉尾空格，不可打印字节打 '.'
static void msc_log_str(const char* s, int n) {
    int last = n - 1;
    while (last >= 0 && (s[last] == ' ' || s[last] == 0)) last--;
    for (int i = 0; i <= last; i++) {
        const char ch = s[i];
        dbg64_putc((ch >= 0x20 && ch < 0x7F) ? ch : '.');
    }
}

// 一条 BOT 命令：CBW -> [数据阶段] -> CSW。返回 0 通过；1/2 = CSW 报的命令失败/阶段错；<0 传输层失败。
static int xhci_bot(XhciDev* d, const uint8_t* cdb, uint32_t cdblen, bool in,
                    uint32_t dlen, uint8_t* data, uint32_t* dgot) {
    if (dgot) *dgot = 0;
    uint8_t* cbw = g_cbw;
    memset_64(cbw, 0, 32);
    cbw[0] = 0x55; cbw[1] = 0x53; cbw[2] = 0x42; cbw[3] = 0x43;        // "USBC" = CBW 签名
    msc_wr32le(cbw + 4, d->tag);
    msc_wr32le(cbw + 8, dlen);
    cbw[12] = in ? 0x80u : 0u;
    cbw[13] = 0;                                                      // LUN 0（本驱动不打多 LUN）
    cbw[14] = (uint8_t)cdblen;
    for (uint32_t i = 0; i < cdblen && i < 16u; i++) cbw[15 + i] = cdb[i];
    d->tag++;
    uint32_t put = 0;
    if (xhci_bulk(d, d->out_dci, d->out_mps, false, cbw, 31, &put) != 0 || put != 31u) return -1;
    if (dlen > 0) {
        uint32_t got = 0;
        if (xhci_bulk(d, in ? d->in_dci : d->out_dci, in ? d->in_mps : d->out_mps,
                      in, data, dlen, &got) != 0) return -2;
        if (dgot) *dgot = got;
    }
    uint8_t* csw = g_csw;
    memset_64(csw, 0, 16);
    uint32_t got = 0;
    if (xhci_bulk(d, d->in_dci, d->in_mps, true, csw, 13, &got) != 0 || got != 13u) return -3;
    if (csw[0] != 0x55 || csw[1] != 0x53 || csw[2] != 0x42 || csw[3] != 0x53) return -4;   // "USBS"
    if (msc_rd32le(csw + 4) != d->tag - 1u) return -5;                 // Tag 必须对上（CBW/CSW 都是小端）
    d->csw_status = csw[12];
    return csw[12] ? (csw[12] == 2 ? 2 : 1) : 0;
}

static int xhci_msc_read10(XhciDev* d, uint32_t lba, uint16_t count, uint8_t* buf) {
    if (!d->msc_ok) return -1;
    if (count == 0 || count > XHCI_MSC_SECTORS) return -1;
    uint8_t cdb[16];
    memset_64(cdb, 0, sizeof(cdb));
    cdb[0] = 0x28;                                        // READ(10)
    msc_wr32be(cdb + 2, lba);
    cdb[7] = (uint8_t)(count >> 8);
    cdb[8] = (uint8_t)count;
    uint32_t got = 0;
    const int r = xhci_bot(d, cdb, 10, true, (uint32_t)count * 512u, buf, &got);
    if (r != 0) return r;
    return (got == (uint32_t)count * 512u) ? 0 : -6;      // 短读也算失败（调用方按失败处理）
}

// 探测：INQUIRY -> TEST UNIT READY -> READ CAPACITY(10) -> READ(10) LBA0（带 CRC32 打点）
static void xhci_msc_probe(XhciDev* d) {
    uint8_t* sc = g_msc_scratch;
    uint8_t cdb[16];
    memset_64(sc, 0, 64);
    memset_64(cdb, 0, sizeof(cdb));
    cdb[0] = 0x12;                                        // INQUIRY
    cdb[4] = 36;                                          // allocation length
    uint32_t got = 0;
    if (xhci_bot(d, cdb, 6, true, 36, sc, &got) != 0 || got < 36u) {
        xlog_begin();
        dbg64_str("[XHCI] msc probe FAILED stage=inquiry");
        xlog_end();
        return;
    }
    for (int i = 0; i < 8; i++) d->vendor_s[i]  = sc[8 + i];
    for (int i = 0; i < 16; i++) d->product_s[i] = sc[16 + i];
    xlog_begin();
    dbg64_str("[XHCI] msc inquiry vendor=");
    msc_log_str((const char*)d->vendor_s, 8);
    dbg64_str(" product=");
    msc_log_str((const char*)d->product_s, 16);
    xlog_end();

    memset_64(cdb, 0, sizeof(cdb));                       // TEST UNIT READY
    (void)xhci_bot(d, cdb, 6, false, 0, nullptr, nullptr);

    memset_64(sc, 0, 64);
    memset_64(cdb, 0, sizeof(cdb));
    cdb[0] = 0x25;                                        // READ CAPACITY(10)
    if (xhci_bot(d, cdb, 10, true, 8, sc, &got) != 0 || got < 8u) {
        xlog_begin();
        dbg64_str("[XHCI] msc probe FAILED stage=read-capacity");
        xlog_end();
        return;
    }
    d->blocks = msc_rd32be(sc) + 1u;                      // "最后一个可寻址块 + 1"
    d->block_size = msc_rd32be(sc + 4);
    if (d->block_size != 512u) {                          // 只支持 512B：如实拒绝，不暴露成块设备
        xlog_begin();
        dbg64_str("[XHCI] msc unsupported block_size=");
        dbg64_dec(d->block_size);
        xlog_end();
        return;
    }
    xlog_begin();
    dbg64_str("[XHCI] msc capacity blocks=");
    dbg64_dec(d->blocks);
    dbg64_str(" block_size=");
    dbg64_dec(d->block_size);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)d->blocks * 512ull);
    xlog_end();

    // ★ 读 LBA 0 一块并打 CRC32 + 头 16 字节：**宿主侧逐字节核对的依据**
    d->msc_ok = true;                                     // 先允许 read10（它自己会检查）
    memset_64(g_sector, 0, 512);
    if (xhci_msc_read10(d, 0, 1, g_sector) != 0) {
        d->msc_ok = false;
        xlog_begin();
        dbg64_str("[XHCI] msc probe FAILED stage=read10-lba0");
        xlog_end();
        return;
    }
    xlog_begin();
    dbg64_str("[XHCI] msc READ(10) ok lba=0 count=1 bytes=512 crc=");
    xhex(xcrc32(g_sector, 512), 8);
    dbg64_str(" head=");
    for (int i = 0; i < 16; i++) xhex(g_sector[i], 2);
    xlog_end();
    d->msc_ok = true;
}

// ==================== init ====================
static void xhci_selftest_log() {
    xlog_begin();
    if (!g_found) {
        dbg64_str("[XHCI] selftest skipped (no controller)");
    } else if (g_selftest_mask) {
        dbg64_str("[XHCI] selftest FAIL mask=");
        dbg64_dec(g_selftest_mask);
    } else {
        dbg64_str("[XHCI] selftest PASS mask=0");
    }
    xlog_end();
}

int xhci64_init64() {
    if (g_inited) return g_ready ? 0 : -1;
    g_inited = true;

    // ---- 1) PCI + BAR0 ----
    if (!xhci_pci_find()) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    const uint32_t cmd = xpci_rd32(g_bus, g_pcidev, g_fn, 0x04);
    xpci_wr32(g_bus, g_pcidev, g_fn, 0x04, (cmd & 0xFFFF0000u) | 0x0006u);   // MEM + BUS MASTER
    const uint32_t b0 = xpci_rd32(g_bus, g_pcidev, g_fn, 0x10);
    uint64_t bar = (uint64_t)(b0 & 0xFFFFFFF0u);
    if (b0 & 0x1u) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found (BAR0 is IO)");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    if ((b0 & 0x6u) == 0x4u)                                             // 64 位 MMIO BAR
        bar |= (uint64_t)xpci_rd32(g_bus, g_pcidev, g_fn, 0x14) << 32;
    if (bar == 0 || bar >= 0x100000000ULL) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found (BAR0 out of identity map)");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    g_cap = bar;

    // ---- 2) 能力寄存器（顺便把"prog-if != 0x30 的候选"复核掉）----
    const uint32_t cap0 = mmio_rd32(XHCI_CAP_CAPLENGTH);
    g_caplen = cap0 & 0xFFu;
    const uint32_t hcs1 = mmio_rd32(XHCI_CAP_HCSPARAMS1);
    const uint32_t hcs2 = mmio_rd32(XHCI_CAP_HCSPARAMS2);
    const uint32_t hcc1 = mmio_rd32(XHCI_CAP_HCCPARAMS1);
    g_max_slots = hcs1 & 0xFFu;
    g_max_ports = (hcs1 >> 24) & 0xFFu;
    g_csz = (hcc1 >> 2) & 1u;
    g_ac64 = hcc1 & 1u;
    g_ctx_bytes = g_csz ? 64u : 32u;
    g_max_scratch = ((hcs2 >> 21) & 0x1Fu) | ((hcs2 >> 27) << 5);
    if (g_caplen < 0x20u || g_caplen > 0x100u || g_max_slots == 0 || g_max_ports == 0 ||
        g_max_slots > 255u) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found (capability check failed, progif=");
        xhex(g_progif, 2);
        dbg64_str(")");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    g_rtsoff = mmio_rd32(XHCI_CAP_RTSOFF) & 0xFFFFFFE0u;
    g_dboff  = mmio_rd32(XHCI_CAP_DBOFF) & 0xFFFFFFFCu;
    if (g_rtsoff == 0 || g_dboff == 0) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found (bad RTSOFF/DBOFF)");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    xlog_begin();
    dbg64_str("[XHCI] pci ");
    dbg64_dec(g_bus); dbg64_putc(':'); dbg64_dec(g_pcidev); dbg64_putc('.'); dbg64_dec(g_fn);
    dbg64_str(" bar0=0x");
    dbg64_hex64(bar);
    dbg64_str(" caplen=0x");
    xhex(g_caplen, 2);
    dbg64_str(" hcs1=0x");
    xhex(hcs1, 8);
    dbg64_str(" max_slots=");
    dbg64_dec(g_max_slots);
    dbg64_str(" max_ports=");
    dbg64_dec(g_max_ports);
    dbg64_str(" csz=");
    dbg64_dec(g_csz);
    dbg64_str(" ver=0x");
    xhex((cap0 >> 16) & 0xFFFFu, 4);
    dbg64_str(" ac64=");
    dbg64_dec(g_ac64);
    xlog_end();
    g_found = true;                                     // ★ 主控认下来了（运行期轮询/状态查询都以它为准）
    // 端口寄存器组本身的偏移是规范固定的（0x400 + 0x10*(n-1)），所以这里不拿它当寻址依据。
    // ★ HCCPARAMS1.xECP 与每个扩展能力的 NEXT 都是 **dword 偏移**（规范 5.4.1.1 / 7.2）。
    //   踩过：原来漏了 "<<" 2"，从偏移 8（= HCSPARAMS2 的值）开始走，第一个能力根本不是
    //   Supported Protocol、NEXT=0 立刻退出 —— 于是 [XHCI] proto 这一行从来没打过。
    //   Supported Protocol 的端口偏移/个数在能力的 **DW2**（原来错读成 DW1 = Name String）。
    for (uint32_t xecp = (((hcc1 >> 16) & 0xFFFFu) << 2); xecp && xecp < 0x1000u; ) {
        const uint32_t dw0 = mmio_rd32(xecp);
        const uint32_t id = dw0 & 0xFFu;
        const uint32_t next = (dw0 >> 8) & 0xFFu;
        if (id == 2u) {                                     // Supported Protocol
            const uint32_t dw2 = mmio_rd32(xecp + 8u);
            xlog_begin();
            dbg64_str("[XHCI] proto rev=");
            dbg64_dec((dw0 >> 24) & 0xFFu);
            dbg64_str(" portoff=0x");
            xhex(dw2 & 0xFFu, 2);
            dbg64_str(" ports=");
            dbg64_dec((dw2 >> 8) & 0xFFu);
            xlog_end();
        }
        if (!next) break;
        xecp += (next << 2);
    }

    // ---- 4) DMA 页 ----
    g_p_rings = (uint8_t*)page_alloc_64();
    g_p_dcbaa = (uint8_t*)page_alloc_64();
    g_p_inctx = (uint8_t*)page_alloc_64();
    g_p_data  = (uint8_t*)page_alloc_64();
    g_p_bounce= (uint8_t*)page_alloc_64();
    if (!g_p_rings || !g_p_dcbaa || !g_p_inctx || !g_p_data || !g_p_bounce) {
        g_state = XHCI_ST_NOT_FOUND;
        xlog_begin();
        dbg64_str("[XHCI] not found (out of pages)");
        xlog_end();
        xhci_selftest_log();
        return -1;
    }
    memset_64(g_p_rings, 0, PAGE_SIZE_64);
    memset_64(g_p_dcbaa, 0, PAGE_SIZE_64);
    memset_64(g_p_inctx, 0, PAGE_SIZE_64);
    memset_64(g_p_data,  0, PAGE_SIZE_64);
    memset_64(g_p_bounce,0, PAGE_SIZE_64);
    g_cmd_trbs = (volatile XhciTrb*)(void*)(g_p_rings + 0x000u);      // 命令环（含 Link TRB）
    g_evt_trbs = (volatile XhciTrb*)(void*)(g_p_rings + 0x400u);      // 事件环（无 Link TRB）
    g_desc    = g_p_data + XHCI_OFF_DESC;
    g_sector  = g_p_data + XHCI_OFF_SECTOR;
    g_cbw     = g_p_data + XHCI_OFF_CBW;
    g_csw     = g_p_data + XHCI_OFF_CSW;
    g_msc_scratch = g_p_data + XHCI_OFF_SCRATCH;
    g_dev[0].report = g_p_data + XHCI_OFF_REP0;
    g_dev[1].report = g_p_data + XHCI_OFF_REP1;

    // scratchpad：HCSPARAMS2 说要几个就准备几个（真机常见 0；有界到 XHCI_SCRATCH_MAX）
    if (g_max_scratch) {
        uint32_t n = g_max_scratch;
        if (n > XHCI_SCRATCH_MAX) n = XHCI_SCRATCH_MAX;
        uint64_t* arr = (uint64_t*)(void*)(g_p_dcbaa + 0x800u);       // 指针数组（64B 对齐）
        for (uint32_t i = 0; i < n; i++) {
            g_p_scratch[i] = (uint8_t*)page_alloc_64();
            if (!g_p_scratch[i]) break;
            memset_64(g_p_scratch[i], 0, PAGE_SIZE_64);
            arr[i] = (uint64_t)(uintptr_t)g_p_scratch[i];
            g_scratch++;
        }
        if (g_scratch) ((uint64_t*)(void*)g_p_dcbaa)[0] = (uint64_t)(uintptr_t)arr;
    }

    // ---- 5) 停控制器 + HCRST ----
    op_wr32(XHCI_OP_USBCMD, op_rd32(XHCI_OP_USBCMD) & ~XHCI_USBCMD_RS);   // RS=0
    {
        uint64_t spin = 0;
        while (!(op_rd32(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) && spin < 200000000ull) { spin++; nop_pause(); }
    }
    op_wr32(XHCI_OP_USBCMD, XHCI_USBCMD_HCRST);                           // ★ 软复位（自清）
    {
        uint64_t spin = 0;
        while ((op_rd32(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) && spin < 4000000000ull) { spin++; nop_pause(); }
        if (op_rd32(XHCI_OP_USBCMD) & XHCI_USBCMD_HCRST) g_selftest_mask |= 1u;
    }
    xlog_begin();
    dbg64_str("[XHCI] reset HCRST ok usbcmd=0x");
    xhex(op_rd32(XHCI_OP_USBCMD), 8);
    dbg64_str(" usbsts=0x");
    xhex(op_rd32(XHCI_OP_USBSTS), 8);
    xlog_end();

    // ---- 6) 编程序列：CONFIG / DCBAAP / CRCR / ERST / ERDP / RS ----
    if (op_rd32(XHCI_OP_PAGESIZE) == 0) op_wr32(XHCI_OP_PAGESIZE, 1u);    // 4KB 页（RO，只有 0 时可写）
    op_wr32(XHCI_OP_DNCTRL, 0);
    op_wr32(XHCI_OP_CONFIG, g_max_slots);                                 // MaxSlotsEn = 实测 MaxSlots
    op_wr64(XHCI_OP_DCBAAP, (uint64_t)(uintptr_t)g_p_dcbaa);
    ring_init(&g_cmd_ring, g_cmd_trbs);
    op_wr64(XHCI_OP_CRCR, (uint64_t)(uintptr_t)g_cmd_trbs | 1u);          // RCS=1
    rt_wr32(XHCI_IR_IMAN, 0);                                             // 不接中断（全程轮询）
    // ★ ERST 顺序按规范：先把 ERSTSZ 清 0（才允许改 ERSTBA）-> 写 ERSTBA -> 写 ERDP -> 再置 ERSTSZ=1
    rt_wr32(XHCI_IR_ERSTSZ, 0);
    volatile uint32_t* erst = (volatile uint32_t*)(void*)(g_p_rings + 0x800u);
    *(volatile uint64_t*)(void*)erst = (uint64_t)(uintptr_t)g_evt_trbs;
    erst[2] = XHCI_EVT_TRBS;                   // 段大小 = 段里 TRB 的**个数**（规范 6.5.2.3.1）
    // ★ 这里原来是"未解 GAP"的现场记录：QEMU 下 Command Completion / Port Status Change 都能收到，
    //   但 Transfer Event 一个都不来。**真因不在事件环**（中断器偏移/ERST 布局/ERDP/EHB 写序全都对，
    //   usb_xhci_runtime_read/write 的 trace 逐条核对过）：是 Setup Stage TRB 的 8 字节 Setup 包
    //   被写成了"内存地址"（见 xhci_control 里那段注释）—— 控制传输在设备那侧根本无法完成，
    //   所以硬件压根没产生过 Transfer Event。地址一改对，事件立刻落环（见 tests/xhci64_test.py）。
    erst[3] = 0;
    __asm__ volatile("" ::: "memory");
    rt_wr64(XHCI_IR_ERSTBA, (uint64_t)(uintptr_t)erst);
    rt_wr64(XHCI_IR_ERDP, (uint64_t)(uintptr_t)g_evt_trbs | 8u);          // EHB 写 1 清
    rt_wr32(XHCI_IR_ERSTSZ, 1);
    // ★ 再写一遍 ERSTBA/ERDP：有些实现要等 ERSTSZ 置位后才会去读 ERST（"第一次写没生效"），
    //   多写两次是幂等的、只多两条 MMIO 写，换的是"事件环一定被硬件认下来"。
    rt_wr64(XHCI_IR_ERSTBA, (uint64_t)(uintptr_t)erst);
    rt_wr64(XHCI_IR_ERDP, (uint64_t)(uintptr_t)g_evt_trbs | 8u);
    // ★ 注意：这里**不能**打开 kusb 轮询（g_evt_ready）—— 启动期枚举全是"同步等一个事件"的传输，
    //   而 kusb 线程每 12ms 就会进来把事件环里的事件消费掉（对不上它关心的 TRB 就丢掉），
    //   结果是同步等待永远匹配不到自己的完成事件（实测踩过：cc=4294967295 全超时，
    //   而 QEMU 侧 trace 显示传输其实都成功了）。轮询在 init 末尾枚举完之后才打开。
    xlog_begin();
    dbg64_str("[XHCI] cmd ring @");
    xhex((uint32_t)(uintptr_t)g_cmd_trbs, 8);
    dbg64_str(" erst @");
    xhex((uint32_t)(uintptr_t)erst, 8);
    dbg64_str(" event ring @");
    xhex((uint32_t)(uintptr_t)g_evt_trbs, 8);
    dbg64_str(" dcbaa @");
    xhex((uint32_t)(uintptr_t)g_p_dcbaa, 8);
    dbg64_str(" scratchpad=");
    dbg64_dec(g_scratch);
    xlog_end();

    // ★ 读回中断器寄存器：ERSTSZ/ERSTBA/ERDP 必须是我们写的值（写到保留区时会读到 0）
    xlog_begin();
    dbg64_str("[XHCI] ir rtsoff=0x");
    xhex(g_rtsoff, 4);
    dbg64_str(" dboff=0x");
    xhex(g_dboff, 4);
    dbg64_str(" erstsz=");
    dbg64_dec(rt_rd32(XHCI_IR_ERSTSZ));
    dbg64_str(" erstba=0x");
    dbg64_hex64(rt_rd64(XHCI_IR_ERSTBA));
    dbg64_str(" erdp=0x");
    dbg64_hex64(rt_rd64(XHCI_IR_ERDP));
    xlog_end();
    op_wr32(XHCI_OP_USBCMD, XHCI_USBCMD_RS);                              // ★ 跑起来
    {
        uint64_t spin = 0;
        while ((op_rd32(XHCI_OP_USBSTS) & XHCI_USBSTS_HCH) && spin < 200000000ull) { spin++; nop_pause(); }
    }
    // ★ 证据：RS=1 之后控制器必须真的"跑起来"（HCH=0、CNR=0、HCE=0）；
    //   这一行也是排障时最先要看的东西（命令环不工作时先看 HCH 是否还挂着）。
    xlog_begin();
    dbg64_str("[XHCI] run usbcmd=0x");
    xhex(op_rd32(XHCI_OP_USBCMD), 8);
    dbg64_str(" usbsts=0x");
    xhex(op_rd32(XHCI_OP_USBSTS), 8);
    dbg64_str(" crcr=0x");
    dbg64_hex64(op_rd64(XHCI_OP_CRCR));
    dbg64_str(" dcbaap=0x");
    dbg64_hex64(op_rd64(XHCI_OP_DCBAAP));
    xlog_end();
    if (op_rd32(XHCI_OP_USBSTS) & XHCI_USBSTS_CNR) g_selftest_mask |= 1u;  // CNR：还没准备好
    if (op_rd32(XHCI_OP_USBSTS) & XHCI_USBSTS_HCE) g_selftest_mask |= 1u;  // HCE：主控内部错

    // ---- 7) 门铃位序自探（见文件头第 5 点）+ 第一条命令（Enable Slot）----
    uint32_t first_slot = 0;
    for (uint32_t m = 0; m < 3u; m++) {
        g_db_mode = m;
        uint32_t s = 0;
        const uint64_t trb = ring_put(&g_cmd_ring, 0, 0, XHCI_TRB_TYPE(TRB_ENABLE_SLOT));
        const uint32_t cc = xhci_cmd_wait(trb, 150, &s, nullptr);
        xlog_begin();
        dbg64_str("[XHCI] cmd enable-slot try m=");
        dbg64_dec(m);
        dbg64_str(" cc=");
        dbg64_dec(cc);
        dbg64_str(" slot=");
        dbg64_dec(s);
        xlog_end();
        if (cc == XHCI_CC_SUCCESS && s) { first_slot = s; break; }
    }
    g_probe_slot = first_slot;                    // ★ 这个槽给第一台设备复用（不自探两次、不浪费槽）
    xlog_begin();
    dbg64_str("[XHCI] doorbell mode=");
    dbg64_dec(g_db_mode);
    dbg64_str(" enable slot=");
    dbg64_dec(first_slot);
    dbg64_str(first_slot ? " ok" : " FAILED");
    xlog_end();
    if (!first_slot) g_selftest_mask |= 1u;
    if (!first_slot) {                       // 排障：事件环第一个 TRB 的 control 字（硬件到底写没写过）
        xlog_begin();
        dbg64_str("[XHCI] evt0 control=0x");
        xhex((uint32_t)g_evt_trbs[0].control, 8);
        dbg64_str(" param=0x");
        dbg64_hex64(g_evt_trbs[0].param);
        xlog_end();
    }


    // ---- 8) 扫根端口：每个有设备的端口做复位 + 枚举（最多 XHCI_MAX_DEV 台）----
    g_state = XHCI_ST_READY;
    int found = 0;
    for (uint32_t n = 1; n <= g_max_ports; n++) {
        const uint32_t v = port_rd(n);
        if (!(v & XHCI_PORTSC_CCS)) {
            xlog_begin();
            dbg64_str("[XHCI] no device on port ");
            dbg64_dec(n);
            xlog_end();
            continue;
        }
        uint32_t speed = 0;
        if (!xhci_port_reset(n, &speed)) {
            xlog_begin();
            dbg64_str("[XHCI] port ");
            dbg64_dec(n);
            dbg64_str(" reset FAILED");
            xlog_end();
            continue;
        }
        xlog_begin();
        dbg64_str("[XHCI] port ");
        dbg64_dec(n);
        dbg64_str(" connected speed=");
        dbg64_str(xspeed_name(speed));
        dbg64_str(" reset ok ped=");
        dbg64_dec((port_rd(n) & XHCI_PORTSC_PED) ? 1 : 0);
        xlog_end();
        g_ports_seen++;
        if (found >= XHCI_MAX_DEV) {
            xlog_begin();
            dbg64_str("[XHCI] port ");
            dbg64_dec(n);
            dbg64_str(" device skipped (limit: 1 keyboard + 1 storage)");
            xlog_end();
            continue;
        }
        if (xhci_enum_port(n, speed) == 0) { found++; g_probe_slot = 0; }   // 自探的槽已被第一台设备用掉
    }
    g_devices = found;
    if (found == 0) {
        g_state = g_ports_seen ? XHCI_ST_ENUM_FAILED : XHCI_ST_NO_DEVICE;
        g_ready = false;
        xhci_selftest_log();
        return g_ports_seen ? -3 : -2;
    }
    // ---- 9) 存储探测（INQUIRY -> CAPACITY -> READ(10) LBA0）----
    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        XhciDev* d = &g_dev[i];
        if (d->used && d->is_msc && d->in_dci && d->out_dci) xhci_msc_probe(d);
        else if (d->used && d->is_msc) g_selftest_mask |= 8u;      // 存储接口端点不全
    }
    g_evt_ready = true;                       // ★ 枚举结束 = 之后的事件交给 kusb 轮询（运行期）
    g_ready = true;
    xhci_selftest_log();
    return 0;
}

// ==================== 轮询 ====================
void xhci64_poll64() {
    if (!g_found || !g_evt_ready || !g_evt_trbs) return;
    if (!xhci_lock_try()) return;                 // 有同步传输在跑：这次不处理，下次再来（绝不阻塞）
    uint32_t guard = 0;
    XhciEvt ev;
    while (guard++ < 64u && evt_next(&ev)) xhci_dispatch(&ev);
    xhci_lock_release();
}

int xhci64_selftest64() { return (int)g_selftest_mask; }

const char* xhci64_state_str64() {
    switch (g_state) {
    case XHCI_ST_NOT_FOUND:   return "not found";
    case XHCI_ST_NO_DEVICE:   return "no device";
    case XHCI_ST_ENUM_FAILED: return "enum failed";
    case XHCI_ST_READY:       return g_ready ? "ready" : "enum failed";
    default:                  return "init";
    }
}

int      xhci64_ports64()          { return g_found ? (int)g_max_ports : 0; }
int      xhci64_doorbell_mode64()  { return (int)g_db_mode; }
uint64_t xhci64_hid_reports64()    { uint64_t n = 0; for (int i = 0; i < XHCI_MAX_DEV; i++) n += g_dev[i].reports; return n; }
uint64_t xhci64_key_events64()     { uint64_t n = 0; for (int i = 0; i < XHCI_MAX_DEV; i++) n += g_dev[i].keys; return n; }

int      xhci64_devices64()        { return g_devices; }
// ==================== 存储对外接口（给 kernel/ata64.cpp 的驱动器号分派用）====================
static XhciDev* msc_dev(int idx) {
    int k = 0;
    for (int i = 0; i < XHCI_MAX_DEV; i++) {
        if (g_dev[i].used && g_dev[i].msc_ok) {
            if (k == idx) return &g_dev[i];
            k++;
        }
    }
    return nullptr;
}

int xhci64_msc_count64() {
    int n = 0;
    for (int i = 0; i < XHCI_MAX_DEV; i++)
        if (g_dev[i].used && g_dev[i].msc_ok) n++;
    return n;
}

bool xhci64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512) {
    XhciDev* d = msc_dev(idx);
    if (!d) return false;
    if (model && model_cap > 0) {
        int o = 0;
        for (int i = 0; i < 8 && d->vendor_s[i] && o < model_cap - 1; i++) model[o++] = (char)d->vendor_s[i];
        if (o < model_cap - 1) model[o++] = ' ';
        for (int i = 0; i < 16 && d->product_s[i] && o < model_cap - 1; i++) model[o++] = (char)d->product_s[i];
        model[o] = 0;
    }
    if (sectors_512) {
        const uint64_t bytes = (uint64_t)d->blocks * (uint64_t)d->block_size;
        *sectors_512 = bytes / 512u;
    }
    return true;
}

bool xhci64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf) {
    XhciDev* d = msc_dev(idx);
    if (!d) return false;
    if (count == 0) return true;
    xhci_lock_acquire();
    uint8_t* p = (uint8_t*)buf;
    bool ok = true;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > XHCI_MSC_SECTORS) n = XHCI_MSC_SECTORS;
        if (xhci_msc_read10(d, lba + done, (uint16_t)n, p) != 0) { ok = false; break; }
        done += n;
        p += n * 512u;
    }
    xhci_lock_release();
    return ok;
}
