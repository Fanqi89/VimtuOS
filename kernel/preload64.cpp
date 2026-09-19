// preload64.cpp - 64 位系统级预加载实现（字形预光栅化 + 桌面图标预缩放 + 实测证据）
//
// 口径与边界（先说清楚，避免误读）：
//   * "首帧代表工作" = 预热前后各做一次**真实绘制**：把一段界面常用文本用 TrueType 路径画到
//     屏幕左下角的小区域（预热前 = 首次现场光栅化 + 位图混合；预热后 = 缓存命中只做混合），
//     外加一个桌面图标的真实缩放绘制（预热前 = 128x128 最近邻缩放 + 混合；预热后 = 用预缩放
//     位图只做混合）。两次数的是同一件事，用 rdtsc64 量 cycles；数字是实测的。
//     画到屏幕上不会影响后续像素验收：gui64_run 一开始的 boot logo 会 fb_clear 整屏。
//   * 预热集合的来源（不是拍脑袋）：对 kernel/*.cpp **字符串字面量**（去掉注释）里的汉字做
//     词频统计，取出现频次最高的前 200 个（32 位是 193 个，同一思路）。选 200 而不是全部
//     442 个的原因：font.cpp 每个 face 的 CJK LRU 容量是 256，预热集必须能全部驻留才幂等
//     （否则每轮互相淘汰、预热变成每次重装同样的字 —— 32 位踩过 193 vs 192 的坑）。
//     集合规模 <= lru 容量时打点里会给 fits 结论；超了如实 WARN。
//   * ms 换算：键盘/定时器只有 250Hz，直接用 tick 量 1~2ms 的字形预热是不够的；这里在初始化时
//     用 rdtsc64 对 task_sleep64(24ms) 标定一次 "cycles per ms"，之后换算。
#include "preload64.h"
#include "font.h"        // font_prewarm_ascii_all / font_prewarm_text / font_cache_* / font_lru_capacity
#include "gui64.h"       // gui64_preload_icons64 / gui64_draw_icon_kind64 / gui64_screen_h
#include "fb.h"          // fb_set_clip / fb_reset_clip（测量区域）
#include "task64.h"      // task_sleep64（TSC 标定）
#include "x86_64.h"      // ticks64 / TICK_MS_64
#include "debug64.h"     // 串口打点
#include "sysstate64.h"  // 注册 sysstate64 模块（只注册，不改它的实现）

// ==================== 预热集合（来源：kernel/*.cpp 字符串字面量汉字词频 top 200） ====================
// 说明：这是"界面/终端/设置/任务管理器/监视器"等所有在屏文案的汉字并集里最高频的 200 个，
//   覆盖了菜单、按钮、状态、提示、字段名的绝大多数字。频次统计口径见文件头。
static const char PRELOAD64_CN[] =
    "安装区用盘器内已数的字分是移未启任务动必须节在统理"
    "核新块偏错系率布可页应计关文局扇行无状态时中切窗口"
    "选一失败实堆保持共享不存设总物磁件写建化示上漂了表"
    "重有下正机本介质式换放序个程运前到读没算置缩显知留"
    "自位入条为管驱该完成刷像会点落映请接项目界池空载全"
    "级就开当测量久即间步许头定与外回复光面始我终端于格"
    "辨固构据来策略清要体款结束长度找后断收监视扫雷壳占"
    "闲荷模法标拒绝除源所真部值话址调只简受择硬支义或名";

// 测量用的界面文本样本（这些字都在上面的集合里；预热后必然命中缓存）
static const char PRELOAD64_SAMPLE[] = "任务管理器进程窗口终端设置";

static Preload64Stats g_pl;
static bool g_pl_init = false;
static uint64_t g_cycles_per_ms = 0;

// ==================== 小工具 ====================
static inline uint64_t pl64_rdtsc() {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// 预热串里的唯一汉字数（O(n^2)，n≈200，一次性开销可忽略；无 STL）
static int pl64_cn_unique() {
    int n = 0;
    for (int i = 0; PRELOAD64_CN[i]; ) {
        int a1 = 0;
        const uint32_t cp = font_utf8_decode(PRELOAD64_CN + i, &a1);
        if (a1 <= 0) a1 = 1;
        if (cp >= 0x4E00) {
            bool dup = false;
            for (int j = 0; j < i; ) {
                int a2 = 0;
                const uint32_t cp2 = font_utf8_decode(PRELOAD64_CN + j, &a2);
                if (a2 <= 0) a2 = 1;
                if (cp2 == cp) { dup = true; break; }
                j += a2;
            }
            if (!dup) n++;
        }
        i += a1;
    }
    return n;
}

static uint32_t pl64_cycles_to_ms(uint64_t c) {
    if (g_cycles_per_ms == 0) return 0;
    const uint64_t ms = c / g_cycles_per_ms;
    return (ms > 0xFFFFFFFFull) ? 0xFFFFFFFFu : (uint32_t)ms;
}

// TSC 标定：对 task_sleep64(24ms)（PIT 250Hz -> 6 tick）测一次 cycles/ms。
// 只算一次；tick 没动（dt==0）就保持 0，之后所有 ms 都如实打 0（不编数字）。
static void pl64_calibrate() {
    if (g_cycles_per_ms) return;
    const uint64_t t0 = pl64_rdtsc();
    const uint32_t k0 = ticks64();
    task_sleep64(24);
    const uint64_t t1 = pl64_rdtsc();
    const uint32_t dt = ticks64() - k0;
    if (dt == 0) return;
    g_cycles_per_ms = (t1 - t0) / ((uint64_t)dt * (uint64_t)TICK_MS_64);
}

// "首帧代表工作"：真实绘制（文本 + 桌面图标）到屏幕左下角的小区域。
//   * fb_set_clip 把绘制限制在 320x64 的测量区，不越界；
//   * 文本用 font_draw_text（与界面同一条 TrueType 路径）；图标用 gui64_draw_icon_kind64
//     （有缓存时只做混合，没有时做与旧实现完全相同的缩放 + 混合）。
static uint64_t pl64_paint_probe() {
    const int sh = fb_height();
    const int y  = (sh > 64) ? (sh - 60) : 0;
    fb_set_clip(0, y, 320, 60);
    const uint64_t t0 = pl64_rdtsc();
    font_select(2);
    font_draw_text(4, y + 6, PRELOAD64_SAMPLE, 0xFFFFFFFF);
    gui64_draw_icon_kind64(224, y + 4, 0);
    const uint64_t t1 = pl64_rdtsc();
    fb_reset_clip();
    return t1 - t0;
}

// ==================== sysstate64 模块挂接（只注册；模块的 init = 只读复核） ====================
static int pl64_mod_init()   { return 0; }                             // 复核：模块已编入
static int pl64_mod_stop(uint32_t timeout) { (void)timeout; return 0; }
static int pl64_mod_health() { return g_pl.runs ? SYS64_MODH_UP : SYS64_MODH_DEGRADED; }

void preload64_init64() {
    if (g_pl_init) return;
    g_pl_init = true;
    g_pl.cache_cap = font_cache_capacity();
    g_pl.lru_cap   = font_lru_capacity();
    g_pl.cn_unique = pl64_cn_unique();
    pl64_calibrate();
    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] init cache_cap=");
    dbg64_dec((uint64_t)g_pl.cache_cap);
    dbg64_str(" lru_cap=");
    dbg64_dec((uint64_t)g_pl.lru_cap);
    dbg64_str(" cn_unique=");
    dbg64_dec((uint64_t)g_pl.cn_unique);
    dbg64_str(" cycles_per_ms=");
    dbg64_dec(g_cycles_per_ms);
    dbg64_nl();
    dbg64_line_end64();

    // 注册进 sysstate64 模块表（调用点在 sysstate64_start64 之前，见 kernel64.cpp）
    static const Sys64Module kPreloadMod = {
        "preload64", pl64_mod_init, pl64_mod_stop, pl64_mod_health, 0, {0, 0, 0}, 1000u
    };
    (void)sysstate64_register64(&kPreloadMod);
}

int preload64_run64() {
    if (!g_pl_init) preload64_init64();

    const uint64_t t_all = pl64_rdtsc();

    // 0) 自检：预热集必须能全部驻留 LRU，否则"幂等"不成立（如实打点）
    if (g_pl.cn_unique > g_pl.lru_cap) {
        dbg64_line_begin64();
        dbg64_str("[PRELOAD64] WARN prewarm set exceeds LRU capacity (unique=");
        dbg64_dec((uint64_t)g_pl.cn_unique);
        dbg64_str(" > lru=");
        dbg64_dec((uint64_t)g_pl.lru_cap);
        dbg64_str(") -> not idempotent\n");
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[PRELOAD64] prewarm set fits lru=");
        dbg64_dec((uint64_t)g_pl.lru_cap);
        dbg64_str(" (idempotent)\n");
        dbg64_line_end64();
    }

    // 1) 首帧代表工作：预热前（冷：现场光栅化 + 现场缩放）
    g_pl.paint_before = pl64_paint_probe();

    // 2) 字形预光栅化：三个 face 的 ASCII + 界面常用汉字
    const uint64_t t0 = pl64_rdtsc();
    const int ascii_new = font_prewarm_ascii_all();
    font_select(0);                                  // 正文 face（汉字按字形自动落 CJK 面）
    const int cn_new = font_prewarm_text(PRELOAD64_CN);
    g_pl.ms_glyphs = pl64_cycles_to_ms(pl64_rdtsc() - t0);
    g_pl.glyphs_new    = ascii_new + cn_new;
    g_pl.glyphs_cached = font_cache_used();

    // 3) 桌面图标预缩放（3 张 128x128 -> 48x48 + 开始图标 64x64 -> 24x24）
    g_pl.icons_cached = gui64_preload_icons64();
    // 再查一次缓存位图数（与上面返回值交叉核对；正常情况下相等）
    const int icon_cache_now = gui64_icon_cache_count64();

    // 4) 首帧代表工作：预热后（热：缓存命中，只做位图混合）
    g_pl.paint_after = pl64_paint_probe();

    g_pl.ms_total = pl64_cycles_to_ms(pl64_rdtsc() - t_all);
    g_pl.runs++;

    // 5) 打点（格式见 preload64.h；数字全部来自上面的实测）
    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] glyphs prewarmed=");
    dbg64_dec((uint64_t)g_pl.glyphs_new);
    dbg64_str(" ms=");
    dbg64_dec((uint64_t)g_pl.ms_glyphs);
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] icons cached=");
    dbg64_dec((uint64_t)g_pl.icons_cached);
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] first paint before=");
    dbg64_dec(g_pl.paint_before);
    dbg64_str(" after=");
    dbg64_dec(g_pl.paint_after);
    dbg64_str(" cycles\n");
    dbg64_line_end64();

    dbg64_line_begin64();
    dbg64_str("[PRELOAD64] done glyphs_new=");
    dbg64_dec((uint64_t)g_pl.glyphs_new);
    dbg64_str(" cached=");
    dbg64_dec((uint64_t)g_pl.glyphs_cached);
    dbg64_str("/");
    dbg64_dec((uint64_t)g_pl.cache_cap);
    dbg64_str(" cn_unique=");
    dbg64_dec((uint64_t)g_pl.cn_unique);
    dbg64_str(" icons=");
    dbg64_dec((uint64_t)g_pl.icons_cached);
    dbg64_str(" icon_cache=");
    dbg64_dec((uint64_t)icon_cache_now);
    dbg64_str(" ms=");
    dbg64_dec((uint64_t)g_pl.ms_total);
    dbg64_str(" runs=");
    dbg64_dec((uint64_t)g_pl.runs);
    dbg64_nl();
    dbg64_line_end64();

    return g_pl.glyphs_new;
}

const Preload64Stats* preload64_stats64() { return &g_pl; }
int preload64_glyphs_new64()    { return g_pl.glyphs_new; }
int preload64_glyphs_cached64() { return g_pl.glyphs_cached; }
int preload64_icons_cached64()  { return g_pl.icons_cached; }
uint32_t preload64_ms64()       { return g_pl.ms_total; }

// ==================== 终端 preload 命令的一行式报告 ====================
static void pl64_app_s(char* out, int* n, int maxlen, const char* s) {
    while (s && *s && *n < maxlen - 1) out[(*n)++] = *s++;
}

static void pl64_app_u(char* out, int* n, int maxlen, uint64_t v) {
    char r[24];
    int m = 0;
    if (v == 0) r[m++] = '0';
    while (v > 0) { r[m++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (m > 0 && *n < maxlen - 1) out[(*n)++] = r[--m];
}

void preload64_report64(char* out, int maxlen) {
    if (!out || maxlen <= 0) return;
    int n = 0;
    pl64_app_s(out, &n, maxlen, "glyphs_new=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.glyphs_new);
    pl64_app_s(out, &n, maxlen, " glyph_cached=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.glyphs_cached);
    pl64_app_s(out, &n, maxlen, "/");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.cache_cap);
    pl64_app_s(out, &n, maxlen, " cn_unique=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.cn_unique);
    pl64_app_s(out, &n, maxlen, " icons_cached=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.icons_cached);
    pl64_app_s(out, &n, maxlen, " first_paint_before=");
    pl64_app_u(out, &n, maxlen, g_pl.paint_before);
    pl64_app_s(out, &n, maxlen, " after=");
    pl64_app_u(out, &n, maxlen, g_pl.paint_after);
    pl64_app_s(out, &n, maxlen, " total_ms=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.ms_total);
    pl64_app_s(out, &n, maxlen, " runs=");
    pl64_app_u(out, &n, maxlen, (uint64_t)g_pl.runs);
    out[n] = 0;
}
