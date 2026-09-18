// smp64.cpp - SMP：BSP 启动 AP（INIT-SIPI-SIPI + 低端跳板）；AP 起来后只"在线 + 不捣乱"
//
// ============================ 这一版做了什么 ============================
//   1) 只读探测：ACPI 给的 CPU 数（acpi_cpu_count64）、MADT 里 enabled 的本地 APIC ID
//      （跳过 BSP 自己）、LAPIC MMIO 基址（MSR 0x1B）、跳板页（0x8000）在当前页表里
//      是否 present+可写。任何一条不满足：一行日志 + 优雅返回，**一个硬件寄存器都不写**。
//   2) 准备：把 ap_trampoline64.asm 的平铺二进制原文拷到物理 0x8000；在页内 0x800
//      建共享数据块（CR3/CR0/CR4/EFER/入口 RIP/本 AP 栈顶/序号/APIC ID/GDT 拷贝源/IDTR）；
//      用 page_alloc_64() 给每个 AP 分配 16KB（4 页）独立内核栈。
//   3) 自检（**发 SIPI 之前**跑）：跳板字节逐字节一致、共享块字段自洽、临时 GDT 与 BSP
//      的 GDT 一致 + index3 是 32 位代码段、每核栈已分配且 16 字节对齐、SIPI 向量与跳板
//      地址一致。→ "[SMP] selftest PASS" / "FAIL mask=<n>"。
//      口径：**AP 没起来属于环境事实，不是自检失败**（发起 SIPI 之后不改结论）。
//   4) 每个 AP：ICR high=目标 ID -> ICR low=INIT assert -> 10ms -> INIT deassert ->
//      200us -> SIPI -> 200us -> SIPI -> 有界等待在线标志（500ms，g_ticks64 计时 +
//      硬自旋上界兜底）。超时打 "[SMP] ap id=<n> timeout" 后继续下一个，绝不挂死。
//   5) AP 侧（AP 自己执行，见 smp64_ap_entry64）：置在线标志 -> lock xadd 计数 ->
//      自己打 "[SMP] ap id=<n> online (stack=<hex> index=<n>)" -> cli+hlt 停住。
//   6) 汇总："[SMP] online=<n>/<total> bsp_lapic_id=<n> trampoline@<hex>"。
//
// ============================ 明确的范围外 ============================
//   * 多核调度：调度器（kernel/task64.cpp）仍是单核，IRQ0 只在 BSP 上跑；AP 不参与调度。
//   * 没有 IPI、没有 per-CPU 数据/GDT/TSS、AP 的 LAPIC 不启用、AP 不接任何中断。
//   * UEFI 路径若固件页表没映射 0x8000（或没进 APIC 模式）→ 本模块优雅跳过（不变砖）。
//
// ============================ 自检失败位（0 = 通过）============================
//   0x01 跳板页：页对齐 + 在恒等映射范围内 + 当前 CR3 里 present 且可写
//   0x02 跳板字节：与内核镜像里的源 blob 逐字节一致；blob 长度合法（0 < len <= 0x800）
//   0x04 共享块：magic/序号/APIC ID/向量自洽，CR3 4KB 对齐非 0，入口 RIP 在内核高半区，
//                IDTR limit 非 0
//   0x08 临时 GDT：0x08/0x10 与 BSP 的 gdt_entry_raw64(1)/(2) 一致；index3 = 32 位
//                代码段（access=0x9A、gran=0xCF）；页内 GDTR 描述符 = limit 63 + base 0x8C00
//   0x10 每核栈：已分配、页对齐、栈顶 16 字节对齐、在恒等映射范围内、互不重叠
//   0x20 模式/向量：g_irq_mode64 == APIC 且 (SIPI 向量 << 12) == 跳板物理地址
#include "smp64.h"
#include "debug64.h"
#include "acpi64.h"
#include "memlayout64.h"
#include "mem_64.h"
#include "x86_64.h"
#include "apic64.h"
#include <stdint.h>
#include <stddef.h>

// 跳板平铺二进制（nasm -f bin -> objcopy；只链进系统内核，见 build64.sh）
extern "C" const uint8_t _binary_build64_ap_trampoline64_bin_start[];
extern "C" const uint8_t _binary_build64_ap_trampoline64_bin_end[];

// ==================== LAPIC / MSR 低层 ====================
static const uint32_t MSR64_IA32_APIC_BASE = 0x1Bu;
static const uint32_t MSR64_IA32_EFER      = 0xC0000080u;
static const uint32_t LAPIC64_ID  = 0x020;
static const uint32_t LAPIC64_ICR_LOW  = 0x300;   // 写这个就发 IPI（低位：向量/模式/dest 简写）
static const uint32_t LAPIC64_ICR_HIGH = 0x310;   // 位 24..31 = 目标 LAPIC ID（物理模式）

static uint64_t g_lapic_base = 0;
static uint32_t g_bsp_lapic_id = 0;

static inline uint64_t smp64_rdmsr64(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static inline uint64_t smp64_read_cr0() { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t smp64_read_cr3() { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t smp64_read_cr4() { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void smp64_sidt(void* dst10) { __asm__ volatile("sidt %0" : "=m"(*(uint8_t(*)[10])dst10)); }

static inline uint32_t smp64_lapic_r32(uint32_t off) {
    return *(const volatile uint32_t*)(uintptr_t)(g_lapic_base + off);
}
static inline void smp64_lapic_w32(uint32_t off, uint32_t v) {
    *(volatile uint32_t*)(uintptr_t)(g_lapic_base + off) = v;
}
// 页表检查是否 present（顺带取回 RW 位）：照 apic64.cpp 的做法走当前 CR3 的 4 级页表，
// 只读、绝不改 —— 目的是"宁可跳过 SMP，也不要 #PF"。
static bool smp64_pa_mapped64(uint64_t pa, bool* writable_out) {
    uint64_t cr3 = smp64_read_cr3();
    if (writable_out) *writable_out = false;
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
    if (e3 & 0x80u) { if (writable_out) *writable_out = (e3 & 2u) != 0; return true; }   // 1GB 大页
    const uint64_t pd = e3 & 0x000FFFFFFFFFF000ull;
    const uint64_t e2 = *(const volatile uint64_t*)(uintptr_t)(pd + (uint64_t)i2 * 8u);
    if (!(e2 & 1u)) return false;
    if (e2 & 0x80u) { if (writable_out) *writable_out = (e2 & 2u) != 0; return true; }   // 2MB 大页
    const uint64_t pt = e2 & 0x000FFFFFFFFFF000ull;
    const uint64_t e1 = *(const volatile uint64_t*)(uintptr_t)(pt + (uint64_t)i1 * 8u);
    if (!(e1 & 1u)) return false;
    if (writable_out) *writable_out = (e1 & 2u) != 0;
    return true;
}
static bool smp64_pa_ok64(uint64_t pa, uint64_t bytes) {
    return pa != 0 && pa < ML64_IDENTITY_BYTES && bytes <= ML64_IDENTITY_BYTES - pa;
}

// ==================== 日志（AP 自己打的那一行）====================
// ★ 两处必须一起看，缺一个就会"两行互相插字符"（实测踩过，见下面注释）：
//   ① AP 侧用**原子 test-and-set** 抢 debug64.h 那把行锁（g_dbg64_line_lock），
//      而不是直接调 dbg64_line_begin64()：后者是单核设计 —— 锁已被占用时按"同一行重入"
//      处理（只加深度、不等锁），SMP 下会把两行字符混在一起并把深度加歪。
//      拿不到锁时有界重试，最后兜底照样把这一行打完（宁可插字符，也不能让 AP 卡死）。
//   ② BSP 侧**不在 AP 打印期间打日志**：BSP 只在自己那行 intent 打完之后才发 SIPI，
//      而 AP 是"先打完自己的行、再置在线标志"；BSP 的等待循环只等这个标志。
//      于是正常路径上两边的打印区间天然不重叠（启动顺序见 smp64_start_ap64）。
//   —— 全量 SMP 安全日志锁需要改 debug64.h（本次范围外，见文件顶部"范围外"）。
static const uint64_t SMP64_LOG_SPIN = 40000000ull;
static void smp64_log_ap_online64(uint32_t apic_id, uint64_t stack_top, uint32_t index) {
    const uint64_t fl = dbg64_irq_save64();                    // AP 本来 IF=0；与 BSP 语义保持一致
    int* const lock = (int*)(uintptr_t)&g_dbg64_line_lock;
    uint64_t spins = 0;
    while (__sync_lock_test_and_set(lock, 1) != 0) {           // 原子 xchg：0 -> 1 才算拿到
        if (++spins > SMP64_LOG_SPIN) break;                   // 有界：极端情况放弃独占，照样打
        __asm__ volatile("pause");
    }
    g_dbg64_line_depth = 1;
    g_dbg64_line_flags = fl;
    dbg64_str("[SMP] ap id=");
    dbg64_dec(apic_id);
    dbg64_str(" online (stack=0x");
    dbg64_hex64(stack_top);
    dbg64_str(" index=");
    dbg64_dec(index);
    dbg64_str(")");
    dbg64_nl();
    g_dbg64_line_depth = 0;
    g_dbg64_line_lock  = 0;
    dbg64_irq_restore64(fl);
}

// ==================== 共享数据块（页内 0x800；字段布局与 asm 的 S_* 一致）====================
struct Smp64Shared64 {
    volatile uint32_t magic;        // 0x00 'SMPT'
    volatile uint32_t index;        // 0x04 AP 序号（1 起）
    volatile uint32_t apic_id;      // 0x08 目标 LAPIC ID
    volatile uint32_t sipi_vector;  // 0x0C SIPI 向量
    volatile uint64_t cr3;          // 0x10
    volatile uint64_t cr0;          // 0x18
    volatile uint64_t cr4;          // 0x20
    volatile uint64_t efer;         // 0x28
    volatile uint64_t entry_rip;    // 0x30
    volatile uint64_t rsp;          // 0x38
    volatile uint64_t flag_ptr;     // 0x40
    volatile uint64_t count_ptr;    // 0x48
    volatile uint8_t  gdt[64];      // 0x50（AP 从这里 rep movsb 到 0x8C00）
    volatile uint8_t  idtr[10];     // 0x90（BSP 的 IDTR，AP 也 lidt）
} __attribute__((packed));
static_assert(offsetof(Smp64Shared64, cr3)       == SMP64_S_CR3,       "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, cr0)       == SMP64_S_CR0,       "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, cr4)       == SMP64_S_CR4,       "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, efer)      == SMP64_S_EFER,      "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, entry_rip) == SMP64_S_ENTRY,     "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, rsp)       == SMP64_S_RSP,       "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, flag_ptr)  == SMP64_S_FLAGPTR,   "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, count_ptr) == SMP64_S_COUNTPTR,  "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, gdt)       == SMP64_S_GDT,       "共享块布局漂了");
static_assert(offsetof(Smp64Shared64, idtr)      == SMP64_S_IDTR,      "共享块布局漂了");
static_assert(sizeof(Smp64Shared64) <= SMP64_OFF_GDT_COPY - SMP64_OFF_SHARED, "共享块溢出页布局");

static Smp64Shared64* smp64_shared64() {
    return (Smp64Shared64*)(uintptr_t)(SMP64_TRAMPOLINE_PHYS + SMP64_OFF_SHARED);
}

// ==================== 每核状态 ====================
struct Ap64 {
    uint64_t stack_base;        // 栈底（页对齐）
    uint64_t stack_top;         // 栈顶（16 字节对齐）
    uint64_t stack_bytes;
    uint32_t apic_id;
    bool     allocated;
};
static Ap64     g_aps[SMP64_MAX_AP + 1];            // 下标 = AP 序号（1 起；0 不用）
static int      g_ap_n = 0;                          // 准备/启动了几个 AP
static volatile uint8_t  g_ap_online[SMP64_MAX_AP + 1] = {0};   // AP 自己置 1
static volatile uint64_t g_ap_online_count = 0;      // AP 用 lock xadd 递增（不含 BSP）
static uint32_t g_blob_len = 0;

// AP 侧要读的指针（共享块里给的是地址；AP 用同一个地址空间直接解引用）
static inline volatile uint8_t*  smp64_flag_ptr64(uint32_t index)  { return &g_ap_online[index]; }
static inline volatile uint64_t* smp64_count_ptr64()               { return &g_ap_online_count; }

uint32_t smp64_online_cpu_count64() { return (uint32_t)(1u + (uint32_t)g_ap_online_count); }

// ==================== MADT：收集 enabled 的本地 APIC ID（跳过 BSP 自己）====================
static uint32_t smp64_u32le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t smp64_u64le(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (uint64_t)p[i];
    return v;
}
static bool smp64_sig_eq(const uint8_t* p, const char* s) {
    for (int i = 0; i < 4; i++) if ((char)p[i] != s[i]) return false;
    return true;
}

// 从 RSDP 重新走到 MADT（acpi64.h 只暴露 CPU 个数，不暴露每个 CPU 的 APIC ID，
// 所以这里自己走一遍 —— 只读，不改 acpi64）。
static int smp64_collect_apic_ids64(uint32_t bsp_id, uint32_t* out, int out_max) {
    int n = 0;
    const uint64_t rsdp = acpi_rsdp_addr64();
    if (!smp64_pa_ok64(rsdp, 36u)) return 0;
    const uint8_t* r = (const uint8_t*)(uintptr_t)rsdp;
    const uint8_t  revision = r[0x0F];
    const uint64_t rsdt = smp64_u32le(r + 0x10);
    const uint64_t xsdt = (revision >= 2) ? smp64_u64le(r + 0x18) : 0;
    const bool use_xsdt = (revision >= 2 && xsdt != 0);
    const uint64_t root = use_xsdt ? xsdt : rsdt;
    if (!smp64_pa_ok64(root, 36u)) return 0;
    const uint8_t* rh = (const uint8_t*)(uintptr_t)root;
    if (!smp64_sig_eq(rh, use_xsdt ? "XSDT" : "RSDT")) return 0;
    const uint32_t rlen = smp64_u32le(rh + 4);
    if (rlen < 36u || !smp64_pa_ok64(root, rlen)) return 0;

    const uint32_t step = use_xsdt ? 8u : 4u;
    const uint32_t cnt = (rlen - 36u) / step;
    for (uint32_t i = 0; i < cnt; i++) {
        const uint8_t* p = rh + 36u + (uint64_t)i * step;
        const uint64_t tp = use_xsdt ? smp64_u64le(p) : (uint64_t)smp64_u32le(p);
        if (!smp64_pa_ok64(tp, 36u)) continue;
        const uint8_t* th = (const uint8_t*)(uintptr_t)tp;
        if (!smp64_sig_eq(th, "APIC")) continue;
        const uint32_t len = smp64_u32le(th + 4);
        if (len < 0x2Cu || !smp64_pa_ok64(tp, len)) return n;

        uint32_t off = 0x2Cu;
        while (off + 2u <= len) {
            const uint8_t* e = th + off;
            const uint8_t et = e[0];
            const uint8_t el = e[1];
            if (el < 2u || off + (uint32_t)el > len) break;
            uint32_t id = 0xFFFFFFFFu;
            if (et == 0u && el >= 8u) {                      // type 0：处理器本地 APIC
                if (smp64_u32le(e + 4) & 1u) id = e[3];      // bit0 = enabled
            } else if (et == 9u && el >= 16u) {              // type 9：x2APIC（ID > 255 暂不用）
                if (smp64_u32le(e + 8) & 1u) id = smp64_u32le(e + 4);
            }
            if (id != 0xFFFFFFFFu && id != bsp_id && id <= 0xFFu && n < out_max) {
                out[n++] = id;
            }
            off += el;
        }
        return n;
    }
    return n;
}

// ==================== 有界延时/等待 ====================
// 用 g_ticks64（PIT 250Hz -> 4ms/tick）计时；**再叠一个硬自旋上界**：万一 tick 不涨
// （IF=0 之类），也不允许挂死启动。ticks/serial 两路都不依赖任何新硬件。
static void smp64_delay_ms64(uint32_t ms) {
    const uint64_t ticks = ((uint64_t)ms + TICK_MS_64 - 1) / TICK_MS_64;
    const uint64_t start = g_ticks64;
    for (uint64_t spin = 0;; spin++) {
        if (g_ticks64 - start >= ticks) return;
        if (spin > 600000000ull) return;                      // 兜底：绝不挂死
        __asm__ volatile("pause");
    }
}
// 微秒级：PIT 只有 4ms 粒度，用端口 I/O 做粗延时（QEMU/真机每次 outb 都是 ~us 量级）。
static void smp64_delay_us64(uint32_t us) {
    for (uint32_t i = 0; i < us; i++) io_wait64();
}

// 读 ICR 的低位（含"投递中"bit12）——写 ICR 前必须等它清零
static bool smp64_icr_idle64() {
    for (uint32_t i = 0; i < 2000000u; i++) {
        if (!(smp64_lapic_r32(LAPIC64_ICR_LOW) & (1u << 12))) return true;
        __asm__ volatile("pause");
    }
    return false;
}
static void smp64_send_ipi64(uint32_t dest_apic_id, uint32_t icr_low) {
    (void)smp64_icr_idle64();
    smp64_lapic_w32(LAPIC64_ICR_HIGH, (uint32_t)((dest_apic_id & 0xFFu) << 24));
    smp64_lapic_w32(LAPIC64_ICR_LOW, icr_low);
    __asm__ volatile("" ::: "memory");
}

// ==================== 准备：跳板 + 共享块 + 每核栈 ====================
static void smp64_fill_gdt64(volatile uint8_t* dst64) {
    // 临时 GDT = BSP 的 8 项（0x08/0x10/0x23/0x2B/0x30 全一致），只把 **index 3（0x18）**
    // 改成"32 位代码段"：AP 在进 IA-32e 之前要先在保护模式里跑一段，而 0x08 是 L=1 的
    // 64 位代码段（IA-32e 没打开时跳 L=1 的描述符会 #GP）。0x18 在内核里是**保留项**
    // （x86_64.cpp 的 GDT 表：index 3 = 0），没有任何代码加载 0x18，改它不影响 BSP。
    for (int i = 0; i < 8; i++) {
        const uint64_t raw = gdt_entry_raw64(i);
        for (int b = 0; b < 8; b++) dst64[i * 8 + b] = (uint8_t)((raw >> (8 * b)) & 0xFFu);
    }
    // index3：{limit_low=0xFFFF, base_low=0, base_mid=0, access=0x9A(P|DPL0|代码|可读),
    //          gran=0xCF(G=1, D=1 32 位, limit_high=0xF), base_high=0}
    dst64[3 * 8 + 0] = 0xFF; dst64[3 * 8 + 1] = 0xFF;
    dst64[3 * 8 + 2] = 0x00; dst64[3 * 8 + 3] = 0x00;
    dst64[3 * 8 + 4] = 0x00;
    dst64[3 * 8 + 5] = 0x9A;
    dst64[3 * 8 + 6] = 0xCF;
    dst64[3 * 8 + 7] = 0x00;
}

static void smp64_write_shared_block64(uint32_t index, uint32_t apic_id, uint64_t stack_top) {
    Smp64Shared64* sh = smp64_shared64();
    sh->magic       = SMP64_SHARED_MAGIC;
    sh->index       = index;
    sh->apic_id     = apic_id;
    sh->sipi_vector = SMP64_SIPI_VECTOR;
    sh->cr3         = smp64_read_cr3();
    sh->cr0         = smp64_read_cr0() | 0x80000001ull;        // PE|PG
    sh->cr4         = smp64_read_cr4() | (1ull << 5);          // PAE
    sh->efer        = smp64_rdmsr64(MSR64_IA32_EFER) | (1ull << 8);   // LME
    sh->entry_rip   = (uint64_t)(uintptr_t)&smp64_ap_entry64;
    sh->rsp         = stack_top;
    sh->flag_ptr    = (uint64_t)(uintptr_t)smp64_flag_ptr64(index);
    sh->count_ptr   = (uint64_t)(uintptr_t)smp64_count_ptr64();
    smp64_sidt((void*)sh->idtr);
}

// 每核栈：尽量 16KB（4 页连续）；分配器给了非连续页就重试几次，仍不行就退化成 1 页
// （AP 本阶段只用几十字节栈，1 页也够；但**如实记在 stack_bytes 里**，自检照样查）。
static void smp64_alloc_ap_stack64(uint32_t index) {
    Ap64& ap = g_aps[index];
    ap.allocated = false;
    ap.stack_base = 0; ap.stack_bytes = 0; ap.stack_top = 0;
    for (int attempt = 0; attempt < 4; attempt++) {
        void* pg[SMP64_AP_STACK_PAGES];
        for (uint32_t i = 0; i < SMP64_AP_STACK_PAGES; i++) pg[i] = page_alloc_64();
        bool ok = true;
        for (uint32_t i = 0; i < SMP64_AP_STACK_PAGES; i++) if (!pg[i]) ok = false;
        for (uint32_t i = 1; ok && i < SMP64_AP_STACK_PAGES; i++) {
            if ((uint64_t)(uintptr_t)pg[i] != (uint64_t)(uintptr_t)pg[0] + (uint64_t)i * 4096u) ok = false;
        }
        if (ok) {
            ap.stack_base  = (uint64_t)(uintptr_t)pg[0];
            ap.stack_bytes = SMP64_AP_STACK_BYTES;
            ap.stack_top   = ap.stack_base + ap.stack_bytes;      // 16KB 是 16 的倍数 -> 16B 对齐
            ap.allocated   = true;
            return;
        }
        for (uint32_t i = 0; i < SMP64_AP_STACK_PAGES; i++) page_free_64(pg[i]);   // 不连续：还回去重来
    }
    void* one = page_alloc_64();
    if (one) {
        ap.stack_base  = (uint64_t)(uintptr_t)one;
        ap.stack_bytes = 4096u;
        ap.stack_top   = ap.stack_base + ap.stack_bytes;
        ap.allocated   = true;
    }
}

static int smp64_prepare64(int ap_n, const uint32_t* apic_ids) {
    int fails = 0;
    uint8_t* page = (uint8_t*)(uintptr_t)SMP64_TRAMPOLINE_PHYS;

    const uint8_t* blob = _binary_build64_ap_trampoline64_bin_start;
    const uint32_t blob_len = (uint32_t)(_binary_build64_ap_trampoline64_bin_end -
                                        _binary_build64_ap_trampoline64_bin_start);
    g_blob_len = blob_len;
    // blob 必须非空、且不与共享块（0x800）重叠 —— 这是硬前提，不满足就别动内存。
    if (blob_len == 0 || blob_len > SMP64_OFF_SHARED) {
        dbg64_line_begin64();
        dbg64_str("[SMP] trampoline blob length invalid len=");
        dbg64_dec(blob_len);
        dbg64_nl();
        dbg64_line_end64();
        return 0x02;
    }

    // 1) 整页清零 -> 2) 原文拷入 blob -> 3) 写共享块 / GDT 拷贝源
    for (uint32_t i = 0; i < 4096u; i++) page[i] = 0;
    for (uint32_t i = 0; i < blob_len; i++) page[i] = blob[i];

    g_ap_n = ap_n;
    for (int i = 1; i <= ap_n; i++) {
        smp64_alloc_ap_stack64((uint32_t)i);
        g_aps[i].apic_id = apic_ids[i - 1];
        if (!g_aps[i].allocated) fails |= 0x10;
    }
    // 共享块先按第一个 AP 填好（每个 AP 启动前会再刷新 index/apic_id/rsp）
    smp64_fill_gdt64(smp64_shared64()->gdt);
    smp64_write_shared_block64(1, g_aps[1].apic_id, g_aps[1].stack_top);
    return fails;
}

// ==================== 自检（发 SIPI 之前）====================
int smp64_selftest64() {
    int fails = 0;

    // 0x01 跳板页：页对齐 + 恒等映射范围 + 当前页表 present 且可写
    const uint64_t tp = SMP64_TRAMPOLINE_PHYS;
    if ((tp & 0xFFFull) != 0 || !smp64_pa_ok64(tp, 4096u)) fails |= 0x01;
    else {
        bool wr = false;
        if (!smp64_pa_mapped64(tp, &wr) || !wr) fails |= 0x01;
    }

    // 0x02 跳板字节：与源 blob 逐字节一致（+ 长度合法）
    const uint8_t* src = _binary_build64_ap_trampoline64_bin_start;
    const uint32_t blob_len = (uint32_t)(_binary_build64_ap_trampoline64_bin_end -
                                        _binary_build64_ap_trampoline64_bin_start);
    if (blob_len == 0 || blob_len > SMP64_OFF_SHARED || g_blob_len != blob_len) fails |= 0x02;
    else {
        const uint8_t* dst = (const uint8_t*)(uintptr_t)tp;
        for (uint32_t i = 0; i < blob_len; i++) {
            if (dst[i] != src[i]) { fails |= 0x02; break; }
        }
    }

    // 0x04 共享块自洽
    const Smp64Shared64* sh = smp64_shared64();
    if (sh->magic != SMP64_SHARED_MAGIC) fails |= 0x04;
    if (sh->sipi_vector != SMP64_SIPI_VECTOR) fails |= 0x04;
    // CR3：取其物理基址（低 12 位是 PCD/PWT 之类标志，不算错），必须非 0 且 < 4GB
    {
        const uint64_t cr3_pa = sh->cr3 & 0x000FFFFFFFFFF000ull;
        if (cr3_pa == 0 || cr3_pa >= ML64_IDENTITY_BYTES) fails |= 0x04;
    }
    if ((sh->cr0 & (1ull << 31)) == 0 || (sh->cr0 & 1ull) == 0) fails |= 0x04;   // PE|PG
    if ((sh->cr4 & (1ull << 5)) == 0) fails |= 0x04;                             // PAE
    if ((sh->efer & (1ull << 8)) == 0) fails |= 0x04;                            // LME
    if (sh->entry_rip < ML64_KERNEL_VA_BASE ||
        sh->entry_rip >= ML64_KERNEL_VA_BASE + (1ull << 30)) fails |= 0x04;
    if (sh->entry_rip != (uint64_t)(uintptr_t)&smp64_ap_entry64) fails |= 0x04;
    if (sh->rsp == 0 || (sh->rsp & 0xFull) != 0) fails |= 0x04;
    if (sh->index == 0 || sh->index > (uint32_t)g_ap_n) fails |= 0x04;
    if (sh->flag_ptr != (uint64_t)(uintptr_t)smp64_flag_ptr64(sh->index)) fails |= 0x04;
    if (sh->count_ptr != (uint64_t)(uintptr_t)smp64_count_ptr64()) fails |= 0x04;
    if (*(const volatile uint16_t*)(const void*)sh->idtr == 0) fails |= 0x04;     // IDTR limit
    {
        uint64_t idt_base = 0;
        for (int i = 9; i >= 2; i--) idt_base = (idt_base << 8) | sh->idtr[i];
        if (idt_base == 0) fails |= 0x04;
    }

    // 0x08 临时 GDT（拷贝源）+ 页内 GDTR 描述符
    const uint64_t want_code = gdt_entry_raw64(1);     // 0x08 内核 64 位代码段
    const uint64_t want_data = gdt_entry_raw64(2);     // 0x10 内核数据段
    uint64_t got_code = 0, got_data = 0;
    for (int b = 7; b >= 0; b--) got_code = (got_code << 8) | sh->gdt[1 * 8 + b];
    for (int b = 7; b >= 0; b--) got_data = (got_data << 8) | sh->gdt[2 * 8 + b];
    if (got_code != want_code || want_code == 0) fails |= 0x08;
    if (got_data != want_data || want_data == 0) fails |= 0x08;
    if (sh->gdt[3 * 8 + 5] != 0x9A || sh->gdt[3 * 8 + 6] != 0xCF) fails |= 0x08; // 32 位代码段
    {
        const uint16_t gdtr_limit = *(const volatile uint16_t*)(const uint8_t*)(uintptr_t)(tp + SMP64_OFF_GDTR);
        const uint32_t gdtr_base  = *(const volatile uint32_t*)(const uint8_t*)(uintptr_t)(tp + SMP64_OFF_GDTR + 2);
        // ★ 这条同时钉住"跳板里的绝对地址 = 页基址 + 页内偏移"这条约定：早期版本漏了页基址，
        //   跳板把 GDT 拷到 0x0C00、从 0x0850 读源 -> AP 在远跳 0x18 时 #GP(0018) 三重故障复位。
        if (gdtr_limit != 63u || gdtr_base != (uint32_t)(tp + SMP64_OFF_GDT_COPY)) fails |= 0x08;
    }

    // 0x10 每核栈：已分配 / 页对齐 / 栈顶 16B 对齐 / 在恒等映射范围内 / 互不重叠
    for (int i = 1; i <= g_ap_n; i++) {
        const Ap64& ap = g_aps[i];
        if (!ap.allocated || ap.stack_base == 0 || (ap.stack_base & 0xFFFull) != 0) { fails |= 0x10; continue; }
        if ((ap.stack_top & 0xFull) != 0 || ap.stack_top != ap.stack_base + ap.stack_bytes) fails |= 0x10;
        if (!smp64_pa_ok64(ap.stack_base, ap.stack_bytes)) fails |= 0x10;
        if (!smp64_pa_mapped64(ap.stack_base, nullptr)) fails |= 0x10;
        for (int j = 1; j < i; j++) {
            if (!g_aps[j].allocated) continue;
            const uint64_t a0 = g_aps[j].stack_base, a1 = a0 + g_aps[j].stack_bytes;
            if (ap.stack_base < a1 && a0 < ap.stack_top) fails |= 0x10;         // 重叠
        }
    }

    // 0x20 模式/向量
    if (g_irq_mode64 != IRQ_MODE64_APIC) fails |= 0x20;
    if ((uint64_t)SMP64_SIPI_VECTOR * 0x1000ull != tp) fails |= 0x20;

    dbg64_line_begin64();
    if (fails == 0) dbg64_str("[SMP] selftest PASS");
    else { dbg64_str("[SMP] selftest FAIL mask="); dbg64_dec((uint64_t)fails); }
    dbg64_nl();
    dbg64_line_end64();
    return fails;
}

// ==================== 启动单个 AP ====================
static bool smp64_wait_ap_online64(int index, uint32_t timeout_ms) {
    const uint64_t ticks = ((uint64_t)timeout_ms + TICK_MS_64 - 1) / TICK_MS_64;
    const uint64_t start = g_ticks64;
    for (uint64_t spin = 0;; spin++) {
        if (g_ap_online[index]) return true;
        if (g_ticks64 - start >= ticks) return false;
        if (spin > 900000000ull) return false;                 // 硬上界：tick 不涨也不挂死
        __asm__ volatile("pause");
    }
}

static bool smp64_start_ap64(int index) {
    Ap64& ap = g_aps[index];
    if (!ap.allocated) return false;

    // 1) 刷新共享块：本 AP 的序号/APIC ID/自己的栈顶（AP 起来第一件事就是读它）
    smp64_write_shared_block64((uint32_t)index, ap.apic_id, ap.stack_top);

    // 2) BSP 侧留一行"意图"（**必须在发 SIPI 之前**打：SIPI 一发出 AP 就可能开始跑并
    //    打它自己那行，两条行交错就难看了）。本行的 stack 会与 AP 自己 online 行里的
    //    stack 逐位对上（测试就是这么断言的）—— 它顺带验证"AP 读到的共享块 == BSP 写的"。
    dbg64_line_begin64();
    dbg64_str("[SMP] ap id=");
    dbg64_dec(ap.apic_id);
    dbg64_str(" sipi vector=0x");
    dbg64_dec(SMP64_SIPI_VECTOR);
    dbg64_str(" index=");
    dbg64_dec((uint64_t)index);
    dbg64_str(" stack=0x");
    dbg64_hex64(ap.stack_top);
    dbg64_nl();
    dbg64_line_end64();

    // 3) 标准启动序列（Intel MP / ACPI 的做法）：
    //    INIT assert(0x4500) -> 10ms -> INIT deassert(0x8500) -> 200us
    //    -> SIPI(0x4600|vector) -> 200us -> SIPI 再一次（给还没起来的第二次机会）
    smp64_send_ipi64(ap.apic_id, 0x00004500u);                       // INIT assert（level）
    smp64_delay_ms64(10);
    smp64_send_ipi64(ap.apic_id, 0x00008500u);                       // INIT deassert
    smp64_delay_us64(200);
    smp64_send_ipi64(ap.apic_id, 0x00004600u | SMP64_SIPI_VECTOR);   // SIPI #1
    smp64_delay_us64(200);
    smp64_send_ipi64(ap.apic_id, 0x00004600u | SMP64_SIPI_VECTOR);   // SIPI #2

    // 4) 有界等待：这段**一行日志都不打**（AP 打完自己那行才会置在线标志；见文件顶部 ②）
    if (smp64_wait_ap_online64(index, SMP64_AP_ONLINE_TIMEOUT_MS)) return true;

    dbg64_line_begin64();
    dbg64_str("[SMP] ap id=");
    dbg64_dec(ap.apic_id);
    dbg64_str(" timeout");
    dbg64_nl();
    dbg64_line_end64();
    return false;
}

// ==================== BSP 入口 ====================
int smp64_init64() {
    // 防御性判断（kernel64.cpp 里也有同样一条；只有 APIC 模式才有 ICR 可用）
    if (g_irq_mode64 != IRQ_MODE64_APIC) {
        dbg64_line_begin64();
        dbg64_str("[SMP] skipped (irq mode = pic)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    const int cpus = acpi_cpu_count64();
    if (cpus <= 1) {
        dbg64_line_begin64();
        dbg64_str("[SMP] single cpu (no AP to start)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    // ---- 阶段 1：只读探测（不通过就一个寄存器都不写）----
    g_lapic_base = smp64_rdmsr64(MSR64_IA32_APIC_BASE) & 0xFFFFF000ull;
    if (g_lapic_base == 0 || !smp64_pa_ok64(g_lapic_base, 0x400u) ||
        !smp64_pa_mapped64(g_lapic_base, nullptr)) {
        dbg64_line_begin64();
        dbg64_str("[SMP] lapic unavailable -> no ap started");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    g_bsp_lapic_id = (smp64_lapic_r32(LAPIC64_ID) >> 24) & 0xFFu;

    if (!smp64_pa_ok64(SMP64_TRAMPOLINE_PHYS, 4096u)) {
        dbg64_line_begin64();
        dbg64_str("[SMP] trampoline page out of identity range -> no ap started");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    {
        bool wr = false;
        if (!smp64_pa_mapped64(SMP64_TRAMPOLINE_PHYS, &wr) || !wr) {
            dbg64_line_begin64();
            dbg64_str("[SMP] trampoline page 0x0000000000008000 not mapped+writable -> no ap started");
            dbg64_nl();
            dbg64_line_end64();
            return 0;
        }
    }

    uint32_t ids[SMP64_MAX_AP];
    int n = smp64_collect_apic_ids64(g_bsp_lapic_id, ids, (int)SMP64_MAX_AP);
    if (n <= 0) {
        dbg64_line_begin64();
        dbg64_str("[SMP] no madt lapic ids -> no ap started");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    if (n > cpus - 1) n = cpus - 1;              // 以 MADT 的 enabled CPU 数为准
    if (n > (int)SMP64_MAX_AP) n = (int)SMP64_MAX_AP;

    dbg64_line_begin64();
    dbg64_str("[SMP] cpus=");
    dbg64_dec((uint64_t)cpus);
    dbg64_str(" ap=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" bsp_lapic_id=");
    dbg64_dec(g_bsp_lapic_id);
    dbg64_str(" lapic_base=0x");
    dbg64_hex64(g_lapic_base);
    dbg64_str(" trampoline@0x");
    dbg64_hex64(SMP64_TRAMPOLINE_PHYS);
    dbg64_str(" vector=0x");
    dbg64_dec(SMP64_SIPI_VECTOR);
    dbg64_nl();
    dbg64_line_end64();

    // ---- 阶段 2：准备（跳板 + 共享块 + 每核栈）----
    const int pmask = smp64_prepare64(n, ids);
    if (pmask != 0) {
        dbg64_line_begin64();
        dbg64_str("[SMP] prepare FAIL mask=");
        dbg64_dec((uint64_t)pmask);
        dbg64_str(" -> no ap started");
        dbg64_nl();
        dbg64_line_end64();
        return pmask;
    }

    // ---- 阶段 3：自检（**必须在发起 SIPI 之前**；AP 没起来不算自检失败）----
    const int smask = smp64_selftest64();
    if (smask != 0) {
        dbg64_line_begin64();
        dbg64_str("[SMP] ap start skipped (selftest FAIL mask=");
        dbg64_dec((uint64_t)smask);
        dbg64_str(")");
        dbg64_nl();
        dbg64_line_end64();
    } else {
        // ---- 阶段 4：逐个 AP 走 INIT-SIPI-SIPI（每个都有界等待）----
        for (int i = 1; i <= n; i++) (void)smp64_start_ap64(i);
    }

    // ---- 阶段 5：汇总（AP 自己那行 online 在 AP 侧打印）----
    const uint32_t online_cpus = smp64_online_cpu_count64();
    dbg64_line_begin64();
    dbg64_str("[SMP] online=");
    dbg64_dec((uint64_t)online_cpus);
    dbg64_str("/");
    dbg64_dec((uint64_t)cpus);
    dbg64_str(" bsp_lapic_id=");
    dbg64_dec(g_bsp_lapic_id);
    dbg64_str(" trampoline@0x");
    dbg64_hex64(SMP64_TRAMPOLINE_PHYS);
    dbg64_nl();
    dbg64_line_end64();

    // 在线计数 vs 每 AP 标志 vs 打印数：三者是"同一次观察"的三种记法。
    // ★ 对不齐也只打 note（**不是** selftest 失败）：AP 未起/起晚了都是环境事实。
    uint32_t flags_set = 0;
    for (int i = 1; i <= n; i++) if (g_ap_online[i]) flags_set++;
    if ((uint64_t)flags_set != (uint64_t)g_ap_online_count ||
        (uint32_t)g_ap_online_count + 1u != online_cpus) {
        dbg64_line_begin64();
        dbg64_str("[SMP] note count/flag mismatch count=");
        dbg64_dec(g_ap_online_count);
        dbg64_str(" flags=");
        dbg64_dec(flags_set);
        dbg64_str(" (env fact, not a selftest failure)");
        dbg64_nl();
        dbg64_line_end64();
    }
    if (online_cpus < (uint32_t)cpus) {
        dbg64_line_begin64();
        dbg64_str("[SMP] note online=");
        dbg64_dec((uint64_t)online_cpus);
        dbg64_str("/");
        dbg64_dec((uint64_t)cpus);
        dbg64_str(" (some AP did not come up; env fact, not a selftest failure)");
        dbg64_nl();
        dbg64_line_end64();
    }
    return 0;
}

// ==================== AP 侧 64 位入口 ====================
// 由跳板的 64 位桩 jmp 进来：CS=0x08 / DS=SS=0x10（与 BSP 一致）、CR3 = BSP 的页表、
// RSP = 共享块里给本 AP 的内核栈顶（RSP%16==8，模拟 call 进来的样子）。
//
// 顺序很重要（见文件顶部 ②）：
//   ① 先自己打那一行 online（"AP 真的在跑"的证据，必须由 AP 打）；
//   ② 再 lock xadd 递增在线计数；
//   ③ **最后**置在线标志 —— BSP 的等待循环只等这个标志，所以等它看到标志时，
//      AP 的这行日志一定已经打完，两条日志不可能交错。
//   ④ cli + hlt 停住（AP 不参与调度、不接中断、不碰任何设备）。
extern "C" void smp64_ap_entry64() {
    const Smp64Shared64* sh = smp64_shared64();
    const uint32_t index   = sh->index;
    const uint32_t apic_id = sh->apic_id;
    const uint64_t rsp     = sh->rsp;

    smp64_log_ap_online64(apic_id, rsp, index);

    // lock xadd（__sync_fetch_and_add 在 x86-64 上就是 lock xadd）：
    // 在线计数必须原子 —— 多个 AP 同时上线时不能丢计数。
    __sync_fetch_and_add((volatile uint64_t*)(uintptr_t)sh->count_ptr, 1);

    if (index >= 1 && index <= (uint32_t)SMP64_MAX_AP) {
        *(volatile uint8_t*)(uintptr_t)sh->flag_ptr = 1;      // ③ 放行 BSP
    }

    for (;;) __asm__ volatile("cli; hlt");
}
