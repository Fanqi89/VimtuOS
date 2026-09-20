// explorer64.cpp - 文件资源管理器 / "此电脑"（Win10 风格的 UI 与交互）
//
// 分工（见 kernel/explorer64.h 的说明）：gui64.cpp 只做外壳钩子（app_mypc_open64 转调本文件），
// 窗口回调、导航状态机、绘制、命中测试全在这里。
//
// 实现要点（都写在这，方便后来人改）：
//   1) **一个窗口里导航**：mode 0 = 此电脑（驱动器卡片），mode 1 = 目录浏览；历史栈 16 条。
//   2) 绘制全部用简单几何图形（无位图资源）：文件夹 = 琥珀色带标签的方块、VAP64 = 蓝色方块 +
//      白色三角、ELF64 = 绿色方块 + 白色 ">_"、文本 = 白纸 + 灰线、二进制/未知 = 灰纸 + 点。
//   3) 打点集中在 exp_log_* 里，全部走 dbg64_line_begin64()/dbg64_line_end64() 行锁。
//   4) 双击（按下->按下 间隔 <= 500ms 且同一条目）= 进入/运行/预览；单击 = 选中。
//      鼠标滚轮在内核里没有（kernel/input.h 的 PS/2 鼠标只报 3 字节包），所以滚动用：
//      上下方向键（NAV_UP/DOWN，0xFD/0xFE）+ 点击内容区右侧的滚动条（上/下半页）+ 翻页按钮语义。
//   5) 所有下标/坐标先钳制再用：滚动、选中、命中测试都有界，越界一律不写不画。
//
// ★ 自动验收（tests/explorer64_test.py）依赖的**几何常量**（改这里必须同步改脚本）：
//     窗口: x=120 y=60 w=660 h=470  ->  客户区原点 (121,85)，客户区 658x444
//     工具栏 26px（文件[8..72] 计算机[76..140] 查看[144..208]）、地址栏 28px
//     （后退[6..32] 前进[34..60] 上级[62..88]、面包屑 x>=96）
//     导航窗格 150px；内容区 x=151..657、y=54..421；状态栏 22px
//     此电脑卡片: x=161 宽 487，第 i 张卡片顶部 y = 84 + i*88（高 78 + 间距 10）
//               容量条: x=card_x+52 y=card_y+34 宽 300 高 12
//     详细信息: 表头 y=58 高 20；行 y=78 起，行高 20；列分隔线 x=内容区+206/+362/+488
#include "explorer64.h"
#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "input.h"
#include "debug64.h"
#include "x86_64.h"
#include "vfs64.h"
#include "drive64.h"
#include "app64.h"
#include "elf64.h"

// ==================== 几何常量（与脚本共享，勿乱改）====================
#define EXP_WIN_X 120
#define EXP_WIN_Y 60
#define EXP_WIN_W 660
#define EXP_WIN_H 470

#define EXP_TOOLBAR_H 26
#define EXP_ADDR_H    28
#define EXP_NAV_W     150
#define EXP_STATUS_H  22

#define EXP_CONTENT_X (EXP_NAV_W + 1)                      // 151
#define EXP_CONTENT_Y (EXP_TOOLBAR_H + EXP_ADDR_H)         // 54
#define EXP_CONTENT_W (EXP_WIN_W - 2 - EXP_CONTENT_X)      // 507
#define EXP_CONTENT_H (EXP_WIN_H - 25 - 1 - EXP_CONTENT_Y - EXP_STATUS_H)   // 368

#define EXP_CARD_TOP   (EXP_CONTENT_Y + 30)                // 84
#define EXP_CARD_X     (EXP_CONTENT_X + 10)                // 161
#define EXP_CARD_W     (EXP_CONTENT_W - 20)                // 487
#define EXP_CARD_H     78
#define EXP_CARD_GAP   10
#define EXP_BAR_W      300
#define EXP_BAR_H      12

#define EXP_DET_HEAD_Y (EXP_CONTENT_Y + 4)                 // 58
#define EXP_DET_HEAD_H 20
#define EXP_DET_ROW_Y  (EXP_CONTENT_Y + 24)                // 78
#define EXP_DET_ROW_H  20
#define EXP_COL_NAME_X 6
#define EXP_COL_NAME_W 200
#define EXP_COL_DATE_X 212
#define EXP_COL_DATE_W 150
#define EXP_COL_TYPE_X 368
#define EXP_COL_TYPE_W 120
#define EXP_COL_SIZE_X 494
#define EXP_COL_SIZE_W 160

#define EXP_ICON_CELL_W 96
#define EXP_ICON_CELL_H 74
#define EXP_ICON_SIZE   32

#define EXP_TB_BTN_W 64                                    // 工具栏按钮固定宽（脚本按固定坐标定位）
#define EXP_NAV_TOP   30                                   // 导航窗格首项 y
#define EXP_NAV_ROW_H 22

#define EXP_MAX_ITEMS 64       // 单页缓存的条目数（更多条目靠翻页，扫描计数仍然完整）
#define EXP_NAV_DEPTH 16       // 路径段上限（与 VFS64_PATH_DEPTH_MAX 对齐）
#define EXP_HIST_MAX  16       // 历史栈深度
#define EXP_CLICK_MS  500      // 双击判定窗口（与外壳桌面图标一致）

// ==================== 配色 ====================
#define C_EXP_BG        rgb(240, 240, 240)     // 客户区底色
#define C_EXP_TOOLBAR   rgb(247, 247, 247)
#define C_EXP_ADDR      rgb(255, 255, 255)
#define C_EXP_NAV       rgb(250, 250, 250)
#define C_EXP_LINE      rgb(190, 190, 190)
#define C_EXP_TEXT      rgb(32, 32, 32)
#define C_EXP_DIM       rgb(120, 120, 120)
#define C_EXP_BTN_HOV   rgb(224, 232, 244)
#define C_EXP_BTN_PRESS rgb(196, 214, 240)
#define C_EXP_NAV_SEL   rgb(204, 232, 255)
#define C_EXP_SEL       rgb(204, 232, 255)     // 列表选中（浅蓝，与容量条深蓝区分）
#define C_EXP_CARD      rgb(255, 255, 255)
#define C_EXP_BAR_BG    rgb(224, 224, 224)
#define C_EXP_BAR_FILL  rgb(0, 120, 215)       // 容量条已用部分（自动验收按这个颜色判定）
#define C_EXP_HEAD      rgb(240, 240, 240)
#define C_EXP_SEP       rgb(200, 200, 200)
#define C_EXP_STATUS    rgb(245, 245, 245)
// 文本预览窗口几何（特意放在资源管理器窗口右侧，互不遮挡；脚本按这个位置判像素）
#define PREV_WIN_X 820
#define PREV_WIN_Y 430
#define PREV_WIN_W 440
#define PREV_WIN_H 300
#define C_EXP_MENU      rgb(255, 255, 255)
#define C_EXP_MENU_HOV  rgb(229, 243, 255)

// 工具栏按钮 id（hit 打点用）
enum { IDC_NONE = 0, IDC_FILE, IDC_COMPUTER, IDC_VIEW, IDC_BACK, IDC_FWD, IDC_UP, IDC_CRUMB, IDC_NAV };
// 内容条目关联的应用（双击行为）
enum { ASSOC_NONE = 0, ASSOC_DIR, ASSOC_VAP, ASSOC_ELF, ASSOC_TEXT };

// ==================== 状态 ====================
struct ExpNav {
    int  mode;                        // 0 = 此电脑，1 = 目录
    char letter;                      // 盘符（mode 1）
    char path[VFS64_PATH_MAX];        // 目录路径（mode 1）
};

static Window* g_win = nullptr;
static Window* g_prev_win = nullptr;

static int  g_mode = 0;
static char g_path[VFS64_PATH_MAX] = "/";
static char g_letter = 'C';
static DriveInfo64 g_cur_drive;
static bool g_cur_drive_ok = false;

static int  g_view = 0;               // 0 = 图标视图，1 = 详细信息
static int  g_sel = -1;               // 选中条目（-1 = 无）
static int  g_scroll = 0;             // 首个可见条目下标
static int  g_items = 0;              // 当前目录条目数（完整计数）
static Vfs64Dirent64 g_ents[EXP_MAX_ITEMS];

static DriveInfo64 g_drives[DRV64_MAX_ENTRIES];
static int  g_drive_n = 0;

static ExpNav g_hist[EXP_HIST_MAX];
static int  g_hist_n = 0, g_hist_pos = -1;

static int  g_menu_open = 0;          // 文件菜单
static int  g_press_id = IDC_NONE;    // 按下反馈（可见）
static uint32_t g_press_tick = 0;
static int  g_hover_id = IDC_NONE;    // hover 高亮（状态变化时标脏）
static uint32_t g_msg_tick = 0;       // 状态栏提示的过期时间
static char g_msg[72] = {0};
static int  g_last_item = -1;         // 双击判定：上一次按下的条目
static uint32_t g_last_tick = 0;

static char g_prev_name[40] = {0};
static char g_prev_buf[1025] = {0};
static int  g_prev_len = 0;

// ==================== 小工具（内核里没有 libc）====================
static void e_strcpy(char* dst, const char* src, int cap) {
    int i = 0;
    if (cap <= 0) return;
    for (; src && src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int e_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static bool e_streq(const char* a, const char* b) {
    while (a && b && *a && *a == *b) { a++; b++; }
    return (a && b) ? (*a == *b) : false;
}
static void e_strcat(char* dst, const char* src, int cap) {
    int n = e_strlen(dst);
    int i = 0;
    for (; src[i] && n < cap - 1; i++) dst[n++] = src[i];
    dst[n] = 0;
}
static int u2s(char* b, uint64_t v) {        // 十进制 -> 字符串，返回写入长度
    char t[24]; int n = 0, k = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    while (n) b[k++] = t[--n];
    b[k] = 0;
    return k;
}
static void text(int x, int y, const char* s, uint32_t fg) {
    font_select(2);                 // simhei 子集：ASCII + 常用汉字（标题栏/应用共用同一面）
    font_draw_text(x, y, s, fg);
}
static int tw(const char* s) { font_select(2); return font_text_width(s); }
static void text_clip(int x, int y, const char* s, uint32_t fg, int maxw) {
    char b[64];
    int i = 0;
    for (; s && s[i] && i < 63; i++) b[i] = s[i];
    b[i] = 0;
    while (i > 0 && tw(b) > maxw) b[--i] = 0;       // 简单截断（够用：名字上限 31B）
    if (b[0]) text(x, y, b, fg);
}

// ---- 人读单位换算（KB -> GB/MB，保留一位小数；自动验收断言换算正确）----
static void fmt_kb64(uint64_t kb, char* out, int cap) {
    // 先按整数运算做"一位小数"：x10 = round(v * 10)
    if (kb >= 1024ull * 1024ull) {
        const uint64_t v10 = (kb * 10ull + (1024ull * 1024ull) / 2) / (1024ull * 1024ull);
        char t[24]; int n = u2s(t, v10 / 10);
        char b[40]; int k = 0;
        for (int i = 0; i < n && k < 30; i++) b[k++] = t[i];
        b[k++] = '.'; b[k++] = (char)('0' + (int)(v10 % 10)); b[k] = 0;
        e_strcpy(out, b, cap); e_strcat(out, " GB", cap);
    } else if (kb >= 1024ull) {
        const uint64_t v10 = (kb * 10ull + 512ull) / 1024ull;
        char t[24]; int n = u2s(t, v10 / 10);
        char b[40]; int k = 0;
        for (int i = 0; i < n && k < 30; i++) b[k++] = t[i];
        b[k++] = '.'; b[k++] = (char)('0' + (int)(v10 % 10)); b[k] = 0;
        e_strcpy(out, b, cap); e_strcat(out, " MB", cap);
    } else {
        char t[24]; u2s(t, kb);
        e_strcpy(out, t, cap); e_strcat(out, " KB", cap);
    }
}
// 字节 -> "N B" / "N.N KB" / "N.N MB"
static void fmt_bytes64(uint64_t b, char* out, int cap) {
    if (b >= 1024ull * 1024ull) {
        const uint64_t v10 = (b * 10ull + (1024ull * 1024ull) / 2) / (1024ull * 1024ull);
        char t[24]; int n = u2s(t, v10 / 10);
        char x[40]; int k = 0;
        for (int i = 0; i < n && k < 30; i++) x[k++] = t[i];
        x[k++] = '.'; x[k++] = (char)('0' + (int)(v10 % 10)); x[k] = 0;
        e_strcpy(out, x, cap); e_strcat(out, " MB", cap);
    } else if (b >= 1024ull) {
        const uint64_t v10 = (b * 10ull + 512ull) / 1024ull;
        char t[24]; int n = u2s(t, v10 / 10);
        char x[40]; int k = 0;
        for (int i = 0; i < n && k < 30; i++) x[k++] = t[i];
        x[k++] = '.'; x[k++] = (char)('0' + (int)(v10 % 10)); x[k] = 0;
        e_strcpy(out, x, cap); e_strcat(out, " KB", cap);
    } else {
        char t[24]; u2s(t, b);
        e_strcpy(out, t, cap); e_strcat(out, " B", cap);
    }
}
// "YYYY-MM-DD HH:MM"（mtime = 0 时给空串：v2 卷/未知）
static void fmt_mtime64(uint32_t packed, char* out, int cap) {
    if (packed == 0) { if (cap > 0) out[0] = 0; return; }
    char ymd[16], hms[16];
    vfs64_time_str64(packed, ymd, (int)sizeof(ymd), hms, (int)sizeof(hms));
    e_strcpy(out, ymd, cap);
    e_strcat(out, " ", cap);
    char hm[8];
    int i = 0;
    for (; i < 5 && hms[i]; i++) hm[i] = hms[i];
    hm[i] = 0;
    e_strcat(out, hm, cap);
}

// ---- 纯函数：滚动钳制 / 父路径 / 段数（自检直接调）----
static int clamp_scroll_pure(int scroll, int items, int per_page) {
    if (items <= 0) return 0;
    if (per_page <= 0) return 0;
    if (items <= per_page) return 0;
    const int maxs = items - per_page;
    if (scroll < 0) return 0;
    if (scroll > maxs) return maxs;
    return scroll;
}
// "/apps/demo" -> "/apps"；"/apps" -> "/"；"/" -> ""（"" 表示"上级 = 此电脑"）
static void path_parent_pure(const char* path, char* out, int cap) {
    out[0] = 0;
    if (!path || !path[0] || (path[0] == '/' && path[1] == 0)) return;
    int n = e_strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    while (n > 1 && path[n - 1] != '/') n--;
    if (n <= 1) { out[0] = '/'; out[1] = 0; return; }
    int i = 0;
    for (; i < n - 1 && i < cap - 1; i++) out[i] = path[i];
    out[i] = 0;
}
static int path_seg_count(const char* path) {
    if (!path) return 0;
    int n = 0, segs = 0, in_seg = 0;
    for (; path[n]; n++) {
        if (path[n] == '/') { in_seg = 0; }
        else if (!in_seg) { in_seg = 1; segs++; }
    }
    return segs;
}
// 取路径的第 k 段（1..segs）写进 out
static void path_seg(const char* path, int k, char* out, int cap) {
    int n = 0, seg = 0, p = 0;
    out[0] = 0;
    if (!path) return;
    while (path[n]) {
        if (path[n] == '/') { n++; continue; }
        seg++;
        p = 0;
        while (path[n] && path[n] != '/') { if (p < cap - 1) out[p++] = path[n]; n++; }
        out[p] = 0;
        if (seg == k) return;
    }
    out[0] = 0;
}
// 历史栈（纯函数，自检可用局部数组驱动）
static void hist_push_pure(ExpNav* h, int* n, int* pos, const ExpNav* rec, int cap) {
    if (*pos + 1 < *n) *n = *pos + 1;              // 截断"前进"分支
    if (*n >= cap) {                               // 满了：丢最旧的一条
        for (int i = 1; i < cap; i++) h[i - 1] = h[i];
        *n = cap - 1;
    }
    h[*n] = *rec;
    (*n)++;
    *pos = *n - 1;
}

// ==================== 打点 ====================
static void exp_log_thispc(int drives, int browsable) {
    dbg64_line_begin64();
    dbg64_str("[UI] explorer thispc drives=");
    dbg64_dec((uint64_t)drives);
    dbg64_str(" browsable=");
    dbg64_dec((uint64_t)browsable);
    dbg64_nl();
    dbg64_line_end64();
}
static void exp_log_card(int idx, const DriveInfo64* d) {
    dbg64_line_begin64();
    dbg64_str("[UI] explorer card idx=");
    dbg64_dec((uint64_t)idx);
    dbg64_str(" letter=");
    if (d->letter) { char l[3]; l[0] = d->letter; l[1] = ':'; l[2] = 0; dbg64_str(l); }
    else dbg64_str("-");
    dbg64_str(" kind=");
    dbg64_str(d->browsable ? "browsable" : "skip");
    dbg64_str(" name=");
    dbg64_str(d->name);
    if (!d->browsable) {
        dbg64_str(" reason=");
        dbg64_str(d->skip == DRV64_SKIP_ESP ? "esp" : "no-fs");
    }
    dbg64_nl();
    dbg64_line_end64();
    if (d->browsable) {
        dbg64_line_begin64();
        dbg64_str("[UI] explorer drive letter=");
        char l[3]; l[0] = d->letter ? d->letter : '?'; l[1] = ':'; l[2] = 0;
        dbg64_str(l);
        dbg64_str(" fs=");
        dbg64_str(d->fs);
        dbg64_str(" total_kb=");
        dbg64_dec(d->total_kb);
        dbg64_str(" free_kb=");
        dbg64_dec(d->free_kb);
        dbg64_nl();
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[UI] explorer notbrowsable name=");
        dbg64_str(d->name);
        dbg64_str(" fs=");
        dbg64_str(d->fs);
        dbg64_str(" reason=");
        dbg64_str(d->skip == DRV64_SKIP_ESP ? "esp" : "no-fs");
        dbg64_nl();
        dbg64_line_end64();
    }
}
static void exp_log_nav() {
    dbg64_line_begin64();
    dbg64_str("[UI] explorer nav path=");
    dbg64_str(g_mode == 0 ? "/" : g_path);
    dbg64_str(" items=");
    dbg64_dec((uint64_t)g_items);
    dbg64_str(" view=");
    dbg64_str(g_view ? "details" : "icons");
    dbg64_nl();
    dbg64_line_end64();
}
static void exp_log_view() {
    dbg64_line_begin64();
    dbg64_str(g_view ? "[UI] explorer view=details rows=" : "[UI] explorer view=icons rows=");
    dbg64_dec((uint64_t)g_items);
    dbg64_nl();
    dbg64_line_end64();
}
static void exp_log_click(int cx, int cy, int sx, int sy, const char* hit) {
    dbg64_line_begin64();
    dbg64_str("[UI] explorer click x=");
    dbg64_dec((uint64_t)cx);
    dbg64_str(" y=");
    dbg64_dec((uint64_t)cy);
    dbg64_str(" sx=");
    dbg64_dec((uint64_t)sx);
    dbg64_str(" sy=");
    dbg64_dec((uint64_t)sy);
    dbg64_str(" hit=");
    dbg64_str(hit);
    dbg64_nl();
    dbg64_line_end64();
}

// 状态栏提示（到期后自动消失 -> 打点 + 标脏）
static void exp_msg(const char* s) {
    e_strcpy(g_msg, s, (int)sizeof(g_msg));
    g_msg_tick = ticks64() + ms_to_ticks64(4000);
    if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
}

// ==================== 驱动器扫描 / 此电脑页 ====================
static void exp_scan_drives() {
    g_drive_n = drive64_scan64();
    if (g_drive_n < 0) g_drive_n = 0;
    if (g_drive_n > (int)DRV64_MAX_ENTRIES) g_drive_n = (int)DRV64_MAX_ENTRIES;
    int browsable = 0, got = 0;
    for (int i = 0; i < g_drive_n; i++) {
        DriveInfo64 d;
        if (drive64_info64(i, &d) != 0 || !d.present) continue;
        g_drives[got++] = d;
        if (d.browsable) browsable++;
        exp_log_card(got - 1, &d);
    }
    g_drive_n = got;
    exp_log_thispc(g_drive_n, browsable);
}

// 目录内容刷新（游标分页扫描：完整计数 + 只缓存前 EXP_MAX_ITEMS 条）
static void exp_refresh_dir() {
    g_items = 0;
    g_sel = -1;
    if (g_scroll < 0) g_scroll = 0;
    uint32_t cursor = 0;
    Vfs64Dirent64 one;
    int guard = 0;
    for (;;) {
        const int r = vfs64_list64(g_path, &one, 1, &cursor);
        if (r <= 0) break;
        if (g_items < EXP_MAX_ITEMS) g_ents[g_items] = one;
        g_items++;
        if (++guard > (int)VFS64_MAX_INODES) break;       // 护栏：绝不无界循环
    }
    g_scroll = clamp_scroll_pure(g_scroll, g_items, 1);
    // 每个条目一行（自动验收按 idx 定位单元格/行；同时把"详细信息"四列的内容也留成证据）
    for (int i = 0; i < g_items && i < EXP_MAX_ITEMS; i++) {
        char mt[32];
        fmt_mtime64(g_ents[i].mtime, mt, (int)sizeof(mt));
        dbg64_line_begin64();
        dbg64_str("[UI] explorer item idx=");
        dbg64_dec((uint64_t)i);
        dbg64_str(" name=");
        dbg64_str(g_ents[i].name);
        dbg64_str(" type=");
        dbg64_str(g_ents[i].type == VFS64_TYPE_DIR ? "dir" : "file");
        dbg64_str(" size=");
        dbg64_dec(g_ents[i].size);
        dbg64_str(" mtime=");
        dbg64_str(mt[0] ? mt : "-");
        dbg64_str(" kind=");
        dbg64_str(vfs64_kind_str64(g_ents[i].kind));
        dbg64_nl();
        dbg64_line_end64();
    }
    exp_log_nav();
}

static void exp_enter_thispc() {
    g_mode = 0;
    g_scroll = 0;
    g_sel = -1;
    exp_scan_drives();
    if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
}

// 进入某个盘符的根目录（只有"当前挂载的卷"能进：vfs64 一次只挂一个卷）
static int exp_enter_drive_letter(char letter) {
    if (!letter) return -1;
    for (int i = 0; i < g_drive_n; i++) {
        if (g_drives[i].letter != letter) continue;
        if (!g_drives[i].browsable) return -1;
        int md = -1, lba = 0;
        Vfs64VolInfo64 vi;
        if (vfs64_mounted_volume64(&md, (uint32_t*)&lba, &vi) == 0) { /* 已挂载 */ }
        g_cur_drive = g_drives[i];

        g_cur_drive_ok = true;
        g_mode = 1;
        e_strcpy(g_path, "/", (int)sizeof(g_path));
        g_scroll = 0;
        exp_refresh_dir();
        dbg64_line_begin64();
        dbg64_str("[UI] explorer drive letter=");
        char l[3]; l[0] = letter; l[1] = ':'; l[2] = 0;
        dbg64_str(l);
        dbg64_str(" fs=");
        dbg64_str(g_drives[i].fs);
        dbg64_str(" total_kb=");
        dbg64_dec(g_drives[i].total_kb);
        dbg64_str(" free_kb=");
        dbg64_dec(g_drives[i].free_kb);
        dbg64_nl();
        dbg64_line_end64();
        if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
        return 0;
    }
    return -1;
}
// 导航 + 历史（push = 1 时压栈）
static int exp_browsable_count() {
    int n = 0;
    for (int i = 0; i < g_drive_n; i++) if (g_drives[i].browsable) n++;
    return n;
}
static void exp_nav_apply(const ExpNav* r) {
    if (r->mode == 0) {
        g_mode = 0;
        e_strcpy(g_path, "/", (int)sizeof(g_path));
        g_items = g_drive_n;
        g_sel = -1;
        g_scroll = 0;
        exp_log_thispc(g_drive_n, exp_browsable_count());   // 回到"此电脑"也要有打点（自动验收据此断言）
    } else {
        g_mode = 1;
        g_letter = r->letter;
        e_strcpy(g_path, r->path, (int)sizeof(g_path));
        if (g_cur_drive_ok) { /* 保持当前盘符信息（状态栏容量显示用） */ }
        g_scroll = 0;
        exp_refresh_dir();
    }
    if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
}
static void exp_nav_to(int mode, char letter, const char* path) {
    ExpNav r;
    r.mode = mode;
    r.letter = letter;
    e_strcpy(r.path, (path && path[0]) ? path : "/", (int)sizeof(r.path));
    hist_push_pure(g_hist, &g_hist_n, &g_hist_pos, &r, EXP_HIST_MAX);
    exp_nav_apply(&r);
}
static void exp_back() {
    if (g_hist_pos <= 0) { exp_msg(gui64_tr("No history entry", "已经没有历史记录")); return; }
    g_hist_pos--;
    exp_nav_apply(&g_hist[g_hist_pos]);
    dbg64_line_begin64();
    dbg64_str("[UI] explorer back path=");
    dbg64_str(g_hist[g_hist_pos].mode == 0 ? "/" : g_hist[g_hist_pos].path);
    dbg64_nl();
    dbg64_line_end64();
}
static void exp_forward() {
    if (g_hist_pos < 0 || g_hist_pos + 1 >= g_hist_n) { exp_msg(gui64_tr("No forward entry", "没有前进记录")); return; }
    g_hist_pos++;
    exp_nav_apply(&g_hist[g_hist_pos]);
    dbg64_line_begin64();
    dbg64_str("[UI] explorer forward path=");
    dbg64_str(g_hist[g_hist_pos].mode == 0 ? "/" : g_hist[g_hist_pos].path);
    dbg64_nl();
    dbg64_line_end64();
}
static void exp_up() {
    if (g_mode == 0) { exp_msg(gui64_tr("Already at This PC", "已经在\"此电脑\"了")); return; }
    char parent[VFS64_PATH_MAX];
    path_parent_pure(g_path, parent, (int)sizeof(parent));
    if (parent[0] == 0) {                       // 根目录的上级 = 此电脑
        exp_nav_to(0, 0, "/");
        dbg64_line_begin64();
        dbg64_str("[UI] explorer up path=/ (this pc)");
        dbg64_nl();
        dbg64_line_end64();
        return;
    }
    exp_nav_to(1, g_letter, parent);
    dbg64_line_begin64();
    dbg64_str("[UI] explorer up path=");
    dbg64_str(parent);
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 打开条目（双击）====================
static void exp_build_path(char* out, int cap, const char* dir, const char* name) {
    e_strcpy(out, dir, cap);
    if (e_strlen(dir) > 1) e_strcat(out, "/", cap);
    e_strcat(out, name, cap);
}
static int exp_assoc(uint32_t type, uint32_t kind) {
    if (type == VFS64_TYPE_DIR) return ASSOC_DIR;
    if (kind == VFS64_KIND_VAP) return ASSOC_VAP;
    if (kind == VFS64_KIND_ELF) return ASSOC_ELF;
    if (kind == VFS64_KIND_TEXT) return ASSOC_TEXT;
    return ASSOC_NONE;
}
static const char* exp_assoc_name(int a) {
    switch (a) {
        case ASSOC_DIR:  return gui64_tr("File folder", "文件夹");
        case ASSOC_VAP:  return gui64_tr("VAP64 application", "VAP64 应用");
        case ASSOC_ELF:  return gui64_tr("ELF64 program", "ELF64 程序");
        case ASSOC_TEXT: return gui64_tr("Text document", "文本文档");
        default:         return gui64_tr("File", "文件");
    }
}

// 绘制用的客户区原点偏移（每个窗口的 draw 回调开始时设置；预览窗口自己用绝对坐标 -> 置 0）
static int g_ox = 0, g_oy = 0;

// 只读文本预览窗口（前 1KB / 20 行）
static void prev_draw(Window* w) {
    const int x = w->client_x + 10;
    int y = w->client_y + 8;
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, rgb(255, 255, 255));
    char hdr[80];
    e_strcpy(hdr, gui64_tr("Read-only preview: ", "只读预览："), (int)sizeof(hdr));
    e_strcat(hdr, g_prev_name, (int)sizeof(hdr));
    text(x, y, hdr, rgb(0, 60, 120));
    y += 20;
    const int lh = 16;
    int line = 0, i = 0;
    while (i <= g_prev_len && line < 20) {
        int j = i;
        while (j < g_prev_len && g_prev_buf[j] != '\n' && (j - i) < 100) j++;
        char tmp[104];
        int k = 0;
        for (int p = i; p < j && k < 100; p++) tmp[k++] = g_prev_buf[p];
        tmp[k] = 0;
        text(x, y, tmp, rgb(40, 40, 40));
        y += lh;
        line++;
        i = (j < g_prev_len && g_prev_buf[j] == '\n') ? j + 1 : j;
        if (i >= g_prev_len) break;
    }
    if (g_prev_len == 0) text(x, y, gui64_tr("(empty file)", "（空文件）"), rgb(120, 120, 120));
}
static void prev_close(Window* w) { (void)w; g_prev_win = nullptr; }
static void exp_open_preview(const char* full, const char* name) {
    g_prev_len = vfs64_read64(full, g_prev_buf, (int)sizeof(g_prev_buf) - 1);
    if (g_prev_len < 0) g_prev_len = 0;
    g_prev_buf[g_prev_len] = 0;
    e_strcpy(g_prev_name, name, (int)sizeof(g_prev_name));
    if (!gui64_window_alive(g_prev_win)) {
        g_prev_win = gui64_create_window(gui64_tr("Text preview", "文本预览"),
                                          PREV_WIN_X, PREV_WIN_Y, PREV_WIN_W, PREV_WIN_H,
                                          prev_draw, nullptr, nullptr, APP_ID_MYPC);
        if (g_prev_win) {
            g_prev_win->on_close = prev_close;
            gui64_set_min_size(g_prev_win, 300, 200);
        }
    } else {
        gui64_set_active(g_prev_win);
    }
    dbg64_line_begin64();
    dbg64_str("[UI] explorer preview name=");
    dbg64_str(name);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)g_prev_len);
    dbg64_nl();
    dbg64_line_end64();
}

static void exp_open_item(int i) {
    if (i < 0 || i >= g_items || i >= EXP_MAX_ITEMS) return;
    const Vfs64Dirent64* d = &g_ents[i];
    const int a = exp_assoc(d->type, d->kind);
    const char* kstr = vfs64_kind_str64(d->kind);
    char full[VFS64_PATH_MAX];
    exp_build_path(full, (int)sizeof(full), g_path, d->name);
    dbg64_line_begin64();
    dbg64_str("[UI] explorer enter name=");
    dbg64_str(d->name);
    dbg64_str(" kind=");
    dbg64_str(kstr);
    dbg64_nl();
    dbg64_line_end64();

    if (a == ASSOC_DIR) {
        exp_nav_to(1, g_letter, full);
        return;
    }
    if (a == ASSOC_VAP) {
        const int rc = app64_launch64(full);
        dbg64_line_begin64();
        dbg64_str("[UI] explorer run name=");
        dbg64_str(d->name);
        dbg64_str(" kind=vap rc=");
        dbg64_dec((uint64_t)(rc < 0 ? 0xFFFFFFFFu : (uint32_t)rc));
        dbg64_nl();
        dbg64_line_end64();
        exp_msg(gui64_tr("Launched VAP64 application", "已运行 VAP64 应用"));
        return;
    }
    if (a == ASSOC_ELF) {
        const int rc = elf64_run64(full);
        dbg64_line_begin64();
        dbg64_str("[UI] explorer run name=");
        dbg64_str(d->name);
        dbg64_str(" kind=elf rc=");
        dbg64_dec((uint64_t)(rc < 0 ? 0xFFFFFFFFu : (uint32_t)rc));
        dbg64_nl();
        dbg64_line_end64();
        exp_msg(gui64_tr("Ran ELF64 program", "已运行 ELF64 程序"));
        return;
    }
    if (a == ASSOC_TEXT) {
        exp_open_preview(full, d->name);
        return;
    }
    dbg64_line_begin64();
    dbg64_str("[UI] explorer noassoc name=");
    dbg64_str(d->name);
    dbg64_str(" kind=");
    dbg64_str(kstr);
    dbg64_nl();
    dbg64_line_end64();
    exp_msg(gui64_tr("There is no application associated with this file",
                     "没有关联的应用打开此文件"));
}

// ---- 绘制坐标约定（重要）：应用回调里一律用**窗口客户区相对坐标**画，这里统一加客户区原点偏移再落到屏幕
//      （外壳的约定：fb_* 用屏幕绝对坐标；见 kernel/gui64.h 的"窗口内容绘制约定"）。----
static void frect(int x, int y, int w, int h, uint32_t c) { fb_fill_rect(g_ox + x, g_oy + y, w, h, c); }
static void drect(int x, int y, int w, int h, uint32_t c) { fb_draw_rect(g_ox + x, g_oy + y, w, h, c); }
static void vline(int x, int y, int h, uint32_t c) { fb_draw_vline(g_ox + x, g_oy + y, h, c); }
static void px(int x, int y, uint32_t c) { fb_putpixel(g_ox + x, g_oy + y, c); }
static void txt(int x, int y, const char* s, uint32_t c) { text(g_ox + x, g_oy + y, s, c); }
static void txt_clip(int x, int y, const char* s, uint32_t c, int maxw) { text_clip(g_ox + x, g_oy + y, s, c, maxw); }

// ==================== 绘制 ====================
static int vis_rows(void) {
    if (g_view == 1) return (EXP_CONTENT_H - 24) / EXP_DET_ROW_H;
    return (EXP_CONTENT_H - 4) / EXP_ICON_CELL_H;
}
static int icon_cols(void) {
    const int c = EXP_CONTENT_W / EXP_ICON_CELL_W;
    return c > 0 ? c : 1;
}
static int per_page(void) {
    return g_view == 1 ? vis_rows() : vis_rows() * icon_cols();
}
static int btn_id_at(int cx, int cy, int* crumb_idx) {
    if (crumb_idx) *crumb_idx = -1;
    if (cy < 0 || cy >= EXP_TOOLBAR_H + EXP_ADDR_H) return IDC_NONE;
    if (cy < EXP_TOOLBAR_H) {                       // 工具栏：固定三按钮
        if (cx >= 8 && cx < 8 + EXP_TB_BTN_W) return IDC_FILE;
        if (cx >= 8 + EXP_TB_BTN_W + 4 && cx < 8 + EXP_TB_BTN_W * 2 + 4) return IDC_COMPUTER;
        if (cx >= 8 + EXP_TB_BTN_W * 2 + 8 && cx < 8 + EXP_TB_BTN_W * 3 + 8) return IDC_VIEW;
        return IDC_NONE;
    }
    if (cy < EXP_TOOLBAR_H + EXP_ADDR_H) {          // 地址栏
        if (cx >= 6 && cx < 32) return IDC_BACK;
        if (cx >= 34 && cx < 60) return IDC_FWD;
        if (cx >= 62 && cx < 88) return IDC_UP;
        if (cx >= 96) {                              // 面包屑（每段可点）
            char seg[48];
            int x = 96;
            // 第 0 段 = 此电脑
            int w0 = tw(gui64_tr("This PC", "此电脑")) + 14;
            if (cx < x + w0) { if (crumb_idx) *crumb_idx = 0; return IDC_CRUMB; }
            x += w0 + 12;
            if (g_mode == 0) return IDC_NONE;
            char lb[24];
            e_strcpy(lb, gui64_tr("Local Disk", "本地磁盘"), (int)sizeof(lb));
            e_strcat(lb, " (", (int)sizeof(lb));
            { char t[3]; t[0] = g_letter; t[1] = ')'; t[2] = 0; e_strcat(lb, t, (int)sizeof(lb)); }
            int w1 = tw(lb) + 14;
            if (cx < x + w1) { if (crumb_idx) *crumb_idx = 1; return IDC_CRUMB; }
            x += w1 + 12;
            const int segs = path_seg_count(g_path);
            for (int k = 1; k <= segs; k++) {
                path_seg(g_path, k, seg, (int)sizeof(seg));
                const int wk = tw(seg) + 14;
                if (cx < x + wk) { if (crumb_idx) *crumb_idx = k + 1; return IDC_CRUMB; }
                x += wk + 12;
                if (x > EXP_CONTENT_X + EXP_CONTENT_W) break;   // 画不下就不再看（有界）
            }
        }
        return IDC_NONE;
    }
    return IDC_NONE;
}
// 面包屑第 k 段 -> 跳转
static void crumb_activate(int k) {
    if (k == 0) { exp_nav_to(0, 0, "/"); return; }
    if (g_mode == 0) return;
    if (k == 1) { exp_nav_to(1, g_letter, "/"); return; }
    char p[VFS64_PATH_MAX];
    p[0] = 0;
    const int want = k - 1;
    for (int i = 1; i <= want; i++) {
        char seg[48];
        path_seg(g_path, i, seg, (int)sizeof(seg));
        if (!seg[0]) return;
        if (p[0]) e_strcat(p, "/", (int)sizeof(p));
        e_strcat(p, seg, (int)sizeof(p));
    }
    if (!p[0]) return;
    exp_nav_to(1, g_letter, p);
}

// 驱动卡片（图标 + 卷标(盘符) + 容量条 + "X 可用，共 Y"）
static void draw_drive_card(const DriveInfo64* d, int cx0, int cy0, int selected) {
    const int x = EXP_CONTENT_X + cx0, y = EXP_CONTENT_Y + cy0;
    frect(x, y, EXP_CARD_W, EXP_CARD_H, C_EXP_CARD);
    drect(x, y, EXP_CARD_W, EXP_CARD_H, selected ? rgb(0, 120, 215) : C_EXP_LINE);
    // 盘图标（几何：蓝灰盘体 + 面）
    frect(x + 12, y + 20, 34, 26, d->browsable ? rgb(120, 144, 176) : rgb(176, 176, 176));
    frect(x + 12, y + 26, 34, 8, rgb(232, 240, 250));
    frect(x + 16, y + 40, 6, 3, rgb(240, 240, 240));
    // 名字
    const uint32_t name_fg = d->browsable ? C_EXP_TEXT : C_EXP_DIM;
    char nm[64];
    if (d->letter) {
        e_strcpy(nm, d->name, (int)sizeof(nm));
        e_strcat(nm, " (", (int)sizeof(nm));
        char t[3]; t[0] = d->letter; t[1] = ')'; t[2] = 0;
        e_strcat(nm, t, (int)sizeof(nm));
    } else {
        e_strcpy(nm, d->name, (int)sizeof(nm));
    }
    txt_clip(x + 58, y + 10, nm, name_fg, EXP_CARD_W - 70);
    // 容量条
    const int bx = x + 58, by = y + 34;
    frect(bx, by, EXP_BAR_W, EXP_BAR_H, C_EXP_BAR_BG);
    drect(bx, by, EXP_BAR_W, EXP_BAR_H, C_EXP_LINE);
    int fill = 0;
    if (d->browsable && d->total_known && d->free_known && d->total_kb > 0) {
        const uint64_t used = (d->total_kb > d->free_kb) ? (d->total_kb - d->free_kb) : 0;
        if (used > 0) {
            fill = (int)((used * (uint64_t)(EXP_BAR_W - 2)) / d->total_kb);
            if (fill < 4) fill = 4;                     // 已用非 0 就给最小可见宽度（Win10 也是这样）
            if (fill > EXP_BAR_W - 2) fill = EXP_BAR_W - 2;
        }
    }
    if (fill > 0) frect(bx + 1, by + 1, fill, EXP_BAR_H - 2, C_EXP_BAR_FILL);
    // 容量文字
    char txt[96];
    txt[0] = 0;
    if (d->browsable && d->total_known && d->free_known) {
        char a[32], b[32];
        fmt_kb64(d->free_kb, a, (int)sizeof(a));
        fmt_kb64(d->total_kb, b, (int)sizeof(b));
        e_strcpy(txt, a, (int)sizeof(txt));
        e_strcat(txt, gui64_tr(" free, ", " 可用，共 "), (int)sizeof(txt));
        e_strcat(txt, b, (int)sizeof(txt));
    } else if (d->total_known) {
        char b[32];
        fmt_kb64(d->total_kb, b, (int)sizeof(b));
        e_strcat(txt, b, (int)sizeof(txt));
    } else {
        e_strcpy(txt, gui64_tr("(capacity unknown)", "（容量未知）"), (int)sizeof(txt));
    }
    e_strcat(txt, "  ", (int)sizeof(txt));
    e_strcat(txt, d->fs, (int)sizeof(txt));
    txt_clip(x + 58, y + 52, txt, C_EXP_DIM, EXP_CARD_W - 70);
    // 不可浏览的条目：灰字说明原因（不假装能点进去）
    if (!d->browsable) {
        const char* why = (d->skip == DRV64_SKIP_ESP)
            ? gui64_tr("EFI System Partition (not browsable)", "EFI 系统分区（不浏览）")
            : gui64_tr("Unknown file system (not browsable)", "未识别文件系统（不浏览）");
        txt_clip(x + 58, y + 52 + 18, why, C_EXP_DIM, EXP_CARD_W - 70);
    }
}

// 条目图标（几何画法；不做位图资源）
static void draw_item_icon(int x, int y, const Vfs64Dirent64* d) {
    const int s = EXP_ICON_SIZE;
    if (d->type == VFS64_TYPE_DIR) {                       // 文件夹：琥珀色 + 标签
        frect(x, y + 4, s, s - 6, rgb(240, 190, 80));
        frect(x, y + 4, 14, 6, rgb(214, 160, 56));
        drect(x, y + 4, s, s - 6, rgb(190, 140, 40));
        frect(x + 2, y + 12, s - 4, 3, rgb(255, 222, 150));
    } else if (d->kind == VFS64_KIND_VAP) {                // VAP64：蓝方块 + 播放三角
        frect(x + 2, y + 3, s - 4, s - 6, rgb(0, 120, 215));
        drect(x + 2, y + 3, s - 4, s - 6, rgb(0, 80, 150));
        for (int i = 0; i < 10; i++)
            vline(x + 12 + i, y + 12 - i / 2, 1 + i, rgb(255, 255, 255));
    } else if (d->kind == VFS64_KIND_ELF) {                // ELF64：绿方块 + ">_"
        frect(x + 2, y + 3, s - 4, s - 6, rgb(40, 150, 90));
        drect(x + 2, y + 3, s - 4, s - 6, rgb(24, 100, 60));
        frect(x + 7, y + 13, 3, 3, rgb(255, 255, 255));
        frect(x + 11, y + 16, 3, 3, rgb(255, 255, 255));
        frect(x + 15, y + 19, 3, 3, rgb(255, 255, 255));
        frect(x + 19, y + 22, 7, 2, rgb(255, 255, 255));
    } else if (d->kind == VFS64_KIND_TEXT) {               // 文本：白纸 + 灰线
        frect(x + 4, y + 2, s - 8, s - 4, rgb(255, 255, 255));
        drect(x + 4, y + 2, s - 8, s - 4, rgb(160, 160, 160));
        for (int i = 0; i < 5; i++) frect(x + 7, y + 7 + i * 4, s - 14, 1, rgb(150, 150, 150));
    } else {                                               // 二进制 / 未知：灰纸 + 点
        frect(x + 4, y + 2, s - 8, s - 4, rgb(238, 238, 238));
        drect(x + 4, y + 2, s - 8, s - 4, rgb(150, 150, 150));
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                px(x + 8 + i * 4, y + 8 + j * 4, rgb(140, 140, 140));
    }
}

static void draw_content(void) {
    const int ax = EXP_CONTENT_X, ay = EXP_CONTENT_Y;
    const int aw = EXP_CONTENT_W, ah = EXP_CONTENT_H;
    frect(ax, ay, aw, ah, rgb(255, 255, 255));
    if (g_mode == 0) {
        // ---- 此电脑：设备和驱动器 (N) ----
        char hdr[64];
        e_strcpy(hdr, gui64_tr("Devices and drives", "设备和驱动器"), (int)sizeof(hdr));
        e_strcat(hdr, " (", (int)sizeof(hdr));
        { char t[8]; u2s(t, (uint64_t)g_items); e_strcat(hdr, t, (int)sizeof(hdr)); }
        e_strcat(hdr, ")", (int)sizeof(hdr));
        txt(ax + 10, ay + 6, hdr, rgb(0, 60, 120));
        int shown = 0;
        const int rows = (ah - 30) / (EXP_CARD_H + EXP_CARD_GAP) + 1;
        for (int i = g_scroll; i < g_drive_n && shown < rows + 1; i++, shown++) {
            const int cy = EXP_CARD_TOP - EXP_CONTENT_Y + (i - g_scroll) * (EXP_CARD_H + EXP_CARD_GAP);
            if (cy + EXP_CARD_H > ah) break;
            draw_drive_card(&g_drives[i], 10, cy, i == g_sel);
        }
        if (g_drive_n == 0) txt(ax + 12, ay + 40, gui64_tr("No drives found", "没有找到驱动器"), C_EXP_DIM);
    } else if (g_view == 0) {
        // ---- 图标视图 ----
        const int cols = icon_cols();
        const int rows = vis_rows();
        for (int i = g_scroll; i < g_items && i < EXP_MAX_ITEMS; i++) {
            const int k = i - g_scroll;
            const int col = k % cols, row = k / cols;
            if (row >= rows) break;
            const int cx = ax + col * EXP_ICON_CELL_W;
            const int cy = ay + 2 + row * EXP_ICON_CELL_H;
            if (i == g_sel) frect(cx + 2, cy, EXP_ICON_CELL_W - 4, EXP_ICON_CELL_H - 6, C_EXP_SEL);
            draw_item_icon(cx + (EXP_ICON_CELL_W - EXP_ICON_SIZE) / 2, cy + 6, &g_ents[i]);
            txt_clip(cx + 4, cy + 44, g_ents[i].name, C_EXP_TEXT, EXP_ICON_CELL_W - 8);
        }
    } else {
        // ---- 详细信息：名称 / 修改日期 / 类型 / 大小（四列 + 分隔线）----
        frect(ax, EXP_DET_HEAD_Y, aw, EXP_DET_HEAD_H, C_EXP_HEAD);
        frect(ax, EXP_DET_HEAD_Y + EXP_DET_HEAD_H - 1, aw, 1, C_EXP_LINE);
        const int seps[3] = { EXP_COL_NAME_X + EXP_COL_NAME_W, EXP_COL_DATE_X + EXP_COL_DATE_W,
                              EXP_COL_TYPE_X + EXP_COL_TYPE_W };
        for (int i = 0; i < 3; i++)
            frect(ax + seps[i], EXP_DET_HEAD_Y, 1, EXP_DET_HEAD_H, C_EXP_SEP);
        txt(ax + EXP_COL_NAME_X + 3, EXP_DET_HEAD_Y + 3, gui64_tr("Name", "名称"), C_EXP_DIM);
        txt(ax + EXP_COL_DATE_X + 3, EXP_DET_HEAD_Y + 3, gui64_tr("Date modified", "修改日期"), C_EXP_DIM);
        txt(ax + EXP_COL_TYPE_X + 3, EXP_DET_HEAD_Y + 3, gui64_tr("Type", "类型"), C_EXP_DIM);
        txt(ax + EXP_COL_SIZE_X + 3, EXP_DET_HEAD_Y + 3, gui64_tr("Size", "大小"), C_EXP_DIM);
        const int rows = vis_rows();
        for (int i = g_scroll; i < g_items && i < EXP_MAX_ITEMS; i++) {
            const int r = i - g_scroll;
            if (r >= rows) break;
            const int ry = EXP_DET_ROW_Y + r * EXP_DET_ROW_H;
            if (i == g_sel) frect(ax + 1, ry, aw - 2, EXP_DET_ROW_H, C_EXP_SEL);
            const Vfs64Dirent64* d = &g_ents[i];
            char date[32], size[32];
            fmt_mtime64(d->mtime, date, (int)sizeof(date));
            if (d->type == VFS64_TYPE_DIR) e_strcpy(size, "-", (int)sizeof(size));
            else fmt_bytes64(d->size, size, (int)sizeof(size));
            txt_clip(ax + EXP_COL_NAME_X + 3, ry + 3, d->name, C_EXP_TEXT, EXP_COL_NAME_W - 6);
            txt(ax + EXP_COL_DATE_X + 3, ry + 3, date, C_EXP_DIM);
            txt_clip(ax + EXP_COL_TYPE_X + 3, ry + 3, exp_assoc_name(exp_assoc(d->type, d->kind)),
                      C_EXP_DIM, EXP_COL_TYPE_W - 6);
            txt(ax + EXP_COL_SIZE_X + 3, ry + 3, size, C_EXP_DIM);
        }
    }
    // 滚动条（有界；点击上下半页）
    if (g_items > per_page()) {
        const int sbx = ax + aw - 12, sby = ay + 2, sbh = ah - 4;
        frect(sbx, sby, 12, sbh, rgb(245, 245, 245));
        drect(sbx, sby, 12, sbh, C_EXP_LINE);
        int th = sbh * per_page() / (g_items > 0 ? g_items : 1);
        if (th < 20) th = 20;
        if (th > sbh) th = sbh;
        const int maxs = g_items - per_page();
        const int ty = sby + (maxs > 0 ? (sbh - th) * g_scroll / maxs : 0);
        frect(sbx + 2, ty + 1, 8, (th > 2 ? th - 2 : th), rgb(190, 200, 210));
        frect(sbx + 3, sby + 2, 6, 8, rgb(160, 160, 160));            // 上箭头
        frect(sbx + 3, sby + sbh - 10, 6, 8, rgb(160, 160, 160));     // 下箭头
    }
    frect(ax, ay + ah - 1, aw, 1, C_EXP_LINE);      // 内容区底边
}

static void draw_toolbar(void) {
    frect(0, 0, EXP_CONTENT_X, EXP_TOOLBAR_H, C_EXP_TOOLBAR);
    frect(0, EXP_TOOLBAR_H - 1, EXP_WIN_W - 2, 1, C_EXP_LINE);
    const char* labels[3];
    labels[0] = gui64_tr("File", "文件");
    labels[1] = gui64_tr("Computer", "计算机");
    labels[2] = gui64_tr("View", "查看");
    const int ids[3] = { IDC_FILE, IDC_COMPUTER, IDC_VIEW };
    for (int i = 0; i < 3; i++) {
        const int x = 8 + i * (EXP_TB_BTN_W + 4);
        uint32_t bg = C_EXP_TOOLBAR;
        if (g_press_id == ids[i]) bg = C_EXP_BTN_PRESS;
        else if (g_hover_id == ids[i]) bg = C_EXP_BTN_HOV;
        if (bg != C_EXP_TOOLBAR) frect(x, 3, EXP_TB_BTN_W, EXP_TOOLBAR_H - 7, bg);
        const int lw = tw(labels[i]);
        txt(x + (EXP_TB_BTN_W - lw) / 2, 6, labels[i], C_EXP_TEXT);
    }
    // 状态栏提示（点"文件"菜单里的项 / 打不开的文件时给可见反馈）
    if (g_msg[0] && (int32_t)(ticks64() - g_msg_tick) < 0)
        txt(EXP_TB_BTN_W * 3 + 24, 6, g_msg, rgb(160, 60, 60));
}
static void draw_addr(void) {
    const int y0 = EXP_TOOLBAR_H;
    frect(0, y0, EXP_CONTENT_X, EXP_ADDR_H, C_EXP_BG);
    const int ids[3] = { IDC_BACK, IDC_FWD, IDC_UP };
    for (int i = 0; i < 3; i++) {
        const int x = 6 + i * 28;
        uint32_t bg = rgb(232, 232, 232);
        if (g_press_id == ids[i]) bg = C_EXP_BTN_PRESS;
        else if (g_hover_id == ids[i]) bg = C_EXP_BTN_HOV;
        frect(x, y0 + 3, 26, EXP_ADDR_H - 6, bg);
        drect(x, y0 + 3, 26, EXP_ADDR_H - 6, C_EXP_LINE);
        const int cx = x + 13, cy = y0 + EXP_ADDR_H / 2;
        const uint32_t gl = (i == 2) ? rgb(0, 90, 160) : ((i == 0 && g_hist_pos <= 0) || (i == 1 && g_hist_pos + 1 >= g_hist_n) ? rgb(170, 170, 170) : rgb(0, 90, 160));
        if (i == 0) { frect(cx - 4, cy - 5, 2, 11, gl); for (int k = 0; k < 5; k++) frect(cx - 2 + k, cy - 4 + k, 1, 9 - 2 * k, gl); }
        else if (i == 1) { frect(cx + 2, cy - 5, 2, 11, gl); for (int k = 0; k < 5; k++) frect(cx + 1 - k, cy - 4 + k, 1, 9 - 2 * k, gl); }
        else { for (int k = 0; k < 6; k++) frect(cx - 5 + k, cy + 4 - k, 11 - 2 * k, 1, gl); }
    }
    // 面包屑（白底 + 边框 + 每段一个可点区域）
    const int bx = 96, bw = EXP_CONTENT_X + EXP_CONTENT_W - 96 - 6;
    frect(bx, y0 + 3, bw, EXP_ADDR_H - 6, C_EXP_ADDR);
    drect(bx, y0 + 3, bw, EXP_ADDR_H - 6, C_EXP_LINE);
    int x = bx + 7;
    const int ty = y0 + 8;
    char nm[64];
    e_strcpy(nm, gui64_tr("This PC", "此电脑"), (int)sizeof(nm));
    txt(x, ty, nm, C_EXP_TEXT);
    x += tw(nm) + 7;
    if (x < bx + bw - 12) { txt(x, ty, ">", C_EXP_DIM); x += 12; }
    if (g_mode == 1) {
        e_strcpy(nm, gui64_tr("Local Disk", "本地磁盘"), (int)sizeof(nm));
        e_strcat(nm, " (", (int)sizeof(nm));
        { char t[3]; t[0] = g_letter; t[1] = ')'; t[2] = 0; e_strcat(nm, t, (int)sizeof(nm)); }
        if (x + tw(nm) > bx + bw - 12) { e_strcpy(nm, gui64_tr("Disk", "磁盘"), (int)sizeof(nm)); }
        txt(x, ty, nm, C_EXP_TEXT);
        x += tw(nm) + 7;
        const int segs = path_seg_count(g_path);
        for (int k = 1; k <= segs; k++) {
            if (x > bx + bw - 24) break;
            char seg[48];
            path_seg(g_path, k, seg, (int)sizeof(seg));
            if (x + tw(seg) > bx + bw - 12) break;
            if (x + 12 < bx + bw - 12) { txt(x, ty, ">", C_EXP_DIM); x += 12; }
            txt(x, ty, seg, C_EXP_TEXT);
            x += tw(seg) + 7;
        }
    }
}
static void draw_nav(void) {
    const int nh = EXP_CONTENT_Y + EXP_CONTENT_H - (EXP_TOOLBAR_H + EXP_ADDR_H);
    frect(0, EXP_CONTENT_Y, EXP_NAV_W, nh, C_EXP_NAV);
    frect(EXP_NAV_W, EXP_CONTENT_Y, 1, nh, C_EXP_LINE);
    txt(8, EXP_CONTENT_Y + 6, gui64_tr("Quick access", "快速访问"), C_EXP_DIM);
    // 节点 0 = 此电脑；节点 1..n = 每个驱动器（可浏览的带盘符，不可浏览的灰字）
    for (int i = 0; i <= g_drive_n; i++) {
        const int y = EXP_CONTENT_Y + EXP_NAV_TOP + i * EXP_NAV_ROW_H;
        if (y + EXP_NAV_ROW_H > EXP_CONTENT_Y + nh) break;
        const bool is_thispc = (i == 0);
        const DriveInfo64* d = is_thispc ? nullptr : &g_drives[i - 1];
        const bool sel = is_thispc ? (g_mode == 0)
                                   : (g_mode == 1 && d && d->letter && d->letter == g_letter);
        if (sel) frect(2, y, EXP_NAV_W - 5, EXP_NAV_ROW_H - 2, C_EXP_NAV_SEL);
        // 小图标：此电脑 = 蓝灰方块；驱动器 = 盘形
        if (is_thispc) {
            frect(8, y + 4, 12, 9, rgb(120, 144, 176));
            frect(6, y + 12, 16, 2, rgb(90, 110, 140));
        } else {
            frect(8, y + 6, 13, 9, d->browsable ? rgb(120, 144, 176) : rgb(180, 180, 180));
            frect(8, y + 9, 13, 3, rgb(232, 240, 250));
        }
        char lb[64];
        if (is_thispc) {
            e_strcpy(lb, gui64_tr("This PC", "此电脑"), (int)sizeof(lb));
        } else {
            e_strcpy(lb, d->name, (int)sizeof(lb));
            if (d->letter) {
                e_strcat(lb, " (", (int)sizeof(lb));
                char t[3]; t[0] = d->letter; t[1] = ')'; t[2] = 0;
                e_strcat(lb, t, (int)sizeof(lb));
            }
        }
        txt_clip(26, y + 4, lb, d->browsable || is_thispc ? C_EXP_TEXT : C_EXP_DIM, EXP_NAV_W - 30);
    }
}
static void draw_status(void) {
    const int y = EXP_CONTENT_Y + EXP_CONTENT_H;
    frect(0, y, EXP_WIN_W - 2, EXP_STATUS_H, C_EXP_STATUS);
    frect(0, y, EXP_WIN_W - 2, 1, C_EXP_LINE);
    char buf[96];
    e_strcpy(buf, "", (int)sizeof(buf));
    if (g_mode == 1) {
        char t[16];
        u2s(t, (uint64_t)g_items);
        e_strcat(buf, t, (int)sizeof(buf));
        e_strcat(buf, gui64_tr(" items", " 个项目"), (int)sizeof(buf));
    } else {
        char t[16];
        u2s(t, (uint64_t)g_items);
        e_strcat(buf, t, (int)sizeof(buf));
        e_strcat(buf, gui64_tr(" drives", " 个驱动器"), (int)sizeof(buf));
    }
    if (g_mode == 1 && g_sel >= 0 && g_sel < g_items && g_sel < EXP_MAX_ITEMS) {
        e_strcat(buf, gui64_tr("   |  selected: ", "   |  选中："), (int)sizeof(buf));
        e_strcat(buf, g_ents[g_sel].name, (int)sizeof(buf));
        if (g_ents[g_sel].type == VFS64_TYPE_FILE) {
            char s[32];
            fmt_bytes64(g_ents[g_sel].size, s, (int)sizeof(s));
            e_strcat(buf, "  ", (int)sizeof(buf));
            e_strcat(buf, s, (int)sizeof(buf));
        }
    }
    txt(8, y + 4, buf, C_EXP_DIM);
    // 当前盘符容量（Win10 状态栏右侧也有容量信息）
    if (g_mode == 1 && g_cur_drive_ok && g_cur_drive.total_known && g_cur_drive.free_known) {
        char a[32], b[32], r[96];
        fmt_kb64(g_cur_drive.free_kb, a, (int)sizeof(a));
        fmt_kb64(g_cur_drive.total_kb, b, (int)sizeof(b));
        e_strcpy(r, a, (int)sizeof(r));
        e_strcat(r, gui64_tr(" free of ", " 可用，共 "), (int)sizeof(r));
        e_strcat(r, b, (int)sizeof(r));
        txt_clip(EXP_CONTENT_X + EXP_CONTENT_W - tw(r) - 6, y + 4, r, C_EXP_DIM, EXP_CONTENT_W);
    }
}
static void draw_filemenu(void) {
    if (!g_menu_open) return;
    const int x = 8, y = EXP_TOOLBAR_H;
    const char* items[2];
    items[0] = gui64_tr("Open Terminal", "打开终端");
    items[1] = gui64_tr("Close window", "关闭窗口");
    frect(x, y, 170, 56, C_EXP_MENU);
    drect(x, y, 170, 56, C_EXP_LINE);
    for (int i = 0; i < 2; i++) {
        const int iy = y + 2 + i * 26;
        if (g_hover_id == IDC_FILE + 10 + i) frect(x + 2, iy, 166, 24, C_EXP_MENU_HOV);
        txt(x + 10, iy + 5, items[i], C_EXP_TEXT);
    }
}
static void exp_draw(Window* w) {
    g_ox = w->client_x;             // 客户区相对坐标 -> 屏幕坐标（见上面的绘制坐标约定）
    g_oy = w->client_y;
    // 按下反馈到期 -> 清掉并标脏（"点击有可见反馈"的收尾）
    if (g_press_id != IDC_NONE && (int32_t)(ticks64() - g_press_tick) >= 0) {
        g_press_id = IDC_NONE;
        gui64_dirty(w->client_x, w->client_y, w->client_w, EXP_TOOLBAR_H + EXP_ADDR_H);
    }
    fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, C_EXP_BG);   // 绝对坐标：客户区底色
    // hover 计算（鼠标在客户区内才高亮；状态变化时把两处按钮都标脏）
    int hov = IDC_NONE;
    {
        const int mx = mouse_get_x() - w->client_x;
        const int my = mouse_get_y() - w->client_y;
        if (mx >= 0 && my >= 0 && mx < w->client_w && my < w->client_h) {
            int ci = -1;
            hov = btn_id_at(mx, my, &ci);
            if (hov == IDC_NONE && g_menu_open && mx >= 8 && mx < 178 && my >= EXP_TOOLBAR_H && my < EXP_TOOLBAR_H + 56)
                hov = IDC_FILE + 10 + (my - EXP_TOOLBAR_H - 2) / 26;
        }
    }
    if (hov != g_hover_id) {
        g_hover_id = hov;
        gui64_dirty(w->client_x, w->client_y, w->client_w, EXP_TOOLBAR_H + EXP_ADDR_H);
        if (hov >= IDC_FILE + 10) gui64_dirty(w->client_x + 8, w->client_y + EXP_TOOLBAR_H, 176, 60);
    }
    // 内容区（自己再设一次裁剪：内容不得溢出到导航窗格/状态栏）
    fb_set_clip(w->client_x + EXP_CONTENT_X, w->client_y + EXP_CONTENT_Y, EXP_CONTENT_W, EXP_CONTENT_H);
    draw_content();
    fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);
    draw_toolbar();
    draw_addr();
    draw_nav();
    draw_filemenu();
    draw_status();
    fb_set_clip(w->client_x, w->client_y, w->client_w, w->client_h);   // 把外壳的客户区裁剪还回去（勿 reset）
}

// ==================== 命中测试 / 点击 ====================
static int hit_card(int cx, int cy) {
    if (g_mode != 0) return -1;
    if (cx < EXP_CARD_X || cx >= EXP_CARD_X + EXP_CARD_W) return -1;
    for (int i = g_scroll; i < g_drive_n; i++) {
        const int y = EXP_CARD_TOP + (i - g_scroll) * (EXP_CARD_H + EXP_CARD_GAP);
        if (cy >= y && cy < y + EXP_CARD_H) return i;
        if (cy < y) break;
    }
    return -1;
}
static int hit_item(int cx, int cy) {
    if (g_mode != 1) return -1;
    const int rx = cx - EXP_CONTENT_X, ry = cy - EXP_CONTENT_Y;
    if (rx < 0 || ry < 0 || rx >= EXP_CONTENT_W || ry >= EXP_CONTENT_H) return -1;
    if (g_view == 0) {
        if (ry < 2) return -1;
        const int cols = icon_cols();
        const int col = rx / EXP_ICON_CELL_W, row = (ry - 2) / EXP_ICON_CELL_H;
        const int k = row * cols + col;
        const int i = g_scroll + k;
        if (k < 0 || i >= g_items) return -1;
        return i;
    }
    if (ry < EXP_DET_ROW_Y - EXP_CONTENT_Y) return -1;
    const int row = (ry - (EXP_DET_ROW_Y - EXP_CONTENT_Y)) / EXP_DET_ROW_H;
    const int i = g_scroll + row;
    if (i >= g_items) return -1;
    return i;
}
static int hit_nav(int cx, int cy, int* out_kind) {
    // 返回节点下标（0 = 此电脑，1..n = 驱动器）；-1 = 没命中。out_kind: 0 此电脑 1 可浏览盘 2 不可浏览
    if (out_kind) *out_kind = 0;
    if (cx < 0 || cx >= EXP_NAV_W) return -1;
    const int ry = cy - (EXP_CONTENT_Y + EXP_NAV_TOP);
    if (ry < 0) return -1;
    const int i = ry / EXP_NAV_ROW_H;
    if (i > g_drive_n) return -1;
    if (i == 0) return 0;
    const int di = i - 1;
    if (out_kind) *out_kind = g_drives[di].browsable ? 1 : 2;
    return i;
}

static void exp_click(Window* w, int cx, int cy) {
    const int sx = w->client_x + cx, sy = w->client_y + cy;
    int crumb = -1;
    int id = btn_id_at(cx, cy, &crumb);
    char hit[24];
    e_strcpy(hit, "none", (int)sizeof(hit));
    if (id == IDC_CRUMB) { e_strcpy(hit, "crumb:", (int)sizeof(hit)); { char t[8]; u2s(t, (uint64_t)crumb); e_strcat(hit, t, (int)sizeof(hit)); } }
    else if (id == IDC_FILE) e_strcpy(hit, "btn:file", (int)sizeof(hit));
    else if (id == IDC_COMPUTER) e_strcpy(hit, "btn:computer", (int)sizeof(hit));
    else if (id == IDC_VIEW) e_strcpy(hit, "btn:view", (int)sizeof(hit));
    else if (id == IDC_BACK) e_strcpy(hit, "btn:back", (int)sizeof(hit));
    else if (id == IDC_FWD) e_strcpy(hit, "btn:fwd", (int)sizeof(hit));
    else if (id == IDC_UP) e_strcpy(hit, "btn:up", (int)sizeof(hit));
    if (id == IDC_NONE) {
        const int ic = hit_card(cx, cy);
        const int ii = hit_item(cx, cy);
        int nk = 0;
        const int ni = hit_nav(cx, cy, &nk);
        if (ic >= 0) { e_strcpy(hit, "card:", (int)sizeof(hit)); { char t[8]; u2s(t, (uint64_t)ic); e_strcat(hit, t, (int)sizeof(hit)); } }
        else if (ii >= 0) { e_strcpy(hit, "item:", (int)sizeof(hit)); { char t[8]; u2s(t, (uint64_t)ii); e_strcat(hit, t, (int)sizeof(hit)); } }
        else if (ni >= 0) { e_strcpy(hit, "nav:", (int)sizeof(hit)); { char t[8]; u2s(t, (uint64_t)ni); e_strcat(hit, t, (int)sizeof(hit)); } }
    }
    exp_log_click(cx, cy, sx, sy, hit);
    g_press_id = (id != IDC_NONE) ? id : IDC_NONE;
    g_press_tick = ticks64() + ms_to_ticks64(120);      // 120ms 的按下反馈
    const uint32_t now = ticks64();
    const bool dbl_ok = (int32_t)(now - g_last_tick) <= (int32_t)ms_to_ticks64(EXP_CLICK_MS);

    // ---- 文件菜单打开时优先处理菜单 ----
    if (g_menu_open) {
        const int mx = cx, my = cy;
        if (mx >= 8 && mx < 178 && my >= EXP_TOOLBAR_H && my < EXP_TOOLBAR_H + 56) {
            const int it = (my - EXP_TOOLBAR_H - 2) / 26;
            g_menu_open = 0;
            dbg64_line_begin64();
            dbg64_str("[UI] explorer menu item=");
            dbg64_dec((uint64_t)it);
            dbg64_nl();
            dbg64_line_end64();
            if (it == 0) { app_term_open64(); return; }
            if (it == 1) { gui64_destroy_window(w); return; }
            return;
        }
        g_menu_open = 0;
        gui64_invalidate_window(w);
        return;
    }
    // ---- 工具栏 / 地址栏 ----
    if (id == IDC_FILE) { g_menu_open = 1; gui64_invalidate_window(w); return; }
    if (id == IDC_COMPUTER) { exp_nav_to(0, 0, "/"); return; }
    if (id == IDC_VIEW) {
        g_view = g_view ? 0 : 1;
        g_scroll = clamp_scroll_pure(g_scroll, g_items, per_page());
        exp_log_view();
        gui64_invalidate_window(w);
        return;
    }
    if (id == IDC_BACK) { exp_back(); return; }
    if (id == IDC_FWD) { exp_forward(); return; }
    if (id == IDC_UP) { exp_up(); return; }
    if (id == IDC_CRUMB) { crumb_activate(crumb); return; }

    // ---- 导航窗格 ----
    {
        int nk = 0;
        const int ni = hit_nav(cx, cy, &nk);
        if (ni >= 0) {
            if (ni == 0) { exp_nav_to(0, 0, "/"); return; }
            const DriveInfo64* d = &g_drives[ni - 1];
            if (!d->browsable) {
                exp_msg(gui64_tr("This entry is not browsable", "该条目不可浏览"));
                dbg64_line_begin64();
                dbg64_str("[UI] explorer nav skip name=");
                dbg64_str(d->name);
                dbg64_str(" reason=not-browsable");
                dbg64_nl();
                dbg64_line_end64();
                return;
            }
            if (d->letter != g_letter || g_mode == 0) {
                if (exp_enter_drive_letter(d->letter) == 0) exp_nav_to(1, d->letter, "/");
            }
            return;
        }
    }
    // ---- 内容区 ----
    if (g_mode == 0) {
        const int ci = hit_card(cx, cy);
        if (ci < 0) { g_sel = -1; return; }
        g_sel = ci;
        const DriveInfo64* d = &g_drives[ci];
        if (dbl_ok && g_last_item == ci) {
            g_last_item = -1;
            if (!d->browsable) {
                exp_msg((d->skip == DRV64_SKIP_ESP)
                        ? gui64_tr("EFI System Partition: not browsable",
                                   "EFI 系统分区：不浏览（避免碰坏引导）")
                        : gui64_tr("Unknown file system: not browsable", "未识别文件系统：不浏览"));
                return;
            }
            if (exp_enter_drive_letter(d->letter) == 0) exp_nav_to(1, d->letter, "/");
            return;
        }
        g_last_item = ci;
        g_last_tick = now;
        gui64_invalidate_window(w);
        return;
    }
    // 目录视图：滚动条 / 条目
    {
        const int rx = cx - EXP_CONTENT_X, ry = cy - EXP_CONTENT_Y;
        if (g_items > per_page() && rx >= EXP_CONTENT_W - 12 && ry >= 0 && ry < EXP_CONTENT_H) {
            int s = g_scroll;
            if (ry < 14) s -= 1;                               // 上箭头
            else if (ry > EXP_CONTENT_H - 14) s += 1;          // 下箭头
            else if (ry < EXP_CONTENT_H / 2) s -= per_page();  // 上半页
            else s += per_page();
            s = clamp_scroll_pure(s, g_items, per_page());
            if (s != g_scroll) {
                g_scroll = s;
                dbg64_line_begin64();
                dbg64_str("[UI] explorer scroll top=");
                dbg64_dec((uint64_t)g_scroll);
                dbg64_str(" items=");
                dbg64_dec((uint64_t)g_items);
                dbg64_nl();
                dbg64_line_end64();
            }
            gui64_invalidate_window(w);
            return;
        }
        const int ii = hit_item(cx, cy);
        if (ii < 0) { g_sel = -1; gui64_invalidate_window(w); return; }
        g_sel = ii;
        if (dbl_ok && g_last_item == ii) {
            g_last_item = -1;
            exp_open_item(ii);
            gui64_invalidate_window(w);
            return;
        }
        g_last_item = ii;
        g_last_tick = now;
        exp_msg("");                                   // 单击条目 = 清掉上一条提示
        gui64_invalidate_window(w);
    }
}

// ==================== 键盘（上下 = 选择/滚动；回车 = 打开；左右 = 后退/前进）====================
static void exp_select_move(int delta) {
    if (g_items <= 0) return;
    int s = (g_sel < 0) ? 0 : g_sel + delta;
    if (s < 0) s = 0;
    if (s > g_items - 1) s = g_items - 1;
    g_sel = s;
    const int pp = per_page();
    int sc = g_scroll;
    if (g_sel < sc) sc = g_sel;
    if (g_sel >= sc + pp) sc = g_sel - pp + 1;
    const int nsc = clamp_scroll_pure(sc, g_items, pp);
    if (nsc != g_scroll) {
        g_scroll = nsc;
        dbg64_line_begin64();
        dbg64_str("[UI] explorer scroll top=");
        dbg64_dec((uint64_t)g_scroll);
        dbg64_str(" items=");
        dbg64_dec((uint64_t)g_items);
        dbg64_nl();
        dbg64_line_end64();
    }
    if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
}
static void exp_key(Window* w, char c) {
    (void)w;
    switch ((unsigned char)c) {
        case 0xFD: exp_select_move(-1); break;         // NAV_UP
        case 0xFE: exp_select_move(+1); break;         // NAV_DOWN
        case 0xFB: exp_back(); break;                  // NAV_LEFT
        case 0xFC: exp_forward(); break;               // NAV_RIGHT
        case '\t':                                     // Tab = 图标视图 <-> 详细信息视图（键盘等同"查看"）
            g_view = g_view ? 0 : 1;
            g_scroll = clamp_scroll_pure(g_scroll, g_items, per_page());
            exp_log_view();
            if (gui64_window_alive(g_win)) gui64_invalidate_window(g_win);
            break;
        case '\n': case '\r':
            if (g_items > 0 && g_sel < 0) g_sel = g_scroll;
            if (g_mode == 0) {
                if (g_sel >= 0 && g_sel < g_drive_n && g_drives[g_sel].browsable) {
                    if (exp_enter_drive_letter(g_drives[g_sel].letter) == 0)
                        exp_nav_to(1, g_drives[g_sel].letter, "/");
                }
            } else if (g_sel >= 0) {
                exp_open_item(g_sel);
            }
            break;
        default: break;
    }
}
static void exp_close(Window* w) {
    (void)w;
    g_win = nullptr;
    g_menu_open = 0;
    g_last_item = -1;
    dbg64_str("[APP] mypc closed");
    dbg64_nl();
}

// ==================== 公共 API ====================
Window* explorer64_window64() { return gui64_window_alive(g_win) ? g_win : nullptr; }

void explorer64_open64() {
    if (gui64_window_alive(g_win)) { gui64_set_active(g_win); gui64_invalidate_window(g_win); return; }
    g_view = 0;
    g_menu_open = 0;
    g_last_item = -1;
    g_hist_n = 0;
    g_hist_pos = -1;
    g_win = gui64_create_window(gui64_tr("File Explorer - This PC", "文件资源管理器 - 此电脑"),
                                EXP_WIN_X, EXP_WIN_Y, EXP_WIN_W, EXP_WIN_H,
                                exp_draw, exp_key, exp_click, APP_ID_MYPC);
    if (!g_win) {
        dbg64_str("[UI] explorer open failed: no window slot");
        dbg64_nl();
        return;
    }
    g_win->on_close = exp_close;
    gui64_set_min_size(g_win, 420, 300);
    exp_enter_thispc();
    dbg64_str("[APP] mypc opened");
    dbg64_nl();
}

void explorer64_reset64() {
    if (gui64_window_alive(g_win)) gui64_destroy_window(g_win);
    if (gui64_window_alive(g_prev_win)) gui64_destroy_window(g_prev_win);
    g_win = nullptr;
    g_prev_win = nullptr;
    g_items = 0;
    g_sel = -1;
    g_scroll = 0;
    dbg64_str("[APP] mypc reset");
    dbg64_nl();
}

// ==================== 自检（纯逻辑，不建窗口、不碰盘）====================
int explorer64_selftest64() {
    int fails = 0;
    char b[40];
    // 1) KB -> 人读单位：1.0 GB / 76.1 MB / 512 KB
    fmt_kb64(1024ull * 1024ull, b, (int)sizeof(b));
    if (!e_streq(b, "1.0 GB")) fails |= 1;
    fmt_kb64(77899ull, b, (int)sizeof(b));
    if (!e_streq(b, "76.1 MB")) fails |= 2;
    fmt_kb64(512ull, b, (int)sizeof(b));
    if (!e_streq(b, "512 KB")) fails |= 4;
    // 2) 字节 -> 人读单位：2.0 KB / 512 B / 5.0 MB
    fmt_bytes64(2048ull, b, (int)sizeof(b));
    if (!e_streq(b, "2.0 KB")) fails |= 8;
    fmt_bytes64(512ull, b, (int)sizeof(b));
    if (!e_streq(b, "512 B")) fails |= 16;
    fmt_bytes64(5ull * 1024 * 1024, b, (int)sizeof(b));
    if (!e_streq(b, "5.0 MB")) fails |= 32;
    // 3) mtime 格式化（0 = 未知 -> 空串）
    fmt_mtime64(0, b, (int)sizeof(b));
    if (b[0] != 0) fails |= 64;
    {
        const uint32_t t = vfs64_pack_time64(2026, 9, 20, 12, 34, 56);
        char t2[32];
        fmt_mtime64(t, t2, (int)sizeof(t2));
        if (!e_streq(t2, "2026-09-20 12:34")) fails |= 128;
    }
    // 4) 父路径 / 段数
    {
        char p[VFS64_PATH_MAX];
        path_parent_pure("/apps/demo", p, (int)sizeof(p));
        if (!e_streq(p, "/apps")) fails |= 256;
        path_parent_pure("/apps", p, (int)sizeof(p));
        if (!e_streq(p, "/")) fails |= 512;
        path_parent_pure("/", p, (int)sizeof(p));
        if (p[0] != 0) fails |= 1024;
        if (path_seg_count("/") != 0) fails |= 2048;
        if (path_seg_count("/apps/demo") != 2) fails |= 4096;
        if (path_seg_count("/a/b/c/d") != 4) fails |= 8192;
        path_seg("/a/b/c/d", 3, p, (int)sizeof(p));
        if (!e_streq(p, "c")) fails |= 16384;
    }
    // 5) 滚动钳制（绝不越界）
    if (clamp_scroll_pure(-5, 100, 10) != 0) fails |= 32768;
    if (clamp_scroll_pure(95, 100, 10) != 90) fails |= 65536;
    if (clamp_scroll_pure(5, 3, 10) != 0) fails |= 131072;
    if (clamp_scroll_pure(7, 0, 10) != 0) fails |= 262144;
    // 6) 历史栈（push 截断前进分支 / 深度有界）
    {
        ExpNav h[4];
        int n = 0, pos = -1;
        ExpNav r;
        for (int i = 0; i < 6; i++) {
            r.mode = 1;
            r.letter = 'C';
            r.path[0] = '/';
            r.path[1] = (char)('a' + i);
            r.path[2] = 0;
            hist_push_pure(h, &n, &pos, &r, 4);
        }
        if (n != 4) fails |= 524288;                       // 深度有界
        if (pos != 3) fails |= 1048576;
        if (h[3].path[1] != 'f') fails |= 2097152;         // 最新在栈顶
        r.mode = 1; r.letter = 'C'; e_strcpy(r.path, "/x", (int)sizeof(r.path));
        pos = 1;                                           // 模拟"后退后再进入" -> 截断
        hist_push_pure(h, &n, &pos, &r, 4);
        if (n != 3) fails |= 4194304;
        if (pos != 2) fails |= 8388608;
    }
    dbg64_str("[EXPL] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_nl();
    return fails;
}
