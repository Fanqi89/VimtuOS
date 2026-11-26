/* user/browser/vimtu_app.h - 前端的进程内共享状态（只有一扇窗口，所以一个全局结构就够）
 *
 * 为什么用"全局 g_app"而不是把状态挂到 gui_window 上：本前端只有一个 surface（像素预算
 * 只够一块 960x540），窗口/画布/地址栏/按钮状态天然只有一份。核心要求 gui_window 是
 * 一个不透明指针，但它只要求"能通过它找到 browser_window"，所以让多扇窗口共享画布、
 * 只让第一扇（primary）真正绘制 —— 这样"点了个 target=_blank 链接"不会让程序崩掉，
 * 只是新窗口不显示（打 UNVERIFIED）。
 */
#ifndef VIMTU_APP_H
#define VIMTU_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vimtu_canvas.h"

#include "utils/errors.h"

struct browser_window;
struct gui_window;
struct browser_window;
struct vimtu_app {
    struct vimtu_canvas canvas;
    bool canvas_ok;

    struct gui_window *win;              /* 主窗口（第一扇） */
    struct browser_window *bw;           /* = win->bw，给外壳用 */

    unsigned frame;                      /* 已绘制帧号 */
    bool need_paint;

    char url[512];                       /* 地址栏文本（当前导航 URL） */
    char title[128];
    char status[256];

    /* Chrome 三个按钮的"真值"：只有当前真能后退/前进才亮 */
    bool can_back;
    bool can_forward;
    bool can_reload;
    bool throbber;                       /* 正在加载（↻ 按钮画成高亮） */

    bool url_editing;                    /* 地址栏编辑态：未实现，仅标记 */
    bool quit;
};

extern struct vimtu_app g_app;
/* ---- vimtu_main.c ---- */
void vimtu_log(const char *fmt, ...);

/* ---- vimtu_table.c ---- */
nserror vimtu_table_register(void);        /* 填 netsurf_table 并 netsurf_register() */
void vimtu_window_paint(void);             /* 重绘一帧（页面 + Chrome）并 flush */
void vimtu_window_mark_dirty(void);        /* 核心 invalidate -> 本帧结束前重画 */
int vimtu_schedule_run(void);              /* 跑一轮到期回调；返回距下一到期 ms（-1 = 无） */
void vimtu_window_back(void);
void vimtu_window_forward(void);
void vimtu_window_reload(void);

/* ---- vimtu_shell.c ---- */
void vimtu_shell_draw(struct vimtu_app *app);          /* 画 44px 的 Edge 风格工具条 */
bool vimtu_shell_click(struct vimtu_app *app, int x, int y);  /* true = 被 Chrome 吃掉 */
void vimtu_shell_motion(struct vimtu_app *app, int x, int y);
#endif /* VIMTU_APP_H */
