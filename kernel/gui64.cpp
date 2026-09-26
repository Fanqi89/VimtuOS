// gui64.cpp - VimtuOS 64 位桌面外壳（桌面 + 图标 + 任务栏 + 开始菜单 + 窗口管理 + 消息循环）
//
// 这是 32 位 gui.cpp（4214 行）里"外壳"那一半的 64 位重写。应用本体已拆到各自文件
// （calc64/mines64/terminal64/settings64/taskmgr64），本文件只负责：
//   * 窗口生命周期与 z 序、拖拽移动、右键缩放、标题栏三按钮、最小化/最大化
//   * 桌面背景 + 图标（双击打开）+ 任务栏（开始按钮/窗口按钮/时钟）
//   * 开始菜单 10 项（与 32 位同名同序）+ 键盘导航
//   * 消息循环：输入分发 + **脏矩形重绘** + 光标
//   * 内置轻量窗口：我的电脑 / 系统监视器 / 关于 / 回收站
//   * 重启 / 关机（照抄 32 位那条"8042 -> 0xCF9 -> 三重故障"回退链）
//
// 关键设计（都吃过教训，别改回去）：
//   1) 脏矩形而不是整屏重绘：安装界面已经证明整屏重绘会把鼠标包读慢十几倍，
//      进而放大位移聚合（光标乱飞）。桌面同理：拖窗口/移光标只重画受影响的小块，
//      用 fb_set_clip + fb_flip_region 提交。缩放 != 100% 时 fb_flip_region 自己
//      退化成整帧（驱动里已处理），这里不用管。
//   2) 光标不做 save-under：每一帧都把光标区域**重画一遍底色再画光标**，
//      因为后备缓冲里本来就留着正确的背景像素，省一套存/取逻辑就不会有残影 bug。
//   3) 文字统一用 simhei 面（face 2）：它的子集含 ASCII + 常用汉字，标题里中英混排
//      不会出现豆腐块。每次自己画字前都 font_select(2)（应用会改 face）。
//   4) 应用的 userdata 一律由应用自己释放：外壳只调 on_close 回调，绝不 kfree，
//      否则会和应用自己的回收逻辑双释放（kfree_64 会拦住并刷日志）。

#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "debug64.h"
#include "mem_64.h"
#include "x86_64.h"
#include "port.h"
// ---- 本轮接线：config64（配置）/ session64（会话策略）/ sysstate64（状态机+ring log）/ panic64（看门狗）----
#include "config64.h"
#include "session64.h"
#include "sysstate64.h"
#include "panic64.h"
#include "explorer64.h"   // 我的电脑/文件资源管理器（本轮：外壳只做钩子，实现全在 explorer64.cpp）
// ---- 本批（Windows 11 现代外观）：设计 Token（theme64）/ 现代图元（gfx64）/ 图像解码（img64）----
#include "theme64.h"
#include "gfx64.h"
#include "img64.h"
// ★ 本批（P1c）：锁屏 / 登录界面 / 多用户骨架（实现全在 locklogin64.cpp + userdb64.cpp）
#include "locklogin64.h"
// ★ P2：开始菜单（startmenu64）+ 四个二级弹窗/通知/设备 toast（panels64）
#include "startmenu64.h"
#include "panels64.h"
#include "settings64.h"      // ★ P3：设置页的启动期生效点（字体大小档 + 自定义渐变壁纸）
#include "desktopops64.h"     // ★ P5：桌面右键菜单 / 玻璃选择框 / 回收站 / 桌面图标集合
#include "icons64.h"         // ★ 本批：外置图标包（真图标）—— 内核里不含图标字节，见 icons64.h

// ==================== 资源符号（build64.sh 用 objcopy 生成）====================
extern "C" const uint8_t _binary_icon_mycomputer_bin_start[];
extern "C" const uint8_t _binary_icon_recyclebin_bin_start[];
extern "C" const uint8_t _binary_icon_terminal_bin_start[];
extern "C" const uint8_t _binary_icon_start_bin_start[];   // 64x64 RGBA（logo/kaisi.png）
extern "C" const uint8_t _binary_logo_rgba_bin_start[];    // 240x150 RGBA（logo/logo.png）
// ★ 本批：真正的 logo/kaisi.png 原始字节（build64.sh 用 objcopy 从 logo/kaisi.png 嵌入）——
//   启动期幂等装进 VimtuFS2 系统卷，开始按钮就从盘上读真图（见 dock_start_icon_init64）。
extern "C" const uint8_t _binary_kaisi_png_start[];
extern "C" const uint8_t _binary_kaisi_png_end[];

#define ICON_SRC_W 128          // 图标源图尺寸（RGBA，见 _make_icons.py）

// ==================== 几何常量 ====================
// ★ 本批（Windows 11 现代外观）：底部 32px 老任务栏 → Dock（高 60 + 离底 16 = 76）。
//   TASKBAR_H 这个名字**保留**，语义改为"底部保留区高度"（窗口最大化/拖拽钳制、图标拖动边界、
#define GUI64_GRIP   6          // ★ P5：窗口边缘/四角抓取带宽（px；窗口布局常数，不是视觉 Token）
#define TITLE_H      24         // 标题栏高
#define TITLE_H      24         // 标题栏高
#define BORDER       1          // 边框
#define DECO_H       (TITLE_H + BORDER)     // 客户区相对窗口顶部的偏移
#define DECO_W       (BORDER * 2)           // 客户区相对窗口左侧的偏移
#define TASKBAR_H    (THEME64_DOCK_H + THEME64_DOCK_MARGIN)
#define DOCK_H       THEME64_DOCK_H
#define DOCK_MARGIN  THEME64_DOCK_MARGIN
#define BTN_W        30         // 标题栏按钮宽
#define ICON_W       48         // 桌面图标位图边长
#define ICON_CELL_W  88         // 图标单元格（含标签）
#define ICON_CELL_H  84
#define MAX_WINS     16
#define MENU_ITEMS   10
#define CLOCK_W      210        // 右下角时钟玻璃片宽度（[UI] clock text 打点仍在这里）
#define LOGO_W       240        // 开机/关机画面用的 logo 嵌入尺寸（logo/logo.png，见 _make_logo.py）
#define LOGO_H       150
#define START_ICON_SRC   64     // 开始按钮图标源尺寸（logo/kaisi.png，见 _make_start_icon.py）
#define START_ICON_DISP  24     // 老接口（gui64_draw_start_icon64）的绘制尺寸；Dock 用 BEGIN_START_DISP
#define DOCK_START_DISP  THEME64_DOCK_ICON   // Dock 开始按钮图标 46（Token 44–48）

// ==================== 配色（Win10 风格）====================
#define C_DESKTOP    rgb(0, 84, 158)
#define C_TITLE_ACT  rgb(32, 32, 32)
#define C_TITLE_INA  rgb(88, 88, 88)
#define C_TITLE_TXT  rgb(255, 255, 255)
#define C_BORDER     rgb(120, 120, 120)
#define C_CLIENT     rgb(240, 240, 240)
#define C_BTN_HOVER  rgb(200, 60, 60)
#define C_TASKBAR    rgb(20, 20, 20)
#define C_TASK_BTN   rgb(48, 48, 48)
#define C_TASK_ACT   rgb(70, 70, 70)
#define C_MENU_BG    rgb(32, 32, 32)
#define C_MENU_SEL   rgb(0, 120, 215)
#define C_MENU_TXT   rgb(240, 240, 240)
#define C_ICON_TXT   rgb(255, 255, 255)
#define C_ICON_SEL   rgb(0, 120, 215)
// 标题栏三按钮（必须肉眼可区分）
#define C_BTN_BG      rgb(64, 64, 64)
#define C_BTN_BG_HOV  rgb(96, 96, 96)
#define C_BTN_CLOSE   rgb(180, 52, 52)
#define C_BTN_CLOSE_H rgb(232, 17, 35)
#define C_BTN_GLYPH   rgb(255, 255, 255)

// ==================== 状态 ====================
static Window   g_wins[MAX_WINS];
static bool     g_used[MAX_WINS];
static Window*  g_z = nullptr;              // z 序链表：头 = 最前
static int      g_screen_w = 0, g_screen_h = 0;
static bool     g_lang_zh = true;
static bool     g_menu_open = false;
static int      g_menu_sel = 0;
static bool     g_click_right = false;
static int      g_dirty_x0, g_dirty_y0, g_dirty_x1, g_dirty_y1;   // 半开区间
static bool     g_dirty_any = false;
static int      g_cur_x = 0, g_cur_y = 0, g_prev_cur_x = -1, g_prev_cur_y = -1;
static bool     g_first_frame = true;

// ==================== ★ P5：拖拽（移动 + 八向缩放）====================
enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE };
// 缩放方向位（八向）：L/R/T/B 组合，例如 RZ_L|RZ_T = 左上角
#define RZ_L   1
#define RZ_R   2
#define RZ_T   4
#define RZ_B   8
static int  g_drag = DRAG_NONE;
static int  g_resize_dir = 0;                 // DRAG_RESIZE 时的方向位
static int  g_drag_dx = 0, g_drag_dy = 0;
static Window* g_drag_w = nullptr;

// 桌面图标（★ P5：项的集合与持久化在 desktopops64.cpp —— 这里只留"选中/拖动"的交互状态）
static int  g_icon_sel = -1;
static uint32_t g_icon_last_tick = 0;
// 桌面图标拖动（>5px 才算拖动）与空白处玻璃选择框的状态（移植自 legacy32/kernel/gui.cpp）
static int  g_icon_drag_idx = -1;              // 正在拖动的图标（-1=无）
static int  g_icon_press_x = 0, g_icon_press_y = 0;            // 按下位置
static int  g_icon_press_icon_x = 0, g_icon_press_icon_y = 0;  // 按下时的图标位置
static bool g_icon_drag_moved = false;
static bool g_start_icon_logged = false;
// 外壳自检期间（gui64_selftest 建/销临时窗口、来回切语言）不触发会话/配置钩子：
// 那些临时窗口不是真实应用实例，自检的语言切换也不该写进持久化配置。
static bool g_in_selftest = false;
static bool g_title_btn_logged = false;

// ==================== ★ P5：光标形状（状态在 input.cpp，切换在下面的命中判定里）====================
static int  g_cursor_shape = MOUSE_CUR_ARROW;
static uint32_t g_cursor_log_budget = 200;

// ==================== ★ P5：最小化飞向 Dock（缩小 + 透明度渐隐）====================
#define FLY_MAX 2
#define FLY_SNAP_W 72
#define FLY_SNAP_H 48
struct FlyAnim {
    Window*  w;
    uint32_t t0;
    int      dur, frame;
    int      fx, fy, fw, fh;      // 起点（窗口几何）
    int      tx, ty, tw, th;      // 终点（Dock 图标）
    int      px, py, pw, ph;      // 上一帧的绘制矩形（擦旧帧用）
};
static FlyAnim g_fly[FLY_MAX];
static int     g_fly_n = 0;
static uint32_t g_fly_snap[FLY_SNAP_W * FLY_SNAP_H];
static bool    g_fly_snap_ok = false;

// ==================== ★ P5：Dock 悬停窗口列表（可选择恢复指定窗口）====================
static int g_winlist_idx = -1;          // Dock 项下标（-1 = 不显示）
static int g_winlist_row = -1;          // 悬停行
static int g_winlist_x = 0, g_winlist_y = 0, g_winlist_w = 0, g_winlist_h = 0;
static int g_winlist_logged = 0;
static int g_rz_log_frames = 0;      // ★ P5：一次缩放拖动最多打 24 行"帧"（begin 处清零）

// ==================== ★ P5：程序加载中的"转圈"光标 + 恢复时的 scale/opacity 弹出 ====================
static Window*  g_load_w = nullptr;
static uint32_t g_load_t0 = 0;
static Window*  g_pop_w = nullptr;
static int      g_load_paints = 0;           // ★ P5：加载中的窗口已经画了几帧
static uint32_t g_pop_t0 = 0;
static bool     g_load_done = true;          // ★ P5：加载中的窗口是否已经画完第一帧（转圈结束条件）
static int      g_pop_dur = 0;
static int      g_pop_frame = 0;

// ---- ★ P5：本批新增函数的**前置声明**（定义在文件后半段"桌面交互细节"一节里）----
static int  resize_dir64(const Window* w, int mx, int my);
static int  resize_dir_shape64(int dir);
static const char* resize_dir_name64(int dir);
static void cursor_shape_update64(int mx, int my);
static Window* top_visible_win64(void);
static void fly_begin64(Window* w);
static int  fly_tick64(void);
static void fly_draw64(void);
static void pop_begin64(Window* w, const char* why);
static int  pop_active64(void);
static void pop_geom64(const Window* w, int* x, int* y, int* ww, int* hh);
static void pop_tick64(void);
static void winlist_update64(int mx, int my);
static void winlist_draw64(void);
static int  winlist_press64(int mx, int my);
static int  dock_anim_tick64(void);
static void log_win_geom64(const Window* win, const char* tag);
// ==================== 配置接线状态（config64 读到外壳里的值）====================
static bool g_text_mirror = false;      // ui.text_mirror：外壳 TrueType 文本水平镜像
static int  g_mouse_sens = 1700;        // mouse.sens（千分比）：1700 = 驱动基线（默认不变）
static bool g_mouse_sens_off = false;   // sens != 1700 才启用缩放（默认路径零行为变化）
static int  g_sens_base_x = -1, g_sens_base_y = -1;   // 上一次看到的驱动位置
static int  g_sens_rem_x = 0, g_sens_rem_y = 0;       // 缩放余数（避免整数除法丢位移）

// 鼠标灵敏度：把驱动**本帧位移**按 sens/1700 缩放后写回驱动（绝对值写回，驱动的内部累加器不受影响）。
// sens == 1700 时整个函数直接返回 —— 默认行为与改动前完全一致（既有鼠标验收不受影响）。
static void mouse_apply_sensitivity() {
    if (!g_mouse_sens_off) return;
    const int rx = mouse_get_x(), ry = mouse_get_y();
    if (g_sens_base_x < 0) { g_sens_base_x = rx; g_sens_base_y = ry; return; }
    const int dx = rx - g_sens_base_x, dy = ry - g_sens_base_y;
    if (!dx && !dy) return;
    g_sens_base_x = rx; g_sens_base_y = ry;
    g_sens_rem_x += dx * g_mouse_sens;
    g_sens_rem_y += dy * g_mouse_sens;
    const int sx = g_sens_rem_x / 1700, sy = g_sens_rem_y / 1700;
    g_sens_rem_x -= sx * 1700;
    g_sens_rem_y -= sy * 1700;
    if (sx || sy) mouse_set_pos(rx - dx + sx, ry - dy + sy);
}

// 帧率/忙占比统计
static uint32_t g_frames = 0;
static uint32_t g_fps = 0;
static uint32_t g_fps_t0 = 0;
static uint32_t g_fps_count = 0;
static uint64_t g_busy_cycles = 0, g_total_cycles = 0;
static uint8_t  g_busy_pct = 0;

static inline uint64_t rdtsc64() {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ==================== 脏矩形 ====================
static void dirty_add(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > g_screen_w) x1 = g_screen_w;
    if (y1 > g_screen_h) y1 = g_screen_h;
    if (x1 <= x || y1 <= y) return;
    if (!g_dirty_any) {
        g_dirty_x0 = x; g_dirty_y0 = y; g_dirty_x1 = x1; g_dirty_y1 = y1;
        g_dirty_any = true;
    } else {
        if (x < g_dirty_x0) g_dirty_x0 = x;
        if (y < g_dirty_y0) g_dirty_y0 = y;
        if (x1 > g_dirty_x1) g_dirty_x1 = x1;
        if (y1 > g_dirty_y1) g_dirty_y1 = y1;
    }
}
void gui64_dirty(int x, int y, int w, int h) { dirty_add(x, y, w, h); }
void gui64_invalidate() { dirty_add(0, 0, g_screen_w, g_screen_h); }

// hover 高亮用：把 (x,y) 处窗口的标题栏三按钮区域标脏（光标移入/移出都要重画）
static void dirty_title_buttons_at(int x, int y) {
    for (Window* w = g_z; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        const int bx = w->x + w->w - BORDER - BTN_W * 3;
        if (x >= bx - 2 && x < bx + BTN_W * 3 + 4 && y >= w->y && y < w->y + TITLE_H + BORDER) {
            dirty_add(bx - 2, w->y, BTN_W * 3 + 8, TITLE_H + BORDER);
            return;
        }
    }
}

// ==================== 窗口内部工具 ====================
static int win_index(Window* w) {
    if (!w) return -1;
    for (int i = 0; i < MAX_WINS; i++) if (&g_wins[i] == w) return i;
    return -1;
}
bool gui64_window_alive(Window* w) {
    int i = win_index(w);
    return i >= 0 && g_used[i] && !g_wins[i].closing;
}
static void recompute_client(Window* w) {
    w->client_x = w->x + BORDER;
    w->client_y = w->y + DECO_H;
    w->client_w = w->w - DECO_W;
    w->client_h = w->h - DECO_H - BORDER;
    if (w->client_w < 1) w->client_w = 1;
    if (w->client_h < 1) w->client_h = 1;
}
static void z_unlink(Window* w) {
    Window** pp = &g_z;
    while (*pp) {
        if (*pp == w) { *pp = w->next; return; }
        pp = &(*pp)->next;
    }
}
static void z_push_front(Window* w) {         // 置顶
    z_unlink(w);
    w->next = g_z;
    g_z = w;
}
static Window* win_at(int x, int y) {         // 命中测试（从最前开始）
    for (Window* w = g_z; w; w = w->next) {
        if (!w->visible || w->minimized) continue;
        if (x >= w->x && x < w->x + w->w && y >= w->y && y < w->y + w->h) return w;
    }
    return nullptr;
}

// ==================== 窗口 API ====================
Window* gui64_create_window(const char* title, int x, int y, int w, int h,
                            WindowDrawFn draw, WindowKeyFn on_key,
                            WindowClickFn on_click, int app_id) {
    int slot = -1;
    for (int i = 0; i < MAX_WINS; i++) if (!g_used[i]) { slot = i; break; }
    if (slot < 0) {
        dbg64_str("[GUI64] create_window: no free slot");
        dbg64_nl();
        return nullptr;
    }
    Window* win = &g_wins[slot];
    // 手工清零（不用 memset 以免依赖外部符号；也避免动到 next 指针）
    win->x = x; win->y = y; win->w = w; win->h = h;
    win->min_w = 120; win->min_h = 80;
    win->visible = true; win->active = false;
    win->minimized = false; win->maximized = false;
    win->norm_x = x; win->norm_y = y; win->norm_w = w; win->norm_h = h;
    win->draw = draw; win->on_key = on_key; win->on_click = on_click;
    win->on_click2 = nullptr; win->on_tick = nullptr; win->on_close = nullptr;
    win->userdata = nullptr;
    win->app_id = (uint8_t)app_id;
    win->mem_owner = MEM_OWNER_APPS_64;
    win->next = nullptr;
    win->anim_t0 = 0; win->anim_dur = 0; win->anim_kind = 0;
    win->closing = 0; win->resizing = 0;
    win->cpu_cycles = 0; win->cpu_pct = 0;
    for (int i = 0; i < 32; i++) win->title[i] = 0;
    if (title) {
        int i = 0;
        for (; title[i] && i < 31; i++) win->title[i] = title[i];
        win->title[i] = 0;
    }
    // 夹到屏幕内
    if (w > g_screen_w) win->w = w = g_screen_w;
    if (h > g_screen_h - TASKBAR_H) win->h = h = g_screen_h - TASKBAR_H;
    if (win->x + w > g_screen_w) win->x = g_screen_w - w;
    if (win->y < 0) win->y = 0;
    if (win->x < 0) win->x = 0;
    recompute_client(win);
    g_used[slot] = true;
    z_push_front(win);
    gui64_set_active(win);
    dirty_add(win->x, win->y, win->w, win->h);
    // ★ P5：窗口刚建出来 = "程序加载中" —— 光标移到它上面会变成转圈（[INPUT64] cursor shape=wait）
    g_load_w = win;
    g_load_done = false;         // ★ P5：新窗口还没画完第一帧 -> 光标移到它上面是转圈
    g_load_paints = 0;
    g_load_t0 = ticks64();
    log_win_geom64(win, "new");   // ★ P5：窗口几何打点（自动验收要知道窗口在哪）
    if (!g_in_selftest)                        // 自检的临时窗口不算应用实例
        session64_app_opened64(app_id);        // 会话策略：清掉"待清理"标志（关掉又立刻打开的场景）
    return win;
}

void gui64_destroy_window(Window* w) {
    int i = win_index(w);
    if (i < 0 || !g_used[i]) return;
    const int w_app = (int)w->app_id;         // 销毁后 app_id 会留在槽里，但语义上属于"已关"
    dirty_add(w->x, w->y, w->w, w->h);
    if (w->on_close) w->on_close(w);          // 应用在这里释放 userdata（外壳绝不代劳）
    z_unlink(w);
    w->next = nullptr;
    w->draw = nullptr; w->on_key = nullptr; w->on_click = nullptr;
    w->on_click2 = nullptr; w->on_tick = nullptr; w->on_close = nullptr;
    w->userdata = nullptr;
    w->visible = false;
    g_used[i] = false;
    // 会话策略（session64）：关窗后是否清该应用的内容状态。这里只登记，真正的 reset 在
    // gui64 主循环的 session64_tick64() 里执行 —— 那是唯一"没有窗口销毁在栈上"的安全点。
    if (!g_in_selftest) session64_app_closed64(w_app);
    if (g_drag_w == w) { g_drag_w = nullptr; g_drag = DRAG_NONE; }
    // 焦点交给最上面的窗口
    Window* top = g_z;
    while (top && (!top->visible || top->minimized)) top = top->next;
    if (top) gui64_set_active(top);
}

// ★ P5：窗口几何打点（自动验收定位窗口用；创建与每次改几何都打一行）
static void log_win_geom64(const Window* win, const char* tag) {
    dbg64_line_begin64();
    dbg64_str("[UI] win geom tag=");
    dbg64_str(tag);
    dbg64_str(" title=");
    dbg64_str(win->title);
    dbg64_str(" app=");
    dbg64_dec((uint64_t)win->app_id);
    dbg64_str(" x=");
    dbg64_dec((uint64_t)win->x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)win->y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)win->w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)win->h);
    dbg64_str(" client=");
    dbg64_dec((uint64_t)win->client_w);
    dbg64_str("x");
    dbg64_dec((uint64_t)win->client_h);
    dbg64_str(" grip=");
    dbg64_dec((uint64_t)GUI64_GRIP);
    dbg64_nl();
    dbg64_line_end64();
}


void gui64_set_active(Window* w) {
    for (Window* p = g_z; p; p = p->next) p->active = false;
    if (!w) return;
    w->active = true;
    w->minimized = false;
    z_push_front(w);
    dirty_add(w->x, w->y, w->w, w->h);
}

void gui64_set_title(Window* w, const char* utf8) {
    if (!gui64_window_alive(w)) return;
    int i = 0;
    for (; utf8 && utf8[i] && i < 31; i++) w->title[i] = utf8[i];
    w->title[i] = 0;
    dirty_add(w->x, w->y, w->w, TITLE_H + BORDER);
}

void gui64_set_min_size(Window* w, int mw, int mh) {
    if (!gui64_window_alive(w)) return;
    w->min_w = mw > 40 ? mw : 40;
    w->min_h = mh > 40 ? mh : 40;
}

void gui64_set_click2(Window* w, WindowClick2Fn fn) { if (gui64_window_alive(w)) w->on_click2 = fn; }
void gui64_set_tick(Window* w, WindowTickFn fn)     { if (gui64_window_alive(w)) w->on_tick = fn; }

void gui64_set_geom(Window* w, int x, int y, int cw, int ch) {
    if (!gui64_window_alive(w)) return;
    dirty_add(w->x, w->y, w->w, w->h);
    w->x = x; w->y = y;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    w->w = cw + DECO_W;
    w->h = ch + DECO_H + BORDER;
    if (w->w < w->min_w) w->w = w->min_w;
    if (w->h < w->min_h) w->h = w->min_h;
    recompute_client(w);
    dirty_add(w->x, w->y, w->w, w->h);
    log_win_geom64(w, "set");    // ★ P5：应用调用 set_geom 后几何变了 -> 打点（自动验收按它定位）
}

// 只在不违反最小尺寸时把窗口调整到指定客户区尺寸（返回 1 = 已应用）
int gui64_fit_window_to_client(Window* w, int cw, int ch) {
    if (!gui64_window_alive(w)) return 0;
    const int nw = cw + DECO_W, nh = ch + DECO_H + BORDER;
    if (nw < w->min_w || nh < w->min_h) return 0;
    if (nw == w->w && nh == w->h) return 1;
    dirty_add(w->x, w->y, w->w, w->h);
    w->w = nw; w->h = nh;
    if (w->x + w->w > g_screen_w) w->x = g_screen_w - w->w;
    if (w->y + w->h > g_screen_h - TASKBAR_H) w->y = g_screen_h - TASKBAR_H - w->h;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    recompute_client(w);
    log_win_geom64(w, "fit");    // ★ P5：应用自己改了几何（终端按列/行排版）也要能定位
    dirty_add(w->x, w->y, w->w, w->h);
    return 1;
}

void gui64_invalidate_window(Window* w) {
    if (!gui64_window_alive(w)) return;
    dirty_add(w->x, w->y, w->w, w->h);
}
void gui64_flip_window(Window* w) {
    if (!gui64_window_alive(w)) return;
    fb_flip_region(w->x, w->y, w->w, w->h);
}

int     gui64_screen_w()        { return g_screen_w; }
int     gui64_screen_h()        { return g_screen_h; }
int     gui64_taskbar_h()       { return TASKBAR_H; }
bool    gui64_start_menu_open() { return g_menu_open; }
Window* gui64_top_window()      { return g_z; }
bool    gui64_click_is_right()  { return g_click_right; }
int     gui64_window_count() {
    int n = 0;
    for (Window* w = g_z; w; w = w->next) if (w->visible) n++;
    return n;
}
Window* gui64_window_at(int i) {
    int k = 0;
    for (Window* w = g_z; w; w = w->next) {
        if (!w->visible) continue;
        if (k == i) return w;
        k++;
    }
    return nullptr;
}
int gui64_app_windows(int app_id) {
    int n = 0;
    for (Window* w = g_z; w; w = w->next)
        if (w->visible && w->app_id == (uint8_t)app_id) n++;
    return n;
}
void gui64_close_all_windows() {
    for (int i = 0; i < MAX_WINS; i++)
        if (g_used[i]) gui64_destroy_window(&g_wins[i]);
}
int gui64_close_app(int app_id) {
    int n = 0;
    // 先收集再销毁：销毁会改链表
    for (int i = 0; i < MAX_WINS; i++) {
        if (g_used[i] && g_wins[i].app_id == (uint8_t)app_id) {
            gui64_destroy_window(&g_wins[i]);
            n++;
        }
    }
    return n;
}
bool        gui64_lang_zh() { return g_lang_zh; }
void        gui64_set_lang_zh(bool zh) {
    // 界面语言是 config64 的 ui.lang（-> store64 持久化）：这样"设置页切语言 -> 重启后还是该语言"成立。
    // 自检（gui64_selftest 会来回切语言）只改内存，不写持久化配置 —— 否则每次启动都会写盘。
    if (g_in_selftest) { g_lang_zh = zh; return; }
    cfg64_set_lang_zh64(zh ? 1 : 0);
    if (g_lang_zh == zh) return;
    g_lang_zh = zh;
    gui64_invalidate();
    dbg64_str(zh ? "[UI] lang zh" : "[UI] lang en");
    dbg64_nl();
    dbg64_line_begin64();
    dbg64_str("[CONF64] set key=ui.lang value=");
    dbg64_dec(zh ? 1 : 0);
    dbg64_str(" type=int (persisted via store64)");
    dbg64_nl();
    dbg64_line_end64();
}
const char* gui64_tr(const char* en, const char* zh) { return g_lang_zh ? zh : en; }
uint32_t    gui64_fps() { return g_fps; }
uint8_t     gui64_cpu_busy_pct() { return g_busy_pct; }


// 关机/重启画面（实现放在后面的"绘制区"：要用那里的 blit_rgba/text_ttf/text_w）
static void power_anim_screen(const char* txt, const char* log_tag);
// ==================== 重启 / 关机（照抄 32 位回退链）====================
void sys_reboot64() {
    // 优雅停止：状态机 STOPPING -> 逆序停模块（含关窗）-> 会话快照 + 配置落盘 flush（store64）。
    // 看门狗在这里被挂起（sysstate64_stop_all64 内部调 panic64_watchdog_suspend64）。
    (void)sysstate64_stop_all64("reboot");
    power_anim_screen(gui64_tr("Restarting...", "正在重启..."), "[UI] reboot anim start");
    dbg64_str("[RESET] sys_reboot64: 8042 pulse");
    dbg64_nl();
    while (inb(0x64) & 1) (void)inb(0x60);     // 排空
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 3000000; i++) { }
    dbg64_str("[RESET] 8042 failed -> PCI reset (0xCF9)");
    dbg64_nl();
    outb(0xCF9, 0x02);
    for (volatile int i = 0; i < 2000; i++) { }
    outb(0xCF9, 0x06);
    for (volatile int i = 0; i < 3000000; i++) { }
    dbg64_str("[RESET] 0xCF9 failed -> triple fault");
    dbg64_nl();
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0" ::"m"(null_idt));
    __asm__ volatile("int $0x03");
    dbg64_str("[RESET] FAILED (hardware refuses to reset)");
    dbg64_nl();
    for (;;) __asm__ volatile("cli; hlt");
}

void sys_shutdown64() {
    (void)sysstate64_stop_all64("shutdown");
    power_anim_screen(gui64_tr("Shutting down...", "正在关机..."), "[UI] shutdown anim start");
    dbg64_str("[SHUTDOWN] sys_shutdown64: ACPI 0x604");
    dbg64_nl();
    outw(0x604, 0x2000);                        // QEMU/VMware 的 ACPI 断电
    for (volatile int i = 0; i < 3000000; i++) { }
    dbg64_str("[SHUTDOWN] 0x604 failed -> 8042 0xFE");
    dbg64_nl();
    outb(0x64, 0xFE);
    for (volatile int i = 0; i < 3000000; i++) { }
    dbg64_str("[SHUTDOWN] FAILED -> halt");
    dbg64_nl();
    for (;;) __asm__ volatile("cli; hlt");
}

// ==================== 图标绘制 ====================
// 把任意尺寸 RGBA 源图最近邻缩放到 dw×dh，并**与后备缓冲现有像素做 alpha 混合**
// （驱动自带的 fb_blit_rgba 是"对黑底混合"，直接铺在蓝色桌面上会发暗，所以这里自己混）。
// a_scale：0..255 的整体不透明度乘数（开机 logo 淡入用；255 = 原样）。
static void blit_rgba(int x, int y, int dw, int dh, const uint8_t* src, int sw, int sh, int a_scale) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    for (int j = 0; j < dh; j++) {
        const int sy = j * sh / dh;
        for (int i = 0; i < dw; i++) {
            const int sx = i * sw / dw;
            const uint8_t* p = src + (((size_t)sy * (size_t)sw) + (size_t)sx) * 4;
            int a = p[3];
            if (a == 0 || a_scale <= 0) continue;
            if (a_scale < 255) a = a * a_scale / 255;
            if (a <= 0) continue;
            const int px = x + i, py = y + j;
            if (px < 0 || py < 0 || px >= g_screen_w || py >= g_screen_h) continue;
            uint32_t out;
            if (a >= 252) {
                out = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            } else {
                const uint32_t bg = fb_get_pixel(px, py);
                const uint32_t r = ((uint32_t)p[0] * (uint32_t)a + ((bg >> 16) & 0xFF) * (uint32_t)(255 - a)) / 255;
                const uint32_t g = ((uint32_t)p[1] * (uint32_t)a + ((bg >> 8) & 0xFF) * (uint32_t)(255 - a)) / 255;
                const uint32_t b = ((uint32_t)p[2] * (uint32_t)a + (bg & 0xFF) * (uint32_t)(255 - a)) / 255;
                out = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            fb_putpixel(px, py, out);
        }
    }
}

// ==================== 开机 logo（淡入）====================
// 桌面首帧之前跑：黑底 + 屏幕居中 logo（240x150 RGBA，logo/logo.png）；
// 12 帧逐帧提高不透明度，帧间用 ticks64() 节流到 ~60Hz，每帧只提交 logo 那块小矩形。
static void boot_logo_fade_in(void) {
    const int x0 = (g_screen_w - LOGO_W) / 2;
    const int y0 = (g_screen_h - LOGO_H) / 2;
    const int frames = 12;
    fb_clear(rgb(0, 0, 0));
    fb_flip();
    uint32_t next = ticks64();
    for (int f = 1; f <= frames; f++) {
        int guard = 0;   // 护栏：万一 PIT 停摆也不至于死在等待里
        while ((int32_t)(ticks64() - next) < 0 && ++guard < 2000000) __asm__ volatile("pause");
        next += PIT_HZ_64 / 60;
        blit_rgba(x0, y0, LOGO_W, LOGO_H, _binary_logo_rgba_bin_start, LOGO_W, LOGO_H,
                  f * 255 / frames);
        fb_flip_region(x0, y0, LOGO_W, LOGO_H);
    }
    dbg64_str("[UI] boot logo show frames=");
    dbg64_dec((uint64_t)frames);
    dbg64_str(" fade=ok");
    dbg64_nl();
}

static const uint8_t* icon_src(int kind) {
    if (kind == 0) return _binary_icon_mycomputer_bin_start;
    if (kind == 1) return _binary_icon_recyclebin_bin_start;
    return _binary_icon_terminal_bin_start;
}
// ★ 本批（真图标）：桌面图标 kind 0..2 <-> 外置图标包里的应用 kind
static const int kIcon64AppKind[3] = { ICON64_A_MYPC, ICON64_A_RECYCLE, ICON64_A_TERMINAL };
// （icon_name 已随桌面图标绘制一起搬到 desktopops64.cpp：item_name()）

// ==================== 桌面图标预缩放缓存（preload64 预热用） ====================
// 背景：draw_icons()/draw_taskbar() 原来**每帧**对 128x128 源图做最近邻缩放（桌面图标 3 张 +
//   开始图标 1 张）；preload64 在进桌面之前把缩小后的位图算好放进缓存，绘制时只做 alpha 混合。
// 缓存与 blit_rgba 的采样公式完全一致（sx = i*sw/dw，sy = j*sh/dh），所以**像素结果逐点相同**，
//   只是把"缩放"从每帧挪到启动期一次（32 位 preload.cpp 的 gui_preload_icons 同一思路）。
static uint8_t g_icon48_cache[3][ICON_W * ICON_W * 4];
static uint8_t g_icon48_ok[3];
static uint8_t g_start24_cache[START_ICON_DISP * START_ICON_DISP * 4];
static bool    g_start24_ok = false;

// 最近邻预缩放（RGBA 原样拷贝 alpha；公式与 blit_rgba 相同）
static void scale_rgba64(const uint8_t* src, int sw, int sh, uint8_t* dst, int dw, int dh) {
    for (int j = 0; j < dh; j++) {
        const int sy = j * sh / dh;
        for (int i = 0; i < dw; i++) {
            const int sx = i * sw / dw;
            const uint8_t* p = src + (((size_t)sy * (size_t)sw) + (size_t)sx) * 4;
            uint8_t* q = dst + (((size_t)j * (size_t)dw) + (size_t)i) * 4;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = p[3];
        }
    }
}

// 预缩放后的位图直接混合上屏（步长 = dw，不再缩放；混合公式与 blit_rgba 相同）
static void blit_rgba_scaled(int x, int y, int dw, int dh, const uint8_t* src) {
    for (int j = 0; j < dh; j++) {
        for (int i = 0; i < dw; i++) {
            const uint8_t* p = src + (((size_t)j * (size_t)dw) + (size_t)i) * 4;
            int a = p[3];
            if (a == 0) continue;
            const int px = x + i, py = y + j;
            // 边界用 fb_width/fb_height（= 渲染缓冲的真实尺寸；gui64_run 里 g_screen_* 就等于它们）。
            // 为什么不用 g_screen_*：preload64 在 gui64_run **之前**跑，那时 g_screen_w=0，
            // 用它会一像素都不画，预热前后的测量就都不成立。
            if (px < 0 || py < 0 || px >= fb_width() || py >= fb_height()) continue;
            uint32_t out;
            if (a >= 252) {
                out = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            } else {
                const uint32_t bg = fb_get_pixel(px, py);
                const uint32_t r = ((uint32_t)p[0] * (uint32_t)a + ((bg >> 16) & 0xFF) * (uint32_t)(255 - a)) / 255;
                const uint32_t g = ((uint32_t)p[1] * (uint32_t)a + ((bg >> 8) & 0xFF) * (uint32_t)(255 - a)) / 255;
                const uint32_t b = ((uint32_t)p[2] * (uint32_t)a + (bg & 0xFF) * (uint32_t)(255 - a)) / 255;
                out = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
            fb_putpixel(px, py, out);
        }
    }
}

// 预缩放：3 个桌面图标（128x128 -> ICON_W）与开始图标（64x64 -> START_ICON_DISP）。
// 返回建好的缓存位图数（4 = 全部成功；资源缺失时如实返回较小的数）。
int gui64_preload_icons64() {
    int n = 0;
    for (int k = 0; k < 3; k++) {
        // ★ 本批（真图标）：优先用外置图标包里的应用图标（Dock/桌面同一份缓存，绘制路径不变）；
        //   包里没有（或解码失败）才用内核内嵌的程序化图标 —— 绝不因为缺文件把图标画空。
        const int ak = kIcon64AppKind[k];
        if (ak > 0 && icons64_export_rgba64(ak, ICON_W, g_icon48_cache[k], ICON_W, ICON_W,
                                            icons64_app_color64(ak)) == 0) {
            g_icon48_ok[k] = 1;
            n++;
            continue;
        }
        const uint8_t* src = icon_src(k);
        if (!src) continue;
        scale_rgba64(src, ICON_SRC_W, ICON_SRC_W, g_icon48_cache[k], ICON_W, ICON_W);
        g_icon48_ok[k] = 1;
        n++;
    }
    if (_binary_icon_start_bin_start) {
        scale_rgba64(_binary_icon_start_bin_start, START_ICON_SRC, START_ICON_SRC,
                     g_start24_cache, START_ICON_DISP, START_ICON_DISP);
        g_start24_ok = true;
        n++;
    }
    return n;
}

int gui64_icon_cache_count64() {
    int n = 0;
    for (int k = 0; k < 3; k++) if (g_icon48_ok[k]) n++;
    if (g_start24_ok) n++;
    return n;
}

// 画桌面图标（kind 0..2）：有缓存只做混合；没缓存（进桌面之前）按旧路径的等价工作量做
// "缩放进临时位图 + 混合"（采样公式与 blit_rgba 完全相同，像素逐点一致）。
void gui64_draw_icon_kind64(int x, int y, int kind) {
    if (x < 0 || y < 0 || kind < 0 || kind > 2) return;
    if (g_icon48_ok[kind]) { blit_rgba_scaled(x, y, ICON_W, ICON_W, g_icon48_cache[kind]); return; }
    static uint8_t scratch[3][ICON_W * ICON_W * 4];
    scale_rgba64(icon_src(kind), ICON_SRC_W, ICON_SRC_W, scratch[kind], ICON_W, ICON_W);
    blit_rgba_scaled(x, y, ICON_W, ICON_W, scratch[kind]);
}

void gui64_draw_start_icon64(int x, int y) {
    if (x < 0 || y < 0) return;
    if (g_start24_ok) { blit_rgba_scaled(x, y, START_ICON_DISP, START_ICON_DISP, g_start24_cache); return; }
    static uint8_t scratch[START_ICON_DISP * START_ICON_DISP * 4];
    scale_rgba64(_binary_icon_start_bin_start, START_ICON_SRC, START_ICON_SRC,
                 scratch, START_ICON_DISP, START_ICON_DISP);
    blit_rgba_scaled(x, y, START_ICON_DISP, START_ICON_DISP, scratch);
}

// 选择框与图标格（图标 + 名字标签）是否相交（移植 32 位的 icon_intersects_sel）
// （icon_hits_sel 已搬到 desktopops64.cpp：desktopops64_rect_hit64）
static void mirror_rect_h(int x, int y, int w, int h) {
    if (w <= 1) return;
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w / 2; i++) {
            const int xa = x + i, xb = x + w - 1 - i;
            const uint32_t a = fb_get_pixel(xa, y + j);
            const uint32_t b = fb_get_pixel(xb, y + j);
            fb_putpixel(xa, y + j, b);
            fb_putpixel(xb, y + j, a);
        }
    }
}
static void text_ttf(int x, int y, const char* s, uint32_t fg) {
    font_select(2);                 // simhei 子集：ASCII + 常用汉字，混排不出豆腐块
    font_draw_text(x, y, s, fg);
    if (g_text_mirror) {            // ui.text_mirror（config64 -> store64 持久化）
        const int w = font_text_width(s);
        const int h = font_line_height();
        if (w > 1 && h > 0) mirror_rect_h(x, y, w, h);
    }
}
static int text_w(const char* s) { font_select(2); return font_text_width(s); }

// 小工具：字符串比较/复制（内核里没有 libc）
static bool str_eq64(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void str_copy64(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static char g_clock_log[40] = {0};   // 上一次打点的时钟文本（变化时才打，避免每秒刷屏以外的噪声）

// ==================== 关机 / 重启画面 ====================
// 黑底 + 居中 logo + 一行提示 + 进度条动画，停留约 1 秒，再走复位/断电链。
// 每帧只提交进度条那条小矩形（不整屏拷贝），用 ticks64() 计时。
static void power_anim_screen(const char* txt, const char* log_tag) {
    dbg64_str(log_tag);
    dbg64_nl();
    const int x0 = (g_screen_w - LOGO_W) / 2;
    const int y0 = (g_screen_h - LOGO_H) / 2 - 20;
    const int pw = 360, ph = 12;
    const int px = (g_screen_w - pw) / 2;
    const int py = y0 + LOGO_H + 56;
    fb_clear(rgb(0, 0, 0));
    blit_rgba(x0, y0, LOGO_W, LOGO_H, _binary_logo_rgba_bin_start, LOGO_W, LOGO_H, 255);
    const int tw = text_w(txt);
    text_ttf((g_screen_w - tw) / 2, y0 + LOGO_H + 22, txt, rgb(240, 240, 240));
    fb_draw_rect(px - 1, py - 1, pw + 2, ph + 2, rgb(120, 120, 120));
    fb_flip();
    const uint32_t t0 = ticks64();
    uint32_t next = t0;
    int frames = 0;
    while ((int32_t)(ticks64() - t0) < (int32_t)PIT_HZ_64 && frames < 240) {   // ~1s
        const uint32_t now = ticks64();
        if ((int32_t)(now - next) >= 0) {
            next += PIT_HZ_64 / 60;
            int pct = frames * 100 / 60;
            if (pct > 100) pct = 100;
            const int fill = pw * pct / 100;
            fb_fill_rect(px, py, pw, ph, rgb(32, 32, 32));
            if (fill > 0) fb_fill_rect(px, py, fill, ph, rgb(0, 120, 215));
            fb_flip_region(px - 1, py - 1, pw + 2, ph + 2);
            frames++;
        }
        __asm__ volatile("pause");
    }
    dbg64_str("[UI] power anim frames=");
    dbg64_dec((uint64_t)frames);
    dbg64_nl();
}

// ==================== Windows 11 风格 Dock（本批：替换老任务栏）====================
// 几何（需求原文）：居中靠下、离屏幕底边 16px、高 60 → dock_y = 屏高 − 76；圆角 24、图标 44–48（取 46）、
//   图标间距 10–12（取 11）；最左固定开始按钮（图标 = logo/kaisi.png）；运行中应用底部小圆点；
//   最小化后图标下方"小横杠"（宽≈图标 40%、跟随主题强调色、**纯视觉不可点击**）。
// 动效：悬停放大 1.20 且邻位 1.04 轻微让位；点击上下回弹（弹簧曲线 THEME64_MS_DOCK=260ms）；
//   减少动画开关打开时全部 0ms/1 帧到位（theme64_dur64）。
// 打点：[DOCK64] geom/items/hover/press/bounce/minbar/clock（都有行数上限，防刷屏）。
// 前置声明：老开始菜单开关（dock_press64 的开始按钮要开它；定义在下面菜单一节）
static void menu_toggle();

#define DOCK_ITEMS 9
// ★ 本批（真图标）：icon64 = 外置图标包里的应用 kind（>0 时优先画真图标；-1 = 没有对应图标）
struct DockItem64 { int app_id; int icon_kind; int icon64; const char* en; const char* zh; };
static const DockItem64 kDockItems[DOCK_ITEMS] = {
    { APP_ID_NONE,     -1, -1,                 "Start",          "开始" },     // 最左固定开始按钮（真图 logo/kaisi.png，不许换）
    { APP_ID_MYPC,      0, ICON64_A_MYPC,      "My Computer",    "我的电脑" },
    { APP_ID_RECYCLE,   1, ICON64_A_RECYCLE,   "Recycle Bin",    "回收站" },
    { APP_ID_TERM,      2, ICON64_A_TERMINAL,  "Terminal",       "终端" },
    { APP_ID_CALC,     -1, ICON64_A_CALC,      "Calculator",     "计算器" },
    { APP_ID_MINES,    -1, ICON64_A_MINES,     "Minesweeper",    "扫雷" },
    { APP_ID_SETTINGS, -1, ICON64_A_SETTINGS,  "Settings",       "设置" },
    { APP_ID_TMGR,     -1, ICON64_A_TMGR,      "Task Manager",   "任务管理器" },
    { APP_ID_MONITOR,  -1, ICON64_A_MONITOR,   "System Monitor", "系统监视器" },
};

static int  g_dock_x = 0, g_dock_y = 0, g_dock_w = 0, g_dock_h = DOCK_H;
static int  g_dock_icon = THEME64_DOCK_ICON, g_dock_gap = THEME64_DOCK_GAP;
static int  g_dock_pressed = -1;            // 当前按下的项（-1 = 无）
static int  g_dock_hover = -1;              // 悬停项
static int  g_dock_scale[DOCK_ITEMS];       // 当前缩放（256 = 100%）
static int  g_dock_shift[DOCK_ITEMS];       // 当前横向让位（px）
static int  g_dock_clock_x = 0, g_dock_clock_w = CLOCK_W;
static bool g_dock_geom_logged = false;
static int  g_dock_log_budget = 220;         // [DOCK64] press/bounce/minbar 行上限（防刷屏）
static int  g_dock_bounce_total = 0;
// 点击回弹动画状态
static int      g_dock_bounce_idx = -1;
static uint32_t g_dock_bounce_t0 = 0;
static int      g_dock_bounce_dur = 0;
static int      g_dock_bounce_frame = 0;
static int      g_dock_bounce_dy = 0;
// Dock 开始按钮图标（46x46 RGBA）：优先 VimtuFS2 的 /logo/kaisi.png、/kaisi.png，
// 兜底内核内嵌的 icon_start.bin（= 构建期 _make_start_icon.py 从 logo/kaisi.png 生成的 RGBA）。
static uint8_t g_dock_start_rgba[DOCK_START_DISP * DOCK_START_DISP * 4];
static bool    g_dock_start_ok = false;
static const char* g_dock_start_src = "builtin:icon_start.bin";

static void dock_log_start64() {
    dbg64_line_begin64();
    dbg64_str("[DOCK64] start icon src=");
    dbg64_str(g_dock_start_src);
    dbg64_str(" size=");
    dbg64_dec((uint64_t)DOCK_START_DISP);
    dbg64_str(" ok=");
    dbg64_dec((uint64_t)(g_dock_start_ok ? 1 : 0));
    dbg64_nl();
    dbg64_line_end64();
}

// 把 Img64（0xAARRGGBB）转成 RGBA 字节流（blit_rgba_scaled 的格式）
static void dock_rgba_from_img64(const Img64* im, uint8_t* dst, int dw, int dh) {
    for (int y = 0; y < dh; y++) {
        const int sy = (int)((int64_t)y * im->h / dh);
        for (int x = 0; x < dw; x++) {
            const int sx = (int)((int64_t)x * im->w / dw);
            const uint32_t c = im->px[(uint64_t)sy * im->w + sx];
            uint8_t* q = dst + (((size_t)y * dw) + x) * 4;
            q[0] = (uint8_t)((c >> 16) & 0xFF);
            q[1] = (uint8_t)((c >> 8) & 0xFF);
            q[2] = (uint8_t)(c & 0xFF);
            q[3] = (uint8_t)((c >> 24) & 0xFF);
        }
    }
}

static void dock_start_icon_init64() {
    // ★ 先把内嵌的**真 PNG 字节**幂等装进系统卷（照 /hello.vap 的既有做法），再按"VimtuFS2 优先"加载：
    //   这样任何带 VimtuFS2 系统卷的盘（安装器装出来的盘、测试夹具盘）上，开始按钮用的都是
    //   logo/kaisi.png 这个真文件的像素；裸 system.img（没有卷）时 install 如实 skip、走内置兜底。
    {
        const uint32_t plen = (uint32_t)(_binary_kaisi_png_end - _binary_kaisi_png_start);
        (void)img64_install_blob64("/logo/kaisi.png", _binary_kaisi_png_start, plen, "logo/kaisi.png");
        (void)img64_install_blob64("/kaisi.png", _binary_kaisi_png_start, plen, "logo/kaisi.png");
    }
    // 1) 优先 VimtuFS2（需求：图标/壁纸/头像优先从 VimtuFS2 读）
    const char* cand[2] = { "/logo/kaisi.png", "/kaisi.png" };
    // 打点的 src 用 `vfs:` 前缀（明确这是从 VimtuFS2 卷里读到的真图）；加载仍用裸路径。
    const char* cand_src[2] = { "vfs:/logo/kaisi.png", "vfs:/kaisi.png" };
    for (int i = 0; i < 2; i++) {
        Img64 im{};
        if (img64_load_vfs64(cand[i], &im) == 0) {
            dock_rgba_from_img64(&im, g_dock_start_rgba, DOCK_START_DISP, DOCK_START_DISP);
            img64_free64(&im);
            g_dock_start_ok = true;
            g_dock_start_src = cand_src[i];  // 打点里写实际路径（验收要求 src=vfs:/logo/kaisi.png）
            dock_log_start64();
            return;
        }
    }
    // 2) 兜底：内核内嵌 RGBA（就是 logo/kaisi.png 的内容）
    if (_binary_icon_start_bin_start) {
        scale_rgba64(_binary_icon_start_bin_start, START_ICON_SRC, START_ICON_SRC,
                     g_dock_start_rgba, DOCK_START_DISP, DOCK_START_DISP);
        g_dock_start_ok = true;
        g_dock_start_src = "builtin:icon_start.bin";
    }
    dock_log_start64();
}

// 第 i 项的**基准**左边界（不含悬停让位）
static int dock_item_x64(int i) {
    return g_dock_x + THEME64_DOCK_PAD + i * (g_dock_icon + g_dock_gap);
}
// 第 i 项的当前绘制左边界（含让位；回弹只作用于 y，所以 x 与基准一致）
static int dock_item_draw_x64(int i) { return dock_item_x64(i) + g_dock_shift[i]; }

// Dock 几何：全部由 Token + config64（dock.size/icon/gap/len）算出
static void dock_geom_init64() {
    g_dock_h = DOCK_H;
    g_dock_icon = cfg64_dock_icon64();
    g_dock_gap = cfg64_dock_gap64();
    const int len_cfg = cfg64_dock_len64();
    g_dock_w = g_dock_icon * DOCK_ITEMS + g_dock_gap * (DOCK_ITEMS - 1) + THEME64_DOCK_PAD * 2;
    if (len_cfg > 0) g_dock_w = len_cfg;
    if (g_dock_w > g_screen_w - 32) g_dock_w = g_screen_w - 32;
    g_dock_x = (g_screen_w - g_dock_w) / 2;                       // 水平居中
    g_dock_y = g_screen_h - DOCK_MARGIN - g_dock_h;               // 离屏幕底边 16px
    g_dock_clock_w = CLOCK_W;
    g_dock_clock_x = g_screen_w - DOCK_MARGIN - g_dock_clock_w;
    for (int i = 0; i < DOCK_ITEMS; i++) { g_dock_scale[i] = 256; g_dock_shift[i] = 0; }
    if (g_dock_geom_logged) return;
    g_dock_geom_logged = true;
    dbg64_line_begin64();
    dbg64_str("[DOCK64] geom x=");
    dbg64_dec((uint64_t)g_dock_x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)g_dock_y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)g_dock_w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)g_dock_h);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)THEME64_R_DOCK);
    dbg64_str(" icon=");
    dbg64_dec((uint64_t)g_dock_icon);
    dbg64_str(" gap=");
    dbg64_dec((uint64_t)g_dock_gap);
    dbg64_str(" items=");
    dbg64_dec((uint64_t)DOCK_ITEMS);
    dbg64_str(" margin=");
    dbg64_dec((uint64_t)DOCK_MARGIN);
    dbg64_str(" center=1 screen=");
    dbg64_dec((uint64_t)g_screen_w);
    dbg64_str("x");
    dbg64_dec((uint64_t)g_screen_h);
    dbg64_nl();
    dbg64_line_end64();
    for (int i = 0; i < DOCK_ITEMS; i++) {
        dbg64_line_begin64();
        dbg64_str("[DOCK64] item idx=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" app=");
        dbg64_dec((uint64_t)kDockItems[i].app_id);
        dbg64_str(" name=");
        dbg64_str(g_lang_zh ? kDockItems[i].zh : kDockItems[i].en);
        dbg64_str(" x=");
        dbg64_dec((uint64_t)dock_item_x64(i));
        dbg64_str(" y=");
        dbg64_dec((uint64_t)(g_dock_y + (g_dock_h - g_dock_icon) / 2));
        dbg64_str(" w=");
        dbg64_dec((uint64_t)g_dock_icon);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)g_dock_icon);
        dbg64_str(" cx=");
        dbg64_dec((uint64_t)(dock_item_x64(i) + g_dock_icon / 2));
        dbg64_str(" cy=");
        dbg64_dec((uint64_t)(g_dock_y + g_dock_h / 2));
        dbg64_nl();
        dbg64_line_end64();
    }
    dbg64_line_begin64();
    dbg64_str("[DOCK64] clock chip x=");
    dbg64_dec((uint64_t)g_dock_clock_x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)g_dock_y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)g_dock_clock_w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)g_dock_h);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)THEME64_R_DOCK);
    dbg64_nl();
    dbg64_line_end64();
}

// ★ P3（设置应用）：Dock 长度/图标尺寸/图标间距被设置页改过之后调一次：
//   清掉"已打点"标记 -> 重新按 config64 算几何（于是会**再打一行 [DOCK64] geom**，
//   这就是"改完 DOCK64 geom 变化"的证据）-> 整屏重绘。
//   不动任何既有绘制逻辑；只有设置页会调它（gui64.h 里没有声明，设置页自己 extern 声明）。
void gui64_dock_reload64() {
    g_dock_geom_logged = false;
    dock_geom_init64();
    gui64_invalidate();
}

// 目标缩放/让位：悬停项 120%（Token），紧邻两项 104% 且向外让位
static void dock_targets64(int* tgt_scale, int* tgt_shift) {
    for (int i = 0; i < DOCK_ITEMS; i++) { tgt_scale[i] = 256; tgt_shift[i] = 0; }
    if (g_dock_hover < 0) return;
    const int hi = g_dock_hover;
    tgt_scale[hi] = 256 * THEME64_DOCK_HOVER_PCT / 100;
    const int grow = g_dock_icon * (THEME64_DOCK_HOVER_PCT - 100) / 100;   // 悬停多出来的宽度
    for (int i = 0; i < DOCK_ITEMS; i++) {
        if (i == hi) continue;
        if (i == hi - 1 || i == hi + 1) {
            const int d = (i < hi) ? -1 : 1;
            tgt_shift[i] = d * (grow / 2 + 2);
            tgt_scale[i] = 256 * THEME64_DOCK_NEIGH_PCT / 100;
        }
    }
}

// 每帧推进 Dock 动效（悬停缩放/让位 + 点击回弹）；返回 1 = 画面有变化（需要重画 Dock 区）
static int dock_anim_tick64() {
    int changed = 0;
    int tgt_scale[DOCK_ITEMS], tgt_shift[DOCK_ITEMS];
    dock_targets64(tgt_scale, tgt_shift);
    const int reduce = theme64_reduce_motion64();
    for (int i = 0; i < DOCK_ITEMS; i++) {
        if (reduce) {                                  // 减少动画：0ms / 1 帧到位
            if (g_dock_scale[i] != tgt_scale[i] || g_dock_shift[i] != tgt_shift[i]) changed = 1;
            g_dock_scale[i] = tgt_scale[i];
            g_dock_shift[i] = tgt_shift[i];
            continue;
        }
        // 快动效 150ms：每帧按定步长逼近（60Hz 下约 9 帧到位）
        const int step = 30;
        int ds = tgt_scale[i] - g_dock_scale[i];
        int dsh = tgt_shift[i] - g_dock_shift[i];
        if (ds > -step && ds < step) {          // 快到位：直接落到目标（否则会停在 109% 这种非整数档）
            if (ds != 0) changed = 1;
            g_dock_scale[i] = tgt_scale[i];
        } else {
            g_dock_scale[i] += ds * step / 256;
            changed = 1;
        }
        if (dsh > -1 && dsh < 1) {
            if (dsh != 0) changed = 1;
            g_dock_shift[i] = tgt_shift[i];
        } else {
            g_dock_shift[i] += dsh / 3;
            changed = 1;
        }
    }
    if (g_dock_bounce_idx >= 0) {
        const uint32_t now = ticks64();
        const uint32_t dur = (uint32_t)g_dock_bounce_dur;
        if (dur == 0) {                                 // 减少动画：1 帧到位
            g_dock_bounce_dy = 0;
            dbg64_line_begin64();
            dbg64_str("[DOCK64] bounce idx=");
            dbg64_dec((uint64_t)g_dock_bounce_idx);
            dbg64_str(" frames=1 motion=reduced");
            dbg64_nl();
            dbg64_line_end64();
            g_dock_bounce_idx = -1;
            changed = 1;
        } else {
            const uint32_t el = now - g_dock_bounce_t0;
            if ((int32_t)el >= (int32_t)dur) {
                g_dock_bounce_dy = 0;
                dbg64_line_begin64();
                dbg64_str("[DOCK64] bounce idx=");
                dbg64_dec((uint64_t)g_dock_bounce_idx);
                dbg64_str(" done frames=");
                dbg64_dec((uint64_t)g_dock_bounce_frame);
                dbg64_str(" motion=spring(260ms)");
                dbg64_nl();
                dbg64_line_end64();
                g_dock_bounce_idx = -1;
                changed = 1;
            } else {
                const int t = (int)(el * 256 / dur);
                g_dock_bounce_dy = -(int)((int64_t)9 * theme64_spring64(t) / 256);  // 向上最多 9px + 回弹
                if (g_dock_bounce_frame < 24) {   // 每次回弹最多打 24 帧（防刷屏；帧计数不受影响）
                dbg64_line_begin64();
                dbg64_str("[DOCK64] bounce idx=");
                dbg64_dec((uint64_t)g_dock_bounce_idx);
                dbg64_str(" frame=");
                dbg64_dec((uint64_t)g_dock_bounce_frame);
                dbg64_str(" t=");
                dbg64_dec((uint64_t)t);
                dbg64_str(" dy=");
                dbg64_dec((uint64_t)(unsigned)(-g_dock_bounce_dy));
                dbg64_str(" dir=");
                dbg64_str(g_dock_bounce_dy < 0 ? "up" : (g_dock_bounce_dy > 0 ? "down" : "rest"));
                dbg64_nl();
                dbg64_line_end64();
                }
                g_dock_bounce_frame++;
                changed = 1;
            }
        }
    }
    return changed;
}

// 命中测试：返回 Dock 项下标（-1 = 不在 Dock 项上）；*on_clock = 落在右下时钟玻璃片上
static int dock_hit64(int mx, int my, int* on_clock) {
    if (on_clock) *on_clock = 0;
    if (my < g_dock_y || my >= g_dock_y + g_dock_h) return -1;
    // ★ 面板最下面 6px 是"运行中小圆点 / 最小化小横杠"那一行：**纯视觉，不可点击**
    //   （要求原文：最小化小横杠"纯视觉不可点击"；这一行也不该把点击落到图标上。）
    if (my >= g_dock_y + g_dock_h - 6) return -1;
    if (mx >= g_dock_clock_x && mx < g_dock_clock_x + g_dock_clock_w) {
        if (on_clock) *on_clock = 1;
        return -1;
    }
    for (int i = 0; i < DOCK_ITEMS; i++) {
        const int cx = dock_item_draw_x64(i) + g_dock_icon / 2;
        const int w = g_dock_icon * g_dock_scale[i] / 256;
        const int h = w;
        const int yy = g_dock_y + (g_dock_h - h) / 2 + (g_dock_bounce_idx == i ? g_dock_bounce_dy : 0);
        if (mx >= cx - w / 2 && mx < cx - w / 2 + w && my >= yy && my < yy + h) return i;
    }
    return -1;
}

// 某应用当前窗口数 / 是否全部最小化
static void dock_app_state64(int app_id, int* nwin, int* nvis, int* all_min) {
    int n = 0, vis = 0, mn = 0;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_used[i]) continue;
        Window* w = &g_wins[i];
        if ((int)w->app_id != app_id) continue;
        n++;
        if (w->visible) vis++;
        if (w->minimized) mn++;
    }
    *nwin = n; *nvis = vis;
    *all_min = (n > 0 && mn == n) ? 1 : 0;
}

static void dock_open_app64(int app_id) {
    switch (app_id) {
        case APP_ID_MYPC:     app_mypc_open64();     break;
        case APP_ID_RECYCLE:  app_recycle_open64();  break;
        case APP_ID_TERM:     app_term_open64();     break;
        case APP_ID_CALC:     app_calc_open64();     break;
        case APP_ID_MINES:    app_mines_open64();    break;
        case APP_ID_SETTINGS: app_settings_open64(); break;
        case APP_ID_TMGR:     app_tmgr_open64();     break;
        case APP_ID_MONITOR:  app_monitor_open64();  break;
        default: break;
    }
}

static void dock_press64(int idx) {
    if (idx < 0 || idx >= DOCK_ITEMS) return;
    // 点击回弹：减少动画时 0ms（1 帧到位）
    g_dock_bounce_idx = idx;
    g_dock_bounce_t0 = ticks64();
    g_dock_bounce_dur = (int)theme64_dur64(THEME64_MS_DOCK);
    g_dock_bounce_frame = 0;
    g_dock_bounce_dy = 0;
    g_dock_bounce_total++;
    if (idx == 0) {
        // 需求：本批点开始按钮只需要"被按下 + 打点"（真正的开始菜单是 P2）。
        // 老开始菜单仍可开（Win 键或此项），既有验收与用户习惯不被打断。
        if (g_dock_log_budget > 0) {
            g_dock_log_budget--;
            dbg64_line_begin64();
            dbg64_str("[DOCK64] start press idx=0 pressed=1 bounce=1 menu=start64 (P2 真开始菜单)");
            dbg64_nl();
            dbg64_line_end64();
        }
        startmenu64_toggle64();
        return;
    }
    const int app = kDockItems[idx].app_id;
    int n = 0, vis = 0, mn = 0;
    dock_app_state64(app, &n, &vis, &mn);
    const char* action = "open";
    if (n == 0) {
        dock_open_app64(app);
        action = "open";
    } else {
        // ★ P5（需求原文"点击 Dock 图标"的语义）：
        //   * 有小横杠（该应用全部窗口都最小化）-> 恢复**最后最小化的那个**窗口（scale+opacity 弹出）；
        //   * 没有小横杠但窗口在桌面上可见 -> 置前/聚焦；**已经在最前面（活动）时**才最小化
        //     （Windows 11 同语义，也是既有 gui_modern64_test 断言 action=minimize 的那条路径）；
        //   * 一个应用的多个窗口共用**一根**小横杠（draw_dock 里按 mn 画一次）。
        Window* last_min = nullptr;
        Window* first_vis = nullptr;
        Window* act = nullptr;
        for (int i = 0; i < MAX_WINS; i++) {
            if (!g_used[i]) continue;
            Window* w = &g_wins[i];
            if ((int)w->app_id != app) continue;
            if (w->minimized) last_min = w;            // 槽位序 ≈ 最后最小化的那个（见 gui64.h 的窗口表）
            else if (!first_vis) first_vis = w;
            if (!w->minimized && w->active) act = w;
        }
        if (act) {
            action = "minimize";
            for (int i = 0; i < MAX_WINS; i++)
                if (g_used[i] && (int)g_wins[i].app_id == app) g_wins[i].minimized = true;
            fly_begin64(act);                          // 缩小 + 渐隐飞向 Dock 图标（需求原文）
            gui64_invalidate();
        } else if (first_vis) {
            action = "activate";                       // 可见但不活动：只置前/聚焦，**不最小化**
            gui64_set_active(first_vis);
            dirty_add(first_vis->x, first_vis->y, first_vis->w, first_vis->h);
        } else if (last_min) {
            action = "restore-last-min";               // 全部最小化：恢复最后最小化的那个
            last_min->minimized = false;
            gui64_set_active(last_min);
            pop_begin64(last_min, "dock");
        } else {
            action = "restore-all";
            for (int i = 0; i < MAX_WINS; i++)
                if (g_used[i] && (int)g_wins[i].app_id == app) { g_wins[i].minimized = false; gui64_set_active(&g_wins[i]); }
            gui64_invalidate();
        }
    }
    if (g_dock_log_budget > 0) {
        g_dock_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[DOCK64] press idx=");
        dbg64_dec((uint64_t)idx);
        dbg64_str(" app=");
        dbg64_dec((uint64_t)app);
        dbg64_str(" action=");
        dbg64_str(action);
        dbg64_str(" wins=");
        dbg64_dec((uint64_t)n);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// 单个图标：圆角正方形彩色渐变底 + 中心字母（前三个内置图标用真位图）；悬停先铺 hover 底色
static void dock_draw_one64(int idx, int cx, int icon_px, int hovered, const Theme64Tokens* t) {
    const int x = cx - icon_px / 2;
    const int y = g_dock_y + (g_dock_h - icon_px) / 2 + (g_dock_bounce_idx == idx ? g_dock_bounce_dy : 0);
    if (hovered) {
        gfx64_fill_round64(x - 4, y - 4, icon_px + 8, icon_px + 8, THEME64_R_ICON + 2,
                           t->dock_hover_bg, 200);
    }
    if (idx == 0) {
        // 开始按钮：真文件 logo/kaisi.png（VimtuFS2 优先，兜底内核内嵌 RGBA）
        if (g_dock_start_ok) {
            static uint8_t scratch[DOCK_START_DISP * DOCK_START_DISP * 4];
            if (icon_px != DOCK_START_DISP) {
                scale_rgba64(g_dock_start_rgba, DOCK_START_DISP, DOCK_START_DISP, scratch, icon_px, icon_px);
                blit_rgba_scaled(x, y, icon_px, icon_px, scratch);
            } else {
                blit_rgba_scaled(x, y, icon_px, icon_px, g_dock_start_rgba);
            }
        } else {
            gfx64_grad_round64(x, y, icon_px, icon_px, THEME64_R_ICON, t->grad_a, t->grad_b, 1, 255);
        }
        return;
    }
    const int kind = kDockItems[idx].icon_kind;
    const int app64 = kDockItems[idx].icon64;
    // ★ 本批（真图标）：Dock 上的应用图标统一走"圆角渐变底 + 外置图标包里的真图标"；
    //   包缺失/解码失败 -> 落回下面的内嵌位图 / 渐变底 + 首字母（界面不会空）。
    //   （不先问 icons64_available64：让"取不到"这条路真的走到 icons64 里打 [ICON64] fallback 点）
    if (app64 > 0) {
        uint32_t a0 = t->grad_a, a1 = t->grad_b;
        if (idx % 3 == 1) { a0 = t->grad_b; a1 = t->accent; }
        else if (idx % 3 == 2) { a0 = t->accent; a1 = t->grad_a; }
        gfx64_grad_round64(x, y, icon_px, icon_px, THEME64_R_ICON, a0, a1, 1, 255);
        gfx64_stroke_round64(x, y, icon_px, icon_px, THEME64_R_ICON, rgb(255, 255, 255), 90);
        const int pad = icon_px / 8;
        if (icon_px - 2 * pad > 0 &&
            icons64_draw_kind64(app64, x + pad, y + pad, icon_px - 2 * pad,
                                icons64_app_color64(app64), 255) == 0) {
            return;
        }
    }
    if (kind >= 0 && kind <= 2) {
        // 我的电脑 / 回收站 / 终端：内核内嵌程序化位图（128x128 -> icon_px）
        blit_rgba(x, y, icon_px, icon_px, icon_src(kind), ICON_SRC_W, ICON_SRC_W, 255);
        return;
    }
    // 其它应用：圆角正方形 + 主题渐变（扁平但有立体感：渐变 + 1px 高光边）
    uint32_t c0 = t->grad_a, c1 = t->grad_b;
    if (idx % 3 == 1) { c0 = t->grad_b; c1 = t->accent; }
    else if (idx % 3 == 2) { c0 = t->accent; c1 = t->grad_a; }
    gfx64_grad_round64(x, y, icon_px, icon_px, THEME64_R_ICON, c0, c1, 1, 255);
    gfx64_stroke_round64(x, y, icon_px, icon_px, THEME64_R_ICON, rgb(255, 255, 255), 90);
    // 中心字母（8x8 位图字体；图标 >=40px 时 scale 3 ≈ 24px）
    const char* nm = g_lang_zh ? kDockItems[idx].zh : kDockItems[idx].en;
    char ch = nm[0];
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    const int sc = icon_px >= 40 ? 3 : 2;
    const int tx = x + (icon_px - 8 * sc) / 2;
    const int ty = y + (icon_px - 8 * sc) / 2;
    fb_draw_char(tx, ty, ch, rgb(255, 255, 255), rgb(0, 0, 0), sc);
}

// Dock 阴影的包围盒（面板 + 远层阴影的最大外扩）：render 用它判断脏区是否需要补画阴影
static void dock_shadow_box64(int* bx, int* by, int* bw, int* bh) {
    const int pad = THEME64_SH_F_BLUR * 2 + 4;
    *bx = g_dock_x - pad;
    *by = g_dock_y - pad;
    *bw = g_dock_w + pad * 2;
    *bh = g_dock_h + pad * 2 + THEME64_SH_F_DY;
}
// Dock 的双层浅阴影（近层 0/2/4 a=0.08 + 远层 0/12/32 a=0.12，Token）——
// ★ 它是"壁纸之上、窗口之下"的**静态图层**：任何一块脏区重画了壁纸，落在阴影里的那部分都会消失，
//   所以 render() 在脏区压到阴影包围盒时会按脏区补画（mask 的 blit 本来就与裁剪求交，代价 = 脏区大小）。
static void draw_dock_shadow(void) {
    const Theme64Tokens* t = theme64_tokens64();
    gfx64_shadow64(g_dock_x, g_dock_y, g_dock_w, g_dock_h, THEME64_R_DOCK, t);
    gfx64_shadow64(g_dock_clock_x, g_dock_y, g_dock_clock_w, g_dock_h, THEME64_R_DOCK, t);
}

// Dock 面板 + 图标 + 运行点/最小化小横杠 + 右下角时钟玻璃片（替换老 32px 任务栏）
static void draw_dock(void) {
    const Theme64Tokens* t = theme64_tokens64();
    // 1) 阴影不在这里画（见 draw_dock_shadow：它由 render 按脏区补画，避免被壁纸重绘抹掉）
    // 2) 毛玻璃(亚克力)：非暗色主题 = **固定默认色**（Token dock_bg，+ 一点背景材质感）；
    //    暗色主题 = 深灰半透（Token 0.45）+ 内高光
    const int alpha = t->dark ? THEME64_A_BACKDROP : (THEME64_A_CARD + 40);
    gfx64_glass64(g_dock_x, g_dock_y, g_dock_w, g_dock_h, THEME64_R_DOCK, 0,
                  t->dock_bg, alpha, t->dock_border, THEME64_A_BORDER, t);
    gfx64_glass64(g_dock_clock_x, g_dock_y, g_dock_clock_w, g_dock_h, THEME64_R_DOCK, 0,
                  t->dock_bg, alpha, t->dock_border, THEME64_A_BORDER, t);
    // 3) 图标 + 运行中小圆点 / 最小化小横杠
    for (int i = 0; i < DOCK_ITEMS; i++) {
        const int cx = dock_item_draw_x64(i) + g_dock_icon / 2;
        const int icon_px = g_dock_icon * g_dock_scale[i] / 256;
        dock_draw_one64(i, cx, icon_px, g_dock_hover == i, t);
        if (i == 0) continue;
        int n = 0, vis = 0, mn = 0;
        dock_app_state64(kDockItems[i].app_id, &n, &vis, &mn);
        if (n > 0) {
            const int bw = g_dock_icon * THEME64_DOCK_BAR_PCT / 100;   // 小横杠宽 = 图标宽 40%
            const int by = g_dock_y + g_dock_h - 5;
            if (mn) {
                // 最小化：图标下方"小横杠"（跟随主题强调色；纯视觉、不可点击）
                gfx64_fill_round64(cx - bw / 2, by, bw, 3, 1, t->dock_bar, 255);
                if (g_dock_log_budget > 0) {
                    g_dock_log_budget--;
                    dbg64_line_begin64();
                    dbg64_str("[DOCK64] minbar idx=");
                    dbg64_dec((uint64_t)i);
                    dbg64_str(" x=");
                    dbg64_dec((uint64_t)(cx - bw / 2));
                    dbg64_str(" y=");
                    dbg64_dec((uint64_t)by);
                    dbg64_str(" w=");
                    dbg64_dec((uint64_t)bw);
                    dbg64_str(" h=3 color=#");
                    // 6 位十六进制（不要用 dbg64_hex64：它是 16 位零填充，验收正则不好写）
                    {
                        static const char* H = "0123456789ABCDEF";
                        const char hx[7] = {
                            H[(t->dock_bar >> 20) & 0xF], H[(t->dock_bar >> 16) & 0xF],
                            H[(t->dock_bar >> 12) & 0xF], H[(t->dock_bar >> 8) & 0xF],
                            H[(t->dock_bar >> 4) & 0xF], H[t->dock_bar & 0xF], 0
                        };
                        dbg64_str(hx);
                    }
                    dbg64_str(" clickable=0");
                    dbg64_nl();
                    dbg64_line_end64();
                    // ★ 确定性证据：对横杠中心做一次命中测试，必须**不命中任何 Dock 项**
                    //   （横杠只是视觉提示；点它既不激活也不恢复。）
                    const int hit = dock_hit64(cx, by + 1, nullptr);
                    dbg64_line_begin64();
                    dbg64_str("[DOCK64] minbar hit_test idx=");
                    dbg64_dec((uint64_t)i);
                    dbg64_str(" x=");
                    dbg64_dec((uint64_t)cx);
                    dbg64_str(" y=");
                    dbg64_dec((uint64_t)(by + 1));
                    dbg64_str(" -> hit=");
                    if (hit < 0) dbg64_str("none");
                    else dbg64_dec((uint64_t)hit);
                    dbg64_str(" (visual only, not clickable)");
                    dbg64_nl();
                    dbg64_line_end64();
                }
            } else {
                // 运行中：底部小圆点（主题强调色）
                gfx64_fill_round64(cx - THEME64_DOCK_DOT_W / 2, by, THEME64_DOCK_DOT_W,
                                   THEME64_DOCK_DOT_W, THEME64_DOCK_DOT_W / 2, t->dock_dot, 255);
            }
        }
    }
    // 4) 右下角时钟玻璃片（保留老的 [UI] clock text 打点与年月日语义）
    int hh = 0, mm = 0, ss = 0, yy = 0, mo = 0, dd = 0, wd = 0;
    rtc_get_time64(&hh, &mm, &ss);
    rtc_get_date64(&yy, &mo, &dd, &wd);
    char buf[40];
    int n = 0;
    buf[n++] = (char)('0' + (yy / 1000) % 10);
    buf[n++] = (char)('0' + (yy / 100) % 10);
    buf[n++] = (char)('0' + (yy / 10) % 10);
    buf[n++] = (char)('0' + yy % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + (mo / 10) % 10);
    buf[n++] = (char)('0' + mo % 10);
    buf[n++] = '-';
    buf[n++] = (char)('0' + (dd / 10) % 10);
    buf[n++] = (char)('0' + dd % 10);
    buf[n++] = ' ';
    buf[n++] = (char)('0' + (hh / 10) % 10);
    buf[n++] = (char)('0' + hh % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + (mm / 10) % 10);
    buf[n++] = (char)('0' + mm % 10);
    buf[n++] = ':';
    buf[n++] = (char)('0' + (ss / 10) % 10);
    buf[n++] = (char)('0' + ss % 10);
    buf[n] = 0;
    font_select(2);
    const int tw = font_text_width(buf);
    text_ttf(g_dock_clock_x + (g_dock_clock_w - tw) / 2,
             g_dock_y + (g_dock_h - THEME64_FS_NORMAL) / 2, buf, t->dock_txt);
    if (!str_eq64(buf, g_clock_log)) {
        str_copy64(g_clock_log, buf, (int)sizeof(g_clock_log));
        dbg64_str("[UI] clock text=");
        dbg64_str(buf);
        dbg64_nl();
    }
}

// ==================== 中间层绘制 ====================
// 桌面背景：壁纸 + 适应模式（gfx64 把 6 种模式铺好的整屏壁纸面缓存着，这里只按脏矩形拷贝；
// 绝不每帧重算缩放/模糊 —— 见 kernel/gfx64.cpp 的 wall_ensure64/wall_compose64）。
static void draw_desktop_bg(int x0, int y0, int x1, int y1) {
    gfx64_wall_draw64(x0, y0, x1 - x0, y1 - y0);
}
// 桌面图标（★ P5：项集合/持久化/圆角正方形底板 + 位图绘制都在 desktopops64.cpp，
// 这里只把"当前选中项"传下去；选中态的像素语义与旧实现一致 = Token sel_bg 不透明色块）。
static void draw_icons(void) {
    desktopops64_draw_icons64(g_icon_sel);
}
static void draw_title_buttons(Window* w) {
    // 三个按钮必须**肉眼可区分**：最小化=短横线、最大化=方框、关闭=红底 ✕；hover 高亮。
    // 几何与 handle_mouse_press 的点击判定一致（bx = 窗口右缘 - 3*BTN_W，点击 y ∈ [w->y+2, w->y+24)）。
    // 颜色本批改为从 Token 取（theme64）：浅色主题用浅灰按钮 + 白字，暗色主题用深灰按钮 + 白字。
    const Theme64Tokens* t = theme64_tokens64();
    const int bx = w->x + w->w - BORDER - BTN_W * 3;
    const int by = w->y + BORDER + 4;
    for (int b = 0; b < 3; b++) {
        const int x = bx + b * BTN_W + 3;
        const int bw = BTN_W - 6, bh = 16;
        const bool hov = (g_cur_x >= bx + b * BTN_W) && (g_cur_x < bx + (b + 1) * BTN_W) &&
                         (g_cur_y >= w->y + BORDER + 1) && (g_cur_y < w->y + TITLE_H);
        uint32_t bg;
        if (b == 2) bg = hov ? t->btn_close_hover : t->btn_close;
        else        bg = hov ? t->btn_bg_hover : t->btn_bg;
        gfx64_fill_round64(x, by, bw, bh, THEME64_R_BUTTON, bg, 255);
        const int cx = x + bw / 2;
        const int cy = by + bh / 2;
        if (b == 0) {                       // 最小化：底部短横线
            fb_fill_rect(cx - 5, cy + 2, 11, 2, t->btn_glyph);
        } else if (b == 1) {                // 最大化：空心方框
            fb_draw_rect(cx - 5, cy - 5, 11, 11, t->btn_glyph);
        } else {                            // 关闭：✕
            for (int i = -4; i <= 4; i++) {
                fb_putpixel(cx + i, cy + i, t->btn_glyph);
                fb_putpixel(cx - i, cy + i, t->btn_glyph);
            }
        }
    }
    if (!g_title_btn_logged) {
        g_title_btn_logged = true;
        dbg64_str("[UI] title btn min/max/close draw ok");
        dbg64_nl();
    }
}

// ★ P5 防回归断言（内核侧）：应用自绘**之后**，客户区不能是一片 client_bg（= 应用什么都没画）。
//   两个成因（详见交付报告）：
//     1) gui64 的 render() 提交后没清 g_dirty_any -> 每帧整屏重绘；
//     2) settings64 的 set_draw() 把"实际绘制"关在 if (cw/ch 变化) 里 -> 不带尺寸变化的重绘
//        只铺 client_bg。
//   这里只对内容型应用（设置）取 4 个角（客户区四角都恰好等于 client_bg 才算"空白"，
//   所以不会因为某个角本来就是底色而误报）；命中时打一行自动验收可以 grep 的失败行：
//     [GUI64] app blank app=5 c=15790320 client=898x594 (client_bg fill only; app painted nothing)
static void check_app_painted64(const Window* w) {
    if (!w || w->app_id != 5 /*APP_ID_SETTINGS*/) return;
    if (w->client_w < 320 || w->client_h < 200) return;
    const uint32_t bg = theme64_tokens64()->client_bg & 0xFFFFFFu;
    const int x0 = w->client_x, y0 = w->client_y;
    const int x1 = x0 + w->client_w - 8, y1 = y0 + w->client_h - 8;
    const uint32_t p0 = fb_get_pixel(x0 + 8, y0 + 8) & 0xFFFFFFu;
    const uint32_t p1 = fb_get_pixel(x0 + 8, y1) & 0xFFFFFFu;
    const uint32_t p2 = fb_get_pixel(x1, y0 + 8) & 0xFFFFFFu;
    const uint32_t p3 = fb_get_pixel(x1, y1) & 0xFFFFFFu;
    if (p0 != bg || p1 != bg || p2 != bg || p3 != bg) return;
    dbg64_line_begin64();
    dbg64_str("[GUI64] app blank app=");
    dbg64_dec((uint64_t)w->app_id);
    dbg64_str(" c=");
    dbg64_dec((uint64_t)p0);
    dbg64_str(" client=");
    dbg64_dec((uint64_t)w->client_w);
    dbg64_str("x");
    dbg64_dec((uint64_t)w->client_h);
    dbg64_str(" (client_bg fill only; app painted nothing)");
    dbg64_nl();
    dbg64_line_end64();
}
// 窗口外观（本批改版）：大圆角 14（Token）+ 双层浅阴影 + 标题栏毛玻璃(内容层 12 模糊缓存) +
// 1px 玻璃边框。**几何完全不变**：边框 1px、标题栏 24、客户区偏移与老实现逐像素一致，
// 所以所有应用布局与老验收（标题栏三按钮/客户区几何）都不受影响。
static void draw_window(Window* w) {
    const Theme64Tokens* t = theme64_tokens64();
    // 1) 双层浅阴影（预生成 alpha mask，按 w/h/r 缓存复用）
    gfx64_shadow64(w->x, w->y, w->w, w->h, THEME64_R_WINDOW, t);
    // 2) 标题栏玻璃：**只铺标题栏那条**（内容层区域模糊 r=12；整窗面积铺会把每帧开销拉到卡住
    //    看门狗的程度 —— 实测 7 个窗口时 5s 超时 PANIC）。底边/侧边的 1px 玻璃边框在第 4 步统一描。
    const uint32_t tb = w->active ? t->title_bg : t->title_bg_ina;
    gfx64_glass64(w->x, w->y, w->w, DECO_H + 2, THEME64_R_WINDOW, 1, tb, THEME64_A_TITLE,
                  0, 0, t);
    // 3) 标题文字（居中偏左）
    const int ty = w->y + BORDER + (TITLE_H - 14) / 2;
    text_ttf(w->x + 8, ty, w->title, t->title_txt);
    // 4) 三个按钮（简化绘制：三条横线/方块；关闭按钮红底）
    draw_title_buttons(w);
    // 5) 客户区底色 + 应用内容（内容卡片：不透明底，老验收依赖 240,240,240）
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, t->client_bg);
    if (w->draw) {
        fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
        const uint64_t t0 = rdtsc64();
        w->draw(w);
        check_app_painted64(w);      // ★ P5 防回归：应用画完之后不能是一片 client_bg
        w->cpu_cycles += rdtsc64() - t0;
        fb_reset_clip();
    }
    // 6) 整窗 1px 玻璃边框（1px 半透明白/黑 = 玻璃厚度与高光边缘）+ 圆角裁切
    gfx64_stroke_round64(w->x, w->y, w->w, w->h, THEME64_R_WINDOW, t->win_frame, THEME64_A_BORDER);
    // 圆角裁切：客户区是方形，把 4 个角"圆角外"的像素用模糊壁纸采样补回（= 玻璃圆角）
    gfx64_corner_cut64(w->x, w->y, w->w, w->h, THEME64_R_WINDOW, 0);
}
// 老 32px 任务栏已在批次"Windows 11 现代外观"里被 draw_dock()（Dock 栏 + 右下时钟玻璃片）取代；
// 这里保留这条注释作为考古标记：时钟 [UI] clock text= 打点与窗口按钮语义都迁到了 draw_dock/dock_press64。
// 开始菜单条目（与 32 位同名同序）
static const char* menu_zh[MENU_ITEMS] = {
    "终端", "我的电脑", "系统监视器", "计算器", "扫雷",
    "设置", "任务管理器", "关于 VimtuOS", "重启", "关机"
};
static const char* menu_en[MENU_ITEMS] = {
    "Terminal", "My Computer", "System Monitor", "Calculator", "Minesweeper",
    "Settings", "Task Manager", "About VimtuOS", "Reboot", "Shutdown"
};
static const char* menu_item(int i) { return g_lang_zh ? menu_zh[i] : menu_en[i]; }

#define MENU_W 240
#define MENU_ITEM_H 30
#define MENU_H (MENU_ITEMS * MENU_ITEM_H + 8)
static int menu_x() { return 4; }
static int menu_y() { return g_screen_h - TASKBAR_H - MENU_H - 2; }

static void draw_menu(void) {
    if (!g_menu_open) return;
    const int mx = menu_x(), my = menu_y();
    fb_fill_rect(mx, my, MENU_W, MENU_H, C_MENU_BG);
    fb_draw_rect(mx, my, MENU_W, MENU_H, rgb(140, 140, 140));
    for (int i = 0; i < MENU_ITEMS; i++) {
        const int iy = my + 4 + i * MENU_ITEM_H;
        if (i == g_menu_sel) fb_fill_rect(mx + 4, iy, MENU_W - 8, MENU_ITEM_H - 2, C_MENU_SEL);
        text_ttf(mx + 14, iy + 7, menu_item(i), C_MENU_TXT);
    }
}

// 光标：按 input.cpp 里的形状状态画不同指针（箭头 / 文本 I 形 / 转圈 / 四向 + 对角缩放）
static void cur_px64(int x, int y, uint32_t c) { fb_putpixel(x, y, c); }
static void cur_line64(int x0, int y0, int x1, int y1, uint32_t c) {
    int dx = x1 - x0, dy = y1 - y0;
    const int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    const int n = (ax > ay ? ax : ay) + 1;
    for (int i = 0; i < n; i++) {
        const int x = n > 1 ? x0 + dx * i / (n - 1) : x0;
        const int y = n > 1 ? y0 + dy * i / (n - 1) : y0;
        cur_px64(x, y, c);
    }
}
static void draw_cursor(void) {
    static const char* cur[] = {
        "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
        "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
        "X.....XXXXX", "X..X..X    ", "X.X.X..X   ", "XX..X..X   ", "X....X..X  ",
        "X.....X..X ", "X......X   ", "X.......X  ", "X........X "
    };
    const uint32_t black = rgb(0, 0, 0), white = rgb(255, 255, 255);
    const int shape = mouse_get_cursor64();
    if (shape == MOUSE_CUR_ARROW || shape == MOUSE_CUR_MOVE) {
        const int rows = 19;
        for (int j = 0; j < rows; j++) {
            const char* r = cur[j];
            for (int i = 0; r[i]; i++) {
                if (r[i] == ' ') break;
                cur_px64(g_cur_x + i, g_cur_y + j, (r[i] == 'X') ? black : white);
            }
        }
        if (shape == MOUSE_CUR_MOVE) {          // 四向移动：箭头 + 十字
            cur_line64(g_cur_x + 16, g_cur_y + 2, g_cur_x + 16, g_cur_y + 14, black);
            cur_line64(g_cur_x + 12, g_cur_y + 8, g_cur_x + 20, g_cur_y + 8, black);
        }
        return;
    }
    if (shape == MOUSE_CUR_TEXT) {              // 文本 I 形
        const int x = g_cur_x + 6, y = g_cur_y + 2, h = 16;
        cur_line64(x, y, x, y + h, black);
        cur_line64(x - 3, y, x + 3, y, black);
        cur_line64(x - 3, y + h, x + 3, y + h, black);
        cur_line64(x + 1, y + 1, x + 1, y + h - 1, white);
        return;
    }
    if (shape == MOUSE_CUR_WAIT) {              // 转圈（缺口随帧转动）
        const int cx = g_cur_x + 8, cy = g_cur_y + 8;
        const int rot = (int)((ticks64() / 3) % 8);
        for (int a = 0; a < 8; a++) {
            if (a == rot) continue;
            const int ang = a * 45;
            const int dx = (ang == 0) ? 6 : (ang == 45 ? 4 : (ang == 90 ? 0 : (ang == 135 ? -4 : (ang == 180 ? -6 : (ang == 225 ? -4 : (ang == 270 ? 0 : 4))))));
            const int dy = (ang == 0) ? 0 : (ang == 45 ? 4 : (ang == 90 ? 6 : (ang == 135 ? 4 : (ang == 180 ? 0 : (ang == 225 ? -4 : (ang == 270 ? -6 : -4))))));
            cur_px64(cx + dx, cy + dy, black);
            if ((dx + dy) % 2 == 0) cur_px64(cx + dx, cy + dy - 1, white);
        }
        return;
    }
    // 缩放箭头：左右 / 上下 / 两条对角线
    const bool hz = (shape == MOUSE_CUR_H);
    const bool vt = (shape == MOUSE_CUR_V);
    const bool d1 = (shape == MOUSE_CUR_D1);
    const int cx = g_cur_x + 8, cy = g_cur_y + 8;
    for (int i = -7; i <= 7; i++) {
        if (hz || d1) cur_px64(cx + i, cy + i, black);
        if (hz) cur_px64(cx + i, cy, black);
        if (vt) cur_px64(cx, cy + i, black);
        if (!hz && !vt && !d1) cur_px64(cx + i, cy - i, black);
    }
    for (int i = 0; i < 4; i++) {
        if (hz) { cur_px64(cx - 7 + i, cy - i, black); cur_px64(cx - 7 + i, cy + i, black);
                  cur_px64(cx + 7 - i, cy - i, black); cur_px64(cx + 7 - i, cy + i, black); }
        if (vt) { cur_px64(cx - i, cy - 7 + i, black); cur_px64(cx + i, cy - 7 + i, black);
                  cur_px64(cx - i, cy + 7 - i, black); cur_px64(cx + i, cy + 7 - i, black); }
        if (d1) { cur_px64(cx - 7 + i, cy - 7 + i, black); cur_px64(cx + 7 - i, cy + 7 - i, black); }
        if (!hz && !vt && !d1) { cur_px64(cx + 7 - i, cy - 7 + i, black); cur_px64(cx - 7 + i, cy + 7 - i, black); }
    }
    if (hz)  cur_line64(cx - 5, cy, cx + 5, cy, white);
    if (vt)  cur_line64(cx, cy - 5, cx, cy + 5, white);
    if (d1)  cur_line64(cx - 5, cy - 5, cx + 5, cy + 5, white);
    if (!hz && !vt && !d1) cur_line64(cx - 5, cy + 5, cx + 5, cy - 5, white);
}

// 全部图层按脏矩形裁剪重绘，然后只提交脏矩形
static void render(void) {
    const uint64_t t0 = rdtsc64();
    const int x0 = g_dirty_x0, y0 = g_dirty_y0, x1 = g_dirty_x1, y1 = g_dirty_y1;
    const int dw = x1 - x0, dh = y1 - y0;
    if (dw <= 0 || dh <= 0) { g_dirty_any = false; return; }

    fb_set_clip(x0, y0, dw, dh);
    draw_desktop_bg(x0, y0, x1, y1);
    draw_icons();
    // ★ P5：鼠标左键拖出的**玻璃选择框**（框内全透明、只有突出边缘；桌面层、窗口之下）
    desktopops64_selbox_draw64();
    // Dock 双层阴影：脏区压到它的包围盒就补画（壁纸/图标已画完，窗口还没画 → 层次正确）
    {
        int bx, by, bw, bh;
        dock_shadow_box64(&bx, &by, &bw, &bh);
        if (!(bx >= x1 || by >= y1 || bx + bw <= x0 || by + bh <= y0)) draw_dock_shadow();
    }
    // 窗口从底到顶画：先把 z 序反转
    Window* stack[MAX_WINS];
    int n = 0;
    for (Window* w = g_z; w && n < MAX_WINS; w = w->next) stack[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        Window* w = stack[i];
        if (!w->visible || w->minimized) continue;
        // ★ 参与判定扩到"窗口 + 阴影包围盒"：脏区落在窗口外的阴影里也要重画阴影（否则会被壁纸重绘抹掉）
        const int sp = THEME64_SH_F_BLUR * 2 + 4;
        if (w->x - sp >= x1 || w->y - sp >= y1 || w->x + w->w + sp <= x0 || w->y + w->h + sp + THEME64_SH_F_DY <= y0)
            continue;
        if (pop_active64() && g_pop_w == w) {
            int px = 0, py = 0, pw = 0, ph = 0;
            pop_geom64(w, &px, &py, &pw, &ph);
            const int sx = w->x, sy = w->y, sw = w->w, sh = w->h;
            w->x = px; w->y = py; w->w = pw; w->h = ph;
            recompute_client(w);
            draw_window(w);
            w->x = sx; w->y = sy; w->w = sw; w->h = sh;
            recompute_client(w);
        } else {
            draw_window(w);
        }
    }
    fly_draw64();                // ★ P5：最小化飞向 Dock 的缩略图（窗口之上、Dock 之下）
    draw_dock();                 // ★ 本批：Windows 11 风格 Dock（替换老 32px 任务栏）
    winlist_draw64();            // ★ P5：Dock 悬停窗口列表
    draw_menu();                 // 老菜单（Win 键）
    desktopops64_menu_draw64();  // ★ P5：桌面右键菜单
    // ★ P2：开始菜单（窗口之上）→ 四个二级弹窗/设备 toast（更靠前）
    startmenu64_draw64();
    draw_cursor();               // ★ P5：按形状画指针（箭头/文本/转圈/缩放）
    panels64_draw64();
    fb_reset_clip();
    fb_flip_region(x0, y0, dw, dh);

    const uint64_t t1 = rdtsc64();
    g_busy_cycles += t1 - t0;
    g_total_cycles += t1 - t0 + 1;
    // ★ P5 回归修复：脏矩形**必须在提交后清掉**（上一轮接线版把这两行连同 g_frames++ 一起
    //   替换成了下面的 g_load_done 块 → g_dirty_any 永远为真 → 每帧都整屏重绘：
    //   壁纸重铺 + 每个窗口重铺 client_bg，而应用自己的重绘是有条件的（settings64 只在
    //   尺寸变化时画内容）→ 设置窗口内容每帧被抹成纯 240,240,240。见报告里的像素证据。）
    g_frames++;
    g_dirty_any = false;
    if (!g_load_done) {
        // ★ P5：窗口画完**两帧**才算"加载完"（保证至少一次光标判定能看到"加载中"，
        //   也保证转圈在慢机型上真的能被用户看到）
        g_load_paints++;
        if (g_load_paints >= 2) g_load_done = true;
    }
}

// ==================== 内置轻量窗口 ====================
static Window* g_mon_win = nullptr;
static void mon_draw(Window* w) {
    const int x = w->client_x + 12;
    int y = w->client_y + 10;
    const int lh = 20;
    char buf[80];
    // 小工具：把十进制写进 buf
    struct L {
        static int u(char* b, uint64_t v) {
            char t[24]; int n = 0;
            if (v == 0) t[n++] = '0';
            while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
            int k = 0;
            while (n) b[k++] = t[--n];
            b[k] = 0;
            return k;
        }
        static int s(char* b, const char* s) { int k = 0; while (s[k]) { b[k] = s[k]; k++; } b[k] = 0; return k; }
        static int cat(char* b, const char* s) { int k = 0; while (b[k]) k++; while (*s) b[k++] = *s++; b[k] = 0; return k; }
    };
    text_ttf(x, y, gui64_tr("System Monitor (live)", "系统监视器（实时）"), rgb(0, 60, 120));
    y += lh + 6;

    // CPU 行
    L::s(buf, gui64_tr("Shell busy : ", "外壳忙占比 : "));
    L::u(buf + 14, g_busy_pct);
    L::cat(buf, "%    ");
    L::cat(buf, gui64_tr("FPS ", "帧率 "));
    { char t[8]; L::u(t, g_fps); L::cat(buf, t); }
    text_ttf(x, y, buf, rgb(40, 40, 40));
    y += lh;

    // 中断 / tick
    L::s(buf, gui64_tr("IRQs ", "中断总数 "));
    { char t[24]; L::u(t, g_irq_total64); L::cat(buf, t); L::cat(buf, "   ticks "); L::u(t, (uint64_t)ticks64()); L::cat(buf, t); }
    text_ttf(x, y, buf, rgb(40, 40, 40));   // ★ P5 回归修复：接线版漏掉了这一行 + y += lh（内容少画一行）
    y += lh;

    // 内存
    uint64_t total_kb = 0, free_kb = 0, heap_kb = 0;
    mem_info_64(&total_kb, &free_kb, &heap_kb);
    L::s(buf, gui64_tr("RAM total ", "物理内存 "));
    { char t[24]; L::u(t, mem_total_ram_64() / (1024 * 1024)); L::cat(buf, t); L::cat(buf, " MB"); }
    text_ttf(x, y, buf, rgb(40, 40, 40));
    y += lh;
    L::s(buf, gui64_tr("Page pool ", "页池 "));
    { char t[24];
      L::u(t, free_kb / 1024); L::cat(buf, t); L::cat(buf, " / ");
      L::u(t, total_kb / 1024); L::cat(buf, t); L::cat(buf, " MB free"); }
    text_ttf(x, y, buf, rgb(40, 40, 40));
    y += lh;
    L::s(buf, gui64_tr("Kernel heap ", "内核堆 "));
    { char t[24]; L::u(t, heap_used_64() / 1024); L::cat(buf, t); L::cat(buf, " / ");
      L::u(t, heap_total_64() / 1024); L::cat(buf, t); L::cat(buf, " KB"); }
    text_ttf(x, y, buf, rgb(40, 40, 40));
    y += lh + 4;

    // 按 owner 的内存占用（条状）
    text_ttf(x, y, gui64_tr("Heap by owner:", "堆归属明细："), rgb(0, 60, 120));
    y += lh;
    for (int i = 0; i < MEM_OWNER_COUNT_64; i++) {
        const uint64_t b = mem_owner_bytes_64(i);
        if (b == 0) continue;
        L::s(buf, mem_owner_name_64(i));
        L::cat(buf, " ");
        { char t[24]; L::u(t, b / 1024); L::cat(buf, t); L::cat(buf, " KB"); }
        text_ttf(x + 8, y, buf, rgb(60, 60, 60));
        y += lh - 4;
    }
    // 窗口数
    L::s(buf, gui64_tr("Windows: ", "窗口数："));
    { char t[8]; L::u(t, (uint64_t)gui64_window_count()); L::cat(buf, t); }
    text_ttf(x, y + 4, buf, rgb(40, 40, 40));
}
void app_monitor_open64() {
    if (gui64_window_alive(g_mon_win)) { gui64_set_active(g_mon_win); return; }
    g_mon_win = gui64_create_window(gui64_tr("System Monitor", "系统监视器"),
                                    150, 90, 420, 340, mon_draw, nullptr, nullptr, APP_ID_MONITOR);
    if (!g_mon_win) return;
    gui64_set_min_size(g_mon_win, 340, 260);
    dbg64_str("[APP] monitor opened");
    dbg64_nl();
}
void app_monitor_reset64() {
    if (gui64_window_alive(g_mon_win)) gui64_destroy_window(g_mon_win);
    g_mon_win = nullptr;
    dbg64_str("[APP] monitor reset");
    dbg64_nl();
}

// ==================== 我的电脑 / 文件资源管理器（转调 kernel/explorer64.cpp）====================
// 本轮把 32 位风格的"静态统计页"换成了真正的 Win10 风格文件管理器（此电脑 + 盘内导航）。
// 外壳这里只保留**钩子**：窗口本身、导航状态机、绘制与交互都在 kernel/explorer64.cpp，
// 理由见 kernel/explorer64.h 顶部（避免把 gui64.cpp 撑成第二个 32 位 gui.cpp）。
// 注意："[APP] mypc opened" / "[APP] mypc closed" 这两行由 explorer64 打印（历史断言依赖）。
void app_mypc_open64() { explorer64_open64(); }

static Window* g_about_win = nullptr;
static void about_draw(Window* w) {
    const int x = w->client_x + 14;
    int y = w->client_y + 12;
    const int lh = 22;
    text_ttf(x, y, "VimtuOS 64-bit", rgb(0, 60, 120)); y += lh + 4;
    text_ttf(x, y, gui64_tr("Pure x86_64 long mode kernel, hand-written.",
                            "纯 x86_64 长模式内核，全手写。"), rgb(40, 40, 40)); y += lh;
    {
        char buf[64]; int n = 0;
        const char* s = "build: clang++ -target x86_64-elf";
        while (s[n]) { buf[n] = s[n]; n++; }
        buf[n] = 0;
        text_ttf(x, y, buf, rgb(90, 90, 90)); y += lh - 4;
    }
    text_ttf(x, y, gui64_tr("Boot: BIOS El Torito / hybrid USB / UEFI",
                            "引导：BIOS 光盘 / U 盘 hybrid / UEFI"), rgb(90, 90, 90)); y += lh;
    text_ttf(x, y, gui64_tr("Desktop: ported apps (mines/calc/term/settings/tmgr)",
                            "桌面：已移植应用（扫雷/计算器/终端/设置/任务管理器）"), rgb(90, 90, 90)); y += lh + 6;
    {
        char buf[64]; int n = 0;
        const char* s = "RAM ";
        while (s[n]) { buf[n] = s[n]; n++; }
        uint64_t mb = mem_total_ram_64() / (1024 * 1024);
        char t[24]; int k = 0;
        if (mb == 0) t[k++] = '0';
        while (mb) { t[k++] = (char)('0' + (mb % 10)); mb /= 10; }
        while (k) buf[n++] = t[--k];
        buf[n++] = ' '; buf[n++] = 'M'; buf[n++] = 'B'; buf[n] = 0;
        text_ttf(x, y, buf, rgb(40, 40, 40));
    }
}
void app_about_open64() {
    if (gui64_window_alive(g_about_win)) { gui64_set_active(g_about_win); return; }
    g_about_win = gui64_create_window(gui64_tr("About VimtuOS", "关于 VimtuOS"),
                                      200, 100, 400, 260, about_draw, nullptr, nullptr, APP_ID_ABOUT);
    if (g_about_win) gui64_set_min_size(g_about_win, 320, 220);
    dbg64_str("[APP] about opened");
    dbg64_nl();
}

// ★ P5：回收站窗口整个搬到 desktopops64.cpp（内容/恢复/永久删除+二次确认/空状态都在那里）；
// 这里保留 app_recycle_open64 这个应用契约入口（[APP] recycle opened 打点也在那边打）。
void app_recycle_open64() { desktopops64_recycle_open64(); }

// ==================== 开始菜单动作（与 32 位 menu_activate 一一对应）====================
static void menu_activate(int idx) {
    dbg64_str("[UI] menu activate idx=");
    dbg64_dec((uint64_t)idx);
    dbg64_nl();
    switch (idx) {
        case 0: app_term_open64();     break;
        case 1: app_mypc_open64();     break;
        case 2: app_monitor_open64();  break;
        case 3: app_calc_open64();     break;
        case 4: app_mines_open64();    break;
        case 5: app_settings_open64(); break;
        case 6: app_tmgr_open64();     break;
        case 7: app_about_open64();    break;
        case 8: sys_reboot64();        break;
        case 9: sys_shutdown64();      break;
        default: break;
    }
}
static void menu_toggle() {
    g_menu_open = !g_menu_open;
    g_menu_sel = 0;
    dirty_add(0, menu_y(), MENU_W + 8, MENU_H + 4);
    dbg64_str(g_menu_open ? "[UI] menu open" : "[UI] menu close");
    dbg64_nl();
}

// ==================== 输入处理 ====================
static int g_prev_btn = 0;      // 上一帧的鼠标按键位

static void press_in_client(Window* w, int mx, int my, int button) {
    const int cx = mx - w->client_x;
    const int cy = my - w->client_y;
    g_click_right = (button == 1);
    if (w->on_click && button == 0) w->on_click(w, cx, cy);
    if (w->on_click2) w->on_click2(w, cx, cy, button);
}

static void handle_mouse_press(int mx, int my, int button) {
    // ★ P2（最先）：设备 toast 关闭按钮 → 二级弹窗 → 开始菜单（层级栈：弹窗 > 菜单 > Dock/窗口/桌面）
    if (panels64_handle_mouse_press64(mx, my, button)) return;
    if (startmenu64_is_open64()) {
        int sx = 0, sy = 0, sw = 0, sh = 0;
        startmenu64_geom64(&sx, &sy, &sw, &sh);
        const bool in_menu = (mx >= sx && mx < sx + sw && my >= sy && my < sy + sh);
        if (in_menu) {
            (void)startmenu64_handle_mouse_press64(mx, my, button);
            dirty_add(sx - 8, sy - 8, sw + 16, sh + 16);
            return;
        }
    }
    // ★ P5：桌面右键菜单 / Dock 悬停窗口列表（层级：弹窗 > 开始菜单 > 右键菜单/窗口列表 > Dock）
    if (desktopops64_menu_press64(mx, my, button)) return;
    if (winlist_press64(mx, my)) return;
    {
        int on_clock = 0;
        const int idx = dock_hit64(mx, my, &on_clock);
        if (idx >= 0) {
            g_menu_open = false;
            if (idx != 0 && startmenu64_is_open64()) startmenu64_close64("dock-item");
            g_dock_pressed = idx;
            dock_press64(idx);
            dirty_add(g_dock_x, g_dock_y, g_dock_w, g_dock_h);
            return;
        }
        if (on_clock) {
            // 时钟玻璃片：纯显示，不响应点击（只是把事件吃掉，避免穿透到桌面拉选择框）
            g_menu_open = false;
            return;
        }
        if (my >= g_dock_y) {
            // Dock 行内的空白（面板两端留白/图标之间）：也吞掉，不穿透桌面
            g_menu_open = false;
            return;
        }
    }
    // 2) 开始菜单（P2 新菜单：点菜单外部关闭；不消费点击，让点击继续落到桌面 —— 与老菜单同语义）
    if (startmenu64_is_open64()) startmenu64_handle_mouse_press64(mx, my, button);
    // 2) 开始菜单
    if (g_menu_open) {
        const int mx0 = menu_x(), my0 = menu_y();
        if (mx >= mx0 && mx < mx0 + MENU_W && my >= my0 && my < my0 + MENU_H) {
            const int idx = (my - my0 - 4) / MENU_ITEM_H;
            if (idx >= 0 && idx < MENU_ITEMS) {
                g_menu_open = false;
                dirty_add(mx0, my0, MENU_W, MENU_H);
                menu_activate(idx);
                return;
            }
        }
        g_menu_open = false;
        dirty_add(mx0, my0, MENU_W, MENU_H);
    }
    // 3) 窗口
    Window* w = win_at(mx, my);
    if (w) {
        gui64_set_active(w);
        // 标题栏按钮
        const int by = w->y + BORDER + 7;
        const int bx = w->x + w->w - BORDER - BTN_W * 3;
        if (my >= by - 6 && my < by + 16 && mx >= bx) {
            const int b = (mx - bx) / BTN_W;
            if (b == 2) {                       // 关闭
                gui64_destroy_window(w);
            } else if (b == 1) {                // 最大化 / 还原
                if (!w->maximized) {
                    w->norm_x = w->x; w->norm_y = w->y; w->norm_w = w->w; w->norm_h = w->h;
                    w->x = 0; w->y = 0; w->w = g_screen_w; w->h = g_screen_h - TASKBAR_H;
                    w->maximized = true;
                } else {
                    w->x = w->norm_x; w->y = w->norm_y; w->w = w->norm_w; w->h = w->norm_h;
                    w->maximized = false;
                }
                recompute_client(w);
                gui64_invalidate();
            } else {                            // 最小化（★ P5：缩小 + 渐隐飞向 Dock 图标）
                w->minimized = true;
                fly_begin64(w);
                gui64_invalidate();
            }
            return;
        }
        // 标题栏 -> 拖动
        if (my < w->y + DECO_H) {
            g_drag = DRAG_MOVE;
            g_drag_w = w;
            g_drag_dx = mx - w->x;
            g_drag_dy = my - w->y;
            return;
        }
        // ★ P5：边缘/四角 -> 八向缩放（左右 / 上下 / 四个角各一种，和 Windows 11 一样）
        {
            const int dir = resize_dir64(w, mx, my);
            if (dir) {
                g_drag = DRAG_RESIZE;
                g_resize_dir = dir;
                g_rz_log_frames = 0;
                g_drag_w = w;
                g_drag_dx = w->w - (mx - w->x);          // 右/下边缘用
                g_drag_dy = w->h - (my - w->y);
                dbg64_line_begin64();
                dbg64_str("[UI] win resize begin dir=");
                dbg64_str(resize_dir_name64(dir));
                dbg64_str(" x=");
                dbg64_dec((uint64_t)w->x);
                dbg64_str(" y=");
                dbg64_dec((uint64_t)w->y);
                dbg64_str(" w=");
                dbg64_dec((uint64_t)w->w);
                dbg64_str(" h=");
                dbg64_dec((uint64_t)w->h);
                dbg64_str(" client=");
                dbg64_dec((uint64_t)w->client_w);
                dbg64_str("x");
                dbg64_dec((uint64_t)w->client_h);
                dbg64_nl();
                dbg64_line_end64();
                return;
            }
        }
        // 客户区 -> 交给应用
        press_in_client(w, mx, my, button);
        return;
    }
    // ★ P5：桌面图标 —— 命中项（集合/持久化在 desktopops64）按下 = 选中 + 记录拖动基准
    {
        const int hit = desktopops64_hit64(mx, my);
        if (hit >= 0) {
            int icx = 0, icy = 0;
            desktopops64_pos64(hit, &icx, &icy);
            g_icon_sel = hit;
            g_icon_drag_idx = hit;
            g_icon_drag_moved = false;
            g_icon_press_x = mx;
            g_icon_press_y = my;
            g_icon_press_icon_x = icx;
            g_icon_press_icon_y = icy;
            dbg64_str("[UI] desktop icon select kind=");
            dbg64_dec((uint64_t)desktopops64_kind64(hit));
            dbg64_nl();
            dirty_add(icx - 6, icy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            return;
        }
    }
    // ★ P5：桌面空白 —— 右键打开桌面右键菜单；左键取消选择并开始拉**玻璃选择框**
    if (g_icon_sel >= 0) {
        int ox = 0, oy = 0;
        desktopops64_pos64(g_icon_sel, &ox, &oy);
        dirty_add(ox - 6, oy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
        g_icon_sel = -1;
    }
    if (button == 1) {                       // 右键（需求：桌面空白处右键 -> 现代风格菜单）
        desktopops64_menu_open64(mx, my);
        return;
    }
    if (button == 0) desktopops64_selbox_begin64(mx, my);
    dirty_add(0, 0, g_screen_w, g_screen_h);
}

static void handle_mouse(void) {
    mouse_apply_sensitivity();               // mouse.sens != 1700 时才动（默认路径零变化）
    const int mx = mouse_get_x(), my = mouse_get_y();
    const int btn = (int)mouse_get_buttons();
    // ★ P5：释放用**事件计数**（帧采样漏边沿 -> 双击/拖拽结束会失效）；按钮位比对只作兜底
    const bool rel_left_evt = mouse_button_released64(0);
    if (rel_left_evt) mouse_consume_released64(0);
    const bool rel_left = rel_left_evt || ((g_prev_btn & 1) && !(btn & 1));
    // 光标移动 -> 新旧两块脏区（标题栏三按钮要跟着重画：hover 高亮）
    if (mx != g_cur_x || my != g_cur_y) {
        dirty_add(g_prev_cur_x - 6, g_prev_cur_y - 4, 24, 24);   // ★ P5：光标图案变大（转圈/对角箭头）
        dirty_add(g_cur_x - 6, g_cur_y - 4, 24, 24);
        dirty_title_buttons_at(g_cur_x, g_cur_y);
        dirty_title_buttons_at(mx, my);
        g_prev_cur_x = g_cur_x; g_prev_cur_y = g_cur_y;
        g_cur_x = mx; g_cur_y = my;
        dirty_add(mx - 6, my - 4, 24, 24);
    }
    // 按下（边沿）
    if (mouse_button_pressed(0)) { mouse_consume_pressed(0); handle_mouse_press(mx, my, 0); }
    if (mouse_button_pressed(1)) { mouse_consume_pressed(1); handle_mouse_press(mx, my, 1); }
    if (mouse_button_pressed(2)) { mouse_consume_pressed(2); handle_mouse_press(mx, my, 2); }
    // ★ P2：二级弹窗/开始菜单的悬停 + 拖动（音量滑块 / 日历左右拖切月 / WiFi 滚动条）
    panels64_handle_mouse_move64(mx, my, btn);
    startmenu64_handle_mouse_move64(mx, my, btn);
    // ★ P5：桌面右键菜单的悬停高亮（菜单开着时鼠标移动换行；模块内部去重 + 只脏菜单矩形）
    if (desktopops64_menu_is_open64()) desktopops64_menu_move64(mx, my);
    // ★ P2：滚轮（PS/2 4 字节包的 Z；弹窗优先，其次开始菜单搜索列表）
    {
        const int dz = mouse_pop_wheel64();
        if (dz != 0) {
            if (!panels64_wheel64(dz)) (void)startmenu64_wheel64(dz);
        }
    }
    // ---- Dock：悬停命中 + 动效推进（悬停放大/邻位让位/点击回弹）----
    {
        int on_clock = 0;
        const int hov = dock_hit64(mx, my, &on_clock);
        if (hov != g_dock_hover) {
            g_dock_hover = hov;
            if (g_dock_log_budget > 0) {
                g_dock_log_budget--;
                dbg64_line_begin64();
                dbg64_str("[DOCK64] hover idx=");
                dbg64_dec((uint64_t)(hov < 0 ? 0 : hov));
                dbg64_str(hov < 0 ? " none" : " scale=");
                if (hov >= 0) dbg64_dec((uint64_t)THEME64_DOCK_HOVER_PCT);
                dbg64_str(" neighbor_shift=");
                dbg64_dec((uint64_t)THEME64_DOCK_NEIGH_PCT);
                dbg64_str(" motion=150ms");
                dbg64_nl();
                dbg64_line_end64();
            }
            dirty_add(g_dock_x, g_dock_y, g_dock_w, g_dock_h);
        }
        if (dock_anim_tick64()) {
            // 动效每帧都在变 → 只重画 Dock 那一条（脏矩形小，不整屏）
            dirty_add(g_dock_x, g_dock_y, g_dock_w, g_dock_h);
        }
    }

    // 桌面图标拖动：位移 >5px（dx²+dy²>25）才算拖动，从而区分单击/双击/拖动（移植自 32 位）
    if ((btn & 1) && g_icon_drag_idx >= 0) {
        const int dx = mx - g_icon_press_x, dy = my - g_icon_press_y;
        int icx = 0, icy = 0;
        desktopops64_pos64(g_icon_drag_idx, &icx, &icy);
        if (!g_icon_drag_moved && dx * dx + dy * dy > 25) {
            g_icon_drag_moved = true;
            dbg64_str("[UI] icon drag idx=");
            dbg64_dec((uint64_t)g_icon_drag_idx);
            dbg64_str(" x=");
            dbg64_dec((uint64_t)icx);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)icy);
            dbg64_nl();
        }
        if (g_icon_drag_moved) {
            int nx = g_icon_press_icon_x + dx;
            int ny = g_icon_press_icon_y + dy;
            if (nx < 2) nx = 2;
            if (nx > g_screen_w - ICON_CELL_W - 2) nx = g_screen_w - ICON_CELL_W - 2;
            if (ny < 2) ny = 2;
            if (ny > g_screen_h - TASKBAR_H - ICON_CELL_H) ny = g_screen_h - TASKBAR_H - ICON_CELL_H;
            if (nx != icx || ny != icy) {
                dirty_add(icx - 6, icy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
                desktopops64_set_pos64(g_icon_drag_idx, nx, ny);
                dirty_add(nx - 6, ny - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            }
        }
    }
    // 玻璃选择框：拖动更新矩形 + 实时选中框内图标（选中底色仍是 Token sel_bg）
    if ((btn & 1) && desktopops64_selbox_active64()) {
        if (desktopops64_selbox_update64(mx, my)) {
            const int sel = desktopops64_selbox_count64();
            if (sel != g_icon_sel) {
                if (g_icon_sel >= 0) {
                    int ox = 0, oy = 0;
                    desktopops64_pos64(g_icon_sel, &ox, &oy);
                    dirty_add(ox - 6, oy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
                }
                g_icon_sel = sel;
                if (g_icon_sel >= 0) {
                    int ox = 0, oy = 0;
                    desktopops64_pos64(g_icon_sel, &ox, &oy);
                    dirty_add(ox - 6, oy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
                }
            }
        }
    }
    // 左键释放：结束图标拖动/选择框；位移未超阈值 = 单击（累计双击打开应用，不变）
    // 左键释放：结束图标拖动/选择框；位移未超阈值 = 单击（累计双击打开应用）
    if (rel_left) {
        if (g_icon_drag_idx >= 0) {
            const int i = g_icon_drag_idx;
            const int kind = desktopops64_kind64(i);
            int icx = 0, icy = 0;
            desktopops64_pos64(i, &icx, &icy);
            // ★ P5：拖到回收站图标上 -> 进回收站（需求原文：可以把快捷方式或文件移入回收站）
            int recv = -1;
            if (g_icon_drag_moved) recv = desktopops64_is_recycle_hit64(mx, my);
            if (g_icon_drag_moved && recv >= 0 && recv != i) {
                dbg64_line_begin64();
                dbg64_str("[DESK64] drag to recycle idx=");
                dbg64_dec((uint64_t)i);
                dbg64_str(" kind=");
                dbg64_dec((uint64_t)kind);
                dbg64_str(" target=");
                dbg64_dec((uint64_t)recv);
                dbg64_nl();
                dbg64_line_end64();
                (void)desktopops64_recycle_add64(i);
                if (desktopops64_menu_is_open64()) desktopops64_menu_close64("drag");
            } else if (!g_icon_drag_moved) {
                const uint32_t now = ticks64();
                const bool dbl = (g_icon_sel == i) && (now - g_icon_last_tick <= ms_to_ticks64(500));
                if (dbl) {
                    dbg64_str("[UI] desktop icon open kind=");
                    dbg64_dec((uint64_t)kind);
                    dbg64_nl();
                    if (kind == DESKOPS_KIND_MYPC) app_mypc_open64();
                    else if (kind == DESKOPS_KIND_RECYCLE) app_recycle_open64();
                    else app_term_open64();
                    g_icon_sel = -1;
                    g_icon_last_tick = 0;
                } else {
                    g_icon_last_tick = now;
                }
            } else {
                dbg64_str("[UI] icon drag idx=");
                dbg64_dec((uint64_t)i);
                dbg64_str(" x=");
                dbg64_dec((uint64_t)icx);
                dbg64_str(" y=");
                dbg64_dec((uint64_t)icy);
                dbg64_nl();
                // 拖动结束：位置已由 desktopops64_set_pos64 写进 config64（图标 0..2 用既有键）
                dbg64_line_begin64();
                dbg64_str("[CONF64] icon ");
                dbg64_dec((uint64_t)kind);
                dbg64_str(" moved to ");
                dbg64_dec((uint64_t)icx);
                dbg64_str(",");
                dbg64_dec((uint64_t)icy);
                dbg64_str(" (persisted via config64/store64)");
                dbg64_nl();
                dbg64_line_end64();
            }
            dirty_add(icx - 6, icy - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            g_icon_drag_idx = -1;
            g_icon_drag_moved = false;
        }
        if (desktopops64_selbox_active64()) desktopops64_selbox_end64();
    }
    // 拖拽窗口（移动 / ★ P5 八向缩放）
    if (g_drag != DRAG_NONE && g_drag_w && gui64_window_alive(g_drag_w)) {
        if (btn & 1) {
            Window* w = g_drag_w;
            dirty_add(w->x, w->y, w->w, w->h);
            if (g_drag == DRAG_MOVE) {
                int nx = mx - g_drag_dx, ny = my - g_drag_dy;
                if (nx < -w->w + 40) nx = -w->w + 40;
                if (nx > g_screen_w - 40) nx = g_screen_w - 40;
                if (ny < 0) ny = 0;
                if (ny > g_screen_h - TASKBAR_H - TITLE_H) ny = g_screen_h - TASKBAR_H - TITLE_H;
                w->x = nx; w->y = ny;
                recompute_client(w);
            } else {
                // ★ P5：八向缩放 —— 左/上边缘同时改 x/y（窗口"朝外"长），右下用 g_drag_dx/dy 保持抓取点
                const int dir = g_resize_dir;
                const int ox = w->x, oy = w->y, ow = w->w, oh = w->h;
                int nw = ow, nh = oh;
                if (dir & RZ_R) nw = mx - w->x + g_drag_dx;
                if (dir & RZ_B) nh = my - w->y + g_drag_dy;
                if (dir & RZ_L) nw = ow + (ox - mx);
                if (dir & RZ_T) nh = oh + (oy - my);
                if (nw < w->min_w) nw = w->min_w;
                if (nh < w->min_h) nh = w->min_h;
                int nx = w->x, ny = w->y;
                if (dir & RZ_L) { nx = ox + ow - nw; if (nx < 0) { nw += nx; nx = 0; } }
                if (dir & RZ_T) { ny = oy + oh - nh; if (ny < 0) { nh += ny; ny = 0; } }
                if (nx + nw > g_screen_w) nw = g_screen_w - nx;
                if (ny + nh > g_screen_h - TASKBAR_H) nh = g_screen_h - TASKBAR_H - ny;
                if (nw >= w->min_w && nh >= w->min_h) {
                    w->x = nx; w->y = ny;
                    w->w = nw; w->h = nh;
                    recompute_client(w);
                    // 窗口内容随尺寸变化（应用每帧按 client_w/h 重排）—— 打点让验收能看到几何 + 客户区一起变
                    // 每帧都打一行（一次拖动最多 24 行；验收要看到"帧"在动）—— 计数在 resize begin 处清零
                    if (dir != 0 && g_rz_log_frames < 24) {
                        g_rz_log_frames++;
                        dbg64_line_begin64();
                        dbg64_str("[UI] win resize dir=");
                        dbg64_str(resize_dir_name64(dir));
                        dbg64_str(" x=");
                        dbg64_dec((uint64_t)w->x);
                        dbg64_str(" y=");
                        dbg64_dec((uint64_t)w->y);
                        dbg64_str(" w=");
                        dbg64_dec((uint64_t)w->w);
                        dbg64_str(" h=");
                        dbg64_dec((uint64_t)w->h);
                        dbg64_str(" client=");
                        dbg64_dec((uint64_t)w->client_w);
                        dbg64_str("x");
                        dbg64_dec((uint64_t)w->client_h);
                        dbg64_nl();
                        dbg64_line_end64();
                        // 会话策略：把新尺寸记进 config64（按应用名，跨重启）
                        if ((int)w->app_id == APP_ID_TERM)     cfg64_set_win64("term", w->w, w->h);
                        else if ((int)w->app_id == APP_ID_MYPC) cfg64_set_win64("mypc", w->w, w->h);
                    }
                }
            }
            dirty_add(w->x - 8, w->y - 8, w->w + 16, w->h + 16);
        } else {
            if (g_drag == DRAG_RESIZE) {
                const Window* w = g_drag_w;
                dbg64_line_begin64();
                dbg64_str("[UI] win resize end dir=");
                dbg64_str(resize_dir_name64(g_resize_dir));
                dbg64_str(" x=");
                dbg64_dec((uint64_t)w->x);
                dbg64_str(" y=");
                dbg64_dec((uint64_t)w->y);
                dbg64_str(" w=");
                dbg64_dec((uint64_t)w->w);
                dbg64_str(" h=");
                dbg64_dec((uint64_t)w->h);
                dbg64_str(" client=");
                dbg64_dec((uint64_t)w->client_w);
                dbg64_str("x");
                dbg64_dec((uint64_t)w->client_h);
                dbg64_nl();
                dbg64_line_end64();
            }
            g_drag = DRAG_NONE;
            g_resize_dir = 0;
            g_drag_w = nullptr;
        }
    }
    // ★ P5：动效推进（Dock 窗口列表 / 最小化飞行 / 恢复弹出）+ 光标形状（按命中区域切换）
    winlist_update64(mx, my);
    (void)fly_tick64();        // 飞行帧只脏自己那两块小矩形（见 fly_tick64 里的 prev/cur dirty）
    pop_tick64();
    cursor_shape_update64(mx, my);
    g_prev_btn = btn;
}

static void handle_keyboard(void) {
    // 热键：Ctrl+Shift+Esc -> 任务管理器
    if (kbd_tm_hotkey()) {
        kbd_consume_tm_hotkey();
        dbg64_str("[UI] hotkey ctrl+shift+esc");
        dbg64_nl();
        app_tmgr_open64();
        return;
    }
    // 热键：Win -> 开始菜单（P2：新开始菜单/弹窗打开时，Win = 关掉它们；否则老菜单，兼容既有验收）
    if (kbd_win_pressed()) {
        kbd_consume_win();
        if (panels64_any_open64() || startmenu64_is_open64()) {
            (void)panels64_close_all64("win-key");
            startmenu64_close64("win-key");
            return;
        }
        menu_toggle();
        return;
    }
    uint8_t c = 0;
    while (kbd_pop_char(&c)) {
        // ★ P5：Ctrl+Shift+<字母> 组合热键 —— 统一走"按键时刻记录"（input.cpp kbd_ctrl_shift_char64）。
        //   为什么：QEMU sendkey 的按下/释放只隔几毫秒，外壳忙一帧再处理时 kbd_ctrl_pressed()
        //   已经变回 false，组合热键会**偶发漏掉**（实测 gui_modern64_test 的 Ctrl+Shift+R 会红）。
        //   覆盖：Ctrl+Shift+T/N/R（主题 / 壁纸适应 / 减动效）+ Ctrl+Shift+W（最小化活动窗口）。
        {
            char csw = 0;
            const bool cs_key = kbd_ctrl_shift_char64(&csw);       // 取走即清空
            if (cs_key && (csw == 'T' || csw == 'N' || csw == 'R')) {
                if (theme64_hotkey64((uint8_t)csw, 1, 1)) {
                    gui64_invalidate();
                    continue;
                }
            }
            if (cs_key && csw == 'W') {
                Window* aw = top_visible_win64();
                dbg64_str("[UI] hotkey ctrl+shift+w minimize");
                dbg64_nl();
                if (aw) {
                    aw->minimized = true;
                    fly_begin64(aw);                   // 缩小 + 渐隐飞向 Dock 图标
                    gui64_invalidate();
                }
                continue;                              // 消费这一次按键（不再落到应用/菜单）
            }
        }
        // 老路径：不带 Shift 的 Ctrl+<字母>（键盘层折成的控制码）—— 保持既有行为
        if (kbd_ctrl_pressed() && theme64_hotkey64(c, 1, kbd_shift_pressed() ? 1 : 0)) {
            char drain = 0;
            (void)kbd_ctrl_shift_char64(&drain);       // 同一按键的"按键时刻记录"一并取走，避免重复触发
            gui64_invalidate();
            continue;
        }
        // ★ P2：二级弹窗优先吃键（ESC 一级）→ 开始菜单（ESC 二级）→ ★ P5 桌面右键菜单 → 老菜单
        if (panels64_handle_key64(c)) continue;
        if (startmenu64_handle_key64(c)) continue;
        if (desktopops64_menu_key64(c)) continue;
        if (g_menu_open) {
            if (c == 0xFD) { g_menu_sel = (g_menu_sel + MENU_ITEMS - 1) % MENU_ITEMS; dirty_add(menu_x(), menu_y(), MENU_W, MENU_H); continue; }
            if (c == 0xFE) { g_menu_sel = (g_menu_sel + 1) % MENU_ITEMS; dirty_add(menu_x(), menu_y(), MENU_W, MENU_H); continue; }
            if (c == '\n' || c == '\r') { const int s = g_menu_sel; g_menu_open = false; dirty_add(menu_x(), menu_y(), MENU_W, MENU_H); menu_activate(s); return; }
            if (c == 0x1B) { menu_toggle(); continue; }
            if (c >= '0' && c <= '9') {
                int idx = (c == '0') ? 9 : (c - '1');
                g_menu_open = false; dirty_add(menu_x(), menu_y(), MENU_W, MENU_H);
                menu_activate(idx);
                return;
            }
            continue;
        }
        // 归一化回车：应用两种都认，但统一成 '\n' 更省心（保留原码给终端也无妨）
        Window* w = g_z;
        while (w && (!w->visible || w->minimized)) w = w->next;
        if (w && w->on_key) w->on_key(w, (char)c);
    }
}

// ==================== ★ P5：桌面交互细节（光标形状 / 八向缩放 / 最小化飞行 / 恢复弹出 / Dock 窗口列表）====================
// 需求原文落点：
//   * 鼠标移到窗口**边缘/四角** -> 左右 / 上下 / 对角箭头（四角各有对应）；
//   * 移到**文本输入框**上 -> I 形文本指针；**程序加载时** -> 转圈；其余 -> 普通箭头。
//   形状**状态**在 input.cpp（mouse_set_cursor64 会打 [INPUT64] cursor shape=...），
//   本文件只按命中区域切换（这是需求原文的分工）。

static void cur_log_set64(int shape, const char* why) {
    if (shape == g_cursor_shape) return;
    g_cursor_shape = shape;
    mouse_set_cursor64(shape);
    if (g_cursor_log_budget > 0) {
        g_cursor_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[GUI64] cursor switch shape=");
        dbg64_str(mouse_cursor_name64(shape));
        dbg64_str(" why=");
        dbg64_str(why);
        dbg64_nl();
        dbg64_line_end64();
    }
}

static Window* top_visible_win64(void) {
    for (Window* w = g_z; w; w = w->next) if (w->visible && !w->minimized) return w;
    return nullptr;
}

// 八向缩放命中：返回方向位（0 = 不在抓取带上）
static int resize_dir64(const Window* w, int mx, int my) {
    if (!w || !w->visible || w->minimized || w->maximized) return 0;
    if (mx < w->x - GUI64_GRIP || mx >= w->x + w->w + GUI64_GRIP) return 0;
    if (my < w->y - GUI64_GRIP || my >= w->y + w->h + GUI64_GRIP) return 0;
    int dir = 0;
    if (mx < w->x + GUI64_GRIP) dir |= RZ_L;
    else if (mx >= w->x + w->w - GUI64_GRIP) dir |= RZ_R;
    if (my < w->y + GUI64_GRIP) dir |= RZ_T;
    else if (my >= w->y + w->h - GUI64_GRIP) dir |= RZ_B;
    return dir;
}
static int resize_dir_shape64(int dir) {
    const bool lr = (dir & (RZ_L | RZ_R)) != 0;
    const bool tb = (dir & (RZ_T | RZ_B)) != 0;
    if (lr && !tb) return MOUSE_CUR_H;
    if (tb && !lr) return MOUSE_CUR_V;
    if ((dir & RZ_L) && (dir & RZ_T)) return MOUSE_CUR_D1;      // 左上 - 右下
    if ((dir & RZ_R) && (dir & RZ_B)) return MOUSE_CUR_D1;
    return MOUSE_CUR_D2;                                        // 右上 - 左下
}
static const char* resize_dir_name64(int dir) {
    switch (dir) {
        case RZ_L:          return "l";
        case RZ_R:          return "r";
        case RZ_T:          return "t";
        case RZ_B:          return "b";
        case RZ_L | RZ_T:   return "tl";
        case RZ_R | RZ_T:   return "tr";
        case RZ_L | RZ_B:   return "bl";
        case RZ_R | RZ_B:   return "br";
        default:            return "?";
    }
}

// 文本输入区（"移到输入框上变成文本指针"）：终端命令行、资源管理器地址栏、开始菜单搜索框
static bool point_in_text_zone64(Window* w, int mx, int my) {
    if (!w) return false;
    if (w->app_id == APP_ID_TERM) return true;                       // 终端客户区整块都是文本输入
    if (w->app_id == APP_ID_MYPC) {                                  // 资源管理器顶部地址栏
        const int bar_h = THEME64_POP_BTN_H;
        return (my >= w->client_y && my < w->client_y + bar_h + 4);
    }
    if (startmenu64_is_open64()) {
        int x = 0, y = 0, ww = 0, hh = 0;
        if (startmenu64_search_rect64(&x, &y, &ww, &hh) &&
            mx >= x && mx < x + ww && my >= y && my < y + hh) return true;
    }
    return false;
}

static void cursor_shape_update64(int mx, int my) {
    int shape = MOUSE_CUR_ARROW;
    const char* why = "default";
    // 1) 程序加载中：窗口已创建但**第一帧还没画完**（且至少持续 ~400ms 让人看得见转圈）-> wait
    if (g_load_w && gui64_window_alive(g_load_w) &&
        (!g_load_done || (int32_t)(ticks64() - g_load_t0) < (int32_t)ms_to_ticks64(400))) {
        if (mx >= g_load_w->x && mx < g_load_w->x + g_load_w->w &&
            my >= g_load_w->y && my < g_load_w->y + g_load_w->h) {
            shape = MOUSE_CUR_WAIT;
            why = "app-load";
        }
    }
    // 2) 窗口边缘/四角 -> 缩放箭头。**先于文本指针判定**：抓取带含窗口内侧 6px，
    //    与 Windows 11 同语义（边缘就是边缘；文本指针只在窗口内部、远离边缘处生效）。
    if (shape == MOUSE_CUR_ARROW && !g_menu_open && !startmenu64_is_open64() && !panels64_any_open64()) {
        for (Window* p = g_z; p; p = p->next) {
            const int dir = resize_dir64(p, mx, my);
            if (dir) { shape = resize_dir_shape64(dir); why = "window-edge"; break; }
            if (p->visible && !p->minimized &&
                mx >= p->x && mx < p->x + p->w && my >= p->y && my < p->y + p->h) break;
        }
    }
    // 3) 文本输入框（终端命令行 / 资源管理器地址栏 / 开始菜单搜索框）-> I 形指针
    if (shape == MOUSE_CUR_ARROW) {
        Window* wt = win_at(mx, my);
        if (wt && point_in_text_zone64(wt, mx, my)) { shape = MOUSE_CUR_TEXT; why = "text-input"; }
    }
    cur_log_set64(shape, why);
}

// ---- 最小化飞行：抓一张降采样快照（从后备缓冲读；72x48 省内存）----
static void fly_snapshot64(const Window* w) {
    g_fly_snap_ok = false;
    if (!w || w->w < 4 || w->h < 4) return;
    for (int j = 0; j < FLY_SNAP_H; j++) {
        const int sy = w->y + j * w->h / FLY_SNAP_H;
        for (int i = 0; i < FLY_SNAP_W; i++) {
            const int sx = w->x + i * w->w / FLY_SNAP_W;
            g_fly_snap[j * FLY_SNAP_W + i] = fb_get_pixel(sx, sy);
        }
    }
    g_fly_snap_ok = true;
}

// 找一个应用在 Dock 里的下标（-1 = 没固定）
static int dock_index_of_app64(int app_id) {
    for (int i = 1; i < DOCK_ITEMS; i++) if (kDockItems[i].app_id == app_id) return i;
    return -1;
}

static void fly_begin64(Window* w) {
    if (!w) return;
    const int idx = dock_index_of_app64((int)w->app_id);
    if (idx < 0) {
        dbg64_str("[DOCK64] minimize fly skipped (app not pinned)");
        dbg64_nl();
        return;
    }
    FlyAnim* f = &g_fly[g_fly_n++];
    f->w = w;
    f->t0 = ticks64();
    f->dur = (int)theme64_dur64(THEME64_MS_NORMAL);
    f->frame = 0;
    f->fx = w->x; f->fy = w->y; f->fw = w->w; f->fh = w->h;
    const int ipx = g_dock_icon;
    f->tw = ipx; f->th = ipx;
    f->tx = dock_item_draw_x64(idx) + g_dock_icon / 2 - ipx / 2;
    f->ty = g_dock_y + (g_dock_h - ipx) / 2;
    f->px = f->fx; f->py = f->fy; f->pw = f->fw; f->ph = f->fh;
    fly_snapshot64(w);
    dbg64_line_begin64();
    dbg64_str("[DOCK64] minimize fly app=");
    dbg64_dec((uint64_t)w->app_id);
    dbg64_str(" dock_idx=");
    dbg64_dec((uint64_t)idx);
    dbg64_str(" from=");
    dbg64_dec((uint64_t)f->fx); dbg64_str(","); dbg64_dec((uint64_t)f->fy);
    dbg64_str(" ");
    dbg64_dec((uint64_t)f->fw); dbg64_str("x"); dbg64_dec((uint64_t)f->fh);
    dbg64_str(" to=");
    dbg64_dec((uint64_t)f->tx); dbg64_str(","); dbg64_dec((uint64_t)f->ty);
    dbg64_str(" ");
    dbg64_dec((uint64_t)f->tw); dbg64_str("x"); dbg64_dec((uint64_t)f->th);
    dbg64_str(" dur=");
    dbg64_dec((uint64_t)f->dur);
    dbg64_str("ms anim=scale+fade");
    dbg64_nl();
    dbg64_line_end64();
}

// 每帧推进飞行；返回 1 = 画面有变化
static int fly_tick64(void) {
    if (g_fly_n == 0) return 0;
    int changed = 0;
    const uint32_t now = ticks64();
    for (int i = 0; i < g_fly_n; i++) {
        FlyAnim* f = &g_fly[i];
        if (f->w == nullptr) continue;
        const int el = (int)(now - f->t0);
        if (el >= f->dur) {
            dbg64_line_begin64();
            dbg64_str("[DOCK64] minimize fly done app=");
            dbg64_dec((uint64_t)f->w->app_id);
            dbg64_str(" frames=");
            dbg64_dec((uint64_t)f->frame);
            dbg64_str(" anim=scale+fade");
            dbg64_nl();
            dbg64_line_end64();
            f->w = nullptr;
            changed = 1;
            continue;
        }
        f->frame++;
        const int t = (int)((int64_t)el * 256 / (f->dur > 0 ? f->dur : 1));
        const int e = theme64_ease64(t);
        const int cx = f->fx + (f->tx - f->fx) * e / 256;
        const int cy = f->fy + (f->ty - f->fy) * e / 256;
        const int cw = f->fw + (f->tw - f->fw) * e / 256;
        const int ch = f->fh + (f->th - f->fh) * e / 256;
        const int alpha = 255 - (255 * t / 256);
        if (f->frame <= 12) {
            dbg64_line_begin64();
            dbg64_str("[DOCK64] minimize fly frame=");
            dbg64_dec((uint64_t)f->frame);
            dbg64_str(" t=");
            dbg64_dec((uint64_t)t);
            dbg64_str(" rect=");
            dbg64_dec((uint64_t)cx); dbg64_str(","); dbg64_dec((uint64_t)cy);
            dbg64_str(" ");
            dbg64_dec((uint64_t)cw); dbg64_str("x"); dbg64_dec((uint64_t)ch);
            dbg64_str(" scale=");
            dbg64_dec((uint64_t)(cw * 100 / (f->fw > 0 ? f->fw : 1)));
            dbg64_str(" alpha=");
            dbg64_dec((uint64_t)alpha);
            dbg64_nl();
            dbg64_line_end64();
        }
        dirty_add(f->px - 2, f->py - 2, f->pw + 4, f->ph + 4);   // 擦上一帧
        f->px = cx; f->py = cy; f->pw = cw; f->ph = ch;
        dirty_add(cx - 2, cy - 2, cw + 4, ch + 4);              // 画本帧
        changed = 1;
    }
    int n = 0;
    for (int i = 0; i < g_fly_n; i++) if (g_fly[i].w) g_fly[n++] = g_fly[i];
    g_fly_n = n;
    return changed;
}

// 画飞行中的缩略图（缩放 + 透明度渐隐）——窗口已 minimized，不会再被正常路径画出来
static void fly_draw64(void) {
    if (g_fly_n == 0 || !g_fly_snap_ok) return;
    const uint32_t now = ticks64();
    for (int i = 0; i < g_fly_n; i++) {
        FlyAnim* f = &g_fly[i];
        if (!f->w) continue;
        const int el = (int)(now - f->t0);
        if (el >= f->dur) continue;
        const int t = (int)((int64_t)el * 256 / (f->dur > 0 ? f->dur : 1));
        const int e = theme64_ease64(t);
        const int cx = f->fx + (f->tx - f->fx) * e / 256;
        const int cy = f->fy + (f->ty - f->fy) * e / 256;
        const int cw = f->fw + (f->tw - f->fw) * e / 256;
        const int ch = f->fh + (f->th - f->fh) * e / 256;
        if (cw < 2 || ch < 2) continue;
        const int alpha = 255 - (255 * t / 256);
        for (int j = 0; j < ch; j++) {
            const int sy = j * FLY_SNAP_H / ch;
            if (sy < 0 || sy >= FLY_SNAP_H) continue;
            for (int k = 0; k < cw; k++) {
                const int sx = k * FLY_SNAP_W / cw;
                if (sx < 0 || sx >= FLY_SNAP_W) continue;
                const uint32_t c = g_fly_snap[sy * FLY_SNAP_W + sx];
                gfx64_blend64(cx + k, cy + j,
                              rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF), alpha);
            }
        }
    }
}

// ---- 恢复时的 scale + opacity 弹出（几何插值 + 逐帧重排；窗口内容随尺寸变化）----
static void pop_begin64(Window* w, const char* why) {
    if (!w) return;
    g_pop_w = w;
    g_pop_t0 = ticks64();
    g_pop_dur = (int)theme64_dur64(THEME64_MS_NORMAL);
    g_pop_frame = 0;
    dirty_add(w->x, w->y, w->w, w->h);
    dbg64_line_begin64();
    dbg64_str("[DOCK64] restore pop app=");
    dbg64_dec((uint64_t)w->app_id);
    dbg64_str(" dur=");
    dbg64_dec((uint64_t)g_pop_dur);
    dbg64_str("ms anim=scale0.88->1.00+opacity why=");
    dbg64_str(why);
    dbg64_nl();
    dbg64_line_end64();
}
static int pop_active64(void) {
    return g_pop_w && gui64_window_alive(g_pop_w) && g_pop_dur > 0 &&
           (int32_t)(ticks64() - g_pop_t0) < (int32_t)g_pop_dur;
}
// 返回窗口该按哪个几何画（弹出动画期间是插值出来的"缩小版"）
static void pop_geom64(const Window* w, int* x, int* y, int* ww, int* hh) {
    *x = w->x; *y = w->y; *ww = w->w; *hh = w->h;
    if (!pop_active64() || g_pop_w != w) return;
    int t = (int)((int64_t)(ticks64() - g_pop_t0) * 256 / (g_pop_dur > 0 ? g_pop_dur : 1));
    if (t > 256) t = 256;
    const int e = theme64_ease64(t);
    const int sc = 880 + 120 * e / 256;                 // 0.88 -> 1.00
    *x = w->x + w->w / 2 - w->w * sc / 2560;
    *y = w->y + w->h / 2 - w->h * sc / 2560;
    *ww = w->w * sc / 2560;
    *hh = w->h * sc / 2560;
}
static void pop_tick64(void) {
    if (g_pop_w == nullptr) return;
    if (!gui64_window_alive(g_pop_w)) { g_pop_w = nullptr; return; }
    if ((int32_t)(ticks64() - g_pop_t0) >= (int32_t)g_pop_dur) {
        dbg64_line_begin64();
        dbg64_str("[DOCK64] restore pop done app=");
        dbg64_dec((uint64_t)g_pop_w->app_id);
        dbg64_str(" frames=");
        dbg64_dec((uint64_t)g_pop_frame);
        dbg64_str(" scale=100 opacity=255");
        dbg64_nl();
        dbg64_line_end64();
        dirty_add(g_pop_w->x, g_pop_w->y, g_pop_w->w, g_pop_w->h);
        g_pop_w = nullptr;
        return;
    }
    g_pop_frame++;
    dirty_add(g_pop_w->x - 2, g_pop_w->y - 2, g_pop_w->w + 4, g_pop_w->h + 4);
}

// ---- Dock 悬停窗口列表（同一应用多窗口：可选择恢复"指定窗口"；一应用只有一根小横杠）----
static void winlist_collect64(int idx, Window** out, int* n) {
    *n = 0;
    if (idx <= 0 || idx >= DOCK_ITEMS) return;
    const int app = kDockItems[idx].app_id;
    for (int i = 0; i < MAX_WINS; i++) {
        if (!g_used[i]) continue;
        Window* w = &g_wins[i];
        if (!w->visible || (int)w->app_id != app) continue;
        out[(*n)++] = w;
    }
}
static void winlist_update64(int mx, int my) {
    static uint32_t dwell_t0 = 0;
    static int      dwell_idx = -1;
    int want = -1;
    // 光标已经在弹出的窗口列表里：保持它开着（否则鼠标一移过去就关，永远点不到行）
    if (g_winlist_idx >= 0 &&
        mx >= g_winlist_x && mx < g_winlist_x + g_winlist_w &&
        my >= g_winlist_y && my < g_winlist_y + g_winlist_h) {
        want = g_winlist_idx;
    } else if (g_dock_hover > 0 && !panels64_any_open64() && !startmenu64_is_open64() && !g_menu_open) {
        Window* tmp[MAX_WINS];
        int n = 0;
        winlist_collect64(g_dock_hover, tmp, &n);
        if (n > 0) {
            if (dwell_idx != g_dock_hover) { dwell_idx = g_dock_hover; dwell_t0 = ticks64(); }
            if (g_winlist_idx == g_dock_hover) want = g_dock_hover;
            // 悬停 ~150ms（Token：快动效）就弹出窗口列表 —— 需求："鼠标放到 Dock 图标上方就会弹出窗口列表"
            else if (g_dock_hover >= 0 && (int32_t)(ticks64() - dwell_t0) >= (int32_t)theme64_dur64(THEME64_MS_FAST))
                want = g_dock_hover;
        }
    }
    if (want < 0) {
        if (g_winlist_idx >= 0) { g_winlist_idx = -1; g_winlist_logged = 0; gui64_invalidate(); }
        {
            // 悬停换了图标（或第一次悬停）就允许重新打点 —— 否则"窗口列表一直开着"会吞掉后续验收证据
            static int logged_for = -1;
            if (logged_for != g_dock_hover) { logged_for = g_dock_hover; g_winlist_logged = 0; }
        }
        return;
    }
    Window* tmp[MAX_WINS];
    int n = 0;
    winlist_collect64(want, tmp, &n);
    int maxw = 0;
    for (int i = 0; i < n; i++) { const int w2 = text_w(tmp[i]->title); if (w2 > maxw) maxw = w2; }
    g_winlist_w = maxw + THEME64_POP_PAD * 2 + 24;
    if (g_winlist_w < 180) g_winlist_w = 180;
    if (g_winlist_w > 360) g_winlist_w = 360;
    g_winlist_h = n * THEME64_POP_ROW + THEME64_POP_PAD;
    const int icx = dock_item_draw_x64(want) + g_dock_icon / 2;
    g_winlist_x = icx - g_winlist_w / 2;
    g_winlist_y = g_dock_y - THEME64_POP_GAP - g_winlist_h;
    if (g_winlist_x < 4) g_winlist_x = 4;
    if (g_winlist_x + g_winlist_w > g_screen_w - 4) g_winlist_x = g_screen_w - 4 - g_winlist_w;
    if (g_winlist_y < 4) g_winlist_y = 4;
    if (!g_winlist_logged) {
        g_winlist_logged = 1;
        dbg64_line_begin64();
        dbg64_str("[DOCK64] winlist idx=");
        dbg64_dec((uint64_t)want);
        dbg64_str(" n=");
        dbg64_dec((uint64_t)n);
        dbg64_str(" x=");
        dbg64_dec((uint64_t)g_winlist_x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)g_winlist_y);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)g_winlist_w);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)g_winlist_h);
        dbg64_str(" row=");
        dbg64_dec((uint64_t)THEME64_POP_ROW);
        dbg64_str(" one-bar-per-app=1");
        dbg64_nl();
        dbg64_line_end64();
        for (int i = 0; i < n; i++) {
            dbg64_line_begin64();
            dbg64_str("[DOCK64] winlist item idx=");
            dbg64_dec((uint64_t)want);
            dbg64_str(" row=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" title=");
            dbg64_str(tmp[i]->title);
            dbg64_str(" minimized=");
            dbg64_dec(tmp[i]->minimized ? 1 : 0);
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    g_winlist_idx = want;
    g_winlist_row = -1;
    if (mx >= g_winlist_x && mx < g_winlist_x + g_winlist_w &&
        my >= g_winlist_y && my < g_winlist_y + g_winlist_h) {
        const int r = (my - g_winlist_y - THEME64_POP_PAD / 2) / THEME64_POP_ROW;
        if (r >= 0 && r < n) g_winlist_row = r;
    }
}
static void winlist_draw64(void) {
    if (g_winlist_idx < 0) return;
    Window* tmp[MAX_WINS];
    int n = 0;
    winlist_collect64(g_winlist_idx, tmp, &n);
    if (n == 0) return;
    const Theme64Tokens* t = theme64_tokens64();
    p2ui_popup64(g_winlist_x, g_winlist_y, g_winlist_w, g_winlist_h, THEME64_POP_R, THEME64_POP_R,
                 t, t->client_bg, t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    for (int i = 0; i < n; i++) {
        const int iy = g_winlist_y + THEME64_POP_PAD / 2 + i * THEME64_POP_ROW;
        if (i == g_winlist_row)
            gfx64_fill_round64(g_winlist_x + 4, iy + 2, g_winlist_w - 8, THEME64_POP_ROW - 4,
                               THEME64_R_BUTTON, t->sel_bg, 110);
        p2ui_text64(g_winlist_x + THEME64_POP_PAD, iy + (THEME64_POP_ROW - p2ui_line_h64()) / 2,
                    tmp[i]->title, tmp[i]->minimized ? t->text_dim : t->text);
        p2ui_fill_circle64(g_winlist_x + g_winlist_w - THEME64_POP_PAD - 4,
                           iy + THEME64_POP_ROW / 2, 3,
                           tmp[i]->minimized ? t->text_dim : t->accent, 220);
    }
}
static int winlist_press64(int mx, int my) {
    if (g_winlist_idx < 0) return 0;
    if (mx < g_winlist_x || mx >= g_winlist_x + g_winlist_w ||
        my < g_winlist_y || my >= g_winlist_y + g_winlist_h) return 0;
    Window* tmp[MAX_WINS];
    int n = 0;
    winlist_collect64(g_winlist_idx, tmp, &n);
    const int r = (my - g_winlist_y - THEME64_POP_PAD / 2) / THEME64_POP_ROW;
    if (r < 0 || r >= n) return 1;
    Window* w = tmp[r];
    const bool was_min = w->minimized;
    dbg64_line_begin64();
    dbg64_str("[DOCK64] winlist restore idx=");
    dbg64_dec((uint64_t)g_winlist_idx);
    dbg64_str(" row=");
    dbg64_dec((uint64_t)r);
    dbg64_str(" title=");
    dbg64_str(w->title);
    dbg64_str(" was_minimized=");
    dbg64_dec(was_min ? 1 : 0);
    dbg64_nl();
    dbg64_line_end64();
    w->minimized = false;
    gui64_set_active(w);
    if (was_min) pop_begin64(w, "winlist");
    g_winlist_idx = -1;
    g_winlist_logged = 0;
    gui64_invalidate();
    return 1;
}

// ==================== 外壳自检 ====================
int gui64_selftest() {
    int fails = 0;
    // 1) 建/销 + app 计数
    Window* a = gui64_create_window("t1", 10, 10, 200, 200, nullptr, nullptr, nullptr, APP_ID_CALC);
    Window* b = gui64_create_window("t2", 20, 20, 300, 260, nullptr, nullptr, nullptr, APP_ID_CALC);
    if (!a || !b) fails |= 1;
    if (gui64_app_windows(APP_ID_CALC) != 2) fails |= 2;
    if (gui64_window_count() != 2) fails |= 4;
    // 2) 客户区几何：client_w = w - 2, client_h = h - 25
    if (a && (a->client_w != a->w - DECO_W || a->client_h != a->h - DECO_H - BORDER)) fails |= 8;
    // 3) fit_window_to_client 精确命中
    if (a) {
        gui64_set_min_size(a, 100, 100);
        if (!gui64_fit_window_to_client(a, 286, 368)) fails |= 16;
        if (a->client_w != 286 || a->client_h != 368) fails |= 32;
        // 违反最小尺寸时必须拒绝
        gui64_set_min_size(a, 400, 400);
        if (gui64_fit_window_to_client(a, 10, 10)) fails |= 64;
        gui64_set_min_size(a, 100, 100);
    }
    // 4) z 序：后建的在前
    if (gui64_top_window() != b) fails |= 128;
    // 5) close_app 关掉全部对应窗口
    if (gui64_close_app(APP_ID_CALC) != 2) fails |= 256;
    if (gui64_window_count() != 0) fails |= 512;
    // 6) 死指针判定
    if (gui64_window_alive(a)) fails |= 1024;
    // 7) 语言切换
    const bool save = g_lang_zh;
    gui64_set_lang_zh(false);
    if (gui64_lang_zh()) fails |= 2048;
    if (gui64_tr("E", "Z")[0] != 'E') fails |= 4096;
    gui64_set_lang_zh(save);
    // 8) 菜单表完整性
    for (int i = 0; i < MENU_ITEMS; i++)
        if (!menu_zh[i] || !menu_en[i] || !menu_zh[i][0] || !menu_en[i][0]) fails |= 8192;
    // 9) 脏矩形并集
    g_dirty_any = false;
    gui64_dirty(10, 10, 20, 20);
    gui64_dirty(40, 30, 10, 10);
    if (!g_dirty_any || g_dirty_x0 != 10 || g_dirty_y0 != 10 || g_dirty_x1 != 50 || g_dirty_y1 != 40)
        fails |= 16384;
    g_dirty_any = false;

    dbg64_str("[GUI64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" slots=");
    dbg64_dec((uint64_t)MAX_WINS);
    dbg64_nl();
    return fails;
}

static int deskpos_x64(int i) { int x = 0, y = 0; desktopops64_pos64(i, &x, &y); return x; }
static int deskpos_y64(int i) { int x = 0, y = 0; desktopops64_pos64(i, &x, &y); return y; }

// ==================== 主循环 ====================
[[noreturn]] void gui64_run(const BootInfo* bi) {
    (void)bi;
    // ---- 先读系统配置（config64 -> store64 持久化；默认表见 kernel/config64.cpp）----
    // 必须在算屏幕尺寸之前读：display.zoom 会改变**渲染分辨率**（fb_width/fb_height）。
    {
        const int z = cfg64_zoom64();
        if (z != fb_get_zoom()) fb_set_zoom(z);
        g_lang_zh    = cfg64_lang_zh64() ? true : false;
        g_text_mirror = cfg64_text_mirror64() ? true : false;
        g_mouse_sens  = cfg64_mouse_sens64();
        g_mouse_sens_off = (g_mouse_sens != 1700);
    }
    g_screen_w = fb_width();
    g_screen_h = fb_height();
    mouse_set_bounds(g_screen_w, g_screen_h);
    for (int i = 0; i < MAX_WINS; i++) g_used[i] = false;
    g_z = nullptr;
    g_icon_sel = -1;
    g_icon_drag_idx = -1;
    g_icon_drag_moved = false;
    // ★ P5：桌面图标集合（项/位置/回收站内容）由 desktopops64 从 config64 载入
    desktopops64_init64();
    {
        dbg64_line_begin64();
        dbg64_str("[CONF64] apply lang=");
        dbg64_str(g_lang_zh ? "zh" : "en");
        dbg64_str(" zoom=");
        dbg64_dec((uint64_t)fb_get_zoom());
        dbg64_str(" mirror=");
        dbg64_dec(g_text_mirror ? 1 : 0);
        dbg64_str(" sens=");
        dbg64_dec((uint64_t)g_mouse_sens);
        dbg64_str(" icon0=");
        dbg64_dec((uint64_t)deskpos_x64(0)); dbg64_str(","); dbg64_dec((uint64_t)deskpos_y64(0));
        dbg64_str(" icon1=");
        dbg64_dec((uint64_t)deskpos_x64(1)); dbg64_str(","); dbg64_dec((uint64_t)deskpos_y64(1));
        dbg64_str(" icon2=");
        dbg64_dec((uint64_t)deskpos_x64(2)); dbg64_str(","); dbg64_dec((uint64_t)deskpos_y64(2));
        dbg64_str(" startup=t");
        dbg64_dec((uint64_t)cfg64_startup64("terminal"));
        dbg64_str(",m");
        dbg64_dec((uint64_t)cfg64_startup64("monitor"));
        dbg64_str(",d");
        dbg64_dec((uint64_t)cfg64_startup64("desktop"));
        dbg64_str(",h");
        dbg64_dec((uint64_t)cfg64_startup64("health"));
        dbg64_nl();
        dbg64_line_end64();
        if (g_mouse_sens_off) {
            dbg64_line_begin64();
            dbg64_str("[CONF64] mouse sens=");
            dbg64_dec((uint64_t)g_mouse_sens);
            dbg64_str("permille applied (baseline 1700 = driver default)");
            dbg64_nl();
            dbg64_line_end64();
        }
    }

    // ---- ★ 本批（真图标）：读外置图标包（系统镜像内核区尾部的固定区间；读不到就一路回落程序化绘制）----
    // 位置讲究：进桌面前、theme/dock 之前 —— 之后所有绘制（Dock/桌面图标/开始菜单/面板/设置）都用它。
    // 为什么不在 kernel64.cpp 里初始化：那一层不链本模块（icons64 只进系统内核），且此处已在启动路径上。
    (void)icons64_init64();
    (void)icons64_selftest64();

    dbg64_str("[GUI64] desktop init ");
    dbg64_dec((uint64_t)g_screen_w);
    dbg64_str("x");
    dbg64_dec((uint64_t)g_screen_h);
    dbg64_str(" taskbar=");
    dbg64_dec((uint64_t)TASKBAR_H);
    dbg64_str(" title_h=");
    dbg64_dec((uint64_t)TITLE_H);
    dbg64_nl();
    // ---- ★ 本批：主题 Token 生效 + 壁纸（VimtuFS2 优先，兜底内置）+ Dock 几何 + 图像解码自检 ----
    // 顺序：theme64_init64（读 ui.theme/ui.reduce_motion）→ wall（读 ui.wall.path → img64 解码）→ dock。
    theme64_init64();
    {
        const Theme64Tokens* t0 = theme64_tokens64();
        char wpath[CFG64_STR_MAX];
        wpath[0] = 0;
        cfg64_wall_path64(wpath, (int)sizeof(wpath));
        bool loaded = false;
        if (wpath[0]) {
            Img64 wim{};
            if (img64_load_vfs64(wpath, &wim) == 0) {
                loaded = (gfx64_wall_set_source64(wim.px, wim.w, wim.h, 1, "vfs:wallpaper") == 0);
                img64_free64(&wim);
            }
        }
        if (!loaded) {
            // 未配置/读不到 → 内核内置兜底壁纸（程序化渐变 + 柔光斑 + 定位标记）
            gfx64_wall_build_default64();
            dbg64_line_begin64();
            dbg64_str("[GFX64] init surface=");
            dbg64_dec((uint64_t)g_screen_w);
            dbg64_str("x");
            dbg64_dec((uint64_t)g_screen_h);
            dbg64_str(" wall=");
            dbg64_dec((uint64_t)gfx64_wall_src_w64());
            dbg64_str("x");
            dbg64_dec((uint64_t)gfx64_wall_src_h64());
            dbg64_str(" src=");
            dbg64_str(gfx64_wall_src_desc64());
            dbg64_str(" theme=");
            dbg64_dec((uint64_t)theme64_id64());
            dbg64_str(" name=");
            dbg64_str(t0->name);
            dbg64_nl();
            dbg64_line_end64();
        }
        const int rc_img = img64_selftest64();
        if (rc_img != 0) {
            dbg64_str("[IMG64] selftest mask=");
            dbg64_dec((uint64_t)rc_img);
            dbg64_nl();
        }
        // ★ P3：设置页持久化的项在这里生效（字体大小档 + 自定义渐变壁纸）。必须在壁纸装载决定
        //   **之后**（否则会被 gfx64_wall_build_default64 覆盖）、锁屏/桌面首帧之前（字号要先生效）。
        settings64_boot_apply64();
        dock_start_icon_init64();
        dock_geom_init64();
        g_dock_hover = -1;
        // ★ P2：开始菜单（几何/自检）与四个二级弹窗 + 设备 toast（锚点全在这里定型）
        startmenu64_init64();
        panels64_init64();
    }
    // ---- 开机 logo：桌面首帧之前先放一段黑底 + 居中 logo 的淡入（~12 帧 ≈ 200ms）----
    boot_logo_fade_in();
    // ---- ★ 本批（P1c）：锁屏 -> 登录（开机顺序：滚屏 -> 开机动画 -> 锁屏 -> 登录 -> 桌面）----
    // 这个调用**不返回**直到登录成功，所以下面的自检/首帧/[GUI64] ready（桌面出现）一定晚于
    // "[LOCK64] lock screen shown"。安装介质内核不链 locklogin64（见 build64.sh），
    // 所以安装向导路径上不会出现锁屏。
    locklogin64_run64();


    g_in_selftest = true;          // 自检里的临时窗口 / 语言来回切 不触发会话与配置钩子
    const int st = gui64_selftest() | desktopops64_selftest64();
    g_in_selftest = false;
    if (st != 0) {
        dbg64_str("[GUI64] selftest FAILED mask=");
        dbg64_dec((uint64_t)st);
        dbg64_nl();
    }

    fb_clear(theme64_tokens64()->desktop_base);   // 首帧背景：主题底色（随后 render 铺壁纸+适应模式）
    g_cur_x = mouse_get_x();
    g_cur_y = mouse_get_y();
    g_prev_cur_x = g_cur_x;
    g_prev_cur_y = g_cur_y;
    gui64_invalidate();
    render();
    gfx64_report64("first_frame");   // 缓存统计（blur_hit/miss、shadow_hit/miss、wall_builds）

    // 这两行都是自动验收的断言行：
    //   "[GUI64] ready"      —— 桌面验收（tests/desktop64_test.py）
    //   "[OS] ready (idle)"  —— 历史断言（install_flow_test / vmware_install_test 依赖，勿删）
    dbg64_str("[GUI64] ready");
    dbg64_nl();
    dbg64_str("[OS] ready (idle)");
    dbg64_nl();
    // ---- 启动项（config64 的 startup.*）+ 会话恢复（session64 的策略）----
    // 放在 "[GUI64] ready" 之后：既有验收看到的启动顺序不变，多出来的只有"按配置自动开窗"。
    if (cfg64_startup64("terminal")) app_term_open64();
    if (cfg64_startup64("monitor")) app_monitor_open64();
    if (cfg64_startup64("health")) (void)sysstate64_health_report64(1, nullptr, 0);
    (void)session64_restore64();
    gui64_invalidate();

    uint32_t next_frame = ticks64();
    uint32_t next_tick = ticks64();
    uint32_t next_cpu_sample = ticks64() + ms_to_ticks64(500);
    uint32_t next_fps = ticks64() + ms_to_ticks64(1000);

    for (;;) {
        const uint32_t now = ticks64();
        // ---- 看门狗心跳（gui64 帧/tick 就是它的心跳源）+ 状态机推进 + 会话/配置推进 ----
        panic64_watchdog_kick64();
        if (sysstate64_tick64()) {
            // 本帧刚进入 RUNNING（桌面首帧）：健康报告已在 tick 里打过；这里武装看门狗
            panic64_watchdog_arm64();
        }
        session64_tick64();          // 关窗后的"清状态"延迟落到这里（避免在销毁路径里递归销毁）
        (void)config64_tick64();     // 配置改动后的延迟落盘（3 秒去抖）
        // ★ 本批：主题/壁纸模式被外部改过（终端 cfg set / 热键）→ 实时生效 + 整屏重建（壁纸+玻璃缓存）
        if (theme64_tick64() | gfx64_wall_tick64()) {
            g_dock_geom_logged = false;
            dock_geom_init64();
            gui64_invalidate();
        }
        // ★ P5：桌面图标集合被外部改过（设置页按钮 / 终端 `cfg set ui.desktop.icons ...`）→ 实时同步
        if (desktopops64_tick64()) {
            desktopops64_clamp64(g_screen_w, g_screen_h);
            gui64_invalidate();
        }
        // ★ P2：开始菜单/弹窗锚在菜单几何上 —— 分辨率/缩放变了（dock 几何重建）时，收起它们免得错位
        if (startmenu64_is_open64() || panels64_any_open64()) {
            static int last_sw = -1, last_sh = -1;
            if (last_sw < 0) {
                last_sw = g_screen_w;      // 第一次看到：只记录，不关（否则一打开就被自己关掉）
                last_sh = g_screen_h;
            } else if (last_sw != g_screen_w || last_sh != g_screen_h) {
                if (startmenu64_is_open64()) startmenu64_close64("screen-change");
                (void)panels64_close_all64("screen-change");
                last_sw = g_screen_w;
                last_sh = g_screen_h;
            }
        }
        // 只有这一个 hook：终端 `loginctl lock` 把状态切回 LOCK 之后，这里会接管输入与整屏重绘，
        // 桌面外壳在锁屏期间既不派发按键也不画窗口。
        // 解锁（登录成功）后的第一帧：整屏重绘一次，把锁屏/登录画面换成桌面。
        {
            static int lock_active = 0;
            if (locklogin64_active64()) {
                if (lock_active == 0) {
                    // ★ P2：切到锁屏/登录层 -> 收起开始菜单与所有弹窗（不留残影/不吃键）
                    if (startmenu64_is_open64()) startmenu64_close64("lock");
                    (void)panels64_close_all64("lock");
                }
                lock_active = 1;
                (void)locklogin64_tick64();
                __asm__ volatile("pause");
                continue;
            }
            if (lock_active) {
                lock_active = 0;
                gui64_invalidate();          // 从锁屏/登录回到桌面：整屏重绘
            }
        }
        handle_mouse();
        handle_keyboard();

        // 每 ~60Hz：跑应用的 tick 回调 + 重绘
        if ((int32_t)(now - next_frame) >= 0) {
            next_frame = now + (PIT_HZ_64 / 60);
            // ★ P2：开始菜单 + 二级弹窗/设备 toast 的每帧推进（动画、时间、设备轮询、toast 生命周期）
            startmenu64_tick64();
            panels64_tick64();
            // 应用的定时回调（计时器等）
            for (Window* w = g_z; w; w = w->next) {
                if (!w->visible) continue;
                if (w->on_tick) {
                    const uint64_t t0 = rdtsc64();
                    w->on_tick(w);
                    w->cpu_cycles += rdtsc64() - t0;
                }
            }
            // 时钟/监视器每秒重画一次
            if ((int32_t)(now - next_tick) >= 0) {
                next_tick = now + PIT_HZ_64;
                dirty_add(g_dock_clock_x, g_dock_y, g_dock_clock_w, g_dock_h);   // 右下角时钟玻璃片
                if (gui64_window_alive(g_mon_win)) gui64_invalidate_window(g_mon_win);
                if (explorer64_window64()) gui64_invalidate_window(explorer64_window64());
                if (gui64_window_alive(g_about_win)) gui64_invalidate_window(g_about_win);
            }
            if (g_dirty_any || g_first_frame) { g_first_frame = false; render(); }
        }

        // 每 500ms：算 CPU 占比 + 各窗口 CPU%
        if ((int32_t)(now - next_cpu_sample) >= 0) {
            next_cpu_sample = now + ms_to_ticks64(500);
            if (g_total_cycles > 0) {
                uint8_t p = (uint8_t)(g_busy_cycles * 100 / g_total_cycles);
                g_busy_pct = p > 100 ? 100 : p;
            }
            g_busy_cycles = 0;
            g_total_cycles = 0;
            uint64_t total = 0;
            for (Window* w = g_z; w; w = w->next) total += w->cpu_cycles;
            for (Window* w = g_z; w; w = w->next) {
                w->cpu_pct = total ? (uint8_t)(w->cpu_cycles * 100 / (total ? total : 1)) : 0;
                w->cpu_cycles = 0;
            }
        }
        // 每秒：帧率
        if ((int32_t)(now - next_fps) >= 0) {
            next_fps = now + ms_to_ticks64(1000);
            g_fps = g_frames;
            g_frames = 0;
            g_fps_count++;
        }
        __asm__ volatile("pause");
    }
}
