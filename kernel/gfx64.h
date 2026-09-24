// gfx64.h - 现代图元层（Windows 11 风格）：圆角 + 双层阴影 + 毛玻璃(亚克力) + 渐变 + 壁纸适应模式
//
// 设计要点（都对着需求原文写，改动前先读）：
//   1) **一切尺寸/半径/透明/时长都从 theme64.h 的 Token 取**，本文件不写死视觉数字（除了算法的
//      采样密度这类"实现细节"：4x4 亚像素覆盖、2 趟盒子模糊等）。
//   2) **毛玻璃必须缓存**（QEMU TCG 无硬件加速）：
//        * 壁纸面（g_wall_surface）= 按适应模式铺好的整屏 RGBA，只在 主题/模式/壁纸源/分辨率 变化时重建；
//        * 墙纸模糊（g_wall_blur）= 对壁纸面整体做一次背景层模糊（r = Token 24），同一次重建里做，
//          绝不在每帧重算 —— 这是"墙纸模糊算一次"的落点；
//        * 内容层玻璃（标题栏/卡片）走按需的小区域模糊缓存（LRU，key = 矩形+半径），
//          只在移动/尺寸变化/内容变化时重算。
//   3) 阴影是**预生成的 alpha mask**（近层/远层各一张，key = 矩形+圆角，LRU 复用），
//      半分辨率生成 + 最近邻放大（阴影是低频渐变，肉眼无差，省一半以上带宽）。
//   4) 所有写像素都走 fb 后备缓冲（fb_surface64）+ fb 裁剪矩形，脏矩形之外不落笔。
//
// 串口打点（自动验收 grep；带防刷屏上限）：
//   [GFX64] init surface=1280x800 wall=1200x900 src=builtin(default wallpaper)
//   [GFX64] wall mode=0 name=fill src=1200x900 screen=1280x800 dst=-,-,1280x960 scale=1066/1000 crop=l0,t60,r0,b60 tiles=1x1 base=#F3F4F6 ms=18
//   [GFX64] wall markers r=96,96 g=1088,96 b=96,788 y=1088,788 size=16
//   [GFX64] wall blur once r=24 src=1280x800 ms=180
//   [GFX64] blur tile x=240 y=730 wh=32x32 var_src=.. var_dst=.. (真模糊证据)
//   [GFX64] shadow mask build w=600 h=60 r=24 ds=2 ms=..
//   [GFX64] glass x=340 y=724 600x60 r=24 layer=backdrop hit=1 tint=#F3F3F3 a=115
//   [GFX64] cache blur_hit=.. blur_miss=.. shadow_hit=.. shadow_miss=..
#pragma once
#include <stdint.h>
#include "theme64.h"

// ==================== 壁纸适应模式（6 种；默认 填充）====================
#define GFX64_WALL_FILL     0     // 填充：等比缩放 + 裁剪（默认，不拉伸变形）
#define GFX64_WALL_FIT      1     // 适应：等比缩放，整张可见，留白
#define GFX64_WALL_STRETCH  2     // 拉伸：铺满（允许变形）
#define GFX64_WALL_TILE     3     // 平铺：1:1 重复
#define GFX64_WALL_CENTER   4     // 居中：1:1 居中，超出裁掉
#define GFX64_WALL_SPAN     5     // 跨屏：按虚拟桌面（单屏时等价填充）
#define GFX64_WALL_MODE_COUNT 6

const char* gfx64_wall_mode_name64(int m);
int  gfx64_wall_mode64();
void gfx64_wall_set_mode64(int mode, const char* why);   // 立即生效 + 持久化 + 打点
int  gfx64_wall_tick64();                                // 每帧：外部改 config 时实时生效

// ==================== 壁纸源 ====================
// 设置壁纸源（RGBA 0x00RRGGBB 或 0xFFRRGGBB，w*h）。copy=1 时本模块自己留一份（调用方可释放）。
int   gfx64_wall_set_source64(const uint32_t* px, int w, int h, int copy, const char* from);
void  gfx64_wall_build_default64();                      // 内置兜底壁纸（渐变+柔光斑+细颗粒+4 个定位标记）
int   gfx64_wall_src_w64();
int   gfx64_wall_src_h64();
const char* gfx64_wall_src_desc64();                     // "builtin" / "vfs:/wallpaper.png" ...
void  gfx64_wall_invalidate64();                         // 主题/模式/源/分辨率变了 → 下次绘制重建
void  gfx64_wall_draw64(int x, int y, int w, int h);      // 把壁纸画进后备缓冲（只动这个矩形）
const char* gfx64_wall_last_geom64();                    // 最近一次 [GFX64] wall 打点原文（验收用）

// ==================== 基础图元 ====================
void gfx64_blend64(int x, int y, uint32_t c, int a);                       // 单像素 alpha 混合（受裁剪）
void gfx64_fill_round64(int x, int y, int w, int h, int r, uint32_t c, int a);
void gfx64_grad_round64(int x, int y, int w, int h, int r, uint32_t c0, uint32_t c1, int diag, int a);
void gfx64_stroke_round64(int x, int y, int w, int h, int r, uint32_t c, int a);
// 双层浅阴影（近层 0/2/4 a=0.08、远层 0/12/32 a=0.12，来自 Token）
void gfx64_shadow64(int x, int y, int w, int h, int r, const Theme64Tokens* t);
// 毛玻璃(亚克力)面板：layer 0 = 背景层（预模糊壁纸，r=Token 24）/ 1 = 内容层（按需模糊缓存，r=Token 12）
void gfx64_glass64(int x, int y, int w, int h, int r, int layer, uint32_t tint, int alpha,
                   uint32_t border_color, int border_alpha, const Theme64Tokens* t);
const uint32_t* gfx64_wall_surface64(int* w, int* h);   // 铺好的整屏壁纸面（只读）
const uint32_t* gfx64_wall_blur64(int* w, int* h);      // 墙纸模糊一次的结果（背景层玻璃的 backdrop）

// 圆角裁切：把 (x,y,w,h,r) 的“圆角外”像素用 backdrop 采样覆盖回来（方形客户区做圆角窗口用）
void gfx64_corner_cut64(int x, int y, int w, int h, int r, int layer);

// ==================== 度量 / 报告（验收用）====================
// 局部方差（只算绿通道，放大 100 倍返回；用于"模糊后方差显著低于原图"）
uint64_t gfx64_var64(const uint32_t* px, int stride, int x, int y, int w, int h);
void gfx64_report64(const char* tag);
