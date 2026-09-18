// fb.cpp - Framebuffer 驱动（VBE LFB 32bpp，手搓）
#include "fb.h"
#include "font8x8.h"
#include "../bootinfo.h"
#include "port.h"

static uint32_t fb_addr = 0;
static int fb_w = 0, fb_h = 0;
static int fb_pitch = 0;
static uint8_t fb_bpp = 32;
static int g_render_w = 0, g_render_h = 0;   // 渲染分辨率（GUI 坐标系；= 物理/缩放）
static int g_zoom = 100;                     // 显示缩放百分比

// ---- 裁剪矩形（半开区间 [x0,x1) × [y0,y1)）：窗口客户区绘制必须被裁剪 ----
static int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;
static int g_clip_on = 0;      // 0 = 不裁剪（整屏）

void fb_set_clip(int x, int y, int w, int h) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_render_w) x1 = g_render_w;
    if (y1 > g_render_h) y1 = g_render_h;
    g_clip_x0 = x; g_clip_y0 = y; g_clip_x1 = x1; g_clip_y1 = y1;
    g_clip_on = 1;
}

void fb_reset_clip() {
    g_clip_on = 0;
    g_clip_x0 = 0; g_clip_y0 = 0;
    g_clip_x1 = g_render_w; g_clip_y1 = g_render_h;
}

// 把矩形与裁剪区求交；返回 false = 完全被裁掉（无需绘制）
static inline bool clip_rect(int* x, int* y, int* w, int* h) {
    if (g_clip_on) {
        if (*x < g_clip_x0) { *w += *x - g_clip_x0; *x = g_clip_x0; }
        if (*y < g_clip_y0) { *h += *y - g_clip_y0; *y = g_clip_y0; }
        if (*x + *w > g_clip_x1) *w = g_clip_x1 - *x;
        if (*y + *h > g_clip_y1) *h = g_clip_y1 - *y;
    }
    if (*x < 0) { *w += *x; *x = 0; }
    if (*y < 0) { *h += *y; *y = 0; }
    if (*x + *w > g_render_w) *w = g_render_w - *x;
    if (*y + *h > g_render_h) *h = g_render_h - *y;
    return (*w > 0 && *h > 0);
}

// 后备缓冲（32bpp 统一格式）：所有绘制先写这里，fb_flip() 一次性提交到 LFB，
// 避免 GUI 每秒整屏重绘时在屏幕上出现中间状态（闪烁/撕裂）。
// 上限 3840x2160x4B ≈ 33MB（支持 4K 分辨率切换），放 .bss（_end 之后）。
static uint32_t backbuf_storage[3840 * 2160];
static uint32_t* backbuf = nullptr;

// Bochs VBE 扩展寄存器（QEMU stdvga 支持）：0x1CE 索引 / 0x1CF 数据
#define VBE_INDEX_ID      0x0
#define VBE_INDEX_XRES    0x1
#define VBE_INDEX_YRES    0x2
#define VBE_INDEX_BPP     0x3
#define VBE_INDEX_ENABLE  0x4

// 切换显示模式（分辨率）。失败返回 false（显示器/显存不支持）。
bool fb_set_mode(int w, int h) {
    if (w < 640 || h < 480 || w > 3840 || h > 2160) { outb(0x402, '1'); return false; }
    outw(0x1CE, VBE_INDEX_ENABLE); outw(0x1CF, 0x00);       // 先关闭显示
    outw(0x1CE, VBE_INDEX_XRES);   outw(0x1CF, (uint16_t)w);
    outw(0x1CE, VBE_INDEX_YRES);   outw(0x1CF, (uint16_t)h);
    outw(0x1CE, VBE_INDEX_BPP);    outw(0x1CF, 32);
    outw(0x1CE, VBE_INDEX_ENABLE); outw(0x1CF, 0x81);        // 开启 + LFB
    // 回读验证（QEMU/真实硬件不支持时会钳制或返回 0）
    outw(0x1CE, VBE_INDEX_XRES); uint16_t rw = inw(0x1CF);
    outw(0x1CE, VBE_INDEX_YRES); uint16_t rh = inw(0x1CF);
    outb(0x402, '3'); outb(0x402, (uint8_t)(rw >> 8)); outb(0x402, (uint8_t)rw);
    outb(0x402, '4'); outb(0x402, (uint8_t)(rh >> 8)); outb(0x402, (uint8_t)rh);
    if (rw != w || rh != h) { outb(0x402, '5'); return false; }
    fb_w = w; fb_h = h;
    fb_bpp = 32;
    fb_pitch = w * 4;
    backbuf = backbuf_storage;
    uint32_t n = (uint32_t)w * h;
    for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
    // 按当前缩放重算渲染分辨率
    g_render_w = fb_w * 100 / g_zoom;
    g_render_h = fb_h * 100 / g_zoom;
    return true;
}

void fb_init(const BootInfo* bi) {
    fb_addr = bi->lfb_addr;
    fb_w = bi->width;
    fb_h = bi->height;
    fb_bpp = bi->bpp;
    fb_pitch = bi->pitch;
    if (fb_pitch == 0) fb_pitch = fb_w * 4;
    backbuf = backbuf_storage;
    // 清零后备缓冲
    uint32_t n = (uint32_t)fb_w * fb_h;
    for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
    g_render_w = fb_w;
    g_render_h = fb_h;
    g_zoom = 100;
}

int fb_width() { return g_render_w ? g_render_w : fb_w; }
int fb_height() { return g_render_h ? g_render_h : fb_h; }
int fb_phys_width() { return fb_w; }
int fb_phys_height() { return fb_h; }
int fb_get_zoom() { return g_zoom; }

// 设置显示缩放：渲染分辨率 = 物理/缩放，提交时最近邻放大
void fb_set_zoom(int pct) {
    if (pct < 100) pct = 100;
    if (pct > 200) pct = 200;
    g_zoom = pct;
    g_render_w = fb_w * 100 / pct;
    g_render_h = fb_h * 100 / pct;
    uint32_t n = (uint32_t)g_render_w * g_render_h;
    for (uint32_t i = 0; i < n; i++) backbuf[i] = 0;
}

// 后备缓冲 -> LFB（整帧提交）
void fb_flip() {
    if (!backbuf || !fb_addr) return;
    if (g_zoom == 100) {
        if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
            // 快速路径：整块逐像素拷贝
            uint32_t* dst = (uint32_t*)fb_addr;
            uint32_t n = (uint32_t)fb_w * fb_h;
            for (uint32_t i = 0; i < n; i++) dst[i] = backbuf[i];
        } else {
            // 慢速路径：逐行按 bpp 转换
            for (int y = 0; y < fb_h; y++) {
                volatile uint8_t* row = (volatile uint8_t*)(fb_addr + y * fb_pitch);
                for (int x = 0; x < fb_w; x++) {
                    uint32_t c = backbuf[y * fb_w + x];
                    if (fb_bpp == 32) {
                        ((volatile uint32_t*)row)[x] = c;
                    } else if (fb_bpp == 24) {
                        row[x * 3] = c & 0xFF;
                        row[x * 3 + 1] = (c >> 8) & 0xFF;
                        row[x * 3 + 2] = (c >> 16) & 0xFF;
                    } else {
                        uint16_t cc = ((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3);
                        ((volatile uint16_t*)row)[x] = cc;
                    }
                }
            }
        }
        return;
    }
    // 缩放路径：最近邻放大渲染区域到整个 LFB
    uint32_t* dst = (uint32_t*)fb_addr;
    int pw = fb_w, ph = fb_h, rw = g_render_w, rh = g_render_h;
    for (int y = 0; y < ph; y++) {
        int sy = y * rh / ph;
        if (sy >= rh) sy = rh - 1;
        const uint32_t* srow = backbuf + sy * rw;
        uint32_t* drow = dst + y * pw;
        for (int x = 0; x < pw; x++) {
            drow[x] = srow[x * rw / pw];
        }
    }
}

// 后备缓冲 -> LFB（仅提交指定区域；用于光标等局部更新，避免整帧 3MB 拷贝拖慢跟手）
void fb_flip_region(int x, int y, int w, int h) {
    if (!backbuf || !fb_addr) return;
    if (g_zoom != 100) { fb_flip(); return; }   // 缩放时无法局部映射，退化整帧
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
        uint32_t* dst = (uint32_t*)fb_addr;
        for (int row = 0; row < h; row++) {
            uint32_t* d = dst + (y + row) * fb_w + x;
            uint32_t* s = backbuf + (y + row) * fb_w + x;
            for (int i = 0; i < w; i++) d[i] = s[i];
        }
    } else {
        for (int yy = y; yy < y + h; yy++) {
            volatile uint8_t* row = (volatile uint8_t*)(fb_addr + yy * fb_pitch);
            for (int xx = x; xx < x + w; xx++) {
                uint32_t c = backbuf[yy * fb_w + xx];
                if (fb_bpp == 32) {
                    ((volatile uint32_t*)row)[xx] = c;
                } else if (fb_bpp == 24) {
                    row[xx * 3] = c & 0xFF;
                    row[xx * 3 + 1] = (c >> 8) & 0xFF;
                    row[xx * 3 + 2] = (c >> 16) & 0xFF;
                } else {
                    uint16_t cc = ((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3);
                    ((volatile uint16_t*)row)[xx] = cc;
                }
            }
        }
    }
}

static inline void putpx(int x, int y, uint32_t color) {
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return;
    if (g_clip_on && (x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1)) return;
    if (backbuf) {
        backbuf[y * g_render_w + x] = color;   // 全部绘制到后备缓冲（渲染分辨率）
    }
}

void fb_putpixel(int x, int y, uint32_t color) { putpx(x, y, color); }

void fb_clear(uint32_t color) {
    if (!backbuf) return;
    // ★ 清屏必须**尊重裁剪**。安装程序为了鼠标跟手改成"脏矩形重绘"（鼠标移动时只重画
    //   光标新旧位置那一小块），如果这里无视裁剪整屏清屏，那一小块的背景就画不回来
    //   （表现为光标拖影、整屏闪烁），局部重绘也就白做了。
    int x0 = g_clip_on ? g_clip_x0 : 0;
    int y0 = g_clip_on ? g_clip_y0 : 0;
    int x1 = g_clip_on ? g_clip_x1 : g_render_w;
    int y1 = g_clip_on ? g_clip_y1 : g_render_h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_render_w) x1 = g_render_w;
    if (y1 > g_render_h) y1 = g_render_h;
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return;
    // 逐行填充（比逐像素 putpx 快得多：整屏重绘时这一项就能省下大量时间）
    uint32_t* base = backbuf + y0 * g_render_w + x0;
    for (int yy = 0; yy < h; yy++) {
        uint32_t* row = base + yy * g_render_w;
        for (int xx = 0; xx < w; xx++) row[xx] = color;
    }
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (backbuf) {
        // 快速路径：直接写后备缓冲（32bpp 统一）
        uint32_t* base = backbuf + y * g_render_w + x;
        for (int yy = 0; yy < h; yy++) {
            uint32_t* row = base + yy * g_render_w;
            for (int xx = 0; xx < w; xx++) row[xx] = color;
        }
        return;
    }
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            putpx(xx, yy, color);
}

// 半透明填充：对后备缓冲现有像素做 alpha 混合（alpha 0-255，越大越不透明）
void fb_fill_rect_alpha(int x, int y, int w, int h, uint32_t color, int alpha) {
    if (!clip_rect(&x, &y, &w, &h)) return;
    if (alpha > 255) alpha = 255;
    if (alpha <= 0) return;
    if (!backbuf) return;
    uint8_t nr = (color >> 16) & 0xFF, ng = (color >> 8) & 0xFF, nb = color & 0xFF;
    int inv = 255 - alpha;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = &backbuf[yy * g_render_w + x];
        for (int xx = 0; xx < w; xx++) {
            uint32_t old = row[xx];
            uint8_t orr = (old >> 16) & 0xFF, og = (old >> 8) & 0xFF, ob = old & 0xFF;
            uint8_t r = (uint8_t)((orr * inv + nr * alpha) / 255);
            uint8_t g = (uint8_t)((og * inv + ng * alpha) / 255);
            uint8_t b = (uint8_t)((ob * inv + nb * alpha) / 255);
            row[xx] = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

void fb_draw_hline(int x, int y, int w, uint32_t color) { fb_fill_rect(x, y, w, 1, color); }
void fb_draw_vline(int x, int y, int h, uint32_t color) { fb_fill_rect(x, y, 1, h, color); }

void fb_draw_rect(int x, int y, int w, int h, uint32_t color) {
    fb_draw_hline(x, y, w, color);
    fb_draw_hline(x, y + h - 1, w, color);
    fb_draw_vline(x, y, h, color);
    fb_draw_vline(x + w - 1, y, h, color);
}

void fb_draw_rect_fill(int x, int y, int w, int h, uint32_t color) {
    fb_fill_rect(x, y, w, h, color);
}

void fb_draw_char(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;
    if (scale < 1) scale = 1;
    const unsigned char* glyph = font8x8[idx];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> (7 - col)) & 1;
            uint32_t color = on ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    putpx(x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

void fb_draw_text(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale) {
    int cx = x;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        fb_draw_char(cx, y, c, fg, bg, scale);
        cx += 8 * scale;
    }
}

void fb_draw_text_n(int x, int y, const char* s, int n, uint32_t fg, uint32_t bg, int scale) {
    for (int i = 0; i < n && s[i]; i++) {
        fb_draw_char(x, y, s[i], fg, bg, scale);
        x += 8 * scale;
    }
}

// ---- 镜像版文字（字符水平翻转后绘制）----
static uint8_t fb_flip_byte(uint8_t v) {
    uint8_t r = 0;
    for (int b = 0; b < 8; b++) r |= ((v >> b) & 1) << (7 - b);
    return r;
}

void fb_draw_char_mirror(int x, int y, char c, uint32_t fg, uint32_t bg, int scale) {
    int idx = (unsigned char)c - 32;
    if (idx < 0 || idx >= 95) return;
    if (scale < 1) scale = 1;
    const unsigned char* glyph = font8x8[idx];
    for (int row = 0; row < 8; row++) {
        unsigned char bits = fb_flip_byte(glyph[row]);
        for (int col = 0; col < 8; col++) {
            bool on = (bits >> (7 - col)) & 1;
            uint32_t color = on ? fg : bg;
            for (int sy = 0; sy < scale; sy++)
                for (int sx = 0; sx < scale; sx++)
                    putpx(x + col * scale + sx, y + row * scale + sy, color);
        }
    }
}

void fb_draw_text_mirror(int x, int y, const char* s, uint32_t fg, uint32_t bg, int scale) {
    int cx = x;
    for (int i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '\n') { cx = x; y += 8 * scale; continue; }
        fb_draw_char_mirror(cx, y, c, fg, bg, scale);
        cx += 8 * scale;
    }
}

// 16x16 位图（1=前景色，0=透明），用于鼠标光标等
void fb_draw_bitmap16(int x, int y, const uint16_t* bmp, int w, int h, uint32_t color) {
    for (int yy = 0; yy < h; yy++) {
        for (int xx = 0; xx < w; xx++) {
            if (bmp[yy] & (1 << (15 - xx))) {
                putpx(x + xx, y + yy, color);
            }
        }
    }
}

// RGBA 图像绘制到后备缓冲（假设背景为黑色，直接按 alpha 混合）
void fb_blit_rgba(int x, int y, const uint8_t* rgba, int w, int h) {
    if (!backbuf) return;
    int by0 = 0, by1 = h;      // 行裁剪范围
    if (g_clip_on) {
        if (y < g_clip_y0) by0 = g_clip_y0 - y;
        if (y + by1 > g_clip_y1) by1 = g_clip_y1 - y;
    }
    for (int j = by0; j < by1; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= g_render_h) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= g_render_w) continue;
            if (g_clip_on && (xx < g_clip_x0 || xx >= g_clip_x1)) continue;
            uint8_t r = rgba[0], g = rgba[1], b = rgba[2], a = rgba[3];
            rgba += 4;
            if (a == 0) continue;
            uint32_t c;
            if (a == 255) {
                c = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
            } else {
                c = 0xFF000000u | ((uint32_t)(r * a / 255) << 16)
                                | ((uint32_t)(g * a / 255) << 8)
                                | ((uint32_t)(b * a / 255));
            }
            backbuf[yy * g_render_w + xx] = c;
        }
    }
}

uint32_t fb_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return 0;
    if (backbuf) return backbuf[y * g_render_w + x];
    if (fb_bpp == 32) return *(volatile uint32_t*)(fb_addr + y * fb_pitch + x * 4);
    return 0;
}
