/* user/browser/vimtu_canvas.h - 浏览器客户区的画布（shm + wl_surface + XRGB8888 像素）
 *
 * 为什么要有这一层：NetSurf 的 plotter 只能拿到"往哪画"的像素指针，拿不到窗口系统；
 * 而 wl_surface 的规矩是"往 shm 里写 + attach + damage + commit + dispatch 才上屏"。
 * 于是把"一块像素 + 一个 surface + 裁剪矩形 + 待提交脏区"封成 canvas，上层（plotters/
 * 外壳/事件循环）只跟 canvas 打交道。
 *
 * ★ 几何为什么要这么定（踩过的坑，写死在这里省得下次再算）：
 *   * 内核单 surface 像素上限 WL64_MAX_PIXELS64 = 524288（2 MiB / 4B）。960x540 = 518400，
 *     是能塞下的最大"像样"窗口；加宽/加高就 ENOSPC（表现为窗口全黑，不是 surface 创建失败）。
 *   * 声明成窗口（set_decor）后，**顶部 25 行归合成器**（kernel/wl64.cpp 的 blit 直接
 *     `if (deco && ry+yy < WL64_DECOR_H64) continue;` 跳过），客户端画的第 0..24 行永远不上屏。
 *     所以客户内容必须从 y = 25 开始画，否则地址栏会被标题栏吃掉一半。
 *   * 于是可用高度 = 540 - 25 = 515：44 px 的 Edge 风格 Chrome + 471 px 的页面区。
 */
#ifndef VIMTU_BROWSER_CANVAS_H
#define VIMTU_BROWSER_CANVAS_H

#include <stdbool.h>
#include <stdint.h>

struct plot_font_style;

/* 画布（= surface）尺寸 */
#define VIMTU_CANVAS_W   960
#define VIMTU_CANVAS_H   540
#define VIMTU_MAX_PIXELS 524288u          /* 内核 WL64_MAX_PIXELS64，硬上限 */

#define VIMTU_DECOR_H    25               /* 合成器标题栏占掉的行数 */
#define VIMTU_CHROME_H   44               /* 自绘的 Edge 风格工具条 */
#define VIMTU_CONTENT_Y  VIMTU_DECOR_H    /* 客户端内容起点 */
#define VIMTU_PAGE_Y     (VIMTU_CONTENT_Y + VIMTU_CHROME_H)
#define VIMTU_PAGE_W     VIMTU_CANVAS_W
#define VIMTU_PAGE_H     (VIMTU_CANVAS_H - VIMTU_PAGE_Y)

struct vimtu_canvas {
    int surf;                             /* wl_surface id（<0 = 没有） */
    int shm;                              /* shm 对象 id */
    int w;
    int h;
    uint32_t *px;                         /* 映射到的像素（XRGB8888，小端 = BGRA 字节序） */
    int clip_x0, clip_y0, clip_x1, clip_y1;
};

/* 建立画布：shm_create -> shm_map -> surface_create -> attach。返回 0 或负错误码 */
int vimtu_canvas_open(struct vimtu_canvas *c, int w, int h);
void vimtu_canvas_close(struct vimtu_canvas *c);

/* 当前活动画布（plotter 的 ctx->priv 为空时兜底用，例如 core 内部thumbnail 渲染） */
struct vimtu_canvas *vimtu_canvas_active(void);

/* 裁剪：与 surface 边界求交（画到框外一律丢弃，绝不越界写像素） */
void vimtu_canvas_clip(struct vimtu_canvas *c, int x0, int y0, int x1, int y1);

/* 纯色填充（colour = 0x00RRGGBB，与 NetSurf 的 colour 同口径） */
void vimtu_canvas_fill(struct vimtu_canvas *c, int x, int y, int w, int h, uint32_t colour);

/* 1px/多 px 边框（只画矩形描边） */
void vimtu_canvas_frame(struct vimtu_canvas *c, int x, int y, int w, int h, int bw, uint32_t colour);

/* 位图缩放贴图。src 是 0xAARRGGBB（非预乘）；opaque=true 时忽略 alpha */
void vimtu_canvas_blit(struct vimtu_canvas *c, int x, int y, int w, int h,
                       const uint32_t *src, int src_w, int src_h, bool opaque);

/* 文字：x = 笔位左端，baseline = 基线 y（NetSurf plotter 的 y 就是基线）。
 * 字形只覆盖 ASCII，其余字符画一个"缺字块"，不假装有字形。 */
void vimtu_canvas_text(struct vimtu_canvas *c, const struct plot_font_style *fstyle,
                       int x, int baseline, const char *utf8, size_t len);

/* 上屏：damage + commit + dispatch(0) */
void vimtu_canvas_flush(struct vimtu_canvas *c, int x, int y, int w, int h);

/* 统计某矩形里有多少像素不是背景色（给 [BROWSER] paint ink= 用，纯证据） */
unsigned vimtu_canvas_count_ink(const struct vimtu_canvas *c, int x, int y, int w, int h,
                                uint32_t background);

#endif /* VIMTU_BROWSER_CANVAS_H */
