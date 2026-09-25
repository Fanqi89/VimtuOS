// syscall64.cpp - Vimtu64 系统调用：int 0x80（自有 ABI）+ syscall 指令（Linux x86_64 ABI）
//
// ============================ 安全模型（两条路径共用，别省）============================
//   所有来自用户态的指针参数都必须先过 user64_range_ok64()：要求整段落在用户窗口
//   （4GiB..4GiB+1MiB）内、且四级页表都是 present + U/S（已映射的用户页）。越界/未映射
//   一律返回错误并打 [SYSCALL] deny nr=<n> arg=<ptr>。**绝不能**让用户传一个内核地址就把
//   内核内存读出去：ring0 读内核地址是合法的，唯一的闸门就是这里的范围校验。
//   write 另加一条：单次 len 上限（演示/日志用途，别用它搬大块数据）。
//
// ============================ Linux x86_64 系统调用号映射表 ============================
//   ★ 这是 syscall 指令路径（int_no = SYSCALL64_INSM_FRAME_MARK64）专用的号段，与
//     int 0x80 的自有号段完全隔离：同一个数字在两条路径上含义不同（例如 1 都表示 write，
//     但 2 在 int 0x80 是 exit、在 Linux 是 open）。
//
//   号  名称               实现状态（★ 批次 C 之后；"真" / "部分" / "-ENOSYS"）
//   ---- ----------------- ------------------------------------------------------------------
//   0    read              真：fd=0（stdin）返回 0 = EOF（没有键盘输入流，如实）；fd>=3 走 kernel/fd64.cpp
//                           的 FD 层（读整文件 + 按游标切片，单次最多 4096B）；其它 fd → -EBADF。
//   1    write             真：fd=1 → 串口 + 屏幕；fd=2 → 只串口；fd>=3 → 真文件（fd64，写到游标处并
//                           立刻整体落盘）；len 上限 4096；越界 -EFAULT。
//   2    open              真：走 fd64（单层路径 /name；O_CREAT|O_TRUNC 支持）；不存在 -ENOENT。
//   3    close             真：fd64_close64 释放 fd 槽 → 0；fd 非法/标准流 → -EBADF。
//   4    stat              真（最小）：按路径走 vfs64_stat，填 144B struct stat；不存在 -ENOENT。
//   5    fstat             真：fd 0/1/2 → 字符设备最小三字段；fd>=3 → fd64_stat64（文件大小/目录 mode）。
//   6    lstat             **部分**：与 stat 同一实现（本文件系统没有符号链接，所以等价）。
//   8    lseek             真：fd>=3 → fd64_lseek64（SET/CUR/END，游标 0..67584）；fd 0..2 → -ESPIPE。
//   9    mmap              真（批次 C 起**每进程**）：进程内 bump 分配器（USER64_MMAP_VA64 起），
//                           页 P|U|W|NX；MAP_FIXED 按调用方地址；空间不足 -ENOMEM（已映射的页回滚）。
//                           没有进程上下文（任务 0 / 共享模式）时退回原来的共享窗口实现。
//   10   mprotect          真（每进程）：逐页改叶子权限（PROT_READ/WRITE/EXEC → 清 NX）。
//   11   munmap            真（每进程）：逐页解除映射 + page_free_64 回收 → 0；范围非法 -EINVAL。
//   12   brk               真（每进程）：首次调用映射 64KiB 可写区，之后按进程维护 brk 端点。
//   13   rt_sigaction      **只记录不投递**：只在**有进程上下文**时读用户 struct 的 handler 存进
//                           进程（无进程时直接返回 0）：内核没有任何信号投递路径（没有用户栈信号帧、
//                           没有 rt_sigreturn、没有 vDSO/restorer）。**不假装投递**。
//   14   rt_sigprocmask    **只记录不投递**：记录屏蔽字；语义同上。
//   15   rt_sigreturn      **-ENOSYS**：没有信号帧可恢复。
//   16   ioctl             真（最小）：TIOCGWINSZ → 25x80 struct winsize；TIOCSWINSZ 忽略返回 0；
//                           其它 → -ENOTTY（与 Linux 对非 tty 一致）。
//   20   writev            真：fd∈{1,2}，最多 8 个 iovec；iovcnt 超限 -EINVAL。
//   21   access            真（最小）：存在性检查（本内核没有权限模型 → mode 忽略，如实注明）。
//   22   pipe / pipe2      **真实现（批次 D）**：fd64 的 64 B 环形缓冲 + 读端/写端两个 fd（写进
//                           用户给的 int[2]）。**没有阻塞语义**：写满短写（无空间 -EAGAIN）、
//                           读空且写端开着 -EAGAIN、写端全关读 0（EOF）。fork 后父子各持一端可通信。
//   24   sched_yield       真：task_yield64() 让出到下一个 tick；没有调度器时直接返回 0。
//   32   dup               真（批次 D 修正语义）：fd64_dup64 —— 新旧 fd 指向**同一个打开文件对象**
//                           （共享偏移游标），引用计数 +1；fd<3 → -EBADF。
//   33   dup2              真（批次 D 修正语义）：目标 fd>=3 时先关旧目标再共享同一个对象；
//                           目标 <=2（重定向标准流）仍 -ENOSYS（没有真设备层）。
//   34   pause             **部分**：本内核不投递信号 -> 有界等待 1 秒后返回 -EINTR（绝不死等）。
//   35   nanosleep         真：走调度器睡眠（有界 60 秒上限，防挂死）。
//   37   alarm             **只记录不投递**：记进程的 alarm 秒数，返回上一次的值（不投 SIGALRM）。
//   39   getpid            真：**进程 pid**（批次 C 起；没有进程上下文时退回任务 id）。
//   56   clone             **部分**：只支持 flags=0 / SIGCHLD(17) 的 fork 语义；CLONE_VM|THREAD 等 → -ENOSYS。
//   57   fork              真：**整页物理复制**（不做 COW，取舍见 kernel/proc64.h 第 5 条）；
//                           子进程从父的 syscall 下一条指令继续、rax=0；父进程 rax=子 pid。
//   58   vfork             **等同 fork**（没有"共享地址空间直到 execve"的优化语义，如实注明）。
//   59   execve            真（静态 ELF）：释放旧映像 → 复用 elf64 装载 → 重建初始栈/auxv →
//                           改写帧直接回到新入口。argv 过 user64_range_ok64；envp 恒为空。
//                           ★ 装载失败会按退出码 127 终止进程（Linux 会保留旧映像继续跑，见 proc64.cpp）。
//   61   wait4             真：阻塞轮询（task_sleep64 + 有界 5 秒超时）；status = (code & 0xFF) << 8；
//                           支持 pid>0 与 -1（任一子）；WNOHANG(1) 支持。
//   62   kill              部分：SIGKILL(9)/SIGTERM(15) → 立即终止（TERM 记退出码 143）；
//                           其它信号"记录但不投递"；不允许自杀（-EINVAL，如实）。
//   63   uname             真：写一份静态 struct utsname（6 x 65B）。
//   74   fsync             **部分**：走到 0（本内核的 fd 只有只读缓存，没有脏数据要刷）。
//   79   getcwd            真：把 "/" 写进用户 buf（len>=2）并返回 buf 指针。
//   82   rename            **-ENOSYS**（vfs64 没有 rename 原语）。
//   83   mkdir             真（走 vfs64，真写盘）；单飞行者：并发调用返回 -EBUSY。
//   84   rmdir             **-ENOSYS**（vfs64 没有删目录原语）。
//   87   unlink            真（走 vfs64，真写盘）；单飞行者同上。
//   89   readlink          **部分**：只对 "/proc/self/exe" 返回当前进程的映像路径；其它 -ENOENT。
//   96   gettimeofday      真：用 g_ticks64（250Hz PIT）造近似值（tv_usec 4ms 粒度）；tz 恒 0。
//   97   getrlimit         部分：本内核没有 per-process 配额 → 返回 RLIM_INFINITY（STACK/NOFILE 给常量）。
//   102  getuid            真：0（没有用户/权限模型，如实）
//   104  getgid            真：0
//   110  getppid           真：**进程 ppid**（没有进程上下文时 0）。
//   158  arch_prctl        **真（批次 C 起写 MSR）**：ARCH_SET_FS 真写 IA32_FS_BASE(0xC0000100)
//                           并记进进程（任务切换时保存/恢复）；非规范地址 -EINVAL；
//                           GET_FS 写回用户指针；SET_GS/GET_GS 等 → -EINVAL。
//   160  setrlimit         **-EPERM**：没有配额可改（不假装成功）。
//   218  set_tid_address   真（最小）：返回 0（没有 clear_child_tid 唤醒路径）。
//   228  clock_gettime     真：PIT tick 造近似值；clockid 0..3；其它 → -EINVAL。
//   231  exit_group        真：**结束整个进程**（本内核 1 进程 1 任务，等价于 exit 的进程级语义）。
//   257  openat            open 的现代入口：dirfd 忽略（只支持 AT_FDCWD）；相对路径要求以 '/' 开头。
//   318  getrandom         真（**非密码学安全**）：ticks + TSC + xorshift 伪随机，单次上限 256 字节。
//
//   其它所有号：**-ENOSYS(-38)** 并且同一个号只打一次 `[SYSCALL] enosys nr=<n>`（防刷屏）。
//   已实现号里做不到的分支一律返回**具体负 errno**（-EBADF/-EFAULT/-EINVAL/-ENOMEM/...），
//   绝不返回 0 假装成功。
//
// ============================ 打点策略 ============================
//   int 0x80（旧路径，行为不变）：只对 write(1)/exit(2) 打 [SYSCALL] nr=...
//   syscall 指令（新路径）：**每个调用都打**
//     `[SYSCALL] insn nr=<n> rdi=<hex> rsi=<hex> rdx=<hex> ret=<hex>`
//     理由：这条路径目前只有演示程序在用（启动期个位数调用），全量打点给出的证据链最完整
//     （"哪个号真的进来了、返回了什么"一目了然），也便于自动验收 grep。代价是刷屏风险：
//     将来若有程序在循环里调 getpid，要么改成"只打 write/exit/失败"，要么加节流。
//   deny / enosys 两条路径共用。
//
// ============================ 已知边界（别把没做的说成做了）============================
//   * 地址空间：**批次 C 起每进程私有**（BIOS 路径，见 kernel/proc64.h）：fork 整页复制、
//     execve 换映像、wait4/kill 都有真语义。但在 **UEFI（固件页表）** 下运行期 mov cr3 不可用
//     （VMware EFI 下已知会立刻复位），于是自动进"共享地址空间模式"：fork/vfork/clone/execve/wait4
//     一律 -ENOSYS、brk/mmap 退回共享窗口、启动期多进程演示跳过 —— 串口打印
//     `[PROC64] cr3 isolation OFF ...` 如实标注，**绝不假装隔离成立**。
//   * 信号：**只记录不投递**（rt_sigaction/rt_sigprocmask 只记录；kill 只有 KILL/TERM 的立即终止）。
//   * 文件 fd（批次 D 起**每进程一张 fd 表**）：进程结构挂 FdTable64（fd64.h），fork 逐槽共享
//     打开文件对象（共享偏移）、execve 默认保留 fd、close 只是引用计数 -1；没有进程上下文
//     （终端/桌面 = 任务 0、安装介质内核）时退回**内核表**。底层是 vfs64（VimtuFS2 单层目录）：
//     路径只有 "/name"、单文件 <= 67584B、没有权限/子目录树；写是"整文件覆盖 + 立刻落盘"。
//     O_APPEND 真实现；pipe(22) 真实现（64B 环形缓冲、无阻塞语义）。边界详见 kernel/fd64.h 与
//     docs/应用层与系统调用说明.md 的 fd 语义表。
//   * glibc/发行版二进制**没有验证过**：PT_INTERP+动态链接器、完整 TLS/vDSO、真 futex、
//     clone/线程、socket/网络 ABI、/proc 与 pty、uid/gid 权限模型大多不在这里。
//     这里验证的是"自有静态 ELF64（ld.lld -static -nostdlib）能 load → ring3 → fork/execve/wait4 → exit"。
//     差距清单见 docs/应用层与系统调用说明.md 的"离 glibc 还差什么"。
//   * 用户态没有 swapgs/per-CPU gs：SYSCALL 入口**不能**用 gs 取内核数据结构，所以内核栈顶
//     只能靠内核内存里的镜像变量（见 syscall_entry64.asm；批次 C 起它是每任务一份的）。
#include "syscall64.h"
#include "usermode64.h"     // user64_range_ok64 / user64_exit_to_kernel64 / 用户窗口常量
#include "vfs64.h"          // Linux open/read 走真实文件系统
#include "fd64.h"           // 批次 B：FD 层（open/read/write/close/lseek/fstat/dup 的公共底座）
#include "fs64.h"           // ★ P4：统一卷分派（stat/access/chmod/chown 按当前卷 + 权限判定）

// ---- 批次 C：proc64（进程/地址空间）的**弱引用** ----
// proc64.cpp 只在系统内核里链接（安装介质内核没有进程/地址空间、没有 task64/elf64）。
// 所以这里全部按弱引用声明：安装内核里这些符号是 0，调用点必须**先判空**——
// 判空失败就按"这个功能不存在"处理（-ENOSYS 或退回共享实现），绝不假装成功。
int  proc64_isolate64()                      __attribute__((weak));
int  proc64_current_pid64()                  __attribute__((weak));
int  proc64_current_ppid64()                 __attribute__((weak));
int  proc64_exe_path64(char*, uint32_t)      __attribute__((weak));
uint64_t proc64_brk64(uint64_t)              __attribute__((weak));
int64_t  proc64_mmap64(uint64_t, uint64_t, uint64_t)      __attribute__((weak));
int64_t  proc64_munmap64(uint64_t, uint64_t)              __attribute__((weak));
int64_t  proc64_mprotect64(uint64_t, uint64_t, uint64_t)  __attribute__((weak));
int64_t  proc64_fork64(pt_regs64*)                        __attribute__((weak));
int64_t  proc64_execve64(pt_regs64*, const char*, const char* const*, uint32_t) __attribute__((weak));
int64_t  proc64_wait4(int, int*, uint32_t)                __attribute__((weak));
int64_t  proc64_kill64(int, int)                          __attribute__((weak));
int64_t  proc64_set_fs_base64(uint64_t)                   __attribute__((weak));
uint64_t proc64_get_fs_base64()                           __attribute__((weak));
int  proc64_record_sigaction64(int, uint64_t)             __attribute__((weak));
int  proc64_record_sigmask64(uint64_t)                    __attribute__((weak));
int  proc64_alarm_set64(int)                              __attribute__((weak));
// ★ P4：每进程凭证（proc64.cpp；安装内核不链它 -> weak 为 0 -> 退化为会话身份/EPERM）
int  proc64_get_cred64(uint32_t*, uint32_t*, uint32_t*, uint32_t*)    __attribute__((weak));
int  proc64_set_cred64(uint32_t, uint32_t, uint32_t, uint32_t)        __attribute__((weak));
static inline bool lx64_have_proc64() { return proc64_isolate64 != nullptr; }
#include "mem_64.h"         // PAGE_SIZE_64 / page_free_64 / PTE_*
#include "debug64.h"
#include "fb.h"             // 屏幕输出（fb_draw_text / fb_flip_region）
#include "proc64.h"         // 批次 C：进程/地址空间（fork/execve/wait4/kill/每进程 brk&mmap/FS 基址）

// task64.cpp 提供（安装程序内核不链接它 -> weak 引用后按"没有调度器"处理）
extern "C" void     task_sleep_ms64(uint32_t ms) __attribute__((weak));
extern "C" uint32_t task_current_id_64() __attribute__((weak));
extern "C" void     task_yield64() __attribute__((weak));

static const uint64_t SYSCALL64_WRITE_MAX = 1024;    // int 0x80 write 单次上限（见文件头说明）
static const uint64_t LX64_WRITE_MAX      = 4096;    // Linux write 单次上限
static const int64_t  LX64_EPERM  = 1;
static const int64_t  LX64_ENOENT = 2;
static const int64_t  LX64_ESRCH  = 3;
static const int64_t  LX64_EINTR  = 4;
static const int64_t  LX64_EBADF  = 9;
static const int64_t  LX64_ECHILD = 10;
static const int64_t  LX64_EAGAIN = 11;
static const int64_t  LX64_ENOMEM = 12;
static const int64_t  LX64_EACCES = 13;
static const int64_t  LX64_EFAULT = 14;
static const int64_t  LX64_EBUSY  = 16;
static const int64_t  LX64_EINVAL = 22;
static const int64_t  LX64_EMFILE = 24;
static const int64_t  LX64_ENOTTY = 25;
static const int64_t  LX64_ESPIPE = 29;
static const int64_t  LX64_ENOSYS = 38;
static const int64_t  LX64_ENOTEMPTY = 39;

// 这些 errno 目前没有调用点，但它们是**对外承诺的错误码表**（文档/测试按这个口径读）；
// 这里用 static_assert 钉住取值 —— 顺带消掉 -Wextra 的"未被引用"告警（本文件要求零告警）。
static_assert(LX64_ESRCH == 3 && LX64_ECHILD == 10 && LX64_EAGAIN == 11 && LX64_EMFILE == 24 &&
              LX64_ESPIPE == 29 && LX64_ENOTEMPTY == 39, "LX64_* 错误码表（= -errno）");

// 批次 C：有"当前进程"（且隔离模式开着）时，brk/mmap/mprotect/munmap 走每进程实现；
// 否则退回原来的共享窗口实现（UEFI/固件页表 -> 共享地址空间模式，行为与批次 B 完全一致）。
static inline bool lx64_per_proc_mm64() {
    return lx64_have_proc64() && proc64_current_pid64() > 0;
}
// ==================== syscall 指令路径的跨模块变量 ====================
// ==================== syscall 指令路径的跨模块变量 ====================
// ★ syscall 指令入口**专用内核栈**（16KiB，静态 .bss，不进镜像）。
//   为什么不用 TSS.rsp0：TSS.rsp0 是"ring3 -> ring0 的中断/异常"切栈用的栈顶，中断帧压在
//   [rsp0-0xD0, rsp0)。SYSCALL 不换栈，入口若也用同一个 rsp0，两者就会**共用同一段内存**
//   ——中断帧与 syscall 帧的 rip/cs/rflags/rsp/ss 槽地址完全重合，任何时序重叠都会让后写的
//   帧覆盖先写的那个（rip/cs 槽首当其冲）。专用栈把这条耦合彻底断开：
//   SYSCALL 入口 -> 分发（全程 IF=0，不可能嵌套）-> sysret；TSS.rsp0 只留给中断。
//   （代价：多 16KiB .bss；好处：syscall 路径和调度/中断路径再无共享内存。）
static uint8_t g_syscall64_stack64[16 * 1024] __attribute__((aligned(16)));
// 内核栈顶（入口汇编 mov rsp, [g_syscall64_kstack64] 用）。
// ★ 批次 C：它是**每任务一份**的（有调度器时由 task64.cpp 的 task_apply_ctx64 在切换时更新；
//   任务 0 / 没有调度器时保持初值 = 下面那块静态专用栈顶）。
extern "C" uint64_t g_syscall64_kstack64 =
    (uint64_t)(uintptr_t)(g_syscall64_stack64 + sizeof(g_syscall64_stack64));
// 静态专用栈顶（任务 0 / 没有调度器时用）：task64.cpp 与 usermode64.cpp 都靠它兜底。
// 为什么保留它而不是彻底删掉这块 .bss：安装介质内核不链接 task64.cpp（没有任务表），
// 那里的 syscall 指令路径仍然需要一个确定的切栈目标。
uint64_t syscall64_static_kstack_top64() {
    return (uint64_t)(uintptr_t)(g_syscall64_stack64 + sizeof(g_syscall64_stack64));
}
// 1 = 本帧已走 exit，入口别 sysret（见 syscall_entry64.asm 的出口 B）
extern "C" uint64_t g_syscall64_exit_to_kernel64 = 0;

// ==================== 打点 ====================
static void syscall64_log64(uint64_t nr, uint64_t rdi, uint64_t rsi, uint64_t rdx, int64_t ret) {
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] nr=");
    dbg64_dec(nr);
    dbg64_str(" rdi=");
    dbg64_hex64(rdi);
    dbg64_str(" rsi=");
    dbg64_hex64(rsi);
    dbg64_str(" rdx=");
    dbg64_hex64(rdx);
    dbg64_str(" ret=");
    dbg64_hex64((uint64_t)ret);
    dbg64_nl();
    dbg64_line_end64();
}

// syscall 指令路径的打点：格式与上面不同（多一个 insn 标记），自动验收靠它区分两条路径。
static void syscall64_log_insn64(uint64_t nr, uint64_t rdi, uint64_t rsi, uint64_t rdx, int64_t ret) {
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] insn nr=");
    dbg64_dec(nr);
    dbg64_str(" rdi=");
    dbg64_hex64(rdi);
    dbg64_str(" rsi=");
    dbg64_hex64(rsi);
    dbg64_str(" rdx=");
    dbg64_hex64(rdx);
    dbg64_str(" ret=");
    dbg64_hex64((uint64_t)ret);
    dbg64_nl();
    dbg64_line_end64();
}

static void syscall64_deny64(uint64_t nr, uint64_t arg) {
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] deny nr=");
    dbg64_dec(nr);
    dbg64_str(" arg=");
    dbg64_hex64(arg);
    dbg64_nl();
    dbg64_line_end64();
}

// 未实现的号：每个号只打一次（防刷屏），返回值仍然是 -ENOSYS。
static void syscall64_enosys_once64(uint64_t nr) {
    static uint8_t seen[512];
    if (nr < sizeof(seen)) {
        if (seen[nr]) return;
        seen[nr] = 1;
    }
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] enosys nr=");
    dbg64_dec(nr);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 屏幕输出（write 用）====================
// 串口走 dbg64_putc（同时写 debugcon），屏幕走 8x8 内置字体 + 局部提交。
// 只画一行（覆盖上一次的内容）：用户态日志很短，够证明"输出到了屏幕"。
static void syscall64_screen_puts64(const char* s, uint64_t n) {
    if (fb_width() <= 0) return;                     // 还没建帧缓冲（理论上不会）
    static char line[100];
    uint32_t k = 0;
    for (uint64_t i = 0; i < n && k < sizeof(line) - 1; i++) {
        char c = s[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        line[k++] = c;
    }
    if (k == 0) return;
    line[k] = 0;
    const int x = 8, y = 8;
    fb_draw_text(x, y, line, 0xFFFFFFFFu, 0xFF000000u, 1);   // 白字黑底
    fb_flip_region(x, y, (int)k * 8 + 2, 10);
}

// ==================== 入口 1）int 0x80：write(fd, buf, len) ====================
static int64_t syscall64_write64(uint64_t nr, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd != 1) { syscall64_deny64(nr, fd); return -1; }            // 只支持 stdout
    if (len == 0 || len > SYSCALL64_WRITE_MAX) { syscall64_deny64(nr, len); return -1; }
    if (!user64_range_ok64(buf, len)) { syscall64_deny64(nr, buf); return -1; }

    const char* p = (const char*)(uintptr_t)buf;                     // 已校验：是用户页，ring0 可读
    for (uint64_t i = 0; i < len; i++) dbg64_putc(p[i]);
    syscall64_screen_puts64(p, len);
    return (int64_t)len;
}

// ==================== 入口 1）int 0x80：sleep_ms(ms) ====================
static int64_t syscall64_sleep64(uint32_t ms) {
    if (ms == 0) return 0;
    if (task_sleep_ms64) { task_sleep_ms64(ms); return 0; }
    // 没有调度器（安装程序内核）：退化为按 tick 忙等，别死自旋
    const uint64_t until = g_ticks64 + ms_to_ticks64(ms);
    uint64_t guard = 0;
    while (g_ticks64 < until && ++guard < 2000000000ULL) __asm__ volatile("hlt");
    return 0;
}

// ==================================================================================
// 入口 2）syscall 指令：Linux x86_64 号段（映射表见文件头）
// ==================================================================================

static void lx64_wr32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void lx64_wr64(uint8_t* p, uint64_t v) {
    lx64_wr32(p, (uint32_t)v);
    lx64_wr32(p + 4, (uint32_t)(v >> 32));
}
// 拷到用户态（调用方保证已过 user64_range_ok64）
static void lx64_copy_to_user64(uint64_t uva, const void* src, uint64_t n) {
    const uint8_t* s = (const uint8_t*)src;
    uint8_t* d = (uint8_t*)(uintptr_t)uva;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}
// 从用户态拷一个 NUL 结尾的短字符串（逐字节校验：一旦跨出已映射的用户页立刻拒绝）。
// 返回 0 = 成功（out 已 NUL 结尾）；-1 = 越界 / 超长 / 没有 NUL。
static int lx64_user_str64(uint64_t uva, char* out, uint32_t cap) {
    if (cap == 0) return -1;
    for (uint32_t i = 0; i + 1u < cap; i++) {
        if (!user64_range_ok64(uva + i, 1)) return -1;
        const char c = *(const char*)(uintptr_t)(uva + i);
        out[i] = c;
        if (c == 0) return 0;
    }
    out[cap - 1u] = 0;
    return -1;
}

// ---- fd 表：交给 kernel/fd64.cpp 的 FD 层（32 项；路径单层、单文件 <= 67584 B）----
// 本轮把这里原来那份"只读、缓存前 512B、只有 4 项"的内联小表整个删掉：
//   * open/read/close/lseek/fstat/dup/fsync 全部转调 fd64_*；
//   * 终端文件命令与 ring3 系统调用因此看到**同一张表**（如实边界见 fd64.h）；
//   * 读不到 offset 原语的限制由 fd64 内部处理（它读整文件到自己的缓冲再切片）。
static const uint32_t LX64_PATH_MAX = 32;       // VFS64_NAME_MAX(27) + "/" + NUL
static const uint32_t LX64_READ_MAX = 4096;     // 单次 read 上限（fd64 内部读整文件，这里只限制拷贝量）
// ring3 read 的内核 bounce（syscall 路径全程 IF=0：单 CPU 上不会被别的任务穿插）
static uint8_t g_lx_readbuf64[LX64_READ_MAX];
// ---- 0）read ----（fd >= 3 走 fd64；stdin 没有输入流 -> 立刻 EOF，如实）
static int64_t lx64_read64(uint64_t nr, uint64_t fd, uint64_t buf, uint64_t len) {
    if (len == 0) return 0;
    if (fd == 0) return 0;                                  // stdin：没有输入流 -> 立刻 EOF（如实）
    if (fd < 3) { syscall64_deny64(nr, fd); return -LX64_EBADF; }
    if (!user64_range_ok64(buf, len)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }
    if (len > LX64_READ_MAX) len = LX64_READ_MAX;
    const int n = fd64_read64((int)fd, g_lx_readbuf64, (int)len);
    if (n < 0) return (int64_t)n;                           // fd64 的负错误码 = -errno（同一口径）
    if (n > 0) lx64_copy_to_user64(buf, g_lx_readbuf64, (uint64_t)n);
    return (int64_t)n;
}

// ---- 1）write ----（fd 1/2 = 串口/屏幕；fd >= 3 = 真文件，走 fd64）
static int64_t lx64_write64(uint64_t nr, uint64_t fd, uint64_t buf, uint64_t len) {
    if (fd == 1 || fd == 2) {
        if (len == 0) return 0;
        if (len > LX64_WRITE_MAX) { syscall64_deny64(nr, len); return -LX64_EINVAL; }
        if (!user64_range_ok64(buf, len)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }
        const char* p = (const char*)(uintptr_t)buf;
        for (uint64_t i = 0; i < len; i++) dbg64_putc(p[i]);
        if (fd == 1) syscall64_screen_puts64(p, len);
        return (int64_t)len;
    }
    if (fd < 3) { syscall64_deny64(nr, fd); return -LX64_EBADF; }
    if (len == 0) return 0;
    if (len > LX64_WRITE_MAX) { syscall64_deny64(nr, len); return -LX64_EINVAL; }
    if (!user64_range_ok64(buf, len)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }
    // 用户指针已经过范围校验、且当前 CR3 就是该进程的地址空间：fd64 直接按内核指针读它是安全的
    const int n = fd64_write64((int)fd, (const void*)(uintptr_t)buf, (int)len);
    return (int64_t)n;
}


// ---- 2 / 257）open / openat ----（走 fd64；flags 按 Linux 子集原样映射，dirfd 仍然忽略）
static int64_t lx64_open64(uint64_t nr, uint64_t path_va, uint64_t flags) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(nr, path_va); return -LX64_EFAULT; }
    if (path[0] != '/') { syscall64_deny64(nr, path_va); return -LX64_EINVAL; }   // 阶段一只支持单层绝对路径
    const int fd = fd64_open64(path, (uint32_t)flags);
    return (fd < 0) ? (int64_t)fd : (int64_t)fd;             // fd64 的负错误码 = -errno（同一口径）
}

// ---- 3）close ----（标准流没有内核对象：与旧行为一致 -> -EBADF）
static int64_t lx64_close64(uint64_t nr, uint64_t fd) {
    const int r = fd64_close64((int)fd);
    if (r != 0) { syscall64_deny64(nr, fd); return r; }
    return 0;
}
// ---- 5）fstat / 4）stat / 6）lstat（x86_64 的 struct stat = 144 字节，字段偏移见 Linux asm/stat.h）----
static const uint32_t LX64_S_IFCHR = 0020000u;
static const uint32_t LX64_S_IFDIR = 0040000u;
static const uint32_t LX64_S_IFREG = 0100000u;
// ★ P4：st_uid/st_gid 不再恒 0；mode 也不再恒 0755/0444 —— 由调用方给出真实值。
static void lx64_fill_stat64_ex(uint8_t* st, uint32_t mode, uint64_t size, uint32_t uid, uint32_t gid) {
    for (uint32_t i = 0; i < 144; i++) st[i] = 0;
    lx64_wr64(st + 0,  1);                          // st_dev
    lx64_wr64(st + 8,  1);                          // st_ino
    lx64_wr64(st + 16, 1);                          // st_nlink
    lx64_wr32(st + 24, mode);                       // st_mode
    lx64_wr32(st + 28, uid);                        // st_uid
    lx64_wr32(st + 32, gid);                        // st_gid
    lx64_wr64(st + 48, size);                       // st_size
    lx64_wr64(st + 56, 4096);                       // st_blksize
    lx64_wr64(st + 64, (size + 511) / 512);         // st_blocks
}
static void lx64_fill_stat64(uint8_t* st, uint32_t mode, uint64_t size) {
    lx64_fill_stat64_ex(st, mode, size, 0, 0);
}
static int64_t lx64_fstat64(uint64_t nr, uint64_t fd, uint64_t st_va) {
    if (!user64_range_ok64(st_va, 144)) { syscall64_deny64(nr, st_va); return -LX64_EFAULT; }
    uint8_t st[144];
    if (fd <= 2) {
        lx64_fill_stat64(st, LX64_S_IFCHR | 0666u, 0);      // 标准流：最小三字段（mode/nlink/size）
    } else {
        int fvol = -1;
        char fpath[FD64_PATH_MAX];
        if (fd64_where64((int)fd, &fvol, fpath, (int)sizeof(fpath)) == 0) {
            Fs64Stat64 si;
            if (fs64_stat64(fvol, fpath, &si) == 0) {
                uint32_t mode = si.mode;
                if ((mode & VFS64_S_IFMT) == 0) mode = (si.type == VFS64_TYPE_DIR) ? (LX64_S_IFDIR | 0755u) : (LX64_S_IFREG | 0644u);
                lx64_fill_stat64_ex(st, mode, si.size, si.uid, si.gid);   // ★ P4：真实 uid/gid/mode
            } else {
                uint32_t ty = 0, sz = 0;
                const int r = fd64_stat64((int)fd, &ty, &sz);
                if (r != 0) { syscall64_deny64(nr, fd); return r; }
                lx64_fill_stat64(st, (ty == VFS64_TYPE_DIR) ? (LX64_S_IFDIR | 0755u) : (LX64_S_IFREG | 0644u), sz);
            }
        } else {
            uint32_t ty = 0, sz = 0;
            const int r = fd64_stat64((int)fd, &ty, &sz);
            if (r != 0) { syscall64_deny64(nr, fd); return r; }
            lx64_fill_stat64(st, (ty == VFS64_TYPE_DIR) ? (LX64_S_IFDIR | 0755u) : (LX64_S_IFREG | 0644u), sz);
        }
    }
    lx64_copy_to_user64(st_va, st, 144);
    return 0;
}

// ---- 8）lseek ----（fd >= 3 走 fd64；标准流 -> -ESPIPE）
static int64_t lx64_lseek64(uint64_t nr, uint64_t fd, int64_t off, uint64_t whence) {
    const int64_t r = fd64_lseek64((int)fd, off, (int)whence);
    if (r < 0) { syscall64_deny64(nr, fd); return r; }
    return r;
}

// ---- 9）mmap：用户窗口内的 bump 分配器 ----
static uint64_t g_lx_mmap_next64 = 0;
static int64_t lx64_mmap64(uint64_t len, uint64_t flags, uint64_t addr) {
    if (len == 0) return -LX64_EINVAL;
    uint64_t n = (len + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
    if (g_lx_mmap_next64 == 0) g_lx_mmap_next64 = USER64_MMAP_VA64;
    uint64_t va;
    if (flags & 0x10u) {                                       // MAP_FIXED：按调用方给的地址
        va = addr & ~((uint64_t)PAGE_SIZE_64 - 1);
    } else {
        va = (g_lx_mmap_next64 + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
    }
    if (va < USER64_MMAP_VA64 || va > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return -LX64_ENOMEM;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - va) return -LX64_ENOMEM;
    for (uint64_t a = va; a < va + n; a += PAGE_SIZE_64) {
        uint64_t phys = 0;
        if (!user64_map_page64(a, PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64, 1, &phys)) return -LX64_ENOMEM;
    }
    user64_paging_sync64();
    if (va + n > g_lx_mmap_next64) g_lx_mmap_next64 = va + n;
    return (int64_t)va;
}

// ---- 10）mprotect：逐页改叶子权限（PROT_READ=1 / WRITE=2 / EXEC=4）----
static int64_t lx64_mprotect64(uint64_t addr, uint64_t len, uint64_t prot) {
    if (len == 0) return 0;
    const uint64_t n = (len + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
    if (addr < USER64_CODE_VA64 || addr > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return -LX64_EINVAL;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - addr) return -LX64_EINVAL;
    const uint64_t flags = PTE_USER_64
                         | ((prot & 2u) ? PTE_WRITE_64 : 0u)
                         | ((prot & 4u) ? 0u : PTE_NX_64);
    for (uint64_t a = addr; a < addr + n; a += PAGE_SIZE_64) {
        if (!user64_remap_flags64(a, flags)) return -LX64_EINVAL;
    }
    user64_paging_sync64();
    return 0;
}

// ---- 11）munmap：逐页解除映射 + 回收物理页 ----
static int64_t lx64_munmap64(uint64_t addr, uint64_t len) {
    if (len == 0) return 0;
    const uint64_t n = (len + PAGE_SIZE_64 - 1) & ~((uint64_t)PAGE_SIZE_64 - 1);
    if (addr < USER64_CODE_VA64 || addr > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return -LX64_EINVAL;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - addr) return -LX64_EINVAL;
    for (uint64_t a = addr; a < addr + n; a += PAGE_SIZE_64) {
        const uint64_t phys = user64_unmap_page64(a);
        if (!phys) return -LX64_EINVAL;                       // 未映射：Linux 也返回 -EINVAL
        page_free_64((void*)(uintptr_t)phys);
    }
    user64_paging_sync64();
    return 0;
}

// ---- 12）brk：固定 64KiB 可写区，首次调用时整块映射 ----
static uint64_t g_lx_brk64 = 0;
static int64_t lx64_brk64(uint64_t addr) {
    if (g_lx_brk64 == 0) {
        for (uint64_t a = USER64_BRK_VA64; a < USER64_BRK_VA64 + USER64_BRK_BYTES64; a += PAGE_SIZE_64) {
            uint64_t phys = 0;
            if (!user64_map_page64(a, PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64, 1, &phys)) return -LX64_ENOMEM;
        }
        user64_paging_sync64();
        g_lx_brk64 = USER64_BRK_VA64;
    }
    if (addr == 0) return (int64_t)g_lx_brk64;                                  // brk(0)：问当前 brk
    if (addr < USER64_BRK_VA64 || addr > USER64_BRK_VA64 + USER64_BRK_BYTES64) {
        return (int64_t)g_lx_brk64;                                            // 失败：返回旧 brk（Linux 同）
    }
    g_lx_brk64 = addr;
    return (int64_t)addr;
}

// ---- 16）ioctl（最小但真：TIOCGWINSZ 写回 25x80）----
struct LxWinsize64 { uint16_t row, col, xpixel, ypixel; };
static int64_t lx64_ioctl64(uint64_t nr, uint64_t fd, uint64_t req, uint64_t arg) {
    if (fd > 2) { syscall64_deny64(nr, fd); return -LX64_EBADF; }
    if (req == 0x5413u) {                                          // TIOCGWINSZ
        if (!user64_range_ok64(arg, sizeof(LxWinsize64))) { syscall64_deny64(nr, arg); return -LX64_EFAULT; }
        LxWinsize64 ws;
        ws.row = 25; ws.col = 80; ws.xpixel = 640; ws.ypixel = 200;  // 终端是 8x8 字体字符网格
        lx64_copy_to_user64(arg, &ws, sizeof(ws));
        return 0;
    }
    if (req == 0x5414u) return 0;                                  // TIOCSWINSZ：忽略（我们改不了分辨率）
    return -LX64_ENOTTY;                                           // 其它请求：与 Linux 对非 tty 一致
}

// ---- 20）writev ----
static int64_t lx64_writev64(uint64_t nr, uint64_t fd, uint64_t iov_va, uint64_t iovcnt) {
    if (fd != 1 && fd != 2) { syscall64_deny64(nr, fd); return -LX64_EBADF; }
    if (iovcnt == 0) return 0;
    if (iovcnt > 8) { syscall64_deny64(nr, iovcnt); return -LX64_EINVAL; }
    if (!user64_range_ok64(iov_va, iovcnt * 16)) { syscall64_deny64(nr, iov_va); return -LX64_EFAULT; }
    int64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        const uint8_t* e = (const uint8_t*)(uintptr_t)(iov_va + i * 16);
        uint64_t base = 0, len = 0;
        for (int k = 0; k < 8; k++) { base |= (uint64_t)e[k] << (8 * k); len |= (uint64_t)e[8 + k] << (8 * k); }
        if (len == 0) continue;
        const int64_t r = lx64_write64(nr, fd, base, len);
        if (r < 0) return r;
        total += r;
    }
    return total;
}

// ---- 63）uname ----
static int64_t lx64_uname64(uint64_t nr, uint64_t buf) {
    if (!user64_range_ok64(buf, 390)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }   // 6 x 65
    static const char* const F[6] = { "VimtuOS", "vimtu64", "0.1.0-vimtu64",
                                      "#1 ring3+syscall", "x86_64", "(none)" };
    uint8_t out[390];
    for (uint32_t i = 0; i < 390; i++) out[i] = 0;
    for (uint32_t f = 0; f < 6; f++) {
        uint32_t k = 0;
        while (F[f][k] && k < 64) { out[f * 65u + k] = (uint8_t)F[f][k]; k++; }
    }
    lx64_copy_to_user64(buf, out, 390);
    return 0;
}

// ---- 79）getcwd：把 "/" 写进用户 buf，返回 buf 指针（Linux 语义）----
static int64_t lx64_getcwd64(uint64_t nr, uint64_t buf, uint64_t size) {
    if (size < 2) return -LX64_EINVAL;
    const uint64_t n = (size < 64) ? size : 64;
    if (!user64_range_ok64(buf, n)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }
    uint8_t* d = (uint8_t*)(uintptr_t)buf;
    d[0] = '/';
    d[1] = 0;
    return (int64_t)buf;
}

// ---- 228）clock_gettime：用 PIT tick 造近似值 ----
static int64_t lx64_clock_gettime64(uint64_t nr, uint64_t clockid, uint64_t ts_va) {
    if (clockid > 3) return -LX64_EINVAL;                       // CLOCK_REALTIME/MONOTONIC/PROCESS_CPUTIME/THREAD
    if (!user64_range_ok64(ts_va, 16)) { syscall64_deny64(nr, ts_va); return -LX64_EFAULT; }
    const uint64_t t = g_ticks64;
    uint8_t ts[16];
    lx64_wr64(ts + 0, t / PIT_HZ_64);                           // tv_sec
    lx64_wr64(ts + 8, (t % PIT_HZ_64) * (1000000000ULL / PIT_HZ_64));  // tv_nsec（250Hz -> 4ms 粒度）
    lx64_copy_to_user64(ts_va, ts, 16);
    return 0;
}

// ==================================================================================
// 批次 C 新增/改写的号（Linux x86_64 号段）：进程、每进程内存、以及一批能实现的补充号
// 每一条的实现状态都在文件头的大表里如实标注（真实现 / 部分 / -ENOSYS）。
// ==================================================================================

// ---- 12）brk / 9）mmap / 10）mprotect / 11）munmap：每进程（有进程时）----
// 为什么保留旧的共享实现：UEFI（固件页表）下没有每进程地址空间 -> 进程建不出来，
// brk/mmap 仍然只能落在引导期那个共享窗口里，行为与批次 B 完全一致（这是"降级但不装"）。
static int64_t lx64_brk_disp64(uint64_t addr) {
    if (lx64_per_proc_mm64()) return (int64_t)proc64_brk64(addr);
    return lx64_brk64(addr);
}
static int64_t lx64_mmap_disp64(uint64_t len, uint64_t flags, uint64_t addr) {
    if (lx64_per_proc_mm64()) return proc64_mmap64(len, flags, addr);
    return lx64_mmap64(len, flags, addr);
}
static int64_t lx64_mprotect_disp64(uint64_t addr, uint64_t len, uint64_t prot) {
    if (lx64_per_proc_mm64()) return proc64_mprotect64(addr, len, prot);
    return lx64_mprotect64(addr, len, prot);
}
static int64_t lx64_munmap_disp64(uint64_t addr, uint64_t len) {
    if (lx64_per_proc_mm64()) return proc64_munmap64(addr, len);
    return lx64_munmap64(addr, len);
}

// ---- 39）getpid / 110）getppid：有进程时返回**进程 pid**（Linux 语义），否则退回任务 id ----
static int64_t lx64_getpid64() {
    if (!lx64_have_proc64()) return task_current_id_64 ? (int64_t)task_current_id_64() : 0;
    const int pid = proc64_current_pid64();
    if (pid > 0) return (int64_t)pid;
    return task_current_id_64 ? (int64_t)task_current_id_64() : 0;
}
static int64_t lx64_getppid64() {
    return lx64_have_proc64() ? (int64_t)proc64_current_ppid64() : 0;
}
// ---- 57/58/56）fork / vfork / clone ----
// vfork 在 Linux 里是"共享地址空间直到 execve"的优化；本内核没有 COW，也没必要为演示程序
// 保留那个语义，所以 vfork **等同 fork**（如实注释，不假装）。
// clone：只支持 flags=0（或只带 SIGCHLD(17) 的 fork 语义），其余（CLONE_VM/CLONE_THREAD/…）-> -ENOSYS。
static int64_t lx64_fork64(uint64_t nr, pt_regs64* r) {
    if (!lx64_have_proc64()) { syscall64_enosys_once64(nr); return -LX64_ENOSYS; }
    return proc64_fork64(r);
}
static int64_t lx64_clone64(uint64_t nr, pt_regs64* r, uint64_t flags) {
    if (flags != 0 && flags != 17u) return -LX64_ENOSYS;
    return lx64_fork64(nr, r);
}

// ---- 59）execve ----
// argv/envp 都是**用户态指针**：先过 user64_range_ok64 校验、再逐级拷进内核缓冲，
// 然后才允许释放旧映像（顺序不能反：拷完才丢地址空间，否则读 argv 时页已经没了）。
static const uint32_t LX64_EXEC_ARGV_MAX   = 8;
static const uint32_t LX64_EXEC_ARGV_BYTES = 96;
static int64_t lx64_execve64(pt_regs64* r, uint64_t path_va, uint64_t argv_va, uint64_t envp_va) {
    if (!lx64_have_proc64()) { syscall64_enosys_once64(59); return -LX64_ENOSYS; }   // 安装内核没有进程
    (void)envp_va;                                       // 本内核没有环境变量（如实：enviroment 恒为空）
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(59, path_va); return -LX64_EFAULT; }
    if (path[0] != '/') { syscall64_deny64(59, path_va); return -LX64_EINVAL; }

    static char argv_buf[LX64_EXEC_ARGV_MAX][LX64_EXEC_ARGV_BYTES];
    const char* argv[LX64_EXEC_ARGV_MAX + 1];
    uint32_t argc = 0;
    if (argv_va) {
        for (; argc < LX64_EXEC_ARGV_MAX; argc++) {
            if (!user64_range_ok64(argv_va + (uint64_t)argc * 8, 8)) { syscall64_deny64(59, argv_va); return -LX64_EFAULT; }
            const uint64_t p = *(const uint64_t*)(uintptr_t)(argv_va + (uint64_t)argc * 8);
            if (p == 0) break;
            if (lx64_user_str64(p, argv_buf[argc], LX64_EXEC_ARGV_BYTES) != 0) { syscall64_deny64(59, p); return -LX64_EFAULT; }
            argv[argc] = argv_buf[argc];
        }
    }
    argv[argc] = nullptr;
    return proc64_execve64(r, path, argv, argc);
}

// ---- 61）wait4 ----
static int64_t lx64_wait4_64(uint64_t pid, uint64_t status_va, uint64_t options) {
    if (!lx64_have_proc64()) { syscall64_enosys_once64(61); return -LX64_ENOSYS; }
    if (status_va && !user64_range_ok64(status_va, 4)) { syscall64_deny64(61, status_va); return -LX64_EFAULT; }
    int status = 0;
    const int64_t rc = proc64_wait4((int)(int32_t)pid, &status, (uint32_t)options);
    if (rc > 0 && status_va) {
        uint8_t b[4];
        lx64_wr32(b, (uint32_t)status);
        lx64_copy_to_user64(status_va, b, 4);
    }
    return rc;
}
// ---- 62）kill ----
static int64_t lx64_kill_wrap64(uint64_t pid, uint64_t sig) {
    if (!lx64_have_proc64()) { syscall64_enosys_once64(62); return -LX64_ENOSYS; }
    return proc64_kill64((int)(int32_t)pid, (int)(int32_t)sig);
}

// ---- 13/14/15）信号：只记录不投递 ----
// 想清楚再写：本内核没有"用户栈上构造信号帧 + rt_sigreturn 恢复"这套机制，
// 也没有 vDSO/restorer。所以 rt_sigaction/rt_sigprocmask **只记录**（handler 地址、屏蔽字），
// kill 也不投递（除 KILL/TERM 的立即终止语义，见 proc64_kill64）；
// rt_sigreturn 永远 -ENOSYS（框架会先拦 sigreturn，不会走到分发器，但这里仍如实返回）。
// 安装介质内核没链 proc64.cpp（lx64_have_proc64() == false）：那就只做参数校验、不记录。
static int64_t lx64_rt_sigaction64(uint64_t sig, uint64_t act_va, uint64_t old_va, uint64_t sigsetsize) {
    // struct sigaction { handler; flags(8B); restorer(8B); mask(8B) } = 32 字节（本内核只读 handler）
    if (act_va && user64_range_ok64(act_va, 32)) {
        const uint64_t handler = *(const uint64_t*)(uintptr_t)act_va;
        if (lx64_have_proc64()) (void)proc64_record_sigaction64((int)sig, handler);
    } else if (act_va) {
        syscall64_deny64(13, act_va);
        return -LX64_EFAULT;
    }
    if (old_va && user64_range_ok64(old_va, 32)) {
        uint8_t z[32];
        for (uint32_t i = 0; i < 32; i++) z[i] = 0;
        lx64_copy_to_user64(old_va, z, 32);
    }
    (void)sigsetsize;
    return 0;
}
static int64_t lx64_rt_sigprocmask64(uint64_t how, uint64_t set_va, uint64_t old_va, uint64_t sigsetsize) {
    (void)how;
    uint64_t mask = 0;
    if (set_va) {
        if (!user64_range_ok64(set_va, 8)) { syscall64_deny64(14, set_va); return -LX64_EFAULT; }
        mask = *(const uint64_t*)(uintptr_t)set_va;
        if (lx64_have_proc64()) (void)proc64_record_sigmask64(mask);
    }
    if (old_va) {
        if (!user64_range_ok64(old_va, 8)) { syscall64_deny64(14, old_va); return -LX64_EFAULT; }
        uint8_t b[8];
        lx64_wr64(b, mask);
        lx64_copy_to_user64(old_va, b, 8);
    }
    (void)sigsetsize;
    return 0;
}

// ---- 4/6）stat / lstat：按路径（走 fs64 的统一分派；不区分符号链接 —— 本文件系统没有）----
// ★ P4：st_uid/st_gid/st_mode 现在给**真实值**（之前恒 0/0755/0444）；缺 x 进不去 -> -EACCES。
static int64_t lx64_stat_path64(uint64_t nr, uint64_t path_va, uint64_t st_va) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(nr, path_va); return -LX64_EFAULT; }
    if (!user64_range_ok64(st_va, 144)) { syscall64_deny64(nr, st_va); return -LX64_EFAULT; }
    Fs64Stat64 si;
    const int rc = fs64_stat64(-1, path, &si);
    if (rc != 0) return (rc == -13) ? -LX64_EACCES : -LX64_ENOENT;
    uint32_t mode = si.mode;
    if ((mode & VFS64_S_IFMT) == 0) mode = (si.type == VFS64_TYPE_DIR) ? (LX64_S_IFDIR | 0755u) : (LX64_S_IFREG | 0644u);
    uint8_t st[144];
    lx64_fill_stat64_ex(st, mode, si.size, si.uid, si.gid);
    lx64_copy_to_user64(st_va, st, 144);
    return 0;
}
// ---- 21）access：真按 r/w/x 判定（★ P4；FAT/旧卷没有权限模型 -> 恒允许）----
static int64_t lx64_access64(uint64_t nr, uint64_t path_va, uint64_t mode) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(nr, path_va); return -LX64_EFAULT; }
    const int rc = fs64_access64(-1, path, (uint32_t)mode & 7u);
    if (rc == 0) return 0;
    if (rc == -13) return -LX64_EACCES;
    return -LX64_ENOENT;
}
// ==================== ★ P4：身份 / 模式 / 属主 系统调用 ====================
// 凭证来源（与 VFS 完全同一份，见 vfs64.h 的"★ P4 权限"）：
//   * 有进程上下文（ring3 程序）-> proc64 的**每进程** uid/gid/euid/egid；
//   * 没有（终端/桌面 = 任务 0、安装介质内核）-> vfs64 的会话身份（userdb64 设的那份）。
// 语义裁剪（如实标注）：
//   * x86_64 **没有** seteuid/setegid 系统调用（glibc 用 setresuid/setresgid 实现）—— 这里实现
//     105 setuid / 106 setgid / 113 setreuid / 114 setregid / 117 setresgid / 118 setresuid；
//     "-1"（0xFFFFFFFF）表示"这一项不改"（Linux 语义）。
//   * **不维护 saved-set-uid**（setresuid 的第三项忽略）；因此"root 降权之后还能不能再回来"按
//     Linux 的 setuid 语义走：root 调 setuid(x) 会把 ruid/euid 都设成 x，之后就不是 root 了（不可逆）。
static void lx64_cred64(uint32_t* uid, uint32_t* gid, uint32_t* euid, uint32_t* egid) {
    uint32_t u = 0, g = 0, eu = 0, eg = 0;
    if (proc64_get_cred64 && proc64_get_cred64(&u, &g, &eu, &eg) == 0) {
        if (uid) *uid = u; if (gid) *gid = g; if (euid) *euid = eu; if (egid) *egid = eg;
        return;
    }
    Vfs64Cred64 c;
    vfs64_get_cred64(&c);
    if (uid) *uid = c.uid; if (gid) *gid = c.gid; if (euid) *euid = c.euid; if (egid) *egid = c.egid;
}
// 统一入口：-1（0xFFFFFFFF）= 不改；非 root 只允许在"已有的 uid/euid（gid/egid）"里取值。
// 没有进程上下文（安装内核）-> -EPERM（诚实：那里没有身份可改）。
static int64_t lx64_apply_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    uint32_t cu = 0, cg = 0, ceu = 0, ceg = 0;
    lx64_cred64(&cu, &cg, &ceu, &ceg);
    const uint32_t nu  = (uid  == 0xFFFFFFFFu) ? cu  : uid;
    const uint32_t ng  = (gid  == 0xFFFFFFFFu) ? cg  : gid;
    const uint32_t neu = (euid == 0xFFFFFFFFu) ? ceu : euid;
    const uint32_t neg = (egid == 0xFFFFFFFFu) ? ceg : egid;
    if (ceu != 0) {                                   // 非 root：不能凭空提权
        const bool uok = (nu == cu || nu == ceu) && (neu == cu || neu == ceu);
        const bool gok = (ng == cg || ng == ceg) && (neg == cg || neg == ceg);
        if (!uok || !gok) return -LX64_EPERM;
    }
    if (!(proc64_set_cred64 && proc64_current_pid64 && proc64_current_pid64() > 0)) return -LX64_EPERM;
    if (proc64_set_cred64(nu, ng, neu, neg) != 0) return -LX64_EPERM;
    return 0;
}
static int64_t lx64_getuid64()  { uint32_t v = 0; lx64_cred64(&v, nullptr, nullptr, nullptr); return (int64_t)v; }
static int64_t lx64_geteuid64() { uint32_t v = 0; lx64_cred64(nullptr, nullptr, &v, nullptr); return (int64_t)v; }
static int64_t lx64_getgid64()  { uint32_t v = 0; lx64_cred64(nullptr, &v, nullptr, nullptr); return (int64_t)v; }
static int64_t lx64_getegid64() { uint32_t v = 0; lx64_cred64(nullptr, nullptr, nullptr, &v); return (int64_t)v; }
// chmod/fchmod：mode 原样交给 vfs64（它只接受类型位 + 0777）；越权/只读卷/旧卷按 errno 原样回报。
static int64_t lx64_chmod_path64(uint64_t path_va, uint64_t mode) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) return -LX64_EFAULT;
    const int rc = fs64_chmod64(-1, path, (uint32_t)mode);
    if (rc == 0) return 0;
    if (rc == -13) return -LX64_EACCES;
    return -LX64_EPERM;                              // -1：非属主/非 root / 不存在 / 旧卷无字段
}
static int64_t lx64_fchmod64(uint64_t fd, uint64_t mode) {
    int fvol = -1;
    char fpath[FD64_PATH_MAX];
    const int r = fd64_where64((int)fd, &fvol, fpath, (int)sizeof(fpath));
    if (r != 0) return r;
    const int rc = fs64_chmod64(fvol, fpath, (uint32_t)mode);
    if (rc == 0) return 0;
    if (rc == -13) return -LX64_EACCES;
    return -LX64_EPERM;
}
// chown/fchown：只有 root 能改（vfs64 内部判定）；owner/group 传 -1 = 不改那一项（Linux 语义）。
static int64_t lx64_chown_path64(uint64_t path_va, uint64_t owner, uint64_t group) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) return -LX64_EFAULT;
    const int rc = fs64_chown64(-1, path, (uint32_t)owner, (uint32_t)group);
    if (rc == 0) return 0;
    if (rc == -13) return -LX64_EACCES;
    return -LX64_EPERM;                              // 非 root / 不存在 / 旧卷无字段
}
static int64_t lx64_fchown64(uint64_t fd, uint64_t owner, uint64_t group) {
    int fvol = -1;
    char fpath[FD64_PATH_MAX];
    const int r = fd64_where64((int)fd, &fvol, fpath, (int)sizeof(fpath));
    if (r != 0) return r;
    const int rc = fs64_chown64(fvol, fpath, (uint32_t)owner, (uint32_t)group);
    if (rc == 0) return 0;
    if (rc == -13) return -LX64_EACCES;
    return -LX64_EPERM;
}
// umask：只影响**新建**文件/目录的模式（vfs64 内部：默认模式 & ~umask）。返回旧值（Linux 语义）。
static int64_t lx64_umask64(uint64_t mask) { return (int64_t)vfs64_umask64((uint32_t)mask); }

// ---- 89）readlink：只对 /proc/self/exe 给出真实值（当前进程的映像路径）----
static int64_t lx64_readlink64(uint64_t nr, uint64_t path_va, uint64_t buf_va, uint64_t size) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(nr, path_va); return -LX64_EFAULT; }
    if (size == 0) return -LX64_EINVAL;
    char exe[PROC64_PATH_MAX];
    const int have = lx64_have_proc64() ? proc64_exe_path64(exe, (uint32_t)sizeof(exe)) : 0;
    if (have == 0 && !(path[0] == '/' && path[1] == 'p' && path[2] == 'r' && path[3] == 'o' &&
                       path[4] == 'c' && path[5] == '/' && path[6] == 's' && path[7] == 'e' &&
                       path[8] == 'l' && path[9] == 'f' && path[10] == '/' && path[11] == 'e' &&
                       path[12] == 'x' && path[13] == 'e' && path[14] == 0)) {
        return -LX64_ENOENT;                              // 只有 /proc/self/exe 有值（其它如实拒绝）
    }
    uint32_t n = 0;
    while (exe[n] && n + 1u < (uint32_t)size) n++;
    if (!user64_range_ok64(buf_va, n)) { syscall64_deny64(nr, buf_va); return -LX64_EFAULT; }
    lx64_copy_to_user64(buf_va, exe, n);                  // readlink 不补 NUL（Linux 语义）
    return (int64_t)n;
}
// ---- 83）mkdir / 87）unlink：真写盘（走 vfs64）----
// 为什么要临时开中断：ATA 的等待是"IRQ14 中断驱动 + 超时回退 PIO 轮询"，全程关中断会走慢路径。
// 为什么现在开中断是安全的：批次 C 起 syscall 入口用的是**每任务**自己的内核栈
// （见 task64.cpp），中断帧压在同一个栈的更高处，不会覆盖 syscall 帧；再用一个 busy 标志
// 把"VFS 变更"变成单飞行者（另一个进程同时来会拿到 -EBUSY，而不是交错写坏卷）。
static volatile int g_lx_vfs_busy64 = 0;
static int64_t lx64_fs_mutate64(uint64_t nr, uint64_t path_va, int is_mkdir) {
    char path[LX64_PATH_MAX];
    if (lx64_user_str64(path_va, path, LX64_PATH_MAX) != 0) { syscall64_deny64(nr, path_va); return -LX64_EFAULT; }
    if (path[0] != '/') return -LX64_EINVAL;
    if (g_lx_vfs_busy64) return -LX64_EBUSY;
    g_lx_vfs_busy64 = 1;
    __asm__ volatile("sti" ::: "memory");
    const int rc = is_mkdir ? vfs64_mkdir(path) : vfs64_unlink(path);
    __asm__ volatile("cli" ::: "memory");
    g_lx_vfs_busy64 = 0;
    return rc == 0 ? 0 : -LX64_ENOENT;
}

// ---- 24）sched_yield / 35）nanosleep / 34）pause / 37）alarm ----
static int64_t lx64_nanosleep64(uint64_t nr, uint64_t req_va, uint64_t rem_va) {
    (void)rem_va;
    if (!user64_range_ok64(req_va, 16)) { syscall64_deny64(nr, req_va); return -LX64_EFAULT; }
    const uint8_t* p = (const uint8_t*)(uintptr_t)req_va;
    uint64_t sec = 0, nsec = 0;
    for (int i = 0; i < 8; i++) { sec |= (uint64_t)p[i] << (8 * i); nsec |= (uint64_t)p[8 + i] << (8 * i); }
    uint64_t ms = sec * 1000u + nsec / 1000000u;
    if (ms > 60000u) ms = 60000u;                        // 有界：最多 60 秒（防挂死）
    if (ms == 0 && nsec > 0) ms = 1;
    if (ms) {
        if (task_sleep_ms64) task_sleep_ms64((uint32_t)ms);
        else {
            const uint64_t until = g_ticks64 + ms_to_ticks64((uint32_t)ms);
            uint64_t guard = 0;
            while (g_ticks64 < until && ++guard < 2000000000ULL) __asm__ volatile("hlt");
        }
    }
    return 0;
}
static int64_t lx64_pause64() {
    // pause(2) 在 Linux 上是"睡到收到信号"。本内核不投递信号，所以永远等不到 ——
    // 有界等待 1 秒后如实返回 -EINTR（"被信号打断"），绝不死等。
    if (task_sleep_ms64) task_sleep_ms64(1000);
    return -LX64_EINTR;
}
static int64_t lx64_alarm64(uint64_t sec) {
    const int prev = lx64_have_proc64() ? proc64_alarm_set64((int)sec) : 0;
    return prev < 0 ? 0 : (int64_t)prev;                 // 只记录，不投递 SIGALRM
}

// ---- 96）gettimeofday / 97）getrlimit / 160）setrlimit / 318）getrandom ----
static int64_t lx64_gettimeofday64(uint64_t nr, uint64_t tv_va, uint64_t tz_va) {
    if (!user64_range_ok64(tv_va, 16)) { syscall64_deny64(nr, tv_va); return -LX64_EFAULT; }
    const uint64_t t = g_ticks64;
    uint8_t b[16];
    lx64_wr64(b + 0, t / PIT_HZ_64);                                  // tv_sec
    lx64_wr64(b + 8, (t % PIT_HZ_64) * (1000000ULL / PIT_HZ_64));     // tv_usec
    lx64_copy_to_user64(tv_va, b, 16);
    if (tz_va) {
        if (!user64_range_ok64(tz_va, 8)) { syscall64_deny64(nr, tz_va); return -LX64_EFAULT; }
        uint8_t z[8];
        for (uint32_t i = 0; i < 8; i++) z[i] = 0;
        lx64_copy_to_user64(tz_va, z, 8);
    }
    return 0;
}
// rlimit：本内核没有配额（堆/页池是全局的、没有 per-process 记账）。如实给出"无限/常量"，
// 而不是假装有配额。setrlimit 只接受"不大于当前值"的请求，其余 -EPERM（Linux 对硬限制也是 -EPERM）。
static int64_t lx64_getrlimit64(uint64_t nr, uint64_t res, uint64_t rlim_va) {
    if (!user64_range_ok64(rlim_va, 16)) { syscall64_deny64(nr, rlim_va); return -LX64_EFAULT; }
    uint64_t cur = 0xFFFFFFFFFFFFFFFFULL, max = 0xFFFFFFFFFFFFFFFFULL;   // RLIM_INFINITY
    if (res == 3) { cur = max = 8 * 1024 * 1024; }                       // RLIMIT_STACK = 8MiB（常量声明）
    if (res == 7) { cur = max = 8; }                                     // RLIMIT_NOFILE = 8（0..2 + 4 文件 + 2 pipe 预留）
    uint8_t b[16];
    lx64_wr64(b + 0, cur);
    lx64_wr64(b + 8, max);
    lx64_copy_to_user64(rlim_va, b, 16);
    return 0;
}
static int64_t lx64_setrlimit64(uint64_t nr, uint64_t res, uint64_t rlim_va) {
    if (!user64_range_ok64(rlim_va, 16)) { syscall64_deny64(nr, rlim_va); return -LX64_EFAULT; }
    (void)res;
    return -LX64_EPERM;                                  // 没有配额可改：如实拒绝（不死循环、不假装成功）
}
// getrandom：没有硬件 RNG 可用，用 ticks + TSC + 一个 xorshift 自增状态造"够用的伪随机"。
// 如实标注：**不是密码学安全随机**（只需满足"每次不同、分布还行"这一类用户态用法）。
static uint64_t g_lx_rng64 = 0x243F6A8885A308D3ULL;
static uint64_t lx64_rng_next64() {
    uint64_t x = g_lx_rng64;
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    x ^= ((uint64_t)hi << 32) | lo;
    x ^= g_ticks64 * 0x9E3779B97F4A7C15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    g_lx_rng64 = x;
    return x;
}
static int64_t lx64_getrandom64(uint64_t nr, uint64_t buf, uint64_t len, uint64_t flags) {
    (void)flags;
    if (len == 0) return 0;
    if (len > 256) len = 256;                            // 有界（单次最多 256 字节，够 glibc 初始化用）
    if (!user64_range_ok64(buf, len)) { syscall64_deny64(nr, buf); return -LX64_EFAULT; }
    uint8_t tmp[256];
    for (uint64_t i = 0; i < len; i++) {
        if ((i & 7u) == 0) { const uint64_t r = lx64_rng_next64(); for (uint32_t k = 0; k < 8; k++) tmp[i + k < len ? i + k : i] = (uint8_t)(r >> (8 * k)); }
    }
    lx64_copy_to_user64(buf, tmp, len);
    return (int64_t)len;
}

// ---- 32/33）dup / dup2 / 74）fsync：只对"真实文件/pipe fd"（>= 3）有意义，全部走 fd64 ----
// ★ 批次 D 语义修正：dup/dup2 返回的新 fd 与旧的**指向同一个打开文件对象**（共享偏移游标、
//   共享目录游标），引用计数 +1；这正是 Linux 的行为（旧实现是"独立游标复制"，已改对）。
//   目标 fd <= 2（重定向标准流）仍然 -ENOSYS：标准流是 syscall64 自己认的虚拟流，没有对象。
// 33）dup2 与 32）dup 共用下面这一条路径（newfd >= 3 = 指定槽位，newfd < 0 = 自动分配）
static int64_t lx64_dup64(uint64_t fd) {
    const int nf = fd64_dup64((int)fd, -1);              // -1 = 自动分配新槽
    return (int64_t)nf;
}
static int64_t lx64_dup2_64(uint64_t oldfd, uint64_t newfd) {
    if (newfd <= 2) return -LX64_ENOSYS;                 // 重定向 stdin/stdout/stderr 需要真设备层：如实 -ENOSYS
    const int nf = fd64_dup64((int)oldfd, (int)newfd);
    return (int64_t)nf;
}
static int64_t lx64_fsync64(uint64_t fd) {
    if (fd <= 2) return 0;                               // 标准流是虚拟的：无脏页
    return (int64_t)fd64_fsync64((int)fd);               // 写路径即时落盘 -> 无脏页
}
// ★ 批次 C 起不再只是"存进内核变量"：glibc 的 TLS 完全依赖 FS.base 真的生效
//   （__thread / errno / stack canary 都从 FS 取）。写入策略：
//     * 记进进程结构（proc64）—— 进程切换时由 task_apply_ctx64 保存/恢复这个 MSR；
//     * 立刻写 MSR（当前任务正在跑，立刻生效）；非规范地址返回 -EINVAL（wrmsr 会 #GP）。
//   没有进程上下文时（例如任务 0 里的 int 0x80）退回按值记录，仍然返回 0 并如实说明。
static uint64_t g_lx_fs_base64 = 0;
static int64_t lx64_arch_prctl64(uint64_t nr, uint64_t code, uint64_t arg) {
    if (code == 0x1002u) {                                          // ARCH_SET_FS
        if (lx64_have_proc64() && proc64_current_pid64() > 0) return proc64_set_fs_base64(arg);
        g_lx_fs_base64 = arg;
        return 0;
    }
    if (code == 0x1003u) {                                          // ARCH_GET_FS
        if (!user64_range_ok64(arg, 8)) { syscall64_deny64(nr, arg); return -LX64_EFAULT; }
        const uint64_t v = (lx64_have_proc64() && proc64_current_pid64() > 0) ? proc64_get_fs_base64() : g_lx_fs_base64;
        lx64_copy_to_user64(arg, &v, 8);
        return 0;
    }
    return -LX64_EINVAL;                                            // SET_GS/GET_GS 等：本内核没有
}

// ---- 22）pipe / pipe2 ----（批次 D：fd64 的真管道对象；pipe2 的 flags 忽略并如实注明）
// 语义：64 字节环形缓冲 + 读端/写端两个 fd（fd64_pipe64）。**没有阻塞**（没有等待队列/
//   唤醒原语）：写满 -> 短写；完全没有空间 -> -EAGAIN；读空且写端还开着 -> -EAGAIN；
//   写端全关 -> 读返回 0（EOF）。fork 之后父子各自持有一端（fd 表逐槽共享同一个对象）。
// fds_va 必须能放下两个 int32（8 字节）且落在已映射的用户页里。
static int64_t lx64_pipe64(uint64_t nr, uint64_t fds_va) {
    if (!user64_range_ok64(fds_va, 8)) { syscall64_deny64(nr, fds_va); return -LX64_EFAULT; }
    int rf = -1, wf = -1;
    const int rc = fd64_pipe64(&rf, &wf);
    if (rc < 0) return (int64_t)rc;                        // fd64 的负错误码 = -errno
    int32_t out[2];
    out[0] = (int32_t)rf;
    out[1] = (int32_t)wf;
    lx64_copy_to_user64(fds_va, out, 8);
    return 0;
}



// ---- Linux 号段分发 ----
// 返回：>= 0 正常返回；< 0 = -errno。exit/execve 走特例：
//   exit   -> 改写帧把控制权交回内核（入口看 g_syscall64_exit_to_kernel64）
//   execve -> 成功时帧已被改写成"回到新入口"，这里直接返回（调用方立刻返回，让 sysret 落地）
// 每个号的状态见文件头大表（真实现 / 部分 / -ENOSYS），未列出的号一律 -ENOSYS 且只打一次日志。
static int64_t syscall64_linux64(pt_regs64* r) {
    const uint64_t nr = r->rax;
    const uint64_t a1 = r->rdi, a2 = r->rsi, a3 = r->rdx;
    const uint64_t a4 = r->r10, a5 = r->r8, a6 = r->r9;

    switch (nr) {
    case 0:   return lx64_read64(nr, a1, a2, a3);
    case 1:   return lx64_write64(nr, a1, a2, a3);
    case 2:   return lx64_open64(nr, a1, a2);                       // open(path, flags, mode)
    case 3:   return lx64_close64(nr, a1);
    case 4:   return lx64_stat_path64(nr, a1, a2);                 // stat：按路径（最小实现）
    case 5:   return lx64_fstat64(nr, a1, a2);
    case 6:   return lx64_stat_path64(nr, a1, a2);                 // lstat：同上（无符号链接）
    case 8:   return lx64_lseek64(nr, a1, (int64_t)a2, a3);
    case 9:   return lx64_mmap_disp64(a2, a4, a1);                 // arch 无关：len/prot/flags/...
    case 10:  return lx64_mprotect_disp64(a1, a2, a3);
    case 11:  return lx64_munmap_disp64(a1, a2);
    case 12:  return lx64_brk_disp64(a1);
    case 13:  return lx64_rt_sigaction64(a1, a2, a3, a4);          // 只记录不投递
    case 14:  return lx64_rt_sigprocmask64(a1, a2, a3, a4);        // 只记录不投递
    case 15:  break;                                               // rt_sigreturn：没有信号帧可恢复 -> -ENOSYS
    case 16:  return lx64_ioctl64(nr, a1, a2, a3);
    case 20:  return lx64_writev64(nr, a1, a2, a3);
    case 21:  return lx64_access64(nr, a1, a2);
    case 22:  return lx64_pipe64(nr, a1);                          // pipe(int[2])：fd64 真管道（批次 D）
    case 24:  if (task_yield64) task_yield64(); return 0;           // sched_yield：真让出
    case 32:  return lx64_dup64(a1);
    case 33:  return lx64_dup2_64(a1, a2);
    case 34:  return lx64_pause64();                                // 有界等待后 -EINTR（如实）
    case 35:  return lx64_nanosleep64(nr, a1, a2);
    case 37:  return lx64_alarm64(a1);                              // 只记录，不投递 SIGALRM
    case 39:  return lx64_getpid64();
    case 56:  return lx64_clone64(nr, r, a1);                        // 只支持 flags=0/SIGCHLD
    case 57:  return lx64_fork64(nr, r);
    case 58:  return lx64_fork64(nr, r);                             // vfork：等同 fork（如实，见函数注释）
    case 59:  return lx64_execve64(r, a1, a2, a3);
    case 61:  return lx64_wait4_64(a1, a2, a3);
    case 62:  return lx64_kill_wrap64(a1, a2);
    case 63:  return lx64_uname64(nr, a1);
    case 74:  return lx64_fsync64(a1);
    case 79:  return lx64_getcwd64(nr, a1, a2);
    case 82:  break;                                                // rename：vfs64 没有 rename -> -ENOSYS
    case 83:  return lx64_fs_mutate64(nr, a1, 1);                    // mkdir：真写盘
    case 84:  break;                                                // rmdir：vfs64 没有删目录 -> -ENOSYS
    case 87:  return lx64_fs_mutate64(nr, a1, 0);                    // unlink：真写盘
    case 89:  return lx64_readlink64(nr, a1, a2, a3);                // 只对 /proc/self/exe 有值
    case 96:  return lx64_gettimeofday64(nr, a1, a2);
    case 97:  return lx64_getrlimit64(nr, a1, a2);
    case 90:  return lx64_chmod_path64(a1, a2);                     // ★ P4：chmod(path, mode)
    case 91:  return lx64_fchmod64(a1, a2);                         // ★ P4：fchmod(fd, mode)
    case 92:  return lx64_chown_path64(a1, a2, a3);                 // ★ P4：chown(path, owner, group)
    case 93:  return lx64_fchown64(a1, a2, a3);                     // ★ P4：fchown(fd, owner, group)
    case 95:  return lx64_umask64(a1);                              // ★ P4：umask（返回旧值）
    case 102: return lx64_getuid64();                               // ★ P4：真实 uid
    case 104: return lx64_getgid64();                               // ★ P4：真实 gid
    case 105: return lx64_apply_cred64((uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a1, 0xFFFFFFFFu);   // setuid
    case 106: return lx64_apply_cred64(0xFFFFFFFFu, (uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a1);   // setgid
    case 107: return lx64_geteuid64();                              // ★ P4：真实 euid
    case 108: return lx64_getegid64();                              // ★ P4：真实 egid
    case 110: return lx64_getppid64();
    case 113: return lx64_apply_cred64((uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a2, 0xFFFFFFFFu);   // setreuid
    case 114: return lx64_apply_cred64(0xFFFFFFFFu, (uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a2);   // setregid
    case 117: return lx64_apply_cred64(0xFFFFFFFFu, (uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a2);   // setresgid
    case 118: return lx64_apply_cred64((uint32_t)a1, 0xFFFFFFFFu, (uint32_t)a2, 0xFFFFFFFFu);   // setresuid
    case 120: return 0;                                             // getgroups：没有附加组（返回 0 个）
    case 158: return lx64_arch_prctl64(nr, a1, a2);                  // FS.base：真写 MSR
    case 160: return lx64_setrlimit64(nr, a1, a2);
    case 218: return 0;                                             // set_tid_address（没有 clear_child_tid 唤醒）
    case 228: return lx64_clock_gettime64(nr, a1, a2);
    case 257: return lx64_open64(nr, a2, a3);                       // openat(dirfd, path, flags, mode)
    case 318: return lx64_getrandom64(nr, a1, a2, a3);               // 伪随机（非密码学安全，如实标注）
    default:
        break;
    }
    (void)a5; (void)a6;
    syscall64_enosys_once64(nr);
    return -LX64_ENOSYS;
}

// ==================== 分发（两条路径共用一个入口）====================
extern "C" void syscall64_dispatch64(pt_regs64* r) {
    // syscall 指令路径：int_no = SYSCALL64_INSM_FRAME_MARK64（见 syscall_entry64.asm）
    if (r->int_no == SYSCALL64_INSM_FRAME_MARK64) {
        const uint64_t nr = r->rax;
        const uint64_t a1 = r->rdi, a2 = r->rsi, a3 = r->rdx;
        int64_t ret;
        if (nr == 60 || nr == 231) {                       // exit / exit_group
            syscall64_log_insn64(nr, a1, a2, a3, 0);
            if (user64_exit_to_kernel64(r, a1)) {
                g_syscall64_exit_to_kernel64 = 1;          // 入口看到它就 jmp ring0 蹦床（别 sysret）
                return;
            }
            ret = 0;                                       // 不在 ring3：当普通调用返回 0
        } else {
            ret = syscall64_linux64(r);
            syscall64_log_insn64(nr, a1, a2, a3, ret);
        }
        // ---- 出口前自检：sysret 只能回"用户窗口内的地址 + CS=0x2B/SS=0x23" ----
        // 帧一旦被谁改坏（本模块踩过：syscall 帧与中断帧共用栈顶导致 rip/cs 槽被覆盖），
        // 直接 sysret 会在**用户的 CS/RSP 上下文里**抛 #GP，非常难查。这里改成：发现异常就
        // 不 sysret，改走内核蹦床（user64_resume_tramp64）回 ring0，并留下证据。
        if ((r->rip < USER64_CODE_VA64 || r->rip >= USER64_CODE_VA64 + USER64_WINDOW_BYTES64) ||
            r->cs != SEL64_UCODE || r->ss != SEL64_UDATA) {
            dbg64_line_begin64();
            dbg64_str("[SYSCALL] insn frame bad rip=");
            dbg64_hex64(r->rip);
            dbg64_str(" cs=");
            dbg64_hex64(r->cs);
            dbg64_str(" ss=");
            dbg64_hex64(r->ss);
            dbg64_str(" -> kernel trampoline\n");
            dbg64_line_end64();
            r->rax = (uint64_t)ret;
            if (user64_exit_to_kernel64(r, 0)) { g_syscall64_exit_to_kernel64 = 1; return; }
            return;                                        // 实在回不去：保持原样（不该发生）
        }
        // ★ 每次正常返回前都必须**重新**清掉出口开关：它是全局量，而本任务的系统调用
        //   可能在 wait4/nanosleep 里被挂起（让别的任务跑），别的任务那会儿走 exit 会把这个
        //   全局置 1；等本任务回来时若不再清一次，入口汇编就会误判"本帧已 exit"并 jmp 到
        //   ring0 蹦床（实测症状：父进程 wait4 返回后整机停住 —— 蹦床按 ctx 复位栈、ret 进
        //   已释放的内核栈）。
        g_syscall64_exit_to_kernel64 = 0;
        r->rax = (uint64_t)ret;
        return;
    }

    // int 0x80 路径（Vimtu64 自有 ABI）：行为与引入 syscall 指令之前完全一致
    const uint64_t nr = r->rax;
    const uint64_t a1 = r->rdi, a2 = r->rsi, a3 = r->rdx;
    int64_t ret = -1;

    switch (nr) {
    case 1:                                                     // write(fd, buf, len)
        ret = syscall64_write64(nr, a1, a2, a3);
        syscall64_log64(nr, a1, a2, a3, ret);
        break;

    case 2:                                                     // exit(code)
        syscall64_log64(nr, a1, a2, a3, 0);
        if (user64_exit_to_kernel64(r, a1)) return;             // 帧已改写：不回用户态
        ret = -1;                                               // 不在 ring3：当普通调用处理
        break;

    case 3:                                                     // getpid()
        ret = task_current_id_64 ? (int64_t)task_current_id_64() : 0;
        break;

    case 4:                                                     // ticks()
        ret = (int64_t)g_ticks64;
        break;

    case 5:                                                     // sleep_ms(ms)
        ret = syscall64_sleep64((uint32_t)a1);
        break;

    case 6: {                                                   // open(path)：本轮接真 FD 层（只读）
        char path[LX64_PATH_MAX];
        if (lx64_user_str64(a1, path, LX64_PATH_MAX) != 0) { ret = -1; break; }
        const int f = fd64_open64(path, 0);
        ret = (f < 0) ? -1 : (int64_t)f;
        break;
    }

    case 7: {                                                   // read(fd, buf, len)
        if (a1 < 3 || a3 == 0) { ret = (a1 < 3) ? -1 : 0; break; }
        uint64_t len = a3;
        if (len > LX64_READ_MAX) len = LX64_READ_MAX;
        if (!user64_range_ok64(a2, len)) { ret = -1; break; }
        const int n = fd64_read64((int)a1, g_lx_readbuf64, (int)len);
        if (n < 0) { ret = -1; break; }
        if (n > 0) lx64_copy_to_user64(a2, g_lx_readbuf64, (uint64_t)n);
        ret = n;
        break;
    }

    case 8:                                                     // close(fd)
        ret = (fd64_close64((int)a1) == 0) ? 0 : -1;
        break;

    default:
        ret = -1;
        break;
    }

    r->rax = (uint64_t)ret;
}

// ==================== MSR：打开 syscall/sysret ====================
static inline uint64_t sc64_rdmsr(uint32_t msr) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void sc64_wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

#define MSR64_EFER   0xC0000080u
#define MSR64_STAR   0xC0000081u
#define MSR64_LSTAR  0xC0000082u
#define MSR64_FMASK  0xC0000084u

// FMASK：入口自动清掉 RFLAGS 的这些位。0x700 = TF(0x100) | IF(0x200) | DF(0x400)。
//   IF -> 入口先关中断（入口自己决定何时开；本实现全程关，sysret 时用 r11 恢复用户 IF）
//   DF -> 保证入口汇编里的字符串/内存操作方向是"向上"（本入口只有 push/pop，防御性）
static const uint32_t SC64_FMASK64 = 0x700u;
// STAR[63:48] = 0x1B -> sysret 的 CS = 0x2B、SS = 0x23；[47:32] = 0x08 -> syscall 的 CS = 0x08、SS = 0x10
static const uint64_t SC64_STAR64  = ((uint64_t)0x1B << 48) | ((uint64_t)0x08 << 32);

// 打开 EFER.SCE + 写 STAR/LSTAR/FMASK。幂等，可重复调用（自检里也调一次，保证测的是配置后的现场）。
static void syscall64_init_msr64() {
    uint64_t efer = sc64_rdmsr(MSR64_EFER);
    if (!(efer & 1u)) {
        sc64_wrmsr(MSR64_EFER, efer | 1u);                      // EFER.SCE = bit0
    }
    sc64_wrmsr(MSR64_STAR,  SC64_STAR64);
    sc64_wrmsr(MSR64_LSTAR, (uint64_t)(uintptr_t)syscall64_insn_entry64);
    sc64_wrmsr(MSR64_FMASK, SC64_FMASK64);

    // 留证（自动验收 grep；回读 MSR 确认真的写进去了）
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] msr init EFER.SCE=");
    dbg64_dec(sc64_rdmsr(MSR64_EFER) & 1u);
    dbg64_str(" star=");
    dbg64_hex64(sc64_rdmsr(MSR64_STAR));
    dbg64_str(" lstar=");
    dbg64_hex64(sc64_rdmsr(MSR64_LSTAR));
    dbg64_str(" fmask=");
    dbg64_hex64(sc64_rdmsr(MSR64_FMASK));
    dbg64_nl();
    dbg64_line_end64();
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] syscall insn entry ready (rustar: STAR=0x1B<<48|0x08<<32 -> CS=0x2B SS=0x23)\n");
    dbg64_line_end64();
}

// ==================== 自检 ====================
// 位含义：bit0 ABI/用户窗口常量自洽、bit1 GDT 现场（选择子/段类型）、bit2 范围校验负例、
//         bit3 syscall 指令路径的 MSR 现场（EFER.SCE / STAR / LSTAR / FMASK / 帧标记）
int syscall64_selftest64() {
    int fail = 0;

    // ---- bit0：ABI 常量 ----
    if (USER64_CODE_VA64 < 0x100000000ULL) fail |= 1;                       // 用户窗口必须在 4GiB 以上
    if (USER64_STACK_VA64 < USER64_CODE_VA64) fail |= 1;
    if (USER64_STACK_VA64 + USER64_STACK_BYTES64 > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) fail |= 1;
    if (SEL64_UCODE != 0x2B || SEL64_UDATA != 0x23) fail |= 1;
    // Linux 路径的固定区必须都落在窗口内、且互不重叠（改常量时这里会先报）
    if (USER64_BRK_VA64 <= USER64_STACK_VA64 + USER64_STACK_BYTES64) fail |= 1;
    if (USER64_BRK_VA64 + USER64_BRK_BYTES64 > USER64_MMAP_VA64) fail |= 1;
    if (USER64_MMAP_VA64 < USER64_BRK_VA64 + USER64_BRK_BYTES64) fail |= 1;
    if (USER64_MMAP_VA64 + USER64_MMAP_MIN_BYTES64 > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) fail |= 1;

    // ---- bit1：GDT 现场（access 字节 = 原始描述符 bits 40..47；gran = bits 48..55）----
    // ★ 屏蔽 CPU 会自己置的位：段被加载时置"访问位 A"（type bit0），ltr 把 TSS 类型
    //   从 0x9（可用）置成 0xB（忙）。屏蔽后才是描述符的静态编码。
    if (((gdt_entry_raw64(1) >> 40) & 0xFE) != 0x9A) fail |= 2;             // 内核代码（未变）
    if (((gdt_entry_raw64(2) >> 40) & 0xFE) != 0x92) fail |= 2;             // 内核数据（未变）
    if (((gdt_entry_raw64(4) >> 40) & 0xFE) != 0xF2) fail |= 2;             // 用户数据 DPL=3
    if (((gdt_entry_raw64(5) >> 40) & 0xFE) != 0xFA) fail |= 2;             // 用户代码 DPL=3
    if (((gdt_entry_raw64(5) >> 48) & 0x20) != 0x20) fail |= 2;             // 用户代码 L=1
    if (((gdt_entry_raw64(6) >> 40) & 0xFD) != 0x89) fail |= 2;             // TSS 在 index 6

    // ---- bit2：范围校验必须拒绝内核地址/窗口外/溢出（安全闸门的最关键用例）----
    if (user64_range_ok64(0x100000, 16)) fail |= 4;                                 // 内核镜像
    if (user64_range_ok64(0xFFFFFFFF80100000ULL, 16)) fail |= 4;                    // 内核高半区
    if (user64_range_ok64(USER64_CODE_VA64 - 1, 2)) fail |= 4;                      // 窗口下界前
    if (user64_range_ok64(USER64_CODE_VA64 + USER64_WINDOW_BYTES64, 8)) fail |= 4;  // 窗口上界外
    if (user64_range_ok64(USER64_CODE_VA64 + USER64_WINDOW_BYTES64 - 1, 4096)) fail |= 4;  // 跨上界
    if (user64_range_ok64(USER64_CODE_VA64, 0)) fail |= 4;                          // len=0
    if (user64_range_ok64(0xFFFFFFFFFFFFFFFFULL, 4096)) fail |= 4;                  // 溢出

    // ---- bit3：syscall 指令路径的 MSR 现场（配一次再看，保证测的是配置后的值）----
    syscall64_init_msr64();
    {
        if (!(sc64_rdmsr(MSR64_EFER) & 1u)) fail |= 8;                              // EFER.SCE
        if (sc64_rdmsr(MSR64_STAR) != SC64_STAR64) fail |= 8;                       // STAR 全 64 位比对
        if (sc64_rdmsr(MSR64_LSTAR) != (uint64_t)(uintptr_t)syscall64_insn_entry64) fail |= 8;
        if (sc64_rdmsr(MSR64_FMASK) != (uint64_t)SC64_FMASK64) fail |= 8;
        if ((SC64_STAR64 >> 48) != 0x1Bu) fail |= 8;                                // sysret CS/SS 的来源
        if (SEL64_UCODE != (uint16_t)(((SC64_STAR64 >> 48) + 16) & 0xFFFF)) fail |= 8;
        if (SEL64_UDATA != (uint16_t)(((SC64_STAR64 >> 48) + 8) & 0xFFFF)) fail |= 8;
        if (SYSCALL64_INSM_FRAME_MARK64 == 0x80ULL) fail |= 8;                      // 必须与 int 0x80 区分
        if (g_syscall64_kstack64 == 0) fail |= 8;                                   // 入口换栈用的栈顶
    }

    return fail;
}

// ==================== 初始化 ====================
void syscall64_init64() {
    // 1) int 0x80 门：idt_build() 里已经装成 0xEE（DPL=3 中断门）；这里再显式装一次并留证，
    //    让"ring3 能 int 0x80"这件事不依赖任何构建顺序。
    idt_set_gate64(128, (uint64_t)(uintptr_t)isr128_64, SEL64_KCODE, 0xEE, 0);
    dbg64_line_begin64();
    dbg64_str("[SYSCALL] int 0x80 gate installed (dpl=3, abi: rax=nr rdi/rsi/rdx=args ret=rax)\n");
    dbg64_line_end64();

    // 2) syscall 指令路径：EFER.SCE + STAR/LSTAR/FMASK（Linux 程序唯一会用的入口）
    syscall64_init_msr64();

    // 3) 自检（含 MSR 现场）
    const int st = syscall64_selftest64();
    if (st == 0) {
        dbg64_line_begin64();
        dbg64_str("[SYSCALL] selftest PASS\n");
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[SYSCALL] selftest FAIL mask=");
        dbg64_dec((uint64_t)st);
        dbg64_nl();
        dbg64_line_end64();
    }
}
