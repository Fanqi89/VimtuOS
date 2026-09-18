// e1000_64.cpp - Intel 82540EM (e1000) 网卡驱动（纯轮询，不要求中断）
//
// 为什么这么写：
//   * QEMU 默认网卡就是 82540EM（-device e1000，6086:100e），用户模式网络（slirp）的
//   * QEMU 默认网卡就是 82540EM（-device e1000，id 8086:100e），用户模式网络（slirp）的
//   * 全程轮询：不开中断、不 unmask PIC —— 与 PIC/TSS/任务中断路径零耦合，也不需要
//     在 ISR 上下文里加锁；代价是收包要主动调用 e1000_recv64()（net64_poll64 干这个）。
//   * 前 4GB 恒等映射（kernel/mem64.cpp 的 phys_to_virt_64 就是恒等），所以 BAR0 物理
//     地址直接当指针用；描述符环/缓冲区来自 page_alloc_64，物理地址 == 虚拟地址。
//
// 初始化必须显式清掉的寄存器（82540EM 复位后部分寄存器不是全 0，真机/QEMU 都可能带着
// 固件或上一次运行留下的值；不清会出现"链路不工作/按错误过滤丢包"）：
//   * CTRL.RST 软复位：把设备拉回已知状态（写 1 后轮询等它自清）；
//   * IMC = 0xFFFFFFFF + 读 ICR：清中断掩码与挂起中断（轮询驱动不需要中断）；
//   * MTA 128×32bit 全 0：多播过滤表。残留的哈希项会让设备按"未配置的过滤"误收/丢包，
//     Linux e1000 驱动在 init 里同样清空整张 MTA；
//   * FCAL/FCAH/FCT/FCTTV = 0：流控（PAUSE 帧地址/类型/发送定时器）。不清的话残留值会
//     让设备按 IEEE 默认 PAUSE 地址生成/响应流控帧，干扰我们"不做流控"的简单模型；
//   * RCTL/TCTL 先写 0 再写配置：保证接收/发送配置只含我们写下的位。
//   （VLAN 滤波只在 CTL.VME=1 时生效，我们不开，所以不碰 VFTA/VET。）
//
// 串口日志格式（验收按行 grep，改前先想清楚）：
//   [E1000] pci <bus>:<dev>.<fn> id=8086:<dev> bar0=0x<hex> mmio=ok
//   [E1000] mac=aa:bb:cc:dd:ee:ff link=up
//   [E1000] init ok tx_desc=32 rx_desc=32
//   [E1000] not found                                  （没网卡：优雅降级，不崩）
//   [E1000] selftest PASS / FAIL mask=<n> / skipped
#include "e1000_64.h"
#include "debug64.h"
#include "mem_64.h"
#include "port.h"
#include <stdint.h>

// ==================== 82540EM 寄存器（只列用到的）====================
enum E1000Reg : uint32_t {
    E1000_REG_CTRL   = 0x0000,   // Device Control
    E1000_REG_STATUS = 0x0008,   // Device Status（只读）
    E1000_REG_EERD   = 0x0014,   // EEPROM Read
    E1000_REG_FCAL   = 0x0028,   // Flow Control Address Low
    E1000_REG_FCAH   = 0x002C,   // Flow Control Address High
    E1000_REG_FCT    = 0x0030,   // Flow Control Type
    E1000_REG_ICR    = 0x00C0,   // Interrupt Cause Read（读清）
    E1000_REG_IMS    = 0x00D0,   // Interrupt Mask Set（自检改读它）
    E1000_REG_IMC    = 0x00D8,   // Interrupt Mask Clear
    E1000_REG_RCTL   = 0x0100,   // Receive Control
    E1000_REG_FCTTV  = 0x0170,   // Flow Control Transmit Timer Value
    E1000_REG_TCTL   = 0x0400,   // Transmit Control
    E1000_REG_TIPG   = 0x0410,   // Transmit Inter Packet Gap
    E1000_REG_RDBAL  = 0x2800,   // RX Descriptor Base Low
    E1000_REG_RDBAH  = 0x2804,   // RX Descriptor Base High
    E1000_REG_RDLEN  = 0x2808,   // RX Descriptor Length（字节）
    E1000_REG_RDH    = 0x2810,   // RX Descriptor Head（硬件）
    E1000_REG_RDT    = 0x2818,   // RX Descriptor Tail（软件）
    E1000_REG_TDBAL  = 0x3800,   // TX Descriptor Base Low
    E1000_REG_TDBAH  = 0x3804,
    E1000_REG_TDLEN  = 0x3808,
    E1000_REG_TDH    = 0x3810,   // TX Descriptor Head（硬件）
    E1000_REG_TDT    = 0x3818,   // TX Descriptor Tail（软件）
    E1000_REG_MTA    = 0x5200,   // Multicast Table Array（128×32bit，必须清零）
    E1000_REG_RAL    = 0x5400,   // Receive Address Low  (RAR0)
    E1000_REG_RAH    = 0x5404,   // Receive Address High (RAR0)
};

// CTRL/STATUS 位
static const uint32_t E1000_CTRL_RST  = 1u << 26;   // 软复位（自清）
static const uint32_t E1000_CTRL_SLU  = 1u << 6;    // Set Link Up
static const uint32_t E1000_STATUS_LU = 1u << 1;    // Link Up

// RCTL 位：EN=接收使能；UPE/MPE=单播/多播混杂（QEMU 用户网络下按需打开）；BAM=广播接收；
// SECRC=硬件剥掉 CRC（我们按"不含 FCS"处理）；BSIZE=0 -> 每个缓冲 2048 字节。
static const uint32_t E1000_RCTL_EN    = 1u << 1;
static const uint32_t E1000_RCTL_UPE   = 1u << 3;
static const uint32_t E1000_RCTL_MPE   = 1u << 4;
static const uint32_t E1000_RCTL_BAM   = 1u << 15;
static const uint32_t E1000_RCTL_SECRC = 1u << 26;

// TCTL 位：EN=发送使能；PSP=短帧补零；CT/COLD=冲突阈值/距离（照 82540EM 推荐值）。
static const uint32_t E1000_TCTL_EN   = 1u << 1;
static const uint32_t E1000_TCTL_PSP  = 1u << 3;
static const uint32_t E1000_TCTL_CT   = 0x0Fu << 4;
static const uint32_t E1000_TCTL_COLD = 0x40u << 12;

// TX 描述符命令/状态位（legacy 16 字节描述符）
static const uint8_t E1000_TXD_CMD_EOP  = 0x01;    // End Of Packet
static const uint8_t E1000_TXD_CMD_IFCS = 0x02;    // Insert FCS/CRC
static const uint8_t E1000_TXD_CMD_RS   = 0x08;    // Report Status（写完置 DD）
static const uint8_t E1000_DESC_DD      = 0x01;    // Descriptor Done
static const uint8_t E1000_DESC_EOP     = 0x02;    // RX：帧结束

static const int E1000_RX_DESC_N = 32;             // RX 环：32 × 16B 描述符
static const int E1000_TX_DESC_N = 32;             // TX 环：32 × 16B 描述符
static const int E1000_RX_BUF_SZ = 2048;           // RCTL.BSIZE=2048
static const int E1000_DESC_BYTES = 16;            // ★ legacy 描述符是 16 字节，必须按 16 对齐

// 支持的 device id：QEMU 默认 e1000 = 0x100E；其余是同族/变体（本驱动只用通用寄存器）。
static bool e1000_id_supported(uint16_t dev) {
    switch (dev) {
        case 0x100E:   // 82540EM（QEMU -device e1000）
        case 0x1004:   // 82543GC
        case 0x10D3:   // 82574L（e1000e 风格，寄存器兼容子集）
        case 0x100F:   // 82545EM
        case 0x10D5:   // 82571
        case 0x15A0:   // I218-LM
            return true;
        default:
            return false;
    }
}

// ==================== legacy 描述符（16 字节，packed）====================
struct __attribute__((packed)) E1000RxDesc {
    uint64_t addr;      // 缓冲物理地址
    uint16_t length;    // 收到的帧长（硬件写）
    uint16_t csum;      // 校验和（不用）
    uint8_t  status;    // DD/EOP（硬件写）
    uint8_t  errors;
    uint16_t special;
};
struct __attribute__((packed)) E1000TxDesc {
    uint64_t addr;      // 缓冲物理地址
    uint16_t length;
    uint8_t  cso;       // 校验和偏移（0）
    uint8_t  cmd;       // EOP|IFCS|RS
    uint8_t  status;    // DD（硬件写）
    uint8_t  css;
    uint16_t special;
};
static_assert(sizeof(E1000RxDesc) == 16, "e1000 RX descriptor must be 16 bytes");
static_assert(sizeof(E1000TxDesc) == 16, "e1000 TX descriptor must be 16 bytes");

// ==================== 驱动状态 ====================
static bool g_found = false;                 // PCI 上找到了目标设备
static bool g_ok = false;                    // 初始化完成、可收可发
static uint8_t g_bus = 0, g_dev = 0, g_fn = 0;
static uint16_t g_dev_id = 0;
static volatile uint32_t* g_mmio = nullptr;  // BAR0（恒等映射：物理地址即指针）
static uint8_t g_mac[6];
static E1000RxDesc* g_rx_ring = nullptr;
static E1000TxDesc* g_tx_ring = nullptr;
static uint8_t* g_rx_buf[E1000_RX_DESC_N];
static uint8_t* g_tx_buf = nullptr;
static int g_rx_cur = 0;                     // 下一个要检查的 RX 描述符（跟着 RDH 走）
static int g_tx_cur = 0;                     // 下一个要填的 TX 描述符（TDT = 它 +1）
static uint64_t g_tx_count = 0;
static uint64_t g_rx_count = 0;
static int g_link = 0;

// ==================== 小工具 ====================
static const char E1000_HEXL[] = "0123456789abcdef";   // 统一小写（验收 grep 用字符类）

static void e1000_hex_digits(uint64_t v, int digits) {
    for (int i = digits - 1; i >= 0; i--)
        dbg64_putc(E1000_HEXL[(v >> (uint32_t)(i * 4)) & 0xFu]);
}

static void e1000_hex_min(uint64_t v) {
    if (v == 0) { dbg64_putc('0'); return; }
    char buf[16];
    int n = 0;
    while (v != 0 && n < 16) { buf[n++] = E1000_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) dbg64_putc(buf[--n]);
}

static void e1000_put_mac(const uint8_t* m) {
    for (int i = 0; i < 6; i++) {
        if (i) dbg64_putc(':');
        e1000_hex_digits(m[i], 2);
    }
}

// 一条完整串口日志 = begin/end 包起来（防被别的上下文插行；见 debug64.h）
static void e1000_log_begin() { dbg64_line_begin64(); }
static void e1000_log_end()   { dbg64_nl(); dbg64_line_end64(); }

// ==================== PCI 配置空间（0xCF8/0xCFC）====================
// 读：只写地址端口、读数据端口（与 hwinfo64 的只读扫描同款）；
// 写：只在**本驱动自己找到的设备**上开 MEM/BUS MASTER（DMA 必需）与做软复位，
//     不碰其它设备。
static uint32_t epci_rd32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    return inl(0xCFCu);
}

static void epci_wr32(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t val) {
    const uint32_t addr = 0x80000000u
                        | ((uint32_t)bus << 16)
                        | ((uint32_t)dev << 11)
                        | ((uint32_t)fn << 8)
                        | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outl(0xCFCu, val);
}

// 扫描 0..255 号总线（连续 4 条空总线提前停，照 hwinfo64 的做法），返回是否找到。
static bool e1000_pci_find() {
    uint32_t empty_run = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        bool bus_has = false;
        for (uint32_t dev = 0; dev < 32; dev++) {
            uint32_t id = epci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x00);
            uint16_t vendor = (uint16_t)(id & 0xFFFFu);
            if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
            bus_has = true;
            // header type bit7 = 多功能设备
            const uint32_t hdr = epci_rd32((uint8_t)bus, (uint8_t)dev, 0, 0x0C);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;
            for (uint32_t fn = 0; fn < nfn; fn++) {
                if (fn != 0) {
                    id = epci_rd32((uint8_t)bus, (uint8_t)dev, (uint8_t)fn, 0x00);
                    vendor = (uint16_t)(id & 0xFFFFu);
                    if (vendor == 0xFFFFu || vendor == 0x0000u) continue;
                }
                const uint16_t device = (uint16_t)(id >> 16);
                if (vendor == 0x8086u && e1000_id_supported(device)) {
                    g_bus = (uint8_t)bus; g_dev = (uint8_t)dev; g_fn = (uint8_t)fn;
                    g_dev_id = device;
                    return true;
                }
            }
        }
        if (bus_has) {
            empty_run = 0;
        } else if (++empty_run >= 4u) {
            break;
        }
    }
    return false;
}

// ==================== MMIO 读写 ====================
static inline void ewr(uint32_t reg, uint32_t v) {
    if (g_mmio) g_mmio[reg >> 2] = v;
    __asm__ volatile("" ::: "memory");       // 防编译器把 MMIO 写挪位/合并
}
static inline uint32_t erd(uint32_t reg) {
    return g_mmio ? g_mmio[reg >> 2] : 0xFFFFFFFFu;
}

// EEPROM 读字（EERD）：bit0=START、bit4=VALID(done)、addr<<8、data=[31:16]。
// 只在 RAR0 里的 MAC 明显非法（全 0/全 FF）时作为兜底使用。
static int e1000_eeprom_read(uint8_t addr, uint16_t* out) {
    if (!g_mmio || !out) return -1;
    ewr(E1000_REG_EERD, 1u | ((uint32_t)addr << 8));
    for (uint32_t i = 0; i < 100000u; i++) {
        const uint32_t v = erd(E1000_REG_EERD);
        if (v & 0x10u) { *out = (uint16_t)(v >> 16); return 0; }
    }
    return -1;                                // 没有 EEPROM/不支持：静默失败
}

static bool e1000_mac_valid() {
    bool all0 = true, allf = true;
    for (int i = 0; i < 6; i++) {
        if (g_mac[i] != 0x00) all0 = false;
        if (g_mac[i] != 0xFF) allf = false;
    }
    return !all0 && !allf;
}

static void e1000_read_mac() {
    const uint32_t ral = erd(E1000_REG_RAL);
    const uint32_t rah = erd(E1000_REG_RAH);
    g_mac[0] = (uint8_t)(ral >> 0);
    g_mac[1] = (uint8_t)(ral >> 8);
    g_mac[2] = (uint8_t)(ral >> 16);
    g_mac[3] = (uint8_t)(ral >> 24);
    g_mac[4] = (uint8_t)(rah >> 0);
    g_mac[5] = (uint8_t)(rah >> 8);
    if (e1000_mac_valid()) return;
    // 兜底：读 EEPROM 前 3 个字。8254x 的 EEPROM word0 = MAC[0..1]、word1 = MAC[2..3]、
    // word2 = MAC[4..5]（每字内部低字节在前，与 RAL/RAH 同一字节序）。
    uint16_t w0 = 0, w1 = 0, w2 = 0;
    if (e1000_eeprom_read(0, &w0) == 0 && e1000_eeprom_read(1, &w1) == 0 &&
        e1000_eeprom_read(2, &w2) == 0) {
        g_mac[0] = (uint8_t)(w0 >> 0);
        g_mac[1] = (uint8_t)(w0 >> 8);
        g_mac[2] = (uint8_t)(w1 >> 0);
        g_mac[3] = (uint8_t)(w1 >> 8);
        g_mac[4] = (uint8_t)(w2 >> 0);
        g_mac[5] = (uint8_t)(w2 >> 8);
    }
}

// 只读等寄存器状态时的轻量退避：纯 pause（本文件也可能在未开中断的上下文被调用，
// 所以不 hlt；所有等待循环都有上界，绝不永久停住）。
static void e1000_idle64() {
    __asm__ volatile("pause");
}

// ==================== 初始化 ====================
static int e1000_setup_rings() {
    // 描述符环 + 每包一个 2KB 缓冲；page_alloc_64 给的是页对齐物理地址（恒等映射）
    g_rx_ring = (E1000RxDesc*)page_alloc_64();
    g_tx_ring = (E1000TxDesc*)page_alloc_64();
    g_tx_buf  = (uint8_t*)page_alloc_64();
    if (!g_rx_ring || !g_tx_ring || !g_tx_buf) return -1;
    memset_64(g_rx_ring, 0, (uint64_t)E1000_RX_DESC_N * sizeof(E1000RxDesc));
    memset_64(g_tx_ring, 0, (uint64_t)E1000_TX_DESC_N * sizeof(E1000TxDesc));
    memset_64(g_tx_buf, 0, (uint64_t)E1000_RX_BUF_SZ);
    for (int i = 0; i < E1000_RX_DESC_N; i++) {
        g_rx_buf[i] = (uint8_t*)page_alloc_64();
        if (!g_rx_buf[i]) return -1;
        memset_64(g_rx_buf[i], 0, (uint64_t)E1000_RX_BUF_SZ);
        g_rx_ring[i].addr = (uint64_t)(uintptr_t)g_rx_buf[i];   // 恒等映射：物理 = 虚拟
        g_rx_ring[i].status = 0;
    }
    // TX 描述符初始标成 DD（"可复用"），这样第一条 e1000_send64 不用等硬件
    for (int i = 0; i < E1000_TX_DESC_N; i++) g_tx_ring[i].status = E1000_DESC_DD;
    return 0;
}

int e1000_init64() {
    if (g_ok) return 0;

    if (!g_found && !e1000_pci_find()) {
        e1000_log_begin();
        dbg64_str("[E1000] not found");
        e1000_log_end();
        return -1;                              // 优雅降级：调用方（net64）继续走"无链路"
    }
    g_found = true;

    // BAR0：bit0=1 是 I/O BAR（本驱动不支持）；bit[2:1]=0b10 是 64 位 BAR，高 32 位在 BAR1。
    const uint32_t bar0 = epci_rd32(g_bus, g_dev, g_fn, 0x10);
    if (bar0 & 0x1u) {
        e1000_log_begin();
        dbg64_str("[E1000] bar0 is I/O BAR, unsupported");
        e1000_log_end();
        return -2;
    }
    uint64_t phys = (uint64_t)(bar0 & 0xFFFFFFF0u);          // 清低 4 位标志位
    if (((bar0 >> 1) & 0x3u) == 0x2u) {
        const uint32_t bar1 = epci_rd32(g_bus, g_dev, g_fn, 0x14);
        phys |= ((uint64_t)bar1) << 32;
    }
    if (phys == 0 || phys >= 0x100000000ull) {
        // 恒等映射只盖前 4GB；BAR 在 4GB 以上无法直接当指针用
        e1000_log_begin();
        dbg64_str("[E1000] bar0 above 4GiB (unsupported by identity map)");
        e1000_log_end();
        return -3;
    }

    // 打开 MEM SPACE（bit1）+ BUS MASTER（bit2）：DMA 必需。只写命令字低 16 位。
    const uint32_t pci_cmd = epci_rd32(g_bus, g_dev, g_fn, 0x04);
    epci_wr32(g_bus, g_dev, g_fn, 0x04, (pci_cmd & 0xFFFFu) | 0x0006u);

    g_mmio = (volatile uint32_t*)(uintptr_t)phys;

    e1000_log_begin();
    dbg64_str("[E1000] pci ");
    dbg64_dec(g_bus); dbg64_putc(':'); dbg64_dec(g_dev); dbg64_putc('.');
    dbg64_dec(g_fn);
    dbg64_str(" id=8086:");
    e1000_hex_digits(g_dev_id, 4);
    dbg64_str(" bar0=0x");
    e1000_hex_min(phys);
    dbg64_str(" mmio=ok");
    e1000_log_end();

    // 1) 软复位：写 CTRL.RST 后轮询等它自清（有界）
    ewr(E1000_REG_CTRL, E1000_CTRL_RST);
    {
        uint32_t spins = 0;
        while ((erd(E1000_REG_CTRL) & E1000_CTRL_RST) != 0 && spins < 1000000u) {
            spins++;
            e1000_idle64();
        }
    }

    // 2) 关中断 + 清挂起中断原因（轮询驱动）
    ewr(E1000_REG_IMC, 0xFFFFFFFFu);
    (void)erd(E1000_REG_ICR);

    // 3) 清多播表（128 项 × 32bit）——见文件头说明，这是 e1000 初始化最容易漏的一步
    for (uint32_t i = 0; i < 128u; i++) ewr(E1000_REG_MTA + i * 4u, 0u);

    // 4) 清流控寄存器（PAUSE 地址/类型/定时器）
    ewr(E1000_REG_FCAH, 0u);
    ewr(E1000_REG_FCAL, 0u);
    ewr(E1000_REG_FCT,  0u);
    ewr(E1000_REG_FCTTV, 0u);

    // 5) RX/TX 先关掉，保证下面配置的是完整一套
    ewr(E1000_REG_RCTL, 0u);
    ewr(E1000_REG_TCTL, 0u);

    // 6) 读 MAC（RAR0；非法时兜底读 EEPROM）
    e1000_read_mac();
    if (!e1000_mac_valid()) {
        e1000_log_begin();
        dbg64_str("[E1000] mac invalid (RAR0 + EEPROM both bad)");
        e1000_log_end();
        g_mmio = nullptr;
        return -4;
    }

    // 7) 描述符环 + 缓冲
    if (e1000_setup_rings() != 0) {
        e1000_log_begin();
        dbg64_str("[E1000] out of memory for descriptor rings");
        e1000_log_end();
        g_mmio = nullptr;
        return -5;
    }

    // 8) TX：环基址（64 位，低/高分开写）、长度、头/尾归零，再开 TCTL/TIPG
    {
        const uint64_t txp = (uint64_t)(uintptr_t)g_tx_ring;
        ewr(E1000_REG_TDBAL, (uint32_t)(txp & 0xFFFFFFFFu));
        ewr(E1000_REG_TDBAH, (uint32_t)(txp >> 32));
        ewr(E1000_REG_TDLEN, (uint32_t)(E1000_TX_DESC_N * E1000_DESC_BYTES));
        ewr(E1000_REG_TDH, 0u);
        ewr(E1000_REG_TDT, 0u);
        ewr(E1000_REG_TCTL, E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT | E1000_TCTL_COLD);
        ewr(E1000_REG_TIPG, 10u | (8u << 10) | (6u << 20));   // 82540EM 推荐值 IPGT=10/IPGR1=8/IPGR2=6
    }

    // 9) RX：环基址 + 长度；RDH=0、RDT=N-1（全环交给硬件），最后写 RCTL 使能
    {
        const uint64_t rxp = (uint64_t)(uintptr_t)g_rx_ring;
        ewr(E1000_REG_RDBAL, (uint32_t)(rxp & 0xFFFFFFFFu));
        ewr(E1000_REG_RDBAH, (uint32_t)(rxp >> 32));
        ewr(E1000_REG_RDLEN, (uint32_t)(E1000_RX_DESC_N * E1000_DESC_BYTES));
        ewr(E1000_REG_RDH, 0u);
        ewr(E1000_REG_RDT, (uint32_t)(E1000_RX_DESC_N - 1));
        g_rx_cur = 0;
        // UPE|MPE：QEMU 用户网络下按需打开混杂（单播/多播全收），BAM 收广播，SECRC 硬件剥 CRC
        ewr(E1000_REG_RCTL, E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                            E1000_RCTL_BAM | E1000_RCTL_SECRC);
    }

    // 10) 开链路：CTRL.SLU；等 STATUS.LU（有界，失败也继续 —— link=down 如实上报）
    ewr(E1000_REG_CTRL, E1000_CTRL_SLU);
    g_link = 0;
    for (uint32_t i = 0; i < 1000000u; i++) {
        if (erd(E1000_REG_STATUS) & E1000_STATUS_LU) { g_link = 1; break; }
        e1000_idle64();
    }

    g_ok = true;

    e1000_log_begin();
    dbg64_str("[E1000] mac=");
    e1000_put_mac(g_mac);
    dbg64_str(" link=");
    dbg64_str(g_link ? "up" : "down");
    e1000_log_end();

    e1000_log_begin();
    dbg64_str("[E1000] init ok tx_desc=");
    dbg64_dec((uint64_t)E1000_TX_DESC_N);
    dbg64_str(" rx_desc=");
    dbg64_dec((uint64_t)E1000_RX_DESC_N);
    e1000_log_end();
    return 0;
}

// ==================== 收发 ====================
int e1000_send64(const void* frame, int len) {
    if (!g_ok || !frame || len <= 0 || len > 1514) return -1;
    E1000TxDesc* d = &g_tx_ring[g_tx_cur];
    // 等这个描述符空出来（DD=1 表示硬件已处理完）；有界自旋，绝不永久卡住
    uint32_t spins = 0;
    while ((d->status & E1000_DESC_DD) == 0) {
        if (++spins > 2000000u) return -1;
        e1000_idle64();
    }
    memset_64(g_tx_buf, 0, (uint64_t)len);
    memcpy_64(g_tx_buf, frame, (uint64_t)len);
    d->addr   = (uint64_t)(uintptr_t)g_tx_buf;
    d->length = (uint16_t)len;
    d->cso    = 0;
    d->cmd    = (uint8_t)(E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS);
    d->status = 0;
    d->css    = 0;
    d->special = 0;
    g_tx_cur = (g_tx_cur + 1) % E1000_TX_DESC_N;
    ewr(E1000_REG_TDT, (uint32_t)g_tx_cur);    // [TDH, TDT) 是硬件要发的描述符
    g_tx_count++;
    return 0;
}

int e1000_recv64(void* buf, int max) {
    if (!g_ok) return -1;
    E1000RxDesc* d = &g_rx_ring[g_rx_cur];
    if ((d->status & E1000_DESC_DD) == 0) return 0;    // 非阻塞：没有新帧
    int got = 0;
    if (d->status & E1000_DESC_EOP) {                  // 只处理单描述符整帧（2048 足够）
        int len = (int)d->length;
        if (len < 0) len = 0;
        if (len > E1000_RX_BUF_SZ) len = E1000_RX_BUF_SZ;
        if (buf && max > 0 && len > 0) {
            got = (len < max) ? len : max;
            memcpy_64(buf, (const void*)(uintptr_t)d->addr, (uint64_t)got);
        }
    }
    d->status = 0;                                     // 归还描述符
    ewr(E1000_REG_RDT, (uint32_t)g_rx_cur);
    g_rx_cur = (g_rx_cur + 1) % E1000_RX_DESC_N;
    if (got > 0) g_rx_count++;
    return got;
}

const uint8_t* e1000_mac64() { return g_ok ? g_mac : nullptr; }
uint64_t e1000_tx64()        { return g_tx_count; }
uint64_t e1000_rx64()        { return g_rx_count; }
int e1000_link64()           { return (g_ok && g_link) ? 1 : 0; }

// ==================== 自检 ====================
int e1000_selftest64() {
    if (!g_ok) {
        e1000_log_begin();
        dbg64_str("[E1000] selftest skipped");         // 没网卡/初始化失败：skipped，不算失败
        e1000_log_end();
        return -1;
    }
    int fails = 0;

    // bit0：IMS 写读回 + IMC 清空（寄存器读写通路）
    ewr(E1000_REG_IMS, 0x1Fu);
    if ((erd(E1000_REG_IMS) & 0x1Fu) != 0x1Fu) fails |= 1;
    ewr(E1000_REG_IMC, 0xFFFFFFFFu);
    if ((erd(E1000_REG_IMS) & 0xFFFFFFFFu) != 0u) fails |= 1;
    (void)erd(E1000_REG_ICR);                          // 清挂起的中断原因

    // bit1：描述符环基址/长度回读（写进去的物理地址必须能读回）
    {
        const uint64_t rxp = ((uint64_t)erd(E1000_REG_RDBAH) << 32) | erd(E1000_REG_RDBAL);
        const uint64_t txp = ((uint64_t)erd(E1000_REG_TDBAH) << 32) | erd(E1000_REG_TDBAL);
        if (rxp != (uint64_t)(uintptr_t)g_rx_ring) fails |= 2;
        if (txp != (uint64_t)(uintptr_t)g_tx_ring) fails |= 2;
        if (erd(E1000_REG_RDLEN) != (uint32_t)(E1000_RX_DESC_N * E1000_DESC_BYTES)) fails |= 2;
        if (erd(E1000_REG_TDLEN) != (uint32_t)(E1000_TX_DESC_N * E1000_DESC_BYTES)) fails |= 2;
    }

    // bit2：MAC 合法
    if (!e1000_mac_valid()) fails |= 4;

    // bit3：RCTL 关键位（接收使能 + 混杂 + 广播 + 剥 CRC）
    {
        const uint32_t rctl = erd(E1000_REG_RCTL);
        if ((rctl & (E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
                     E1000_RCTL_BAM | E1000_RCTL_SECRC)) !=
            (E1000_RCTL_EN | E1000_RCTL_UPE | E1000_RCTL_MPE |
             E1000_RCTL_BAM | E1000_RCTL_SECRC)) fails |= 8;
    }

    e1000_log_begin();
    dbg64_str("[E1000] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
    }
    e1000_log_end();
    return fails;
}
