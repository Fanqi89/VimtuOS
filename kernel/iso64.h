// iso64.h - ★ v0.11.8：Ring 3 **真隔离**层（硬件隔离位 + 地址空间审计 + 自检）
//
// 背景（现状事实，别当成已经做到的）：
//   * 每进程有独立 CR3（BIOS 路径 `proc64_init64` 探测通过后 `[PROC64] init mode=isolated`），
//     但 PML4[511]（内核高半区）与 PML4[0]→PDPT[0..3]（前 4GB 恒等映射）是**从引导期页表原样复制**的：
//     内核映射在每个用户地址空间里都存在 —— U/S=0（ring3 访问会 #PF），但**内核自己**没有任何
//     "不能执行用户页 / 不能写只读页" 的硬件约束，而且没有 KPTI 式双页表。
//   * 因此本模块先把**能立刻补上的那层**补齐，并把"没做的那层"如实打点：
//       ① CR0.WP  = 1  —— 内核（CPL0）也不能写只读页（挡住"内核 bug 顺手改坏用户 .text/页表"）
//       ② CR4.SMEP= 1  —— 内核不能**执行**用户页（挡住提权跳板/注入代码被执行）
//       ③ CR4.SMAP      —— **本版不开**：开了之后内核每次读用户内存都要 STAC/CLSTAC，
//                          本内核里用户内存访问点太多（路径字符串/iovec/elf 装载），要单独一轮
//                          逐点包起来；这里只如实报告 CPU 支持与否，绝不假装开了。
//       ④ KPTI 双页表 —— **不存在**（PML4[511] 共享是设计事实）。见 iso64_audit64 的统计。
//
// 审计（真证据，不是嘴说）：
//   `iso64_audit64(cr3, pid, why)` 走一遍四级页表，逐类统计**用户可访问位**：
//     khigh  = PML4[511] 链上的 present 项数（内核高半区）—— **必须全 U/S=0**
//     ident  = PML4[0]→PDPT[0..3] 链上的 present 项数（前 4GB 恒等映射/内核数据/MMIO）—— **必须全 U/S=0**
//     uwin   = PDPT[4]（4GiB..5GiB 用户窗口）里的 present 叶子页—— **必须全 U/S=1**
//     另外把 PML4[0] 的 PDPT[5..511] 判为"窗口外的用户地址"——**必须全不存在**（出现就是漏洞）
//   任何一条不符 -> `us_violation` 计数 +1，日志 ok=0。
//
// 串口打点（验收 grep）：
//   [ISO64] hw wp=1 smep=1 smep_supported=1 smap_supported=0 smap_on=0 cr0=<hex> cr4=<hex> ok=1
//   [ISO64] audit pid=<n|-> space=<hex> khigh=<n> ident=<n> uwin=<n> outside=<n> us_violation=0 ok=1
//   [ISO64] selftest PASS|FAIL mask=<n> （6 位：wp/smep/内核地址被拒/用户地址可用/内核半区审计/窗口审计）
//   [ISO64] mode kpti=0 smap=0 reason=...   （如实：还没做的层）
#pragma once
#include <stdint.h>

// 硬件位现状（给外壳/终端显示用）
int iso64_wp_on64();          // 1 = CR0.WP 已置位
int iso64_smep_on64();        // 1 = CR4.SMEP 已置位
int iso64_smap_supported64(); // 1 = CPU 支持 SMAP（v0.12.0 起：支持就打开）
int iso64_smep_supported64();

// 打开硬件隔离位（启动期调一次，进程出现之前）。返回 1 = WP 成功。
int iso64_init64();

// 审计一个地址空间：cr3 = 0 表示"当前 CR3"。pid < 0 时日志打 pid=-。
// 返回 us_violation（0 = 干净）。
int iso64_audit64(uint64_t cr3, int pid, const char* why);
// ★ v0.11.9（KPTI）：审计**用户表** —— 判据反过来：内核高半区必须不存在（khigh=0）。
int iso64_audit_user64(uint64_t cr3, int pid, const char* why);
// 审计内核地址空间 + 所有活着的进程地址空间（每个进程一行，外加一行汇总）。返回总违规数。
int iso64_audit_all64(const char* why);

// ★ 活进程审计：造一个真进程（proc64_create64）-> 映一页用户页
//   -> 审计它的地址空间 -> 销毁。返回 1 = 正常、-1 = 不适用（共享空间模式）、0 = 失败。
int iso64_probe64();
// 把 CR0.WP 重新置位（幂等）；返回 1 = 本次真的补了（即之前被清掉了）。
// ★ v0.12.0：它**不再**当启动期兜底用（那会把判据自己抹掉）；只在"确实掉了"时救人一命并留痕。
int iso64_reassert64();
// ★ v0.12.0：**只查不补**的 WP 现场检查（判据）。日志 `[ISO64] wp check at <where> wp=<0|1> cr0=<hex>`。
//   返回 1 = WP 此刻在。掉了就额外打 `[ISO64] wp leak detected at <where>` 并补上（先保住防护），
//   但**不掩盖**：调用方/验收脚本看到那行就是 FAIL。
int iso64_check_wp64(const char* where);
// ★ v0.12.0：SMAP —— 内核访问用户页必须显式开窗（RFLAGS.AC）。
//   用法：在**确实要读写用户内存**的那一小段前后成对调用
//       iso64_stac64();  …用户访存…  iso64_clac64();
//   SMAP 没开（CPU 不支持 / 未置位）时两者都是**空操作**，同一份代码在两种 CPU 上都对。
//   纪律：STAC 窗口里只放真正的用户访存，别在中间调别的内核函数（窗口一大就等于没开）。
//   ★ 为什么全是 weak：`kernel/iso64.cpp` 只在**桌面内核**（build64.sh 的 SRCS_DESKTOP）里链接，
//     基础/安装介质内核没有它。weak 声明 + 下面那三个带判空的宏 = "没链接就是空操作"，
//     链接期不会因为少一个模块而失败（项目里 weak 钩子本来就是这套口径）。
void iso64_stac64()          __attribute__((weak));
void iso64_clac64()          __attribute__((weak));
int  iso64_smap_on64();           // 1 = CR4.SMAP 已置位
int  iso64_smap_violations64();   // 本次启动累计的内核态 SMAP 违规数（0 才是干净）
// 内核态 #PF 的 SMAP 违规处置（由 x86_64.cpp 的 isr_handler64 调用）。
// 返回 1 = 已处理（探针模式：关掉 SMAP 让指令重试；默认：只杀当前进程，绝不 PANIC 整机）；0 = 交回 PANIC。
int  iso64_smap_fault64(void* regs) __attribute__((weak));
int iso64_selftest64();
// ★ v0.12.0：从 ring3 进内核的第一件事（isr_handler64 / syscall 分发器开头调）—— AC=0，
//   内核侧永远从"用户页不可访问"开始，不继承上一次开窗留下的状态。
void iso64_clac_on_entry64() __attribute__((weak));
// ★ v0.12.0：PIT 钩子（只有探针模式会做事：定时把 SMAP 重新武装，用于把违规点成串收全）。
void iso64_tick64()          __attribute__((weak));
void iso64_report64();        // 一行现状（启动期/终端可用）
// 带判空的调用宏：没链接 iso64.cpp 的内核里这些符号为 0 -> 直接跳过（零行为改变）。
#define ISO64_STAC64()        do { if (iso64_stac64)          iso64_stac64();          } while (0)
#define ISO64_CLAC64()        do { if (iso64_clac64)          iso64_clac64();          } while (0)
#define ISO64_CLAC_ON_ENTRY() do { if (iso64_clac_on_entry64) iso64_clac_on_entry64(); } while (0)
#define ISO64_TICK64()        do { if (iso64_tick64)          iso64_tick64();          } while (0)
#define ISO64_SMAP_FAULT(r)   ((iso64_smap_fault64) ? iso64_smap_fault64(r) : 0)

#ifdef __cplusplus
// ★ v0.12.0：给"整函数都是用户访存"的地方用的作用域守卫（任何 return 路径都会关窗）。
//   用法：函数第一行 `Iso64UserAccess64 ua;`。
struct Iso64UserAccess64 {
    Iso64UserAccess64()  { ISO64_STAC64(); }
    ~Iso64UserAccess64() { ISO64_CLAC64(); }
};
#endif
