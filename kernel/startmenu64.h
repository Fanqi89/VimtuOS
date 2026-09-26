// startmenu64.h - ★ P2：Windows 11 风格**开始菜单** + P2 UI 工具箱（四个二级弹窗与通知共用）
//
// 需求原文里的数字（全部走 theme64.h 的 Token，本文件不另写死视觉数字）：
//   位置：屏幕水平居中；底边 = Dock 顶边上方 10~12px（Token 11）；上面直边大圆角 24、下面直边小圆角 10；
//         竖直长方形、偏正方形一点、整体较小。
//   左上角：时间（上）+ 年月日（下）；右侧依次 网络 → 声音 → 中/英 → 消息通知 图标
//           （统一极简线性风格、线宽一致、端点圆润；浅色主题深灰/黑线、暗色主题白/浅灰线，跟随主题）。
//   搜索框：顶部偏右、长方形小圆角、跟随主题色的半透明亚克力、左侧搜索图标 + 白色"搜索"二字；
//           可搜索系统中的应用，回车/点击启动（复用既有 app 启动入口）。
//   搜索框下方：偏右的"已固定应用网格"（默认 2 列 × 4 行）；有输入时搜索结果列表优先。
//   左下角：小圆形用户头像 + 右侧用户名（取当前登录用户）。
//   右下角：圆形带阴影的亚克力电源按钮 → 在当前界面内弹出竖长方形小圆角框（关机/重启/锁定）；
//           电源按钮左边是「设置」按钮（点击先关开始菜单再打开设置窗口）。
//   交互：点菜单外部 / 点开始按钮 / ESC 三种都能关；ESC 两级退出（先关二级弹窗，再关开始菜单）。
//
// 串口打点（自动验收 grep；每类有上限防刷屏）：
//   [START64] init screen=WxH menu=400x420 r_top=24 r_bottom=10 gap_dock=11 anchors=menu
//   [START64] geom x=.. y=.. w=.. h=.. bottom=.. dock_top=.. gap=.. center=1 screen=WxH
//   [START64] open why=... / [START64] close why=...
//   [START64] grid cols=2 rows=4 n=8 tile=108x62 x=.. y=..
//   [START64] tile idx=.. app=.. name=.. x=.. y=.. w=.. h=..
//   [START64] search box x=.. y=.. w=.. h=.. r=8 acrylic=accent alpha=150 icon=search text=搜索
//   [START64] status icon net x=.. y=.. kind=ethernet wired=1 wireless=0
//   [START64] status icon sound x=.. y=.. volume=.. / lang x=.. y=.. text=英 ime=0
//   [START64] status icon notif x=.. y=.. unread=.. badge=none|digit|dot
//   [START64] user avatar x=.. y=.. d=30 name=.. / power btn x=.. y=.. d=38 settings x=.. y=..
//   [START64] search q=<..> hits=<n> first=<name> (app=<id>)
//   [START64] launch app=<id> name=<..> via=search-enter|tile|result
//   [START64] power menu x=.. y=.. w=.. h=.. rows=3 row0=shutdown row1=reboot row2=lock
//   [START64] power action lock|reboot|shutdown anim=<ms>
//   [START64] click item=<..> / [START64] hover item=<..> / [START64] click outside close=1
//   [START64] selftest PASS mask=0
#pragma once
#include <stdint.h>
#include "theme64.h"

// ==================== P2 UI 工具箱（开始菜单 + 四个弹窗 + 设备通知共用）====================
// 为什么要有：需求要求"上面大圆角 24、下面小圆角 10"这种**混合圆角**，而 gfx64_glass64/fill_round64
// 只吃单一圆角。这里按 theme64 Token 实现混合圆角的填充/亚克力/1px 描边（带 1px 抗锯齿），
// 并补一套"极简线性图标"（线宽一致、圆端点）——全部只用 gfx64 的基础图元与 fb 后备缓冲。
#define P2UI_ICON_NET_GLOBE   0    // 未联网：网状圆形球体
#define P2UI_ICON_NET_ETHER   1    // 有线：以太网
#define P2UI_ICON_NET_WIFI    2    // 无线：Wi-Fi（本批没有无线驱动 -> 实际不会选中，但图标要齐）
#define P2UI_ICON_SOUND       3
#define P2UI_ICON_SOUND_MUTE  4
#define P2UI_ICON_BELL        5
#define P2UI_ICON_SEARCH      6
#define P2UI_ICON_CLOSE       7
#define P2UI_ICON_POWER       8
#define P2UI_ICON_GEAR        9
#define P2UI_ICON_LOCK        10
#define P2UI_ICON_REBOOT      11
#define P2UI_ICON_SHUTDOWN    12
#define P2UI_ICON_CHEV_L      13
#define P2UI_ICON_CHEV_R      14
#define P2UI_ICON_PLUS        15
#define P2UI_ICON_PERSON      16
#define P2UI_ICON_USB         17
#define P2UI_ICON_MONITOR     18
#define P2UI_ICON_CHECK       19
#define P2UI_ICON_COUNT       20

// 混合圆角（上 rt / 下 rb）矩形：[填充] [亚克力(毛玻璃)] [1px 描边]。a = 0..255。
void p2ui_fill_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t c, int a);
void p2ui_acrylic_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t tint, int a,
                          const Theme64Tokens* t);
void p2ui_stroke_mixed64(int x, int y, int w, int h, int rt, int rb, uint32_t c, int a);
// 双层浅阴影（近 0/2/4 a=0.08 + 远 0/12/32 a=0.12）：gfx64_shadow64 的薄封装（统一从 Token 取）
void p2ui_shadow64(int x, int y, int w, int h, int r, const Theme64Tokens* t);
// 亚克力"弹窗"的一套：阴影 + 混合圆角玻璃 + 1px 半透明白高光边缘（四个二级弹窗统一用它）
void p2ui_popup64(int x, int y, int w, int h, int rt, int rb, const Theme64Tokens* t, uint32_t tint, int a);
// 线性图标（极简：线宽一致、端点圆润）。size = 图标外框边长；线宽取 Token。
void p2ui_icon64(int kind, int x, int y, int size, uint32_t c, int a);
// 线性图元
void p2ui_line64(int x0, int y0, int x1, int y1, int thick, uint32_t c, int a);
void p2ui_ring64(int cx, int cy, int r, int thick, uint32_t c, int a);
void p2ui_fill_circle64(int cx, int cy, int r, uint32_t c, int a);
void p2ui_ellipse64(int cx, int cy, int rx, int ry, int thick, uint32_t c, int a);
void p2ui_arc64(int cx, int cy, int r, int deg0, int deg1, int thick, uint32_t c, int a);
// 文本（mono 面，中英混排；与外壳 text_ttf 同一面）
int  p2ui_text_w64(const char* s);
void p2ui_text64(int x, int y, const char* s, uint32_t c);
int  p2ui_line_h64();
// 主题色辅助：暗色主题用浅色线条
uint32_t p2ui_icon_color64(const Theme64Tokens* t);
uint32_t p2ui_icon_dim_color64(const Theme64Tokens* t);

// ==================== 开始菜单 ====================
void startmenu64_init64();                       // gui64_run 里（theme64/gfx64/dock 就绪之后）调一次
bool startmenu64_is_open64();
void startmenu64_open64(const char* why);
void startmenu64_close64(const char* why);
void startmenu64_toggle64();                     // Dock 开始按钮：open why=start-button / close why=toggle
// ★ 缺陷 1：Win 键专用开关 —— 打开/关闭都打 [START64] open|close why=win-key（验收判据）
void startmenu64_win_key_toggle64();
int  startmenu64_handle_mouse_press64(int mx, int my, int button);   // 1 = 已消费
void startmenu64_handle_mouse_move64(int mx, int my, int buttons);
int  startmenu64_handle_key64(uint8_t c);        // 1 = 已消费（含 ESC 一级）
int  startmenu64_wheel64(int dz);                // 1 = 已消费
void startmenu64_tick64();                       // 开关动画 + 每秒时间/状态刷新
void startmenu64_draw64();

// 几何（验收/报告用；坐标都是屏幕绝对坐标）
void startmenu64_geom64(int* x, int* y, int* w, int* h);
int  startmenu64_time_rect64(int* x, int* y, int* w, int* h);      // 左上角"时间+年月日"区域（点它 = 日历）
int  startmenu64_search_rect64(int* x, int* y, int* w, int* h);
int  startmenu64_status_rect64(int which, int* x, int* y, int* w, int* h);  // 0=net 1=sound 2=lang 3=notif
int  startmenu64_user_rect64(int* x, int* y, int* w, int* h);
int  startmenu64_power_rect64(int* x, int* y, int* w, int* h);
int  startmenu64_settings_rect64(int* x, int* y, int* w, int* h);
int  startmenu64_tile_rect64(int idx, int* x, int* y, int* w, int* h);      // 固定网格 2×4
int  startmenu64_result_rect64(int idx, int* x, int* y, int* w, int* h);    // 搜索结果行
int  startmenu64_pm_rect64(int row, int* x, int* y, int* w, int* h);        // 电源二级菜单：row 0/1/2
int  startmenu64_pm_panel64(int* x, int* y, int* w, int* h);
int  startmenu64_status_icon_kind64(int which);   // 0=net 1=sound 2=lang 3=notif -> P2UI_ICON_*
int  startmenu64_hits64();                        // 最近一次搜索命中数
void startmenu64_launch64(int app_id, const char* via);
int  startmenu64_selftest64();                    // 纯逻辑自检（几何/网格/搜索匹配），返回失败位掩码
