// gpu64.cpp - 通用 GPU 层（P1）：枚举 + 厂商识别 + BAR/显存窗口 + 命令寄存器 + 只读探测
//
// 这个文件负责回答一个问题：**这台机器的显卡叫什么、窗口有多大、能不能被我们碰**。
// 它不画一个像素，也不接管 scanout（边界见 gpu64.h）。三家厂商的"原生"驱动（模式设置、显存管理、
// 命令提交、2D/3D 加速）按期走：P2 = AMD（SMU/DCN，**用户手上没有 AMD 卡，已明确跳过**）、
// P3 = Intel（display engine / GTT，**本批已开初步驱动**，见 kernel/intel_gpu64.cpp）、
// P4 = NVIDIA（nouveau 路线，法律风险最高，**本批不做**）。P1 只是它们共同的地基。
//
// ==================== 为什么复用 kernel/pci64.cpp 而不是再写一份 0xCF8/0xCFC ====================
// 项目里已经有 7 份各驱动私有的 PCI 访问器（apci_/epci_/hpci_/npci_/…），互不可见；而 kernel/pci64.cpp
// 是**公共**的那一份，且已经带了我们最需要的东西：BAR 真实大小（写全 1 回读法）+ 还原回读确认 +
// 8 条结果缓存。这里直接用，不新增第二套配置空间代码（省字节，也少一处出错的地方）。
//
// ==================== 风险（如实写在最前面，不藏在实现里）====================
//   1) 探 BAR 大小的标准做法是"往 BAR 写 0xFFFFFFFF -> 回读掩码 -> 按原值回写 -> 回读确认"。
//      那几微秒里，device 的地址译码是被改掉的。本项目当前上屏走的是**同一个**显卡的 BAR0 窗口
//      （LFB），所以这段窗口里任何一次帧提交都会写丢。kernel/pci64.cpp 已经做了"立刻还原 + 回读确认"，
//      本文件再加两道：
//        * 只在 **fb_init() 之后、桌面还没开始画** 的时刻跑一次（调用点见 kernel64.cpp）；
//        * 探测期间用 dbg64_irq_save64() 关中断（单核下把"定时器里顺手刷屏"这条路彻底堵掉）。
//      真机上仍有理论竞态（SMP AP 若已在跑并且也在写 LFB）—— 这里如实记下，不假装不存在。
//   2) 厂商专用探测一律**只读**，而且只读"身份寄存器"（三家在 init 第一行都会读的那几个）：
//        Intel : BAR0+0x70008 PIPEA_CONF / BAR0+0x70000 PIPEA_DSL / BAR0+0x70180 DSPACNTR
//        AMD   : BAR0+0x00000（R_000000）/ BAR2+0x0 MM_INDEX / BAR2+0x4 MM_DATA
//        NVIDIA: BAR0+0x00000 NV_PMC_BOOT_0
//      绝不写厂商寄存器、绝不改 MM_INDEX（所以 AMD 的 MM_DATA 只是"当前索引下的值"，不作为识别依据）。
//      读回 0xFFFFFFFF 一律判 dead-mmio 并如实 skipped（设备没上电/没时钟时就是全 1）。
//   3) 打开命令寄存器的 mem-decode(bit1) + bus-master(bit2) 是**必须**的一步（P2/P3/P4 都要 DMA），
//      但它改的是设备配置。这里用 16 位写只碰 command 字，不碰 status（status 的高位是 W1C，
//      用 32 位回写会顺手清掉"检测到奇偶错/系统错"的记录）。
//
// ==================== 串口打点（纯 ASCII；新行只加在末尾，不动既有行）====================
//   [GPU] scan buses=<扫了几条总线> devices=<枚举到的 PCI 功能数> gpus=<class 0x03 数> known=<认得厂商数>
//         unknown=<未知厂商数> fb=<w>x<h> backend=vbe
//   [GPU] dev idx=0 bdf=00:02.0 vendor=1234 device=1111 name=unknown class=030000
//         bar0=0xfd000000 size=16777216 mmio=ok cmd=0x0107 mem=1 bm=1      （这一行是本机 stdvga 实测值）
//   [GPU] probe vendor=intel idx=1 pipe_conf=0x00000000 pipe_dsl=0x00000000 dspa_ctrl=0x00000000 pipe_on=0
//   [GPU] probe vendor=nvidia idx=2 boot0=0x0f0000a1 chip=0x0f0 rev=0xa1
//   [GPU] probe vendor=amd idx=3 reg0=0x00000000 bar2=0x00000000 mm_index=0x00000000 mm_data=0x00000000
//   [GPU] probe vendor=unknown idx=0 skipped reason=no-safe-probe
//   [GPU] probe vendor=nvidia idx=2 skipped reason=no-mmio|dead-mmio|small-mmio
//   [GPU] report gpus=1 known=0 unknown=1 probed=0 skipped=1 mmio_ok=1 cmd_on=1 extra=0
//         accel=none-not-implemented
//
//   ★ backend= 是**事实**而不是口味：值来自 kernel/fb.cpp 的 fb64_source_name64() ——
//     "vbe" = BIOS loader（boot/loader64.asm）设的 VBE 模式 + LFB；
//     "gop" = UEFI loader（boot/efi/uefi64.c）交来的 GOP 帧缓冲（v0.9.8 起高 32 位也一并交过来）。
//   两家的上屏路径都在 fb.cpp 里，本文件只读它、不改它。
//
//   ★ v0.9.8 新增两类行（都在既有行之后，不动老前缀）：
//     [GPU] driver vendor=<intel|amd|nvidia|unknown> idx=<n> status=safe-probe mode=readonly
//     [GPU] driver vendor=<amd|nvidia|unknown> idx=<n> status=none fallback=no-touch reason=<...>
//     [INTEL] …（见 kernel/intel_gpu64.cpp 文件头）
#include "gpu64.h"
#include "vmware64.h"          // ★ v0.9.18：VMware SVGA 只读能力探测
#include "pci64.h"
#include "intel_gpu64.h"   // ★ v0.9.8：P3 Intel 初步驱动（本文件只负责"分派"，不搬它的实现）
#include "debug64.h"
#include "fb.h"          // fb_phys_width / fb_phys_height（只用于报告行）
#include "port.h"        // inl/outl/inw/outw（0xCF8/0xCFC 配置口）

// ==================== 打点小工具 ====================
// ★ 体积纪律（与 virtio_gpu64.cpp 同款）：debug64.h 的 dbg64_str/putc 都是 static inline，每次展开
//   都带 outb + 串口自旋 + 环形缓冲 sink 调用；这里 40+ 处打点若全内联，.text 会白涨好几 KB。
//   所以日志原语一律走 __attribute__((noinline)) 包装（每个原语只保留一份实现）。
static const char* const GPU64_HEXL = "0123456789abcdef";
__attribute__((noinline)) static void ga_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void ga_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void ga_num(uint64_t v) { dbg64_dec(v); }
static void ga_begin() { dbg64_line_begin64(); }          // 一行内不被别的任务/中断插进来
static void ga_end()   { dbg64_nl(); dbg64_line_end64(); }
__attribute__((noinline)) static void ga_hex(uint64_t v) {   // "0x" + 最短十六进制
    ga_puts("0x");
    if (!v) { ga_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = GPU64_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) ga_ch(b[--n]);
}
__attribute__((noinline)) static void ga_hexfix(uint32_t v, int digits) {   // 定宽（无前缀）
    char b[8]; int n = 0;
    while (n < digits) { b[n++] = GPU64_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) ga_ch(b[--n]);
}
// BDF：bus:dev.fn（与 [VGPU] pci 行同一种写法之外的紧凑格式，方便一行里塞下更多字段）
__attribute__((noinline)) static void ga_bdf(uint32_t bdf) {
    ga_hexfix((bdf >> 8) & 0xFFu, 2); ga_ch(':');
    ga_hexfix((bdf >> 3) & 0x1Fu, 2); ga_ch('.');
    ga_ch((char)('0' + (char)(bdf & 0x7u)));
}

// ==================== 状态（全部 .bss/.data，无动态分配）====================
static Gpu64Dev64 g_dev[GPU64_MAX64];
static uint32_t   g_n = 0;            // 记录了几张卡
static uint32_t   g_extra = 0;        // 超出 GPU64_MAX64 被丢掉的额外显示设备数（如实报告）
static uint32_t   g_devs = 0;         // 枚举到的 PCI 功能数（不等于一张卡）
static uint32_t   g_buses = 0;        // 实际扫了几条总线（连续 4 条空总线提前停）
static uint32_t   g_known = 0, g_unknown = 0;
static int        g_init = 0;

// 每张卡的"跳过原因"（内部用，不进公开结构体 —— 结构体只放结论，原因只用于打点）
enum {
    GS_NONE = 0,     // 探测成功
    GS_IO,           // BAR0 是 I/O 端口 BAR（不是 MMIO，本层不碰）
    GS_ABSENT,       // BAR0 未实现 / 地址在 4GiB 恒等映射之外
    GS_SMALL,        // BAR0 窗口太小，装不下要读的寄存器
    GS_DEAD,         // 读回 0xFFFFFFFF（设备没上电/没时钟/槽位被摘）
    GS_NOPROBE       // 厂商未知 —— 没有"安全的探测寄存器"可用，如实不碰
};
static uint8_t g_skip[GPU64_MAX64];

// ==================== 配置空间小工具 ====================
static uint32_t ga_bdfv(uint32_t bus, uint32_t dev, uint32_t fn) {
    return ((bus & 0xFFu) << 8) | ((dev & 0x1Fu) << 3) | (fn & 0x7u);
}

// 16 位写命令寄存器。pci64 只提供 32 位写，而 0x04 的**高 16 位是 status**（几个位是 W1C）：
// 用 32 位回写会顺手把"检测到奇偶错/系统错/目标中止"的记录清掉。所以这里单独走一次 0xCFC 的
// 16 位写（只落在 command 字的低 16 位），既不动 status，也不需要给 pci64 加接口。
static void ga_cfg_wr16(uint32_t bdf, uint8_t off, uint16_t v) {
    const uint32_t addr = 0x80000000u | (((bdf >> 8) & 0xFFu) << 16) | (((bdf >> 3) & 0x1Fu) << 11)
                        | ((bdf & 0x7u) << 8) | (uint32_t)(off & 0xFCu);
    outl(0xCF8u, addr);
    outw(0xCFCu, v);
}

// BAR 的**基址**（只看地址位，不写配置空间 -> 不改译码）。I/O BAR / 空槽返回 0。
static uint64_t ga_bar_base(uint32_t bdf, uint8_t bar) {
    const uint8_t o = (uint8_t)(0x10u + 4u * bar);
    const uint32_t lo = pci64_cfg_rd32(bdf, o);
    if ((lo & 1u) || !(lo & ~0x0Fu)) return 0;                       // I/O BAR / 空槽
    uint64_t base = (uint64_t)(lo & ~0x0Fu);
    if (((lo >> 1) & 0x3u) == 0x2u) {                                // 64 位 BAR：高 32 位一起读
        base |= ((uint64_t)pci64_cfg_rd32(bdf, (uint8_t)(o + 4u)) << 32);
    }
    return base;
}

static inline uint32_t ga_rd32(uint64_t pa) {
    return *(volatile uint32_t*)(uintptr_t)pa;                       // 0..4GiB 恒等映射（memlayout64.h）
}

// ==================== 厂商分类 ====================
static uint8_t ga_vclass(uint16_t pci_vendor) {
    switch (pci_vendor) {
        case 0x8086u: return GPU64_VENDOR64_INTEL;
        case 0x1002u:                                                // AMD/ATI 图形
        case 0x1022u: return GPU64_VENDOR64_AMD;                     // AMD 同门（芯片组/APU 侧）
        case 0x10DEu: return GPU64_VENDOR64_NVIDIA;
        default:      return GPU64_VENDOR64_UNKNOWN;
    }
}

const char* gpu64_vendor_name64(uint16_t pci_vendor) {
    switch (ga_vclass(pci_vendor)) {
        case GPU64_VENDOR64_INTEL:  return "intel";
        case GPU64_VENDOR64_AMD:    return "amd";
        case GPU64_VENDOR64_NVIDIA: return "nvidia";
        default:                    return "unknown";
    }
}

// ==================== 单张卡：BAR + 命令寄存器 + 厂商只读探测（只记录，不打点）====================
static void ga_probe64(Gpu64Dev64* p) {
    g_skip[p->index] = GS_NONE;

    // ---- BAR0：真实窗口大小（写全 1 回读法，pci64 内部已做"立刻还原 + 回读确认 + 缓存"）----
    // 关中断的理由见文件头"风险 1"：探窗口的那几微秒里这块卡的地址译码是关的。
    {
        Pci64Bar64 bar;
        for (uint32_t i = 0; i < sizeof(bar); i++) ((uint8_t*)&bar)[i] = 0;   // 不用 memset（-nostdlib）
        const uint64_t fl = dbg64_irq_save64();
        const int rc = pci64_bar_probe64(p->bdf, 0, &bar);
        dbg64_irq_restore64(fl);
        if (rc == 0) {
            p->bar0_phys = bar.phys;
            p->bar0_size = bar.size;
            p->bar0_64   = bar.is_64;
            p->bar0_ok   = 1;
        } else if (rc == PCI64_EINVAL64) {
            p->bar0_io = 1;
            g_skip[p->index] = GS_IO;
        } else {
            g_skip[p->index] = GS_ABSENT;                             // 未实现 / 在 4GiB 之外
        }
    }

    // ---- 命令寄存器：mem-decode(bit1) + bus-master(bit2) ----
    // 不开 mem-decode，MMIO 窗口根本不应答；不开 bus-master，P2~P4 的任何 DMA 都会失败。
    {
        const uint16_t cmd0 = pci64_cfg_rd16(p->bdf, 0x04u);
        const uint16_t want = (uint16_t)(cmd0 | 0x0006u);
        if (want != cmd0) ga_cfg_wr16(p->bdf, 0x04u, want);
        p->cmd_word = pci64_cfg_rd16(p->bdf, 0x04u);                   // 回读确认（不信"写成功了"）
        p->cmd_ok   = ((p->cmd_word & 0x0006u) == 0x0006u) ? 1 : 0;
    }

    // ★ v0.9.18：VMware SVGA II（vendor 0x15AD）—— 它的寄存器常在 **I/O 端口**（BAR0 = I/O BAR），
    //   所以这条分支必须放在"没有 MMIO 窗口就 return"**之前**。探测只读（唯一一次写是 SVGA_REG_ID
    //   的版本协商，不影响显示），并在串口打一行 [VMWARE] svga …（含 3D 能力位）。
    if (p->vendor == 0x15ADu) {
        const uint32_t b0 = pci64_cfg_rd32(p->bdf, 0x10u);
        const int is_io = (b0 & 1u) ? 1 : 0;
        const uint64_t base = (uint64_t)(is_io ? (b0 & ~0x3u) : (b0 & ~0x0Fu));
        p->probe_ok = vmware64_svga_probe64(p->bdf, is_io, base) ? 1 : 0;
        if (!p->probe_ok) g_skip[p->index] = GS_NOPROBE;
        return;
    }

    // ---- 厂商专用只读探测 ----
    if (!p->bar0_ok) {                                                // 没有可解引用的 MMIO 窗口
        if (p->vendor_tag == GPU64_VENDOR64_UNKNOWN) g_skip[p->index] = GS_NOPROBE;
        return;
    }
    switch (p->vendor_tag) {
        case GPU64_VENDOR64_INTEL: {
            // 显示引擎的 pipe/plane 状态（i915 模式设置时读的就是这几个；只读绝对安全）。
            // 0x70008 = PIPEA_CONF（bit31 = pipe enable）、0x70000 = PIPEA_DSL（扫描行号）、
            // 0x70180 = DSPACNTR（主 plane 控制，bit31 = plane enable）。
            if (p->bar0_size < 0x70400ull) { g_skip[p->index] = GS_SMALL; return; }
            const uint32_t conf = ga_rd32(p->bar0_phys + 0x70008u);
            if (conf == 0xFFFFFFFFu) { g_skip[p->index] = GS_DEAD; return; }
            p->probe0 = conf;
            p->probe1 = ga_rd32(p->bar0_phys + 0x70000u);
            p->probe2 = ga_rd32(p->bar0_phys + 0x70180u);
            p->probe_ok = 1;
            break;
        }
        case GPU64_VENDOR64_AMD: {
            // BAR0 = 寄存器窗口（R_000000 起）。BAR2 = MM_INDEX/MM_DATA 间接窗口。
            // ★ 只读，不改 MM_INDEX —— 所以 mm_data 只是"当前索引下的值"，不作为识别依据。
            const uint32_t reg0 = ga_rd32(p->bar0_phys);
            if (reg0 == 0xFFFFFFFFu) { g_skip[p->index] = GS_DEAD; return; }
            p->probe0 = reg0;
            const uint64_t mm = ga_bar_base(p->bdf, 2);
            if (mm && mm < 0x100000000ull) {
                p->bar2_phys = mm;
                p->probe1 = ga_rd32(mm + 0x0u);                        // MM_INDEX（只读当前值）
                p->probe2 = ga_rd32(mm + 0x4u);                        // MM_DATA（当前索引下的值）
            }
            p->probe_ok = 1;
            break;
        }
        case GPU64_VENDOR64_NVIDIA: {
            // NV_PMC_BOOT_0 @ BAR0+0x000000：卡一上电就能读的身份寄存器（nouveau 的第一行）。
            // chipset = (boot0 & 0x1ff00000) >> 20，revision = boot0 & 0xff。
            const uint32_t boot0 = ga_rd32(p->bar0_phys);
            if (boot0 == 0xFFFFFFFFu) { g_skip[p->index] = GS_DEAD; return; }
            p->probe0 = boot0;
            p->probe_ok = 1;
            break;
        }
        default:
            g_skip[p->index] = GS_NOPROBE;                             // 未知厂商：绝不乱读它的寄存器
            break;
    }
}

// ==================== 枚举 ====================
static void ga_record64(uint32_t bdf, uint16_t vendor, uint16_t device, uint32_t cls) {
    if (g_n >= (uint32_t)GPU64_MAX64) { g_extra++; return; }           // 如实记账，不静默丢
    Gpu64Dev64* p = &g_dev[g_n];
    for (uint32_t i = 0; i < sizeof(*p); i++) ((uint8_t*)p)[i] = 0;
    p->bdf    = bdf;
    p->vendor = vendor;
    p->device = device;
    p->class_reg = cls;
    p->index  = (uint8_t)g_n;
    p->vendor_tag = ga_vclass(vendor);
    p->known  = (p->vendor_tag != GPU64_VENDOR64_UNKNOWN) ? 1 : 0;
    if (p->known) g_known++; else g_unknown++;
    ga_probe64(p);                                                     // 只记录；打点在下面统一输出
    g_n++;
}

void gpu64_init64(void) {
    if (g_init) return;
    g_init = 1;

    // ---- 1) 扫 class 0x03：bus 0..255 × dev 0..31 × fn 0..7（能连续 4 条空总线就提前停）----
    uint32_t empties = 0;
    for (uint32_t bus = 0; bus < 256; bus++) {
        int bus_has = 0;
        for (uint32_t d = 0; d < 32; d++) {
            const uint32_t bdf0 = ga_bdfv(bus, d, 0);
            const uint32_t id0  = pci64_cfg_rd32(bdf0, 0x00u);
            const uint16_t ven0 = (uint16_t)(id0 & 0xFFFFu);
            if (ven0 == 0xFFFFu || ven0 == 0x0000u) continue;
            bus_has = 1;
            const uint32_t hdr = pci64_cfg_rd32(bdf0, 0x0Cu);
            const uint32_t nfn = (hdr & 0x00800000u) ? 8u : 1u;        // 配置空间 0x0E bit7 = 多功能
            for (uint32_t fn = 0; fn < nfn; fn++) {
                const uint32_t bdf = ga_bdfv(bus, d, fn);
                uint32_t id = id0;
                if (fn) {
                    id = pci64_cfg_rd32(bdf, 0x00u);
                    const uint16_t v = (uint16_t)(id & 0xFFFFu);
                    if (v == 0xFFFFu || v == 0x0000u) continue;
                }
                g_devs++;
                const uint32_t cls = pci64_cfg_rd32(bdf, 0x08u);
                if (((cls >> 24) & 0xFFu) != 0x03u) continue;          // 只要显示控制器（VGA/3D/其它）
                ga_record64(bdf, (uint16_t)(id & 0xFFFFu), (uint16_t)(id >> 16), cls);
            }
        }
        g_buses = bus + 1;
        if (bus_has) { empties = 0; }
        else if (++empties >= 4u) break;
    }

    // ---- 2) 一行汇总（先出，再出每张卡的细节；与 [VGPU]/[HWUI] 的"先报告后细节"习惯一致）----
    ga_begin();
    ga_puts("[GPU] scan buses="); ga_num(g_buses);
    ga_puts(" devices=");         ga_num(g_devs);
    ga_puts(" gpus=");            ga_num(g_n);
    ga_puts(" known=");           ga_num(g_known);
    ga_puts(" unknown=");         ga_num(g_unknown);
    ga_puts(" fb=");              ga_num((uint64_t)fb_phys_width()); ga_ch('x'); ga_num((uint64_t)fb_phys_height());
    // ★ v0.9.8：如实写**这次真的用的**上屏路径（vbe = BIOS loader 设的 VBE 模式 /
    //   gop = UEFI loader 交来的 GOP 帧缓冲）。原来这里硬编码 vbe —— 接了 GOP 之后
    //   那句话就成了假话，而真机取证最需要的恰恰是"这台机器走的是哪条路"。
    ga_puts(" backend="); ga_puts(fb64_source_name64());
    ga_end();

    // ---- 3) 每张卡：身份 + 窗口 + 命令寄存器，然后一条厂商探测结论 ----
    uint32_t probed = 0, skipped = 0, mmio_ok = 0, cmd_on = 0;
    for (uint32_t i = 0; i < g_n; i++) {
        const Gpu64Dev64* p = &g_dev[i];
        if (p->bar0_ok) mmio_ok++;
        if (p->cmd_ok)  cmd_on++;
        if (p->probe_ok) probed++; else skipped++;

        ga_begin();
        ga_puts("[GPU] dev idx=");  ga_num(p->index);
        ga_puts(" bdf=");           ga_bdf(p->bdf);
        ga_puts(" vendor=");        ga_hexfix(p->vendor, 4);
        ga_puts(" device=");        ga_hexfix(p->device, 4);
        ga_puts(" name=");          ga_puts(gpu64_vendor_name64(p->vendor));
        // class 码 = (class<<16)|(subclass<<8)|prog-if。配置空间 0x08 的原始 dword 是**反过来的**
        //   （rev | prog-if<<8 | subclass<<16 | class<<24），所以要右移 8 位再取低 24 位：
        //   0x03000002 -> 0x030000。★ 第一版这里漏了右移，打出来是 class=000002（实测踩到）。
        ga_puts(" class=");         ga_hexfix((p->class_reg >> 8) & 0xFFFFFFu, 6);
        if (p->bar0_ok) {
            ga_puts(" bar0=");      ga_hex(p->bar0_phys);
            ga_puts(" size=");      ga_num(p->bar0_size);
            ga_puts(" mmio=ok");
        } else {
            ga_puts(" bar0=0x0 size=0 mmio=");
            ga_puts(p->bar0_io ? "io" : "skip");                   // I/O BAR 与"没有窗口"分开写
        }
        ga_puts(" cmd=0x");         ga_hexfix(p->cmd_word, 4);
        ga_puts(" mem=");           ga_ch((char)('0' + ((p->cmd_word >> 1) & 1u)));   // bit1 = MEM decode
        ga_puts(" bm=");            ga_ch((char)('0' + ((p->cmd_word >> 2) & 1u)));   // bit2 = bus master
        ga_end();

        ga_begin();
        ga_puts("[GPU] probe vendor="); ga_puts(gpu64_vendor_name64(p->vendor));
        ga_puts(" idx=");               ga_num(p->index);
        if (!p->probe_ok) {
            ga_puts(" skipped reason=");
            switch (g_skip[i]) {
                case GS_IO:    ga_puts("io-bar"); break;
                case GS_SMALL: ga_puts("small-mmio"); break;
                case GS_DEAD:  ga_puts("dead-mmio"); break;
                case GS_NOPROBE: ga_puts("no-safe-probe"); break;
                default:       ga_puts("no-mmio"); break;
            }
            ga_end();
            continue;
        }
        if (p->vendor_tag == GPU64_VENDOR64_INTEL) {
            ga_puts(" pipe_conf="); ga_hex(p->probe0);
            ga_puts(" pipe_dsl=");  ga_hex(p->probe1);
            ga_puts(" dspa_ctrl="); ga_hex(p->probe2);
            ga_puts(" pipe_on=");   ga_ch((char)('0' + ((p->probe0 >> 31) & 1u)));
        } else if (p->vendor_tag == GPU64_VENDOR64_NVIDIA) {
            const uint32_t boot0 = p->probe0;
            ga_puts(" boot0=");     ga_hex(boot0);
            ga_puts(" chip=");      ga_hex((boot0 >> 20) & 0x1FFu);    // nouveau 的 chipset 取法
            ga_puts(" rev=");       ga_hex(boot0 & 0xFFu);
        } else {                                                       // AMD
            ga_puts(" reg0=");      ga_hex(p->probe0);
            ga_puts(" bar2=");      ga_hex(p->bar2_phys);
            ga_puts(" mm_index=");  ga_hex(p->probe1);
            ga_puts(" mm_data=");   ga_hex(p->probe2);
        }
        ga_end();
    }

    // ---- 4) 报告（accel=none-not-implemented 是**结论**，不是占位符：P1 不含任何加速）----
    ga_begin();
    ga_puts("[GPU] report gpus=");   ga_num(g_n);
    ga_puts(" known=");              ga_num(g_known);
    ga_puts(" unknown=");            ga_num(g_unknown);
    ga_puts(" probed=");             ga_num(probed);
    ga_puts(" skipped=");            ga_num(skipped);
    ga_puts(" mmio_ok=");            ga_num(mmio_ok);
    ga_puts(" cmd_on=");             ga_num(cmd_on);
    ga_puts(" extra=");              ga_num(g_extra);
    ga_puts(" accel=none-not-implemented");
    ga_end();

    // ---- 5) ★ v0.9.8：驱动分派 + **统一的安全回退**（用户要求 4/5 的代码落点）----
    // 只有 Intel 有初步驱动（P3：只读探测 + GTT 采样）。**AMD / NVIDIA / 未知厂商走同一段代码**：
    // 不写一个厂商寄存器、不接管 scanout、上屏继续走既有那条路（VBE 或 GOP）。
    // 这条纪律就是"绝对不黑屏"——没有专属驱动时我们**什么都不动**。
    //   ★ 特意不为 NVIDIA 和 AMD 各写一份：用户的要求就是两家逻辑一模一样；写两份迟早会走偏。
    for (uint32_t i = 0; i < g_n; i++) {
        const Gpu64Dev64* p = &g_dev[i];
        const int handled = (p->vendor_tag == GPU64_VENDOR64_INTEL) ? intel64_probe64(p) : 0;
        ga_begin();
        ga_puts("[GPU] driver vendor="); ga_puts(gpu64_vendor_name64(p->vendor));
        ga_puts(" idx=");                ga_num(p->index);
        if (handled) {
            ga_puts(" status=safe-probe mode=readonly");      // P3 初版：只读，不接管显示
        } else {
            ga_puts(" status=none fallback=no-touch reason=");
            if (p->vendor_tag == GPU64_VENDOR64_UNKNOWN)    ga_puts("unknown-vendor");
            else if (p->vendor_tag == GPU64_VENDOR64_INTEL) ga_puts("mode-off");
            else                                            ga_puts("no-dedicated-driver");
        }
        ga_end();
    }
    // 自检：没有 Intel 设备时也要留下"这条路径被走到了"的证据（真机取证要能区分
    //   "没有核显"与"驱动根本没跑"）。
    intel64_selftest64();
}

// ==================== 对外查询 ====================
int gpu64_count64(void) { return (int)g_n; }
int gpu64_known_count64(void) { return (int)g_known; }
int gpu64_unknown_count64(void) { return (int)g_unknown; }

const Gpu64Dev64* gpu64_dev64(int idx) {
    if (idx < 0 || (uint32_t)idx >= g_n) return nullptr;
    return &g_dev[idx];
}
