// usb64.cpp - USB 主机：UHCI（Intel USB 1.1 主控）+ HID 引导键盘
//
// ============================ 为什么这么做 ============================
// 1) 主控选择：只在 PCI 上找 **UHCI**（vendor=8086，device=7020/7112/2412/24C2/2658）。
//    EHCI/xHCI 没做 —— 没插 UHCI 卡/机器上只有 EHCI 时打一行 "[USB64] not found"，
//    此后本模块彻底沉默（系统照常启动、桌面照常工作）。
// 2) 寄存器访问：UHCI 的 **BAR4 通常是 I/O BAR**（bit0=1）-> 用 inb/inw/inl + outb/outw/outl
//    访问（kernel/port.h 第 21-28 行已经有 outl/inl 这组 32 位端口读写内联汇编，
//    所以本文件**不需要**自己再加内联汇编，直接用即可）。若 BAR4 是 MMIO BAR（bit0=0，
//    某些非 Intel 芯片组），就把它当**恒等映射**的 32 位寄存器数组用（与 e1000_64.cpp
//    对 BAR0 的处理同一套假设：前 4GB 物理空间恒等映射）。I/O 布局是 16 位寄存器，
//    MMIO 布局是"同样的偏移落在 32 位字的低/高半字"（PORTSC2 在 0x12 即 0x10 的高半字），
//    uhci_rd16/wr16 把这个差异吃掉。
// 3) 传输层取舍 —— **一个队列头（QH）+ 每次传输一条纯 TD 链**（不用多 QH 调度）：
//    * 队列里的 TD 链本身是 UHCI 的标准做法（TD=32 字节：link/status/token/buffer）；
//    * 需要一个 QH 的唯一原因是：**发布新传输必须是单次原子写**。QH 的 element 指针是一个
//      16 字节对齐的 32 位字，硬件读它时要么看到旧值（上一次的链/空链），要么看到新值；
//      纯 TD 链则要靠"帧列表项"或"链头 TD 的 ACTIVE 位"当发布点，而在本内核里 250Hz 的
//      PIT 抢占会在任意两条指令之间把 kusb 线程切走 —— 半成品链会因此被硬件看到。
//      代价只多 16 字节（一个 QH），换来的是"发布即原子"。
//    * 队列**永远**以"不活跃的空 TD（link=TERM）"收尾（Linux uhci 的同款做法）：
//      硬件走完最后一个真 TD 后只会看到"不活跃 + 终止"，不会顺着失效指针乱跑。
//    * 帧列表 1024 项**全部**指向这一个 QH（= 每个 1ms 帧都处理一遍），所以中断端点
//      实际上是每帧（1ms）被轮询一次 —— 对键盘这个频率绰绰有余。
//      取舍说明：中断 TD 被设备 NAK 时硬件会把整个队列停在这一帧（UHCI 的队列不越过
//      错误完成状态），所以**枚举/控制传输期间不挂中断 TD**，控制传输跑完再重新武装。
//      本驱动所有控制传输都只发生在启动期（枚举 + SET_CONFIGURATION + SET_PROTOCOL/IDLE），
//      运行期只有中断 TD 在队列里 —— 不存在"键盘把控制传输饿死"的情况。
// 4) 轮询而不是中断：不接 IRQ11/IRQ10、不使能 USBINTR，由 kusb 内核线程调 usb64_poll64()
//    （PIT 250Hz 计时，约 12ms 一次；其余时间 task_yield64()，不占桌面 CPU）。
// 5) 键盘事件接入点：HID 用法码 -> **PS/2 集 1 扫描码** -> input.cpp 的
//    kbd_inject_scancode()。也就是说 USB 键盘和 PS/2 键盘汇入**同一个环形队列**，
//    Shift/Ctrl/Caps/方向键/WIN 键标志/Ctrl+Shift+Esc 热键全部复用现成逻辑，
//    桌面外壳 gui64.cpp 一行都不用改（这是"真的送到桌面外壳"的最短路径）。
//
// 没验证到的点（如实记录，见文件末）：
//   * 低速（low-speed）设备：代码里按 PORTSC.LSDA 支持（TD 状态 LS 位 + 端口不使能时
//     仍可枚举），但 QEMU 的 usb-kbd 是全速设备，**低速路径没有实机/仿真验证**。
//   * 只能识别**直接插在根端口**上的设备（没有 hub/地址分配多设备；只用一个地址 1）。
//   * 只做引导键盘（没做报告描述符解析、没做 typematic 自动重复、没做 USB 鼠标/存储）。
// ======================================================================
#include "usb64.h"
#include "port.h"        // inb/inw/inl + outb/outw/outl（32 位端口读写这里已有，不用另加内联汇编）
#include "debug64.h"     // 串口打点（行锁 begin/end）
#include "input.h"       // kbd_inject_scancode()：注入到 PS/2 同一条按键队列
#include "mem_64.h"      // page_alloc_64 / memset_64（恒等映射：物理地址即指针）
#include "x86_64.h"      // g_ticks64 / ms_to_ticks64 / nop_pause()
#include <stdint.h>
#include <stddef.h>

// ==================== UHCI 寄存器（偏移，与 Intel UHCI 规范和 Linux uhci-hcd.h 对齐）====
#define UHCI_USBCMD      0x00u     // 16 位
#define UHCI_USBSTS      0x02u     // 16 位
#define UHCI_USBINTR     0x04u     // 16 位
#define UHCI_FRNUM       0x06u     // 16 位
#define UHCI_FLBASEADD   0x08u     // 32 位（帧列表基址，必须 4KB 对齐）
#define UHCI_SOFMOD      0x0Cu     // 8 位
#define UHCI_PORTSC1     0x10u     // 16 位
#define UHCI_PORTSC2     0x12u     // 16 位

#define USBCMD_RS        0x0001u    // Run/Stop
#define USBCMD_HCRESET   0x0002u    // 软复位（自清）
#define USBCMD_CF        0x0040u    // Configure Flag
#define USBCMD_MAXP      0x0080u    // 最大包 64 字节

#define PORTSC_CCS       0x0001u    // 设备已连接
#define PORTSC_CSC       0x0002u    // 连接状态变化（写 1 清）
#define PORTSC_PE        0x0004u    // 端口使能
#define PORTSC_PEC       0x0008u    // 使能变化（写 1 清）
#define PORTSC_RD        0x0040u    // 恢复检测
#define PORTSC_LSDA      0x0100u    // 低速设备
#define PORTSC_PR        0x0200u    // 端口复位
#define PORTSC_SUSP      0x1000u    // 挂起
#define PORTSC_WZ        0xE000u    // 位 15:13 必须写 0

#define PID_SETUP        0x2Du
#define PID_IN           0x69u
#define PID_OUT          0xE1u

// TD status（Linux uhci-hcd.h 的定义顺序，注意 ACTIVE 是 bit23、ACTLEN 在 bit10:0）
#define TD_ACTIVE        (1u << 23)
#define TD_STALLED       (1u << 22)
#define TD_DBUFERR       (1u << 21)
#define TD_BABBLE        (1u << 20)
#define TD_NAK           (1u << 19)
#define TD_CRCTIMEO      (1u << 18)
#define TD_BITSTUFF      (1u << 17)
#define TD_IOC           (1u << 24)
#define TD_LS            (1u << 26)
#define TD_SPD           (1u << 29)
#define TD_ACTLEN_MASK   0x7FFu
#define TD_MAXERR3       (3u << 27)                       // C_ERR = 3 次重试
#define TD_ERR_MASK      (TD_STALLED | TD_DBUFERR | TD_BABBLE | TD_CRCTIMEO | TD_BITSTUFF)

#define UHCI_PTR_TERM    0x00000001u
#define UHCI_PTR_QH      0x00000002u

// TD token：bit7:0 = PID、bit14:8 = 设备地址、bit18:15 = 端点、bit19 = DATA toggle、
//           bit31:21 = 期望长度（**编码成 n-1**，0 字节 -> 0x7FF，与 Linux uhci_explen 一致）
#define UHCI_TOKEN(pid, addr, ep, toggle, len) \
    ((uint32_t)(pid) | ((uint32_t)(addr) << 8) | ((uint32_t)(ep) << 15) | \
     ((toggle) ? (1u << 19) : 0u) | ((((uint32_t)(len) - 1u) & 0x7FFu) << 21))
// 实际长度同样是 n-1 编码（Linux uhci_actual_length）：(status + 1) & 0x7FF
#define UHCI_ACTLEN(st)  (((st) + 1u) & TD_ACTLEN_MASK)
#define UHCI_EXPLEN(tk)  ((((tk) >> 21) + 1u) & TD_ACTLEN_MASK)

// ==================== 硬件数据结构（必须 16 字节对齐；恒等映射 -> 虚拟地址就是物理地址）====
struct UhciTd {                  // 32 字节：link / status / token / buffer
    volatile uint32_t link;
    volatile uint32_t status;
    volatile uint32_t token;
    volatile uint32_t buffer;
} __attribute__((aligned(16)));

struct UhciQh {                  // 16 字节：link / element（低两位是 T/QH 标志）
    volatile uint32_t link;
    volatile uint32_t element;
    volatile uint32_t rsvd0;
    volatile uint32_t rsvd1;
} __attribute__((aligned(16)));

#define USB64_FL_ENTRIES  1024u
#define USB64_FL_BYTES    (USB64_FL_ENTRIES * 4u)      // 4096 字节 = 正好一页
#define USB64_TD_SLOTS    64                           // TD 池槽数（16 + 64*32 = 2064 字节，一页够）
#define USB64_TD_IDLE     0                            // 槽 0 = 永久"空 TD"（绝不复用）
#define USB64_CTL_TIMEOUT_MS 250u                      // 单次控制传输的等待上限
#define USB64_CTL_MAX_TD  32                           // 一次控制传输最多几个数据 TD（256B/8 = 32）
#define USB64_CTL_BUF_BYTES 256u                       // 控制数据缓冲（描述符）大小
// ==================== 全局状态 ====================
enum Usb64State : uint32_t {
    USB64_ST_INIT = 0,
    USB64_ST_NOT_FOUND,
    USB64_ST_NO_DEVICE,
    USB64_ST_ENUM_FAILED,
    USB64_ST_READY
};

static bool      g_inited      = false;
static bool      g_found       = false;      // 找到 UHCI 主控
static bool      g_ready       = false;      // HID 引导键盘就绪（中断 TD 已武装）
static uint32_t  g_state       = USB64_ST_INIT;

static uint8_t   g_bus = 0, g_dev = 0, g_fn = 0;
static uint16_t  g_io_base     = 0;          // I/O BAR4 基址
static volatile uint32_t* g_mmio = nullptr;  // MMIO BAR4（非 0 时用 MMIO 而不是 I/O）
static int       g_ports       = 0;          // 根端口数

// DMA 结构（page_alloc_64 给的页，恒等映射：物理地址即指针）
static uint8_t*  g_fl_page     = nullptr;    // 1024 项帧列表
static uint8_t*  g_td_page     = nullptr;    // QH + TD 池
static uint8_t*  g_data_page   = nullptr;    // 数据缓冲（SETUP 包/描述符/报告）
static UhciQh*   g_qh          = nullptr;
static volatile UhciTd* g_idle_td  = nullptr;    // 永久不活跃的"空 TD"（停链用）
static volatile UhciTd* g_irq_td   = nullptr;    // 中断 IN（HID 报告）
static volatile UhciTd* g_irq_tail = nullptr;    // 中断链的收尾空 TD
static int       g_td_next     = 1;              // TD 池轮转游标（槽 0 永不使用）

static uint8_t*  g_setup_buf   = nullptr;    // 8 字节 SETUP 包
static uint8_t*  g_ctl_buf     = nullptr;    // 控制传输的数据缓冲（256 字节）
static uint8_t*  g_null_buf    = nullptr;    // 状态阶段（0 字节）用的哨兵缓冲
static uint8_t*  g_report_buf  = nullptr;    // 8 字节 HID 报告

// 设备状态
static uint8_t   g_addr        = 0;          // 分配到的 USB 地址（1）
static bool      g_low_speed   = false;
static uint8_t   g_ctl_mps     = 8;          // EP0 最大包（先按 8 收，再读设备描述符修正）
static uint8_t   g_ep_in       = 0;          // HID 中断 IN 端点号
static uint16_t  g_ep_mps      = 0;          // 端点最大包（引导键盘 = 8）
static uint16_t  g_vendor      = 0;
static uint16_t  g_product     = 0;
static uint8_t   g_toggle      = 0;          // 中断 IN 的 DATA toggle

// 统计
static uint64_t  g_hid_reports = 0;
static uint64_t  g_key_events  = 0;
static int       g_devices     = 0;

// HID 报告的边沿检测状态
static uint8_t   g_last_mods   = 0;
static uint8_t   g_last_keys[6] = {0, 0, 0, 0, 0, 0};

// ==================== 小工具 ====================
static void usb_log_begin() { dbg64_line_begin64(); }
static void usb_log_end()   { dbg64_nl(); dbg64_line_end64(); }

// 固定宽度大写十六进制（与 dbg64_hex64 的风格一致）
static void usb_hex(uint32_t v, int digits) {
    static const char* H = "0123456789ABCDEF";
    char buf[9];
    if (digits > 8) digits = 8;
    for (int i = digits - 1; i >= 0; i--) { buf[i] = H[v & 0xFu]; v >>= 4; }
    for (int i = 0; i < digits; i++) dbg64_putc(buf[i]);
}

static inline void usb_barrier() { __asm__ volatile("" ::: "memory"); }

// 有界忙等：用 PIT 计时（中断开着，别的任务/桌面照常被调度），再加硬自旋上界兜底。
static void usb_delay_ms(uint32_t ms) {
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(ms ? ms : 1);
    uint64_t spin = 0;
    while ((g_ticks64 - t0) < want && spin < 400000000ull) { spin++; nop_pause(); }
}

// ==================== 寄存器访问（I/O 与 MMIO 两种 BAR4 都支持）====================
static uint16_t uhci_rd16(uint16_t off) {
    if (g_mmio) {
        const uint32_t w = g_mmio[off >> 2];
        return (uint16_t)((off & 2u) ? (w >> 16) : (w & 0xFFFFu));
    }
    return inw((uint16_t)(g_io_base + off));
}
static void uhci_wr16(uint16_t off, uint16_t v) {
    if (g_mmio) {
        // MMIO：16 位寄存器落在 32 位字的低/高半字 -> 读-改-写，别踩到隔壁端口寄存器
        uint32_t w = g_mmio[off >> 2];
        if (off & 2u) w = (w & 0x0000FFFFu) | ((uint32_t)v << 16);
        else          w = (w & 0xFFFF0000u) | (uint32_t)v;
        g_mmio[off >> 2] = w;
        usb_barrier();
        return;
    }
    outw((uint16_t)(g_io_base + off), v);
}
static uint32_t uhci_rd32(uint16_t off) {
    if (g_mmio) return g_mmio[off >> 2];
    return inl((uint16_t)(g_io_base + off));
}
static void uhci_wr32(uint16_t off, uint32_t v) {
    if (g_mmio) { g_mmio[off >> 2] = v; usb_barrier(); return; }
    outl((uint16_t)(g_io_base + off), v);
}

// ==================== PCI（0xCF8/0xCFC；与 hwinfo64/e1000_64 同一套做法）====================
static uint32_t upci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}
static void upci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    const uint32_t addr = 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                          ((uint32_t)fn << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, val);
}

// UHCI 的设备 ID：PIIX3(7020)/PIIX4(7112)/ICH(2412)/ICH0(24C2)/ICH4(2658)
static bool uhci_id_supported(uint16_t id) {
    return id == 0x7020u || id == 0x7112u || id == 0x2412u || id == 0x24C2u || id == 0x2658u;
}

static bool uhci_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = upci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
            bus_has = true;
            const uint32_t hdr = upci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = upci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint16_t device = (uint16_t)(id >> 16);
                if (vendor == 0x8086u && uhci_id_supported(device)) {
                    g_bus = (uint8_t)bus; g_dev = (uint8_t)dev; g_fn = (uint8_t)fn;
                    return true;
                }
            }
        }
        if (bus_has) empty_run = 0;
        else if (++empty_run >= 4u) break;
    }
    return false;
}

// ==================== TD 池 ====================
static volatile UhciTd* td_slot(int i) {
    // 页首 16 字节放 QH -> TD 槽从 +16 开始，每槽 32 字节（都是 16 字节对齐）
    return (volatile UhciTd*)(void*)(g_td_page + 16 + (size_t)i * 32u);
}
static int td_next_slot(int cur) {
    cur++;
    if (cur >= USB64_TD_SLOTS) cur = 1;          // 槽 0 是永久空 TD，绝不复用
    return cur;
}
static inline void td_fill(volatile UhciTd* td, uint32_t status, uint32_t token, uint32_t buf) {
    td->status = status;
    td->token  = token;
    td->buffer = buf;
}
static inline uint32_t td_phys(const volatile UhciTd* td) { return (uint32_t)(uintptr_t)td; }
static inline uint32_t qh_phys(const volatile UhciQh* qh) { return (uint32_t)(uintptr_t)qh; }

// 把 QH 的 element 指回"永久空 TD"（硬件下一帧只会看到：不活跃 -> 终止）
static void uhci_stop_queue() {
    g_qh->element = td_phys(g_idle_td);
    usb_barrier();
}

// 超时/出错后把整条链标成不活跃（免得硬件还在半路上反复重试半成品）
static void uhci_abort_chain(volatile UhciTd* head) {
    uhci_stop_queue();
    volatile UhciTd* t = head;
    for (int i = 0; i < 16 && t; i++) {
        t->status &= ~TD_ACTIVE;
        const uint32_t l = t->link;
        if (l & UHCI_PTR_TERM) break;
        if (l == 0) break;
        t = (volatile UhciTd*)(uintptr_t)(l & ~0xFu);
    }
    usb_barrier();
}

// 等待一个 TD 完成（ACTIVE 被硬件清掉）。返回 false = 超时。
static bool usb_wait_td(const volatile UhciTd* td, uint32_t timeout_ms) {
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(timeout_ms ? timeout_ms : 1);
    uint64_t spin = 0;
    for (;;) {
        if (!(td->status & TD_ACTIVE)) return true;
        if ((g_ticks64 - t0) >= want) return false;
        if (++spin > 400000000ull) return false;
        nop_pause();
    }
}

// ==================== 控制传输（SETUP / DATA / STATUS 三阶段）====================
// rt/req/val/idx/len = 标准 USB 控制请求字段；data = 数据缓冲（IN 收 / OUT 发，可为 nullptr）
// 返回 0 = 成功；-1 = 超时或硬件错；-2 = 设备 STALL。
static int usb_control64(uint8_t addr, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx,
                         uint16_t len, uint8_t* data, uint16_t* out_len) {
    if (out_len) *out_len = 0;

    // ---- SETUP 包（8 字节，PID=SETUP，toggle 恒为 DATA0）----
    uint8_t* su = g_setup_buf;
    su[0] = rt; su[1] = req;
    su[2] = (uint8_t)(val & 0xFFu); su[3] = (uint8_t)(val >> 8);
    su[4] = (uint8_t)(idx & 0xFFu); su[5] = (uint8_t)(idx >> 8);
    su[6] = (uint8_t)(len & 0xFFu); su[7] = (uint8_t)(len >> 8);

    const bool in_dir = (rt & 0x80u) != 0;
    const uint32_t lsflag = g_low_speed ? (uint32_t)TD_LS : 0u;

    // 先把硬件从队列上摘下来，再重填槽位（发布点在函数尾部那一次 32 位写）
    uhci_stop_queue();

    int slot = g_td_next;
    volatile UhciTd* setup_td = td_slot(slot); slot = td_next_slot(slot);

    td_fill(setup_td, TD_MAXERR3 | lsflag | TD_ACTIVE,
            UHCI_TOKEN(PID_SETUP, addr, 0, 0, 8), (uint32_t)(uintptr_t)su);
    setup_td->link = UHCI_PTR_TERM;                 // 下面按需要接到数据/状态 TD

    volatile UhciTd* prev = setup_td;
    volatile UhciTd* first_data = nullptr;
    volatile UhciTd* last_data  = nullptr;

    // ---- DATA 阶段：按 EP0 最大包切片；数据阶段第一个包用 DATA1（USB 规定）----
    if (len > 0 && data == nullptr) len = 0;        // 防御：说了有数据却没给缓冲
    uint32_t toggle = 1;
    uint16_t remain = len;
    uint8_t* dp = data;
    int data_tds = 0;
    while (remain > 0 && data_tds < USB64_CTL_MAX_TD) {
        data_tds++;
        uint16_t pktsze = (uint16_t)(g_ctl_mps ? g_ctl_mps : 8);
        uint32_t st = TD_MAXERR3 | lsflag | TD_ACTIVE;
        if (in_dir) st |= TD_SPD;                   // IN 方向：短包检测（最后一包除外）
        if (remain <= pktsze) { pktsze = remain; st &= ~(uint32_t)TD_SPD; }

        volatile UhciTd* t = td_slot(slot); slot = td_next_slot(slot);
        td_fill(t, st, UHCI_TOKEN(in_dir ? PID_IN : PID_OUT, addr, 0, toggle, pktsze),
                (uint32_t)(uintptr_t)dp);
        t->link = UHCI_PTR_TERM;
        prev->link = td_phys(t);
        prev = t;
        if (!first_data) first_data = t;
        last_data = t;
        toggle ^= 1;
        dp += pktsze;
        remain -= pktsze;
    }
    if (remain != 0) { uhci_abort_chain(setup_td); return -1; }   // 切片太多（理论到不了）

    // ---- STATUS 阶段：方向与数据阶段相反（无数据阶段 -> IN）；零长度；toggle 恒 DATA1 ----
    const uint32_t st_pid = (len > 0 && in_dir) ? (uint32_t)PID_OUT : (uint32_t)PID_IN;
    volatile UhciTd* status_td = td_slot(slot); slot = td_next_slot(slot);
    td_fill(status_td, TD_MAXERR3 | lsflag | TD_ACTIVE,
            UHCI_TOKEN(st_pid, addr, 0, 1, 0), (uint32_t)(uintptr_t)g_null_buf);
    prev->link = td_phys(status_td);

    // ---- 链尾：不活跃的空 TD + 终止位（硬件走到这里只会停下）----
    volatile UhciTd* tail = td_slot(slot); slot = td_next_slot(slot);
    td_fill(tail, 0, UHCI_TOKEN(PID_OUT, addr, 0, 0, 0), 0);
    tail->link = UHCI_PTR_TERM;
    status_td->link = td_phys(tail);

    g_td_next = slot;

    // ★ 发布：一次 32 位对齐写（硬件要么看到空链，要么看到这条链）
    usb_barrier();
    g_qh->element = td_phys(setup_td);
    usb_barrier();

    // ---- 等状态阶段完成 ----
    if (!usb_wait_td(status_td, USB64_CTL_TIMEOUT_MS)) {
        uhci_abort_chain(setup_td);
        return -1;
    }

    const uint32_t ss = status_td->status;
    if (ss & TD_STALLED) { uhci_abort_chain(setup_td); return -2; }
    if (ss & TD_ERR_MASK) { uhci_abort_chain(setup_td); return -3; }

    // ---- 汇总数据阶段实际收到的字节数（按"短包即结束"的规则）----
    uint16_t total = 0;
    if (first_data) {
        volatile UhciTd* t = first_data;
        for (;;) {
            const uint32_t ts = t->status;
            if (ts & TD_ERR_MASK) { uhci_abort_chain(setup_td); return -3; }
            const uint16_t act = (uint16_t)UHCI_ACTLEN(ts);
            total = (uint16_t)(total + act);
            const uint16_t want = (uint16_t)UHCI_EXPLEN(t->token);
            if (act < want) break;                  // 短包：数据阶段到此为止
            if (t == last_data) break;
            const uint32_t l = t->link;
            if (l & UHCI_PTR_TERM) break;
            t = (volatile UhciTd*)(uintptr_t)(l & ~0xFu);
        }
    }
    if (out_len) *out_len = total;

    uhci_stop_queue();
    return 0;
}

// ==================== 枚举失败打点（多打一行 stage，方便定位卡在哪一步）====================
static int usb_enum_fail(const char* stage, int rc) {
    g_state = USB64_ST_ENUM_FAILED;
    usb_log_begin();
    dbg64_str("[USB64] enum FAILED stage=");
    dbg64_str(stage);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(uint32_t)(-rc));
    usb_log_end();
    return -1;
}

// ==================== HID 用法码 -> PS/2 集 1 扫描码 ====================
// 返回值 0 = 不支持；0xE0xx 表示需要先发 0xE0 前缀的扩展码（bit15:8 = 0xE0）。
static uint16_t hid_usage_to_scan(uint8_t u) {
    switch (u) {
    // 字母
    case 0x04: return 0x1E; case 0x05: return 0x30; case 0x06: return 0x2E;
    case 0x07: return 0x20; case 0x08: return 0x12; case 0x09: return 0x21;
    case 0x0A: return 0x22; case 0x0B: return 0x23; case 0x0C: return 0x17;
    case 0x0D: return 0x24; case 0x0E: return 0x25; case 0x0F: return 0x26;
    case 0x10: return 0x32; case 0x11: return 0x31; case 0x12: return 0x18;
    case 0x13: return 0x19; case 0x14: return 0x10; case 0x15: return 0x13;
    case 0x16: return 0x1F; case 0x17: return 0x14; case 0x18: return 0x16;
    case 0x19: return 0x2F; case 0x1A: return 0x11; case 0x1B: return 0x2D;
    case 0x1C: return 0x15; case 0x1D: return 0x2C;
    // 数字行（Shift 由 input.cpp 的修饰键状态决定，这里只给物理键位）
    case 0x1E: return 0x02; case 0x1F: return 0x03; case 0x20: return 0x04;
    case 0x21: return 0x05; case 0x22: return 0x06; case 0x23: return 0x07;
    case 0x24: return 0x08; case 0x25: return 0x09; case 0x26: return 0x0A;
    case 0x27: return 0x0B;
    // 编辑/空白键
    case 0x28: return 0x1C;      // Enter
    case 0x29: return 0x01;      // Esc（input.cpp 显式投递 0x1B）
    case 0x2A: return 0x0E;      // Backspace
    case 0x2B: return 0x0F;      // Tab
    case 0x2C: return 0x39;      // Space
    case 0x2D: return 0x0C; case 0x2E: return 0x0D; case 0x2F: return 0x1A;
    case 0x30: return 0x1B; case 0x31: return 0x2B; case 0x33: return 0x27;
    case 0x34: return 0x28; case 0x35: return 0x29; case 0x36: return 0x33;
    case 0x37: return 0x34; case 0x38: return 0x35;
    case 0x39: return 0x3A;      // Caps Lock
    // 功能键 F1..F12
    case 0x3A: return 0x3B; case 0x3B: return 0x3C; case 0x3C: return 0x3D;
    case 0x3D: return 0x3E; case 0x3E: return 0x3F; case 0x3F: return 0x40;
    case 0x40: return 0x41; case 0x41: return 0x42; case 0x42: return 0x43;
    case 0x43: return 0x44; case 0x44: return 0x57; case 0x45: return 0x58;
    // 导航键：input.cpp 认 E0 前缀（方向键会被投成 NAV_* 供窗口/菜单用）
    case 0x49: return 0xE052;    // Insert
    case 0x4A: return 0xE047;    // Home
    case 0x4B: return 0xE049;    // Page Up
    case 0x4C: return 0xE053;    // Delete
    case 0x4D: return 0xE04F;    // End
    case 0x4E: return 0xE051;    // Page Down
    case 0x4F: return 0xE04D;    // 右
    case 0x50: return 0xE04B;    // 左
    case 0x51: return 0xE050;    // 下
    case 0x52: return 0xE048;    // 上
    // 修饰键（HID 的 0xE0..0xE7 与 byte0 的 8 个位一一对应）
    case 0xE0: return 0x1D;      // 左 Ctrl
    case 0xE1: return 0x2A;      // 左 Shift
    case 0xE2: return 0x38;      // 左 Alt
    case 0xE3: return 0xE05B;    // 左 GUI（Win 键 -> input.cpp 的 win_key_flag）
    case 0xE4: return 0xE01D;    // 右 Ctrl
    case 0xE5: return 0x36;      // 右 Shift
    case 0xE6: return 0xE038;    // 右 Alt
    case 0xE7: return 0xE05C;    // 右 GUI
    default:   return 0;
    }
}

// 把扫描码（含可选 E0 前缀）注入 PS/2 同一条按键队列。
// cli/sti 包住"前缀 + 码"这两个字节：kbd_push 允许 IRQ1 重入的话，head 会被写乱
// （IRQ1 也往同一个环形缓冲写），所以注入期间必须互斥。
static void usb_inject_key(uint16_t code, bool down) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    if (code & 0xFF00u) kbd_inject_scancode(0xE0);
    uint8_t sc = (uint8_t)(code & 0xFFu);
    if (!down) sc = (uint8_t)(sc | 0x80u);
    kbd_inject_scancode(sc);
    if (fl & 0x200ull) __asm__ volatile("sti" ::: "memory");
}

// 一个"新按下 / 释放"边沿：打点 + 注入 + 计数。
static void usb_key_edge(uint8_t usage, bool down) {
    usb_log_begin();
    dbg64_str("[USB64] hid report key=");
    usb_hex(usage, 2);
    dbg64_str(down ? " down=1" : " down=0");
    usb_log_end();

    if (down) g_key_events++;
    const uint16_t code = hid_usage_to_scan(usage);
    if (code) usb_inject_key(code, down);
}

// ==================== HID 引导键盘报告（8 字节）解析 ====================
// byte0 = 修饰键位图；byte1 = 保留；byte2..7 = 最多 6 个同时按下的用法码。
// 只对"上一次报告 vs 这一次报告"的**边沿**产生事件（新按下 -> down=1，消失 -> down=0），
// 所以按住不放不会被重复触发，也不会每个报告都刷屏。
static bool key_in_list(const uint8_t* list, uint8_t k) {
    for (int i = 0; i < 6; i++) if (list[i] == k) return true;
    return false;
}
static void usb_handle_report(const uint8_t* r) {
    g_hid_reports++;
    static const uint8_t MOD_USAGE[8] = { 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7 };
    const uint8_t mods = r[0];

    // 1) 修饰键的按下边沿（先按修饰键，再按普通键 -> Shift/Ctrl 组合才对）
    for (int i = 0; i < 8; i++) {
        const bool was = (g_last_mods >> i) & 1u;
        const bool now = (mods >> i) & 1u;
        if (now && !was) usb_key_edge(MOD_USAGE[i], true);
    }
    // 2) 普通键的按下边沿（0 = 空槽，1 = ErrorRollOver，都跳过）
    for (int i = 0; i < 6; i++) {
        const uint8_t k = r[2 + i];
        if (k == 0 || k == 1) continue;
        if (!key_in_list(g_last_keys, k)) usb_key_edge(k, true);
    }
    // 3) 普通键的释放边沿
    for (int i = 0; i < 6; i++) {
        const uint8_t k = g_last_keys[i];
        if (k == 0 || k == 1) continue;
        if (!key_in_list(&r[2], k)) usb_key_edge(k, false);
    }
    // 4) 修饰键的释放边沿
    for (int i = 0; i < 8; i++) {
        const bool was = (g_last_mods >> i) & 1u;
        const bool now = (mods >> i) & 1u;
        if (was && !now) usb_key_edge(MOD_USAGE[i], false);
    }

    for (int i = 0; i < 6; i++) g_last_keys[i] = r[2 + i];
    g_last_mods = mods;
}

// ==================== 中断 IN 端点（HID 报告）====================
// 武装：QH.element 指向中断 TD（ACTIVE）—— 硬件每帧重试，直到设备给出报告（NAK 时保持 ACTIVE）。
static void usb_arm_interrupt() {
    const uint32_t lsflag = g_low_speed ? (uint32_t)TD_LS : 0u;
    uhci_stop_queue();                                   // 先摘链，再改槽位
    td_fill(g_irq_td, TD_MAXERR3 | lsflag | TD_ACTIVE | TD_SPD,
            UHCI_TOKEN(PID_IN, g_addr, g_ep_in, g_toggle, 8), (uint32_t)(uintptr_t)g_report_buf);
    g_irq_td->link = td_phys(g_irq_tail);
    td_fill(g_irq_tail, 0, UHCI_TOKEN(PID_OUT, g_addr, g_ep_in, 0, 0), 0);
    g_irq_tail->link = UHCI_PTR_TERM;
    usb_barrier();
    g_qh->element = td_phys(g_irq_td);                   // ★ 发布
    usb_barrier();
}

void usb64_poll64() {
    if (!g_ready || !g_irq_td) return;
    const uint32_t st = g_irq_td->status;
    if (st & TD_ACTIVE) return;                          // 还没完成（没有报告时硬件一直 NAK）

    const uint16_t act = (uint16_t)UHCI_ACTLEN(st);
    if (st & TD_ERR_MASK) {
        // 硬件/设备侧错误（CRC/Stall…）：报告作废，重新武装（toggle 保守保持不变）
    } else if (act == 8) {
        usb_handle_report(g_report_buf);
        g_toggle ^= 1;                                   // 成功的 IN 事务 -> 下一个包换 toggle
    } else {
        // 短包（正常引导键盘不会出现）：清零剩余字节后仍按 8 字节报告解析，避免状态错乱
        for (int i = (int)act; i < 8; i++) g_report_buf[i] = 0;
        usb_handle_report(g_report_buf);
        g_toggle ^= 1;
    }
    usb_arm_interrupt();
}

// ==================== 端口复位 ====================
// 复位序列（UHCI 规范 + Linux uhci-hub 同款）：置 PR -> 等 ~50ms -> 清 PR -> 等 10ms
// -> 清 CSC/PEC（写 1）-> 置 PE。读回 LSDA 判断低速/全速。
static bool usb_port_reset(int idx, bool* low_speed) {
    const uint16_t reg = (idx == 1) ? (uint16_t)UHCI_PORTSC1 : (uint16_t)UHCI_PORTSC2;
    uint16_t v = uhci_rd16(reg);
    if (!(v & PORTSC_CCS)) return false;

    // 先清掉可能挂着的状态变化位与挂起
    uhci_wr16(reg, (uint16_t)((v & ~(uint32_t)PORTSC_WZ) & ~(uint32_t)(PORTSC_SUSP | PORTSC_RD)));
    usb_delay_ms(2);

    v = uhci_rd16(reg);
    uhci_wr16(reg, (uint16_t)(((v | PORTSC_PR) & ~(uint32_t)PORTSC_WZ)));
    usb_delay_ms(50);                                    // 规范要求 PR 至少保持 10ms，留足余量
    v = uhci_rd16(reg);
    uhci_wr16(reg, (uint16_t)(((v & ~(uint32_t)PORTSC_PR) & ~(uint32_t)PORTSC_WZ)));
    usb_delay_ms(10);

    v = uhci_rd16(reg);
    // 清 CSC/PEC（写 1 清除），再显式使能端口；PR 已经由硬件/软件清掉
    uint16_t nv = (uint16_t)(v | PORTSC_CSC | PORTSC_PEC);
    nv = (uint16_t)(nv & ~(uint32_t)(PORTSC_PR | PORTSC_WZ));
    uhci_wr16(reg, nv);
    usb_delay_ms(2);
    v = uhci_rd16(reg);
    uhci_wr16(reg, (uint16_t)(((v | PORTSC_PE) & ~(uint32_t)PORTSC_WZ)));
    usb_delay_ms(2);

    v = uhci_rd16(reg);
    *low_speed = (v & PORTSC_LSDA) != 0;
    return (v & PORTSC_CCS) != 0;
}

// ==================== 枚举 ====================
// GET_DESCRIPTOR(Device,8) -> SET_ADDRESS(1) -> GET_DESCRIPTOR(Device,18)
// -> GET_DESCRIPTOR(Config,9) -> GET_DESCRIPTOR(Config,total) -> 解析 HID 接口/端点
// -> SET_CONFIGURATION(1) -> SET_PROTOCOL(0 引导) + SET_IDLE(0)
static int usb_enumerate() {
    uint8_t* buf = g_ctl_buf;                       //  DMA 缓冲：必须在页池里（恒等映射物理地址）
    const uint16_t buf_bytes = USB64_CTL_BUF_BYTES;
    uint16_t n = 0;

    // ---- 1) 8 字节设备描述符（默认地址 0；此时还不知道设备 mps，只能按 8 字节收）----
    g_ctl_mps = 8;
    if (usb_control64(0, 0x80, 6, 0x0100, 0, 8, buf, &n) != 0 || n < 8)
        return usb_enum_fail("get-device-8", 1);
    uint8_t mps = buf[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;
    g_ctl_mps = mps;

    // ---- 2) SET_ADDRESS(1)（0 字节数据阶段）----
    if (usb_control64(0, 0x00, 5, 1, 0, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-address", 2);
    g_addr = 1;
    usb_delay_ms(10);                                    // 设备切换地址的恢复时间

    // ---- 3) 完整设备描述符（18 字节，现在在地址 1）----
    if (usb_control64(g_addr, 0x80, 6, 0x0100, 0, 18, buf, &n) != 0 || n < 18)
        return usb_enum_fail("get-device-18", 3);
    g_vendor  = (uint16_t)(buf[8] | ((uint16_t)buf[9] << 8));
    g_product = (uint16_t)(buf[10] | ((uint16_t)buf[11] << 8));

    // ★ 这一行的 addr=0 指的是"枚举时发现它的默认地址"（8 字节描述符就是在地址 0 读的，
    //   mps 也来自那一次读）；vendor/product 只存在于 18 字节描述符里，而按 USB 规定
    //   它必须在 SET_ADDRESS 之后才能安全读全 —— 所以三个字段来自同一次枚举的不同步。
    usb_log_begin();
    dbg64_str("[USB64] device addr=0 mps=");
    dbg64_dec((uint64_t)mps);
    dbg64_str(" vendor=");
    usb_hex(g_vendor, 4);
    dbg64_str(" product=");
    usb_hex(g_product, 4);
    usb_log_end();
    usb_log_begin();
    dbg64_str("[USB64] set address=");
    dbg64_dec((uint64_t)g_addr);
    dbg64_str(" ok");
    usb_log_end();

    // ---- 4) 配置描述符前 9 字节（拿 wTotalLength）----
    if (usb_control64(g_addr, 0x80, 6, 0x0200, 0, 9, buf, &n) != 0 || n < 9)
        return usb_enum_fail("get-config-9", 4);
    uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    const uint8_t ifaces = buf[4];
    if (total < 9) return usb_enum_fail("get-config-badlen", 5);
    if (total > buf_bytes) total = buf_bytes;

    // ---- 5) 完整配置描述符 ----
    if (usb_control64(g_addr, 0x80, 6, 0x0200, 0, total, buf, &n) != 0 || n < 9)
        return usb_enum_fail("get-config", 6);

    // ---- 6) 遍历描述符链，找 HID 引导键盘接口 + 它的 IN 中断端点 ----
    //    4 = 接口描述符、5 = 端点描述符、0x21 = HID 描述符。
    //    引导键盘固定 8 字节报告，所以"wMaxPacketSize + 端点地址"从 HID 接口下的
    //    端点描述符取（HID 描述符自己的 wDescriptorLength 只是报告描述符长度，不需要）。
    int hid_iface = -1;
    uint8_t ep_in = 0;
    uint16_t ep_mps = 0;
    bool in_hid = false;
    const uint8_t* p = buf;
    const uint8_t* end = buf + (n < total ? n : total);
    while (p + 2 <= end && p[0] >= 2u) {
        const uint8_t blen = p[0], btype = p[1];
        if (btype == 4u && blen >= 9u) {                       // 接口
            const uint8_t iclass = p[5], isub = p[6];
            in_hid = (iclass == 3u && isub == 1u);             // HID + Boot Interface
            if (in_hid) hid_iface = p[2];
        } else if (btype == 5u && blen >= 7u) {                // 端点
            const uint8_t ea = p[2];
            const uint16_t mp = (uint16_t)(p[4] | ((uint16_t)p[5] << 8));
            if (in_hid && (ea & 0x80u) && ep_in == 0) { ep_in = ea; ep_mps = (uint16_t)(mp & 0x7FFu); }
        } else if (btype == 0x21u) {                           // HID 描述符（这里只需跳过）
        }
        if (p + blen > end) break;
        p += blen;
    }
    if (hid_iface < 0 || ep_in == 0) return usb_enum_fail("hid-interface", 7);

    // ---- 7) SET_CONFIGURATION(1) ----
    if (usb_control64(g_addr, 0x00, 9, 1, 0, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-config", 8);

    g_ep_in  = (uint8_t)(ep_in & 0x0Fu);
    g_ep_mps = ep_mps;
    g_devices = 1;

    usb_log_begin();
    dbg64_str("[USB64] config set value=1 ifaces=");
    dbg64_dec((uint64_t)ifaces);
    dbg64_str(" hid=1 ep_in=");
    usb_hex(ep_in, 2);
    dbg64_str(" mps=");
    dbg64_dec((uint64_t)ep_mps);
    usb_log_end();

    // ---- 8) HID 引导协议：SET_PROTOCOL(0) + SET_IDLE(0) ----
    //    bmRequestType=0x21（类请求、接口、主机到设备）、wIndex = 接口号、wLength = 0
    if (usb_control64(g_addr, 0x21, 0x0B, 0, (uint16_t)hid_iface, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-protocol", 9);
    if (usb_control64(g_addr, 0x21, 0x0A, 0, (uint16_t)hid_iface, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-idle", 10);

    usb_log_begin();
    dbg64_str("[USB64] hid boot protocol set (8-byte reports)");
    usb_log_end();
    return 0;
}

// ==================== 初始化 ====================
// 自检位（0 = 全过；没主控 / 没设备时直接返回 0 —— 那是合法降级，不算失败）：
//   bit0 内存结构没建起来   bit1 控制器没在跑（USBCMD.RS 读回 0）
//   bit2 端口数不合理       bit3 帧列表项没指向 QH
//   bit4 FRBASEADD 读回值不对（寄存器读写本身不通）
//   bit5 有设备但没就绪     bit6 中断 TD 没建    bit7 端点号/包长不对
int usb64_selftest64() {
    if (!g_found) return 0;
    uint32_t mask = 0;
    if (!g_fl_page || !g_td_page || !g_data_page || !g_qh) mask |= 1u;
    if (!(uhci_rd16(UHCI_USBCMD) & USBCMD_RS))             mask |= 2u;
    if (g_ports <= 0)                                      mask |= 4u;
    if (g_fl_page && g_qh) {
        const uint32_t want = qh_phys(g_qh) | UHCI_PTR_QH;
        const volatile uint32_t* fl = (const volatile uint32_t*)(const void*)g_fl_page;
        if (fl[0] != want || fl[USB64_FL_ENTRIES - 1] != want) mask |= 8u;
    }
    if (g_fl_page && uhci_rd32(UHCI_FLBASEADD) != (uint32_t)(uintptr_t)g_fl_page) mask |= 16u;
    if (g_devices > 0) {                                                // 有设备就该就绪
        if (!g_ready)       mask |= 32u;
        if (!g_irq_td)      mask |= 64u;
        if (g_ep_in == 0 || g_ep_mps != 8) mask |= 128u;                // 引导键盘：8 字节报告
    }
    return (int)mask;
}

static void usb_selftest_log() {
    const int st = usb64_selftest64();
    usb_log_begin();
    if (st == 0 && g_state == USB64_ST_READY) {
        dbg64_str("[USB64] selftest PASS");
    } else if (st == 0) {
        dbg64_str("[USB64] selftest skipped (");
        dbg64_str(usb64_state_str64());
        dbg64_putc(')');
    } else {
        dbg64_str("[USB64] selftest FAIL mask=");
        dbg64_dec((uint64_t)st);
    }
    usb_log_end();
}

int usb64_init64() {
    if (g_inited) return g_ready ? 0 : -1;
    g_inited = true;

    // ---- 1) PCI 找 UHCI ----
    if (!uhci_pci_find()) {
        g_state = USB64_ST_NOT_FOUND;
        usb_log_begin();
        dbg64_str("[USB64] not found (no UHCI controller)");
        usb_log_end();
        usb_selftest_log();
        return -1;
    }
    g_found = true;

    // ---- 2) BAR4：I/O BAR（bit0=1）或 MMIO BAR ----
    const uint32_t bar4 = upci_rd32(g_bus, g_dev, g_fn, 0x20);
    if (bar4 & 0x1u) {
        g_io_base = (uint16_t)(bar4 & 0xFFFCu);
        g_mmio = nullptr;
    } else {
        const uint32_t phys = bar4 & 0xFFFFFFF0u;
        if (phys == 0) {
            g_state = USB64_ST_NOT_FOUND;
            usb_log_begin();
            dbg64_str("[USB64] not found (bad BAR4)");
            usb_log_end();
            usb_selftest_log();
            return -1;
        }
        g_mmio = (volatile uint32_t*)(uintptr_t)phys;
    }
    // 打开 I/O(bit0) + MEM(bit1) + BUS MASTER(bit2)：DMA + 寄存器访问都要（与 e1000_64 同款）
    const uint32_t cmd = upci_rd32(g_bus, g_dev, g_fn, 0x04);
    upci_wr32(g_bus, g_dev, g_fn, 0x04, (cmd & 0xFFFFu) | 0x0007u);

    g_ports = 2;                                          // 支持列表里的主控都是 2 个根端口
    usb_log_begin();
    dbg64_str("[USB64] uhci pci ");
    dbg64_dec(g_bus); dbg64_putc(':'); dbg64_dec(g_dev); dbg64_putc('.'); dbg64_dec(g_fn);
    dbg64_str(" io=");
    usb_hex(g_mmio ? (uint32_t)(uintptr_t)g_mmio : (uint32_t)g_io_base, 4);
    dbg64_str(" ports=");
    dbg64_dec((uint64_t)g_ports);
    dbg64_str(g_mmio ? " mmio=1" : "");
    usb_log_end();

    // ---- 3) 内存结构（帧列表 / QH / TD 池 / 数据缓冲）----
    g_fl_page   = (uint8_t*)page_alloc_64();
    g_td_page   = (uint8_t*)page_alloc_64();
    g_data_page = (uint8_t*)page_alloc_64();
    if (!g_fl_page || !g_td_page || !g_data_page) {
        g_state = USB64_ST_NOT_FOUND;
        usb_log_begin();
        dbg64_str("[USB64] not found (out of pages)");
        usb_log_end();
        usb_selftest_log();
        return -1;
    }
    memset_64(g_fl_page, 0, PAGE_SIZE_64);
    memset_64(g_td_page, 0, PAGE_SIZE_64);
    memset_64(g_data_page, 0, PAGE_SIZE_64);

    g_qh       = (UhciQh*)(void*)g_td_page;                // 页首 16 字节 = QH
    g_idle_td  = td_slot(USB64_TD_IDLE);                   // 槽 0 = 永久空 TD
    g_setup_buf  = g_data_page + 0x000;                    // 8 字节
    g_ctl_buf    = g_data_page + 0x010;                    // 256 字节（描述符）
    g_report_buf = g_data_page + 0x200;                    // 8 字节
    g_null_buf   = g_data_page + 0x300;                    // 状态阶段（零长度）哨兵
    g_irq_td   = td_slot(1);
    g_irq_tail = td_slot(2);
    g_td_next  = 3;

    // 空 TD：永远不活跃、token 合法（PID_OUT/长度 0）、link 终止
    td_fill(g_idle_td, 0, UHCI_TOKEN(PID_OUT, 0x7Fu, 0, 0, 0), 0);
    g_idle_td->link = UHCI_PTR_TERM;
    td_fill(g_irq_tail, 0, UHCI_TOKEN(PID_OUT, 0x7Fu, 0, 0, 0), 0);
    g_irq_tail->link = UHCI_PTR_TERM;

    // ---- 4) 软复位（HCRESET 自清）+ 关中断（本驱动全程轮询）----
    uhci_wr16(UHCI_USBINTR, 0);
    uhci_wr16(UHCI_USBCMD, USBCMD_HCRESET);
    {
        uint32_t spins = 0;
        while ((uhci_rd16(UHCI_USBCMD) & USBCMD_HCRESET) && spins < 2000000u) { spins++; nop_pause(); }
    }
    uhci_wr16(UHCI_USBCMD, 0);
    uhci_wr16(UHCI_USBSTS, 0x3Fu);                         // 清状态位（写 1 清）

    // ---- 5) 帧列表：1024 项全部指向 QH；QH 的 element 先指向空 TD ----
    g_qh->link    = UHCI_PTR_TERM;                         // 队列后面没有别的 QH
    g_qh->element = td_phys(g_idle_td);
    g_qh->rsvd0 = 0; g_qh->rsvd1 = 0;
    {
        volatile uint32_t* fl = (volatile uint32_t*)(void*)g_fl_page;
        for (uint32_t i = 0; i < USB64_FL_ENTRIES; i++) fl[i] = qh_phys(g_qh) | UHCI_PTR_QH;
    }
    usb_barrier();
    uhci_wr16(UHCI_FRNUM, 0);
    uhci_wr32(UHCI_FLBASEADD, (uint32_t)(uintptr_t)g_fl_page);
    {   // SOFMOD = 64（帧长正好 1ms）
        const uint8_t sof = 64;
        if (g_mmio) { uint32_t w = g_mmio[UHCI_SOFMOD >> 2]; w = (w & ~0xFFu) | sof; g_mmio[UHCI_SOFMOD >> 2] = w; }
        else        { outb((uint16_t)(g_io_base + UHCI_SOFMOD), sof); }
    }
    uhci_wr16(UHCI_USBCMD, USBCMD_RS | USBCMD_CF | USBCMD_MAXP);   // 跑起来
    usb_barrier();
    usb_delay_ms(5);

    usb_log_begin();
    dbg64_str("[USB64] frame list @");
    usb_hex((uint32_t)(uintptr_t)g_fl_page, 8);
    dbg64_str(" (1024 entries)");
    usb_log_end();

    // ---- 6) 扫根端口：第一个有设备的端口做复位 + 枚举 ----
    int chosen = -1;
    for (int i = 1; i <= g_ports; i++) {
        const uint16_t reg = (i == 1) ? (uint16_t)UHCI_PORTSC1 : (uint16_t)UHCI_PORTSC2;
        const uint16_t v = uhci_rd16(reg);
        if (!(v & PORTSC_CCS)) {
            usb_log_begin();
            dbg64_str("[USB64] no device on port ");
            dbg64_dec((uint64_t)i);
            usb_log_end();
            continue;
        }
        bool low = false;
        const bool ok = usb_port_reset(i, &low);
        if (!ok) {
            usb_log_begin();
            dbg64_str("[USB64] no device on port ");
            dbg64_dec((uint64_t)i);
            usb_log_end();
            continue;
        }
        g_low_speed = low;
        usb_log_begin();
        dbg64_str("[USB64] port ");
        dbg64_dec((uint64_t)i);
        dbg64_str(" connected speed=");
        dbg64_str(low ? "low" : "full");
        dbg64_str(" reset ok");
        usb_log_end();
        chosen = i;
        break;                                             // 单设备：只认第一个
    }
    if (chosen < 0) {
        g_state = USB64_ST_NO_DEVICE;
        usb_selftest_log();
        return -2;
    }

    // ---- 7) 枚举 + HID 引导协议 ----
    if (usb_enumerate() != 0) {
        usb_selftest_log();
        return -3;
    }

    // ---- 8) 武装中断端点：从这一刻起按键会进 PS/2 同一条队列 ----
    g_toggle = 0;
    usb_arm_interrupt();
    g_ready = true;
    g_state = USB64_ST_READY;
    usb_selftest_log();
    return 0;
}

// ==================== 只读接口 ====================
const char* usb64_state_str64() {
    switch (g_state) {
    case USB64_ST_NOT_FOUND:   return "not found";
    case USB64_ST_NO_DEVICE:   return "no device";
    case USB64_ST_ENUM_FAILED: return "enum failed";
    case USB64_ST_READY:       return "ready";
    default:                   return "init";
    }
}
int      usb64_ports64()       { return g_found ? g_ports : 0; }
int      usb64_devices64()     { return g_devices; }
uint64_t usb64_hid_reports64() { return g_hid_reports; }
uint64_t usb64_key_events64()  { return g_key_events; }
