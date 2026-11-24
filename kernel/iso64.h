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
int iso64_smap_supported64(); // 1 = CPU 支持 SMAP（本版**不打开**）
int iso64_smep_supported64();

// 打开硬件隔离位（启动期调一次，进程出现之前）。返回 1 = WP 成功。
int iso64_init64();

// 审计一个地址空间：cr3 = 0 表示"当前 CR3"。pid < 0 时日志打 pid=-。
// 返回 us_violation（0 = 干净）。
int iso64_audit64(uint64_t cr3, int pid, const char* why);
// 审计内核地址空间 + 所有活着的进程地址空间（每个进程一行，外加一行汇总）。返回总违规数。
int iso64_audit_all64(const char* why);

// ★ 活进程审计：造一个真进程（proc64_create64）-> 映一页用户页
//   -> 审计它的地址空间 -> 销毁。返回 1 = 正常、-1 = 不适用（共享空间模式）、0 = 失败。
int iso64_probe64();
// 把 CR0.WP 重新置位（幂等）；返回 1 = 本次真的补了（即之前被清掉了）。
int iso64_reassert64();
int iso64_selftest64();
void iso64_report64();        // 一行现状（启动期/终端可用）
