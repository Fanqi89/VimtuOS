// proc64.h - Vimtu64 进程与地址空间（每进程私有用户窗口 + CR3 切换 + fork/execve/wait4）
//
// ============================ 设计（一页纸）============================
// 1) 进程与任务的关系：**任务（线程）属于进程**。
//    进程表（本文件，PROC64_MAX 槽）持有地址空间与"用户现场"；调度器的任务表（task64.h）
//    通过 task_bind_proc64() 拿到三样要在切换时装载的东西：CR3、IA32_FS_BASE、syscall 专用栈顶。
//    task64 因此**不认识** Proc64 结构（只存一个 void* proc 与三个已算好的数值），
//    既有任务语义/打点/回收竞态修复完全不动（见 task64.cpp 里 task_apply_ctx64 的说明）。
//
// 2) 页表共享策略（为什么这么切）：
//    引导期（BIOS 路径，loader64.asm）建的 PML4 物理 0x40000：
//      PML4[0]   -> PDPT(0x41000)：PDPT[0..3] = 4 张 PD，2MB 大页恒等映射 **0..4GB**
//      PML4[511] -> PDPT_hi(0x46000)：PDPT_hi[510] -> PD0，VA 0xFFFFFFFF80000000+PA 直映 0..1GB
//    每进程的新地址空间 = 新 PML4 页 + 新 PDPT 页：
//      * PML4[511] **原样复制**引导期的项 -> 内核高半区（内核代码/数据/栈）在所有进程里共享同一批页表；
//      * PML4[0]   -> **新 PDPT**：PDPT[0..3] 原样复制引导期的项（前 4GB 恒等映射共享同一批 PD），
//        PDPT[4] 及之后 = 0 —— 用户窗口（VA 4GiB..5GiB）因此每进程私有（自己分配 PD/PT）。
//      ★ 用户窗口选在 4GiB 起（既有约定，见 usermode64.h），正好落在 PML4[0]/PDPT[4]：
//        PDPT[4] 一条项就能把"整个用户窗口"私有化，代价只有 1 个 PDPT 页 + 按需的 PD/PT。
//      ★ 恒等映射必须共享：内核堆/页池/LFB 都是按**物理地址**直接访问的（<4GB）。
//
// 3) CR3 切换点（两处，缺一不可）：
//    * 调度器切任务时（kernel/task64.cpp 的 schedule64 / task_exit64 / task_start64）：
//      task_apply_ctx64(next) -> tss_set_rsp0 + syscall 栈顶 + mov cr3 + wrmsr FS.base；
//    * 任务自己显式切：proc64_switch_to64(proc)（fork 出的子任务第一次跑、execve 之后等）。
//    切 CR3 本身就会刷新非全局 TLB 项（用户页 PTE 都没打 PTE_GLOBAL），所以不额外 invlpg。
//
// 4) UEFI 降级（实测驱动，见 proc64_init64 的探测与串口打点）：
//    UEFI 路径（boot/efi/uefi64.c）**不换 CR3**、把高半区映射挂进固件当前活动的 PML4 —— 那个
//    页表属于固件（只读、格式由固件决定；VMware EFI 下运行期 mov cr3 已知会立刻复位）。所以
//    只有在"引导期页表确实是我们自己建的"（BIOS 路径：CR3 == ML64_PML4_PHYS == 0x40000）时
//    才启用每进程 CR3；否则进 **共享地址空间模式**（`g_proc64_isolate64 == 0`）：
//      fork/vfork/clone/execve/wait4 一律 -ENOSYS，brk/mmap 退回原来的共享窗口实现，
//      启动期多进程演示直接跳过。串口与文档都如实标注，**绝不假装隔离成立**。
//
// 5) fork 的取舍：**整页物理复制，不做 COW**。
//    每个已映射的用户页分配一个新物理页 + memcpy 4KB，页表结构也逐级复制（新 PT/PD）。
//    为什么不上 COW：COW 需要"写时缺页 + 引用计数 + 页表项只读"三件套，缺页处理要能区分
//    用户 COW 与内核 #PF（还得改 ISR 路径），而本内核的页池只有 ~384MB、演示映像只有几页 ——
//    整页复制的实现风险与调试成本远低于 COW，语义上完全等价（父子互不可见）。
//    页数上限 PROC64_FORK_MAX_PAGES（见 proc64.cpp），超了 fork 直接 -ENOMEM 并如实打点。
//
// 6) 地址空间生命周期：
//    proc64_create64：新 PML4 + 新 PDPT（只有这两页是"进程基础设施"）。
//    proc64_release_user_area64：走自己 PDPT[4] 子树，逐叶子 free 物理页 + 回收 PT/PD，
//      最后清 PDPT[4]（**不动** PDPT[0..3] 的共享恒等映射）。
//    proc64_destroy64：先把 CR3 切回内核地址空间（否则会 free 掉正在用的 PML4），再回收用户区
//      + PDPT + PML4 + 记住的 syscall 栈/任务（任务侧由 task64 的回收路径负责）。
//
// 7) ★ A4-5 信号：每进程一份 Sig64State64（未决/屏蔽/handler/restorer），语义与打点见 kernel/sig64.h；
//    本文件负责进程**进程组**（pgid）与"杀哪个进程"（sig64 通过下面那组访问器拿进程表）。
// 串口打点（自动验收 tests/proc64_test.py grep，格式勿改）：
//   [PROC64] init mode=isolated|shared cr3=<hex> proc_max=<n>
//   [PROC64] create pid=<n> name=<s> cr3=<hex> ppid=<n>
//   [PROC64] fork parent=<pid> child=<pid> cr3=<hex>
//   [PROC64] execve path=<p> pid=<n> entry=<hex>
//   [PROC64] wait4 pid=<n> status=<n>
//   [PROC64] kill pid=<n> sig=<n>
//   [PROC64] exit pid=<n> code=<n> cr3_released=1
//   [PROC64] selftest PASS / [PROC64] selftest FAIL mask=<n>
//   [PROC64] uefi exp A stage=<map|enter|ok|fail> err=<hex>        （实验构建专用，见下）
//   [PROC64] uefi exp B stage=<build|cr3|enter|ok|fail> err=<hex>  （实验构建专用）
//   [PROC64] uefi exp result=A|B|none mode=isolated|shared         （实验构建专用）
#pragma once
#include <stdint.h>
#include "x86_64.h"     // pt_regs64（fork/execve 要直接改帧）
#include "input64.h"    // ★ A5 前置：每进程事件队列（Ev64Queue）内嵌在 Proc64 里
#include "usermode64.h" // ★ A5 前置：shm 映射窗的位置由 USER64_CODE_VA64/WINDOW_BYTES64 算出来
#include "sig64.h"      // ★ A4-5：每进程信号状态（Sig64State64）内嵌在 Proc64 里

// ==================== 规模与状态 ====================
static const int PROC64_MAX          = 16;    // 进程槽（与 TASK64_MAX 一致：1 进程 1 任务）
static const int PROC64_NAME_MAX     = 16;
static const int PROC64_PATH_MAX     = 32;
static const int PROC64_CHILDREN_MAX = 8;     // 每个进程最多同时记 8 个子（够演示/够验收）
// ★ A4-2a：每进程工作目录缓冲（与 VFS64_PATH_MAX 一致；cwd 是"进程属性"，见 proc64.cpp 的 Proc64 注释）
#define PROC64_CWD_MAX 128

enum Proc64State : uint32_t {
    PROC64_FREE    = 0,
    PROC64_READY   = 1,     // 已建好地址空间，任务已建但还没被调度过
    PROC64_RUNNING = 2,     // 正在（或被抢占着）跑 ring3
    PROC64_SLEEP   = 3,     // 阻塞在系统调用里（wait4 轮询等）
    PROC64_EXITED  = 4      // 已退出（僵尸：等父进程 wait4 收；地址空间已释放）
};

// 进程快照（任务管理器进程页 / 终端 ps：纯值拷贝，不暴露内部指针）
struct Proc64Info {
    uint32_t pid;
    uint32_t ppid;
    uint32_t state;              // Proc64State
    uint32_t task_id;
    uint32_t is_current;
    uint32_t pages;              // 用户页数（已映射的 4KB 页）
    char     name[PROC64_NAME_MAX];
    uint64_t cr3;
    int32_t  exit_code;          // 已退出才有意义
    uint32_t term_sig;           // 被哪个信号终止（0 = 正常退出）
};

// ==================== 生命周期 ====================
// 启动期调用一次：探测"引导期页表是不是我们自己的" -> 决定隔离模式，打印 [PROC64] init。
// 幂等。安装程序内核不链本文件（proc64.cpp 只在 SRCS_OS 里）。
void proc64_init64();
// 1 = 每进程 CR3 生效（真正隔离）；0 = 共享地址空间模式（UEFI/固件页表：fork/execve -ENOSYS）
int  proc64_isolate64();
// 位掩码自检（0 = 全过）：模式探测、进程表槽位、页表克隆布局、fork 上限常量、uid/信号表
int  proc64_selftest64();

// ==================== 进程 ====================
// 建一个进程（新地址空间；**不建任务、不跑**）。返回 pid（>= 1）或 -1（槽满/页表页不足/共享模式）。
int  proc64_create64(const char* name, int ppid);
// 在进程 p 的地址空间里装载 ELF 并建任务（任务入口 = ring3 跑该 ELF，退出后进程转 EXITED）。
// 返回 0 = 已就绪（等调度器跑它）；-1 = 失败（已打印 reason）。
int  proc64_start_elf64(int pid, const char* path);
// ★ A4-5：同上，但初始栈按调用方给的 **argv** 摆（编辑器这类要文件名参数的程序用它）。
// argv[0] 约定为主程序路径；argc 上限 16（elf64 的初始栈构建上限）。
int  proc64_start_elf64_argv64(int pid, const char* path, const char* const* argv, uint32_t argc);
// 当前进程的父 pid / 映像路径（getppid(110) 与 readlink("/proc/self/exe")(89) 用）。
// 没有进程上下文时分别返回 0 与 0（= 没有值）。
int  proc64_current_ppid64();
int  proc64_exe_path64(char* out, uint32_t cap);   // 返回 1 = 已填入；0 = 当前没有进程
// 显式切到该进程的地址空间（fork 出的子任务第一次跑时用；调度器切换也会做）。
void proc64_switch_to64(int pid);
// 切回内核地址空间（释放地址空间 / 装载别的进程之前用）
// 收回用户区（叶子物理页 + PT/PD）并释放 PDPT/PML4；进程槽转 FREE。
// 必须先把 CR3 切回内核地址空间（内部会做）。
void proc64_destroy64(int pid);

// ==================== 查询 ====================
int   proc64_current_pid64();                     // 当前进程 pid；-1 = 不在进程上下文
int   proc64_find64(int pid);                     // 1 = 存在，0 = 不存在
// cwd：写进 out（NUL 结尾）并返回 1；没有进程上下文返回 0（调用方按 "/" 处理）。
// ---- ★ A4-2a：每进程工作目录（chdir(80) / getcwd(79)）+ 子进程 fd 表（内核给外部程序传 fd 用）----
int   proc64_cwd64(char* out, uint32_t cap);
// 改变**当前进程**的 cwd（path 必须是已规范化的**绝对路径**且确实存在、是目录）。
// 返回 0 = 成功；-2 = 不存在（ENOENT）；-20 = 不是目录（ENOTDIR）；-1 = 没有进程上下文。
int   proc64_chdir64(const char* path);
// 按 pid 拿它的 fd 表（proc64_create64 时分配；pid 不存在返回 nullptr）。
struct FdTable64;
FdTable64* proc64_fdtab_of64(int pid);
int   proc64_count64();                           // 占用的进程槽数
int   proc64_info64(int idx, Proc64Info* out);    // 按槽位下标取快照（0 = 该槽空）
const char* proc64_current_name64();

// ==================== 给 syscall64 / 调度器用 ====================
// fork：复制当前进程的地址空间与用户现场 -> 新进程 + 新任务（从 ring3 同一点继续、rax=0）。
// r = 当前系统调用帧（ring3 现场：rip/rsp/rflags/GPR 都在里面）。返回子 pid（父进程 rax）或负错误码。
int64_t proc64_fork64(pt_regs64* r);
// execve：**在当前进程里换映像**（释放旧用户区 -> 装载新 ELF -> 重建初始栈/auxv）。
// 成功时改写 r 让 syscall 出口直接回到用户态的**新入口**，返回 0；失败返回负错误码（r 未改）。
int64_t proc64_execve64(pt_regs64* r, const char* path, const char* const* argv, uint32_t argc);
// wait4：阻塞等待子进程（task_sleep64 轮询，见 proc64.cpp 的说明：不做等待队列）。
// status_out != nullptr 时写入 Linux 编码的 status（(code & 0xFF) << 8；被信号终止的进程见下）。
// options: 1 = WNOHANG。返回 pid；-ECHILD/-EAGAIN 等负错误码。
int64_t proc64_wait4(int pid, int* status_out, uint32_t options);
// kill：**真投递**（★ A4-5 起）：pid>0 单个进程；pid==0 自己的进程组；pid<0 = 进程组 |pid|（-1 = 全部）。
//   实现见 kernel/sig64.cpp 的 sig64_sys_kill64；本函数只是 syscall64 的入口。
int64_t proc64_kill64(int pid, int sig);
// ---- 进程侧的内存管理（brk/mmap/mprotect/munmap 走这里；每进程独立且可回收）----
uint64_t proc64_brk64(uint64_t addr);                                  // Linux brk 语义
int64_t  proc64_mmap64(uint64_t len, uint64_t flags, uint64_t addr);   // 每进程 bump 分配器
int64_t  proc64_mprotect64(uint64_t addr, uint64_t len, uint64_t prot);
int64_t  proc64_munmap64(uint64_t addr, uint64_t len);                 // 逐页 unmap + 回收
// 显式切到该进程的地址空间（fork 出的子任务第一次跑时用；调度器切换也会做）。
void proc64_switch_to64(int pid);
// 显式切回**内核**地址空间（释放地址空间之前必须做：否则会 free 掉正在用的 PML4）。
void proc64_switch_to_kernel64();
uint64_t proc64_user_pages64(int pid);

// IA32_FS_BASE（TLS）：真写 MSR 0xC0000100 + 记进进程（切换时保存/恢复）。
// 返回 0 = 成功；-EINVAL(-22) = 值不是规范地址或当前不在进程上下文。
int64_t  proc64_set_fs_base64(uint64_t v);
uint64_t proc64_get_fs_base64();

// ==================== ★ A4-5：信号（真投递；语义/打点见 kernel/sig64.h）====================
// sig64.cpp 通过下面这组访问器拿进程表（它是弱引用它们：安装介质内核不链 proc64.cpp）。
// 每进程状态本体在 Proc64::sig（Sig64State64），生命周期与进程一致。
Sig64State64* proc64_sig_state_of64(int pid);          // nullptr = 没有这个 pid / 槽空闲
Sig64State64* proc64_sig_state_current64();            // nullptr = 没有进程上下文（任务 0/共享模式）
int  proc64_sig_die_other64(int pid, uint32_t code, int sig);   // 杀**别的**进程（SIGKILL/默认动作）；1 = 已终止
int  proc64_sig_die_current64(int sig, pt_regs64* r);           // 杀**当前**进程（改写帧回内核）；1 = 已安排
int  proc64_pgid_of64(int pid);                                 // 进程组 id；-1 = 没有这个 pid
int  proc64_proc_pid_at64(int idx);                             // 按进程表槽位取 pid；-1 = 空槽
int  proc64_setpgid64(int pid, int pgid);                       // setpgid(109)：0 = 成功；负数 = -errno
int   proc64_sig_pending64(int sig);          // 记录型查询（不投递）
int   proc64_alarm_set64(int sec);            // 只记录（无投递）；返回上一次的秒数
// ★ P4：每进程凭证（uid/gid/euid/egid；root = 0）。fork/execve 继承；setuid/setgid/seteuid 改它。
//   * proc64_get_cred64：0 = 已填入（当前进程）；-1 = 没有进程上下文（调用方退化为会话身份）。
//   * proc64_set_cred64：改当前进程凭证并**立刻发布**给 VFS；-1 = 没有进程上下文（不改）。
int   proc64_get_cred64(uint32_t* uid, uint32_t* gid, uint32_t* euid, uint32_t* egid);
int   proc64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid);

// 内嵌 /proc64.elf 的幂等安装（与 elf64_install_builtin64 同一套路；VFS 没挂载就返回 -1）
int   proc64_install_builtin64(int drive, uint32_t part_lba);
// 启动期多进程演示（父子打印 pid / execve / wait4 / kill）。返回 0 = 跑完（含跳过/失败，只打点）。
// ==================== 每进程 fd 表（批次 D）====================
// Proc64 上挂一张 fd64 的 FdTable64（指针，池由 fd64.cpp 管）：
//   * proc64_create64 分配、fork 时 fd64_table_clone64 **逐槽共享**（引用计数 +1，父子共享偏移）、
//     execve 默认**保留**（没有 O_CLOEXEC）、退出/回收时 close_all + 归还表；
//   * 没有进程上下文（任务 0 桌面/终端、安装介质内核）时 fd64 回退到内核表；
//   * 语义表与边界见 docs/应用层与系统调用说明.md（fd 章节）：dup 共享对象、O_APPEND、pipe。
// pipe 演示：建一个进程跑 /pipe64.elf（fork 后父子各持一端通信）。隔离模式关闭时只打一行跳过。
int proc64_pipe_demo64(const char* path);

// ==================== UEFI 运行期 CR3 实验（编译期开关，默认关）====================
// 只在 -DPROC64_UEFI_CR3_EXPERIMENT=1 的构建里存在（默认构建不编、行为不变）：
//   方案 A：临时清 CR0.WP -> 往**固件活动 PML4** 里挂用户窗口 -> 恢复 WP -> 试进 ring3；
//   方案 B：自带一份 PML4（复制固件的内核映射 + 用户窗口）-> mov cr3 -> 试进 ring3。
// start 版建一个**延迟任务**（保证 [GUI64] ready 已经打出来，实验即使触发复位也留有桌面证据）。
// 结论与两条固件路径的实测现象见 docs/UEFI地址空间实验报告.md。
#if defined(PROC64_UEFI_CR3_EXPERIMENT) && (PROC64_UEFI_CR3_EXPERIMENT == 1)
int proc64_uefi_cr3_experiment_start64();
#endif
// 位置：os_boot_path 里"现有 ring3 演示之后、gui64_run 之前"。隔离模式关闭时只打一行跳过。
int   proc64_demo64(const char* path);

// ==================== ★ A5 前置（1/2）：每进程输入事件队列（自有 ABI 12 input_poll）====================
// 队列本体 = Ev64Queue（kernel/input64.h），内嵌在 Proc64 里；事件层（input64.cpp）通过
// proc64_evq_of64() / proc64_evq_of_current64() 拿它。路由/焦点/背压语义全在 input64.h 里，
// 这里只负责\"挂载点 + 生命周期\"：
//   * create 时清零；fork 时**子进程拿一份空队列**（事件不继承 —— 父子不是同一个前台）；
// ★ 必须是 extern "C"：input64.cpp 用**弱引用**取它们（安装介质内核不链 proc64.cpp）。
//   弱符号按 C 名解析 —— 若这里是 C++ 名字，弱引用永远解析不到、静默变成 0（症状：
//   \"所有事件都进不了队列\"，与 task64.h 里 task_slot_current64 踩过的是同一个坑）。
extern "C" void* proc64_evq_of64(int pid);              // nullptr = 没有这个 pid
extern "C" void* proc64_evq_of_current64();             // nullptr = 当前不在进程上下文

// ==================== ★ A5 前置（2/2）：共享内存缓冲（自有 ABI 13 shm_create / 14 shm_map）====================
// 目标：支撑\"一个进程画、另一个进程看/提交\"（Wayland 的 buffer 语义）的最小地基。
// 实现（为什么这么切）：
//   * **对象表**：8 个槽，每槽 = {页帧数组, 页数, 引用数, 拥有者 pid}；页帧由 page_alloc_64 分配
//     （零初始化），对象持有它们 —— 页帧**不属于任何进程**，进程退出不会把它们当成\"自己的页\"回收；
//   * **每进程句柄表**：Proc64.shm[]（4 个槽）= 该进程持有句柄的对象 id。**fork 继承**
//     （逐槽 +1 引用；父子共享同一批页帧），**execve 默认关闭**（换映像时逐槽 -1），退出时逐槽 -1；
//   * **映射**：每进程一块固定映射窗（4GiB+16MiB 顶部 256 KiB = 4 槽 × 64 KiB，全部落在
//     PDPT[4] = 用户窗口内，所以页表回收/统计的那些既有路径仍然成立）。shm_map 把**同一批物理页**
//     挂进调用进程的页表（PTE U|W|NX）—— 两边写同一块内存，这就是\"共享\"的落地方式；
//   * 回收：引用数归零时把页帧还回页池（打点 `[SHM64] release id=.. refs=0 freed=..`）。
// ★ 如实边界（别把没做的说成做了）：没有 mmap 文件语义（没有 fd、没有 offset 分页共享的
//   文件后端）、没有跨进程同步原语（锁/信号量/futex）、没有 shm_unlink/权限位；引用计数也
//   只在\"进程退出/execve\"上递减，没有显式 close 系统调用。
static const uint32_t SHM64_SLOTS64       = 4;                                   // 每进程最多 4 个映射窗
static const uint64_t SHM64_SLOT_BYTES64  = 64ULL * 1024ULL;                     // 单窗 = 单对象上限
static const uint64_t SHM64_WINDOW_BYTES64 = SHM64_SLOTS64 * SHM64_SLOT_BYTES64; // 256 KiB
static const uint64_t SHM64_WINDOW_VA64   = USER64_CODE_VA64 + USER64_WINDOW_BYTES64 - SHM64_WINDOW_BYTES64;
static const uint32_t SHM64_HANDLES64     = 4;                                   // 每进程句柄表槽数
static const uint32_t SHM64_MAX_PAGES64   = 16;                                  // 单对象页数上限（= 64 KiB）

// 错误码（负数；口径与 A1 的 fb_map / EV64_* 一致）
static const int64_t SHM64_EPERM64  = -1;   // 没有进程上下文 / 本进程没有这个对象的句柄
static const int64_t SHM64_EFAULT64 = -2;   // out_va 用户指针非法
static const int64_t SHM64_EINVAL64 = -3;   // size/offset/len 非法（0、越界、非页对齐）
static const int64_t SHM64_ENOMEM64 = -4;   // 对象表/页池满；映射窗被占用；页表页不足

// shm_create(size)：建对象 -> 返回**跨进程可共享的对象 id**（>= 1）/ 负错误码。
// 调用进程同时拿到一个句柄（引用数 = 1）。打点：`[SHM64] create pid=.. id=.. size=.. pages=.. refs=1`
int64_t proc64_shm_create64(uint64_t size);
// shm_map(id, offset, len, out_va)：把对象的一段映射进**当前进程**（用户可读写），
// *out_va 写回用户态基址；返回 0 = 成功，负数 = 错误码。offset 必须 4KiB 对齐。
// 打点：`[SHM64] map pid=.. id=.. va=0x.. off=.. len=.. pages=.. refs=.. u=1 slot=..`
int64_t proc64_shm_map64(uint64_t id, uint64_t offset, uint64_t len, uint64_t out_va_uptr);
// 当前进程持有句柄的对象数（任务管理器/自检用；不动状态）
int     proc64_shm_held64();

// ---- A5 前置的 ring3 演示（/evshm.elf：shm_create -> 画 -> fb_flip -> 循环 input_poll）----
// 幂等把内嵌 blob 装进系统卷（`/evshm.elf`），再以**真进程**跑它（父 fork/子读同一块 shm）。
// 位置：os_boot_path 里 proc64_pipe_demo64 之后、ring3_slot_reuse_demo64 之前。
// 返回 0 = 跑完（含跳过/超时，只打点）。
int proc64_evshm_install64(int drive, uint32_t part_lba);
int proc64_evshm_demo64(const char* path);
