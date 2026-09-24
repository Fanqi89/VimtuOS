// fb.h - Framebuffer 图形驱动（VBE LFB，32bpp）
#pragma once
#include <stdint.h>

void fb_init(const struct BootInfo* bi);
bool fb_set_mode(int w, int h);     // Bochs VBE 切换分辨率（失败返回 false）
void fb_flip();                 // 后备缓冲 -> LFB 一次性提交（消除闪烁）
void fb_flip_region(int x, int y, int w, int h);  // 仅提交指定区域（光标等局部更新）
void fb_clear(uint32_t color);
void fb_fill_rect(int x, int y, int w, int h, uint32_t color);
void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha);  // 半透明填充（alpha 0-255）
void fb_putpixel(int x, int y, uint32_t color);
void fb_draw_hline(int x, int y, int w, uint32_t color);
void fb_draw_vline(int x, int y, int h, uint32_t color);
void fb_draw_rect(int x, int y, int w, int h, uint32_t color);           // 空心矩形
void fb_draw_rect_fill(int x, int y, int w, int h, uint32_t color);      // 实心
void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale);
void fb_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale);
void fb_draw_text_n(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int scale);
void fb_draw_char_mirror(int x, int y, char c, uint32_t fg, uint32_t bg, int scale);  // 水平镜像
void fb_draw_text_mirror(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale);
void fb_draw_bitmap16(int x, int y, const uint16_t* bmp, int w, int h, uint32_t color);
void fb_blit_rgba(int x, int y, const uint8_t* rgba, int w, int h);   // RGBA 图像（黑底 alpha 混合）
int fb_width();
int fb_height();
int fb_phys_width();    // 物理分辨率（LFB/VBE 模式）
int fb_phys_height();
int fb_get_zoom();      // 当前缩放百分比（100/125/150）
void fb_set_zoom(int pct);  // 设置显示缩放（渲染分辨率 = 物理/缩放，提交时放大）
// 裁剪矩形（半开区间）：窗口客户区内容必须裁剪在客户区内；否则内容会画到窗口外，
// 拖动窗口时这些"溢出像素"会被脏区提交上屏，表现为拖影/残影。
void fb_set_clip(int x, int y, int w, int h);
void fb_reset_clip();
void fb_get_clip64(int* x, int* y, int* w, int* h);   // 读当前裁剪矩形（gfx64 的图元必须遵守它）
uint32_t* fb_surface64(int* w, int* h);               // 后备缓冲基址（32bpp，stride = fb_width()）
uint32_t fb_get_pixel(int x, int y);

// 颜色辅助
inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
