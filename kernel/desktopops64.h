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
#define DESKOPS_KIND_APP       8     // ★ 应用注册表来的桌面快捷方式（用户用 /bin/vpkg 装的应用；
                                     //   名字/图标/exec 全在注册表里，见 kernel/app64.h）
                                     //   注意：它不写进 ui.desktop.icons 的集合字符串（那是 0..7 的
                                     //   内置项）；快捷方式的有无由注册表的 desktop 标志决定，
                                     //   位置写回注册表（跨重启持久化）。
#define DESKOPS_KIND_FILE      9     // ★ v0.11.7：**桌面上的真实文件/文件夹** —— 真源是 <home>/Desktop 目录本身
                                     //   （list 出来的每一条就是一个桌面项；name = basename，path = 卷内完整路径）。
                                     //   与 KIND_APP 的差别：APP 指向注册表条目；FILE 指向卷里真实存在的条目。
                                     //   拖到桌面 = fs64_rename_to64 真移动；删除 = trash64_put64 真进 /Recycle。
                                     //   **不进** ui.desktop.icons（那个串只编码 0..7 的 kind）；位置写
                                     //   /etc/desktop.links（v0.11.7 起退化成**位置缓存**，只供 x/y）。

#define DESKOPS_MAX_ITEMS      16    // 桌面项上限（3 内置 + 5 文件夹 + 应用快捷方式 + 文件快捷方式）
#define DESKOPS_RECYCLE_MAX    8     // 回收站内容上限
#define DESKOPS_LINK_MAX       8     // ★ v0.11.1：文件快捷方式条数上限（= /etc/desktop.links 行数上限）
#define DESKOPS_LINK_PATH_MAX  128   // 单条路径上限（与 VFS64_PATH_MAX 同口径）

// ---- 生命周期 ----
void desktopops64_init64();      // gui64_run 里（dock 几何就绪之后、首帧之前）调一次
int  desktopops64_tick64();      // 每帧：外部改 config（终端 `cfg set ui.desktop.icons ...`）时同步；1 = 需重绘

// ---- 桌面项 ----
int  desktopops64_count64();
void desktopops64_set_pos64(int i, int x, int y);        // 拖动结束落点（写 config64 / 应用则写注册表）
int  desktopops64_app64(int i);                          // 第 i 项是应用快捷方式时 -> 注册表下标；否则 -1
int  desktopops64_kind64(int i);
const char* desktopops64_name64(int i);
void desktopops64_pos64(int i, int* x, int* y);
int  desktopops64_hit64(int mx, int my);                 // 命中图标单元格（含名字标签区）；-1 = 无
void desktopops64_set_pos64(int i, int x, int y);        // 拖动结束落点（写 config64）
void desktopops64_clamp64(int x, int y);                 // 屏幕尺寸变化时把图标钳回屏内
void desktopops64_draw_icons64(int selected);            // 画全部桌面项（选中底 + 圆角正方形底板 + 位图/文件夹）
int  desktopops64_is_recycle_hit64(int mx, int my);       // 命中"回收站"那一项
void desktopops64_restore_defaults64(const char* why);   // 恢复默认桌面图标（设置页/终端）
// ---- ★ v0.11.7：文件项（桌面 = 真实文件夹的落点）----
//   真源 = <home>/Desktop 的目录条目（见 kernel/deskdir64.h）；本文件只把它变成"图标项"。
//   位置缓存：/etc/desktop.links（系统卷，纯文本，一行一条 5 段 `dir|letter|x|y|path`；
//   4 段旧格式也认）。**只有 path 落在当前桌面目录里的行才算数**，其余计进日志的 skipped=。
//   为什么位置还要落盘：网格吸附是"用户摆的位置"，重启后要一模一样；
//   而"哪个条目在桌面上"不再由这个文件决定 —— 目录里有什么，桌面就有什么。
//   add：落点吸附网格 + 占用避让 + **真移动**（同卷 rename / 跨卷复制+删源）；失败如实返回 0 并打点。
int  desktopops64_add_file64(const char* path, int is_dir, int sx, int sy, char letter, const char* why);
int  desktopops64_file_path64(int i, char* out, int cap);   // 第 i 项是文件项 -> 卷内完整路径；否则 -1
char desktopops64_file_letter64(int i);                      // 卷字母（'A'..'Z'；非文件项 = 0）
int  desktopops64_is_file64(int i);                          // 1 = 该项是桌面上的真实文件/文件夹
int  desktopops64_file_count64();                            // 当前文件项条数
int  desktopops64_remove_file64(int i, const char* why);     // 真删：送进 /Recycle（右键删除走这里）
void desktopops64_links_reload64(const char* why);           // 重读位置缓存 + 重列桌面目录（自检/终端）
const char* desktopops64_links_path64();                     // "/etc/desktop.links"（位置缓存；验收脚本/日志共用）
const char* desktopops64_desk_dir64();                       // 当前桌面目录（未登录退化 /home/vimtu/Desktop）

// ---- 回收站 ----
int  desktopops64_recycle_count64();
int  desktopops64_recycle_add64(int icon_idx);           // 桌面项 -> 回收站（1 = 成功）
void desktopops64_recycle_open64();                      // 打开回收站窗口（无则建）
void desktopops64_recycle_reset64();                     // 会话重置（关窗清状态）
// ★ v0.11.1：按路径打开资源管理器（文件快捷方式双击/右键"打开"走这里；实现在 explorer64.cpp）
// 按路径 + 卷字母打开资源管理器（文件快捷方式双击/右键"打开"走这里；实现在 explorer64.cpp）
void explorer64_open_path64(const char* path, char letter);

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

// ---- ★ P7a-7：网格吸附 / 占用避让（桌面图标不再自由重叠）----
//   snap：把自由落点吸到**最近的网格单元左上角**（网格 = 图标单元格 88x84，原点 = 默认位置 24,24）；
//   place：吸附后若该格已被别的项占用，按"环"从近到远找最近空格（就近避让）；返回 1 = 避让过；
//   drop：落点最终生效 = 吸附 + 避让 + 写内存/config（图标 0..2 用既有键）+ 打点 [DESK64] snap …。
void desktopops64_snap64(int* x, int* y);
int  desktopops64_place64(int i, int x, int y, int* ox, int* oy);
void desktopops64_drop64(int i, int x, int y);

// ---- ★ P7a-8：**针对某个桌面图标**的右键菜单（打开/重命名/删除/属性）----
void desktopops64_icon_menu_open64(int idx, int mx, int my);
int  desktopops64_menu_target64();                        // 当前菜单目标项下标（-1 = 桌面空白菜单）

// ---- ★ P7a-9：多选集合 / 组拖动（Windows 方案）----
//   单击 = 选中一个；Ctrl = 加选/反选；Shift = 范围选；框选 = 一组；按住任一已选图标 = 移动整组
//   （组内相对位置保持）；单击空白 = 清除选择。
int  desktopops64_sel_count64();
int  desktopops64_is_selected64(int i);
int  desktopops64_sel_at64(int k);                        // 第 k 个选中项下标（-1 = 越界）
void desktopops64_select_only64(int i);
void desktopops64_select_toggle64(int i);
void desktopops64_select_range64(int i);
void desktopops64_select_clear64(void);
void desktopops64_select_rect64(void);                    // 按当前选择框矩形重算选中集合
void desktopops64_drag_begin64(void);                     // 组拖动开始（记住每个选中项起点）
int  desktopops64_drag_group64(int dx, int dy);           // 组拖动位移（相对起点）；1 = 有变化
void desktopops64_drag_commit64(void);                    // 组拖动结束（逐项吸附+避让+持久化+打点）
int  desktopops64_selftest64();
