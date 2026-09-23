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
// 6) ★ 批次 O：**USB 存储（U 盘）—— Bulk-Only Transport + SCSI 只读**：
//    * 批量传输复用同一套 TD 链机制（一次最多 64 包 = 4KB，一包一个 TD，DATA0/DATA1 逐包翻转，
//      IN 方向开短包检测"短包即结束"，NAK 由硬件按帧重试）；等待用 g_ticks64 计时**有界超时**
//      （USB64_BULK_TIMEOUT_MS = 600ms），超时/出错一律 abort 整条链，绝不挂死。
//    * 一个 QH 同一时刻只能挂一条链 —— 所以传输期间中断 TD 不在队列上，传完立刻重新武装；
//      传输与 kusb 的 poll 用自旋锁互斥（poll 是 try-lock，拿不到就下次再来，绝不阻塞）。
//    * SCSI：INQUIRY / TEST UNIT READY / REQUEST SENSE / READ CAPACITY(10) / READ(10)；
//      **只读**：没有 WRITE(10)（上层 ata64_write 对 USB 驱动器号直接返回失败并打点）。
//    * 驱动器号接入见 kernel/ata64.h 的 ATA64_USB_BASE（24）：识别/读走本模块，
//      上层（part64/drive64/vfs64/fs64/fat64/explorer64）一行都不用改。
//
// 没验证到的点（如实记录，见文件末）：
//   * 低速（low-speed）设备：代码里按 PORTSC.LSDA 支持（TD 状态 LS 位 + 端口不使能时
//     仍可枚举），但 QEMU 的 usb-kbd 是全速设备，**低速路径没有实机/仿真验证**。
//   * 只识别**直接插在根端口**上的设备（没有 hub/地址分配多设备；最多 2 台：1 键盘 + 1 U 盘）。
//   * 没做：EHCI(USB 2.0)/xHCI(USB 3.x) 主控、USB 鼠标、集线器、拔出检测（热插拔）、
//     U 盘上的分区表解析（分区表由上层 part64/drive64 读，本模块只提供"按扇区读"）、
//     块大小 ≠ 512 的盘（如实拒绝：打点后不暴露成块设备）、USB 存储的写。
// ======================================================================
#include "usb64.h"
#include "port.h"        // inb/inw/inl + outb/outw/outl（32 位端口读写这里已有，不用另加内联汇编）
#include "debug64.h"     // 串口打点（行锁 begin/end）
#include "input.h"       // kbd_inject_scancode()：注入到 PS/2 同一条按键队列
#include "memlayout64.h" // ★ 必须先于 mem_64.h（PAGE_SIZE_64 会撞）；ML64_KERNEL_VA_BASE 用它
#include "mem_64.h"      // page_alloc_64 / memset_64（低内存恒等映射：物理地址即指针）
#include "x86_64.h"      // g_ticks64 / ms_to_ticks64 / nop_pause()
#include <stdint.h>
#include <stddef.h>

extern "C" char __bss_end[];               // 内核镜像高半区上界（判断指针是否落在内核镜像里）
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
#define USB64_TD_SLOTS    100                          // TD 池槽数：16 + 100*32 = 3216 字节（一页 4096 够）。
                                                       // ★ 批次 O：从 64 提到 100 —— 批量传输要 64 个包
                                                       //   （4KB）+ 链尾，槽 0 = 空 TD、1/2 = 中断 TD/tail，
                                                       //   3..99 = 传输链可用。
#define USB64_TD_IDLE     0                            // 槽 0 = 永久"空 TD"（绝不复用）
#define USB64_CTL_TIMEOUT_MS 250u                      // 单次控制传输的等待上限
#define USB64_CTL_MAX_TD  32                           // 一次控制传输最多几个数据 TD（256B/8 = 32）
#define USB64_CTL_BUF_BYTES 256u                       // 控制数据缓冲（描述符）大小
// ★ 批次 O：USB 存储（批量传输 + BOT）用到的上界。都是**有界**的（绝不无限等）。
#define USB64_BULK_TIMEOUT_MS 600u                     // 单次批量传输的等待上限（正常 1 帧就完成）
#define USB64_BULK_MAX_BYTES  4096u                    // 单次批量调用的字节上限（64 包 × 64B）
#define USB64_BULK_MAX_TD     64                       // 单次批量链的 TD 上限（一包一个 TD）
#define USB64_MSC_MAX_DATA    4096u                    // 一条 BOT 命令的数据阶段上限（= 8 个 512B 扇区）
#define USB64_MSC_MAX_SECTORS 8                        // 同上，按 512B 扇区数表达
#define USB64_MAX_DEV         2                        // 最多两台设备：1 个 HID 键盘 + 1 个 USB 存储
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
// ★ 批次 O：**DMA 暂存页** —— 调用方缓冲区落在"认不出物理地址"的地方时的兜底（见 usb_pa32）
static uint8_t*  g_bounce_page = nullptr;    // 4096 字节（页池：恒等映射，物理地址可用）
// ★ 批次 O：USB 存储（U 盘）在 g_data_page 里用到的额外缓冲（都在同一页，恒等映射）
static uint8_t*  g_msc_sec     = nullptr;    // 512 字节：READ(10) 的扇区缓冲 / 自检读的那一块
static uint8_t*  g_cbw_buf     = nullptr;    // 64 字节：CBW（31 字节有效）
static uint8_t*  g_csw_buf     = nullptr;    // 16 字节：CSW（13 字节有效）
static uint8_t*  g_msc_scratch = nullptr;    // 64 字节：INQUIRY / SENSE / CAPACITY 的临时数据

// 设备状态（★ 批次 O：本结构只描述**唯一的 HID 引导键盘**；U 盘的上下文在 g_msc 里，
//   USB64_MAX_DEV = 2 = 1 个键盘 + 1 个存储，两种角色各只有一个实例）
static bool      g_hid_present = false;      // 枚举到 HID 引导键盘（bit5..bit7 自检只看它）
static uint8_t   g_addr        = 0;          // 键盘分配到的 USB 地址（1；有 U 盘时可能是 2）
static bool      g_low_speed   = false;      // 键盘端口是不是低速
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
// ★ 物理地址换算（与 ahci64/nvme64 同款约定，踩过坑）：
//   * 低内存（< 4GB，含帧列表/TD 池/页池/DMA 暂存）：恒等映射，PA == VA；
//   * 内核镜像高半区对象（.bss/.data 里的静态缓冲区，例如 drive64.cpp 的扇区缓冲
//     —— **上层 ata64_read 传进来的就是这种指针**）：直映关系 PA = VA - (VA_BASE - 物理基址)；
//   * 认不出来：返回 0，调用方退回 **DMA 暂存页**（拷贝进出），绝不把错地址交给硬件。
//   为什么必须有这一层：UHCI 的 TD 里放的是**物理**地址，把高半区虚拟地址直接塞进去，
//   QEMU/硬件会往物理低地址写 —— 控制器报"传了 512 字节"，调用方缓冲区却一个字没变
//   （实测踩过：MBR 签名读不到、FAT 探测全灭）。
static inline uint32_t usb_pa32(const void* ptr) {
    const uint64_t v = (uint64_t)(uintptr_t)ptr;
    if (v == 0) return 0;
    if (v < 0x100000000ULL) return (uint32_t)v;
    if (v >= ML64_KERNEL_VA_BASE &&
        v < ((uint64_t)(uintptr_t)__bss_end) + 0x10000ULL) {
        return (uint32_t)(v - (ML64_KERNEL_VA_BASE - (uint64_t)ML64_KERNEL_BASE));
    }
    return 0;
}

static inline void usb_barrier() { __asm__ volatile("" ::: "memory"); }


// ==================== 传输互斥 + 设备上下文 ====================
// 为什么需要互斥：批量传输（U 盘）是**同步自旋等待**的（由发起的文件管理器/终端线程调用），
// 而 kusb 内核线程每 ~12ms 也会进来调 usb64_poll64() 重新武装中断 TD；两者都写 QH.element。
// 做法：传输侧自旋拿锁（临界区只有几十条指令，拿不到也只是多转几圈）；poll 侧**try-lock**，
// 拿不到就"这次不重新武装"，下次再来 —— 绝不阻塞、绝不挂死。
static volatile uint32_t g_usb_lock = 0;

static inline bool usb_lock_try64() {
    uint32_t v = 1;
    __asm__ volatile("xchgl %0, %1" : "+r"(v) : "m"(g_usb_lock) : "memory");
    return v == 0;                                  // 拿到的标志 = 换出来的旧值是 0
}

static void usb_lock_acquire64() {
    uint64_t spins = 0;
    while (!usb_lock_try64() && spins < 400000000ull) { spins++; nop_pause(); }
}

static inline void usb_lock_release64() {
    __asm__ volatile("" ::: "memory");
    g_usb_lock = 0;
}

// 一台设备在**枚举/控制传输/批量传输**时需要的最小上下文：
//   addr      = 目标 USB 地址（枚举第一步是 0，SET_ADDRESS 之后才是分配到的地址）
//   ctl_mps   = EP0 最大包   low_speed = 低速设备标志（TD 的 LS 位 + 控制传输分片大小）
struct Usb64Ctl64 {
    uint8_t  addr;
    uint8_t  ctl_mps;
    bool     low_speed;
    uint16_t vendor;
    uint16_t product;
};

// 前置声明：控制/批量传输跑完要把中断 TD 重新挂回队列（定义在本文件后半，见 usb_arm_interrupt）
static void usb_arm_interrupt();
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
// c = 目标设备的上下文（EP0 最大包 / 低速标志从它取；**地址仍用参数 addr** —— 枚举第一步
//     必须在默认地址 0 上收发，而那时 c->addr 还没生效）；
// rt/req/val/idx/len = 标准 USB 控制请求字段；data = 数据缓冲（IN 收 / OUT 发，可为 nullptr）。
// 返回 0 = 成功；-1 = 超时或硬件错；-2 = 设备 STALL。
static int usb_control64(const Usb64Ctl64* c, uint8_t addr, uint8_t rt, uint8_t req, uint16_t val,
                         uint16_t idx, uint16_t len, uint8_t* data, uint16_t* out_len) {
    if (out_len) *out_len = 0;
    if (!c) return -1;
    // ★ 批次 O：控制传输与 kusb 的 poll / 批量传输共用同一个 QH —— 全程互斥（见 usb_lock_*）。
    usb_lock_acquire64();

    // ---- SETUP 包（8 字节，PID=SETUP，toggle 恒为 DATA0）----
    uint8_t* su = g_setup_buf;
    su[0] = rt; su[1] = req;
    su[2] = (uint8_t)(val & 0xFFu); su[3] = (uint8_t)(val >> 8);
    su[4] = (uint8_t)(idx & 0xFFu); su[5] = (uint8_t)(idx >> 8);
    su[6] = (uint8_t)(len & 0xFFu); su[7] = (uint8_t)(len >> 8);

    const bool in_dir = (rt & 0x80u) != 0;
    const uint32_t lsflag = c->low_speed ? (uint32_t)TD_LS : 0u;

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
        uint16_t pktsze = (uint16_t)(c->ctl_mps ? c->ctl_mps : 8);
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
    if (remain != 0) {                              // 切片太多（理论到不了）
        uhci_abort_chain(setup_td);
        usb_lock_release64();
        return -1;
    }

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
        usb_lock_release64();
        return -1;
    }

    const uint32_t ss = status_td->status;
    if (ss & TD_STALLED) { uhci_abort_chain(setup_td); usb_lock_release64(); return -2; }
    if (ss & TD_ERR_MASK) { uhci_abort_chain(setup_td); usb_lock_release64(); return -3; }

    // ---- 汇总数据阶段实际收到的字节数（按"短包即结束"的规则）----
    uint16_t total = 0;
    if (first_data) {
        volatile UhciTd* t = first_data;
        for (;;) {
            const uint32_t ts = t->status;
            if (ts & TD_ERR_MASK) { uhci_abort_chain(setup_td); usb_lock_release64(); return -3; }
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
    // 控制传输期间中断 TD 也不在队列上：传完立刻重新武装（否则键盘会一直待机）
    if (g_ready && g_irq_td) usb_arm_interrupt();
    usb_lock_release64();
    return 0;
}

// ==================== 枚举失败打点（多打一行 stage，方便定位卡在哪一步）====================
// 注意：**不在这里改 g_state** —— 批次 O 起一台设备失败不代表整个主控失败（例如键盘枚举
// 失败但 U 盘成功了），状态由 usb64_init64 在所有端口都试完后统一判定。
static int usb_enum_fail(const char* stage, int rc) {
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
    // ★ 批次 O：有传输在跑时（mass storage 的批量传输是同步自旋等待的）不碰队列 ——
    //   拿不到锁就"这次不重新武装"，下次轮询再来。绝不阻塞、绝不挂死。
    if (!usb_lock_try64()) return;
    const uint32_t st = g_irq_td->status;
    if (st & TD_ACTIVE) { usb_lock_release64(); return; }   // 还没完成（没有报告时硬件一直 NAK）

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
    usb_lock_release64();
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


// ==================== 批量传输（Bulk IN / Bulk OUT）====================
// 复用同一套"TD 链 + QH 发布"机制（见文件头第 3 点）。与中断/控制传输的差别：
//   * 一次最多 64 个包（USB64_BULK_MAX_TD = 64 × 64B = 4KB），**有界超时**（不挂死）；
//   * IN 方向：除最后一包外都开 **SPD（短包检测）** —— 短包 = 传输自然结束
//     （INQUIRY / CSW 都可能短），链上后面的 TD 不再被执行；我们扫链时"遇到第一个短包就收尾"；
//   * NAK 由硬件按帧重试（TD 保持 ACTIVE、队列停在这一帧），软件只负责超时与出错判定。
// 结果码 -> 打点用的原因（与 [USBST] read FAILED reason=... 对齐；STALL/硬件错都归 "nak"）。
#define USB64_XFER_OK       0
#define USB64_XFER_TIMEOUT (-1)
#define USB64_XFER_STALL   (-2)
#define USB64_XFER_HWERR   (-3)

static const char* usb_xfer_reason(int r) {
    switch (r) {
    case USB64_XFER_TIMEOUT: return "timeout";
    case USB64_XFER_STALL:   return "nak";
    case USB64_XFER_HWERR:   return "nak";
    default:                 return "?";
    }
}

// 一次批量传输：ep = 端点号（不含方向位）、in = 方向、len = 字节数、mps = 该端点最大包。
// toggle = 进出参（进入时是本次第一个包的 DATA 位，返回时推进到"下一个包该用的值"）。
static int usb_bulk64(const Usb64Ctl64* c, uint8_t ep, bool in, uint8_t* buf, uint32_t len,
                      uint16_t mps, uint8_t* toggle, uint32_t* got_out) {
    if (got_out) *got_out = 0;
    uint16_t pk = mps ? mps : 64;
    if (pk > 64) pk = 64;                                // UHCI/全速：单包最大 64 字节
    if (len == 0) return USB64_XFER_OK;
    if (len > USB64_BULK_MAX_BYTES) return USB64_XFER_HWERR;    // 调用方必须自己分块
    // ★ 物理地址：UHCI 的 TD 里必须放**物理**地址（见 usb_pa32 的踩坑说明）。
    //   调用方缓冲区落在"认不出的地址空间"时退回 **DMA 暂存页**（拷进/拷出），绝不把错地址给硬件。
    uint8_t* dma = buf;
    bool bounce = false;
    if (usb_pa32(buf) == 0) {
        if (!g_bounce_page) return USB64_XFER_HWERR;
        dma = g_bounce_page;
        bounce = true;
        if (!in) memcpy_64(dma, buf, len);               // OUT：先把要发的数据拷进暂存页
    }

    usb_lock_acquire64();
    uhci_stop_queue();

    const uint32_t lsflag = c->low_speed ? (uint32_t)TD_LS : 0u;
    int slot = g_td_next;
    uint8_t tg = toggle ? *toggle : 0;
    volatile UhciTd* first = nullptr;
    volatile UhciTd* prev  = nullptr;
    volatile UhciTd* tds[USB64_BULK_MAX_TD];
    int n = 0;
    uint32_t remain = len;
    uint8_t* dp = dma;
    while (remain > 0 && n < (int)USB64_BULK_MAX_TD) {
        const uint32_t chunk = (remain < pk) ? remain : pk;
        const uint32_t pa = usb_pa32(dp);
        if (pa == 0) {                                   // 防御：暂存页本身认不出（不该发生）
            if (first) uhci_abort_chain(first);
            g_td_next = slot;
            usb_lock_release64();
            return USB64_XFER_HWERR;
        }
        uint32_t st = TD_MAXERR3 | lsflag | TD_ACTIVE;
        if (in && remain > pk) st |= TD_SPD;             // 短包检测：短包 = 传输结束
        volatile UhciTd* t = td_slot(slot); slot = td_next_slot(slot);
        td_fill(t, st, UHCI_TOKEN(in ? (uint32_t)PID_IN : (uint32_t)PID_OUT, c->addr, ep, tg, chunk),
                pa);
        t->link = UHCI_PTR_TERM;
        if (prev) prev->link = td_phys(t);
        prev = t;
        if (!first) first = t;
        tds[n++] = t;
        tg ^= 1;
        dp += chunk;
        remain -= chunk;
    }
    if (remain != 0 || !first) {                         // 防御：太长（调用方没分块）
        if (first) uhci_abort_chain(first);
        g_td_next = slot;
        usb_lock_release64();
        return USB64_XFER_HWERR;
    }
    // 链尾：不活跃的空 TD + 终止（硬件走完最后一个真 TD 只会看到"不活跃 + 终止"）
    volatile UhciTd* tail = td_slot(slot); slot = td_next_slot(slot);
    td_fill(tail, 0, UHCI_TOKEN((uint32_t)PID_OUT, c->addr, ep, 0, 0), 0);
    tail->link = UHCI_PTR_TERM;
    prev->link = td_phys(tail);
    g_td_next = slot;

    // ★ 发布：一次 32 位对齐写（硬件要么看到空链，要么看到这条链）
    usb_barrier();
    g_qh->element = td_phys(first);
    usb_barrier();

    // ---- 等链完成 / 短包 / 出错 / 超时 ----
    int res = USB64_XFER_OK;
    uint32_t total = 0;
    int idx = 0;
    bool done = false;
    const uint64_t t0 = g_ticks64;
    const uint64_t want = ms_to_ticks64(USB64_BULK_TIMEOUT_MS);
    uint64_t spin = 0;
    for (;;) {
        for (; idx < n; idx++) {
            const uint32_t st = tds[idx]->status;
            if (st & TD_ACTIVE) break;                    // 还没完成（或被前面的短包截停）
            if (st & TD_STALLED) { res = USB64_XFER_STALL; done = true; break; }
            if (st & TD_ERR_MASK) { res = USB64_XFER_HWERR; done = true; break; }
            const uint16_t act = (uint16_t)UHCI_ACTLEN(st);
            total += act;
            // 短包 = 传输结束（剩下的 TD 不会再被执行，队列停在这里）
            if (act < (uint16_t)UHCI_EXPLEN(tds[idx]->token)) { idx++; done = true; break; }
        }
        if (done) break;
        if (idx >= n) break;                             // 整条链都完成了
        if ((g_ticks64 - t0) >= want || ++spin > 400000000ull) { res = USB64_XFER_TIMEOUT; break; }
        nop_pause();
    }

    if (res != USB64_XFER_OK) uhci_abort_chain(first);
    else                      uhci_stop_queue();
    if (toggle) *toggle = tg;
    if (got_out) *got_out = total;
    // 暂存页兜底：IN 方向把真正收到的字节拷回调用方缓冲区（短包时只拷实际长度）
    if (bounce && in && total > 0) memcpy_64(buf, dma, (total < len) ? total : len);

    // ★ 批量传输期间中断 TD 不在队列上（一个 QH 同一时刻只挂一条链）——传完立刻重新武装，
    //   否则键盘会一直"待机"（poll 看到它还 ACTIVE 就不会重新发布）。
    if (g_ready && g_irq_td) usb_arm_interrupt();
    usb_lock_release64();
    return res;
}

// ==================== USB 存储：Bulk-Only Transport（BOT）+ SCSI 只读子集 ====================
// 依据 USB MSC BOT 规范（rev 1.0）：
//   CBW（31 字节）：签名 'USBC'（0x43425355）、Tag、dCBWDataTransferLength、方向（bit7 = IN）、
//     LUN、CB 长度、16 字节 CDB；
//   CSW（13 字节）：签名 'USBS'（0x53425355）、Tag 回显、dCSWDataResidue、状态（0 = 通过）。
// DATA toggle（如实按规范）：CBW 恒 DATA0、CSW 恒 DATA1、数据阶段从 DATA0 起逐包翻转；
//   每条命令开始时两条批量端点的 toggle 都从 DATA0 重新开始（Linux usb-storage 同款假设）。
// 本批只做**读**：INQUIRY / TEST UNIT READY / REQUEST SENSE / READ CAPACITY(10) / READ(10)。
#define BOT_CBW_SIG 0x43425355u
#define BOT_CSW_SIG 0x53425355u
#define BOT_CBW_LEN 31
#define BOT_CSW_LEN 13
#define BOT_DIR_IN  0x80u

struct Usb64Msc64 {
    bool     present;          // 枚举到 BOT 接口
    bool     supported;        // 探测全过 + 块大小 512（才算一块可用的块设备）
    uint8_t  addr;
    bool     low_speed;
    uint8_t  iface;
    uint8_t  lun;
    uint8_t  ep_in, ep_out;
    uint16_t mps_in, mps_out;
    uint32_t csw_residue;      // dCSWDataResidue（应为 0；!= 0 = 数据阶段没搬完 -> 当失败）
    uint32_t dbg_data_got;     // 最近一次数据阶段实际搬到的字节数（诊断/排障）
    uint32_t tag;              // CBW/CSW 配对的 Tag（自增）
    uint32_t blocks;           // 块数（= READ CAPACITY(10) 的"最后一个 LBA + 1"）
    uint32_t block_size;
    uint8_t  csw_status;       // 最近一次 CSW 状态字节
    uint32_t reads_ok, reads_fail;
    int      last_reason;      // 见 usb64_msc_last_reason64()
    char     vendor[9];        // INQUIRY 的厂商（8 字节 + NUL）
    char     product[17];      // INQUIRY 的型号（16 字节 + NUL）
    uint8_t  removable;        // INQUIRY 的 RMB 位（= 可移动介质）
    uint32_t selftest_mask;
};
static Usb64Msc64 g_msc;
static uint32_t   g_usbst_read_logs = 0;      // 成功读的打点上限（防刷屏；失败另有上限）
static uint32_t   g_usbst_fail_logs = 0;
#define USBST_READ_LOG_MAX 128u
#define USBST_FAIL_LOG_MAX 64u

static uint32_t msc_rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint32_t msc_rd32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void msc_wr32le(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void msc_wr16be(uint8_t* p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

// 打印定长字段（INQUIRY 的厂商/型号是空格填充的）；不可打印字节替换成 '.'，别把控制字符灌进串口
static void msc_log_str(const char* s, int n) {
    int last = n - 1;
    while (last >= 0 && (s[last] == ' ' || s[last] == 0)) last--;
    for (int i = 0; i <= last; i++) {
        const char ch = s[i];
        dbg64_putc((ch >= 0x20 && ch < 0x7F) ? ch : '.');
    }
}

// 一条 BOT 命令：CBW -> [数据阶段] -> CSW。
// 返回 0 = 通过；1/2 = CSW 报的命令失败/阶段错；<0 = 传输层失败（见 g_msc.last_reason）。
static int usb_bot64(uint8_t* cdb, uint8_t cdb_len, bool dir_in, uint32_t data_len, uint8_t* data) {
    Usb64Msc64& m = g_msc;
    if (!m.present) { m.last_reason = 5; return -1; }
    if (data_len > USB64_MSC_MAX_DATA) { m.last_reason = 4; return -1; }

    Usb64Ctl64 c;
    c.addr = m.addr; c.ctl_mps = 64; c.low_speed = m.low_speed; c.vendor = 0; c.product = 0;

    // ---- CBW（31 字节，DATA0）----
    uint8_t* cbw = g_cbw_buf;
    for (int i = 0; i < BOT_CBW_LEN; i++) cbw[i] = 0;
    msc_wr32le(cbw + 0, BOT_CBW_SIG);
    m.tag++;
    msc_wr32le(cbw + 4, m.tag);
    msc_wr32le(cbw + 8, data_len);
    cbw[12] = dir_in ? (uint8_t)BOT_DIR_IN : 0;
    cbw[13] = (uint8_t)(m.lun & 0x0Fu);
    cbw[14] = (uint8_t)(cdb_len & 0x1Fu);
    for (int i = 0; i < 16; i++) cbw[15 + i] = (i < (int)cdb_len) ? cdb[i] : 0;

    uint8_t  tg = 0;
    uint32_t got = 0;
    int r = usb_bulk64(&c, m.ep_out, false, cbw, BOT_CBW_LEN, m.mps_out, &tg, &got);
    if (r != USB64_XFER_OK || got != BOT_CBW_LEN) {
        m.last_reason = (r == USB64_XFER_TIMEOUT) ? 2 : 3;
        return -1;
    }

    // ---- 数据阶段（toggle 从 DATA0 起；短包由 usb_bulk64 的短包检测收尾）----
    if (data_len > 0 && data) {
        tg = 0;
        got = 0;
        r = usb_bulk64(&c, dir_in ? m.ep_in : m.ep_out, dir_in, data, data_len,
                       dir_in ? m.mps_in : m.mps_out, &tg, &got);
        m.dbg_data_got = got;
        if (r != USB64_XFER_OK) { m.last_reason = (r == USB64_XFER_TIMEOUT) ? 2 : 3; return -1; }
        // 短包（got < data_len）不算传输层错误：设备可以只回它有的（INQUIRY 常见），
        // 由调用方按 got 判断；但**一个字节都没有**就是真失败。
        if (got == 0) { m.last_reason = 1; return -1; }
    } else {
        m.dbg_data_got = 0;
    }

    // ---- CSW（13 字节，DATA1）----
    tg = 1;
    got = 0;
    r = usb_bulk64(&c, m.ep_in, true, g_csw_buf, BOT_CSW_LEN, m.mps_in, &tg, &got);
    if (r != USB64_XFER_OK || got != BOT_CSW_LEN) {
        m.last_reason = (r == USB64_XFER_TIMEOUT) ? 2 : 3;
        return -1;
    }
    const uint32_t sig = msc_rd32le(g_csw_buf);
    const uint32_t tag = msc_rd32le(g_csw_buf + 4);
    m.csw_status  = g_csw_buf[12];
    m.csw_residue = msc_rd32le(g_csw_buf + 8);        // dCSWDataResidue（规范字段，诊断/校验都用它）
    if (sig != BOT_CSW_SIG || tag != m.tag) { m.last_reason = 1; return -1; }   // 签名/Tag 不对 = 阶段错
    if (m.csw_status != 0) { m.last_reason = 1; return (int)m.csw_status; }
    // ★ 数据阶段没搬完（residue != 0）也要**当成失败**：上层（ata64）以为整段都读到了，
    //   实际只有前面一部分 —— 不报错的话 FAT/MBR 会解析到"混合数据"（实测踩过：MBR 签名读不到）。
    if (m.csw_residue != 0) { m.last_reason = 6; return -2; }
    m.last_reason = 0;
    return 0;
}

// REQUEST SENSE(0x03)：出错时读 18 字节 SENSE 并把 key/asc/ascq 打进串口（定位用）
static void usb_msc_log_sense64(const char* what) {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x03;
    cdb[4] = 18;
    for (int i = 0; i < 18; i++) g_msc_scratch[i] = 0;
    const int r = usb_bot64(cdb, 6, true, 18, g_msc_scratch);
    usb_log_begin();
    dbg64_str("[USBST] sense after=");
    dbg64_str(what);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(r < 0 ? 99u : (uint32_t)r));
    if (r == 0 && g_msc_scratch[0] == 0x70u) {              // 固定格式 SENSE 数据
        dbg64_str(" key=");
        usb_hex((uint32_t)(g_msc_scratch[2] & 0x0Fu), 1);
        dbg64_str(" asc=");
        usb_hex(g_msc_scratch[12], 2);
        dbg64_str(" ascq=");
        usb_hex(g_msc_scratch[13], 2);
    }
    usb_log_end();
}

// INQUIRY(0x12)：36 字节标准查询数据（厂商 8 + 型号 16 + 版本 4），RMB 位看是否可移动
static int usb_msc_inquiry64() {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x12;
    cdb[4] = 36;
    for (int i = 0; i < 36; i++) g_msc_scratch[i] = 0;
    const int r = usb_bot64(cdb, 6, true, 36, g_msc_scratch);
    if (r != 0) { usb_msc_log_sense64("inquiry"); return -1; }
    g_msc.removable = (g_msc_scratch[1] & 0x80u) ? 1 : 0;
    for (int i = 0; i < 8; i++)  g_msc.vendor[i]  = (char)g_msc_scratch[8 + i];
    g_msc.vendor[8] = 0;
    for (int i = 0; i < 16; i++) g_msc.product[i] = (char)g_msc_scratch[16 + i];
    g_msc.product[16] = 0;
    usb_log_begin();
    dbg64_str("[USBST] inquiry vendor=");
    msc_log_str(g_msc.vendor, 8);
    dbg64_str(" product=");
    msc_log_str(g_msc.product, 16);
    dbg64_str(g_msc.removable ? " rmb=1" : " rmb=0");
    usb_log_end();
    return 0;
}

// READ CAPACITY(10)(0x25)：8 字节（最后 LBA(4, 大端) + 块大小(4, 大端)）
// 打点：[USBST] capacity blocks=<n> block_size=<n> bytes=<n> cap_mb=<n>[ cap_gb=<x.yy>]
static int usb_msc_capacity64() {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x25;
    for (int i = 0; i < 8; i++) g_msc_scratch[i] = 0;
    const int r = usb_bot64(cdb, 10, true, 8, g_msc_scratch);
    if (r != 0) { usb_msc_log_sense64("read-capacity"); return -1; }
    const uint32_t last = msc_rd32be(g_msc_scratch);
    const uint32_t bs   = msc_rd32be(g_msc_scratch + 4);
    g_msc.blocks = last + 1u;
    g_msc.block_size = bs;
    const uint64_t bytes = (uint64_t)g_msc.blocks * (uint64_t)bs;
    usb_log_begin();
    dbg64_str("[USBST] capacity blocks=");
    dbg64_dec((uint64_t)g_msc.blocks);
    dbg64_str(" block_size=");
    dbg64_dec((uint64_t)bs);
    dbg64_str(" bytes=");
    dbg64_dec(bytes);
    dbg64_str(" cap_mb=");
    dbg64_dec(bytes / (1024ull * 1024ull));
    if (bytes >= (1024ull * 1024ull * 1024ull)) {           // ≥ 1GB 时再给一个两位小数的 GB
        const uint64_t gb100 = (bytes * 100ull) / (1024ull * 1024ull * 1024ull);
        dbg64_str(" cap_gb=");
        dbg64_dec(gb100 / 100ull);
        dbg64_putc('.');
        const uint64_t frac = gb100 % 100ull;
        dbg64_putc((char)('0' + (frac / 10ull)));
        dbg64_putc((char)('0' + (frac % 10ull)));
    }
    usb_log_end();
    return 0;
}

// READ(10)(0x28)：lba(4, 大端) @2..5、传输块数(2, 大端) @7..8；数据 = count × 512 字节
// 打点：[USBST] read lba=<n> count=<n> ok / read FAILED lba=<n> reason=<...>
static int usb_msc_read10_64(uint32_t lba, uint16_t count, uint8_t* buf) {
    uint8_t cdb[16];
    for (int i = 0; i < 16; i++) cdb[i] = 0;
    cdb[0] = 0x28;
    cdb[2] = (uint8_t)(lba >> 24); cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);  cdb[5] = (uint8_t)lba;
    msc_wr16be(cdb + 7, count);
    const uint32_t bytes = (uint32_t)count * 512u;
    const int r = usb_bot64(cdb, 10, true, bytes, buf);
    if (r != 0) {
        g_msc.reads_fail++;
        if (g_usbst_fail_logs < USBST_FAIL_LOG_MAX) {
            g_usbst_fail_logs++;
            usb_log_begin();
            dbg64_str("[USBST] read FAILED lba=");
            dbg64_dec((uint64_t)lba);
            dbg64_str(" count=");
            dbg64_dec((uint64_t)count);
            dbg64_str(" reason=");
            dbg64_str(r > 0 ? "csw status" : usb_xfer_reason(r));
            if (r > 0) {
                dbg64_str(" csw=");
                dbg64_dec((uint64_t)g_msc.csw_status);
            }
            usb_log_end();
            if (r > 0) usb_msc_log_sense64("read10");
        }
        return -1;
    }
    g_msc.reads_ok++;
    if (g_usbst_read_logs < USBST_READ_LOG_MAX) {
        g_usbst_read_logs++;
        usb_log_begin();
        dbg64_str("[USBST] read lba=");
        dbg64_dec((uint64_t)lba);
        dbg64_str(" count=");
        dbg64_dec((uint64_t)count);
        dbg64_str(" ok");
        usb_log_end();
        if (g_usbst_read_logs == USBST_READ_LOG_MAX) {
            usb_log_begin();
            dbg64_str("[USBST] read log capped at ");
            dbg64_dec((uint64_t)USBST_READ_LOG_MAX);
            dbg64_str(" lines (further successes not printed; counters still count)");
            usb_log_end();
        }
    }
    return 0;
}
// ==================== 枚举（一台设备）====================
// 流程与改动前一致，只是从"只做第一台设备"变成"每台设备各做一遍"：
//   GET_DESCRIPTOR(Device,8) -> SET_ADDRESS(addr) -> GET_DESCRIPTOR(Device,18)
//   -> GET_DESCRIPTOR(Config,9) -> GET_DESCRIPTOR(Config,total)
//   -> 遍历描述符链：HID 引导键盘（class=3 sub=1，中断 IN）/ USB 存储 BOT
//      （class=8 sub=6 proto=0x50，两个批量端点）
//   -> SET_CONFIGURATION(1) -> 角色化（键盘：SET_PROTOCOL(0) + SET_IDLE(0) + 武装中断端点）
// 返回 0 = 这台设备被本驱动接管（键盘或存储）；-1 = 失败 / 不是支持的设备（调用方继续看别的端口）。
static int usb_enum_port(int port_idx, uint8_t addr, bool low_speed) {
    Usb64Ctl64 c;
    c.addr = addr; c.ctl_mps = 8; c.low_speed = low_speed; c.vendor = 0; c.product = 0;
    uint8_t* buf = g_ctl_buf;                       // DMA 缓冲：必须在页池里（恒等映射物理地址）
    const uint16_t buf_bytes = USB64_CTL_BUF_BYTES;
    uint16_t n = 0;

    // ---- 1) 8 字节设备描述符（默认地址 0；此时还不知道设备 mps，只能按 8 字节收）----
    if (usb_control64(&c, 0, 0x80, 6, 0x0100, 0, 8, buf, &n) != 0 || n < 8)
        return usb_enum_fail("get-device-8", 1);
    uint8_t mps = buf[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64) mps = 8;
    c.ctl_mps = mps;

    // ---- 2) SET_ADDRESS(addr)（0 字节数据阶段）----
    if (usb_control64(&c, 0, 0x00, 5, (uint16_t)addr, 0, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-address", 2);
    usb_delay_ms(10);                                    // 设备切换地址的恢复时间

    // ---- 3) 完整设备描述符（18 字节，现在在新地址上）----
    if (usb_control64(&c, addr, 0x80, 6, 0x0100, 0, 18, buf, &n) != 0 || n < 18)
        return usb_enum_fail("get-device-18", 3);
    c.vendor  = (uint16_t)(buf[8] | ((uint16_t)buf[9] << 8));
    c.product = (uint16_t)(buf[10] | ((uint16_t)buf[11] << 8));

    // ★ 这一行的 addr=0 指的是"枚举时发现它的默认地址"（8 字节描述符就是在地址 0 读的，
    //   mps 也来自那一次读）；vendor/product 只存在于 18 字节描述符里，而按 USB 规定
    //   它必须在 SET_ADDRESS 之后才能安全读全 —— 所以三个字段来自同一次枚举的不同步。
    usb_log_begin();
    dbg64_str("[USB64] device addr=0 mps=");
    dbg64_dec((uint64_t)mps);
    dbg64_str(" vendor=");
    usb_hex(c.vendor, 4);
    dbg64_str(" product=");
    usb_hex(c.product, 4);
    usb_log_end();
    usb_log_begin();
    dbg64_str("[USB64] set address=");
    dbg64_dec((uint64_t)addr);
    dbg64_str(" ok");
    usb_log_end();

    // ---- 4) 配置描述符前 9 字节（拿 wTotalLength）----
    if (usb_control64(&c, addr, 0x80, 6, 0x0200, 0, 9, buf, &n) != 0 || n < 9)
        return usb_enum_fail("get-config-9", 4);
    uint16_t total = (uint16_t)(buf[2] | ((uint16_t)buf[3] << 8));
    const uint8_t ifaces = buf[4];
    if (total < 9) return usb_enum_fail("get-config-badlen", 5);
    if (total > buf_bytes) total = buf_bytes;

    // ---- 5) 完整配置描述符 ----
    if (usb_control64(&c, addr, 0x80, 6, 0x0200, 0, total, buf, &n) != 0 || n < 9)
        return usb_enum_fail("get-config", 6);

    // ---- 6) 遍历描述符链，找两种我们支持的接口 ----
    //    4 = 接口描述符、5 = 端点描述符（bmAttributes 低 2 位：3 = 中断、2 = 批量）、
    //    0x21 = HID 描述符（这里只需跳过）。引导键盘固定 8 字节报告，所以端点信息从端点
    //    描述符取；USB 存储要两个**批量**端点（IN = 设备到主机、OUT = 主机到设备）。
    int hid_iface = -1; uint8_t hid_ep = 0; uint16_t hid_mps = 0;
    int msc_iface = -1; uint8_t msc_in = 0, msc_out = 0; uint16_t msc_mps_in = 0, msc_mps_out = 0;
    bool in_hid = false, in_msc = false;
    const uint8_t* p = buf;
    const uint8_t* end = buf + (n < total ? n : total);
    while (p + 2 <= end && p[0] >= 2u) {
        const uint8_t blen = p[0], btype = p[1];
        if (btype == 4u && blen >= 9u) {                       // 接口
            const uint8_t iclass = p[5], isub = p[6], iproto = p[7];
            in_hid = (iclass == 3u && isub == 1u);             // HID + Boot Interface
            in_msc = (iclass == 8u && isub == 6u && iproto == 0x50u);   // Mass Storage + SCSI + BOT
            if (in_hid && hid_iface < 0) hid_iface = p[2];
            if (in_msc && msc_iface < 0) msc_iface = p[2];
        } else if (btype == 5u && blen >= 7u) {                // 端点
            const uint8_t ea = p[2];
            const uint8_t attr = (uint8_t)(p[3] & 0x03u);
            const uint16_t mp = (uint16_t)((p[4] | ((uint16_t)p[5] << 8)) & 0x7FFu);
            if (in_hid && attr == 3u && (ea & 0x80u) && hid_ep == 0) { hid_ep = ea; hid_mps = mp; }
            if (in_msc && attr == 2u) {                        // ★ 批量端点
                if ((ea & 0x80u) && msc_in == 0)         { msc_in = ea;  msc_mps_in = mp; }
                else if (!(ea & 0x80u) && msc_out == 0)  { msc_out = ea; msc_mps_out = mp; }
            }
        } else if (btype == 0x21u) {                           // HID 描述符（这里只需跳过）
        }
        if (p + blen > end) break;
        p += blen;
    }

    // 角色判定：键盘优先（已经有键盘就不占），其次 USB 存储（已经有存储就不占）。
    int role = 0;                                              // 0=不支持 1=HID 键盘 2=USB 存储
    if (hid_iface >= 0 && hid_ep != 0 && !g_hid_present)       role = 1;
    else if (msc_iface >= 0 && msc_in != 0 && msc_out != 0 && !g_msc.present) role = 2;
    if (role == 0) {
        usb_log_begin();
        dbg64_str("[USB64] device addr=0 port=");
        dbg64_dec((uint64_t)port_idx);
        dbg64_str(" unsupported interface (skipped: only HID boot keyboard and USB storage BOT)");
        usb_log_end();
        return -1;
    }

    // ---- 7) SET_CONFIGURATION(1) ----
    if (usb_control64(&c, addr, 0x00, 9, 1, 0, 0, nullptr, nullptr) != 0)
        return usb_enum_fail("set-config", 8);

    if (role == 1) {
        // ---- HID 引导键盘 ----
        g_addr    = addr;
        g_low_speed = low_speed;
        g_vendor  = c.vendor;
        g_product = c.product;
        g_ep_in   = (uint8_t)(hid_ep & 0x0Fu);
        g_ep_mps  = hid_mps;
        g_hid_present = true;

        usb_log_begin();
        dbg64_str("[USB64] config set value=1 ifaces=");
        dbg64_dec((uint64_t)ifaces);
        dbg64_str(" hid=1 ep_in=");
        usb_hex(hid_ep, 2);
        dbg64_str(" mps=");
        dbg64_dec((uint64_t)hid_mps);
        usb_log_end();

        // ---- 8) HID 引导协议：SET_PROTOCOL(0) + SET_IDLE(0) ----
        //    bmRequestType=0x21（类请求、接口、主机到设备）、wIndex = 接口号、wLength = 0
        if (usb_control64(&c, addr, 0x21, 0x0B, 0, (uint16_t)hid_iface, 0, nullptr, nullptr) != 0)
            return usb_enum_fail("set-protocol", 9);
        if (usb_control64(&c, addr, 0x21, 0x0A, 0, (uint16_t)hid_iface, 0, nullptr, nullptr) != 0)
            return usb_enum_fail("set-idle", 10);

        usb_log_begin();
        dbg64_str("[USB64] hid boot protocol set (8-byte reports)");
        usb_log_end();

        g_toggle = 0;
        usb_arm_interrupt();                       // 从这一刻起按键会进 PS/2 同一条队列
        g_ready = true;
        return 0;
    }

    // ---- USB 存储（Bulk-Only Transport）：记下两个批量端点，探测放在所有设备枚举完之后 ----
    g_msc.present   = true;
    g_msc.supported = false;                       // 探测（INQUIRY/CAPACITY/READ）过了才置 true
    g_msc.addr      = addr;
    g_msc.low_speed = low_speed;
    g_msc.iface     = (uint8_t)msc_iface;
    g_msc.lun       = 0;
    g_msc.ep_in     = (uint8_t)(msc_in & 0x0Fu);
    g_msc.ep_out    = (uint8_t)(msc_out & 0x0Fu);
    g_msc.mps_in    = msc_mps_in ? msc_mps_in : 64u;
    g_msc.mps_out   = msc_mps_out ? msc_mps_out : 64u;
    g_msc.tag       = 0;
    g_msc.last_reason = 0;

    usb_log_begin();
    dbg64_str("[USB64] config set value=1 ifaces=");
    dbg64_dec((uint64_t)ifaces);
    dbg64_str(" msc=1 ep_in=");
    usb_hex(msc_in, 2);
    dbg64_str(" ep_out=");
    usb_hex(msc_out, 2);
    dbg64_str(" mps=");
    dbg64_dec((uint64_t)g_msc.mps_in);
    usb_log_end();

    // ★ 自动验收用的固定打点（class=08 / sub=06 / proto=50 = BOT，任务书里指定的格式）
    usb_log_begin();
    dbg64_str("[USBST] iface found class=08 sub=06 proto=50 ep_in=");
    usb_hex(msc_in, 2);
    dbg64_str(" ep_out=");
    usb_hex(msc_out, 2);
    usb_log_end();
    return 0;
}
// ==================== USB 存储：探测（INQUIRY / TUR / READ CAPACITY / READ(10)）====================
// 探测顺序照 USB MSC 的常规做法（也是任务要求）：
//   INQUIRY -> TEST UNIT READY -> （出错时 REQUEST SENSE 打点）-> READ CAPACITY(10) -> READ(10) LBA0
// 自检位（[USBST] selftest mask=，0 = 全过；**没插 U 盘时整行打 skipped**）：
//   bit0(1)  存储设备没枚举到（防御用，正常不会出现在 FAIL 里）
//   bit1(2)  INQUIRY 失败          bit2(4)  READ CAPACITY 失败
//   bit3(8)  TEST UNIT READY 失败  bit4(16) 块大小不是 512（本批只支持 512，如实拒绝）
//   bit5(32) READ(10) LBA 0 失败   bit6(64) LBA 0 读回**全 0**（可疑：通路可能读到空数据）
static void usb_msc_probe() {
    uint32_t mask = 0;
    if (!g_msc.present) {
        usb_log_begin();
        dbg64_str("[USBST] selftest skipped (no storage device)");
        usb_log_end();
        return;
    }

    // ---- 1) INQUIRY：厂商 + 型号（顺带看 RMB 位 = 可移动）----
    if (usb_msc_inquiry64() != 0) mask |= 2u;

    // ---- 2) TEST UNIT READY：没数据阶段；刚上电的介质可能要一点时间，重试 3 次 ----
    {
        uint8_t cdb[16];
        for (int i = 0; i < 16; i++) cdb[i] = 0;
        cdb[0] = 0x00;
        int r = -1;
        for (int t = 0; t < 3 && r != 0; t++) {
            r = usb_bot64(cdb, 6, false, 0, nullptr);
            if (r != 0) usb_delay_ms(60);
        }
        if (r != 0) {
            usb_msc_log_sense64("test-unit-ready");
            mask |= 8u;
        }
    }

    // ---- 3) READ CAPACITY(10)：块数 + 块大小（换算成 MB/GB 一起打点）----
    if (usb_msc_capacity64() != 0) mask |= 4u;
    if (g_msc.present && g_msc.block_size != 512u) {
        mask |= 16u;
        usb_log_begin();
        dbg64_str("[USBST] block_size=");
        dbg64_dec((uint64_t)g_msc.block_size);
        dbg64_str(" != 512 -> not exposed as a block device (this batch only reads 512-byte blocks)");
        usb_log_end();
    }

    // ---- 4) READ(10) LBA 0：真的从盘上读一块（只读，不动盘）----
    if (mask == 0) {
        if (usb_msc_read10_64(0, 1, g_msc_sec) != 0) {
            mask |= 32u;
        } else {
            bool all0 = true;
            for (uint32_t i = 0; i < 512u; i++) if (g_msc_sec[i] != 0) { all0 = false; break; }
            if (all0) mask |= 64u;
        }
    }

    // ★ supported = 探测全过：探测没过的盘**不暴露成块设备**（宁可如实说"没盘"，
    //   也不给上层一个读必失败的驱动器号 —— 那会让 FAT 挂载/浏览到处报错）。
    g_msc.supported = (mask == 0);
    g_msc.selftest_mask = mask;

    usb_log_begin();
    if (mask == 0) {
        dbg64_str("[USBST] selftest PASS mask=0");
    } else {
        dbg64_str("[USBST] selftest FAIL mask=");
        dbg64_dec((uint64_t)mask);
    }
    usb_log_end();
}

// ==================== 初始化 ====================
// 自检位（0 = 全过；没主控 / 没设备时直接返回 0 —— 那是合法降级，不算失败）：
//   bit0 内存结构没建起来   bit1 控制器没在跑（USBCMD.RS 读回 0）
//   bit2 端口数不合理       bit3 帧列表项没指向 QH
//   bit4 FRBASEADD 读回值不对（寄存器读写本身不通）
//   bit5 有键盘但没就绪     bit6 中断 TD 没建    bit7 端点号/包长不对
//   bit8(256) ★ 批次 O：U 盘枚举到了但 BOT 探测没过（[USBST] selftest mask != 0）
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
    if (g_hid_present) {                                                // 有键盘就该就绪
        if (!g_ready)       mask |= 32u;
        if (!g_irq_td)      mask |= 64u;
        if (g_ep_in == 0 || g_ep_mps != 8) mask |= 128u;                // 引导键盘：8 字节报告
    }
    if (g_msc.present && g_msc.selftest_mask) mask |= 256u;              // ★ 批次 O：U 盘探测失败
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

    // ---- 3) 内存结构（帧列表 / QH / TD 池 / 数据缓冲 / DMA 暂存页）----
    g_fl_page     = (uint8_t*)page_alloc_64();
    g_td_page     = (uint8_t*)page_alloc_64();
    g_data_page   = (uint8_t*)page_alloc_64();
    g_bounce_page = (uint8_t*)page_alloc_64();      // ★ 批次 O：认不出物理地址时的 DMA 暂存
    if (!g_fl_page || !g_td_page || !g_data_page || !g_bounce_page) {
        g_state = USB64_ST_NOT_FOUND;
        usb_log_begin();
        dbg64_str("[USB64] not found (out of pages)");
        usb_log_end();
        usb_selftest_log();
        return -1;
    }
    memset_64(g_fl_page, 0, PAGE_SIZE_64);
    memset_64(g_td_page, 0, PAGE_SIZE_64);

    g_qh       = (UhciQh*)(void*)g_td_page;                // 页首 16 字节 = QH
    g_idle_td  = td_slot(USB64_TD_IDLE);                   // 槽 0 = 永久空 TD
    g_setup_buf  = g_data_page + 0x000;                    // 8 字节
    g_ctl_buf    = g_data_page + 0x010;                    // 256 字节（描述符）
    g_report_buf = g_data_page + 0x200;                    // 8 字节
    g_null_buf   = g_data_page + 0x300;                    // 状态阶段（零长度）哨兵
    g_msc_sec    = g_data_page + 0x400;                    // 512 字节（READ(10) 的扇区缓冲）
    g_cbw_buf    = g_data_page + 0x600;                    // 64 字节（CBW）
    g_csw_buf    = g_data_page + 0x640;                    // 16 字节（CSW）
    g_msc_scratch= g_data_page + 0x680;                    // 64 字节（INQUIRY/SENSE/CAPACITY）
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

    // ---- 6) 扫**所有**根端口：每个有设备的端口做复位 + 枚举 ----
    // ★ 批次 O：从"只认第一个端口的一台设备"扩到"最多两台（1 键盘 + 1 U 盘）"。
    //   地址按端口顺序分配（1、2…）；已经拿够角色（键盘/存储各一个）后，剩下的设备只复位不枚举
    //   （避免给不用的设备分配地址，也就不会出现"地址占着但没人管"的状态）。
    int      found = 0;
    uint8_t  next_addr = 1;
    bool     any_connected = false;
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
        any_connected = true;
        bool low = false;
        const bool ok = usb_port_reset(i, &low);
        if (!ok) {
            usb_log_begin();
            dbg64_str("[USB64] no device on port ");
            dbg64_dec((uint64_t)i);
            usb_log_end();
            continue;
        }
        usb_log_begin();
        dbg64_str("[USB64] port ");
        dbg64_dec((uint64_t)i);
        dbg64_str(" connected speed=");
        dbg64_str(low ? "low" : "full");
        dbg64_str(" reset ok");
        usb_log_end();

        if (found >= USB64_MAX_DEV || next_addr > 0x7Fu) {
            usb_log_begin();
            dbg64_str("[USB64] port ");
            dbg64_dec((uint64_t)i);
            dbg64_str(" device skipped (device limit reached: this driver handles 1 keyboard + 1 storage)");
            usb_log_end();
            continue;
        }
        if (usb_enum_port(i, next_addr, low) == 0) { found++; next_addr++; }
    }
    if (found == 0) {
        g_state = any_connected ? USB64_ST_ENUM_FAILED : USB64_ST_NO_DEVICE;
        if (!any_connected) {
            // （"no device on port n" 已经在上面逐端口打过了）
        }
        usb_selftest_log();
        return any_connected ? -3 : -2;
    }
    g_devices = found;

    // ---- 7) ★ 批次 O：USB 存储（U 盘）探测：INQUIRY -> TUR -> READ CAPACITY -> READ(10) ----
    //   位置：所有设备枚举完之后（这时地址/端点/配置都已经生效）。只读，不动盘上内容。
    (void)usb64_msc_selftest64();          // 没插 U 盘时打 skipped；探测结果在这里打

    // ---- 8) 状态：有键盘就当"ready"（HID 中断端点已武装）；只插 U 盘也算 ready ----
    g_state = USB64_ST_READY;
    usb_selftest_log();
    return g_hid_present ? 0 : -1;
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

// ==================== USB 存储：对外只读接口（给 kernel/ata64.cpp 的驱动器号分派用）====================
// 只有"探测全过（supported）"的 U 盘才算一块可用的块设备 —— 如实拒绝而不是给一个读必失败的盘。
int usb64_msc_count64() { return (g_msc.present && g_msc.supported) ? 1 : 0; }

// 型号 = "厂商 + 空格 + 型号"（已去尾空格）；sectors_512 = 总扇区数（按 512B 换算）
bool usb64_msc_info64(int idx, char* model, int model_cap, uint64_t* sectors_512) {
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

// 按 512B 扇区读（ata64 的语义）；一条 READ(10) 最多 8 个扇区（4KB），多了就自动分块。
bool usb64_msc_read64(int idx, uint32_t lba, uint32_t count, void* buf) {
    if (idx != 0 || !g_msc.present || !g_msc.supported) return false;
    if (count == 0) return true;
    uint8_t* p = (uint8_t*)buf;
    for (uint32_t done = 0; done < count; ) {
        uint32_t n = count - done;
        if (n > USB64_MSC_MAX_SECTORS) n = USB64_MSC_MAX_SECTORS;
        if (usb_msc_read10_64(lba + done, (uint16_t)n, p) != 0) return false;
        done += n;
        p += n * 512u;
    }
    return true;
}

int usb64_msc_selftest64() {
    if (!g_msc.present) {
        usb_log_begin();
        dbg64_str("[USBST] selftest skipped (no storage device)");
        usb_log_end();
        return 0;                                   // 没插 U 盘 = 合法降级，不算失败
    }
    usb_msc_probe();
    return (int)g_msc.selftest_mask;
}

const char* usb64_msc_last_reason64() {
    switch (g_msc.last_reason) {
    case 0:  return "ok";
    case 1:  return "csw status";
    case 2:  return "timeout";
    case 3:  return "nak";
    case 4:  return "block-size";
    default: return "no-device";
    }
}
