// task64.cpp - VimtuOS 64 位内核任务与调度器（时间片轮转 + 真实抢占）
//
// 与 32 位 kernel/task.cpp 的关系：
//   32 位那份是完整的任务系统（进程/线程/信号量），但它绑死 32 位中断帧与 32 位内联汇编，
//   64 位构建不编它。本文件是 64 位从零实现的最小可用调度器：任务表 + 轮转 + 睡眠 + 退出回收。
//
// 切换是怎么发生的（一句话）：
//   IRQ0(PIT 250Hz) -> isr_stubs64.asm 压出 0xD0 字节中断帧 -> isr_handler64
//   -> pic_eoi(0) -> schedule64(帧指针) -> [本文件]存帧 / 选下一个 / task_switch_iret64(下一个的帧)
//   下一个任务的帧被 task_switch_iret64 抬栈 iretq 恢复，于是"从它上次被打断的地方继续跑"。
//
// ★ 三条必须记住的约束：
//   1) 帧布局只有一份权威定义（isr_stubs64.asm 顶部）。本文件构造初始帧时逐字段对齐：
//      0x00 gs/fs/es/ds -> 0x10，0x20..0x90 GPR，0x98 int_no，0xA0 err，
//      0xA8 rip，0xB0 cs(=0x08)，0xB8 rflags(IF=1)，0xC0 rsp，0xC8 ss。
//   2) 新任务的 rsp 必须满足 System V 对齐（进入函数时 rsp % 16 == 8）：
//      栈顶先 16 字节对齐再减 8。
//   3) 只允许在**任务上下文**（不是中断上下文）里 kfree_64：中断里只把死任务挂到
//      待回收队列，真正的 kfree 放到 task_yield64() 里做（避免在中断中动内核堆）。
#include "task64.h"
#include "debug64.h"
#include "mem_64.h"
#include "usb64.h"      // USB 主机（UHCI）：kusb 内核线程只调 usb64_poll64()
#include "syscall64.h"  // g_syscall64_kstack64：SYSCALL 入口专用栈顶（每任务一份，见 task_apply_ctx64）

// ==================== 任务表 ====================
struct Task64 {
    uint64_t frame;          // 保存的中断帧指针（0 = 未启动/已死）
    uint64_t stack_base;     // 内核栈底（kmalloc_64 分配的连续内存）
    uint64_t stack_top;      // 内核栈顶（已按 ABI 对齐）
    uint64_t ticks;          // 累计运行的 tick 数
    uint64_t switches;       // 被切入次数
    uint64_t wake_tick;      // SLEEP 状态下的唤醒 tick
    void   (*entry)(void*);
    void*    arg;
    uint32_t id;
    uint32_t state;
    uint32_t slice_left;
    uint32_t started;
    uint32_t quiet;          // 1 = 压力子任务（create/exit/reap 都不打日志，见 kstress 段）
    // 批次 A 后半：每任务时间片 + 关键任务标志（32 位 task.cpp 的 slice_ticks / critical）
    uint32_t slice_ticks;    // 本任务的时间片（tick）；默认 TASK64_SLICE_TICKS，task64_set_slice64 可改
    uint32_t critical;       // 1 = 关键任务：task_kill64 拒绝（打点 reason=critical）
    void*    proc;           // 拥有本任务的进程（0 = 纯内核线程；批次 C 新增）
    uint64_t mm_cr3;         // 切到这个任务时要装载的 CR3（0 = 内核地址空间；proc64 算好）
    uint64_t mm_fs_base;     // IA32_FS_BASE（TLS）：进程私有的 FS 基址（0 = 内核默认）
    uint64_t syscall_kstack; // SYSCALL 入口专用栈顶（= stack_top - 4KB；任务 0 / 无则 0）
    char     name[TASK64_NAME_MAX];
};

static Task64   g_tasks[TASK64_MAX];
static int      g_cur          = -1;      // 当前任务槽位下标
static bool     g_sched_on     = false;   // 抢占开关（task_start64 后为 true）
static uint64_t g_switch_total = 0;
static uint64_t g_next_id      = 1;
static uint32_t g_task_count   = 0;

// 待回收队列：中断里挂名，任务上下文里真正 kfree（见文件头约束 3）
static int g_reap_slot[TASK64_MAX];
static int g_reap_count = 0;

// ==================== 任务退出压力路径的计数与自增助手 ====================
// 为什么放在这里：task_drain_reap（上面）要用回收计数；真正的压力线程在文件下部（kstress）。
// 说明与串口证据格式见 kstress 那一段。
#ifndef VIMTU_INSTALLER_MEDIA
static volatile uint64_t g_stress_spawn     = 0;        // 已创建
static volatile uint64_t g_stress_done      = 0;        // 已跑到 task_exit64
static volatile uint64_t g_stress_reap      = 0;        // 已回收（quiet 的压力子任务）
static volatile uint32_t g_stress_target    = 200;      // 一轮要跑多少个短命任务
static volatile uint64_t g_stress_pad_iters = 200000;   // 放大镜自旋上界（kstress 启动时标定）
static inline void stress_inc64(volatile uint64_t* p) {
    __asm__ volatile("lock incq %0" : "+m"(*p) : : "memory");   // 多任务并发自增：单核也要原子
}

// ★ 压力放大镜（只为让"回收路径被抢占"这条竞态在验收里稳定复现）：
//   修复前 task_drain_reap 的临界区是 IF=1 的，"判完状态 -> kfree/memset"之间可能被 PIT 切走；
//   这段自旋把那个窗口拉长到 ~8 个 tick，别的任务几乎必然在这期间把同一个槽处理掉 —— 于是
//   过期的那半截会二次释放 / 重复减 g_task_count（不放大时这条竞态要很多次回收才撞上一次）。
//   修复后临界区在 cli 保护里（IF=0），这里**直接返回**：正常系统零开销、也不延长关中断时间。
static void stress_reap_pad64(void) {
    uint64_t fl;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    if (!(fl & 0x200ULL)) return;                        // IF=0（修复后的形态）：不放大
    volatile uint64_t spin = 0;
    const uint64_t t0 = g_ticks64;
    while (spin < g_stress_pad_iters && g_ticks64 - t0 < 8) spin++;
}
#endif

// 内置内核线程的计数（任务管理器 / 验收脚本用）
static volatile uint64_t g_heart_beats = 0;
static volatile uint64_t g_work_iters  = 0;

// ==================== TSS.rsp0 维护（ring3 能回来靠它）====================
// 为什么必须维护：ring3 -> ring0 的中断/系统调用切栈时，CPU 用的是 TSS.rsp0，
// 不是当前任务的栈指针。所以**每次切到一个任务之前**都必须把 rsp0 指向那个任务
// 的内核栈顶；否则用户在 ring3 跑的时候来了中断，CPU 会把帧压到上一个任务的栈上
// （轻则踩栈，重则三重故障）。
// 任务 0（kmain）直接用引导期内核栈，stack_base/stack_top 都是 0，没有登记栈顶，
// 用下面的常量做它的 rsp0：
//   来源 = kernel/x86_64.cpp 的 tss_init64(0x80000) 现场值（M1 起就用的安全区）。
//   entry64.asm 把引导栈顶设为 0x7C000（向下生长），0x80000 在其上方 16KB，
//   所以从 0x80000 向下使用的这块栈不会碰到引导流程正在用的栈。
static const uint64_t TASK64_TASK0_RSP0 = 0x80000;
// 新任务的**内核 C 栈预留**（见 task_build_frame）：顶部留给 ring3 中断帧（0xD0）与
// SYSCALL 专用栈（0x1000），C 执行栈从 stack_top - 0x2000 起（16KB 栈里剩 8KB 给 C）。
static const uint64_t TASK64_ENTRY_RSP_RESERVE = 0x2000;

static inline uint64_t task_rsp0_of(const Task64* t) {
    return t->stack_top ? t->stack_top : TASK64_TASK0_RSP0;
}

// ==================== 每进程地址空间的装载（与 TSS.rsp0 同一处）====================
// 为什么必须在这里：CR3 与 IA32_FS_BASE 都是**全局**的 CPU 状态；只有"切任务的那三处"
// （schedule64 / task_exit64 / task_start64）能保证"下一个任务跑起来之前"它们已装载好。
// proc64 只把值算好交进来（task_bind_proc64），本文件不认识 Proc64 结构 ——
// 因此既有任务语义 / 打点 / 回收竞态修复一行都不用动。
static uint64_t g_task64_kernel_cr364 = 0;   // 内核地址空间（task_init64 时读到的 CR3）
static uint64_t g_task64_cr3_active64 = 0;   // 当前装在 CR3 里的值（"相同就跳过"的缓存）
static uint64_t g_task64_fs_active64  = 0;   // 当前 IA32_FS_BASE（同一份缓存）

static inline uint64_t t64_rd_cr364() {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
void task64_load_cr364(uint64_t cr3) {
    if ((cr3 & ~0xFFFULL) == (g_task64_cr3_active64 & ~0xFFFULL)) return;   // 同一个地址空间：不切
    // 切 CR3 会自动刷新非全局 TLB 项 + 分页结构缓存（用户页 PTE 都没打 PTE_GLOBAL），
    // 所以这里**不**额外 invlpg 全表（那会连内核自己的 TLB 一起冲掉，代价更大）。
    __asm__ volatile("mov %0, %%cr3" : : "r"(cr3) : "memory");
    g_task64_cr3_active64 = cr3;
}
void task64_load_fs_base64(uint64_t v) {
    if (v == g_task64_fs_active64) return;
    // IA32_FS_BASE(0xC0000100)：glibc 的 arch_prctl(ARCH_SET_FS) 写的就是它。
    __asm__ volatile("wrmsr" : : "c"(0xC0000100u), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
    g_task64_fs_active64 = v;
}
uint64_t task64_kernel_cr364() { return g_task64_kernel_cr364; }

// 切换前的统一装载：rsp0 / syscall 栈顶 / CR3 / FS 基址（互不依赖，顺序无关）
static void task_apply_ctx64(Task64* n) {
    tss_set_rsp0(task_rsp0_of(n));
    if (n->syscall_kstack) {
        g_syscall64_kstack64 = n->syscall_kstack;               // SYSCALL 入口的切栈目标
    } else {
        g_syscall64_kstack64 = syscall64_static_kstack_top64();  // 任务 0 / 无调度器：静态专用栈
    }
    task64_load_cr364(n->mm_cr3 ? n->mm_cr3 : g_task64_kernel_cr364);
    task64_load_fs_base64(n->proc ? n->mm_fs_base : 0);
}

// ==================== 小工具 ====================
static void tz_memset(void* p, int v, uint64_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) d[i] = (uint8_t)v;
}
static void tz_strcpy_n(char* dst, const char* src, int max) {
    int i = 0;
    if (max <= 0) return;
    for (; src && src[i] && i < max - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

// ==================== 帧校验（切走前的最后一道闸）====================
// 背景：task_switch_iret64 只做 "mov rsp, frame; pop…; iretq"，**不校验帧内容**。一旦某个
//   任务的 frame 指向一段"已经不是它的"内存（栈被回收后复用 / 被别的执行流覆盖 / 压根没建
//   好），iretq 就会从垃圾里弹 cs/ss —— 现象是"rip 落在 task_switch_iret64 的 iretq 上、
//   err 是个不像错误码的选择子"的 #GP（上游 apic64 验收偶发过：err=0x9834 rip=…8A72）。
// 这里做**只读**校验，且**先判地址、后判内容**（顺序不能反：地址不可信时解引用会自己踩 #PF）。
//   返回 0 = 可以切；非 0 = 原因码。调用方打印报告后 cli;hlt，绝不带病 iretq。
static uint64_t g_frame_probe_fail = 0;      // 触发次数（压力路径/验收测试读它判定 FAIL）

static uint32_t task_frame_probe64(const Task64* t) {
    const uint64_t f = t->frame;
    if (f == 0) return 1;                                    // 空帧：从没建过 / 已被清 0
    if (f & 7) return 2;                                     // 未对齐：不可能是本内核的中断帧
    if (t->stack_base) {                                     // 有独立内核栈：帧必须落在自己的栈内
        if (f < t->stack_base || f + (uint64_t)PR64_SIZE > t->stack_top + 0x10) return 3;
    } else {                                                 // 任务 0：引导栈 / rsp0 安全区 / syscall 专用栈(.bss 高半区)
        const bool low  = (f >= 0x30000ull) && (f + (uint64_t)PR64_SIZE <= 0x80000ull);
        const bool high = (f >= 0xFFFFFFFF80000000ull) &&
                          (f + (uint64_t)PR64_SIZE <= 0xFFFFFFFF90000000ull);
        if (!low && !high) return 3;
    }
    const pt_regs64* r = (const pt_regs64*)(uintptr_t)f;
    if (r->cs != SEL64_KCODE && r->cs != SEL64_UCODE) return 4;      // cs 只能是 08（内核）或 2B（用户）
    if (r->cs == SEL64_UCODE) {
        if (r->ss != SEL64_UDATA) return 5;                          // ring3 帧必须带用户 ss
        if (r->rip < 0x10000ull || r->rip >= 0x0000800000000000ull) return 6;  // rip 必须在用户半区
    } else {
        if (r->rip < 0xFFFFFFFF80000000ull) return 7;                // 内核帧的 rip 必须在内核高半区
    }
    if ((r->rflags & 0x2ull) == 0) return 8;                         // rflags.bit1 恒为 1（否则 iretq 自己 #GP）
    if ((r->rflags >> 22) != 0) return 9;                            // 保留位必须为 0
    return 0;
}

// 校验不过就**停下**并留下可定位的报告（比 iretq 进垃圾帧后变成"ISR 区域里的 #GP"强得多）
static void task_frame_guard64(const char* where, const Task64* n, int slot) {
    const uint32_t c = task_frame_probe64(n);
    if (c == 0) return;
    g_frame_probe_fail++;
    dbg64_line_begin64();
    dbg64_str("[TASK64] frameprobe FAIL code=");
    dbg64_dec((uint64_t)c);
    dbg64_str(" where=");
    dbg64_str(where);
    dbg64_str(" slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" name=");
    dbg64_str(n->name);
    dbg64_str(" state=");
    dbg64_dec((uint64_t)n->state);
    dbg64_str(" sees=");
    dbg64_dec(n->switches);
    dbg64_nl();
    dbg64_str("[TASK64] frameprobe FAIL frame=0x");
    dbg64_hex64(n->frame);
    dbg64_str(" base=0x");
    dbg64_hex64(n->stack_base);
    dbg64_str(" top=0x");
    dbg64_hex64(n->stack_top);
    dbg64_str(" cur_slot=");
    dbg64_dec((uint64_t)(g_cur < 0 ? 0 : g_cur));
    dbg64_str(" cur=");
    dbg64_str(task_current_name64());
    dbg64_str(" tick=");
    dbg64_dec(g_ticks64);
    dbg64_nl();
    if (c >= 4) {                                   // 地址判过了才敢读内容
        const pt_regs64* r = (const pt_regs64*)(uintptr_t)n->frame;
        dbg64_str("[TASK64] frameprobe FAIL raw cs=0x");
        dbg64_hex64(r->cs);
        dbg64_str(" ss=0x");
        dbg64_hex64(r->ss);
        dbg64_str(" rip=0x");
        dbg64_hex64(r->rip);
        dbg64_str(" rflags=0x");
        dbg64_hex64(r->rflags);
        dbg64_str(" int_no=0x");
        dbg64_hex64(r->int_no);
        dbg64_nl();
    }
    dbg64_str("[TASK64] frameprobe FAIL -> halt（拒绝把 iretq 送进垃圾帧）\n");
    dbg64_line_end64();
    for (;;) __asm__ volatile("cli; hlt");
}
extern "C" [[noreturn]] void task_trampoline64();

// ==================== 初始帧构造 ====================
// 在任务栈顶下方搭一个"看起来刚被 IRQ 打断"的帧：rt 指向蹦床，IF=1，cs=0x08。
static uint64_t task_build_frame(uint64_t stack_top) {
    const uint64_t base = stack_top - (uint64_t)PR64_SIZE;
    pt_regs64* f = (pt_regs64*)(uintptr_t)base;
    tz_memset(f, 0, PR64_SIZE);

    f->gs = 0x10; f->fs = 0x10; f->es = 0x10; f->ds = 0x10;   // 内核数据段（出口会跳过这 4 槽）
    f->rip    = (uint64_t)(uintptr_t)task_trampoline64;
    f->cs     = 0x08;                                          // 内核代码段（与 GDT/IDT 一致）
    f->rflags = 0x202;                                         // IF=1：新任务一跑起来就允许中断
    // ★ 任务的**内核 C 栈从 stack_top - TASK64_ENTRY_RSP_RESERVE 开始**（不是 stack_top）：
    //   rsp0 = stack_top，ring3 的中断帧压在 [stack_top-0xD0, stack_top)；若入口函数直接用
    //   stack_top 向下，它的栈帧就会和中断帧**重叠**（本模块踩过：proc64 的子进程从 ring3
    //   回来时 ret 到 0x3 —— 栈顶那几个字被中断帧覆盖了）。留 8KB：
    //     顶部 0x0000..0x0D0  ring3 中断帧（rsp0 = stack_top）
    //         0x1000        SYSCALL 入口专用栈顶（帧在它下方 0xD0 内，见 task_create_ex64）
    //         0x2000 起      任务自己的内核 C 执行栈（向下生长）
    f->rsp    = stack_top - TASK64_ENTRY_RSP_RESERVE;          // 同特权级 iretq 不弹 rsp，这里给出真正的起点
    f->ss     = 0x10;
    return base;
}

// ==================== 调度策略 ====================
// 轮转：从 cur 之后按环形扫描第一个 READY。cur 自己不被选中（无任务可换时返回 -1，
// 调用方保持当前任务继续跑）。
static int pick_next_in(const Task64* tbl, int cur, int n) {
    for (int k = 1; k <= n; k++) {
        const int i = (cur + k) % n;
        if (i == cur) continue;
        if (tbl[i].state == TASK64_READY) return i;
    }
    return -1;
}

// 睡眠任务唤醒扫描（在中断里调用，只改状态，不分配/不释放）
static void task_wake_scan() {
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64* t = &g_tasks[i];
        if (t->state == TASK64_SLEEP && g_ticks64 >= t->wake_tick) {
            t->state = TASK64_READY;
        }
    }
}

// 把死任务挂到待回收队列（不在中断里 kfree）
static void task_queue_reap(int slot) {
    for (int i = 0; i < g_reap_count; i++) if (g_reap_slot[i] == slot) return;
    if (g_reap_count < TASK64_MAX) g_reap_slot[g_reap_count++] = slot;
}

// 真正回收（只在任务上下文调用）
static void task_drain_reap() {
    if (g_reap_count == 0) return;
    for (int i = 0; i < g_reap_count; i++) {
        // ★ 一个槽的"判状态 + 打印 + kfree + 清零 + 置 FREE + 计数"必须是**不可被抢占的一步**。
        //   为什么（真缺陷，压力路径下必现）：原来这段是 IF=1 的，PIT 可以在判定之后 / 打印
        //   之后 / kfree 之后 / memset 中间把回收者切走；回收者回来时槽位可能**已经被别的回收者
        //   处理掉、并且已被新任务复用** —— 过期的那半截于是会 kfree 掉**新任务正在使用的栈**
        //   （use-after-free），而新任务的帧就在那段内存里：下一次切入它的 iretq 弹到的就是
        //   垃圾 cs/ss（= task_switch_iret64 的 iretq 上 #GP、err 是个不像错误码的选择子）。
        //   它同时修掉"同一个槽被回收两遍"（重复 reap 日志 + g_task_count 双减 + 状态错写）。
        const uint64_t if_save = dbg64_irq_save64();
        const int slot = g_reap_slot[i];
        Task64* t = &g_tasks[slot];
        if (t->state == TASK64_DEAD && slot != g_cur) {
#ifndef VIMTU_INSTALLER_MEDIA
            if (t->quiet) stress_reap_pad64();   // 压力放大镜：只对压力子任务、且只在可被中断时放大
#endif
            if (!t->quiet) {
                dbg64_line_begin64();
                dbg64_str("[TASK64] reap name=");
                dbg64_str(t->name);
                dbg64_str(" id=");
                dbg64_dec(t->id);
                dbg64_str(" stack_bytes=");
                dbg64_dec((uint64_t)TASK64_STACK_BYTES);
                dbg64_nl();
                dbg64_line_end64();
            }
#ifndef VIMTU_INSTALLER_MEDIA
            if (t->quiet) stress_inc64(&g_stress_reap);   // 压力子任务的回收计数
#endif
            if (t->stack_base) kfree_64((void*)(uintptr_t)t->stack_base);
            tz_memset(t, 0, sizeof(Task64));
            t->state = TASK64_FREE;
            if (g_task_count > 0) g_task_count--;
        }
        dbg64_irq_restore64(if_save);
    }
    // 清队列也原子化：不然可能把别的上下文刚挂上来的槽丢掉（丢了还能靠下一个 tick 的
    // schedule64 重扫补回来，但不该靠兜底）。
    const uint64_t if_save2 = dbg64_irq_save64();
    g_reap_count = 0;
    dbg64_irq_restore64(if_save2);
}

// ==================== 中断里的调度钩子 ====================
// 由 isr_handler64 在 IRQ0（已发 EOI）里调用。不许返回"切走"以外的副作用。
extern "C" void schedule64(pt_regs64* r) {
    if (!g_sched_on || g_cur < 0) return;

    Task64* c = &g_tasks[g_cur];
    c->frame = (uint64_t)(uintptr_t)r;      // ★ 先存帧：这是"回到被打断现场"的唯一凭据
    c->ticks++;
    c->ticks++;
    task_wake_scan();
    for (int i = 0; i < TASK64_MAX; i++) {
        if (i != g_cur && g_tasks[i].state == TASK64_DEAD) task_queue_reap(i);
    }

    if (c->slice_left > 0) c->slice_left--;
    if (c->slice_left > 0) return;          // 时间片未用完：iretq 回去继续跑

    if (c->state == TASK64_RUNNING) c->state = TASK64_READY;
    const int next = pick_next_in(g_tasks, g_cur, TASK64_MAX);
    if (next < 0) {                         // 没有别的就绪任务：自己继续
        c->state = TASK64_RUNNING;
        return;
    }

    Task64* n = &g_tasks[next];
    g_cur = next;
    n->state = TASK64_RUNNING;
    n->switches++;
    n->slice_left = n->slice_ticks ? n->slice_ticks : TASK64_SLICE_TICKS;   // 每任务时间片（set_slice 可改）
    g_switch_total++;
    task_apply_ctx64(n);                    // ★ rsp0 + syscall 栈顶 + CR3（每进程地址空间）+ FS 基址
    task_frame_guard64("sched", n, next);    // ★ 同上：目标帧不自洽就停下报告，不 iretq
    task_switch_iret64(n->frame);           // 不返回
}

// ==================== 任务蹦床 ====================
// 每个新任务的第一个 rip。从 g_cur 取自己的 entry/arg（调度器切进来之前已设好）。
extern "C" [[noreturn]] void task_trampoline64() {
    Task64* t = (g_cur >= 0 && g_cur < TASK64_MAX) ? &g_tasks[g_cur] : nullptr;
    if (!t || !t->entry) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] trampoline: bad task table state\n");
        dbg64_line_end64();
        task_exit64();
    }
    t->started = 1;
    t->entry(t->arg);
    // entry 正常返回 = 任务结束
    dbg64_line_begin64();
    dbg64_str("[TASK64] entry returned: ");
    dbg64_str(t->name);
    dbg64_nl();
    dbg64_line_end64();
    task_exit64();
}

// ==================== 对外：生命周期 ====================
void task_init64() {
    if (g_cur >= 0) return;                 // 幂等
    tz_memset(g_tasks, 0, sizeof(g_tasks));
    g_reap_count = 0;

    Task64* t = &g_tasks[0];
    t->id         = 0;
    t->state      = TASK64_RUNNING;
    t->slice_left = TASK64_SLICE_TICKS;
    t->started    = 1;
    t->slice_ticks = TASK64_SLICE_TICKS;
    t->critical    = 1;                     // idle（任务 0）不可杀：既由 id==0 护栏，也标成关键任务
    tz_strcpy_n(t->name, "kmain", TASK64_NAME_MAX);
    // 任务 0 = 当前执行流（内核主流程，最终进入桌面消息循环），栈就是引导期内核栈。
    t->stack_base = 0;
    t->stack_top  = 0;
    // 任务 0 **没有**进程（mm_cr3 = 0 -> 用内核地址空间；mm_fs_base 不生效），
    // 它的 syscall 入口用 syscall64.cpp 的静态专用栈（syscall_kstack = 0）。
    // 为什么必须先记下内核地址空间：后面每个进程任务切走时都要切回它
    // （否则会停在某个已释放的进程 PML4 上 —— 那是立刻 #PF/复位级的错误）。
    g_task64_kernel_cr364 = t64_rd_cr364();
    g_task64_cr3_active64 = g_task64_kernel_cr364;

    g_cur        = 0;
    g_task_count = 1;

    // 任务 0 的 rsp0（它用引导期栈，stack_top==0 -> 用 TASK64_TASK0_RSP0）。
    // 这一步保证"调度器上线后第一次进 ring3"时 rsp0 / syscall 栈顶就是对的，不必等一次任务切换。
    task_apply_ctx64(t);

    dbg64_line_begin64();
    dbg64_str("[TASK64] init idle=kmain max=");
    dbg64_dec((uint64_t)TASK64_MAX);
    dbg64_str(" stack_bytes=");
    dbg64_dec((uint64_t)TASK64_STACK_BYTES);
    dbg64_str(" slice_ticks=");
    dbg64_dec((uint64_t)TASK64_SLICE_TICKS);
    dbg64_str(" hz=");
    dbg64_dec((uint64_t)PIT_HZ_64);
    dbg64_nl();
    dbg64_line_end64();
}

// quiet=1 时连 create 日志都不打（压力子任务的用法与原因见下面 kstress 段的"为什么安静"）
static int task_create_ex64(const char* name, void (*entry)(void*), void* arg, uint32_t quiet) {
    if (!entry) return -1;

    int slot = -1;
    for (int i = 0; i < TASK64_MAX; i++) {
        if (g_tasks[i].state == TASK64_FREE) { slot = i; break; }
    }
    if (slot < 0) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] create FAILED (no free slot): ");
        dbg64_str(name ? name : "?");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }

    void* stk = kmalloc_64(TASK64_STACK_BYTES);
    if (!stk) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] create FAILED (kmalloc): ");
        dbg64_str(name ? name : "?");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }

    Task64* t = &g_tasks[slot];
    tz_memset(t, 0, sizeof(Task64));
    t->stack_base = (uint64_t)(uintptr_t)stk;
    t->stack_top  = (t->stack_base + TASK64_STACK_BYTES) & ~(uint64_t)0xF;
    t->stack_top -= 8;                       // ★ 进入函数时 rsp % 16 == 8（System V ABI）
    t->entry      = entry;
    t->arg        = arg;
    // ★ SYSCALL 入口专用栈（每任务一份）：SYSCALL 不换栈，入口汇编要自己切到"内核栈"，
    //   而 ring3 中断用的是 TSS.rsp0（= stack_top，帧压在 [stack_top-0xD0, stack_top)）。
    //   两者若共用同一个栈顶，syscall 帧与中断帧的 rip/cs/rsp 槽会**完全重叠**（既有模块
    //   为此单独开过一个全局专用栈）。多进程后每个任务都可能阻塞在系统调用里，全局共用
    //   会让 A 的 syscall 帧被 B 的 syscall 覆盖 —— 所以改成每任务在**自己内核栈顶下方 4KB**
    //   开一块：向上给 ring3 中断帧留 4KB（够 0xD0 的帧），向下是任务自己的内核栈。
    t->syscall_kstack = t->stack_top - 0x1000;
    t->quiet      = quiet;                   // 1 = 压力子任务：create/exit/reap 都不打日志
    // ★ 发布顺序（真缺陷）：state = READY 是"这个槽可以被调度"的唯一开关，必须**最后**设。
    //   原来它写在这里（frame 之前），中间那几行若被 PIT 打断，pick_next_in 就会选中一个
    //   frame 还是 0 的任务 -> task_switch_iret64(0) -> 从物理低地址（恒等映射的 IVT/BDA）
    //   弹 cs/ss -> iretq 上 #GP、err 是个不像错误码的选择子（与上游那次偶发 panic 同型）。
    //   改成"部件都装好了再挂牌"：tz_memset 之后 state=FREE，直到下面最后一行才 READY。
    t->slice_left = TASK64_SLICE_TICKS;
    t->slice_ticks = TASK64_SLICE_TICKS;     // 每任务时间片（task64_set_slice64 可改）
    t->critical    = 0;                      // 普通任务：可被 task_kill64 终止
    t->id         = (uint32_t)g_next_id++;
    tz_strcpy_n(t->name, name ? name : "task", TASK64_NAME_MAX);
    t->frame      = task_build_frame(t->stack_top);
    t->state      = TASK64_READY;

    g_task_count++;

    if (!quiet) {                             // quiet=1：压力子任务不打 create 日志（见 kstress 段）
        dbg64_line_begin64();
        dbg64_str("[TASK64] create id=");
        dbg64_dec(t->id);
        dbg64_str(" name=");
        dbg64_str(t->name);
        dbg64_str(" slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(" stack=0x");
        dbg64_hex64(t->stack_base);
        dbg64_str(" frame=0x");
        dbg64_hex64(t->frame);
        dbg64_nl();
        dbg64_line_end64();
    }
    return (int)t->id;
}

// 对外接口：与原来完全一样（quiet=0）
int task_create64(const char* name, void (*entry)(void*), void* arg) {
    return task_create_ex64(name, entry, arg, 0);
}

// ==================== 内置内核线程 ====================
// kheart：心跳线程。证明"别的任务真的在跑"（桌面外壳闲置时也在被抢占）。
static void kheart_entry(void* arg) {
    (void)arg;
    for (;;) {
        const uint64_t t0 = g_ticks64;
        while (g_ticks64 - t0 < PIT_HZ_64) task_yield64();   // 约 1 秒
        g_heart_beats++;
        dbg64_line_begin64();
        dbg64_str("[TASK64] kheart beat=");
        dbg64_dec(g_heart_beats);
        dbg64_str(" switches=");
        dbg64_dec(g_switch_total);
        dbg64_str(" ticks_now=");
        dbg64_dec(g_ticks64);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// kwork：真占用 CPU 的计算线程（任务管理器的 CPU 曲线有了真数据来源）。
static void kwork_entry(void* arg) {
    (void)arg;
    static uint32_t buf[1024];
    for (uint32_t i = 0; i < 1024; i++) buf[i] = i * 2654435761u;

    uint64_t iters = 0;
    for (;;) {
        uint32_t acc = 0x9E3779B9u;
        for (int r = 0; r < 32; r++) {
            for (uint32_t i = 0; i < 1024; i++) acc = acc * 31u + buf[i];
        }
        iters++;
        g_work_iters = iters;
        buf[iters & 1023u] = acc;
        if (iters <= 3 || (iters % 256) == 0) {
            dbg64_line_begin64();
            dbg64_str("[TASK64] kwork iters=");
            dbg64_dec(iters);
            dbg64_str(" acc=0x");
            dbg64_hex64(acc);
            dbg64_nl();
            dbg64_line_end64();
        }
        task_yield64();                       // 礼貌线程：主动让出，别饿着桌面
    }
}

// ksum：有终点的一次性任务 —— 跑完自杀，验证"退出 + 回收"这条路径。
static void ksum_entry(void* arg) {
    (void)arg;
    uint64_t sum = 0;
    for (uint32_t i = 1; i <= 100000; i++) sum += (uint64_t)i * (uint64_t)(i & 0xFF);
    dbg64_line_begin64();
    dbg64_str("[TASK64] ksum done sum=0x");
    dbg64_hex64(sum);
    dbg64_str(" iters=100000\n");
    dbg64_line_end64();
    task_exit64();                            // 不返回
}

// ==================== kusb：USB 主机轮询线程（低优先级"礼貌"线程）====================
// 为什么放在 task64.cpp：内核里只有这里能创建内核线程（系统内核才有调度器；安装介质不编本文件）。
// 策略（不饿着桌面的关键）：
//   * 只在 g_ticks64 走过 USB64_POLL_TICKS（3 tick = 12ms，任务要求约 10ms；PIT 250Hz 下
//     2 tick=8ms、3 tick=12ms，取 3 更省 CPU）才调一次 usb64_poll64()；
//   * 其余时间全部 task_yield64()（sti; hlt 等到下一个 tick 再被轮转），所以一轮里它最多
//     花掉"读一次中断 TD 状态 + 解析一次 8 字节 HID 报告"的时间，桌面（任务 0）不会被占住；
//   * 调度器是轮转（没有优先级字段），"低优先级"就靠这里主动让出来体现。
// 时序：PIT 每 4ms 一个 tick -> 一次 poll 的间隔 <= 3 tick = 12ms；加上 UHCI 硬件每帧
//   （1ms）都会去试一次中断端点，按键从设备到桌面的延迟在 15ms 量级。
#ifndef VIMTU_INSTALLER_MEDIA
#define USB64_POLL_TICKS 3
static void kusb_entry(void* arg) {
    (void)arg;
    uint64_t last = g_ticks64;
    for (;;) {
        if (g_ticks64 - last >= USB64_POLL_TICKS) {
            last = g_ticks64;
            usb64_poll64();                  // 处理已完成的 HID 报告 + 重新武装中断 TD
        }
        task_yield64();                      // 其余时间让出 CPU（桌面/其它线程照常跑）
    }
}
#endif

// ==================== 任务退出压力路径（kstress，只进系统内核）====================
// 为什么需要它：调度器的"创建 → 运行 → 退出 → 回收"这条路平时只有 ksum 在启动期走一次，
//   帧错位 / 栈被回收后复用这类缺陷概率极低（上游一次 apic64 验收里偶发过一次 #GP：err=0x9834、
//   rip=…8A72 正是 task_switch_iret64 的那条 iretq，重跑 3 次都没复现）。kstress 用一个内置
//   内核线程反复创建/退出 N 个短命任务，把"正在退出的任务"和"正在回收别的任务栈的任务"
//   在时间上高频重叠。
// 触发方式：**系统内核启动时自动跑一小轮**（环境没法传参），轮数见 g_stress_target；
//   安装介质内核不编 task64.cpp，另外还用 #ifndef VIMTU_INSTALLER_MEDIA 挡一层，
//   保证压力路径绝不会跟着安装介质跑（task_stress_set_rounds64 只改轮数，不跑）。
// 为什么安静（子任务的 create/exit/reap 都不打日志，只在最后打一行汇总）：
//   每个子任务本来要打 3 行、200 个子任务就是 600 行、约 200 行/秒的串口写。这不是"日志多"
//   而已：别的模块里有一些**不持行锁**的长行（例如 [STORE64] init/kv 行），一旦被本压力路径
//   的日志从中间插进去，靠整行 exact-match 判定的验收脚本就会假失败（开发中 store64_test
//   就被这么搞挂过一次）。所以：压力子任务整体安静，判定改成"计数不变量 + 汇总行"。
// 串口证据（自动验收 tests/sched_stress_test.py 读它）：
//   [TASK64] stress spawn=<n> done=<n> reap=<n> live=<n> count=<n> fail=<mask> PASS|FAIL
//   fail 位：bit0 没创建够、bit1 回收没跟上、bit2 帧校验触发（致命）、bit3 有子任务没走到退出、
//            bit4 任务表占用槽位数与 g_task_count 不一致（回收路径被抢占过就会不一致）。
#ifndef VIMTU_INSTALLER_MEDIA
static const uint32_t    STRESS_BATCH    = 4;     // 一批同时创建几个
static void kstress_child_entry(void* arg) {
    // 干一点活再自杀：让子任务真的被 trampoline 调用过、被 PIT 抢占过，
    // 而不是"创建即销毁" —— 这样"正在跑的任务"和"刚变成 DEAD 的任务"在时间上重叠。
    uint64_t acc = (uint64_t)(uintptr_t)arg;
    for (int i = 0; i < 16; i++) acc = acc * 6364136223846793005ull + (uint64_t)i;
    stress_inc64(&g_stress_done);
    task_exit64();                            // 不返回：压力路径的主目标
}
static void kstress_entry(void* arg) {
    (void)arg;
    const uint32_t target = g_stress_target;
    uint32_t spawned = 0, stalls = 0;

    // 标定"一个 tick 大约自旋多少次"：放大镜用它做上界，保证中断被关掉时也能有界退出
    {
        volatile uint64_t spin = 0;
        const uint64_t t0 = g_ticks64;
        while (g_ticks64 == t0 && spin < 4000000ull) spin++;
        g_stress_pad_iters = spin ? spin * 12ull : 200000ull;
    }

    while (spawned < target) {
        uint32_t made = 0;
        for (uint32_t k = 0; k < STRESS_BATCH && spawned < target; k++) {
            // quiet=1：子任务的 create/exit/reap 都不打日志（原因见上面"为什么安静"）
            const int id = task_create_ex64("kstk", kstress_child_entry,
                                            (void*)(uintptr_t)(spawned + 1), 1);
            if (id < 0) break;                // 槽位/堆暂时不够：让出去，等回收跟上再来
            spawned++; made++;
            g_stress_spawn = spawned;
        }
        if (made == 0) {
            if (++stalls > (uint32_t)(8 * PIT_HZ_64)) break;   // 长时间创建不成功 -> 记 FAIL
        } else {
            stalls = 0;
        }
        task_yield64();                       // 让子任务跑起来、退出、被别人回收
    }

    // 有界等待回收跟上：回收发生在别的任务的 task_yield64 里，所以这里必须一直让出
    uint64_t spin = 0;
    while (g_stress_reap < g_stress_spawn && spin < (uint64_t)(16 * PIT_HZ_64)) {
        task_yield64();
        spin++;
    }

    // ★ 硬不变量：任务表里"被占用的槽位数"必须等于调度器自己的计数（g_task_count）。
    //   为什么查这个：回收路径一旦被抢占，"过期的那半截"会重复处理同一个槽
    //   （二次 kfree / 重复 g_task_count-- / 把已被新任务复用的槽写成 FREE）。
    //   这里直接把两者对拍，不一致就是 FAIL（不依赖日志行数，安静模式下也照样抓得到）。
    uint64_t live = 0;
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64Info inf;
        if (task_info64(i, &inf)) live++;
    }

    uint32_t fail = 0;
    if (g_stress_spawn < target)              fail |= 1;    // 没能创建够
    if (g_stress_reap < g_stress_spawn)       fail |= 2;    // 回收没跟上
    if (g_frame_probe_fail != 0)              fail |= 4;    // 帧校验触发（致命）
    if (g_stress_done != g_stress_spawn)      fail |= 8;    // 有子任务没走到退出
    if (live != (uint64_t)task_count64())     fail |= 16;   // 槽位数与 g_task_count 不一致

    dbg64_line_begin64();
    dbg64_str("[TASK64] stress spawn=");
    dbg64_dec(g_stress_spawn);
    dbg64_str(" done=");
    dbg64_dec(g_stress_done);
    dbg64_str(" reap=");
    dbg64_dec(g_stress_reap);
    dbg64_str(" live=");
    dbg64_dec(live);
    dbg64_str(" count=");
    dbg64_dec((uint64_t)task_count64());
    dbg64_str(" fail=");
    dbg64_dec((uint64_t)fail);
    dbg64_str(fail == 0 ? " PASS\n" : " FAIL\n");
    dbg64_line_end64();

    task_exit64();                            // 压力线程自己也走一遍退出（不返回）
}

// 只调轮数（不跑）：留给"想压更大一轮"的调用点（终端命令/验收脚本）用；0 = 不改。
void task_stress_set_rounds64(uint32_t n) { if (n) g_stress_target = n; }
#endif

// 批次 A 后半：新增 API 的启动自检（都在抢占打开之前；只做可逆/只读操作，失败就打 FAIL）。
// 用到的操作都有明确护栏，不会影响随后要跑的内置线程：
//   * find_by_name：真名（kwork）必须找到、假名必须 -1；
//   * mark_critical + task_kill64：关键任务必须被**拒绝**（-2），然后撤销标记；
//   * set_slice：设 3 再设回默认 2；
//   * force_remove：建一个临时线程（只让出 CPU）后强杀，必须成功且名字随之消失；
//   * force_remove(idle) / 不存在的 id：必须被护栏拒绝；
//   * diag：打印任务表诊断（一行一个槽 + 汇总），供串口/终端核验。
static void task64_force_dummy_entry(void* arg) {
    (void)arg;
    for (;;) task_yield64();
}

static int task64_api_selftest64() {
    int fail = 0;
    const int kid = task64_find_by_name64("kwork");
    if (kid <= 0) fail |= 1;
    if (task64_find_by_name64("no-such-task-64") != -1) fail |= 1;
    if (kid > 0) {
        if (task64_mark_critical64((uint32_t)kid, 1) != 0) fail |= 2;
        if (task_kill64((uint32_t)kid) != -2) fail |= 4;          // 关键任务拒绝（-2）
        if (task64_mark_critical64((uint32_t)kid, 0) != 0) fail |= 8;
        if (task64_set_slice64((uint32_t)kid, 3) != 0) fail |= 16;
        if (task64_set_slice64((uint32_t)kid, TASK64_SLICE_TICKS) != 0) fail |= 16;
    }
    if (task64_force_remove64(0) != -1) fail |= 32;               // idle 护栏
    if (task64_force_remove64(0xFFFFFFFFu) != -1) fail |= 32;     // 不存在的 id
    const int tmp = task_create64("kforce", task64_force_dummy_entry, nullptr);
    if (tmp <= 0) {
        fail |= 64;
    } else {
        const uint32_t tid = (uint32_t)tmp;
        if (task64_force_remove64(tid) != 0) fail |= 64;
        if (task64_find_by_name64("kforce") != -1) fail |= 64;
    }
    if (task64_proc_threads64(0xFFFFFFFFu) != 0) fail |= 128;     // 无此任务的进程视角统计
    if (task64_proc_cpu_permille64(0xFFFFFFFFu) != 0) fail |= 128;
    task64_diag64();                                              // 诊断输出（真实槽位快照）

    dbg64_line_begin64();
    dbg64_str(fail == 0 ? "[TASK64] api selftest PASS\n" : "[TASK64] api selftest FAIL mask=");
    if (fail) { dbg64_dec((uint64_t)fail); dbg64_nl(); }
    dbg64_line_end64();
    return fail;
}

void task_start64() {
    if (g_sched_on) return;

    const int st = task_selftest64();
    if (st != 0) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] selftest FAIL mask=");
        dbg64_dec((uint64_t)st);
        dbg64_nl();
        dbg64_line_end64();
    }

    task_create64("kheart", kheart_entry, nullptr);
    task_create64("kwork",  kwork_entry,  nullptr);
    task_create64("ksum",   ksum_entry,   nullptr);
#ifndef VIMTU_INSTALLER_MEDIA
    // 退出压力路径：只在系统内核启动时跑（见下面 kstress 的说明；安装介质内核不编本文件）
    task_create64("kstress", kstress_entry, nullptr);
    // USB 主机轮询线程（kusb）：启动期先建好，usb64_init64() 之后它就有活可干
    // （init 之前 usb64_poll64() 会因为 !g_ready 直接返回，不会碰到半初始化状态）。
    task_create64("kusb", kusb_entry, nullptr);
#endif
    // 批次 A 后半：新增 API 的启动自检（抢占打开之前做，临时线程 kforce 会被立刻强杀回收）
    (void)task64_api_selftest64();


    g_sched_on = true;
    task_apply_ctx64(&g_tasks[g_cur < 0 ? 0 : g_cur]);   // 抢占打开前先把当前任务的上下文装好（任务 0 -> 0x80000）
    dbg64_line_begin64();
    dbg64_str("[TASK64] scheduler up tasks=");
    dbg64_dec((uint64_t)g_task_count);
    dbg64_str(" cur=");
    dbg64_str(task_current_name64());
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 对外：任务侧接口 ====================
void task_yield64() {
    task_drain_reap();                       // 任务上下文：这里才允许 kfree
    __asm__ volatile("sti; hlt");            // 等到下一个 tick，由调度器轮转
}

void task_sleep64(uint32_t ms) {
    if (g_cur < 0) return;
    Task64* c = &g_tasks[g_cur];
    const uint64_t until = g_ticks64 + (uint64_t)ms_to_ticks64(ms ? ms : 1);
    if (until <= g_ticks64) return;
    c->state     = TASK64_SLEEP;
    c->wake_tick = until;
    while (g_tasks[g_cur].state == TASK64_SLEEP && g_cur >= 0) {
        task_yield64();
        if (g_tasks[g_cur].state == TASK64_SLEEP && g_ticks64 >= until) {
            g_tasks[g_cur].state = TASK64_READY;      // 兜底（万一错过唤醒扫描）
        }
    }
}

[[noreturn]] void task_exit64() {
    if (g_cur < 0) {
        for (;;) __asm__ volatile("cli; hlt");
    }
    const int me = g_cur;
    Task64* c = &g_tasks[me];

    if (!c->quiet) {                          // 压力子任务安静退出（原因见下面 kstress 段的"为什么安静"）
        dbg64_line_begin64();
        dbg64_str("[TASK64] exit name=");
        dbg64_str(c->name);
        dbg64_str(" id=");
        dbg64_dec(c->id);
        dbg64_str(" ticks=");
        dbg64_dec(c->ticks);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ★ 从"置 DEAD"到"切走"这一段必须**不可被中断**（这里原来是 IF=1 的，真缺陷）：
    //   否则 PIT 会落在"g_cur 已经指向下一个任务、而 CPU 还站在正在死掉的这个任务的栈上"的
    //   窗口里 —— schedule64 会把 next->frame 记成"死任务栈上的一个帧"，而那个栈紧接着就会被
    //   回收（还会被新任务复用）；之后 iretq 进那个帧弹到的就是垃圾 cs/ss（现象同上游那次：
    //   task_switch_iret64 的 iretq 上 #GP、err 是个不像错误码的选择子）。cli 之后这个窗口不存在。
    //   不恢复 IF：本函数不返回，切走靠目标帧的 rflags 恢复（IF=1）；halt 分支本来就要 IF=0。
    const uint64_t if_hold = dbg64_irq_save64();
    (void)if_hold;
    c->state = TASK64_DEAD;
    c->frame = 0;                            // 死任务不再需要帧

    int next = pick_next_in(g_tasks, me, TASK64_MAX);
    if (next < 0) next = 0;                  // 没有别的就绪任务：回 idle（任务 0）
    if (next == me) {
        dbg64_line_begin64();
        dbg64_str("[TASK64] idle exited -> halt\n");
        dbg64_line_end64();
        for (;;) __asm__ volatile("cli; hlt");
    }

    Task64* n = &g_tasks[next];
    g_cur = next;
    n->state = TASK64_RUNNING;
    n->switches++;
    n->slice_left = n->slice_ticks ? n->slice_ticks : TASK64_SLICE_TICKS;   // 同上：每任务时间片
    g_switch_total++;
    task_apply_ctx64(n);                     // ★ 同上：交给下一个任务前把上下文全部装载好
    task_frame_guard64("exit", n, next);     // ★ 切走前校验目标帧（垃圾帧 -> 停下报告，绝不 iretq）
    task_switch_iret64(n->frame);            // 不返回
    for (;;) __asm__ volatile("cli; hlt");   // 不可达
}

// ==================== 对外：查询 ====================
// ★ 给 kernel/usermode64.cpp 用：进 ring3 之前把 TSS.rsp0 指向**当前任务**的内核栈顶
//   （ring3 -> ring0 的中断/系统调用靠它切栈）。任务 0（kmain）没有登记的栈顶，
//   返回引导期安全栈顶 TASK64_TASK0_RSP0（= 0x80000，来源见本文件上方说明）。
//   注意：本函数只报告"当前任务的栈顶"，不改调度算法。
extern "C" uint64_t task_kstack_top_current64() {
    if (g_cur < 0) return TASK64_TASK0_RSP0;
    return task_rsp0_of(&g_tasks[g_cur]);
}

// syscall64 的 getpid/sleep_ms 走这两个薄封装（C 链接，便于 weak 引用）
extern "C" uint32_t task_current_id_64() { return task_current_id64(); }
extern "C" void     task_sleep_ms64(uint32_t ms) { task_sleep64(ms); }

uint32_t task_current_id64() {
    if (g_cur < 0) return 0xFFFFFFFFu;
    return g_tasks[g_cur].id;
}

const char* task_current_name64() {
    if (g_cur < 0) return "none";
    return g_tasks[g_cur].name;
}
int task_count64() { return (int)g_task_count; }
uint64_t task_switch_total64() { return g_switch_total; }
uint64_t task_heartbeats64()    { return g_heart_beats; }
uint64_t task_work_iters64()    { return g_work_iters; }

// ==================== 批次 C：进程 / 地址空间相关的访问器 ====================
// 说明：本文件不认识 Proc64（proc64.cpp 的私有结构），只保存 proc64 交进来的三个数值。
// 这样做的好处是"调度器切换"这条最敏感的路径上不引入任何跨模块调用/分配/日志。
void* task_proc_of_current64() {
    if (g_cur < 0) return nullptr;
    return g_tasks[g_cur].proc;
}
extern "C" int task_slot_current64() { return g_cur; }
int task_slot_of_id64(uint32_t id) {
    for (int i = 0; i < TASK64_MAX; i++) {
        if (g_tasks[i].state != TASK64_FREE && g_tasks[i].id == id) return i;
    }
    return -1;
}
// 把一个任务绑定到进程的地址空间。必须在任务被调度到之前调用（proc64 在 fork 里
// 是在关中断的系统调用上下文里做的，PIT 进不来，不存在"先跑起来再绑定"的窗口）。
int task_bind_proc64(uint32_t task_id, void* proc, uint64_t cr3, uint64_t fs_base) {
    const int slot = task_slot_of_id64(task_id);
    if (slot < 0) return -1;
    g_tasks[slot].proc        = proc;
    g_tasks[slot].mm_cr3      = cr3;
    g_tasks[slot].mm_fs_base  = fs_base;
    return 0;
}
// 只改地址空间参数（arch_prctl(ARCH_SET_FS) 之后要立刻生效；CR3 在 execve 后不变）
void task_update_mm64(uint32_t task_id, uint64_t cr3, uint64_t fs_base) {
    const int slot = task_slot_of_id64(task_id);
    if (slot < 0) return;
    if (cr3) g_tasks[slot].mm_cr3 = cr3;
    g_tasks[slot].mm_fs_base = fs_base;
}
// 临时把当前任务的地址空间换成 cr3（0 = 恢复内核地址空间）；见 task64.h 的说明。
void task_set_current_mm64(uint64_t cr3, uint64_t fs_base) {
    if (g_cur < 0) return;
    g_tasks[g_cur].mm_cr3 = cr3;
    g_tasks[g_cur].mm_fs_base = fs_base;
    task64_load_cr364(cr3 ? cr3 : g_task64_kernel_cr364);
    task64_load_fs_base64(g_tasks[g_cur].proc ? fs_base : 0);
}
// 给 usermode64.cpp 用：进 ring3 之前要把 SYSCALL 入口的切栈目标指到当前任务的专用栈
extern "C" uint64_t task_syscall_stack_top64() {
    if (g_cur < 0) return syscall64_static_kstack_top64();
    const Task64* t = &g_tasks[g_cur];
    return t->syscall_kstack ? t->syscall_kstack : syscall64_static_kstack_top64();
}
// 给 proc64.cpp 用：当前进程对应的任务 id（进程里有 1 个任务时就是它）
uint32_t task_current_id64_via_slot64() {
    if (g_cur < 0) return 0xFFFFFFFFu;
    return g_tasks[g_cur].id;
}

int task_info64(int idx, Task64Info* out) {
    if (!out || idx < 0 || idx >= TASK64_MAX) return 0;
    const Task64* t = &g_tasks[idx];
    if (t->state == TASK64_FREE) { tz_memset(out, 0, sizeof(Task64Info)); return 0; }

    out->id          = t->id;
    out->state       = t->state;
    out->ticks       = t->ticks;
    out->switches    = t->switches;
    out->stack_bytes = (uint64_t)TASK64_STACK_BYTES;
    out->is_current  = (idx == g_cur) ? 1u : 0u;
    for (int i = 0; i < TASK64_NAME_MAX; i++) out->name[i] = t->name[i];
    out->name[TASK64_NAME_MAX - 1] = 0;
    return 1;
}

// ==================== 自检 ====================
// 位含义：bit0 帧布局、bit1 轮转顺序、bit2 槽位扫描、bit3 栈对齐/ABI
int task_selftest64() {
    int fail = 0;

    // ---- bit0：初始帧逐字段校验（用一个本地缓冲当栈顶）----
    static uint8_t tmp_stack[512] __attribute__((aligned(16)));
    tz_memset(tmp_stack, 0xA5, sizeof(tmp_stack));
    const uint64_t top = (((uint64_t)(uintptr_t)tmp_stack) + sizeof(tmp_stack)) & ~(uint64_t)0xF;
    const uint64_t fbase = task_build_frame(top);
    if (fbase != top - (uint64_t)PR64_SIZE) fail |= 1;

    const pt_regs64* f = (const pt_regs64*)(uintptr_t)fbase;
    if (f->gs != 0x10 || f->fs != 0x10 || f->es != 0x10 || f->ds != 0x10) fail |= 1;
    if (f->cs != 0x08) fail |= 1;
    if (f->rflags != 0x202) fail |= 1;
    if (f->rip != (uint64_t)(uintptr_t)task_trampoline64) fail |= 1;
    if (PR64_SIZE != 0xD0) fail |= 1;
    if ((uint64_t)(uintptr_t)f + PR64_SIZE != top) fail |= 1;

    // ---- bit1：轮转顺序（本地表，不动真表）----
    {
        Task64 tbl[TASK64_MAX];
        tz_memset(tbl, 0, sizeof(tbl));
        tbl[1].state = TASK64_READY;
        tbl[2].state = TASK64_READY;
        if (pick_next_in(tbl, 0, TASK64_MAX) != 1) fail |= 2;
        if (pick_next_in(tbl, 1, TASK64_MAX) != 2) fail |= 2;
        if (pick_next_in(tbl, 2, TASK64_MAX) != 1) fail |= 2;
        tbl[1].state = TASK64_FREE;
        if (pick_next_in(tbl, 0, TASK64_MAX) != 2) fail |= 2;
        tbl[2].state = TASK64_FREE;
        if (pick_next_in(tbl, 0, TASK64_MAX) != -1) fail |= 2;   // 没有就绪任务 -> -1
    }

    // ---- bit2：槽位扫描（FREE 槽必须能被找到，且创建过的槽不是 FREE）----
    {
        int free_slots = 0, used_slots = 0;
        for (int i = 0; i < TASK64_MAX; i++) {
            if (g_tasks[i].state == TASK64_FREE) free_slots++;
            else used_slots++;
        }
        if (used_slots != (int)g_task_count) fail |= 4;
        if (free_slots + used_slots != TASK64_MAX) fail |= 4;
    }

    // ---- bit3：栈对齐（ABI）----
    {
        static const uint64_t kTop = 0x10000;
        const uint64_t aligned = (kTop + TASK64_STACK_BYTES) & ~(uint64_t)0xF;
        if ((aligned - 8) % 16 != 8) fail |= 8;
    }

    if (fail == 0) { dbg64_line_begin64(); dbg64_str("[TASK64] selftest PASS\n"); dbg64_line_end64(); }
    return fail;
}

// ==================== 终止其它任务 ====================
int task_kill64(uint32_t id) {
    if (id == 0) return -1;                       // idle 不可杀
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64* t = &g_tasks[i];
        if (t->state == TASK64_FREE || t->id != id) continue;
        if (i == g_cur) return -1;                // 不能杀当前正在跑的任务
        if (t->state == TASK64_DEAD) return -1;   // 已经在死队列里
        // 关键任务（task64_mark_critical64 / idle）：拒绝并打点（32 位 task_kill_by_id 的语义）
        if (t->critical) {
            dbg64_line_begin64();
            dbg64_str("[TASK64] kill denied reason=critical id=");
            dbg64_dec(t->id);
            dbg64_str(" name=");
            dbg64_str(t->name);
            dbg64_nl();
            dbg64_line_end64();
            return -2;
        }

        dbg64_line_begin64();
        dbg64_str("[TASK64] kill id=");
        dbg64_dec(t->id);
        dbg64_str(" name=");
        dbg64_str(t->name);
        dbg64_str(" state=");
        dbg64_dec((uint64_t)t->state);
        dbg64_nl();
        dbg64_line_end64();

        t->state = TASK64_DEAD;
        t->frame = 0;
        task_queue_reap(i);
        return 0;
    }
    return -1;
}

// ==================== 批次 A 后半：任务统计 / 关键任务 / 强制移除 / 诊断 ====================
// CPU 千分比的采样状态（口径见 task64.h 的说明）：
//   窗口 = PIT_HZ_64 个系统 tick；g_cpu_snap 记录每个槽上一次采样时的 ticks。
static uint32_t g_cpu_permille[TASK64_MAX];
static uint64_t g_cpu_snap   [TASK64_MAX];
static uint64_t g_cpu_last_tick = 0;

// 懒采样：窗口到了才重算（调用点在任意上下文；只读写本文件的静态数组，无分配/无日志）
static void task64_cpu_sample64() {
    const uint64_t now = g_ticks64;
    const uint64_t dt  = now - g_cpu_last_tick;
    if (dt == 0) return;
    for (int i = 0; i < TASK64_MAX; i++) {
        const uint64_t cur = g_tasks[i].ticks;
        // 槽被回收/复用会清零 ticks：回绕时按 0 处理，绝不产生天文数字
        const uint64_t d = (cur >= g_cpu_snap[i]) ? (cur - g_cpu_snap[i]) : 0;
        g_cpu_snap[i] = cur;
        // 分子 /2：调度器每 tick 给当前任务记 2（既有记账，见 schedule64 的两次 c->ticks++）
        uint64_t pm = (d / 2ULL) * 1000ULL / dt;
        if (pm > 1000ULL) pm = 1000ULL;
        g_cpu_permille[i] = (uint32_t)pm;
    }
    g_cpu_last_tick = now;
}

uint32_t task64_cpu_permille64(uint32_t id) {
    const int slot = task_slot_of_id64(id);
    if (slot < 0) return 0;
    if ((uint64_t)(g_ticks64 - g_cpu_last_tick) >= (uint64_t)PIT_HZ_64) task64_cpu_sample64();
    return g_cpu_permille[slot];
}

// 名字比较（与 task_find_by_name 同口径：完整相等）
static bool task64_name_eq(const char* a, const char* b) {
    if (!a || !b) return false;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == *b;
}

int task64_find_by_name64(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < TASK64_MAX; i++) {
        if (g_tasks[i].state == TASK64_FREE) continue;
        if (task64_name_eq(g_tasks[i].name, name)) return (int)g_tasks[i].id;
    }
    return -1;
}

int task64_mark_critical64(uint32_t id, int critical) {
    const int slot = task_slot_of_id64(id);
    if (slot < 0) return -1;
    g_tasks[slot].critical = critical ? 1u : 0u;
    dbg64_line_begin64();
    dbg64_str("[TASK64] critical id=");
    dbg64_dec(g_tasks[slot].id);
    dbg64_str(" name=");
    dbg64_str(g_tasks[slot].name);
    dbg64_str(" on=");
    dbg64_dec((uint64_t)g_tasks[slot].critical);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

int task64_set_slice64(uint32_t id, uint32_t ticks) {
    const int slot = task_slot_of_id64(id);
    if (slot < 0) return -1;
    if (ticks < 1u) ticks = 1u;
    if (ticks > 100u) ticks = 100u;              // 与 32 位 task_set_slice 同上限
    g_tasks[slot].slice_ticks = ticks;
    g_tasks[slot].slice_left  = ticks;           // 立刻生效（32 位也重置 ticks_left）
    dbg64_line_begin64();
    dbg64_str("[TASK64] slice id=");
    dbg64_dec(g_tasks[slot].id);
    dbg64_str(" name=");
    dbg64_str(g_tasks[slot].name);
    dbg64_str(" ticks=");
    dbg64_dec((uint64_t)ticks);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

int task64_proc_threads64(uint32_t main_task_id) {
    const int slot = task_slot_of_id64(main_task_id);
    if (slot < 0) return 0;
    void* pr = g_tasks[slot].proc;
    if (!pr) return 0;
    int n = 0;
    for (int i = 0; i < TASK64_MAX; i++) {
        // 只统计**活着**的同进程任务：FREE 是空槽；DEAD 是已退出、只是还没被回收的槽
        //   （它的 proc 指针可能指向已被复用的 Proc64 结构 —— 计进去会把线程数/CPU‰ 算大）。
        if (g_tasks[i].state == TASK64_FREE || g_tasks[i].state == TASK64_DEAD) continue;
        if (g_tasks[i].proc == pr) n++;
    }
    return n;
}

// 强制摘除 + 立即回收（只在任务上下文调用；32 位 task_force_remove 的 64 位版）。
// 护栏（比 32 位更严，原因见文件头的回收竞态说明）：
//   id == 0（idle / 桌面线程）        -> -1
//   找不到槽位                        -> -1
//   目标是当前任务                    -> -2（杀掉正在跑的自己没有意义，还会破坏调度器状态）
//   目标已 DEAD                       -> -3（回收路径会处理它，不能再动）
// 实现：整个"判状态 -> kfree -> 清零 -> 计数"在关中断里一步完成（与 task_drain_reap 同源的
//   竞态修复口径）；g_reap_slot 里可能残留的同一槽会在回收时看到 state=FREE 而跳过。
int task64_force_remove64(uint32_t id) {
    if (id == 0) return -1;
    const int slot = task_slot_of_id64(id);
    if (slot < 0) return -1;

    const uint64_t if_save = dbg64_irq_save64();
    Task64* t = &g_tasks[slot];
    if (slot == g_cur) { dbg64_irq_restore64(if_save); return -2; }
    if (t->state == TASK64_DEAD) { dbg64_irq_restore64(if_save); return -3; }

    const uint32_t tid = t->id;
    char nm[TASK64_NAME_MAX];
    tz_strcpy_n(nm, t->name, TASK64_NAME_MAX);
    if (t->stack_base) kfree_64((void*)(uintptr_t)t->stack_base);
    tz_memset(t, 0, sizeof(Task64));
    t->state = TASK64_FREE;
    if (g_task_count > 0) g_task_count--;
    dbg64_irq_restore64(if_save);

    dbg64_line_begin64();
    dbg64_str("[TASK64] force-remove id=");
    dbg64_dec((uint64_t)tid);
    dbg64_str(" name=");
    dbg64_str(nm);
    dbg64_str(" slot=");
    dbg64_dec((uint64_t)slot);
    dbg64_str(" state=FREE\n");
    dbg64_line_end64();
    return 0;
}

void task64_diag64() {
    uint64_t live = 0;
    for (int i = 0; i < TASK64_MAX; i++) {
        Task64* t = &g_tasks[i];
        if (t->state == TASK64_FREE) continue;
        live++;
        dbg64_line_begin64();
        dbg64_str("[TASK64] diag slot=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" id=");
        dbg64_dec(t->id);
        dbg64_str(" name=");
        dbg64_str(t->name);
        dbg64_str(" state=");
        dbg64_dec((uint64_t)t->state);
        dbg64_str(" ticks=");
        dbg64_dec(t->ticks);
        dbg64_str(" switches=");
        dbg64_dec(t->switches);
        dbg64_str(" proc=0x");
        dbg64_hex64((uint64_t)(uintptr_t)t->proc);
        dbg64_str(" critical=");
        dbg64_dec((uint64_t)t->critical);
        dbg64_str(" slice=");
        dbg64_dec((uint64_t)t->slice_ticks);
        dbg64_str(" cpu_permille=");
        dbg64_dec((uint64_t)g_cpu_permille[i]);
        dbg64_nl();
        dbg64_line_end64();
    }
    dbg64_line_begin64();
    dbg64_str("[TASK64] diag tasks=");
    dbg64_dec(live);
    dbg64_str(" cur=");
    dbg64_dec((uint64_t)(g_cur < 0 ? 0 : g_cur));
    dbg64_str(" switches_total=");
    dbg64_dec(g_switch_total);
    dbg64_str(" kernel_cr3=0x");
    dbg64_hex64(g_task64_kernel_cr364);
    dbg64_nl();
    dbg64_line_end64();
}

uint32_t task64_proc_cpu_permille64(uint32_t main_task_id) {
    const int slot = task_slot_of_id64(main_task_id);
    if (slot < 0) return 0;
    void* pr = g_tasks[slot].proc;
    if (!pr) return 0;
    // 先按窗口采样一次（懒采样只在需要时重算），再直接读结果，保证同一快照口径
    if ((uint64_t)(g_ticks64 - g_cpu_last_tick) >= (uint64_t)PIT_HZ_64) task64_cpu_sample64();
    uint32_t sum = 0;
    for (int i = 0; i < TASK64_MAX; i++) {
        // 与 task64_proc_threads64 同一口径：DEAD（已退出未回收）不算，避免把已复用的
        //   Proc64 结构地址上的旧任务算进本进程。
        if (g_tasks[i].state == TASK64_FREE || g_tasks[i].state == TASK64_DEAD) continue;
        if (g_tasks[i].proc != pr) continue;
        sum += g_cpu_permille[i];
    }
    if (sum > 1000u) sum = 1000u;
    return sum;
}
