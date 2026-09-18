// apic64.cpp - LAPIC + IOAPIC 接管中断路由（64 位系统内核专用）
//
// ============================ 这一版做了什么 ============================
//   1) LAPIC：IA32_APIC_BASE(MSR 0x1B) 读基址 + 置 bit11（enable）；读 ID(0x20)；
//      SVR(0xF0) = 0x100|0xFF（软件使能 + spurious 向量 0xFF）；TPR(0x80)=0；
//      LVT Timer/Thermal/Perf/LINT0/LINT1/Error（0x320/0x330/0x340/0x350/0x360/0x370）
//      全部写掩码位 —— **不启用 LAPIC 定时器**，PIT 仍是唯一的 tick 源
//      （时间片口径保持不变，task64 的 schedule64 钩子照旧由 IRQ0 驱动）。
//   2) IOAPIC：基址取 acpi_ioapic_addr64(0)（MMIO 0x00 选索引 / 0x10 读写数据）；
//      版本寄存器(0x01) 位 16..23 = 最大重定向项数。先把所有项掩码，再只放开：
//        IRQ0(PIT)->向量32、IRQ1(键盘)->33、IRQ12(鼠标)->44、IRQ14(ATA)->46；
//        GSI 用 MADT 的 ISO（type 2）覆盖：有 ISO 按 ISO 的 GSI/极性/触发，
//        没有 ISO 时 IRQ==GSI 且按 ISA 默认（边沿、高有效）。SCI(IRQ9) 之类不主动碰
//        （它们和其它所有项一样保持掩码 —— 原来的 8259 路径也从没放开过它们）。
//        destination = 本 LAPIC ID，delivery mode = fixed、physical。
//   3) EOI 分派：见 kernel/x86_64.cpp 的 pic_eoi()（按 g_irq_mode64 走 LAPIC 0xB0 或 8259）。
//      切到 APIC 后把两个 8259 的 IMR 全写 1，避免同一中断被投递两次。
//
// ============================ 不许变砖的设计 ============================
//   * 前置检查阶段只读不写：ACPI/MADT、MSR、页表映射、MMIO 版本寄存器全部读通
//     才进入切换；任何一条不满足就打印
//     "[APIC] unavailable reason=<...> -> stay on 8259 PIC" 并保持 PIC。
//   * MMIO 页映射检查（apic64_pa_mapped64）：走当前 CR3 的 4 级页表，确认
//     LAPIC/IOAPIC 物理页在**当前页表**里是 present 的。BIOS 路径 loader64.asm 把前
//     4GB 恒等映射，一定过；UEFI 路径用的是固件页表，MMIO 页未必映射 —— 没过就留 PIC，
//     绝不是 #PF + PANIC（这条是专门为"不变砖"加的）。
//   * 切换全程 cli；切前保存 8259 IMR、LAPIC 寄存器、IOAPIC 重定向表原值。
//   * 在线自检（读回）失败 -> 整体回滚到切换前状态 + 退回 PIC + 打印失败掩码。
//   * SVR 的 spurious 向量 0xFF 在 IDT 里原本没有门（伪中断会 #GP -> PANIC）：
//     本模块在切换时给 IDT[0xFF] 装一个裸 iretq 门（apic64_spurious_stub64），
//     把最后一块砖风险补上（伪中断不需要 EOI —— SDM 明确说伪中断不写 EOI）。
//
// ============================ 自检位定义（0 = 通过）============================
//   APIC 模式：
//     0x01 LAPIC 基址有效（非 0 / 4KB 对齐 / ≥0xF0000000 / <4GB / 页表 present）
//     0x02 LAPIC 基址 == MADT 记录的 LAPIC 地址
//     0x04 LAPIC ID 可读且与 MADT 首个 enabled 处理器的 APIC ID 一致
//     0x08 SVR 写读回一致（0x1FF）
//     0x10 IOAPIC 版本可读、max_redir ≥ 23 且与编程时一致
//     0x20 四条路由读回位域正确（掩码=0 / 向量 / dest / 极性 / 触发）
//     0x40 其余重定向项保持掩码
//     0x80 g_irq_mode64 == APIC（EOI/掩码确实走 APIC 路径）
//   PIC 模式（降级）：只校验"模式分派自洽"（mode==PIC 且没有半接管状态）-> PASS。
#include "apic64.h"
#include "debug64.h"
#include "acpi64.h"
#include "memlayout64.h"
#include "x86_64.h"
#include <stdint.h>

// ==================== 常量 ====================
static const uint32_t MSR64_IA32_APIC_BASE = 0x1Bu;

// LAPIC MMIO 寄存器偏移
static const uint32_t LAPIC64_ID  = 0x020;   // 位 24..31 = APIC ID
static const uint32_t LAPIC64_EOI = 0x0B0;
static const uint32_t LAPIC64_SVR = 0x0F0;
static const uint32_t LAPIC64_TPR = 0x080;
static const uint32_t LAPIC64_LVT_TIMER   = 0x320;
static const uint32_t LAPIC64_LVT_THERMAL = 0x330;
static const uint32_t LAPIC64_LVT_PERF    = 0x340;
static const uint32_t LAPIC64_LVT_LINT0   = 0x350;
static const uint32_t LAPIC64_LVT_LINT1   = 0x360;
static const uint32_t LAPIC64_LVT_ERROR   = 0x370;
static const uint32_t LAPIC64_LVT_MASK    = 0x10000u;
static const uint32_t LAPIC64_SVR_ENABLE  = 0x100u;
static const uint32_t LAPIC64_SPURIOUS    = 0xFFu;

// IOAPIC MMIO
static const uint32_t IOAPIC64_SEL        = 0x00;   // 索引窗口
static const uint32_t IOAPIC64_WIN        = 0x10;   // 数据窗口
static const uint32_t IOAPIC64_REG_VER    = 0x01;
// 重定向项（每项 = 索引 0x10+2*n / 0x10+2*n+1 两个 32 位寄存器）
static const uint32_t REDIR64_MASK   = 1u << 16;    // 1 = 掩码
static const uint32_t REDIR64_LEVEL  = 1u << 15;    // 1 = level 触发（默认 edge=0）
static const uint32_t REDIR64_LOW    = 1u << 13;    // 1 = 低有效（默认高有效=0）

// 只开放我们用到的 4 条线：向量沿用 8259 重映射后的 32+n（IDT 桩不用改）
struct ApicRoute64 {
    uint8_t  irq;
    uint32_t gsi;
    uint8_t  vec;
    uint32_t flags;        // 重定向项 lo 里的极性/触发位（REDIR64_LEVEL/REDIR64_LOW）
    bool     programmed;
};
static const int ROUTES64_N = 4;
static ApicRoute64 g_routes[ROUTES64_N] = {
    {  0,  0, 32, 0, false },   // PIT        -> 向量 32
    {  1,  1, 33, 0, false },   // 键盘       -> 向量 33
    { 12, 12, 44, 0, false },   // PS/2 鼠标  -> 向量 44
    { 14, 14, 46, 0, false },   // IDE 主通道 -> 向量 46
};

// MADT 解析结果（只读）
static const int MADT64_MAX_ISO = 16;
struct MadtIso64 { uint8_t bus; uint8_t source; uint32_t gsi; uint16_t flags; };
static bool       g_madt_ok = false;
static uint32_t   g_madt_lapic_addr = 0;
static bool       g_madt_bsp_ok = false;
static uint8_t    g_madt_bsp_id = 0;
static MadtIso64  g_isos[MADT64_MAX_ISO];
static int        g_iso_n = 0;

// 切换后的状态
static bool     g_apic_ready   = false;
static uint64_t g_lapic_base   = 0;     // 物理地址（前 4GB 恒等映射，可直接当指针）
static uint32_t g_lapic_id     = 0;
static uint64_t g_ioapic_base  = 0;
static uint32_t g_ioapic_gsi0  = 0;
static uint32_t g_ioapic_max   = 0;     // 最大重定向项索引（版本寄存器给出）

// 切换前保存（回滚用）
static uint64_t g_saved_msr     = 0;
static uint8_t  g_saved_imr_m   = 0xFF;
static uint8_t  g_saved_imr_s   = 0xFF;
static uint32_t g_saved_svr     = 0;
static uint32_t g_saved_tpr     = 0;
static uint32_t g_saved_lvt[6]  = {0};
static uint32_t g_saved_redir[2 * 256];     // lo/hi 交错；最多 256 项（8 位字段）

// ==================== 低层访问 ====================
static inline uint64_t rdmsr64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static inline void wrmsr64(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint32_t mmio_r32(uint64_t pa) {
    return *(const volatile uint32_t*)(uintptr_t)pa;
}
static inline void mmio_w32(uint64_t pa, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)pa = v;
}

static inline uint32_t lapic_r32(uint32_t off)      { return mmio_r32(g_lapic_base + off); }
static inline void     lapic_w32(uint32_t off, uint32_t v) {
    mmio_w32(g_lapic_base + off, v);
    __asm__ volatile("" ::: "memory");          // MMIO 写后再读（自检读回）要有序
}
static inline uint32_t lapic_id_read()              { return (lapic_r32(LAPIC64_ID) >> 24) & 0xFFu; }

static inline uint32_t ioapic_r32(uint32_t reg) {
    mmio_w32(g_ioapic_base + IOAPIC64_SEL, reg);
    return mmio_r32(g_ioapic_base + IOAPIC64_WIN);
}
static inline void ioapic_w32(uint32_t reg, uint32_t v) {
    mmio_w32(g_ioapic_base + IOAPIC64_SEL, reg);
    mmio_w32(g_ioapic_base + IOAPIC64_WIN, v);
}
// 重定向项 lo/hi 寄存器索引（项 n 从 0x10 + 2*n 开始）
static inline uint32_t redir_lo_read(uint32_t n)        { return ioapic_r32(0x10u + 2u * n); }
static inline uint32_t redir_hi_read(uint32_t n)        { return ioapic_r32(0x11u + 2u * n); }
static inline void redir_write(uint32_t n, uint32_t lo, uint32_t hi) {
    ioapic_w32(0x10u + 2u * n, lo);
    ioapic_w32(0x11u + 2u * n, hi);
}

// SVR 的 spurious 向量（0xFF）在 IDT 里没有门 -> 伪中断会 #GP/PANIC。
// 这里给 IDT[0xFF] 装一个"裸 iretq"门：伪中断没有错误码、也没有我们的帧，
// 直接 iretq 回被打断处即可（SDM：伪中断不需要写 EOI，写了反而可能触发 level 重投递）。
extern "C" __attribute__((naked, used)) void apic64_spurious_stub64() {
    __asm__ volatile("iretq");
}

// ==================== 页表映射检查（防 #PF 变砖）====================
// 走当前 CR3 的 4 级页表，判断物理地址 pa 作为**同一地址的 VA** 是否 present。
// 只读页表（页表本身总在已被恒等映射的低端 RAM 里）。返回 false 时调用方必须放弃 APIC。
static bool apic64_pa_mapped64(uint64_t pa) {
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    if (cr3 == 0) return false;
    const uint64_t pml4 = cr3 & 0x000FFFFFFFFFF000ull;

    const uint32_t i4 = (uint32_t)((pa >> 39) & 0x1FFu);
    const uint32_t i3 = (uint32_t)((pa >> 30) & 0x1FFu);
    const uint32_t i2 = (uint32_t)((pa >> 21) & 0x1FFu);
    const uint32_t i1 = (uint32_t)((pa >> 12) & 0x1FFu);

    const uint64_t e4 = *(const volatile uint64_t*)(uintptr_t)(pml4 + (uint64_t)i4 * 8u);
    if (!(e4 & 1u)) return false;
    const uint64_t pdpt = e4 & 0x000FFFFFFFFFF000ull;

    const uint64_t e3 = *(const volatile uint64_t*)(uintptr_t)(pdpt + (uint64_t)i3 * 8u);
    if (!(e3 & 1u)) return false;
    if (e3 & 0x80u) return true;                       // 1GB 大页
    const uint64_t pd = e3 & 0x000FFFFFFFFFF000ull;

    const uint64_t e2 = *(const volatile uint64_t*)(uintptr_t)(pd + (uint64_t)i2 * 8u);
    if (!(e2 & 1u)) return false;
    if (e2 & 0x80u) return true;                       // 2MB 大页
    const uint64_t pt = e2 & 0x000FFFFFFFFFF000ull;

    const uint64_t e1 = *(const volatile uint64_t*)(uintptr_t)(pt + (uint64_t)i1 * 8u);
    return (e1 & 1u) != 0;
}

// ==================== MADT 里找 ISO（IRQ -> GSI 覆盖）====================
static bool apic64_pa_ok64(uint64_t pa, uint64_t bytes) {
    return pa != 0 && pa < ML64_IDENTITY_BYTES && bytes <= ML64_IDENTITY_BYTES - pa;
}

static uint32_t apic64_u32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t apic64_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
    return v;
}
static bool apic64_sig_eq(const uint8_t* p, const char* s) {
    for (int i = 0; i < 4; i++) if ((char)p[i] != s[i]) return false;
    return true;
}

// 解析一张 MADT：只取 LAPIC 地址（含 type 5 覆盖）/ 首个 enabled 处理器 ID / ISO 表
static bool apic64_parse_madt(const uint8_t* m, uint32_t len) {
    if (len < 0x2Cu) return false;
    uint32_t lapic = apic64_u32(m + 0x24);
    g_iso_n = 0;
    g_madt_bsp_ok = false;
    g_madt_bsp_id = 0;

    uint32_t off = 0x2Cu;
    while (off + 2u <= len) {
        const uint8_t* e = m + off;
        const uint8_t et = e[0];
        const uint8_t el = e[1];
        if (el < 2u || off + (uint32_t)el > len) break;      // 条目越界：停止扫描
        if (et == 0u && el >= 8u) {                          // 处理器本地 APIC
            if (!g_madt_bsp_ok && (apic64_u32(e + 4) & 1u)) {
                g_madt_bsp_id = e[3];
                g_madt_bsp_ok = true;
            }
        } else if (et == 2u && el >= 10u) {                  // ISO：IRQ -> GSI 覆盖
            if (g_iso_n < MADT64_MAX_ISO) {
                MadtIso64& iso = g_isos[g_iso_n++];
                iso.bus    = e[2];
                iso.source = e[3];
                iso.gsi    = apic64_u32(e + 4);
                iso.flags  = (uint16_t)((uint16_t)e[8] | ((uint16_t)e[9] << 8));
            }
        } else if (et == 5u && el >= 12u) {                  // LAPIC 地址覆盖（64 位）
            const uint64_t a = apic64_u64(e + 4);
            if (a != 0 && a < ML64_IDENTITY_BYTES) lapic = (uint32_t)a;
        }
        off += el;
    }
    g_madt_lapic_addr = lapic;
    g_madt_ok = true;
    return true;
}

// 从 RSDP 重新走到 MADT（acpi64.cpp 不暴露 MADT 地址；这里只读、只为了拿 ISO）
static bool apic64_find_madt() {
    const uint64_t rsdp = acpi_rsdp_addr64();
    if (!apic64_pa_ok64(rsdp, 36u)) return false;
    const uint8_t* r = (const uint8_t*)(uintptr_t)rsdp;
    const uint8_t  revision = r[0x0F];
    const uint64_t rsdt = apic64_u32(r + 0x10);
    const uint64_t xsdt = (revision >= 2) ? apic64_u64(r + 0x18) : 0;

    uint64_t root = (revision >= 2 && xsdt != 0) ? xsdt : rsdt;
    const bool use_xsdt = (revision >= 2 && xsdt != 0);
    if (!apic64_pa_ok64(root, 36u)) return false;
    const uint8_t* rh = (const uint8_t*)(uintptr_t)root;
    if (!apic64_sig_eq(rh, use_xsdt ? "XSDT" : "RSDT")) return false;
    const uint32_t rlen = apic64_u32(rh + 4);
    if (rlen < 36u || !apic64_pa_ok64(root, rlen)) return false;

    const uint32_t step = use_xsdt ? 8u : 4u;
    const uint32_t n = (rlen - 36u) / step;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* p = rh + 36u + (uint64_t)i * step;
        const uint64_t tp = use_xsdt ? apic64_u64(p) : (uint64_t)apic64_u32(p);
        if (!apic64_pa_ok64(tp, 36u)) continue;
        const uint8_t* th = (const uint8_t*)(uintptr_t)tp;
        if (!apic64_sig_eq(th, "APIC")) continue;
        const uint32_t len = apic64_u32(th + 4);
        if (len < 0x2Cu || !apic64_pa_ok64(tp, len)) return false;
        return apic64_parse_madt(th, len);
    }
    return false;
}

// ISO 查表：返回 IRQ 的 GSI，并把极性/触发翻译成重定向项 lo 的位（ISA 默认 边沿/高有效）
static uint32_t apic64_iso_lookup(uint8_t irq, uint32_t* flags_out) {
    *flags_out = 0;                       // 0 = 边沿 + 高有效 = ISA 默认
    for (int i = 0; i < g_iso_n; i++) {
        if (g_isos[i].bus != 0u || g_isos[i].source != irq) continue;
        const uint16_t f = g_isos[i].flags;
        const uint16_t pol = (uint16_t)(f & 0x3u);        // 0=总线默认 1=高有效 3=低有效
        const uint16_t trg = (uint16_t)((f >> 2) & 0x3u); // 0=总线默认 1=边沿 3=level
        if (pol == 3u) *flags_out |= REDIR64_LOW;
        if (trg == 3u) *flags_out |= REDIR64_LEVEL;
        return g_isos[i].gsi;
    }
    return irq;                           // 没有 ISO：IRQ == GSI
}

// ==================== 打印 ====================
static void apic64_phex(uint64_t v) { dbg64_str("0x"); dbg64_hex64(v); }

static void apic64_unavailable(const char* reason) {
    dbg64_line_begin64();
    dbg64_str("[APIC] unavailable reason=");
    dbg64_str(reason);
    dbg64_str(" -> stay on 8259 PIC");
    dbg64_nl();
    dbg64_line_end64();
}

static void apic64_print_routes() {
    for (int i = 0; i < ROUTES64_N; i++) {
        if (!g_routes[i].programmed) continue;
        dbg64_line_begin64();
        dbg64_str("[APIC] route irq");
        dbg64_dec(g_routes[i].irq);
        dbg64_str(" -> gsi=");
        dbg64_dec(g_routes[i].gsi);
        dbg64_str(" vec=");
        dbg64_dec(g_routes[i].vec);
        dbg64_str(" dest=");
        dbg64_dec(g_lapic_id);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== 切换 ====================
// 保存现场（cli 内调用）
static void apic64_save_state(uint64_t msr) {
    g_saved_msr   = msr;
    g_saved_imr_m = inb64(0x21);
    g_saved_imr_s = inb64(0xA1);
    g_saved_svr   = lapic_r32(LAPIC64_SVR);
    g_saved_tpr   = lapic_r32(LAPIC64_TPR);
    g_saved_lvt[0] = lapic_r32(LAPIC64_LVT_TIMER);
    g_saved_lvt[1] = lapic_r32(LAPIC64_LVT_THERMAL);
    g_saved_lvt[2] = lapic_r32(LAPIC64_LVT_PERF);
    g_saved_lvt[3] = lapic_r32(LAPIC64_LVT_LINT0);
    g_saved_lvt[4] = lapic_r32(LAPIC64_LVT_LINT1);
    g_saved_lvt[5] = lapic_r32(LAPIC64_LVT_ERROR);
    for (uint32_t n = 0; n <= g_ioapic_max && n < 256u; n++) {
        g_saved_redir[2u * n]      = redir_lo_read(n);
        g_saved_redir[2u * n + 1u] = redir_hi_read(n);
    }
}

// 整体回滚到切换前（cli 内调用）
static void apic64_rollback_locked() {
    const uint32_t dest_hi = (uint32_t)g_lapic_id << 24;
    // 1) 先断开新路径：所有重定向项掩码
    for (uint32_t n = 0; n <= g_ioapic_max && n < 256u; n++) {
        redir_write(n, REDIR64_MASK, dest_hi);
    }
    // 2) 恢复 IOAPIC 原值
    for (uint32_t n = 0; n <= g_ioapic_max && n < 256u; n++) {
        redir_write(n, g_saved_redir[2u * n], g_saved_redir[2u * n + 1u]);
    }
    // 3) 恢复 LAPIC 寄存器 + MSR（原样写回，enable 位恢复成原来的值）
    lapic_w32(LAPIC64_SVR, g_saved_svr);
    lapic_w32(LAPIC64_TPR, g_saved_tpr);
    lapic_w32(LAPIC64_LVT_TIMER,   g_saved_lvt[0]);
    lapic_w32(LAPIC64_LVT_THERMAL, g_saved_lvt[1]);
    lapic_w32(LAPIC64_LVT_PERF,    g_saved_lvt[2]);
    lapic_w32(LAPIC64_LVT_LINT0,   g_saved_lvt[3]);
    lapic_w32(LAPIC64_LVT_LINT1,   g_saved_lvt[4]);
    lapic_w32(LAPIC64_LVT_ERROR,   g_saved_lvt[5]);
    wrmsr64(MSR64_IA32_APIC_BASE, g_saved_msr);
    // 4) 恢复 8259 IMR（旧路径复活；此时中断仍全关，不会有半切换窗口）
    outb64(0x21, g_saved_imr_m);
    outb64(0xA1, g_saved_imr_s);
    // 5) 回到 PIC 模式
    g_irq_mode64 = IRQ_MODE64_PIC;
    g_apic_ready = false;
}

// 把某条路由按当前 g_routes[] 写回（masked=仍掩码）
static void apic64_write_route(const ApicRoute64& r, bool masked) {
    const uint32_t n = r.gsi - g_ioapic_gsi0;
    const uint32_t dest_hi = (uint32_t)g_lapic_id << 24;   // 物理模式，dest = 本 LAPIC ID
    uint32_t lo = (uint32_t)r.vec | r.flags;
    if (masked) lo |= REDIR64_MASK;
    redir_write(n, lo, dest_hi);
}

// ==================== 初始化 ====================
// 降级路径的统一出口：unavailable 行 + 模式行 + 自检（PIC 模式的自检只验"分派自洽"）。
// 任何前置检查不过都走这里 —— 串口上始终能看到模式结论和 selftest 结果。
static bool apic64_fallback_pic(const char* reason) {
    apic64_unavailable(reason);
    dbg64_line_begin64();
    dbg64_str("[APIC] irq mode = pic");
    dbg64_nl();
    dbg64_line_end64();
    (void)apic64_selftest64();
    return false;
}

bool apic64_init64() {
    if (g_irq_mode64 == IRQ_MODE64_APIC) return true;      // 幂等：已经接管

    // ---- 阶段 1：只读探测（这一步绝不写任何硬件寄存器）----
    if (acpi_rsdp_addr64() == 0) return apic64_fallback_pic("no-acpi");
    if (!apic64_find_madt())     return apic64_fallback_pic("no-madt");

    const uint64_t msr = rdmsr64(MSR64_IA32_APIC_BASE);
    const uint64_t base = msr & 0xFFFFF000ull;
    if (base == 0) return apic64_fallback_pic("lapic-base-zero");
    if (base < 0xF0000000ull || base >= ML64_IDENTITY_BYTES) {
        return apic64_fallback_pic("lapic-base-invalid");
    }
    // MADT 的 LAPIC 地址必须和 MSR 基址一致（不一致说明我们对这张表/这台机器的
    // 理解有偏差 —— 宁可留在 PIC，也不拿一个没把握的地址去写 MMIO）
    if (g_madt_lapic_addr == 0 ||
        ((uint64_t)g_madt_lapic_addr & 0xFFFFF000ull) != base) {
        return apic64_fallback_pic("lapic-madt-mismatch");
    }
    // MMIO 页必须在当前页表里 present（UEFI 固件页表可能没映射：直接读会 #PF）
    if (!apic64_pa_mapped64(base) || !apic64_pa_mapped64(base + 0x3F0u)) {
        return apic64_fallback_pic("lapic-unmapped");
    }

    if (acpi_ioapic_count64() < 1) return apic64_fallback_pic("no-ioapic");
    const uint64_t iob = acpi_ioapic_addr64(0);
    if (iob == 0 || (iob & 0xFFFull) != 0) return apic64_fallback_pic("ioapic-addr-invalid");
    if (iob < 0xF0000000ull || iob >= ML64_IDENTITY_BYTES) {
        return apic64_fallback_pic("ioapic-addr-invalid");
    }
    if (!apic64_pa_mapped64(iob) || !apic64_pa_mapped64(iob + IOAPIC64_WIN)) {
        return apic64_fallback_pic("ioapic-unmapped");
    }

    // 这些只读访问本身已经验证了映射；版本寄存器再确认 max_redir
    g_lapic_base  = base;
    g_ioapic_base = iob;
    g_ioapic_gsi0 = acpi_ioapic_gsi_base64(0);
    const uint32_t ver = ioapic_r32(IOAPIC64_REG_VER);
    const uint32_t max_redir = (ver >> 16) & 0xFFu;
    if (max_redir < 23u) return apic64_fallback_pic("ioapic-maxredir-low");

    const uint32_t id = lapic_id_read();
    if (id > 0xFFu) return apic64_fallback_pic("lapic-id-unreadable");
    g_lapic_id    = id;
    g_ioapic_max  = (max_redir > 255u) ? 255u : max_redir;

    // 算 GSI / 极性 / 触发（有 ISO 按 ISO，没有按 ISA 默认）
    for (int i = 0; i < ROUTES64_N; i++) {
        uint32_t f = 0;
        g_routes[i].gsi   = apic64_iso_lookup(g_routes[i].irq, &f);
        g_routes[i].flags = f;
        g_routes[i].programmed = false;
        if (g_routes[i].gsi < g_ioapic_gsi0 ||
            g_routes[i].gsi - g_ioapic_gsi0 > g_ioapic_max) {
            return apic64_fallback_pic("route-gsi-out-of-range");
        }
    }

    // ---- 阶段 2：切换（全程 cli；先掩码 IOAPIC 项并屏蔽 8259，再放行路由）----
    const uint64_t fl = dbg64_irq_save64();

    apic64_save_state(msr);

    // 2a) 所有重定向项掩码（保留原向量/dest 无所谓，反正后面要恢复/重编）
    for (uint32_t n = 0; n <= g_ioapic_max && n < 256u; n++) {
        redir_write(n, REDIR64_MASK, (uint32_t)g_lapic_id << 24);
    }
    // 2b) 四条路由先按"掩码"编好（armed but muted）
    for (int i = 0; i < ROUTES64_N; i++) apic64_write_route(g_routes[i], true);
    // 2c) 屏蔽两个 8259（从这一刻起 8259 不再投递，避免同一中断双投递）
    outb64(0x21, 0xFF);
    outb64(0xA1, 0xFF);
    // 2d) 启用 LAPIC + SVR/TPR/LVT
    wrmsr64(MSR64_IA32_APIC_BASE, base | (1ull << 11));    // bit11 = global enable
    lapic_w32(LAPIC64_SVR, LAPIC64_SVR_ENABLE | LAPIC64_SPURIOUS);
    lapic_w32(LAPIC64_TPR, 0);
    lapic_w32(LAPIC64_LVT_TIMER,   LAPIC64_LVT_MASK);      // 不启用 LAPIC 定时器
    lapic_w32(LAPIC64_LVT_THERMAL, LAPIC64_LVT_MASK);
    lapic_w32(LAPIC64_LVT_PERF,    LAPIC64_LVT_MASK);
    lapic_w32(LAPIC64_LVT_LINT0,   LAPIC64_LVT_MASK);
    lapic_w32(LAPIC64_LVT_LINT1,   LAPIC64_LVT_MASK);
    lapic_w32(LAPIC64_LVT_ERROR,   LAPIC64_LVT_MASK);
    // 2e) 伪中断门（0xFF）——补上 IDT 里唯一会因 APIC 出现的空洞
    idt_set_gate64(0xFF, (uint64_t)(uintptr_t)apic64_spurious_stub64, SEL64_KCODE, 0x8E, 0);
    // 2f) 模式切换 + 放行四条路由
    g_irq_mode64 = IRQ_MODE64_APIC;
    g_apic_ready = true;
    for (int i = 0; i < ROUTES64_N; i++) {
        apic64_write_route(g_routes[i], false);
        g_routes[i].programmed = true;
    }

    dbg64_irq_restore64(fl);

    // ---- 阶段 3：打点（严格格式，自动验收 grep 这些行）----
    dbg64_line_begin64();
    dbg64_str("[APIC] lapic base=");
    apic64_phex(g_lapic_base);
    dbg64_str(" id=");
    dbg64_dec(g_lapic_id);
    dbg64_str(" svr=");
    apic64_phex(LAPIC64_SVR_ENABLE | LAPIC64_SPURIOUS);
    dbg64_str(" enabled=1");
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[APIC] ioapic #0 addr=");
    apic64_phex(g_ioapic_base);
    dbg64_str(" gsi_base=");
    dbg64_dec(g_ioapic_gsi0);
    dbg64_str(" max_redir=");
    dbg64_dec(g_ioapic_max);
    dbg64_nl();
    dbg64_line_end64();

    apic64_print_routes();

    dbg64_line_begin64();
    dbg64_str("[APIC] pic masked all (8259 disabled)");
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[APIC] irq mode = apic");
    dbg64_nl();
    dbg64_line_end64();

    // ---- 阶段 4：在线自检；失败就整体回滚到 PIC（绝不带病运行）----
    const int fails = apic64_selftest64();
    if (fails != 0) {
        const uint64_t fl2 = dbg64_irq_save64();
        apic64_rollback_locked();
        dbg64_irq_restore64(fl2);
        apic64_unavailable("selftest-fail");
        dbg64_line_begin64();
        dbg64_str("[APIC] irq mode = pic");
        dbg64_nl();
        dbg64_line_end64();
        return false;
    }
    return true;
}

// ==================== EOI / 掩码路径（x86_64.cpp 调用）====================
extern "C" void apic64_eoi64() {
    if (g_lapic_base == 0) return;
    lapic_w32(LAPIC64_EOI, 0);          // 写 0 即 EOI（写别的值行为未定义）
}

extern "C" void apic64_unmask_irq64(uint8_t irq) {
    if (g_irq_mode64 != IRQ_MODE64_APIC || !g_apic_ready) return;
    for (int i = 0; i < ROUTES64_N; i++) {
        if (g_routes[i].irq != irq) continue;
        apic64_write_route(g_routes[i], false);      // 幂等：本来就是放开的
        return;
    }
    // 本内核只用 IRQ0/1/12/14；其它 IRQ 不接（保持掩码）。只提示一次，不改表。
    static uint16_t warn_mask = 0;
    if (irq < 16u && !(warn_mask & (uint16_t)(1u << irq))) {
        warn_mask |= (uint16_t)(1u << irq);
        dbg64_line_begin64();
        dbg64_str("[APIC] unmask irq");
        dbg64_dec(irq);
        dbg64_str(" not routed -> ignored (stays masked)");
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== 自检 ====================
int apic64_selftest64() {
    int fails = 0;

    if (g_irq_mode64 != IRQ_MODE64_APIC) {
        // ---- PIC 模式（降级）：只校验"分派自洽"，不假装 APIC 已启用 ----
        if (g_irq_mode64 != IRQ_MODE64_PIC || g_apic_ready) fails |= 0x80;
    } else {
        // bit0：LAPIC 基址有效（与 MSR/对齐/范围一致）
        const uint64_t msr = rdmsr64(MSR64_IA32_APIC_BASE);
        const uint64_t base = msr & 0xFFFFF000ull;
        if (base == 0 || base != g_lapic_base || (base & 0xFFFull) != 0 ||
            base < 0xF0000000ull || base >= ML64_IDENTITY_BYTES) {
            fails |= 0x01;
        }
        // bit1：与 MADT 记录的 LAPIC 地址一致
        if (!g_madt_ok || ((uint64_t)g_madt_lapic_addr & 0xFFFFF000ull) != g_lapic_base) {
            fails |= 0x02;
        }
        // bit2：LAPIC ID 可读且与 MADT 首个 enabled 处理器的 APIC ID 一致
        const uint32_t id_now = lapic_id_read();
        if (id_now != g_lapic_id || id_now > 0xFFu) fails |= 0x04;
        if (g_madt_bsp_ok && (uint32_t)g_madt_bsp_id != g_lapic_id) fails |= 0x04;
        // bit3：SVR 写读回一致
        if (lapic_r32(LAPIC64_SVR) != (LAPIC64_SVR_ENABLE | LAPIC64_SPURIOUS)) fails |= 0x08;
        // bit4：IOAPIC 版本/max_redir 合理且与编程时一致
        const uint32_t ver = ioapic_r32(IOAPIC64_REG_VER);
        const uint32_t max_redir = (ver >> 16) & 0xFFu;
        if (max_redir < 23u || max_redir > 255u || max_redir != g_ioapic_max) fails |= 0x10;
        // bit5：四条路由读回位域正确（未掩码 / 向量 / dest / 极性 / 触发）
        for (int i = 0; i < ROUTES64_N; i++) {
            if (!g_routes[i].programmed) { fails |= 0x20; continue; }
            const uint32_t n = g_routes[i].gsi - g_ioapic_gsi0;
            const uint32_t lo = redir_lo_read(n);
            const uint32_t hi = redir_hi_read(n);
            if ((lo & 0xFFu) != (uint32_t)g_routes[i].vec) fails |= 0x20;
            if (lo & REDIR64_MASK) fails |= 0x20;
            if ((lo & REDIR64_LEVEL) != (g_routes[i].flags & REDIR64_LEVEL)) fails |= 0x20;
            if ((lo & REDIR64_LOW)   != (g_routes[i].flags & REDIR64_LOW))   fails |= 0x20;
            if (((hi >> 24) & 0xFFu) != g_lapic_id) fails |= 0x20;
        }
        // bit6：其余重定向项必须保持掩码
        for (uint32_t n = 0; n <= g_ioapic_max && n < 256u; n++) {
            bool ours = false;
            for (int i = 0; i < ROUTES64_N; i++) {
                if (g_routes[i].programmed && g_routes[i].gsi - g_ioapic_gsi0 == n) { ours = true; break; }
            }
            if (ours) continue;
            if (!(redir_lo_read(n) & REDIR64_MASK)) { fails |= 0x40; break; }
        }
        // bit7：模式分派与实际生效路径一致（EOI/掩码确实走 APIC）
        if (g_irq_mode64 != IRQ_MODE64_APIC || !g_apic_ready) fails |= 0x80;
    }

    dbg64_line_begin64();
    dbg64_str("[APIC] selftest ");
    if (fails == 0) {
        dbg64_str("PASS");
    } else {
        dbg64_str("FAIL mask=");
        dbg64_dec((uint64_t)fails);
    }
    dbg64_nl();
    dbg64_line_end64();
    return fails;
}
