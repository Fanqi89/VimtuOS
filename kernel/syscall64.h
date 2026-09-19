// syscall64.h - 两个系统调用入口：自有 ABI 的 int 0x80 + Linux ABI 的 syscall 指令
//
// ============================ 入口 1：int 0x80（Vimtu64 自有 ABI）============================
//   rax = 调用号；rdi / rsi / rdx = 参数 1/2/3；返回值放 rax（负数 = 错误）。
//   门：IDT[128] 装成 0xEE（P=1、DPL=3 中断门，见 kernel/x86_64.cpp 的 idt_build），
//       所以 ring3 直接 int 0x80 即可；内核侧入口 isr128_64 -> isr_handler64 ->
//       syscall64_dispatch64（本文件）。
//
// 调用号：
//   1 write(fd, buf, len)   fd=1：输出到 串口 + 屏幕。buf 是**用户态地址**，必须通过
//                           user64_range_ok64（用户窗口内、已映射为用户页），越界返回 -1。
//   2 exit(code)            结束 ring3：由 usermode64 改写系统调用帧回到内核（见其说明）
//   3 getpid()              当前任务 id（没有调度器的内核返回 0）
//   4 ticks()               PIT tick 数（250Hz）
//   5 sleep_ms(ms)          睡眠（走 task_sleep64；没有调度器时按 tick 忙等）
//   6/7/8 open/read/close   **未实现**：需要挂载卷才能有真实语义，这里如实返回 -1
//                           （不做假成功；要用时先把 vfs64 的挂载卷接进来）
//   ★ 这条路径的行为与号段**不动**（app64/demo64/既有测试都靠它）。
//
// ============================ 入口 2：syscall 指令（Linux x86_64 ABI）============================
//   寄存器约定与 Linux 完全一致：rax = 调用号；rdi/rsi/rdx/r10/r8/r9 = 参数 1..6；
//   返回 rax（负值 = -errno）；rcx/r11 被 CPU 覆盖（Linux 也一样，属于易失）。
//   入口汇编在 kernel/syscall_entry64.asm（LSTAR 指向它）：SYSCALL 不换栈，所以入口
//   自己切到内核栈、按 isr_stubs64.asm 的 0xD0 布局压出一份完整用户帧，再调本文件的分发器。
//   帧结构与 int 0x80 **完全相同**，只有 int_no 槽不同：syscall 指令路径填
//   SYSCALL64_INSM_FRAME_MARK64，分发器靠它分流（两条路径的号段语义互不影响）。
//
//   号段映射（完整表 + 每条的实现状态见 kernel/syscall64.cpp 顶部的大注释表）。
//   能做到的**真做**，做不到的一律 -ENOSYS(-38) 并且每个号只打一次
//   `[SYSCALL] enosys nr=<n>`（避免刷屏），绝不假装成功。
//
// 串口打点策略（避免刷屏；自动验收 grep 用，格式勿改）：
//   [SYSCALL] nr=<n> rdi=<hex> rsi=<hex> rdx=<hex> ret=<hex>        int 0x80：只对 write(1)/exit(2)
//   [SYSCALL] insn nr=<n> rdi=<hex> rsi=<hex> rdx=<hex> ret=<hex>   syscall 指令：每个调用都打
//                                                                  （调用量小，证据链最完整）
//   [SYSCALL] deny nr=<n> arg=<ptr>                                 任何安全校验失败（两条路径共用）
//   [SYSCALL] enosys nr=<n>                                         未实现的号（每号只打一次）
//   [SYSCALL] msr init ...                                          MSR/STAR/LSTAR/FMASK 初始化留证
//   [SYSCALL] selftest PASS / [SYSCALL] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>
#include "x86_64.h"

// 启动期调用：装 int 0x80 门 + 配 syscall 指令的 MSR（SCE/STAR/LSTAR/FMASK）+ 自检。幂等。
void syscall64_init64();
int  syscall64_selftest64();                     // 位掩码自检，0 = 全过

// 中断分发入口：isr_handler64 在 no == 128 时调用（x86_64.cpp 里是 weak 引用）
extern "C" void syscall64_dispatch64(pt_regs64* r);

// ==================== syscall 指令路径（给汇编入口 / usermode64 用）====================
// 帧标记：syscall 指令路径的 int_no 槽填这个值（int 0x80 是 0x80）。改它必须同步
// kernel/syscall_entry64.asm 的 %define FRAME_MARK。
#define SYSCALL64_INSM_FRAME_MARK64 0x180ULL


// LSTAR 的目标（kernel/syscall_entry64.asm）。syscall64_init_msr64() 把它写进 LSTAR。
extern "C" void syscall64_insn_entry64();

// 入口出口开关：1 = 本帧的 exit 已把控制权交回内核（放行时不要 sysretq，走 ring0 蹦床）。
// 由分发器的 exit 分支置位，入口汇编读它。
extern "C" uint64_t g_syscall64_exit_to_kernel64;
// 内核栈顶（**当前任务**的 SYSCALL 入口专用栈）。SYSCALL 不换栈，入口靠这个值切栈。
// 批次 C 起它是**每任务一份**的：
//   * 有调度器时由 task64.cpp 的 task_apply_ctx64() 在每次任务切换时更新
//     （= 该任务内核栈顶下方 4KB；见 task64.cpp 里"为什么不能共用一块"的说明）；
//   * 任务 0 / 没有调度器（安装介质内核）时等于下面那个静态专用栈顶，值不变。
// usermode64.cpp 每次进 ring3 前也会把它对齐一次（与 tss_set_rsp0 同一处）。
extern "C" uint64_t g_syscall64_kstack64;
// 静态专用栈顶（.bss 里那块 16KiB；任务 0 与无调度器场景用它）—— task64/ usermode64 用它兜底
uint64_t syscall64_static_kstack_top64();
