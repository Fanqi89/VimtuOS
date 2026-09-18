// ============================================================================
// mines64.cpp - VimtuOS 纯 64 位内核：扫雷（Minesweeper）
//
// 移植自 Vimtu32/kernel/gui.cpp 的"内置扫雷"段（MsState / ms_* / MS_* / ms_open_game）。
// 与 32 位的行为差异（有意为之）：
//   * 不再有全局棋盘兜底（32 位的 g_ms_fb / g_ms_cur）：每窗口棋盘只挂在
//     Window::userdata 上，回调一律从 w->userdata 取自己的状态；难度也是
//     **每实例一份**（32 位是全局共享，切一个窗口的难度会改所有窗口）。
//   * 计时按 PIT_HZ_64 = 250 换算（32 位旧版写死 100Hz，计时快 2.5 倍）。
//   * 颜色一律 rgb()（0xFFRRGGBB）。32 位手写 0x00RRGGBB，0xFF0000 会显示成绿色。
//   * 绘制用屏幕绝对坐标（w->client_x/y + 局部坐标），外壳已设好客户区裁剪。
//   * 计时 LED 靠 on_tick 按"秒变化"重绘（64 位外壳是脏矩形提交，不再每帧全屏刷）。
//   * 洪水式展开用显式栈而非递归（高级 30x16 最坏几百层，避免压爆内核栈）。
//   * 结束时间 end_sec 先算再置 over（32 位先置 over 再 ms_sec()，拿到的是旧值）。
//
// 串口日志（自动验收断言）：
//   [APP] mines opened diff=N          N=0/1/2 -> 0=初级 1=中级 2=高级
//   [APP] mines closed                 窗口关闭（Esc；或外壳销毁窗口后被检测到）
//   [APP] mines reset                  会话重置：所有实例棋盘清空
//   [APP] mines limit reached (4)      已有 4 个实例，第 5 次打开只激活最近的那个
//   [APP] mines win sec=N              全部非雷格翻开 -> 赢
//   [APP] mines boom sec=N             踩雷 -> 输
//   [APP] mines nomem                  堆分配失败（正常不该出现）
//   [UI] mines layout client=WxH cell=N grid=CxR   客户区/格边长变化时才打一行
//   [UI] mines diff=N                  难度切换（键盘 1/2/3 或鼠标点底部按钮）
//
// 窗口契约（gui64.h）：
//   * 打开：gui64_create_window(..., APP_ID_MINES) + gui64_set_click2()（右键标旗）
//           + gui64_set_tick()（计时/关窗检测）+ gui64_set_min_size(w, 260, 280)。
//   * 状态：kmalloc_64，分配时 mem_owner_set_64(MEM_OWNER_MINES_64) 包住。
//   * 关窗：gui64.h 明说"关窗时外壳不会替你释放 userdata"，所以本文件在
//     Esc 关窗时自己 kfree_64，并且用 gui64_window_alive() 检测"被外壳销毁"的
//     实例（无 on_close 钩子），检测到后打 closed 行并释放状态。
//     如果外壳改成自己释放 userdata（32 位 gui_destroy_window 就是那么做的），
//     请告知：把 ms_reap_closed() 里的 kfree_64 去掉即可（见报告"集成注意"）。
// ============================================================================

#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "mem_64.h"
#include "x86_64.h"
#include "debug64.h"

// ==================== 常量（对齐 32 位 MS_*）====================
#define MS_MAX_COLS   30                    // 高级 30x16
#define MS_MAX_ROWS   16
#define MS_MAX_CELLS  (MS_MAX_COLS * MS_MAX_ROWS)
#define MS_MAX_INST   4                     // 最多 4 个实例（第 5 次只激活最近的）
#define MS_CELL_MAX   30
#define MS_CELL_MIN   12
#define MS_PAD        8
#define MS_TOP_H      56
#define MS_BTN_H      26
#define MS_BTN_GAP    6
#define MS_LED_W      46
#define MS_LED_H      28
#define MS_MAGIC      0x4D533634u           // 'MS64'：userdata 合法性校验
#define MS_STATE_QUEUED 3                   // 洪水展开的"已入栈"临时态（绘制不会看到）

// 颜色：一律 rgb(r,g,b) -> 0xFFRRGGBB
#define MS_COL_BG        rgb(192, 192, 192)
#define MS_COL_CLOSED    rgb(192, 192, 192)
#define MS_COL_OPEN      rgb(224, 224, 224)
#define MS_COL_BOOM      rgb(255, 128, 128)
#define MS_COL_HI        rgb(255, 255, 255)
#define MS_COL_LO        rgb(128, 128, 128)
#define MS_COL_TEXT      rgb(0, 0, 0)
#define MS_COL_RED       rgb(255, 0, 0)
#define MS_COL_LED_BG    rgb(0, 0, 0)
#define MS_COL_LED_FG    rgb(255, 48, 48)
#define MS_COL_FACE      rgb(255, 255, 0)
#define MS_COL_BTN       rgb(240, 240, 240)
#define MS_COL_BTN_SEL   rgb(16, 140, 224)
#define MS_COL_BTN_LINE  rgb(128, 128, 128)
#define MS_COL_WHITE     rgb(255, 255, 255)
#define MS_COL_CURSOR    rgb(255, 0, 0)

// ==================== 每实例状态 ====================
struct MinesState {
    uint32_t magic;                          // MS_MAGIC
    int      diff;                           // 本实例难度 0/1/2
    uint8_t  mine[MS_MAX_ROWS][MS_MAX_COLS]; // 1=雷
    uint8_t  state[MS_MAX_ROWS][MS_MAX_COLS];// 0=未翻开 1=翻开 2=旗
    bool     inited;                         // 雷场已布置（首次点击时，避开首格）
    bool     over;
    bool     won;
    int      opened;                         // 已翻开格数
    int      flags;                          // 旗数
    uint32_t start_tick;
    bool     started;                        // 已开始计时（首次翻开那一刻）
    int      end_sec;
    int      cur_x, cur_y;                   // 键盘光标
    int      shown_sec;                      // 计时 LED 上次画出的秒（用于按秒重绘）
    int      log_cell, log_cw, log_ch;       // 布局日志去重（每实例各自一份）
    uint32_t dup_tick;                       // 同 tick 同点击去重（见 ms_click_dup）
    int      dup_x, dup_y, dup_btn;
    bool     reported;                       // 本局结束行（win/boom）是否已打
};

// ==================== 全局（非棋盘状态）====================
struct MinesSlot { Window* w; MinesState* st; };

static MinesSlot g_ms_slot[MS_MAX_INST];     // 实例登记表（关窗检测 / 激活最近窗口）
static Window*   g_ms_last = nullptr;        // 最近打开的扫雷窗口
static int       g_ms_diff = 0;              // 全局难度：新开窗口继承（32 位也是全局难度）
static uint32_t  g_ms_rand_state = 0x9E3779B9u;
static bool      g_ms_rand_seeded = false;
static uint16_t  g_ms_stack[MS_MAX_CELLS];   // 洪水展开显式栈（单线程，回调内一次性用完）

// ==================== 小工具 ====================
static int ms_cols_of(int d)  { return d == 2 ? 30 : (d == 1 ? 16 : 9); }
static int ms_rows_of(int d)  { return d == 2 ? 16 : 9; }
static int ms_mines_of(int d) { return d == 2 ? 99 : (d == 1 ? 40 : 10); }

static void ms_log_diff(int d) {
    dbg64_str("[UI] mines diff=");
    dbg64_dec((uint64_t)d);
    dbg64_nl();
}

// 32 位用同样的 LCG；只是首次使用时用 ticks64 播种，免得每局雷场完全一样
static uint32_t ms_rand() {
    if (!g_ms_rand_seeded) {
        g_ms_rand_seeded = true;
        g_ms_rand_state ^= ticks64() * 2654435761u;
    }
    g_ms_rand_state = g_ms_rand_state * 1103515245u + 12345u;
    return (g_ms_rand_state >> 16) & 0x7FFFu;
}

static MinesState* ms_state(Window* w) {
    if (!w || !w->userdata) return nullptr;
    MinesState* st = (MinesState*)w->userdata;
    if (st->magic != MS_MAGIC) return nullptr;
    return st;
}

// 外壳可能把左键既投给 on_click 又投给 on_click2（契约允许任一实现）：
// 同一 tick 内完全相同的点击只处理一次，避免翻开/标旗被执行两遍。
static bool ms_click_dup(MinesState* st, int x, int y, int btn) {
    uint32_t t = ticks64();
    if (st->dup_btn == btn && st->dup_x == x && st->dup_y == y && st->dup_tick == t) return true;
    st->dup_tick = t; st->dup_x = x; st->dup_y = y; st->dup_btn = btn;
    return false;
}

// ==================== 记时（PIT 250Hz，不能写死 100）====================
static int ms_sec_now(MinesState* st) {
    if (!st->started) return 0;
    uint32_t t = ticks64();
    uint32_t dt = t - st->start_tick;                 // 无符号回绕安全
    int s = (int)(dt / PIT_HZ_64);
    if (s < 0) s = 0;
    if (s > 999) s = 999;
    return s;
}

static int ms_sec(MinesState* st) {
    if (!st->inited) return 0;
    if (st->over) return st->end_sec;
    return ms_sec_now(st);
}

// ==================== 棋盘逻辑（全部以 st 为参数，无全局棋盘）====================
static int ms_count_around(MinesState* st, int cx, int cy) {
    int n = 0;
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            int x = cx + dx, y = cy + dy;
            if (x >= 0 && x < C && y >= 0 && y < R && st->mine[y][x]) n++;
        }
    return n;
}

// 布雷（避开 avoid 格，保证首次点击安全）
static void ms_init(MinesState* st, int avoid_x, int avoid_y) {
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    for (int y = 0; y < MS_MAX_ROWS; y++)
        for (int x = 0; x < MS_MAX_COLS; x++) {
            st->mine[y][x] = 0;
            st->state[y][x] = 0;
        }
    int mines = ms_mines_of(st->diff);
    int placed = 0;
    // 雷数不该超过格数（高级 99 < 480），留个保险免得死循环
    while (placed < mines && placed < C * R) {
        int x = (int)(ms_rand() % (uint32_t)C);
        int y = (int)(ms_rand() % (uint32_t)R);
        if (x == avoid_x && y == avoid_y) continue;
        if (!st->mine[y][x]) { st->mine[y][x] = 1; placed++; }
    }
    st->inited = true;
    st->over = false;
    st->won = false;
    st->opened = 0;
    st->flags = 0;
    st->end_sec = 0;
    st->start_tick = 0;
    st->started = false;
    st->shown_sec = -1;
    st->reported = false;
    st->cur_x = C / 2;
    st->cur_y = R / 2;
}

static void ms_state_clear(MinesState* st) {
    for (int y = 0; y < MS_MAX_ROWS; y++)
        for (int x = 0; x < MS_MAX_COLS; x++) {
            st->mine[y][x] = 0;
            st->state[y][x] = 0;
        }
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    st->inited = false;
    st->over = false;
    st->won = false;
    st->opened = 0;
    st->flags = 0;
    st->end_sec = 0;
    st->start_tick = 0;
    st->shown_sec = -1;
    st->reported = false;
    st->cur_x = C / 2;
    st->cur_y = R / 2;
    st->dup_btn = -1;
    st->dup_x = -1;
    st->dup_y = -1;
    st->dup_tick = 0;
}

static void ms_log_over(MinesState* st, bool win) {
    if (st->reported) return;
    st->reported = true;
    dbg64_str(win ? "[APP] mines win sec=" : "[APP] mines boom sec=");
    dbg64_dec((uint64_t)(st->end_sec < 0 ? 0 : st->end_sec));
    dbg64_nl();
}

// 洪水式展开：显式栈（每格最多入栈一次），不用递归
static void ms_flood(MinesState* st, int sx, int sy) {
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    int sp = 0;
    st->state[sy][sx] = MS_STATE_QUEUED;
    g_ms_stack[sp++] = (uint16_t)(sy * C + sx);
    while (sp > 0) {
        int idx = (int)g_ms_stack[--sp];
        int cx = idx % C, cy = idx / C;
        if (st->state[cy][cx] != MS_STATE_QUEUED) continue;
        st->state[cy][cx] = 1;
        st->opened++;
        if (ms_count_around(st, cx, cy) != 0) continue;   // 数字格：不展开邻居
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) {
                if (!dx && !dy) continue;
                int nx = cx + dx, ny = cy + dy;
                if (nx < 0 || nx >= C || ny < 0 || ny >= R) continue;
                if (st->state[ny][nx] != 0) continue;      // 已翻开/旗/已入栈
                st->state[ny][nx] = MS_STATE_QUEUED;
                g_ms_stack[sp++] = (uint16_t)(ny * C + nx);
            }
    }
}

// 翻开一格（首次点击才布雷并开始计时）
static void ms_open(MinesState* st, int x, int y) {
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    if (st->over) return;
    if (x < 0 || x >= C || y < 0 || y >= R) return;
    if (!st->inited) ms_init(st, x, y);                  // 首次翻开才布雷（避开本格）
    if (!st->started) {                                  // 计时从首次翻开开始（此前标旗只布雷）
        st->started = true;
        st->start_tick = ticks64();
        if (st->start_tick == 0) st->start_tick = 1;
    }
    if (st->state[y][x] != 0) return;                    // 已翻开或旗
    if (st->mine[y][x]) {                                // 踩雷：即输
        st->state[y][x] = 1;
        st->opened++;
        st->end_sec = ms_sec_now(st);
        st->over = true;
        ms_log_over(st, false);
        return;
    }
    ms_flood(st, x, y);
    if (st->opened == C * R - ms_mines_of(st->diff)) {    // 全部非雷格翻开 -> 赢
        st->end_sec = ms_sec_now(st);
        st->won = true;
        st->over = true;
        ms_log_over(st, true);
    }
}

static void ms_toggle_flag(MinesState* st, int x, int y) {
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    if (st->over) return;
    if (x < 0 || x >= C || y < 0 || y >= R) return;
    if (!st->inited) ms_init(st, -1, -1);                // 未开局标旗：只布雷不开始计时
    if (st->state[y][x] == 0)      { st->state[y][x] = 2; st->flags++; }
    else if (st->state[y][x] == 2) { st->state[y][x] = 0; st->flags--; }
}

// ==================== 布局（全部由客户区尺寸推导，对齐 32 位 ms_layout）====================
struct MsLayout {
    int C, R;                       // 列/行数
    int cw, ch;                     // 客户区宽高
    int cell;                       // 格边长 12..30
    int pad;
    int top_h;                      // 顶部信息区高度
    int gx, gy;                     // 网格左上角（客户区局部坐标）
    int gw, gh;                     // 网格像素尺寸
    int led_w, led_h;
    int btn_y, btn_h, btn_w, btn_gap, btn_x0;
};

static void ms_layout(Window* w, MinesState* st, MsLayout* L) {
    L->C = ms_cols_of(st->diff);
    L->R = ms_rows_of(st->diff);
    L->cw = w->client_w;
    L->ch = w->client_h;
    if (L->cw < 40) L->cw = 40;
    if (L->ch < 40) L->ch = 40;
    L->pad = MS_PAD;
    L->led_w = MS_LED_W;
    L->led_h = MS_LED_H;
    L->btn_h = MS_BTN_H;
    L->btn_gap = MS_BTN_GAP;
    L->top_h = L->led_h + L->pad * 2;                    // 顶栏 = LED 高 + 上下留白
    if (L->top_h > 52) L->top_h = 52;
    if (L->ch < 240) L->top_h = 40;
    // 底部难度按钮行：宽度随窗口收缩
    L->btn_w = (L->cw - L->pad * 2 - L->btn_gap * 2) / 3;
    if (L->btn_w > 72) L->btn_w = 72;
    if (L->btn_w < 34) L->btn_w = 34;
    // 格边长：由可用宽/高共同决定（12..30px，保持方块）
    int avail_w = L->cw - L->pad * 2;
    int avail_h = L->ch - L->top_h - L->pad - 8 - L->btn_h;
    if (avail_w < 40) avail_w = 40;
    if (avail_h < 40) avail_h = 40;
    int cell = avail_w / L->C;
    int cellh = avail_h / L->R;
    if (cellh < cell) cell = cellh;
    if (cell > MS_CELL_MAX) cell = MS_CELL_MAX;
    if (cell < MS_CELL_MIN) cell = MS_CELL_MIN;
    L->cell = cell;
    L->gw = L->C * cell;
    L->gh = L->R * cell;
    L->gx = (L->cw - L->gw) / 2;                         // 网格水平居中
    if (L->gx < L->pad) L->gx = L->pad;
    int space = L->ch - L->top_h - L->pad - 8 - L->btn_h;
    L->gy = L->top_h + (space - L->gh) / 2;              // 在可用高度里垂直居中
    if (L->gy < L->top_h) L->gy = L->top_h;
    int total = L->btn_w * 3 + L->btn_gap * 2;
    L->btn_x0 = (L->cw - total) / 2;
    L->btn_y = L->gy + L->gh + 8;
    if (L->btn_y + L->btn_h > L->ch - 2) L->btn_y = L->ch - 2 - L->btn_h;   // 贴底
    if (L->btn_y < 0) L->btn_y = 0;
}

// 客户区"自然尺寸"（格边长 = 30px）：新建窗口的初始几何用
static int ms_win_cw(int diff) { return MS_PAD * 2 + ms_cols_of(diff) * MS_CELL_MAX; }
static int ms_win_ch(int diff) {
    return MS_TOP_H + ms_rows_of(diff) * MS_CELL_MAX + MS_PAD + 8 + MS_BTN_H;
}

// 难度切换后：当前窗口放不下新棋盘（格边长会被压到最小）时，把窗口长大到刚好放得下。
// 32 位直接改 w->w/w->h；64 位只能走外壳接口（应用只读几何字段）。
static void ms_fit_window(Window* w, int diff) {
    if (!w) return;
    const int min_cell = 16;
    int need_cw = MS_PAD * 2 + ms_cols_of(diff) * min_cell + 4;
    int need_ch = MS_TOP_H + ms_rows_of(diff) * min_cell + MS_PAD + 8 + MS_BTN_H + 4;
    if (w->client_w >= need_cw && w->client_h >= need_ch) return;   // 只长不缩（同 32 位）
    int req_cw = w->client_w < need_cw ? need_cw : w->client_w;
    int req_ch = w->client_h < need_ch ? need_ch : w->client_h;
    gui64_fit_window_to_client(w, req_cw, req_ch);
}

// ==================== 绘制辅助（像素风）====================
static void ms_fill_circle(int cx, int cy, int r, uint32_t color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) fb_putpixel(cx + dx, cy + dy, color);
}

// 雷：8 根尖刺 + 实心球（按格边长缩放；32 位固定 8/12px，小格里会溢出）
static void ms_draw_mine(int cx, int cy, int S) {
    int mx = cx + S / 2, my = cy + S / 2;
    int r = S * 8 / 30;      if (r < 2) r = 2;
    int reach = S * 12 / 30; if (reach < r + 2) reach = r + 2;
    static const int dirs[8][2] = {
        {0, -10}, {0, 10}, {-10, 0}, {10, 0},
        {-7, -7}, {7, 7}, {-7, 7}, {7, -7}
    };
    for (int i = 0; i < 8; i++)
        for (int t = r; t <= reach; t++)
            fb_putpixel(mx + dirs[i][0] * t / 10, my + dirs[i][1] * t / 10, MS_COL_TEXT);
    ms_fill_circle(mx, my, r, MS_COL_TEXT);
    if (r >= 3) fb_putpixel(mx - r / 2, my - r / 2, MS_COL_HI);   // 一点高光
}

// 旗：竖杆 + 红色三角旗面（按格边长缩放）
static void ms_draw_flag(int cx, int cy, int S) {
    int ph = S * 15 / 30; if (ph < 6) ph = 6;    // 杆高
    int fw = S * 8 / 30;  if (fw < 3) fw = 3;    // 旗面宽（= 高）
    int px = cx + S / 2 - fw / 2;
    int top = cy + (S - ph) / 2;
    fb_draw_vline(px, top, ph, MS_COL_TEXT);
    for (int i = 0; i < fw; i++) {
        int w = fw - i;
        fb_draw_hline(px, top + i, w, MS_COL_RED);
    }
    fb_draw_hline(px - fw / 2, top + ph - 1, fw + fw / 2, MS_COL_TEXT);   // 底座
}

// 错误旗：打叉（32 位是两条红横线，像 #；这里按需求画 X）
static void ms_draw_x(int cx, int cy, int S, uint32_t col) {
    int m = S / 5; if (m < 2) m = 2;
    int x0 = cx + m, y0 = cy + m;
    int x1 = cx + S - 1 - m, y1 = cy + S - 1 - m;
    int dx = x1 - x0, dy = y1 - y0;
    if (dx < 1 || dy < 1) return;
    int steps = dx > dy ? dx : dy;
    for (int i = 0; i <= steps; i++) {
        int px = x0 + dx * i / steps;
        int py = y0 + dy * i / steps;
        fb_putpixel(px, py, col);
        fb_putpixel(x1 - (px - x0), py, col);
        if (S >= 22) {                      // 粗一点
            fb_putpixel(px, py + 1, col);
            fb_putpixel(x1 - (px - x0), py + 1, col);
        }
    }
}

// 数字 1-8 各自的配色（对齐 32 位 ms_num_color）
static uint32_t ms_num_color(int n) {
    switch (n) {
        case 1: return rgb(0, 0, 255);
        case 2: return rgb(0, 128, 0);
        case 3: return rgb(255, 0, 0);
        case 4: return rgb(0, 0, 128);
        case 5: return rgb(128, 0, 0);
        case 6: return rgb(0, 128, 128);
        case 7: return rgb(0, 0, 0);
        default: return rgb(128, 128, 128);
    }
}

static void ms_draw_digit(int cx, int cy, int S, int n, int cell) {
    char buf[2] = { (char)('0' + n), 0 };
    uint32_t col = ms_num_color(n);
    int lh = font_line_height();
    if (cell >= lh + 4) {                       // 格够大：TrueType（同 32 位）
        int tw = font_text_width(buf);
        font_draw_text(cx + (S - tw) / 2, cy + (S - lh) / 2, buf, col);
    } else {                                    // 小格：8x8 位图字体（带底色，保证不出格）
        fb_draw_char(cx + (S - 8) / 2, cy + (S - 8) / 2, buf[0], col, MS_COL_OPEN, 1);
    }
}

// 笑脸（r = 半径；死=X 眼+直嘴，赢=张嘴笑）
static void ms_draw_face(int cx, int cy, int r, bool dead, bool win) {
    ms_fill_circle(cx, cy, r, MS_COL_FACE);
    int ex = r * 5 / 13; if (ex < 2) ex = 2;
    int ey = r * 3 / 13; if (ey < 2) ey = 2;
    if (dead) {
        int s = r >= 10 ? 2 : 1;
        for (int i = -s; i <= s; i++) {
            fb_putpixel(cx - ex + i, cy - ey + i, MS_COL_TEXT);
            fb_putpixel(cx - ex + i, cy - ey - i, MS_COL_TEXT);
            fb_putpixel(cx + ex + i, cy - ey + i, MS_COL_TEXT);
            fb_putpixel(cx + ex + i, cy - ey - i, MS_COL_TEXT);
        }
        fb_draw_hline(cx - ex, cy + ey + 1, 2 * ex + 1, MS_COL_TEXT);        // 直嘴
    } else {
        int es = r >= 10 ? 2 : 1;
        fb_fill_rect(cx - ex, cy - ey, es, es, MS_COL_TEXT);                 // 眼睛
        fb_fill_rect(cx + ex, cy - ey, es, es, MS_COL_TEXT);
        if (win) {                                                          // 张嘴笑
            int mh = r / 4; if (mh < 1) mh = 1;
            int mw = 2 * ex + 2;
            fb_draw_hline(cx - mw / 2, cy + ey + mh, mw, MS_COL_TEXT);
            fb_draw_vline(cx - mw / 2, cy + ey + 1, mh, MS_COL_TEXT);
            fb_draw_vline(cx + mw / 2, cy + ey + 1, mh, MS_COL_TEXT);
        } else {
            fb_draw_hline(cx - ex, cy + ey + 1, 2 * ex + 1, MS_COL_TEXT);
        }
    }
}

static void ms_led(char* out, int n) {
    if (n < 0) n = 0;
    if (n > 999) n = 999;
    out[0] = (char)('0' + n / 100);
    out[1] = (char)('0' + (n / 10) % 10);
    out[2] = (char)('0' + n % 10);
    out[3] = 0;
}

// 立体斜面：raised=凸起（未翻开） 凹陷=已翻开
static void ms_bevel(int cx, int cy, int S, bool raised) {
    uint32_t hi = raised ? MS_COL_HI : MS_COL_LO;
    uint32_t lo = raised ? MS_COL_LO : MS_COL_HI;
    fb_draw_hline(cx, cy, S, hi);
    fb_draw_vline(cx, cy, S, hi);
    fb_draw_hline(cx, cy + S - 1, S, lo);
    fb_draw_vline(cx + S - 1, cy, S, lo);
}

static void ms_draw_cell(int cx, int cy, int cell, int col, int row, MinesState* st) {
    int S = cell;
    uint8_t s = st->state[row][col];
    bool is_mine = st->mine[row][col] != 0;
    bool show_mine = is_mine && (s == 1 || (st->over && s != 2) || st->won);
    if (s == 2) {
        // 旗（游戏结束时错误的旗画 X）
        fb_fill_rect(cx, cy, S, S, MS_COL_CLOSED);
        ms_bevel(cx, cy, S, true);
        if (st->over && !is_mine) ms_draw_x(cx, cy, S, MS_COL_RED);
        else                      ms_draw_flag(cx, cy, S);
    } else if (show_mine) {
        fb_fill_rect(cx, cy, S, S, (st->over && s == 1) ? MS_COL_BOOM : MS_COL_OPEN);
        ms_bevel(cx, cy, S, false);
        ms_draw_mine(cx, cy, S);
    } else if (s == 1) {
        fb_fill_rect(cx, cy, S, S, MS_COL_OPEN);
        ms_bevel(cx, cy, S, false);
        int n = ms_count_around(st, col, row);
        if (n > 0) ms_draw_digit(cx, cy, S, n, cell);
    } else {
        fb_fill_rect(cx, cy, S, S, MS_COL_CLOSED);
        ms_bevel(cx, cy, S, true);
    }
    // 键盘光标
    if (!st->over && col == st->cur_x && row == st->cur_y && S >= 8)
        fb_draw_rect(cx + 2, cy + 2, S - 4, S - 4, MS_COL_CURSOR);
}

static const char* ms_diff_label(int d) {
    if (d == 0) return gui64_tr("Easy", "初级");
    if (d == 1) return gui64_tr("Medium", "中级");
    return gui64_tr("Hard", "高级");
}

// ==================== 实例登记 / 关窗检测 ====================
static void ms_register(Window* w, MinesState* st) {
    for (int i = 0; i < MS_MAX_INST; i++) {
        if (!g_ms_slot[i].w) { g_ms_slot[i].w = w; g_ms_slot[i].st = st; return; }
    }
}

static int ms_find_slot(Window* w) {
    for (int i = 0; i < MS_MAX_INST; i++) if (g_ms_slot[i].w == w) return i;
    return -1;
}

static Window* ms_latest_live() {
    if (g_ms_last && gui64_window_alive(g_ms_last)) return g_ms_last;
    for (int i = MS_MAX_INST - 1; i >= 0; i--)
        if (g_ms_slot[i].w && gui64_window_alive(g_ms_slot[i].w)) return g_ms_slot[i].w;
    return nullptr;
}

// 外壳销毁窗口时没有 on_close 钩子：靠 gui64_window_alive() 发现"我的窗口没了"，
// 补打 closed 行并释放 kmalloc 的棋盘状态（gui64.h 明说外壳不替应用释放 userdata）。
static void ms_reap_closed() {
    for (int i = 0; i < MS_MAX_INST; i++) {
        Window* w = g_ms_slot[i].w;
        if (!w) continue;
        if (gui64_window_alive(w) && w->app_id == APP_ID_MINES) continue;   // 还活着，是我的
        MinesState* st = g_ms_slot[i].st;
        g_ms_slot[i].w = nullptr;
        g_ms_slot[i].st = nullptr;
        if (g_ms_last == w) g_ms_last = nullptr;
        if (st && st->magic == MS_MAGIC) {
            st->magic = 0;
            int prev = mem_owner_get_64();
            mem_owner_set_64(MEM_OWNER_MINES_64);
            kfree_64(st);
            mem_owner_set_64(prev);
        }
        dbg64_str("[APP] mines closed");
        dbg64_nl();
    }
}

// Esc 关窗：自己释放状态（先把 userdata 摘掉，避免外壳再释放一次）
static void ms_close_window(Window* w) {
    MinesState* st = ms_state(w);
    int slot = ms_find_slot(w);
    if (slot >= 0) { g_ms_slot[slot].w = nullptr; g_ms_slot[slot].st = nullptr; }
    if (g_ms_last == w) g_ms_last = nullptr;
    if (st) {
        w->userdata = nullptr;
        st->magic = 0;
        int prev = mem_owner_get_64();
        mem_owner_set_64(MEM_OWNER_MINES_64);
        kfree_64(st);
        mem_owner_set_64(prev);
    }
    dbg64_str("[APP] mines closed");
    dbg64_nl();
    gui64_destroy_window(w);
}

// ==================== 绘制（屏幕绝对坐标）====================
static void ms_draw(Window* w) {
    ms_reap_closed();
    MinesState* st = ms_state(w);
    if (!st) {          // userdata 尚未挂上（创建瞬间）等：只铺底
        if (w) {
            font_select(0);
            fb_fill_rect(w->client_x, w->client_y, w->client_w, w->client_h, MS_COL_BG);
        }
        return;
    }
    MsLayout L;
    ms_layout(w, st, &L);
    // 布局可观测：客户区 / 格边长变化时才写一行（每帧写会刷屏）
    if (L.cell != st->log_cell || w->client_w != st->log_cw || w->client_h != st->log_ch) {
        st->log_cell = L.cell;
        st->log_cw = w->client_w;
        st->log_ch = w->client_h;
        dbg64_str("[UI] mines layout client=");
        dbg64_dec((uint64_t)(w->client_w < 0 ? 0 : w->client_w));
        dbg64_str("x");
        dbg64_dec((uint64_t)(w->client_h < 0 ? 0 : w->client_h));
        dbg64_str(" cell=");
        dbg64_dec((uint64_t)L.cell);
        dbg64_str(" grid=");
        dbg64_dec((uint64_t)L.C);
        dbg64_str("x");
        dbg64_dec((uint64_t)L.R);
        dbg64_nl();
    }

    int x0 = w->client_x, y0 = w->client_y;
    font_select(0);                                   // LED 数字用正文 face
    fb_fill_rect(x0, y0, L.cw, L.ch, MS_COL_BG);

    // 顶部：剩余雷数 LED + 笑脸 + 计时 LED
    char led[4];
    int led_y = y0 + (L.top_h - L.led_h) / 2;
    fb_fill_rect(x0 + L.pad, led_y, L.led_w, L.led_h, MS_COL_LED_BG);
    ms_led(led, ms_mines_of(st->diff) - st->flags);
    int lw = font_text_width(led);
    font_draw_text(x0 + L.pad + (L.led_w - lw) / 2,
                   led_y + (L.led_h - font_line_height()) / 2, led, MS_COL_LED_FG);

    int fr = L.top_h < 48 ? 10 : 13;
    int maxr = L.top_h / 2 - 2;
    if (fr > maxr) fr = maxr;
    if (fr < 6) fr = 6;
    ms_draw_face(x0 + L.cw / 2, y0 + L.top_h / 2, fr,
                 st->over && !st->won, st->won);

    fb_fill_rect(x0 + L.cw - L.pad - L.led_w, led_y, L.led_w, L.led_h, MS_COL_LED_BG);
    int elsec = ms_sec(st);
    ms_led(led, elsec);
    st->shown_sec = elsec;
    lw = font_text_width(led);
    font_draw_text(x0 + L.cw - L.pad - L.led_w + (L.led_w - lw) / 2,
                   led_y + (L.led_h - font_line_height()) / 2, led, MS_COL_LED_FG);

    // 网格（与命中判定同一套几何）
    for (int row = 0; row < L.R; row++)
        for (int col = 0; col < L.C; col++)
            ms_draw_cell(x0 + L.gx + col * L.cell, y0 + L.gy + row * L.cell,
                         L.cell, col, row, st);

    // 底部难度按钮行
    for (int d = 0; d < 3; d++) {
        int bx = x0 + L.btn_x0 + d * (L.btn_w + L.btn_gap);
        bool sel = (d == st->diff);
        fb_fill_rect(bx, y0 + L.btn_y, L.btn_w, L.btn_h, sel ? MS_COL_BTN_SEL : MS_COL_BTN);
        fb_draw_rect(bx, y0 + L.btn_y, L.btn_w, L.btn_h, MS_COL_BTN_LINE);
        const char* lab = ms_diff_label(d);
        int lw2 = font_text_width(lab);
        font_draw_text(bx + (L.btn_w - lw2) / 2,
                       y0 + L.btn_y + (L.btn_h - font_line_height()) / 2,
                       lab, sel ? MS_COL_WHITE : MS_COL_TEXT);
    }
}

// ==================== 点击 ====================
static bool ms_grid_hit(const MsLayout* L, int cx, int cy, int* col, int* row) {
    if (L->cell <= 0) return false;
    int c = (cx - L->gx) / L->cell;
    int r = (cy - L->gy) / L->cell;
    if (c < 0 || c >= L->C || r < 0 || r >= L->R) return false;
    if (cx < L->gx + c * L->cell || cy < L->gy + r * L->cell) return false;
    *col = c;
    *row = r;
    return true;
}

static void ms_set_diff(Window* w, MinesState* st, int d) {
    if (d < 0 || d > 2 || d == st->diff) return;
    st->diff = d;
    g_ms_diff = d;                    // 新开实例继承（32 位难度是全局的）
    ms_state_clear(st);
    ms_log_diff(d);
    ms_fit_window(w, d);              // 放不下就把窗口长大（只长不缩，同 32 位）
}

static void ms_click_left(Window* w, MinesState* st, int cx, int cy) {
    MsLayout L;
    ms_layout(w, st, &L);
    // 笑脸按钮：重开本局
    if (cy >= 2 && cy < 2 + L.top_h && cx >= L.cw / 2 - 20 && cx < L.cw / 2 + 20) {
        ms_state_clear(st);
        gui64_invalidate_window(w);
        return;
    }
    // 底部难度按钮行
    if (cy >= L.btn_y && cy < L.btn_y + L.btn_h) {
        int rel = cx - L.btn_x0;
        int d = rel / (L.btn_w + L.btn_gap);
        if (rel >= 0 && d >= 0 && d < 3 && (rel % (L.btn_w + L.btn_gap)) < L.btn_w)
            ms_set_diff(w, st, d);
        gui64_invalidate_window(w);
        return;
    }
    // 网格
    int col = 0, row = 0;
    if (!ms_grid_hit(&L, cx, cy, &col, &row)) return;
    st->cur_x = col;
    st->cur_y = row;
    ms_open(st, col, row);
    gui64_invalidate_window(w);
}

static void ms_click_right(Window* w, MinesState* st, int cx, int cy) {
    MsLayout L;
    ms_layout(w, st, &L);
    int col = 0, row = 0;
    if (!ms_grid_hit(&L, cx, cy, &col, &row)) return;
    st->cur_x = col;
    st->cur_y = row;
    ms_toggle_flag(st, col, row);
    gui64_invalidate_window(w);
}

static void ms_on_click(Window* w, int cx, int cy) {
    MinesState* st = ms_state(w);
    if (!st) return;
    if (ms_click_dup(st, cx, cy, 0)) return;
    ms_click_left(w, st, cx, cy);
}

static void ms_on_click2(Window* w, int cx, int cy, int button) {
    MinesState* st = ms_state(w);
    if (!st) return;
    if (ms_click_dup(st, cx, cy, button)) return;
    if (button == 1) ms_click_right(w, st, cx, cy);   // 右键标旗
    else             ms_click_left(w, st, cx, cy);
}

// ==================== 键盘 ====================
static void ms_on_key(Window* w, char c) {
    MinesState* st = ms_state(w);
    if (!st) return;
    unsigned char cc = (unsigned char)c;             // 方向键高位 >= 0x80，必须按无符号比
    if (cc == 0x1B) {                                // Esc：关窗（棋盘丢弃）
        ms_close_window(w);
        return;
    }
    int C = ms_cols_of(st->diff), R = ms_rows_of(st->diff);
    if (cc == 0xFD)      { if (st->cur_y > 0) st->cur_y--; }                  // 上
    else if (cc == 0xFE) { if (st->cur_y < R - 1) st->cur_y++; }              // 下
    else if (cc == 0xFB) { if (st->cur_x > 0) st->cur_x--; }                  // 左
    else if (cc == 0xFC) { if (st->cur_x < C - 1) st->cur_x++; }              // 右
    else if (cc == '\n' || cc == '\r') ms_open(st, st->cur_x, st->cur_y);     // Enter 翻开
    else if (cc == 'f' || cc == 'F')   ms_toggle_flag(st, st->cur_x, st->cur_y);
    else if (cc == 'r' || cc == 'R')   ms_state_clear(st);                     // R 重开
    else if (cc >= '1' && cc <= '3')   ms_set_diff(w, st, cc - '1');           // 1/2/3 难度
    else return;
    gui64_invalidate_window(w);
}

// ==================== 定时（计时 LED 按秒重绘 + 关窗检测）====================
static void ms_on_tick(Window* w) {
    ms_reap_closed();
    MinesState* st = ms_state(w);
    if (!st) return;
    int s = ms_sec(st);
    if (s != st->shown_sec) {                 // 只在秒变化时重绘（脏矩形外壳）
        st->shown_sec = s;
        gui64_invalidate_window(w);
    }
}

// ==================== 应用入口 ====================
void app_mines_open64() {
    ms_reap_closed();
    int live = gui64_app_windows(APP_ID_MINES);
    if (live >= MS_MAX_INST) {                       // 上限 4：只激活最近的，不新建
        Window* w = ms_latest_live();
        if (w) gui64_set_active(w);
        dbg64_str("[APP] mines limit reached (");
        dbg64_dec((uint64_t)MS_MAX_INST);
        dbg64_str(")");
        dbg64_nl();
        return;
    }
    int prev_owner = mem_owner_get_64();
    mem_owner_set_64(MEM_OWNER_MINES_64);
    MinesState* st = (MinesState*)kmalloc_64(sizeof(MinesState));
    mem_owner_set_64(prev_owner);
    if (!st) {
        dbg64_str("[APP] mines nomem");
        dbg64_nl();
        return;
    }
    // 清状态字段（不 memset 整个结构：字段逐个初始化，避免依赖 libc）
    st->magic = MS_MAGIC;
    st->diff = (g_ms_diff >= 0 && g_ms_diff <= 2) ? g_ms_diff : 0;
    st->inited = false;
    st->over = false;
    st->won = false;
    st->opened = 0;
    st->flags = 0;
    st->start_tick = 0;
    st->started = false;
    st->end_sec = 0;
    st->cur_x = 0;
    st->cur_y = 0;
    st->shown_sec = -1;
    st->log_cell = -1;
    st->log_cw = -1;
    st->log_ch = -1;
    st->dup_tick = 0;
    st->dup_x = -1;
    st->dup_y = -1;
    st->dup_btn = -1;
    st->reported = false;
    ms_state_clear(st);

    // 请求的客户区 = 该难度的"自然尺寸"（格 30px），但不超过屏幕可用范围
    int req_cw = ms_win_cw(st->diff);
    int req_ch = ms_win_ch(st->diff);
    int max_cw = gui64_screen_w() - 16;
    int max_ch = gui64_screen_h() - gui64_taskbar_h() - 40;
    if (max_cw < 120) max_cw = 120;
    if (max_ch < 120) max_ch = 120;
    if (req_cw > max_cw) req_cw = max_cw;
    if (req_ch > max_ch) req_ch = max_ch;

    // 外层尺寸估计（标题栏 + 边框），用于居中/级联摆放
    int est_w = req_cw + 10;
    int est_h = req_ch + 32;
    int inst = live;
    int wx = (gui64_screen_w() - est_w) / 2 + inst * 24;
    int wy = 48 + inst * 20;
    if (wx < 0) wx = 0;
    if (wx + est_w > gui64_screen_w()) wx = gui64_screen_w() - est_w;
    if (wx < 0) wx = 0;
    int max_y = gui64_screen_h() - gui64_taskbar_h() - est_h - 4;
    if (wy > max_y) wy = max_y < 0 ? 0 : max_y;

    Window* w = gui64_create_window(gui64_tr("Minesweeper", "扫雷"), wx, wy, est_w, est_h,
                                    ms_draw, ms_on_key, ms_on_click, APP_ID_MINES);
    if (!w) {
        st->magic = 0;
        int prev = mem_owner_get_64();
        mem_owner_set_64(MEM_OWNER_MINES_64);
        kfree_64(st);
        mem_owner_set_64(prev);
        dbg64_str("[APP] mines nomem");
        dbg64_nl();
        return;
    }
    w->userdata = st;                                  // 回调从这里取自己的棋盘
    gui64_set_click2(w, ms_on_click2);                 // 右键标旗
    gui64_set_tick(w, ms_on_tick);                     // 计时/关窗检测
    gui64_set_min_size(w, 260, 280);
    gui64_fit_window_to_client(w, req_cw, req_ch);     // 客户区对齐自然尺寸
    ms_register(w, st);
    g_ms_last = w;

    dbg64_str("[APP] mines opened diff=");
    dbg64_dec((uint64_t)st->diff);
    dbg64_nl();
    gui64_invalidate_window(w);
}

// 会话重置：清空**所有**实例的棋盘（窗口本身保留，同 32 位 ms_reset）
void app_mines_reset64() {
    ms_reap_closed();                                  // 顺手回收已被外壳关掉的实例
    int n = gui64_window_count();
    for (int i = 0; i < n; i++) {
        Window* w = gui64_window_at(i);
        if (!w || w->app_id != APP_ID_MINES) continue;
        MinesState* st = ms_state(w);
        if (!st) continue;
        ms_state_clear(st);
        st->diff = (g_ms_diff >= 0 && g_ms_diff <= 2) ? g_ms_diff : st->diff;
        gui64_invalidate_window(w);
    }
    dbg64_str("[APP] mines reset");
    dbg64_nl();
}
