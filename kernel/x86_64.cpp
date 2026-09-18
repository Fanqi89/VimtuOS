// x86_64.cpp - Vimtu64 平台层实现（64 位长模式：PIC / PIT / IDT / TSS / RTC）
//
// 与 32 位 kernel/x86.cpp 的关系：这是 64 位版本，接口见 x86_64.h。
//   32 位构建用 x86.cpp，64 位构建用本文件，互不影响。
//
// 关键差异（64 位专有）：
//   * IDT 门是 16 字节、偏移拆成 low/mid/high 三段，基址 64 位；
//   * GDTR/IDTR 的基址是 64 位；
//   * 必须建立 TSS 并 ltr：Intel SDM 要求长模式下 IDT 里存在中断/陷阱门时 TR 有效；
//   * 中断帧由 isr_stubs64.asm 定义（见那里的权威注释），本文件按 pt_regs64 解释；
//   * 长模式下 PUSH/POP Sreg 受限（PUSH 全禁、POP 只允许 fs/gs），所以帧里的
//     段寄存器槽是占位值，内核里这些选择子恒为 0x10。
#include "x86_64.h"
#include "debug64.h"

// ==================== 全局状态 ====================
volatile uint64_t g_ticks64 = 0;
volatile uint64_t g_irq_total64 = 0;
// 中断路由模式（见 x86_64.h）：0 = 8259 PIC，1 = LAPIC+IOAPIC（kernel/apic64.cpp 置位）。
// 定义放在平台层，EOI / pic_unmask64 两条路径都在这里分派。
volatile uint32_t g_irq_mode64 = 0;

static uint32_t g_syscall_count = 0;
static uint32_t g_page_fault_count = 0;

// ==================== 描述符与寄存器结构 ====================
struct GDTEntry64 {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
} __attribute__((packed));

struct TSSEntry64 {                  // 64 位 TSS 描述符占两个普通槽（16 字节）
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  gran;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct GDTR64 { uint16_t limit; uint64_t base; } __attribute__((packed));
struct IDTR64 { uint16_t limit; uint64_t base; } __attribute__((packed));

struct IDTEntry64 {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;            // bit0..2 = IST 索引，其余保留
    uint8_t  type_attr;      // P(bit7) | DPL(bit6..5) | 0 | 门类型(0xE 中断门 / 0xF 陷阱门)
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed));

struct TSS64 {               // 104 字节；本系统只用 rsp0
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

static IDTEntry64 g_idt[256];
static IDTR64     g_idtr;
static TSS64      g_tss;
static GDTR64     g_gdtr;
// ==================== GDT 布局（8 项；顺序是硬约束，别乱排）====================
//   index  选择子  DPL  描述
//     0    ----     0    null
//     1    0x08     0    内核代码（64 位，L=1）
//     2    0x10     0    内核数据
//     3    (0x18)   0    保留（历史 TSS 槽；现在必须空出来）
//     4    0x23     3    用户数据
//     5    0x2B     3    用户代码（64 位，L=1）
//     6..7 0x30     0    TSS（64 位 TSS 描述符占 2 个 8 字节槽）
//
// ★ 为什么必须是这个顺序：今后要上长模式 syscall/sysret 时，SYSRET 用
//   STAR[63:48]（标准值 0x1B）自动算出用户选择子：CS = STAR[63:48]+16 = 0x2B、
//   SS = STAR[63:48]+8 = 0x23；SYSCALL 用 STAR[47:32]/[31:16] 得内核 CS/SS
//   （0x08/0x10）。也就是说 用户 SS 必须落在 index 4、用户 CS 必须落在 index 5，
//   内核 CS/DS 必须留在 index 1/2 —— TSS 只能退到 index 6/7。
//   把用户段排到别的下标会让 sysret 落到错误的选择子（表现为回到用户态即 #GP），
//   而改下标必须同时改 STAR；这里一次排到位，避免以后再搬。
//
// ★ 改动后必须确认既有选择子不变：内核 CS=0x08、内核 DS=0x10（IDT/中断桩/
//   task64 的帧构造/entry64.asm 的段加载都写死了这两个值）。
static GDTEntry64 g_gdt[8];

// ==================== 中断机制探针（诊断专用）====================
// 只有 build64.sh 传 -DVIMTU_PROBE_INT 时才编译。用途：把"IDT/门/桩"与"PIC/PIT 投递"
// 分开验证 —— 探针桩在栈顶压一个标记，再把 rsp 交给这里，帧里只有 CPU 自己的字段。


// ==================== IDT ====================
void idt_set_gate64(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist) {
    g_idt[num].off_low   = (uint16_t)(base & 0xFFFF);
    g_idt[num].selector  = sel;
    g_idt[num].ist       = (uint8_t)(ist & 0x07);
    g_idt[num].type_attr = flags;
    g_idt[num].off_mid   = (uint16_t)((base >> 16) & 0xFFFF);
    g_idt[num].off_high  = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    g_idt[num].zero      = 0;
}

// 中断桩入口表：isr0_64..isr47_64 + isr128_64（int 0x80）
typedef void (*isr_fn)();
static const isr_fn kIsrTable[49] = {
    isr0_64,  isr1_64,  isr2_64,  isr3_64,  isr4_64,  isr5_64,  isr6_64,  isr7_64,
    isr8_64,  isr9_64,  isr10_64, isr11_64, isr12_64, isr13_64, isr14_64, isr15_64,
    isr16_64, isr17_64, isr18_64, isr19_64, isr20_64, isr21_64, isr22_64, isr23_64,
    isr24_64, isr25_64, isr26_64, isr27_64, isr28_64, isr29_64, isr30_64, isr31_64,
    isr32_64, isr33_64, isr34_64, isr35_64, isr36_64, isr37_64, isr38_64, isr39_64,
    isr40_64, isr41_64, isr42_64, isr43_64, isr44_64, isr45_64, isr46_64, isr47_64,
    isr128_64,
};

static void idt_build() {
    for (int i = 0; i < 49; i++) {
        const uint8_t idx = (i < 48) ? (uint8_t)i : (uint8_t)128;
        // 0x8E = P=1 DPL=0 中断门(0xE)；int 0x80 用 0xEE 允许 DPL=3
        const uint8_t flags = (i == 48) ? 0xEE : 0x8E;
        idt_set_gate64(idx, (uint64_t)(uintptr_t)kIsrTable[i], 0x08, flags, 0);
    }
    // 其余向量保持"未安装"（offset=0、P=0），一旦触发就会 #GP —— 这正是我们想要的
    for (int n = 0; n < 256; n++) {
        if (n < 48 || n == 128) continue;
        idt_set_gate64((uint8_t)n, 0, 0x08, 0x8E, 0);
        g_idt[n].type_attr = 0x0E;      // 清 P 位：未安装
    }
}

// ==================== TSS ====================
static void tss_write_descriptor() {
    const uint64_t base = (uint64_t)(uintptr_t)&g_tss;
    const uint32_t limit = sizeof(TSS64) - 1;
    // ★ TSS 描述符装在 index 6（+7 作为高 8 字节），因为 index 4/5 被用户 DS/CS 占了
    //   （顺序约束见文件里 g_gdt 上方的说明）。选择子 = 6<<3 = 0x30。
    g_gdt[6].limit_low = (uint16_t)(limit & 0xFFFF);
    g_gdt[6].base_low  = (uint16_t)(base & 0xFFFF);
    g_gdt[6].base_mid  = (uint8_t)((base >> 16) & 0xFF);
    g_gdt[6].access    = 0x89;                                   // P=1 DPL=0 类型 0x9（64 位可用 TSS）
    g_gdt[6].gran      = (uint8_t)((limit >> 16) & 0x0F);        // 无粒度、非 32 位
    g_gdt[6].base_high = (uint8_t)((base >> 24) & 0xFF);
    TSSEntry64* hi = (TSSEntry64*)&g_gdt[6];                     // 覆盖 g_gdt[6] 与 g_gdt[7]
    hi->base_upper = (uint32_t)(base >> 32);
    hi->reserved   = 0;
}

void tss_set_rsp0(uint64_t rsp0) { g_tss.rsp0 = rsp0; }

static void tss_init(uint64_t rsp0) {
    uint8_t* p = (uint8_t*)&g_tss;
    for (uint32_t i = 0; i < sizeof(TSS64); i++) p[i] = 0;

    // ---- GDT 8 项（顺序是硬约束：见 g_gdt 上方说明；内核选择子 0x08/0x10 不许变）----
    g_gdt[0] = GDTEntry64{0, 0, 0, 0, 0, 0};                          // null
    g_gdt[1] = GDTEntry64{0xFFFF, 0x0000, 0x00, 0x9A, 0x20, 0x00};    // 0x08 内核 64 位代码
    g_gdt[2] = GDTEntry64{0xFFFF, 0x0000, 0x00, 0x92, 0x00, 0x00};    // 0x10 内核数据
    g_gdt[3] = GDTEntry64{0, 0, 0, 0, 0, 0};                          // 3：保留（index 4/5 让给用户段）
    // 用户段：access = 内核项的 access | DPL(0x60)。代码段带 L=1（gran bit5），数据段不带。
    g_gdt[4] = GDTEntry64{0xFFFF, 0x0000, 0x00, 0xF2, 0x00, 0x00};    // 0x23 用户数据 DPL=3
    g_gdt[5] = GDTEntry64{0xFFFF, 0x0000, 0x00, 0xFA, 0x20, 0x00};    // 0x2B 用户 64 位代码 DPL=3
    // g_gdt[6..7] 由 tss_write_descriptor() 填（TSS 描述符 = 2 个槽）

    g_tss.rsp0 = rsp0;
    g_tss.iomap_base = sizeof(TSS64);        // 指向 TSS 末尾 -> 无 I/O 位图
    tss_write_descriptor();

    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base  = (uint64_t)(uintptr_t)&g_gdt;
    __asm__ volatile("lgdt %0" : : "m"(g_gdtr));
    __asm__ volatile("ltr %0" : : "r"(SEL64_TSS));

    // 串口留证：内核选择子必须仍是 08/10（不变），用户段 23/2B，TSS 30
    dbg64_str("[X64] gdt kcode=08 kdata=10 udata=23 ucode=2B tss=30 limit=");
    dbg64_dec((uint64_t)g_gdtr.limit);
    dbg64_str(" usr_ok=");
    dbg64_dec(gdt_entry_raw64(4) != 0 && gdt_entry_raw64(5) != 0 ? 1 : 0);
    dbg64_nl();
}

// 自检/调试用：返回 GDT 第 index 项的原始 8 字节（未对齐读，按字节拼）。越界返回 0。
uint64_t gdt_entry_raw64(int index) {
    if (index < 0 || index >= (int)(sizeof(g_gdt) / sizeof(g_gdt[0]))) return 0;
    const uint8_t* b = (const uint8_t*)&g_gdt[index];
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

// ==================== PIC ====================
static void pic_remap() {
    outb64(0x20, 0x11); io_wait64();    // ICW1
    outb64(0xA0, 0x11); io_wait64();
    outb64(0x21, 0x20); io_wait64();    // ICW2：主片 -> 0x20
    outb64(0xA1, 0x28); io_wait64();    //       从片 -> 0x28
    outb64(0x21, 0x04); io_wait64();    // ICW3：主片 IRQ2 接从片
    outb64(0xA1, 0x02); io_wait64();
    outb64(0x21, 0x01); io_wait64();    // ICW4：8086 模式
    outb64(0xA1, 0x01); io_wait64();
    outb64(0x21, 0xFF);                 // 先屏蔽全部，由各驱动按需放开
    outb64(0xA1, 0xFF);
}

// ---- APIC 接管后的 EOI / 掩码分派（实现在 kernel/apic64.cpp，只进系统内核）----
// 用 weak 声明：安装程序内核不链接 apic64.cpp，这两个符号为空，下面两个函数自然
// 退化回纯 8259 路径（安装内核 g_irq_mode64 恒为 0，根本不会走到 APIC 分支）。
extern "C" void apic64_eoi64()                   __attribute__((weak));
extern "C" void apic64_unmask_irq64(uint8_t irq) __attribute__((weak));

// 中断结束（EOI）统一入口 —— 按 g_irq_mode64 分派两条完全不同的路径：
//   * PIC 模式（默认）：写 8259 的 EOI（从片 0xA0 先写，再主片 0x20）；
//   * APIC 模式：**必须**写 LAPIC 的 EOI 寄存器（0xB0），写 8259 完全无效
//     （PIC 的 IMR 此时已全 1，8259 也不再投递）—— 漏掉这一步的表现是"中断只来一次"。
// 调用点保持原样（本文件里 PIT/键盘/鼠标/ATA 与 default 分支都走这里）。
static void pic_eoi(uint8_t irq) {
    if (g_irq_mode64 == 1) {                     // APIC：EOI 走 LAPIC（实现在 apic64.cpp）
        if (apic64_eoi64) apic64_eoi64();
        return;
    }
    if (irq >= 8) outb64(0xA0, 0x20);
    outb64(0x20, 0x20);
}

static void pic_unmask(uint8_t irq) {
    const uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    const uint8_t bit = (uint8_t)(irq & 7);
    uint8_t mask = inb64(port);
    mask &= (uint8_t)~(1u << bit);
    outb64(port, mask);
}

// 公开版本：驱动可调用它开放自己的中断线（x86_64.h 有声明）。
// 注意：从片（IRQ8..15）只有主片 IRQ2 也被开放时才会真正到达 CPU。
void pic_unmask64(uint8_t irq) {
    if (g_irq_mode64 == 1) {                     // APIC：改 IOAPIC 重定向表（PIC 已全屏蔽）
        if (apic64_unmask_irq64) apic64_unmask_irq64(irq);
        return;
    }
    if (irq >= 8) {
        pic_unmask(2);          // 级联：先放开主片的 IRQ2
    }
    pic_unmask(irq);
}

// ---- 设备中断入口（驱动提供，weak：未链接时不报错）----
extern "C" void irq1_handler()  __attribute__((weak));    // input.cpp：PS/2 键盘
extern "C" void irq12_handler() __attribute__((weak));    // input.cpp：PS/2 鼠标
extern "C" void irq14_handler() __attribute__((weak));    // 将来的 ATA 完成中断

// ==================== IDT 诊断（排障用，可保留）====================
static void idt_diag_dump() {
    dbg64_str("[X64] isr32=");
    dbg64_hex64((uint64_t)(uintptr_t)isr32_64);
    dbg64_str(" table[32]=");
    dbg64_hex64((uint64_t)(uintptr_t)kIsrTable[32]);
    dbg64_nl();
    const uint8_t* e = (const uint8_t*)&g_idt[32];
    dbg64_str("[X64] idt[32]=");
    for (int i = 0; i < 16; i++) { dbg64_hex64(e[i]); dbg64_str(" "); }
    dbg64_nl();
    dbg64_str("[X64] idt[32] sel=");
    dbg64_hex64((uint64_t)e[2] | ((uint64_t)e[3] << 8));
    dbg64_str(" attr=");
    dbg64_hex64(e[5]);
    dbg64_str(" idtr.limit=");
    dbg64_hex64(g_idtr.limit);
    dbg64_str(" idtr.base=");
    dbg64_hex64(g_idtr.base);
    dbg64_nl();
}

// ==================== PIT（IRQ0，250Hz）====================
void pit_init64(uint32_t freq_hz) {
    if (freq_hz == 0) freq_hz = PIT_HZ_64;
    uint32_t div = 1193182u / freq_hz;
    if (div == 0) div = 1;
    if (div > 65535) div = 65535;
    outb64(0x43, 0x36);                                 // 通道 0，先低后高，方式 3
    outb64(0x40, (uint8_t)(div & 0xFF));
    outb64(0x40, (uint8_t)((div >> 8) & 0xFF));
    pic_unmask(0);
}

uint32_t ticks64() { return (uint32_t)g_ticks64; }

// ==================== 中断分发 ====================
extern "C" void schedule64(pt_regs64* r);       // 弱默认实现见文件末尾
// int 0x80 系统调用分发器（kernel/syscall64.cpp）。weak：未链接时 int 0x80 退化为
// 返回 -1，而不是链接失败（见下面 isr_handler64 的 no==128 分支）。
extern "C" void syscall64_dispatch64(pt_regs64* r) __attribute__((weak));

extern "C" void isr_handler64(pt_regs64* r) {
    const uint64_t no = r->int_no;
    g_irq_total64++;

    if (no == 32) {
        g_ticks64++;
        pic_eoi(0);
        schedule64(r);
        return;
    }
    if (no == 128) {
        // ---- int 0x80：系统调用（自有 ABI，见 kernel/syscall64.h 顶部说明）----
        // 分发器由 kernel/syscall64.cpp 提供；这里用 weak 声明：万一某个内核裁剪掉
        // 了它，也不会链接失败，只是把返回值置 -1 后原样回到用户态。
        if (syscall64_dispatch64) syscall64_dispatch64(r);
        else r->rax = (uint64_t)-1;
        return;
    }
    if (no >= 32 && no < 48) {          // IRQ 32..47
        const uint8_t irq = (uint8_t)(no - 32);
        if (irq == 7 || irq == 15) {    // 伪中断：不回应 EOI
            return;
        }
        // ---- 设备中断路由 ----
        // 驱动提供 C 链接的入口；用 weak 声明，未链接该驱动时不会链接失败。
        if (irq == 1)  { if (irq1_handler)  irq1_handler();  pic_eoi(1);  return; }   // PS/2 键盘
        if (irq == 12) { if (irq12_handler) irq12_handler(); pic_eoi(12); return; }   // PS/2 鼠标（从片）
        if (irq == 14) { if (irq14_handler) irq14_handler(); pic_eoi(14); return; }   // IDE 主通道
        pic_eoi(irq);
        return;
    }
    if (no < 32) {
        // 把出错上下文打全：只知道"异常号"没法定位（UEFI 路径下曾只报 exception 13，
        // 靠 RIP 才认出是在哪条指令）。RIP/CS/RFLAGS/ERR 都来自中断帧。
        dbg64_str("[PANIC] cpu exception ");
        dbg64_dec(no);
        dbg64_str(" err=");
        dbg64_hex64(r->err_code);
        dbg64_str(" rip=");
        dbg64_hex64(r->rip);
        dbg64_str(" cs=");
        dbg64_hex64(r->cs & 0xFFFF);
        dbg64_str(" rflags=");
        dbg64_hex64(r->rflags);
        dbg64_str(" rsp=");
        dbg64_hex64(r->rsp);
        dbg64_str(" cr2=");
        { uint64_t cr2; __asm__ volatile("mov %%cr2, %0" : "=r"(cr2)); dbg64_hex64(cr2); }
        dbg64_nl();
        for (;;) __asm__ volatile("cli; hlt");
    }
}

// ==================== 平台初始化 ====================
void x86_init64() {
    __asm__ volatile("cli");

    pic_remap();
    tss_init(0x80000);                  // rsp0 先用安全区，任务系统落地后会改成任务栈
    idt_build();

    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base  = (uint64_t)(uintptr_t)&g_idt;
    __asm__ volatile("lidt %0" : : "m"(g_idtr));

    pit_init64(PIT_HZ_64);



    idt_diag_dump();

    dbg64_str("[X64] PIC remapped, IDT/TSS loaded, PIT ");
    dbg64_dec(PIT_HZ_64);
    dbg64_str("Hz");
    dbg64_nl();
}

// ==================== RTC（CMOS 0x70/0x71）====================
static uint8_t cmos_read64(uint8_t reg) {
    outb64(0x70, reg);
    return inb64(0x71);
}
static int bcd64(int v) { return (v & 0x0F) + ((v >> 4) * 10); }

void rtc_init64() { (void)cmos_read64(0x00); }

void rtc_get_time64(int* h, int* m, int* s) {
    *h = bcd64(cmos_read64(0x04));
    *m = bcd64(cmos_read64(0x02));
    *s = bcd64(cmos_read64(0x00));
}

void rtc_get_date64(int* y, int* mo, int* d, int* wd) {
    *y  = bcd64(cmos_read64(0x09)) + 2000;
    *mo = bcd64(cmos_read64(0x08));
    *d  = bcd64(cmos_read64(0x07));
    *wd = bcd64(cmos_read64(0x06));
}

// ==================== 调度钩子（弱默认实现）====================
// kernel/task64.cpp 落地后提供强符号覆盖它。
extern "C" __attribute__((weak)) void schedule64(pt_regs64* r) { (void)r; }
