// kpti64.cpp - KPTI 的 C 侧：共享入口区的建立 / 门的重定向 / 双 PML4 配对 / 自检
//
// 分工（看清楚再改）：
//   * 这里只做"数据面"：分物理页、填 PD/PT、写 GDT/TSS/IDT 副本、配对 PML4[255]、自检打点；
//   * 真正跑在 ring3↔ring0 边界上的代码在 kernel/kpti_blob64.asm（有详细注释与约束说明）；
//   * gate/TSS/GDT/IDT 的"搬家"落在 kernel/x86_64.cpp（那是它们的所有者），这里只调它。
//
// 打开条件（三条同时满足才启用）：
//   1) 引导期页表是**我们自己的**（BIOS 路径：CR3 = 0x40000；UEFI 固件表不行 —— 那张表只读、
//      格式由固件决定，而且 VMware EFI 下运行期 mov cr3 已知会复位，见 proc64.cpp 的说明）；
//   2) 安装介质内核不启用（没有进程/没有 ring3 应用，加了只是多一层风险，如实打 reason）；
//   3) 需要的内存（IDT 页 + PD + PT 三个页）分得到。
// 任何一条不满足 → kpti64_on64() 恒 0，**不映射、不改门、不改 TSS/LSTAR**，
// 系统行为与 v0.11.8 逐指令一致，并在串口如实写 [KPTI64] init skipped reason=...
#include "kpti64.h"
#include "debug64.h"
#include "mem_64.h"
#include "x86_64.h"
// ★ 不 include memlayout64.h：它把 PAGE_SIZE_64 写成 static const 变量，而 mem_64.h 写成宏
//   （同名，同文件包含过不去）。这里只需要两个常量，直接引一份并注明真源。
//   真源 ①：kernel/memlayout64.h 的 ML64_PML4_PHYS（loader 建页表用的 PML4 物理地址）
static const uint64_t KPTI64_PML4_PHYS = 0x40000ULL;
//   真源 ②：直映起点 VA = 0xFFFFFFFF80000000 + PA（见 kernel/linker64.ld 顶部说明）
static const uint64_t KPTI64_DIRECT_BASE = 0xFFFFFFFF80000000ULL;

extern "C" {
extern uint8_t  kpti64_blob64[];
extern uint64_t kpti64_offtab64[];
extern uint8_t  kptii_gdt64[];
extern uint8_t  kptii_tss64[];
extern uint8_t  kptii_entry_stk64[];
extern uint8_t  kptii_ret_stk64[];
extern uint64_t kptii_entry_rsp64;
extern uint64_t kptii_ret_rsp64;
extern uint64_t kptii_kstack64;
extern uint8_t  kpti64_iret_frame64[];
extern uint8_t  kpti64_syscall_entry64[];
extern uint64_t g_isr64_exit64;         // isr_stubs64.asm 的"ISR 公共出口"指针（默认 = 原位出口）
}

// kernel/syscall64.cpp 里的同名函数（同一份 C++ 链接；KPTI 启用后把 LSTAR 指到共享入口）
void syscall64_kpti_set_lstar64(uint64_t entry);

// 入口栈 / 返回栈大小（与 kpti_blob64.asm 的 times 声明一致；改一处必须改两处）
#define KPTI64_ENTRY_STK_BYTES 1792
#define KPTI64_RET_STK_BYTES   512

static int         g_pair_fail_n = 0;   // "这个进程没配上对"的次数（降级，不是失败）
static uint64_t g_offtab_va = 0;        // 向量偏移表在共享区的 VA（按实际偏移算出）
static uint64_t g_idt_va    = 0;        // 共享 IDT 的 VA
static uint32_t g_img_pages = 0;        // 共享区里从镜像映射的页数（其后一页是 IDT）
static int         g_inited = 0;
static int         g_on     = 0;
static int         g_fail   = 0;
static int         g_st_fail = 0;
static const char* g_reason = "init-not-run";
static uint64_t g_page0_pa = 0;         // .text.kpti 起始页的物理地址（= 共享区页 0）
static uint64_t g_pd_phys  = 0;
static uint64_t g_pt_phys  = 0;
static uint64_t g_idt_phys = 0;

static inline uint64_t k_rd_cr3_64() { uint64_t v; __asm__ volatile("mov %%cr3, %0" : "=r"(v)); return v; }
static inline uint64_t k_page64(uint64_t v) { return v & 0x000FFFFFFFFFF000ULL; }

// 符号 → 共享 VA。理由：本节链接在内核高半区，但**被整段映射**到 KPTI64_BASE；
// 所以"节内偏移"在两个地址上完全一样，换算只需加基址。
static inline uint64_t k_off64(const void* sym) {
    return (uint64_t)(uintptr_t)sym - (uint64_t)(uintptr_t)kpti64_blob64;
}
static inline uint64_t k_dav64(const void* sym) { return KPTI64_BASE + k_off64(sym); }
static inline void     k_wr64(uint64_t va, uint64_t v) { *(volatile uint64_t*)(uintptr_t)va = v; }
static inline uint64_t k_rd64(uint64_t va) { return *(const volatile uint64_t*)(uintptr_t)va; }

int kpti64_on64(void) { return g_on; }
const char* kpti64_reason64(void) { return g_reason; }
uint64_t kpti64_pd_phys64(void) { return g_pd_phys; }

uint64_t kpti64_tramp_raw64(int vec) {
    if (vec < 0 || vec > 255) return 0;
    const uint64_t off = kpti64_offtab64[vec];
    return off ? (KPTI64_BASE + off) : 0;
}
uint64_t kpti64_tramp64(int vec) { return g_on ? kpti64_tramp_raw64(vec) : 0; }

uint64_t kpti64_iret_routine64(void) {
    return g_on ? k_dav64(kpti64_iret_frame64) : 0;
}

uint64_t kpti64_entry_rsp064(void) {
    return k_dav64(kptii_entry_stk64) + KPTI64_ENTRY_STK_BYTES;
}
void kpti64_set_kstack64(uint64_t top) {
    if (g_on) k_wr64(k_dav64(&kptii_kstack64), top);
}

uint64_t kpti64_offtab_va64(void) { return g_offtab_va; }
uint64_t kpti64_idt_va64(void) { return g_idt_va; }
uint64_t kpti64_syscall_entry_va64(void) {
    return g_on ? k_dav64(kpti64_syscall_entry64) : 0;
}

// PML4[255] = "数据槽"：non-present 项里存同一进程的另一张表的物理地址。
// 为什么可以这么用：bit0=0 ⇒ CPU 永远不把它当映射（非 present 项不检查保留位），
// 而 [255] 这个下标在两张表里都没有别的用处（用户窗口是 [4]，内核高半区是 [511]）。
int kpti64_link_pair64(uint64_t pml4_kern_phys, uint64_t pml4_user_phys) {
    if (!g_on) return 0;
    if (!pml4_kern_phys || !pml4_user_phys) return -1;
    if (pml4_kern_phys >= 0x100000000ULL || pml4_user_phys >= 0x100000000ULL) {
        // 蹦床要在恒等直映区里读这两个槽（mov rax,cr3; mov rbx,[rax+255*8]）：4GB 以上做不到。
        // ★ 这里**不关 KPTI**：只让"这一个进程"没有配对（它的 ring3 走蹦床的 .plain 分支，
        //   行为等价于 v0.11.8）。整层关掉会把已经重定向的门留在半空中，比降级更危险。
        g_pair_fail_n++;
        return -1;
    }
    *(volatile uint64_t*)(uintptr_t)(pml4_kern_phys + 255ULL * 8) = pml4_user_phys;   // non-present
    *(volatile uint64_t*)(uintptr_t)(pml4_user_phys + 255ULL * 8) = pml4_kern_phys;
    return 0;
}

uint64_t kpti64_peer_cr364(uint64_t cr3) {
    if (!cr3) return 0;
    const uint64_t pa = k_page64(cr3);
    if (pa >= 0x100000000ULL) return 0;
    return *(const volatile uint64_t*)(uintptr_t)(pa + 255ULL * 8) & 0x000FFFFFFFFFF000ULL;
}

// ============================ 建立共享入口区 ============================
int kpti64_init64(void) {
    if (g_inited) return g_on ? 0 : -1;
    g_inited = 1;

#ifdef VIMTU_INSTALLER_MEDIA
    // 安装介质内核没有进程、也不跑 ring3 应用：整层不启用（不映射、不改门），如实打点。
    g_fail = 1;
    g_reason = "installer-media";
    dbg64_str("[KPTI64] init skipped reason=installer-media kpti=0\n");
    return -1;
#else
    const uint64_t cr3 = k_rd_cr3_64();
    if (k_page64(cr3) != KPTI64_PML4_PHYS) {
        g_fail = 1;
        g_reason = "boot-cr3-not-own";
        dbg64_line_begin64();
        dbg64_str("[KPTI64] init skipped reason=boot-cr3-not-own cr3=");
        dbg64_hex64(cr3);
        dbg64_str(" expect=");
        dbg64_hex64(KPTI64_PML4_PHYS);
        dbg64_str(" kpti=0\n");
        dbg64_line_end64();
        return -1;
    }

    // ---- 1) 物理页：IDT 页 + PD + PT（页 0..2 直接用镜像里那三页，不额外占内存）----
    void* idt_p = page_alloc_64();
    void* pd_p  = page_alloc_64();
    void* pt_p  = page_alloc_64();
    if (!idt_p || !pd_p || !pt_p) {
        g_fail = 1;
        g_reason = "page-alloc-failed";
        dbg64_str("[KPTI64] init skipped reason=page-alloc-failed kpti=0\n");
        return -1;
    }
    for (uint32_t i = 0; i < PAGE_SIZE_64; i++) {
        ((uint8_t*)idt_p)[i] = 0;
        ((uint8_t*)pd_p)[i]  = 0;
        ((uint8_t*)pt_p)[i]  = 0;
    }

    const uint64_t blob_pa = (uint64_t)(uintptr_t)kpti64_blob64 - KPTI64_DIRECT_BASE;
    const uint64_t idt_pa  = (uint64_t)(uintptr_t)idt_p;
    const uint64_t pd_pa   = (uint64_t)(uintptr_t)pd_p;
    const uint64_t pt_pa   = (uint64_t)(uintptr_t)pt_p;
    if (blob_pa >= 0x100000000ULL || (blob_pa + 0x8000) >= 0x100000000ULL ||
        idt_pa >= 0x100000000ULL || pd_pa >= 0x100000000ULL || pt_pa >= 0x100000000ULL) {
        g_fail = 1; g_reason = "above-4g";
        dbg64_str("[KPTI64] init skipped reason=above-4g kpti=0\n");
        return -1;
    }
    // 偏移表必须**页对齐**（kpti_blob64.asm 用 align 4096 保证；这里如实核一遍）。
    // ★ 不写死页号：v0.11.9 实测过它从页 2 挪到页 1（因为给蹦床 0 加了 8 字节魔数）。
    const uint64_t off_tab = k_off64(kpti64_offtab64);
    if (off_tab == 0 || (off_tab & 0xFFFULL) != 0 || off_tab + 2048 > 0x8000ULL) {
        g_fail = 1; g_reason = "offtab-layout";
        dbg64_line_begin64();
        dbg64_str("[KPTI64] init skipped reason=offtab-layout off=");
        dbg64_hex64(off_tab);
        dbg64_str(" kpti=0\n");
        dbg64_line_end64();
        return -1;
    }
    const uint32_t img_pages = (uint32_t)((off_tab + 2048 + 0xFFFULL) / 0x1000ULL);   // 镜像里要映射的页数
    const uint64_t idt_va    = KPTI64_BASE + (uint64_t)img_pages * 0x1000ULL;

    // ---- 2) PT：页 0..img_pages-1 = 镜像里的 .text.kpti（RW、可执行、U/S=0）；
    //            紧随其后一页 = IDT（运行时分配）----
    uint64_t* pt = (uint64_t*)(uintptr_t)pt_pa;
    for (uint32_t i = 0; i < img_pages; i++)
        pt[i] = (blob_pa + (uint64_t)i * 0x1000ULL) | PTE_PRESENT_64 | PTE_WRITE_64;
    pt[img_pages] = idt_pa | PTE_PRESENT_64 | PTE_WRITE_64;
    g_offtab_va = KPTI64_BASE + off_tab;
    g_idt_va    = idt_va;
    g_img_pages = img_pages;

    // ---- 3) PD[0] -> 上面那张 PT；再挂到引导期 PDPT[6]（6 GiB..7 GiB）----
    //   ★ 不用 PDPT[5]：那是 USER64_FB_VA64（framebuffer/DRM 窗口），v0.9 起就占着。
    uint64_t* pd = (uint64_t*)(uintptr_t)pd_pa;
    pd[0] = pt_pa | PTE_PRESENT_64 | PTE_WRITE_64;

    uint64_t* pml4 = (uint64_t*)(uintptr_t)k_page64(cr3);
    const uint64_t e0 = pml4[0];
    if (!(e0 & PTE_PRESENT_64) || (e0 & PTE_2MB_64)) {
        g_fail = 1; g_reason = "pml4-0-bad";
        dbg64_str("[KPTI64] init skipped reason=pml4.0-bad kpti=0\n");
        return -1;
    }
    uint64_t* boot_pdpt = (uint64_t*)(uintptr_t)k_page64(e0);
    if (boot_pdpt[6] & PTE_PRESENT_64) {     // 6..7 GiB 本来就占着：不抢，如实放弃
        g_fail = 1; g_reason = "pdpt-5-busy";
        dbg64_str("[KPTI64] init skipped reason=pdpt.5-busy kpti=0\n");
        return -1;
    }
    boot_pdpt[6] = pd_pa | PTE_PRESENT_64 | PTE_WRITE_64;      // U/S=0：ring3 摸不到
    __asm__ volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");  // 刷 TLB

    g_page0_pa = blob_pa;
    g_pd_phys  = pd_pa;
    g_pt_phys  = pt_pa;
    g_idt_phys = idt_pa;

    // ---- 4) 变量初始化（入口栈顶 / 返回栈顶 / 内核栈顶兜底）----
    k_wr64(k_dav64(&kptii_entry_rsp64), kpti64_entry_rsp064());
    k_wr64(k_dav64(&kptii_ret_rsp64),   k_dav64(kptii_ret_stk64) + KPTI64_RET_STK_BYTES);
    k_wr64(k_dav64(&kptii_kstack64),    0x80000ULL);            // 与 tss_init64(0x80000) 的兜底一致

    // ---- 5) 逐字节核对：共享 VA 上看到的确实是镜像里那一段（别名映射真的成立）----
    {
        const uint8_t* src = (const uint8_t*)(uintptr_t)blob_pa;
        const uint8_t* dst = (const uint8_t*)(uintptr_t)KPTI64_BASE;
        int bad = 0;
        for (uint32_t i = 0; i < 256; i++) if (src[i] != dst[i]) bad++;
        if (bad) {
            g_fail = 1; g_reason = "alias-mismatch";
            dbg64_line_begin64();
            dbg64_str("[KPTI64] init skipped reason=alias-mismatch bad=");
            dbg64_dec((uint64_t)bad);
            dbg64_str(" kpti=0\n");
            dbg64_line_end64();
            return -1;
        }
    }

    // ---- 6) 真正启用：GDT/TSS/IDT 搬家 + 门重定向 + ISR 出口 + syscall 入口 ----
    g_on = 1;
    const uint64_t gdt_va = k_dav64(kptii_gdt64);
    const uint64_t tss_va = k_dav64(kptii_tss64);
    const uint64_t ent    = kpti64_entry_rsp064();
    if (x86_64_kpti_enable64(idt_va, gdt_va, tss_va, ent) != 0) {
        g_on = 0; g_fail = 1; g_reason = "x86_64-enable-failed";
        dbg64_str("[KPTI64] init failed reason=x86_64-enable-failed kpti=0\n");
        return -1;
    }
    g_isr64_exit64 = k_dav64(kpti64_iret_frame64);      // ISR 公共出口改走共享区
    syscall64_kpti_set_lstar64(k_dav64(kpti64_syscall_entry64));
    kpti64_set_kstack64(0x80000ULL);                    // 与 TSS 副本的兜底保持一致
    g_reason = "on";

    dbg64_line_begin64();
    dbg64_str("[KPTI64] init ok area=");
    dbg64_hex64(KPTI64_BASE);
    dbg64_str(" blob_pa=");
    dbg64_hex64(blob_pa);
    dbg64_str(" pd=");
    dbg64_hex64(g_pd_phys);
    dbg64_str(" pt=");
    dbg64_hex64(g_pt_phys);
    dbg64_str(" idt=");
    dbg64_hex64(g_idt_phys);
    dbg64_str(" entry_rsp=");
    dbg64_hex64(ent);
    dbg64_str(" tramp=");
    {
        int n = 0;
        for (int v = 0; v < 256; v++) if (kpti64_tramp_raw64(v)) n++;
        dbg64_dec((uint64_t)n);
    }
    dbg64_str(" kpti=1\n");
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[KPTI64] mode kpti=1 entry=int80+irq+syscall rsp0=shared dual_cr3=1"
              " (user table PML4[511]=0; kernel table full)\n");
    dbg64_line_end64();
    // ---- 7) 自检（6 位）：判据都在 kpti64_selftest64 里，`iso` 命令与验收脚本读同一份 ----
    {
        const int m = kpti64_selftest64();
        dbg64_line_begin64();
        dbg64_str("[KPTI64] selftest ");
        dbg64_str(m == 0 ? "PASS" : "FAIL");
        dbg64_str(" mask=");
        dbg64_dec((uint64_t)((~m) & 0x3F));
        dbg64_str(" bits=");
        dbg64_dec((uint64_t)m);
        dbg64_str(" kpti=1\n");
        dbg64_line_end64();
    }
    return 0;
#endif
}

// ============================ 自检（6 位；0 = 全绿）============================
//   bit0 共享区在当前页表里真的 mapped（PDPT[6] → PD[0] → PT[0] present 且指向本镜像页）
//   bit1 IDTR.base == 共享 IDT 页（门真的在那里）
//   bit2 门 0x80 / 0x20 的目标 == 共享蹦床（不是内核高半区的 isrN_64）
//   bit3 入口栈顶 / 返回栈顶已初始化（非 0、落在共享区窗口内）
//   bit4 当前表的 PML4[255] 是"非 present 数据槽"，且 kpti64_peer_cr364 读得出来
//   bit5 本任务内核栈顶镜像非 0 且 != 入口栈顶（搬家目标不可能是入口栈自己 —— 那会自覆盖）
int kpti64_selftest64(void) {
    if (!g_on) { g_st_fail = 0x3F; return 0x3F; }   // 未启用：六位全红（调用方自己判要不要当失败）
    int fail = 0;

    {   // bit0
        const uint64_t cr3 = k_rd_cr3_64();
        const uint64_t* pml4 = (const uint64_t*)(uintptr_t)k_page64(cr3);
        const uint64_t e0 = pml4[0];
        int ok = 0;
        if ((e0 & PTE_PRESENT_64) && !(e0 & PTE_2MB_64)) {
            const uint64_t* pdpt = (const uint64_t*)(uintptr_t)k_page64(e0);
            const uint64_t e6 = pdpt[6];              // ★ 6 GiB（PDPT[5] 是 fb 窗口，不是我们）
            if ((e6 & PTE_PRESENT_64) && !(e6 & PTE_USER_64) && k_page64(e6) == g_pd_phys) {
                const uint64_t* pd = (const uint64_t*)(uintptr_t)k_page64(e6);
                const uint64_t pde = pd[0];
                if ((pde & PTE_PRESENT_64) && !(pde & PTE_USER_64) && k_page64(pde) == g_pt_phys) {
                    const uint64_t* pt = (const uint64_t*)(uintptr_t)k_page64(pde);
                    if ((pt[0] & PTE_PRESENT_64) && k_page64(pt[0]) == g_page0_pa) ok = 1;
                }
            }
        }
        if (!ok) fail |= 1;
    }
    if (x86_64_kpti_idt_base64() != g_idt_va) fail |= 2;                                      // bit1
    if (x86_64_kpti_gate_target64(0x80) != kpti64_tramp_raw64(128)) fail |= 4;                // bit2
    if (x86_64_kpti_gate_target64(0x20) != kpti64_tramp_raw64(32))  fail |= 4;
    {   // bit3
        const uint64_t er = k_rd64(k_dav64(&kptii_entry_rsp64));
        const uint64_t rr = k_rd64(k_dav64(&kptii_ret_rsp64));
        if (!(er > KPTI64_BASE && er <= KPTI64_BASE + 0x4000)) fail |= 8;
        if (!(rr > KPTI64_BASE && rr <= KPTI64_BASE + 0x4000)) fail |= 8;
    }
    {   // bit4
        const uint64_t cr3 = k_rd_cr3_64();
        const uint64_t* pml4 = (const uint64_t*)(uintptr_t)k_page64(cr3);
        const uint64_t slot = pml4[255];
        if (slot & PTE_PRESENT_64) fail |= 16;
        if (kpti64_peer_cr364(cr3) != k_page64(slot)) fail |= 16;
    }
    {   // bit5
        const uint64_t ks = k_rd64(k_dav64(&kptii_kstack64));
        const uint64_t er = k_rd64(k_dav64(&kptii_entry_rsp64));
        if (ks == 0 || ks == er) fail |= 32;
    }
    g_st_fail = fail;
    return fail;
}

void kpti64_report64(void) {
    dbg64_line_begin64();
    dbg64_str("[KPTI64] status kpti=");
    dbg64_dec((uint64_t)g_on);
    dbg64_str(" reason=");
    dbg64_str(g_reason ? g_reason : "-");
    dbg64_str(" area=");
    dbg64_hex64(KPTI64_BASE);
    dbg64_str(" idt_base=");
    dbg64_hex64(g_on ? x86_64_kpti_idt_base64() : 0);
    dbg64_str(" gdt_base=");
    dbg64_hex64(g_on ? x86_64_kpti_gdt_base64() : 0);
    dbg64_str(" entry_rsp=");
    dbg64_hex64(g_on ? k_rd64(k_dav64(&kptii_entry_rsp64)) : 0);
    dbg64_str(" kstack=");
    dbg64_hex64(g_on ? k_rd64(k_dav64(&kptii_kstack64)) : 0);
    dbg64_str(" selftest_fail=");
    dbg64_dec((uint64_t)g_st_fail);
    dbg64_nl();
    dbg64_line_end64();
}
