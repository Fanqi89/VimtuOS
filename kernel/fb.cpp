// fb.cpp - Framebuffer 驱动（VBE LFB 32bpp，手搓）
#include "fb.h"
#include "font8x8.h"
#include "../bootinfo.h"
#include "port.h"
// ★ 驱动线 3：显示后端（virtio-gpu 2D 设备路径 / 软件 LFB 路径）。这里**只调用**它，
//   不反向依赖：没有设备时 vgpu64_blit64() 一律返回 0，下面的软件路径一个字节都没变。
#include "virtio_gpu64.h"
#include "debug64.h"     // fb_backend_log64() 的 [FB64] 打点
#include "x86_64.h"        // ★ ⑭：g_ticks64 / TICK_MS_64（帧节拍的粗睡眠/标定基准）

static uint32_t fb_addr = 0;
static int fb_w = 0, fb_h = 0;
static int fb_pitch = 0;
static uint8_t fb_bpp = 32;
static int g_render_w = 0, g_render_h = 0;   // 渲染分辨率（GUI 坐标系；= 物理/缩放）
static int g_zoom = 100;                     // 显示缩放百分比

// ---- 裁剪矩形（半开区间 [x0,x1) × [y0,y1)）：窗口客户区绘制必须被裁剪 ----
static int g_clip_x0 = 0, g_clip_y0 = 0, g_clip_x1 = 0, g_clip_y1 = 0;
static int g_clip_on = 0;      // 0 = 不裁剪（整屏）

// ---- A1：内核侧绘制/提交总开关（1 = 正常；0 = 用户绘图演示期间"内核不再画"）----
// 为什么需要它：A1 要让"屏幕上这一段确实是**用户程序**在画"这件事可验证 —— 演示期间把内核侧
// 的所有绘制/提交都挡掉（含 write(1,..) 的屏幕回显、启动期的局部提交），只放行用户程序自己的
// fb_flip（fb_user_flip64）。开关由 kernel/usermode64.cpp 的 user64_run_fbdemo64() 切换，
// 用户程序退出（或被 kill 的回收路径）时一定恢复。
static int g_kernel_paint64 = 1;

void fb_kernel_paint64(int on) { g_kernel_paint64 = on ? 1 : 0; }
int  fb_kernel_paint_on64() { return g_kernel_paint64; }

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
    if (!g_kernel_paint64) return false;      // ★ A1：内核侧绘制关闭时一律不画（含清屏/填充）
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
// ★ A1：**必须 4KB 对齐** —— 用户态绘图时这整块会被逐页映射给 ring3（见 fb_surface_phys64），
//   对齐之后映射就是"整块原样"，不用做页内偏移，也不会顺带暴露别的内核变量。
static uint32_t backbuf_storage[3840 * 2160] __attribute__((aligned(4096)));
static uint32_t* backbuf = nullptr;

void fb_get_clip64(int* x, int* y, int* w, int* h) {
    if (g_clip_on) {
        *x = g_clip_x0; *y = g_clip_y0;
        *w = g_clip_x1 - g_clip_x0; *h = g_clip_y1 - g_clip_y0;
    } else {
        *x = 0; *y = 0; *w = fb_width(); *h = fb_height();
    }
}

// 后备缓冲（32bpp）基址 + 渲染分辨率：现代图元层（gfx64）直接写这里，避免每像素走函数调用。
uint32_t* fb_surface64(int* w, int* h) {
    if (!backbuf) backbuf = backbuf_storage;
    if (w) *w = fb_width();
    if (h) *h = fb_height();
    return backbuf;
}

// A1：后备缓冲的物理基址（4KB 对齐）+ 字节数。恒等映射（PA 0..4GB）与高半区直映
// （VA = 0xFFFFFFFF80000000 + PA，见 boot/loader64.asm）在引导期都已建好，所以：
//   PA = VA - 0xFFFFFFFF80000000（与 kernel/proc64.cpp、kernel/kernel64.cpp 用的是同一条公式）。
uint64_t fb_surface_phys64(int* out_bytes) {
    uint32_t* bb = fb_surface64(nullptr, nullptr);
    if (!bb) { if (out_bytes) *out_bytes = 0; return 0; }
    if (out_bytes) *out_bytes = fb_width() * fb_height() * 4;
    return (uint64_t)(uintptr_t)bb - 0xFFFFFFFF80000000ULL;
}


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

// 后备缓冲 -> LFB（整帧提交）：共享实现（内核侧 fb_flip 与 A1 的 fb_user_flip64 都走它）
static void fb_flip_all64() {
    if (!backbuf || !fb_addr) return;
    // ★ 驱动线 3：设备路径优先。virtio-gpu 2D 可用时整屏也用**一个区域**提交：
    //   TRANSFER_TO_HOST_2D(整屏) + RESOURCE_FLUSH —— 不再让 CPU 逐像素写 LFB。
    //   返回 0（没有设备/几何不匹配/超时）就原样往下走既有软件路径（这一条是硬要求）。
    if (g_zoom == 100 && vgpu64_blit64(0, 0, fb_w, fb_h)) return;
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

// 后备缓冲 -> LFB（整帧提交）：**内核侧**提交入口（A1 演示期间被开关挡住）
void fb_flip() {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧提交关闭（用户程序独占屏幕）
    fb_flip_all64();
}

// 后备缓冲 -> LFB（仅提交指定区域；用于光标等局部更新，避免整帧 3MB 拷贝拖慢跟手）
// 共享实现：fb_flip_region（内核侧，受开关影响）与 fb_user_flip64（用户态提交，不受影响）都用它。
static void fb_blit_region64(int x, int y, int w, int h) {
    if (!backbuf || !fb_addr) return;
    if (g_zoom != 100) { fb_flip_all64(); return; }   // 缩放时无法局部映射，退化整帧
    // ★ 驱动线 3：设备路径优先（TRANSFER_TO_HOST_2D + RESOURCE_FLUSH 只搬脏矩形）。
    //   真走了设备就直接返回；否则（无设备/降级/超时）原样走下面的软件路径。
    if (vgpu64_blit64(x, y, w, h)) return;
    vgpu64_note_soft_blit64(x, y, w, h);              // 如实统计"这次走的是软件路径"
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

// 后备缓冲 -> LFB（仅提交指定区域；用于光标等局部更新，避免整帧 3MB 拷贝拖慢跟手）
// **内核侧**提交入口：A1 演示期间被"内核侧绘制/提交"开关挡住（用户程序用 fb_user_flip64）。
void fb_flip_region(int x, int y, int w, int h) {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧提交关闭
    fb_blit_region64(x, y, w, h);
}
// A1：**用户程序**的区域提交（fb_flip(10) 的实现）。与 fb_flip_region 的区别只有两点：
//   1) 不被"内核侧绘制开关"挡住 —— 这是用户自己的提交；
//   2) 坐标在这里**再夹一次**（syscall 侧已经夹过，双保险：不越界、不崩）。
void fb_user_flip64(int x, int y, int w, int h) {
    // 夹取在 fb_blit_region64 里做（负 x/y 夹到 0、w/h 夹到边界），这里只转交。
    fb_blit_region64(x, y, w, h);
}

// ★ 驱动线 3：显示后端查询 / 打点 / **强制软件路径**提交（三者都不改变默认行为）
const char* fb_backend_name64() { return vgpu64_backend_name64(); }

// 一行"当前显示后端"（规格要求：启动期打一行 [FB64] backend=...）。几何不在这里重复打
// （[G64] fb render= / [HWUI] 显示区块都有）—— 少一次内联 dbg64 展开就是几百字节（余量很紧）。
void fb_backend_log64(const char* backend) {
    dbg64_line_begin64();
    dbg64_str("[FB64] backend=");
    dbg64_str(backend ? backend : "?");
    dbg64_nl();
    dbg64_line_end64();
}

// 强制走软件路径的区域提交：**不看后端**（设备自检要的就是"软件路径的屏幕内容"）。
void fb_soft_flip_region64(int x, int y, int w, int h) {
    if (!backbuf || !fb_addr) return;
    if (g_zoom != 100) { fb_flip_all64(); return; }
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > fb_w) w = fb_w - x;
    if (y + h > fb_h) h = fb_h - y;
    if (w <= 0 || h <= 0) return;
    if (fb_bpp == 32 && fb_pitch == fb_w * 4) {
        uint32_t* dst = (uint32_t*)(uintptr_t)fb_addr;
        for (int row = 0; row < h; row++) {
            uint32_t* d = dst + (y + row) * fb_w + x;
            uint32_t* s = backbuf + (y + row) * fb_w + x;
            for (int i = 0; i < w; i++) d[i] = s[i];
        }
    } else {
        for (int yy = y; yy < y + h; yy++) {
            volatile uint8_t* row = (volatile uint8_t*)(uintptr_t)(fb_addr + (uint32_t)yy * (uint32_t)fb_pitch);
            for (int xx = x; xx < x + w; xx++) {
                uint32_t c = backbuf[yy * fb_w + xx];
                if (fb_bpp == 32)      ((volatile uint32_t*)row)[xx] = c;
                else if (fb_bpp == 24) { row[xx * 3] = c & 0xFF; row[xx * 3 + 1] = (c >> 8) & 0xFF; row[xx * 3 + 2] = (c >> 16) & 0xFF; }
                else                   ((volatile uint16_t*)row)[xx] = (uint16_t)(((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3));
            }
        }
    }
    vgpu64_note_soft_blit64(x, y, w, h);
}

// ---- 逐像素绘制（全部走后备缓冲；受内核侧绘制开关 + 裁剪矩形约束）----
static inline void putpx(int x, int y, uint32_t color) {
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭时一律不画
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return;
    if (g_clip_on && (x < g_clip_x0 || y < g_clip_y0 || x >= g_clip_x1 || y >= g_clip_y1)) return;
    if (backbuf) {
        backbuf[y * g_render_w + x] = color;   // 全部绘制到后备缓冲（渲染分辨率）
    }
}

void fb_putpixel(int x, int y, uint32_t color) { putpx(x, y, color); }

void fb_clear(uint32_t color) {
    if (!backbuf) return;
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭（清屏也算绘制）
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
    if (!g_kernel_paint64) return;              // ★ A1：内核侧绘制关闭（RGBA 直写后备缓冲也算绘制）
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

// ★ 驱动线 3：读 LFB 的像素（自检用；只支持 32bpp，别的 bpp 返回 0）。
uint32_t fb_lfb_pixel64(int x, int y) {
    if (!fb_addr || x < 0 || y < 0 || x >= fb_w || y >= fb_h) return 0;
    if (fb_bpp != 32) return 0;
    return ((volatile uint32_t*)(uintptr_t)fb_addr)[y * (fb_pitch / 4) + x];
}

uint32_t fb_get_pixel(int x, int y) {
    if (x < 0 || y < 0 || x >= g_render_w || y >= g_render_h) return 0;
    if (backbuf) return backbuf[y * g_render_w + x];
    if (fb_bpp == 32) return *(volatile uint32_t*)(fb_addr + y * fb_pitch + x * 4);
    return 0;
}

// ==================== ★ 用户计划 ⑭：VSync + 交换链 + 混合渲染（GPU 主 / CPU 次）====================
// 全部行为由 QEMU fw_cfg 的 "opt/vimtu/vsync" 配置串开启（缺省：立刻返回 0，零打点零副作用，
// 既有启动路径一个字节都不变）。为什么用 fw_cfg 当配置通道：验收脚本只能改 QEMU 命令行
// （-fw_cfg）；本仓库的 loader 不传 cmdline，fw_cfg 端口 0x510/0x511 在 QEMU 上恒在，
// 没有该通道的机器（VMware/真机）读回 0xFF -> 条目数越界 -> 直接放弃（有界）。
//
// 配置串（';' 分隔；值里不许有逗号 —— QEMU 的 -fw_cfg 按逗号切属性）：
//   demo=1              开帧引擎（缺省 0 = 关）
//   bufs=2|3            交换链块数（缺省 2）
//   gpu=auto|on|off     设备路径策略（缺省 auto = 有设备就用）
//   frames=N            跑多少帧（缺省 240）
//   fps=N               目标刷新率（缺省 60）
//   inject=none|timeout|illegal|gone   故障注入种类（缺省 none）
//   inj_at=N            第几个 virtio-gpu 命令之后开始注入（缺省 120）
//   bench=0|1           末尾做"设备 vs 软件整帧提交"耗时对比（缺省 1）

// ---- 打点小工具（非内联：一份实现。体积纪律与 virtio_gpu64.cpp 相同）----
__attribute__((noinline)) static void fb64_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void fb64_num(uint64_t v) { dbg64_dec(v); }
static void fb64_log_begin() { dbg64_line_begin64(); }
static void fb64_log_end() { dbg64_nl(); dbg64_line_end64(); }

static inline uint64_t fb64_rdtsc(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
static int fb64_irqs_on(void) {                       // hlt 只在 IF=1 时用（否则会睡死）
    uint64_t fl = 0;
    __asm__ volatile("pushfq; popq %0" : "=r"(fl));
    return (fl & 0x200ull) ? 1 : 0;
}

// ---------- ⑭-1 配置通道：fw_cfg 文件目录（键 0x19）+ 一条字符串文件 ----------
// 【实测踩坑】selector 是"大端 16 位"，但 0x510/0x511 两个字节里低字节在 0x511（与数据口
// 重叠）；QEMU 11 对"两次 8 位写"的解释跟老版本/文档写法不一定一致（本批第一次实测就没读到
// 目录：读回的条目数越界 -> 直接放弃 -> 帧引擎一行不打）。所以这里**四种写法都试一遍**，
// 谁能让目录条目数落到 1..64 就固定用它（每种只读 4 字节，有界，绝不挂）。
static uint8_t fb64_fwcfg_rd8(void) { return inb(0x511); }
static uint32_t fb64_fwcfg_rd32(void) {               // fw_cfg 的数字字段一律大端
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | (uint32_t)fb64_fwcfg_rd8();
    return v;
}
static int g_fwcfg_variant = -1;                      // -1 = 还没探测出来
// ★ 真缺陷修复（本批修订）：原来"选条目"和"读目录条目数（4B）"是同一个函数 —— 于是**每选一次
//   都会从数据口吃掉前 4 字节**。对目录（0x0019）这正好是条目数（本意），但对**一般文件**就是
//   文件的前 4 个字节：配置串 "demo=1;bufs=2;…" 的前 4 字节 "demo" 被吃掉 ->
//   fb64_cfg_find("demo") 在整个串里都找不到 -> `[VSYNC] cfg present but demo=0` ->
//   **帧引擎在任何机器上都起不来**（①~④ 的实测数据一个都拿不到；实测：加 4 字节前缀
//   "xxxx;demo=1;…" 就正常起来 —— 那就是本缺陷的硬证据）。
//   现在拆开："只切换"与"切换+读条目数"，探测/读目录走后者（语义不变），读一般文件走前者。
static void fb64_fwcfg_sel_write(int variant, uint16_t sel) {
    switch (variant) {
        case 0: outb(0x510, (uint8_t)(sel >> 8)); outb(0x510, (uint8_t)(sel & 0xFF)); break;
        case 1: outb(0x510, (uint8_t)(sel & 0xFF)); outb(0x510, (uint8_t)(sel >> 8)); break;
        case 2: outw(0x510, (uint16_t)((sel << 8) | (sel >> 8))); break;
        default: outw(0x510, sel); break;
    }
}
static int fb64_fwcfg_sel_try(int variant, uint16_t sel) {
    fb64_fwcfg_sel_write(variant, sel);
    const uint32_t cnt = fb64_fwcfg_rd32();           // 目录首 4 字节 = 条目数（大端）
    return (cnt >= 1 && cnt <= 64) ? (int)cnt : -1;
}
static void fb64_fwcfg_select(uint16_t sel) {         // 用已确定的那种写法选条目
    fb64_fwcfg_sel_write(g_fwcfg_variant, sel);       // ★ 不读数据口：第 0 字节 = 文件第 0 字节
}
// 探测：返回 1 = 找到可用的写法（g_fwcfg_variant 固定下来），0 = 这台机器没有可读的 fw_cfg
static int fb64_fwcfg_probe(void) {
    for (int v = 0; v < 4; v++) {
        if (fb64_fwcfg_sel_try(v, 0x0019) > 0) { g_fwcfg_variant = v; return 1; }
    }
    return 0;
}
// 找一条名字匹配的文件（name = "opt/vimtu/vsync"）。返回 1 = 找到（*sel/*size 填好）。
// 有界：条目数上界 64、每条固定 64 字节 -> 最多 64 次。
static int fb64_fwcfg_find(const char* name, uint16_t* sel, uint32_t* size) {
    if (g_fwcfg_variant < 0 && !fb64_fwcfg_probe()) return 0;
    const int cnt = fb64_fwcfg_sel_try(g_fwcfg_variant, 0x0019);
    if (cnt < 1) return 0;
    for (int i = 0; i < cnt; i++) {
        const uint32_t sz = fb64_fwcfg_rd32();                   // 文件长度（大端）
        const uint16_t s = (uint16_t)(((uint32_t)fb64_fwcfg_rd8() << 8) | (uint32_t)fb64_fwcfg_rd8());
        (void)fb64_fwcfg_rd8(); (void)fb64_fwcfg_rd8();          // reserved（2B）
        char nm[56];
        for (int k = 0; k < 56; k++) nm[k] = (char)fb64_fwcfg_rd8();
        nm[55] = 0;                                              // 保证有结尾
        int eq = 1;
        for (int k = 0; ; k++) {
            if (nm[k] != name[k]) { eq = 0; break; }
            if (!name[k]) break;
        }
        if (eq) { *sel = s; *size = sz; return 1; }
    }
    return 0;
}
static char g_vs_cfg[192];
static int  fb64_strlen(const char* s) { int n = 0; while (s[n]) n++; return n; }
static void fb64_cfg_load(void) {
    g_vs_cfg[0] = 0;
    uint16_t sel = 0; uint32_t sz = 0;
    if (!fb64_fwcfg_find("opt/vimtu/vsync", &sel, &sz)) return;
    if (sz == 0 || sz > sizeof(g_vs_cfg) - 1) return;
    fb64_fwcfg_select(sel);
    int n = 0;
    for (uint32_t i = 0; i < sz; i++) {
        const char c = (char)fb64_fwcfg_rd8();
        if (!c) break;
        g_vs_cfg[n++] = c;
    }
    g_vs_cfg[n] = 0;
}
static const char* fb64_cfg_find(const char* key) {
    const int kl = fb64_strlen(key);
    const char* p = g_vs_cfg;
    while (*p) {
        int m = 1;
        for (int i = 0; i < kl; i++) if (p[i] != key[i]) { m = 0; break; }
        if (m && p[kl] == '=') return p + kl + 1;
        while (*p && *p != ';') p++;
        if (*p == ';') p++;
    }
    return nullptr;
}
static int fb64_cfg_int(const char* key, int def) {
    const char* p = fb64_cfg_find(key);
    if (!p) return def;
    int v = 0, any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (int)(*p - '0'); p++; any = 1; }
    return any ? v : def;
}
static int fb64_cfg_str(const char* key, const char* val) {      // 值到 ';' 或串尾为止
    const char* p = fb64_cfg_find(key);
    if (!p) return 0;
    for (int i = 0; ; i++) {
        const char a = p[i];
        const char ea = (a == ';' || a == 0) ? 0 : a;
        if (ea != val[i]) return 0;
        if (!val[i]) break;
    }
    return 1;
}

// ---------- ⑭-2 交换链（后备缓冲切成 2..3 块；整帧提交）----------
#define FB64_SWAP_MAX 3
static uint32_t* g_sw_slot[FB64_SWAP_MAX] = { nullptr, nullptr, nullptr };
static int g_sw_n = 1;          // 块数（1 = 没开：行为与改动前完全一致）
static int g_sw_front = 0;      // 在屏上的块
static int g_sw_back = 0;       // 当前绘制块（== backbuf）
static int g_sw_allow_gpu = 1;  // 帧引擎允许设备路径（gpu=off -> 0）

int fb_swap_count64() { return g_sw_n; }
int fb_swap_front64() { return g_sw_front; }

int fb_swap_enable64(int n) {
    const int w = fb_width(), h = fb_height();
    if (w <= 0 || h <= 0) return 1;
    const uint64_t frame_px = (uint64_t)w * (uint64_t)h;
    int maxn = (int)((uint64_t)(sizeof(backbuf_storage) / sizeof(backbuf_storage[0])) / frame_px);
    if (maxn > FB64_SWAP_MAX) maxn = FB64_SWAP_MAX;
    if (n > maxn) n = maxn;
    if (n < 2) { g_sw_n = 1; backbuf = backbuf_storage; return 1; }
    g_sw_n = n;
    for (int i = 0; i < n; i++) g_sw_slot[i] = backbuf_storage + (uint64_t)i * frame_px;
    g_sw_front = 0;                    // 0 号块就是"当前在屏上的内容"（设备路径 = primary resource）
    g_sw_back = 1;                     // 先画 1 号块 —— 绝不动正在被扫描出的那块
    backbuf = g_sw_slot[g_sw_back];
    return n;
}

// 软件路径整帧提交：**原地**写 LFB（Bochs VBE 没有页翻转原语；宿主扫描可能看到中间态 -> 如实标）
static void fb64_present_soft(int slot) {
    if (!fb_addr) return;
    uint32_t* src = g_sw_slot[slot];
    if (!src) return;
    const int w = fb_w, h = fb_h;
    if (g_zoom != 100) {                       // 缩放：走既有整帧路径（按 render 尺寸放大）
        uint32_t* save = backbuf;
        backbuf = src;
        fb_flip_all64();
        backbuf = save;
        return;
    }
    if (fb_bpp == 32 && fb_pitch == w * 4) {
        uint32_t* dst = (uint32_t*)fb_addr;
        const uint32_t n = (uint32_t)w * (uint32_t)h;
        for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    } else {
        for (int y = 0; y < h; y++) {
            volatile uint8_t* row = (volatile uint8_t*)(fb_addr + y * fb_pitch);
            for (int x = 0; x < w; x++) {
                const uint32_t c = src[(uint64_t)y * (uint64_t)w + (uint64_t)x];
                if (fb_bpp == 32) ((volatile uint32_t*)row)[x] = c;
                else if (fb_bpp == 24) { row[x * 3] = c & 0xFF; row[x * 3 + 1] = (c >> 8) & 0xFF; row[x * 3 + 2] = (c >> 16) & 0xFF; }
                else ((volatile uint16_t*)row)[x] = (uint16_t)(((c >> 16 & 0xF8) << 8) | ((c >> 8 & 0xFC) << 3) | (c >> 3));
            }
        }
    }
    vgpu64_note_soft_blit64(0, 0, w, h);
}

static int g_sw_degrade_logged = 0;

// 整帧提交当前绘制块：设备路径 = 整帧 TRANSFER+FLUSH（+ 需要时 SET_SCANOUT 翻页）；
// 失败/不可用 -> 本帧自动走软件路径（**继续出帧**，绝不 PANIC / 绝不挂死）。
int fb_frame_present64() {
    if (g_sw_n < 2) { fb_flip(); return 0; }
    const int slot = g_sw_back;
    int dev = 0;
    if (g_sw_allow_gpu && vgpu64_ready64()) {
        dev = vgpu64_present64(slot, fb_width(), fb_height()) ? 1 : 0;
        if (!dev && !g_sw_degrade_logged) {
            g_sw_degrade_logged = 1;
            fb64_log_begin();
            fb64_puts("[FB64] present dev=0 -> soft-lfb (帧引擎自动降级，继续出帧)");
            fb64_log_end();
        }
    }
    if (!dev) fb64_present_soft(slot);
    g_sw_front = slot;
    g_sw_back = (slot + 1) % g_sw_n;
    backbuf = g_sw_slot[g_sw_back];
    return dev;
}

// ---------- ⑭-3 帧节拍：硬件回扫（有则用）/ 节拍器（估计 + 粗睡 + 细自旋）----------
static uint64_t g_vs_cyc_per_ms = 0;
static uint64_t fb64_calib_cyc_per_ms(void) {
    const uint64_t t0 = g_ticks64, c0 = fb64_rdtsc();
    uint32_t spin = 0;
    while (g_ticks64 - t0 < 20 && ++spin < 200000000u) __asm__ volatile("pause");   // 80 ms
    const uint64_t dt = g_ticks64 - t0;
    if (!dt) return 0;
    return (fb64_rdtsc() - c0) * (uint64_t)TICK_MS_64 / dt;
}
// VGA 输入状态寄存器 1（0x3DA）bit3 = 垂直回扫/垂直同步期
static inline int fb64_vga_vretrace(void) { return (inb(0x3DA) & 0x08) ? 1 : 0; }
// 实测 200 ms 里的上升沿数；返回 1 = 信号可信（能用硬件节拍）
static int fb64_vblank_probe(uint64_t* edges_out, uint64_t* ms_out) {
    const uint64_t t0 = g_ticks64;
    uint64_t edges = 0;
    int last = fb64_vga_vretrace();
    uint32_t spin = 0;
    while (g_ticks64 - t0 < 50) {                       // 50 tick = 200 ms
        const int v = fb64_vga_vretrace();
        if (v && !last) edges++;
        last = v;
        if (++spin > 200000000u) break;                 // 有界
    }
    const uint64_t dt = g_ticks64 - t0;
    *edges_out = edges;
    const uint64_t ms = dt * (uint64_t)TICK_MS_64;
    *ms_out = ms;
    if (dt == 0) return 0;
    const uint64_t hz = edges * 1000ull / ms;
    return (edges >= 5 && hz >= 25 && hz <= 300) ? 1 : 0;      // 25..300 Hz 才算可信
}
// 等下一次垂直回扫**起点**（先等当前回扫结束，再等下一个上升沿）。有界（2 tick = 8 ms 上限）。
static int fb64_vblank_wait(void) {
    const uint64_t t0 = g_ticks64;
    uint32_t spin = 0;
    while (fb64_vga_vretrace()) {                       // ① 当前回扫期内：等它结束
        if (g_ticks64 - t0 > 2 || ++spin > 20000000u) return 0;
    }
    spin = 0;
    while (!fb64_vga_vretrace()) {                      // ② 等下一次回扫开始（= 新一帧的起点）
        if (g_ticks64 - t0 > 2 || ++spin > 20000000u) return 0;
    }
    return 1;
}
// 节拍器：粗等用 hlt 睡到"还剩 1 ms"，细等用 rdtsc 自旋（对齐到目标时刻）
static void fb64_wait_until(uint64_t deadline_cyc) {
    if (fb64_irqs_on()) {
        for (int guard = 0; guard < 64; guard++) {
            const uint64_t now = fb64_rdtsc();
            if ((int64_t)(deadline_cyc - now) <= (int64_t)g_vs_cyc_per_ms) break;
            __asm__ volatile("hlt");
        }
    }
    uint64_t spin = 0;
    while ((int64_t)(deadline_cyc - fb64_rdtsc()) > 0) {
        __asm__ volatile("pause");
        if (++spin > 4000000000u) break;                // 兜底（时钟异常时不死循环）
    }
}

// ---------- ⑭-4 帧统计（中位数 / p95 / 抖动）+ 8 条色带图案 ----------
#define FB64_GAP_MAX 1024
static uint64_t g_vs_gap[FB64_GAP_MAX];
static int      g_vs_gap_n = 0;
static uint64_t g_vs_frames = 0;
static uint64_t g_vs_med = 0, g_vs_p95 = 0, g_vs_jit = 0;
static const char* g_vs_mode = "off";
static uint64_t g_vs_gpu_frames = 0, g_vs_cpu_frames = 0;

static uint64_t fb64_stat_med(uint64_t* a, int n) {     // 插入排序 + 取中位（一次性，n<=1024）
    for (int i = 1; i < n; i++) {
        const uint64_t v = a[i];
        int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
        a[j + 1] = v;
    }
    return n ? a[n / 2] : 0;
}
static const uint32_t g_vs_pal[8] = {
    0xFF0000u, 0x00FF00u, 0x0000FFu, 0xFFFF00u, 0xFF00FFu, 0x00FFFFu, 0xFF8000u, 0x8000FFu,
};
// 第 f 帧的图案：第 i 条带的颜色 = pal[(i + f) % 8]（帧与帧之间**整帧都变**；验收脚本靠这 8 个
// 颜色反推"这一屏属于哪一帧"——同一张截图里出现两个不同帧的带序 = 撕裂）。
static void fb64_draw_bands(uint32_t* buf, int w, int h, uint64_t f) {
    for (int b = 0; b < 8; b++) {
        const uint32_t c = g_vs_pal[(uint32_t)((b + (int)(f & 7ull)) & 7)];
        const int y0 = (int)((int64_t)h * b / 8);
        const int y1 = (int)((int64_t)h * (b + 1) / 8);
        for (int y = y0; y < y1; y++) {
            uint32_t* row = buf + (uint64_t)y * (uint64_t)w;
            for (int x = 0; x < w; x++) row[x] = c;
        }
    }
}

static const char* fb64_inj_name(int k) {
    switch (k) {
        case 1: return "timeout";
        case 2: return "illegal";
        case 3: return "gone";
        default: return "none";
    }
}

// 末尾对比：同一帧内容，设备整帧提交 vs 软件整帧提交（rdtsc 多轮中位数）
static void fb64_bench_present(int slot, int w, int h, int n_runs) {
    uint64_t dev_cyc[9], soft_cyc[9];
    if (n_runs > 9) n_runs = 9;
    for (int i = 0; i < n_runs; i++) {
        uint64_t c0 = fb64_rdtsc();
        const int ok = (g_sw_allow_gpu && vgpu64_ready64()) ? vgpu64_present64(slot, w, h) : 0;
        dev_cyc[i] = ok ? (fb64_rdtsc() - c0) : 0;
        c0 = fb64_rdtsc();
        fb64_present_soft(slot);
        soft_cyc[i] = fb64_rdtsc() - c0;
    }
    const uint64_t md = fb64_stat_med(dev_cyc, n_runs);
    const uint64_t ms_ = fb64_stat_med(soft_cyc, n_runs);
    const uint64_t to_us = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms / 1000ull) : 0;
    fb64_log_begin();
    fb64_puts("[VSYNC] bench present_n="); fb64_num((uint64_t)n_runs);
    fb64_puts(" px="); fb64_num((uint64_t)w * (uint64_t)h);
    if (to_us) {
        fb64_puts(" gpu_med_us="); fb64_num(md / to_us);
        fb64_puts(" soft_med_us="); fb64_num(ms_ / to_us);
        fb64_puts(" ratio_x100="); fb64_num(ms_ ? (md * 100ull / ms_) : 0);
    } else {
        fb64_puts(" gpu_med_cyc="); fb64_num(md);
        fb64_puts(" soft_med_cyc="); fb64_num(ms_);
    }
    fb64_log_end();
}

// ==================== 帧引擎主体 ====================
__attribute__((noinline)) static int fb_vsync_engine64(int bufs, int gpu_mode, int frames,
                                                       int fps, int bench_on) {
    const int w = fb_width(), h = fb_height();
    const int n_sw = fb_swap_enable64(bufs);
    if (n_sw < 2) {
        fb64_log_begin();
        fb64_puts("[VSYNC] swap unavailable frames=1 (后备缓冲放不下两块) -> skip");
        fb64_log_end();
        return 0;
    }
    g_sw_allow_gpu = (gpu_mode == 0) ? 0 : 1;          // gpu=off 强制软件路径
    // 设备路径：设备可用 + 交换链每块一个 resource（SET_SCANOUT 翻页）
    int dev_frames = g_sw_allow_gpu ? vgpu64_swap_init64(n_sw) : 0;
    if (dev_frames < 2) dev_frames = 0;
    const int dev_on = dev_frames ? 1 : 0;
    if (!dev_on) g_sw_allow_gpu = 0;                   // 交换链不成立就别每帧白试设备
    if (gpu_mode == 1 && !dev_on) {
        fb64_log_begin();
        fb64_puts("[VSYNC] gpu=on but device/swap unavailable -> cpu path (如实降级)");
        fb64_log_end();
    }
    if (fps < 1) fps = 60;
    if (fps > 240) fps = 240;
    const uint64_t target_us = 1000000ull / (uint64_t)fps;
    const uint64_t period_cyc = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms * 1000ull / (uint64_t)fps) : 0;

    // 帧节拍来源：先探硬件垂直回扫；没有就标 mode=pace（按刷新率估计 + 睡眠/自旋混合）
    uint64_t vb_edges = 0, vb_ms = 0;
    const int hw = fb64_vblank_probe(&vb_edges, &vb_ms);
    g_vs_mode = hw ? "hw" : "pace";

    fb64_log_begin();
    fb64_puts("[VSYNC] demo start mode="); fb64_puts(g_vs_mode);
    fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
    fb64_puts(" gpu="); fb64_puts(gpu_mode == 1 ? "on" : (gpu_mode == 0 ? "off" : "auto"));
    fb64_puts(" backend="); fb64_puts(vgpu64_backend_name64());
    fb64_puts(" dev_frames="); fb64_num((uint64_t)dev_frames);
    fb64_puts(" target_us="); fb64_num(target_us);
    fb64_puts(" frames="); fb64_num((uint64_t)frames);
    fb64_puts(" vblank_edges="); fb64_num(vb_edges);
    fb64_puts(" vblank_ms="); fb64_num(vb_ms);
    fb64_puts(" in_place="); fb64_num(dev_on ? 0ull : 1ull);
    fb64_log_end();

    uint64_t t_next = fb64_rdtsc();
    uint64_t t_prev = 0;
    int path_prev = -1;
    const uint64_t start_cyc = fb64_rdtsc();
    const uint64_t to_us = g_vs_cyc_per_ms ? (g_vs_cyc_per_ms / 1000ull) : 0;
    for (int f = 0; f < frames; f++) {
        if (hw) (void)fb64_vblank_wait();
        else if (period_cyc) fb64_wait_until(t_next);
        const uint64_t t0 = fb64_rdtsc();
        fb64_draw_bands(backbuf, w, h, (uint64_t)f);            // CPU：逻辑 + 像素（8 条色带整体变化）
        const uint64_t t1 = fb64_rdtsc();
        const int dev = fb_frame_present64();                   // 整帧提交（GPU 优先 / 失败降级 CPU）
        const uint64_t t2 = fb64_rdtsc();
        const uint64_t gap = t_prev ? (t0 - t_prev) : 0;
        if (gap && g_vs_gap_n < FB64_GAP_MAX) g_vs_gap[g_vs_gap_n++] = to_us ? (gap / to_us) : gap;
        t_prev = t0;
        g_vs_frames++;
        if (dev) g_vs_gpu_frames++; else g_vs_cpu_frames++;
        // 逐帧打点（有界：头 4 帧 + 每 100 帧 + 最后一帧）
        if (f < 4 || (f % 100) == 0 || f == frames - 1) {
            fb64_log_begin();
            fb64_puts("[VSYNC] frame n="); fb64_num((uint64_t)f);
            fb64_puts(" buf="); fb64_num((uint64_t)g_sw_front);
            fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
            fb64_puts(" who="); fb64_puts(dev ? "gpu" : "cpu");
            if (to_us) {
                fb64_puts(" draw_us="); fb64_num((t1 - t0) / to_us);
                fb64_puts(" pres_us="); fb64_num((t2 - t1) / to_us);
                fb64_puts(" gap_us="); fb64_num(gap / to_us);
            }
            fb64_puts(" mode="); fb64_puts(g_vs_mode);
            fb64_log_end();
        }
        // 降级事件（GPU -> CPU 的路径切换）：只打一次，随后继续出帧
        const int path_now = dev ? 1 : 0;
        if (path_prev == 1 && path_now == 0) {
            fb64_log_begin();
            fb64_puts("[VSYNC] degrade frame="); fb64_num((uint64_t)f);
            fb64_puts(" reason="); fb64_puts(vgpu64_info64()->last_err);
            fb64_puts(" -> path=soft backend="); fb64_puts(vgpu64_backend_name64());
            fb64_puts(" (继续出帧)");
            fb64_log_end();
        }
        path_prev = path_now;
        t_next += period_cyc ? period_cyc : 1;
        if ((int64_t)(t_next - fb64_rdtsc()) < 0) t_next = fb64_rdtsc();   // 落后了就重锚
    }
    const uint64_t end_cyc = fb64_rdtsc();

    // ---- 统计：帧间隔中位数 / p95 / 抖动（相对中位数的平均绝对偏差）----
    if (g_vs_gap_n > 0) {
        const int n = g_vs_gap_n;
        g_vs_med = fb64_stat_med(g_vs_gap, n);
        int i95 = (int)((int64_t)n * 95 / 100);
        if (i95 >= n) i95 = n - 1;
        if (i95 < 0) i95 = 0;
        g_vs_p95 = g_vs_gap[i95];
        uint64_t sum = 0;
        for (int i = 0; i < n; i++) sum += (g_vs_gap[i] > g_vs_med) ? (g_vs_gap[i] - g_vs_med) : (g_vs_med - g_vs_gap[i]);
        g_vs_jit = sum / (uint64_t)n;
    }
    fb64_log_begin();
    fb64_puts("[VSYNC] stats frames="); fb64_num(g_vs_frames);
    fb64_puts(" bufs="); fb64_num((uint64_t)n_sw);
    fb64_puts(" mode="); fb64_puts(g_vs_mode);
    fb64_puts(" target_us="); fb64_num(target_us);
    fb64_puts(" med_us="); fb64_num(g_vs_med);
    fb64_puts(" p95_us="); fb64_num(g_vs_p95);
    fb64_puts(" jitter_us="); fb64_num(g_vs_jit);
    fb64_puts(" samples="); fb64_num((uint64_t)g_vs_gap_n);
    fb64_puts(" gpu_frames="); fb64_num(g_vs_gpu_frames);
    fb64_puts(" cpu_frames="); fb64_num(g_vs_cpu_frames);
    if (to_us && frames > 0) {
        fb64_puts(" avg_frame_us="); fb64_num((end_cyc - start_cyc) / to_us / (uint64_t)frames);
    }
    fb64_log_end();

    // ---- 收尾：把最后画好的那一帧交回 0 号块（primary resource 绑的就是它）----
    uint32_t* last = g_sw_slot[g_sw_front];
    if (last && last != backbuf_storage) {
        const uint32_t npx = (uint32_t)w * (uint32_t)h;
        for (uint32_t i = 0; i < npx; i++) backbuf_storage[i] = last[i];
    }
    backbuf = backbuf_storage;
    g_sw_n = 1; g_sw_front = 0; g_sw_back = 0;
    if (bench_on) fb64_bench_present(0, w, h, 5);
    int ok0 = 0;
    if (gpu_mode != 0 && vgpu64_ready64()) ok0 = vgpu64_present64(0, w, h);
    if (!ok0) fb64_present_soft(0);
    fb64_log_begin();
    fb64_puts("[VSYNC] demo done frames="); fb64_num(g_vs_frames);
    fb64_puts(" backend="); fb64_puts(vgpu64_backend_name64());
    fb64_log_end();
    return (int)g_vs_frames;
}

const char* fb_vsync_mode64() { return g_vs_mode; }
void fb_vsync_stats64(uint64_t* frames, uint64_t* med_us, uint64_t* p95_us, uint64_t* jit_us) {
    if (frames) *frames = g_vs_frames;
    if (med_us) *med_us = g_vs_med;
    if (p95_us) *p95_us = g_vs_p95;
    if (jit_us) *jit_us = g_vs_jit;
}

// 入口：读配置 -> 设注入 -> 跑帧引擎（没有 fw_cfg 配置时一行不打、直接返回 0）
int fb_vsync_run64() {
    fb64_cfg_load();
    if (g_vs_cfg[0] == 0) return 0;
    const int bufs = fb64_cfg_int("bufs", 2);
    const int gpu_mode = fb64_cfg_str("gpu", "off") ? 0 : (fb64_cfg_str("gpu", "on") ? 1 : 2);
    const int frames = fb64_cfg_int("frames", 240);
    const int fps = fb64_cfg_int("fps", 60);
    const int bench_on = fb64_cfg_str("bench", "0") ? 0 : 1;
    int inj = 0;
    if (fb64_cfg_str("inject", "timeout")) inj = 1;
    else if (fb64_cfg_str("inject", "illegal")) inj = 2;
    else if (fb64_cfg_str("inject", "gone")) inj = 3;
    const int inj_at = fb64_cfg_int("inj_at", 120);
    if (!fb64_cfg_int("demo", 0)) {
        fb64_log_begin();
        fb64_puts("[VSYNC] cfg present but demo=0 -> skip bufs="); fb64_num((uint64_t)bufs);
        fb64_log_end();
        return 0;
    }
    if (!g_vs_cyc_per_ms) g_vs_cyc_per_ms = fb64_calib_cyc_per_ms();
    fb64_log_begin();
    fb64_puts("[VSYNC] cfg bufs="); fb64_num((uint64_t)bufs);
    fb64_puts(" gpu="); fb64_puts(gpu_mode == 1 ? "on" : (gpu_mode == 0 ? "off" : "auto"));
    fb64_puts(" frames="); fb64_num((uint64_t)frames);
    fb64_puts(" fps="); fb64_num((uint64_t)fps);
    fb64_puts(" inject="); fb64_puts(fb64_inj_name(inj));
    fb64_puts(" inj_at="); fb64_num((uint64_t)inj_at);
    fb64_puts(" bench="); fb64_num((uint64_t)bench_on);
    fb64_puts(" cyc_per_ms="); fb64_num(g_vs_cyc_per_ms);
    fb64_log_end();
    if (inj) vgpu64_set_inject64(inj, (uint32_t)inj_at);
    return fb_vsync_engine64(bufs, gpu_mode, frames, fps, bench_on);
}
