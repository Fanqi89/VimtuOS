// x86_64.h - Vimtu64 平台层接口（64 位长模式专用）
//
// 与 32 位 kernel/x86.h 的关系：
//   x86.h 描述 32 位保护模式的中断帧/门/时钟；本文件是 64 位版本，两者互不影响。
//   64 位构建使用本文件，32 位构建继续用 x86.h。
//
// ★ 中断帧布局的**权威定义**在 kernel/isr_stubs64.asm 的顶部注释里，本文件的
//   struct pt_regs64 必须与它逐字段一致（字段顺序 = 内存顺序 = push 逆序）。
#pragma once
#include <stdint.h>

// ==================== 中断帧（0xD0 = 208 字节）====================
// 低地址字段在前；保存的帧指针指向 frame 的**最低地址**（gs 槽）。
//
// ★ 这份字段顺序必须与 kernel/isr_stubs64.asm 的 push 顺序严格对应。
//   桩在进 C 之前依次 push：段占位槽×4、GPR×15、int_no/err（由宏压）、
//   再往上才是 CPU 压的 rip/cs/rflags(/rsp/ss)。因为 push 朝低地址走，
//   **最后压的字段在最低地址**，所以内存升序排列正好是下面这个顺序：
//
//     0x00 gs  0x08 fs  0x10 es  0x18 ds        桩压（占位 0x10）
//     0x20 r15 0x28 r14 0x30 r13 0x38 r12       桩压（push r15 最后 → 最低）
//     0x40 r11 0x48 r10 0x50 r9  0x58 r8
//     0x60 rdi 0x68 rsi 0x70 rbp 0x78 rbx
//     0x80 rdx 0x88 rcx 0x90 rax
//     0x98 int_no  0xA0 err_code                CPU 帧之前由桩压入
//     0xA8 rip     0xB0 cs  0xB8 rflags         CPU 压入
//     0xC0 rsp     0xC8 ss                      CPU 压入（仅特权级切换时有效）
//
// 踩坑记录：最初把 ss/rsp/rflags/cs/rip 写在了结构体最前面（0x00 起），
// 结果 int_no 被读成 0（实际读到的是 r14 槽），PIT 中断被当成除零异常 →
// 误判成"帧错位"。实测 raw dump 前 4 个 qword 正是桩压的 0x10 占位槽。
struct pt_regs64 {
    uint64_t gs, fs, es, ds;                        // 0x00..0x18
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;  // 0x20..0x58
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;     // 0x60..0x90
    uint64_t int_no, err_code;                      // 0x98, 0xA0
    uint64_t rip, cs, rflags, rsp, ss;              // 0xA8..0xC8
};

// 常用偏移（供汇编/C 两侧共用，见上面表格）
static const uint32_t PR64_INT_NO  = 0x98;
static const uint32_t PR64_ERR     = 0xA0;
static const uint32_t PR64_RIP     = 0xA8;
static const uint32_t PR64_CS      = 0xB0;
static const uint32_t PR64_RFLAGS  = 0xB8;
static const uint32_t PR64_SIZE    = 0xD0;

// ==================== 中断桩入口（kernel/isr_stubs64.asm 提供）====================
extern "C" {
void isr0_64();  void isr1_64();  void isr2_64();  void isr3_64();
void isr4_64();  void isr5_64();  void isr6_64();  void isr7_64();
void isr8_64();  void isr9_64();  void isr10_64(); void isr11_64();
void isr12_64(); void isr13_64(); void isr14_64(); void isr15_64();
void isr16_64(); void isr17_64(); void isr18_64(); void isr19_64();
void isr20_64(); void isr21_64(); void isr22_64(); void isr23_64();
void isr24_64(); void isr25_64(); void isr26_64(); void isr27_64();
void isr28_64(); void isr29_64(); void isr30_64(); void isr31_64();
void isr32_64(); void isr33_64(); void isr34_64(); void isr35_64();
void isr36_64(); void isr37_64(); void isr38_64(); void isr39_64();
void isr40_64(); void isr41_64(); void isr42_64(); void isr43_64();
void isr44_64(); void isr45_64(); void isr46_64(); void isr47_64();
void isr128_64();                       // int 0x80（系统调用门，DPL=3）

void task_switch_iret64(uint64_t next_frame);      // 不返回：切到目标帧
void task_switch_yield64(uint64_t* prev_slot, uint64_t next_frame);

// 中断分发（isr_stubs64.asm -> 这里）
void isr_handler64(pt_regs64* r);
// 调度钩子（M1 起由 task64.cpp 提供实现；本文件给出弱默认实现）
void schedule(pt_regs64* r);
}

// ==================== 段选择子（GDT 布局，唯一定义见 x86_64.cpp 顶部注释）====================
// ★ 顺序是**硬约束**（将来上 sysret 时用）：sysret 会用 STAR[63:48]=0x1B 反推
//   CS=STAR[63:48]+16=0x2B、SS=STAR[63:48]+8=0x23，所以用户 DS 必须在 index 4、
//   用户 CS 必须在 index 5，TSS 只能退到 index 6/7。改动前先读 x86_64.cpp 的说明。
//   0 null / 1 内核代码 0x08 / 2 内核数据 0x10 / 3 保留 / 4 用户数据 0x23 /
//   5 用户代码 0x2B / 6..7 TSS 0x30
static const uint16_t SEL64_KCODE = 0x08;
static const uint16_t SEL64_KDATA = 0x10;
static const uint16_t SEL64_UDATA = 0x23;    // index 4, DPL=3
static const uint16_t SEL64_UCODE = 0x2B;    // index 5, DPL=3
static const uint16_t SEL64_TSS   = 0x30;    // index 6（占用 6/7 两个 8 字节槽）

// ==================== 平台初始化 ====================
void x86_init64();                      // PIC 重映射 + PIT + IDT + TSS，最后开中断
void idt_set_gate64(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags, uint8_t ist);
void tss_set_rsp0(uint64_t rsp0);
uint64_t gdt_entry_raw64(int index);     // 自检/调试用：GDT 第 index 项原始 8 字节（0 = 越界）
// ==================== PIT 时钟 ====================
#define PIT_HZ_64    250
#define TICK_MS_64   (1000 / PIT_HZ_64)     // 4ms
void pit_init64(uint32_t freq_hz);
extern volatile uint64_t g_ticks64;         // 自启动以来的 tick 数
extern volatile uint64_t g_irq_total64;     // 中断总数（任务管理器性能页用）

// 中断路由模式：0 = 8259 PIC（默认 / 拿不到 APIC 时的降级），1 = LAPIC+IOAPIC 接管。
// 定义在 kernel/x86_64.cpp；kernel/apic64.cpp 成功切换后置 1。EOI 与
// pic_unmask64() 都按它分派（APIC 模式写 LAPIC EOI 0xB0 / 编程 IOAPIC 重定向项）。
extern volatile uint32_t g_irq_mode64;

// PIC 相关（供驱动按需开放中断线）
void pic_unmask64(uint8_t irq);
static inline uint32_t ms_to_ticks64(uint32_t ms) { return (ms + TICK_MS_64 - 1) / TICK_MS_64; }
static inline uint32_t ticks_to_ms64(uint32_t t)  { return t * TICK_MS_64; }
uint32_t ticks64();                          // 低 32 位（与 32 位接口兼容，便于移植 GUI 代码）

// ==================== RTC ====================
void rtc_init64();
void rtc_get_time64(int* h, int* m, int* s);
void rtc_get_date64(int* y, int* mo, int* d, int* wd);

// ==================== 端口 I/O ====================
static inline void outb64(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline uint8_t inb64(uint16_t port) {
    uint8_t v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port)); return v;
}
static inline void io_wait64() { outb64(0x80, 0); }
