// iso64.cpp - ★ v0.11.8：Ring 3 真隔离层（硬件隔离位 + 地址空间审计 + 自检）
//   设计说明、现状事实、没做的那层都写在 iso64.h 顶部；这里只写实现。
//
// 三条硬约束（改之前先读）：
//   1) 本文件**只读页表、只改 CR0/CR4 的两位**，绝不动别人的映射；审计发现违规只打点，不改映射
//      （悄悄"修好"等于把证据擦掉）。
//   2) 走页表用的是**恒等映射直接解引用物理地址**（与 proc64.cpp 的 p64_phys64 同一条假设：
//      前 4GB 恒等映射常驻，页表页永远落在这里）。
//   3) 打点里的数字必须是**数出来的**（present 项数），不是常量：ok=1 时 khigh/ident/uwin 一定是正数，
//      否则说明这次审计根本没走到页表（那时 ok=0）。
#include "iso64.h"
#include "x86_64.h"
#include "mem_64.h"
#include "debug64.h"
#include "task64.h"        // task64_kernel_cr364
#include "proc64.h"        // 进程表快照（pid + cr3）
#include "hwinfo64.h"      // hw_info64()->smep（CPU 支持与否）
#include "kpti64.h"        // ★ v0.11.9：KPTI 状态（审计要认共享入口区、要判用户表）
#include "sig64.h"         // ★ v0.12.0：SMAP 违规 = 只杀当前进程（SIG64_SEGV），不 PANIC 整机

// ==================== 小工具 ====================
static inline uint64_t iso_rd_cr0_64() { uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v; }
static inline uint64_t iso_rd_cr4_64() { uint64_t v; __asm__ volatile("mov %%cr4, %0" : "=r"(v)); return v; }
static inline void iso_wr_cr0_64(uint64_t v) { __asm__ volatile("mov %0, %%cr0" :: "r"(v) : "memory"); }
static inline void iso_wr_cr4_64(uint64_t v) { __asm__ volatile("mov %0, %%cr4" :: "r"(v) : "memory"); }
static inline uint64_t* iso_phys64(uint64_t pa) { return (uint64_t*)(uintptr_t)pa; }   // 恒等映射
static inline uint64_t iso_page64(uint64_t v) { return v & 0x000FFFFFFFFFF000ULL; }
static inline int iso_present64(uint64_t e) { return (e & PTE_PRESENT_64) ? 1 : 0; }
static inline int iso_user64(uint64_t e) { return (e & PTE_USER_64) ? 1 : 0; }

#define ISO_CR0_WP   (1ULL << 16)
#define ISO_CR4_SMEP (1ULL << 20)
#define ISO_CR4_SMAP (1ULL << 21)

static int g_wp = 0, g_smep = 0, g_smep_ok = 0, g_smap_ok = 0;
// ★ v0.12.0：SMAP 运行期状态 + 违规计数（计数是判据：0 才叫"清单清零"）
static int g_smap = 0, g_smap_viol = 0;
static uint32_t g_smap_rearm = 0;
static int g_audit_runs = 0, g_audit_violations = 0;

// CPUID 7.0：EBX bit7 = SMEP、bit20 = SMAP（叶不存在时为 0）
static void iso_cpuid7_64(uint32_t* ebx_out) {
    uint32_t a = 0, b = 0, c = 0, d = 0;
    uint32_t max_leaf = 0, max_leaf_b = 0, max_leaf_c = 0, max_leaf_d = 0;
    __asm__ volatile("cpuid" : "=a"(max_leaf), "=b"(max_leaf_b), "=c"(max_leaf_c), "=d"(max_leaf_d) : "a"(0));
    *ebx_out = 0;
    if (max_leaf < 7) return;
    a = 7; c = 0;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(a), "c"(c));
    *ebx_out = b;
}

int iso64_wp_on64() { return g_wp; }
int iso64_smep_on64() { return g_smep; }
int iso64_smep_supported64() { return g_smep_ok; }
int iso64_smap_supported64() { return g_smap_ok; }

// ==================== 硬件隔离位 ====================
int iso64_init64() {
    // CPUID 探测（只报告，不改状态）
    uint32_t ebx7 = 0;
    iso_cpuid7_64(&ebx7);
    g_smep_ok = ((ebx7 >> 7) & 1u) ? 1 : 0;
    g_smap_ok = ((ebx7 >> 20) & 1u) ? 1 : 0;
    const int hw_smep = (hw_info64() && hw_info64()->cpu.smep) ? 1 : 0;
    if (hw_smep) g_smep_ok = 1;      // 与 hwinfo 结论取并（hwinfo 在 CPL0 早期也算过一遍）

    // ① CR0.WP：内核也不许写只读页
    uint64_t cr0 = iso_rd_cr0_64();
    if (!(cr0 & ISO_CR0_WP)) {
        iso_wr_cr0_64(cr0 | ISO_CR0_WP);
        cr0 = iso_rd_cr0_64();
    }
    g_wp = (cr0 & ISO_CR0_WP) ? 1 : 0;

    // ② CR4.SMEP：内核不许执行用户页（CPU 支持才开；开了立刻回读确认）
    uint64_t cr4 = iso_rd_cr4_64();
    if (g_smep_ok && !(cr4 & ISO_CR4_SMEP)) {
        iso_wr_cr4_64(cr4 | ISO_CR4_SMEP);
        cr4 = iso_rd_cr4_64();
    }
    g_smep = (cr4 & ISO_CR4_SMEP) ? 1 : 0;

    // ③ ★ v0.12.0：CR4.SMAP —— 内核**读写**用户页也要过闸门（CPU 支持才开；开了立刻回读确认）。
    //   打开之后内核里每一处"确实要碰用户内存"的点都必须自己开窗（iso64_stac64/clac64），
    //   漏掉一处就 #PF —— 那正好是判据（见 iso64_smap_fault64：只杀进程，不 PANIC 整机）。
    if (g_smap_ok && !(cr4 & ISO_CR4_SMAP)) {
        iso_wr_cr4_64(cr4 | ISO_CR4_SMAP);
        cr4 = iso_rd_cr4_64();
    }
    g_smap = (cr4 & ISO_CR4_SMAP) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[ISO64] hw wp=");      dbg64_dec((uint64_t)g_wp);
    dbg64_str(" smep=");              dbg64_dec((uint64_t)g_smep);
    dbg64_str(" smep_supported=");    dbg64_dec((uint64_t)g_smep_ok);
    dbg64_str(" smap_supported=");    dbg64_dec((uint64_t)g_smap_ok);
    dbg64_str(" smap_on=");           dbg64_dec((uint64_t)g_smap);
    dbg64_str(" cr0=");               dbg64_hex64(cr0);
    dbg64_str(" cr4=");               dbg64_hex64(cr4);
    dbg64_str(g_wp ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();

    // ★ v0.11.9：KPTI 到底开没开，如实打（不再是写死的 kpti=0）
    dbg64_line_begin64();
    dbg64_str("[ISO64] mode kpti=");
    dbg64_dec((uint64_t)kpti64_on64());
    dbg64_str(" smap=");
    dbg64_dec((uint64_t)g_smap);
    dbg64_str(" reason=");
    if (kpti64_on64())
        dbg64_str("user-space-has-no-kernel-half (user table PML4[511]=0; entry trampolines at 6 GiB)");
    else
        dbg64_str(kpti64_reason64());
    dbg64_nl();
    dbg64_line_end64();
    return g_wp;
}

// 把 CR0.WP 重新置位（幂等）。返回 1 = 之前确实掉了、这次补上了（**这是真发现，要打点**）。
int iso64_reassert64() {
    const uint64_t cr0 = iso_rd_cr0_64();
    if (cr0 & ISO_CR0_WP) { g_wp = 1; return 0; }
    iso_wr_cr0_64(cr0 | ISO_CR0_WP);
    g_wp = (iso_rd_cr0_64() & ISO_CR0_WP) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[ISO64] wp reassert was=0 now=");
    dbg64_dec((uint64_t)g_wp);
    dbg64_str(" cr0_was="); dbg64_hex64(cr0);
    dbg64_nl();
    dbg64_line_end64();
    return g_wp;
}

// ==================== ★ v0.12.0：WP 现场检查（判据）====================
int iso64_check_wp64(const char* where) {
    const uint64_t cr0 = iso_rd_cr0_64();
    const int on = (cr0 & ISO_CR0_WP) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[ISO64] wp check at ");
    dbg64_str(where ? where : "?");
    dbg64_str(" wp=");  dbg64_dec((uint64_t)on);
    dbg64_str(" cr0="); dbg64_hex64(cr0);
    dbg64_nl();
    dbg64_line_end64();
    if (!on) {
        dbg64_line_begin64();
        dbg64_str("[ISO64] wp leak detected at ");
        dbg64_str(where ? where : "?");
        dbg64_str(" (CR0.WP=0) -> reassert (this line is a FAIL by itself)");
        dbg64_nl();
        dbg64_line_end64();
        (void)iso64_reassert64();          // 先保住防护；"掉了"这件事已经记在日志里，不掩盖
    }
    return on;
}

// ==================== ★ v0.12.0：SMAP（内核访问用户页要开窗）====================
// 三条口径（改之前先读）：
//   1) `stac`/`clac` 只有在 CR4.SMAP=1 时才合法（否则 #UD），所以两个包装函数都先看 g_smap；
//      SMAP 没开时它们**编译成一次读全局 + 分支**，语义上是空操作 —— 同一份代码两种 CPU 都对。
//   2) 窗口纪律：`iso64_stac64()` 与 `iso64_clac64()` 之间**只放真正的用户访存**。
//      中间去调内核的其它函数 = 把保护范围扩大到它里面（等于没开）。
//   3) 违规处置**不 PANIC 整机**（isr_handler64 里调 iso64_smap_fault64）：
//        * 产品行为：只杀当前进程（SIGSEGV），内核继续跑；
//        * 探针行为（ISO64_SMAP_PROBE=1，仅开发期）：关掉 SMAP 让那条指令重试，并按 tick 重新打开，
//          于是一次启动就能把"漏了 STAC 的点"成串收下来（每条带 rip -> objdump 定位到源码行）。
// 探针模式**不进交付构建**：交付时必须是 0；发版判据 = 探针模式下跑一遍主回归，violations=0。
static const int      ISO64_SMAP_PROBE         = 0;
static const uint32_t ISO64_SMAP_REARM_TICKS64 = 64;      // 250 Hz 下约 0.26 s

// ★ 嵌套安全：开窗是**引用计数**，不是布尔。理由（本版踩到过）：e64_build_stack64 开了一个大窗，
//   里面调 e64_fill_random64（它自己也开窗）—— 内层的 CLAC 会把外层还开着的窗一起关掉，
//   于是外层剩下的写用户页立刻 #PF（实测 `[SMAP64] violation … e64_build_stack64+0x62`）。
//   计数进内核时清零（iso64_clac_on_entry64），所以跨 syscall 绝不会累积。
static int g_smap_ac_depth = 0;
void iso64_stac64() {
    if (!g_smap) return;
    if (g_smap_ac_depth++ == 0) __asm__ volatile("stac" : : : "memory");
}
void iso64_clac64() {
    if (!g_smap) return;
    if (g_smap_ac_depth > 0 && --g_smap_ac_depth == 0) __asm__ volatile("clac" : : : "memory");
}
// 进内核就 CLAC：AC 是 RFLAGS 里的位，ring3 理论上能把它带进来（Linux 同款处理）。
// 这里不是"防 ring3"（AC 对 ring3 没有 SMAP 含义），而是保证内核侧**永远从 AC=0 开始**、
// 而且开窗计数从 0 开始（不继承上一次开窗留下的状态）。
void iso64_clac_on_entry64() {
    g_smap_ac_depth = 0;
    if (g_smap) __asm__ volatile("clac" : : : "memory");
}

int iso64_smap_on64()         { return g_smap; }
int iso64_smap_violations64() { return g_smap_viol; }


// 内核态 #PF 的 SMAP 违规处置。见 iso64.h 的契约。
int iso64_smap_fault64(void* regs) {
    if (!g_smap || !regs) return 0;
    pt_regs64* r = (pt_regs64*)regs;
    if (r->int_no != 14) return 0;
    if ((r->cs & 3u) == 3u) return 0;                        // ring3 自己的 #PF：走 sig64 那条路
    // 判据：**CPL0 的 #PF + AC=0 + 目标地址确实是一张可访问的用户页**（这就是 SMAP 的三要素）。
    //   ★ 不要用页错误码的 U/S 位来认 SMAP：Intel SDM 与 QEMU 的 TCG 在这里不一致（实测
    //     `-cpu max` 上 SMAP 拦下的写，PFEC 给的是 err=0x3，U/S=0；按 SDM 的说法会是 U/S=1）。
    //     靠 U/S 判定 = 在 QEMU 上一条都认不出来、直接掉进 PANIC（本版就是这么踩到的）。
    //   写访问还要再核一层"这一页本来就可写"：否则那是 CR0.WP 的活（内核写只读页），不是 SMAP 的事。
    if (!(r->err_code & 1u)) return 0;                       // 页不存在：跟 SMAP 无关
    if (r->rflags & (1ull << 18)) return 0;                  // AC=1：在开着的窗口里，不是 SMAP 拦的
    uint64_t cr2 = 0;
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    if (cr2 >= 0x0000800000000000ull) return 0;              // 内核地址：不是 SMAP 的事
    if (user64_range_ok64(cr2, 1) != 1) return 0;            // 不在用户窗口 / 不可用户访问：不是 SMAP 的事
    if ((r->err_code & 2u) && !(user64_page_flags64(cr2) & PTE_WRITE_64)) return 0;   // 只读页写：CR0.WP 的活

    g_smap_viol++;
    const int pid = proc64_current_pid64 ? proc64_current_pid64() : -1;
    dbg64_line_begin64();
    dbg64_str("[SMAP64] violation n=");  dbg64_dec((uint64_t)g_smap_viol);
    dbg64_str(" pid=");                  dbg64_dec((uint64_t)(uint32_t)(pid < 0 ? 0 : pid));
    dbg64_str(" cr2=");                  dbg64_hex64(cr2);
    dbg64_str(" rip=");                  dbg64_hex64(r->rip);
    dbg64_str(" err=");                  dbg64_hex64(r->err_code);
    dbg64_str(ISO64_SMAP_PROBE ? " -> probe: smap off, retry"
                               : " -> sig=11 (kernel touched user page without STAC)");
    dbg64_nl();
    dbg64_line_end64();

    if (ISO64_SMAP_PROBE) {
        // 关掉 SMAP：iretq 之后这条指令原地重试就会成功；tick 钩子过一会儿再打开
        iso_wr_cr4_64(iso_rd_cr4_64() & ~ISO_CR4_SMAP);
        g_smap = 0;
        g_smap_rearm = ISO64_SMAP_REARM_TICKS64;
        return 1;
    }
    // 产品行为（v0.12.0 取舍，如实写在 iso64.h）：**只杀当前进程**（与 ring3 缺页同一套语义），
    //   绝不 PANIC 整机。没有进程上下文时（内核任务 0 上走到的路径，比如自检自己读用户页）
    //   退化为"关掉 SMAP 让指令重试 + 大声留痕"（降级而不是变砖）—— 判据看 [ISO64] status 的
    //   smap= 从 1 变 0、以及 `smap degraded` 那行。这条路径是"内核 bug 的兜底"，不是正常运行路径。
    if (proc64_sig_die_current64 && proc64_sig_state_current64 && proc64_sig_state_current64()) {
        (void)proc64_sig_die_current64(SIG64_SEGV, r);
        return 1;
    }
    iso_wr_cr4_64(iso_rd_cr4_64() & ~ISO_CR4_SMAP);
    g_smap = 0;
    dbg64_line_begin64();
    dbg64_str("[SMAP64] smap degraded (no process context, CR4.SMAP=0 from now on)");
    dbg64_nl();
    dbg64_line_end64();
    return 1;                                                // iretq 后原地重试
}

// PIT 钩子（只给探针模式下"重新武装 SMAP"用；非探针模式是空函数）
void iso64_tick64() {
    if (!ISO64_SMAP_PROBE || g_smap_rearm == 0) return;
    if (--g_smap_rearm) return;
    if (g_smap_ok && !(iso_rd_cr4_64() & ISO_CR4_SMAP)) {
        iso_wr_cr4_64(iso_rd_cr4_64() | ISO_CR4_SMAP);
        g_smap = 1;
    }
}

// ==================== 地址空间审计 ====================
// 走四级：统计五类 present 项 —— 内核高半区 / 前 4GB 恒等映射 / 用户窗口 / 窗口外 / 共享入口区，
// 并逐项校验 U/S 位，任何一条不符都算 us_violation。审计**只读页表、不改映射**
// （悄悄"修好"等于把证据擦掉）。
//
// expect_kern 决定"内核高半区该不该存在"，这是 KPTI 的核心判据：
//   1 = 内核表（必须有 PML4[511]，且链上每一项 U/S=0）
//   0 = 用户表（KPTI 下**必须没有** PML4[511] —— 有一条就说明 KPTI 没做到）
static int iso_audit_body64(uint64_t cr3, int pid, const char* why, int expect_kern) {
    if (cr3 == 0) {                                   // 0 = "当前 CR3"
        uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v));
        cr3 = v;
    }
    const uint64_t pml4_pa = iso_page64(cr3);
    uint64_t* pml4 = iso_phys64(pml4_pa);
    int khigh = 0, ident = 0, uwin = 0, outside = 0, entry = 0, fbwin = 0, viol = 0;

    // ---- ① 内核高半区：PML4[511] → PDPT_hi → PD（可能 1GB 大页、2MB 大页或 PT）----
    const uint64_t e511 = pml4[511];
    if (!expect_kern) {
        // 用户表：这条必须为空（KPTI 的全部意义就在这儿）
        if (iso_present64(e511)) viol++;
    } else if (iso_present64(e511)) {
        khigh++;
        if (iso_user64(e511)) viol++;                     // 内核半区的顶层不许用户可访问
        uint64_t* pdpt_hi = iso_phys64(iso_page64(e511));
        for (int i = 0; i < 512; i++) {
            const uint64_t ep = pdpt_hi[i];
            if (!iso_present64(ep)) continue;
            khigh++;
            if (iso_user64(ep)) viol++;
            if (ep & PTE_1GB_64) continue;                // 1GB 大页：到此为止
            uint64_t* pd = iso_phys64(iso_page64(ep));
            for (int j = 0; j < 512; j++) {
                const uint64_t ed = pd[j];
                if (!iso_present64(ed)) continue;
                khigh++;
                if (iso_user64(ed)) viol++;
            }
        }
    } else {
        viol++;                                           // 内核表里没有内核半区 = 内核自己在跑什么？
    }

    // ---- ①b PML4[255]：KPTI 的"数据槽"（non-present 里存另一张表的物理地址）。
    //   它必须非 present —— 真 present 就等于把 128TB 的窗口开出来了，那是越权。
    if (iso_present64(pml4[255])) viol++;

    // ---- ② 前 4GB 恒等映射：PML4[0] → PDPT，只允许 0..3（每 1GB）----
    const uint64_t e0 = pml4[0];
    if (!iso_present64(e0)) {
        viol++;
    } else {
        uint64_t* pdpt = iso_phys64(iso_page64(e0));
        for (int i = 0; i < 4; i++) {
            const uint64_t ep = pdpt[i];
            if (!iso_present64(ep)) continue;
            ident++;
            if (iso_user64(ep)) viol++;                   // 恒等映射绝不许用户可访问
            if (ep & PTE_1GB_64) continue;
            uint64_t* pd = iso_phys64(iso_page64(ep));
            for (int j = 0; j < 512; j++) {
                const uint64_t ed = pd[j];
                if (!iso_present64(ed)) continue;
                ident++;
                if (iso_user64(ed)) viol++;
            }
        }
        // 窗口外的用户地址（PDPT[5..511]）：只允许两处**已知**映射，其余一律算越权。
        //   ① PDPT[5]（5~6 GiB）= USER64_FB_VA64：framebuffer / DRM / wl surface 窗（v0.9 起的既有设计，
        //      见 kernel/usermode64.h）→ 单独记 fbwin，且顶层必须 U/S=1（ring3 要能画）
        //   ② PDPT[6]（6~7 GiB）= ★ v0.11.9 KPTI 共享入口区（两张表都映射；PD 物理地址
        //      必须等于 kpti64_pd_phys64，且 U/S=0）→ 记 entry
        // 认不出来的 present 项一律 outside++ / viol++（判据不降强度）。
        for (int i = 5; i < 512; i++) {
            const uint64_t ep = pdpt[i];
            if (!iso_present64(ep)) continue;
            if (i == 5 && iso_user64(ep) && !(ep & PTE_1GB_64)) { fbwin++; continue; }
            if (i == 6 && kpti64_pd_phys64() != 0 &&
                iso_page64(ep) == kpti64_pd_phys64() && !iso_user64(ep) && !(ep & 0x80ULL)) {
                entry++;
                continue;
            }
            outside++; viol++;
        }
        // ③ 用户窗口：PDPT[4]（4GiB..5GiB）里的 present 叶子页必须是 U/S=1
        const uint64_t e4 = pdpt[4];
        if (iso_present64(e4)) {
            if (!iso_user64(e4)) viol++;                  // 顶层没打开 U/S = 窗口整段进不去
            if (!(e4 & PTE_1GB_64)) {
                uint64_t* pd = iso_phys64(iso_page64(e4));
                for (int j = 0; j < 512; j++) {
                    const uint64_t ed = pd[j];
                    if (!iso_present64(ed)) continue;
                    if (ed & PTE_2MB_64) {
                        uwin++;
                        if (!iso_user64(ed)) viol++;      // 用户窗口的叶子页必须用户可访问
                        continue;
                    }
                    if (!iso_user64(ed)) viol++;
                    uint64_t* pt = iso_phys64(iso_page64(ed));
                    for (int k = 0; k < 512; k++) {
                        const uint64_t ef = pt[k];
                        if (!iso_present64(ef)) continue;
                        uwin++;
                        if (!iso_user64(ef)) viol++;
                    }
                }
            }
        }
    }

    g_audit_runs++;
    g_audit_violations += viol;
    dbg64_line_begin64();
    dbg64_str("[ISO64] audit pid=");
    if (pid < 0) dbg64_str("-"); else dbg64_dec((uint64_t)pid);
    dbg64_str(" space=");      dbg64_hex64(pml4_pa);
    dbg64_str(" khigh=");      dbg64_dec((uint64_t)khigh);
    dbg64_str(" ident=");      dbg64_dec((uint64_t)ident);
    dbg64_str(" uwin=");       dbg64_dec((uint64_t)uwin);
    dbg64_str(" outside=");    dbg64_dec((uint64_t)outside);
    dbg64_str(" us_violation="); dbg64_dec((uint64_t)viol);
    dbg64_str(" why=");        dbg64_str(why ? why : "-");
    dbg64_str(viol == 0 ? " ok=1" : " ok=0");
    // ★ v0.11.9：末尾追加两个字段（不动前面的顺序，既有验收的 grep 继续成立）
    dbg64_str(" entry=");      dbg64_dec((uint64_t)entry);
    dbg64_str(" fbwin=");      dbg64_dec((uint64_t)fbwin);
    dbg64_str(" kind=");       dbg64_str(expect_kern ? "kern" : "user");
    dbg64_nl();
    dbg64_line_end64();
    return viol;
}

// 内核表（必须有内核高半区）。这是外部唯一入口，语义与 v0.11.8 一致。
int iso64_audit64(uint64_t cr3, int pid, const char* why) {
    return iso_audit_body64(cr3, pid, why, 1);
}
// ★ v0.11.9（KPTI）：用户表（**不许**有内核高半区）。khigh=0 才是对的。
int iso64_audit_user64(uint64_t cr3, int pid, const char* why) {
    return iso_audit_body64(cr3, pid, why, 0);
}

int iso64_audit_all64(const char* why) {
    int total = 0;
    total += iso64_audit64(task64_kernel_cr364(), -1, why ? why : "kernel");
    const int n = proc64_count64();
    int seen = 0;
    for (int i = 0; i < PROC64_MAX; i++) {
        const int pid = proc64_proc_pid_at64(i);
        if (pid < 0) continue;
        Proc64Info info;
        if (proc64_info64(i, &info) == 0) continue;
        if (!info.cr3) continue;
        total += iso64_audit64(info.cr3, pid, why ? why : "proc");
        // ★ v0.11.9（KPTI）：再单独审这张进程的**用户表** —— 判据反过来：
        //   内核高半区必须**没有**（khigh=0）。这两行合起来才是"ring3 什么都摸不到"的证据。
        {
            const uint64_t ucr3 = proc64_user_cr364(pid);
            if (kpti64_on64() && ucr3 && ucr3 != info.cr3)
                total += iso64_audit_user64(ucr3, pid, why ? why : "proc-user");
        }
        seen++;
    }
    dbg64_line_begin64();
    dbg64_str("[ISO64] audit all spaces=");
    dbg64_dec((uint64_t)(seen + 1));
    dbg64_str(" procs=");      dbg64_dec((uint64_t)seen);
    dbg64_str(" slots=");      dbg64_dec((uint64_t)n);
    dbg64_str(" violations="); dbg64_dec((uint64_t)total);
    dbg64_str(total == 0 ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();
    return total;
}

// ==================== 自检（6 位）====================
int iso64_selftest64() {
    int fails = 0;

    // ① CR0.WP 已置位（内核不能偷偷写只读页）
    //    ★ v0.12.0 更正（这是 v0.11.8/v0.11.9 的一处误判，留档）：那两版把这一位读到的 wp=0
    //    当成"启动期有路径把 WP 清了"。真相是**自检跑在初化之前** —— kernel64.cpp 里
    //    iso64_selftest64() 排在 iso64_init64() 前面，那时 CR0 还是 boot/loader64.asm 打开分页时
    //    写的 0x80000011（PG|ET|PE，本来就没有 WP 位），于是自检自己把 WP 补上并打了一句
    //    "wp leak detected"。顺序修正之后这一位就真的在判"有没有泄漏"了。
    //    判据口径：**掉了就算失败**（不再靠"补完再复判"把 FAIL 掩盖成 PASS）；补只是先保住防护。
    int wp_leak = 0;
    if (!(iso_rd_cr0_64() & ISO_CR0_WP)) {
        wp_leak = 1;
        dbg64_line_begin64();
        dbg64_str("[ISO64] wp leak detected at selftest (CR0.WP=0) -> reassert");
        dbg64_nl();
        dbg64_line_end64();
        (void)iso64_reassert64();
        fails |= 1;                                   // ★ 泄漏本身就是失败
    }
    if (!g_wp || !(iso_rd_cr0_64() & ISO_CR0_WP)) fails |= 1;
    // ② CR4.SMEP：CPU 支持就必须置位；不支持则记通过（如实：这台 CPU 没有）
    if (g_smep_ok && !(iso_rd_cr4_64() & ISO_CR4_SMEP)) fails |= 2;
    // ③ 内核高半区地址必须被用户指针闸门拒绝
    if (user64_range_ok64(0xFFFFFFFF80100000ULL, 8) != 0) fails |= 4;
    // ④ 用户窗口地址必须被接受（闸门不是"一律拒绝"的假货）。
    //    但**没有进程时窗口是空的**，闸门当然拒 —— 那时如实记 win=none 并跳过这一位；
    //    真正的"窗口可用"判据在 iso64_probe64() 里（造进程 + 映页 + 再判一次）。
    const int win_live = (proc64_count64() > 0) ? 1 : 0;
    if (win_live && user64_range_ok64(USER64_CODE_VA64, 8) != 1) fails |= 8;
    // ⑤⑥ 内核地址空间 + 所有进程地址空间的审计必须零违规
    const int v = iso64_audit_all64("selftest");
    if (v != 0) fails |= 16;

    const int mask = (~fails) & 0x3F;
    dbg64_line_begin64();
    dbg64_str("[ISO64] selftest ");
    dbg64_str(mask == 0x3F ? "PASS" : "FAIL");
    dbg64_str(" mask=");   dbg64_dec((uint64_t)mask);
    dbg64_str(" wp=");     dbg64_dec((uint64_t)g_wp);
    dbg64_str(" smep=");   dbg64_dec((uint64_t)g_smep);
    dbg64_str(" audited="); dbg64_dec((uint64_t)g_audit_runs);
    dbg64_str(" violations="); dbg64_dec((uint64_t)g_audit_violations);
    dbg64_str(" win=");   dbg64_dec((uint64_t)win_live);
    dbg64_str(" wp_leak="); dbg64_dec((uint64_t)wp_leak);
    dbg64_str(" cr0=");   dbg64_hex64(iso_rd_cr0_64());
    dbg64_str(" cr4=");   dbg64_hex64(iso_rd_cr4_64());
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}

// ==================== 活进程审计探针 ====================
// 为什么需要它：实测这台机器上"终端 shell"不是独立进程（`iso` 里 procs=0），
//   于是"活进程地址空间零违规"这条判据没有对象。这里自己造一个：
//   建进程 -> 切过去 -> 映一页用户页 -> 审计（此时 uwin>=1 才有意义）-> 切回 -> 销毁。
int iso64_probe64() {
    if (!proc64_isolate64()) {
        dbg64_line_begin64();
        dbg64_str("[ISO64] probe skip reason=shared-address-space (no per-proc CR3 on this boot path)");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    const int pid = proc64_create64("iso_probe", -1);
    if (pid < 0) {
        dbg64_line_begin64();
        dbg64_str("[ISO64] probe FAIL reason=create pid=");
        dbg64_dec((uint64_t)(pid < 0 ? 0 : (uint32_t)pid));
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    proc64_switch_to64(pid);
    uint64_t pa = 0;
    const int mr = user64_map_page64(USER64_CODE_VA64, PTE_USER_64 | PTE_WRITE_64, 1, &pa);
    const int range = user64_range_ok64(USER64_CODE_VA64, 8);
    const int viol = iso64_audit64(0, pid, "probe");
    // ★ v0.11.9（KPTI）：再单独审用户表（khigh 必须 0）—— 这才是"这套地址空间里
    //   根本摸不到内核"的直接证据。没有配对时如实跳过（ucr3 == 内核表）。
    int viol_u = 0;
    int have_u = 0;
    {
        const uint64_t ucr3 = proc64_user_cr364(pid);
        uint64_t cur; __asm__ volatile("mov %%cr3, %0" : "=r"(cur));
        if (kpti64_on64() && ucr3 && ucr3 != (cur & 0x000FFFFFFFFFF000ULL)) {
            viol_u = iso64_audit_user64(ucr3, pid, "probe-user");
            have_u = 1;
        }
    }
    proc64_switch_to_kernel64();
    proc64_destroy64(pid);

    const int ok = (mr == 1 && range == 1 && viol == 0 && viol_u == 0) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[ISO64] probe pid=");  dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" map=");               dbg64_dec((uint64_t)(mr == 1 ? 1 : 0));
    dbg64_str(" range=");             dbg64_dec((uint64_t)(range == 1 ? 1 : 0));
    dbg64_str(" us_violation=");      dbg64_dec((uint64_t)viol);
    dbg64_str(" pa=");                dbg64_hex64(pa);
    // ★ v0.11.9：用户表那一遍（khigh 必须 0）—— kpti_u=1 表示"真的审到了那张表"
    dbg64_str(" kpti_u=");            dbg64_dec((uint64_t)have_u);
    dbg64_str(" u_violation=");       dbg64_dec((uint64_t)viol_u);
    dbg64_str(ok ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

void iso64_report64() {
    dbg64_line_begin64();
    dbg64_str("[ISO64] status wp=");   dbg64_dec((uint64_t)g_wp);
    dbg64_str(" smep=");               dbg64_dec((uint64_t)g_smep);
    dbg64_str(" smap=");               dbg64_dec((uint64_t)g_smap);
    dbg64_str("(supported=");          dbg64_dec((uint64_t)g_smap_ok);
    dbg64_str(") kpti=");              dbg64_dec((uint64_t)kpti64_on64());
    dbg64_str(" kpti_reason=");        dbg64_str(kpti64_reason64());
    dbg64_str(" audited=");    dbg64_dec((uint64_t)g_audit_runs);
    dbg64_str(" violations=");         dbg64_dec((uint64_t)g_audit_violations);
    dbg64_str(" smap_violations=");    dbg64_dec((uint64_t)g_smap_viol);
    dbg64_nl();
    dbg64_line_end64();
}
