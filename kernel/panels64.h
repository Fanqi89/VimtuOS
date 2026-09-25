// panels64.h - ★ P2：四个二级弹窗（通知 / 声音 / 网络 / 日历）+ 通知列表 + 设备插拔 toast
//
// 统一要求（需求原文）：亚克力半透明、圆角、**双层浅阴影 + 1px 高光边缘**、配色跟随主题、
//   暗色主题自动变深灰亚克力、**不压住 Dock、不超出屏幕**、锚定在触发按钮附近；ESC 优先关弹窗。
//   位置**跟随开始菜单位置**（全部由 startmenu64 的几何算出，不是固定屏幕坐标）。
//   * 通知面板：开始菜单**右侧**、竖直长方形；逐条清除 + 全部清除；空状态提示；未读 -> 消息图标小红点/数字角标。
//   * 声音面板：开始菜单**右侧**、四边圆角横长方形；输出源（音箱/耳机）+ 滑动条 + 百分比。
//     ★ 声卡驱动本批**不做**：数值只走内存态，并如实打点 "audio driver not implemented yet"。
//   * 网络面板：开始菜单**左侧**、竖向长方形；上 WiFi 区（无无线驱动 -> 如实空态："无无线硬件"）、
//     下**以太网区固定最下方不随滚动**（真实 e1000 链路：已连接/未连接/未插网线）。
//   * 日历面板：点开始菜单左上角时间 -> 在**该区域上方**弹出；360–420 × 420–480；6 行 × 7 列日期网格；
//     今天主题强调色圆形高亮；滚轮/左右拖动/左右方向键切月、PageUp/PageDown 切年；「今天」回本月。
//   * 设备插拔 toast：从桌面右侧滑入、停留几秒自动滑出、可手动关闭、多个堆叠不互相覆盖。
//     数据源（**如实**）：e1000 链路（QEMU `set_link` 可插拔）+ usb64 的存储/设备计数轮询差分；
//     usb64 没有热插拔/拔出检测，所以 U 盘那条路径**不会**凭空产生事件（见 [TOAST64] 打点）。
//
// 串口打点（自动验收 grep；每类有上限防刷屏）：
//   [PANEL64] init anchors=menu screen=WxH dock_top=.. blur=24 edge=1px
//   [PANEL64] open panel=notif anchor=right-of-menu x=.. y=.. w=.. h=.. r=14 shadow=2 edge=1 dock_top=.. follow=menu
//   [PANEL64] close panel=.. why=..  /  [PANEL64] esc close=..  /  [PANEL64] click outside close=..
//   [PANEL64] notif list n=.. unread=.. first=..   /  [PANEL64] notif remove idx=.. n=..  /  [PANEL64] notif clear all n=..
//   [PANEL64] notif empty (no notifications)
//   [PANEL64] sound panel x=.. y=.. w=.. h=.. slider=x,y,w,h pct=.. src=speaker
//   [PANEL64] sound volume=.. src=speaker drag=0/1  +  [PANEL64] audio driver not implemented yet (memory-state only)
//   [PANEL64] net panel x=.. y=.. w=.. h=.. wifi_area=.. eth_row=..   （eth_row 固定：不随滚动）
//   [PANEL64] net wifi count=0 state=no-hardware text=无无线硬件 (no wireless driver; truthful empty state)
//   [PANEL64] net ethernet y=.. h=.. state=connected text=已连接 fixed=1
//   [PANEL64] cal panel x=.. y=.. w=.. h=.. r=14 grid=7x6 cell=.. today=YYYY-MM-DD sel=YYYY-MM-DD
//   [PANEL64] cal month ym=YYYY-MM via=wheel|arrow|pageup|pagedown|today
//   [PANEL64] cal select date=YYYY-MM-DD  /  [PANEL64] cal year panel x=.. y=.. w=.. h=.. years=2022..2033
//   [PANEL64] cal anim dir=in|out t=..ms s=.. dy=.. alpha=..
//   [PANEL64] selftest PASS mask=0
//   [TOAST64] init src=e1000_link64+usb64 counts poll=500ms stack=4
//   [TOAST64] device src=.. state=.. text=..  /  [TOAST64] toast idx=.. kind=.. title=.. x=.. y=.. w=.. h=..
//   [TOAST64] toast idx=.. state=in|hold|out x=.. frame=..  /  [TOAST64] toast close idx=.. by=click|timeout
//   [TOAST64] toast stack n=.. overlap=0
#pragma once
#include <stdint.h>
#include "theme64.h"
#include "startmenu64.h"

// 弹窗 id
#define PANEL64_NONE   0
#define PANEL64_NOTIF  1     // 通知面板
#define PANEL64_SOUND  2     // 声音面板
#define PANEL64_NET    3     // 网络面板
#define PANEL64_CAL    4     // 日历面板

// 通知/toast 图标种类
#define PANEL64_ICON_INFO    P2UI_ICON_BELL
#define PANEL64_ICON_USB     P2UI_ICON_USB
#define PANEL64_ICON_NET     P2UI_ICON_NET_ETHER
#define PANEL64_ICON_SOUND   P2UI_ICON_SOUND

// 设备种类（设备插拔通知用）
#define PANEL64_DEV_USB     0
#define PANEL64_DEV_NET     1

void panels64_init64();                     // gui64_run 里（startmenu64_init64 之后）调一次
int  panels64_open64();                     // 当前打开的弹窗（PANEL64_*）；0 = 没有
int  panels64_any_open64();
void panels64_toggle64(int which, const char* why);
void panels64_close64(const char* why);     // 关当前弹窗（ESC 一级）
int  panels64_close_all64(const char* why); // 返回 1 = 真的关掉了弹窗

int  panels64_handle_mouse_press64(int mx, int my, int button);   // 1 = 已消费
void panels64_handle_mouse_move64(int mx, int my, int buttons);   // 悬停 + 拖动（buttons.bit0 = 左键按住）
int  panels64_handle_key64(uint8_t c);      // 1 = 已消费（ESC 关弹窗）
int  panels64_wheel64(int dz);              // 1 = 已消费
void panels64_tick64();                     // 动画 + 设备轮询 + toast 生命周期
void panels64_draw64();

// 几何（屏幕绝对坐标；全部由开始菜单几何推导 -> "位置跟随开始菜单"）
int  panels64_rect64(int which, int* x, int* y, int* w, int* h);
int  panels64_eth_rect64(int* x, int* y, int* w, int* h);       // 以太网区（固定最下方）
int  panels64_slider_rect64(int* x, int* y, int* w, int* h);    // 声音滑轨（左小右大）
int  panels64_cal_cell_rect64(int idx, int* x, int* y, int* w, int* h);  // 0..41（6 行 × 7 列）
int  panels64_cal_prev_rect64(int* x, int* y, int* w, int* h);
int  panels64_cal_next_rect64(int* x, int* y, int* w, int* h);
int  panels64_cal_today_rect64(int* x, int* y, int* w, int* h);
int  panels64_cal_year_rect64(int* x, int* y, int* w, int* h);

// 通知（图标/标题/内容/时间；逐条 + 全部清除；未读角标）
void panels64_notify64(int icon_kind, const char* title, const char* body);
int  panels64_notif_count64();
int  panels64_notif_unread64();
int  panels64_notif_remove64(int idx);      // 0 = 越界
int  panels64_notif_clear64();
void panels64_notif_mark_read64();

// 设备插拔（数据源在 panels64_tick64 的轮询里；这个入口给未来驱动/别的模块用）
void panels64_device_event64(int kind, int plugged, const char* name);

// toast（从桌面右侧滑入、停留几秒自动滑出、可手动关、多个堆叠）
int  panels64_toast_count64();
int  panels64_toast_rect64(int idx, int* x, int* y, int* w, int* h);
int  panels64_toast_state64(int idx);       // 0=in 1=hold 2=out 3=gone
int  panels64_toast_close64(int idx, const char* by);

// 声音（内存态；声卡驱动未实现 —— 见 [PANEL64] audio driver 打点）
int  panels64_volume64();
void panels64_set_volume64(int v, const char* why);
const char* panels64_src_name64();          // "speaker" / "headphones"
void panels64_set_src64(int src, const char* why);

// 日历（验收用）
int  panels64_cal_ym64(int* y, int* m);
int  panels64_cal_sel64(int* y, int* m, int* d);
int  panels64_cal_grid_hit64(int mx, int my, int* y, int* m, int* d);   // 返回 1 = 命中某天
int  panels64_cal_year_panel_open64();

// 无线（本批 0 个：如实空态；接口留着给未来的无线驱动，绝不编造列表）
int  panels64_wifi_add64(const char* ssid, int signal, int secured);

int  panels64_selftest64();
