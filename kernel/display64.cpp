// display64.cpp - 运行期显示层（实现；设计与边界见 display64.h）
//
// 实现要点（与 32 位 display.cpp 的对应关系逐条写在 display64.h）：
//   * 模式清单来自 BootInfo（物理 0x1000）+ 模式表（物理 0x7400）。内核跑在高半区，
//     但引导层保留了前 4GB 恒等映射（ML64_IDENTITY_BYTES），所以这两个物理地址可直接当指针。
//   * 0x3DA 实测：VGA 输入状态寄存器 1 的 bit3 = 垂直回扫中。密集采样数 **0->1 上升沿**，
//     窗口 = 63 个 tick（PIT 250Hz -> 252ms），样本数有硬上限（PIT 万一不走也不会死循环）。
//     只接受 40..200Hz 的边沿率 —— QEMU 的 stdvga 每次读 0x3DA 都翻转状态位，边沿率 = 读速率，
//     会被这一段**如实拒绝**（绝不当成刷新率报出去）。
//   * CRTC 推算（0x3CC + 0x3D4/0x3D5）作为 0x3DA 不可测时的备用；必须先过"CRTC 显示尺寸 ==
//     当前实际分辨率"的自洽检查（QEMU 在 VBE 直帧缓冲模式下不按时序编程 CRTC，会被拒绝）。
//   * EDID 首选时序来自 kernel/edid64.cpp（引导期 EDID）。实测值/采用值与 EDID 的 ±5% 对比
//     结果打 match=0|1。
//   * 运行期 DDC 再探测：**不做**（理由见头文件），只打一行 skipped。
#include "display64.h"
#include "port.h"
#include "debug64.h"
#include "x86_64.h"        // g_ticks64 / PIT_HZ_64
#include "memlayout64.h"   // 恒等映射说明
#include "fb.h"            // fb_phys_width/height（BootInfo 不可用时的退回口径）
#include "edid64.h"        // EDID 首选时序（只读）
#include "../bootinfo.h"   // BootInfo / BootMode / BOOT_INFO_ADDR / BOOT_MODE_LIST / BOOT_MODE_MAX

// ==================== 状态 ====================
static Disp64Mode g_modes[DISP64_MODE_MAX];
static Disp64Info g_info;                     // 静态零初始化 -> 未探测时 src=none/refresh=0

#define DISP64_CAL_TICKS   63u                 // 密集标定窗口 = 63 tick = 252ms（PIT 250Hz）
#define DISP64_CAL_MAX     2000000             // 样本上限（防"PIT 不走"时死循环）
#define DISP64_HZ_MIN_X10  400                 // 接受下限 40.0Hz
#define DISP64_HZ_MAX_X10  2000                // 接受上限 200.0Hz

// ==================== 小工具（无 libc） ====================

// ±5% 容差比较（任一为 0 = 无法比较 -> 0）
// ±5% 容差比较（任一为 0 = 无法比较 -> 0）。自检用合成样本离线验证，不依赖平台。
static int display64_match_x10(int a_x10, int b_x10) {
    if (a_x10 <= 0 || b_x10 <= 0) return 0;
    const int lo = b_x10 - b_x10 / 20;         // -5%
    const int hi = b_x10 + b_x10 / 20;         // +5%
    return (a_x10 >= lo && a_x10 <= hi) ? 1 : 0;
}

// ==================== 小工具（无 libc） ====================
const char* display64_src_name64(int src) {
    switch (src) {
        case DISP64_SRC_VGA:  return "vga";
        case DISP64_SRC_CRTC: return "crtc";
        case DISP64_SRC_EDID: return "edid";
        default:              return "none";
    }
}

static inline int d64_vretrace_bit() { return (inb(0x3DA) >> 3) & 1; }

static int d64_fmt_hz(char* out, int cap, int x10) {
    // "60.0"（无单位）。cap < 6 时返回 0 并写不进就写空串。
    if (cap < 6) { if (cap > 0) out[0] = 0; return 0; }
    int ip = x10 / 10;
    char tmp[8];
    int n = 0;
    if (ip == 0) tmp[n++] = '0';
    while (ip > 0 && n < 7) { tmp[n++] = (char)('0' + ip % 10); ip /= 10; }
    int i = 0;
    while (n > 0) out[i++] = tmp[--n];
    out[i++] = '.';
    out[i++] = (char)('0' + (x10 % 10));
    out[i] = 0;
    return 1;
}

// ==================== 0x3DA 实测 ====================
// 返回 0x3DA 上升沿率 ×10（不可测时返回 0）；*samples / *edges 回填诊断值。
static int d64_measure_vretrace64(int* samples, int* edges) {
    const uint32_t t0 = (uint32_t)g_ticks64;
    int n = 0, e = 0;
    int last = d64_vretrace_bit();
    while ((uint32_t)(g_ticks64 - t0) < DISP64_CAL_TICKS) {
        const int b = d64_vretrace_bit();
        n++;
        if (b && !last) e++;
        last = b;
        if (n >= DISP64_CAL_MAX) break;        // 安全网：PIT 不走也不死循环
    }
    if (samples) *samples = n;
    if (edges)   *edges   = e;
    const uint32_t ms = ((uint32_t)(g_ticks64 - t0) * 1000u) / (uint32_t)PIT_HZ_64;
    if (ms == 0) return 0;
    const int hz10 = (int)(((uint32_t)e * 10000u) / ms);
    if (hz10 < DISP64_HZ_MIN_X10 || hz10 > DISP64_HZ_MAX_X10) return 0;   // 如实拒绝不可信边沿率
    return hz10;
}

// ==================== CRTC 时序推算（备用路径） ====================
// 刷新率 = 像素时钟 / (水平总周期 × 垂直总周期)；三个量都从视频控制器寄存器读出。
// 必须先通过"CRTC 显示尺寸 == 当前实际分辨率"的自洽检查，否则说明适配器没按时序编程 CRTC。
static int d64_crtc_refresh_x10(int* out_clk_khz, int* out_ht, int* out_vt) {
    static const int tab[4] = { 25175, 28322, 40000, 50000 };  // 标准 VGA 时钟表（kHz）
    if (g_info.w < 320 || g_info.h < 200) return 0;

    const int misc = inb(0x3CC);
    int clk = tab[(misc >> 2) & 3];
    outb(0x3D4, 0x0C);
    if (inb(0x3D5) & 0x04) clk /= 2;                           // 8 点时钟
    outb(0x3D4, 0x01);
    const int hdisp = (inb(0x3D5) + 1) * 8;
    outb(0x3D4, 0x12);
    const int vdisp_raw = inb(0x3D5);
    outb(0x3D4, 0x07);
    const int ov = inb(0x3D5);
    const int vdisp = (vdisp_raw | ((ov & 0x02) << 7) | ((ov & 0x40) << 3)) + 1;
    if (hdisp > g_info.w + 8 || hdisp < g_info.w - 8) return 0;  // 自洽检查
    if (vdisp > g_info.h + 8 || vdisp < g_info.h - 8) return 0;

    outb(0x3D4, 0x00);
    const int ht = inb(0x3D5) + 5;
    outb(0x3D4, 0x06);
    const int vt_raw = inb(0x3D5);
    const int vt = (vt_raw | ((ov & 0x01) << 8) | ((ov & 0x20) << 4)) + 2;
    // ★ 第二道自洽检查（实测踩到）：总周期必须**大于**有效显示尺寸，否则寄存器里是另一套
    //   模式（QEMU stdvga 在 VBE 直帧缓冲模式下就出现过 htotal*8 < hdisp 的组合 —— 那时
    //   推算出来的 78.8Hz 是假的）。这里如实拒绝，宁可用 EDID/未知。
    if (ht <= 0 || vt <= 0) return 0;
    if (ht * 8 <= hdisp || vt <= vdisp) return 0;
    if (out_clk_khz) *out_clk_khz = clk;
    if (out_ht)      *out_ht = ht;
    if (out_vt)      *out_vt = vt;
    const uint32_t den = (uint32_t)ht * 8u * (uint32_t)vt;
    if (den == 0) return 0;
    const int r = (int)(((uint32_t)clk * 10000u) / den);
    if (r < DISP64_HZ_MIN_X10 || r > DISP64_HZ_MAX_X10) return 0;
    return r;
}

// ==================== 模式清单 ====================
static int d64_parse_modes64(const BootInfo* bi) {
    int n = 0;
    if (!bi || bi->magic != BOOT_INFO_MAGIC || bi->mode_count == 0) return 0;
    int want = (int)bi->mode_count;
    if (want > DISP64_MODE_MAX) want = DISP64_MODE_MAX;
    const uint8_t* raw = (const uint8_t*)(uintptr_t)BOOT_MODE_LIST;   // 引导层写下的 8B/项
    for (int i = 0; i < want; i++) {
        const uint8_t* p = raw + (uint32_t)i * 8u;
        Disp64Mode m;
        m.mode   = (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
        m.width  = (uint16_t)((uint16_t)p[2] | ((uint16_t)p[3] << 8));
        m.height = (uint16_t)((uint16_t)p[4] | ((uint16_t)p[5] << 8));
        m.bpp    = p[6];
        m.pad    = p[7];
        if (m.width < 320 || m.height < 200 || m.bpp < 8) continue;    // 明显无效的项不进清单
        g_modes[n++] = m;
    }
    return n;
}

// ==================== 探测（init / reprobe 共用） ====================
static void d64_measure_and_log64() {
    char hz[12];

    // 1) 0x3DA 实测
    int samples = 0, edges = 0;
    const int vga10 = d64_measure_vretrace64(&samples, &edges);
    g_info.vga_x10     = vga10;
    g_info.vga_samples = samples;
    g_info.vga_edges   = edges;
    dbg64_line_begin64();
    if (vga10 > 0) {
        d64_fmt_hz(hz, (int)sizeof(hz), vga10);
        dbg64_str("[DISP64] vga measure=");
        dbg64_str(hz);
        dbg64_str("Hz samples=");
        dbg64_dec((uint64_t)samples);
        dbg64_str(" edges=");
        dbg64_dec((uint64_t)edges);
        dbg64_nl();
        dbg64_line_end64();
        dbg64_line_begin64();
        dbg64_str("[DISP64] vga measure=");
        dbg64_str(hz);
        dbg64_str("Hz edid=");
        if (g_info.edid_x10 > 0) {
            char eh[12];
            d64_fmt_hz(eh, (int)sizeof(eh), g_info.edid_x10);
            dbg64_str(eh);
            dbg64_str("Hz match=");
            dbg64_dec((uint64_t)display64_match_x10(vga10, g_info.edid_x10));
        } else {
            dbg64_str("n/a match=0");
        }
        dbg64_nl();
        dbg64_line_end64();
    } else {
        // 如实降级：不编造刷新率。reason 说明为什么不可测（QEMU 的 0x3DA 每次读都翻转）。
        dbg64_str("[DISP64] vga measure=n/a samples=");
        dbg64_dec((uint64_t)samples);
        dbg64_str(" edges=");
        dbg64_dec((uint64_t)edges);
        dbg64_str((edges > 0 && samples > 0 && edges * 4 > samples)
                  ? " reason=0x3da-toggles-per-read (untrustworthy edge rate)\n"
                  : " reason=no-vretrace-edges\n");
        dbg64_line_end64();
    }

    // 2) CRTC 备用推算（自洽检查不过就 0）
    int clk = 0, ht = 0, vt = 0;
    g_info.crtc_x10 = d64_crtc_refresh_x10(&clk, &ht, &vt);
    if (g_info.crtc_x10 > 0) {
        d64_fmt_hz(hz, (int)sizeof(hz), g_info.crtc_x10);
        dbg64_line_begin64();
        dbg64_str("[DISP64] crtc measure=");
        dbg64_str(hz);
        dbg64_str("Hz clk=");
        dbg64_dec((uint64_t)clk);
        dbg64_str(" htotal=");
        dbg64_dec((uint64_t)ht);
        dbg64_str(" vtotal=");
        dbg64_dec((uint64_t)vt);
        dbg64_nl();
        dbg64_line_end64();
    }

    // 3) 采用值：实测 > CRTC > EDID > 未知（顺序即"实测优先于报告值"）
    g_info.refresh_x10 = 0;
    g_info.src = DISP64_SRC_NONE;
    if (g_info.vga_x10 > 0)       { g_info.refresh_x10 = g_info.vga_x10;  g_info.src = DISP64_SRC_VGA; }
    else if (g_info.crtc_x10 > 0) { g_info.refresh_x10 = g_info.crtc_x10; g_info.src = DISP64_SRC_CRTC; }
    else if (g_info.edid_x10 > 0) { g_info.refresh_x10 = g_info.edid_x10; g_info.src = DISP64_SRC_EDID; }

    // 4) 与 EDID 首选时序的对比（实测不可测时，对采用值（可能是 CRTC/EDID）与 EDID 也比一次）
    g_info.edid_match = display64_match_x10(g_info.refresh_x10, g_info.edid_x10);
}

void display64_init64() {
    const BootInfo* bi = (const BootInfo*)(uintptr_t)BOOT_INFO_ADDR;

    // 1) 当前物理模式：BootInfo 实测；magic 不对就退回 fb 物理分辨率（如实标注）
    if (bi->magic == BOOT_INFO_MAGIC && bi->width >= 320 && bi->height >= 200) {
        g_info.w        = bi->width;
        g_info.h        = bi->height;
        g_info.bpp      = bi->bpp ? bi->bpp : 32;
        g_info.pitch    = bi->pitch ? bi->pitch : (int)bi->width * 4;
        g_info.mode_num = bi->mode_num;
        g_info.from_fb  = 0;
    } else {
        g_info.w        = fb_phys_width();
        g_info.h        = fb_phys_height();
        g_info.bpp      = 32;
        g_info.pitch    = g_info.w * 4;
        g_info.mode_num = 0;
        g_info.from_fb  = 1;
    }

    // 2) 模式清单
    g_info.mode_count = d64_parse_modes64(bi);

    // 3) EDID 首选时序（只读；没数据就是 0，不编造）
    {
        const Edid64* ed = edid64_get();
        g_info.edid_x10 = (ed->valid && ed->refresh_x10 > 0) ? (int)ed->refresh_x10 : 0;
    }

    // 4) 实测 + 采用值 + 打点
    d64_measure_and_log64();

    // 5) 清单逐条 + 总数
    for (int i = 0; i < g_info.mode_count; i++) {
        dbg64_line_begin64();
        dbg64_str("[DISP64] mode list i=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" mode=0x");
        dbg64_hex64(g_modes[i].mode);
        dbg64_str(" ");
        dbg64_dec(g_modes[i].width);
        dbg64_str("x");
        dbg64_dec(g_modes[i].height);
        dbg64_str("x");
        dbg64_dec(g_modes[i].bpp);
        dbg64_nl();
        dbg64_line_end64();
    }
    dbg64_line_begin64();
    dbg64_str("[DISP64] mode list=");
    dbg64_dec((uint64_t)g_info.mode_count);
    if (g_info.mode_count == 0) dbg64_str(" (no mode table from boot loader)");
    dbg64_nl();
    dbg64_line_end64();

    // 6) init 汇总行（src 如实标注）
    {
        char hz[12];
        dbg64_line_begin64();
        dbg64_str("[DISP64] init modes=");
        dbg64_dec((uint64_t)g_info.mode_count);
        dbg64_str(" cur=");
        dbg64_dec((uint64_t)g_info.w);
        dbg64_str("x");
        dbg64_dec((uint64_t)g_info.h);
        dbg64_str("@");
        if (g_info.refresh_x10 > 0) {
            d64_fmt_hz(hz, (int)sizeof(hz), g_info.refresh_x10);
            dbg64_str(hz);
        } else {
            dbg64_str("unknown");
        }
        dbg64_str(" src=");
        dbg64_str(display64_src_name64(g_info.src));
        dbg64_str(" bpp=");
        dbg64_dec((uint64_t)g_info.bpp);
        dbg64_str(" pitch=");
        dbg64_dec((uint64_t)g_info.pitch);
        if (g_info.from_fb) dbg64_str(" (bootinfo unavailable: fb physical size)");
        dbg64_nl();
        dbg64_line_end64();
    }

    // 7) 运行期 DDC 再探测：如实跳过（理由见头文件；绝不假装做了）
    dbg64_line_begin64();
    dbg64_str("[DISP64] ddc runtime re-probe skipped (i2c bit-bang risk; boot-time EDID only)\n");
    dbg64_line_end64();

    g_info.init_done = 1;
}

void display64_reprobe64() {
    if (!g_info.init_done) { display64_init64(); return; }
    d64_measure_and_log64();
    dbg64_line_begin64();
    dbg64_str("[DISP64] reprobe cur=");
    dbg64_dec((uint64_t)g_info.w);
    dbg64_str("x");
    dbg64_dec((uint64_t)g_info.h);
    dbg64_str("@");
    if (g_info.refresh_x10 > 0) {
        char hz[12];
        d64_fmt_hz(hz, (int)sizeof(hz), g_info.refresh_x10);
        dbg64_str(hz);
    } else {
        dbg64_str("unknown");
    }
    dbg64_str(" src=");
    dbg64_str(display64_src_name64(g_info.src));
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 查询 ====================
const Disp64Info* display64_info64() { return &g_info; }
int display64_mode_count64() { return g_info.mode_count; }
const Disp64Mode* display64_mode_at64(int i) {
    if (i < 0 || i >= g_info.mode_count) return nullptr;
    return &g_modes[i];
}
int display64_refresh_str64(char* out, int cap) {
    if (!out || cap < 6) { if (out && cap > 0) out[0] = 0; return 0; }
    if (g_info.refresh_x10 <= 0) {
        const char* u = "unknown";
        int i = 0;
        while (u[i] && i + 1 < cap) { out[i] = u[i]; i++; }
        out[i] = 0;
        return 0;
    }
    return d64_fmt_hz(out, cap, g_info.refresh_x10);
}

void display64_report64() {
    char hz[12];
    dbg64_line_begin64();
    dbg64_str("[DISP64] report ");
    dbg64_dec((uint64_t)g_info.w);
    dbg64_str("x");
    dbg64_dec((uint64_t)g_info.h);
    dbg64_str("@");
    if (g_info.refresh_x10 > 0) { d64_fmt_hz(hz, (int)sizeof(hz), g_info.refresh_x10); dbg64_str(hz); }
    else                        dbg64_str("unknown");
    dbg64_str(" src=");
    dbg64_str(display64_src_name64(g_info.src));
    dbg64_str(" vga=");
    if (g_info.vga_x10 > 0) { d64_fmt_hz(hz, (int)sizeof(hz), g_info.vga_x10); dbg64_str(hz); }
    else                    dbg64_str("n/a");
    dbg64_str(" crtc=");
    if (g_info.crtc_x10 > 0) { d64_fmt_hz(hz, (int)sizeof(hz), g_info.crtc_x10); dbg64_str(hz); }
    else                     dbg64_str("n/a");
    dbg64_str(" edid=");
    if (g_info.edid_x10 > 0) { d64_fmt_hz(hz, (int)sizeof(hz), g_info.edid_x10); dbg64_str(hz); }
    else                     dbg64_str("n/a");
    dbg64_str(" match=");
    dbg64_dec((uint64_t)g_info.edid_match);
    dbg64_str(" modes=");
    dbg64_dec((uint64_t)g_info.mode_count);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 自检 ====================
// 位含义：1 = 模式清单条数越界 / 2 = 当前分辨率不合理 / 4 = 刷新率与来源不自洽 /
//         8 = ±5% 对比逻辑（合成样本）/ 16 = 模式表条目字段不合理
int display64_selftest64() {
    if (!g_info.init_done) display64_init64();
    int fail = 0;

    if (g_info.mode_count < 0 || g_info.mode_count > DISP64_MODE_MAX) fail |= 1;
    if (g_info.w < 320 || g_info.h < 200 || g_info.pitch < g_info.w) fail |= 2;

    // 4：来源与值必须同生同死（绝不出现 "src=vga 但 refresh=0" 这种谎）
    if (g_info.src == DISP64_SRC_NONE && g_info.refresh_x10 != 0) fail |= 4;
    if (g_info.src != DISP64_SRC_NONE && g_info.refresh_x10 <= 0) fail |= 4;
    if (g_info.src == DISP64_SRC_VGA && g_info.vga_x10 <= 0) fail |= 4;
    if (g_info.src == DISP64_SRC_EDID && g_info.edid_x10 <= 0) fail |= 4;
    if (g_info.refresh_x10 > 0 &&
        (g_info.refresh_x10 < DISP64_HZ_MIN_X10 || g_info.refresh_x10 > DISP64_HZ_MAX_X10)) fail |= 4;

    // 8：±5% 对比逻辑（离线合成样本，和平台无关）
    if (display64_match_x10(600, 600) != 1) fail |= 8;
    if (display64_match_x10(600, 750) != 0) fail |= 8;
    if (display64_match_x10(0, 600) != 0)   fail |= 8;
    if (display64_match_x10(600, 0) != 0)   fail |= 8;
    if (display64_match_x10(628, 600) != 1) fail |= 8;   // +4.6%：在容差内
    if (display64_match_x10(632, 600) != 0) fail |= 8;   // +5.3%：出容差

    // 16：清单里的每个条目字段必须合理
    for (int i = 0; i < g_info.mode_count; i++) {
        if (g_modes[i].width < 320 || g_modes[i].height < 200 || g_modes[i].bpp < 8) fail |= 16;
    }

    dbg64_line_begin64();
    if (fail == 0) {
        dbg64_str("[DISP64] selftest PASS\n");
    } else {
        dbg64_str("[DISP64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
    }
    dbg64_line_end64();
    return fail;
}
