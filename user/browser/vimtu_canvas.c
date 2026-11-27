/* user/browser/vimtu_canvas.c - 画布实现（像素写入 + 裁剪 + 上屏）
 *
 * 为什么像素操作全自己写、不借 NetSurf 的工具函数：NetSurf 的 plotter 契约是
 * "一切都用 ctx->plot 里的函数画"，像素格式只有前端知道。这里就是那个"知道格式"的地方：
 *   * 内部一律 XRGB8888：一个 uint32 的 0x00RRGGBB（小端字节序 B,G,R,X）。写像素时
 *     额外置 0xFF000000 —— 合成器目前是直接 memcpy 不混合，置 alpha 只是让"以后有人
 *     加混合"时不至于按 alpha=0 把整面当透明的。
 *   * 所有写入都过 clip：NetSurf 会给我们越界的矩形（负宽、超大宽高都见过），
 *     裁剪放在最底层一次做掉，上层（plotters）就不用重复判边界。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "netsurf/plot_style.h"

#include "vimtu_abi.h"
#include "vimtu_canvas.h"
#include "vimtu_font.h"

/* 当前活动画布（见 vimtu_canvas_active 的说明） */
static struct vimtu_canvas *g_active;

struct vimtu_canvas *vimtu_canvas_active(void)
{
    return g_active;
}

int vimtu_canvas_open(struct vimtu_canvas *c, int w, int h)
{
    unsigned bytes;
    void *va = NULL;
    int surf;

    if (c == NULL) {
        return -1;
    }
    memset(c, 0, sizeof(*c));
    c->surf = -1;
    c->shm = -1;

    if (w <= 0 || h <= 0) {
        return -2;
    }
    /* 内核硬上限：超了 surface_create 会 ENOSPC，而且现场表现是"窗口在、内容全黑" */
    if ((unsigned)w * (unsigned)h > VIMTU_MAX_PIXELS) {
        return -3;
    }
    if ((unsigned)w * (unsigned)h > V64_SHM_MAX_BYTES / 4u) {
        return -3;
    }

    bytes = (unsigned)w * (unsigned)h * 4u;

    c->shm = v64_shm_create(bytes);
    if (c->shm < 0) {
        return -4;
    }
    if (v64_shm_map(c->shm, 0, bytes, &va) < 0 || va == NULL) {
        return -5;
    }
    c->px = (uint32_t *)va;
    c->w = w;
    c->h = h;

    surf = v64_wl_surface_create(w, h, V64_WL_FORMAT_XRGB8888);
    if (surf < 0) {
        return -6;
    }
    if (v64_wl_surface_attach(surf, c->shm, 0) < 0) {
        (void)v64_wl_surface_destroy(surf);
        return -7;
    }
    c->surf = surf;

    /* 先整面清成白底：attach 之后没 commit 过，屏幕上看不到，但至少不会把 shm 里的垃圾
     * 当成"已经画过一帧" */
    memset(c->px, 0xFF, (size_t)bytes);

    vimtu_canvas_clip(c, 0, 0, w, h);
    g_active = c;
    return 0;
}

void vimtu_canvas_close(struct vimtu_canvas *c)
{
    if (c == NULL) {
        return;
    }
    if (c->surf >= 0) {
        (void)v64_wl_surface_destroy(c->surf);
    }
    c->surf = -1;
    c->shm = -1;
    c->px = NULL;
    if (g_active == c) {
        g_active = NULL;
    }
}

void vimtu_canvas_clip(struct vimtu_canvas *c, int x0, int y0, int x1, int y1)
{
    if (c == NULL) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if (y0 < 0) {
        y0 = 0;
    }
    if (x1 > c->w) {
        x1 = c->w;
    }
    if (y1 > c->h) {
        y1 = c->h;
    }
    if (x1 < x0) {
        x1 = x0;
    }
    if (y1 < y0) {
        y1 = y0;
    }
    c->clip_x0 = x0;
    c->clip_y0 = y0;
    c->clip_x1 = x1;
    c->clip_y1 = y1;
}

static inline void put_px(struct vimtu_canvas *c, int x, int y, uint32_t v)
{
    if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1) {
        return;
    }
    c->px[(size_t)y * (size_t)c->w + (size_t)x] = v | 0xFF000000u;
}

/* 把 colour 0x00RRGGBB 按 alpha 混到目标像素上（alpha: 0..255） */
static inline void blend_px(struct vimtu_canvas *c, int x, int y, uint32_t colour, unsigned a)
{
    uint32_t d;
    unsigned ia;
    unsigned r;
    unsigned g;
    unsigned b;

    if (a == 0) {
        return;
    }
    if (x < c->clip_x0 || x >= c->clip_x1 || y < c->clip_y0 || y >= c->clip_y1) {
        return;
    }
    d = c->px[(size_t)y * (size_t)c->w + (size_t)x];
    if (a >= 255) {
        c->px[(size_t)y * (size_t)c->w + (size_t)x] = 0xFF000000u | (colour & 0xFFFFFFu);
        return;
    }
    ia = 255u - a;
    r = ((((colour >> 16) & 0xFFu) * a) + (((d >> 16) & 0xFFu) * ia) + 127u) / 255u;
    g = ((((colour >> 8) & 0xFFu) * a) + (((d >> 8) & 0xFFu) * ia) + 127u) / 255u;
    b = (((colour & 0xFFu) * a) + ((d & 0xFFu) * ia) + 127u) / 255u;
    c->px[(size_t)y * (size_t)c->w + (size_t)x] = 0xFF000000u | (r << 16) | (g << 8) | b;
}

void vimtu_canvas_fill(struct vimtu_canvas *c, int x, int y, int w, int h, uint32_t colour)
{
    int x0, y0, x1, y1, yy, xx;
    uint32_t v = 0xFF000000u | (colour & 0xFFFFFFu);

    if (c == NULL || c->px == NULL || w <= 0 || h <= 0) {
        return;
    }
    x0 = x;
    y0 = y;
    x1 = x + w;
    y1 = y + h;
    if (x0 < c->clip_x0) {
        x0 = c->clip_x0;
    }
    if (y0 < c->clip_y0) {
        y0 = c->clip_y0;
    }
    if (x1 > c->clip_x1) {
        x1 = c->clip_x1;
    }
    if (y1 > c->clip_y1) {
        y1 = c->clip_y1;
    }
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = c->px + (size_t)yy * (size_t)c->w;
        for (xx = x0; xx < x1; xx++) {
            row[xx] = v;
        }
    }
}

void vimtu_canvas_frame(struct vimtu_canvas *c, int x, int y, int w, int h, int bw, uint32_t colour)
{
    if (bw <= 0 || w <= 0 || h <= 0) {
        return;
    }
    vimtu_canvas_fill(c, x, y, w, bw, colour);                    /* 上 */
    vimtu_canvas_fill(c, x, y + h - bw, w, bw, colour);           /* 下 */
    vimtu_canvas_fill(c, x, y + bw, bw, h - 2 * bw, colour);      /* 左 */
    vimtu_canvas_fill(c, x + w - bw, y + bw, bw, h - 2 * bw, colour); /* 右 */
}

void vimtu_canvas_blit(struct vimtu_canvas *c, int x, int y, int w, int h,
                       const uint32_t *src, int src_w, int src_h, bool opaque)
{
    int dy;

    if (c == NULL || c->px == NULL || src == NULL) {
        return;
    }
    if (w <= 0 || h <= 0 || src_w <= 0 || src_h <= 0) {
        return;
    }

    for (dy = 0; dy < h; dy++) {
        int py = y + dy;
        int sy;
        const uint32_t *srow;

        if (py < c->clip_y0 || py >= c->clip_y1) {
            continue;
        }
        sy = (int)(((int64_t)dy * src_h) / h);
        if (sy >= src_h) {
            sy = src_h - 1;
        }
        srow = src + (size_t)sy * (size_t)src_w;

        for (int dx = 0; dx < w; dx++) {
            int px = x + dx;
            int sx;
            uint32_t s;

            if (px < c->clip_x0 || px >= c->clip_x1) {
                continue;
            }
            sx = (int)(((int64_t)dx * src_w) / w);
            if (sx >= src_w) {
                sx = src_w - 1;
            }
            s = srow[sx];

            if (opaque) {
                put_px(c, px, py, s & 0xFFFFFFu);
            } else {
                blend_px(c, px, py, s & 0xFFFFFFu, (s >> 24) & 0xFFu);
            }
        }
    }
}

/* 画一个字形：alpha 是 4bit/px、2 像素一个字节、高半字节在左（与 user/wm/wm.c 同一口径）。
 * 按整数倍率放大：目的像素 (dgy,dgx) 取源 (dgy/scale, dgx/scale)，不插值 ——
 * 16px 字模放大后仍然是"字"，比双线性糊成一团更可读。 */
static void draw_glyph(struct vimtu_canvas *c, int pen_x, int baseline,
                       const struct vimtu_glyph *g, int scale, uint32_t fg)
{
    int top = baseline - VIMTU_FONT_BASELINE * scale + g->by * scale;
    int left = pen_x + g->bx * scale;
    int rowb = (g->w + 1) / 2;
    int dgy;
    int dgx;

    for (dgy = 0; dgy < g->h * scale; dgy++) {
        int gy = dgy / scale;
        const unsigned char *srow = g->alpha + (size_t)gy * (size_t)rowb;

        for (dgx = 0; dgx < g->w * scale; dgx++) {
            int gx = dgx / scale;
            unsigned char byte = srow[gx >> 1];
            unsigned a = (gx & 1) ? (unsigned)(byte & 0x0Fu) : (unsigned)(byte >> 4);

            if (a == 0) {
                continue;
            }
            /* 4bit -> 8bit：a * 17（0xF -> 255），否则抗锯齿会显得过淡 */
            blend_px(c, left + dgx, top + dgy, fg, a * 17u);
        }
    }
}

void vimtu_canvas_text(struct vimtu_canvas *c, const struct plot_font_style *fstyle,
                       int x, int baseline, const char *utf8, size_t len)
{
    int scale;
    uint32_t fg;
    int pen = x;
    size_t off = 0;

    if (c == NULL || c->px == NULL || utf8 == NULL || len == 0) {
        return;
    }
    scale = vimtu_font_scale(fstyle);
    fg = (fstyle != NULL) ? (fstyle->foreground & 0xFFFFFFu) : 0u;

    while (off < len) {
        uint32_t ucs4 = 0;
        size_t nxt;
        struct vimtu_glyph g;

        nxt = vimtu_font_utf8_next(utf8, len, off, &ucs4);
        if (nxt <= off) {
            break;                       /* 兜底：任何情况下都不许原地打转 */
        }

        if (vimtu_font_glyph(ucs4, &g)) {
            if (g.alpha != NULL && ucs4 != (uint32_t)' ') {
                draw_glyph(c, pen, baseline, &g, scale, fg);
            }
            /* ★ v0.12.6：推进量必须与 layout->width 完全一致（同一个函数），否则
             *   "量出来 800px、画出来 700px"，行内后缀与后续片段就会错位/重叠。
             *   原来这里用字模自带的 g.adv（8），而 layout 用 VIMTU_FONT_ADV —— 两个真源。 */
            pen += vimtu_font_advance(fstyle);
        } else {
            /* 缺字（非 ASCII）：画一个下划线状的替代块。为什么不画"豆腐块"：
             * 中文页面满屏豆腐块会盖住版面判断，只留下一条短横更诚实。 */
            vimtu_canvas_fill(c, pen + scale, baseline - 2 * scale, 6 * scale, scale, fg);
            pen += vimtu_font_advance(fstyle);
        }
        off = nxt;
    }
}

void vimtu_canvas_flush(struct vimtu_canvas *c, int x, int y, int w, int h)
{
    if (c == NULL || c->surf < 0) {
        return;
    }
    (void)v64_wl_surface_damage(c->surf, x, y, w, h);
    (void)v64_wl_surface_commit(c->surf);
    /* commit 之后必须 dispatch 一次才真上屏（kernel/wl64.h 的语义要点） */
    (void)v64_wl_dispatch(0, NULL, 0);
}

unsigned vimtu_canvas_count_ink(const struct vimtu_canvas *c, int x, int y, int w, int h,
                                uint32_t background)
{
    unsigned ink = 0;
    int yy;
    uint32_t bg = background & 0xFFFFFFu;

    if (c == NULL || c->px == NULL) {
        return 0;
    }
    for (yy = y; yy < y + h; yy++) {
        int xx;
        if (yy < 0 || yy >= c->h) {
            continue;
        }
        for (xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= c->w) {
                continue;
            }
            if ((c->px[(size_t)yy * (size_t)c->w + (size_t)xx] & 0xFFFFFFu) != bg) {
                ink++;
            }
        }
    }
    return ink;
}
