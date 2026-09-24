// gfx64.cpp - 现代图元层实现（见 gfx64.h 的设计说明）：
//   圆角(带 4x4 亚像素覆盖)/双层阴影(预生成 alpha mask + LRU)/毛玻璃(壁纸模糊一次 + 内容层
//   区域模糊缓存)/渐变填充/壁纸 6 种适应模式/局部方差度量。
//
// 无浮点、无 libc：定点整数 + kmalloc_64。
#include "gfx64.h"
#include "fb.h"
#include "config64.h"
#include "mem_64.h"
#include "debug64.h"
#include "x86_64.h"     // ticks64
#include "panic64.h"    // panic64_watchdog_pause64/unpause64（整屏重建是长操作，见 wall_ensure64）

// ==================== 画布 / 裁剪 ====================
static uint32_t* g_fb = nullptr;
static int g_fw = 0, g_fh = 0;

static inline uint32_t* canvas64() {
    if (!g_fb) g_fb = fb_surface64(&g_fw, &g_fh);
    return g_fb;
}
static inline void clip_get64(int* x, int* y, int* w, int* h) { fb_get_clip64(x, y, w, h); }
// 与裁剪区求交（半开区间）；返回 false = 全被裁掉
static inline bool rect_clip64(int* x, int* y, int* w, int* h) {
    int cx, cy, cw, ch;
    clip_get64(&cx, &cy, &cw, &ch);
    if (*x < cx) { *w += *x - cx; *x = cx; }
    if (*y < cy) { *h += *y - cy; *y = cy; }
    if (*x + *w > cx + cw) *w = cx + cw - *x;
    if (*y + *h > cy + ch) *h = cy + ch - *y;
    return (*w > 0 && *h > 0);
}

// ==================== 颜色 / 混合 ====================
static inline int cc_r(uint32_t c) { return (int)((c >> 16) & 0xFF); }
static inline int cc_g(uint32_t c) { return (int)((c >> 8) & 0xFF); }
static inline int cc_b(uint32_t c) { return (int)(c & 0xFF); }

// ★ 性能关键：每像素混合里**不能有除法**（QEMU TCG 下整数除要几十个周期；双层阴影 + 毛玻璃
//   每帧要混合十几万像素，用 /255 会把一帧拖到几百毫秒，进而把看门狗/宿主验收的时序拖崩）。
//   v/255 用 (v*257+128)>>16 近似：对 0..65535 的 v 误差 <= 1 级，肉眼与验收都无差。
static inline int div255_64(int v) { return (v * 257 + 128) >> 16; }

static inline uint32_t blend_c64(uint32_t dst, uint32_t c, int a) {
    if (a >= 255) return c & 0x00FFFFFF;
    if (a <= 0) return dst;
    const int ia = 255 - a;
    const int r = div255_64(cc_r(c) * a + cc_r(dst) * ia);
    const int g = div255_64(cc_g(c) * a + cc_g(dst) * ia);
    const int b = div255_64(cc_b(c) * a + cc_b(dst) * ia);
    return (uint32_t)((r << 16) | (g << 8) | b);
}

void gfx64_blend64(int x, int y, uint32_t c, int a) {
    uint32_t* px = canvas64();
    int cx, cy, cw, ch;
    clip_get64(&cx, &cy, &cw, &ch);
    if (x < cx || y < cy || x >= cx + cw || y >= cy + ch) return;
    px[(uint64_t)y * g_fw + x] = blend_c64(px[(uint64_t)y * g_fw + x], c, a);
}

// 水平整段混合（内部快路径：a>=255 直接写）
static inline void blend_run64(uint32_t* row, int x0, int x1, uint32_t c, int a) {
    if (a >= 255) {
        const uint32_t v = c & 0x00FFFFFF;
        for (int x = x0; x < x1; x++) row[x] = v;
        return;
    }
    if (a <= 0) return;
    for (int x = x0; x < x1; x++) row[x] = blend_c64(row[x], c, a);
}

// ==================== 圆角覆盖率表（4x4 亚像素）====================
// 表按半径缓存（最多 4 个半径：Dock 24 / 窗口 14 / 卡片 12 / 图标 10）。
#define GFX64_COV_MAX_R 32
#define GFX64_COV_SLOTS 8
static uint8_t g_cov_tab[GFX64_COV_SLOTS][GFX64_COV_MAX_R * GFX64_COV_MAX_R];
static int     g_cov_r[GFX64_COV_SLOTS] = {0, 0, 0, 0};
static int     g_cov_next = 0;

static const uint8_t* cov_table64(int r) {
    if (r <= 0) return nullptr;
    if (r > GFX64_COV_MAX_R) r = GFX64_COV_MAX_R;
    for (int i = 0; i < GFX64_COV_SLOTS; i++)
        if (g_cov_r[i] == r) return g_cov_tab[i];
    const int slot = g_cov_next;
    g_cov_next = (g_cov_next + 1) % GFX64_COV_SLOTS;
    uint8_t* tab = g_cov_tab[slot];
    g_cov_r[slot] = r;
    for (int j = 0; j < r; j++) {
        for (int i = 0; i < r; i++) {
            int inside = 0;
            // 圆心在 (r, r)（连续坐标）；像素 [i,i+1)x[j,j+1) 的 4x4 子采样
            for (int sy = 0; sy < 4; sy++) {
                for (int sx = 0; sx < 4; sx++) {
                    const int px8 = i * 8 + 1 + sx * 2;      // 1/8 像素定点
                    const int py8 = j * 8 + 1 + sy * 2;
                    const int dx8 = px8 - r * 8, dy8 = py8 - r * 8;
                    if ((int64_t)dx8 * dx8 + (int64_t)dy8 * dy8 <= (int64_t)(r * 8) * (r * 8)) inside++;
                }
            }
            tab[j * r + i] = (uint8_t)(inside * 255 / 16);
        }
    }
    return tab;
}

// 单像素覆盖率（dx/dy = 相对矩形左上角的整数坐标）
static inline int cov_px64(const uint8_t* tab, int r, int w, int h, int dx, int dy) {
    if (!tab) return 255;
    int i = -1, j = -1;
    if (dx < r) i = dx; else if (dx >= w - r) i = w - 1 - dx;
    if (dy < r) j = dy; else if (dy >= h - r) j = h - 1 - dy;
    if (i < 0 || j < 0) return 255;
    return tab[j * r + i];
}

// ==================== 圆角矩形填充 / 渐变 / 描边 ====================
static void fill_round_impl64(int x, int y, int w, int h, int r, uint32_t c, int a, int grad_diag,
                              uint32_t c1) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!rect_clip64(&x0, &y0, &ww, &hh)) return;
    uint32_t* px = canvas64();
    const uint8_t* tab = cov_table64(r);
    for (int yy = y0; yy < y0 + hh; yy++) {
        uint32_t* row = px + (uint64_t)yy * g_fw;
        const int dy = yy - y;
        const bool mid_row = (dy >= r) && (dy < h - r);
        if (mid_row) {
            if (grad_diag == 0 && c1 == c) {
                blend_run64(row, x0, x0 + ww, c, a);
            } else {
                for (int xx = x0; xx < x0 + ww; xx++) {
                    uint32_t col = c;
                    if (c1 != c) {
                        const int dx = xx - x;
                        int t = grad_diag ? (dx + dy) * 256 / (w + h ? w + h : 1) : dx * 256 / (w ? w : 1);
                        if (t < 0) t = 0; if (t > 255) t = 255;
                        const int it = 255 - t;
                        const int rr = div255_64(cc_r(c) * it + cc_r(c1) * t);
                        const int gg = div255_64(cc_g(c) * it + cc_g(c1) * t);
                        const int bb = div255_64(cc_b(c) * it + cc_b(c1) * t);
                        col = (uint32_t)((rr << 16) | (gg << 8) | bb);
                    }
                    row[xx] = blend_c64(row[xx], col, a);
                }
            }
            continue;
        }
        for (int xx = x0; xx < x0 + ww; xx++) {
            const int cov = cov_px64(tab, r, w, h, xx - x, dy);
            if (cov <= 0) continue;
            const int aa = div255_64(cov * a);
            if (aa <= 0) continue;
            uint32_t col = c;
            if (c1 != c) {
                const int dx2 = xx - x;
                int t = grad_diag ? (dx2 + dy) * 256 / (w + h ? w + h : 1) : dx2 * 256 / (w ? w : 1);
                if (t < 0) t = 0; if (t > 255) t = 255;
                const int it = 255 - t;
                const int rr = div255_64(cc_r(c) * it + cc_r(c1) * t);
                const int gg = div255_64(cc_g(c) * it + cc_g(c1) * t);
                const int bb = div255_64(cc_b(c) * it + cc_b(c1) * t);
                col = (uint32_t)((rr << 16) | (gg << 8) | bb);
            }
            row[xx] = blend_c64(row[xx], col, aa);
        }
    }
}

void gfx64_fill_round64(int x, int y, int w, int h, int r, uint32_t c, int a) {
    fill_round_impl64(x, y, w, h, r, c, a, 0, c);
}
void gfx64_grad_round64(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1, int diag, int a) {
    fill_round_impl64(x, y, w, h, r, c0, a, diag, c1);
}

// 1px 描边（只遍历周带；角上用覆盖率差做 AA）
void gfx64_stroke_round64(int x, int y, int w, int h, int r, uint32_t c, int a) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!rect_clip64(&x0, &y0, &ww, &hh)) return;
    if (r <= 0) {                      // 直角矩形：四条 1px 边
        for (int xx = x0; xx < x0 + ww; xx++)
            for (int k = 0; k < 2; k++) {
                const int yy = (k == 0) ? y : y + h - 1;
                if (yy >= y0 && yy < y0 + hh) gfx64_blend64(xx, yy, c, a);
            }
        for (int yy = y0; yy < y0 + hh; yy++)
            for (int k = 0; k < 2; k++) {
                const int xx = (k == 0) ? x : x + w - 1;
                if (xx >= x0 && xx < x0 + ww) gfx64_blend64(xx, yy, c, a);
            }
        return;
    }
    uint32_t* px = canvas64();
    const uint8_t* tab = cov_table64(r);
    const uint8_t* tabi = cov_table64(r > 1 ? r - 1 : 0);
    const int iw = w - 2, ih = h - 2;      // 内矩形（1px 厚）
    for (int yy = y0; yy < y0 + hh; yy++) {
        uint32_t* row = px + (uint64_t)yy * g_fw;
        const int dy = yy - y;
        const bool horiz_edge = (dy == 0) || (dy == h - 1);
        for (int xx = x0; xx < x0 + ww; xx++) {
            const int dx = xx - x;
            if (!horiz_edge && dx != 0 && dx != w - 1) continue;   // 周带之外的像素不碰
            int co = cov_px64(tab, r, w, h, dx, dy);
            int ci = 0;
            if (dx >= 1 && dx <= w - 2 && dy >= 1 && dy <= h - 2)
                ci = cov_px64(tabi, r > 1 ? r - 1 : 0, iw, ih, dx - 1, dy - 1);
            const int cov = co - ci;
            if (cov <= 0) continue;
            row[xx] = blend_c64(row[xx], c, div255_64(cov * a));
        }
    }
}

// ==================== 双层阴影（预生成 alpha mask + LRU）====================
// mask 以 1/2 分辨率生成（ds=2）：阴影是低频渐变，最近邻放大肉眼无差，写入带宽省 3/4。
#define GFX64_SH_SLOTS 6
struct Gfx64ShMask {
    bool     used;
    int      w, h, r;
    int      nw, nh, fw, fh;          // 半分辨率尺寸
    uint8_t* near_m;
    uint8_t* far_m;
    int      near_x, near_y, far_x, far_y;   // mask 左上角（屏幕坐标，已含 y 偏移）
    uint32_t stamp;
};
static Gfx64ShMask g_shmask[GFX64_SH_SLOTS];
static uint32_t g_sh_stamp = 0;
static uint32_t g_sh_hit = 0, g_sh_miss = 0;

// 半分辨率 mask 生成：先按覆盖率写形状，再做 2 趟盒子模糊（半径 = Token 模糊/2 / ds）
static void mask_build64(uint8_t* m, int mw, int mh, int ox, int oy, int w, int h, int r, int blur) {
    const uint8_t* tab = cov_table64(r);
    const int radius = (blur / 2) / 2;         // ds=2 + 两趟
    const int pad = radius * 2;
    for (int my = 0; my < mh; my++) {
        for (int mx = 0; mx < mw; mx++) {
            // 半分辨率像素 = 2x2 全分辨率块，取 4 个象限中心的覆盖平均
            int cov = 0;
            for (int q = 0; q < 4; q++) {
                const int fx = ox + mx * 2 + (q & 1) + 0;        // 全分辨率坐标（块内偏移 0/1）
                const int fy = oy + my * 2 + ((q >> 1) & 1) + 0;
                const int dx = fx, dy = fy;
                if (dx < 0 || dy < 0 || dx >= w || dy >= h) continue;
                cov += cov_px64(tab, r, w, h, dx, dy);
            }
            m[(uint64_t)my * mw + mx] = (uint8_t)(cov / 4);
        }
    }
    if (radius <= 0) return;
    // 水平 + 垂直各若干趟（就地，用一条临时行/列）。趟数：小半径（近层 4px）2 趟就够；
    // 大半径（远层 32px）用 3 趟 —— 盒式 3 趟的尾巴更接近高斯（CSS box-shadow blur 的观感）。
    // ★ 用**滑动窗口**（O(1)/像素）：早期版本每像素重算 2r+1 次和，700x460 的窗口要 1.2 秒，
    //   多开几个窗口就把主循环拖过看门狗 5s（实测 PANIC64 stop=WATCHDOG_TIMEOUT）。
    const int npass = (blur >= 16) ? 3 : 2;
    static uint8_t rowbuf[4096];
    static uint8_t colbuf[2048];
    for (int pass = 0; pass < npass; pass++) {
        for (int my = 0; my < mh; my++) {
            uint8_t* row = m + (uint64_t)my * mw;
            const int n = mw > 4096 ? 4096 : mw;
            for (int i = 0; i < n; i++) rowbuf[i] = row[i];
            const int win = 2 * radius + 1;
            int sum = 0;
            for (int i = -radius; i <= radius; i++) {
                const int xi = i < 0 ? 0 : (i >= mw ? mw - 1 : i);
                sum += (xi < n ? rowbuf[xi] : row[xi]);
            }
            for (int mx = 0; mx < mw; mx++) {
                row[mx] = (uint8_t)(sum / win);
                const int xa = mx - radius, xb = mx + radius + 1;
                const int ia = xa < 0 ? 0 : (xa >= mw ? mw - 1 : xa);
                const int ib = xb < 0 ? 0 : (xb >= mw ? mw - 1 : xb);
                sum += (ib < n ? rowbuf[ib] : row[ib]) - (ia < n ? rowbuf[ia] : row[ia]);
            }
        }
        for (int mx = 0; mx < mw; mx++) {
            const int nh = mh > 2048 ? 2048 : mh;
            for (int i = 0; i < nh; i++) colbuf[i] = m[(uint64_t)i * mw + mx];
            const int win = 2 * radius + 1;
            int sum = 0;
            for (int i = -radius; i <= radius; i++) {
                const int yi = i < 0 ? 0 : (i >= mh ? mh - 1 : i);
                sum += (yi < nh ? colbuf[yi] : m[(uint64_t)yi * mw + mx]);
            }
            for (int my = 0; my < mh; my++) {
                m[(uint64_t)my * mw + mx] = (uint8_t)(sum / win);
                const int ya = my - radius, yb = my + radius + 1;
                const int ia = ya < 0 ? 0 : (ya >= mh ? mh - 1 : ya);
                const int ib = yb < 0 ? 0 : (yb >= mh ? mh - 1 : yb);
                sum += ((ib < nh ? colbuf[ib] : m[(uint64_t)ib * mw + mx]) -
                        (ia < nh ? colbuf[ia] : m[(uint64_t)ia * mw + mx]));
            }
        }
        (void)pad;
    }
}

static Gfx64ShMask* sh_mask_get64(int w, int h, int r, const char* why) {
    for (int i = 0; i < GFX64_SH_SLOTS; i++)
        if (g_shmask[i].used && g_shmask[i].w == w && g_shmask[i].h == h && g_shmask[i].r == r) {
            g_shmask[i].stamp = ++g_sh_stamp;
            g_sh_hit++;
            return &g_shmask[i];
        }
    g_sh_miss++;
    // 淘汰最旧
    int victim = 0;
    for (int i = 1; i < GFX64_SH_SLOTS; i++)
        if (!g_shmask[i].used || g_shmask[i].stamp < g_shmask[victim].stamp) victim = i;
    Gfx64ShMask* s = &g_shmask[victim];
    if (s->used) {
        if (s->near_m) kfree_64(s->near_m);
        if (s->far_m) kfree_64(s->far_m);
    }
    *s = Gfx64ShMask{};
    s->used = true;
    s->w = w; s->h = h; s->r = r;
    const int pad_n = THEME64_SH_N_BLUR * 2 + 2;
    const int pad_f = THEME64_SH_F_BLUR * 2 + 2;
    const int box_n = w + pad_n * 2, box_f = w + pad_f * 2;
    const int boxy_n = h + pad_n * 2, boxy_f = h + pad_f * 2;
    s->nw = (box_n + 1) / 2; s->nh = (boxy_n + 1) / 2;
    s->fw = (box_f + 1) / 2; s->fh = (boxy_f + 1) / 2;
    s->near_x = -pad_n; s->near_y = -pad_n + THEME64_SH_N_DY;
    s->far_x  = -pad_f; s->far_y  = -pad_f + THEME64_SH_F_DY;
    const uint64_t t0 = ticks64();
    s->near_m = (uint8_t*)kmalloc_64((uint64_t)s->nw * s->nh);
    s->far_m  = (uint8_t*)kmalloc_64((uint64_t)s->fw * s->fh);
    // ★ mask 原点必须是 **-pad**：mask 像素 m 对应屏幕 (x-pad+2m)，所以它的"外形坐标"从 -pad 起算。
    //   （写 +pad 会让形状在 mask 里整体偏移 2*pad，阴影会缺一半并错位。）
    if (s->near_m) mask_build64(s->near_m, s->nw, s->nh, -pad_n, -pad_n, w, h, r, THEME64_SH_N_BLUR);
    if (s->far_m)  mask_build64(s->far_m, s->fw, s->fh, -pad_f, -pad_f, w, h, r, THEME64_SH_F_BLUR);
    dbg64_line_begin64();
    dbg64_str("[GFX64] shadow mask build w=");
    dbg64_dec((uint64_t)w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)h);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)r);
    dbg64_str(" ds=2 near=");
    dbg64_dec((uint64_t)s->nw);
    dbg64_str("x");
    dbg64_dec((uint64_t)s->nh);
    dbg64_str(" far=");
    dbg64_dec((uint64_t)s->fw);
    dbg64_str("x");
    dbg64_dec((uint64_t)s->fh);
    dbg64_str(" why=");
    dbg64_str(why ? why : "?");
    dbg64_str(" ticks=");
    dbg64_dec((uint64_t)(ticks64() - t0));
    dbg64_nl();
    dbg64_line_end64();
    return s;
}

// 把半分辨率 mask 按 2x2 块铺到屏幕（与裁剪求交；a 为图层 alpha 0..255）
static void mask_blit64(const uint8_t* m, int mw, int mh, int mx0, int my0, int a, uint32_t col) {
    if (!m || a <= 0) return;
    int cx, cy, cw, ch;
    clip_get64(&cx, &cy, &cw, &ch);
    uint32_t* px = canvas64();
    int my1 = (cy - my0 + 1) / 2, my2 = (cy + ch - my0 + 1) / 2;
    if (my1 < 0) my1 = 0;
    if (my2 > mh) my2 = mh;
    for (int my = my1; my < my2; my++) {
        int sy = my0 + my * 2;
        int hgt = 2;
        if (sy < cy) { hgt -= (cy - sy); sy = cy; }
        if (sy + hgt > cy + ch) hgt = cy + ch - sy;
        if (hgt <= 0) continue;
        const uint8_t* row = m + (uint64_t)my * mw;
        int mx1 = (cx - mx0 + 1) / 2, mx2 = (cx + cw - mx0 + 1) / 2;
        if (mx1 < 0) mx1 = 0;
        if (mx2 > mw) mx2 = mw;
        for (int mx = mx1; mx < mx2; mx++) {
            const int ma = div255_64(row[mx] * a);
            if (ma <= 2) continue;                    // 极浅的阴影不落笔（也省带宽）
            int sx = mx0 + mx * 2;
            int wid = 2;
            if (sx < cx) { wid -= (cx - sx); sx = cx; }
            if (sx + wid > cx + cw) wid = cx + cw - sx;
            if (wid <= 0) continue;
            for (int i = 0; i < hgt; i++) {
                uint32_t* r = px + (uint64_t)(sy + i) * g_fw;
                for (int j = 0; j < wid; j++) r[sx + j] = blend_c64(r[sx + j], col, ma);
            }
        }
    }
}

void gfx64_shadow64(int x, int y, int w, int h, int r, const Theme64Tokens* t) {
    if (w <= 0 || h <= 0 || !t) return;
    Gfx64ShMask* s = sh_mask_get64(w, h, r, "shadow");
    if (!s) return;
    if (s->far_m)  mask_blit64(s->far_m, s->fw, s->fh, x + s->far_x, y + s->far_y, THEME64_SH_F_A, t->shadow_far);
    if (s->near_m) mask_blit64(s->near_m, s->nw, s->nh, x + s->near_x, y + s->near_y, THEME64_SH_N_A, t->shadow_near);
}

// ==================== 模糊（可分离盒子模糊多趟 ≈ 高斯）====================
// 就地 2 趟盒子模糊（半径 r），源/目标是 RGBA 缓冲（w x h，stride = w）
static void box_blur_buf64(uint32_t* buf, int w, int h, int r) {
    if (r <= 0 || w <= 0 || h <= 0) return;
    static uint32_t scratch[4096];
    for (int pass = 0; pass < 2; pass++) {
        for (int y = 0; y < h; y++) {
            uint32_t* row = buf + (uint64_t)y * w;
            const int n = w > 4096 ? 4096 : w;
            for (int i = 0; i < n; i++) scratch[i] = row[i];
            const int win = 2 * r + 1;
            int sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) {
                const int xi = i < 0 ? 0 : (i >= w ? w - 1 : i);
                const uint32_t c = (xi < n ? scratch[xi] : row[xi]);
                sr += cc_r(c); sg += cc_g(c); sb += cc_b(c);
            }
            for (int x = 0; x < w; x++) {
                row[x] = (uint32_t)(((sr / win) << 16) | ((sg / win) << 8) | (sb / win));
                const int xa = x - r, xb = x + r + 1;
                const int ia = xa < 0 ? 0 : (xa >= w ? w - 1 : xa);
                const int ib = xb < 0 ? 0 : (xb >= w ? w - 1 : xb);
                const uint32_t ca = (ia < n ? scratch[ia] : row[ia]);
                const uint32_t cb = (ib < n ? scratch[ib] : row[ib]);
                sr += cc_r(cb) - cc_r(ca);
                sg += cc_g(cb) - cc_g(ca);
                sb += cc_b(cb) - cc_b(ca);
            }
        }
        for (int x = 0; x < w; x++) {
            const int n = h > 4096 ? 4096 : h;
            for (int i = 0; i < n; i++) scratch[i] = buf[(uint64_t)i * w + x];
            const int win = 2 * r + 1;
            int sr = 0, sg = 0, sb = 0;
            for (int i = -r; i <= r; i++) {
                const int yi = i < 0 ? 0 : (i >= h ? h - 1 : i);
                const uint32_t c = (yi < n ? scratch[yi] : buf[(uint64_t)yi * w + x]);
                sr += cc_r(c); sg += cc_g(c); sb += cc_b(c);
            }
            for (int y = 0; y < h; y++) {
                buf[(uint64_t)y * w + x] = (uint32_t)(((sr / win) << 16) | ((sg / win) << 8) | (sb / win));
                const int ya = y - r, yb = y + r + 1;
                const int ia = ya < 0 ? 0 : (ya >= h ? h - 1 : ya);
                const int ib = yb < 0 ? 0 : (yb >= h ? h - 1 : yb);
                const uint32_t ca = (ia < n ? scratch[ia] : buf[(uint64_t)ia * w + x]);
                const uint32_t cb = (ib < n ? scratch[ib] : buf[(uint64_t)ib * w + x]);
                sr += cc_r(cb) - cc_r(ca);
                sg += cc_g(cb) - cc_g(ca);
                sb += cc_b(cb) - cc_b(ca);
            }
        }
    }
}

// 区域模糊（内容层玻璃用；结果进 LRU 缓存）
#define GFX64_BLUR_SLOTS 8
struct Gfx64BlurEnt {
    bool used;
    int  x, y, w, h, r;
    uint32_t* buf;
    uint32_t stamp;
};
static Gfx64BlurEnt g_blur_cache[GFX64_BLUR_SLOTS];
static uint32_t g_blur_stamp = 0;
static uint32_t g_blur_hit = 0, g_blur_miss = 0;
static uint32_t g_blur_miss_log = 0;

static uint32_t* panel_blur64(int x, int y, int w, int h, int r) {
    for (int i = 0; i < GFX64_BLUR_SLOTS; i++)
        if (g_blur_cache[i].used && g_blur_cache[i].x == x && g_blur_cache[i].y == y &&
            g_blur_cache[i].w == w && g_blur_cache[i].h == h && g_blur_cache[i].r == r) {
            g_blur_cache[i].stamp = ++g_blur_stamp;
            g_blur_hit++;
            return g_blur_cache[i].buf;
        }
    g_blur_miss++;
    int victim = 0;
    for (int i = 1; i < GFX64_BLUR_SLOTS; i++)
        if (!g_blur_cache[i].used || g_blur_cache[i].stamp < g_blur_cache[victim].stamp) victim = i;
    Gfx64BlurEnt* e = &g_blur_cache[victim];
    if (e->used && e->buf) { kfree_64(e->buf); e->buf = nullptr; }
    if (!e->used) { *e = Gfx64BlurEnt{}; e->used = true; }
    e->x = x; e->y = y; e->w = w; e->h = h; e->r = r;
    e->buf = (uint32_t*)kmalloc_64((uint64_t)w * h * 4);
    if (!e->buf) return nullptr;
    // 从壁纸面取（含 3r 边缘扩展，避免边界变暗）——壁纸面不存在时用主题底色
    int sw = 0, sh = 0;
    const uint32_t* src = gfx64_wall_surface64(&sw, &sh);
    const Theme64Tokens* t = theme64_tokens64();
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            int sx = x + i, sy = y + j;
            uint32_t c = t->desktop_base;
            if (src && sx >= 0 && sy >= 0 && sx < sw && sy < sh) c = src[(uint64_t)sy * sw + sx];
            e->buf[(uint64_t)j * w + i] = c;
        }
    }
    box_blur_buf64(e->buf, w, h, r);
    if (g_blur_miss_log < 12) {
        g_blur_miss_log++;
        dbg64_line_begin64();
        dbg64_str("[GFX64] blur region miss x=");
        dbg64_dec((uint64_t)(unsigned)x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)(unsigned)y);
        dbg64_str(" wh=");
        dbg64_dec((uint64_t)w);
        dbg64_str("x");
        dbg64_dec((uint64_t)h);
        dbg64_str(" r=");
        dbg64_dec((uint64_t)r);
        dbg64_nl();
        dbg64_line_end64();
    }
    return e->buf;
}

// ==================== 毛玻璃面板 ====================
void gfx64_glass64(int x, int y, int w, int h, int r, int layer, uint32_t tint, int alpha,
                   uint32_t border_color, int border_alpha, const Theme64Tokens* t) {
    if (w <= 0 || h <= 0) return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!rect_clip64(&x0, &y0, &ww, &hh)) return;
    // 1) backdrop：背景层 = 预模糊壁纸（模糊只算过一次）；内容层 = 区域模糊缓存
    const uint32_t* back = nullptr;
    int bw = 0, bh = 0;
    int bx = 0, by = 0;
    if (layer == 0) {
        back = gfx64_wall_blur64(&bw, &bh);
        bx = 0; by = 0;
    } else {
        const int br = THEME64_BLUR_CONTENT;
        // 区域稍微扩展（避免刚移动时边缘露出未更新的模糊）
        back = panel_blur64(x - 2, y - 2, w + 4, h + 4, br);
        bw = w + 4; bh = h + 4; bx = x - 2; by = y - 2;
    }
    const Theme64Tokens* tk = t ? t : theme64_tokens64();
    uint32_t* px = canvas64();
    const uint8_t* tab = cov_table64(r);
    for (int yy = y0; yy < y0 + hh; yy++) {
        uint32_t* row = px + (uint64_t)yy * g_fw;
        const int dy = yy - y;
        const bool mid_row = (dy >= r) && (dy < h - r);
        for (int xx = x0; xx < x0 + ww; xx++) {
            const int dx = xx - x;
            int cov = 255;
            if (!mid_row) cov = cov_px64(tab, r, w, h, dx, dy);
            else if (dx < r || dx >= w - r) cov = cov_px64(tab, r, w, h, dx, dy);
            if (cov <= 0) continue;
            uint32_t src = tk->desktop_base;
            if (back) {
                const int sx = xx - bx, sy = yy - by;
                if (sx >= 0 && sy >= 0 && sx < bw && sy < bh) src = back[(uint64_t)sy * bw + sx];
            }
            const uint32_t glass = blend_c64(src, tint, alpha);
            if (cov >= 255) row[xx] = glass;
            else row[xx] = blend_c64(row[xx], glass, cov);
        }
    }
    // 2) 1px 半透明边框（玻璃厚度/高光边缘）
    gfx64_stroke_round64(x, y, w, h, r, border_color, border_alpha);
    // 3) 暗色主题：顶边 1px 内高光
    if (tk->dark) {
        for (int xx = x + r; xx < x + w - r; xx++)
            gfx64_blend64(xx, y + 1, tk->highlight, THEME64_A_HIGHLIGHT);
    }
}

// ==================== 壁纸 ====================
// 内置兜底壁纸：1200x900（4:3，与 1280x800 的 16:10 不同 → 6 种适应模式在像素上可区分）
#define GFX64_DEF_W 1200
#define GFX64_DEF_H 900
#define GFX64_DEF_MARK 16
#define GFX64_DEF_INSET 96

static uint32_t* g_wallpx = nullptr;       // 壁纸源（RGBA）
static int g_wallw = 0, g_wallh = 0;
static const char* g_wall_desc = "none";
static int g_mode = GFX64_WALL_FILL;
static int g_mode_cfg = -1;

static uint32_t* g_surface = nullptr;      // 铺好的整屏壁纸面（RGBA）
static uint32_t* g_surface_blur = nullptr; // 壁纸模糊一次的结果（背景层玻璃用）
static int g_sw = 0, g_sh = 0;
static bool g_surface_valid = false;
static int g_surface_theme = -99;
static int g_surface_mode = -99;
static const uint32_t* g_surface_src = nullptr;

static char g_wall_geom[320] = {0};
static int  g_wall_log_budget = 12;        // [GFX64] wall 行上限（防刷屏）
static uint64_t g_wall_build_count = 0;

// ---- 内置壁纸生成（也是模糊证据的载体：细颗粒在玻璃下被抹平）----
// 组成：主题渐变（对角） + 网格柔光斑（160 间距，半径 62） + 细颗粒（±4） + 4 个定位标记
//   标记（16x16，不透明，写在最后）：红 TL(96,96) 绿 TR(1088,96) 蓝 BL(96,788) 黄 BR(1088,788)
void gfx64_wall_build_default64() {
    const Theme64Tokens* t = theme64_tokens64();
    if (g_wallpx) { kfree_64(g_wallpx); g_wallpx = nullptr; }
    g_wallw = GFX64_DEF_W;
    g_wallh = GFX64_DEF_H;
    g_wallpx = (uint32_t*)kmalloc_64((uint64_t)g_wallw * g_wallh * 4);
    if (!g_wallpx) { g_wallw = g_wallh = 0; return; }
    const int wa_r = cc_r(t->wall_a), wa_g = cc_g(t->wall_a), wa_b = cc_b(t->wall_a);
    const int wb_r = cc_r(t->wall_b), wb_g = cc_g(t->wall_b), wb_b = cc_b(t->wall_b);
    const int bl_r = cc_r(t->wall_blob), bl_g = cc_g(t->wall_blob), bl_b = cc_b(t->wall_blob);
    const int denom = g_wallw + g_wallh;
    for (int y = 0; y < g_wallh; y++) {
        uint32_t* row = g_wallpx + (uint64_t)y * g_wallw;
        for (int x = 0; x < g_wallw; x++) {
            int tt = (x + y) * 256 / denom;
            const int it = 255 - tt;
            int r = (wa_r * it + wb_r * tt) / 255;
            int g = (wa_g * it + wb_g * tt) / 255;
            int b = (wa_b * it + wb_b * tt) / 255;
            // 柔光斑：网格中心 (80 + 160i, 80 + 160j)，半径 62，软边
            const int gx = ((x % 160) - 80), gy = ((y % 160) - 80);
            const int d2 = gx * gx + gy * gy;
            if (d2 < 62 * 62) {
                const int a = (62 * 62 - d2) * 90 / (62 * 62);   // 0..~90/255
                r = (bl_r * a + r * (255 - a)) / 255;
                g = (bl_g * a + g * (255 - a)) / 255;
                b = (bl_b * a + b * (255 - a)) / 255;
            }
            // 细颗粒（确定性哈希；±4 —— 玻璃模糊后局部方差显著下降 = 真模糊的像素级证据）
            const uint32_t hs = ((uint32_t)x * 73856093u) ^ ((uint32_t)y * 19349663u);
            const int grain = (int)((hs >> 5) & 7) - 4;
            r += grain; g += grain; b += grain;
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            row[x] = (uint32_t)((r << 16) | (g << 8) | b);
        }
    }
    struct Mark { int x, y; uint32_t c; const char* n; };
    const Mark marks[4] = {
        { GFX64_DEF_INSET,                    GFX64_DEF_INSET,                    rgb(192, 48, 48),  "r" },
        { GFX64_DEF_W - GFX64_DEF_INSET - GFX64_DEF_MARK, GFX64_DEF_INSET,         rgb(32, 160, 64),  "g" },
        { GFX64_DEF_INSET,                    GFX64_DEF_H - GFX64_DEF_INSET - GFX64_DEF_MARK, rgb(48, 64, 192), "b" },
        { GFX64_DEF_W - GFX64_DEF_INSET - GFX64_DEF_MARK, GFX64_DEF_H - GFX64_DEF_INSET - GFX64_DEF_MARK, rgb(224, 192, 48), "y" },
    };
    for (int m = 0; m < 4; m++) {
        for (int y = marks[m].y; y < marks[m].y + GFX64_DEF_MARK; y++) {
            if (y < 0 || y >= g_wallh) continue;
            uint32_t* row = g_wallpx + (uint64_t)y * g_wallw;
            for (int x = marks[m].x; x < marks[m].x + GFX64_DEF_MARK; x++) {
                if (x < 0 || x >= g_wallw) continue;
                row[x] = marks[m].c;
            }
        }
    }
    g_wall_desc = "builtin(default wallpaper)";
    dbg64_line_begin64();
    dbg64_str("[GFX64] wall markers r=");
    dbg64_dec((uint64_t)marks[0].x); dbg64_str(","); dbg64_dec((uint64_t)marks[0].y);
    dbg64_str(" g="); dbg64_dec((uint64_t)marks[1].x); dbg64_str(","); dbg64_dec((uint64_t)marks[1].y);
    dbg64_str(" b="); dbg64_dec((uint64_t)marks[2].x); dbg64_str(","); dbg64_dec((uint64_t)marks[2].y);
    dbg64_str(" y="); dbg64_dec((uint64_t)marks[3].x); dbg64_str(","); dbg64_dec((uint64_t)marks[3].y);
    dbg64_str(" size=");
    dbg64_dec((uint64_t)GFX64_DEF_MARK);
    dbg64_str(" src=");
    dbg64_dec((uint64_t)g_wallw); dbg64_str("x"); dbg64_dec((uint64_t)g_wallh);
    dbg64_nl();
    dbg64_line_end64();
}

int gfx64_wall_set_source64(const uint32_t* px, int w, int h, int copy, const char* from) {
    if (!px || w <= 0 || h <= 0) return -1;
    if (g_wallpx) { kfree_64(g_wallpx); g_wallpx = nullptr; }
    if (copy) {
        g_wallpx = (uint32_t*)kmalloc_64((uint64_t)w * h * 4);
        if (!g_wallpx) return -1;
        for (uint64_t i = 0; i < (uint64_t)w * h; i++) g_wallpx[i] = px[i] & 0x00FFFFFF;
    } else {
        g_wallpx = (uint32_t*)px;
    }
    g_wallw = w; g_wallh = h;
    g_wall_desc = from ? from : "?";
    gfx64_wall_invalidate64();
    dbg64_line_begin64();
    dbg64_str("[GFX64] wall source set ");
    dbg64_dec((uint64_t)w);
    dbg64_str("x");
    dbg64_dec((uint64_t)h);
    dbg64_str(" from=");
    dbg64_str(g_wall_desc);
    dbg64_str(" copy=");
    dbg64_dec((uint64_t)copy);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

int gfx64_wall_src_w64() { return g_wallw; }
int gfx64_wall_src_h64() { return g_wallh; }
const char* gfx64_wall_src_desc64() { return g_wall_desc; }
const char* gfx64_wall_last_geom64() { return g_wall_geom; }

void gfx64_wall_invalidate64() {
    g_surface_valid = false;
    // 内容层模糊缓存与壁纸面绑定 → 一起失效
    for (int i = 0; i < GFX64_BLUR_SLOTS; i++) {
        if (g_blur_cache[i].used && g_blur_cache[i].buf) kfree_64(g_blur_cache[i].buf);
        g_blur_cache[i] = Gfx64BlurEnt{};
    }
}

static const char* mode_name64(int m) {
    switch (m) {
        case GFX64_WALL_FILL:    return "fill";
        case GFX64_WALL_FIT:     return "fit";
        case GFX64_WALL_STRETCH: return "stretch";
        case GFX64_WALL_TILE:    return "tile";
        case GFX64_WALL_CENTER:  return "center";
        case GFX64_WALL_SPAN:    return "span";
        default:                 return "?";
    }
}
const char* gfx64_wall_mode_name64(int m) { return mode_name64(m); }
int gfx64_wall_mode64() { return g_mode; }

static void log_wall_geom64(int mode, int dx, int dy, int dw, int dh, int num, int den, int tiles_x, int tiles_y) {
    // 裁剪量（源坐标系之外的可见范围 → 上下/左右各裁掉多少）
    int crop_l = 0, crop_t = 0, crop_r = 0, crop_b = 0;
    if (dx < 0) crop_l = -dx;
    if (dy < 0) crop_t = -dy;
    if (dx + dw > fb_width()) crop_r = dx + dw - fb_width();
    if (dy + dh > fb_height()) crop_b = dy + dh - fb_height();
    // 组装打点行（同时留一份给 [GFX64] wall_last，便于验收双查）
    char* o = g_wall_geom;
    struct W {
        static void s(char*& p, const char* s) { while (*s) *p++ = *s++; }
        static void d(char*& p, int v) {
            char t[12]; int n = 0;
            if (v < 0) { *p++ = '-'; v = -v; }
            if (v == 0) t[n++] = '0';
            while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
            while (n > 0) *p++ = t[--n];
        }
    };
    W::s(o, "mode=");     W::d(o, mode);
    W::s(o, " name=");    W::s(o, mode_name64(mode));
    W::s(o, " src=");     W::d(o, g_wallw); W::s(o, "x"); W::d(o, g_wallh);
    W::s(o, " screen=");  W::d(o, fb_width()); W::s(o, "x"); W::d(o, fb_height());
    W::s(o, " dst=");     W::d(o, dx); W::s(o, ","); W::d(o, dy); W::s(o, ",");
    W::d(o, dw); W::s(o, "x"); W::d(o, dh);
    W::s(o, " scale=");   W::d(o, num); W::s(o, "/"); W::d(o, den);
    W::s(o, " crop=l");   W::d(o, crop_l); W::s(o, ",t"); W::d(o, crop_t);
    W::s(o, ",r");        W::d(o, crop_r); W::s(o, ",b"); W::d(o, crop_b);
    W::s(o, " tiles=");   W::d(o, tiles_x); W::s(o, "x"); W::d(o, tiles_y);
    *o = 0;
    if (g_wall_log_budget > 0) {
        g_wall_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[GFX64] wall ");
        dbg64_str(g_wall_geom);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// 源的定点采样（双线性，Q16.16）
static inline uint32_t src_sample64(int fx16, int fy16) {
    // fx16/fy16 = 源坐标 * 65536
    int x0i = fx16 >> 16, y0i = fy16 >> 16;
    int xf = (fx16 >> 8) & 0xFF, yf = (fy16 >> 8) & 0xFF;
    if (x0i < 0) { x0i = 0; xf = 0; }
    if (y0i < 0) { y0i = 0; yf = 0; }
    int x1i = x0i + 1, y1i = y0i + 1;
    if (x0i >= g_wallw) x0i = g_wallw - 1;
    if (y0i >= g_wallh) y0i = g_wallh - 1;
    if (x1i >= g_wallw) { x1i = g_wallw - 1; xf = 0; }
    if (y1i >= g_wallh) { y1i = g_wallh - 1; yf = 0; }
    const uint32_t c00 = g_wallpx[(uint64_t)y0i * g_wallw + x0i];
    const uint32_t c10 = g_wallpx[(uint64_t)y0i * g_wallw + x1i];
    const uint32_t c01 = g_wallpx[(uint64_t)y1i * g_wallw + x0i];
    const uint32_t c11 = g_wallpx[(uint64_t)y1i * g_wallw + x1i];
    const int i00 = (255 - xf) * (255 - yf), i10 = xf * (255 - yf);
    const int i01 = (255 - xf) * yf, i11 = xf * yf;
    const int r = (cc_r(c00) * i00 + cc_r(c10) * i10 + cc_r(c01) * i01 + cc_r(c11) * i11) / (255 * 255);
    const int g = (cc_g(c00) * i00 + cc_g(c10) * i10 + cc_g(c01) * i01 + cc_g(c11) * i11) / (255 * 255);
    const int b = (cc_b(c00) * i00 + cc_b(c10) * i10 + cc_b(c01) * i01 + cc_b(c11) * i11) / (255 * 255);
    return (uint32_t)((r << 16) | (g << 8) | b);
}

static void wall_compose64() {
    const int W = fb_width(), H = fb_height();
    if (!g_wallpx || g_wallw <= 0 || g_wallh <= 0 || W <= 0 || H <= 0) return;
    if (g_sw != W || g_sh != H) {
        if (g_surface) { kfree_64(g_surface); g_surface = nullptr; }
        if (g_surface_blur) { kfree_64(g_surface_blur); g_surface_blur = nullptr; }
        g_sw = W; g_sh = H;
        g_surface = (uint32_t*)kmalloc_64((uint64_t)W * H * 4);
        g_surface_blur = (uint32_t*)kmalloc_64((uint64_t)W * H * 4);
    }
    if (!g_surface) return;
    const Theme64Tokens* t = theme64_tokens64();
    const uint64_t t0 = ticks64();
    int dx = 0, dy = 0, dw = W, dh = H, num = 1000, den = 1000, tiles_x = 1, tiles_y = 1;
    const int mode = g_mode;
    if (mode == GFX64_WALL_TILE) {
        dw = g_wallw; dh = g_wallh; dx = 0; dy = 0;
        tiles_x = (W + g_wallw - 1) / g_wallw;
        tiles_y = (H + g_wallh - 1) / g_wallh;
        num = 1000; den = 1000;
    } else if (mode == GFX64_WALL_CENTER) {
        dw = g_wallw; dh = g_wallh;
        dx = (W - dw) / 2; dy = (H - dh) / 2;
        num = 1000; den = 1000;
    } else if (mode == GFX64_WALL_STRETCH) {
        dw = W; dh = H; dx = 0; dy = 0;
    } else {
        // fill / fit / span：等比缩放
        int n1 = W * 1000 / g_wallw, n2 = H * 1000 / g_wallh;
        const bool fit = (mode == GFX64_WALL_FIT);
        num = fit ? (n1 < n2 ? n1 : n2) : (n1 > n2 ? n1 : n2);
        den = 1000;
        // 填充/跨屏必须**铺满**：向上取整（否则最右/最下会留 1px 底色缝）；适应则向下取整不变形
        dw = fit ? (g_wallw * num / 1000) : ((g_wallw * num + 999) / 1000);
        dh = fit ? (g_wallh * num / 1000) : ((g_wallh * num + 999) / 1000);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        dx = (W - dw) / 2;
        dy = (H - dh) / 2;
    }
    // 1) 底色（留白）
    for (uint64_t i = 0; i < (uint64_t)W * H; i++) g_surface[i] = t->desktop_base;
    // 2) 铺图
    const bool tile = (mode == GFX64_WALL_TILE);
    const bool stretch = (mode == GFX64_WALL_STRETCH);
    for (int y = 0; y < H; y++) {
        // ★ 平铺模式：图块会**重复**到 dst 之外，所以不能按 dst 裁剪（否则右边/下边留底色）
        if (!tile && (y < dy || y >= dy + dh)) continue;
        uint32_t* row = g_surface + (uint64_t)y * W;
        for (int x = 0; x < W; x++) {
            if (!tile && (x < dx || x >= dx + dw)) continue;
            int sx16, sy16;
            if (tile) {
                int sx = (x - dx) % g_wallw; if (sx < 0) sx += g_wallw;
                int sy = (y - dy) % g_wallh; if (sy < 0) sy += g_wallh;
                sx16 = sx << 16; sy16 = sy << 16;
            } else if (stretch) {
                sx16 = (int)(((int64_t)(x - dx) * g_wallw << 16) / dw);
                sy16 = (int)(((int64_t)(y - dy) * g_wallh << 16) / dh);
            } else {
                sx16 = (int)(((int64_t)(x - dx) * g_wallw << 16) / dw);
                sy16 = (int)(((int64_t)(y - dy) * g_wallh << 16) / dh);
            }
            row[x] = src_sample64(sx16, sy16);
        }
    }
    const uint64_t t1 = ticks64();
    log_wall_geom64(mode, dx, dy, dw, dh, num, den, tiles_x, tiles_y);
    // 3) 墙纸模糊：**只算一次**（背景层玻璃 = r=Token 24）
    if (g_surface_blur) {
        for (uint64_t i = 0; i < (uint64_t)W * H; i++) g_surface_blur[i] = g_surface[i];
        box_blur_buf64(g_surface_blur, W, H, THEME64_BLUR_BACKDROP);
        dbg64_line_begin64();
        dbg64_str("[GFX64] wall blur once r=");
        dbg64_dec((uint64_t)THEME64_BLUR_BACKDROP);
        dbg64_str(" src=");
        dbg64_dec((uint64_t)W); dbg64_str("x"); dbg64_dec((uint64_t)H);
        dbg64_str(" compose_ticks=");
        dbg64_dec((uint64_t)(t1 - t0));
        dbg64_str(" ticks=");
        dbg64_dec((uint64_t)(ticks64() - t0));
        dbg64_str(" builds=");
        dbg64_dec(++g_wall_build_count);
        dbg64_nl();
        dbg64_line_end64();
        // 真模糊证据：同一块区域（Dock 背后）在原图与模糊图上的局部方差
        const int vx = (W - 600) / 2, vy = H - THEME64_DOCK_MARGIN - THEME64_DOCK_H;
        const uint64_t vs = gfx64_var64(g_surface, W, vx, vy, 32, 32);
        const uint64_t vd = gfx64_var64(g_surface_blur, W, vx, vy, 32, 32);
        dbg64_line_begin64();
        dbg64_str("[GFX64] blur tile x=");
        dbg64_dec((uint64_t)vx);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)vy);
        dbg64_str(" wh=32x32 var_src=");
        dbg64_dec(vs);
        dbg64_str(" var_dst=");
        dbg64_dec(vd);
        dbg64_nl();
        dbg64_line_end64();
    }
    g_surface_valid = true;
    g_surface_theme = theme64_id64();
    g_surface_mode = g_mode;
    g_surface_src = g_wallpx;
}

static void wall_ensure64() {
    if (!g_wallpx) gfx64_wall_build_default64();
    if (!g_surface_valid || g_surface_theme != theme64_id64() || g_surface_mode != g_mode ||
        g_surface_src != g_wallpx || g_sw != fb_width() || g_sh != fb_height()) {
        panic64_watchdog_pause64();
        // ★ 整屏壁纸 + 模糊面重建在 QEMU TCG 下单次要几秒（切主题/切适应模式都会走这里），
        //   而 GUI 帧心跳的看门狗阈值只有 5s —— 这是**正常的长操作**，不是卡死。
        //   照 kernel/terminal64.cpp 对"大文件 I/O 长操作"的既有做法：暂停看门狗，做完再恢复
        //   （pause/unpause 都会刷新 kick 时间）。不这么做时，主题切换在 TCG 下会偶发
        //   [WD64] watchdog fire stale≈5.1s -> [PANIC64] WATCHDOG_TIMEOUT（本批实测踩到）。
        // 主题变了 → 内置壁纸要按新主题重生成（文件壁纸不受主题影响）
        if (g_wall_desc && g_wall_desc[0] == 'b' && g_surface_theme != theme64_id64()) {
            gfx64_wall_build_default64();
            g_wall_log_budget = 12;
        }
        wall_compose64();
        panic64_watchdog_unpause64();
    }
}

const uint32_t* gfx64_wall_surface64(int* w, int* h) {
    wall_ensure64();
    if (w) *w = g_sw;
    if (h) *h = g_sh;
    return g_surface_valid ? g_surface : nullptr;
}
const uint32_t* gfx64_wall_blur64(int* w, int* h) {
    wall_ensure64();
    if (w) *w = g_sw;
    if (h) *h = g_sh;
    return g_surface_valid ? g_surface_blur : nullptr;
}

void gfx64_wall_draw64(int x, int y, int w, int h) {
    wall_ensure64();
    if (!g_surface_valid) return;
    int x0 = x, y0 = y, ww = w, hh = h;
    if (!rect_clip64(&x0, &y0, &ww, &hh)) return;
    uint32_t* px = canvas64();
    for (int yy = y0; yy < y0 + hh; yy++) {
        const uint32_t* src = g_surface + (uint64_t)yy * g_sw + x0;
        uint32_t* dst = px + (uint64_t)yy * g_fw + x0;
        for (int i = 0; i < ww; i++) dst[i] = src[i];
    }
}

void gfx64_wall_set_mode64(int mode, const char* why) {
    if (mode < 0 || mode >= GFX64_WALL_MODE_COUNT) return;
    g_mode = mode;
    g_mode_cfg = mode;
    cfg64_set_wall_mode64(mode);
    gfx64_wall_invalidate64();
    g_wall_log_budget = 12;
    dbg64_line_begin64();
    dbg64_str("[GFX64] switch wall mode=");
    dbg64_dec((uint64_t)mode);
    dbg64_str(" name=");
    dbg64_str(mode_name64(mode));
    dbg64_str(" why=");
    dbg64_str(why ? why : "?");
    dbg64_str(" persisted=cfg64.ui.wall.mode");
    dbg64_nl();
    dbg64_line_end64();
}

int gfx64_wall_tick64() {
    if (g_mode_cfg < 0) {
        const int m = cfg64_wall_mode64();
        if (m != g_mode) {
            g_mode = m;
            g_mode_cfg = m;
            gfx64_wall_invalidate64();
            dbg64_line_begin64();
            dbg64_str("[GFX64] switch wall mode=");
            dbg64_dec((uint64_t)m);
            dbg64_str(" name=");
            dbg64_str(mode_name64(m));
            dbg64_str(" why=cfg hotkey/db");
            dbg64_nl();
            dbg64_line_end64();
            return 1;
        }
        return 0;
    }
    const int m = cfg64_wall_mode64();
    if (m != g_mode_cfg && m >= 0 && m < GFX64_WALL_MODE_COUNT) {
        g_mode = m; g_mode_cfg = m;
        gfx64_wall_invalidate64();
        dbg64_line_begin64();
        dbg64_str("[GFX64] switch wall mode=");
        dbg64_dec((uint64_t)m);
        dbg64_str(" name=");
        dbg64_str(mode_name64(m));
        dbg64_str(" why=cfg");
        dbg64_nl();
        dbg64_line_end64();
        return 1;
    }
    return 0;
}

// ==================== 度量 // 圆角裁切：方形客户区的窗口要用它把 4 个"圆角外"的像素补回 backdrop（layer 0 = 预模糊壁纸）采样。
// 只在 cov < 255 的像素上动手：内部像素与 1px 边框原样保留。
void gfx64_corner_cut64(int x, int y, int w, int h, int r, int layer) {
    if (r <= 0 || w <= 2 * r || h <= 2 * r) return;
    const uint8_t* tab = cov_table64(r);
    if (!tab) return;
    int bw = 0, bh = 0;
    const uint32_t* back = gfx64_wall_blur64(&bw, &bh);
    if (!back) return;
    (void)layer;
    uint32_t* px = canvas64();
    int cx, cy, cw, ch;
    clip_get64(&cx, &cy, &cw, &ch);
    for (int c = 0; c < 4; c++) {
        const int ox = (c & 1) ? (x + w - r) : x;
        const int oy = (c & 2) ? (y + h - r) : y;
        for (int j = 0; j < r; j++) {
            const int sy = oy + j;
            if (sy < cy || sy >= cy + ch) continue;
            uint32_t* row = px + (uint64_t)sy * g_fw;
            for (int i = 0; i < r; i++) {
                const int sx = ox + i;
                if (sx < cx || sx >= cx + cw) continue;
                int cov;
                if (c == 0)      cov = tab[j * r + i];
                else if (c == 1) cov = tab[j * r + (r - 1 - i)];
                else if (c == 2) cov = tab[(r - 1 - j) * r + i];
                else             cov = tab[(r - 1 - j) * r + (r - 1 - i)];
                if (cov >= 255) continue;
                const int covr = 255 - cov;
                uint32_t out = row[sx];
                if (sx < bw && sy < bh) {
                    const uint32_t src = back[(uint64_t)sy * bw + sx];
                    if (cov <= 0) out = src;
                    else out = blend_c64(out, src, covr);
                } else if (cov <= 0) {
                    out = theme64_tokens64()->desktop_base;
                }
                row[sx] = out;
            }
        }
    }
}

// ==================== 度量 / 报告 ====================
uint64_t gfx64_var64(const uint32_t* px, int stride, int x, int y, int w, int h) {
    if (!px) return 0;
    int64_t sum = 0, sum2 = 0;
    int n = 0;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) {
            const int g = cc_g(px[(uint64_t)(y + j) * stride + (x + i)]);
            sum += g;
            sum2 += (int64_t)g * g;
            n++;
        }
    }
    if (n <= 0) return 0;
    const int64_t mean = sum / n;
    int64_t var = sum2 / n - mean * mean;
    if (var < 0) var = 0;
    return (uint64_t)(var * 100);
}

void gfx64_report64(const char* tag) {
    dbg64_line_begin64();
    dbg64_str("[GFX64] cache");
    if (tag && tag[0]) { dbg64_str(" tag="); dbg64_str(tag); }
    dbg64_str(" blur_hit=");
    dbg64_dec((uint64_t)g_blur_hit);
    dbg64_str(" blur_miss=");
    dbg64_dec((uint64_t)g_blur_miss);
    dbg64_str(" shadow_hit=");
    dbg64_dec((uint64_t)g_sh_hit);
    dbg64_str(" shadow_miss=");
    dbg64_dec((uint64_t)g_sh_miss);
    dbg64_str(" wall_builds=");
    dbg64_dec(g_wall_build_count);
    dbg64_str(" wall_mode=");
    dbg64_str(mode_name64(g_mode));
    dbg64_str(" wall_src=");
    dbg64_str(g_wall_desc ? g_wall_desc : "?");
    dbg64_str(" surface=");
    dbg64_dec((uint64_t)fb_width());
    dbg64_str("x");
    dbg64_dec((uint64_t)fb_height());
    dbg64_nl();
    dbg64_line_end64();
}
