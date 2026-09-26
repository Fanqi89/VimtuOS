// locklogin64.cpp - 锁屏 / 登录界面 / 密码框 的实现（Windows 11 风格；数字与打点见 locklogin64.h）
//
// 结构（从上到下）：
//   1) 小工具 + 打点上限
//   2) 像素/文字原语（后备缓冲直写 + "字形墨高"可控的大号文字：先按 natural size 光栅化到左上角
//      scratch，读回墨迹掩膜，再**按墨高缩放**画到目标位置 —— 这样"时间 64–80px / 年月日 16–20px"
//      是按**字形墨高**（不是行高）成立的，且可被自动验收按像素量出来）
//   3) 背景：清晰 = gfx64 的整屏壁纸面；模糊 = 自算的 20px 盒式模糊（一次性，缓存在 g_blur）
//      "清晰 <-> 模糊" 用逐像素线性插值 + Token 缓动做动画（250–350ms，可反向）
//   4) 头像（内置程序化 3 种 / VimtuFS2 图片）/ 亚克力面板 / 登录按钮 / 密码框
//   5) 状态机（LOCK -> LOGIN -> DESKTOP）+ 输入 + 每帧 tick
#include "locklogin64.h"
#include "userdb64.h"
#include "theme64.h"
#include "gfx64.h"
#include "img64.h"
#include "config64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "debug64.h"
#include "mem_64.h"
#include "x86_64.h"
#include "panic64.h"      // 看门狗心跳（锁屏循环里也要踢）

// ==================== 打点（每类上限，防刷屏）====================
enum { LK_LOG_LOCK = 0, LK_LOG_LOGIN, LK_LOG_ANIM, LK_LOG_ERR, LK_LOG_N };
// 每类上限（防刷屏）：LOCK（锁屏事件）/ LOGIN（登录流程）/ ANIM（动画采样）/ ERR
//   实测：LOGGIN 类在"多次锁屏+多次登录+口令流程"的验收里 16 条不够（`password wrong` 会被吞掉），
//   所以给到 64；动画采样本身在 anim_tick64 里还有"每 60ms 一条"的节流。
static const int kLkCap[LK_LOG_N] = { 24, 64, 48, 32 };
static int g_lk_used[LK_LOG_N];

static int lk_log_ok64(int kind) {
    if (kind < 0 || kind >= LK_LOG_N) return 0;
    if (g_lk_used[kind] >= kLkCap[kind]) {
        if (g_lk_used[kind] == kLkCap[kind]) {
            g_lk_used[kind]++;
            dbg64_line_begin64();
            dbg64_str("[LOCK64] log budget reached kind=");
            dbg64_dec((uint64_t)kind);
            dbg64_str(" cap=");
            dbg64_dec((uint64_t)kLkCap[kind]);
            dbg64_nl();
            dbg64_line_end64();
        }
        return 0;
    }
    g_lk_used[kind]++;
    return 1;
}

// ==================== 小工具 ====================
static int   s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int   s_eq(const char* a, const char* b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}
static void  put2d64(char* out, int v) {         // 两位十进制（不足补 0）
    out[0] = (char)('0' + (v / 10) % 10);
    out[1] = (char)('0' + v % 10);
}
static void  put4d64(char* out, int v) {
    out[0] = (char)('0' + (v / 1000) % 10);
    out[1] = (char)('0' + (v / 100) % 10);
    out[2] = (char)('0' + (v / 10) % 10);
    out[3] = (char)('0' + v % 10);
}

// Zeller 同余：0=周日 … 6=周六（QEMU/真机的 RTC 星期字段不一定可信，这里用日期自己算）
static int weekday_ymd64(int y, int m, int d) {
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    int yy = y, mm = m;
    if (mm < 3) { mm += 12; yy -= 1; }
    const int k = ((yy % 100) + 100) % 100;
    const int j = yy / 100 + (yy < 0 ? -1 : 0);
    int h = (d + (13 * (mm + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    h = ((h % 7) + 7) % 7;
    return (h + 6) % 7;                          // 0=周日
}
static const char* weekday_zh64(int wd) {
    switch (wd) {
        case 0: return "星期日";
        case 1: return "星期一";
        case 2: return "星期二";
        case 3: return "星期三";
        case 4: return "星期四";
        case 5: return "星期五";
        case 6: return "星期六";
        default: return "星期?";
    }
}
static const char* weekday_en64(int wd) {
    switch (wd) {
        case 0: return "Sunday";
        case 1: return "Monday";
        case 2: return "Tuesday";
        case 3: return "Wednesday";
        case 4: return "Thursday";
        case 5: return "Friday";
        case 6: return "Saturday";
        default: return "?";
    }
}

// ==================== 状态 ====================
static int      g_state = LOCKLOGIN_STATE_LOCK;
static int      g_sel = 0;                 // 选中的"可见用户"下标（0 起）
static int      g_pw_active = 0;           // 密码框是否弹出
static char     g_pw[40];                  // 输入的密码（只在内存里，绝不打日志）
static int      g_pw_len = 0;
static int      g_pw_fail = 0;             // 1 = 上一次输错（显示提示）
static uint32_t g_pw_attempts = 0;
static uint32_t g_caret_t0 = 0;
static int      g_caret_on = 1;

static uint32_t g_lock_t0 = 0;             // 进入锁屏的时刻（自动登录计时）
static int      g_auto_login = 0;          // ★ 缺陷 4 修复：ui.login.auto **默认 0** = 必须等用户显式输入
                                           //   （回车/点登录按钮）才进桌面；无密码用户也不许自动跳过。
                                           //   置 1 才恢复"空闲 N 秒自动登录"的旧行为（只对无密码用户）。
static uint32_t g_auto_ms = 8000;          // ui.login.auto_delay_ms（仅 auto=1 时有意义）
static int      g_auto_fired = 0;

static int      g_blur_cur = 0;            // 当前背景模糊强度 0..255
static int      g_anim_active = 0;
static int      g_anim_from = 0, g_anim_to = 0;
static uint32_t g_anim_t0 = 0;
static uint32_t g_anim_ms = LOCKLOGIN_ANIM_MS;
static int      g_anim_dir_in = 1;
static uint32_t g_anim_logged = 0;
static int      g_anim_last_v = -1;
static uint32_t* g_blur = nullptr;         // 20px 模糊后的整屏背景
static int       g_blur_w = 0, g_blur_h = 0;
static int       g_blur_ok = 0;
static const uint32_t* g_blur_src = nullptr;
static int       g_blur_fail_logged = 0;
static int       g_time_y = 0, g_time_mo = 0, g_time_d = 0;   // RTC 日期（int：直接喂 rtc_get_date64）
static uint32_t  g_blur_ms = 0;

static int      g_time_h = 0, g_time_mi = 0, g_time_s = 0;
static int      g_time_wd = 0;
static char     g_time_str[8] = "00:00";
static char     g_date_str[40] = "";
static uint32_t g_sec_tick = 0;            // 每秒刷新一次的 tick 标记

static int      g_cur_x = -1, g_cur_y = -1;
static int      g_prev_cur_x = -1, g_prev_cur_y = -1;   // 光标旧位置（局部重绘用）
static uint32_t g_bg_tile_var = 0;         // 锁屏时的背景局部方差（清晰证据）
static int      g_wall_logged = 0;
static int      g_lock_logged = 0;

// 布局（每次 paint 时按屏幕尺寸算一遍）
struct LkGeom64 {
    int W, H;
    // 锁屏
    int panel_x, panel_y, panel_w, panel_h;
    int time_cx, time_y, date_cx, date_y;
    int time_box_x, time_box_y, time_box_w, time_box_h;
    int date_box_x, date_box_y, date_box_w, date_box_h;
    // 登录
    int av_cx, av_cy, av_d;
    int name_cx, name_y;
    int small_y, small_d, small_gap, small_x0;
    int btn_x, btn_y, btn_w, btn_h;
    int pw_x, pw_y, pw_w, pw_h, pw_btn_x, pw_btn_y, pw_btn_d;
    int err_y, hint_y;
    // 动画中间帧的"覆盖层矩形"（里面的像素在淡入期间保持不变，用来省掉整屏重画）
    int hole_x, hole_y, hole_w, hole_h;
};
static LkGeom64 g_geo;

static void geom64(int W, int H) {
    g_geo.W = W; g_geo.H = H;
    g_geo.panel_w = 720;
    g_geo.panel_h = 240;
    g_geo.panel_x = (W - g_geo.panel_w) / 2;
    g_geo.panel_y = H / 2 - 170;                      // 居中偏上
    if (g_geo.panel_y < 24) g_geo.panel_y = 24;
    g_geo.time_cx = W / 2;
    g_geo.time_y  = g_geo.panel_y + 40;
    g_geo.date_cx = W / 2;
    g_geo.date_y  = g_geo.panel_y + 150;
    g_geo.time_box_x = 0; g_geo.time_box_y = 0; g_geo.time_box_w = 0; g_geo.time_box_h = 0;
    g_geo.date_box_x = 0; g_geo.date_box_y = 0; g_geo.date_box_w = 0; g_geo.date_box_h = 0;

    g_geo.av_d = LOCKLOGIN_AVATAR_PX;
    g_geo.av_cx = W / 2;
    g_geo.av_cy = 190;
    g_geo.name_cx = W / 2;
    g_geo.name_y = g_geo.av_cy + g_geo.av_d / 2 + 18;
    g_geo.small_d = 44;
    g_geo.small_gap = 18;
    g_geo.small_y = g_geo.name_y + 34;
    g_geo.btn_w = LOCKLOGIN_BTN_PX;
    g_geo.btn_h = LOCKLOGIN_BTN_PX;
    g_geo.btn_x = W / 2 - LOCKLOGIN_BTN_PX / 2;
    g_geo.btn_y = g_geo.small_y + g_geo.small_d + 26;
    g_geo.pw_w = LOCKLOGIN_PW_W;
    g_geo.pw_h = LOCKLOGIN_PW_H;
    g_geo.pw_btn_d = LOCKLOGIN_PW_BTN;
    g_geo.pw_x = W / 2 - (LOCKLOGIN_PW_W + 8 + LOCKLOGIN_PW_BTN) / 2;
    g_geo.pw_y = g_geo.btn_y + LOCKLOGIN_BTN_PX + 46;
    g_geo.pw_btn_x = g_geo.pw_x + LOCKLOGIN_PW_W + 8;
    g_geo.pw_btn_y = g_geo.pw_y;
    g_geo.err_y = g_geo.pw_y + LOCKLOGIN_PW_H + 10;
    g_geo.hint_y = H - 44;
    // 覆盖层矩形：头像/用户名/小头像/登录按钮/密码框/错误提示 的并集（两侧各留 8px 余量）
    {
        const int x0 = g_geo.pw_x - 8;
        const int x1 = g_geo.pw_btn_x + g_geo.pw_btn_d + 8;
        const int y0 = g_geo.av_cy - g_geo.av_d / 2 - 20;
        const int y1 = g_geo.err_y + 28;
        g_geo.hole_x = x0 < 0 ? 0 : x0;
        g_geo.hole_y = y0 < 0 ? 0 : y0;
        g_geo.hole_w = (x1 > W ? W : x1) - g_geo.hole_x;
        g_geo.hole_h = (y1 > H ? H : y1) - g_geo.hole_y;
    }
}

// ==================== 后备缓冲直写（裁剪按 paint 开始时设定）====================
static uint32_t* g_fb = nullptr;
static int g_fw = 0, g_fh = 0;
static int g_cx0 = 0, g_cy0 = 0, g_cx1 = 0, g_cy1 = 0;

static inline void putpx64(int x, int y, uint32_t c) {
    if (x < g_cx0 || y < g_cy0 || x >= g_cx1 || y >= g_cy1) return;
    g_fb[(uint64_t)y * g_fw + x] = c;
}
static inline uint32_t blend64(uint32_t bg, uint32_t fg, int a) {
    if (a <= 0) return bg;
    if (a >= 255) return fg;
    const int ia = 255 - a;
    const int r = (int)((fg >> 16) & 0xFF), g = (int)((fg >> 8) & 0xFF), b = (int)(fg & 0xFF);
    const int br = (int)((bg >> 16) & 0xFF), bgc = (int)((bg >> 8) & 0xFF), bb = (int)(bg & 0xFF);
    return 0xFF000000u | ((uint32_t)((r * a + br * ia) / 255) << 16) |
                         ((uint32_t)((g * a + bgc * ia) / 255) << 8) |
                          (uint32_t)((b * a + bb * ia) / 255);
}

// ==================== 背景：清晰 <-> 模糊（20px）====================
// 一维盒式模糊（**就地**；边界复制；ring 保存即将离开窗口的原值）
static void box_blur_line64(uint32_t* v, int n, int r) {
    if (n <= 0 || r <= 0 || r > 30) return;
    if (n <= 2 * r + 2) return;                      // 太短：直接不动（避免越界读）
    static uint32_t ring[32];                        // 保存即将离开窗口的原值（r <= 30 -> 31 个足够）
    const int win = 2 * r + 1;
    const uint32_t edge_l = v[0];
    const uint32_t edge_r = v[n - 1];
    // ★ 除法无关：1/win 预先算成定点倒数（QEMU TCG 里 idiv 极慢，每像素 3 次除法 = 实测 5.7 秒）
    const uint32_t recip = (uint32_t)(65536u / (uint32_t)win);
    int sr = 0, sg = 0, sb = 0;
    for (int i = -r; i <= r; i++) {
        const uint32_t c = (i < 0) ? edge_l : v[i];
        sr += (int)((c >> 16) & 0xFF); sg += (int)((c >> 8) & 0xFF); sb += (int)(c & 0xFF);
    }
    for (int x = 0; x < n; x++) {
        const uint32_t orig = v[x];                  // 原值（还没被覆盖）
        ring[x & 31] = orig;
        // 输出（就地写；右端 x+r+1 还没被写过，所以还是原值）
        v[x] = 0xFF000000u | (((uint32_t)sr * recip >> 16) << 16) |
                              (((uint32_t)sg * recip >> 16) << 8) |
                               ((uint32_t)sb * recip >> 16);
        // 窗口右移：加入 x+r+1（越界复制右边界），剔除 x-r（越界复制左边界）
        const int xn = x + r + 1;
        const uint32_t cadd = (xn < n) ? v[xn] : edge_r;
        const uint32_t csub = (x - r >= 0) ? ring[(x - r) & 31] : edge_l;
        sr += (int)((cadd >> 16) & 0xFF) - (int)((csub >> 16) & 0xFF);
        sg += (int)((cadd >> 8) & 0xFF) - (int)((csub >> 8) & 0xFF);
        sb += (int)(cadd & 0xFF) - (int)(csub & 0xFF);
    }
}

// 整屏模糊：横向按行、纵向按列（列先拷进 scratch）
#define LK_COL_MAX 1200
static void blur_screen64(uint32_t* buf, int w, int h, int r) {
    if (w <= 0 || h <= 0 || w > (1 << 14) || h > LK_COL_MAX) return;
    for (int y = 0; y < h; y++) {
        box_blur_line64(buf + (uint64_t)y * w, w, r);
        if ((y & 63) == 0) panic64_watchdog_kick64();   // QEMU TCG 下单趟要几秒：喂狗
    }
    static uint32_t col[LK_COL_MAX];
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) col[y] = buf[(uint64_t)y * w + x];
        box_blur_line64(col, h, r);
        for (int y = 0; y < h; y++) buf[(uint64_t)y * w + x] = col[y];
        if ((x & 63) == 0) panic64_watchdog_kick64();
    }
}

static const uint32_t* wall_surface64(int* w, int* h) { return gfx64_wall_surface64(w, h); }

// 计算（或复用）模糊背景
static void ensure_blur64() {
    int w = 0, h = 0;
    const uint32_t* surf = wall_surface64(&w, &h);
    if (!surf || w <= 0 || h <= 0) { g_blur_ok = 0; return; }
    if (g_blur && (g_blur_w != w || g_blur_h != h)) {
        kfree_64(g_blur);
        g_blur = nullptr;
        g_blur_ok = 0;
    }
    if (!g_blur) {
        g_blur = (uint32_t*)kmalloc_64((uint64_t)w * h * 4);
        if (!g_blur) {
            if (!g_blur_fail_logged && lk_log_ok64(LK_LOG_ERR)) {
                g_blur_fail_logged = 1;
                dbg64_line_begin64();
                dbg64_str("[LOGIN64] blur FAILED reason=alloc bytes=");
                dbg64_dec((uint64_t)w * (uint64_t)h * 4);
                dbg64_str(" -> background stays sharp (blur=0)\n");
                dbg64_line_end64();
            }
            g_blur_ok = 0;
            return;
        }
        g_blur_w = w; g_blur_h = h;
        g_blur_ok = 0;
    }
    if (g_blur_ok && g_blur_src == surf) return;
    const uint64_t t0 = ticks64();
    for (uint64_t i = 0; i < (uint64_t)w * h; i++) g_blur[i] = surf[i];
    blur_screen64(g_blur, w, h, LOCKLOGIN_BLUR_PX);
    g_blur_ms = (uint32_t)(ticks64() - t0);
    g_blur_src = surf;
    g_blur_ok = 1;
    // 真模糊的像素证据：与 gfx64 同一块 32x32 tile（Dock 正上方）在原图/模糊图上的局部方差
    const int vx = (w - 600) / 2;
    const int vy = h - THEME64_DOCK_MARGIN - THEME64_DOCK_H;
    const uint64_t vs = gfx64_var64(surf, w, vx, vy, 32, 32);
    const uint64_t vd = gfx64_var64(g_blur, w, vx, vy, 32, 32);
    g_bg_tile_var = vs;
    if (lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOGIN64] blur tile x=");
        dbg64_dec((uint64_t)vx);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)vy);
        dbg64_str(" wh=32x32 r=");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_str(" var_src=");
        dbg64_dec(vs);
        dbg64_str(" var_dst=");
        dbg64_dec(vd);
        dbg64_str(" ms=");
        dbg64_dec((uint64_t)g_blur_ms);
        dbg64_str(" tile=");
        dbg64_dec((uint64_t)w);
        dbg64_str("x");
        dbg64_dec((uint64_t)h);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// 除法无关的通道插值：out = (s*(256-a) + b*a) >> 8（a 0..256；QEMU TCG 下比 /255 快很多）
static inline uint32_t lerp256_64(uint32_t s, uint32_t b, int a) {
    const uint32_t ia = (uint32_t)(256 - a);
    const uint32_t au = (uint32_t)a;
    const uint32_t sr = (s >> 16) & 0xFFu, sg = (s >> 8) & 0xFFu, sb = s & 0xFFu;
    const uint32_t br = (b >> 16) & 0xFFu, bg = (b >> 8) & 0xFFu, bb = b & 0xFFu;
    return 0xFF000000u | (((sr * ia + br * au) >> 8) << 16) |
                         (((sg * ia + bg * au) >> 8) << 8) |
                          ((sb * ia + bb * au) >> 8);
}

// 画背景矩形（清晰/模糊按 g_blur_cur 插值）
//   a == 0   -> 直接拷清晰面（锁屏：完全清晰）
//   a >= 256 -> 直接拷模糊面（登录：模糊 20px）
//   中间     -> 逐像素插值；整屏时按 2x2 块插值（QEMU TCG 下每帧 1M 像素太贵，
//               两个面都是低频图，2x2 块在 300ms 的淡入里肉眼无差；端点仍是精确拷贝）
static void blit_bg64(int x, int y, int w, int h) {
    int sw = 0, sh = 0;
    const uint32_t* sharp = wall_surface64(&sw, &sh);
    const uint32_t* blur = (g_blur_ok && g_blur) ? g_blur : nullptr;
    const Theme64Tokens* t = theme64_tokens64();
    const int a = g_blur_cur;
    const int full = (x <= 0 && y <= 0 && x + w >= g_fw && y + h >= g_fh);
    if (full && sharp && (!blur || a <= 0) && sw == g_fw && sh == g_fh) {
        // 整屏 + 完全清晰：直接按行拷（最快路径）
        for (int j = 0; j < g_fh; j++) {
            const uint32_t* src = sharp + (uint64_t)j * sw;
            uint32_t* dst = g_fb + (uint64_t)j * g_fw;
            for (int i = 0; i < g_fw; i++) dst[i] = src[i];
        }
        return;
    }
    if (full && blur && a >= 256 && sw == g_fw && sh == g_fh) {
        for (int j = 0; j < g_fh; j++) {
            const uint32_t* src = blur + (uint64_t)j * sw;
            uint32_t* dst = g_fb + (uint64_t)j * g_fw;
            for (int i = 0; i < g_fw; i++) dst[i] = src[i];
        }
        return;
    }
    // 淡入/淡出中间帧：按 2x2 块插值（两个面都是低频图，300ms 的淡入里肉眼无差；端点仍是精确拷贝）
    const int blocks = (blur && a > 0 && a < 256) ? 2 : 1;
    const int as = a > 256 ? 256 : a;
    for (int j = y; j < y + h; j += blocks) {
        for (int i = x; i < x + w; i += blocks) {
            uint32_t c;
            if (!sharp || i < 0 || j < 0 || i >= sw || j >= sh) {
                c = t->desktop_base;
            } else {
                c = sharp[(uint64_t)j * sw + i];
                if (blur && as > 0) c = lerp256_64(c, blur[(uint64_t)j * sw + i], as);
            }
            if (blocks == 1) {
                putpx64(i, j, c);
                continue;
            }
            for (int bj = 0; bj < 2; bj++) {
                const int yy = j + bj;
                if (yy >= y + h) break;
                for (int bi = 0; bi < 2; bi++) {
                    const int xx = i + bi;
                    if (xx >= x + w) break;
                    putpx64(xx, yy, c);
                }
            }
        }
    }
}

// ==================== 大号文字（按字形墨高缩放）====================
// 做法：① 用 natural size 把整串画到屏幕左上角的 scratch（黑底白字）② 读回覆盖率掩膜
//       ③ 按目标墨高做最近邻放大，用 Token 颜色混合到目标位置 ④ 把 scratch 区域用背景还原。
// 这样"时间 64–80px、年月日 16–20px"是**字形墨高**（不是行高），可被自动验收按像素量出来。
#define LK_MASK_W 256
#define LK_MASK_H 40
static uint8_t g_mask[LK_MASK_W * LK_MASK_H];

struct LkBox64 { int x, y, w, h; };
// 还原 scratch 区域：临时把"绘制裁剪区"放宽到整个屏幕（scratch 在 (0,0,256,40)，
// 可能落在本次 paint 的脏矩形之外 —— 不还原的话左上角会留下黑块）
static void scratch_restore64(void) {
    const int sx0 = g_cx0, sy0 = g_cy0, sx1 = g_cx1, sy1 = g_cy1;
    g_cx0 = 0; g_cy0 = 0; g_cx1 = g_fw; g_cy1 = g_fh;
    blit_bg64(0, 0, LK_MASK_W, LK_MASK_H);
    g_cx0 = sx0; g_cy0 = sy0; g_cx1 = sx1; g_cy1 = sy1;
}


// 把 str 按 target_ink 的**字形墨高**居中画在 (cx, y_top)；返回实际盒子
static void big_text64(const char* str, int cx, int y_top, int target_ink, uint32_t color, LkBox64* box_out) {
    if (box_out) { box_out->x = cx; box_out->y = y_top; box_out->w = 0; box_out->h = 0; }
    if (!str || !str[0] || target_ink < 4) return;
    // 1) 用 natural size 画到左上角 scratch（黑底白字），再读回覆盖率掩膜
    fb_reset_clip();
    fb_fill_rect(0, 0, LK_MASK_W, LK_MASK_H, 0);
    fb_set_clip(0, 0, LK_MASK_W, LK_MASK_H);
    font_select(FONT_FACE_ASCII);
    font_draw_text(1, 1, str, rgb(255, 255, 255));
    fb_reset_clip();
    int minx = 9999, miny = 9999, maxx = -1, maxy = -1;
    for (int y = 0; y < LK_MASK_H; y++) {
        for (int x = 0; x < LK_MASK_W; x++) {
            const uint32_t c = fb_get_pixel(x, y);
            const int cov = (int)((c >> 8) & 0xFFu);
            g_mask[y * LK_MASK_W + x] = (uint8_t)cov;
            if (cov > 24) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    // 2) 立刻把 scratch 区域用背景还原（不然左上角会留黑块）
    scratch_restore64();
    if (maxx < 0) return;
    const int bw = maxx - minx + 1;
    const int bh = maxy - miny + 1;
    const int dh = target_ink;
    const int dw = (int)((int64_t)bw * dh / bh);
    const int x0 = cx - dw / 2;
    for (int j = 0; j < dh; j++) {
        const int sy = miny + (int)((int64_t)j * bh / dh);
        for (int i = 0; i < dw; i++) {
            const int sx = minx + (int)((int64_t)i * bw / dw);
            const int cov = g_mask[sy * LK_MASK_W + sx];
            if (cov > 8) gfx64_blend64(x0 + i, y_top + j, color, cov);
        }
    }
    if (box_out) { box_out->x = x0; box_out->y = y_top; box_out->w = dw; box_out->h = dh; }
}

// ==================== 头像 ====================
// 内置头像 3 种（按 uid 选色；程序化绘制 = 不占磁盘、不依赖图片解码）
static void builtin_avatar_color64(int idx, uint32_t* c0, uint32_t* c1) {
    const Theme64Tokens* t = theme64_tokens64();
    switch (((idx % 3) + 3) % 3) {
        case 0:  *c0 = t->grad_a; *c1 = t->accent;   break;
        case 1:  *c0 = t->accent; *c1 = t->accent2;  break;
        default: *c0 = t->grad_b; *c1 = t->grad_a;   break;
    }
}

#define LK_AV_MAX 4
struct LkAvCache64 { uint8_t* rgba; int d; int tried; int ok; };
static LkAvCache64 g_av_cache[LK_AV_MAX];

// 画圆形头像（居中在 cx,cy，直径 d）；avatar_idx = 用户下标（决定内置配色与图片缓存槽）
static void avatar_draw64(int cx, int cy, int d, const User64Entry* e, int avatar_idx) {
    const Theme64Tokens* t = theme64_tokens64();
    int r = d / 2;
    if (r < 8) r = 8;
    const int x = cx - r, y = cy - r;
    // 轻微外阴影（Token：近/远两层浅阴影）
    gfx64_shadow64(x, y, 2 * r, 2 * r, r, t);
    // 可选的真图片头像（PNG/BMP；JPEG 本批不支持 -> 如实打点并退回内置）
    uint8_t* img_px = nullptr;
    int img_d = 0;
    if (e && e->avatar[0] && !s_eq(e->avatar, "builtin")) {
        // 缓存槽：按用户下标取模（同一个用户重复画头像时不再解码）
        const int slot = (avatar_idx >= 0) ? (avatar_idx % LK_AV_MAX) : 0;
        LkAvCache64* c = &g_av_cache[slot];
        if (!c->tried) {
            c->tried = 1;
            Img64 im{};
            if (img64_load_vfs64(e->avatar, &im) == 0) {
                c->rgba = (uint8_t*)kmalloc_64((uint64_t)d * d * 4);
                if (c->rgba) {
                    img64_scale64(&im, (uint32_t*)c->rgba, d, d);
                    c->d = d;
                    c->ok = 1;
                }
                img64_free64(&im);
            }
            if (!c->ok && lk_log_ok64(LK_LOG_ERR)) {
                dbg64_line_begin64();
                dbg64_str("[LOCK64] avatar user=");
                dbg64_str(e->name);
                dbg64_str(" src=");
                dbg64_str(e->avatar);
                dbg64_str(" FAILED reason=");
                dbg64_str(img64_last_err64());
                dbg64_str(" -> builtin fallback\n");
                dbg64_line_end64();
            }
        }
        if (c->ok) { img_px = c->rgba; img_d = c->d; }
    }
    uint32_t c0, c1;
    builtin_avatar_color64(avatar_idx < 0 ? 0 : avatar_idx, &c0, &c1);
    const int r2 = r * r;
    const int r2o = (r + 1) * (r + 1);
    for (int j = 0; j < 2 * r; j++) {
        for (int i = 0; i < 2 * r; i++) {
            const int dx = i - r, dy = j - r;
            const int d2 = dx * dx + dy * dy;
            if (d2 > r2o) continue;
            int a = 255;
            if (d2 > r2) a = 110;                       // 1px 软边（避免锯齿）
            uint32_t c;
            if (img_px && img_d == d) {
                const uint8_t* p = img_px + ((uint64_t)j * d + i) * 4;
                c = rgb(p[0], p[1], p[2]);
                a = a * (int)p[3] / 255;
            } else {
                // 对角渐变底 + 轻微柔光
                const int tt = ((i + j) * 255) / (2 * r > 0 ? 2 * r : 1);
                c = blend64(c0, c1, tt > 255 ? 255 : tt);
            }
            if (a <= 0) continue;
            const uint32_t bg = (g_fb && (x + i) >= g_cx0 && (x + i) < g_cx1 &&
                                 (y + j) >= g_cy0 && (y + j) < g_cy1)
                                ? g_fb[(uint64_t)(y + j) * g_fw + (x + i)] : 0;
            putpx64(x + i, y + j, blend64(bg, c, a));
        }
    }
    // 中央字母/字（首字符；CJK 名字也能画）
    if (e && e->name[0]) {
        char one[8];
        int n = 0;
        const unsigned char ch = (unsigned char)e->name[0];
        int bytes = 1;
        if (ch >= 0xF0) bytes = 4; else if (ch >= 0xE0) bytes = 3; else if (ch >= 0xC0) bytes = 2;
        for (int i = 0; i < bytes && e->name[i] && n < 7; i++) one[n++] = e->name[i];
        one[n] = 0;
        const int ink = d * 40 / 100;
        big_text64(one, cx, cy - ink / 2, ink, rgb(255, 255, 255), nullptr);
    }
}

// ==================== 亚克力面板 / 按钮 / 光标 ====================
static void panel64(int x, int y, int w, int h, int r, int layer, uint32_t tint, int alpha) {
    const Theme64Tokens* t = theme64_tokens64();
    gfx64_glass64(x, y, w, h, r, layer, tint, alpha, t->win_frame, THEME64_A_BORDER, t);
}

static void arrow_button64(int x, int y, int s) {
    const Theme64Tokens* t = theme64_tokens64();
    gfx64_grad_round64(x, y, s, s, LOCKLOGIN_BTN_R, t->grad_a, t->grad_b, 1, 255);
    gfx64_stroke_round64(x, y, s, s, LOCKLOGIN_BTN_R, t->highlight, THEME64_A_BORDER);
    // 居中右箭头（一条横杆 + 一个三角头；颜色取 Token）
    const uint32_t fg = t->highlight;
    const int cy = y + s / 2;
    const int shaft_w = s / 4;                 // 横杆长
    const int shaft_h = s / 12;                // 横杆粗
    const int head_w  = s / 4;                 // 三角头长
    const int head_h  = s / 3;                 // 三角头全高
    const int ax = x + (s - (shaft_w + head_w)) / 2;
    for (int i = 0; i < shaft_w; i++)
        for (int j = -shaft_h / 2; j <= shaft_h / 2; j++) putpx64(ax + i, cy + j, fg);
    for (int i = 0; i < head_w; i++) {
        const int hh = (head_h / 2) * (head_w - i) / head_w;
        for (int j = -hh; j <= hh; j++) putpx64(ax + shaft_w + i, cy + j, fg);
    }
}

static void cursor64(void) {
    const Theme64Tokens* t = theme64_tokens64();
    const uint32_t fill = t->highlight;        // 白箭头
    const uint32_t edge = t->text;             // 深色描边（浅色背景上也看得见）
    for (int r = 0; r < 12; r++) {
        const int w = (r < 7) ? (r / 2 + 1) : 3;
        for (int i = 0; i < w; i++) putpx64(g_cur_x + i, g_cur_y + r, fill);
    }
    for (int r = 0; r < 12; r++) putpx64(g_cur_x - 1, g_cur_y + r, edge);
    for (int i = 0; i < 4; i++) putpx64(g_cur_x + i, g_cur_y - 1, edge);
    for (int i = 0; i < 5; i++) putpx64(g_cur_x + i, g_cur_y + 6, edge);
    putpx64(g_cur_x + 3, g_cur_y + 11, edge);
}

// ==================== 时间 ====================
static void refresh_time64(int force) {
    const uint32_t now = ticks64();
    if (!force && (uint32_t)(now - g_sec_tick) < PIT_HZ_64) return;
    g_sec_tick = now;
    rtc_get_time64(&g_time_h, &g_time_mi, &g_time_s);
    int rtc_wd = 0;
    rtc_get_date64(&g_time_y, &g_time_mo, &g_time_d, &rtc_wd);
    if (g_time_y < 2000 || g_time_y > 2063 || g_time_mo < 1 || g_time_mo > 12 || g_time_d < 1 || g_time_d > 31) {
        g_time_y = 2000; g_time_mo = 1; g_time_d = 1;      // RTC 读坏了也不要画 0000-00-00
    }
    g_time_wd = weekday_ymd64(g_time_y, g_time_mo, g_time_d);
    if (g_time_wd < 0) g_time_wd = ((rtc_wd % 7) + 7) % 7;
    put2d64(g_time_str, g_time_h);
    g_time_str[2] = ':';
    put2d64(g_time_str + 3, g_time_mi);
    g_time_str[5] = 0;
    char* p = g_date_str;
    put4d64(p, g_time_y); p += 4;
    *p++ = '-'; put2d64(p, g_time_mo); p += 2;
    *p++ = '-'; put2d64(p, g_time_d); p += 2;
    *p++ = ' ';
    const char* wdstr = weekday_zh64(g_time_wd);
    for (int i = 0; wdstr[i] && p < g_date_str + (int)sizeof(g_date_str) - 1; i++) *p++ = wdstr[i];
    *p = 0;
    (void)weekday_en64;                        // 英文星期名给未来 P3 设置页用（此处保留）
}

// ==================== 绘制：锁屏 ====================
static void paint_lock64(void) {
    const Theme64Tokens* t = theme64_tokens64();
    // 白色亚克力面板（Token：卡片 alpha + 卡片圆角）
    panel64(g_geo.panel_x, g_geo.panel_y, g_geo.panel_w, g_geo.panel_h,
            THEME64_R_CARD, 1, t->title_bg, THEME64_A_CARD);
    // 时间（大号，墨高 72）
    LkBox64 tb{};
    big_text64(g_time_str, g_geo.time_cx, g_geo.time_y, LOCKLOGIN_TIME_INK, t->text, &tb);
    // 年月日 + 星期几（墨高 18）
    LkBox64 db{};
    big_text64(g_date_str, g_geo.date_cx, g_geo.date_y, LOCKLOGIN_DATE_INK, t->title_txt, &db);
    // 底部提示（natural size；英文/中文各一行）
    font_select(FONT_FACE_ASCII);
    const char* hint = "Press Enter or click to sign in";
    const int hw = font_text_width(hint);
    font_draw_text(g_geo.W / 2 - hw / 2, g_geo.hint_y, hint, t->text_dim);
    // 打点（每次进入锁屏只打一遍）
    if (!g_lock_logged) {
        g_lock_logged = 1;
        g_geo.time_box_x = tb.x; g_geo.time_box_y = tb.y; g_geo.time_box_w = tb.w; g_geo.time_box_h = tb.h;
        g_geo.date_box_x = db.x; g_geo.date_box_y = db.y; g_geo.date_box_w = db.w; g_geo.date_box_h = db.h;
        if (lk_log_ok64(LK_LOG_LOCK)) {
            dbg64_line_begin64();
            dbg64_str("[LOCK64] lock text time_ink=");
            dbg64_dec((uint64_t)tb.h);
            dbg64_str("px time_box=");
            dbg64_dec((uint64_t)tb.x); dbg64_str(",");
            dbg64_dec((uint64_t)tb.y); dbg64_str(",");
            dbg64_dec((uint64_t)tb.w); dbg64_str(",");
            dbg64_dec((uint64_t)tb.h);
            dbg64_str(" date_ink=");
            dbg64_dec((uint64_t)db.h);
            dbg64_str("px date_box=");
            dbg64_dec((uint64_t)db.x); dbg64_str(",");
            dbg64_dec((uint64_t)db.y); dbg64_str(",");
            dbg64_dec((uint64_t)db.w); dbg64_str(",");
            dbg64_dec((uint64_t)db.h);
            dbg64_str(" panel_box=");
            dbg64_dec((uint64_t)g_geo.panel_x); dbg64_str(",");
            dbg64_dec((uint64_t)g_geo.panel_y); dbg64_str(",");
            dbg64_dec((uint64_t)g_geo.panel_w); dbg64_str(",");
            dbg64_dec((uint64_t)g_geo.panel_h);
            dbg64_nl();
            dbg64_line_end64();
        }
    }
}

// ==================== 绘制：登录 ====================
static void paint_login64(void) {
    const Theme64Tokens* t = theme64_tokens64();
    const User64Entry* e = userdb64_visible64(g_sel);
    // 头像（圆形 104 + 轻微外阴影）
    avatar_draw64(g_geo.av_cx, g_geo.av_cy, g_geo.av_d, e, g_sel);
    // 用户名（墨高 22）
    if (e) {
        LkBox64 nb{};
        big_text64(e->name, g_geo.name_cx, g_geo.name_y, 22, t->text, &nb);
    }
    // 其他普通用户的小头像（可点；root 不会出现在这里）
    const int nvis = userdb64_normal_count64();
    if (nvis > 1) {
        const int total = nvis * g_geo.small_d + (nvis - 1) * g_geo.small_gap;
        int x0 = (g_geo.W - total) / 2;
        if (x0 < 8) x0 = 8;
        for (int i = 0; i < nvis; i++) {
            const int x = x0 + i * (g_geo.small_d + g_geo.small_gap);
            const User64Entry* u = userdb64_visible64(i);
            if (!u) continue;
            if (i == g_sel) {
                // 选中：加一圈高亮描边（Token 强调色）
                gfx64_stroke_round64(x - 3, g_geo.small_y - 3, g_geo.small_d + 6, g_geo.small_d + 6,
                                     (g_geo.small_d + 6) / 2, t->accent, 200);
            }
            avatar_draw64(x + g_geo.small_d / 2, g_geo.small_y + g_geo.small_d / 2, g_geo.small_d, u, i);
        }
        g_geo.small_x0 = x0;
    } else {
        g_geo.small_x0 = 0;
    }
    // 登录按钮（96×96 / 圆角 24 / 主题渐变）
    arrow_button64(g_geo.btn_x, g_geo.btn_y, g_geo.btn_w);
    // 密码框 + 确认按钮
    if (g_pw_active) {
        panel64(g_geo.pw_x, g_geo.pw_y, g_geo.pw_w, g_geo.pw_h, THEME64_R_BUTTON, 1, t->title_bg, THEME64_A_CARD);
        font_select(FONT_FACE_ASCII);
        char stars[48];
        int n = 0;
        for (int i = 0; i < g_pw_len && n < (int)sizeof(stars) - 2; i++) stars[n++] = '*';
        if (g_caret_on) stars[n++] = '|';
        stars[n] = 0;
        font_draw_text(g_geo.pw_x + 14, g_geo.pw_y + (g_geo.pw_h - font_line_height()) / 2, stars, t->text);
        if (g_pw_len == 0 && !g_pw_fail) {
            const char* ph = "password";
            font_draw_text(g_geo.pw_x + 14, g_geo.pw_y + (g_geo.pw_h - font_line_height()) / 2, ph, t->text_dim);
        }
        // 确认按钮：白底半透明 + 黑字「确认」
        const uint32_t white = rgb(255, 255, 255);
        gfx64_fill_round64(g_geo.pw_btn_x, g_geo.pw_btn_y, g_geo.pw_btn_d, g_geo.pw_btn_d,
                          THEME64_R_BUTTON, white, 210);
        gfx64_stroke_round64(g_geo.pw_btn_x, g_geo.pw_btn_y, g_geo.pw_btn_d, g_geo.pw_btn_d,
                            THEME64_R_BUTTON, t->win_frame, 160);
        {
            const char* ok = "确认";
            font_select(FONT_FACE_CJK);
            const int tw = font_text_width(ok);
            font_draw_text(g_geo.pw_btn_x + (g_geo.pw_btn_d - tw) / 2,
                           g_geo.pw_btn_y + (g_geo.pw_btn_d - font_line_height()) / 2, ok, rgb(0, 0, 0));
        }
        if (g_pw_fail) {
            const char* err = "密码错误，请重试";
            font_select(FONT_FACE_CJK);
            const int ew = font_text_width(err);
            font_draw_text(g_geo.W / 2 - ew / 2, g_geo.err_y, err, t->btn_close);
        }
    }
}

// ==================== 绘制：整块 / 局部 ====================
// 淡入/淡出中的省算优化（QEMU TCG 没有 GPU，整屏逐像素插值太贵）：
//   中间帧只重画"背景 - 中央覆盖层矩形"，并且覆盖层本身（玻璃面板/文字/头像/按钮）不重画 ——
//   它们在动画期间**内容不变**（面板玻璃取的是 gfx64 的壁纸面，不取屏幕背景），端点帧会完整重画。
static int g_overlay_valid = 0;                  // 1 = 覆盖层已经画在当前屏上
static int g_paint_ms_budget = 6;
static uint32_t g_lock_paint_n = 0;

static void paint64(int px, int py, int pw, int ph) {
    int fw = 0, fh = 0;
    g_fb = fb_surface64(&fw, &fh);
    if (!g_fb) return;                              // 没有后备缓冲：不画（绝不解引用空指针）
    g_fw = fw ? fw : fb_width();
    g_fh = fh ? fh : fb_height();
    g_cx0 = px; g_cy0 = py; g_cx1 = px + pw; g_cy1 = py + ph;
    if (g_cx0 < 0) g_cx0 = 0;
    if (g_cy0 < 0) g_cy0 = 0;
    if (g_cx1 > g_fw) g_cx1 = g_fw;
    if (g_cy1 > g_fh) g_cy1 = g_fh;
    geom64(g_fw, g_fh);                             // 每次绘制按屏幕尺寸重算布局
    if (g_cx1 <= g_cx0 || g_cy1 <= g_cy0) return;
    const uint64_t t0 = ticks64();
    // gfx64 的原语走 fb 的裁剪矩形；这里把它设成同一块
    fb_set_clip(g_cx0, g_cy0, g_cx1 - g_cx0, g_cy1 - g_cy0);
    const int mid_fade = (g_anim_active && g_blur_cur > 0 && g_blur_cur < 256);
    if (mid_fade && g_overlay_valid) {
        // 中间帧：背景拆成"覆盖层矩形之外"的 4 块画（覆盖层矩形里保留上一帧的面板像素）
        const int hx = g_geo.hole_x, hy = g_geo.hole_y, hw = g_geo.hole_w, hh = g_geo.hole_h;
        const int ix0 = g_cx0 > hx ? g_cx0 : hx;
        const int iy0 = g_cy0 > hy ? g_cy0 : hy;
        const int ix1 = g_cx1 < hx + hw ? g_cx1 : hx + hw;
        const int iy1 = g_cy1 < hy + hh ? g_cy1 : hy + hh;
        if (ix1 <= ix0 || iy1 <= iy0) {
            blit_bg64(g_cx0, g_cy0, g_cx1 - g_cx0, g_cy1 - g_cy0);      // 没有交集：整块画
        } else {
            blit_bg64(g_cx0, g_cy0, g_cx1 - g_cx0, iy0 - g_cy0);                 // 上
            blit_bg64(g_cx0, iy1, g_cx1 - g_cx0, g_cy1 - iy1);                   // 下
            blit_bg64(g_cx0, iy0, ix0 - g_cx0, iy1 - iy0);                       // 左
            blit_bg64(ix1, iy0, g_cx1 - ix1, iy1 - iy0);                         // 右
        }
    } else {
        blit_bg64(g_cx0, g_cy0, g_cx1 - g_cx0, g_cy1 - g_cy0);
        if (g_state == LOCKLOGIN_STATE_LOCK) paint_lock64();
        else paint_login64();
        g_overlay_valid = 1;
    }
    cursor64();
    fb_reset_clip();
    fb_flip_region(g_cx0, g_cy0, g_cx1 - g_cx0, g_cy1 - g_cy0);
    // 帧耗时证据（前 6 次整屏绘制；QEMU TCG 下的真实性能）
    const uint32_t ms = (uint32_t)((ticks64() - t0) * 1000u / PIT_HZ_64);
    if (g_paint_ms_budget > 0 && (g_cx1 - g_cx0) >= g_fw / 2) {
        g_paint_ms_budget--;
        g_lock_paint_n++;
        dbg64_line_begin64();
        dbg64_str("[LOCK64] paint full frame=");
        dbg64_dec((uint64_t)g_lock_paint_n);
        dbg64_str(" ms=");
        dbg64_dec((uint64_t)ms);
        dbg64_str(" rect=");
        dbg64_dec((uint64_t)(g_cx1 - g_cx0));
        dbg64_str("x");
        dbg64_dec((uint64_t)(g_cy1 - g_cy0));
        dbg64_str(" blur=");
        dbg64_dec((uint64_t)(g_blur_cur * LOCKLOGIN_BLUR_PX / 256));
        dbg64_str("/");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_nl();
        dbg64_line_end64();
    }
}

static void paint_full64(void) { paint64(0, 0, fb_width(), fb_height()); }

// ==================== 动画 ====================
static void anim_start64(int from, int to, int dir_in) {
    g_anim_from = from;
    g_anim_to = to;
    g_anim_t0 = ticks64();
    g_anim_ms = theme64_dur64(LOCKLOGIN_ANIM_MS);
    if (g_anim_ms < 250) g_anim_ms = 250;       // 需求：250–350ms（减少动画开关最多把它压到 250）
    if (g_anim_ms > 350) g_anim_ms = 350;
    g_anim_active = 1;
    g_anim_dir_in = dir_in;
    g_anim_logged = 0;
    g_anim_last_v = -1;
}

// 推进动画；返回 1 = 需要重绘
static int anim_tick64(void) {
    if (!g_anim_active) return 0;
    const uint32_t now = ticks64();
    const uint32_t el = (uint32_t)(now - g_anim_t0);
    const uint32_t wall_ms = el * 1000u / PIT_HZ_64;
    int done = 0;
    uint32_t t_ms = wall_ms;
    if (wall_ms >= g_anim_ms) {
        g_blur_cur = g_anim_to;
        g_anim_active = 0;
        done = 1;
        t_ms = g_anim_ms;                    // 动画的**逻辑时钟**封顶在时长（帧可能落得更晚）
        g_overlay_valid = 0;                 // 结束帧要完整重画（覆盖层与最终背景一起）
    } else {
        int tt = (int)((uint64_t)wall_ms * 256u / (g_anim_ms ? g_anim_ms : 1));
        if (tt > 256) tt = 256;
        const int ev = theme64_ease64(tt);
        g_blur_cur = g_anim_from + (g_anim_to - g_anim_from) * ev / 256;
    }
    // 采样打点（同一动画里 v 变化且间隔 >= 60ms 才打；上限 12 行/类，防刷屏）
    const int v = (g_blur_cur * LOCKLOGIN_BLUR_PX) / 256;
    if (done || (v != g_anim_last_v && (g_anim_logged == 0 || t_ms - g_anim_logged >= 60))) {
        if (lk_log_ok64(LK_LOG_ANIM)) {
            g_anim_logged = t_ms;
            g_anim_last_v = v;
            dbg64_line_begin64();
            dbg64_str("[LOGIN64] blur anim dir=");
            dbg64_str(g_anim_dir_in ? "in" : "out");
            dbg64_str(" t=");
            dbg64_dec((uint64_t)t_ms);
            dbg64_str("ms v=");
            dbg64_dec((uint64_t)v);
            dbg64_str("/");
            dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
            dbg64_str(" wall=");
            dbg64_dec((uint64_t)wall_ms);
            dbg64_str("ms anim=");
            dbg64_dec((uint64_t)g_anim_ms);
            dbg64_str("ms\n");
            dbg64_line_end64();
        }
    }
    return 1;
}

// ==================== 状态切换 ====================
static const User64Entry* sel_user64() { return userdb64_visible64(g_sel); }

static void clamp_sel64() {
    const int n = userdb64_normal_count64();
    if (n <= 0) { g_sel = 0; return; }
    if (g_sel < 0) g_sel = 0;
    if (g_sel >= n) g_sel = n - 1;
}

static void log_userlist64() {
    if (!lk_log_ok64(LK_LOG_LOGIN)) return;
    dbg64_line_begin64();
    dbg64_str("[LOGIN64] userlist n=");
    dbg64_dec((uint64_t)userdb64_normal_count64());
    dbg64_str(" names=");
    const int n = userdb64_normal_count64();
    for (int i = 0; i < n; i++) {
        const User64Entry* u = userdb64_visible64(i);
        if (i) dbg64_str(",");
        dbg64_str(u ? u->name : "?");
    }
    if (n == 0) dbg64_str("(none)");
    dbg64_str(" root=hidden=1 sel=");
    dbg64_dec((uint64_t)g_sel);
    dbg64_nl();
    dbg64_line_end64();
}

static void enter_lock64(const char* why) {
    g_overlay_valid = 0;                   // 界面换了：覆盖层必须重画
    g_state = LOCKLOGIN_STATE_LOCK;
    g_pw_active = 0;
    g_pw_len = 0;
    g_pw[0] = 0;
    g_pw_fail = 0;
    g_lock_t0 = ticks64();
    clamp_sel64();
    ensure_blur64();                       // 让 [LOGIN64] blur tile 行提前备好（模糊结果可复用）
    if (!g_wall_logged) {
        g_wall_logged = 1;
        const int lock_mode = cfg64_lock_wall_mode64();
        const int desk_mode = cfg64_wall_mode64();
        if (lk_log_ok64(LK_LOG_LOCK)) {
            dbg64_line_begin64();
            dbg64_str("[LOCK64] wall src=");
            dbg64_str(gfx64_wall_src_desc64());
            dbg64_str(" lock_mode=");
            dbg64_dec((uint64_t)(lock_mode < 0 ? 0 : lock_mode));
            dbg64_str(" desktop_mode=");
            dbg64_dec((uint64_t)(desk_mode < 0 ? 0 : desk_mode));
            dbg64_str(" applied=");
            dbg64_dec((uint64_t)(lock_mode == desk_mode ? (lock_mode < 0 ? 0 : lock_mode) : desk_mode));
            dbg64_str(lock_mode == desk_mode ? " screen=" : " (single adapted surface from gfx64; lock mode != desktop) screen=");
            dbg64_dec((uint64_t)fb_width());
            dbg64_str("x");
            dbg64_dec((uint64_t)fb_height());
            dbg64_nl();
            dbg64_line_end64();
        }
        // 清晰证据：背景 tile 的局部方差（与 [GFX64] blur tile 的 var_src 同源同位置）
        const int w = fb_width(), h = fb_height();
        int sw = 0, sh = 0;
        const uint32_t* surf = wall_surface64(&sw, &sh);
        const int vx = (w - 600) / 2, vy = h - THEME64_DOCK_MARGIN - THEME64_DOCK_H;
        const uint64_t var = surf ? gfx64_var64(surf, sw, vx, vy, 32, 32) : 0;
        g_bg_tile_var = var;
        if (lk_log_ok64(LK_LOG_LOCK)) {
            dbg64_line_begin64();
            dbg64_str("[LOCK64] bg tile x=");
            dbg64_dec((uint64_t)vx);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)vy);
            dbg64_str(" wh=32x32 var=");
            dbg64_dec(var);
            dbg64_str(" src=wall-surface blur=0 (sharp)");
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    if (lk_log_ok64(LK_LOG_LOCK)) {
        dbg64_line_begin64();
        dbg64_str("[LOCK64] lock screen shown time=");
        dbg64_str(g_time_str);
        dbg64_str(" date=");
        dbg64_str(g_date_str);
        dbg64_str(" weekday=");
        dbg64_str(weekday_zh64(g_time_wd));
        dbg64_str(" blur_bg=0 why=");
        dbg64_str(why ? why : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
}

static void enter_login64(const char* why) {
    g_state = LOCKLOGIN_STATE_LOGIN;
    g_pw_active = 0;
    g_pw_len = 0;
    g_pw[0] = 0;
    g_pw_fail = 0;
    ensure_blur64();
    anim_start64(g_blur_cur, 256, 1);
    g_overlay_valid = 0;                     // 界面换了（锁屏->登录）：覆盖层必须重画
    const User64Entry* u = sel_user64();
    if (lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOGIN64] login screen shown blur=");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_str(" anim=");
        dbg64_dec((uint64_t)g_anim_ms);
        dbg64_str("ms user=");
        dbg64_str(u ? u->name : "-");
        dbg64_str(" password=");
        dbg64_str((u && u->hash[0]) ? "required" : "none");
        dbg64_str(" avatar=");
        dbg64_str((u && u->avatar[0]) ? u->avatar : "builtin");
        dbg64_str(" why=");
        dbg64_str(why ? why : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
    log_userlist64();
    if (u && !u->avatar[0] && lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOCK64] avatar user=");
        dbg64_str(u->name);
        dbg64_str(" src=builtin:");
        dbg64_dec((uint64_t)(g_sel % 3));
        dbg64_str(" (procedural)\n");
        dbg64_line_end64();
    }
}

static void back_to_lock64(const char* why) {
    g_state = LOCKLOGIN_STATE_LOCK;
    g_overlay_valid = 0;                   // 回锁屏：覆盖层（面板+大号时间）必须重画
    g_pw_active = 0;
    g_pw_len = 0;
    g_pw[0] = 0;
    g_pw_fail = 0;
    if (lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOGIN64] lock screen restored (ESC) anim=");
        dbg64_dec((uint64_t)g_anim_ms);
        dbg64_str("ms blur ");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_str("->0 why=");
        dbg64_str(why ? why : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
    anim_start64(g_blur_cur, 0, 0);
}

static void do_login64(const char* how) {
    const User64Entry* u = sel_user64();
    if (!u) return;
    if (userdb64_login64(u->name) != 0) return;
    g_pw_active = 0;
    g_pw_len = 0;
    g_pw[0] = 0;
    g_pw_fail = 0;
    g_state = LOCKLOGIN_STATE_DESKTOP;
    g_anim_active = 0;
    g_blur_cur = 0;
    if (lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOGIN64] login ok user=");
        dbg64_str(u->name);
        dbg64_str(" uid=");
        dbg64_dec((uint64_t)u->uid);
        dbg64_str(" via=");
        dbg64_str(how ? how : "-");
        dbg64_str(" blur=");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_str(" (session identity set; GUI user fixed)\n");
        dbg64_line_end64();
    }
}

// 尝试提交登录（按需弹密码框）
static void submit_login64(const char* how) {
    const User64Entry* u = sel_user64();
    if (!u) return;
    if (!u->hash[0]) {                     // 没设密码：直接进桌面
        do_login64(how);
        return;
    }
    if (!g_pw_active) {                    // 第一次回车/点击：弹密码框
        g_pw_active = 1;
        g_pw_len = 0;
        g_pw[0] = 0;
        g_pw_fail = 0;
        g_caret_t0 = ticks64();
        g_caret_on = 1;
        if (lk_log_ok64(LK_LOG_LOGIN)) {
            dbg64_line_begin64();
            dbg64_str("[LOGIN64] password prompt user=");
            dbg64_str(u->name);
            dbg64_str(" box=");
            dbg64_dec((uint64_t)LOCKLOGIN_PW_W);
            dbg64_str("x");
            dbg64_dec((uint64_t)LOCKLOGIN_PW_H);
            dbg64_str(" btn=");
            dbg64_dec((uint64_t)LOCKLOGIN_PW_BTN);
            dbg64_str("x");
            dbg64_dec((uint64_t)LOCKLOGIN_PW_BTN);
            dbg64_str(" (frosted glass; white button with black text)\n");
            dbg64_line_end64();
        }
        return;
    }
    // 已有密码输入：校验
    const int idx = userdb64_find64(u->name);
    if (idx >= 0 && userdb64_check_password64(idx, g_pw) == 1) {
        do_login64(how);
        return;
    }
    g_pw_attempts++;
    g_pw_fail = 1;
    for (int i = 0; i < (int)sizeof(g_pw); i++) g_pw[i] = 0;   // 失败就丢掉输入（不留内存明文）
    g_pw_len = 0;
    if (lk_log_ok64(LK_LOG_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[LOGIN64] password wrong user=");
        dbg64_str(u->name);
        dbg64_str(" attempt=");
        dbg64_dec((uint64_t)g_pw_attempts);
        dbg64_str(" (input discarded; hash mismatch)\n");
        dbg64_line_end64();
    }
}

// ==================== 输入 ====================
static int hit64(int mx, int my, int x, int y, int w, int h) {
    return (mx >= x && my >= y && mx < x + w && my < y + h);
}

static void lock_input_touch64() { g_lock_t0 = ticks64(); }

static void handle_click64(int mx, int my) {
    if (g_state == LOCKLOGIN_STATE_LOCK) {
        enter_login64("click");
        return;
    }
    if (g_state != LOCKLOGIN_STATE_LOGIN) return;
    // 小头像（切换用户）
    const int nvis = userdb64_normal_count64();
    if (nvis > 1) {
        const int total = nvis * g_geo.small_d + (nvis - 1) * g_geo.small_gap;
        int x0 = (g_geo.W - total) / 2;
        if (x0 < 8) x0 = 8;
        for (int i = 0; i < nvis; i++) {
            const int x = x0 + i * (g_geo.small_d + g_geo.small_gap);
            if (hit64(mx, my, x, g_geo.small_y, g_geo.small_d, g_geo.small_d)) {
                if (i != g_sel) {
                    g_sel = i;
                    g_pw_active = 0;
                    g_pw_len = 0;
                    g_pw_fail = 0;
                    const User64Entry* u = sel_user64();
                    if (lk_log_ok64(LK_LOG_LOGIN)) {
                        dbg64_line_begin64();
                        dbg64_str("[LOGIN64] user select idx=");
                        dbg64_dec((uint64_t)i);
                        dbg64_str(" user=");
                        dbg64_str(u ? u->name : "-");
                        dbg64_str(" avatar=");
                        dbg64_str((u && u->avatar[0]) ? u->avatar : "builtin");
                        dbg64_nl();
                        dbg64_line_end64();
                    }
                }
                return;
            }
        }
    }
    // 确认按钮
    if (g_pw_active && hit64(mx, my, g_geo.pw_btn_x, g_geo.pw_btn_y, g_geo.pw_btn_d, g_geo.pw_btn_d)) {
        if (lk_log_ok64(LK_LOG_LOGIN)) {
            dbg64_line_begin64();
            dbg64_str("[LOGIN64] login button key=click\n");
            dbg64_line_end64();
        }
        submit_login64("click-confirm");
        return;
    }
    // 密码框本体：点击 = 聚焦（无操作，保持焦点）
    if (g_pw_active && hit64(mx, my, g_geo.pw_x, g_geo.pw_y, g_geo.pw_w, g_geo.pw_h)) return;
    // 登录按钮
    if (hit64(mx, my, g_geo.btn_x, g_geo.btn_y, g_geo.btn_w, g_geo.btn_h)) {
        if (lk_log_ok64(LK_LOG_LOGIN)) {
            dbg64_line_begin64();
            dbg64_str("[LOGIN64] login button key=click\n");
            dbg64_line_end64();
        }
        submit_login64("click");
        return;
    }
}

static void handle_key64(uint8_t c) {
    lock_input_touch64();
    if (g_state == LOCKLOGIN_STATE_LOCK) {
        if (c == '\n' || c == '\r' || c == ' ') enter_login64("key");
        return;
    }
    if (g_state != LOCKLOGIN_STATE_LOGIN) return;
    if (g_pw_active) {
        if (c == 0x1B) {                          // ESC：取消密码框（留在登录界面）
            g_pw_active = 0;
            g_pw_len = 0;
            for (int i = 0; i < (int)sizeof(g_pw); i++) g_pw[i] = 0;
            if (lk_log_ok64(LK_LOG_LOGIN)) {
                dbg64_line_begin64();
                dbg64_str("[LOGIN64] login cancel (ESC from password box)\n");
                dbg64_line_end64();
            }
            return;
        }
        if (c == '\n' || c == '\r') {
            if (lk_log_ok64(LK_LOG_LOGIN)) {
                dbg64_line_begin64();
                dbg64_str("[LOGIN64] login button key=enter\n");
                dbg64_line_end64();
            }
            submit_login64("key-enter");
            return;
        }
        if (c == '\b' || c == 0x7F) {
            if (g_pw_len > 0) g_pw[--g_pw_len] = 0;
            g_pw_fail = 0;
            return;
        }
        if (c >= 0x20 && c < 0x7F && g_pw_len < (int)sizeof(g_pw) - 1) {
            g_pw[g_pw_len++] = (char)c;
            g_pw[g_pw_len] = 0;
            g_pw_fail = 0;
        }
        return;
    }
    if (c == 0x1B) { back_to_lock64("esc"); return; }
    // 左右方向键：切换选中的普通用户（Windows 风格；也给自动验收一条**确定性**的切换路径，
    // 不依赖鼠标闭环定位。0xFB/0xFC = input.cpp 的 NAV_LEFT/NAV_RIGHT）
    if (c == 0xFB || c == 0xFC) {
        const int n = userdb64_normal_count64();
        if (n > 0) {
            g_sel = (c == 0xFC) ? (g_sel + 1) % n : (g_sel + n - 1) % n;
            g_pw_active = 0;
            g_pw_len = 0;
            g_pw_fail = 0;
            g_overlay_valid = 0;
            const User64Entry* u = sel_user64();
            if (lk_log_ok64(LK_LOG_LOGIN)) {
                dbg64_line_begin64();
                dbg64_str("[LOGIN64] user select idx=");
                dbg64_dec((uint64_t)g_sel);
                dbg64_str(" user=");
                dbg64_str(u ? u->name : "-");
                dbg64_str(" avatar=");
                dbg64_str((u && u->avatar[0]) ? u->avatar : "builtin");
                dbg64_str(" via=arrow\n");
                dbg64_line_end64();
            }
        }
        return;
    }
    if (c == '\n' || c == '\r' || c == ' ') {
        if (lk_log_ok64(LK_LOG_LOGIN)) {
            dbg64_line_begin64();
            dbg64_str("[LOGIN64] login button key=enter\n");
            dbg64_line_end64();
        }
        submit_login64("key-enter");
    }
}

// ==================== 每帧 tick（gui64 主循环 / 开机循环共用）====================
static void locklogin64_tick_once64() {
    // 1) 时间（每秒）：字符串变了就重画"时间/日期盒子"那一小块（不整屏，QEMU 下整屏很贵）
    {
        char prev[48];
        int k = 0;
        for (int i = 0; g_time_str[i] && k < 47; i++) prev[k++] = g_time_str[i];
        prev[k] = 0;
        refresh_time64(0);
        if (g_state == LOCKLOGIN_STATE_LOCK && (s_eq(prev, g_time_str) == 0)) {
            const int x0 = (g_geo.time_box_x < g_geo.date_box_x ? g_geo.time_box_x : g_geo.date_box_x) - 8;
            const int y0 = g_geo.time_box_y - 8;
            const int x1 = (g_geo.time_box_x + g_geo.time_box_w > g_geo.date_box_x + g_geo.date_box_w
                            ? g_geo.time_box_x + g_geo.time_box_w : g_geo.date_box_x + g_geo.date_box_w) + 8;
            const int y1 = g_geo.date_box_y + g_geo.date_box_h + 8;
            if (x1 > x0 && y1 > y0) paint64(x0, y0, x1 - x0, y1 - y0);
        }
    }
    // 2) 输入
    const int mx = mouse_get_x(), my = mouse_get_y();
    if (mx != g_cur_x || my != g_cur_y) {
        const int ox = g_cur_x, oy = g_cur_y;
        g_prev_cur_x = ox; g_prev_cur_y = oy;
        g_cur_x = mx; g_cur_y = my;
        if (ox >= 0) paint64(ox - 2, oy - 2, 18, 22);
        paint64(mx - 2, my - 2, 18, 22);
    }
    if (mouse_button_pressed(0)) { mouse_consume_pressed(0); handle_click64(mx, my); }
    uint8_t c = 0;
    int had_key = 0;
    while (kbd_pop_char(&c)) { had_key = 1; handle_key64(c); }
    // 2b) 口令框区域局部重画（输入/闪烁；比整屏便宜一个数量级）
    if (g_pw_active && g_state != LOCKLOGIN_STATE_DESKTOP) {
        const int x0 = g_geo.pw_x - 8, y0 = g_geo.pw_y - 8;
        const int x1 = g_geo.pw_btn_x + g_geo.pw_btn_d + 8;
        const int y1 = g_geo.err_y + 26;
        if (had_key) paint64(x0, y0, x1 - x0, y1 - y0);
        if ((uint32_t)(ticks64() - g_caret_t0) >= PIT_HZ_64 / 2) {     // 光标闪烁
            g_caret_t0 = ticks64();
            g_caret_on = !g_caret_on;
            paint64(x0, y0, x1 - x0, y1 - y0);
        }
    } else if (had_key && g_state != LOCKLOGIN_STATE_DESKTOP) {
        g_overlay_valid = 0;                     // 其他按键：下一次绘制重画覆盖层
    }
    // 4) 动画
    if (anim_tick64()) paint_full64();
    // 5) 自动登录（★ 缺陷 4 修复：**默认关闭**。）
    //    需求：登录界面必须等用户**显式输入**（回车 / 点登录按钮）才进桌面；无密码用户也不许自动跳过。
    //    所以 ui.login.auto 的默认值从 1 改成 0；只有显式 `cfg set ui.login.auto 1`（或打了这个键的老盘）
    //    才恢复"空闲 N 秒自动进桌面"的旧行为，且只对**无密码**用户生效。
    //    ★ 每秒重读一次配置：终端里 `cfg set ui.login.auto 1` 能立刻生效。
    {
        static uint32_t cfg_recheck = 0;
        if ((uint32_t)(ticks64() - cfg_recheck) >= PIT_HZ_64) {
            cfg_recheck = ticks64();
            g_auto_login = config64_get_bool64("ui.login.auto", 0);
            const int ms = config64_get_int64("ui.login.auto_delay_ms", (int)g_auto_ms);
            if (ms >= 1000 && ms <= 60000) g_auto_ms = (uint32_t)ms;
        }
    }
    if (g_state != LOCKLOGIN_STATE_DESKTOP && g_auto_login && !g_pw_active && !g_auto_fired) {
        const User64Entry* u = sel_user64();
        if (u && !u->hash[0] && g_auto_ms > 0 && (uint32_t)(ticks64() - g_lock_t0) >= ms_to_ticks64(g_auto_ms)) {
            g_auto_fired = 1;
            if (lk_log_ok64(LK_LOG_LOCK)) {
                dbg64_line_begin64();
                dbg64_str("[LOCK64] auto login user=");
                dbg64_str(u->name);
                dbg64_str(" delay=");
                dbg64_dec((uint64_t)g_auto_ms);
                dbg64_str("ms (ui.login.auto=1; set 0 to require a key/click)\n");
                dbg64_line_end64();
            }
            if (g_state == LOCKLOGIN_STATE_LOCK) enter_login64("auto");
            submit_login64("auto");
            paint_full64();
        }
    }
    // 6) 状态切换时整屏重绘
    if (g_state != LOCKLOGIN_STATE_DESKTOP) {
        static int last_state = -1;
        if (last_state != g_state) {
            last_state = g_state;
            paint_full64();
        }
    }
}

// 开机路径：锁屏 -> 登录 -> 桌面（返回时 state == DESKTOP）
void locklogin64_run64() {
    locklogin64_init64();
    (void)locklogin64_selftest64();      // 纯逻辑自检（数字区间/星期换算/用户过滤）→ [LOCK64] selftest PASS
    refresh_time64(1);
    g_lock_logged = 0;
    g_wall_logged = 0;
    g_auto_fired = 0;
    g_blur_cur = 0;
    g_anim_active = 0;
    g_cur_x = mouse_get_x(); g_cur_y = mouse_get_y();
    g_prev_cur_x = g_cur_x; g_prev_cur_y = g_cur_y;
    g_state = LOCKLOGIN_STATE_LOCK;
    enter_lock64("boot");
    paint_full64();                  // 先把锁屏画出来（清晰背景 + 大号时间）
    // 20px 背景模糊**算一次**缓存起来（QEMU TCG 下要几百 ms）——放在首帧之后，
    // 锁屏先可见；登录时（清晰->模糊）直接用缓存，动画才跑得动。
    ensure_blur64();
    if (lk_log_ok64(LK_LOG_LOCK)) {
        dbg64_line_begin64();
        dbg64_str("[LOCK64] bg blur ready r=");
        dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
        dbg64_str(" ticks=");
        dbg64_dec((uint64_t)g_blur_ms);
        dbg64_str("ms=");
        dbg64_dec((uint64_t)(g_blur_ms * 1000u / PIT_HZ_64));
        dbg64_str(" cached=1 (computed once, reused by the login fade)\n");
        dbg64_line_end64();
    }
    // ★ 自动登录计时从"锁屏真正可交互"开始算：开机时那一次 20px 背景模糊（QEMU TCG 下几秒）
    //   不该吃掉用户看到的锁屏时间，也不该让绘制/模糊期间注入的输入被自动登录抢先。
    g_lock_t0 = ticks64();
    while (g_state != LOCKLOGIN_STATE_DESKTOP) {
        panic64_watchdog_kick64();
        (void)config64_tick64();
        locklogin64_tick_once64();
        __asm__ volatile("pause");
    }
    fb_reset_clip();
}

// 终端 `loginctl lock`：回到锁屏（gui64 主循环的 hook 会接管输入与重绘）
void locklogin64_lock64(const char* why) {
    if (g_state == LOCKLOGIN_STATE_LOCK) return;
    kbd_drain();                    // 丢掉残留按键（含刚刚那个回车）
    mouse_drain();
    g_auto_fired = 0;
    g_lock_logged = 0;
    g_cur_x = mouse_get_x(); g_cur_y = mouse_get_y();
    g_prev_cur_x = g_cur_x; g_prev_cur_y = g_cur_y;
    if (lk_log_ok64(LK_LOG_LOCK)) {
        dbg64_line_begin64();
        dbg64_str("[LOCK64] lock requested by=");
        dbg64_str(why ? why : "?");
        dbg64_str(" (loginctl lock)\n");
        dbg64_line_end64();
    }
    g_blur_cur = 0;
    g_anim_active = 0;
    enter_lock64("loginctl lock");
    locklogin64_tick_once64();
    (void)locklogin64_selftest64();
}

void locklogin64_init64() {
    for (int i = 0; i < LK_LOG_N; i++) g_lk_used[i] = 0;
    g_auto_login = config64_get_bool64("ui.login.auto", 0);   // ★ 缺陷 4：默认 0 = 只等用户输入
    g_auto_ms = (uint32_t)config64_get_int64("ui.login.auto_delay_ms", 8000);
    // 默认选中"上次登录的用户"（找不到就第一个普通用户）
    const char* last = userdb64_last_user64();
    g_sel = 0;
    if (last && last[0]) {
        const int n = userdb64_normal_count64();
        for (int i = 0; i < n; i++) {
            const User64Entry* u = userdb64_visible64(i);
            if (u && s_eq(u->name, last)) { g_sel = i; break; }
        }
    }
    clamp_sel64();
    g_lock_t0 = ticks64();
}

int locklogin64_state64() { return g_state; }
const char* locklogin64_state_name64() {
    switch (g_state) {
        case LOCKLOGIN_STATE_LOCK: return "LOCK";
        case LOCKLOGIN_STATE_LOGIN: return "LOGIN";
        default: return "DESKTOP";
    }
}
int locklogin64_active64() { return g_state != LOCKLOGIN_STATE_DESKTOP; }

// gui64 主循环的 hook：状态 != DESKTOP 时由本层接管（返回 1 = 本帧已被消费）
int locklogin64_tick64() {
    if (g_state == LOCKLOGIN_STATE_DESKTOP) return 0;
    locklogin64_tick_once64();
    return 1;
}

// ==================== 自检 ====================
int locklogin64_selftest64() {
    int fails = 0;
    // 1) 需求里的数字必须落在区间里（改坏了这里立刻红）
    if (LOCKLOGIN_TIME_INK < 64 || LOCKLOGIN_TIME_INK > 80) fails |= 1;
    if (LOCKLOGIN_DATE_INK < 16 || LOCKLOGIN_DATE_INK > 20) fails |= 2;
    if (LOCKLOGIN_ANIM_MS < 250 || LOCKLOGIN_ANIM_MS > 350) fails |= 4;
    if (LOCKLOGIN_AVATAR_PX < 96 || LOCKLOGIN_AVATAR_PX > 112) fails |= 8;
    if (LOCKLOGIN_BTN_PX != 96 || LOCKLOGIN_BTN_R != 24) fails |= 16;
    if (LOCKLOGIN_PW_W != 360 || LOCKLOGIN_PW_H != 48 || LOCKLOGIN_PW_BTN != 48) fails |= 32;
    if (LOCKLOGIN_BLUR_PX != 20) fails |= 64;
    // 2) 星期换算（Zeller）：2026-09-25 = 星期五(5)、2000-01-01 = 星期六(6)
    if (weekday_ymd64(2026, 9, 25) != 5) fails |= 128;
    if (weekday_ymd64(2000, 1, 1) != 6) fails |= 256;
    // 3) 登录界面代码路径只看普通用户（root 不进列表）
    const int n = userdb64_normal_count64();
    for (int i = 0; i < n; i++) {
        const User64Entry* u = userdb64_visible64(i);
        if (!u || (u->flags & USERDB64_F_ROOT) || u->hidden) fails |= 512;
    }
    if (userdb64_find_root64() == nullptr) fails |= 512;
    // 4) 时间/日期字符串格式（HH:MM / YYYY-MM-DD 星期X）
    refresh_time64(1);
    if (s_len(g_time_str) != 5 || g_time_str[2] != ':') fails |= 1024;
    if (s_len(g_date_str) < 10 || g_date_str[4] != '-' || g_date_str[7] != '-') fails |= 1024;
    // 5) 一秒宽度 > 0（动画时间基）
    if (PIT_HZ_64 == 0) fails |= 2048;

    dbg64_line_begin64();
    dbg64_str("[LOCK64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" time_ink=");
    dbg64_dec((uint64_t)LOCKLOGIN_TIME_INK);
    dbg64_str(" date_ink=");
    dbg64_dec((uint64_t)LOCKLOGIN_DATE_INK);
    dbg64_str(" blur=");
    dbg64_dec((uint64_t)LOCKLOGIN_BLUR_PX);
    dbg64_str(" anim=");
    dbg64_dec((uint64_t)LOCKLOGIN_ANIM_MS);
    dbg64_str("ms avatar=");
    dbg64_dec((uint64_t)LOCKLOGIN_AVATAR_PX);
    dbg64_str(" btn=");
    dbg64_dec((uint64_t)LOCKLOGIN_BTN_PX);
    dbg64_str("x");
    dbg64_dec((uint64_t)LOCKLOGIN_BTN_PX);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)LOCKLOGIN_BTN_R);
    dbg64_str(" pw=");
    dbg64_dec((uint64_t)LOCKLOGIN_PW_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)LOCKLOGIN_PW_H);
    dbg64_nl();
    dbg64_line_end64();
    return fails;
}
