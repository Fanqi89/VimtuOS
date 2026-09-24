// theme64.h - VimtuOS 64 位桌面**设计 Token 唯一真源** + 主题表 + 主题切换 + 减少动画开关
//
// 本批（Windows 11 风格现代简约外观）的全部视觉数字都写在这里：
//   圆角 / 模糊半径 / 透明度 / 双层阴影 / 动效时长 / 缓动 / 字号 / 颜色。
// ★ 规矩：任何新绘制代码都**只从这里取值**，不许再往别处写死数字。
//   （老代码里的 C_DESKTOP/C_TITLE_ACT 等宏属于上一批的"Win10 风格"遗留，正随本批逐项替换。）
//
// 数字来源（用户需求原文，别改）：
//   圆角：窗口 12–16（取 14）、卡片 12、按钮 8–10（取 9）、Dock 22–26（取 24）
//   模糊：背景层 20–30（取 24）、内容层 8–16（取 12）
//   透明度：背景材质 0.35–0.55（取 0.45）、内容卡片 0.72–0.85（取 0.80）
//   阴影：近层 0 2 4 rgba(0,0,0,0.08)、远层 0 12 32 rgba(0,0,0,0.12)
//   动效：快 150ms、普通 200–250ms（取 220）、大面板 300–350ms（取 320）；缓动 cubic-bezier(0.2,0,0,1)
//   主题：白（默认）/暗/蓝白渐变/粉白渐变 + 自定义渐变（粉绿、粉紫）+ 自定义背景图片
//
// 串口打点（自动验收 grep；每类都有上限，防刷屏）：
//   [THEME64] init themes=6 theme=0 name=white dark=0 reduce_motion=0
//   [THEME64] apply theme=<id> name=<name> dark=<0/1> accent=#RRGGBB why=<原因>
//   [THEME64] motion reduce=<0/1> fast/normal/large=<ms>
//   [THEME64] switch wall mode=...
#pragma once
#include <stdint.h>

// ==================== 设计 Token（数字唯一真源）====================
// ---- 圆角（px）----
#define THEME64_R_WINDOW      14      // 窗口 12–16 → 14
#define THEME64_R_CARD        12      // 卡片 12
#include <stdint.h>
#include "fb.h"        // rgb()
#define THEME64_R_BUTTON       9      // 按钮 8–10 → 9
#define THEME64_R_DOCK        24      // Dock 22–26 → 24
#define THEME64_R_ICON        10      // Dock 图标（圆角正方形）
// ---- 模糊半径（px）----
#define THEME64_BLUR_BACKDROP 24      // 背景层（Dock/大面板）
#define THEME64_BLUR_CONTENT  12      // 内容层（标题栏/卡片）
// ---- 透明度（0..255；需求给的是 0..1）----
#define THEME64_A_BACKDROP    (45 * 255 / 100)   // 0.45 → 材质层
#define THEME64_A_CARD        (80 * 255 / 100)   // 0.80 → 内容卡片
#define THEME64_A_TITLE       (72 * 255 / 100)   // 标题栏（内容层，略低于卡片）
#define THEME64_A_BORDER      110                // 1px 玻璃边框（半透明白/黑）
#define THEME64_A_HIGHLIGHT   38                 // 暗色主题的内高光
// ---- 双层浅阴影（近/远）----
#define THEME64_SH_N_DY        2      // 近层偏移 y
#define THEME64_SH_N_BLUR      4      // 近层模糊
#define THEME64_SH_N_A        20      // 近层 alpha = 0.08*255
#define THEME64_SH_F_DY       12      // 远层偏移 y
#define THEME64_SH_F_BLUR     32      // 远层模糊
#define THEME64_SH_F_A        31      // 远层 alpha = 0.12*255
// ---- 动效（ms）----
#define THEME64_MS_FAST      150
#define THEME64_MS_NORMAL    220
#define THEME64_MS_LARGE     320
#define THEME64_MS_DOCK      260      // Dock 点击回弹
#define THEME64_EASE_C1       51      // cubic-bezier(0.2,0,0,1) 的第一控制点 x=0.2（*256）
// ---- 字号（TrueType 面 2 = simhei）----
#define THEME64_FS_NORMAL     14
#define THEME64_FS_SMALL      12
#define THEME64_FS_TITLE      14
// ---- Dock 几何（需求原文：居中靠下、离底 16px、高 60、图标 44–48、间距 10–12）----
#define THEME64_DOCK_H        60
#define THEME64_DOCK_MARGIN   16      // 离屏幕底边
#define THEME64_DOCK_ICON     46      // 图标 44–48 → 46
#define THEME64_DOCK_GAP      11      // 图标间距 10–12 → 11
#define THEME64_DOCK_PAD      9       // 面板内左右留白
#define THEME64_DOCK_HOVER_PCT 120    // 悬停放大 1.20（1.15–1.25）
#define THEME64_DOCK_NEIGH_PCT 104    // 邻位轻微让位（让位幅度 = 100→104）
#define THEME64_DOCK_DOT_W     5      // 运行中小圆点
#define THEME64_DOCK_BAR_PCT    40    // 最小化小横杠宽 = 图标宽 * 40%

// ==================== 主题 id / 名称 ====================
#define THEME64_ID_WHITE      0      // 白（默认）
#define THEME64_ID_DARK       1      // 暗
#define THEME64_ID_BLUEGRAD   2      // 蓝白渐变
#define THEME64_ID_PINKGRAD   3      // 粉白渐变
#define THEME64_ID_PINKGREEN  4      // 自定义渐变：粉绿
#define THEME64_ID_PINKPURPLE 5      // 自定义渐变：粉紫
#define THEME64_THEME_COUNT   6

// ==================== Token 结构 ====================
// 一个主题 = 一组颜色 + 两个渐变端点。几何/模糊/动效 token 是**全主题共用**的（上面的宏）。
struct Theme64Tokens {
    const char* name;
    int      dark;              // 1 = 暗色主题（Dock 走深灰半透，阴影更深 + 内高光）

    // ---- 窗口 / 内容 ----
    uint32_t win_frame;         // 窗口外框（1px 玻璃边框色）
    uint32_t title_bg;          // 标题栏玻璃底色（不透明底；实际绘制按其 alpha 与模糊背景混合）
    uint32_t title_bg_ina;      // 非活动窗口标题栏
    uint32_t title_txt;
    uint32_t client_bg;         // 客户区底色（内容卡片，0.80 alpha 见 THEME64_A_CARD）
    uint32_t text;
    uint32_t text_dim;

    // ---- 强调 / 选择 ----
    uint32_t accent;            // 主色（Dock 运行点/小横杠/选中）
    uint32_t accent2;
    uint32_t sel_bg;            // 桌面图标选中底
    uint32_t sel_border;
    uint32_t icon_txt;
    uint32_t sel_fill;          // 桌面选择框填充（半透明）
    uint32_t sel_edge;          // 桌面选择框边

    // ---- Dock ----
    uint32_t dock_bg;           // 非暗色主题 = **固定默认色**（不跟随壁纸）；暗色 = 深灰半透
    uint32_t dock_border;
    uint32_t dock_txt;
    uint32_t dock_dot;          // 运行中小圆点
    uint32_t dock_bar;          // 最小化小横杠（跟随主题强调色）
    uint32_t dock_hover_bg;

    // ---- 标题栏三按钮（老验收依赖：三按钮必须肉眼可区分）----
    uint32_t btn_bg;
    uint32_t btn_bg_hover;
    uint32_t btn_close;
    uint32_t btn_close_hover;
    uint32_t btn_glyph;

    // ---- 壁纸 / 渐变 ----
    uint32_t wall_a;            // 壁纸渐变起点（内置兜底壁纸用）
    uint32_t wall_b;            // 壁纸渐变终点
    uint32_t wall_blob;         // 壁纸上的柔光斑
    uint32_t grad_a;            // 主题渐变 A（Dock 图标底面/面板渐变）
    uint32_t grad_b;            // 主题渐变 B
    uint32_t desktop_base;      // 壁纸模式留白处的底色

    // ---- 阴影 ----
    uint32_t shadow_near;       // rgba 阴影色（含 alpha）
    uint32_t shadow_far;
    uint32_t highlight;         // 暗色主题的内高光（1px 顶边）
};

// ==================== API ====================
// 初始化：读 config64 的 ui.theme / ui.reduce_motion / ui.wall.* → 生效 + 打点。必须在 config64_init64 之后。
void theme64_init64();
const Theme64Tokens* theme64_tokens64();          // 当前主题 token（永不为空）
int  theme64_id64();
const char* theme64_name64(int id);               // 非法 id 返回 "?"
int  theme64_is_dark64();
int  theme64_reduce_motion64();                   // 1 = 减少动画（所有动效退化到 0ms/1 帧）
// 切主题：立即生效 + 写 config64（跨重启持久化）+ 打点 [THEME64] apply ...
void theme64_set_theme64(int id, const char* why);
void theme64_cycle_theme64(const char* why);      // 0→1→...→5→0
void theme64_set_reduce_motion64(int on, const char* why);
// 每帧调用：config64 里的值被外部改过（例如终端 `cfg set ui.theme 1` / `cfg set ui.reduce_motion 1`）时实时生效
int  theme64_tick64();   // 返回 1 = 有变化（调用方需要重绘）

// 动效：减少动画时返回 0（= 立即到位），否则返回 token 时长
uint32_t theme64_dur64(int base_ms);
// 缓动（整数定点，无浮点）：t/e 都在 0..256
//   ease  = 逼近 cubic-bezier(0.2,0,0,1)（三次方 ease-out，控制点见 THEME64_EASE_C1）
//   spring= 回弹曲线（采样表，>256 表示过冲）
int theme64_ease64(int t);
int theme64_spring64(int t);

// 主题切换热键（Ctrl+Shift+T 循环 / Ctrl+Shift+M 换壁纸模式 / Ctrl+Shift+R 减少动画）——
// 由 gui64 的消息循环调用；返回 1 = 已消费（调用方不要再把字符喂给应用）
int theme64_hotkey64(uint8_t c, int ctrl, int shift);
