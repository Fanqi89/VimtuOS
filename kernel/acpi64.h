// acpi64.h - ACPI 表解析（RSDP -> RSDT/XSDT -> FADT/MADT/HPET/MCFG）
//
// 定位：64 位内核的"平台信息采集"模块。只把固件已经放好的表读出来给上层用
//       （电源管理 / CPU 个数显示 / 将来的 APIC、SMP、PCIe ECAM）：
//         * 不做 AML 解释（DSDT/SSDT 一律不碰）
//         * 不切换 ACPI 模式（不写 SMI_CMD）、不改 SCI 路由、不接管中断
//         * 唯一一处写硬件是 acpi_poweroff64() 的 PM1a_CNT 一次写
//
// 前提（务必知道）：boot/loader64.asm 保留了前 4GB 恒等映射，物理地址可以直接
//       当指针用 —— 见 kernel/memlayout64.h 的 ML64_IDENTITY_BYTES。
//       因此本模块不依赖内核堆、不依赖分页服务，启动早期任何一刻都能调用。
//
// 调用时机建议：kernel64.cpp 里紧跟 mem_selftest_64() 之后、x86_init64() 之前
//       （不需要中断，早调用早拿到 SCI/PM1a 信息给后面的电源管理用）。
#pragma once
#include <stdint.h>

// 解析 RSDP 与它指向的所有表。找不到 RSDP 时打印一行日志后优雅返回：
// 不崩、不写任何端口、后续 getter 全部返回 0/回退值。可重复调用（幂等）。
void acpi_init64();

// 启动期自检：返回 0 = 全部通过，非 0 = 失败项位掩码（位含义见 acpi64.cpp 里的注释）。
// 内部打印 "[ACPI64] selftest PASS" 或 "[ACPI64] selftest FAIL mask=<n>"。
int acpi_selftest64();

// ==================== 基本信息 ====================
uint64_t acpi_rsdp_addr64();     // RSDP 物理地址；0 = 没找到
uint8_t  acpi_revision64();      // RSDP 的 ACPI 版本号（0/1 = ACPI 1.0，>=2 = ACPI 2.0+）

// ==================== CPU / APIC（来自 MADT "APIC"）====================
int      acpi_cpu_count64();     // MADT 里 enabled 的本地 APIC 数（含 x2APIC）；拿不到时回退为 1
int      acpi_ioapic_count64();  // 记录到的 IOAPIC 个数
uint64_t acpi_ioapic_addr64(int i);        // 第 i 个 IOAPIC 的物理基址；i 越界返回 0
uint32_t acpi_ioapic_gsi_base64(int i);    // 第 i 个 IOAPIC 的 GSI 基址；i 越界返回 0
uint64_t acpi_lapic_madt_addr64();         // MADT 给出的本地 APIC 物理地址（含 type 5 覆盖）
int      acpi_pcat_compat64();   // MADT flags bit0：1 = 同时存在 8259（PCAT_COMPAT）

// ==================== 电源管理 / 设备 ====================
int      acpi_sci_irq64();       // FADT 的 SCI_INT（SCI 中断向量）；无 FADT 时 0
uint64_t acpi_reset_reg_addr64(); // FADT RESET_REG 的 IO 端口；0 = 不可用（内存空间/缺失）
uint8_t  acpi_reset_reg_value64();// 写 RESET_REG 的值（硬复位备用）
uint64_t acpi_hpet_addr64();     // HPET 寄存器块物理地址；无 HPET 表时 0
uint64_t acpi_mcfg_base64();     // 首个 MCFG ECAM 基址；无 MCFG 或无条目时 0
uint32_t acpi_mcfg_segment_count64(); // MCFG 里的段组数量（每项 16 字节）；无 MCFG 时 0

// 尝试软关机（S5）：往 FADT 的 PM1a_CNT_BLK 写 SLP_TYPa=5 | SLP_EN。
// 拿不到 FADT / PM1a_CNT_BLK 不是合法端口时**一个端口都不写**并返回 false，
// 调用方可以安全地继续走 8042 / 0x604 的回退链（见 kernel/gui64.cpp 的 sys_shutdown64）。
bool acpi_poweroff64();
