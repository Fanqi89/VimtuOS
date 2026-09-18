// debug64.h - 64 位内核启动期最早期调试输出（无需任何模块初始化）
//
// 用途：从进入长模式的第一条 C++ 语句开始就能打日志。内核还没有 IDT、没有堆、
//       没有任务系统时，唯一可靠的输出通道就是 I/O 端口：
//         * 0x402 = QEMU debugcon（-debugcon file:...）
//         * 0x3F8 = COM1 串口（-serial file:...）
//       两条同时写，QEMU/VMware/真机串口都能看到。
//
// 注意：在 IDT 未就绪的阶段只允许用 hlt 而非中断等待，调用方自己保证。
#pragma once
#include <stdint.h>

static inline void dbg64_outb(uint16_t port, uint8_t v) {
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline void dbg64_putc(char c) {
    dbg64_outb(0x402, (uint8_t)c);
    // COM1：等发送保持寄存器空（LSR bit5），带超时，避免无串口时死等
    for (uint32_t spin = 0; spin < 0x10000; spin++) {
        uint8_t lsr;
        __asm__ volatile("inb %1, %0" : "=a"(lsr) : "Nd"((uint16_t)0x3FD));
        if (lsr & 0x20) break;
    }
    dbg64_outb(0x3F8, (uint8_t)c);
}

static inline void dbg64_str(const char* s) {
    while (*s) dbg64_putc(*s++);
}

// 以 16 进制打印 64 位值（固定 16 位宽，便于日志比对）
static inline void dbg64_hex64(uint64_t v) {
    static const char* H = "0123456789ABCDEF";
    char buf[17];
    for (int i = 15; i >= 0; i--) { buf[i] = H[v & 0xF]; v >>= 4; }
    buf[16] = 0;
    dbg64_str(buf);
}

static inline void dbg64_dec(uint64_t v) {
    char tmp[21]; int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n > 0) dbg64_putc(tmp[--n]);
}

static inline void dbg64_nl() { dbg64_str("\r\n"); }

// ==================== 行级原子（多任务/中断里打日志不互相插行）====================
// 背景：任务会被 PIT 抢占；两条日志同时写 COM1/debugcon 时，一行的中间会被另一行
//   插进去（实测：user64 验收偶发缺 "[USER64] map code="，日志里前导 '[' 被相邻
//   的 "[TASK64] reap" 吞掉）。做法：在**一条完整日志行**的第一条 dbg64_* 之前调用
//   dbg64_line_begin64()，在收尾的 dbg64_nl() 之后调用 dbg64_line_end64()。
//
// 实现（单核）：先 cli，再拿一个自旋锁标志 + 重入深度；end 时按保存的 IF 决定是否 sti。
//   * 持锁期间 IF=0（行很短），代价是这段时间 PIT 中断被推迟；
//   * 从 ISR/ring0 关键区（IF=0）调用时不会把中断提前打开（不会误 sti）；
//   * 单核下"锁已被占用"只可能是同一条行重入（持锁者 IF=0，别的上下文进不来），
//     所以重入只加深度、不会死锁；自旋写法留给将来 SMP。
inline volatile int      g_dbg64_line_lock  = 0;   // 0=空闲 1=正在写一行
inline volatile int      g_dbg64_line_depth = 0;   // 同一条行内的 begin/end 嵌套深度
inline volatile uint64_t g_dbg64_line_flags = 0;   // 拿锁时的 RFLAGS（bit9=IF，用于恢复）

static inline uint64_t dbg64_irq_save64() {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(fl) : : "memory");
    return fl;
}
static inline void dbg64_irq_restore64(uint64_t fl) {
    if (fl & 0x200ULL) __asm__ volatile("sti" ::: "memory");
}

static inline void dbg64_line_begin64() {
    // ★ 先查锁、后关中断：如果"锁已被占用"（单核下只可能是本上下文重入，或出现了
    //   begin/end 不配对的笔误），只加深度、**不再动 IF**。这样即使将来漏写一个
    //   end，也不会把中断永久关掉把系统冻住（实测踩过一次：漏一个 end -> 桌面卡死）。
    if (g_dbg64_line_lock) {
        g_dbg64_line_depth++;
        return;
    }
    const uint64_t fl = dbg64_irq_save64();
    g_dbg64_line_lock  = 1;
    g_dbg64_line_depth = 1;
    g_dbg64_line_flags = fl;
}

static inline void dbg64_line_end64() {
    if (g_dbg64_line_depth <= 0) return;              // 不配对调用：不动状态、不动 IF
    if (g_dbg64_line_depth > 1) { g_dbg64_line_depth--; return; }
    g_dbg64_line_depth = 0;
    g_dbg64_line_lock  = 0;
    dbg64_irq_restore64(g_dbg64_line_flags);
}

// COM1 初始化（115200 8N1）。loader 已经初始化过一次，内核早期再确保一次。
static inline void dbg64_serial_init() {
    dbg64_outb(0x3F9, 0x00);   // 关中断
    dbg64_outb(0x3FB, 0x80);   // DLAB=1
    dbg64_outb(0x3F8, 0x01);   // 除数低 = 1 -> 115200
    dbg64_outb(0x3F9, 0x00);
    dbg64_outb(0x3FB, 0x03);   // 8N1
    dbg64_outb(0x3FA, 0xC7);   // FIFO 使能 + 清空
    dbg64_outb(0x3FC, 0x0B);   // RTS/DSR
}
