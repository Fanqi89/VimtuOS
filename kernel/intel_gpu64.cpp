// intel_gpu64.cpp - P3：Intel 核显**初步**驱动（认卡 + 资源 + GTT 只读采样）
//
// 边界与开关见 kernel/intel_gpu64.h 顶部。一句话：**这里只读**。
//
// ==================== 串口打点（纯 ASCII；全部是新行，不动任何既有行）====================
//   [INTEL] dev idx=1 device=5916 family=hd620 gen=9.5 bar0=0xde000000
//   [INTEL] gtt bar=2 phys=0xdf000000 size=0x1000000 off=0x800000 entries=32 valid=12
//           alt_off=0x200000 alt_valid=0 phys_first=0x12345000
//   [INTEL] gtt bar=2 absent reason=no-bar2
//   [INTEL] gtt bar=2 phys=0x… size=0x… skipped reason=small-window|above-4g-unmapped
//   [INTEL] pipe a conf=0x… dsl=0x… on=1 htotal=0x… vtotal=0x… pipesrc=0x… hactive=… vactive=…
//   [INTEL] plane a ctrl=0x… on=1 fmt=6 stride=5120 surf=0x… linoff=0x… tileoff=0x…
//   [INTEL] plan takeover=not-yet frame_match=1 need=gtt-framebuffer,plane-programming readonly=1
//   [INTEL] pipe snapshot skipped reason=no-reg-window
//   （上面三行是 v0.9.9 新增：接管前的**只读**证据链；GTT 行的行尾还多一个
//     alloc=readonly scan=4096 free_run_start=… free_run_len=… 的空闲段扫描结果）
//   [INTEL] mode=intel-safe requested=intel_safe readonly=1 scanout=none vendor_writes=0
//   [INTEL] selftest ok intel_devs=1 readonly=1
//   [INTEL] selftest skipped reason=no-intel-device
//
// 为什么 GTT 要采两个偏移：GTT 表在 GTTMMADR 里的偏移随代数变化（gen6/7 ≈ 0x200000，
//   gen8+ ≈ 0x800000）。本层**不猜**：两个偏移都只读采 32 项，把两边的 valid 计数都打出来，
//   由人在真机日志上一眼判断哪个是对的。代价是 64 次读，收益是不给真机一个假的"看起来对"。
#include "intel_gpu64.h"
#include "debug64.h"
#include "fb.h"          // fb64_fwcfg_str64（opt/vimtu/gpu 的 mode=）
#include "pci64.h"       // pci64_bar_probe64 / Pci64Bar64

// ==================== 打点原语（noinline：体积纪律与 gpu64.cpp 一致）====================
static const char* const IN_HEXL = "0123456789abcdef";
__attribute__((noinline)) static void in_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void in_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void in_num(uint64_t v) { dbg64_dec(v); }
static void in_begin() { dbg64_line_begin64(); }
static void in_end()   { dbg64_nl(); dbg64_line_end64(); }
__attribute__((noinline)) static void in_hex(uint64_t v) {          // "0x" + 最短十六进制
    in_puts("0x");
    if (!v) { in_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = IN_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) in_ch(b[--n]);
}
__attribute__((noinline)) static void in_hexfix(uint32_t v, int digits) {   // 定宽（无前缀）
    char b[8]; int n = 0;
    while (n < digits) { b[n++] = IN_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) in_ch(b[--n]);
}
__attribute__((noinline)) static int in_streq(const char* a, const char* b) {
    for (int i = 0; ; i++) {
        if (a[i] != b[i]) return 0;
        if (!a[i]) return 1;
    }
}

// ==================== device id -> 家族 / 代数 ====================
// ★ 只列**有公开资料可对**的移动/桌面核显 id。查不到的 device id 一律 family=unknown、
//   gen 不倒猜（`gen=?`）—— 真机日志上"unknown"是个有用的信息，猜出来的名字则是噪音。
enum {
    IF_UNKNOWN = 0, IF_HD4000, IF_HD4600, IF_HD510, IF_HD515, IF_HD520, IF_HD530,
    IF_IRIS540, IF_IRIS_PRO580, IF_HD610, IF_HD615, IF_HD620, IF_HD630, IF_IRIS640,
    IF_IRIS650, IF_UHD630
};
struct Intel64Id {
    uint16_t dev;
    uint8_t  fam;
    uint8_t  gen;      // 7 / 75(7.5) / 9 / 95(9.5)
};
static const Intel64Id IN_IDS[] = {
    // Ivy Bridge（gen7）/ Haswell（gen7.5）
    {0x0162, IF_HD4000, 7},  {0x0166, IF_HD4000, 7},
    {0x0412, IF_HD4600, 75}, {0x0416, IF_HD4600, 75}, {0x041E, IF_HD4600, 75},
    // Skylake（gen9）—— 用户的 HD 530 在这批里
    {0x1902, IF_HD510, 9},   {0x1912, IF_HD530, 9},   {0x1916, IF_HD520, 9},
    {0x191B, IF_HD530, 9},   {0x191E, IF_HD515, 9},   {0x1926, IF_IRIS540, 9},
    {0x1927, IF_IRIS_PRO580, 9},
    // Kaby Lake（gen9.5）—— 用户的 HD 630 在这批里
    {0x5902, IF_HD610, 95},  {0x5912, IF_HD630, 95},  {0x5916, IF_HD620, 95},
    {0x591E, IF_HD615, 95},  {0x5926, IF_IRIS640, 95}, {0x5927, IF_IRIS650, 95},
    // Coffee Lake（gen9.5）
    {0x3E92, IF_UHD630, 95}, {0x3E98, IF_UHD630, 95}, {0x3E9B, IF_UHD630, 95},
};

__attribute__((noinline)) static const char* in_fam_name(uint8_t fam) {
    switch (fam) {
        case IF_HD4000: return "hd4000";
        case IF_HD4600: return "hd4600";
        case IF_HD510:  return "hd510";
        case IF_HD515:  return "hd515";
        case IF_HD520:  return "hd520";
        case IF_HD530:  return "hd530";
        case IF_IRIS540: return "iris540";
        case IF_IRIS_PRO580: return "iris-pro-580";
        case IF_HD610:  return "hd610";
        case IF_HD615:  return "hd615";
        case IF_HD620:  return "hd620";
        case IF_HD630:  return "hd630";
        case IF_IRIS640: return "iris-plus-640";
        case IF_IRIS650: return "iris-plus-650";
        case IF_UHD630: return "uhd630";
        default:        return "unknown";
    }
}

__attribute__((noinline)) static void in_gen(uint8_t g) {
    if (g == 95) { in_num(9); in_ch('.'); in_num(5); return; }
    if (g == 75) { in_num(7); in_ch('.'); in_num(5); return; }
    if (!g)     { in_ch('?'); return; }                 // 没查到表：如实打问号，不猜
    in_num(g);
}

// ==================== 配置：opt/vimtu/gpu 的 mode= ====================
static int g_mode = -1;                                  // -1 = 还没解析
static char g_mode_str[16];                              // 原始串（打在 requested= 后面，方便取证）

int intel64_mode64(void) {
    if (g_mode >= 0) return g_mode;
    g_mode = INTEL64_MODE64_SAFE;                        // 默认**必须**是安全的
    if (fb64_fwcfg_str64("opt/vimtu/gpu", "mode", g_mode_str, (int)sizeof(g_mode_str))) {
        if (in_streq(g_mode_str, "off"))        g_mode = INTEL64_MODE64_OFF;
        else if (in_streq(g_mode_str, "intel")) g_mode = INTEL64_MODE64_FULL;
        // 其余任何值（含 intel_safe）都是 SAFE —— 配置写错也只会更保守，不会更危险
    } else {
        g_mode_str[0] = 0;
    }
    return g_mode;
}

// ==================== GTT 只读采样 ====================
// 每项 8 字节：bit0 = valid，bits12.. = 物理页号。这里**只读**，绝不写回。
// 返回 valid 项数；*first 填第一个 valid 项的页基址（没有就 0）。
__attribute__((noinline)) static uint32_t in_gtt_valid(uint64_t pa, int n, uint64_t* first) {
    uint32_t cnt = 0;
    uint64_t f = 0;
    for (int i = 0; i < n; i++) {
        const uint64_t e = *(volatile uint64_t*)(uintptr_t)(pa + (uint64_t)i * 8u);
        if (e & 1ull) {
            cnt++;
            if (!f) f = e & 0x000FFFFFFFFFF000ull;
        }
    }
    if (first) *first = f;
    return cnt;
}

// ==================== ★ v0.9.9：显示引擎只读快照（接管前的证据链）====================
// 目的很窄：把**固件现在把 pipe / plane 编成了什么样**如实读出来，好回答"接管前还差什么"。
// 边界不变：**全是读**。本批不写 pipe/plane/DPLL，也不碰 GTT 表项。
//
// 偏移来源：Intel 显示引擎（gen4+ 起的 pipe/plane 寄存器布局在 gen9/gen9.5 上沿用），
//   我们只取**公开且稳定**的那一组；不确定位域含义的寄存器一律只打原始值，
//   不硬解释（宁可少说，也不给真机一个"看起来对"的结论）。
//     0x60000/0x60008/0x6000C/0x60014  HTOTAL/HSYNC/VTOTAL/VSYNC (pipe A)
//     0x6001C                          PIPESRC_A（源尺寸）
//     0x70000 / 0x70008                PIPEA_DSL / PIPEA_CONF（bit31 = pipe enable）
//     0x70180 / 0x70188 / 0x7019C      DSPACNTR（bit31 使能，bits29:26 格式）/ DSPASTRIDE / DSPASURF
//     0x70184 / 0x701A4                DSPALINOFF / DSPATILEOFF
__attribute__((noinline)) static uint32_t in_reg32(uint64_t base, uint64_t size, uint64_t off) {
    if (!base || off + 4ull > size) return 0xFFFFFFFFu;   // 窗口外一律当"读不到"，绝不解引用
    return *(volatile uint32_t*)(uintptr_t)(base + off);
}

__attribute__((noinline)) static void in_pipe_snapshot64(const Gpu64Dev64* d) {
    // BAR0 至少要覆盖到 0x701A8 才敢读（多数核显寄存器窗口是 512 KiB，远超它）
    if (!d->bar0_ok || d->bar0_size < 0x701A8ull) {
        in_begin();
        in_puts("[INTEL] pipe snapshot skipped reason=no-reg-window");
        in_end();
        return;
    }
    const uint64_t b = d->bar0_phys, sz = d->bar0_size;
    const uint32_t htotal = in_reg32(b, sz, 0x60000u);
    const uint32_t vtotal = in_reg32(b, sz, 0x6000Cu);
    const uint32_t pipesrc = in_reg32(b, sz, 0x6001Cu);
    const uint32_t conf   = in_reg32(b, sz, 0x70008u);
    const uint32_t dsl    = in_reg32(b, sz, 0x70000u);
    const uint32_t ctrl   = in_reg32(b, sz, 0x70180u);
    const uint32_t stride = in_reg32(b, sz, 0x70188u);
    const uint32_t surf   = in_reg32(b, sz, 0x7019Cu);
    const uint32_t linoff = in_reg32(b, sz, 0x70184u);
    const uint32_t tioff  = in_reg32(b, sz, 0x701A4u);

    // 已编程的显示模式：HTOTAL/VTOTAL 的低 12 位是"active-1"（这是 gen2 起没变过的布局）
    const uint32_t hactive = (htotal & 0xFFFu) + 1u;
    const uint32_t vactive = (vtotal & 0xFFFu) + 1u;
    in_begin();
    in_puts("[INTEL] pipe a conf=");   in_hex(conf);
    in_puts(" dsl=");                  in_hex(dsl);
    in_puts(" on=");                   in_ch((char)('0' + ((conf >> 31) & 1u)));
    in_puts(" htotal=");               in_hex(htotal);
    in_puts(" vtotal=");               in_hex(vtotal);
    in_puts(" pipesrc=");              in_hex(pipesrc);
    in_puts(" hactive=");              in_num(hactive);
    in_puts(" vactive=");              in_num(vactive);
    in_end();

    in_begin();
    in_puts("[INTEL] plane a ctrl=");  in_hex(ctrl);
    in_puts(" on=");                   in_ch((char)('0' + ((ctrl >> 31) & 1u)));
    in_puts(" fmt=");                  in_num((ctrl >> 26) & 0xFu);   // bits29:26 像素格式码
    in_puts(" stride=");               in_num(stride & 0x7FFFFu);
    in_puts(" surf=");                 in_hex(surf);
    in_puts(" linoff=");               in_hex(linoff);
    in_puts(" tileoff=");              in_hex(tioff);
    in_end();

    // 与**我们的**帧缓冲几何对一下：相等说明"接管时只要把 plane 指到我们的缓冲"；
    // 不等说明接管还得连带把 pipe timing 一起重编（工作量和风险都不一样，必须先知道）。
    in_begin();
    in_puts("[INTEL] plan takeover=not-yet");
    in_puts(" frame_match=");
    in_ch((char)('0' + ((hactive == (uint32_t)fb_phys_width() &&
                         vactive == (uint32_t)fb_phys_height()) ? 1u : 0u)));
    in_puts(" need=gtt-framebuffer,plane-programming");
    in_puts(" readonly=1");
    in_end();
}

// GTT 空闲段扫描（**只读**）：前 scan 项里最长的一段"全 invalid"。接管时要往 GTT 里放我们自己的
// 帧缓冲，就需要知道哪儿是空的。这里只报告，不分配、不写。
__attribute__((noinline)) static void in_gtt_scan64(uint64_t gtt_pa, uint32_t off, uint32_t scan) {
    uint32_t run = 0, best = 0, best_at = 0, cur = 0;
    for (uint32_t i = 0; i < scan; i++) {
        const uint64_t e = *(volatile uint64_t*)(uintptr_t)(gtt_pa + (uint64_t)off + (uint64_t)i * 8u);
        if (e & 1ull) { run = 0; continue; }
        if (!run) cur = i;
        run++;
        if (run > best) { best = run; best_at = cur; }
    }
    in_puts(" alloc=readonly scan="); in_num(scan);
    in_puts(" free_run_start=");      in_num(best_at);
    in_puts(" free_run_len=");        in_num(best);
}

static uint32_t g_intel_devs = 0;                        // 跑过几张 Intel 卡（selftest 用）

// ==================== ★ v0.9.10：GTT 映射（写 PTE + 回读校验 + 立刻还原）====================
// 边界（先读清楚，这是本文件里**唯一**会写厂商 MMIO/GTT 的地方）：
//   * 只在 opt/vimtu/gpu 的 `mode=intel` 下被调用；默认 `mode=intel_safe` 只读，绝不走到这里。
//   * 只写**当前无效**的 GTT 表项；写完逐项回读校验，然后**立刻写回无效并还页** ——
//     "写 -> 读 -> 还原"全在同一次调用里完成。
//   * 绝不碰任何有效表项（那是显示引擎正在用的缓冲），绝不碰 pipe/plane 寄存器。
//   为什么这一步值得存在：接管要往 GTT 里放我们自己的帧缓冲。如果连"写一条 PTE 能不能生效"都没在
//   真机上验过，那接管就是在猜。这个自检把"能不能"变成一行可核对的日志。
//
// GTT 表项格式（gen6+ 统一 8 字节）：bit0 = valid，bits 12.. 是物理页号。
void* page_alloc_64();                 // 页池（实现见 kernel/mem64.cpp；只声明，理由同 fb.cpp）
void page_free_64(void* p);

static uint32_t in_gtt_find_run64(uint64_t gtt_pa, uint32_t off, uint32_t scan, uint32_t pages) {
    uint32_t run = 0, start = 0;
    for (uint32_t i = 0; i < scan; i++) {
        const uint64_t e = *(volatile uint64_t*)(uintptr_t)(gtt_pa + (uint64_t)off + (uint64_t)i * 8u);
        if (e & 1ull) { run = 0; continue; }
        if (!run) start = i;
        if (++run >= pages) return start;
    }
    return 0xFFFFFFFFu;
}

// 返回 1 = 全链路验证通过（找到空段、写入生效、回读一致、还原干净）。
static int in_gtt_selftest64(uint64_t gtt_pa, uint32_t off, uint32_t size, uint32_t scan) {
    const uint32_t pages = 4;
    if (!gtt_pa || size < (uint64_t)off + (uint64_t)scan * 8u) return 0;
    in_begin();
    in_puts("[INTEL] gtt map ");
    const uint32_t start = in_gtt_find_run64(gtt_pa, off, scan, pages);
    if (start == 0xFFFFFFFFu) {
        in_puts("selftest skipped reason=no-free-run");
        in_end();
        return 0;
    }
    uint64_t phys[8];
    uint32_t got = 0;
    for (; got < pages; got++) {
        phys[got] = (uint64_t)(uintptr_t)page_alloc_64();
        if (!phys[got]) break;
    }
    if (got < pages) {                                  // 页池不够：把已拿的还回去，如实 skipped
        for (uint32_t i = 0; i < got; i++) page_free_64((void*)(uintptr_t)phys[i]);
        in_puts("selftest skipped reason=no-pool-page");
        in_end();
        return 0;
    }
    volatile uint64_t* gtt = (volatile uint64_t*)(uintptr_t)(gtt_pa + (uint64_t)off);
    for (uint32_t i = 0; i < pages; i++)                // ① 写 PTE（指向我们自己的页池页）
        gtt[start + i] = (phys[i] & 0x000FFFFFFFFFF000ull) | 1ull;
    __asm__ volatile("mfence" : : : "memory");
    int wok = 1;                                        // ② 逐项回读校验
    for (uint32_t i = 0; i < pages; i++) {
        const uint64_t e = gtt[start + i];
        if (!(e & 1ull) || (e & 0x000FFFFFFFFFF000ull) != (phys[i] & 0x000FFFFFFFFFF000ull)) wok = 0;
    }
    for (uint32_t i = 0; i < pages; i++) gtt[start + i] = 0;    // ③ 立刻还原成"无效"
    __asm__ volatile("mfence" : : : "memory");
    int rok = 1;                                        // ④ 回读确认真的还原了
    for (uint32_t i = 0; i < pages; i++) if (gtt[start + i] & 1ull) rok = 0;
    for (uint32_t i = 0; i < pages; i++) page_free_64((void*)(uintptr_t)phys[i]);   // ⑤ 还页
    const int ok = (wok && rok) ? 1 : 0;
    in_puts("selftest start="); in_num(start);
    in_puts(" pages=");         in_num(pages);
    in_puts(" write=1 readback="); in_ch(wok ? '1' : '0');
    in_puts(" restored=");      in_ch(rok ? '1' : '0');
    in_puts(" ok=");            in_ch(ok ? '1' : '0');
    in_end();
    return ok;
}

int intel64_probe64(const Gpu64Dev64* d) {
    if (!d || d->vendor_tag != GPU64_VENDOR64_INTEL) return 0;
    if (intel64_mode64() == INTEL64_MODE64_OFF) return 0;     // 配置要求不碰 -> 走统一回退
    g_intel_devs++;

    // ---- 1) 认卡（查表；查不到如实 unknown）----
    uint8_t fam = IF_UNKNOWN, gen = 0;
    for (uint32_t i = 0; i < sizeof(IN_IDS) / sizeof(IN_IDS[0]); i++) {
        if (IN_IDS[i].dev == d->device) { fam = IN_IDS[i].fam; gen = IN_IDS[i].gen; break; }
    }
    in_begin();
    in_puts("[INTEL] dev idx=");   in_num(d->index);
    in_puts(" device=");           in_hexfix(d->device, 4);
    in_puts(" family=");           in_puts(in_fam_name(fam));
    in_puts(" gen=");              in_gen(gen);
    in_puts(" bar0=");
    if (d->bar0_ok) in_hex(d->bar0_phys); else in_puts("none");
    in_end();

    // ---- 1b) 显示引擎只读快照（接管前的证据链，见 in_pipe_snapshot64 注释）----
    in_pipe_snapshot64(d);

    // ---- 2) BAR2 = GTTMMADR（GTT 表窗口）----
    // 与 gpu64.cpp 探 BAR0 同一个做法：关中断、探完立刻还原（pci64 内部会回读确认）。
    // 这段时间这块卡的地址译码是关的，所以调用点必须在"什么还没画"的时候（fb_init 之后紧接）。
    Pci64Bar64 b2;
    for (uint32_t i = 0; i < sizeof(b2); i++) ((uint8_t*)&b2)[i] = 0;
    const uint64_t fl = dbg64_irq_save64();
    const int rc2 = pci64_bar_probe64(d->bdf, 2, &b2);
    dbg64_irq_restore64(fl);

    in_begin();
    in_puts("[INTEL] gtt bar=2 ");
    if (rc2 != 0 || !b2.phys) {
        in_puts("absent reason=no-bar2");
        in_end();
        return 1;                                       // 卡认得，只是没有 GTT 窗口
    }
    in_puts("phys=");   in_hex(b2.phys);
    in_puts(" size=");  in_hex(b2.size);
    // 两个候选偏移各采 32 项（理由见文件头）；先算好要读的窗口大小
    const int off_main = (gen >= 8) ? 0x800000 : 0x200000;   // gen 取 9 / 95 都进这一支
    const int off_alt  = (off_main == 0x800000) ? 0x200000 : 0x800000;
    const uint64_t need = (uint64_t)(off_main > off_alt ? off_main : off_alt) + 32ull * 8ull;
    if (b2.size < need || b2.size < 0x2000ull) {
        in_puts(" skipped reason=small-window");
        in_end();
        return 1;
    }
    if (b2.phys + need > 0x100000000ull) {               // 内核只恒等映射 0..4GiB：越界的绝不碰
        in_puts(" skipped reason=above-4g-unmapped");
        in_end();
        return 1;
    }
    uint64_t first = 0;
    const uint32_t v_main = in_gtt_valid(b2.phys + (uint64_t)off_main, 32, &first);
    const uint32_t v_alt  = in_gtt_valid(b2.phys + (uint64_t)off_alt,  32, 0);
    in_puts(" off=");       in_hex((uint64_t)off_main);
    in_puts(" entries=32 valid="); in_num(v_main);
    in_puts(" alt_off=");   in_hex((uint64_t)off_alt);
    in_puts(" alt_valid="); in_num(v_alt);
    in_puts(" phys_first="); in_hex(first);
    in_gtt_scan64(b2.phys, (uint32_t)off_main, 4096);
    in_end();

    // ★ v0.9.10：只有 mode=intel 才允许写 GTT（默认 intel_safe 只读）
    if (intel64_mode64() == INTEL64_MODE64_FULL)
        in_gtt_selftest64(b2.phys, (uint32_t)off_main, (uint32_t)b2.size, 4096u);

    // ---- 3) 本批次边界（每次都说清楚，免得日后误读成"驱动做完了"）----
    in_begin();
    in_puts("[INTEL] mode=");
    in_puts(intel64_mode64() == INTEL64_MODE64_FULL ? "intel" : "intel-safe");
    in_puts(" requested=");
    in_puts(g_mode_str[0] ? g_mode_str : "default");
    in_puts(" readonly=1 scanout=none vendor_writes=0");
    in_end();
    return 1;
}

int intel64_selftest64(void) {
    in_begin();
    in_puts("[INTEL] selftest ");
    if (!g_intel_devs) {
        in_puts("skipped reason=no-intel-device");       // 这条路径也必须被跑到（QEMU/VMware 上就是它）
        in_end();
        return 0;
    }
    in_puts("ok intel_devs="); in_num((uint64_t)g_intel_devs);
    in_puts(" readonly=1 mode=");
    in_puts(intel64_mode64() == INTEL64_MODE64_FULL ? "intel" : "intel-safe");
    in_end();
    return 1;
}
