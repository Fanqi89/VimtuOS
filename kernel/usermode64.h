// usermode64.h - Vimtu64 用户态（ring3）最小支撑：用户页 + 进出 ring3 + 回收
//
// 与 32 位的关系：32 位构建没有用户态（全程 ring0），本文件是 64 位专有的"应用层"地基。
//
// 用户地址空间（VA 选在 4GiB 以上）：
//   loader（boot/loader64.asm:lm64_build_paging）只把 0..4GB 做了恒等映射，4GiB 以上
//   在 PML4[0] 里还没有 PDPT 项 —— 对内核完全空闲，正好给用户态用；而且内核正在用的
//   低 4GB 因此不会被套上用户权限。用户页要求 PML4E[0]/PDPTE/PDE/PTE 四级都打开 U/S。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [USER64] map code=<va> stack=<va> code_pages=<n> stack_pages=<n>
//   [USER64] enter ring3 entry=<va> rsp=<va>
//   [USER64] back to kernel (ring0)
//   [USER64] selftest PASS / [USER64] selftest FAIL mask=<n>
#pragma once
#include <stdint.h>
#include "x86_64.h"      // pt_regs64 / SEL64_* / PR64_SIZE

// ==================== 用户内存布局（唯一定义点）====================
// 整个用户地址空间 = **一个 1MiB 的窗口**：4GiB .. 4GiB+1MiB。窗口内再分区：
//   4GiB + 0x00000 .. USER64_STACK_VA64   ELF64 装载区（PT_LOAD 段按 p_vaddr 落在这里）
//   USER64_STACK_VA64 .. +16KiB           用户栈（VAP64 演示与 ELF64 共用；ELF 初始栈也建在这）
//   4GiB + 0x40000 .. +0x40000+64KiB      brk 区（Linux brk(12) 的固定可写区）
//   USER64_TEST_VA64 .. +4KiB             自检临时页（user64_selftest64 用完即解除映射）
//   4GiB + 0x90000 .. 4GiB+1MiB           mmap 碰撞分配器（Linux mmap(9)）
// ★ 分区互不重叠这一条由 syscall64_selftest64 的 bit0 断言把关（改常量先跑它）。
static const uint64_t USER64_CODE_VA64      = 0x0000000100000000ULL;  // 4GiB：用户代码/入口
static const uint64_t USER64_STACK_VA64     = 0x0000000100010000ULL;  // 4GiB+64KiB：用户栈底
static const uint64_t USER64_TEST_VA64      = 0x0000000100080000ULL;  // 自检用测试页（窗口内）
static const uint64_t USER64_WINDOW_BYTES64 = 1024ULL * 1024ULL;      // 用户窗口 = 4GiB..4GiB+1MiB
static const uint64_t USER64_STACK_BYTES64  = 16ULL * 1024ULL;        // 用户栈 16KiB = 4 页
static const uint64_t USER64_BRK_VA64       = 0x0000000100040000ULL;  // 4GiB+256KiB：brk 固定区
static const uint64_t USER64_BRK_BYTES64    = 64ULL * 1024ULL;        // 64KiB
static const uint64_t USER64_MMAP_VA64      = 0x0000000100090000ULL;  // 4GiB+576KiB：mmap 起点
static const uint64_t USER64_MMAP_MIN_BYTES64 = 64ULL * 1024ULL;      // mmap 至少要有这么多可用

// 自检（位掩码，0 = 全过）：用户页映射/权限位、iretq 帧字段、选择子/GDT 现场、范围校验
int user64_selftest64();

// 跑一个用户程序 blob：拷到用户代码页 -> 建映射 -> iretq 进 ring3 -> exit 后回到本函数。
// 返回 0 = 完整跑完（含收尾回收）；-1 = 参数/内存失败。
int user64_run_blob64(const void* blob, uint32_t size, const char* name);

// 用户窗口是否可用（批次 C：UEFI 下固件页表只读 -> 连 U/S 都写不进去 -> 必须如实返回 0）：
// 1 = 可用（BIOS 路径）；0 = 不可用（UEFI 路径，ring3 演示必须跳过，见 usermode64.cpp 的说明）。
int user64_available64();
// 从**指定入口/用户 rsp** 进 ring3 并等它回来（不映射/不回收任何页：ELF64 加载器自己
// 把段和栈准备好再调这里）。返回用户 exit 的退出码；-1 = 参数非法或本任务已在 ring3。
int user64_enter_at64(uint64_t entry, uint64_t user_rsp, const char* name);

// 从一份**现成的用户帧**进 ring3（批次 C：fork 出的子进程用）。
// 为什么需要它：fork 的子进程必须从"父进程 syscall 的下一条指令"继续、且寄存器状态与父
// 完全相同（只有 rax=0）—— 那就不能用 user64_enter_at64（它只给 entry/rsp、其余清零）。
// frame 必须来自一次真实的 ring3 系统调用帧（cs=0x2B / ss=0x23）。返回用户 exit 的退出码。
int user64_enter_frame64(const pt_regs64* frame, const char* name);

// ★ 多进程（批次 C）后"用户窗口"是**每进程私有**的（见 kernel/proc64.h）：
//   usermode64.cpp 里的页级原语（user64_map_page64 等）操作的都是**当前 CR3** 指向的那份
//   地址空间 —— 所以调用它们之前必须先 proc64_switch_to64(该进程) / 或让调度器切好 CR3。

// ==================== 给 syscall64 用 ====================
// 用户态指针安全校验：va..va+len 必须完全落在**用户窗口内、且已映射为用户页**。
// 返回 1 = 允许（ring0 可以安全读这块内存）；0 = 拒绝（越界/未映射/内核页）。
int user64_range_ok64(uint64_t va, uint64_t len);

// exit(2) 的落地实现：把当前 int 0x80 的中断帧改写成"回 ring0"的帧，
// 让 isr64_common 的出口 iretq 落到内核蹦床（见 usermode64.cpp 顶部说明）。
// 返回 1 = 已改写（不会正常返回用户态）；0 = 当前不在 ring3（调用方按普通返回处理）。
int user64_exit_to_kernel64(pt_regs64* r, uint64_t code);
// ==================== 给 task64 用：任务槽位回收（批次 B 的缺陷根治）====================
// 清掉**某个任务槽**的 ring3 状态：in_ring3 位 + 每任务保存区 g_user64_ctx64[slot]。
// 为什么必须由回收/强杀路径调用（真缺陷，不是补丁）：ring3 进程被 kill（SIGKILL/SIGTERM）时
//   不会从 user64_enter64 正常返回，"进 ring3 后清位"那三行根本没机会执行 —— 该槽的位就
//   永远留着；复用该槽的新任务会在 user64_enter_frame64 的自检上失败（reason=2），症状是
//   [USER64] enter FAILED reason=2。所以 task64 的 task_kill64 / task_drain_reap /
//   task_exit64 / task64_force_remove64 都要调它（见 kernel/task64.cpp 的调用点）。
// 打点：只在**确实清掉了脏位**时打一行，别的任务回收不打（避免刷屏）：
//   [USER64] slot release slot=<n> in_ring3=1 -> cleared
// 安装介质内核不链 task64.cpp，所以只有系统内核会调它；本函数本身两份内核都编得进去。
extern "C" void user64_slot_release64(int slot);
// ==================== 给 syscall64 / elf64 用的页级原语 ====================
// 为什么导出这几个（而不是让加载器自己走页表）：页表走法（大页判断、中间层补 U/S、
// CR3 重载）与"用户窗口"的定义都在 usermode64.cpp 里，重复实现两份迟早会漂。
//
// 在用户窗口里分配/映射一个 4KB 页。alloc=1：没映射时分配物理页并**清零**；
// 已经映射时直接复用（只改权限位），**绝不重复分配**（否则会泄漏物理页）。
// 返回 1 = 成功（out_phys 可传 nullptr）；0 = 失败（VA 未页对齐/越出窗口/页池空/撞大页）。
int      user64_map_page64(uint64_t va, uint64_t leaf_flags, int alloc, uint64_t* out_phys);
// 只改**已存在**映射的叶子权限位（ELF 装载时先按 P|W|U 写内容、最后再收紧权限就用它）。
// 返回 1 = 成功；0 = 该 VA 没有映射（不新建）。
int      user64_remap_flags64(uint64_t va, uint64_t leaf_flags);
// 解除映射并返回原来的物理地址（0 = 本来就没映射）。调用方自己决定要不要 page_free_64。
uint64_t user64_unmap_page64(uint64_t va);
// 把新页表项投递到 TLB/分页结构缓存（重载 CR3）。批量改完后调一次。
void     user64_paging_sync64();
// 该 VA 是否已映射为 ring3 可访问的用户页（四级都 present+U/S 且不是大页）。
int      user64_page_is_user_ok64(uint64_t va);
// 读叶子 PTE 的**标志位**（0 = 没有映射；物理地址不外泄）。自检/调试用。
uint64_t user64_page_flags64(uint64_t va);
