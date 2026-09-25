// desktopops64.cpp - ★ P5：桌面交互细节实现（右键菜单 / 玻璃选择框 / 回收站 / 桌面图标集合）
//   设计说明、打点格式、持久化键都在 desktopops64.h 顶部；这里只写实现。
//
// 几条硬约束（改之前先读）：
//   1) **所有颜色/圆角/阴影/时长都从 theme64 Token（+ p2ui 工具箱）取**，本文件不写死视觉数字；
//      唯一例外是"桌面图标单元格尺寸"（与 gui64.cpp 的 ICON_W/ICON_CELL 一致的那组布局常数，
//      它是外壳布局的一部分，不是视觉 Token）。
//   2) **兼容既有验收**：`[UI] selbox x0=...`（鼠标闭环定位）、`[UI] desktop icon select kind=`、
//      `[UI] desktop icon open kind=`、`[UI] icon drag idx=..`、`[CONF64] icon i moved to x,y`
//      这几行由本模块原样打出（gui_modern64_test 的闭环探测依赖第一行）。
//   3) 选择框**框内一个像素都不铺**（需求：框中间全透明，能看到被框选的图标/文件）。
#include "desktopops64.h"
#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "theme64.h"
#include "gfx64.h"
#include "startmenu64.h"     // P2 UI 工具箱（p2ui_*：亚克力/混合圆角/线性图标/文本）
#include "config64.h"
#include "debug64.h"
#include "x86_64.h"

// ---- 桌面图标单元格（与 gui64.cpp 的 ICON_W/ICON_CELL_W/ICON_CELL_H 一致）----
#define DOPS_ICON_W      48
#define DOPS_CELL_W      88
#define DOPS_CELL_H      84
#define DOPS_ICON_X0     24      // 默认位置（= config64 的 ui.icon*.x/y 默认值）
#define DOPS_ICON_Y0     24
#define DOPS_ICON_DY     84

// ==================== 状态 ====================
struct DeskItem { int kind; int x, y; };
static DeskItem g_items[DESKOPS_MAX_ITEMS];
static int      g_n = 0;
static int      g_recycle[DESKOPS_RECYCLE_MAX];
static int      g_rec_n = 0;
static char     g_set_cache[CFG64_STR_MAX];
static uint32_t g_tick_div = 0;
static int      g_rec_empty_logged = 0;      // "空回收站" 打点只在状态变化时打一次（防刷屏）

// 右键菜单
struct DeskMenuItem { unsigned char id; const char* en; const char* zh; unsigned char enabled; const char* why; };
static const DeskMenuItem kMenu[] = {
    { 0, "View",            "查看",      0, "no submenu in this batch"          },
    { 1, "Sort by",         "排序方式",  0, "no submenu in this batch"          },
    { 2, "Refresh",         "刷新",      1, nullptr                             },
    { 3, "New folder",      "新建文件夹", 1, nullptr                            },
    { 4, "Paste",           "粘贴",      0, "no clipboard in this batch"         },
    { 5, "Personalize",     "个性化",    1, nullptr                             },
    { 6, "Display settings","显示设置",  1, nullptr                             },
    { 7, "Open terminal",   "打开终端",  1, nullptr                             },
};
#define DOPS_MENU_N ((int)(sizeof(kMenu) / sizeof(kMenu[0])))
#define DOPS_MENU_ID_VIEW      0
#define DOPS_MENU_ID_SORT      1
#define DOPS_MENU_ID_REFRESH   2
#define DOPS_MENU_ID_NEWFOLDER 3
#define DOPS_MENU_ID_PASTE     4
#define DOPS_MENU_ID_PERSON    5
#define DOPS_MENU_ID_DISPLAY   6
#define DOPS_MENU_ID_TERM      7

static int  g_menu_open = 0;
static int  g_menu_x = 0, g_menu_y = 0, g_menu_w = 0, g_menu_h = 0;
static int  g_menu_hover = -1;
static int  g_menu_item_h = THEME64_POP_ROW;

// 玻璃选择框
static int  g_sel_active = 0;
static int  g_sel_x0 = 0, g_sel_y0 = 0, g_sel_x1 = 0, g_sel_y1 = 0;
static int  g_sel_sel = -1;
static int  g_sel_logged = 0;

// 回收站窗口
static Window* g_rec_win = nullptr;
static int  g_rec_sel = -1;
static int  g_rec_confirm = -1;              // >= 0 = 二次确认中的回收站项（**确认前不删**）

// ==================== 小工具（内核里没有 libc）====================
static bool s_eq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_cpy(char* d, const char* s, int cap) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}

// 图标名（按 kind；语言跟随外壳）
static const char* item_name(int kind) {
    if (kind == DESKOPS_KIND_MYPC)    return gui64_tr("My Computer", "我的电脑");
    if (kind == DESKOPS_KIND_RECYCLE) return gui64_tr("Recycle Bin", "回收站");
    if (kind == DESKOPS_KIND_TERM)    return gui64_tr("Terminal", "终端");
    return gui64_tr("New folder", "新建文件夹");
}

// ==================== 持久化（ui.desktop.icons = "桌面集合|回收站集合"）====================
static void set_cache_sync(void) {
    char buf[CFG64_STR_MAX];
    int n = 0;
    for (int i = 0; i < g_n && n < (int)sizeof(buf) - 3; i++) {
        if (i) buf[n++] = ',';
        buf[n++] = (char)('0' + (g_items[i].kind % 10));
    }
    buf[n++] = '|';
    for (int i = 0; i < g_rec_n && n < (int)sizeof(buf) - 3; i++) {
        if (i) buf[n++] = ',';
        buf[n++] = (char)('0' + (g_recycle[i] % 10));
    }
    buf[n] = 0;
    s_cpy(g_set_cache, buf, (int)sizeof(g_set_cache));
    cfg64_set_desktop_icons64(buf);
}
static void set_cache_only(void) {
    char buf[CFG64_STR_MAX];
    int n = 0;
    for (int i = 0; i < g_n && n < (int)sizeof(buf) - 3; i++) { if (i) buf[n++] = ','; buf[n++] = (char)('0' + (g_items[i].kind % 10)); }
    buf[n++] = '|';
    for (int i = 0; i < g_rec_n && n < (int)sizeof(buf) - 3; i++) { if (i) buf[n++] = ','; buf[n++] = (char)('0' + (g_recycle[i] % 10)); }
    buf[n] = 0;
    s_cpy(g_set_cache, buf, (int)sizeof(g_set_cache));
}

// 项的默认位置（内置 3 项用既有 ui.icon*.x/y；文件夹按槽位排）
static void item_default_pos(int kind, int* x, int* y) {
    if (kind >= 0 && kind <= 2) { *x = cfg64_icon_x64(kind); *y = cfg64_icon_y64(kind); }
    else { *x = DOPS_ICON_X0; *y = DOPS_ICON_Y0 + kind * DOPS_ICON_DY; }
    const int sw = gui64_screen_w(), sh = gui64_screen_h();
    if (sw > 0 && sh > 0) {
        if (*x > sw - DOPS_CELL_W - 2) *x = sw - DOPS_CELL_W - 2;
        if (*y > sh - gui64_taskbar_h() - DOPS_CELL_H) *y = sh - gui64_taskbar_h() - DOPS_CELL_H;
    }
    if (*x < 2) *x = 2;
    if (*y < 2) *y = 2;
}

// 解析 "a,b|c,d" 并重建集合（不写回配置；写回只发生在用户动作时）
static void apply_set(const char* s) {
    int wd[DESKOPS_MAX_ITEMS], nd = 0;
    int wr[DESKOPS_RECYCLE_MAX], nr = 0;
    int i = 0;
    bool in_bin = false;
    while (s && s[i]) {
        const char c = s[i++];
        if (c == '|') { in_bin = true; continue; }
        if (c == ',') continue;
        if (c < '0' || c > '9') continue;
        const int k = c - '0';
        if (in_bin) { if (nr < DESKOPS_RECYCLE_MAX) wr[nr++] = k; }
        else         { if (nd < DESKOPS_MAX_ITEMS) wd[nd++] = k; }
    }
    g_n = 0;
    for (int a = 0; a < nd; a++) {
        if (wd[a] > 7) continue;
        DeskItem it{};
        it.kind = wd[a];
        item_default_pos(it.kind, &it.x, &it.y);
        g_items[g_n++] = it;
    }
    g_rec_n = 0;
    for (int a = 0; a < nr; a++) {
        if (wr[a] > 7) continue;
        g_recycle[g_rec_n++] = wr[a];
    }
    set_cache_only();
}

static void log_items(void) {
    for (int i = 0; i < g_n; i++) {
        dbg64_line_begin64();
        dbg64_str("[DESK64] item idx=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" kind=");
        dbg64_dec((uint64_t)g_items[i].kind);
        dbg64_str(" name=");
        dbg64_str(item_name(g_items[i].kind));
        dbg64_str(" x=");
        dbg64_dec((uint64_t)g_items[i].x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)g_items[i].y);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)DOPS_ICON_W);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)DOPS_ICON_W);
        dbg64_str(" cell=");
        dbg64_dec((uint64_t)DOPS_CELL_W);
        dbg64_str("x");
        dbg64_dec((uint64_t)DOPS_CELL_H);
        dbg64_nl();
        dbg64_line_end64();
    }
}

static void log_set_line(const char* tag) {
    dbg64_line_begin64();
    dbg64_str(tag);
    dbg64_str(" items=");
    dbg64_dec((uint64_t)g_n);
    dbg64_str(" set=");
    dbg64_str(g_set_cache);
    dbg64_str(" recycle=");
    dbg64_dec((uint64_t)g_rec_n);
    dbg64_nl();
    dbg64_line_end64();
}

void desktopops64_init64() {
    char buf[CFG64_STR_MAX];
    cfg64_desktop_icons64(buf, (int)sizeof(buf));
    apply_set(buf);
    log_set_line("[DESK64] init");
    log_items();
}

// 每帧（约 4Hz 抽查）：终端 `cfg set ui.desktop.icons 1,2` / 设置页按钮改了配置 -> 实时同步桌面
int desktopops64_tick64() {
    if (++g_tick_div < 15) return 0;
    g_tick_div = 0;
    char buf[CFG64_STR_MAX];
    cfg64_desktop_icons64(buf, (int)sizeof(buf));
    if (s_eq(buf, g_set_cache)) return 0;
    apply_set(buf);
    log_set_line("[DESK64] set changed");
    return 1;
}

// ==================== 桌面项访问 ====================
int desktopops64_count64() { return g_n; }
int desktopops64_kind64(int i) { return (i >= 0 && i < g_n) ? g_items[i].kind : -1; }
const char* desktopops64_name64(int i) { return (i >= 0 && i < g_n) ? item_name(g_items[i].kind) : "?"; }
void desktopops64_pos64(int i, int* x, int* y) {
    if (i < 0 || i >= g_n) { if (x) *x = 0; if (y) *y = 0; return; }
    if (x) *x = g_items[i].x;
    if (y) *y = g_items[i].y;
}
int desktopops64_sel_target64(int i) { return (i >= 0 && i < g_n) ? g_items[i].kind : -1; }

void desktopops64_set_pos64(int i, int x, int y) {
    if (i < 0 || i >= g_n) return;
    g_items[i].x = x; g_items[i].y = y;
    if (g_items[i].kind <= 2) cfg64_set_icon64(g_items[i].kind, x, y);   // 既有键（跨重启）
}

void desktopops64_clamp64(int x, int y) {
    for (int i = 0; i < g_n; i++) {
        if (g_items[i].x < 2) g_items[i].x = 2;
        if (g_items[i].y < 2) g_items[i].y = 2;
        if (g_items[i].x > x - DOPS_CELL_W - 2) g_items[i].x = x - DOPS_CELL_W - 2;
        if (g_items[i].y > y - 2 - DOPS_CELL_H) g_items[i].y = y - 2 - DOPS_CELL_H;
    }
}

int desktopops64_hit64(int mx, int my) {
    for (int i = 0; i < g_n; i++) {
        const int x = g_items[i].x, y = g_items[i].y;
        if (mx >= x - 4 && mx < x + DOPS_ICON_W + 4 && my >= y - 4 && my < y + DOPS_ICON_W + 18) return i;
    }
    return -1;
}
int desktopops64_is_recycle_hit64(int mx, int my) {
    const int i = desktopops64_hit64(mx, my);
    return (i >= 0 && g_items[i].kind == DESKOPS_KIND_RECYCLE) ? i : -1;
}

// 命中某矩形（框选判定；与 gui64 的 icon_hits_sel 同语义：图标格 + 名字标签）
int desktopops64_rect_hit64(int i, int x0, int y0, int x1, int y1) {
    if (i < 0 || i >= g_n) return 0;
    if (x0 > x1) { const int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { const int t = y0; y0 = y1; y1 = t; }
    const int ix0 = g_items[i].x - 6, iy0 = g_items[i].y - 6;
    const int ix1 = ix0 + DOPS_CELL_W, iy1 = iy0 + DOPS_CELL_H;
    return !(ix1 < x0 || ix0 > x1 || iy1 < y0 || iy0 > y1) ? 1 : 0;
}

// ==================== 桌面项绘制（简约圆角正方形底板 + 位图/文件夹）====================
static void draw_folder_glyph64(int x, int y, int size, const Theme64Tokens* t) {
    // 简约圆角正方形底板（与 Dock 图标一致：主题渐变 + 1px 高光边）
    gfx64_grad_round64(x, y, size, size, THEME64_R_ICON, t->grad_a, t->grad_b, 1, 255);
    gfx64_stroke_round64(x, y, size, size, THEME64_R_ICON, rgb(255, 255, 255), THEME64_A_EDGE);
    // 文件夹几何：外框 + 顶边小台阶（画进底板内）
    const int inset = size / 6;
    const int fx = x + inset, fy = y + inset + size / 8;
    const int fw = size - inset * 2, fh = size - inset * 2 - size / 8;
    const int tab = fw / 3;
    gfx64_fill_round64(fx, fy - size / 10, tab, size / 10 + 2, 2, t->accent, 220);
    gfx64_fill_round64(fx, fy, fw, fh, THEME64_R_ICON / 2, t->accent2, 235);
    gfx64_stroke_round64(fx, fy, fw, fh, THEME64_R_ICON / 2, rgb(255, 255, 255), THEME64_A_EDGE);
}

void desktopops64_draw_icons64(int selected) {
    const Theme64Tokens* t = theme64_tokens64();
    for (int i = 0; i < g_n; i++) {
        const int x = g_items[i].x, y = g_items[i].y;
        const int k = g_items[i].kind;
        if (selected == i)
            gfx64_fill_round64(x - 4, y - 4, DOPS_ICON_W + 8, DOPS_ICON_W + 20, THEME64_R_ICON, t->sel_bg, 255);
        if (k <= 2) {
            // 软件快捷方式：**简约圆角正方形**底板（主题渐变 + 1px 高光边）+ 真图标位图居中
            gfx64_grad_round64(x, y, DOPS_ICON_W, DOPS_ICON_W, THEME64_R_ICON, t->grad_a, t->grad_b, 1, 255);
            gui64_draw_icon_kind64(x + 4, y + 4, k);     // 40x40 位图落在 48 的圆角底板内
            gfx64_stroke_round64(x, y, DOPS_ICON_W, DOPS_ICON_W, THEME64_R_ICON, rgb(255, 255, 255), THEME64_A_EDGE);
        } else {
            draw_folder_glyph64(x, y, DOPS_ICON_W, t);
        }
        const char* nm = item_name(k);
        const int tw = p2ui_text_w64(nm);
        int tx = x + (DOPS_ICON_W - tw) / 2;
        if (tx < 0) tx = 0;
        const uint32_t plate = t->dark ? rgb(0, 0, 0) : rgb(255, 255, 255);
        gfx64_fill_round64(x + (DOPS_ICON_W - tw) / 2 - 4, y + DOPS_ICON_W + 1, tw + 8, 18,
                           THEME64_R_BUTTON, plate, 150);
        p2ui_text64(tx, y + DOPS_ICON_W + 3, nm, t->icon_txt);
    }
}

// ==================== 恢复默认桌面图标 ====================
void desktopops64_restore_defaults64(const char* why) {
    g_n = 0;
    for (int k = 0; k <= 2; k++) {
        DeskItem it{};
        it.kind = k;
        it.x = DOPS_ICON_X0;
        it.y = DOPS_ICON_Y0 + k * DOPS_ICON_DY;
        g_items[g_n++] = it;
        cfg64_set_icon64(k, it.x, it.y);            // 位置键也回到默认
    }
    g_rec_n = 0;
    g_rec_confirm = -1;
    g_rec_empty_logged = 0;
    set_cache_sync();
    log_set_line("[DESK64] icons reset");
    dbg64_line_begin64();
    dbg64_str("[DESK64] icons reset why=");
    dbg64_str(why ? why : "-");
    dbg64_str(" defaults=3 persisted=1 via=ui.desktop.icons");
    dbg64_nl();
    dbg64_line_end64();
    gui64_invalidate();
}

// ==================== 回收站 ====================
int desktopops64_recycle_count64() { return g_rec_n; }

static void rec_log_count(const char* tag) {
    dbg64_line_begin64();
    dbg64_str(tag);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)g_rec_n);
    dbg64_str(" desktop=");
    dbg64_dec((uint64_t)g_n);
    dbg64_nl();
    dbg64_line_end64();
}

int desktopops64_recycle_add64(int icon_idx) {
    if (icon_idx < 0 || icon_idx >= g_n) return 0;
    const int kind = g_items[icon_idx].kind;
    if (kind == DESKOPS_KIND_RECYCLE) return 0;                 // 回收站自己不能被拖进去
    if (g_rec_n >= DESKOPS_RECYCLE_MAX) return 0;
    g_recycle[g_rec_n++] = kind;
    for (int i = icon_idx; i + 1 < g_n; i++) g_items[i] = g_items[i + 1];
    g_n--;
    g_rec_confirm = -1;
    set_cache_sync();
    dbg64_line_begin64();
    dbg64_str("[RECYCLE64] add kind=");
    dbg64_dec((uint64_t)kind);
    dbg64_str(" name=");
    dbg64_str(item_name(kind));
    dbg64_str(" n=");
    dbg64_dec((uint64_t)g_rec_n);
    dbg64_str(" desktop=");
    dbg64_dec((uint64_t)g_n);
    dbg64_str(" persisted=1");
    dbg64_nl();
    dbg64_line_end64();
    gui64_invalidate();
    return 1;
}

static void rec_restore(int i) {
    if (i < 0 || i >= g_rec_n) return;
    const int kind = g_recycle[i];
    for (int k = i; k + 1 < g_rec_n; k++) g_recycle[k] = g_recycle[k + 1];
    g_rec_n--;
    if (g_n < DESKOPS_MAX_ITEMS) {
        DeskItem it{};
        it.kind = kind;
        item_default_pos(kind, &it.x, &it.y);
        g_items[g_n++] = it;
    }
    g_rec_confirm = -1;
    set_cache_sync();
    dbg64_line_begin64();
    dbg64_str("[RECYCLE64] restore kind=");
    dbg64_dec((uint64_t)kind);
    dbg64_str(" name=");
    dbg64_str(item_name(kind));
    dbg64_str(" n=");
    dbg64_dec((uint64_t)g_rec_n);
    dbg64_str(" desktop=");
    dbg64_dec((uint64_t)g_n);
    dbg64_nl();
    dbg64_line_end64();
    gui64_invalidate();
}

static void rec_delete(int i) {
    if (i < 0 || i >= g_rec_n) return;
    const int kind = g_recycle[i];
    for (int k = i; k + 1 < g_rec_n; k++) g_recycle[k] = g_recycle[k + 1];
    g_rec_n--;
    g_rec_confirm = -1;
    if (g_rec_sel >= g_rec_n) g_rec_sel = g_rec_n - 1;
    set_cache_sync();
    dbg64_line_begin64();
    dbg64_str("[RECYCLE64] delete done kind=");
    dbg64_dec((uint64_t)kind);
    dbg64_str(" name=");
    dbg64_str(item_name(kind));
    dbg64_str(" n=");
    dbg64_dec((uint64_t)g_rec_n);
    dbg64_str(" permanent=1");
    dbg64_nl();
    dbg64_line_end64();
    gui64_invalidate();
}

static void rec_empty(void) {
    g_rec_n = 0;
    g_rec_sel = -1;
    g_rec_confirm = -1;
    set_cache_sync();
    dbg64_str("[RECYCLE64] delete done empty-all permanent=1 n=0");
    dbg64_nl();
    gui64_invalidate();
}

// ---- 回收站窗口几何/绘制 ----
#define REC_ROW_H  26
static void rec_btn_rect(const Window* w, int idx, int* x, int* y, int* bw, int* bh) {
    const char* lbl0 = gui64_tr("Restore", "恢复");
    const char* lbl1 = gui64_tr("Delete permanently", "永久删除");
    const char* lbl2 = gui64_tr("Empty", "清空");
    const char* lb[3] = { lbl0, lbl1, lbl2 };
    int total = 0;
    int widths[3];
    for (int i = 0; i < 3; i++) {
        widths[i] = p2ui_text_w64(lb[i]) + THEME64_POP_PAD * 2;
        total += widths[i] + THEME64_POP_GAP;
    }
    total -= THEME64_POP_GAP;
    int bx = w->client_x + w->client_w - THEME64_POP_PAD - total;
    if (bx < w->client_x + THEME64_POP_PAD) bx = w->client_x + THEME64_POP_PAD;
    for (int i = 0; i < 3; i++) {
        if (i == idx) {
            *x = bx; *y = w->client_y + w->client_h - THEME64_POP_BTN_H - THEME64_POP_PAD;
            *bw = widths[i]; *bh = THEME64_POP_BTN_H;
            return;
        }
        bx += widths[i] + THEME64_POP_GAP;
    }
    *x = 0; *y = 0; *bw = 0; *bh = 0;
}
static void rec_confirm_rect(const Window* w, int idx, int* x, int* y, int* bw, int* bh) {
    const int cw = 320, ch = 130;
    const int cx = w->client_x + (w->client_w - cw) / 2;
    const int cy = w->client_y + (w->client_h - ch) / 2;
    const int bwd = 110, bht = THEME64_POP_BTN_H;
    if (idx == 0) { *x = cx + cw - bwd * 2 - THEME64_POP_GAP - THEME64_POP_PAD; *y = cy + ch - bht - THEME64_POP_PAD; }
    else          { *x = cx + cw - bwd - THEME64_POP_PAD; *y = cy + ch - bht - THEME64_POP_PAD; }
    *bw = bwd; *bh = bht;
}
// 二次确认对话框的按钮矩形打点（自动验收要点"确认/取消"）
static void rec_log_confirm(const Window* w, int which) {
    int c0x = 0, c0y = 0, c0w = 0, c0h = 0, c1x = 0, c1y = 0, c1w = 0, c1h = 0;
    rec_confirm_rect(w, 0, &c0x, &c0y, &c0w, &c0h);
    rec_confirm_rect(w, 1, &c1x, &c1y, &c1w, &c1h);
    dbg64_line_begin64();
    dbg64_str("[RECYCLE64] confirm dialog which=");
    dbg64_dec((uint64_t)which);
    dbg64_str(" confirm_btn=");
    dbg64_dec((uint64_t)c0x); dbg64_str(","); dbg64_dec((uint64_t)c0y);
    dbg64_str("-"); dbg64_dec((uint64_t)c0w); dbg64_str("x"); dbg64_dec((uint64_t)c0h);
    dbg64_str(" cancel_btn=");
    dbg64_dec((uint64_t)c1x); dbg64_str(","); dbg64_dec((uint64_t)c1y);
    dbg64_str("-"); dbg64_dec((uint64_t)c1w); dbg64_str("x"); dbg64_dec((uint64_t)c1h);
    dbg64_str(" two_step=1 (nothing deleted before confirm)");
    dbg64_nl();
    dbg64_line_end64();
}
static void rec_row_rect(const Window* w, int i, int* x, int* y, int* bw, int* bh) {
    *x = w->client_x + 8;
    *y = w->client_y + 34 + i * REC_ROW_H;
    *bw = w->client_w - 16;
    *bh = REC_ROW_H - 2;
}

static void rec_draw(Window* w) {
    const Theme64Tokens* t = theme64_tokens64();
    const bool zh = gui64_lang_zh();
    p2ui_text64(w->client_x + 12, w->client_y + 8,
                zh ? "回收站" : "Recycle Bin", t->text);
    {
        char b[48]; int n = 0;
        b[n++] = '(';
        b[n++] = (char)('0' + (g_rec_n / 10) % 10);
        b[n++] = (char)('0' + g_rec_n % 10);
        b[n++] = ')';
        b[n] = 0;
        p2ui_text64(w->client_x + 12 + p2ui_text_w64(zh ? "回收站" : "Recycle Bin") + 6, w->client_y + 8, b, t->text_dim);
    }
    if (g_rec_n == 0) {
        // 空状态（需求：空回收站要有空状态）
        p2ui_text64(w->client_x + 14, w->client_y + 46,
                    gui64_tr("Recycle Bin is empty.", "回收站是空的。"), t->text_dim);
        p2ui_text64(w->client_x + 14, w->client_y + 46 + p2ui_line_h64() + 4,
                    gui64_tr("Drag desktop shortcuts here to delete them.",
                             "把桌面快捷方式拖进来即可删除（可恢复）。"), t->text_dim);
        if (!g_rec_empty_logged) {
            g_rec_empty_logged = 1;
            dbg64_str("[RECYCLE64] empty (recycle bin is empty)");
            dbg64_nl();
        }
    } else {
        g_rec_empty_logged = 0;
        for (int i = 0; i < g_rec_n; i++) {
            int x, y, bw, bh;
            rec_row_rect(w, i, &x, &y, &bw, &bh);
            if (i == g_rec_sel) gfx64_fill_round64(x - 2, y - 1, bw + 4, bh + 2, THEME64_R_BUTTON, t->sel_bg, 90);
            p2ui_text64(x + 4, y + (bh - p2ui_line_h64()) / 2, item_name(g_recycle[i]), t->text);
            p2ui_text64(x + bw - 120, y + (bh - p2ui_line_h64()) / 2,
                        gui64_tr("shortcut", "快捷方式"), t->text_dim);
        }
    }
    // 底部按钮
    const char* lb[3] = { gui64_tr("Restore", "恢复"), gui64_tr("Delete permanently", "永久删除"),
                          gui64_tr("Empty", "清空") };
    for (int i = 0; i < 3; i++) {
        int x, y, bw, bh;
        rec_btn_rect(w, i, &x, &y, &bw, &bh);
        if (bw > 0) {
            const uint32_t bg = (i == 1) ? t->btn_close : t->btn_bg;
            gfx64_fill_round64(x, y, bw, bh, THEME64_R_BUTTON, bg, 235);
            p2ui_text64(x + (bw - p2ui_text_w64(lb[i])) / 2, y + (bh - p2ui_line_h64()) / 2, lb[i], t->btn_glyph);
        }
    }
    // 二次确认（永久删除）
    if (g_rec_confirm != -1) {
        const int cw = 320, ch = 130;
        const int cx = w->client_x + (w->client_w - cw) / 2;
        const int cy = w->client_y + (w->client_h - ch) / 2;
        p2ui_popup64(cx, cy, cw, ch, THEME64_POP_R, THEME64_POP_R, t, t->client_bg, THEME64_A_POP);
        p2ui_text64(cx + 16, cy + 14, gui64_tr("Delete permanently?", "永久删除？"), t->text);
        p2ui_text64(cx + 16, cy + 14 + p2ui_line_h64() + 6,
                    gui64_tr("This cannot be undone.", "删除后不可恢复。"), t->text_dim);
        const char* lbl[2] = { gui64_tr("Confirm", "确认"), gui64_tr("Cancel", "取消") };
        for (int i = 0; i < 2; i++) {
            int x, y, bw, bh;
            rec_confirm_rect(w, i, &x, &y, &bw, &bh);
            gfx64_fill_round64(x, y, bw, bh, THEME64_R_BUTTON, i == 0 ? t->btn_close : t->btn_bg, 240);
            p2ui_text64(x + (bw - p2ui_text_w64(lbl[i])) / 2, y + (bh - p2ui_line_h64()) / 2, lbl[i], t->btn_glyph);
        }
    }
}

static void rec_click(Window* w, int cx, int cy) {
    const int mx = w->client_x + cx, my = w->client_y + cy;
    if (g_rec_confirm != -1) {
        for (int i = 0; i < 2; i++) {
            int x, y, bw, bh;
            rec_confirm_rect(w, i, &x, &y, &bw, &bh);
            if (mx >= x && mx < x + bw && my >= y && my < y + bh) {
                if (i == 0) { if (g_rec_confirm == -2) rec_empty(); else rec_delete(g_rec_confirm); }
                else {
                    dbg64_line_begin64();
                    dbg64_str("[RECYCLE64] delete ask cancel idx=");
                    dbg64_dec((uint64_t)g_rec_confirm);
                    dbg64_str(" n=");
                    dbg64_dec((uint64_t)g_rec_n);
                    dbg64_nl();
                    dbg64_line_end64();
                    g_rec_confirm = -1;
                }
                gui64_invalidate_window(w);
                return;
            }
        }
        return;     // 确认框是模态：其余点击先不处理
    }
    for (int i = 0; i < g_rec_n; i++) {
        int x, y, bw, bh;
        rec_row_rect(w, i, &x, &y, &bw, &bh);
        if (mx >= x && mx < x + bw && my >= y && my < y + bh) {
            g_rec_sel = i;
            dbg64_line_begin64();
            dbg64_str("[RECYCLE64] select idx=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" kind=");
            dbg64_dec((uint64_t)g_recycle[i]);
            dbg64_nl();
            dbg64_line_end64();
            gui64_invalidate_window(w);
            return;
        }
    }
    for (int i = 0; i < 3; i++) {
        int x, y, bw, bh;
        rec_btn_rect(w, i, &x, &y, &bw, &bh);
        if (bw > 0 && mx >= x && mx < x + bw && my >= y && my < y + bh) {
            if (i == 0) {                       // 恢复
                const int s = (g_rec_sel >= 0) ? g_rec_sel : 0;
                if (g_rec_n > 0) rec_restore(s);
            } else if (i == 1) {                // 永久删除 -> 先二次确认（确认前不删）
                const int s = (g_rec_sel >= 0) ? g_rec_sel : 0;
                if (g_rec_n > 0) {
                    g_rec_confirm = s;
                    dbg64_line_begin64();
                    dbg64_str("[RECYCLE64] delete ask kind=");
                    dbg64_dec((uint64_t)g_recycle[s]);
                    dbg64_str(" name=");
                    dbg64_str(item_name(g_recycle[s]));
                    dbg64_str(" confirm=1");
                    dbg64_nl();
                    dbg64_line_end64();
                    rec_log_confirm(w, 1);
                }
            } else if (g_rec_n > 0) {            // 清空（同样是永久删除，先确认）
                g_rec_confirm = -2;              // -2 = 清空全部待确认
                dbg64_str("[RECYCLE64] delete ask empty-all confirm=1");
                dbg64_nl();
                rec_log_confirm(w, 2);
            }
            gui64_invalidate_window(w);
            return;
        }
    }
}

static void rec_key(Window* w, char c) {
    (void)w;
    if (c == 0x1B) {
        if (g_rec_confirm != -1) { g_rec_confirm = -1; gui64_invalidate_window(g_rec_win); }
    }
}

void desktopops64_recycle_open64() {
    if (gui64_window_alive(g_rec_win)) { gui64_set_active(g_rec_win); return; }
    const int sw = gui64_screen_w(), sh = gui64_screen_h();
    int w = 460, h = 300;
    if (w > sw - 40) w = sw - 40;
    if (h > sh - gui64_taskbar_h() - 40) h = sh - gui64_taskbar_h() - 40;
    g_rec_win = gui64_create_window(gui64_tr("Recycle Bin", "回收站"),
                                    (sw - w) / 2, (sh - h) / 2 - 20, w, h,
                                    rec_draw, rec_key, rec_click, APP_ID_RECYCLE);
    if (!g_rec_win) return;
    gui64_set_min_size(g_rec_win, 360, 220);
    g_rec_sel = g_rec_n > 0 ? 0 : -1;
    g_rec_confirm = -1;
    dbg64_str("[APP] recycle opened");
    dbg64_nl();
    rec_log_count("[RECYCLE64] count");
    // ★ 回收站窗口几何 + 按钮矩形（自动验收要点按钮；坐标都是屏幕绝对坐标）
    {
        dbg64_line_begin64();
        dbg64_str("[RECYCLE64] geom x=");
        dbg64_dec((uint64_t)g_rec_win->x);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)g_rec_win->y);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)g_rec_win->w);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)g_rec_win->h);
        dbg64_str(" client=");
        dbg64_dec((uint64_t)g_rec_win->client_w);
        dbg64_str("x");
        dbg64_dec((uint64_t)g_rec_win->client_h);
        dbg64_str(" row_h=");
        dbg64_dec((uint64_t)REC_ROW_H);
        dbg64_nl();
        dbg64_line_end64();
        static const char* nm[3] = { "restore", "delete", "empty" };
        for (int i = 0; i < 3; i++) {
            int bx = 0, by = 0, bw = 0, bh = 0;
            rec_btn_rect(g_rec_win, i, &bx, &by, &bw, &bh);
            dbg64_line_begin64();
            dbg64_str("[RECYCLE64] btn idx=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" name=");
            dbg64_str(nm[i]);
            dbg64_str(" x=");
            dbg64_dec((uint64_t)bx);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)by);
            dbg64_str(" w=");
            dbg64_dec((uint64_t)bw);
            dbg64_str(" h=");
            dbg64_dec((uint64_t)bh);
            dbg64_nl();
            dbg64_line_end64();
        }
        for (int i = 0; i < g_rec_n && i < DESKOPS_RECYCLE_MAX; i++) {
            int bx = 0, by = 0, bw = 0, bh = 0;
            rec_row_rect(g_rec_win, i, &bx, &by, &bw, &bh);
            dbg64_line_begin64();
            dbg64_str("[RECYCLE64] row idx=");
            dbg64_dec((uint64_t)i);
            dbg64_str(" x=");
            dbg64_dec((uint64_t)bx);
            dbg64_str(" y=");
            dbg64_dec((uint64_t)by);
            dbg64_str(" w=");
            dbg64_dec((uint64_t)bw);
            dbg64_str(" h=");
            dbg64_dec((uint64_t)bh);
            dbg64_nl();
            dbg64_line_end64();
        }
    }
}
void desktopops64_recycle_reset64() {
    if (gui64_window_alive(g_rec_win)) gui64_destroy_window(g_rec_win);
    g_rec_win = nullptr;
    g_rec_sel = -1;
    g_rec_confirm = -1;
    dbg64_str("[APP] recycle closed");
    dbg64_nl();
}

// ==================== 桌面右键菜单 ====================
static void menu_measure(void) {
    int w = 0;
    for (int i = 0; i < DOPS_MENU_N; i++) {
        const int tw = p2ui_text_w64(gui64_lang_zh() ? kMenu[i].zh : kMenu[i].en);
        if (tw > w) w = tw;
    }
    g_menu_w = w + THEME64_POP_PAD * 2 + 28;      // 28 = 左侧图标列（Token 组合出来的）
    if (g_menu_w < 180) g_menu_w = 180;
    if (g_menu_w > 320) g_menu_w = 320;
    g_menu_h = DOPS_MENU_N * g_menu_item_h + THEME64_POP_PAD;
}

void desktopops64_menu_geom64(int* x, int* y, int* w, int* h) {
    if (x) *x = g_menu_x;
    if (y) *y = g_menu_y;
    if (w) *w = g_menu_w;
    if (h) *h = g_menu_h;
}
int desktopops64_menu_is_open64() { return g_menu_open; }
int desktopops64_menu_items64() { return DOPS_MENU_N; }

void desktopops64_menu_open64(int mx, int my) {
    menu_measure();
    const int sw = gui64_screen_w(), sh = gui64_screen_h() - gui64_taskbar_h();
    int x = mx, y = my;
    if (x + g_menu_w > sw - 4) x = sw - 4 - g_menu_w;
    if (y + g_menu_h > sh - 4) y = sh - 4 - g_menu_h;
    if (x < 4) x = 4;
    if (y < 4) y = 4;
    g_menu_x = x; g_menu_y = y;
    g_menu_open = 1;
    g_menu_hover = -1;
    gui64_invalidate();
    dbg64_line_begin64();
    dbg64_str("[DESK64] menu open x=");
    dbg64_dec((uint64_t)g_menu_x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)g_menu_y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)g_menu_w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)g_menu_h);
    dbg64_str(" items=");
    dbg64_dec((uint64_t)DOPS_MENU_N);
    {
        int en = 0;
        for (int i = 0; i < DOPS_MENU_N; i++) if (kMenu[i].enabled) en++;
        dbg64_str(" enabled=");
        dbg64_dec((uint64_t)en);
    }
    dbg64_str(" row=");
    dbg64_dec((uint64_t)g_menu_item_h);
    dbg64_str(" r=");
    dbg64_dec((uint64_t)THEME64_POP_R);
    dbg64_str(" shadow=2 edge=1 (acrylic)");
    dbg64_nl();
    dbg64_line_end64();
    for (int i = 0; i < DOPS_MENU_N; i++) {
        dbg64_line_begin64();
        dbg64_str("[DESK64] menu item idx=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" id=");
        dbg64_dec((uint64_t)kMenu[i].id);
        dbg64_str(" name=");
        dbg64_str(gui64_lang_zh() ? kMenu[i].zh : kMenu[i].en);
        dbg64_str(" enabled=");
        dbg64_dec((uint64_t)kMenu[i].enabled);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)(g_menu_y + THEME64_POP_PAD / 2 + i * g_menu_item_h));
        dbg64_str(" h=");
        dbg64_dec((uint64_t)g_menu_item_h);
        if (!kMenu[i].enabled) { dbg64_str(" why="); dbg64_str(kMenu[i].why ? kMenu[i].why : "n/a"); }
        dbg64_nl();
        dbg64_line_end64();
    }
}

void desktopops64_menu_close64(const char* why) {
    if (!g_menu_open) return;
    g_menu_open = 0;
    g_menu_hover = -1;
    gui64_invalidate();
    dbg64_line_begin64();
    dbg64_str("[DESK64] menu close why=");
    dbg64_str(why ? why : "-");
    dbg64_nl();
    dbg64_line_end64();
}

void desktopops64_menu_move64(int mx, int my) {
    if (!g_menu_open) return;
    int hov = -1;
    if (mx >= g_menu_x && mx < g_menu_x + g_menu_w && my >= g_menu_y && my < g_menu_y + g_menu_h) {
        hov = (my - g_menu_y - THEME64_POP_PAD / 2) / g_menu_item_h;
        if (hov < 0 || hov >= DOPS_MENU_N) hov = -1;
        else if (!kMenu[hov].enabled) hov = -1;      // 置灰项不参与高亮
    }
    if (hov != g_menu_hover) {
        g_menu_hover = hov;
        gui64_dirty(g_menu_x, g_menu_y, g_menu_w, g_menu_h);
    }
}

static void menu_action(int idx) {
    const DeskMenuItem& it = kMenu[idx];
    const char* done = "1";
    const char* why = "";
    if (!it.enabled) {
        done = "0";
        why = it.why ? it.why : "disabled";
    } else {
        switch (it.id) {
            case DOPS_MENU_ID_REFRESH:
                gui64_invalidate();
                why = "desktop repainted";
                break;
            case DOPS_MENU_ID_NEWFOLDER: {
                if (g_n >= DESKOPS_MAX_ITEMS) { done = "0"; why = "max desktop items"; break; }
                int slot = DESKOPS_KIND_FOLDER;
                for (int k = DESKOPS_KIND_FOLDER; k <= 7; k++) {
                    bool used = false;
                    for (int i = 0; i < g_n; i++) if (g_items[i].kind == k) used = true;
                    for (int i = 0; i < g_rec_n; i++) if (g_recycle[i] == k) used = true;
                    if (!used) { slot = k; break; }
                }
                DeskItem nit{};
                nit.kind = slot;
                item_default_pos(slot, &nit.x, &nit.y);
                g_items[g_n++] = nit;
                set_cache_sync();
                dbg64_line_begin64();
                dbg64_str("[DESK64] newfolder name=");
                dbg64_str(item_name(slot));
                dbg64_str(" kind=");
                dbg64_dec((uint64_t)slot);
                dbg64_str(" n=");
                dbg64_dec((uint64_t)g_n);
                dbg64_str(" x=");
                dbg64_dec((uint64_t)nit.x);
                dbg64_str(" y=");
                dbg64_dec((uint64_t)nit.y);
                dbg64_str(" persisted=1 (desktop shortcut entry; no /Desktop volume in this batch)");
                dbg64_nl();
                dbg64_line_end64();
                gui64_invalidate();
                why = "folder entry added";
                break;
            }
            case DOPS_MENU_ID_PERSON:
                app_settings_open64();
                why = "settings opened (personalization group; page switching is another line's file)";
                break;
            case DOPS_MENU_ID_DISPLAY:
                app_settings_open64();
                why = "settings opened (default page = display)";
                break;
            case DOPS_MENU_ID_TERM:
                app_term_open64();
                why = "terminal opened";
                break;
            default:
                done = "0";
                why = "not implemented";
                break;
        }
    }
    dbg64_line_begin64();
    dbg64_str("[DESK64] menu action id=");
    dbg64_dec((uint64_t)it.id);
    dbg64_str(" name=");
    dbg64_str(gui64_lang_zh() ? it.zh : it.en);
    dbg64_str(" done=");
    dbg64_str(done);
    dbg64_str(" why=");
    dbg64_str(why);
    dbg64_nl();
    dbg64_line_end64();
}

int desktopops64_menu_press64(int mx, int my, int button) {
    if (!g_menu_open) return 0;
    (void)button;
    if (mx >= g_menu_x && mx < g_menu_x + g_menu_w && my >= g_menu_y && my < g_menu_y + g_menu_h) {
        const int idx = (my - g_menu_y - THEME64_POP_PAD / 2) / g_menu_item_h;
        desktopops64_menu_close64("action");
        if (idx >= 0 && idx < DOPS_MENU_N) menu_action(idx);
        return 1;
    }
    desktopops64_menu_close64("outside");     // 点外部关闭（需求）
    return 1;
}

int desktopops64_menu_key64(uint8_t c) {
    if (!g_menu_open) return 0;
    if (c == 0x1B) { desktopops64_menu_close64("esc"); return 1; }
    if (c == 0xFD) { g_menu_hover = (g_menu_hover <= 0) ? DOPS_MENU_N - 1 : g_menu_hover - 1; gui64_dirty(g_menu_x, g_menu_y, g_menu_w, g_menu_h); return 1; }
    if (c == 0xFE) { g_menu_hover = (g_menu_hover + 1) % DOPS_MENU_N; gui64_dirty(g_menu_x, g_menu_y, g_menu_w, g_menu_h); return 1; }
    if (c == '\n' || c == '\r') {
        const int idx = (g_menu_hover >= 0) ? g_menu_hover : 0;
        desktopops64_menu_close64("action");
        menu_action(idx);
        return 1;
    }
    return 1;     // 菜单打开时吞掉其它键（与开始菜单同语义）
}

void desktopops64_menu_draw64() {
    if (!g_menu_open) return;
    const Theme64Tokens* t = theme64_tokens64();
    const bool zh = gui64_lang_zh();
    // 亚克力 + 圆角 + 双层浅阴影 + 1px 高光边（p2ui_popup64 一次到位，数字全在 Token）
    p2ui_popup64(g_menu_x, g_menu_y, g_menu_w, g_menu_h, THEME64_POP_R, THEME64_POP_R,
                 t, t->client_bg, t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    for (int i = 0; i < DOPS_MENU_N; i++) {
        const int iy = g_menu_y + THEME64_POP_PAD / 2 + i * g_menu_item_h;
        if (i == g_menu_hover)
            gfx64_fill_round64(g_menu_x + 4, iy + 2, g_menu_w - 8, g_menu_item_h - 4,
                               THEME64_R_BUTTON, t->sel_bg, 110);
        const uint32_t fg = kMenu[i].enabled ? t->text : t->text_dim;
        const int ty = iy + (g_menu_item_h - p2ui_line_h64()) / 2;
        p2ui_text64(g_menu_x + THEME64_POP_PAD + 22, ty,
                    zh ? kMenu[i].zh : kMenu[i].en, fg);
        if (!kMenu[i].enabled) {
            // 置灰项打点提示（需求：做不到的项如实置灰并打点）—— 画一个小圆点做视觉标记
            p2ui_fill_circle64(g_menu_x + THEME64_POP_PAD + 10, iy + g_menu_item_h / 2, 3, fg, 200);
        } else {
            p2ui_fill_circle64(g_menu_x + THEME64_POP_PAD + 10, iy + g_menu_item_h / 2, 3, t->accent, 220);
        }
    }
}

// ==================== 玻璃选择框 ====================
int desktopops64_selbox_active64() { return g_sel_active; }
void desktopops64_selbox_rect64(int* x, int* y, int* w, int* h) {
    int bx = g_sel_x0 < g_sel_x1 ? g_sel_x0 : g_sel_x1;
    int by = g_sel_y0 < g_sel_y1 ? g_sel_y0 : g_sel_y1;
    int bw = g_sel_x0 < g_sel_x1 ? g_sel_x1 - g_sel_x0 : g_sel_x0 - g_sel_x1;
    int bh = g_sel_y0 < g_sel_y1 ? g_sel_y1 - g_sel_y0 : g_sel_y0 - g_sel_y1;
    if (x) *x = bx;
    if (y) *y = by;
    if (w) *w = bw;
    if (h) *h = bh;
}
int desktopops64_selbox_count64() { return g_sel_sel; }
void desktopops64_selbox_begin64(int x, int y) {
    g_sel_active = 1;
    g_sel_x0 = x; g_sel_y0 = y;
    g_sel_x1 = x; g_sel_y1 = y;
    g_sel_sel = -1;
    g_sel_logged = 0;
}
static int sel_recompute(void) {
    int sel = -1;
    for (int i = 0; i < g_n; i++)
        if (desktopops64_rect_hit64(i, g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1)) sel = i;
    if (sel != g_sel_sel) { g_sel_sel = sel; return 1; }
    return 0;
}
int desktopops64_selbox_update64(int x, int y) {
    if (!g_sel_active) return 0;
    if (x == g_sel_x1 && y == g_sel_y1) return 0;
    g_sel_x1 = x; g_sel_y1 = y;
    sel_recompute();
    int bx, by, bw, bh;
    desktopops64_selbox_rect64(&bx, &by, &bw, &bh);
    gui64_dirty(bx - 3, by - 3, bw + 6, bh + 6);
    // 玻璃选择框的第一帧就打出几何证据（边缘可见 + 框内 alpha=0）
    if (!g_sel_logged && bw > 0 && bh > 0) {
        g_sel_logged = 1;
        dbg64_line_begin64();
        dbg64_str("[DESK64] selbox glass x=");
        dbg64_dec((uint64_t)bx);
        dbg64_str(" y=");
        dbg64_dec((uint64_t)by);
        dbg64_str(" w=");
        dbg64_dec((uint64_t)bw);
        dbg64_str(" h=");
        dbg64_dec((uint64_t)bh);
        dbg64_str(" corner=");
        dbg64_dec((uint64_t)THEME64_R_BUTTON);
        dbg64_str(" edge=1 inside_alpha=0 sel=");
        dbg64_dec((uint64_t)(g_sel_sel < 0 ? 0 : g_sel_sel + 1));
        dbg64_nl();
        dbg64_line_end64();
    }
    return 1;
}
void desktopops64_selbox_end64() {
    if (!g_sel_active) return;
    int bx, by, bw, bh;
    desktopops64_selbox_rect64(&bx, &by, &bw, &bh);
    int sel = 0;
    for (int i = 0; i < g_n; i++) if (desktopops64_rect_hit64(i, g_sel_x0, g_sel_y0, g_sel_x1, g_sel_y1)) sel++;
    // ★ 兼容既有验收的格式（gui_modern64_test 的鼠标闭环靠这一行定位光标）
    dbg64_str("[UI] selbox x0=");
    dbg64_dec((uint64_t)bx);
    dbg64_str(" y0=");
    dbg64_dec((uint64_t)by);
    dbg64_str(" x1=");
    dbg64_dec((uint64_t)(bx + bw));
    dbg64_str(" y1=");
    dbg64_dec((uint64_t)(by + bh));
    dbg64_str(" sel=");
    dbg64_dec((uint64_t)sel);
    dbg64_nl();
    dbg64_str("[DESK64] selbox result sel=");
    dbg64_dec((uint64_t)sel);
    dbg64_str(" glass=1 inside_alpha=0");
    dbg64_nl();
    g_sel_active = 0;
    g_sel_sel = -1;
    gui64_dirty(bx - 3, by - 3, bw + 6, bh + 6);
}

// 圆角矩形内部判定（px,py 相对矩形左上角；用于"只画边框环"）
static bool rr_inside(int /*x*/, int /*y*/, int w, int h, int r, int px, int py) {
    if (px < 0 || py < 0 || px >= w || py >= h) return false;
    if (r <= 0) return true;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    const int cx = px - (w - 1) / 2, cy = py - (h - 1) / 2;    // 相对中心
    const int hw = w / 2 - r, hh = h / 2 - r;
    int dx = cx < 0 ? -cx : cx, dy = cy < 0 ? -cy : cy;
    if (dx > hw) dx -= hw; else dx = 0;
    if (dy > hh) dy -= hh; else dy = 0;
    return (dx * dx + dy * dy) <= r * r;
}

// 玻璃选择框绘制：**框内一个像素都不铺**；只画 3px 玻璃环（Token sel_fill）+ 突出边缘（Token sel_edge）
void desktopops64_selbox_draw64() {
    if (!g_sel_active) return;
    int bx, by, bw, bh;
    desktopops64_selbox_rect64(&bx, &by, &bw, &bh);
    if (bw < 2 || bh < 2) return;
    const Theme64Tokens* t = theme64_tokens64();
    const int r = THEME64_R_BUTTON;
    const int ring = 3;
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            if (!rr_inside(bx, by, bw, bh, r, x, y)) continue;
            const bool outer = rr_inside(bx + ring, by + ring, bw - ring * 2, bh - ring * 2, r - ring, x - ring, y - ring);
            if (outer) continue;                      // 框内：**不落笔**（全透明）
            gfx64_blend64(bx + x, by + y, t->sel_fill, 70);
        }
    }
    gfx64_stroke_round64(bx, by, bw, bh, r, t->sel_edge, 255);                    // 边缘（看得清）
    gfx64_stroke_round64(bx + 1, by + 1, bw - 2, bh - 2, r - 1, rgb(255, 255, 255), 120);   // 玻璃高光
    gfx64_stroke_round64(bx - 1, by - 1, bw + 2, bh + 2, r + 1, t->sel_edge, 90);         // 外扩 1px：更突出
}

// ==================== 自检 ====================
int desktopops64_selftest64() {
    int fails = 0;
    // 1) 圆角内部判定：中心在内、四角在外
    if (!rr_inside(0, 0, 40, 40, 9, 20, 20)) fails |= 1;
    if (rr_inside(0, 0, 40, 40, 9, 0, 0)) fails |= 2;
    // 2) 集合解析：默认 "0,1,2|" 必须是 3 项
    char save[CFG64_STR_MAX];
    s_cpy(save, g_set_cache, (int)sizeof(save));
    const int n_save = g_n, r_save = g_rec_n;
    DeskItem items_save[DESKOPS_MAX_ITEMS];
    for (int i = 0; i < g_n; i++) items_save[i] = g_items[i];
    int rec_save[DESKOPS_RECYCLE_MAX];
    for (int i = 0; i < g_rec_n; i++) rec_save[i] = g_recycle[i];

    apply_set("0,1,2|");
    if (g_n != 3 || g_recycle[0] != 0 || g_rec_n != 0) fails |= 4;
    apply_set("0,1|2");
    if (g_n != 2 || g_rec_n != 1 || g_recycle[0] != 2) fails |= 8;
    apply_set("");
    if (g_n != 0 || g_rec_n != 0) fails |= 16;
    // 恢复现场
    g_n = n_save; g_rec_n = r_save;
    for (int i = 0; i < n_save; i++) g_items[i] = items_save[i];
    for (int i = 0; i < r_save; i++) g_recycle[i] = rec_save[i];
    s_cpy(g_set_cache, save, (int)sizeof(g_set_cache));

    dbg64_str("[DESK64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" items=");
    dbg64_dec((uint64_t)g_n);
    dbg64_nl();
    return fails;
}
