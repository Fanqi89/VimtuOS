/* user/browser/vimtu_plot.c - 全局 plotter 表（NetSurf 的"一支笔"）
 *
 * 为什么是"全局符号 const struct plotter_table plot"：
 *   NetSurf 的核心不引用任何全局笔，它用 struct redraw_context.plot 传表（见
 *   include/netsurf/plotters.h）。但前端惯例是导出一个总表叫 plot，redraw_context 里
 *   `.plot = &plot` 指过去 —— 保留这个名字，是为了让"读过大前端源码的人"一眼认出来，
 *   也让 vimtu_main.c 里起手就写 &plot。
 *
 * 为什么只做"纯色矩形 + 文字 + 位图"这几件：
 *   本前端的画布是 XRGB8888 的软件缓冲，没有任何加速；圆角/阴影/渐变都靠上层（CSS 已经
 *   把大部分效果降成矩形/纯色）。于是：
 *     * rectangle 负责填充 + 边框（CSS 的 background / border 全走这里）；
 *     * polygon 负责斜边框（CSS 三角形的边框靠它）；
 *     * line / disc 给表单控件、进度条一类的简单装饰；
 *     * arc / path 返回 OK 但什么都不画 —— SVG 路径与圆弧装饰在这里是"不支持"，不是"忘了"。
 *
 * ★ 坐标口径（最容易被搞错的地方）：NetSurf 传给 plotter 的坐标是**画布坐标**，
 *   因为 browser_window_redraw(bw, x, y, clip, ctx) 里的 (x,y) 就是"文档原点在画布里的位置"。
 *   所以这里不做任何平移/滚动换算，滚动由调用方（vimtu_table.c 的 paint）通过 x/y 参数表达。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* utils/errors.h 必须先来：netsurf 系列头 里用了 nserror 却不自带这个头 */
#include "utils/errors.h"
#include "netsurf/plot_style.h"
#include "netsurf/plotters.h"

#include "vimtu_bitmap.h"
#include "vimtu_canvas.h"

/* ctx->priv 放画布指针（我们自己创建 redraw_context 时总是这么填）；
 * 核心内部另起的 redraw_context（缩略图渲染）priv 是空的，退回到活动画布。 */
static struct vimtu_canvas *cv_of(const struct redraw_context *ctx)
{
    if (ctx != NULL && ctx->priv != NULL) {
        return (struct vimtu_canvas *)ctx->priv;
    }
    return vimtu_canvas_active();
}

static int stroke_width(const plot_style_t *pstyle)
{
    int w = 1;

    if (pstyle != NULL) {
        w = plot_style_fixed_to_int(pstyle->stroke_width);
    }
    return (w < 1) ? 1 : w;
}

static nserror vimtu_plot_clip(const struct redraw_context *ctx, const struct rect *clip)
{
    struct vimtu_canvas *c = cv_of(ctx);

    if (c == NULL) {
        return NSERROR_OK;
    }
    if (clip == NULL) {
        vimtu_canvas_clip(c, 0, 0, c->w, c->h);
    } else {
        vimtu_canvas_clip(c, clip->x0, clip->y0, clip->x1, clip->y1);
    }
    return NSERROR_OK;
}

static nserror vimtu_plot_arc(const struct redraw_context *ctx, const plot_style_t *pstyle,
                              int x, int y, int radius, int angle1, int angle2)
{
    /* 圆弧没有实现：本前端的"圆"只出现在 disc（填充圆）里，CSS 的 border-radius
     * 已经被上层降成矩形。返回 OK 而不是错误：绘图失败不该让整页 redraw 中止。 */
    (void)ctx;
    (void)pstyle;
    (void)x;
    (void)y;
    (void)radius;
    (void)angle1;
    (void)angle2;
    return NSERROR_OK;
}

static nserror vimtu_plot_disc(const struct redraw_context *ctx, const plot_style_t *pstyle,
                               int x, int y, int radius)
{
    struct vimtu_canvas *c = cv_of(ctx);
    uint32_t colour;
    int dy;

    if (c == NULL || pstyle == NULL || radius <= 0) {
        return NSERROR_OK;
    }
    colour = (pstyle->fill_type != PLOT_OP_TYPE_NONE) ? pstyle->fill_colour : pstyle->stroke_colour;

    /* 逐行填：对每一行求半宽，得到一个正圆（不追求子像素精度） */
    for (dy = -radius; dy <= radius; dy++) {
        int64_t rr = (int64_t)radius * radius - (int64_t)dy * dy;
        int half = 0;
        while ((int64_t)(half + 1) * (half + 1) <= rr) {
            half++;
        }
        vimtu_canvas_fill(c, x - half, y + dy, half * 2 + 1, 1, colour);
    }
    return NSERROR_OK;
}

/* 线段：Bresenham + 方形笔（粗线就是多画几遍偏移一位的线）。
 * 为什么不用"矩形近似"：CSS 的斜边框、表单控件对勾都靠 line，矩形近似会缺角。 */
static void draw_line(struct vimtu_canvas *c, int x0, int y0, int x1, int y1,
                      int thickness, uint32_t colour)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int dy = (y1 > y0) ? (y1 - y0) : (y0 - y1);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;
    int t;

    for (;;) {
        for (t = 0; t < thickness; t++) {
            vimtu_canvas_fill(c, x0 + t, y0, 1, 1, colour);
            if (t > 0) {
                vimtu_canvas_fill(c, x0, y0 + t, 1, 1, colour);
            }
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = err * 2;
            if (e2 > -dy) {
                err -= dy;
                x0 += sx;
            }
            if (e2 < dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static nserror vimtu_plot_line(const struct redraw_context *ctx, const plot_style_t *pstyle,
                               const struct rect *line)
{
    struct vimtu_canvas *c = cv_of(ctx);
    uint32_t colour;
    int thickness;

    if (c == NULL || pstyle == NULL || line == NULL) {
        return NSERROR_OK;
    }
    if (pstyle->stroke_type == PLOT_OP_TYPE_NONE) {
        return NSERROR_OK;
    }
    colour = pstyle->stroke_colour;
    thickness = stroke_width(pstyle);

    draw_line(c, line->x0, line->y0, line->x1, line->y1, thickness, colour);
    return NSERROR_OK;
}

static nserror vimtu_plot_rectangle(const struct redraw_context *ctx, const plot_style_t *pstyle,
                                    const struct rect *r)
{
    struct vimtu_canvas *c = cv_of(ctx);
    int w;
    int h;

    if (c == NULL || pstyle == NULL || r == NULL) {
        return NSERROR_OK;
    }
    w = r->x1 - r->x0;
    h = r->y1 - r->y0;
    if (w <= 0 || h <= 0) {
        return NSERROR_OK;
    }

    if (pstyle->fill_type != PLOT_OP_TYPE_NONE) {
        vimtu_canvas_fill(c, r->x0, r->y0, w, h, pstyle->fill_colour);
    }
    if (pstyle->stroke_type != PLOT_OP_TYPE_NONE) {
        vimtu_canvas_frame(c, r->x0, r->y0, w, h, stroke_width(pstyle), pstyle->stroke_colour);
    }
    return NSERROR_OK;
}

/* 多边形填充：扫描线 + 奇偶规则。
 * 为什么要它：CSS 的斜边框/三角形（border-style: solid 的四条梯形边）全走 polygon，
 * 不实现就只剩直角矩形，页面边框会"缺角"。
 * 坐标用 2 倍整数（yc = 2*y+1 取像素中心）避免浮点，也就避开了 -mcmodel=large 下的
 * 软浮点转换开销。 */
static nserror vimtu_plot_polygon(const struct redraw_context *ctx, const plot_style_t *pstyle,
                                  const int *p, unsigned int n)
{
    struct vimtu_canvas *c = cv_of(ctx);
    int xs[64];
    int miny, maxy, y;
    unsigned int i;

    if (c == NULL || pstyle == NULL || p == NULL || n < 3) {
        return NSERROR_OK;
    }
    if (pstyle->fill_type == PLOT_OP_TYPE_NONE) {
        return NSERROR_OK;
    }
    if (n > (unsigned int)(sizeof(xs) / sizeof(xs[0]))) {
        return NSERROR_OK;      /* 顶点太多：宁可不画，也不越界写栈 */
    }

    miny = maxy = p[1];
    for (i = 1; i < n; i++) {
        int py = p[i * 2 + 1];
        if (py < miny) {
            miny = py;
        }
        if (py > maxy) {
            maxy = py;
        }
    }
    if (miny < c->clip_y0) {
        miny = c->clip_y0;
    }
    if (maxy > c->clip_y1) {
        maxy = c->clip_y1;
    }

    for (y = miny; y < maxy; y++) {
        int yc = 2 * y + 1;            /* 像素中心的 2 倍 */
        int nxs = 0;

        for (i = 0; i < n; i++) {
            unsigned int k = (i + 1 == n) ? 0 : (i + 1);
            int x0 = p[i * 2], y0 = 2 * p[i * 2 + 1];
            int x1 = p[k * 2], y1 = 2 * p[k * 2 + 1];

            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                int64_t num = (int64_t)(yc - y0) * (int64_t)(x1 - x0);
                int64_t den = (int64_t)(y1 - y0);
                xs[nxs++] = x0 + (int)(num / den);
            }
        }

        /* 交点排序（插入排序：nxs <= 64，够用且没有额外依赖） */
        for (i = 1; i < (unsigned int)nxs; i++) {
            int v = xs[i];
            unsigned int k = i;
            while (k > 0 && xs[k - 1] > v) {
                xs[k] = xs[k - 1];
                k--;
            }
            xs[k] = v;
        }

        for (i = 0; i + 1 < (unsigned int)nxs; i += 2) {
            int w = xs[i + 1] - xs[i];
            if (w > 0) {
                vimtu_canvas_fill(c, xs[i], y, w, 1, pstyle->fill_colour);
            }
        }
    }
    return NSERROR_OK;
}

static nserror vimtu_plot_path(const struct redraw_context *ctx, const plot_style_t *pstyle,
                               const float *p, unsigned int n, const float transform[6])
{
    /* 贝塞尔路径（SVG / CSS clip-path）没有实现：需要浮点光栅化器，
     * 与"先让页面能显示能滚动能点击"的目标相比优先级最低。 */
    (void)ctx;
    (void)pstyle;
    (void)p;
    (void)n;
    (void)transform;
    return NSERROR_OK;
}

static nserror vimtu_plot_bitmap(const struct redraw_context *ctx, struct bitmap *bitmap,
                                 int x, int y, int width, int height, colour bg,
                                 bitmap_flags_t flags)
{
    struct vimtu_canvas *c = cv_of(ctx);
    const uint32_t *px = NULL;
    int sw = 0, sh = 0;
    bool opaque = false;
    int stride_px;

    (void)bg;
    if (c == NULL || bitmap == NULL || width <= 0 || height <= 0) {
        return NSERROR_OK;
    }
    if (!vimtu_bitmap_pixels(bitmap, &px, &sw, &sh, &opaque) || px == NULL) {
        return NSERROR_OK;
    }

    /* px 是紧凑排列（stride = w*4），vimtu_canvas_blit 假设的也是紧凑排列 */
    stride_px = sw;

    if ((flags & (BITMAPF_REPEAT_X | BITMAPF_REPEAT_Y)) == 0) {
        vimtu_canvas_blit(c, x, y, width, height, px, stride_px, sh, opaque);
        return NSERROR_OK;
    }

    /* 平铺：以 (x,y) 为第一块，在裁剪区内铺满。
     * 上界用画布裁剪矩形兜住，不用 flags 之外的信息 —— 平铺区域由 clip 决定，
     * 这是 NetSurf 的约定（background-repeat）。 */
    {
        int tx0 = (flags & BITMAPF_REPEAT_X) ? x : x;
        int ty0 = (flags & BITMAPF_REPEAT_Y) ? y : y;
        int x_end = c->clip_x1;
        int y_end = c->clip_y1;
        int ty;

        /* 起点往前退到裁剪区外一块，保证左/上边缘也被铺到 */
        if (flags & BITMAPF_REPEAT_X) {
            while (tx0 > c->clip_x0) {
                tx0 -= width;
            }
        }
        if (flags & BITMAPF_REPEAT_Y) {
            while (ty0 > c->clip_y0) {
                ty0 -= height;
            }
        }

        for (ty = ty0; ty < y_end; ty += height) {
            int tx;
            for (tx = tx0; tx < x_end; tx += width) {
                vimtu_canvas_blit(c, tx, ty, width, height, px, stride_px, sh, opaque);
                if ((flags & BITMAPF_REPEAT_X) == 0) {
                    break;
                }
            }
            if ((flags & BITMAPF_REPEAT_Y) == 0) {
                break;
            }
        }
    }
    return NSERROR_OK;
}

static nserror vimtu_plot_text(const struct redraw_context *ctx, const plot_font_style_t *fstyle,
                               int x, int y, const char *text, size_t length)
{
    struct vimtu_canvas *c = cv_of(ctx);

    if (c == NULL || text == NULL || length == 0) {
        return NSERROR_OK;
    }
    /* y 是基线（NetSurf 的契约），坐标已经是画布坐标 */
    vimtu_canvas_text(c, fstyle, x, y, text, length);
    return NSERROR_OK;
}

static nserror vimtu_plot_group_start(const struct redraw_context *ctx, const char *name)
{
    (void)ctx;
    (void)name;
    return NSERROR_OK;          /* 矢量导出用的分组，屏幕上没意义 */
}

static nserror vimtu_plot_group_end(const struct redraw_context *ctx)
{
    (void)ctx;
    return NSERROR_OK;
}

/* flush：把当前这帧真正推到 surface 上（damage+commit+dispatch）。
 * 规范里说"前端显示用笔的 flush 必须是 NULL"（只有 knockout 内部用），我们确实也把
 * option_knockout 关掉了，所以核心不会在 redraw 中间调它；这里仍然给出一个真的实现，
 * 让"最后一笔 = 上屏"这条语义有个具名落点（事件循环末尾也直接调它）。 */
static nserror vimtu_plot_flush(const struct redraw_context *ctx)
{
    struct vimtu_canvas *c = cv_of(ctx);

    if (c != NULL) {
        vimtu_canvas_flush(c, 0, 0, c->w, c->h);
    }
    return NSERROR_OK;
}

/* 全局笔（见文件头）。option_knockout = false：knockout 需要额外的队列与多次 flush，
 * 在软件画布上没有收益，反而把"一帧一提交"这条简单语义复杂化。 */
const struct plotter_table plot = {
    .clip = vimtu_plot_clip,
    .arc = vimtu_plot_arc,
    .disc = vimtu_plot_disc,
    .line = vimtu_plot_line,
    .rectangle = vimtu_plot_rectangle,
    .polygon = vimtu_plot_polygon,
    .path = vimtu_plot_path,
    .bitmap = vimtu_plot_bitmap,
    .text = vimtu_plot_text,
    .group_start = vimtu_plot_group_start,
    .group_end = vimtu_plot_group_end,
    .flush = vimtu_plot_flush,
    .option_knockout = false,
};
