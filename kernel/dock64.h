// dock64.h - ★ v0.8.0：Dock 运行时动态列表 + 右键上下文菜单（公开 API）
//
// 为什么有这层：需求要求 Dock 图标列表是**运行时**的（Vec<TaskbarItem> 语义），
//   固定项持久化、运行项随窗口事件增删、右键出上下文菜单。数据与绘制在 kernel/gui64.cpp，
//   上下文菜单在 kernel/dockmenu64.cpp —— 两者用这份头文件对接。
//
// 数据（对应需求的 TaskbarItem）：app_id / window_id / title / icon / kind / is_running
//   kind = DOCK_KIND_START（开始按钮，永远最左，不属于固定列表）
//        = DOCK_KIND_PIN  （固定项：持久化，重启后仍在）
//        = DOCK_KIND_RUN  （运行项：一个窗口一项，不合并；不持久化）
#pragma once

// ---- Dock 列表 / 固定列表 ----
void        dock64_on_window_create64(int win_id, int app_id, const char* title);
void        dock64_on_window_close64(int win_id);
int         dock64_count64();
int         dock64_kind64(int i);            // DOCK_KIND_*；越界 -1
int         dock64_app64(int i);
int         dock64_win64(int i);             // RUN 项的 window_id；其他 -1
const char* dock64_title64(int i);
int         dock64_is_pinned64(int app_id);
int         dock64_pin_count64();
int         dock64_run_count64();
void        dock64_pin_set64(int app_id, int on);      // 立即持久化 + 重建列表
int         dock64_app_windows64(int app_id);          // 该应用当前窗口数（不合并判定用）
void        dock64_close_window64(int win_id);         // 关某个窗口（只移除对应运行项）
void        dock64_close_app_windows64(int app_id);    // 关该应用所有窗口
void        dock64_geom64(int* x, int* y, int* w, int* h);
int         dock64_hit64(int mx, int my);              // Dock 项命中（-1 = 不在项上）

// ---- 右键上下文菜单 ----
enum { DOCKMENU_OWNER_NONE = 0, DOCKMENU_OWNER_DOCK = 1, DOCKMENU_OWNER_START = 2 };
void dockmenu64_open_for_dock64(int idx, int mx, int my);          // 右键 Dock 项
void dockmenu64_open_for_app64(int app_id, int win_id, int mx, int my);  // 右键开始菜单应用
int  dockmenu64_is_open64();
void dockmenu64_close64(const char* why);
int  dockmenu64_press64(int mx, int my, int button);   // 1 = 吃掉了这次点击
void dockmenu64_move64(int mx, int my);
int  dockmenu64_key64(int c);          // 1 = 吃掉这个键（Up/Down/Enter/ESC）
void dockmenu64_draw64();
