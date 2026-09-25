// proc64.cpp - Vimtu64 进程与地址空间实现（每进程 CR3 + fork/execve/wait4/kill）
//
// 设计、页表共享策略、取舍的完整说明在 kernel/proc64.h 顶部（先读那份）。
// 本文件只额外记三件容易踩的事：
//
// 1) 写"别人的页表"一律走**物理地址**（恒等映射，见 proc64.h 第 2 条）：
//    分配页池页时拿到的指针就是它的物理地址，而 PDPT[0..3] / PML4[511] 在所有进程里共享同一批
//    页表 —— 所以 ring0 用物理地址读写任意进程的页表页永远是安全的，不需要切 CR3。
//    反过来，**改自己的**用户窗口（mmap/brk/elf 装载）必须走用户 VA 助手
//    （user64_map_page64 等），因为它们按当前 CR3 走表并且会重载 CR3 投递 TLB。
//
// 2) 切换 CR3 的时机（只有三处）：
//    * 调度器切任务：task64.cpp 的 task_apply_ctx64（mm_cr3 / mm_fs_base 由本文件算好交进去）；
//    * 本文件显式切：proc64_switch_to64 / proc64_switch_to_kernel64
//      （装载子进程映像、释放地址空间前必须切回内核地址空间 —— 否则会 free 掉正在用的 PML4）；
//    * execve 不切：换映像就是在**当前**进程自己的地址空间里做。
//
// 3) fork 的"整页物理复制"走到哪一步为止（如实边界）：
//    只复制**已映射的 4KB 用户页** + 重建 PT/PD 结构；2MB 大页（用户窗口里不该出现）直接失败；
//    页数上限 PROC64_FORK_MAX_PAGES，超了 -ENOMEM（512MB 机器上页池约 384MB，够 256 页×16 进程，
//    但还是立上限，避免某个大映像把页池吃穿）。
#include "proc64.h"
#include "task64.h"
#include "usermode64.h"     // 用户窗口常量 / 页级助手 / user64_enter_frame64 / 范围校验
#include "elf64.h"          // execve 复用装载逻辑（elf64_load_for_exec64 / elf64_forget64）
#include "vfs64.h"          // /proc64.elf 的安装与读取
#include "syscall64.h"      // SYSCALL64_INSM_FRAME_MARK64（execve 改帧时保持入口标记）
#include "fd64.h"           // 批次 D：每进程 fd 表（fdtab）+ 引用计数对象（fork/execve/退出都要用）
#include "memlayout64.h"    // ML64_PML4_PHYS（引导期页表自证用）
#include "mem_64.h"         // page_alloc_64 / page_free_64 / PTE_*
#include "debug64.h"

// 内嵌的 /pipe64.elf（批次 D：ring3 pipe 演示 —— fork 后父子各持一端通信）
extern "C" const uint8_t _binary_build64_pipe64_elf_start[];
extern "C" const uint8_t _binary_build64_pipe64_elf_end[];
static const char PROC64_PIPE_PATH64[] = "/pipe64.elf";
// 内嵌的 /proc64.elf（build64.sh：nasm -f elf64 -> ld.lld -static -> objcopy -I binary 嵌进系统内核）。
extern "C" const uint8_t _binary_build64_proc64_elf_start[];
extern "C" const uint8_t _binary_build64_proc64_elf_end[];
static const char PROC64_PATH64[] = "/proc64.elf";

// ---- errno（与 syscall64.cpp 的 Linux 号段同一套数值）----
static const int64_t P64_EPERM   = 1;
static const int64_t P64_ENOENT  = 2;
static const int64_t P64_ESRCH   = 3;
static const int64_t P64_EINTR   = 4;
static const int64_t P64_ECHILD  = 10;
static const int64_t P64_EAGAIN  = 11;
static const int64_t P64_ENOMEM  = 12;
static const int64_t P64_EFAULT  = 14;
static const int64_t P64_EINVAL  = 22;
static const int64_t P64_ENOSYS  = 38;

// 这三个 errno 目前没有调用点，但它们是**对外承诺的错误码表**的一部分（与 syscall64 同口径）；
// static_assert 钉住取值，同时消掉 -Wextra 的"未被引用"告警（本文件要求零告警）。
static_assert(P64_EPERM == 1 && P64_EINTR == 4 && P64_EFAULT == 14, "P64_* 错误码表（= -errno）");

// ---- 规模常量 ----
static const uint32_t PROC64_FORK_MAX_PAGES    = 256;   // fork 整页复制的页数上限（≈1MiB）
static const int      PROC64_WAIT_TIMEOUT_SEC  = 5;     // wait4 有界等待（防挂死）
static const int      PROC64_DEMO_TIMEOUT_SEC  = 12;    // 启动期演示的有界等待
static const int      PROC64_PIPE_TIMEOUT_SEC  = 12;    // pipe 演示的有界等待（同一口径）
static const int      PROC64_SIG_MAX           = 32;    // 记录型信号表（只记录不投递）

// ==================== 进程表 ====================
struct Proc64 {
    uint32_t state;
    int32_t  pid;
    int32_t  ppid;
    uint32_t task_id;
    uint32_t exit_code;
    uint32_t term_sig;          // 被哪个信号终止（0 = 正常退出）
    uint32_t sigchld;           // 有子可收（子进程退出时置位；只记录不投递）
    uint32_t sig_pending;       // 记录型：收到过哪些信号（位图，最多 32 个）
    uint32_t alarm_sec;         // alarm() 记录的秒数（无投递）
    uint64_t fs_base;           // IA32_FS_BASE（TLS；glibc 的 arch_prctl(ARCH_SET_FS)）
    uint64_t cr3;               // 该进程的页表根（物理地址）——切任务时装载
    uint64_t pml4_phys;         // 我们自己分配的 PML4 页（释放用）
    uint64_t pdpt_phys;         // 我们自己分配的 PDPT 页（用户窗口 PDPT[4] 的父表）
    uint64_t brk_start;         // brk 区（每进程独立）
    uint64_t brk_end;
    uint64_t brk_limit;
    uint64_t mmap_next;         // mmap 的 bump 分配器（每进程独立）
    uint64_t frame;             // 用户现场（pt_regs64，仅 fork 出的子进程/启动映像用）
    uint64_t entry;             // 最近一次装载的入口（readlink /proc/self/exe 相关日志用）
    uint64_t sighand[PROC64_SIG_MAX];   // rt_sigaction 记录（只记录不投递）
    FdTable64* fdtab;                   // ★ 批次 D：每进程 fd 表（fd64.h；池由 fd64.cpp 管）
    // ★ P4：每进程凭证（uid/gid/euid/egid；root = 0）。fork/execve 继承，setuid/setgid/seteuid 改它；
    //   任务被调度时由 task64_cred_hook64 -> vfs64_set_proc_cred64 发布给 VFS（切回任务 0 恢复会话身份）。
    uint32_t uid, gid, euid, egid;
    uint32_t sigmask_lo, sigmask_hi;    // rt_sigprocmask 记录
    char     name[PROC64_NAME_MAX];
    char     exe[PROC64_PATH_MAX];
};
static Proc64   g_procs[PROC64_MAX];
static int      g_proc_count   = 0;
static int32_t  g_next_pid64   = 1;


// fd64 通过弱符号问"当前进程的 fd 表"（没有进程上下文 -> nullptr -> 内核表兜底）。
// 定义放在 p64_current64 之后（它在下面"进程查找"一节里）。
static Proc64* p64_current64();
extern "C" FdTable64* proc64_fdtab_of_current64();
// 引导期状态（proc64_init64 探测一次）
static uint64_t g_boot_cr364   = 0;      // 内核地址空间（引导期页表根）
static void p64_publish_cred64(Proc64* p);           // ★ P4：凭证发布（定义在 create 之后）
static int      g_isolate64    = 0;      // 1 = 每进程 CR3 生效；0 = 共享地址空间模式
static bool     g_init_done64  = false;
static bool     g_shared_warn64 = false; // "共享模式"的说明只打一次

// 小工具
static void p64_zero(void* p, uint32_t n) { uint8_t* d = (uint8_t*)p; for (uint32_t i = 0; i < n; i++) d[i] = 0; }
static void p64_memcpy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst; const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static void p64_strcpy_n(char* dst, const char* src, int max) {
    int i = 0;
    if (max <= 0) return;
    for (; src && src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
// 取页框物理地址。★ 必须按**物理地址位宽**掩（bit12..51），不能用 ~0xFFFULL：
//   用户页的 PTE 里 NX 在 bit63（栈/mmap/brk 页都带 NX），~0xFFFULL 会把它留下，
//   于是"物理地址"变成非规范地址 —— 后面拿它当指针用就是 #GP err=0（本模块踩过一次：
//   fork 里 memcpy 源页正好是带 NX 的栈页）。
static inline uint64_t p64_page64(uint64_t v) { return v & 0x000FFFFFFFFFF000ULL; }
static inline uint64_t* p64_phys64(uint64_t pa) { return (uint64_t*)(uintptr_t)pa; }   // 恒等映射：物理可直接解引用
static inline uint64_t p64_rd_cr364() { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t p64_align_up64(uint64_t v) { return (v + 0xFFFULL) & ~0xFFFULL; }
static inline bool p64_canonical64(uint64_t v) { return ((v >> 47) == 0) || ((v >> 47) == 0x1FFFFULL); }

// 日志助手（行锁 + 统一前缀）
static void p64_log2(const char* a, const char* b) {
    dbg64_line_begin64(); dbg64_str(a); dbg64_str(b ? b : "?"); dbg64_nl(); dbg64_line_end64();
}
static void p64_log31(const char* a, uint64_t n, const char* b) {
    dbg64_line_begin64(); dbg64_str(a); dbg64_dec(n); dbg64_str(b); dbg64_nl(); dbg64_line_end64();
}

// ==================== 进程查找 ====================
static Proc64* p64_slot64(int idx) {
    if (idx < 0 || idx >= PROC64_MAX) return nullptr;
    if (g_procs[idx].state == PROC64_FREE) return nullptr;
    return &g_procs[idx];
}
static Proc64* p64_find64(int pid) {
    if (pid <= 0) return nullptr;
    for (int i = 0; i < PROC64_MAX; i++) {
        if (g_procs[i].state != PROC64_FREE && g_procs[i].pid == pid) return &g_procs[i];
    }
    return nullptr;
}
static Proc64* p64_current64() {
    void* p = task_proc_of_current64();          // 任务的 proc 指针（task64 只存不解析）
    if (!p) return nullptr;
    Proc64* q = (Proc64*)p;
    if (q < &g_procs[0] || q >= &g_procs[PROC64_MAX]) return nullptr;   // 防御：不是我们的表
    if (q->state == PROC64_FREE) return nullptr;
    return q;
}

// fd64 的弱引用目标（fd64.cpp 在安装介质内核里也要编，所以那边是 weak 声明 + 判空）
extern "C" FdTable64* proc64_fdtab_of_current64() {
    Proc64* p = p64_current64();
    return p ? p->fdtab : nullptr;
}
static int p64_free_slot64() {
    for (int i = 0; i < PROC64_MAX; i++) if (g_procs[i].state == PROC64_FREE) return i;
    return -1;
}

// ==================== 地址空间 ====================
// 建一份新地址空间：新 PML4 + 新 PDPT；内核高半区与"前 4GB 恒等映射"共享引导期的页表。
// 用户窗口（PML4[0]/PDPT[4]）私有：PDPT 里那一条留 0，用的时候再分配 PD/PT。
static int p64_build_as64(Proc64* p) {
    void* pml4p = page_alloc_64();
    if (!pml4p) return -1;
    void* pdptp = page_alloc_64();
    if (!pdptp) { page_free_64(pml4p); return -1; }
    p64_zero(pml4p, PAGE_SIZE_64);
    p64_zero(pdptp, PAGE_SIZE_64);

    uint64_t* pml4 = p64_phys64((uint64_t)(uintptr_t)pml4p);
    uint64_t* pdpt = p64_phys64((uint64_t)(uintptr_t)pdptp);
    const uint64_t* boot = p64_phys64(p64_page64(g_boot_cr364));

    // 1) 顶层整表复制（512 项）：内核高半区（PML4[511]）**共享同一批页表**，
    //    前 4GB 恒等映射的那条链（PML4[0] -> PDPT[0..3] 的 PD）也照抄 —— 内核堆/页池/LFB
    //    都是按物理地址直接访问的，这条链必须原样成立。
    for (int i = 0; i < 512; i++) pml4[i] = boot[i];

    // 2) 但 PML4[0] 指向**我们自己**的 PDPT：把引导期那张 PDPT 整表复制过来，
    //    然后**只把 PDPT[4] 清 0** —— VA 4GiB..5GiB（用户窗口）就是这一条，
    //    于是用户窗口对每个进程私有，其余（含固件给我们留下的映射）完全一致。
    const uint64_t e0 = boot[0];
    if (!(e0 & PTE_PRESENT_64)) { page_free_64(pdptp); page_free_64(pml4p); return -1; }
    if (e0 & 0x80ULL) { page_free_64(pdptp); page_free_64(pml4p); return -1; }   // 1GB 大页：不可能
    const uint64_t* boot_pdpt = p64_phys64(p64_page64(e0));
    for (int i = 0; i < 512; i++) pdpt[i] = boot_pdpt[i];
    pdpt[4] = 0;                                        // ★ 用户窗口（4GiB..5GiB）私有
    pml4[0] = (uint64_t)(uintptr_t)pdptp | (e0 & 0xFFFULL);

    p->pml4_phys = (uint64_t)(uintptr_t)pml4p;
    p->pdpt_phys = (uint64_t)(uintptr_t)pdptp;
    p->cr3       = p->pml4_phys;
    return 0;
}

void proc64_switch_to_kernel64() {                      // 内部/给 task64 兜底用
    task64_load_cr364(task64_kernel_cr364());
    task64_load_fs_base64(0);
}
void proc64_switch_to64(int pid) {
    Proc64* p = p64_find64(pid);
    if (!p) return;
    if (p->cr3) task64_load_cr364(p->cr3);
    task64_load_fs_base64(p->fs_base);
    p64_publish_cred64(p);                           // ★ P4：显式切地址空间时也把凭证带上
}
// "当前 CPU 的 CR3 是不是就是这个进程的"：决定释放地址空间前要不要切回内核地址空间。
// ★ 绝不能无条件切：fork 失败回滚时当前任务是**父进程**（它自己的地址空间正在用），
//   若此时切到内核地址空间，父进程回 ring3 时用户窗口没映射 -> 取指 #PF。
static inline bool p64_cr3_is64(const Proc64* p) {
    return p->cr3 != 0 && p64_page64(p64_rd_cr364()) == p64_page64(p->cr3);
}
// 释放用户区：PDPT[4] 子树里的叶子物理页 + PT/PD 页表页，最后清 PDPT[4]。
// 返回释放的页数。**不动** PDPT[0..3]（共享的恒等映射）。
static uint32_t p64_release_area64(Proc64* p) {
    if (!p->pdpt_phys) return 0;
    uint32_t n = 0;
    uint64_t* pdpt = p64_phys64(p->pdpt_phys);
    const uint64_t e3 = pdpt[4];
    if (!(e3 & PTE_PRESENT_64)) return 0;
    if (e3 & 0x80ULL) { pdpt[4] = 0; return 0; }                     // 1GB 大页（防御）
    uint64_t* pd = p64_phys64(p64_page64(e3));
    for (int i2 = 0; i2 < 512; i2++) {
        const uint64_t e2 = pd[i2];
        if (!(e2 & PTE_PRESENT_64)) continue;
        if (e2 & 0x80ULL) { pd[i2] = 0; continue; }                  // 2MB 大页（防御）
        uint64_t* pt = p64_phys64(p64_page64(e2));
        for (int i1 = 0; i1 < 512; i1++) {
            const uint64_t e1 = pt[i1];
            if (!(e1 & PTE_PRESENT_64)) continue;
            page_free_64((void*)(uintptr_t)p64_page64(e1));
            pt[i1] = 0;
            n++;
        }
        page_free_64(pt);
        n++;
        pd[i2] = 0;
    }
    page_free_64(pd);
    n++;
    pdpt[4] = 0;
    return n;
}
// 统计用户区里的叶子页数（任务管理器/快照用）
static uint32_t p64_count_pages64(const Proc64* p) {
    if (!p->pdpt_phys) return 0;
    const uint64_t* pdpt = p64_phys64(p->pdpt_phys);
    const uint64_t e3 = pdpt[4];
    if (!(e3 & PTE_PRESENT_64) || (e3 & 0x80ULL)) return 0;
    const uint64_t* pd = p64_phys64(p64_page64(e3));
    uint32_t n = 0;
    for (int i2 = 0; i2 < 512; i2++) {
        const uint64_t e2 = pd[i2];
        if (!(e2 & PTE_PRESENT_64) || (e2 & 0x80ULL)) continue;
        const uint64_t* pt = p64_phys64(p64_page64(e2));
        for (int i1 = 0; i1 < 512; i1++) if (pt[i1] & PTE_PRESENT_64) n++;
    }
    return n;
}

// ==================== 初始化 / 模式探测 ====================
void proc64_init64() {
    if (g_init_done64) return;
    g_init_done64 = true;
    p64_zero(g_procs, (uint32_t)sizeof(g_procs));
    g_boot_cr364 = p64_rd_cr364();

    // ★ 判定"引导期页表是不是我们自己的"（决定能不能安全地 mov cr3）：
    //   BIOS 路径（boot/loader64.asm 的 lm64_build_paging）把 CR3 指到物理 0x40000 并建成
    //   linker 文档里那套结构：PML4[0] -> 0x41000（4GB 恒等映射）、PML4[511] -> 0x46000
    //   （高半区直映）。这两个条件同时成立 => 页表完全由我们掌控，切 CR3 一定安全。
    //   UEFI 路径（boot/efi/uefi64.c）**不换 CR3**，只把高半区映射挂进固件当前活动的 PML4：
    //   那个页表属于固件（只读、格式由固件决定），而且 VMware EFI 下运行期 mov cr3 已知会
    //   立刻复位 —— 所以判定失败时进"共享地址空间模式"，并在串口与文档里如实标注。
    int own = 0;
    uint64_t why = 0;
    if (p64_page64(g_boot_cr364) == (uint64_t)ML64_PML4_PHYS) {
        const uint64_t* pml4 = p64_phys64(p64_page64(g_boot_cr364));
        const uint64_t e0 = pml4[0], ehi = pml4[511];
        if ((e0 & PTE_PRESENT_64) && p64_page64(e0) == 0x41000ULL &&
            (ehi & PTE_PRESENT_64) && p64_page64(ehi) == 0x46000ULL) {
            // 再核对一张 PD 是否真的是 2MB 大页（P|RW|PS），以及高半区 PDPT[510] 存在
            const uint64_t* pdpt = p64_phys64(0x41000ULL);
            const uint64_t* pd = nullptr;
            if ((pdpt[0] & PTE_PRESENT_64) && !(pdpt[0] & 0x80ULL)) pd = p64_phys64(p64_page64(pdpt[0]));
            const uint64_t* pdpt_hi = p64_phys64(0x46000ULL);
            if (pd && (pd[0] & (PTE_PRESENT_64 | PTE_WRITE_64 | 0x80ULL)) == (PTE_PRESENT_64 | PTE_WRITE_64 | 0x80ULL) &&
                (pdpt_hi[510] & PTE_PRESENT_64)) {
                own = 1;
            } else {
                why = 2;
            }
        } else {
            why = 1;
        }
    } else {
        why = 3;
    }
#if defined(PROC64_UEFI_CR3_EXPERIMENT) && (PROC64_UEFI_CR3_EXPERIMENT == 1)
    // ★ 实验构建：**不在这里**强行打开隔离（批次 C 的那个占位行为已废弃 —— 它会让"固件页表 +
    //   每进程 CR3"直接上电，复位/失败都看不出是哪一步）。改成：先如实保持 shared，由
    //   proc64_uefi_cr3_experiment_start64() 在**桌面起来之后**按 A -> B 顺序实测，
    //   实测通过（mov cr3 存活 + 真进 ring3）才把 g_isolate64 打开。见本文件末尾的实验段。
    if (!own) {
        dbg64_line_begin64();
        dbg64_str("[PROC64] uefi cr3 experiment armed (mode stays shared until A/B are verified; check=");
        dbg64_dec(why);
        dbg64_str(")\n");
        dbg64_line_end64();
    }
#endif
    g_isolate64 = own;

    dbg64_line_begin64();
    dbg64_str("[PROC64] init mode=");
    dbg64_str(own ? "isolated" : "shared");
    dbg64_str(" cr3=");
    dbg64_hex64(g_boot_cr364);
    dbg64_str(" proc_max=");
    dbg64_dec((uint64_t)PROC64_MAX);
    dbg64_str(" fork_max_pages=");
    dbg64_dec((uint64_t)PROC64_FORK_MAX_PAGES);
    dbg64_nl();
    dbg64_line_end64();

    if (!own) {
        dbg64_line_begin64();
        dbg64_str("[PROC64] cr3 isolation OFF (boot paging is firmware-owned, check=");
        dbg64_dec(why);
        dbg64_str("): shared address space mode -> fork/vfork/clone/execve/wait4 = -ENOSYS");
        dbg64_nl();
        dbg64_line_end64();
    }
}

int proc64_isolate64() { return g_isolate64; }

int proc64_current_ppid64() {
    Proc64* p = p64_current64();
    return (p && p->ppid > 0) ? (int)p->ppid : 0;
}
int proc64_exe_path64(char* out, uint32_t cap) {
    Proc64* p = p64_current64();
    if (!p || !out || cap == 0) return 0;
    uint32_t i = 0;
    for (; i + 1u < cap && p->exe[i]; i++) out[i] = p->exe[i];
    out[i] = 0;
    return 1;
}
static void p64_shared_note64() {
    if (g_shared_warn64) return;
    g_shared_warn64 = true;
    p64_log2("[PROC64] ", "denied (shared address space mode: no per-process cr3)");
}

int proc64_current_pid64() {
    Proc64* p = p64_current64();
    return p ? (int)p->pid : -1;
}
int proc64_find64(int pid) { return p64_find64(pid) ? 1 : 0; }
int proc64_count64() { return g_proc_count; }
const char* proc64_current_name64() {
    Proc64* p = p64_current64();
    return p ? p->name : "none";
}
int proc64_info64(int idx, Proc64Info* out) {
    if (!out || idx < 0 || idx >= PROC64_MAX) return 0;
    Proc64* p = p64_slot64(idx);
    if (!p) { p64_zero(out, (uint32_t)sizeof(Proc64Info)); return 0; }
    Proc64* cur = p64_current64();
    out->pid        = (uint32_t)p->pid;
    out->ppid       = (uint32_t)(p->ppid < 0 ? 0 : p->ppid);
    out->state      = p->state;
    out->task_id    = p->task_id;
    out->is_current = (cur == p) ? 1u : 0u;
    out->pages      = p64_count_pages64(p);
    out->cr3        = p->cr3;
    out->exit_code  = (int32_t)p->exit_code;
    out->term_sig   = p->term_sig;
    for (int i = 0; i < PROC64_NAME_MAX; i++) out->name[i] = p->name[i];
    out->name[PROC64_NAME_MAX - 1] = 0;
    return 1;
}
uint64_t proc64_user_pages64(int pid) {
    Proc64* p = p64_find64(pid);
    return p ? (uint64_t)p64_count_pages64(p) : 0;
}

// ==================== 创建 / 退出 / 释放 ====================
int proc64_create64(const char* name, int ppid) {
    if (!g_isolate64) { p64_shared_note64(); return -1; }
    const int slot = p64_free_slot64();
    if (slot < 0) { p64_log2("[PROC64] create FAILED reason=no-slot name=", name); return -1; }
    Proc64* p = &g_procs[slot];
    p64_zero(p, (uint32_t)sizeof(Proc64));
    // ★ 批次 D：先拿一张 fd 表（池满 = 建不了进程；16 进程 + 内核表 < FD64_TABLE_MAX=20，正常不会）
    p->fdtab = fd64_table_alloc64();
    if (!p->fdtab) {
        p64_zero(p, (uint32_t)sizeof(Proc64));
        p64_log2("[PROC64] create FAILED reason=fd-table name=", name);
        return -1;
    }
    if (p64_build_as64(p) != 0) {
        fd64_table_close_all64(p->fdtab);
        fd64_table_free64(p->fdtab);
        p64_zero(p, (uint32_t)sizeof(Proc64));
        p64_log2("[PROC64] create FAILED reason=pagetable name=", name);
        return -1;
    }
    p->pid     = g_next_pid64++;
    if (g_next_pid64 <= 0) g_next_pid64 = 1;
    p->ppid    = ppid;
    p->state   = PROC64_READY;
    p->cr3     = p->pml4_phys;
    p->fs_base = 0;
    p64_strcpy_n(p->name, name ? name : "proc", PROC64_NAME_MAX);
    p64_strcpy_n(p->exe, "?", PROC64_PATH_MAX);
    {   // ★ P4：新进程**继承当前凭证**（内核启动期 = root；fork = 父进程；终端 run = 终端会话身份）
        Vfs64Cred64 cr;
        vfs64_get_cred64(&cr);
        p->uid = cr.uid; p->gid = cr.gid; p->euid = cr.euid; p->egid = cr.egid;
    }
    g_proc_count++;

    dbg64_line_begin64();
    dbg64_str("[PROC64] create pid=");
    dbg64_dec((uint64_t)p->pid);
    dbg64_str(" name=");
    dbg64_str(p->name);
    dbg64_str(" cr3=");
    dbg64_hex64(p->cr3);
    dbg64_str(" ppid=");
    dbg64_dec((uint64_t)(p->ppid < 0 ? 0 : p->ppid));
    dbg64_str(" uid=");
    dbg64_dec(p->euid);
    dbg64_nl();
    dbg64_line_end64();
    return (int)p->pid;
}

// ==================== ★ P4：每进程凭证（uid/gid/euid/egid）+ 发布给 VFS ====================
// 发布点有两个：
//   * 任务被调度时（task64 的 task_apply_ctx64 -> task64_cred_hook64，见下）；
//   * 显式切地址空间（proc64_switch_to64）。
// have = 0（proc == nullptr，任务 0/内核线程）-> vfs64 恢复**会话身份**（userdb64 设的那份）。
static void p64_publish_cred64(Proc64* p) {
    if (p) vfs64_set_proc_cred64(1, p->uid, p->gid, p->euid, p->egid);
    else   vfs64_set_proc_cred64(0, 0, 0, 0, 0);
}
// task64.cpp 的弱引用目标：每次任务切换都会调用（安装介质内核不链本文件 -> 该符号缺失，弱引用为 0）
extern "C" void task64_cred_hook64(void* proc) {
    Proc64* p = (Proc64*)proc;
    if (p && (p < &g_procs[0] || p >= &g_procs[PROC64_MAX])) p = nullptr;   // 防御：不是我们的表
    if (p && p->state == PROC64_FREE) p = nullptr;
    p64_publish_cred64(p);
}
// 系统调用侧用：取当前进程凭证（返回 -1 = 没有进程上下文，调用方退化为"会话身份"）
int proc64_get_cred64(uint32_t* uid, uint32_t* gid, uint32_t* euid, uint32_t* egid) {
    Proc64* p = p64_current64();
    if (!p) return -1;
    if (uid) *uid = p->uid;
    if (gid) *gid = p->gid;
    if (euid) *euid = p->euid;
    if (egid) *egid = p->egid;
    return 0;
}
// 系统调用侧用：改当前进程凭证（setuid/setgid/seteuid/setegid 的落点）；
//   改完**立刻发布**给 VFS（本进程之后即使不切换任务也按新身份判定）。
int proc64_set_cred64(uint32_t uid, uint32_t gid, uint32_t euid, uint32_t egid) {
    Proc64* p = p64_current64();
    if (!p) return -1;
    p->uid = uid; p->gid = gid; p->euid = euid; p->egid = egid;
    p64_publish_cred64(p);
    return 0;
}

// 释放进程槽（地址空间在 p64_exit64 里已经还了）。调用方保证它的任务已经不会再跑。
void proc64_destroy64(int pid) {
    Proc64* p = p64_find64(pid);
    if (!p) return;
    if (p->state != PROC64_EXITED) {                    // 还活着：先把它的任务打死（不该发生）
        if (p->task_id) task_kill64(p->task_id);
    }
    if (p64_cr3_is64(p)) proc64_switch_to_kernel64();   // ★ 只在"正在用它的地址空间"时才切走
    if (p->fdtab) {                                     // ★ 批次 D：关掉这张表里的所有 fd（引用 -1）
        fd64_table_close_all64(p->fdtab);
        fd64_table_free64(p->fdtab);
        p->fdtab = nullptr;
    }
    if (p->pdpt_phys || p->pml4_phys) {
        p64_release_area64(p);
        if (p->pdpt_phys) page_free_64((void*)(uintptr_t)p->pdpt_phys);
        if (p->pml4_phys) page_free_64((void*)(uintptr_t)p->pml4_phys);
    }
    const int32_t keep_pid = p->pid;
    p64_zero(p, (uint32_t)sizeof(Proc64));
    p->state = PROC64_FREE;
    if (g_proc_count > 0) g_proc_count--;
    dbg64_line_begin64();
    dbg64_str("[PROC64] reap pid=");
    dbg64_dec((uint64_t)keep_pid);
    dbg64_nl();
    dbg64_line_end64();
}

// 退出：记退出码/信号 -> **释放地址空间与 CR3** -> 置 EXITED（僵尸，等父进程 wait4 收）。
// 为什么地址空间立刻还：僵尸只需要"退出码 + 状态"，用户页/页表页没有保留价值（Linux 会保留
// 一部分用于 core dump/调试，本内核没有那些设施）。
static void p64_exit64(Proc64* p, uint32_t code, uint32_t sig) {
    if (!p || p->state == PROC64_EXITED) return;
    const uint32_t pages = p64_count_pages64(p);
    // ★ 只有"退出的是**当前任务自己**的进程"时才能动 CR3/TCB：
    //   kill 路径是在**别人**（父进程）的上下文里替目标收尾的，那时当前任务的 CR3 属于父进程、
    //   绝不能切走 —— 否则父进程一回到 ring3 就取指 #PF（实测症状：kill 之后父进程在
    //   0x10000040B 上 instruction-fetch fault）。顺带：PML4 页马上要还给页池，所以**自己**退出
    //   时还必须把 TCB 里的 mm_cr3 清 0，免得被抢占再切回来时去装载一块已释放的页表。
    const bool self = p64_cr3_is64(p);
    if (self) task_set_current_mm64(0, 0);
    p64_release_area64(p);
    if (p->pdpt_phys) { page_free_64((void*)(uintptr_t)p->pdpt_phys); p->pdpt_phys = 0; }
    if (p->pml4_phys) { page_free_64((void*)(uintptr_t)p->pml4_phys); p->pml4_phys = 0; }
    // ★ 批次 D：fd 表随进程退出一起收（close_all = 每个 fd 引用计数 -1；fork 出来的兄弟进程
    //   若还共享着同一个 OpenFile64/pipe，对象会活下来 —— 这就是 POSIX 的语义）
    if (p->fdtab) {
        fd64_table_close_all64(p->fdtab);
        fd64_table_free64(p->fdtab);
        p->fdtab = nullptr;
    }
    p->cr3       = 0;
    p->exit_code = code & 0xFFu;
    p->term_sig  = sig;
    p->state     = PROC64_EXITED;
    p->frame     = 0;
    p->mmap_next = 0;
    p->brk_start = p->brk_end = p->brk_limit = 0;
    // 父进程的"有子可收"（SIGCHLD 语义：只置标志、不投递）
    Proc64* par = (p->ppid > 0) ? p64_find64(p->ppid) : nullptr;
    if (par) par->sigchld = 1;

    dbg64_line_begin64();
    dbg64_str("[PROC64] exit pid=");
    dbg64_dec((uint64_t)p->pid);
    dbg64_str(" code=");
    dbg64_dec((uint64_t)p->exit_code);
    dbg64_str(" cr3_released=1 pages_freed=");
    dbg64_dec((uint64_t)pages);
    if (p->term_sig) { dbg64_str(" sig="); dbg64_dec((uint64_t)p->term_sig); }
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 任务入口：在进程自己的地址空间里跑 ring3 ====================
extern "C" void proc64_task_entry64(void* arg) {
    Proc64* p = (Proc64*)arg;
    if (!p) { task_exit64(); }
    const int pid = p->pid;
    p->state = PROC64_RUNNING;
    // 显式切地址空间：不依赖"调度器已经切过"（任务第一次跑起来时调度器当然切过，
    // 但这里再切一次是幂等的、也让"这个任务属于这个进程"这件事自证——见 proc64.h 第 3 条）。
    if (p->cr3) { task64_load_cr364(p->cr3); task64_load_fs_base64(p->fs_base); }
    // 诊断（启动期演示只有一次，留一行证据：帧/入口/栈/CR3 到底对不对）
    dbg64_line_begin64();
    dbg64_str("[PROC64] task pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" frame=0x");
    dbg64_hex64(p->frame);
    dbg64_str(" cr3=0x");
    dbg64_hex64(p64_rd_cr364());
    if (p->frame) {
        const pt_regs64* f = (const pt_regs64*)(uintptr_t)p->frame;
        dbg64_str(" rip=0x");
        dbg64_hex64(f->rip);
        dbg64_str(" rsp=0x");
        dbg64_hex64(f->rsp);
        dbg64_str(" cs=0x");
        dbg64_hex64(f->cs);
        dbg64_str(" ss=0x");
        dbg64_hex64(f->ss);
    }
    dbg64_nl();
    dbg64_line_end64();
    const uint64_t rc = user64_enter_frame64((const pt_regs64*)(uintptr_t)p->frame, p->name);
    p64_exit64(p, (uint32_t)(rc & 0xFFULL), 0);
    task_exit64();                                      // 不返回（任务侧回收交给 task64）
}

// ==================== 装载 ELF 并启动（启动期演示 / execve 都复用）====================
int proc64_start_elf64(int pid, const char* path) {
    Proc64* p = p64_find64(pid);
    if (!p) return -1;
    if (!g_isolate64) { p64_shared_note64(); return -1; }
    if (!path || path[0] != '/') return -1;

    // 1) 切到该进程的地址空间：装载必须在它自己的 CR3 下做（页级助手按当前 CR3 走表）
    if (p->cr3) task_set_current_mm64(p->cr3, 0);        // ★ 装载期间**对抢占安全**地保持在该进程地址空间
    task64_load_fs_base64(p->fs_base);
    elf64_forget64();

    uint64_t entry = 0, rsp = 0;
    const char* argv[2] = { path, nullptr };
    if (elf64_load_for_exec64(path, argv, 1, &entry, &rsp) != 0) {
        task_set_current_mm64(0, 0);
        proc64_switch_to_kernel64();
        p64_log2("[PROC64] start FAILED reason=load path=", path);
        return -1;
    }
    dbg64_line_begin64();
    dbg64_str("[PROC64] start loaded entry=0x");
    dbg64_hex64(entry);
    dbg64_str(" rsp=0x");
    dbg64_hex64(rsp);
    dbg64_str(" pages=");
    dbg64_dec((uint64_t)p64_count_pages64(p));
    dbg64_nl();
    dbg64_line_end64();
    task_set_current_mm64(0, 0);                         // 恢复内核地址空间（后面回到任务 0 的内核流程）
    proc64_switch_to_kernel64();
    // 2) 用户帧（ring3 入口）：与 usermode64 的构造口径一致（cs=0x2B / ss=0x23 / IF=1）
    //    ★ 顺序有讲究：任务一旦建好（state=READY）就可能被调度器立刻跑起来 ——
    //      它在 proc64_task_entry64 里第一件事就是读 p->frame，所以帧必须**先装好再挂牌**。
    //      （这里踩过：先 task_create64 再写 frame，任务在下一个 PIT tick 跑起来时 frame 还是 0，
    //       进 ring3 直接失败、进程以 255 退出 —— 症状就是"init 秒退"。）
    static pt_regs64 s_frames[PROC64_MAX];
    const int idx = (int)(p - &g_procs[0]);
    p64_zero(&s_frames[idx], PR64_SIZE);
    s_frames[idx].gs = SEL64_UDATA; s_frames[idx].fs = SEL64_UDATA;
    s_frames[idx].es = SEL64_UDATA; s_frames[idx].ds = SEL64_UDATA;
    s_frames[idx].rip    = entry;
    s_frames[idx].cs     = SEL64_UCODE;
    s_frames[idx].rflags = 0x202;
    s_frames[idx].rsp    = rsp;
    s_frames[idx].ss     = SEL64_UDATA;
    p->frame = (uint64_t)(uintptr_t)&s_frames[idx];
    p->entry = entry;
    p64_strcpy_n(p->exe, path, PROC64_PATH_MAX);

    // 3) 任务：入口 = proc64_task_entry64（它会进 ring3 并等退出）
    const int tid = task_create64(p->name, proc64_task_entry64, p);
    if (tid < 0) {
        p64_log2("[PROC64] start FAILED reason=task path=", path);
        return -1;
    }
    p->task_id = (uint32_t)tid;
    task_bind_proc64((uint32_t)tid, p, p->cr3, p->fs_base);
    return 0;
}

// ==================== fork ====================
// 整页物理复制（**不做 COW**，取舍见 proc64.h 第 5 条）。
// 复制顺序：先按父的 PD/PT 结构在子里建出同样的结构，叶子逐页"分配新页 + memcpy 4KB"，
// 权限位（含 NX）原样照抄。返回 0 = 成功（*out_pages = 复制的页数）。
static int p64_copy_user_area64(const Proc64* src, Proc64* dst, uint32_t* out_pages) {
    uint32_t n = 0;
    if (out_pages) *out_pages = 0;
    const uint64_t* spdpt = p64_phys64(src->pdpt_phys);
    const uint64_t e3 = spdpt[4];
    if (!(e3 & PTE_PRESENT_64)) return 0;                       // 父进程没有用户页：空拷贝
    if (e3 & 0x80ULL) return -1;
    const uint64_t* spd = p64_phys64(p64_page64(e3));
    uint64_t* dpdpt = p64_phys64(dst->pdpt_phys);
    uint64_t* dpd = nullptr;

    for (int i2 = 0; i2 < 512; i2++) {
        const uint64_t e2 = spd[i2];
        if (!(e2 & PTE_PRESENT_64)) continue;
        if (e2 & 0x80ULL) return -1;                            // 2MB 大页：不支持（不该出现）
        const uint64_t* spt = p64_phys64(p64_page64(e2));
        uint64_t* dpt = nullptr;
        for (int i1 = 0; i1 < 512; i1++) {
            const uint64_t e1 = spt[i1];
            if (!(e1 & PTE_PRESENT_64)) continue;
            if (n >= PROC64_FORK_MAX_PAGES) return -1;          // 超上限：调用方回滚
            void* np = page_alloc_64();
            if (!np) return -1;
            p64_memcpy(np, (const void*)(uintptr_t)p64_page64(e1), PAGE_SIZE_64);
            if (!dpd) {                                         // 需要时才分配里的 PD 页
                void* npd = page_alloc_64();
                if (!npd) { page_free_64(np); return -1; }
                p64_zero(npd, PAGE_SIZE_64);
                dpd = p64_phys64((uint64_t)(uintptr_t)npd);
                dpdpt[4] = (uint64_t)(uintptr_t)npd | (e3 & 0xFFFULL);
            }
            if (!dpt) {
                void* npt = page_alloc_64();
                if (!npt) { page_free_64(np); return -1; }
                p64_zero(npt, PAGE_SIZE_64);
                dpt = p64_phys64((uint64_t)(uintptr_t)npt);
                dpd[i2] = (uint64_t)(uintptr_t)npt | (e2 & 0xFFFULL);
            }
            dpt[i1] = ((uint64_t)(uintptr_t)np) | (e1 & (0xFFFULL | PTE_NX_64));
            n++;
        }
    }
    if (out_pages) *out_pages = n;
    return 0;
}

int64_t proc64_fork64(pt_regs64* r) {
    if (!g_isolate64) { p64_shared_note64(); return -P64_ENOSYS; }
    Proc64* par = p64_current64();
    if (!par) return -P64_ENOSYS;                       // 内核线程不能 fork
    if (!r || r->cs != SEL64_UCODE || r->ss != SEL64_UDATA) return -P64_EINVAL;
    if (par->state == PROC64_EXITED) return -P64_ESRCH;

    const int cpid = proc64_create64(par->name, (int)par->pid);
    if (cpid < 0) return -P64_ENOMEM;
    Proc64* c = p64_find64(cpid);
    if (!c) return -P64_ENOMEM;

    // ★ 批次 D：继承父进程整张 fd 表（逐槽共享同一个 OpenFile64 -> 父子共享偏移/pipe 端）。
    //   放在复制用户区之前：失败了才轮到 p64_exit64 去回收（它会 close_all 这张表）。
    const uint32_t fds = fd64_table_used64(par->fdtab);
    if (fd64_table_clone64(c->fdtab, par->fdtab) != 0) {
        p64_exit64(c, 1, 0);
        proc64_destroy64(cpid);
        p64_log2("[PROC64] fork FAILED reason=fd-table parent-name=", par->name);
        return -P64_ENOMEM;
    }

    uint32_t pages = 0;
    if (p64_copy_user_area64(par, c, &pages) != 0) {
        p64_exit64(c, 1, 0);                            // 复用退出路径把半成品清干净
        proc64_destroy64(cpid);
        p64_log2("[PROC64] fork FAILED reason=copy-or-limit parent-name=", par->name);
        return -P64_ENOMEM;
    }

    // 内存记账继承：brk 区间与 mmap 游标（地址空间是复制来的，所以两者必须一致）
    c->brk_start = par->brk_start;
    c->brk_end   = par->brk_end;
    c->brk_limit = par->brk_limit;
    c->mmap_next = par->mmap_next;
    c->fs_base   = par->fs_base;                        // TLS 基址继承（切换时装载）

    // ★ 子进程的用户现场 = 父进程这一次系统调用的帧，只把 rax 改成 0。
    //   为什么这样最稳：帧里已经有"下一条用户指令（rip）/用户 rsp/rflags/全部 GPR"，
    //   而 task_switch_iret64 弹的就是同一份 0xD0 布局 —— 于是"父子从同一点继续、返回值不同"
    //   这条 fork 语义是**帧级的直接对应**，不需要任何手工拼装（这也是最容易出错的一步）。
    static pt_regs64 s_cframes[PROC64_MAX];
    const int cidx = (int)(c - &g_procs[0]);
    s_cframes[cidx] = *r;
    s_cframes[cidx].rax = 0;                            // 子进程 fork 返回 0
    s_cframes[cidx].int_no = 0;
    s_cframes[cidx].err_code = 0;
    s_cframes[cidx].gs = SEL64_UDATA; s_cframes[cidx].fs = SEL64_UDATA;
    s_cframes[cidx].es = SEL64_UDATA; s_cframes[cidx].ds = SEL64_UDATA;
    c->frame = (uint64_t)(uintptr_t)&s_cframes[cidx];
    c->entry = s_cframes[cidx].rip;

    const int tid = task_create64(c->name, proc64_task_entry64, c);
    if (tid < 0) {
        p64_exit64(c, 1, 0);
        proc64_destroy64(cpid);
        p64_log2("[PROC64] fork FAILED reason=task parent-name=", par->name);
        return -P64_ENOMEM;
    }
    c->task_id = (uint32_t)tid;
    task_bind_proc64((uint32_t)tid, c, c->cr3, c->fs_base);

    dbg64_line_begin64();
    dbg64_str("[PROC64] fork parent=");
    dbg64_dec((uint64_t)par->pid);
    dbg64_str(" child=");
    dbg64_dec((uint64_t)c->pid);
    dbg64_str(" cr3=");
    dbg64_hex64(c->cr3);
    dbg64_str(" pages=");
    dbg64_dec((uint64_t)pages);
    dbg64_str(" fds=");
    dbg64_dec((uint64_t)fds);
    dbg64_str(" fds_shared=1");
    dbg64_nl();
    dbg64_line_end64();
    return (int64_t)c->pid;
}

// ==================== execve ====================
int64_t proc64_execve64(pt_regs64* r, const char* path, const char* const* argv, uint32_t argc) {
    if (!g_isolate64) { p64_shared_note64(); return -P64_ENOSYS; }
    Proc64* p = p64_current64();
    if (!p) return -P64_ENOSYS;
    if (!r || r->cs != SEL64_UCODE || r->ss != SEL64_UDATA) return -P64_EINVAL;
    if (!path || path[0] != '/') return -P64_EINVAL;

    // 1) 释放旧映像（整个用户区 + 页表页）。注意：释放之后如果装载失败，这个进程就没有映像了 ——
    //    与 Linux 不同（Linux 会保留旧映像继续跑），这里会把它按"退出码 127（command not found）"
    //    终止，并在日志里如实写明。取舍理由：保留旧映像需要"先装到临时地址空间再原子切换"，
    //    那要再引入一套影子地址空间/两份记账，收益只是"execve 失败还能继续跑"这一种边角场景。
    const uint32_t old_pages = p64_count_pages64(p);
    p64_release_area64(p);
    elf64_forget64();
    // ★ 批次 D：execve **默认保留**所有 fd（Linux 语义；本内核没有实现 O_CLOEXEC，如实注明）
    const uint32_t fds_kept = fd64_table_used64(p->fdtab);

    uint64_t entry = 0, rsp = 0;
    if (elf64_load_for_exec64(path, argv, argc, &entry, &rsp) != 0) {
        dbg64_line_begin64();
        dbg64_str("[PROC64] execve FAILED path=");
        dbg64_str(path);
        dbg64_str(" pid=");
        dbg64_dec((uint64_t)p->pid);
        dbg64_str(" reason=load -> process terminated (code 127)");
        dbg64_nl();
        dbg64_line_end64();
        p64_exit64(p, 127, 0);
        return -P64_ENOENT;
    }

    // 2) 内存记账重置（新映像：brk 从零开始、mmap 游标回起点）
    p->brk_start = p->brk_end = p->brk_limit = 0;
    p->mmap_next = 0;
    p->entry     = entry;
    p64_strcpy_n(p->exe, path, PROC64_PATH_MAX);

    // 3) 改这一帧：syscall 出口直接回到用户态的**新入口**。
    //    syscall 指令路径的出口汇编取 0xA8(rip)->rcx、0xB8(rflags)->r11、0xC0(rsp)->rsp，
    //    所以这三个槽都要写；GPR 清 0（execve 后旧程序的寄存器状态没有意义，Linux 同样清）。
    r->rip    = entry;
    r->rsp    = rsp;
    r->rflags = 0x202;                                  // IF=1（回到 ring3 后能被 PIT 抢占）
    r->rax    = 0;
    r->rdi = 0; r->rsi = 0; r->rdx = 0; r->rcx = 0; r->rbx = 0; r->rbp = 0;
    r->r8 = 0; r->r9 = 0; r->r10 = 0; r->r11 = 0; r->r12 = 0; r->r13 = 0; r->r14 = 0; r->r15 = 0;
    r->int_no = SYSCALL64_INSM_FRAME_MARK64;            // 保持本帧来自哪条入口（打点/分流用）

    dbg64_line_begin64();
    dbg64_str("[PROC64] execve path=");
    dbg64_str(path);
    dbg64_str(" pid=");
    dbg64_dec((uint64_t)p->pid);
    dbg64_str(" entry=");
    dbg64_hex64(entry);
    dbg64_str(" rsp=");
    dbg64_hex64(rsp);
    dbg64_str(" argc=");
    dbg64_dec((uint64_t)argc);
    dbg64_str(" old_pages=");
    dbg64_dec((uint64_t)old_pages);
    dbg64_str(" fds_kept=");
    dbg64_dec((uint64_t)fds_kept);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== wait4 ====================
// 为什么用"task_sleep64 轮询"而不是等待队列：
//   本内核的调度器没有"阻塞/唤醒"原语（只有 SLEEP = 睡到某个 tick），也没有等待队列对象；
//   要加一套"进程挂到子进程的 wait queue 上、子退出时唤醒"需要改调度器与回收路径
//   （那条路径刚修过一个回收竞态，见 task64.cpp），而父进程等待子进程在演示场景里只有
//   几十毫秒 —— 用 2ms 粒度轮询 + 有界超时（PROC64_WAIT_TIMEOUT_SEC）在代价与正确性上更划算。
//   轮询期间父进程在睡眠，CPU 交给子进程，语义上就是"阻塞等待"。
int64_t proc64_wait4(int pid, int* status_out, uint32_t options) {
    Proc64* par = p64_current64();
    const int ppid = par ? (int)par->pid : -1;
    int target = -1;
    if (pid > 0) {
        Proc64* c = p64_find64(pid);
        if (!c) return -P64_ECHILD;
        target = pid;
    } else {
        // -1 / 0：任一子进程（0 在 Linux 里是"同进程组"，本内核没有进程组 -> 等同 -1）
        for (int i = 0; i < PROC64_MAX; i++) {
            Proc64* c = &g_procs[i];
            if (c->state == PROC64_FREE) continue;
            if (ppid >= 0 && c->ppid == ppid) { target = (int)c->pid; break; }
        }
        if (target < 0) return -P64_ECHILD;
    }
    Proc64* c = p64_find64(target);
    if (!c) return -P64_ECHILD;
    if ((int)c->ppid != ppid && par) return -P64_ECHILD;    // 只允许等自己的子进程

    if (c->state != PROC64_EXITED) {
        if (options & 1u) return 0;                        // WNOHANG
        const uint64_t t0 = g_ticks64;
        while (c->state != PROC64_EXITED) {
            if (g_ticks64 - t0 > (uint64_t)PIT_HZ_64 * (uint64_t)PROC64_WAIT_TIMEOUT_SEC) {
                p64_log31("[PROC64] wait4 TIMEOUT pid=", (uint64_t)target, " (child not exited)");
                return -P64_EAGAIN;
            }
            if (par) par->state = PROC64_SLEEP;
            task_sleep_ms64(2);
            if (par) par->state = PROC64_RUNNING;
        }
    }

    const uint32_t code = c->exit_code;
    // Linux 编码：status = (code & 0xFF) << 8。★ 如实标注差异：被信号终止的进程在 Linux 里是
    // "低 7 位放信号号"（SIGTERM -> 15），本内核**按时说明的方案**改为"目标以退出码 143 结束"，
    // 所以这里给出的是 (143 & 0xFF) << 8；信号号本身在 [PROC64] kill/exit 行里如实打出。
    const int status = (int)((code & 0xFFu) << 8);
    if (status_out) *status_out = status;
    const int rpid = (int)c->pid;
    dbg64_line_begin64();
    dbg64_str("[PROC64] wait4 pid=");
    dbg64_dec((uint64_t)rpid);
    dbg64_str(" status=");
    dbg64_dec((uint64_t)status);
    dbg64_str(" raw_code=");
    dbg64_dec((uint64_t)code);
    if (c->term_sig) { dbg64_str(" by_sig="); dbg64_dec((uint64_t)c->term_sig); }
    dbg64_nl();
    dbg64_line_end64();

    proc64_destroy64(rpid);                                // 收尸（Linux: wait4 之后子进程记录消失）
    return (int64_t)rpid;
}

// ==================== kill ====================
int64_t proc64_kill64(int pid, int sig) {
    if (!g_isolate64) { p64_shared_note64(); return -P64_ENOSYS; }
    if (pid <= 0) return -P64_EINVAL;
    if (sig <= 0 || sig >= 64) return -P64_EINVAL;
    Proc64* t = p64_find64(pid);
    if (!t) return -P64_ESRCH;
    Proc64* cur = p64_current64();
    if (cur && cur->pid == pid) {
        p64_log31("[PROC64] kill self not supported pid=", (uint64_t)pid, " (no signal delivery)");
        return -P64_EINVAL;
    }

    if (sig == 9 || sig == 15) {
        // 立即终止路径（SIGKILL=9 / SIGTERM=15）：把目标任务打死（task_kill64 -> DEAD + 待回收），
        // 然后释放它的地址空间并置 EXITED。为什么能立刻做：单核 + 我们正在跑，目标任务此刻
        // 一定不在运行；它的非全局 TLB 项在切 CR3 时已经被刷掉，所以还页是安全的。
        const uint32_t code = (sig == 15) ? 143u : 137u;
        dbg64_line_begin64();
        dbg64_str("[PROC64] kill pid=");
        dbg64_dec((uint64_t)pid);
        dbg64_str(" sig=");
        dbg64_dec((uint64_t)sig);
        dbg64_str(" task_id=");
        dbg64_dec((uint64_t)t->task_id);
        dbg64_nl();
        dbg64_line_end64();
        if (t->task_id) (void)task_kill64(t->task_id);
        p64_exit64(t, code, (uint32_t)sig);
        return 0;
    }
    // 其它信号：**只记录不投递**（本内核没有信号投递路径：没有用户栈上的信号帧、没有
    // rt_sigreturn、没有 vDSO/restorer）。这里如实记录到进程的位图里，并打一行说明。
    if (sig < PROC64_SIG_MAX) t->sig_pending |= (1u << sig);
    dbg64_line_begin64();
    dbg64_str("[PROC64] kill pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" sig=");
    dbg64_dec((uint64_t)sig);
    dbg64_str(" recorded=1 delivered=0");
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 每进程内存管理 ====================
uint64_t proc64_brk64(uint64_t addr) {
    Proc64* p = p64_current64();
    if (!p) return 0;
    if (p->brk_start == 0) {
        // 首次：把整块 64KiB 可写区映射出来（与既有的全局实现同口径，便于回归对照）
        for (uint64_t a = USER64_BRK_VA64; a < USER64_BRK_VA64 + USER64_BRK_BYTES64; a += PAGE_SIZE_64) {
            uint64_t phys = 0;
            if (!user64_map_page64(a, PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64, 1, &phys)) return 0;
        }
        user64_paging_sync64();
        p->brk_start = USER64_BRK_VA64;
        p->brk_end   = USER64_BRK_VA64;
        p->brk_limit = USER64_BRK_VA64 + USER64_BRK_BYTES64;
    }
    if (addr == 0) return p->brk_end;
    if (addr < p->brk_start || addr > p->brk_limit) return p->brk_end;   // 失败：返回旧 brk（Linux 同）
    p->brk_end = addr;
    return addr;
}

int64_t proc64_mmap64(uint64_t len, uint64_t flags, uint64_t addr) {
    Proc64* p = p64_current64();
    if (!p) return -P64_ENOSYS;
    if (len == 0) return -P64_EINVAL;
    if (len > USER64_WINDOW_BYTES64) return -P64_ENOMEM;
    const uint64_t n = p64_align_up64(len);
    uint64_t va;
    if (flags & 0x10u) va = p64_page64(addr);                            // MAP_FIXED
    else va = p64_align_up64(p->mmap_next ? p->mmap_next : USER64_MMAP_VA64);
    if (va < USER64_MMAP_VA64) return -P64_ENOMEM;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - va) return -P64_ENOMEM;

    uint32_t made = 0;
    for (uint64_t a = va; a < va + n; a += PAGE_SIZE_64) {
        uint64_t phys = 0;
        if (!user64_map_page64(a, PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64, 1, &phys)) {
            // 回滚已经映射的页（每进程的映射表要能"可回收"，不能泄漏）
            for (uint64_t b = va; b < a; b += PAGE_SIZE_64) {
                const uint64_t ph = user64_unmap_page64(b);
                if (ph) page_free_64((void*)(uintptr_t)ph);
            }
            user64_paging_sync64();
            (void)made;
            return -P64_ENOMEM;
        }
        made++;
    }
    user64_paging_sync64();
    if (va + n > p->mmap_next) p->mmap_next = va + n;
    return (int64_t)va;
}

int64_t proc64_munmap64(uint64_t addr, uint64_t len) {
    if (len == 0) return 0;
    const uint64_t n = p64_align_up64(len);
    if (addr < USER64_CODE_VA64 || addr > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return -P64_EINVAL;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - addr) return -P64_EINVAL;
    for (uint64_t a = addr; a < addr + n; a += PAGE_SIZE_64) {
        const uint64_t phys = user64_unmap_page64(a);
        if (!phys) return -P64_EINVAL;                                   // 未映射：Linux 也返回 -EINVAL
        page_free_64((void*)(uintptr_t)phys);
    }
    user64_paging_sync64();
    return 0;
}

int64_t proc64_mprotect64(uint64_t addr, uint64_t len, uint64_t prot) {
    if (len == 0) return 0;
    const uint64_t n = p64_align_up64(len);
    if (addr < USER64_CODE_VA64 || addr > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return -P64_EINVAL;
    if (n > (USER64_CODE_VA64 + USER64_WINDOW_BYTES64) - addr) return -P64_EINVAL;
    const uint64_t f = PTE_USER_64
                     | ((prot & 2u) ? PTE_WRITE_64 : 0u)
                     | ((prot & 4u) ? 0u : PTE_NX_64);
    for (uint64_t a = addr; a < addr + n; a += PAGE_SIZE_64) {
        if (!user64_remap_flags64(a, f)) return -P64_EINVAL;
    }
    user64_paging_sync64();
    return 0;
}

// ==================== FS 基址（TLS）====================
int64_t proc64_set_fs_base64(uint64_t v) {
    Proc64* p = p64_current64();
    if (!p) return -P64_EINVAL;
    if (v != 0 && !p64_canonical64(v)) return -P64_EINVAL;              // 非规范地址：wrmsr 会 #GP
    p->fs_base = v;
    task_update_mm64(task_current_id_64(), 0, v);                       // 记账更新（CR3 不变）
    task64_load_fs_base64(v);                                           // ★ 立刻写 MSR（本任务正在跑）
    return 0;
}
uint64_t proc64_get_fs_base64() {
    Proc64* p = p64_current64();
    return p ? p->fs_base : 0;
}

// ==================== 信号（只记录不投递）====================
int proc64_record_sigaction64(int sig, uint64_t handler) {
    Proc64* p = p64_current64();
    if (!p) return -P64_EINVAL;
    if (sig <= 0 || sig >= PROC64_SIG_MAX) return -P64_EINVAL;
    if (sig == 9 || sig == 19) return -P64_EINVAL;                      // SIGKILL/SIGSTOP 不可捕获
    p->sighand[sig] = handler;
    return 0;
}
int proc64_record_sigmask64(uint64_t mask) {
    Proc64* p = p64_current64();
    if (!p) return -P64_EINVAL;
    p->sigmask_lo = (uint32_t)mask;
    p->sigmask_hi = (uint32_t)(mask >> 32);
    return 0;
}
int proc64_sig_pending64(int sig) {
    Proc64* p = p64_current64();
    if (!p || sig <= 0 || sig >= PROC64_SIG_MAX) return 0;
    return (p->sig_pending >> sig) & 1u;
}
int proc64_alarm_set64(int sec) {
    Proc64* p = p64_current64();
    if (!p) return -P64_EINVAL;
    const int prev = (int)p->alarm_sec;
    p->alarm_sec = (sec > 0) ? (uint32_t)sec : 0u;
    return prev;
}

// ==================== 内嵌 /proc64.elf 的幂等安装 ====================
int proc64_install_builtin64(int drive, uint32_t part_lba) {
    const uint32_t bytes = (uint32_t)(_binary_build64_proc64_elf_end - _binary_build64_proc64_elf_start);
    if (bytes < 64 || !elf64_blob_ok64(_binary_build64_proc64_elf_start, bytes)) {
        p64_log2("[PROC64] install FAILED reason=blob path=/proc64.elf", "");
        return -1;
    }
    // ★ 多卷（为什么不会写错卷）：/proc64.elf、/pipe64.elf 都是**系统卷**上的文件 —— 挂载用
    //   vfs64_mount_system64（登记系统卷槽，不改别的槽），探/写一律 *_on64(系统卷槽, …)：
    //   用户浏览 D: 时这次安装也只落 C:（on64 只在单次调用期间临时切卷，返回前切回）。
    uint32_t t = 0, sz = 0;
    {
        const int sys0 = vfs64_system_slot64();
        if (sys0 < 0 || vfs64_stat_on64(sys0, "/", &t, &sz) != 0) {
            if (vfs64_mount_system64(drive, part_lba) != 0) { p64_log2("[PROC64] install FAILED reason=", "mount"); return -1; }
        }
    }
    const int sys = vfs64_system_slot64();
    if (vfs64_stat_on64(sys, PROC64_PATH64, &t, &sz) == 0) {            // 幂等
        dbg64_line_begin64();
        dbg64_str("[PROC64] install skipped (exists) /proc64.elf size=");
        dbg64_dec(sz);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const int w = vfs64_write_on64(sys, PROC64_PATH64, _binary_build64_proc64_elf_start, (int)bytes);
    if (w != (int)bytes) { p64_log2("[PROC64] install FAILED reason=", "write"); return -1; }
    dbg64_line_begin64();
    dbg64_str("[PROC64] install ok path=/proc64.elf bytes=");
    dbg64_dec(bytes);
    dbg64_nl();
    dbg64_line_end64();
    // 不在这里 return：接着装 /pipe64.elf（下面那段）
    // ★ 批次 D：顺手把 ring3 pipe 演示程序（/pipe64.elf）也幂等装进去 —— 同一套"blob 校验 +
    //   幂等跳过 + 真写盘"的路径，避免再写一份几乎相同的函数。
    {
        const uint32_t pbytes = (uint32_t)(_binary_build64_pipe64_elf_end - _binary_build64_pipe64_elf_start);
        if (pbytes < 64 || !elf64_blob_ok64(_binary_build64_pipe64_elf_start, pbytes)) {
            p64_log2("[PROC64] install FAILED reason=blob path=/pipe64.elf", "");
            return -1;
        }
        if (vfs64_stat_on64(sys, PROC64_PIPE_PATH64, &t, &sz) == 0) {
            dbg64_line_begin64();
            dbg64_str("[PROC64] install skipped (exists) /pipe64.elf size=");
            dbg64_dec(sz);
            dbg64_nl();
            dbg64_line_end64();
        } else {
            const int w2 = vfs64_write_on64(sys, PROC64_PIPE_PATH64, _binary_build64_pipe64_elf_start, (int)pbytes);
            if (w2 != (int)pbytes) { p64_log2("[PROC64] install FAILED reason=", "write(/pipe64.elf)"); return -1; }
            dbg64_line_begin64();
            dbg64_str("[PROC64] install ok path=/pipe64.elf bytes=");
            dbg64_dec(pbytes);
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    return 0;
}

// ==================== 批次 D：ring3 pipe 演示（fork 后父子各持一端通信）====================
// 复杂度同样在用户程序（user/pipe64.asm）：pipe(22) -> fork(57) -> 子进程写写端 / 父进程读读端
//   -> 父进程打印收到的内容 -> wait4(61) -> exit(60)。内核这侧只做：建进程 + 有界等待 + 打点。
// 为什么值得单开一个演示而不是塞进 /proc64.elf：proc64_test 断言了那次 fork 的 pages=8（映像 4 页
//   + 栈 4 页），往它里面加代码会挪动页数 —— 新程序不动既有证据。
int proc64_pipe_demo64(const char* path) {
    if (!g_isolate64) {
        dbg64_line_begin64();
        dbg64_str("[PROC64] pipe-demo skipped (shared address space mode: no fork)\n");
        dbg64_line_end64();
        return 0;
    }
    const char* p = (path && path[0] == '/') ? path : PROC64_PIPE_PATH64;
    uint32_t t = 0, sz = 0;
    if (vfs64_stat(p, &t, &sz) != 0) {
        p64_log2("[PROC64] pipe-demo skipped (no elf on vfs) path=", p);
        return 0;
    }
    const int pid = proc64_create64("pipedemo", 0);
    if (pid < 0) { p64_log2("[PROC64] pipe-demo skipped (create failed) path=", p); return 0; }
    if (proc64_start_elf64(pid, p) != 0) {
        proc64_destroy64(pid);
        p64_log2("[PROC64] pipe-demo skipped (start failed) path=", p);
        return 0;
    }
    const uint64_t t0 = g_ticks64;
    Proc64* ip = p64_find64(pid);
    while (ip && ip->state != PROC64_EXITED &&
           (g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * (uint64_t)PROC64_PIPE_TIMEOUT_SEC) {
        task_sleep64(2);
    }
    const int exited = (ip && ip->state == PROC64_EXITED) ? 1 : 0;
    const uint32_t code = ip ? ip->exit_code : 0;
    if (!exited) {
        p64_log31("[PROC64] pipe-demo TIMEOUT pid=", (uint64_t)pid, " (still running)");
        (void)proc64_kill64(pid, 9);
    }
    if (p64_find64(pid)) proc64_destroy64(pid);
    dbg64_line_begin64();
    dbg64_str("[PROC64] pipe-demo done pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" exited=");
    dbg64_dec((uint64_t)exited);
    dbg64_str(" code=");
    dbg64_dec((uint64_t)code);
    dbg64_str(" ticks=");
    dbg64_dec(g_ticks64 - t0);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 启动期多进程演示 ====================
// 复杂度都在用户程序（user/proc64.asm）里：fork -> 子 execve /hello.elf -> 父 wait4 ->
// 再 fork 两个子（一个 execve 自己带 argv、一个等被 kill）-> 父 wait4 收两次 -> exit(0)。
// 内核这侧只负责：建 init 进程 + 跑它 + 有界等待 + 收尾 + 打点。
int proc64_demo64(const char* path) {
    if (!g_isolate64) {
        dbg64_line_begin64();
        dbg64_str("[PROC64] demo skipped (shared address space mode: no per-process cr3)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    const char* p = (path && path[0] == '/') ? path : PROC64_PATH64;
    uint32_t t = 0, sz = 0;
    if (vfs64_stat(p, &t, &sz) != 0) {
        p64_log2("[PROC64] demo skipped (no elf on vfs) path=", p);
        return 0;
    }
    const int pid = proc64_create64("init", 0);
    if (pid < 0) { p64_log2("[PROC64] demo skipped (create failed) path=", p); return 0; }
    if (proc64_start_elf64(pid, p) != 0) {
        proc64_destroy64(pid);
        p64_log2("[PROC64] demo skipped (start failed) path=", p);
        return 0;
    }

    // 有界等待：init 自己会 fork/execve/wait4/kill，最后 exit(0)
    const uint64_t t0 = g_ticks64;
    Proc64* ip = p64_find64(pid);
    uint64_t next_hb = t0 + 250;
    while (ip && ip->state != PROC64_EXITED &&
           (g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * (uint64_t)PROC64_DEMO_TIMEOUT_SEC) {
        if (g_ticks64 >= next_hb) {                     // 心跳：证明"内核还活着 + 目标进程什么状态"
            next_hb = g_ticks64 + 250;
            dbg64_line_begin64();
            dbg64_str("[PROC64] demo alive ticks=");
            dbg64_dec(g_ticks64 - t0);
            dbg64_str(" pid=");
            dbg64_dec((uint64_t)pid);
            dbg64_str(" state=");
            dbg64_dec((uint64_t)ip->state);
            dbg64_str(" exit_after_wake=");
            dbg64_dec((uint64_t)ip->exit_code);
            dbg64_str(" procs=");
            dbg64_dec((uint64_t)g_proc_count);
            dbg64_nl();
            dbg64_line_end64();
        }
        task_sleep64(2);
    }
    const int exited = (ip && ip->state == PROC64_EXITED) ? 1 : 0;
    const uint32_t code = ip ? ip->exit_code : 0;
    if (!exited) {
        p64_log31("[PROC64] demo TIMEOUT pid=", (uint64_t)pid, " (init still running)");
        (void)proc64_kill64(pid, 9);
    }
    if (p64_find64(pid)) proc64_destroy64(pid);

    dbg64_line_begin64();
    dbg64_str("[PROC64] demo done pid=");
    dbg64_dec((uint64_t)pid);
    dbg64_str(" exited=");
    dbg64_dec((uint64_t)exited);
    dbg64_str(" code=");
    dbg64_dec((uint64_t)code);
    dbg64_str(" ticks=");
    dbg64_dec(g_ticks64 - t0);
    dbg64_str(" procs_left=");
    dbg64_dec((uint64_t)g_proc_count);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== 自检 ====================
// 位含义：bit0 模式探测（引导期页表自证一致）、bit1 进程表槽位/pid 递增、
//         bit2 地址空间布局（PML4[511] 共享 + PDPT[0..3] 共享 + PDPT[4] 空）、
//         bit3 常量与上限自洽、bit4 创建/释放往返（用户页与页表页都还回去了）
int proc64_selftest64() {
    int fail = 0;

    // ---- bit0：模式探测 ----
    {
        const uint64_t cr3 = p64_rd_cr364();
        if (g_boot_cr364 == 0) fail |= 1;
        if (p64_page64(cr3) != (uint64_t)ML64_PML4_PHYS && g_isolate64) fail |= 1;  // BIOS 路径必须是 0x40000
        if (g_isolate64 && (g_boot_cr364 & ~0xFFFULL) != (uint64_t)ML64_PML4_PHYS) fail |= 1;
    }

    // ---- bit3：常量自洽 ----
    if (USER64_WINDOW_BYTES64 != 1024ULL * 1024ULL) fail |= 8;
    if (USER64_MMAP_VA64 < USER64_BRK_VA64 + USER64_BRK_BYTES64) fail |= 8;
    if (USER64_MMAP_VA64 + USER64_MMAP_MIN_BYTES64 > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) fail |= 8;
    if (PROC64_FORK_MAX_PAGES < 16 || PROC64_FORK_MAX_PAGES > 4096) fail |= 8;
    if (PROC64_MAX < 2 || PROC64_MAX > 64) fail |= 8;
    if (PROC64_MAX > 16) fail |= 8;                       // 内存里那两张静态帧表是 16 项（见 fork/start）

    // ---- bit1/bit2/bit4：只在隔离模式下才能真建进程 ----
    if (g_isolate64) {
        const int n0 = g_proc_count;
        const int a = proc64_create64("selftest", 0);
        const int b = proc64_create64("selftest2", 0);
        if (a <= 0 || b <= 0 || a == b) fail |= 2;         // 槽位/pid 递增
        if (!proc64_find64(a) || !proc64_find64(b)) fail |= 2;
        if (g_proc_count != n0 + 2) fail |= 2;
        if (a > 0) {
            Proc64* pa = p64_find64(a);
            Proc64* pb = p64_find64(b);
            // bit2：布局自证
            if (pa) {
                const uint64_t* pml4 = p64_phys64(pa->pml4_phys);
                const uint64_t* boot = p64_phys64(p64_page64(g_boot_cr364));
                if (pml4[511] != boot[511]) fail |= 4;                       // 内核高半区共享
                if (p64_page64(pml4[0]) == p64_page64(boot[0])) fail |= 4;   // 顶层不能共用
                const uint64_t* pdpt = p64_phys64(p64_page64(pml4[0]));
                const uint64_t* bpdpt = p64_phys64(p64_page64(boot[0]));
                for (int i = 0; i < 4; i++) if (pdpt[i] != bpdpt[i]) fail |= 4;  // 前 4GB 恒等映射共享
                if (pdpt[4] != 0) fail |= 4;                                 // 用户窗口私有（初始为空）
                if (pa->cr3 != pa->pml4_phys) fail |= 4;
            }
            // bit4：地址空间互不相同 + 释放干净
            if (pb && pa && pa->cr3 == pb->cr3) fail |= 2;
        }
        if (a > 0) proc64_destroy64(a);
        if (b > 0) proc64_destroy64(b);
        if (g_proc_count != n0) fail |= 16;
        if (proc64_find64(a) || proc64_find64(b)) fail |= 16;
    }

    if (fail == 0) { dbg64_line_begin64(); dbg64_str("[PROC64] selftest PASS"); dbg64_nl(); dbg64_line_end64(); }
    else {
        dbg64_line_begin64();
        dbg64_str("[PROC64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
        dbg64_line_end64();
    }
    return fail;
}

// ==================== UEFI 运行期 CR3 实验（编译期开关，默认关）====================
// 做什么（完整做法/现象/结论见 docs/UEFI地址空间实验报告.md）：
//   背景：UEFI 路径下内核跑在**固件当前活动的 PML4** 上（EDK2 把页表页标成只读），所以现在
//   UEFI 一律降级成"共享地址空间模式"。这份代码只在 -DPROC64_UEFI_CR3_EXPERIMENT=1 的构建里
//   存在，用来实测两件事：能不能把用户窗口**就地**挂进固件 PML4；运行期 `mov cr3` 到底行不行。
//   方案 A（就地挂载）：临时清 CR0.WP -> 在固件 PML4[0] 的 PDPT[4] 挂自建 PD -> 恢复 WP ->
//     开 WP-kludge（usermode64）+ 覆盖 user64_available64 -> 试进 ring3。
//   方案 B（自带 PML4）：新建 PML4/PDPT，复制固件的 512 项 + 0..4GB 的 PDPTE，PDPT[4] 给用户
//     窗口留空 -> 打 stage=cr3（在 mov cr3 **之前**，复位时这就是最后一行）-> mov cr3 ->
//     活了就打 stage=enter -> 试进 ring3。成功则把隔离模式真的打开（g_isolate64 = 1）。
//   两个方案都做（规格要求），按顺序各打点，最后一行给 result=A|B|none mode=isolated|shared。
#if defined(PROC64_UEFI_CR3_EXPERIMENT) && (PROC64_UEFI_CR3_EXPERIMENT == 1)
extern "C" uint64_t g_user64_exit_code64;      // usermode64.cpp：最近一次 ring3 exit 的 code

// ring3 探针（int 0x80 自有 ABI）：write(1, msg, len) + exit(2, 0) + jmp $。
// 字符串放在用户代码页偏移 0x40（同页内，页映射已经覆盖）。
static const char P64_EXP_MSG64[] = "PROC64-UEFI-EXP-RING3\n";
static uint32_t p64_exp_build_blob64(uint8_t* out) {
    const uint32_t slen = (uint32_t)(sizeof(P64_EXP_MSG64) - 1);
    const uint64_t sva  = USER64_CODE_VA64 + 0x40;
    uint32_t n = 0;
    out[n++] = 0xB8; out[n++] = 0x01; out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00;  // mov eax,1
    out[n++] = 0xBF; out[n++] = 0x01; out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00;  // mov edi,1
    out[n++] = 0x48; out[n++] = 0xBE;                                                    // movabs rsi,str
    for (int i = 0; i < 8; i++) out[n++] = (uint8_t)(sva >> (8 * i));
    out[n++] = 0xBA;                                                                     // mov edx,len
    for (int i = 0; i < 4; i++) out[n++] = (uint8_t)(slen >> (8 * i));
    out[n++] = 0xCD; out[n++] = 0x80;                                                    // int 0x80
    out[n++] = 0xB8; out[n++] = 0x02; out[n++] = 0x00; out[n++] = 0x00; out[n++] = 0x00;  // mov eax,2
    out[n++] = 0x31; out[n++] = 0xFF;                                                     // xor edi,edi
    out[n++] = 0xCD; out[n++] = 0x80;                                                     // int 0x80
    out[n++] = 0xEB; out[n++] = 0xFE;                                                     // jmp $
    while (n < 0x40) out[n++] = 0x00;
    for (uint32_t i = 0; i <= slen; i++) out[n++] = (uint8_t)P64_EXP_MSG64[i];
    return n;
}
// 试进 ring3：返回 0 = 真的跑完且退出码 0；非 0 = 失败（值就是原始退出码/错误）
static int p64_exp_try_ring364(const char* tag) {
    uint8_t blob[128];
    const uint32_t n = p64_exp_build_blob64(blob);
    g_user64_exit_code64 = 0xFFFFFFFFULL;         // 区分"没跑到 exit"（0xFF..FF 会原样出现）
    if (user64_run_blob64(blob, n, tag) != 0) return 1;
    return (int)(g_user64_exit_code64 & 0xFFULL);
}
static void p64_exp_stage64(const char* tag, const char* stage, uint64_t err) {
    dbg64_line_begin64();
    dbg64_str("[PROC64] uefi exp ");
    dbg64_str(tag);
    dbg64_str(" stage=");
    dbg64_str(stage);
    dbg64_str(" err=");
    dbg64_hex64(err);
    dbg64_nl();
    dbg64_line_end64();
}
// 临时关 CR0.WP（写固件只读页表页的唯一手法；与 boot/efi/uefi64.c 同款）
static inline uint64_t p64_wp_off64() {
    uint64_t c0; __asm__ volatile("mov %%cr0, %0" : "=r"(c0));
    const uint64_t off = c0 & ~0x10000ULL;
    __asm__ volatile("mov %0, %%cr0" ::"r"(off) : "memory");
    return c0;
}

static int g_uefi_exp_a_ok64 = 0;     // 方案 A 是否已经成功（B 回滚时要把 WP-kludge 恢复成它的状态）
static inline void p64_wp_on64(uint64_t c0) { __asm__ volatile("mov %0, %%cr0" ::"r"(c0) : "memory"); }

// ---- 方案 A：就地挂载（不换 CR3）----
static int p64_uefi_exp_a64() {
    const uint64_t cr3 = p64_rd_cr364();
    uint64_t* pml4 = p64_phys64(p64_page64(cr3));
    const uint64_t e0 = pml4[0];
    if (!(e0 & PTE_PRESENT_64) || (e0 & 0x80ULL)) { p64_exp_stage64("A", "map", 1); return 1; }
    uint64_t* pdpt = p64_phys64(p64_page64(e0));
    void* pd = page_alloc_64();
    if (!pd) { p64_exp_stage64("A", "map", 2); return 1; }
    p64_zero(pd, PAGE_SIZE_64);
    const uint64_t c0 = p64_wp_off64();
    pml4[0]  = e0 | PTE_WRITE_64 | PTE_USER_64;                    // 顶层打开 U/S + RW
    pdpt[4]  = (uint64_t)(uintptr_t)pd | PTE_PRESENT_64 | PTE_WRITE_64 | PTE_USER_64;  // 用户窗口
    p64_wp_on64(c0);
    // 读回校验：写入真的落地了（读不会 #PF，所以这一步失败是可判定的）
    if (!(pml4[0] & PTE_USER_64) || p64_page64(pdpt[4]) != (uint64_t)(uintptr_t)pd) {
        p64_exp_stage64("A", "map", 3);
        // 只有"确实没挂上"才回收这个 PD 页：挂上了却读回不一致的话，页表里还指着它，
        // 回收会留下悬空项（后面 user64_map_page64 复用同一物理页 -> 页表被踩烂）。
        if (p64_page64(pdpt[4]) != (uint64_t)(uintptr_t)pd) page_free_64(pd);
        return 1;
    }
    p64_exp_stage64("A", "map", 0);

    user64_set_wp_kludge64(1);                                     // 允许挂 PDPTE/PD/PTE（只在本实验里）
    user64_force_available64(1);
    p64_exp_stage64("A", "enter", 0);
    const int rc = p64_exp_try_ring364("cr3expA");
    if (rc == 0) { p64_exp_stage64("A", "ok", 0); g_uefi_exp_a_ok64 = 1; return 0; }
    p64_exp_stage64("A", "fail", (uint64_t)(uint32_t)rc);
    return 1;
}

// ---- 方案 B：自带 PML4 + mov cr3（实验的核心）----
static int p64_uefi_exp_b64() {
    const uint64_t fw_cr3 = p64_rd_cr364();
    const uint64_t* fw = p64_phys64(p64_page64(fw_cr3));
    void* pml4p = page_alloc_64();
    void* pdptp = pml4p ? page_alloc_64() : nullptr;
    if (!pml4p || !pdptp) {
        if (pml4p) page_free_64(pml4p);
        if (pdptp) page_free_64(pdptp);
        p64_exp_stage64("B", "build", 1);
        return 1;
    }
    p64_zero(pml4p, PAGE_SIZE_64);
    p64_zero(pdptp, PAGE_SIZE_64);
    uint64_t* pml4 = p64_phys64((uint64_t)(uintptr_t)pml4p);
    uint64_t* pdpt = p64_phys64((uint64_t)(uintptr_t)pdptp);
    for (int i = 0; i < 512; i++) pml4[i] = fw[i];                 // 内核高半区 + 固件留下的映射
    const uint64_t e0 = fw[0];
    if (!(e0 & PTE_PRESENT_64) || (e0 & 0x80ULL)) {
        page_free_64(pdptp); page_free_64(pml4p);
        p64_exp_stage64("B", "build", 2);
        return 1;
    }
    const uint64_t* fw_pdpt = p64_phys64(p64_page64(e0));
    for (int i = 0; i < 512; i++) pdpt[i] = fw_pdpt[i];            // 前 4GB 恒等映射照抄
    pdpt[4] = 0;                                                   // 用户窗口私有（按需分配）
    pml4[0] = (uint64_t)(uintptr_t)pdptp | (e0 & 0xFFFULL) | PTE_WRITE_64 | PTE_USER_64;
    if (pml4[0] != ((uint64_t)(uintptr_t)pdptp | (e0 & 0xFFFULL) | PTE_WRITE_64 | PTE_USER_64)) {
        page_free_64(pdptp); page_free_64(pml4p);
        p64_exp_stage64("B", "build", 3);
        return 1;
    }
    p64_exp_stage64("B", "build", 0);

    const uint64_t new_cr3 = (uint64_t)(uintptr_t)pml4p;
    p64_exp_stage64("B", "cr3", 0);                                // ★ mov cr3 之前：VMware 若复位，这行是最后一行
    __asm__ volatile("mov %0, %%cr3" ::"r"(new_cr3) : "memory");
    uint64_t got = 0;
    __asm__ volatile("mov %%cr3, %0" : "=r"(got));
    if (p64_page64(got) != new_cr3) {                              // 读回不一致：单 CPU 上不该发生
        __asm__ volatile("mov %0, %%cr3" ::"r"(fw_cr3) : "memory");
        page_free_64(pdptp); page_free_64(pml4p);
        p64_exp_stage64("B", "fail", 4);
        return 1;
    }
    p64_exp_stage64("B", "enter", 0);                              // ★ 能打这行 = 运行期 mov cr3 存活
    user64_set_wp_kludge64(0);                                     // 自带 PML4 都是我们的页：不需要 WP 手法
    user64_force_available64(1);
    const int rc = p64_exp_try_ring364("cr3expB");
    if (rc == 0) {
        p64_exp_stage64("B", "ok", 0);
        g_isolate64  = 1;                                          // ★ 实验成功：隔离模式真的打开
        g_boot_cr364 = new_cr3;                                    // 后续进程从这份 PML4 克隆内核映射
        dbg64_line_begin64();
        dbg64_str("[PROC64] cr3 isolation ON (uefi experiment B verified: per-process cr3 works here)\n");
        dbg64_line_end64();
        return 0;
    }
    p64_exp_stage64("B", "fail", (uint64_t)(uint32_t)rc);
    // 回滚：回到固件 PML4（ring3 没跑起来，别把这个任务留在半成品地址空间里）
    __asm__ volatile("mov %0, %%cr3" ::"r"(fw_cr3) : "memory");
    page_free_64(pdptp);
    page_free_64(pml4p);
    // WP-kludge 恢复成"方案 A 的状态"：A 成功过就还要靠它写共享窗口的页表
    user64_set_wp_kludge64(g_uefi_exp_a_ok64 ? 1 : 0);
    user64_force_available64(g_uefi_exp_a_ok64 ? 1 : 0);
    return 1;
}

static int g_uefi_exp_state64 = 0;    // 0 = 没跑过 1 = 跑过（幂等）
static void p64_uefi_exp_run64() {
    if (g_uefi_exp_state64) return;
    g_uefi_exp_state64 = 1;
    const int a = p64_uefi_exp_a64();
    const int b = p64_uefi_exp_b64();
    dbg64_line_begin64();
    dbg64_str("[PROC64] uefi exp result=");
    dbg64_str((b == 0) ? "B" : (a == 0 ? "A" : "none"));
    dbg64_str(" mode=");
    dbg64_str(g_isolate64 ? "isolated" : "shared");
    dbg64_nl();
    dbg64_line_end64();
}
// 延迟任务入口：先睡 5 秒 —— 保证 [GUI64] ready 已经打出来（实验即使触发 VMware 复位，
// 串口里也已经留下"桌面起来了"这条证据，测试因此能区分"实验失败"与"系统本身起不来"）。
static void p64_uefi_exp_task64(void*) {
    task_sleep64(5000);
    dbg64_line_begin64();
    dbg64_str("[PROC64] uefi exp begin (A: attach into firmware PML4, B: own PML4 + mov cr3)\n");
    dbg64_line_end64();
    p64_uefi_exp_run64();
}
int proc64_uefi_cr3_experiment_start64() {
    if (g_uefi_exp_state64) return 0;
    if (g_isolate64) {                                             // BIOS 路径：本来就隔离，不用实验
        dbg64_line_begin64();
        dbg64_str("[PROC64] uefi exp skipped (boot paging is ours: isolation already on)\n");
        dbg64_line_end64();
        g_uefi_exp_state64 = 1;
        return 0;
    }
    const int tid = task_create64("cr3exp", p64_uefi_exp_task64, nullptr);
    dbg64_line_begin64();
    if (tid < 0) dbg64_str("[PROC64] uefi exp FAILED (task create)\n");
    else {
        dbg64_str("[PROC64] uefi exp task started id=");
        dbg64_dec((uint64_t)tid);
        dbg64_str(" (runs 5s after boot, after GUI ready)\n");
    }
    dbg64_line_end64();
    return (tid < 0) ? -1 : 0;
}
#endif  // PROC64_UEFI_CR3_EXPERIMENT
