/* user/browser/vimtu_font.c - 字形表 + 度量（见 vimtu_font.h 的"为什么"）
 *
 * 数据来自 user/wm/wm_ttf.h（构建期预光栅化的 ASCII 16px em 字模）。
 * 这里只做三件事：查表、把 pt 换算成整数倍率、解 UTF-8。
 * 不在这里画像素：绘制统一走 vimtu_canvas_text()，保证"量"和"画"用的是同一份度量。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* netsurf/plot_style.h 用了 assert() 与 libwapcaplet，但不自带 assert.h */
#include <assert.h>

#include "utils/errors.h"
#include "netsurf/plot_style.h"
#include "vimtu_font.h"

/* 字模数据（static const，只在本 TU 里用）。相对路径而不是改 -I：构建脚本的 INCS 不该为
 * 一个数据文件再加一条 include 路径（判据是硬要求，加路径属于改判据的邻域，能不加就不加）。 */
#include "../wm/wm_ttf.h"

/* 字模覆盖 ASCII 32..126；其余字符返回 false，由调用方画缺字块 */
bool vimtu_font_glyph(uint32_t ucs4, struct vimtu_glyph *out)
{
    const struct WmTfGlyph *g;

    if (out == NULL) {
        return false;
    }
    if (ucs4 < (uint32_t)WM_TF_FIRST || ucs4 > (uint32_t)WM_TF_LAST) {
        return false;
    }

    g = &g_wm_tf[ucs4 - (uint32_t)WM_TF_FIRST];

    out->w = (int)g->w;
    out->h = (int)g->h;
    out->bx = (int)g->bx;
    out->by = (int)g->by;
    out->adv = (g->adv != 0) ? (int)g->adv : VIMTU_FONT_ADV;
    /* w/h == 0（空格）时 off 是"下一个字形的起点"，解引用它也不算错，但没必要 */
    out->alpha = (g->w != 0 && g->h != 0) ? &g_wm_tf_alpha[(int)g->off] : NULL;

    return true;
}

/* 字号 -> 整数倍率。
 *   fstyle->size 是 22:10 定点 pt（PLOT_STYLE_SCALE = 1024）。
 *   px = pt * 96 / 72 = pt * 4 / 3。
 * 默认配置下 font_size = 128（= 12.8pt）-> px = 17 -> scale 1（16px 字模），
 * h1（2em = 25.6pt）-> px = 34 -> scale 2。倍率封顶 3，避免超大标题把字模糊成马赛克。 */
int vimtu_font_scale(const struct plot_font_style *fstyle)
{
    int px;
    int scale;

    if (fstyle == NULL) {
        return 1;
    }

    px = (int)(((int64_t)fstyle->size * 4) / ((int64_t)PLOT_STYLE_SCALE * 3));
    if (px < VIMTU_FONT_EM) {
        return 1;                       /* 小字号也至少给一份完整字模，不做亚像素缩放 */
    }

    scale = (px + (VIMTU_FONT_EM / 2)) / VIMTU_FONT_EM;
    if (scale < 1) {
        scale = 1;
    }
    if (scale > 3) {
        scale = 3;
    }
    return scale;
}

int vimtu_font_advance(const struct plot_font_style *fstyle)
{
    return VIMTU_FONT_ADV * vimtu_font_scale(fstyle);
}

int vimtu_font_line_height(const struct plot_font_style *fstyle)
{
    return VIMTU_FONT_EM * vimtu_font_scale(fstyle);
}

/* 解一个 UTF-8 字符。
 * 为什么自己写 20 行而不是用 utils/utf8.h：那一层的 utf8_next() 对"截断/非法序列"的
 * 前进量有它自己的口径（可能一次吞掉多字节），而这里必须保证 **len 内一定前进** ——
 * 页面里出现被 layout 切断的半截 UTF-8 是常态（文本按字节数切给 plotter），
 * 一旦不前进就是死循环。 */
size_t vimtu_font_utf8_next(const char *s, size_t len, size_t off, uint32_t *ucs4)
{
    const unsigned char *p = (const unsigned char *)s;
    unsigned char c;
    uint32_t val;
    size_t need;
    size_t i;

    if (ucs4 != NULL) {
        *ucs4 = 0xFFFD;
    }
    if (s == NULL || off >= len) {
        return len;
    }

    c = p[off];
    if (c < 0x80) {
        if (ucs4 != NULL) {
            *ucs4 = (uint32_t)c;
        }
        return off + 1;
    }

    if ((c & 0xE0) == 0xC0) {
        need = 1;
        val = (uint32_t)(c & 0x1Fu);
    } else if ((c & 0xF0) == 0xE0) {
        need = 2;
        val = (uint32_t)(c & 0x0Fu);
    } else if ((c & 0xF8) == 0xF0) {
        need = 3;
        val = (uint32_t)(c & 0x07u);
    } else {
        return off + 1;                 /* 非法首字节：按 1 字节前进 */
    }

    /* 序列必须完整落在 [off, len) 里；被 len 边界切开的按 1 字节前进，绝不越界读 */
    if (off + need >= len) {
        return off + 1;
    }


    for (i = 1; i <= need; i++) {
        if ((p[off + i] & 0xC0) != 0x80) {
            return off + 1;             /* 续字节不合法 */
        }
        val = (val << 6) | (uint32_t)(p[off + i] & 0x3F);
    }

    if (ucs4 != NULL) {
        *ucs4 = val;
    }
    return off + need + 1;
}
