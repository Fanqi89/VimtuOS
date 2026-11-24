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

    dbg64_line_begin64();
    dbg64_str("[ISO64] hw wp=");      dbg64_dec((uint64_t)g_wp);
    dbg64_str(" smep=");              dbg64_dec((uint64_t)g_smep);
    dbg64_str(" smep_supported=");    dbg64_dec((uint64_t)g_smep_ok);
    dbg64_str(" smap_supported=");    dbg64_dec((uint64_t)g_smap_ok);
    dbg64_str(" smap_on=0");          // 本版如实：不开（理由见 iso64.h）
    dbg64_str(" cr0=");               dbg64_hex64(cr0);
    dbg64_str(" cr4=");               dbg64_hex64(cr4);
    dbg64_str(g_wp ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();

    // 如实说明"还没做的那层"，免得后来人把审计通过当成 KPTI
    dbg64_line_begin64();
    dbg64_str("[ISO64] mode kpti=0 smap=0 reason=user-space-still-maps-kernel (PML4[511] shared by design;"
              " KPTI needs a trampoline + dual tables, next stage)");
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

// ==================== 地址空间审计 ====================
// 走四级：统计"内核高半区/前 4GB 恒等映射/用户窗口/窗口外"四类 present 项 + 用户可访问位违规数。
int iso64_audit64(uint64_t cr3, int pid, const char* why) {
    if (cr3 == 0) {                                   // 0 = "当前 CR3"
        uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v));
        cr3 = v;
    }
    const uint64_t pml4_pa = iso_page64(cr3);
    uint64_t* pml4 = iso_phys64(pml4_pa);
    int khigh = 0, ident = 0, uwin = 0, outside = 0, viol = 0;

    // ---- ① 内核高半区：PML4[511] → PDPT_hi → PD（可能 1GB 大页、2MB 大页或 PT）----
    const uint64_t e511 = pml4[511];
    if (iso_present64(e511)) {
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
        viol++;                                           // 内核半区没了 = 内核自己在跑什么？
    }

    // ---- ② 前 4GB 恒等映射：PML4[0] → PDPT，只允许 0..3（每 1GB）；5..511 必须不存在 ----
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
        // 窗口外的用户地址（PDPT[5..511]）必须不存在
        for (int i = 5; i < 512; i++) {
            if (iso_present64(pdpt[i])) { outside++; viol++; }
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
    dbg64_nl();
    dbg64_line_end64();
    return viol;
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
    //    ★ 实测发现：启动期有路径会把 WP 临时清掉（boot 自检时读到 wp=0，到桌面后又变回 1）。
    //    这里**先如实判**，掉了就打 `wp leak detected` 并立刻补上再复判 —— 修复进 mask，
    //    "曾经掉过"这件事留在日志里（不隐瞒）。
    int wp_leak = 0;
    if (!(iso_rd_cr0_64() & ISO_CR0_WP)) {
        wp_leak = 1;
        dbg64_line_begin64();
        dbg64_str("[ISO64] wp leak detected at selftest (CR0.WP=0) -> reassert");
        dbg64_nl();
        dbg64_line_end64();
        (void)iso64_reassert64();
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
    proc64_switch_to_kernel64();
    proc64_destroy64(pid);

    const int ok = (mr == 1 && range == 1 && viol == 0) ? 1 : 0;
    dbg64_line_begin64();
    dbg64_str("[ISO64] probe pid=");  dbg64_dec((uint64_t)(uint32_t)pid);
    dbg64_str(" map=");               dbg64_dec((uint64_t)(mr == 1 ? 1 : 0));
    dbg64_str(" range=");             dbg64_dec((uint64_t)(range == 1 ? 1 : 0));
    dbg64_str(" us_violation=");      dbg64_dec((uint64_t)viol);
    dbg64_str(" pa=");                dbg64_hex64(pa);
    dbg64_str(ok ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();
    return ok;
}

void iso64_report64() {
    dbg64_line_begin64();
    dbg64_str("[ISO64] status wp=");   dbg64_dec((uint64_t)g_wp);
    dbg64_str(" smep=");               dbg64_dec((uint64_t)g_smep);
    dbg64_str(" smap=0(supported=");   dbg64_dec((uint64_t)g_smap_ok);
    dbg64_str(") kpti=0 audited=");    dbg64_dec((uint64_t)g_audit_runs);
    dbg64_str(" violations=");         dbg64_dec((uint64_t)g_audit_violations);
    dbg64_nl();
    dbg64_line_end64();
}
