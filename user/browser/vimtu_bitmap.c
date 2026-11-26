/* user/browser/vimtu_bitmap.c - gui_bitmap_table 实现
 *
 * 为什么是这张表：NetSurf 的图像解码器（GIF/BMP/ICO/SVG）不直接碰前端像素格式，而是通过
 * 这张表拿到一块可写的 32bpp 缓冲，再按 bitmap_set_format() 声明的布局把像素写进去。
 * 所以这里唯一要"想清楚"的东西是格式协商：
 *   * 声明 BITMAP_LAYOUT_B8G8R8A8（字节序 B,G,R,A -> uint32 = 0xAARRGGBB）
 *   * pma = false（非预乘）：贴图时用 straight alpha 混合，和 vimtu_canvas_blit() 的口径一致。
 *     核心在 desktop/bitmap.c 里有 bitmap_format_convert()，解码器给的布局/预乘方式与这里
 *     不一致时它会替我们转，所以这个声明不会把某个解码器"逼死"。
 *   * 画布是 XRGB8888（0x00RRGGBB），位图是 0xAARRGGBB：RGB 通道位序相同，只需跳过 alpha。
 *
 * render()（把内容渲染进缩略图位图）在这里是**空实现**：本前端的本地历史/缩略图界面没做，
 * 走到这里的调用（历史记录缩略图）没有用户可见后果。返回 NOT_IMPLEMENTED 而不是 OK，
 * 是为了不假装做过。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* utils/errors.h 必须先来：netsurf 系列头 里用了 nserror 却不自带这个头 */
#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "netsurf/plotters.h"

#include "vimtu_bitmap.h"

bool vimtu_bitmap_pixels(const struct bitmap *bm, const uint32_t **px, int *w, int *h, bool *opaque)
{
    if (bm == NULL || bm->px == NULL) {
        return false;
    }
    if (px != NULL) {
        *px = bm->px;
    }
    if (w != NULL) {
        *w = bm->w;
    }
    if (h != NULL) {
        *h = bm->h;
    }
    if (opaque != NULL) {
        *opaque = bm->opaque;
    }
    return true;
}

static void *vimtu_bitmap_create(int width, int height, enum gui_bitmap_flags flags)
{
    struct bitmap *bm;

    if (width <= 0 || height <= 0) {
        return NULL;
    }
    /* 防御：单张图像 32MiB 上限。没有这条，一张被声明成 30000x30000 的 GIF
     * 会在 malloc 里直接把整个 2GiB 地址空间要光（本进程还要放 NetSurf + mbedTLS）。 */
    if ((int64_t)width * (int64_t)height > 8 * 1024 * 1024) {
        return NULL;
    }

    bm = (struct bitmap *)calloc(1, sizeof(*bm));
    if (bm == NULL) {
        return NULL;
    }
    bm->w = width;
    bm->h = height;
    bm->stride = (size_t)width * 4u;
    bm->opaque = ((flags & BITMAP_OPAQUE) != 0);

    bm->px = (uint32_t *)malloc(bm->stride * (size_t)height);
    if (bm->px == NULL) {
        free(bm);
        return NULL;
    }
    if ((flags & BITMAP_CLEAR) != 0) {
        memset(bm->px, 0, bm->stride * (size_t)height);
    }
    return bm;
}

static void vimtu_bitmap_destroy(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;

    if (bm == NULL) {
        return;
    }
    free(bm->px);
    free(bm);
}

static void vimtu_bitmap_set_opaque(void *bitmap, bool opaque)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    if (bm != NULL) {
        bm->opaque = opaque;
    }
}

static bool vimtu_bitmap_get_opaque(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    return (bm != NULL) ? bm->opaque : false;
}

static unsigned char *vimtu_bitmap_get_buffer(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    return (bm != NULL) ? (unsigned char *)bm->px : NULL;
}

static size_t vimtu_bitmap_get_rowstride(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    return (bm != NULL) ? bm->stride : 0;
}

static int vimtu_bitmap_get_width(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    return (bm != NULL) ? bm->w : 0;
}

static int vimtu_bitmap_get_height(void *bitmap)
{
    struct bitmap *bm = (struct bitmap *)bitmap;
    return (bm != NULL) ? bm->h : 0;
}

static void vimtu_bitmap_modified(void *bitmap)
{
    /* 没有 GPU 缓存/后端纹理需要同步：像素就是那块 malloc */
    (void)bitmap;
}

static nserror vimtu_bitmap_render(struct bitmap *bitmap, struct hlcache_handle *content)
{
    (void)bitmap;
    (void)content;
    return NSERROR_NOT_IMPLEMENTED;      /* 缩略图渲染没做，见文件头 */
}

static struct gui_bitmap_table vimtu_bitmap_table = {
    .create = vimtu_bitmap_create,
    .destroy = vimtu_bitmap_destroy,
    .set_opaque = vimtu_bitmap_set_opaque,
    .get_opaque = vimtu_bitmap_get_opaque,
    .get_buffer = vimtu_bitmap_get_buffer,
    .get_rowstride = vimtu_bitmap_get_rowstride,
    .get_width = vimtu_bitmap_get_width,
    .get_height = vimtu_bitmap_get_height,
    .modified = vimtu_bitmap_modified,
    .render = vimtu_bitmap_render,
};

struct gui_bitmap_table *vimtu_get_bitmap_table(void)
{
    return &vimtu_bitmap_table;
}
