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
// ★ 驱动线 3：直接读 **LFB**（物理显存）里的像素 —— 软件路径的产物在这里，设备路径的产物在
//   resource 里。自检里的"设备 vs 软件逐字节等价"拿这一份和 TRANSFER_FROM_HOST_2D 回读的那份比。
uint32_t fb_lfb_pixel64(int x, int y);

// ==================== ★ 驱动线 3：显示后端选择（virtio-gpu 2D 设备路径 vs 软件 LFB 路径）====================
// 语义：**没有 virtio-gpu（或初始化失败/运行期降级）时，下面这些全是空操作**，
//   所有上屏仍原样走既有软件路径（CPU 逐行写 LFB）—— 行为与改动前完全一致。
// 后端名的唯一真源在 kernel/virtio_gpu64.cpp（"virtio-gpu-2d" / "soft-lfb"）。
const char* fb_backend_name64();
// 打一行 "[FB64] backend=<...>"（virtio_gpu64 在初始化完成/降级时调用；启动期一次）
void fb_backend_log64(const char* backend);
// **强制**软件路径的区域提交（不受后端选择影响）：
//   设备自检的第一段要用它做出"软件路径"的屏幕内容（那段里 scanout 还没设，屏幕上就是 LFB）。
void fb_soft_flip_region64(int x, int y, int w, int h);
// ==================== A1：用户态绘图（映射显存 + 提交区域）====================
// 目标：**ring3 程序自己把画面画到屏幕上**，内核只做两件事：把后备缓冲映射进用户地址空间、
// 把用户画好的区域提交到 LFB（见 kernel/syscall64.cpp 的 fb_map(9) / fb_flip(10) / fb_present(11)）。
//
// 后备缓冲的**物理基址**（4KB 对齐）+ 字节数（out_bytes 可为 nullptr）。为什么需要它：
//   内核跑在高半区（VA = 0xFFFFFFFF80000000 + PA），而映射给用户的是**物理页**；
//   backbuf_storage 带 aligned(4096)，所以返回值可以直接逐页映射，不用做页内偏移。
uint64_t fb_surface_phys64(int* out_bytes);
// A1 演示开关：0 = **关掉内核侧绘制与提交**（用户程序独占屏幕；fb_user_flip64 不受影响）。
//   由 user64_run_fbdemo64()（kernel/usermode64.cpp）在演示前后切换，进程退出/被 kill 时恢复。
void fb_kernel_paint64(int on);
int  fb_kernel_paint_on64();
// A1：用户态的区域提交（fb_flip(10) 的实现）——把后备缓冲的矩形拷到 LFB。
//   **不受**内核绘制开关影响（这是用户程序自己的提交，不是内核在画）；坐标在此再夹一次。
void fb_user_flip64(int x, int y, int w, int h);
// 颜色辅助
inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
