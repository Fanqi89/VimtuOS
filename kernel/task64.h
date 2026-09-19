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
// ==================== 任务统计 / 关键任务（批次 A 后半：从 32 位 task.cpp 搬语义）====================
// 与 32 位 kernel/task.cpp 的对应关系：
//   task64_cpu_permille64      <- task_sample_cpu + task_cpu_pct（口径见 task64.cpp 的注释）
//   task64_find_by_name64      <- task_find_by_name（64 位返回**任务 id**，不是槽位下标：
//                                 64 位的 id 才是对外句柄 —— task_kill64/task_info64 都吃 id）
//   task64_mark_critical64     <- task_mark_critical（关键任务在 task_kill64 里被拒绝并打点）
//   task64_set_slice64         <- task_set_slice（每任务时间片；默认值仍是 TASK64_SLICE_TICKS）
//   task64_force_remove64      <- task_force_remove（硬清理；护栏更严：idle/当前/已 DEAD 一律拒绝）
//   task64_diag64              <- 32 位 task_diag_log 的逐槽版本（一行一个槽，供终端/测试读）
//
// CPU 千分比口径（★ 与 32 位的差别必须说明）：
//   采样窗口 = 1 秒（PIT_HZ_64 个 tick），懒采样（第一次查询时若窗口已过就重算）。
//   分子 = 该任务 ticks 增量 / 2 —— 调度器**每个 tick 给当前任务记 2**（既有记账，
//         见 schedule64 的 c->ticks++ 两次），所以除以 2 才是"真实 tick 数"；
//   分母 = 系统 tick 增量（g_ticks64，250Hz）。
//   满载单任务读到 1000‰（单核）；所有任务之和可以超过 1000‰（含内核线程），不做归一化。
uint32_t task64_cpu_permille64(uint32_t id);            // 千分比（0..1000）；无此 id 返回 0
int  task64_set_slice64(uint32_t id, uint32_t ticks);   // 每任务时间片（1..100 tick）；0 / -1
int  task64_find_by_name64(const char* name);           // 返回任务 id（只扫非 FREE 槽）；-1 = 找不到
int  task64_force_remove64(uint32_t id);                // 强制摘除+回收；见实现里的返回码注释
int  task64_mark_critical64(uint32_t id, int critical); // 0 = 已设置；-1 = 无此 id
void task64_diag64();                                   // 打印任务表诊断（一行一个槽 + 汇总）
// 进程（proc64）视角的两个只读统计：以"该进程主任务绑定的 proc 指针"为键。
//   用途 = 任务管理器进程页的线程数 / CPU‰；没有 proc 的内核线程返回 0。
int      task64_proc_threads64(uint32_t main_task_id);        // 任务表里绑定到同一进程的任务数
uint32_t task64_proc_cpu_permille64(uint32_t main_task_id);   // 这些任务的 CPU‰ 之和（截断 1000）
// ==================== 每进程地址空间（批次 C：进程级 CR3 / FS 基址）====================
// 为什么在这里、为什么只有这几个函数（最小改动的理由）：
//   任务的"上下文"除了中断帧之外还多了两样**全局**的东西要在切换时装载：
//     * CR3（页表根）：每进程私有用户窗口的入口；
//     * IA32_FS_BASE(0xC0000100)：glibc 的 arch_prctl(ARCH_SET_FS) 写的就是这个 MSR，
//       它是**全局**的 —— 进程切换时必须保存/恢复，否则进程间互相泄漏 TLS。
//   调度器不需要知道 Proc64 的内部布局：proc64.cpp 在建进程时用 task_bind_proc64()
//   把"切到这个任务前要装载的三个数值"交给任务表：
//     cr3（0 = 用内核地址空间）、fs_base、syscall 专用栈顶（= 每任务内核栈顶下方 4KB，
//     见 kernel/syscall_entry64.asm：SYSCALL 不换栈，入口靠它切栈）。
//   装载点与 TSS.rsp0 完全同一处（task_apply_ctx64）：schedule64 / task_exit64 / task_start64。
int  task_bind_proc64(uint32_t task_id, void* proc, uint64_t cr3, uint64_t fs_base);
void task_update_mm64(uint32_t task_id, uint64_t cr3, uint64_t fs_base);
void* task_proc_of_current64();
// ★ 必须是 extern "C"：usermode64.cpp 用**弱引用**取它（安装介质内核不链 task64.cpp），
//   弱符号按 C 名解析；若这里用 C++ 名字（_Z20task_slot_current64v），弱引用就永远解析不到、
//   静默变成 0 —— 症状是"所有任务都被当成槽位 0"，多进程下 in_ring3 位图张冠李戴。
extern "C" int task_slot_current64();             // 当前任务槽位；-1 = 没有调度器/未登记
int  task_slot_of_id64(uint32_t id);              // 任务 id -> 槽位；-1 = 找不到
// 当前任务的 syscall 入口专用栈顶（没有调度器时 = 静态专用栈，见 syscall64.cpp）
extern "C" uint64_t task_syscall_stack_top64();
// 两个装载原语：与调度器共用同一份"值相同就跳过"的缓存，避免出现两套缓存各自漂移
void     task64_load_cr364(uint64_t cr3);
void     task64_load_fs_base64(uint64_t v);
uint64_t task64_kernel_cr364();                   // 内核地址空间（task_init64 时读到的 CR3）
// ---- 任务退出压力路径（kstress，只进系统内核；安装介质内核没有这些符号）----
// 系统内核启动时自动跑一小轮（反复创建/退出短命任务，压"创建→退出→回收"这条路），
// 串口打点见 kernel/task64.cpp 的 kstress 段（子任务整体安静，只在最后打一行）：
//   [TASK64] stress spawn=<n> done=<n> reap=<n> live=<n> count=<n> fail=<mask> PASS|FAIL
//   fail bit4 = 任务表占用槽位数与 g_task_count 不一致（回收路径被抢占过就会不一致）。
// 临时把**当前任务**的地址空间换成 cr3（cr3 = 0 表示恢复"内核地址空间"）。
// 为什么需要：proc64 装载/释放进程映像时要求"这段时间里 CR3 一直是该进程的"，但任务
// 可能被 PIT 抢占（任务 0 的 mm_cr3 本来是 0 = 内核地址空间，一被抢占再切回来就变回内核
// CR3 了 —— 症状是"映像被装进了共享窗口，进程自己的地址空间是空的"）。改 TCB 里的值
// 之后，调度器每次切回本任务都会把它恢复成 cr3，装载因此对抢占是安全的。
// ★ 调用方负责在结束后再调一次 task_set_current_mm64(0, 0) 恢复内核地址空间。
void task_set_current_mm64(uint64_t cr3, uint64_t fs_base);
// 自检脚本：tests/sched_stress_test.py。这里只提供"改轮数"（0 = 不改）；不跑。
#ifndef VIMTU_INSTALLER_MEDIA
void task_stress_set_rounds64(uint32_t n);
#endif
