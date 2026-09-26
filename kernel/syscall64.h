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
//   6 open(path)         批次 B 起**真实现**：走 kernel/fd64.cpp 的 FD 层（只读；单层路径 "/name"）；
//                         失败返回 -1（不做假成功）。
//   7 read(fd, buf, len) 批次 B 起**真实现**：fd >= 3 从 FD 层读（单次上限 4096B）并拷进用户 buf；
//                         buf 先过 user64_range_ok64；fd 非法/越界 -> -1。
//   8 close(fd)          批次 B 起**真实现**：释放 FD 层的槽；fd 非法 -> -1。
//   ★ 这条路径的**号段与既有行为其余部分不动**（app64/demo64/既有测试都靠它）。
//
//   ==================== A1：用户态绘图（ring3 自己画屏；内核只映射显存 + 提交区域）====================
//   9  fb_map(out_va**, out_info**)      把内核的**后备缓冲**（back buffer）按**用户可读写**页表
//                                        映射进**当前进程**的地址空间，返回用户态 VA + 几何。
//                                        rdi = 用户指针（写 8 字节 VA），rsi = 用户指针（写 32 字节
//                                        Fb64Info，布局见本文件下面）；返回 0 = 成功。
//                                        **幂等**：同一个地址空间重复调用返回同一个 VA（页表已存在
//                                        就直接复用，绝不重复分配/重复映射）。
//   10 fb_flip(x, y, w, h)               把用户刚画好的矩形从后备缓冲**提交到屏幕**（LFB）。
//                                        rdi/rsi/rdx/r10 = x/y/w/h（有符号）；返回 0 = 已提交
//                                        （含**夹取**后的部分提交）、1 = 完全越界被拒（打点 clip=reject，
//                                        什么都不提交、不崩）。
//   11 fb_present()                      整屏提交（缩放模式下的整帧路径），返回 0。
//   错误码（负数，两个入口的 errno 风格一致，**不与 Linux 号段共用号**）：
//     -1 = EPERM  用户窗口/页表不可用（UEFI 固件只读页表，见 kernel/usermode64.cpp）
//     -2 = EFAULT 用户指针非法（没通过 user64_range_ok64）
//     -3 = ENODEV 没有帧缓冲（fb 未初始化 / 没有后备缓冲）
//     -4 = ENOMEM 页表页不足（映射失败）
//   打点（自动验收 tests/fbmap64_test.py grep，格式勿改）：
//     [FB64] map pid=<n> va=<hex> pa=<hex> w=<n> h=<n> pitch=<n> fmt=<n> pages=<n> re=<0|1> u=<0|1>
//     [FB64] flip pid=<n> x=<n> y=<n> w=<n> h=<n> clip=ok|clamped|reject
//     [FB64] present pid=<n> w=<n> h=<n>
//     [FB64] map FAILED pid=<n> reason=<...> err=<n>
//   ★ 越界一律"夹取 or 拒绝"，绝不让用户参数直接进内核的绘制路径（不越界、不崩）。
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


// ==================== A1：用户态绘图接口（fb_map/fb_flip/fb_present）====================
// 结构体布局是**内核与用户程序之间的 ABI**：改字段必须同时改 docs/应用层与系统调用说明.md
// 的"用户态绘图与事件接口（A1）"一节与 user/fbdemo.asm 的读法（它按固定偏移读）。
struct Fb64Info {
    uint32_t width;      // +0  后备缓冲宽（像素；= 渲染分辨率，见 fb_width()）
    uint32_t height;     // +4  高
    uint32_t pitch;      // +8  每行字节数（= width*4）
    uint32_t format;     // +12 像素格式：0 = XRGB8888（0x00RRGGBB，内存里低字节是蓝）
    uint64_t size;       // +16 后备缓冲总字节数（= height*pitch）
    uint64_t va;         // +24 用户态可读写的映射基址（与 out_va 相同）
};
static const uint32_t SYSCALL64_FB_INFO_SIZE64 = 32;
static const uint32_t SYSCALL64_FB_FMT_XRGB8888 = 0;

// 错误码（见本文件头部的 A1 段）：负数，返回值放 rax。
static const int64_t SYSCALL64_FB_EPERM64  = -1;   // 用户窗口不可用（UEFI 固件页表）
static const int64_t SYSCALL64_FB_EFAULT64 = -2;   // 用户指针非法
static const int64_t SYSCALL64_FB_ENODEV64 = -3;   // 没有帧缓冲/后备缓冲
static const int64_t SYSCALL64_FB_ENOMEM64 = -4;   // 映射失败（页表页不足）
// fb_flip 的返回码：0 = 已提交（可能被夹取）、1 = 完全越界被拒（打点 clip=reject）
static const int64_t SYSCALL64_FB_FLIPPED64 = 0;
static const int64_t SYSCALL64_FB_REJECT64  = 1;
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
