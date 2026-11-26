/* user/browser/vimtu_bitmap.h - 前端位图（NetSurf 的 struct bitmap 是不透明类型，内部结构我自己定）
 *
 * 为什么自己定内部结构：核心只通过 gui_bitmap_table 的 get_buffer/get_rowstride/get_width...
 * 访问位图，永远不解引用字段。所以"位图长什么样"完全由前端决定 —— 那就定成最省事的一种：
 * 一块 malloc 的 32bpp 缓冲 + 宽高 + stride + opaque 标志。
 *
 * 像素格式：向核心声明 BITMAP_LAYOUT_B8G8R8A8 / pma=false（见 vimtu_bitmap.c 的说明），
 * 即内存里一个 uint32 = 0xAARRGGBB（非预乘），小端字节序 B,G,R,A —— 正好也是画布的 0x00RRGGBB
 * 加上 alpha 字节，贴图时只需按 alpha 混合，不用换通道。
 */
#ifndef VIMTU_BITMAP_H
#define VIMTU_BITMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 核心看到的"struct bitmap"就是它（不透明指针，字段只有本前端用） */
struct bitmap {
    int w;
    int h;
    size_t stride;              /* 字节/行 */
    bool opaque;
    uint32_t *px;
};

/* 给 plotter 用的取值接口（避免 plotter 直接解引用，保持一处定义） */
bool vimtu_bitmap_pixels(const struct bitmap *bm, const uint32_t **px, int *w, int *h, bool *opaque);

#endif /* VIMTU_BITMAP_H */
