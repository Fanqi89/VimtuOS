// desktopops64.h - ★ P5：桌面交互细节（右键菜单 / 玻璃选择框 / 回收站 / 桌面图标集合）
//
// 需求原文（GUI升级.txt 里"桌面"那一段）：
//   * 桌面上显示的是应用程序的快捷方式，可以在桌面上移动；
//   * **在桌面按鼠标左键拖动会出现四个角为小圆角的玻璃材质选择框**（框中间全透明，能看到被框选的图标/文件，
//     框边缘突出以便看清）；
//   * 可以把快捷方式或文件**移入回收站**（回收站里可以选择删除或恢复，删除是**永久删除**，
//     永久删除还要有**二次确认**）；
//   * 当桌面上的快捷图标删掉了/桌面上没有某个快捷方式图标，可以**在设置中把快捷图标调回来**；
//   * 还要添加**鼠标右键菜单**；
//   * 桌面上所有软件图标为**简约圆角正方形**（与 Dock 一致）。
//
// 本模块是上面这些"桌面层"状态的唯一真源（外壳 gui64.cpp 只做钩子与命中分发）：
//   1) 桌面项集合（kind 0=我的电脑 1=回收站 2=终端 3..7=用户新建文件夹），**跨重启持久化**：
//        config64 键 `ui.desktop.icons` = "桌面集合|回收站集合"（如 "0,1,2|"），
//        "0,1,2" 与旧行为一致（= 默认三项），所以**老系统/老验收看到的桌面一字不差**；
//        位置仍用既有键 ui.icon{0,1,2}.x/y（拖动结束时写）。
//   2) 回收站内容（内存态 + 上面的持久化集合；打开回收站可恢复 / 永久删除，永久删除要二次确认）。
//   3) 桌面右键菜单（亚克力 + 圆角 + 双层浅阴影 + 1px 高光边，全部走 theme64 Token + p2ui 工具箱）。
//   4) 玻璃选择框（框内**一个像素都不铺**，只有 2px 高光边 + 1px 内亮边；颜色用 Token sel_fill/sel_edge）。
//
// 串口打点（自动验收 grep；每类都有上限防刷屏）：
//   [DESK64] init items=3 set=0,1,2| recycle=0 pos=8,8;96,8;8,96
//   [DESK64] item idx=.. kind=.. name=.. x=.. y=.. w=.. h=.. (cx/cy)
//   [DESK64] menu open x=.. y=.. w=.. h=.. items=7 enabled=4 r=14 shadow=2 edge=1
//   [DESK64] menu item idx=.. id=.. name=.. enabled=0|1 y=.. h=..
//   [DESK64] menu close why=esc|outside|action|toggle
//   [DESK64] menu action id=.. name=.. done=0|1 why=..
//   [DESK64] selbox x0=.. y0=.. x1=.. y1=.. sel=..        （**保持既有格式**：鼠标闭环定位靠它）
//   [DESK64] selbox glass x=.. y=.. w=.. h=.. corner=.. edge=.. inside_alpha=0
//   [RECYCLE64] count n=.. desktop=..
//   [RECYCLE64] add kind=.. name=.. n=.. desktop=..
//   [RECYCLE64] restore kind=.. name=.. n=.. desktop=..
//   [RECYCLE64] delete ask kind=.. name=.. confirm=1|0
//   [RECYCLE64] delete done kind=.. name=.. n=.. permanent=1
//   [RECYCLE64] empty (recycle bin is empty)
//   [DESK64] icons reset why=.. set=0,1,2|
//   [DESK64] selftest PASS mask=0
#pragma once
#include <stdint.h>

// 桌面项种类
#define DESKOPS_KIND_MYPC      0
#define DESKOPS_KIND_RECYCLE   1
#define DESKOPS_KIND_TERM      2
#define DESKOPS_KIND_FOLDER    3     // 用户"新建文件夹"（3..7 共 5 个槽）

#define DESKOPS_MAX_ITEMS      8     // 桌面项上限（3 内置 + 5 文件夹）
#define DESKOPS_RECYCLE_MAX    8     // 回收站内容上限

// ---- 生命周期 ----
void desktopops64_init64();      // gui64_run 里（dock 几何就绪之后、首帧之前）调一次
int  desktopops64_tick64();      // 每帧：外部改 config（终端 `cfg set ui.desktop.icons ...`）时同步；1 = 需重绘

// ---- 桌面项 ----
int  desktopops64_count64();
int  desktopops64_kind64(int i);
const char* desktopops64_name64(int i);
void desktopops64_pos64(int i, int* x, int* y);
int  desktopops64_hit64(int mx, int my);                 // 命中图标单元格（含名字标签区）；-1 = 无
void desktopops64_set_pos64(int i, int x, int y);        // 拖动结束落点（写 config64）
void desktopops64_clamp64(int x, int y);                 // 屏幕尺寸变化时把图标钳回屏内
void desktopops64_draw_icons64(int selected);            // 画全部桌面项（选中底 + 圆角正方形底板 + 位图/文件夹）
int  desktopops64_is_recycle_hit64(int mx, int my);       // 命中"回收站"那一项
void desktopops64_restore_defaults64(const char* why);   // 恢复默认桌面图标（设置页/终端）

// ---- 回收站 ----
int  desktopops64_recycle_count64();
int  desktopops64_recycle_add64(int icon_idx);           // 桌面项 -> 回收站（1 = 成功）
void desktopops64_recycle_open64();                      // 打开回收站窗口（无则建）
void desktopops64_recycle_reset64();                     // 会话重置（关窗清状态）

// ---- 桌面右键菜单 ----
int  desktopops64_menu_is_open64();
int  desktopops64_rect_hit64(int i, int x0, int y0, int x1, int y1);   // 图标格与矩形相交（框选判定用）
int  desktopops64_sel_target64(int i);                                  // 第 i 项的 kind（-1 = 越界）

// ==================== 跨模块钩子（实现在 explorer64.cpp；本文件只声明，避免改它的 .h）====================
// 「文件资源管理器只能显示可见分区，系统分区默认是隐藏状态的」——开关键 ui.explorer.show_system：
//   0（默认）= 非引导盘上的**系统分区**（MBR 类型 0xEF / 系统卷）在"此电脑"里不列出；
//   1 = 全部列出。引导盘（系统盘）上的卷保持既有界面（既有 explorer64_test 断言它们必须在列表里）。
void explorer64_set_show_system64(int on, const char* why);
int  explorer64_show_system64();
void explorer64_rescan64(const char* why);

void desktopops64_menu_open64(int mx, int my);
void desktopops64_menu_close64(const char* why);
int  desktopops64_menu_press64(int mx, int my, int button);   // 1 = 消费
int  desktopops64_menu_key64(uint8_t c);                      // 1 = 消费（ESC 关菜单）
void desktopops64_menu_move64(int mx, int my);                // 悬停高亮
void desktopops64_menu_draw64();
int  desktopops64_menu_items64();
void desktopops64_menu_geom64(int* x, int* y, int* w, int* h);

// ---- 玻璃选择框 ----
void desktopops64_selbox_begin64(int x, int y);
int  desktopops64_selbox_update64(int x, int y);         // 拖动中；1 = 矩形/选中集有变化
void desktopops64_selbox_end64();                        // 松开（打 [UI] selbox x0=... 兼容行）
int  desktopops64_selbox_active64();
void desktopops64_selbox_rect64(int* x, int* y, int* w, int* h);
int  desktopops64_selbox_count64();                      // 框内项数
void desktopops64_selbox_draw64();                       // 玻璃：中间全透明 + 突出边缘

int  desktopops64_selftest64();
