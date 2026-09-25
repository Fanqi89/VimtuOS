// startmenu64.cpp - ★ P2：开始菜单（Windows 11 风格）+ P2 UI 工具箱（四个二级弹窗/通知共用）
//
// 读代码顺序建议：1) 工具箱（混合圆角/亚克力/线性图标/文本）2) 几何 3) 绘制 4) 输入 5) 自检。
// 一切视觉数字来自 theme64.h 的 Token；颜色来自当前主题 Token（浅色深灰线 / 暗色浅灰线自动切换）。
//
// 与外壳（gui64.cpp）的接线：
//   * Dock 开始按钮（dock_press64 idx=0）-> startmenu64_toggle64()
//   * render() 里窗口之后 -> startmenu64_draw64()（再之后是 panels64_draw64）
//   * handle_mouse_press/handle_mouse/handle_keyboard -> startmenu64_handle_*（面板优先，见 gui64.cpp）
#include "startmenu64.h"
#include "panels64.h"

#include "gui64.h"          // 应用启动入口 / gui64_dirty / 屏尺寸
#include "gfx64.h"          // 圆角/阴影/毛玻璃/壁纸面
#include "fb.h"
#include "font.h"
#include "input.h"          // ESC / 方向键 / Caps / 中英指示
#include "x86_64.h"         // ticks64/ms_to_ticks64/rtc_get_*
#include "debug64.h"
#include "userdb64.h"       // 当前登录用户（左下角头像 + 用户名）
#include "locklogin64.h"    // 锁定 -> 回锁屏
#include "e1000_64.h"       // 以太网真实链路（网络图标：有线 = 以太网）
#include "mem_64.h"

// ==================== 小工具（内核里没有 libc）====================
static int s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void s_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static void s_cat(char* dst, const char* src, int cap) {
    int i = s_len(dst), k = 0;
    while (src && src[k] && i < cap - 1) dst[i++] = src[k++];
    dst[i] = 0;
}
// 十进制/定宽十进制进 buf（返回写入长度）
static int u_dec(char* b, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    while (n) b[k++] = t[--n];
    b[k] = 0;
    return k;
}
static int u_dec2(char* b, int v) {          // 两位定宽（时间/日期用）
    b[0] = (char)('0' + (v / 10) % 10);
    b[1] = (char)('0' + v % 10);
    b[2] = 0;
    return 2;
}
static int u_dec4(char* b, int v) {
    for (int i = 0; i < 4; i++) b[i] = (char)('0' + (v / (i == 0 ? 1000 : i == 1 ? 100 : i == 2 ? 10 : 1)) % 10);
    b[4] = 0;
    return 4;
}
static char s_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }
static bool s_has_icase(const char* hay, const char* needle) {
    if (!needle || !needle[0]) return false;
    for (int i = 0; hay && hay[i]; i++) {
        int k = 0;
        while (needle[k] && hay[i + k] && s_lower(hay[i + k]) == s_lower(needle[k])) k++;
        if (!needle[k]) return true;
    }
    return false;
}
// Zeller 同余：0=周日 … 6=周六（RTC 的星期字段不一定可信，用日期自己算，与 locklogin64 同口径）
static int weekday_ymd64(int y, int m, int d) {
    if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
    int yy = y, mm = m;
    if (mm < 3) { mm += 12; yy -= 1; }
    const int k = yy % 100, j = yy / 100;
    const int h = (d + (13 * (mm + 1)) / 5 + k + k / 4 + j / 4 + 5 * j) % 7;
    return ((h + 6) % 7 + 7) % 7;            // h: 0=周六 -> 转成 0=周日
}
// 角度表（cos*1024，0..90°）：整数三角，无浮点、无 libm。
static const int16_t kCos1024[91] = {
    1024, 1024, 1023, 1022, 1021, 1020, 1018, 1017, 1014, 1011, 1008, 1005, 1002,  998,  994,  989,
     984,  979,  973,  968,  962,  956,  949,  943,  936,  928,  920,  913,  904,  896,  887,  878,
     868,  859,  849,  839,  828,  818,  807,  796,  785,  773,  761,  749,  737,  724,  711,  698,
     685,  672,  658,  644,  630,  616,  602,  587,  573,  558,  543,  528,  512,  496,  481,  465,
     449,  433,  417,  401,  384,  368,  351,  334,  317,  300,  283,  265,  248,  231,  214,  196,
     179,  162,  145,  127,  110,   93,   76,   59,   42,   25,    0
};
static inline int p2_cos64(int deg) {
    int d = ((deg % 360) + 360) % 360;
    if (d > 180) d = 360 - d;
    if (d > 90) d = 180 - d;
    return kCos1024[d];
}
static inline int p2_sin64(int deg) { return p2_cos64(90 - deg); }
static int days_in_month64(int y, int m) {
    static const int dm[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) return 30;
    if (m == 2) {
        const bool leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    return dm[m - 1];
}

// ==================== 打点（每类上限防刷屏）====================
static int g_log_budget = 260;      // [START64] 常规行上限
static void lg(const char* tag, const char* a, int64_t v, const char* b) {
    if (g_log_budget <= 0) return;
    g_log_budget--;
    dbg64_line_begin64();
    dbg64_str("[START64] ");
    dbg64_str(tag);
    if (a) dbg64_str(a);
    if (v >= 0) dbg64_dec((uint64_t)v);
    if (b) dbg64_str(b);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== P2 UI 工具箱 ====================
static inline uint32_t p2_blend64(uint32_t dst, uint32_t c, int a) {
    if (a <= 0) return dst;
    if (a >= 255) return 0xFF000000u | (c & 0xFFFFFFu);
    const int dr = (int)((dst >> 16) & 0xFF), dg = (int)((dst >> 8) & 0xFF), db = (int)(dst & 0xFF);
    const int r = dr + (((int)((c >> 16) & 0xFF) - dr) * a) / 255;
    const int g = dg + (((int)((c >> 8) & 0xFF) - dg) * a) / 255;
    const int b = db + (((int)(c & 0xFF) - db) * a) / 255;
    return rgb((uint8_t)r, (uint8_t)g, (uint8_t)b);
}
static int p2_isqrt64(int64_t v) {
    if (v <= 0) return 0;
    int64_t x = v, y = (x + 1) / 2;
    while (y < x) { x = y; y = (x + v / x) / 2; }
    return (int)x;
}
// 圆角覆盖率（8.8 定点：像素中心 (px8,py8) 相对圆心 (cx8,cy8)，半径 r 像素）—— 1px 抗锯齿
static int corner_cov64(int px8, int py8, int cx8, int cy8, int r) {
    if (r <= 0) return 255;
    const int dx = px8 - cx8, dy = py8 - cy8;
    const int d8 = p2_isqrt64((int64_t)dx * dx + (int64_t)dy * dy);
    int cov = r * 256 - d8 + 128;
    if (cov <= 0) return 0;
    if (cov >= 256) return 255;
    return cov;
}
// 混合圆角（上 rt / 下 rb）矩形在像素 (i,j) 的覆盖率
static int mixed_cov64(int i, int j, int w, int h, int rt, int rb) {
    const int x8 = i * 256 + 128, y8 = j * 256 + 128;
    if (rt > w / 2) rt = w / 2;
    if (rb > w / 2) rb = w / 2;
    if (rt > h / 2) rt = h / 2;
    if (rb > h / 2) rb = h / 2;
    if (j < rt) {
        if (i < rt)      return corner_cov64(x8, y8, rt * 256, rt * 256, rt);
        if (i >= w - rt) return corner_cov64(x8, y8, (w - rt) * 256, rt * 256, rt);
        return 255;
    }
    if (j >= h - rb) {
        if (i < rb)      return corner_cov64(x8, y8, rb * 256, (h - rb) * 256, rb);
        if (i >= w - rb) return corner_cov64(x8, y8, (w - rb) * 256, (h - rb) * 256, rb);
        return 255;
    }
    return 255;
}

void p2ui_fill_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t c, int a) {
    if (w <= 0 || h <= 0 || a <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int cov = mixed_cov64(i, j, w, h, rt, rb);
            if (cov <= 0) continue;
            gfx64_blend64(x + i, y + j, c, a * cov / 255);
        }
}

void p2ui_acrylic_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t tint, int a,
                          const Theme64Tokens* t) {
    if (w <= 0 || h <= 0) return;
    const Theme64Tokens* tk = t ? t : theme64_tokens64();
    int bw = 0, bh = 0;
    // 背景层：**预模糊壁纸**（模糊只算过一次，Token 半径 THEME64_BLUR_BACKDROP）
    const uint32_t* back = gfx64_wall_blur64(&bw, &bh);
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            const int cov = mixed_cov64(i, j, w, h, rt, rb);
            if (cov <= 0) continue;
            const int sx = x + i, sy = y + j;
            uint32_t src = tk->desktop_base;
            if (back && sx >= 0 && sy >= 0 && sx < bw && sy < bh) src = back[(uint64_t)sy * bw + sx];
            const uint32_t glass = p2_blend64(src, tint, a);
            gfx64_blend64(sx, sy, glass, cov);
        }
    }
}

void p2ui_stroke_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t c, int a) {
    if (w <= 2 || h <= 2 || a <= 0) return;
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            const int outer = mixed_cov64(i, j, w, h, rt, rb);
            if (outer <= 0) continue;
            const int inner = mixed_cov64(i - 1, j - 1, w - 2, h - 2,
                                          rt > 0 ? rt - 1 : 0, rb > 0 ? rb - 1 : 0);
            const int edge = outer - inner;
            if (edge <= 0) continue;
            gfx64_blend64(x + i, y + j, c, a * edge / 255);
        }
}

void p2ui_shadow64(int x, int y, int w, int h, int r, const Theme64Tokens* t) {
    // 双层浅阴影：近层 0/2/4 a=0.08 + 远层 0/12/32 a=0.12（Token，一次调用画两层）
    gfx64_shadow64(x, y, w, h, r, t ? t : theme64_tokens64());
}

void p2ui_popup64(int x, int y, int w, int h, int rt, int rb, const Theme64Tokens* t,
                  uint32_t tint, int a) {
    const Theme64Tokens* tk = t ? t : theme64_tokens64();
    p2ui_shadow64(x, y, w, h, rt, tk);                    // 1) 双层浅阴影
    p2ui_acrylic_mixed64(x, y, w, h, rt, rb, tint, a, tk); // 2) 亚克力（背景模糊 20–30 Token）
    p2ui_stroke_mixed64(x, y, w, h, rt, rb, 0xFFFFFFu, THEME64_A_EDGE);  // 3) 1px 半透明白高光边
    if (tk->dark)
        for (int i = rt; i < w - rt; i++) gfx64_blend64(x + i, y + 1, tk->highlight, THEME64_A_HIGHLIGHT);
}

// ---- 线性图元（端点圆润：沿线段/圆环按点到线的距离铺 1px 覆盖，粗度 = thick）----
void p2ui_line64(int x0, int y0, int x1, int y1, int thick, uint32_t c, int a) {
    if (thick < 1) thick = 1;
    const int half = thick / 2;
    int bx0 = x0 < x1 ? x0 : x1, bx1 = x0 < x1 ? x1 : x0;
    int by0 = y0 < y1 ? y0 : y1, by1 = y0 < y1 ? y1 : y0;
    const int dx = x1 - x0, dy = y1 - y0;
    const int64_t len2 = (int64_t)dx * dx + (int64_t)dy * dy;
    for (int py = by0 - half - 2; py <= by1 + half + 2; py++) {
        for (int px = bx0 - half - 2; px <= bx1 + half + 2; px++) {
            // 点到线段距离（定点近似：先求投影参数 t，再算距离）
            int64_t t = 0;
            if (len2 > 0) {
                t = ((int64_t)(px - x0) * dx + (int64_t)(py - y0) * dy) * 256 / len2;
                if (t < 0) t = 0;
                if (t > 256) t = 256;
            }
            const int qx = x0 + (int)(t * dx / 256);
            const int qy = y0 + (int)(t * dy / 256);
            const int ddx = px - qx, ddy = py - qy;
            const int d8 = p2_isqrt64(((int64_t)ddx * ddx + (int64_t)ddy * ddy) * 256);
            const int rr8 = thick * 128;                   // 半径 = thick/2（8.8 定点），圆端点
            int cov = rr8 - d8 + 128;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            gfx64_blend64(px, py, c, a * cov / 256);
        }
    }
}
void p2ui_ring64(int cx, int cy, int r, int thick, uint32_t c, int a) {
    if (thick < 1) thick = 1;
    const int o = r + thick / 2 + 2;
    for (int j = -o; j <= o; j++)
        for (int i = -o; i <= o; i++) {
            const int d8 = p2_isqrt64(((int64_t)i * i + (int64_t)j * j) * 256);
            int diff = d8 - r * 256;
            if (diff < 0) diff = -diff;
            int cov = thick * 128 - diff + 128;              // 环 |d-r| <= thick/2（带 1px AA）
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            gfx64_blend64(cx + i, cy + j, c, a * cov / 256);
        }
}
void p2ui_fill_circle64(int cx, int cy, int r, uint32_t c, int a) {
    for (int j = -r - 1; j <= r + 1; j++)
        for (int i = -r - 1; i <= r + 1; i++) {
            const int d8 = p2_isqrt64(((int64_t)i * i + (int64_t)j * j) * 256);
            int cov = r * 256 - d8 + 128;
            if (cov <= 0) continue;
            if (cov > 256) cov = 256;
            gfx64_blend64(cx + i, cy + j, c, a * cov / 256);
        }
}
void p2ui_ellipse64(int cx, int cy, int rx, int ry, int thick, uint32_t c, int a) {
    // 参数采样 + 圆点描边（rx/ry 很小，96 步足够平滑）
    const int steps = 96;
    int px = 0, py = 0;
    for (int s = 0; s <= steps; s++) {
        const int ang = s * 360 / steps;
        const int nx = cx + p2_cos64(ang) * rx / 1024;
        const int ny = cy + p2_sin64(ang) * ry / 1024;
        if (s > 0) p2ui_line64(px, py, nx, ny, thick, c, a);
        px = nx; py = ny;
    }
}
void p2ui_arc64(int cx, int cy, int r, int deg0, int deg1, int thick, uint32_t c, int a) {
    // 屏幕坐标：0° = 右，角度顺时针增加（y 向下）。Wi-Fi 弧 = 225°..315°，钟形 = 180°..360°。
    const int span = deg1 - deg0;
    int steps = span < 0 ? -span : span;
    if (steps < 2) steps = 2;
    if (steps > 96) steps = 96;
    int px = 0, py = 0;
    for (int s = 0; s <= steps; s++) {
        const int ang = deg0 + span * s / steps;
        const int nx = cx + p2_cos64(ang) * r / 1024;
        const int ny = cy + p2_sin64(ang) * r / 1024;
        if (s > 0) p2ui_line64(px, py, nx, ny, thick, c, a);
        px = nx; py = ny;
    }
}

int  p2ui_line_h64() { font_select(2); return font_line_height(); }
int  p2ui_text_w64(const char* s) { font_select(2); return font_text_width(s); }
void p2ui_text64(int x, int y, const char* s, uint32_t c) { font_select(2); font_draw_text(x, y, s, c); }
uint32_t p2ui_icon_color64(const Theme64Tokens* t) {
    const Theme64Tokens* tk = t ? t : theme64_tokens64();
    return tk->dark ? tk->text : rgb(48, 52, 60);            // 浅色主题深灰/黑线、暗色白/浅灰线
}
uint32_t p2ui_icon_dim_color64(const Theme64Tokens* t) {
    const Theme64Tokens* tk = t ? t : theme64_tokens64();
    return tk->text_dim;
}

// ==================== 线性图标 ====================
static void icon_globe64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s / 2;
    p2ui_ring64(cx, cy, r, lw, c, a);
    p2ui_ellipse64(cx, cy, r / 2, r, lw, c, a);                     // 经线
    p2ui_line64(cx - r, cy, cx + r, cy, lw, c, a);                  // 赤道
    p2ui_line64(cx - r * 4 / 5, cy - r / 2, cx + r * 4 / 5, cy - r / 2, lw, c, a);
    p2ui_line64(cx - r * 4 / 5, cy + r / 2, cx + r * 4 / 5, cy + r / 2, lw, c, a);
}
static void icon_ethernet64(int x, int y, int s, uint32_t c, int a, int lw) {
    // RJ45 插头（极简线条）：插头体 + 上方卡扣 + 3 根引脚
    const int w = s * 3 / 5, h = s * 2 / 5;
    const int bx = x + (s - w) / 2, by = y + (s - h) / 2 + 1;
    p2ui_stroke_mixed64(bx, by, w, h, 2, 2, c, a);
    p2ui_line64(bx + w / 2, by, bx + w / 2, by - s / 6, lw, c, a);   // 线缆
    for (int i = 0; i < 3; i++) {
        const int px = bx + w / 4 + i * (w / 4);
        p2ui_line64(px, by + h - 1, px, by + h / 2, 1, c, a);
    }
}
static void icon_wifi64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s / 2;
    for (int i = 0; i < 3; i++)
        p2ui_arc64(cx, cy + r - 1, r - i * (r / 3), 225, 315, lw, c, a);
    p2ui_fill_circle64(cx, cy + r - 1, lw, c, a);
}
static void icon_sound64(int x, int y, int s, uint32_t c, int a, int lw, bool muted) {
    const int cy = y + s / 2;
    const int wx = x + s / 6, ww = s / 5, wh = s / 3;
    p2ui_fill_mixed64(wx, cy - wh / 2, ww, wh, 1, 1, c, a);          // 音箱体
    // 喇叭锥（用两条斜线 + 顶/底边画成梯形轮廓）
    p2ui_line64(wx + ww, cy - wh / 2, wx + ww + s / 4, cy - s / 3, lw, c, a);
    p2ui_line64(wx + ww, cy + wh / 2, wx + ww + s / 4, cy + s / 3, lw, c, a);
    p2ui_line64(wx + ww + s / 4, cy - s / 3, wx + ww + s / 4, cy + s / 3, lw, c, a);
    if (muted) {
        const int mx = x + s * 3 / 4;
        p2ui_line64(mx - s / 6, cy - s / 6, x + s, cy + s / 6, lw, c, a);
        p2ui_line64(mx - s / 6, cy + s / 6, x + s, cy - s / 6, lw, c, a);
    } else {
        p2ui_arc64(x + s / 2 + s / 8, cy, s / 3, 300, 60, lw, c, a);
        p2ui_arc64(x + s / 2 + s / 8, cy, s / 2 - 1, 300, 60, lw, c, a);
    }
}
static void icon_bell64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s * 2 / 5;
    p2ui_arc64(cx, cy, r, 180, 360, lw, c, a);                       // 钟形上半圆
    p2ui_line64(cx - r, cy, cx - r, cy + s / 6, lw, c, a);           // 两侧竖边
    p2ui_line64(cx + r, cy, cx + r, cy + s / 6, lw, c, a);
    p2ui_line64(cx - r, cy + s / 6, cx + r, cy + s / 6, lw, c, a);   // 钟口
    p2ui_line64(cx, cy + s / 6, cx, cy + s / 4, lw, c, a);           // 钟舌
    p2ui_line64(cx - s / 8, cy - r, cx + s / 8, cy - r, lw, c, a);   // 顶提手
}
static void icon_search64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s * 2 / 5;
    p2ui_ring64(cx - s / 10, cy - s / 10, r, lw, c, a);
    p2ui_line64(cx - s / 10 + r * 3 / 4, cy - s / 10 + r * 3 / 4, cx + s / 2 - 1, cy + s / 2 - 1, lw, c, a);
}
static void icon_close64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s / 3;
    p2ui_line64(cx - r, cy - r, cx + r, cy + r, lw, c, a);
    p2ui_line64(cx - r, cy + r, cx + r, cy - r, lw, c, a);
}
static void icon_power64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s * 2 / 5;
    p2ui_arc64(cx, cy + 1, r, 300, 240, lw, c, a);
    p2ui_line64(cx, cy - r - 1, cx, cy, lw, c, a);
}
static void icon_gear64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s / 4;
    p2ui_ring64(cx, cy, r, lw, c, a);
    for (int i = 0; i < 8; i++) {
        const int ang = i * 45;                                  // 8 个齿：整数三角，无浮点
        const int ox = p2_cos64(ang), oy = p2_sin64(ang);
        p2ui_line64(cx + ox * r / 1024, cy + oy * r / 1024,
                    cx + ox * (s / 2) / 1024, cy + oy * (s / 2) / 1024, lw, c, a);
    }
}
static void icon_lock64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int bw = s * 2 / 3, bh = s * 2 / 5;
    p2ui_stroke_mixed64(cx - bw / 2, cy, bw, bh, 2, 2, c, a);
    p2ui_arc64(cx, cy, s / 4, 180, 360, lw, c, a);
    p2ui_fill_circle64(cx, cy + bh / 2, 1 + lw / 2, c, a);
}
static void icon_reboot64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    const int r = s * 2 / 5;
    p2ui_arc64(cx, cy, r, 300, 200, lw, c, a);
    p2ui_line64(cx + r * 3 / 4, cy - r * 3 / 4, cx + r * 9 / 10, cy - r / 3, lw, c, a);
    p2ui_line64(cx + r * 3 / 4, cy - r * 3 / 4, cx + r / 3, cy - r * 9 / 10, lw, c, a);
}
static void icon_chev64(int cx, int cy, int s, uint32_t c, int a, int lw, bool right) {
    const int w = s / 4, h = s / 3;
    if (right) {
        p2ui_line64(cx - w / 2, cy - h, cx + w / 2, cy, lw, c, a);
        p2ui_line64(cx + w / 2, cy, cx - w / 2, cy + h, lw, c, a);
    } else {
        p2ui_line64(cx + w / 2, cy - h, cx - w / 2, cy, lw, c, a);
        p2ui_line64(cx - w / 2, cy, cx + w / 2, cy + h, lw, c, a);
    }
}
static void icon_person64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    p2ui_ring64(cx, cy - s / 5, s / 5, lw, c, a);
    p2ui_arc64(cx, cy + s / 2, s / 2 - 1, 200, 340, lw, c, a);
}
static void icon_usb64(int x, int y, int s, uint32_t c, int a, int lw) {
    const int cy = y + s / 2;
    p2ui_stroke_mixed64(x + s / 4, cy - s / 4, s / 2, s / 2, 2, 2, c, a);
    p2ui_line64(x + s / 4 + s / 4, cy - s / 4, x + s / 4 + s / 4, cy - s / 3, lw, c, a);
    p2ui_fill_circle64(x + s / 4 + s / 4, cy - s / 3 - 1, lw, c, a);
}
static void icon_monitor64(int x, int y, int s, uint32_t c, int a, int lw) {
    const int w = s - 2, h = s * 2 / 3;
    p2ui_stroke_mixed64(x + 1, y, w, h, 2, 2, c, a);
    p2ui_line64(x + s / 2, y + h, x + s / 2, y + h + s / 8, lw, c, a);
    p2ui_line64(x + s / 4, y + h + s / 8, x + s * 3 / 4, y + h + s / 8, lw, c, a);
}
static void icon_check64(int cx, int cy, int s, uint32_t c, int a, int lw) {
    p2ui_line64(cx - s / 3, cy, cx - s / 10, cy + s / 4, lw, c, a);
    p2ui_line64(cx - s / 10, cy + s / 4, cx + s / 3, cy - s / 4, lw, c, a);
}
void p2ui_icon64(int kind, int x, int y, int size, uint32_t c, int a) {
    const int lw = THEME64_SM_ICON_LINE;
    const int cx = x + size / 2, cy = y + size / 2;
    switch (kind) {
        case P2UI_ICON_NET_GLOBE:  icon_globe64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_NET_ETHER:  icon_ethernet64(x, y, size, c, a, lw); break;
        case P2UI_ICON_NET_WIFI:   icon_wifi64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_SOUND:      icon_sound64(x, y, size, c, a, lw, false); break;
        case P2UI_ICON_SOUND_MUTE: icon_sound64(x, y, size, c, a, lw, true); break;
        case P2UI_ICON_BELL:       icon_bell64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_SEARCH:     icon_search64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_CLOSE:      icon_close64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_POWER:      icon_power64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_GEAR:       icon_gear64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_LOCK:       icon_lock64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_REBOOT:     icon_reboot64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_SHUTDOWN:   icon_power64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_CHEV_L:     icon_chev64(cx, cy, size, c, a, lw, false); break;
        case P2UI_ICON_CHEV_R:     icon_chev64(cx, cy, size, c, a, lw, true); break;
        case P2UI_ICON_PLUS:       p2ui_line64(cx - size / 4, cy, cx + size / 4, cy, lw, c, a);
                                   p2ui_line64(cx, cy - size / 4, cx, cy + size / 4, lw, c, a); break;
        case P2UI_ICON_PERSON:     icon_person64(cx, cy, size, c, a, lw); break;
        case P2UI_ICON_USB:        icon_usb64(x, y, size, c, a, lw); break;
        case P2UI_ICON_MONITOR:    icon_monitor64(x, y, size, c, a, lw); break;
        case P2UI_ICON_CHECK:      icon_check64(cx, cy, size, c, a, lw); break;
        default: break;
    }
}

// ==================== 应用表（搜索 + 固定网格）====================
struct SmApp64 { int app_id; int icon_kind; const char* zh; const char* en; const char* alias; };
static const SmApp64 kApps[] = {
    { APP_ID_TERM,     P2UI_ICON_MONITOR, "终端",       "Terminal",      "term shell console" },
    { APP_ID_MYPC,     P2UI_ICON_MONITOR, "我的电脑",   "My Computer",   "mypc computer explorer files" },
    { APP_ID_SETTINGS, P2UI_ICON_GEAR,    "设置",       "Settings",      "settings cfg control" },
    { APP_ID_CALC,     P2UI_ICON_PLUS,    "计算器",     "Calculator",    "calc calculator" },
    { APP_ID_MINES,    P2UI_ICON_PLUS,    "扫雷",       "Minesweeper",   "mines mine game" },
    { APP_ID_TMGR,     P2UI_ICON_MONITOR, "任务管理器", "Task Manager",  "tmgr taskmgr process" },
    { APP_ID_MONITOR,  P2UI_ICON_MONITOR, "系统监视器", "System Monitor","monitor perf" },
    { APP_ID_ABOUT,    P2UI_ICON_PERSON,  "关于",       "About VimtuOS", "about version" },
    { APP_ID_RECYCLE,  P2UI_ICON_CLOSE,   "回收站",     "Recycle Bin",   "recycle trash bin" },
};
#define SM_APP_N ((int)(sizeof(kApps) / sizeof(kApps[0])))
// 默认固定网格 2 列 × 4 行（8 个）
static const int kPinned[THEME64_SM_TILE_COLS * THEME64_SM_TILE_ROWS] = {
    0, 1, 2, 3, 4, 5, 6, 7
};
#define SM_PINNED_N 8

// ==================== 状态 ====================
static bool g_open = false;
static bool g_pm_open = false;               // 电源二级菜单
static int  g_pm_anim = -1;                  // 正在按下的电源菜单行（动态效果）
static uint32_t g_pm_anim_t0 = 0;
static int  g_pm_anim_frame = 0;
static char g_query[32] = {0};
static int  g_hits = 0;
static int  g_hit_app[8];
static int  g_hit_sel = 0;
static int  g_hover = -1;                    // 当前悬停项（-1 = 无；用于高亮与验收打点）
static uint32_t g_opened_t0 = 0;
static int  g_geom_logged = false;
static int  g_grid_logged = false;
static int  g_hover_budget = 60;
static int  g_click_budget = 120;
// 时间/日期（每秒刷新；跟随 RTC）
static int  g_hh = 0, g_mm = 0, g_ss = 0, g_yy = 0, g_mo = 0, g_dd = 0, g_wd = 0;
static char g_time_txt[8] = "--:--";
static char g_date_txt[48] = "";
static char g_date_only[24] = "";
static uint32_t g_last_sec = 0xFFFFFFFFu;
// 网络图标 kind（变化时打点）
static int  g_net_kind = -1;
static int  g_user_logged = false;

// ==================== 几何 ====================
static int sm_w() { return THEME64_SM_W; }
static int sm_h() { return THEME64_SM_H; }
static int sm_x() { return (fb_width() - sm_w()) / 2; }        // 屏幕水平居中
static int dock_top64() { int m = 0, h = 0; (void)m; (void)h; return fb_height() - THEME64_DOCK_MARGIN - THEME64_DOCK_H; }
static int sm_y() { return dock_top64() - THEME64_SM_GAP_DOCK - sm_h(); }   // 底边 = Dock 顶边上方 11px

static int status_y64() { return sm_y() + THEME64_SM_PAD; }
static int status_x64(int which) {
    const int total = THEME64_SM_ICON * 4 + THEME64_SM_ICON_GAP * 3;
    const int x0 = sm_x() + sm_w() - THEME64_SM_PAD - total;
    return x0 + which * (THEME64_SM_ICON + THEME64_SM_ICON_GAP);
}
static int search_x64() { return sm_x() + sm_w() / 2 - 10; }
static int search_y64() { return status_y64() + 2 * p2ui_line_h64() + 8; }
static int search_w64() { return sm_x() + sm_w() - THEME64_SM_PAD - search_x64(); }
static int content_top64() { return search_y64() + THEME64_SM_SEARCH_H + 12; }
static int user_row_y64() { return sm_y() + sm_h() - THEME64_SM_PAD - THEME64_SM_AVATAR; }
static int power_x64() { return sm_x() + sm_w() - THEME64_SM_PAD - THEME64_SM_POWER; }
static int power_y64() { return sm_y() + sm_h() - THEME64_SM_PAD - THEME64_SM_POWER; }
static int settings_x64() { return power_x64() - 12 - 96; }
static int settings_y64() { return power_y64() + THEME64_SM_POWER / 2 - 17; }
static int grid_x64() {
    const int gw = THEME64_SM_TILE_W * THEME64_SM_TILE_COLS;      // 偏右：右对齐到内边距
    return sm_x() + sm_w() - THEME64_SM_PAD - gw;
}
static int grid_y64() { return content_top64(); }
static int tile_x64(int idx) { return grid_x64() + (idx % THEME64_SM_TILE_COLS) * THEME64_SM_TILE_W; }
static int tile_y64(int idx) { return grid_y64() + (idx / THEME64_SM_TILE_COLS) * THEME64_SM_TILE_H; }
static int result_x64() { return sm_x() + THEME64_SM_PAD; }
static int result_y64() { return content_top64(); }
static int result_w64() { return sm_w() - THEME64_SM_PAD * 2; }
static int pm_x64() { return sm_x() + sm_w() - THEME64_SM_PAD - THEME64_PM_W; }
static int pm_h64() { return THEME64_PM_ROW * 3 + 8 * 2; }
static int pm_y64() { return power_y64() - 8 - pm_h64(); }

void startmenu64_geom64(int* x, int* y, int* w, int* h) {
    if (x) *x = sm_x();
    if (y) *y = sm_y();
    if (w) *w = sm_w();
    if (h) *h = sm_h();
}
int startmenu64_time_rect64(int* x, int* y, int* w, int* h) {
    if (x) *x = sm_x() + THEME64_SM_PAD;
    if (y) *y = status_y64();
    if (w) *w = 190;
    if (h) *h = 2 * p2ui_line_h64();
    return 1;
}
int startmenu64_search_rect64(int* x, int* y, int* w, int* h) {
    if (x) *x = search_x64();
    if (y) *y = search_y64();
    if (w) *w = search_w64();
    if (h) *h = THEME64_SM_SEARCH_H;
    return 1;
}
int startmenu64_status_rect64(int which, int* x, int* y, int* w, int* h) {
    if (which < 0 || which > 3) return 0;
    if (x) *x = status_x64(which);
    if (y) *y = status_y64();
    if (w) *w = THEME64_SM_ICON;
    if (h) *h = THEME64_SM_ICON;
    return 1;
}
int startmenu64_user_rect64(int* x, int* y, int* w, int* h) {
    if (x) *x = sm_x() + THEME64_SM_PAD;
    if (y) *y = user_row_y64();
    if (w) *w = THEME64_SM_AVATAR + 8 + 120;
    if (h) *h = THEME64_SM_AVATAR;
    return 1;
}
int startmenu64_power_rect64(int* x, int* y, int* w, int* h) {
    if (x) *x = power_x64();
    if (y) *y = power_y64();
    if (w) *w = THEME64_SM_POWER;
    if (h) *h = THEME64_SM_POWER;
    return 1;
}
int startmenu64_settings_rect64(int* x, int* y, int* w, int* h) {
    if (x) *x = settings_x64();
    if (y) *y = settings_y64();
    if (w) *w = 96;
    if (h) *h = 34;
    return 1;
}
int startmenu64_tile_rect64(int idx, int* x, int* y, int* w, int* h) {
    if (idx < 0 || idx >= SM_PINNED_N) return 0;
    if (x) *x = tile_x64(idx);
    if (y) *y = tile_y64(idx);
    if (w) *w = THEME64_SM_TILE_W - 8;
    if (h) *h = THEME64_SM_TILE_H - 6;
    return 1;
}
int startmenu64_result_rect64(int idx, int* x, int* y, int* w, int* h) {
    if (idx < 0 || idx >= g_hits) return 0;
    if (x) *x = result_x64();
    if (y) *y = result_y64() + idx * THEME64_SM_SROW;
    if (w) *w = result_w64();
    if (h) *h = THEME64_SM_SROW - 4;
    return 1;
}
int startmenu64_pm_panel64(int* x, int* y, int* w, int* h) {
    if (x) *x = pm_x64();
    if (y) *y = pm_y64();
    if (w) *w = THEME64_PM_W;
    if (h) *h = pm_h64();
    return 1;
}
int startmenu64_pm_rect64(int row, int* x, int* y, int* w, int* h) {
    if (row < 0 || row > 2) return 0;
    if (x) *x = pm_x64() + 8;
    if (y) *y = pm_y64() + 8 + row * THEME64_PM_ROW;
    if (w) *w = THEME64_PM_W - 16;
    if (h) *h = THEME64_PM_ROW - 4;
    return 1;
}
int startmenu64_status_icon_kind64(int which) {
    if (which == 0) return g_net_kind < 0 ? P2UI_ICON_NET_GLOBE : g_net_kind;
    if (which == 1) return panels64_volume64() <= 0 ? P2UI_ICON_SOUND_MUTE : P2UI_ICON_SOUND;
    if (which == 3) return P2UI_ICON_BELL;
    return P2UI_ICON_PERSON;
}
int startmenu64_hits64() { return g_hits; }
bool startmenu64_is_open64() { return g_open; }

// ==================== 时间/状态刷新 ====================
static void time_refresh64(bool force) {
    int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, rwd = 0;
    rtc_get_time64(&hh, &mm, &ss);
    rtc_get_date64(&yy, &mo, &dd, &rwd);
    g_hh = hh; g_mm = mm; g_ss = ss; g_yy = yy; g_mo = mo; g_dd = dd;
    int wd = weekday_ymd64(yy, mo, dd);
    if (wd < 0) wd = ((rwd % 7) + 7) % 7;
    g_wd = wd;
    if (!force && g_time_txt[0] && g_ss == (int)(g_last_sec & 0x7F)) return;
    g_last_sec = (uint32_t)ss;
    char b[8];
    u_dec2(b, hh); g_time_txt[0] = b[0]; g_time_txt[1] = b[1]; g_time_txt[2] = ':';
    u_dec2(b, mm); g_time_txt[3] = b[0]; g_time_txt[4] = b[1]; g_time_txt[5] = 0;
    char d[48]; d[0] = 0;
    u_dec4(b, yy); s_cat(d, b, (int)sizeof(d));
    s_cat(d, "年", (int)sizeof(d));
    u_dec(b, (uint64_t)mo); s_cat(d, b, (int)sizeof(d));
    s_cat(d, "月", (int)sizeof(d));
    u_dec(b, (uint64_t)dd); s_cat(d, b, (int)sizeof(d));
    s_cat(d, "日", (int)sizeof(d));
    s_copy(g_date_only, d, (int)sizeof(g_date_only));
    static const char* wdzh[7] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
    s_cat(d, " ", (int)sizeof(d));
    s_cat(d, wdzh[wd < 0 ? 0 : (wd > 6 ? 0 : wd)], (int)sizeof(d));
    s_copy(g_date_txt, d, (int)sizeof(g_date_txt));
}
static void net_state_refresh64() {
    const int wired = e1000_link64() == 1 ? 1 : 0;
    const int wireless = 0;                     // 本批**没有无线网卡驱动**（如实）
    const int kind = wired ? P2UI_ICON_NET_ETHER : (wireless ? P2UI_ICON_NET_WIFI : P2UI_ICON_NET_GLOBE);
    if (kind == g_net_kind) return;
    g_net_kind = kind;
    dbg64_line_begin64();
    dbg64_str("[START64] status icon net x=");
    dbg64_dec((uint64_t)status_x64(0));
    dbg64_str(" y=");
    dbg64_dec((uint64_t)status_y64());
    dbg64_str(" kind=");
    dbg64_str(kind == P2UI_ICON_NET_ETHER ? "ethernet" :
              kind == P2UI_ICON_NET_WIFI ? "wifi" : "globe");
    dbg64_str(" wired=");
    dbg64_dec((uint64_t)wired);
    dbg64_str(" wireless=");
    dbg64_dec((uint64_t)wireless);
    dbg64_str(" (wired+wifi 同时存在时显示以太网)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 初始化 ====================
static void log_init64() {
    dbg64_line_begin64();
    dbg64_str("[START64] init screen=");
    dbg64_dec((uint64_t)fb_width());
    dbg64_str("x");
    dbg64_dec((uint64_t)fb_height());
    dbg64_str(" menu=");
    dbg64_dec((uint64_t)THEME64_SM_W);
    dbg64_str("x");
    dbg64_dec((uint64_t)THEME64_SM_H);
    dbg64_str(" r_top=");
    dbg64_dec((uint64_t)THEME64_SM_R_TOP);
    dbg64_str(" r_bottom=");
    dbg64_dec((uint64_t)THEME64_SM_R_BOTTOM);
    dbg64_str(" gap_dock=");
    dbg64_dec((uint64_t)THEME64_SM_GAP_DOCK);
    dbg64_str(" icons=line width=");
    dbg64_dec((uint64_t)THEME64_SM_ICON_LINE);
    dbg64_str(" anchors=menu search=apps");
    dbg64_nl();
    dbg64_line_end64();
}

// 状态区图标打点：声音/中英/通知（含未读角标）——只在状态变化时各打一行（防刷屏）
static int g_snd_logged = -1, g_lang_logged = -1, g_unread_logged = -1;
static void status_log64(int force) {
    const int vol = panels64_volume64();
    if (force || vol != g_snd_logged) {
        if (!force && g_log_budget > 0) g_log_budget--;
        g_snd_logged = vol;
        dbg64_line_begin64();
        dbg64_str("[START64] status icon sound x=");
        dbg64_dec((uint64_t)status_x64(1));
        dbg64_str(" y=");
        dbg64_dec((uint64_t)status_y64());
        dbg64_str(" volume=");
        dbg64_dec((uint64_t)vol);
        dbg64_str(" icon=");
        dbg64_str(vol <= 0 ? "mute" : "speaker");
        dbg64_str(" (memory-state only: no audio driver yet)");
        dbg64_nl();
        dbg64_line_end64();
    }
    const int lang = kbd_lang64();
    if (force || lang != g_lang_logged) {
        g_lang_logged = lang;
        dbg64_line_begin64();
        dbg64_str("[START64] status icon lang x=");
        dbg64_dec((uint64_t)status_x64(2));
        dbg64_str(" y=");
        dbg64_dec((uint64_t)status_y64());
        dbg64_str(" text=");
        dbg64_str(lang ? "中" : "英");
        dbg64_str(" ime=");
        dbg64_dec((uint64_t)kbd_ime_available64());
        dbg64_str(" shift_toggles=");
        dbg64_dec((uint64_t)kbd_shift_toggles64());
        dbg64_str(" (no Chinese IME -> only 英)");
        dbg64_nl();
        dbg64_line_end64();
    }
    const int unread = panels64_notif_unread64();
    if (force || unread != g_unread_logged) {
        g_unread_logged = unread;
        dbg64_line_begin64();
        dbg64_str("[START64] status icon notif x=");
        dbg64_dec((uint64_t)status_x64(3));
        dbg64_str(" y=");
        dbg64_dec((uint64_t)status_y64());
        dbg64_str(" unread=");
        dbg64_dec((uint64_t)unread);
        dbg64_str(" badge=");
        dbg64_str(unread > 0 ? (unread <= 9 ? "digit" : "dot") : "none");
        if (unread > 0) {
            dbg64_str(" badge_color=#");
            static const char* H = "0123456789ABCDEF";
            const char hx[7] = {
                H[(THEME64_RED_BADGE >> 20) & 0xF], H[(THEME64_RED_BADGE >> 16) & 0xF],
                H[(THEME64_RED_BADGE >> 12) & 0xF], H[(THEME64_RED_BADGE >> 8) & 0xF],
                H[(THEME64_RED_BADGE >> 4) & 0xF], H[THEME64_RED_BADGE & 0xF], 0
            };
            dbg64_str(hx);
            dbg64_str(" badge_pos=x");
            dbg64_dec((uint64_t)(status_x64(3) + THEME64_SM_ICON - THEME64_SM_BADGE / 2 - 2));
            dbg64_str(",y");
            dbg64_dec((uint64_t)(status_y64() - 3));
            dbg64_str(" d=");
            dbg64_dec((uint64_t)THEME64_SM_BADGE);
        }
        dbg64_nl();
        dbg64_line_end64();
    }
}
void startmenu64_init64() {
    g_open = false;
    g_pm_open = false;
    g_query[0] = 0;
    g_hits = 0;
    g_hit_sel = 0;
    g_geom_logged = false;
    g_grid_logged = false;
    g_open = false;
    time_refresh64(true);
    net_state_refresh64();
    log_init64();
    const int st = startmenu64_selftest64();
    if (st != 0) lg("selftest FAIL mask=", nullptr, (int64_t)st, " (geometry/grid/search)");
    else {
        dbg64_line_begin64();
        dbg64_str("[START64] selftest PASS mask=0");
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== 搜索 ====================
static void search_update64(const char* why) {
    g_hits = 0;
    for (int i = 0; i < SM_APP_N && g_hits < 8; i++) {
        const SmApp64& a = kApps[i];
        if (s_has_icase(a.zh, g_query) || s_has_icase(a.en, g_query) || s_has_icase(a.alias, g_query))
            g_hit_app[g_hits++] = i;
    }
    if (g_hit_sel >= g_hits) g_hit_sel = g_hits ? g_hits - 1 : 0;
    if (g_query[0]) {
        dbg64_line_begin64();
        dbg64_str("[START64] search q=");
        dbg64_str(g_query);
        dbg64_str(" hits=");
        dbg64_dec((uint64_t)g_hits);
        dbg64_str(" first=");
        dbg64_str(g_hits ? kApps[g_hit_app[0]].en : "-");
        dbg64_str(" (app=");
        if (g_hits) dbg64_dec((uint64_t)kApps[g_hit_app[0]].app_id);
        else dbg64_str("-");
        dbg64_str(") why=");
        dbg64_str(why);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== 启动应用（复用既有 app 启动入口）====================
void startmenu64_launch64(int app_id, const char* via) {
    dbg64_line_begin64();
    dbg64_str("[START64] launch app=");
    dbg64_dec((uint64_t)app_id);
    dbg64_str(" via=");
    dbg64_str(via);
    dbg64_nl();
    dbg64_line_end64();
    switch (app_id) {
        case APP_ID_TERM:     app_term_open64();     break;
        case APP_ID_MYPC:     app_mypc_open64();     break;
        case APP_ID_SETTINGS: app_settings_open64(); break;
        case APP_ID_CALC:     app_calc_open64();     break;
        case APP_ID_MINES:    app_mines_open64();    break;
        case APP_ID_TMGR:     app_tmgr_open64();     break;
        case APP_ID_MONITOR:  app_monitor_open64();  break;
        case APP_ID_ABOUT:    app_about_open64();    break;
        case APP_ID_RECYCLE:  app_recycle_open64();  break;
        default: break;
    }
}

// ==================== 开关 ====================
static void dirty_menu64(int pad) {
    gui64_dirty(sm_x() - pad, sm_y() - pad, sm_w() + pad * 2,
                sm_h() + THEME64_SM_GAP_DOCK + pad * 2);
}
static void dirty_pm64() {
    gui64_dirty(pm_x64() - 12, pm_y64() - 12, THEME64_PM_W + 24, pm_h64() + 24);
}
void startmenu64_open64(const char* why) {
    if (g_open) return;
    g_open = true;
    g_query[0] = 0;
    g_hits = 0;
    g_hit_sel = 0;
    g_pm_open = false;
    g_opened_t0 = ticks64();
    time_refresh64(true);
    net_state_refresh64();
    status_log64(false);          // 首次打开时把 声音/中英/未读角标 的状态与几何写进日志
    dirty_menu64(16);
    dbg64_line_begin64();
    dbg64_str("[START64] open why=");
    dbg64_str(why);
    dbg64_str(" x=");
    dbg64_dec((uint64_t)sm_x());
    dbg64_str(" y=");
    dbg64_dec((uint64_t)sm_y());
    dbg64_str(" w=");
    dbg64_dec((uint64_t)sm_w());
    dbg64_str(" h=");
    dbg64_dec((uint64_t)sm_h());
    dbg64_nl();
    dbg64_line_end64();
    if (!g_geom_logged) {
        g_geom_logged = true;
        const int bottom = sm_y() + sm_h();
        dbg64_line_begin64();
        dbg64_str("[START64] geom x=");
        dbg64_dec((uint64_t)sm_x());
        dbg64_str(" y=");
        dbg64_dec((uint64_t)sm_y());
        dbg64_str(" w=");
        dbg64_dec((uint64_t)sm_w());
        dbg64_str(" h=");
        dbg64_dec((uint64_t)sm_h());
        dbg64_str(" bottom=");
        dbg64_dec((uint64_t)bottom);
        dbg64_str(" dock_top=");
        dbg64_dec((uint64_t)dock_top64());
        dbg64_str(" gap=");
        dbg64_dec((uint64_t)(dock_top64() - bottom));
        dbg64_str(" center=1 screen=");
        dbg64_dec((uint64_t)fb_width());
        dbg64_str("x");
        dbg64_dec((uint64_t)fb_height());
        dbg64_str(" r_top=");
        dbg64_dec((uint64_t)THEME64_SM_R_TOP);
        dbg64_str(" r_bottom=");
        dbg64_dec((uint64_t)THEME64_SM_R_BOTTOM);
        dbg64_nl();
        dbg64_line_end64();
    }
    if (!g_grid_logged) {
        g_grid_logged = true;
        dbg64_line_begin64();
        dbg64_str("[START64] grid cols=");
        dbg64_dec((uint64_t)THEME64_SM_TILE_COLS);
        dbg64_str(" rows=");
        dbg64_dec((uint64_t)THEME64_SM_TILE_ROWS);
        dbg64_str(" n=");
        dbg64_dec((uint64_t)SM_PINNED_N);
        dbg64_str(" tile=");
        dbg64_dec((uint64_t)THEME64_SM_TILE_W);
        dbg64_str("x");
        dbg64_dec((uint64_t)THEME64_SM_TILE_H);
        dbg64_str(" x=");
        dbg64_dec((uint64_t)grid_x64());
        dbg64_str(" y=");
        dbg64_dec((uint64_t)grid_y64());
        dbg64_nl();
        dbg64_line_end64();
        for (int i = 0; i < SM_PINNED_N; i++) {
            int x = 0, y = 0, w = 0, h = 0;
            startmenu64_tile_rect64(i, &x, &y, &w, &h);
            const SmApp64& a = kApps[kPinned[i]];
            dbg64_line_begin64();
            dbg64_str("[START64] tile idx=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" app=");
            dbg64_dec((uint64_t)a.app_id);
            dbg64_str(" name=");
            dbg64_str(a.zh);          // 中文名没有空格：日志 token 好切（英文名带空格留给 en= 字段）
            dbg64_str(" en=");
            dbg64_str(a.en);
            dbg64_str(" x=");
            dbg64_dec((uint64_t)x);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)y);
            dbg64_str(" w=");
            dbg64_dec((uint64_t)w);
            dbg64_str(" h=");
            dbg64_dec((uint64_t)h);
            dbg64_nl();
            dbg64_line_end64();
        }
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_search_rect64(&x, &y, &w, &h);
        dbg64_line_begin64();
        dbg64_str("[START64] search box x=");
        dbg64_dec((uint64_t)x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)y);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)w);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)h);
        dbg64_str(" r=");
        dbg64_dec((uint64_t)THEME64_SM_SEARCH_R);
        dbg64_str(" acrylic=accent alpha=");
        dbg64_dec((uint64_t)THEME64_SM_A_SEARCH);
        dbg64_str(" icon=search text=搜索");
        dbg64_nl();
        dbg64_line_end64();
        startmenu64_time_rect64(&x, &y, &w, &h);
        startmenu64_user_rect64(&x, &y, &w, &h);
        const User64Entry* u = userdb64_gui_user64();
        if (!u) u = userdb64_session_user64();
        dbg64_line_begin64();
        dbg64_str("[START64] user avatar x=");
        dbg64_dec((uint64_t)(sm_x() + THEME64_SM_PAD));
        dbg64_str(" y=");
        dbg64_dec((uint64_t)user_row_y64());
        dbg64_str(" d=");
        dbg64_dec((uint64_t)THEME64_SM_AVATAR);
        dbg64_str(" name=");
        dbg64_str(u ? u->name : "-");
        dbg64_str(" src=");
        dbg64_str(u && u->avatar[0] ? "userdb" : "builtin:procedural");
        dbg64_nl();
        dbg64_line_end64();
        startmenu64_power_rect64(&x, &y, &w, &h);
        dbg64_line_begin64();
        dbg64_str("[START64] power btn x=");
        dbg64_dec((uint64_t)power_x64());
        dbg64_str(" y=");
        dbg64_dec((uint64_t)power_y64());
        dbg64_str(" d=");
        dbg64_dec((uint64_t)THEME64_SM_POWER);
        dbg64_str(" settings x=");
        dbg64_dec((uint64_t)settings_x64());
        dbg64_str(" y=");
        dbg64_dec((uint64_t)settings_y64());
        dbg64_str(" shadow=2 acrylic=1");
        dbg64_nl();
        dbg64_line_end64();
        g_user_logged = true;
    }
}
void startmenu64_close64(const char* why) {
    if (!g_open) return;
    g_open = false;
    const bool had_pm = g_pm_open;
    g_pm_open = false;
    dirty_menu64(16);
    if (had_pm) dirty_pm64();
    (void)g_user_logged;
    dbg64_line_begin64();
    dbg64_str("[START64] close why=");
    dbg64_str(why);
    dbg64_nl();
    dbg64_line_end64();
}
void startmenu64_toggle64() {
    if (g_open) startmenu64_close64("toggle");
    else startmenu64_open64("start-button");
}

// ==================== 绘制 ====================
static void draw_avatar64(int x, int y, int d) {
    const Theme64Tokens* t = theme64_tokens64();
    const int r = d / 2;
    const int cx = x + r, cy = y + r;
    p2ui_fill_circle64(cx, cy, r, t->grad_a, 255);      // 头像不投影（需求没要求，省一个阴影 mask 键）
    // 上亮下暗的柔光（两段渐变圆）
    p2ui_fill_circle64(cx, cy - 2, r - 2, t->grad_b, 90);
    p2ui_ring64(cx, cy, r, 1, 0xFFFFFFu, THEME64_A_EDGE);
    const User64Entry* u = userdb64_gui_user64();
    if (!u) u = userdb64_session_user64();
    if (u && u->name[0]) {
        char one[8];
        int n = 0;
        const unsigned char ch = (unsigned char)u->name[0];
        int bytes = 1;
        if (ch >= 0xF0) bytes = 4; else if (ch >= 0xE0) bytes = 3; else if (ch >= 0xC0) bytes = 2;
        for (int i = 0; i < bytes && u->name[i] && n < 7; i++) one[n++] = u->name[i];
        one[n] = 0;
        const int tw = p2ui_text_w64(one);
        const int lh = p2ui_line_h64();
        p2ui_text64(cx - tw / 2, cy - lh / 2, one, rgb(255, 255, 255));
    }
}

static void draw_status64() {
    const Theme64Tokens* t = theme64_tokens64();
    const uint32_t c = p2ui_icon_color64(t);
    const int y = status_y64();
    const int s = THEME64_SM_ICON;
    time_refresh64(false);
    net_state_refresh64();
    // 网络：有线 = 以太网；无线 = Wi-Fi；都没有 = 网状球体（跟随主题线色）
    p2ui_icon64(startmenu64_status_icon_kind64(0), status_x64(0), y, s, c, 255);
    // 声音：音量 0 = 静音图标
    const int vol = panels64_volume64();
    p2ui_icon64(vol <= 0 ? P2UI_ICON_SOUND_MUTE : P2UI_ICON_SOUND, status_x64(1), y, s, c, 255);
    // 中/英（无中文输入法：只显示"英"）
    {
        const char* lang = kbd_lang64() ? "中" : "英";
        const int x = status_x64(2), tw = p2ui_text_w64(lang);
        const int lh = p2ui_line_h64();
        p2ui_text64(x + (s - tw) / 2, y + (s - lh) / 2, lang, c);
    }
    // 消息通知 + 未读红点/数字角标
    const int ni = 3;
    p2ui_icon64(P2UI_ICON_BELL, status_x64(ni), y, s, c, 255);
    const int unread = panels64_notif_unread64();
    if (unread > 0) {
        const int bx = status_x64(ni) + s - THEME64_SM_BADGE / 2 - 2;
        const int by = y - 3;
        p2ui_fill_circle64(bx + THEME64_SM_BADGE / 2, by + THEME64_SM_BADGE / 2,
                           THEME64_SM_BADGE / 2, THEME64_RED_BADGE, 255);
        if (unread <= 9) {
            char b[4];
            u_dec(b, (uint64_t)unread);
            const int tw = p2ui_text_w64(b), lh = p2ui_line_h64();
            p2ui_text64(bx + THEME64_SM_BADGE / 2 - tw / 2,
                        by + THEME64_SM_BADGE / 2 - lh / 2, b, rgb(255, 255, 255));
        }
    }
}

static void draw_tile64(int idx, bool hover) {
    const Theme64Tokens* t = theme64_tokens64();
    int x = 0, y = 0, w = 0, h = 0;
    startmenu64_tile_rect64(idx, &x, &y, &w, &h);
    if (hover)
        p2ui_fill_mixed64(x, y, w, h, THEME64_R_BUTTON, THEME64_R_BUTTON, t->accent, 46);
    const SmApp64& a = kApps[kPinned[idx]];
    const int isz = THEME64_SM_TILE_ICON;
    const int ix = x + (w - isz) / 2, iy = y + 6;
    // 图标：圆角方块渐变底 + 线性图标（统一线性风格）
    p2ui_fill_mixed64(ix, iy, isz, isz, THEME64_R_ICON, THEME64_R_ICON, t->grad_a, 235);
    p2ui_icon64(a.icon_kind, ix + 4, iy + 4, isz - 8, rgb(255, 255, 255), 235);
    const char* nm = p2ui_text_w64(a.zh) <= w - 6 ? a.zh : a.en;
    const int tw = p2ui_text_w64(nm), lh = p2ui_line_h64();
    p2ui_text64(x + (w - tw) / 2, iy + isz + 4, nm, t->dark ? t->text : rgb(32, 32, 32));
    (void)lh;
}

static void draw_menu64() {
    const Theme64Tokens* t = theme64_tokens64();
    const int x = sm_x(), y = sm_y(), w = sm_w(), h = sm_h();
    // 打开动效：fade + translateY（减少动画时 1 帧到位）
    int dy = 0, al = 255;
    {
        const int dur = (int)theme64_dur64(THEME64_MS_NORMAL);
        if (dur > 0) {
            const int el = (int)((ticks64() - g_opened_t0) * 250 / (uint32_t)dur);
            const int e = theme64_ease64(el > 256 ? 256 : el);
            dy = THEME64_CAL_DY0 - THEME64_CAL_DY0 * e / 256;
            al = 255 * (140 + 115 * e / 256) / 255;
        }
    }
    const int my = y + dy;
    // 1) 菜单主体：亚克力 + 双层浅阴影 + 1px 高光边（上圆角 24 / 下圆角 10）
    const int alpha = t->dark ? THEME64_SM_A_MENU_DARK : THEME64_SM_A_MENU;
    p2ui_popup64(x, my, w, h, THEME64_SM_R_TOP, THEME64_SM_R_BOTTOM, t, t->dock_bg, alpha * al / 255);
    static int shape_logged = 0;      // 混合圆角（上 24 / 下 10）走的就是这条渲染路径（一次打点）
    if (!shape_logged) {
        shape_logged = 1;
        dbg64_line_begin64();
        dbg64_str("[START64] shape mixed_corners=1 r_top=");
        dbg64_dec((uint64_t)THEME64_SM_R_TOP);
        dbg64_str(" r_bottom=");
        dbg64_dec((uint64_t)THEME64_SM_R_BOTTOM);
        dbg64_str(" corners=TL,TR:r_top BL,BR:r_bottom acrylic=1 shadow=2 edge=1px w=");
        dbg64_dec((uint64_t)w);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)h);
        dbg64_nl();
        dbg64_line_end64();
    }
    // 2) 左上角：时间（上）+ 年月日（下）
    time_refresh64(false);
    {
        const uint32_t tc = t->dark ? t->text : rgb(24, 26, 32);
        p2ui_text64(x + THEME64_SM_PAD, my + THEME64_SM_PAD, g_time_txt, tc);
        p2ui_text64(x + THEME64_SM_PAD, my + THEME64_SM_PAD + p2ui_line_h64(), g_date_only, t->text_dim);
    }
    // 3) 状态区图标（线性、跟随主题）
    draw_status64();
    // 4) 搜索框（顶部偏右、长方形小圆角、跟随主题色亚克力、白色"搜索"）
    {
        const int sx = search_x64(), sy = my + (search_y64() - sm_y()), sw = search_w64();
        p2ui_fill_mixed64(sx, sy, sw, THEME64_SM_SEARCH_H, THEME64_SM_SEARCH_R, THEME64_SM_SEARCH_R,
                          t->accent, THEME64_SM_A_SEARCH);
        p2ui_stroke_mixed64(sx, sy, sw, THEME64_SM_SEARCH_H, THEME64_SM_SEARCH_R, THEME64_SM_SEARCH_R,
                            0xFFFFFFu, THEME64_A_EDGE);
        p2ui_icon64(P2UI_ICON_SEARCH, sx + 8, sy + (THEME64_SM_SEARCH_H - 16) / 2, 16, rgb(255, 255, 255), 255);
        const int lh = p2ui_line_h64();
        if (g_query[0]) {
            p2ui_text64(sx + 30, sy + (THEME64_SM_SEARCH_H - lh) / 2, g_query, rgb(255, 255, 255));
            // Caps 指示灯（Caps 语义接线：文本输入处能看到状态）
            if (kbd_caps_on())
                p2ui_text64(sx + sw - 34, sy + (THEME64_SM_SEARCH_H - lh) / 2, "A", rgb(255, 255, 255));
        } else {
            p2ui_text64(sx + 30, sy + (THEME64_SM_SEARCH_H - lh) / 2, "搜索", rgb(255, 255, 255));
        }
    }
    // 5) 内容：有输入时结果优先；无输入时固定网格（2 列 × 4 行，偏右）
    const int cy0 = my + (content_top64() - sm_y());
    if (g_query[0]) {
        if (g_hits == 0) {
            p2ui_text64(result_x64() + 6, cy0 + 10, "没有匹配的应用", t->text_dim);
        } else {
            for (int i = 0; i < g_hits && i < 6; i++) {
                int rx = 0, ry = 0, rw = 0, rh = 0;
                startmenu64_result_rect64(i, &rx, &ry, &rw, &rh);
                ry = my + (ry - sm_y());
                if (i == g_hit_sel)
                    p2ui_fill_mixed64(rx, ry, rw, rh, THEME64_R_BUTTON, THEME64_R_BUTTON, t->accent, 60);
                const SmApp64& a = kApps[g_hit_app[i]];
                p2ui_icon64(a.icon_kind, rx + 6, ry + (rh - 18) / 2, 18, p2ui_icon_color64(t), 255);
                const int lh = p2ui_line_h64();
                p2ui_text64(rx + 32, ry + (rh - lh) / 2, a.zh, t->dark ? t->text : rgb(24, 26, 32));
                const int ew = p2ui_text_w64(a.en);
                p2ui_text64(rx + rw - ew - 10, ry + (rh - lh) / 2, a.en, t->text_dim);
            }
        }
    } else {
        for (int i = 0; i < SM_PINNED_N; i++) draw_tile64(i, g_hover == 10 + i);
    }
    // 6) 左下角：小圆形用户头像 + 用户名
    {
        const User64Entry* u = userdb64_gui_user64();
        if (!u) u = userdb64_session_user64();
        const int ax = x + THEME64_SM_PAD, ay = my + (user_row_y64() - sm_y());
        draw_avatar64(ax, ay, THEME64_SM_AVATAR);
        const int lh = p2ui_line_h64();
        p2ui_text64(ax + THEME64_SM_AVATAR + 8, ay + (THEME64_SM_AVATAR - lh) / 2,
                    u ? u->name : "user", t->dark ? t->text : rgb(24, 26, 32));
    }
    // 7) 右下角：「设置」按钮 + 圆形带阴影的亚克力电源按钮
    {
        const int sx = settings_x64(), sy = my + (settings_y64() - sm_y());
        const bool hov = g_hover == 8;
        p2ui_fill_mixed64(sx, sy, 96, 34, THEME64_R_BUTTON, THEME64_R_BUTTON, t->dock_bg,
                          (t->dark ? 200 : 220) + (hov ? 20 : 0));
        p2ui_stroke_mixed64(sx, sy, 96, 34, THEME64_R_BUTTON, THEME64_R_BUTTON, 0xFFFFFFu, THEME64_A_EDGE);
        p2ui_icon64(P2UI_ICON_GEAR, sx + 8, sy + 8, 18, p2ui_icon_color64(t), 255);
        const int lh = p2ui_line_h64();
        p2ui_text64(sx + 32, sy + (34 - lh) / 2, "设置", p2ui_icon_color64(t));
        const int px = power_x64(), py = my + (power_y64() - sm_y());
        const int pcx = px + THEME64_SM_POWER / 2, pcy = py + THEME64_SM_POWER / 2;
        p2ui_shadow64(px, py, THEME64_SM_POWER, THEME64_SM_POWER, THEME64_SM_POWER / 2, t);
        p2ui_fill_circle64(pcx, pcy, THEME64_SM_POWER / 2,
                           t->dock_bg, t->dark ? 210 : 235);
        p2ui_ring64(pcx, pcy, THEME64_SM_POWER / 2, 1, 0xFFFFFFu, THEME64_A_EDGE);
        p2ui_icon64(P2UI_ICON_POWER, px + 9, py + 9, THEME64_SM_POWER - 18,
                    t->dark ? t->text : rgb(24, 26, 32), 255);
    }
    // 8) 电源二级菜单（**当前界面内**的竖长方形小圆角框）
    if (g_pm_open) {
        static const char* rows_zh[3] = {"关机", "重启", "锁定"};
        const int pxm = pm_x64(), pym = my + (pm_y64() - sm_y());
        const int ppm = pm_h64();
        // 菜单内的二级框不投外阴影（它在菜单亚克力之上，投影会多一个 mask 键）
        p2ui_fill_mixed64(pxm, pym, THEME64_PM_W, ppm, THEME64_PM_R, THEME64_PM_R, t->dock_bg,
                          t->dark ? 232 : 240);
        p2ui_stroke_mixed64(pxm, pym, THEME64_PM_W, ppm, THEME64_PM_R, THEME64_PM_R, 0xFFFFFFu,
                            THEME64_A_EDGE);
        for (int i = 0; i < 3; i++) {
            int bx = 0, by = 0, bw = 0, bh = 0;
            startmenu64_pm_rect64(i, &bx, &by, &bw, &bh);
            by = my + (by - sm_y());
            const bool hov = (g_hover == 30 + i);
            const bool anim = (g_pm_anim == i);
            const int inset = anim ? 2 : 0;                 // Win11 那种按下动态效果：轻微内缩 + 高亮
            p2ui_fill_mixed64(bx + inset, by + inset, bw - inset * 2, bh - inset * 2,
                              THEME64_PM_R, THEME64_PM_R, hov || anim ? t->accent : t->dock_bg,
                              hov || anim ? 60 : 90);
            const int isz = 18;
            p2ui_icon64(i == 0 ? P2UI_ICON_SHUTDOWN : i == 1 ? P2UI_ICON_REBOOT : P2UI_ICON_LOCK,
                        bx + 10, by + (bh - isz) / 2, isz, hov || anim ? 0xFFFFFFu : p2ui_icon_color64(t),
                        hov || anim ? 255 : 230);
            const int lh = p2ui_line_h64();
            p2ui_text64(bx + 38, by + (bh - lh) / 2, rows_zh[i],
                        hov || anim ? 0xFFFFFFu : (t->dark ? t->text : rgb(24, 26, 32)));
        }
    }
}

void startmenu64_draw64() { if (g_open) draw_menu64(); }

// ==================== 命中测试 ====================
static bool in_rect64(int mx, int my, int x, int y, int w, int h) {
    return mx >= x && my >= y && mx < x + w && my < y + h;
}
static int hit_item64(int mx, int my) {
    if (!g_open) return -1;
    // 电源二级菜单优先
    if (g_pm_open) {
        for (int i = 0; i < 3; i++) {
            int x = 0, y = 0, w = 0, h = 0;
            startmenu64_pm_rect64(i, &x, &y, &w, &h);
            if (in_rect64(mx, my, x, y, w, h)) return 30 + i;
        }
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_pm_panel64(&x, &y, &w, &h);
        if (in_rect64(mx, my, x, y, w, h)) return 29;      // 面板内部空白
    }
    if (g_query[0]) {
        for (int i = 0; i < g_hits && i < 6; i++) {
            int x = 0, y = 0, w = 0, h = 0;
            startmenu64_result_rect64(i, &x, &y, &w, &h);
            if (in_rect64(mx, my, x, y, w, h)) return 20 + i;
        }
    } else {
        for (int i = 0; i < SM_PINNED_N; i++) {
            int x = 0, y = 0, w = 0, h = 0;
            startmenu64_tile_rect64(i, &x, &y, &w, &h);
            if (in_rect64(mx, my, x, y, w, h)) return 10 + i;
        }
    }
    for (int i = 0; i < 4; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_status_rect64(i, &x, &y, &w, &h);
        if (in_rect64(mx, my, x - 4, y - 4, w + 8, h + 8)) return 2 + i;   // 2=net 3=sound 4=lang 5=notif
    }
    int x = 0, y = 0, w = 0, h = 0;
    startmenu64_search_rect64(&x, &y, &w, &h);
    if (in_rect64(mx, my, x, y, w, h)) return 1;
    startmenu64_time_rect64(&x, &y, &w, &h);
    if (in_rect64(mx, my, x, y, w, h)) return 0;
    startmenu64_power_rect64(&x, &y, &w, &h);
    if (in_rect64(mx, my, x, y, w, h)) return 7;
    startmenu64_settings_rect64(&x, &y, &w, &h);
    if (in_rect64(mx, my, x, y, w, h)) return 8;
    startmenu64_user_rect64(&x, &y, &w, &h);
    if (in_rect64(mx, my, x, y, w, h)) return 6;
    return -1;
}
static const char* item_name64(int item) {
    if (item == 0) return "time";
    if (item == 1) return "search";
    if (item == 2) return "net";
    if (item == 3) return "sound";
    if (item == 4) return "lang";
    if (item == 5) return "notif";
    if (item == 6) return "user";
    if (item == 7) return "power";
    if (item == 8) return "settings";
    if (item == 29) return "powermenu";
    if (item >= 30) return "poweritem";
    if (item >= 20) return "result";
    if (item >= 10) return "tile";
    return "?";
}

// ==================== 输入 ====================
int startmenu64_handle_mouse_press64(int mx, int my, int button) {
    if (!g_open) return 0;
    const bool inside = in_rect64(mx, my, sm_x(), sm_y(), sm_w(), sm_h());
    const int item = hit_item64(mx, my);
    if (button != 0) return inside ? 1 : 0;
    if (item < 0) {
        // 点菜单外部：关菜单（不消费，让点击继续落到桌面，与老菜单语义一致）
        if (!inside) {
            startmenu64_close64("click-outside");
            return 0;
        }
        return 1;
    }
    if (g_pm_open && item != 29 && item < 30) {
        g_pm_open = false;                       // 点到菜单别处 -> 电源菜单收起
        dirty_pm64();
    }
    if (g_click_budget > 0) {
        g_click_budget--;
        dbg64_line_begin64();
        dbg64_str("[START64] click item=");
        dbg64_str(item_name64(item));
        if (item >= 10 && item < 20) {
            dbg64_str(" idx=");
            dbg64_dec((uint64_t)(item - 10));
        }
        dbg64_str(" at=");
        dbg64_dec((uint64_t)mx);
        dbg64_str(",");
        dbg64_dec((uint64_t)my);
        dbg64_nl();
        dbg64_line_end64();
    }
    switch (item) {
        case 0:                                  // 左上角时间 -> 日历（在该区域上方弹出）
            panels64_toggle64(PANEL64_CAL, "start64-time");
            return 1;
        case 1:                                  // 搜索框：聚焦
            g_query[0] = 0;
            g_hits = 0;
            return 1;
        case 2: panels64_toggle64(PANEL64_NET, "start64-status-net"); return 1;
        case 3: panels64_toggle64(PANEL64_SOUND, "start64-status-sound"); return 1;
        case 4: {                                // 中/英：切 GUI 语言（键盘语言仍是"英"，如实）
            const bool zh = gui64_lang_zh();
            gui64_set_lang_zh(!zh);
            dbg64_line_begin64();
            dbg64_str("[START64] click item=lang gui_lang=");
            dbg64_str(!zh ? "zh" : "en");
            dbg64_str(" kbd_lang=");
            dbg64_str(kbd_lang64() ? "中" : "英");
            dbg64_str(" ime=0");
            dbg64_nl();
            dbg64_line_end64();
            return 1;
        }
        case 5: panels64_toggle64(PANEL64_NOTIF, "start64-status-notif"); return 1;
        case 6: return 1;                        // 用户：本批无账户页
        case 7:                                  // 电源按钮 -> 当前界面内的小圆角框
            g_pm_open = !g_pm_open;
            g_hover = -1;
            dirty_pm64();
            dbg64_line_begin64();
            dbg64_str("[START64] power menu ");
            dbg64_str(g_pm_open ? "open" : "close");
            dbg64_str(" x=");
            dbg64_dec((uint64_t)pm_x64());
            dbg64_str(" y=");
            dbg64_dec((uint64_t)pm_y64());
            dbg64_str(" w=");
            dbg64_dec((uint64_t)THEME64_PM_W);
            dbg64_str(" h=");
            dbg64_dec((uint64_t)pm_h64());
            dbg64_str(" rows=3 row0=shutdown row1=reboot row2=lock");
            dbg64_nl();
            dbg64_line_end64();
            return 1;
        case 8:                                  // 设置：先关开始菜单，再打开设置窗口
            startmenu64_close64("settings");
            dbg64_line_begin64();
            dbg64_str("[START64] settings open (menu closed first)");
            dbg64_nl();
            dbg64_line_end64();
            app_settings_open64();
            return 1;
        case 29: return 1;
        default: break;
    }
    if (item >= 30 && item <= 32) {              // 电源菜单三项
        static const char* act[3] = {"shutdown", "reboot", "lock"};
        g_pm_anim = item - 30;                   // 动态效果（按下高亮 + 轻微内缩）
        g_pm_anim_t0 = ticks64();
        g_pm_anim_frame = 0;
        dirty_pm64();
        dbg64_line_begin64();
        dbg64_str("[START64] power action ");
        dbg64_str(act[g_pm_anim]);
        dbg64_str(" anim=");
        dbg64_dec((uint64_t)theme64_dur64(THEME64_MS_FAST));
        dbg64_str("ms");
        dbg64_nl();
        dbg64_line_end64();
        if (g_pm_anim == 0) { sys_shutdown64(); }
        else if (g_pm_anim == 1) { sys_reboot64(); }
        else { g_pm_open = false; locklogin64_lock64("start64"); }
        return 1;
    }
    if (item >= 20 && item < 20 + 8) {           // 搜索结果
        const int i = item - 20;
        if (i < g_hits) {
            const SmApp64& a = kApps[g_hit_app[i]];
            startmenu64_close64("launch");
            startmenu64_launch64(a.app_id, "result");
        }
        return 1;
    }
    if (item >= 10 && item < 10 + SM_PINNED_N) { // 固定网格
        const SmApp64& a = kApps[kPinned[item - 10]];
        startmenu64_close64("launch");
        startmenu64_launch64(a.app_id, "tile");
        return 1;
    }
    return 1;
}

void startmenu64_handle_mouse_move64(int mx, int my, int buttons) {
    (void)buttons;
    if (!g_open) return;
    const int item = hit_item64(mx, my);
    if (item != g_hover) {
        g_hover = item;
        if (item >= 0 && g_hover_budget > 0) {
            g_hover_budget--;
            dbg64_line_begin64();
            dbg64_str("[START64] hover item=");
            dbg64_str(item_name64(item));
            if (item >= 10 && item < 20) {
                dbg64_str(" idx=");
                dbg64_dec((uint64_t)(item - 10));
            }
            dbg64_nl();
            dbg64_line_end64();
        }
        dirty_menu64(10);
    }
    // 电源菜单按下动效推进（轻微内缩 + 高亮，150ms）
    if (g_pm_anim >= 0) {
        const int dur = (int)theme64_dur64(THEME64_MS_FAST);
        if (dur <= 0 || (uint32_t)(int32_t)(ticks64() - g_pm_anim_t0) >= ms_to_ticks64((uint32_t)dur)) {
            g_pm_anim = -1;
            dirty_pm64();
        } else {
            g_pm_anim_frame++;
            dirty_pm64();
        }
    }
}

int startmenu64_wheel64(int dz) {
    if (!g_open || dz == 0) return 0;
    // 搜索结果多时滚轮换选择（本批最多 6 条，够用；日历/WiFi 的滚轮在 panels64）
    if (g_query[0] && g_hits > 1) {
        g_hit_sel += (dz > 0 ? -1 : 1);
        if (g_hit_sel < 0) g_hit_sel = 0;
        if (g_hit_sel > g_hits - 1) g_hit_sel = g_hits - 1;
        dirty_menu64(0);
        return 1;
    }
    return 0;
}

int startmenu64_handle_key64(uint8_t c) {
    if (!g_open) return 0;
    if (c == 0x1B) {                             // ESC：先关电源二级菜单，再关开始菜单（两级退出）
        if (g_pm_open) {
            g_pm_open = false;
            dirty_pm64();
            dbg64_line_begin64();
            dbg64_str("[START64] esc level=2 close=power-menu");
            dbg64_nl();
            dbg64_line_end64();
            return 1;
        }
        startmenu64_close64("esc");
        return 1;
    }
    if (c == 0x08) {                             // 退格
        int n = s_len(g_query);
        if (n > 0) {
            g_query[n - 1] = 0;
            search_update64("backspace");
            dirty_menu64(0);
        }
        return 1;
    }
    if (c == '\n' || c == '\r') {                // 回车 -> 启动第一个命中
        if (g_query[0] && g_hits > 0) {
            const SmApp64& a = kApps[g_hit_app[g_hit_sel < g_hits ? g_hit_sel : 0]];
            startmenu64_close64("search-enter");
            startmenu64_launch64(a.app_id, "search-enter");
        }
        return 1;
    }
    if (c == 0xFD) { g_hit_sel = g_hit_sel > 0 ? g_hit_sel - 1 : 0; dirty_menu64(0); return 1; }  // 上
    if (c == 0xFE) { if (g_hit_sel < g_hits - 1) g_hit_sel++; dirty_menu64(0); return 1; }        // 下
    if (c == 0xFB || c == 0xFC) return 1;        // 左右（本批菜单内不响应）
    if (c >= 0x20 && c < 0x7F) {                 // 可打印字符 -> 搜索框（Caps/Shift 已由 input.cpp 处理）
        const int n = s_len(g_query);
        if (n < (int)sizeof(g_query) - 1) {
            g_query[n] = (char)c;
            g_query[n + 1] = 0;
            search_update64("typing");
            dirty_menu64(0);
        }
        return 1;
    }
    return 1;                                     // 菜单打开时其它键一律不喂给应用
}

// ==================== 每帧 ====================
void startmenu64_tick64() {
    if (!g_open) return;
    // 时间每秒变 -> 只重画左上角那一小块
    const int ss = g_ss;
    time_refresh64(false);
    if (g_ss != ss) {
        gui64_dirty(sm_x(), sm_y(), 200, 2 * p2ui_line_h64() + 8);
    }
    net_state_refresh64();
    status_log64(false);                 // 声音/中英/未读角标变化时打点（角标 = 红点/数字）
    // 打开动效：动效期间每帧重画菜单
    const int dur = (int)theme64_dur64(THEME64_MS_NORMAL);
    if (dur > 0 && (uint32_t)(int32_t)(ticks64() - g_opened_t0) < ms_to_ticks64((uint32_t)dur))
        dirty_menu64(0);
}

// ==================== 自检（纯逻辑）====================
int startmenu64_selftest64() {
    int fails = 0;
    const int W = fb_width(), H = fb_height();
    if (H <= 0) return 1;
    // 1) 几何：水平居中 + 底边离 Dock 顶边 10~12px + 上圆角 24 / 下圆角 10
    const int center_dx = (sm_x() + sm_w() / 2) - W / 2;
    if (center_dx > 1 || center_dx < -1) fails |= 1;
    const int gap = dock_top64() - (sm_y() + sm_h());
    if (gap < 10 || gap > 12) fails |= 2;
    if (THEME64_SM_R_TOP != 24 || THEME64_SM_R_BOTTOM != 10) fails |= 4;
    if (sm_y() < 0) fails |= 8;
    if (sm_x() < 0 || sm_x() + sm_w() > W) fails |= 16;
    // 2) 固定网格 2 列 × 4 行，且不越出菜单/不压 Dock
    if (THEME64_SM_TILE_COLS != 2 || THEME64_SM_TILE_ROWS != 4) fails |= 32;
    for (int i = 0; i < SM_PINNED_N; i++) {
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_tile_rect64(i, &x, &y, &w, &h);
        if (x < sm_x() || x + w > sm_x() + sm_w() || y + h > sm_y() + sm_h()) fails |= 64;
        if (y + h > dock_top64()) fails |= 128;
    }
    // 3) 搜索框在顶部偏右、在菜单内
    {
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_search_rect64(&x, &y, &w, &h);
        if (x + w / 2 < sm_x() + sm_w() / 2) fails |= 256;      // 偏右
        if (y < sm_y() || y + h > sm_y() + sm_h()) fails |= 512;
    }
    // 4) 状态区图标：从左到右 = 网络 -> 声音 -> 中/英 -> 通知；都在菜单内
    {
        for (int i = 0; i < 4; i++) {
            int x = 0, y = 0, w = 0, h = 0;
            startmenu64_status_rect64(i, &x, &y, &w, &h);
            if (x < sm_x() || x + w > sm_x() + sm_w() || y + h > sm_y() + sm_h()) fails |= 1024;
            if (i > 0) {
                int px = 0, py = 0, pw = 0, ph = 0;
                startmenu64_status_rect64(i - 1, &px, &py, &pw, &ph);
                if (px + pw > x) fails |= 2048;                 // 顺序：网络 -> 声音 -> 中/英 -> 通知
            }
        }
    }
    // 5) 电源按钮/设置按钮在右下角，且不压 Dock
    {
        int x = 0, y = 0, w = 0, h = 0;
        startmenu64_power_rect64(&x, &y, &w, &h);
        if (y + h > dock_top64() || x < sm_x() + sm_w() / 2 || y < sm_y() + sm_h() / 2) fails |= 4096;
        startmenu64_settings_rect64(&x, &y, &w, &h);
        if (x + w > power_x64()) fails |= 8192;                 // 设置按钮在电源按钮左边
    }
    // 6) 搜索匹配（名字：中/英/别名，大小写不敏感）
    {
        const char* q = "calc";
        int hit = -1;
        for (int i = 0; i < SM_APP_N; i++)
            if (s_has_icase(kApps[i].zh, q) || s_has_icase(kApps[i].en, q) || s_has_icase(kApps[i].alias, q)) { hit = i; break; }
        if (hit < 0 || kApps[hit].app_id != APP_ID_CALC) fails |= 16384;
        if (!s_has_icase(kApps[0].zh, "终端")) fails |= 32768;
        if (!s_has_icase(kApps[0].en, "TERM")) fails |= 65536;
    }
    // 7) 日历/星期换算（与时区无关的固定样本）
    if (weekday_ymd64(2026, 9, 23) != 3) fails |= 131072;       // 2026-09-23 = 星期三
    if (days_in_month64(2024, 2) != 29 || days_in_month64(2025, 2) != 28) fails |= 262144;
    return fails;
}
