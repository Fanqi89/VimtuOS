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
#include "../bootinfo.h"

// ==================== 资源符号（build64.sh 用 objcopy 生成）====================
extern "C" const uint8_t _binary_icon_mycomputer_bin_start[];
extern "C" const uint8_t _binary_icon_recyclebin_bin_start[];
extern "C" const uint8_t _binary_icon_terminal_bin_start[];
extern "C" const uint8_t _binary_icon_start_bin_start[];   // 64x64 RGBA（logo/kaisi.png）
extern "C" const uint8_t _binary_logo_rgba_bin_start[];    // 240x150 RGBA（logo/logo.png）

#define ICON_SRC_W 128          // 图标源图尺寸（RGBA，见 _make_icons.py）

// ==================== 几何常量 ====================
#define TITLE_H      24         // 标题栏高
#define BORDER       1          // 边框
#define DECO_H       (TITLE_H + BORDER)     // 客户区相对窗口顶部的偏移
#define DECO_W       (BORDER * 2)           // 客户区相对窗口左侧的偏移
#define TASKBAR_H    32
#define BTN_W        30         // 标题栏按钮宽
#define ICON_W       48         // 桌面图标位图边长
#define ICON_CELL_W  88         // 图标单元格（含标签）
#define ICON_CELL_H  84
#define MAX_WINS     16
#define MENU_ITEMS   10
#define CLOCK_W      260        // 任务栏右侧时钟（年月日 + 时间）占位宽度
#define LOGO_W       240        // 开机/关机画面用的 logo 嵌入尺寸（logo/logo.png，见 _make_logo.py）
#define LOGO_H       150
#define START_ICON_SRC   64     // 开始按钮图标源尺寸（logo/kaisi.png，见 _make_start_icon.py）
#define START_ICON_DISP  24     // 开始按钮实际绘制尺寸（任务栏高 32，上下留 4px）

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

// 拖拽
enum { DRAG_NONE = 0, DRAG_MOVE, DRAG_RESIZE };
static int  g_drag = DRAG_NONE;
static int  g_drag_dx = 0, g_drag_dy = 0;
static Window* g_drag_w = nullptr;

// 桌面图标
struct DeskIcon { int kind; int x, y; };     // kind: 0=我的电脑 1=回收站 2=终端
static DeskIcon g_icons[3];
static int  g_icon_sel = -1;
static uint32_t g_icon_last_tick = 0;
// 桌面图标拖动（>5px 才算拖动）与空白处选择框的状态（移植自 legacy32/kernel/gui.cpp）
static int  g_icon_drag_idx = -1;              // 正在拖动的图标（-1=无）
static int  g_icon_press_x = 0, g_icon_press_y = 0;            // 按下位置
static int  g_icon_press_icon_x = 0, g_icon_press_icon_y = 0;  // 按下时的图标位置
static bool g_icon_drag_moved = false;
static bool g_selbox_active = false;
static int  g_sel_x0 = 0, g_sel_y0 = 0, g_sel_x1 = 0, g_sel_y1 = 0;
static bool g_start_icon_logged = false;
static bool g_title_btn_logged = false;
static int  sel_box_x() { return g_sel_x0 < g_sel_x1 ? g_sel_x0 : g_sel_x1; }
static int  sel_box_y() { return g_sel_y0 < g_sel_y1 ? g_sel_y0 : g_sel_y1; }
static int  sel_box_w() { return g_sel_x0 < g_sel_x1 ? g_sel_x1 - g_sel_x0 : g_sel_x0 - g_sel_x1; }
static int  sel_box_h() { return g_sel_y0 < g_sel_y1 ? g_sel_y1 - g_sel_y0 : g_sel_y0 - g_sel_y1; }

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
    return win;
}

void gui64_destroy_window(Window* w) {
    int i = win_index(w);
    if (i < 0 || !g_used[i]) return;
    dirty_add(w->x, w->y, w->w, w->h);
    if (w->on_close) w->on_close(w);          // 应用在这里释放 userdata（外壳绝不代劳）
    z_unlink(w);
    w->next = nullptr;
    w->draw = nullptr; w->on_key = nullptr; w->on_click = nullptr;
    w->on_click2 = nullptr; w->on_tick = nullptr; w->on_close = nullptr;
    w->userdata = nullptr;
    w->visible = false;
    g_used[i] = false;
    if (g_drag_w == w) { g_drag_w = nullptr; g_drag = DRAG_NONE; }
    // 焦点交给最上面的窗口
    Window* top = g_z;
    while (top && (!top->visible || top->minimized)) top = top->next;
    if (top) gui64_set_active(top);
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
    if (g_lang_zh == zh) return;
    g_lang_zh = zh;
    gui64_invalidate();
    dbg64_str(zh ? "[UI] lang zh" : "[UI] lang en");
    dbg64_nl();
}
const char* gui64_tr(const char* en, const char* zh) { return g_lang_zh ? zh : en; }
uint32_t    gui64_fps() { return g_fps; }
uint8_t     gui64_cpu_busy_pct() { return g_busy_pct; }


// 关机/重启画面（实现放在后面的"绘制区"：要用那里的 blit_rgba/text_ttf/text_w）
static void power_anim_screen(const char* txt, const char* log_tag);
// ==================== 重启 / 关机（照抄 32 位回退链）====================
void sys_reboot64() {
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
static void draw_icon_rgba(int x, int y, int size, const uint8_t* src, int src_w) {
    blit_rgba(x, y, size, size, src, src_w, src_w, 255);
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
static const char* icon_name(int kind) {
    if (kind == 0) return gui64_tr("My Computer", "我的电脑");
    if (kind == 1) return gui64_tr("Recycle Bin", "回收站");
    return gui64_tr("Terminal", "终端");
}

// 选择框与图标格（图标 + 名字标签）是否相交（移植 32 位的 icon_intersects_sel）
static bool icon_hits_sel(int i, int x0, int y0, int x1, int y1) {
    if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
    const int ix0 = g_icons[i].x - 6, iy0 = g_icons[i].y - 6;
    const int ix1 = ix0 + ICON_CELL_W, iy1 = iy0 + ICON_CELL_H;
    return !(ix1 < x0 || ix0 > x1 || iy1 < y0 || iy0 > y1);
}

// ==================== 外壳文字 ====================
static void text_ttf(int x, int y, const char* s, uint32_t fg) {
    font_select(2);                 // simhei 子集：ASCII + 常用汉字，混排不出豆腐块
    font_draw_text(x, y, s, fg);
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

// ==================== 中间层绘制 ====================
static void draw_desktop_bg(int x0, int y0, int x1, int y1) {
    fb_fill_rect(x0, y0, x1 - x0, y1 - y0, C_DESKTOP);
}
static void draw_icons(void) {
    for (int i = 0; i < 3; i++) {
        const DeskIcon& ic = g_icons[i];
        if (g_icon_sel == i)
            fb_fill_rect(ic.x - 4, ic.y - 4, ICON_W + 8, ICON_W + 20, C_ICON_SEL);
        draw_icon_rgba(ic.x, ic.y, ICON_W, icon_src(ic.kind), ICON_SRC_W);
        const char* nm = icon_name(ic.kind);
        const int tw = text_w(nm);
        int tx = ic.x + (ICON_W - tw) / 2;
        if (tx < 0) tx = 0;
        // 文字加一圈描边，避免蓝底上看不清
        fb_fill_rect(ic.x + (ICON_W - tw) / 2 - 2, ic.y + ICON_W + 2, tw + 4, 18, C_ICON_SEL);
        text_ttf(tx, ic.y + ICON_W + 3, nm, C_ICON_TXT);
    }
}
static void draw_title_buttons(Window* w) {
    // 三个按钮必须**肉眼可区分**：最小化=短横线、最大化=方框、关闭=红底 ✕；hover 高亮。
    // 几何与 handle_mouse_press 的点击判定一致（bx = 窗口右缘 - 3*BTN_W，点击 y ∈ [w->y+2, w->y+24)）。
    const int bx = w->x + w->w - BORDER - BTN_W * 3;
    const int by = w->y + BORDER + 4;
    for (int b = 0; b < 3; b++) {
        const int x = bx + b * BTN_W + 3;
        const int bw = BTN_W - 6, bh = 16;
        const bool hov = (g_cur_x >= bx + b * BTN_W) && (g_cur_x < bx + (b + 1) * BTN_W) &&
                         (g_cur_y >= w->y + BORDER + 1) && (g_cur_y < w->y + TITLE_H);
        uint32_t bg;
        if (b == 2) bg = hov ? C_BTN_CLOSE_H : C_BTN_CLOSE;
        else        bg = hov ? C_BTN_BG_HOV : C_BTN_BG;
        fb_fill_rect(x, by, bw, bh, bg);
        const int cx = x + bw / 2;
        const int cy = by + bh / 2;
        if (b == 0) {                       // 最小化：底部短横线
            fb_fill_rect(cx - 5, cy + 2, 11, 2, C_BTN_GLYPH);
        } else if (b == 1) {                // 最大化：空心方框
            fb_draw_rect(cx - 5, cy - 5, 11, 11, C_BTN_GLYPH);
        } else {                            // 关闭：✕
            for (int i = -4; i <= 4; i++) {
                fb_putpixel(cx + i, cy + i, C_BTN_GLYPH);
                fb_putpixel(cx - i, cy + i, C_BTN_GLYPH);
            }
        }
    }
    if (!g_title_btn_logged) {
        g_title_btn_logged = true;
        dbg64_str("[UI] title btn min/max/close draw ok");
        dbg64_nl();
    }
}
static void draw_window(Window* w) {
    // 边框 + 标题栏
    fb_fill_rect(w->x, w->y, w->w, w->h, C_BORDER);
    fb_fill_rect(w->x + BORDER, w->y + BORDER, w->w - BORDER * 2, TITLE_H - BORDER,
                 w->active ? C_TITLE_ACT : C_TITLE_INA);
    // 标题文字（居中偏左）
    const int ty = w->y + BORDER + (TITLE_H - 14) / 2;
    text_ttf(w->x + 8, ty, w->title, C_TITLE_TXT);
    // 三个按钮（简化绘制：三条横线/方块；关闭按钮红底）
    draw_title_buttons(w);
    // 客户区底色 + 应用内容
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, C_CLIENT);
    if (w->draw) {
        fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
        const uint64_t t0 = rdtsc64();
        w->draw(w);
        w->cpu_cycles += rdtsc64() - t0;
        fb_reset_clip();
    }
}
static void draw_taskbar(void) {
    const int y = g_screen_h - TASKBAR_H;
    fb_fill_rect(0, y, g_screen_w, TASKBAR_H, C_TASKBAR);
    // 开始按钮：logo/kaisi.png（64x64 RGBA）缩放到 24x24（点击区仍是 mx<34，见 handle_mouse_press）
    blit_rgba(6, y + (TASKBAR_H - START_ICON_DISP) / 2, START_ICON_DISP, START_ICON_DISP,
              _binary_icon_start_bin_start, START_ICON_SRC, START_ICON_SRC, 255);
    if (!g_start_icon_logged) {
        g_start_icon_logged = true;
        dbg64_str("[UI] start icon blit size=");
        dbg64_dec((uint64_t)START_ICON_DISP);
        dbg64_nl();
    }
    // 窗口按钮
    int bx = 34;
    for (Window* w = g_z; w; w = w->next) {
        if (!w->visible) continue;
        const int bw = text_w(w->title) + 16;
        if (bx + bw > g_screen_w - CLOCK_W) break;
        fb_fill_rect(bx, y + 3, bw, TASKBAR_H - 6, w->active ? C_TASK_ACT : C_TASK_BTN);
        text_ttf(bx + 8, y + 8, w->title, C_TITLE_TXT);
        bx += bw + 4;
    }
    // 时钟（右对齐）：日期 + 时间，如 "2026-09-19 12:11:35"（必须含年月日）
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
    const int tw = text_w(buf);
    text_ttf(g_screen_w - tw - 10, y + (TASKBAR_H - 14) / 2, buf, C_TITLE_TXT);
    // 时钟文本打点：只在文本变化（每秒）时打一行，便于自动验收 grep 年月日
    if (!str_eq64(buf, g_clock_log)) {
        str_copy64(g_clock_log, buf, (int)sizeof(g_clock_log));
        dbg64_str("[UI] clock text=");
        dbg64_str(buf);
        dbg64_nl();
    }
}

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

static void draw_cursor(void) {
    static const char* cur[] = {
        "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
        "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
        "X.....XXXXX", "X..X..X    ", "X.X.X..X   ", "XX..X..X   ", "X....X..X  ",
        "X.....X..X ", "X......X   ", "X.......X  ", "X........X "
    };
    const int rows = 19;
    for (int j = 0; j < rows; j++) {
        const char* r = cur[j];
        for (int i = 0; r[i]; i++) {
            if (r[i] == ' ') break;
            const uint32_t c = (r[i] == 'X') ? rgb(0, 0, 0) : rgb(255, 255, 255);
            fb_putpixel(g_cur_x + i, g_cur_y + j, c);
        }
    }
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
    // 鼠标左键选择框（Win10 风格半透明矩形：桌面层、窗口之下）
    if (g_selbox_active) {
        const int sbx = sel_box_x(), sby = sel_box_y();
        const int sbw = sel_box_w(), sbh = sel_box_h();
        if (sbw > 0 && sbh > 0) {
            fb_fill_rect_alpha(sbx, sby, sbw, sbh, rgb(0, 128, 255), 40);
            fb_draw_rect(sbx, sby, sbw, sbh, rgb(64, 160, 255));
        }
    }
    // 窗口从底到顶画：先把 z 序反转
    Window* stack[MAX_WINS];
    int n = 0;
    for (Window* w = g_z; w && n < MAX_WINS; w = w->next) stack[n++] = w;
    for (int i = n - 1; i >= 0; i--) {
        Window* w = stack[i];
        if (!w->visible || w->minimized) continue;
        if (w->x >= x1 || w->y >= y1 || w->x + w->w <= x0 || w->y + w->h <= y0) continue;
        draw_window(w);
    }
    draw_taskbar();
    draw_menu();
    draw_cursor();
    fb_reset_clip();
    fb_flip_region(x0, y0, dw, dh);

    const uint64_t t1 = rdtsc64();
    g_busy_cycles += t1 - t0;
    g_total_cycles += t1 - t0 + 1;
    g_frames++;
    g_dirty_any = false;
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
    text_ttf(x, y, buf, rgb(40, 40, 40));
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

static Window* g_mypc_win = nullptr;
static void mypc_draw(Window* w) {
    const int x = w->client_x + 12;
    int y = w->client_y + 10;
    text_ttf(x, y, gui64_tr("Devices and drives", "设备和驱动器"), rgb(0, 60, 120));
    y += 26;
    // 磁盘（本内核没有 ATA 驱动，展示的是真实布局常量，不编造容量）
    fb_fill_rect(x, y, w->client_w - 24, 54, rgb(255, 255, 255));
    fb_draw_rect(x, y, w->client_w - 24, 54, rgb(180, 180, 180));
    text_ttf(x + 10, y + 8, gui64_tr("Local Disk (C:)", "本地磁盘 (C:)"), rgb(20, 20, 20));
    {
        char buf[96]; int n = 0;
        const char* s = "LBA 9..8008 kernel, 8009..8072 settings";
        for (int i = 0; s[i] && n < 90; i++) buf[n++] = s[i];
        buf[n] = 0;
        text_ttf(x + 10, y + 30, buf, rgb(90, 90, 90));
    }
    y += 64;
    fb_fill_rect(x, y, w->client_w - 24, 54, rgb(255, 255, 255));
    fb_draw_rect(x, y, w->client_w - 24, 54, rgb(180, 180, 180));
    text_ttf(x + 10, y + 8, gui64_tr("Install media (D:)", "安装介质 (D:)"), rgb(20, 20, 20));
    text_ttf(x + 10, y + 30, gui64_tr("payload at LBA 8192 (installer kernel only)",
                                     "载荷在 LBA 8192（仅安装程序内核可读）"), rgb(90, 90, 90));
    y += 66;
    text_ttf(x, y, gui64_tr("No filesystem driver yet (store/VFS not ported to 64-bit).",
                            "尚未有文件系统驱动（store/VFS 未移植到 64 位）。"), rgb(150, 60, 60));
}
void app_mypc_open64() {
    if (gui64_window_alive(g_mypc_win)) { gui64_set_active(g_mypc_win); return; }
    g_mypc_win = gui64_create_window(gui64_tr("My Computer", "我的电脑"),
                                     120, 80, 380, 300, mypc_draw, nullptr, nullptr, APP_ID_MYPC);
    if (g_mypc_win) gui64_set_min_size(g_mypc_win, 320, 260);
    dbg64_str("[APP] mypc opened");
    dbg64_nl();
}

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

static Window* g_rec_win = nullptr;
static void recycle_draw(Window* w) {
    text_ttf(w->client_x + 14, w->client_y + 14,
             gui64_tr("Recycle Bin is empty.", "回收站是空的。"), rgb(60, 60, 60));
}
void app_recycle_open64() {
    if (gui64_window_alive(g_rec_win)) { gui64_set_active(g_rec_win); return; }
    g_rec_win = gui64_create_window(gui64_tr("Recycle Bin", "回收站"),
                                    260, 120, 300, 180, recycle_draw, nullptr, nullptr, APP_ID_RECYCLE);
    if (g_rec_win) gui64_set_min_size(g_rec_win, 240, 150);
    dbg64_str("[APP] recycle opened");
    dbg64_nl();
}

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
    // 1) 任务栏
    const int ty = g_screen_h - TASKBAR_H;
    if (my >= ty) {
        g_menu_open = false;
        if (mx < 34) { menu_toggle(); return; }
        int bx = 34;
        for (Window* w = g_z; w; w = w->next) {
            if (!w->visible) continue;
            const int bw = text_w(w->title) + 16;
            if (bx + bw > g_screen_w - 90) break;
            if (mx >= bx && mx < bx + bw) {
                if (w->active && !w->minimized) { w->minimized = true; dirty_add(0, 0, g_screen_w, g_screen_h); }
                else { w->minimized = false; gui64_set_active(w); }
                dirty_add(0, ty, g_screen_w, TASKBAR_H);
                return;
            }
            bx += bw + 4;
        }
        return;
    }
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
            } else {                            // 最小化
                w->minimized = true;
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
        // 右下角 -> 缩放
        if (mx >= w->x + w->w - 14 && my >= w->y + w->h - 14) {
            g_drag = DRAG_RESIZE;
            g_drag_w = w;
            g_drag_dx = w->w - (mx - w->x);
            g_drag_dy = w->h - (my - w->y);
            return;
        }
        // 客户区 -> 交给应用
        press_in_client(w, mx, my, button);
        return;
    }
    // 4) 桌面图标（按下 = 选中 + 记录拖动基准；松开时区分 单击/双击/拖动）
    for (int i = 0; i < 3; i++) {
        const DeskIcon& ic = g_icons[i];
        if (mx >= ic.x - 4 && mx < ic.x + ICON_W + 4 && my >= ic.y - 4 && my < ic.y + ICON_W + 18) {
            g_icon_sel = i;
            g_icon_drag_idx = i;
            g_icon_drag_moved = false;
            g_icon_press_x = mx;
            g_icon_press_y = my;
            g_icon_press_icon_x = ic.x;
            g_icon_press_icon_y = ic.y;
            g_selbox_active = false;
            dbg64_str("[UI] desktop icon select kind=");
            dbg64_dec((uint64_t)i);
            dbg64_nl();
            dirty_add(ic.x - 6, ic.y - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            return;
        }
    }
    // 5) 桌面空白：取消选择；左键开始拉 Win10 风格选择框（桌面层、窗口之下）
    if (g_icon_sel >= 0) {
        const DeskIcon& old = g_icons[g_icon_sel];
        dirty_add(old.x - 6, old.y - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
        g_icon_sel = -1;
    }
    if (button == 0) {
        g_selbox_active = true;
        g_sel_x0 = mx; g_sel_y0 = my;
        g_sel_x1 = mx; g_sel_y1 = my;
    } else {
        g_selbox_active = false;
    }
    dirty_add(0, 0, g_screen_w, g_screen_h);
}

static void handle_mouse(void) {
    const int mx = mouse_get_x(), my = mouse_get_y();
    const int btn = (int)mouse_get_buttons();
    const bool rel_left = (g_prev_btn & 1) && !(btn & 1);   // 左键释放（边沿）
    // 光标移动 -> 新旧两块脏区（标题栏三按钮要跟着重画：hover 高亮）
    if (mx != g_cur_x || my != g_cur_y) {
        dirty_add(g_prev_cur_x, g_prev_cur_y, 12, 20);
        dirty_add(g_cur_x, g_cur_y, 12, 20);
        dirty_title_buttons_at(g_cur_x, g_cur_y);
        dirty_title_buttons_at(mx, my);
        g_prev_cur_x = g_cur_x; g_prev_cur_y = g_cur_y;
        g_cur_x = mx; g_cur_y = my;
        dirty_add(mx, my, 12, 20);
    }
    // 按下（边沿）
    if (mouse_button_pressed(0)) { mouse_consume_pressed(0); handle_mouse_press(mx, my, 0); }
    if (mouse_button_pressed(1)) { mouse_consume_pressed(1); handle_mouse_press(mx, my, 1); }
    if (mouse_button_pressed(2)) { mouse_consume_pressed(2); handle_mouse_press(mx, my, 2); }

    // 桌面图标拖动：位移 >5px（dx²+dy²>25）才算拖动，从而区分单击/双击/拖动（移植自 32 位）
    if ((btn & 1) && g_icon_drag_idx >= 0) {
        const int dx = mx - g_icon_press_x, dy = my - g_icon_press_y;
        if (!g_icon_drag_moved && dx * dx + dy * dy > 25) {
            g_icon_drag_moved = true;
            dbg64_str("[UI] icon drag idx=");
            dbg64_dec((uint64_t)g_icon_drag_idx);
            dbg64_str(" x=");
            dbg64_dec((uint64_t)g_icons[g_icon_drag_idx].x);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)g_icons[g_icon_drag_idx].y);
            dbg64_nl();
        }
        if (g_icon_drag_moved) {
            DeskIcon& ic = g_icons[g_icon_drag_idx];
            int nx = g_icon_press_icon_x + dx;
            int ny = g_icon_press_icon_y + dy;
            if (nx < 2) nx = 2;
            if (nx > g_screen_w - ICON_CELL_W - 2) nx = g_screen_w - ICON_CELL_W - 2;
            if (ny < 2) ny = 2;
            if (ny > g_screen_h - TASKBAR_H - ICON_CELL_H) ny = g_screen_h - TASKBAR_H - ICON_CELL_H;
            if (nx != ic.x || ny != ic.y) {
                dirty_add(ic.x - 6, ic.y - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
                ic.x = nx; ic.y = ny;
                dirty_add(ic.x - 6, ic.y - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            }
        }
    }
    // 空白处选择框：拖动更新矩形 + 实时选中框内图标（复用 g_icon_sel / C_ICON_SEL 绘制）
    if ((btn & 1) && g_selbox_active) {
        if (mx != g_sel_x1 || my != g_sel_y1) {
            dirty_add(sel_box_x() - 2, sel_box_y() - 2, sel_box_w() + 4, sel_box_h() + 4);
            g_sel_x1 = mx; g_sel_y1 = my;
            int sel = -1;
            for (int i = 0; i < 3; i++)
                if (icon_hits_sel(i, g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1)) sel = i;
            if (sel != g_icon_sel) {
                if (g_icon_sel >= 0)
                    dirty_add(g_icons[g_icon_sel].x - 6, g_icons[g_icon_sel].y - 6,
                              ICON_CELL_W + 12, ICON_CELL_H + 12);
                g_icon_sel = sel;
                if (g_icon_sel >= 0)
                    dirty_add(g_icons[g_icon_sel].x - 6, g_icons[g_icon_sel].y - 6,
                              ICON_CELL_W + 12, ICON_CELL_H + 12);
            }
            dirty_add(sel_box_x() - 2, sel_box_y() - 2, sel_box_w() + 4, sel_box_h() + 4);
        }
    }
    // 左键释放：结束图标拖动/选择框；位移未超阈值 = 单击（累计双击打开应用，不变）
    if (rel_left) {
        if (g_icon_drag_idx >= 0) {
            const int i = g_icon_drag_idx;
            if (!g_icon_drag_moved) {
                const uint32_t now = ticks64();
                const bool dbl = (g_icon_sel == i) && (now - g_icon_last_tick <= ms_to_ticks64(500));
                if (dbl) {
                    dbg64_str("[UI] desktop icon open kind=");
                    dbg64_dec((uint64_t)i);
                    dbg64_nl();
                    if (i == 0) app_mypc_open64();
                    else if (i == 1) app_recycle_open64();
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
                dbg64_dec((uint64_t)g_icons[i].x);
                dbg64_str(" y=");
                dbg64_dec((uint64_t)g_icons[i].y);
                dbg64_nl();
            }
            dirty_add(g_icons[i].x - 6, g_icons[i].y - 6, ICON_CELL_W + 12, ICON_CELL_H + 12);
            g_icon_drag_idx = -1;
            g_icon_drag_moved = false;
        }
        if (g_selbox_active) {
            const int x0 = sel_box_x(), y0 = sel_box_y();
            const int bw = sel_box_w(), bh = sel_box_h();
            int sel = 0;
            for (int i = 0; i < 3; i++)
                if (icon_hits_sel(i, g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1)) sel++;
            dbg64_str("[UI] selbox x0=");
            dbg64_dec((uint64_t)x0);
            dbg64_str(" y0=");
            dbg64_dec((uint64_t)y0);
            dbg64_str(" x1=");
            dbg64_dec((uint64_t)(x0 + bw));
            dbg64_str(" y1=");
            dbg64_dec((uint64_t)(y0 + bh));
            dbg64_str(" sel=");
            dbg64_dec((uint64_t)sel);
            dbg64_nl();
            g_selbox_active = false;
            dirty_add(x0 - 2, y0 - 2, bw + 4, bh + 4);
        }
    }
    // 拖拽窗口（保持原有语义：只有窗口拖动走这里）
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
                int nw = mx - w->x + g_drag_dx;
                int nh = my - w->y + g_drag_dy;
                if (nw < w->min_w) nw = w->min_w;
                if (nh < w->min_h) nh = w->min_h;
                if (w->x + nw > g_screen_w) nw = g_screen_w - w->x;
                if (w->y + nh > g_screen_h - TASKBAR_H) nh = g_screen_h - TASKBAR_H - w->y;
                if (nw > 0 && nh > 0) { w->w = nw; w->h = nh; recompute_client(w); }
            }
            dirty_add(w->x, w->y, w->w, w->h);
        } else {
            g_drag = DRAG_NONE;
            g_drag_w = nullptr;
        }
    }
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
    // 热键：Win -> 开始菜单
    if (kbd_win_pressed()) {
        kbd_consume_win();
        menu_toggle();
        return;
    }
    uint8_t c = 0;
    while (kbd_pop_char(&c)) {
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

// ==================== 主循环 ====================
[[noreturn]] void gui64_run(const BootInfo* bi) {
    (void)bi;
    g_screen_w = fb_width();
    g_screen_h = fb_height();
    mouse_set_bounds(g_screen_w, g_screen_h);
    for (int i = 0; i < MAX_WINS; i++) g_used[i] = false;
    g_z = nullptr;
    g_icon_sel = -1;
    g_icon_drag_idx = -1;
    g_icon_drag_moved = false;
    g_selbox_active = false;
    g_icons[0] = DeskIcon{0, 24,  24};
    g_icons[1] = DeskIcon{1, 24,  24 + ICON_CELL_H};
    g_icons[2] = DeskIcon{2, 24,  24 + ICON_CELL_H * 2};

    dbg64_str("[GUI64] desktop init ");
    dbg64_dec((uint64_t)g_screen_w);
    dbg64_str("x");
    dbg64_dec((uint64_t)g_screen_h);
    dbg64_str(" taskbar=");
    dbg64_dec((uint64_t)TASKBAR_H);
    dbg64_str(" title_h=");
    dbg64_dec((uint64_t)TITLE_H);
    dbg64_nl();

    // ---- 开机 logo：桌面首帧之前先放一段黑底 + 居中 logo 的淡入（~12 帧 ≈ 200ms）----
    boot_logo_fade_in();

    const int st = gui64_selftest();
    if (st != 0) {
        dbg64_str("[GUI64] selftest FAILED mask=");
        dbg64_dec((uint64_t)st);
        dbg64_nl();
    }

    fb_clear(C_DESKTOP);
    g_cur_x = mouse_get_x();
    g_cur_y = mouse_get_y();
    g_prev_cur_x = g_cur_x;
    g_prev_cur_y = g_cur_y;
    gui64_invalidate();
    render();

    // 这两行都是自动验收的断言行：
    //   "[GUI64] ready"      —— 桌面验收（tests/desktop64_test.py）
    //   "[OS] ready (idle)"  —— 历史断言（install_flow_test / vmware_install_test 依赖，勿删）
    dbg64_str("[GUI64] ready");
    dbg64_nl();
    dbg64_str("[OS] ready (idle)");
    dbg64_nl();

    uint32_t next_frame = ticks64();
    uint32_t next_tick = ticks64();
    uint32_t next_cpu_sample = ticks64() + ms_to_ticks64(500);
    uint32_t next_fps = ticks64() + ms_to_ticks64(1000);

    for (;;) {
        const uint32_t now = ticks64();
        handle_mouse();
        handle_keyboard();

        // 每 ~60Hz：跑应用的 tick 回调 + 重绘
        if ((int32_t)(now - next_frame) >= 0) {
            next_frame = now + (PIT_HZ_64 / 60);
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
                dirty_add(g_screen_w - CLOCK_W, g_screen_h - TASKBAR_H, CLOCK_W, TASKBAR_H);
                if (gui64_window_alive(g_mon_win)) gui64_invalidate_window(g_mon_win);
                if (gui64_window_alive(g_mypc_win)) gui64_invalidate_window(g_mypc_win);
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
