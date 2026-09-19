// usermode64.cpp - Vimtu64 用户态（ring3）：用户页映射、进 ring3、退出回到 ring0
//
// ============================ 机制说明（为什么这么做）============================
// 1) 用户 VA 选在 4GiB 以上（USER64_CODE_VA64/STACK_VA64，见 usermode64.h）：
//    前 4GB 是 loader 的恒等映射（内核自己正在用），4GiB 以上在 PML4[0] 里还没有 PDPT
//    项 —— 干净。用户页必须把 PML4E[0]/PDPTE/PDE/PTE 四级都补上并打开 U/S 位，
//    缺任何一级 ring3 都会 #PF。给 PML4E[0] 打开 U/S 不危险：低 4GB 的 PDPTE 仍是
//    U/S=0，ring3 到不了任何内核页。
//
// 2) 进 ring3：手工构造 0xD0 的**用户态 iretq 帧**（格式 = kernel/isr_stubs64.asm 的
//    权威定义）：cs=0x2B、ss=0x23、rflags=0x202（IF=1，用户程序可被 PIT 抢占）、
//    rip=用户入口、rsp=用户栈顶；然后用 task_switch_iret64(帧) 抬栈 iretq。
//    注意 task_switch_iret64 出口只 add rsp,32 跳过 4 个段槽（长模式不能 pop ds/es），
//    但 iretq 弹出的 cs/ss 来自帧里的 0xB0/0xC8 槽 —— 那两个值必须正确。
//
// 3) 回内核（最容易出错的一段，做法与理由）：
//    用户程序用 int 0x80 的 exit(2) 退出。此时 CPU 已经把完整的 5 字用户帧
//    （rip/cs/rflags/rsp/ss）连同桩压的字段放在 TSS.rsp0 指向的内核栈上，所以**不需要**
//    另建"内核返回帧"：直接**改写当前系统调用帧**——rip=user64_resume_tramp64、
//    cs=0x08（内核代码段）、rflags=0x002（IF=0）——然后正常从 isr_handler64 返回，
//    由 isr64_common 的出口 iretq 落到蹦床。这是同特权级（0->0）iretq：只弹
//    rip/cs/rflags，不弹 rsp/ss（与内核 PIT 路径同一行为，既有系统一直这么跑）。
//    蹦床 user64_resume_tramp64 进去时 rsp 落在 syscall 栈帧上方（内核栈内，安全），
//    它立刻 cli + 用进 ring3 前保存的 rsp/callee-saved 复位，再 ret 回到
//    user64_run_blob64 的调用点 —— 保存/恢复的都是真实值，不依赖任何"魔法偏移"，
//    内核继续正常跑（后面 os_boot_path 继续 gui64_run 进桌面）。
//
// 4) 进 ring3 前把 TSS.rsp0 指向当前任务内核栈顶（task_kstack_top_current64）：
//    ring3 里来的中断/系统调用只能由 CPU 用 rsp0 切栈；调度器每次切换任务也会更新它
//    （kernel/task64.cpp）。
//
// 5) EFER.NXE：loader 只置了 LME，没置 NXE。用户栈页要打 PTE.NX（数据页不可执行），
//    所以这里显式打开 NXE（只影响"bit63 的含义"，现有页表项 bit63 全为 0，无副作用）。
//
// 串口打点（自动验收 grep，格式勿改）：
//   [USER64] map code=<va> stack=<va> code_pages=<n> stack_pages=<n>
//   [USER64] enter ring3 entry=<va> rsp=<va>
//   [USER64] back to kernel (ring0)
//   [USER64] selftest PASS / [USER64] selftest FAIL mask=<n>
#include "usermode64.h"
#include "debug64.h"
#include "memlayout64.h"    // ML64_PML4_PHYS（判断"引导期页表是不是我们自己的"）
#include "mem_64.h"       // page_alloc_64 / page_free_64 / PTE_* / PAGE_SIZE_64
#include "syscall64.h"  // g_syscall64_kstack64：SYSCALL 入口的切栈目标（每任务一份，见 u64_set_kernel_stack64）
// 页表助手（自己走表）里的 CR3 读法；u64_rd_cr364 在下面定义，这里先用一个前置声明式的小函数。
static inline uint64_t u64_rd_cr364();


// ==================== 用户窗口是否可用（批次 C：UEFI 下必须如实说"不可用"）====================
// 为什么需要这个判断（实测驱动，不是保守起见）：
//   用户窗口要能用，必须往**引导期页表的顶层**写东西（给 PML4[0]/PDPTE/PDE/PTE 打开 U/S —— 见
//   u64_walk64）。BIOS 路径（boot/loader64.asm）那张 PML4（物理 0x40000）是我们自己建的、可写；
//   UEFI 路径（boot/efi/uefi64.c）**不换 CR3**，内核跑在固件当前活动的 PML4 上，而 EDK2 把
//   自己的页表页标成**只读** —— 于是任何"顺手把顶层项打开 U/S"的写入都会立刻 #PF
//   （实测：err=0000000000000003、cr2=0x1F801000，正是固件 PML4 那一页）。
// 结论：UEFI 下**这个用户窗口根本建立不起来**，所以这里如实返回 0，由调用方（os_boot_path /
//   proc64）打一行说明并跳过 ring3 演示，绝不假装成功、更不装作隔离成立。
//   （真正的修法是引导期就把固件页表克隆到自己的页面上再切 CR3 —— 但 VMware EFI 下 mov cr3
//    会立刻复位，见 boot/efi/uefi64.c 的说明；那是引导期的事，本批次不做。）
static int g_user64_avail64 = -1;                 // -1 = 还没判过
int user64_available64() {
    if (g_user64_avail64 >= 0) return g_user64_avail64;
    const uint64_t cr3 = u64_rd_cr364();
    g_user64_avail64 = (cr3 == (uint64_t)ML64_PML4_PHYS) ? 1 : 0;
    dbg64_line_begin64();
    if (g_user64_avail64) {
        dbg64_str("[USER64] user window available (boot paging is ours, cr3=0x");
        dbg64_hex64(cr3);
        dbg64_str(")");
    } else {
        dbg64_str("[USER64] user window NOT available (boot paging is firmware-owned, cr3=0x");
        dbg64_hex64(cr3);
        dbg64_str("): firmware page tables are read-only -> ring3 demos skipped (UEFI path)");
    }
    dbg64_nl();
    dbg64_line_end64();
    return g_user64_avail64;
}
// ==================== 与汇编桥共享的保存区（批次 C：**每任务一份**）====================
// 为什么改成每任务一份（真缺陷级的设计约束）：
//   原来 g_user64_k_rsp64 / g_user64_k_rbx64 ... 是**单份全局**的，前提是"同时只有一个用户
//   程序在 ring3"。多进程后：任务 A 进 ring3 被 PIT 抢占 -> 任务 B 也进 ring3（覆盖这批全局）
//   -> B 先退出（蹦床复位到 B 的 rsp，没问题）-> 之后 A 退出时蹦床会复位到 **B 的** rsp，
//   也就是从 A 的"退出"跳到 B 的调用栈上继续跑 —— 直接踩烂内核。所以：
//     * 每个任务槽位一份 User64Ctx64（rsp + callee-saved）；
//     * 汇编只认一个指针 g_user64_ctxp64（进 ring3 前由 C 侧按"当前任务槽位"设好，
//       exit 的时候由 user64_exit_to_kernel64 再设一次 —— 同一任务，所以两者必然一致）；
//     * in_ring3 也变成按槽位的位图（多进程时"我这一帧该不该改写"只能问自己）。
// extern "C"：汇编块按名字引用（见下面 user64_enter64/user64_resume_tramp64）。
static const int U64_CTX_MAX = 20;                 // >= TASK64_MAX(16)，留几个兜底槽
struct User64Ctx64 { uint64_t rsp, rbx, rbp, r12, r13, r14, r15; };
static User64Ctx64 g_user64_ctx64[U64_CTX_MAX];
extern "C" uint64_t g_user64_ctxp64 = (uint64_t)(uintptr_t)&g_user64_ctx64[0];  // 当前任务的保存区
extern "C" uint64_t g_user64_exit_code64 = 0;  // 最近一次用户 exit(code) 的 code（蹦床写进 rax）
// 是否正处在"用户程序跑在 ring3"的状态：按槽位位图（exit 只对**本任务**这种状态才改写帧）
static uint32_t g_user64_in_ring3_mask64 = 0;

// 进 ring3 用的用户帧（静态：task_switch_iret64 只读它，且进入时 IF=0、立刻被 iretq 消费，
// 所以多任务共用一块也安全 —— 见下面 user64_enter64 的 cli 说明）。
static pt_regs64 g_user64_frame64;

extern "C" uint64_t user64_enter64(uint64_t next_frame);   // 不返回：exit 后从 call 之后继续
extern "C" void     user64_resume_tramp64();               // 由被改写的 syscall 帧 iretq 落到这里
// task64.cpp 提供（安装程序内核不链它 -> weak 引用，见下面对每个引用的判空）
extern "C" uint64_t task_kstack_top_current64() __attribute__((weak));
extern "C" uint64_t task_syscall_stack_top64()  __attribute__((weak));
extern "C" int      task_slot_current64()       __attribute__((weak));

// 当前任务槽位（0 = 任务 0 / 没有调度器）。多进程的"每任务一份"就靠它。
static int u64_ctx_slot64() {
    if (!task_slot_current64) return 0;
    const int s = task_slot_current64();
    return (s >= 0 && s < U64_CTX_MAX) ? s : 0;
}
// 把汇编用的保存区指针指向当前任务那一份（进 ring3 前 / exit 改写帧前各一次）
static void u64_ctx_bind64() { g_user64_ctxp64 = (uint64_t)(uintptr_t)&g_user64_ctx64[u64_ctx_slot64()]; }
static inline void u64_in_ring3_set64(int on) {
    const uint32_t bit = 1u << u64_ctx_slot64();
    if (on) g_user64_in_ring3_mask64 |= bit; else g_user64_in_ring3_mask64 &= ~bit;
}
static inline bool u64_in_ring3_get64() { return (g_user64_in_ring3_mask64 >> u64_ctx_slot64()) & 1u; }

// ==================== 槽位回收（给 task64 的回收/强杀路径调用）====================
// 缺陷与修法见 usermode64.h 的同名声明。三条不可少的动作：
//   1) 清 in_ring3 位（不这么做，复用该槽的新任务就是 reason=2 进不去）；
//   2) 清每任务保存区（rsp/callee-saved）—— 死任务永远不会再读它，但留着脏数据没有意义，
//      也会让"槽位到底干净不干净"这件事无从核对；
//   3) 如果汇编指针 g_user64_ctxp64 正好指着这一槽，就把它挪回 0 号槽（防御：任何后续
//      误用都指向一块静态内存，而不是某个已被 memset 的任务状态）。
// 调用时机关中断（task_drain_reap 的临界区）/ 单 CPU：位图与保存区的读改写不会被并发穿插。
extern "C" void user64_slot_release64(int slot) {
    if (slot < 0 || slot >= U64_CTX_MAX) return;
    const uint32_t bit = 1u << slot;
    const bool was_in_ring3 = (g_user64_in_ring3_mask64 & bit) != 0;
    g_user64_in_ring3_mask64 &= ~bit;
    uint64_t* w = (uint64_t*)&g_user64_ctx64[slot];
    for (uint32_t i = 0; i < (uint32_t)(sizeof(User64Ctx64) / sizeof(uint64_t)); i++) w[i] = 0;
    if (g_user64_ctxp64 == (uint64_t)(uintptr_t)&g_user64_ctx64[slot]) {
        g_user64_ctxp64 = (uint64_t)(uintptr_t)&g_user64_ctx64[0];
    }
    if (was_in_ring3) {                         // 只有真的清掉脏位才打点（正常退出/普通任务不打）
        dbg64_line_begin64();
        dbg64_str("[USER64] slot release slot=");
        dbg64_dec((uint64_t)slot);
        dbg64_str(" in_ring3=1 -> cleared\n");
        dbg64_line_end64();
    }
}

// ==================== 进出 ring3 的汇编桥 ====================
// 只能写在文件作用域的 asm 块里：这两段是"半个函数"的跳转目标，C++ 表达不出来。
//   user64_enter64(rdi = 目标帧)：保存内核继续点（rsp + callee-saved）后跳 task_switch_iret64。
//   user64_resume_tramp64       ：复位栈/寄存器后 ret，回到 user64_run_blob64 的调用点。
__asm__(
".text\n"
".globl user64_enter64\n"
".type user64_enter64,@function\n"
"user64_enter64:\n"
"    cli\n"                                     // 保存期间不能被 PIT 抢占（下面写同一批全局）
"    movq g_user64_ctxp64(%rip), %rax\n"          // ★ 每任务一份的保存区（C 侧已按槽位设好）
"    movq %rbx, 8(%rax)\n"
"    movq %rbp, 16(%rax)\n"
"    movq %r12, 24(%rax)\n"
"    movq %r13, 32(%rax)\n"
"    movq %r14, 40(%rax)\n"
"    movq %r15, 48(%rax)\n"
"    movq %rsp, 0(%rax)\n"                      // rsp 最后写（rax 被上面用掉；帧里的 rax 另行弹出）
"    jmp task_switch_iret64\n"                  // 不返回：按 rdi 指向的帧 iretq（可切到 ring3）
".size user64_enter64, .-user64_enter64\n"
".globl user64_resume_tramp64\n"
".type user64_resume_tramp64,@function\n"
"user64_resume_tramp64:\n"                       // 入口处 cs=0x08（内核），rsp 在内核栈内但不保证位置
"    cli\n"
"    movw $0x10, %ax\n"
"    movw %ax, %ss\n"                            // 恢复内核 SS（长模式下基址无意义，只为与 entry64 一致）
"    movq g_user64_ctxp64(%rip), %rax\n"          // ★ 同一份保存区（exit 前 C 侧刚设过；IF=0 期间不会被换）
"    movq 0(%rax), %rsp\n"                       // ★ 复位到进 ring3 前的内核栈
"    movq 8(%rax), %rbx\n"
"    movq 16(%rax), %rbp\n"
"    movq 24(%rax), %r12\n"
"    movq 32(%rax), %r13\n"
"    movq 40(%rax), %r14\n"
"    movq 48(%rax), %r15\n"
"    movq g_user64_exit_code64(%rip), %rax\n"    // 返回值 = 退出码（C 侧同时用返回值，见 enter_at64）
"    sti\n"                                      // 栈已复位，可以开中断
"    ret\n"                                      // [rsp] = user64_enter64 的返回地址
".size user64_resume_tramp64, .-user64_resume_tramp64\n"
);

// ==================== 页表助手（自己走表）====================
// 注意：kernel/mem_64.h 里的 map_page_64 / pml4_set_entry_64 / pt_set_entry_64 ... 只有
// **声明**没有实现（恒等映射时代留下的接口），引用它们会链接失败；所以这里自己走四级表。
// 恒等映射：页池地址（物理）可以直接解引用（< 4GB，PDPTE[0..3] 的 2MB 大页映射）。
static const uint64_t U64_PS = 0x80;      // PDPTE/PDE 的 PS 位（大页）
static const uint64_t U64_TBL_FLAGS = PTE_PRESENT_64 | PTE_WRITE_64 | PTE_USER_64;  // 中间层统一 P|RW|US

static inline uint64_t u64_rd_cr364() {
    uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v;
}
static inline void u64_flush_tlb64() {                       // 重载 CR3：清 TLB + 分页结构缓存
    __asm__ volatile("mov %0, %%cr3" : : "r"(u64_rd_cr364()) : "memory");
}
static inline void u64_invlpg64(uint64_t va) {
    __asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}
static inline uint64_t* u64_tbl64(uint64_t phys) { return (uint64_t*)(uintptr_t)phys; }
static inline void u64_zero_page64(void* p) {
    uint8_t* b = (uint8_t*)p;
    for (uint32_t i = 0; i < PAGE_SIZE_64; i++) b[i] = 0;
}

// 走到 va 的叶子 PTE。alloc=1 时按需分配缺失的 PD/PT（页表页**不回收**，见 run_blob 收尾说明）。
// 返回 nullptr：PML4 项不存在 / 撞上大页 / 需要分配但页池空了。
static uint64_t* u64_walk64(uint64_t va, int alloc) {
    uint64_t* pml4 = u64_tbl64(u64_rd_cr364() & ~0xFFFULL);
    const uint64_t i4 = (va >> 39) & 0x1FF, i3 = (va >> 30) & 0x1FF,
                   i2 = (va >> 21) & 0x1FF, i1 = (va >> 12) & 0x1FF;

    uint64_t e = pml4[i4];
    if (!(e & PTE_PRESENT_64)) return nullptr;
    // ★ 顶层打开 U/S：低 4GB 的 PDPTE 仍是 U/S=0，所以不扩大 ring3 的可达范围
    if ((e & (PTE_WRITE_64 | PTE_USER_64)) != (PTE_WRITE_64 | PTE_USER_64)) {
        pml4[i4] = e | PTE_WRITE_64 | PTE_USER_64;
        u64_flush_tlb64();
    }
    uint64_t* pdpt = u64_tbl64(pml4[i4] & ~0xFFFULL);
    e = pdpt[i3];
    if (e & PTE_PRESENT_64) {
        if (e & U64_PS) return nullptr;                  // 1GiB 大页：用户窗口不该出现
        if ((e & (PTE_WRITE_64 | PTE_USER_64)) != (PTE_WRITE_64 | PTE_USER_64)) pdpt[i3] = e | PTE_WRITE_64 | PTE_USER_64;
    } else {
        if (!alloc) return nullptr;
        void* np = page_alloc_64();
        if (!np) return nullptr;
        u64_zero_page64(np);
        pdpt[i3] = (uint64_t)(uintptr_t)np | U64_TBL_FLAGS;
    }
    uint64_t* pd = u64_tbl64(pdpt[i3] & ~0xFFFULL);
    e = pd[i2];
    if (e & PTE_PRESENT_64) {
        if (e & U64_PS) return nullptr;                  // 2MB 大页：同上
        if ((e & (PTE_WRITE_64 | PTE_USER_64)) != (PTE_WRITE_64 | PTE_USER_64)) pd[i2] = e | PTE_WRITE_64 | PTE_USER_64;
    } else {
        if (!alloc) return nullptr;
        void* np = page_alloc_64();
        if (!np) return nullptr;
        u64_zero_page64(np);
        pd[i2] = (uint64_t)(uintptr_t)np | U64_TBL_FLAGS;
    }
    uint64_t* pt = u64_tbl64(pd[i2] & ~0xFFFULL);
    return &pt[i1];
}

// ==================== 给 syscall64 / elf64 用的页级原语 ====================
// 说明见 usermode64.h。这里只是把上面的静态页表助手包一层，保证"用户窗口/权限位"只有一套实现。
//
// ★ 标志位掩码（踩过的坑，别改回去）：PTE 的**低 12 位就是权限位**（P/W/U/PWT/PCD/A/D/PAT/G），
//   高位里只有 bit63(NX) 是权限。所以"保留权限"= (f & 0xFFF) | (f & PTE_NX)，而
//   (f & ~0xFFF) 拿到的是**物理地址位**——曾经就是这里写成了 & ~0xFFF，结果 P|U 全被抹掉，
//   用户页变成内核页，进 ring3 前 user64_page_is_user64() 立刻判"没映射成用户页"。
static inline uint64_t u64_leaf_keep64(uint64_t leaf_flags) {
    return (leaf_flags & 0xFFFULL) | (leaf_flags & PTE_NX_64);
}

int user64_map_page64(uint64_t va, uint64_t leaf_flags, int alloc, uint64_t* out_phys) {
    if (va & 0xFFFULL) return 0;                                          // 必须页对齐
    if (va < USER64_CODE_VA64 || va >= USER64_CODE_VA64 + USER64_WINDOW_BYTES64) return 0;
    uint64_t* pte = u64_walk64(va, alloc ? 1 : 0);
    if (!pte) return 0;
    const uint64_t keep = u64_leaf_keep64(leaf_flags);
    if (*pte & PTE_PRESENT_64) {                                          // 已映射：复用物理页，只改权限
        const uint64_t phys = *pte & ~0xFFFULL;
        *pte = phys | keep | PTE_PRESENT_64;
        if (out_phys) *out_phys = phys;
        return 1;
    }
    if (!alloc) return 0;
    void* p = page_alloc_64();
    if (!p) return 0;
    u64_zero_page64(p);                                                   // 新页清零（ELF 的 .bss/栈都靠这条）
    *pte = ((uint64_t)(uintptr_t)p & ~0xFFFULL) | keep | PTE_PRESENT_64;
    if (out_phys) *out_phys = (uint64_t)(uintptr_t)p;
    return 1;
}

int user64_remap_flags64(uint64_t va, uint64_t leaf_flags) {
    uint64_t* pte = u64_walk64(va, 0);
    if (!pte || !(*pte & PTE_PRESENT_64)) return 0;
    *pte = (*pte & ~0xFFFULL) | u64_leaf_keep64(leaf_flags) | PTE_PRESENT_64;
    return 1;
}

uint64_t user64_unmap_page64(uint64_t va) {
    uint64_t* pte = u64_walk64(va, 0);
    if (!pte || !(*pte & PTE_PRESENT_64)) return 0;
    // ★ 只取物理地址位（bit12..51）：不能用 ~0xFFFULL —— 它会留下 bit63(NX)，
    //   于是调用方拿到一个"非规范地址"再去 page_free_64，等于把野指针交给页池
    //   （栈/mmap/brk 页都带 NX，这条路径每次都会踩）。
    const uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
    *pte = 0;
    u64_invlpg64(va);
    return phys;
}

void user64_paging_sync64() { u64_flush_tlb64(); }

// ---- 进 ring3 前的内核栈：TSS.rsp0（ring3 中断/异常）与 SYSCALL 入口栈 ----
// 两者不能共用同一块内存：中断帧压在 [rsp0-0xD0, rsp0)，而 SYSCALL 入口也自己压 0xD0 的帧，
// 一旦共用就会互相覆盖 rip/cs/rsp 槽（既有模块为此开过一个全局专用栈）。
// 批次 C 起每个任务都有**自己**的 syscall 栈（= 自己内核栈顶下方 4KB，见 task64.cpp），
// 因为多进程下每个任务都可能阻塞在系统调用里（例如父进程在 wait4 里等子进程）。
static void u64_set_kernel_stack64() {
    uint64_t rsp0 = 0x80000;                                             // 兜底：与 tss_init64/task64 任务 0 一致
    if (task_kstack_top_current64) rsp0 = task_kstack_top_current64();
    tss_set_rsp0(rsp0);
    if (task_syscall_stack_top64) {
        const uint64_t sk = task_syscall_stack_top64();
        if (sk) g_syscall64_kstack64 = sk;                                // SYSCALL 入口的切栈目标
    }
}

static bool u64_map64(uint64_t va, uint64_t phys, uint64_t leaf_flags) {
    uint64_t* pte = u64_walk64(va, 1);
    if (!pte) return false;
    *pte = (phys & ~0xFFFULL) | leaf_flags | PTE_PRESENT_64;
    return true;
}

static void u64_unmap64(uint64_t va) {
    uint64_t* pte = u64_walk64(va, 0);
    if (!pte) return;
    *pte = 0;
    u64_invlpg64(va);
}

// 该 VA 是否是"已映射且 ring3 可访问"的用户页（四级都要 present + U/S，且不能是大页）
static bool u64_page_is_user64(uint64_t va) {
    const uint64_t* pml4 = u64_tbl64(u64_rd_cr364() & ~0xFFFULL);
    uint64_t e = pml4[(va >> 39) & 0x1FF];
    if (!(e & PTE_PRESENT_64) || !(e & PTE_USER_64)) return false;
    const uint64_t* pdpt = u64_tbl64(e & ~0xFFFULL);
    e = pdpt[(va >> 30) & 0x1FF];
    if (!(e & PTE_PRESENT_64) || !(e & PTE_USER_64) || (e & U64_PS)) return false;
    const uint64_t* pd = u64_tbl64(e & ~0xFFFULL);
    e = pd[(va >> 21) & 0x1FF];
    if (!(e & PTE_PRESENT_64) || !(e & PTE_USER_64) || (e & U64_PS)) return false;
    const uint64_t* pt = u64_tbl64(e & ~0xFFFULL);
    e = pt[(va >> 12) & 0x1FF];
    if (!(e & PTE_PRESENT_64) || !(e & PTE_USER_64)) return false;
    return true;
}

uint64_t user64_page_flags64(uint64_t va) {
    uint64_t* pte = u64_walk64(va, 0);
    if (!pte) return 0;
    // ★ 低 12 位就是权限位（P/W/U/...），bit63 是 NX；中间是物理地址位（不外泄）。
    //   这里踩过一次：写成 & ~0xFFF 会把 P|U 全滤掉，调用方就以为"没映射/权限不对"。
    return u64_leaf_keep64(*pte);
}

int user64_page_is_user_ok64(uint64_t va) { return u64_page_is_user64(va) ? 1 : 0; }

// ==================== EFER.NXE ====================
static void u64_enable_nxe64() {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u));
    if (lo & (1u << 11)) return;                        // EFER.NXE 已开
    lo |= (1u << 11);
    __asm__ volatile("wrmsr" : : "c"(0xC0000080u), "a"(lo), "d"(hi));
    dbg64_line_begin64();
    dbg64_str("[USER64] EFER.NXE enabled (user stack pages use PTE.NX)\n");
    dbg64_line_end64();
}

// ==================== 用户态 iretq 帧 ====================
// 帧格式 = isr_stubs64.asm 的 0xD0 布局（逐字段对齐，改布局必须同步这里）
static void u64_build_user_frame64(pt_regs64* f, uint64_t entry, uint64_t rsp) {
    uint8_t* p = (uint8_t*)f;
    for (uint32_t i = 0; i < PR64_SIZE; i++) p[i] = 0;
    f->gs = SEL64_UDATA; f->fs = SEL64_UDATA; f->es = SEL64_UDATA; f->ds = SEL64_UDATA;  // 占位：出口跳过
    f->rip    = entry;
    f->cs     = SEL64_UCODE;      // 0x2B：iretq 弹出后 CPL=3（描述符 DPL=3）
    f->rflags = 0x202;            // IF=1：用户程序可被 PIT 抢占；IOPL=0（in/out 会 #GP）
    f->rsp    = rsp;
    f->ss     = SEL64_UDATA;      // 0x23：特权级切换时 iretq 真的会弹它并加载
}

// ==================== 范围校验（syscall64 的安全闸门）====================
int user64_range_ok64(uint64_t va, uint64_t len) {
    if (len == 0) return 0;
    const uint64_t base  = USER64_CODE_VA64;
    const uint64_t limit = USER64_CODE_VA64 + USER64_WINDOW_BYTES64;
    if (va < base || va >= limit) return 0;
    if (len > USER64_WINDOW_BYTES64) return 0;              // 先挡住溢出（va+len）
    const uint64_t end = va + len;
    if (end > limit) return 0;
    for (uint64_t a = va & ~0xFFFULL; a < end; a += PAGE_SIZE_64) {
        if (!u64_page_is_user64(a)) return 0;               // 未映射/内核页一律拒绝
    }
    return 1;
}

// ==================== exit(2)：改写当前系统调用帧回内核 ====================
// ★ 批次 C：改成**按任务槽位**判"我是不是在 ring3"，并把汇编蹦床用的保存区指针
//   重新指向**当前任务**那一份 —— exit 与进入永远是同一个任务，所以指针必然一致；
//   多进程时这一步是把"退出回到哪个任务的调用点"钉死的唯一依据（见文件头说明）。
int user64_exit_to_kernel64(pt_regs64* r, uint64_t code) {
    if (!r || !u64_in_ring3_get64()) return 0;              // 本任务不在 ring3：按普通返回处理
    u64_ctx_bind64();                                       // 蹦床读的就是它（IF=0，期间不会被换走）
    g_user64_exit_code64 = code;
    r->rip    = (uint64_t)(uintptr_t)user64_resume_tramp64; // 内核蹦床
    r->cs     = SEL64_KCODE;                                // 0x08：同特权级 iretq（只弹 3 个字）
    r->rflags = 0x002;                                      // IF=0：蹦床复位栈后再 sti
    r->rsp    = 0;                                          // 占位（同特权级 iretq 不弹 rsp）
    r->ss     = SEL64_KDATA;                                // 占位
    return 1;
}

// ==================== 进 ring3 跑一个 blob ====================
// 失败路径（页池耗尽等）只打印并返回 -1：此时最多泄漏已分配的几个 4KB 页（本函数
// 只跑启动期一次演示），不值得为它把回收逻辑写得更容易出错。
int user64_run_blob64(const void* blob, uint32_t size, const char* name) {
    static bool busy = false;
    if (busy) {
        dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (busy)\n"); dbg64_line_end64();
        return -1;
    }
    if (!blob || size == 0) {
        dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (bad blob)\n"); dbg64_line_end64();
        return -1;
    }

    const uint32_t code_pages  = (size + (uint32_t)PAGE_SIZE_64 - 1) / (uint32_t)PAGE_SIZE_64;
    const uint32_t stack_pages = (uint32_t)(USER64_STACK_BYTES64 / PAGE_SIZE_64);
    if (code_pages == 0 || code_pages > 8) {
        dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (blob too big)\n"); dbg64_line_end64();
        return -1;
    }
    busy = true;

    u64_enable_nxe64();

    uint64_t phys_pages[16];
    uint32_t phys_n = 0;
    const uint8_t* src = (const uint8_t*)blob;

    // ---- 代码页：拷 blob -> 映射 用户可读可执行（不可写，无 NX）----
    for (uint32_t i = 0; i < code_pages; i++) {
        void* p = page_alloc_64();
        if (!p) {
            dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (no page)\n"); dbg64_line_end64();
            busy = false; return -1;
        }
        uint8_t* dst = (uint8_t*)p;
        const uint32_t off = i * (uint32_t)PAGE_SIZE_64;
        uint32_t n = size - off;
        if (n > PAGE_SIZE_64) n = PAGE_SIZE_64;
        for (uint32_t k = 0; k < PAGE_SIZE_64; k++) dst[k] = (k < n) ? src[off + k] : 0;
        phys_pages[phys_n++] = (uint64_t)(uintptr_t)p;
        if (!u64_map64(USER64_CODE_VA64 + i * PAGE_SIZE_64, (uint64_t)(uintptr_t)p, PTE_USER_64)) {
            dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (map code)\n"); dbg64_line_end64();
            busy = false; return -1;
        }
    }

    // ---- 栈页：零页 -> 映射 用户可读写 + NX（数据页不可执行）----
    for (uint32_t i = 0; i < stack_pages; i++) {
        void* p = page_alloc_64();
        if (!p) {
            dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (no page)\n"); dbg64_line_end64();
            busy = false; return -1;
        }
        u64_zero_page64(p);
        phys_pages[phys_n++] = (uint64_t)(uintptr_t)p;
        if (!u64_map64(USER64_STACK_VA64 + i * PAGE_SIZE_64, (uint64_t)(uintptr_t)p,
                       PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64)) {
            dbg64_line_begin64(); dbg64_str("[USER64] run FAILED (map stack)\n"); dbg64_line_end64();
            busy = false; return -1;
        }
    }

    u64_flush_tlb64();      // 把新页表项投递到 TLB/分页结构缓存（新 VA 本无 TLB 项，重载最稳）

    // ---- 进 ring3 前把 TSS.rsp0（以及 syscall 指令路径用的镜像）指到当前任务内核栈顶 ----
    // ring3 的中断靠 TSS.rsp0 切栈；SYSCALL 不换栈，靠 g_syscall64_kstack64 切栈。
    u64_set_kernel_stack64();

    const uint64_t user_rsp = USER64_STACK_VA64 + USER64_STACK_BYTES64 - 16;   // 16 字节对齐的栈顶
    dbg64_line_begin64();
    dbg64_str("[USER64] map code=");
    dbg64_hex64(USER64_CODE_VA64);
    dbg64_str(" stack=");
    dbg64_hex64(USER64_STACK_VA64);
    dbg64_str(" code_pages=");
    dbg64_dec((uint64_t)code_pages);
    dbg64_str(" stack_pages=");
    dbg64_dec((uint64_t)stack_pages);
    dbg64_str(" name=");
    dbg64_str(name ? name : "?");
    dbg64_nl();
    dbg64_line_end64();

    u64_build_user_frame64(&g_user64_frame64, USER64_CODE_VA64, user_rsp);
    dbg64_line_begin64();
    dbg64_str("[USER64] enter ring3 entry=");
    dbg64_hex64(USER64_CODE_VA64);
    dbg64_str(" rsp=");
    dbg64_hex64(user_rsp);
    dbg64_nl();
    dbg64_line_end64();

    g_user64_exit_code64 = 0;
    u64_ctx_bind64();                       // ★ 保存区指向**本任务**那一份（多进程关键）
    u64_in_ring3_set64(1);
    // ★ 从这里进入 ring3；用户程序 exit(2) 时由被改写的 syscall 帧回到下面这行之后。
    //   返回值 = 用户 exit 的退出码（汇编蹦床把 code 放在 rax 里返回）——不读全局量，
    //   因为 sti 之后本任务可能被抢占、另一个进程的 exit 会覆盖全局量。
    const uint64_t u64_rc = user64_enter64((uint64_t)(uintptr_t)&g_user64_frame64);
    (void)u64_rc;
    u64_in_ring3_set64(0);

    // ---- 收尾：清叶子 PTE（对应 VA 不会再被访问）+ 释放用户数据页 ----
    // 中间页表页（PDPTE/PD/PT）**保留不回收**：一是它们只有 8~12KB、且是"用户窗口"的
    // 一次性基础设施；二是立即释放会让页表里留下指向已释放页的悬空项（下次映射到同一
    // VA 时可能读到被重分配的垃圾），保留最稳。
    for (uint32_t i = 0; i < code_pages; i++)  u64_unmap64(USER64_CODE_VA64 + i * PAGE_SIZE_64);
    for (uint32_t i = 0; i < stack_pages; i++) u64_unmap64(USER64_STACK_VA64 + i * PAGE_SIZE_64);
    for (uint32_t i = 0; i < phys_n; i++) page_free_64((void*)(uintptr_t)phys_pages[i]);

    dbg64_line_begin64();
    dbg64_str("[USER64] back to kernel (ring0)");
    dbg64_nl();
    dbg64_str("[USER64] demo done name=");
    dbg64_str(name ? name : "?");
    dbg64_str(" rc=");
    dbg64_dec(g_user64_exit_code64);
    dbg64_nl();
    dbg64_line_end64();

    busy = false;
    return 0;
}

// ==================== 从 ELF 入口进 ring3 ====================
// 与 user64_run_blob64 的分工：这里**不映射、不回收任何页** —— 段与初始栈由 ELF64 加载器
// （kernel/elf64.cpp）自己在用户窗口里建好，本函数只负责"把 TSS.rsp0/内核栈切好、抬栈
// iretq 进 ring3、exit 后回到这里"。所以它返回的是**用户 exit 的退出码**（不是 0/-1）。
int user64_enter_at64(uint64_t entry, uint64_t user_rsp, const char* name) {
    // ★ 批次 C：不再用"全局 busy"判断（多进程下允许多个用户任务同时处在 ring3，
    //   各自被 PIT 抢占）。真正的互斥单位是**任务**：同一个任务不可能两次进 ring3
    //   （进 ring3 后只能通过 exit 回来，中途没有"再次进入"的路径），所以这里只做
    //   "本任务是否已经在 ring3"的自检。
    if (u64_in_ring3_get64()) {
        dbg64_line_begin64();
        dbg64_str("[USER64] enter FAILED (this task already in ring3)");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    const uint64_t base = USER64_CODE_VA64, limit = base + USER64_WINDOW_BYTES64;
    if (entry < base || entry >= limit) {                     // 入口必须在用户窗口内
        dbg64_line_begin64(); dbg64_str("[USER64] enter FAILED (entry outside window)"); dbg64_nl(); dbg64_line_end64();
        return -1;
    }
    if (user_rsp <= base || user_rsp > limit) {
        dbg64_line_begin64(); dbg64_str("[USER64] enter FAILED (rsp outside window)"); dbg64_nl(); dbg64_line_end64();
        return -1;
    }
    if (!u64_page_is_user64(entry & ~0xFFFULL)) {             // 入口那一页必须已映射为用户页
        dbg64_line_begin64(); dbg64_str("[USER64] enter FAILED (entry page not mapped user)"); dbg64_nl(); dbg64_line_end64();
        return -1;
    }

    u64_enable_nxe64();
    u64_set_kernel_stack64();

    u64_build_user_frame64(&g_user64_frame64, entry, user_rsp);
    dbg64_line_begin64();
    dbg64_str("[USER64] enter ring3 entry=");
    dbg64_hex64(entry);
    dbg64_str(" rsp=");
    dbg64_hex64(user_rsp);
    dbg64_str(" name=");
    dbg64_str(name ? name : "?");
    dbg64_nl();
    dbg64_line_end64();

    g_user64_exit_code64 = 0;
    u64_ctx_bind64();                       // ★ 保存区指向本任务那一份
    u64_in_ring3_set64(1);
    // ★ 进入 ring3；exit（int 0x80 或 syscall 指令）后由内核蹦床回到下面这行之后。
    const uint64_t rc = user64_enter64((uint64_t)(uintptr_t)&g_user64_frame64);
    u64_in_ring3_set64(0);

    const uint32_t code = (uint32_t)(rc & 0xFF);
    dbg64_line_begin64();
    dbg64_str("[USER64] back to kernel (ring0) exit_code=");
    dbg64_dec((uint64_t)code);
    dbg64_nl();
    dbg64_line_end64();
    return (int)code;
}

// ==================== 从一份"现成的用户帧"进 ring3（批次 C：fork 的子进程／execve）====================
// 与 user64_enter_at64 的区别：入口/栈/GPR 全部来自调用方给的帧（fork 复制的是**父进程的
// syscall 帧** —— 父子除了 rax 之外寄存器完全相同，这正是 Linux fork 的语义），
// 本函数只做安全校验 + 抬栈 iretq。返回用户 exit 的退出码。
int user64_enter_frame64(const pt_regs64* frame, const char* name) {
    // 每一处失败都留一个原因码（多进程后"进不去 ring3"必须能一眼定位到是哪一条校验）
    uint32_t why = 0;
    if (!frame) why = 1;
    else if (u64_in_ring3_get64()) why = 2;
    else if (frame->cs != SEL64_UCODE || frame->ss != SEL64_UDATA) why = 3;
    else if (frame->rip < USER64_CODE_VA64 || frame->rip >= USER64_CODE_VA64 + USER64_WINDOW_BYTES64) why = 4;
    else if (frame->rsp <= USER64_CODE_VA64 || frame->rsp > USER64_CODE_VA64 + USER64_WINDOW_BYTES64) why = 5;
    else if (!u64_page_is_user64(frame->rip & ~0xFFFULL)) why = 6;
    else if (!u64_page_is_user64((frame->rsp - 1) & ~0xFFFULL)) why = 7;
    if (why) {
        dbg64_line_begin64();
        dbg64_str("[USER64] enter FAILED (frame) reason=");
        dbg64_dec((uint64_t)why);
        dbg64_str(" name=");
        dbg64_str(name ? name : "?");
        dbg64_str(" cr3=0x");
        dbg64_hex64(u64_rd_cr364());
        if (frame) {
            dbg64_str(" rip=0x");
            dbg64_hex64(frame->rip);
            dbg64_str(" rsp=0x");
            dbg64_hex64(frame->rsp);
            dbg64_str(" cs=0x");
            dbg64_hex64(frame->cs);
            dbg64_str(" ss=0x");
            dbg64_hex64(frame->ss);
        }
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }

    // 帧原样拷进静态缓冲：iretq 立刻消费它（IF=0 且不返回），多任务共用也安全。
    g_user64_frame64 = *frame;
    g_user64_frame64.gs = SEL64_UDATA; g_user64_frame64.fs = SEL64_UDATA;
    g_user64_frame64.es = SEL64_UDATA; g_user64_frame64.ds = SEL64_UDATA;  // 占位：出口跳过
    if ((g_user64_frame64.rflags & 0x202ULL) != 0x202ULL) g_user64_frame64.rflags = 0x202;

    u64_enable_nxe64();
    u64_set_kernel_stack64();

    dbg64_line_begin64();
    dbg64_str("[USER64] enter ring3 entry=");
    dbg64_hex64(g_user64_frame64.rip);
    dbg64_str(" rsp=");
    dbg64_hex64(g_user64_frame64.rsp);
    dbg64_str(" name=");
    dbg64_str(name ? name : "?");
    dbg64_str(" frame=1");
    dbg64_nl();
    dbg64_line_end64();

    g_user64_exit_code64 = 0;
    u64_ctx_bind64();
    u64_in_ring3_set64(1);
    const uint64_t rc = user64_enter64((uint64_t)(uintptr_t)&g_user64_frame64);
    u64_in_ring3_set64(0);

    dbg64_line_begin64();
    dbg64_str("[USER64] back to kernel (ring0) exit_code=");
    dbg64_dec(rc & 0xFFULL);
    dbg64_str(" frame=1");
    dbg64_nl();
    dbg64_line_end64();
    return (int)(rc & 0xFFULL);
}

// ==================== 自检 ====================
// 位含义：bit0 用户页映射 + VA 读写 + 权限位、bit1 iretq 帧字段、bit2 选择子/GDT 现场、
//         bit3 范围校验（正例/负例）、bit4 页分配回收往返
int user64_selftest64() {
    int fail = 0;

    u64_enable_nxe64();

    // ---- bit0/bit3：映射一个测试页 -> 通过 VA 写读 -> 权限位/范围校验正确 ----
    {
        void* p = page_alloc_64();
        if (!p) {
            fail |= 1 | 16;
        } else {
            u64_zero_page64(p);
            const uint64_t testva = USER64_TEST_VA64;
            if (!u64_map64(testva, (uint64_t)(uintptr_t)p, PTE_USER_64 | PTE_WRITE_64)) {
                fail |= 1;
            } else {
                u64_flush_tlb64();
                volatile uint64_t* q = (volatile uint64_t*)(uintptr_t)testva;
                const uint64_t pat = 0x5A5A1234DEADBEEFULL;
                q[0] = pat;
                if (q[0] != pat) fail |= 1;                          // 映射后的 VA 能读到写进的内容
                if (q[3] != 0) fail |= 1;                             // 新页是 0（顺便读别的偏移）

                // 叶子权限位：P|W|U，且非 NX（测试页当数据页）
                uint64_t* pte = u64_walk64(testva, 0);
                if (!pte || ((*pte & (PTE_PRESENT_64 | PTE_WRITE_64 | PTE_USER_64)) !=
                             (PTE_PRESENT_64 | PTE_WRITE_64 | PTE_USER_64))) fail |= 1;

                // 范围校验：已映射的页必须放行；窗口内未映射地址必须拒绝
                if (!user64_range_ok64(testva, 8)) fail |= 8;
                if (user64_range_ok64(testva + 0x20000, 8)) fail |= 8;
            }
            u64_unmap64(testva);
            page_free_64(p);
        }
    }

    // ---- bit1：用户态 iretq 帧逐字段（用静态帧缓冲，不动真帧）----
    {
        static pt_regs64 f;
        u64_build_user_frame64(&f, 0x1111222233334444ULL, 0x5555666677778888ULL);
        if (PR64_SIZE != 0xD0) fail |= 2;
        if (f.rip != 0x1111222233334444ULL) fail |= 2;
        if (f.rsp != 0x5555666677778888ULL) fail |= 2;
        if (f.cs != 0x2B || f.ss != 0x23) fail |= 2;
        if (f.rflags != 0x202) fail |= 2;
        if (f.gs != 0x23 || f.fs != 0x23 || f.es != 0x23 || f.ds != 0x23) fail |= 2;
        if (f.int_no != 0 || f.err_code != 0) fail |= 2;
    }

    // ---- bit2：选择子数值 + GDT 现场（内核选择子必须仍是 08/10）----
    {
        if (SEL64_KCODE != 0x08 || SEL64_KDATA != 0x10) fail |= 4;
        if (SEL64_UCODE != 0x2B || SEL64_UDATA != 0x23) fail |= 4;
        if ((SEL64_UCODE >> 3) != 5 || (SEL64_UDATA >> 3) != 4) fail |= 4;   // sysret 顺序约束
        if ((SEL64_UCODE & 3) != 3 || (SEL64_UDATA & 3) != 3) fail |= 4;     // RPL=3
        // ★ 比较前屏蔽掉 **CPU 自己会置的位**：加载段时置"访问位 A"（type bit0），
        //   ltr 会把 TSS 类型从 0x9（可用）置成 0xB（忙）。屏蔽后才是描述符的静态编码。
        if (((gdt_entry_raw64(1) >> 40) & 0xFE) != 0x9A) fail |= 4;          // 内核代码 DPL0
        if (((gdt_entry_raw64(2) >> 40) & 0xFE) != 0x92) fail |= 4;          // 内核数据 DPL0
        if (((gdt_entry_raw64(4) >> 40) & 0xFE) != 0xF2) fail |= 4;          // 用户数据 DPL3
        if (((gdt_entry_raw64(5) >> 40) & 0xFE) != 0xFA) fail |= 4;          // 用户代码 DPL3
        if (((gdt_entry_raw64(5) >> 48) & 0x20) != 0x20) fail |= 4;          // 代码段 L=1（64 位）
        if (((gdt_entry_raw64(6) >> 40) & 0xFD) != 0x89) fail |= 4;          // TSS 已挪到 index 6
        uint16_t cs; __asm__ volatile("mov %%cs, %0" : "=r"(cs));
        if ((cs & 0xFFFF) != 0x08) fail |= 4;                                // 当前内核 CS 没变
    }

    // ---- bit3：范围校验负例（绝不能放行内核地址/窗口外/溢出）----
    {
        if (user64_range_ok64(0x100000, 16)) fail |= 8;                              // 内核镜像
        if (user64_range_ok64(0xFFFFFFFF80100000ULL, 16)) fail |= 8;                 // 内核高半区
        if (user64_range_ok64(USER64_CODE_VA64 - 1, 2)) fail |= 8;                   // 窗口下界前
        if (user64_range_ok64(USER64_CODE_VA64 + USER64_WINDOW_BYTES64, 8)) fail |= 8;// 窗口上界外
        if (user64_range_ok64(USER64_CODE_VA64 + USER64_WINDOW_BYTES64 - 1, 4096)) fail |= 8; // 跨上界
        if (user64_range_ok64(USER64_CODE_VA64, 0)) fail |= 8;                       // len=0
        if (user64_range_ok64(0xFFFFFFFFFFFFFFFFULL, 4096)) fail |= 8;               // 溢出
    }

    // ---- bit4：页分配/回收往返 ----
    {
        void* a = page_alloc_64();
        void* b = page_alloc_64();
        if (!a || !b || a == b) fail |= 16;
        if (a) page_free_64(a);
        if (b) page_free_64(b);
    }

    if (fail == 0) {
        dbg64_line_begin64();
        dbg64_str("[USER64] selftest PASS\n");
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[USER64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
        dbg64_line_end64();
    }
    return fail;
}
