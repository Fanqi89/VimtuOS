// apic64.h - LAPIC + IOAPIC 接管中断路由（64 位系统内核专用）
//
// 定位：把中断路由从 8259 PIC 切到 LAPIC + IOAPIC。**只有系统内核链接本模块**
//   （见 build64.sh 的 SRCS_OS）；安装介质内核不调用、不链接它，保持纯 PIC。
//
// 硬要求（比"启用 APIC"优先级更高）：**任何情况下都不许把系统弄砖**。
//   所以本模块的开关是"试探式"的：
//     * 拿不到 ACPI/MADT、LAPIC 基址无效、IOAPIC 地址无效/重定向项不足、
//       LAPIC/IOAPIC 的 MMIO 页在当前页表里没映射（UEFI 固件页表的常见情况）
//       —— 任意一条不满足：**一个硬件寄存器都不写**，打印
//       "[APIC] unavailable reason=<...> -> stay on 8259 PIC"，保持 8259 PIC，系统照常运行。
//     * 切换动作全部在 cli 下完成，且切换前保存 8259 IMR / LAPIC 寄存器 / IOAPIC 重定向表，
//       在线自检失败时**整体回滚**到切换前状态（再退回 PIC）。
//
// 与调度器/驱动的约定：
//   * PIT 仍走 8259 的 IRQ0 逻辑线（GSI 按 MADT 的 ISO 覆盖，QEMU 上是 GSI2），
//     向量仍是 32 —— 不启用 LAPIC 定时器，避免和调度器的时间片口径打架；
//   * 设备中断的 EOI 由 kernel/x86_64.cpp 的 pic_eoi() 按 g_irq_mode64 分派：
//     APIC 模式写 LAPIC 的 EOI 寄存器（0xB0），PIC 模式写 0x20/0xA0；
//   * 驱动调 pic_unmask64(irq) 时，APIC 模式下会转成"放开对应的 IOAPIC 重定向项"。
#pragma once
#include <stdint.h>

// 中断接管模式：0 = 8259 PIC（默认/降级），1 = LAPIC + IOAPIC。
// 定义在 kernel/x86_64.cpp（x86_64.h 声明），成功切换后才置 1。
#define IRQ_MODE64_PIC   0u
#define IRQ_MODE64_APIC  1u

// 尝试启用 LAPIC + IOAPIC 并接管中断路由（幂等）：
//   成功 -> 打开 IRQ0/IRQ1/IRQ12/IRQ14 四条路由、屏蔽 8259，返回 true；
//   任何前置条件不满足 -> 保持 PIC，打印 unavailable/warn 行，返回 false。
// 结尾会调用 apic64_selftest64() 并打印 [APIC] selftest PASS/FAIL mask=<n>；
// 在线自检失败会自动回滚到 PIC（返回 false）。
bool apic64_init64();

// 启动自检：0 = 通过，非 0 = 失败位掩码（位含义见 apic64.cpp 顶部注释）。
// PIC 降级模式下只校验"模式分派自洽"（不假装 APIC 已启用）。
int apic64_selftest64();

// ---- 供 kernel/x86_64.cpp 的 EOI / pic_unmask64 路径调用 ----
// 该文件用 weak 声明引用它们：安装程序内核不链接 apic64.cpp 时不会链接失败。
extern "C" void apic64_eoi64();                   // 写 LAPIC EOI（0xB0）
extern "C" void apic64_unmask_irq64(uint8_t irq); // 放开 IRQ 对应的 IOAPIC 路由
