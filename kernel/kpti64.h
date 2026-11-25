// kpti64.h - KPTI（内核页表隔离 / 双页表）—— v0.11.9
//
// 目标（判据先写死在这儿）：
//   1) 用户地址空间里 **PML4[511] 为 0** —— ring3 跑着的时候，内核高半区一个字节都映射不到；
//   2) 每个进程有**两张** PML4：内核表（完整映射，内核态用）/ 用户表（只保留用户窗口 + 共享入口区）；
//   3) 进出内核的每条路径（int 0x80 / 异常 / IRQ / syscall 指令 / iret 返回）都只经过
//      KPTI64_BASE 这一小段"两张表都映射"的共享代码；
//   4) 上述三条都能被 `iso` 命令和 tests/iso64_test.py **数出来**，不是嘴上说的。
//
// ============================ 共享入口区布局（固定 VA）============================
//   KPTI64_BASE = 0x0000000180000000（6 GiB；落在 PML4[4] → 私有 PDPT 的 [6]，
//   在每一张页表里都挂同一张 PD/PT —— 见 kernel/kpti64.cpp 的 init 与 proc64 的 p64_build_as64）
//     +0x0000  页 0：入口/出口代码 + 变量 + GDT/TSS 副本 + 入口栈/返回栈（来自 kernel/kpti_blob64.asm）
//     +0x1000  页 1：（入口/返回栈跨页部分）
//     +0x2000  页 2：向量偏移表（256 × qword）
//     +0x3000  页 3：IDT（4 KiB，运行时从 g_idt 拷过来）
//   页 0..2 的物理页就是内核镜像里 .text.kpti 那一段（不额外占内存）；页 3 是 page_alloc_64 的页。
//
// ★ 为什么用 6 GiB 这个"看起来像用户地址"的窗口：它既不在内核高半区（PML4[511]），
//   也不在用户窗口（PDPT[4]，4~5 GiB）里；
//   ★ 更不能占 PDPT[5]（5~6 GiB）—— 那是 USER64_FB_VA64 的 framebuffer/DRM 窗口（v0.9 起就有）。
//   用户指针闸门 user64_range_ok64 照样拒 6 GiB，
//   审计里它单独算一类（entry），U/S 必须为 0。
//
// ★ PML4[255] 是"数据槽"：non-present 项里存同一进程的另一张表的物理地址。
//   入口蹦床靠它找到内核表，出口靠它找到用户表 —— 这样**不需要**在每次任务切换时维护
//   一个"当前进程用哪张表"的全局变量（少一处就少一处漏）。见 kpti_blob64.asm 的注释。
#pragma once
#include <stdint.h>

static const uint64_t KPTI64_BASE      = 0x0000000180000000ULL;  // 6 GiB（PDPT[6]）
// ★ 页号**不能写死**：节内布局随代码长度浮动（v0.11.9 实测变过一次 —— 蹦床 0 的偏移
//   从 0 变成 8 之后，偏移表就从页 2 挪到了页 1）。真实页号由 kpti64_init64() 按
//   kpti64_offtab64 的**实际偏移**算出来，下面两个函数只是查询接口。
uint64_t kpti64_offtab_va64(void);       // 向量偏移表在共享区的 VA
uint64_t kpti64_idt_va64(void);          // 共享 IDT 的 VA（偏移表页之后那一页）

// 建立一个可用的双页表体系（幂等）。返回 0 = 已启用 / -1 = 本环境不支持（如实打点）。
int  kpti64_init64(void);

// 1 = KPTI 真的在生效（门已重定向、TSS.rsp0 = 共享入口栈、LSTAR = 共享 syscall 入口、
//     新建进程的用户表不再映射内核高半区）
int  kpti64_on64(void);

// 某个向量在共享区里的蹦床 VA（0 = 没有；未启用时恒 0，免得门被指到没映射的地址）
uint64_t kpti64_tramp64(int vec);
// 同上，但不管有没有启用（只给"搬家时重定向门"用）
uint64_t kpti64_tramp_raw64(int vec);

uint64_t kpti64_entry_rsp064(void);      // 共享入口栈顶（= KPTI 模式下 TSS.rsp0 的值）
void     kpti64_set_kstack64(uint64_t top);   // 本任务真实内核栈顶（帧从入口栈搬到这里）
uint64_t kpti64_syscall_entry_va64(void);   // 共享 syscall 入口 VA（0 = 未启用）
const char* kpti64_reason64(void);       // 没启用的原因（启用时 = "on"）

// 给两个 PML4 互相写对方的物理地址到 [255] 数据槽（proc64 建地址空间时调）
int  kpti64_link_pair64(uint64_t pml4_kern_phys, uint64_t pml4_user_phys);

// 共享区 PD 的物理地址（审计认"这一条是入口区、不是越权映射"用）
uint64_t kpti64_pd_phys64(void);
// PML4[255] 里存的对方表物理地址（0 = 没配对，即"共享地址空间"）
uint64_t kpti64_peer_cr364(uint64_t cr3);

int  kpti64_selftest64(void);            // 6 位判据（见 kpti64.cpp；0 = 全绿）
void kpti64_report64(void);              // 串口一行摘要
