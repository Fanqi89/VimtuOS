/* user/browser/vimtu_layout.c - gui_layout_table（文本度量）
 *
 * 为什么这张表这么"贫":  它是 NetSurf 在排版阶段唯一能问前端"这段字有多宽"的入口。
 * 本前端是单宽位图字体（每字符 8*scale 像素），所以三个成员都是同一个循环的不同问法：
 *   width    -> 总宽
 *   position -> x 落在第几个**字节**偏移上（注意是字节偏移，不是字符序号！）
 *   split    -> 放得下的最长前缀在哪里断
 * 三者必须共用 vimtu_font_advance()，否则"量出来 100px、画出来 96px"，行尾就会错位。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* utils/errors.h 必须先来：netsurf 系列头 里用了 nserror 却不自带这个头 */
#include "utils/errors.h"
#include "netsurf/layout.h"
#include "netsurf/plot_style.h"

#include "vimtu_font.h"

static nserror vimtu_layout_width(const struct plot_font_style *fstyle, const char *string,
                                  size_t length, int *width)
{
    int adv;
    int total = 0;
    size_t off = 0;

    if (width == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    if (string == NULL || length == 0) {
        *width = 0;
        return NSERROR_OK;
    }

    adv = vimtu_font_advance(fstyle);
    while (off < length) {
        size_t nxt = vimtu_font_utf8_next(string, length, off, NULL);
        if (nxt <= off) {
            break;
        }
        total += adv;
        off = nxt;
    }
    *width = total;
    return NSERROR_OK;
}

static nserror vimtu_layout_position(const struct plot_font_style *fstyle, const char *string,
                                     size_t length, int x, size_t *char_offset, int *actual_x)
{
    int adv;
    int pen = 0;
    size_t off = 0;

    if (char_offset == NULL || actual_x == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    *char_offset = 0;
    *actual_x = 0;
    if (string == NULL || length == 0) {
        return NSERROR_OK;
    }

    adv = vimtu_font_advance(fstyle);
    while (off < length) {
        size_t nxt = vimtu_font_utf8_next(string, length, off, NULL);
        if (nxt <= off) {
            break;
        }
        /* 落在 [pen, pen+adv) 里就选这个字符：取"最近的字符左边界"（NetSurf 的惯例） */
        if (x < pen + adv / 2) {
            *char_offset = off;
            *actual_x = pen;
            return NSERROR_OK;
        }
        pen += adv;
        off = nxt;
    }
    *char_offset = length;
    *actual_x = pen;
    return NSERROR_OK;
}

static nserror vimtu_layout_split(const struct plot_font_style *fstyle, const char *string,
                                  size_t length, int x, size_t *char_offset, int *actual_x)
{
    int adv;
    int pen = 0;
    size_t off = 0;
    size_t last = 1;              /* 契约：绝不能返回 0 */

    if (char_offset == NULL || actual_x == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    *char_offset = length;
    *actual_x = 0;
    if (string == NULL || length == 0) {
        *char_offset = 0;
        return NSERROR_OK;
    }

    adv = vimtu_font_advance(fstyle);
    while (off < length) {
        size_t nxt = vimtu_font_utf8_next(string, length, off, NULL);
        if (nxt <= off) {
            break;
        }
        if (pen + adv > x) {
            /* 再放一个字符就超宽：断在这里 */
            *char_offset = (off == 0) ? last : off;
            *actual_x = pen;
            return NSERROR_OK;
        }
        pen += adv;
        off = nxt;
        last = off;
    }

    *char_offset = length;
    *actual_x = pen;
    return NSERROR_OK;
}

static struct gui_layout_table vimtu_layout_table = {
    .width = vimtu_layout_width,
    .position = vimtu_layout_position,
    .split = vimtu_layout_split,
};

struct gui_layout_table *vimtu_get_layout_table(void)
{
    return &vimtu_layout_table;
}
