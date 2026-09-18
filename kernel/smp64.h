// smp64.h - SMP：启动 AP（INIT-SIPI-SIPI + 低端跳板），**只进系统内核**
//
// 定位（本阶段做到哪、不做到哪，别误读）：
//   * 做到：BSP 用 LAPIC 的 ICR 对每个 AP 走标准启动序列（INIT assert -> 等 10ms ->
//     SIPI -> 等 200us -> 再 SIPI），AP 从低端跳板进 64 位世界、用**自己的栈**、
//     递增在线计数、自己打一行 "[SMP] ap id=<n> online"，然后 cli+hlt 停住。
//   * 不做到（明确的范围外）：多核调度（调度器仍是单核，IRQ0 只在 BSP 上跑）、
//     IPI、per-CPU 数据/TSS/GDT、AP 自己的 LAPIC 接管、AP 参与任何设备中断。
//     —— 所以 AP 起来之后只是"在线且不捣乱"。
//
// ★ 三条硬规矩（与 apic64.cpp 同一套价值观：宁可不启动，也不许把系统弄砖）：
//   1) 只读探测在前：跳板页是否在**当前页表**里可写、LAPIC 基址是否有效、MADT 能不能
//      给出 APIC ID —— 任一条不满足：一行日志 + 优雅返回，一个硬件寄存器都不写。
//   2) 启动全程**有界等待**：每个 AP 最多等 SMP64_AP_ONLINE_TIMEOUT_MS（用 g_ticks64
//      计时 + 硬自旋上界兜底），超时打 "[SMP] ap id=<n> timeout" 后继续下一个，
//      绝不挂在启动里。
//   3) 自检在**发 SIPI 之前**跑：只校验"我们自己准备的东西"（跳板字节、共享块、
//      GDT 拷贝、每核栈、IDTR）。AP 没起来是**环境事实**，不是自检失败 ——
//      发起 SIPI 之后不再改 selftest 结论（只在 online= 行里如实报数）。
#pragma once
#include <stdint.h>

// ==================== 跳板（kernel/ap_trampoline64.asm，按 0x8000 汇编）====================
// ★ 为什么是 0x8000：见 ap_trampoline64.asm 顶部（0x0F00/0x1000/0x2000/0x7000/0x7400/
//   0x7600/0x7800/0x7BFF/0x9000/0x40000../0x100000 全都占了；0x8000..0x8FFF 是唯一
//   页对齐、落在 64KB 实模式寻址范围内、且不与既有布局冲突的低端页）。
//   SeaBIOS 自己的 trampoline 区也是 0x8000-0x9000，但那是 **BIOS POST 期间**的事 ——
//   内核跑到这里（os_boot_path）时 SeaBIOS 早已不再使用这一页。
static const uint32_t SMP64_TRAMPOLINE_PHYS = 0x8000;   // 也 = SIPI 向量 << 12
static const uint32_t SMP64_SIPI_VECTOR     = 0x08;     // SIPI 向量：CS=0x0800, IP=0
// 页内固定偏移（必须与 ap_trampoline64.asm 的 %define 一致；自检会读回核对）
static const uint32_t SMP64_OFF_GDTR        = 0x40;     // dw limit=63, dd base=0x8C00
static const uint32_t SMP64_OFF_PM32        = 0x60;     // 32 位保护模式段
static const uint32_t SMP64_OFF_LM64        = 0xC0;     // 64 位入口桩
static const uint32_t SMP64_OFF_SHARED      = 0x800;    // 共享数据块
static const uint32_t SMP64_OFF_GDT_COPY    = 0xC00;    // 临时 GDT 拷贝目标
static const uint32_t SMP64_RM_STACK_TOP    = 0x8F00;   // 实模式栈顶（SS=0，向下）

// 共享块内的字段偏移（相对 SMP64_OFF_SHARED；与 asm 的 S_* 一致）
static const uint32_t SMP64_S_MAGIC    = 0x00;   // u32 'SMPT'
static const uint32_t SMP64_S_INDEX    = 0x04;   // u32 AP 序号（1 起）
static const uint32_t SMP64_S_APICID   = 0x08;   // u32 目标 LAPIC ID
static const uint32_t SMP64_S_VECTOR   = 0x0C;   // u32 SIPI 向量
static const uint32_t SMP64_S_CR3      = 0x10;   // u64 内核 PML4（BSP 当前 CR3）
static const uint32_t SMP64_S_CR0      = 0x18;   // u64 BSP CR0 | PE|PG
static const uint32_t SMP64_S_CR4      = 0x20;   // u64 BSP CR4 | PAE
static const uint32_t SMP64_S_EFER     = 0x28;   // u64 BSP EFER | LME
static const uint32_t SMP64_S_ENTRY    = 0x30;   // u64 64 位入口（内核高半区函数）
static const uint32_t SMP64_S_RSP      = 0x38;   // u64 本 AP 的内核栈顶（16B 对齐）
static const uint32_t SMP64_S_FLAGPTR  = 0x40;   // u64 本 AP 在线标志的指针
static const uint32_t SMP64_S_COUNTPTR = 0x48;   // u64 在线原子计数指针
static const uint32_t SMP64_S_GDT      = 0x50;   // u8[64] 临时 GDT（AP 拷到 0x8C00）
static const uint32_t SMP64_S_IDTR     = 0x90;   // u8[10] BSP 的 IDTR（AP 也 lidt）
static const uint32_t SMP64_SHARED_MAGIC = 0x54504D53u;  // 'SMPT'（小端）

// ==================== 规模/超时 ====================
static const uint32_t SMP64_MAX_AP          = 8;        // 最多启动多少个 AP
static const uint32_t SMP64_AP_STACK_PAGES  = 4;        // 每核 4 页 = 16KB 内核栈
static const uint32_t SMP64_AP_STACK_BYTES  = SMP64_AP_STACK_PAGES * 4096u;
static const uint32_t SMP64_AP_ONLINE_TIMEOUT_MS = 500; // 每个 AP 的有界等待（g_ticks64 计时）

// ==================== 接口 ====================
// BSP 侧启动流程（幂等；任何前置条件不满足都优雅返回 0，绝不挂死、绝不变砖）：
//   查 CPU 数 -> 准备跳板/共享块/每核栈 -> 自检(发 SIPI 之前) -> 逐个 INIT-SIPI-SIPI
//   -> 打 "[SMP] online=<n>/<total> bsp_lapic_id=<n> trampoline@<hex>"。
// 返回 0 = 走完流程（**不代表每个 AP 都起来了**：AP 未起是环境事实，看 online= 行）；
// 非 0 = 自检失败位掩码（此时不发 SIPI，也不启动任何 AP）。
int smp64_init64();

// 启动自检（0 = 通过，非 0 = 失败位掩码；位定义见 smp64.cpp）。
// 口径：**只查"BSP 自己准备好的东西"**，必须在发起 SIPI 之前调用；发起之后 AP 起没起
// 不影响它的结论（那属于环境事实，在 online= 行里报数）。
// 内部打印 "[SMP] selftest PASS" 或 "[SMP] selftest FAIL mask=<n>"。
int smp64_selftest64();

// AP 侧的 64 位入口（由跳板的 64 位桩 jmp 进来，不返回）。
// 它由 AP 自己执行：**先自己打一行 online 日志** -> 原子递增在线计数 -> 置在线标志
// （顺序见 smp64.cpp：BSP 只等"标志"，标志最后置 => 两边日志不会交错）
// -> cli+hlt 停住（本阶段 AP 不参与调度、不接中断）。
extern "C" void smp64_ap_entry64();

// 在线 CPU 数（含 BSP）：1 = 只有 BSP（未启动任何 AP）。
uint32_t smp64_online_cpu_count64();
