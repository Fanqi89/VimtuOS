// task64.h - VimtuOS 64 位内核任务与调度器（多任务）
//
// 设计要点（为什么这么做）：
//   * 任务上下文**复用中断帧**（pt_regs64，0xD0 字节），不另立一套寄存器保存格式 ——
//     唯一权威定义在 kernel/isr_stubs64.asm 顶部，改帧布局必须同步三处。
//   * 抢占发生在 IRQ0（PIT 250Hz）里：isr_handler64 先 pic_eoi(0) 再调 schedule64(r)，
//     调度器把当前任务的中断帧指针存进 TCB，然后 task_switch_iret64(next->frame) 抬栈 iretq。
//     因此"切换"不需要任何额外汇编，switch64.asm 的 yield 版也不参与（保留备用）。
//   * 每个任务有独立内核栈（内核堆分配，连续内存），初始帧手工构造，rip 指向统一
//     蹦床 task_trampoline64：蹦床读 g_cur 取 TCB 里的 entry/arg 再调用。
//   * 时间片 = TASK64_SLICE_TICKS 个 tick（4ms/tick）。内核线程要"礼貌"：
//     长循环里主动 task_yield64()，否则桌面外壳（任务 0）会被饿着。
//
// 自动验收断言（串口，tests/sched64_test.py 读日志判定）：
//   [TASK64] scheduler up tasks=N    调度器接管
//   [TASK64] kheart beat=N           内核线程被真实调度（N 会持续增长）
//   [TASK64] ksum done sum=0x...     任务跑完并自杀
//   [TASK64] reap name=ksum          退出回收路径生效
//   [TASK64] selftest PASS           帧布局 / 轮转 / 槽位自检
#pragma once
#include <stdint.h>
#include "x86_64.h"     // pt_regs64 / PR64_SIZE / task_switch_iret64

// ---- 任务状态 ----
enum Task64State : uint32_t {
    TASK64_FREE    = 0,
    TASK64_READY   = 1,
    TASK64_RUNNING = 2,
    TASK64_SLEEP   = 3,
    TASK64_DEAD    = 4
};

static const int      TASK64_MAX         = 16;
static const int      TASK64_NAME_MAX    = 16;
static const uint32_t TASK64_STACK_BYTES = 16 * 1024;   // 每任务 16KB 内核栈（内核堆分配）
static const uint32_t TASK64_SLICE_TICKS = 2;           // 时间片 = 2 tick = 8ms

// 任务信息快照（任务管理器 / terminal ps 用；纯值拷贝，不暴露内部指针）
struct Task64Info {
    uint32_t id;
    uint32_t state;                    // Task64State
    char     name[TASK64_NAME_MAX];    // ASCII
    uint64_t ticks;                    // 累计运行 tick
    uint64_t switches;                 // 被切入次数
    uint64_t stack_bytes;
    uint32_t is_current;
};

// ---- 调度钩子（强符号，覆盖 kernel/x86_64.cpp 里的 weak 空实现）----
extern "C" void schedule64(pt_regs64* r);

// ---- 生命周期 ----
void task_init64();                                       // 把当前执行流登记为任务 0（idle/桌面）
int  task_create64(const char* name, void (*entry)(void*), void* arg);  // 返回任务 id 或 -1
void task_start64();                                      // 建内置内核线程 + 打开抢占（幂等）

// ---- 任务侧接口（只能从任务上下文调用）----
void task_yield64();                                      // 让出到下一个 tick（sti; hlt）
void task_sleep64(uint32_t ms);
[[noreturn]] void task_exit64();                          // 任务自杀（不返回）

// ---- 查询（任意上下文可调用）----
uint32_t    task_current_id64();
const char* task_current_name64();
int         task_count64();                               // 已占用槽位数（含 idle）
int         task_info64(int idx, Task64Info* out);        // 按槽位下标取快照（0 = 该槽空）
uint64_t    task_switch_total64();                        // 累计上下文切换次数
// ★ 给 usermode64 用：当前任务的内核栈顶（= 应该写进 TSS.rsp0 的值；任务 0 返回
//   引导期安全栈顶 0x80000）。ring3 -> ring0 的中断/系统调用靠 TSS.rsp0 切栈。
//   声明成 extern "C"：安装程序内核不链 task64.cpp，usermode64.cpp 用 weak 引用它。
extern "C" uint64_t task_kstack_top_current64();
// ★ 给 syscall64 用的 C 链接薄封装（sleep_ms / getpid）。syscall64.cpp 用 weak 引用它们：
//   安装程序内核不链接 task64.cpp，这两个调用点要能退化成"没有调度器"的行为。
extern "C" void     task_sleep_ms64(uint32_t ms);
extern "C" uint32_t task_current_id_64();
uint64_t    task_heartbeats64();                          // kheart 心跳计数（调度器活着的最强证据）
uint64_t    task_work_iters64();                          // kwork 迭代次数
int         task_selftest64();                            // 位掩码自检，0 = 全过

// 终止其它任务（任务管理器"结束任务" / 终端 kill 用）。
// 返回 0 = 已标记终止；-1 = 不允许或找不到。
//   规则：idle（任务 0）与**当前任务**不可终止；已经 DEAD/FREE 的返回 -1。
//   实现只把目标置为 DEAD 并挂进待回收队列：真正的 kfree 由某个任务的
//   task_yield64() 完成（不能在中断里动内核堆，见文件头约束 3）。
int task_kill64(uint32_t id);

// ---- 任务退出压力路径（kstress，只进系统内核；安装介质内核没有这些符号）----
// 系统内核启动时自动跑一小轮（反复创建/退出短命任务，压"创建→退出→回收"这条路），
// 串口打点见 kernel/task64.cpp 的 kstress 段（子任务整体安静，只在最后打一行）：
//   [TASK64] stress spawn=<n> done=<n> reap=<n> live=<n> count=<n> fail=<mask> PASS|FAIL
//   fail bit4 = 任务表占用槽位数与 g_task_count 不一致（回收路径被抢占过就会不一致）。
// 自检脚本：tests/sched_stress_test.py。这里只提供"改轮数"（0 = 不改）；不跑。
#ifndef VIMTU_INSTALLER_MEDIA
void task_stress_set_rounds64(uint32_t n);
#endif
