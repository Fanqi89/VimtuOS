// dockmenu64.cpp - ★ v0.8.0：Dock 图标 / 开始菜单应用图标的**右键上下文菜单**
//
// 需求原文（要点）：
//   * 右键 Dock 图标或开始菜单应用列表里的应用图标 → 弹出上下文菜单；
//     未固定显示"固定到 Dock"，已固定显示"取消固定"；运行中显示"关闭窗口"/"关闭所有窗口"；
//     同一个菜单可以**同时**包含固定状态项与关闭项（例：已固定且正在运行 -> 取消固定 + 关闭窗口）。
//   * 右键"具体运行图标" -> "关闭窗口"关该图标对应的**那个窗口**；同一应用有多个运行窗口时可额外
//     显示"关闭所有窗口"（关掉该应用所有窗口）。
//   * 开始菜单应用列表：只有一个运行窗口 -> "关闭窗口"；多个运行窗口 -> "关闭所有窗口"。
//   * 取消固定只移除固定图标（不关窗口）；关闭窗口只移除对应运行图标（不取消固定）；
//     关闭所有窗口只移除该应用所有运行图标（不取消固定）。固定状态必须持久化。
//
// 实现约定（与仓库既有风格一致）：
//   * 视觉数字全部走 theme64 Token / p2ui 工具箱（本文件不写死圆角、行高、内边距）。
//   * 打点（自动验收用，均有上限防刷屏）：
//       [DOCK64] menu open owner=dock|start app=<id> win=<id|-> pinned=<0|1> rows=<n> items=<a,b,..> x= y= w= h=
//       [DOCK64] menu act pid=<..> owner=.. app=.. win=.. act=pin|unpin|close-win|close-all pinned_after=<0|1> running_after=<n>
//       [DOCK64] menu close why=outside|esc|action|overlap
//   * 点菜单外面：关闭菜单并**吃掉**这次点击（与桌面右键菜单/开始菜单同语义，避免穿透）。
#include "dock64.h"
#include "gui64.h"
#include "theme64.h"
#include "gfx64.h"
#include "startmenu64.h"     // p2ui_*（亚克力弹窗/文本/线性图标）
#include "debug64.h"

#define DOCKMENU_ROWS_MAX 4

enum {
    DOCKMENU_ACT_NONE = 0,
    DOCKMENU_ACT_PIN = 1,
    DOCKMENU_ACT_UNPIN = 2,
    DOCKMENU_ACT_CLOSE_WIN = 3,
    DOCKMENU_ACT_CLOSE_ALL = 4
};

struct DockMenuRow64 { int act; const char* zh; const char* en; };

static int  g_open = 0;
static int  g_owner = DOCKMENU_OWNER_NONE;
static int  g_app = -1;
static int  g_win = -1;                 // -1 = 未指定具体窗口
static int  g_idx = -1;                 // Dock 下标（owner=dock 时）
static int  g_x = 0, g_y = 0, g_w = 0, g_h = 0;
static int  g_hover = -1;
static int  g_last_mx = -1, g_last_my = -1;   // ★ v0.8.0：上一个鼠标位置（没动就不抢键盘选中）
static int  g_rows = 0;
static DockMenuRow64 g_row[DOCKMENU_ROWS_MAX];
static int  g_log_budget = 200;         // 防刷屏

static void dm_log_open() {
    if (g_log_budget <= 0) return;
    g_log_budget--;
    dbg64_line_begin64();
    dbg64_str("[DOCK64] menu open owner=");
    dbg64_str(g_owner == DOCKMENU_OWNER_DOCK ? "dock" : "start");
    dbg64_str(" app=");
    dbg64_dec((uint64_t)(g_app < 0 ? 0 : g_app));
    dbg64_str(" win=");
    if (g_win < 0) dbg64_str("-"); else dbg64_dec((uint64_t)g_win);
    dbg64_str(" pinned=");
    dbg64_dec((uint64_t)(dock64_is_pinned64(g_app) ? 1 : 0));
    dbg64_str(" rows=");
    dbg64_dec((uint64_t)g_rows);
    dbg64_str(" items=");
    for (int i = 0; i < g_rows; i++) {
        if (i) dbg64_str(",");
        const int a = g_row[i].act;
        dbg64_str(a == DOCKMENU_ACT_PIN ? "pin" : (a == DOCKMENU_ACT_UNPIN ? "unpin" :
                  (a == DOCKMENU_ACT_CLOSE_WIN ? "close-win" : "close-all")));
    }
    dbg64_str(" x=");
    dbg64_dec((uint64_t)g_x);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)g_y);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)g_w);
    dbg64_str(" h=");
    dbg64_dec((uint64_t)g_h);
    dbg64_nl();
    dbg64_line_end64();
}

// 菜单行的文案（中/英）—— 只有"文案"是本文件的静态内容，**是否出现**完全由运行时状态决定
static void dm_push_row(int act) {
    if (g_rows >= DOCKMENU_ROWS_MAX) return;
    DockMenuRow64* r = &g_row[g_rows++];
    r->act = act;
    if (act == DOCKMENU_ACT_PIN)        { r->zh = "固定到 Dock";    r->en = "Pin to Dock"; }
    else if (act == DOCKMENU_ACT_UNPIN) { r->zh = "取消固定";       r->en = "Unpin from Dock"; }
    else if (act == DOCKMENU_ACT_CLOSE_WIN) { r->zh = "关闭窗口";   r->en = "Close window"; }
    else                                { r->zh = "关闭所有窗口";   r->en = "Close all windows"; }
}

static void dm_layout64(int mx, int my) {
    int wmax = 0;
    for (int i = 0; i < g_rows; i++) {
        const int tw = p2ui_text_w64(gui64_lang_zh() ? g_row[i].zh : g_row[i].en);
        if (tw > wmax) wmax = tw;
    }
    g_w = wmax + THEME64_POP_PAD * 2 + 22;
    g_h = THEME64_POP_PAD + g_rows * THEME64_POP_ROW;
    g_x = mx;
    g_y = my;
    const int sw = gui64_screen_w();
    const int sh = gui64_screen_h();
    if (g_x + g_w > sw - 4) g_x = sw - g_w - 4;
    if (g_x < 4) g_x = 4;
    if (g_y + g_h > sh - 4) g_y = my - g_h - 8;          // 贴底就翻到光标上方
    if (g_y < 4) g_y = 4;
    g_hover = 0;                  // ★ 打开即选中第 0 行（Enter 直接可用，Down 从 0->1）
    g_last_mx = mx; g_last_my = my;   // ★ 以**打开位置**为基准：指针不动就不抢键盘选中的行
}

static void dm_build64(int owner, int app_id, int win_id) {
    g_rows = 0;
    g_owner = owner;
    g_app = app_id;
    g_win = win_id;
    // ① 固定状态项（一直有；固定 <-> 取消固定）
    if (app_id >= 0) dm_push_row(dock64_is_pinned64(app_id) ? DOCKMENU_ACT_UNPIN : DOCKMENU_ACT_PIN);
    // ② 关闭项（按"运行中"的实际情况决定，可与 ① 同时存在）
    const int nwin = (app_id >= 0) ? dock64_app_windows64(app_id) : 0;
    if (win_id >= 0) {
        dm_push_row(DOCKMENU_ACT_CLOSE_WIN);              // 右键的是**具体运行图标**：关这一个窗口
        if (nwin > 1) dm_push_row(DOCKMENU_ACT_CLOSE_ALL);  // 同应用多窗口：额外给"关闭所有窗口"
    } else if (nwin == 1) {
        dm_push_row(DOCKMENU_ACT_CLOSE_WIN);              // 开始菜单：单窗口 -> 关闭窗口
    } else if (nwin > 1) {
        dm_push_row(DOCKMENU_ACT_CLOSE_ALL);              // 开始菜单：多窗口 -> 关闭所有窗口
    }
}

void dockmenu64_open_for_dock64(int idx, int mx, int my) {
    const int app = dock64_app64(idx);
    const int win = dock64_win64(idx);
    dm_build64(DOCKMENU_OWNER_DOCK, app, win);
    g_idx = idx;
    if (g_rows <= 0) { g_open = 0; return; }              // 没有可用项：不弹空菜单（如实不打点）
    dm_layout64(mx, my);
    g_open = 1;
    gui64_invalidate();
    dm_log_open();
}

void dockmenu64_open_for_app64(int app_id, int win_id, int mx, int my) {
    dm_build64(DOCKMENU_OWNER_START, app_id, win_id);
    g_idx = -1;
    if (g_rows <= 0) { g_open = 0; return; }
    dm_layout64(mx, my);
    g_open = 1;
    gui64_invalidate();
    dm_log_open();
}

int dockmenu64_is_open64() { return g_open; }

void dockmenu64_close64(const char* why) {
    if (!g_open) return;
    g_open = 0;
    g_hover = -1;
    if (g_log_budget > 0) {
        g_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[DOCK64] menu close why=");
        dbg64_str(why ? why : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
    gui64_invalidate();
}

static void dm_apply64(int act) {
    const int app = g_app;
    const int win = g_win;
    // ★ 需求：三种动作**互不牵连** —— 取消固定不关窗口、关闭窗口不取消固定、关闭所有窗口不取消固定
    if (act == DOCKMENU_ACT_PIN) dock64_pin_set64(app, 1);
    else if (act == DOCKMENU_ACT_UNPIN) dock64_pin_set64(app, 0);
    else if (act == DOCKMENU_ACT_CLOSE_WIN) {
        if (win >= 0) dock64_close_window64(win);
        else if (app >= 0) {                              // 开始菜单只有窗口数=1 的情况：关那唯一一个
            for (int i = 0; i < dock64_count64(); i++)
                if (dock64_win64(i) >= 0 && dock64_app64(i) == app) { dock64_close_window64(dock64_win64(i)); break; }
        }
    } else if (act == DOCKMENU_ACT_CLOSE_ALL) {
        if (app >= 0) dock64_close_app_windows64(app);
    }
    if (g_log_budget > 0) {
        g_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[DOCK64] menu act owner=");
        dbg64_str(g_owner == DOCKMENU_OWNER_DOCK ? "dock" : "start");
        dbg64_str(" app=");
        dbg64_dec((uint64_t)(app < 0 ? 0 : app));
        dbg64_str(" win=");
        if (win < 0) dbg64_str("-"); else dbg64_dec((uint64_t)win);
        dbg64_str(" act=");
        dbg64_str(act == DOCKMENU_ACT_PIN ? "pin" : (act == DOCKMENU_ACT_UNPIN ? "unpin" :
                  (act == DOCKMENU_ACT_CLOSE_WIN ? "close-win" : "close-all")));
        dbg64_str(" pinned_after=");
        dbg64_dec((uint64_t)(dock64_is_pinned64(app) ? 1 : 0));
        dbg64_str(" pinned_n=");
        dbg64_dec((uint64_t)dock64_pin_count64());
        dbg64_str(" running_n=");
        dbg64_dec((uint64_t)dock64_run_count64());
        dbg64_nl();
        dbg64_line_end64();
    }
}

static int dm_row_at64(int mx, int my) {
    if (!g_open) return -1;
    if (mx < g_x || mx >= g_x + g_w || my < g_y || my >= g_y + g_h) return -1;
    const int rel = my - g_y - THEME64_POP_PAD / 2;
    if (rel < 0) return -1;
    const int r = rel / THEME64_POP_ROW;
    return (r >= 0 && r < g_rows) ? r : -1;
}

int dockmenu64_press64(int mx, int my, int button) {
    if (!g_open) return 0;
    const int r = dm_row_at64(mx, my);
    if (r >= 0) {
        const int act = g_row[r].act;
        if (button == 0 || button == 1) {
            dockmenu64_close64("action");
            dm_apply64(act);
        }
        return 1;
    }
    dockmenu64_close64("outside");
    return 1;                                             // 吃掉这次点击，不穿透到 Dock/桌面
}

void dockmenu64_move64(int mx, int my) {
    if (!g_open) return;
    // ★ v0.8.0：鼠标**没真的动**就不要把 hover 抢回去。否则用键盘 Down 选到第 2 行后，
    //   下一个鼠标事件（光标本就停在菜单上）会把它重置回第 1 行，Enter 就执行错项。
    // 位移 < 8px 当作抖动（客户机光标会有几毫米漂）：不抢键盘选中的行。
    const int adx = (mx > g_last_mx) ? (mx - g_last_mx) : (g_last_mx - mx);
    const int ady = (my > g_last_my) ? (my - g_last_my) : (g_last_my - my);
    if (g_last_mx >= 0 && adx + ady < 8) return;
    g_last_mx = mx;
    g_last_my = my;
    const int r = dm_row_at64(mx, my);
    if (r != g_hover) {
        g_hover = r;
        gui64_dirty(g_x, g_y, g_w, g_h);
    }
}

// ★ v0.8.0：键盘操作（Up/Down 切行、Enter 执行、ESC 关）。
//   为什么：菜单只能用鼠标点是不够的（高分辨率/无鼠标场景）；
//   同时它让自动验收不必赌相对鼠标定位（客户机 2fps 时鼠标事件会拖影）。
int dockmenu64_key64(int c) {
    if (!g_open) return 0;
    if (g_log_budget > 0) {                    // 诊断：看清菜单到底收到什么键
        g_log_budget--;
        dbg64_line_begin64();
        dbg64_str("[DOCK64] menu key c=");
        dbg64_dec((uint64_t)(uint8_t)c);
        dbg64_str(" hover=");
        dbg64_dec((uint64_t)(g_hover < 0 ? 0 : g_hover));
        dbg64_str(" rows=");
        dbg64_dec((uint64_t)g_rows);
        dbg64_nl();
        dbg64_line_end64();
    }
    if (c == 27) { dockmenu64_close64("esc"); return 1; }        // ESC
    if (c == 0xFD) {                                              // Up
        g_hover = (g_hover <= 0) ? g_rows - 1 : g_hover - 1;
        gui64_dirty(g_x, g_y, g_w, g_h);
        return 1;
    }
    if (c == 0xFE) {                                              // Down
        g_hover = (g_hover + 1) % g_rows;
        gui64_dirty(g_x, g_y, g_w, g_h);
        return 1;
    }
    if (c == 13 || c == 10) {                                     // Enter
        const int r = (g_hover >= 0 && g_hover < g_rows) ? g_hover : 0;
        const int act = g_row[r].act;
        dockmenu64_close64("action");
        dm_apply64(act);
        return 1;
    }
    return 1;                                                     // 开着就吃掉其它键（不泄漏给底下窗口）
}

void dockmenu64_draw64() {
    if (!g_open) return;
    const Theme64Tokens* t = theme64_tokens64();
    const bool zh = gui64_lang_zh();
    p2ui_popup64(g_x, g_y, g_w, g_h, THEME64_POP_R, THEME64_POP_R, t,
                 t->client_bg, t->dark ? THEME64_A_POP_DARK : THEME64_A_POP);
    for (int i = 0; i < g_rows; i++) {
        const int iy = g_y + THEME64_POP_PAD / 2 + i * THEME64_POP_ROW;
        if (i == g_hover)
            gfx64_fill_round64(g_x + 4, iy + 2, g_w - 8, THEME64_POP_ROW - 4,
                               THEME64_R_BUTTON, t->sel_bg, 110);
        const int ty = iy + (THEME64_POP_ROW - p2ui_line_h64()) / 2;
        p2ui_text64(g_x + THEME64_POP_PAD + 22, ty, zh ? g_row[i].zh : g_row[i].en, t->text);
        const int act = g_row[i].act;
        const uint32_t dot = (act == DOCKMENU_ACT_PIN || act == DOCKMENU_ACT_UNPIN) ? t->accent : t->text_dim;
        p2ui_fill_circle64(g_x + THEME64_POP_PAD + 10, iy + THEME64_POP_ROW / 2, 3, dot, 220);
    }
}
