// calc64.cpp - 计算器应用（Vimtu64 纯 64 位内核版）
// ============================================================================
// 来源：Vimtu32/kernel/gui.cpp 的"计算器"部分（CALC_* 宏、CALC_KEYS/CALC_LABELS、
//       CalcState、calc_layout / calc_parse / calc_format_q / calc_update_disp /
//       calc_do / calc_key / calc_click / calc_draw / calc_open），
//       行为以 32 位实现为准，接口按 kernel/gui64.h 的应用契约重排：
//         * 绘制在 draw 回调里用**屏幕绝对坐标**（w->client_x/y + 局部坐标），
//           外壳负责裁剪到客户区；
//         * 每实例状态挂 Window::userdata（kmalloc_64，归 MEM_OWNER_CALC_64）；
//         * 布局全部由客户区尺寸推导（参考 32 位 calc_layout，键位整体居中）。
//
// 与 32 位的差异（有意为之，详见各函数注释）：
//   1) 无浮点：Q16.16 定点整数运算，乘/除直接用 64 位整数指令（32 位是 32 位分解
//      + idiv 的变通写法；其 q16_mul 丢掉了 a1*a2 项，小数×小数偏差很大，
//      64 位这里用完整精度，见 q16_mul64 注释）；
//   2) 客户区几何取 client_x/client_y/client_w/client_h（不再假设标题栏 24px）；
//   3) font_draw_text 只有前景色，显示区/按键底色一律自己先 fb_fill_rect；
//   4) 布局日志按**每实例**去重（32 位是进程级 static：尺寸相同的第二个窗口
//      不会再打行；这里每个实例开窗后至少打一行，便于自动验收）；
//   5) 外壳的 Window 没有 on_close 钩子，所以"点 X / 任务管理器结束任务"这类
//      外壳主动关窗靠探活回收（calc_reap64，见该函数注释）。
//
// 串口日志（自动验收依赖，逐字保留）：
//   [APP] calc opened                 新建实例成功
//   [APP] calc reset                  清空所有实例状态
//   [APP] calc closed                 关窗（Esc 立即打；外壳关窗由探活补打）
//   [UI] calc layout key=WxH disp=WxH client=WxH   布局尺寸变化时打一行
//   [APP] calc limit reached (8)      到多开上限，只激活最近那个
//   [APP] calc no memory / [APP] calc window failed   失败路径
// ============================================================================
#include <stdint.h>

#include "gui64.h"
#include "fb.h"
#include "font.h"
#include "mem_64.h"
#include "debug64.h"

// ==================== 常量（与 32 位逐一对应）====================
#define CALC_PAD      8
#define CALC_DISP_W   239          // 默认窗口尺寸用（disp_w 实际由客户区宽度推导）
#define CALC_DISP_H   44
#define CALC_BW       56
#define CALC_BH       36
#define CALC_GAP      5
#define CALC_BUF_MAX  24           // 输入缓冲容量 / 显示串裁剪上限（32 位同值）
#define CALC_DISP_MAX 44           // disp[] 容量（32 位同值）
#define CALC_MAX_INST 8            // 多开上限（第 9 次只激活最近那个）
#define CALC_MIN_W    200          // 与 32 位 gui_min_size(APP_ID_CALC) 一致
#define CALC_MIN_H    240

// 颜色：0xAARRGGBB（与 32 位 calc_draw 用的三个色值一致；alpha 无意义，填 FF）
static const uint32_t CALC_COL_TEXT   = 0xFF000000u;   // 0x000000
static const uint32_t CALC_COL_BORDER = 0xFF808080u;   // 0x808080
static const uint32_t CALC_COL_DISP   = 0xFFF0F4F8u;   // 0xF0F4F8 显示区
static const uint32_t CALC_COL_KEY    = 0xFFF0F0F0u;   // 0xF0F0F0 普通键
static const uint32_t CALC_COL_KEY_TOP = 0xFFF8E8E8u;  // 0xF8E8E8 第一行（C/<-/%//）
static const uint32_t CALC_COL_KEY_OP = 0xFFE0E8F4u;   // 0xE0E8F4 运算符列

// 键位表：内部键码 / 显示标签（顺序与 32 位完全一致）
static const char CALC_KEYS[5][4] = {
    {'C', 'B', '%', '/'},
    {'7', '8', '9', '*'},
    {'4', '5', '6', '-'},
    {'1', '2', '3', '+'},
    {'N', '0', '.', '='},
};
static const char* const CALC_LABELS[5][4] = {
    {"C", "<-", "%", "/"},
    {"7", "8", "9", "*"},
    {"4", "5", "6", "-"},
    {"1", "2", "3", "+"},
    {"+/-", "0", ".", "="},
};

// ==================== 每实例状态（挂 Window::userdata）====================
struct CalcState64 {
    char    buf[CALC_BUF_MAX];    // 当前输入缓冲
    char    disp[CALC_DISP_MAX];  // 显示串（表达式 acc+op+buf / 结果 / "Error"）
    int64_t acc;                  // 累积值 Q16.16
    char    op;                   // 当前运算符
    bool    pending;              // 等待第二操作数
    bool    fresh;                // 新数字应覆盖 buf
    bool    err;                  // 错误（除零 / 数值过大 / 商超 Q16 范围）
    // 布局日志去重（每实例一份：开窗后第一帧必然打一行）
    int     last_kw, last_kh, last_cw, last_ch;
};

// ==================== 实例槽位表（探活 / 上限 / 最近实例用）====================
// 外壳不提供 on_close，也不替应用释放 userdata，所以这里自己记 (窗口, 状态) 对：
//   * 既能把状态指针和窗口指针分开保存（窗口被外壳释放后不去解引用它），
//   * 又能给 gui64_window_alive() 喂悬空指针做探活。
struct CalcSlot64 {
    Window*      win;
    CalcState64* st;
};
static CalcSlot64 g_slots[CALC_MAX_INST];

static int calc_strlen64(const char* s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

// 把状态恢复成干净初值（与 32 位 calc_state_clear 一致）
static void calc_state_clear64(CalcState64* st) {
    st->buf[0] = '0'; st->buf[1] = 0;
    st->disp[0] = '0'; st->disp[1] = 0;
    st->acc = 0; st->op = 0;
    st->pending = false; st->fresh = false; st->err = false;
    st->last_kw = -1; st->last_kh = -1; st->last_cw = -1; st->last_ch = -1;
}

static void calc_slot_remove_index64(int i) {
    if (i < 0 || i >= CALC_MAX_INST) return;
    for (int j = i; j + 1 < CALC_MAX_INST; j++) g_slots[j] = g_slots[j + 1];
    g_slots[CALC_MAX_INST - 1].win = nullptr;
    g_slots[CALC_MAX_INST - 1].st  = nullptr;
}

static int calc_slot_index64(Window* w) {
    for (int i = 0; i < CALC_MAX_INST; i++) if (g_slots[i].win == w) return i;
    return -1;
}

// 记入槽位（保持开窗顺序：末尾 = 最近打开的实例）
static void calc_slot_add64(Window* w, CalcState64* st) {
    // 窗口指针可能复用已关闭窗口的地址：先清掉同地址的陈旧槽（防止误判 closed）
    int old = calc_slot_index64(w);
    if (old >= 0) {
        CalcState64* dead = g_slots[old].st;
        calc_slot_remove_index64(old);
        if (dead && dead != st) kfree_64(dead);
    }
    for (int i = 0; i < CALC_MAX_INST; i++) {
        if (!g_slots[i].win) { g_slots[i].win = w; g_slots[i].st = st; return; }
    }
    // 正常到不了这里（open 前已确认实例数 < CALC_MAX_INST）
    if (st) kfree_64(st);
}

// 最近打开的实例（多开上限时激活它）
static Window* calc_most_recent64() {
    for (int i = CALC_MAX_INST - 1; i >= 0; i--) if (g_slots[i].win) return g_slots[i].win;
    return nullptr;
}

// 探活回收：外壳主动关窗（点 X、任务管理器结束任务、关全部窗口）时没有任何回调，
// 因此每次 tick / 打开 / reset 都扫一遍槽位：指针已不在外壳窗口表里 = 该实例已关闭，
// 释放状态并补打 "[APP] calc closed"。
static void calc_reap64() {
    for (int i = 0; i < CALC_MAX_INST; i++) {
        Window* w = g_slots[i].win;
        if (!w) continue;
        if (gui64_window_alive(w)) continue;
        CalcState64* st = g_slots[i].st;
        calc_slot_remove_index64(i);
        if (st) kfree_64(st);
        dbg64_str("[APP] calc closed");
        dbg64_nl();
        i--;   // 压实后同一下标已是别的槽位，重新检查
    }
}

static CalcState64* calc_state_of64(Window* w) {
    if (w && w->userdata) return (CalcState64*)w->userdata;
    return nullptr;
}

// 状态兜底：正常路径在 app_calc_open64 里挂好；万一外壳在挂之前就回调了
// （draw/click/tick），这里补一份，保证任何回调都有状态可用。
static CalcState64* calc_ensure_state64(Window* w) {
    if (!w) return nullptr;
    if (w->userdata) return (CalcState64*)w->userdata;
    mem_owner_set_64(MEM_OWNER_CALC_64);
    CalcState64* st = (CalcState64*)kmalloc_64((uint64_t)sizeof(CalcState64));
    mem_owner_set_64(MEM_OWNER_KERNEL_64);
    if (!st) return nullptr;
    calc_state_clear64(st);
    w->userdata = st;
    calc_slot_add64(w, st);
    return st;
}

// ==================== Q16.16 定点运算（只用整数）====================
// Q16.16 乘法：64 位下单条 imul 就够（输入已限制在 int32 范围内，|a*b| <= 2^62 不溢出）。
// 注：32 位版 q16_mul 用 32 位分解时丢掉了 a1*a2 项（1/256 级分量），小数×小数会明显偏小
//     （例：0.5*0.5 = 0）。64 位这里按标准 Q16.16 取完整精度，其余语义（截断/取整方向）
//     与 32 位一致。
static int64_t q16_mul64(int64_t a, int64_t b) {
    return (a * b) >> 16;      // floor(a*b / 65536)
}

// Q16.16 除法（|a/b| > 32767 视为溢出，与 32 位一致）
static int64_t q16_div64(int64_t a, int64_t b, CalcState64* st) {
    if (b == 0) { st->err = true; return 0; }
    int64_t absa = a < 0 ? -a : a;
    int64_t absb = b < 0 ? -b : b;
    if (absa > (absb << 15)) { st->err = true; return 0; }   // 商超 Q16 范围
    return (a << 16) / b;                                    // 64/64 除法，商已确认在 int32 内
}

// 定点四则运算（结果超 int32 Q16 范围置 err）
static int64_t calc_apply64(char op, int64_t a, int64_t b, CalcState64* st) {
    int64_t r = 0;
    switch (op) {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = q16_mul64(a, b); break;
        case '/': return q16_div64(a, b, st);
        default:  return b;
    }
    if (r > 0x7FFFFFFFLL || r < -0x7FFFFFFFLL - 1) { st->err = true; return 0; }
    return r;
}

// 解析输入串 -> Q16.16（带范围检查，逻辑与 32 位 calc_parse 相同）
static int64_t calc_parse64(const char* s, bool* err) {
    bool neg = false;
    if (*s == '-') { neg = true; s++; }
    int64_t ip = 0;
    while (*s >= '0' && *s <= '9') {
        ip = (ip << 3) + (ip << 1) + (*s - '0');
        if (ip > 32767) { *err = true; return 0; }
        s++;
    }
    int64_t q = ip << 16;
    if (*s == '.') {
        s++;
        int64_t fp = 0;
        int digits = 0;
        int64_t scale = 1;
        while (*s >= '0' && *s <= '9' && digits < 5) {
            fp = (fp << 3) + (fp << 1) + (*s - '0');
            scale = (scale << 3) + (scale << 1);
            digits++;
            s++;
        }
        if (digits > 0) q += (fp << 16) / scale;
    }
    return neg ? -q : q;
}

// Q16.16 -> 显示串（最多 5 位小数，去尾零；与 32 位 calc_format_q 相同）
static void calc_format_q64(int64_t v, char* out, int maxlen) {
    bool neg = v < 0;
    if (neg) v = -v;
    int64_t ip = v >> 16;
    int64_t frac = ((v & 0xFFFF) * 100000) >> 16;   // 0..99999
    int pos = 0;
    if (neg && pos < maxlen - 1) out[pos++] = '-';
    char tmp[16];
    int tn = 0;
    if (ip == 0) tmp[tn++] = '0';
    while (ip > 0 && tn < 15) { tmp[tn++] = (char)('0' + (int)(ip % 10)); ip /= 10; }
    while (tn > 0 && pos < maxlen - 1) out[pos++] = tmp[--tn];
    if (frac > 0) {
        char fbuf[8];
        int64_t f = frac;
        for (int i = 4; i >= 0; i--) { fbuf[i] = (char)('0' + (int)(f % 10)); f /= 10; }
        int end = 5;
        while (end > 0 && fbuf[end - 1] == '0') end--;
        if (pos < maxlen - 1) out[pos++] = '.';
        for (int i = 0; i < end && pos < maxlen - 1; i++) out[pos++] = fbuf[i];
    }
    out[pos] = 0;
}

// 重建显示串：无运算时 = buf；有运算时 = "acc 运算符 [当前输入]"；错误 = "Error"
static void calc_update_disp64(CalcState64* st) {
    int n = 0;
    if (st->err) {
        const char* e = "Error";
        while (e[n] && n < CALC_DISP_MAX - 1) { st->disp[n] = e[n]; n++; }
        st->disp[n] = 0;
        return;
    }
    if (st->pending && st->op) {
        char acc_s[16];
        calc_format_q64(st->acc, acc_s, (int)sizeof(acc_s));
        int i = 0;
        while (acc_s[i] && n < CALC_DISP_MAX - 1) { st->disp[n++] = acc_s[i++]; }
        if (n < CALC_DISP_MAX - 1) st->disp[n++] = st->op;
        if (!st->fresh) {
            int m = 0;
            while (st->buf[m] && n < CALC_DISP_MAX - 1) { st->disp[n++] = st->buf[m++]; }
        }
    } else {
        int m = 0;
        while (st->buf[m] && n < CALC_DISP_MAX - 1) { st->disp[n++] = st->buf[m++]; }
    }
    st->disp[n] = 0;
}

// ==================== 布局（按客户区尺寸推导，键位整体居中）====================
struct CalcLayout64 {
    int pad, gap;
    int disp_w, disp_h;      // 显示区（客户区局部坐标）
    int key_w, key_h;        // 单个按键
    int grid_y;              // 键位网格起始 y（客户区局部坐标）
};

// 与 32 位 calc_layout 逐行相同，只是输入换成客户区宽高（cw/ch）
static void calc_layout64(int cw, int ch, CalcLayout64* L) {
    L->pad = (cw < 240 || ch < 260) ? 6 : 8;
    L->gap = (cw < 240 || ch < 260) ? 4 : 5;
    L->disp_w = cw - L->pad * 2;
    if (L->disp_w < 80) L->disp_w = 80;
    L->disp_h = ch / 5;
    if (L->disp_h < 30) L->disp_h = 30;
    if (L->disp_h > 72) L->disp_h = 72;
    int avail_w = cw - L->pad * 2 - L->gap * 3;
    int top = L->pad + L->disp_h + L->pad;
    int avail_h = ch - top - L->pad - L->gap * 4;
    if (avail_w < 40) avail_w = 40;
    if (avail_h < 40) avail_h = 40;
    L->key_w = avail_w / 4;
    L->key_h = avail_h / 5;
    if (L->key_w < 22) L->key_w = 22;
    if (L->key_h < 14) L->key_h = 14;
    if (L->key_w > 96) L->key_w = 96;
    if (L->key_h > 64) L->key_h = 64;
    int grid_h = L->key_h * 5 + L->gap * 4;
    L->grid_y = top + ((avail_h - grid_h) / 2);      // 剩余空间里垂直居中
    if (L->grid_y < top) L->grid_y = top;
}

// ==================== 按键执行 ====================
// 与 32 位 calc_do 逐分支相同（连续运算、除零置 Error、超长输入截断、
// 出错后仅 C 可恢复等）
static void calc_do64(Window* w, char key) {
    CalcState64* st = calc_ensure_state64(w);
    if (!st) return;
    if (st->err && key != 'C') return;   // 出错后仅 C 可恢复
    bool perr = false;
    switch (key) {
        case 'C':
            st->buf[0] = '0'; st->buf[1] = 0;
            st->acc = 0; st->op = 0;
            st->pending = false; st->fresh = false; st->err = false;
            break;
        case 'B':   // 退格
            if (st->fresh) break;
            {
                int n = calc_strlen64(st->buf);
                if (n <= 1 || (n == 2 && st->buf[0] == '-')) {
                    st->buf[0] = '0'; st->buf[1] = 0; st->fresh = true;
                } else {
                    st->buf[n - 1] = 0;
                }
            }
            break;
        case 'N':   // 正负号
            if (st->fresh) {
                int64_t v = calc_parse64(st->buf, &perr);
                if (!perr) calc_format_q64(-v, st->buf, CALC_BUF_MAX);
            } else {
                int n = calc_strlen64(st->buf);
                if (n == 0 || (st->buf[0] == '0' && n == 1)) break;
                if (st->buf[0] == '-') {
                    for (int i = 1; i <= n; i++) st->buf[i - 1] = st->buf[i];
                } else {
                    for (int i = n; i >= 0; i--) st->buf[i + 1] = st->buf[i];
                    st->buf[0] = '-';
                }
            }
            break;
        case '%':
            {
                int64_t v = calc_parse64(st->buf, &perr);
                if (perr) break;
                if (st->pending && st->op) {
                    int64_t p = q16_mul64(st->acc, v);      // acc*v (Q16)
                    if (p > 0x7FFFFFFFLL || p < -0x7FFFFFFFLL - 1) p = 0x7FFFFFFFLL;
                    v = p / 100;                             // 基于 acc 的百分比
                } else {
                    v = v / 100;                             // 无运算时直接 /100
                }
                calc_format_q64(v, st->buf, CALC_BUF_MAX);
                st->fresh = true;
            }
            break;
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
            if (st->fresh) { st->buf[0] = '0'; st->buf[1] = 0; st->fresh = false; }
            if (st->buf[0] == '0' && st->buf[1] == 0) {
                st->buf[0] = key; st->buf[1] = 0;
            } else {
                int n = calc_strlen64(st->buf);
                if (n < CALC_BUF_MAX - 1) { st->buf[n] = key; st->buf[n + 1] = 0; }
            }
            break;
        case '.':
            if (st->fresh) { st->buf[0] = '0'; st->buf[1] = 0; st->fresh = false; }
            {
                bool has_dot = false;
                for (int i = 0; st->buf[i]; i++) if (st->buf[i] == '.') has_dot = true;
                if (!has_dot) {
                    int n = calc_strlen64(st->buf);
                    if (n < CALC_BUF_MAX - 1) { st->buf[n] = '.'; st->buf[n + 1] = 0; }
                }
            }
            break;
        case '+': case '-': case '*': case '/':
            {
                if (st->pending && st->op && st->fresh) {
                    st->op = key;   // 连续按运算符：替换
                    break;
                }
                int64_t v = calc_parse64(st->buf, &perr);
                if (perr) break;
                if (st->pending && st->op) {
                    st->acc = calc_apply64(st->op, st->acc, v, st);
                    if (st->err) break;
                    calc_format_q64(st->acc, st->buf, CALC_BUF_MAX);
                } else {
                    st->acc = v;
                }
                st->op = key;
                st->pending = true;
                st->fresh = true;
            }
            break;
        case '=':
            if (st->pending && st->op) {
                int64_t v = calc_parse64(st->buf, &perr);
                if (perr) break;
                st->acc = calc_apply64(st->op, st->acc, v, st);
                if (st->err) break;
                calc_format_q64(st->acc, st->buf, CALC_BUF_MAX);
                st->pending = false;
                st->op = 0;
                st->fresh = true;
            }
            break;
        default:
            break;
    }
    calc_update_disp64(st);
    gui64_invalidate_window(w);   // 32 位在这里置 dirty=true（外壳据此重绘）
}

// ==================== 绘制 ====================
static void calc_draw64(Window* w) {
    CalcState64* st = calc_ensure_state64(w);
    if (!st) return;
    CalcLayout64 L;
    calc_layout64(w->client_w, w->client_h, &L);

    // 布局可观测：尺寸变化时写一行（自动验收用；每实例各自去重）
    if (L.key_w != st->last_kw || L.key_h != st->last_kh ||
        w->client_w != st->last_cw || w->client_h != st->last_ch) {
        st->last_kw = L.key_w; st->last_kh = L.key_h;
        st->last_cw = w->client_w; st->last_ch = w->client_h;
        dbg64_str("[UI] calc layout key=");
        dbg64_dec((uint64_t)L.key_w); dbg64_str("x"); dbg64_dec((uint64_t)L.key_h);
        dbg64_str(" disp=");
        dbg64_dec((uint64_t)L.disp_w); dbg64_str("x"); dbg64_dec((uint64_t)L.disp_h);
        dbg64_str(" client=");
        dbg64_dec((uint64_t)w->client_w); dbg64_str("x"); dbg64_dec((uint64_t)w->client_h);
        dbg64_nl();
    }

    const int x0 = w->client_x, y0 = w->client_y;   // 客户区左上（屏幕绝对坐标）

    // ---- 显示区：先铺底色（font 只有前景色），再画边框、右对齐文本 ----
    const int dx = x0 + L.pad, dy = y0 + L.pad;
    fb_fill_rect(dx, dy, L.disp_w, L.disp_h, CALC_COL_DISP);
    fb_draw_rect(dx, dy, L.disp_w, L.disp_h, CALC_COL_BORDER);
    char disp[CALC_BUF_MAX];
    {
        int i = 0;
        while (st->disp[i] && i < CALC_BUF_MAX - 1) { disp[i] = st->disp[i]; i++; }
        disp[i] = 0;
    }
    int tw = font_text_width(disp);
    if (tw > L.disp_w - 12) {   // 超宽：从左侧截断保留右侧表达式
        const char* p = disp;
        while (*p && font_text_width(p) > L.disp_w - 12) p++;
        int k = 0;
        while (p[k] && k < CALC_BUF_MAX - 1) { disp[k] = p[k]; k++; }
        disp[k] = 0;
        tw = font_text_width(disp);
    }
    int tx = dx + L.disp_w - 6 - tw;
    if (tx < dx + 4) tx = dx + 4;
    font_draw_text(tx, dy + (L.disp_h - font_line_height()) / 2, disp, CALC_COL_TEXT);

    // ---- 按键（尺寸随客户区缩放，整体居中）----
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 4; col++) {
            int bx = x0 + L.pad + col * (L.key_w + L.gap);
            int by = y0 + L.grid_y + row * (L.key_h + L.gap);
            uint32_t bg = CALC_COL_KEY;
            if (row == 0) bg = CALC_COL_KEY_TOP;            // C / <- / % / /
            else if (col == 3) bg = CALC_COL_KEY_OP;        // 运算符列
            fb_fill_rect(bx, by, L.key_w, L.key_h, bg);
            fb_draw_rect(bx, by, L.key_w, L.key_h, CALC_COL_BORDER);
            const char* label = CALC_LABELS[row][col];
            int lw = font_text_width(label);
            int tlx = bx + (L.key_w - lw) / 2;
            if (tlx < bx) tlx = bx;
            font_draw_text(tlx, by + (L.key_h - font_line_height()) / 2, label, CALC_COL_TEXT);
        }
    }
}

// ==================== 鼠标点击（客户区坐标）====================
static void calc_click64(Window* w, int cx, int cy) {
    if (!w) return;
    CalcLayout64 L;
    calc_layout64(w->client_w, w->client_h, &L);
    int bx0 = L.pad, by0 = L.grid_y;
    if (cx < bx0 || cy < by0) return;
    int step_x = L.key_w + L.gap, step_y = L.key_h + L.gap;
    if (step_x <= 0 || step_y <= 0) return;
    int col = (cx - bx0) / step_x;
    int row = (cy - by0) / step_y;
    if (col < 0 || col >= 4 || row < 0 || row >= 5) return;
    int rx = (cx - bx0) % step_x;
    int ry = (cy - by0) % step_y;
    if (rx >= L.key_w || ry >= L.key_h) return;   // 落在键间空隙
    calc_do64(w, CALC_KEYS[row][col]);
}

// ==================== 关窗（Esc / 外壳关窗回收）====================
static void calc_close64(Window* w) {
    if (!w) return;
    CalcState64* st = nullptr;
    int idx = calc_slot_index64(w);
    if (idx >= 0) { st = g_slots[idx].st; calc_slot_remove_index64(idx); }
    if (!st) st = calc_state_of64(w);
    w->userdata = nullptr;          // 外壳不负责释放，先置空更安全
    gui64_destroy_window(w);        // 之后 w 可能失效，不要再解引用
    if (st) kfree_64(st);
    dbg64_str("[APP] calc closed");
    dbg64_nl();
}

// ==================== 键盘输入 ====================
static void calc_key64(Window* w, char c) {
    unsigned char uc = (unsigned char)c;
    if (uc == 0x1B) {   // Esc：统一"关窗"约定（与点 X 等价，状态随实例丢弃）
        calc_close64(w);
        return;
    }
    int key = -1;
    if (c >= '0' && c <= '9') key = c;
    else if (c == '.') key = '.';
    else if (c == '+') key = '+';
    else if (c == '-') key = '-';
    else if (c == '*') key = '*';
    else if (c == '/') key = '/';
    else if (c == '=' || c == '\n' || c == '\r') key = '=';
    else if (uc == 8 || uc == 0x7F) key = 'B';                        // Backspace
    else if (c == 'c' || c == 'C' || c == 'r' || c == 'R') key = 'C'; // C / R 清零
    else if (c == 'b' || c == 'B') key = 'B';                         // B 退格（等同 <-）
    else if (c == '%') key = '%';
    else if (c == 'n' || c == 'N') key = 'N';
    if (key >= 0) calc_do64(w, (char)key);
}

// ==================== 定时回调：只做关窗探活 ====================
static void calc_tick64(Window* w) {
    (void)w;
    calc_reap64();
}

// ==================== 应用入口 ====================
// 打开：每次新建一个实例（上限 8 个；第 9 次只激活最近那个）
void app_calc_open64() {
    calc_reap64();   // 先回收外壳已关掉的实例（没有 on_close 钩子）
    int inst = gui64_app_windows(APP_ID_CALC);
    if (inst >= CALC_MAX_INST) {
        Window* last = calc_most_recent64();
        if (last) gui64_set_active(last);
        dbg64_str("[APP] calc limit reached (");
        dbg64_dec((uint64_t)CALC_MAX_INST);
        dbg64_str(")");
        dbg64_nl();
        return;
    }

    mem_owner_set_64(MEM_OWNER_CALC_64);
    CalcState64* st = (CalcState64*)kmalloc_64((uint64_t)sizeof(CalcState64));
    mem_owner_set_64(MEM_OWNER_KERNEL_64);
    if (!st) {
        dbg64_str("[APP] calc no memory");
        dbg64_nl();
        return;
    }
    calc_state_clear64(st);   // 新实例一定是干净状态（"软件内容不保存"）

    // 默认窗口尺寸：与 32 位 calc_open 相同的算式（255 x 292）
    int win_w = CALC_PAD * 2 + CALC_BW * 4 + CALC_GAP * 3;
    int win_h = 24 + CALC_PAD * 3 + CALC_DISP_H + CALC_BH * 5 + CALC_GAP * 4;
    int px = 150 + inst * 26, py = 80 + inst * 22;   // 多开时错开摆放
    // 屏幕内钳制（多开后不让窗口跑出屏幕 / 压到任务栏）
    int sw = gui64_screen_w();
    int sh = gui64_screen_h() - gui64_taskbar_h();
    if (sw < 320) sw = 320;
    if (sh < 240) sh = 240;
    if (px + win_w > sw - 8) px = sw - 8 - win_w;
    if (py + win_h > sh - 8) py = sh - 8 - win_h;
    if (px < 8) px = 8;
    if (py < 8) py = 8;

    Window* w = gui64_create_window(gui64_tr("Calculator", "计算器"),
                                    px, py, win_w, win_h,
                                    calc_draw64, calc_key64, calc_click64,
                                    APP_ID_CALC);
    if (!w) {
        kfree_64(st);
        dbg64_str("[APP] calc window failed");
        dbg64_nl();
        return;
    }
    w->userdata = st;                        // 每实例一份状态
    calc_slot_add64(w, st);
    gui64_set_min_size(w, CALC_MIN_W, CALC_MIN_H);
    gui64_set_tick(w, calc_tick64);          // 关窗探活（60Hz）
    if (!w->active) gui64_set_active(w);     // 外壳一般会激活新窗口，这里兜底

    dbg64_str("[APP] calc opened");
    dbg64_nl();
}

// 会话重置：清空**所有**实例的内容（32 位 calc_reset 语义）
void app_calc_reset64() {
    dbg64_str("[APP] calc reset");
    dbg64_nl();
    calc_reap64();
    for (int i = 0; i < CALC_MAX_INST; i++) {
        if (g_slots[i].st) calc_state_clear64(g_slots[i].st);
    }
    for (int i = 0; i < CALC_MAX_INST; i++) {
        if (g_slots[i].win) gui64_invalidate_window(g_slots[i].win);   // 显示回到 0
    }
}
