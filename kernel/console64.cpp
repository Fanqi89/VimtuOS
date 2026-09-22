// console64.cpp - 开机滚屏引导控制台（boot console）+ 启动日志环形缓冲 + dmesg
//
// 见 kernel/console64.h 的接口说明。本文件四块：
//   1) 环形缓冲：dbg64_putc() 的唯一出口镜像进来的字符 -> 攒行 -> 存进 16 KiB 环（含 tick/等级）
//      * 状态（g_ring64）放在 section(".data.con64")：kmain64 的 bss_clear_64() 不会清掉它，
//        所以"[LM64] ENTERED LONG MODE"这几行也能被缓存（它们打在清 BSS 之前）。
//      * dbg64.h 的 sink 指针是 .bss（零初始化）—— 清完 BSS 必须 con64_rehook64() 重挂。
//      * 环形满 -> 覆盖最旧的行（dropped 计数）；头部 16 行另存一份"头部保留区"，
//        这样 dmesg 里永远看得到最早的启动行（长模式/BSS/BootInfo/E820）。
//   2) 屏幕控制台：等宽面（face 2）逐字符画；黑底；行首 Linux 风格时间戳；
//      按关键字着色（FAIL/PANIC=红、WARN=黄、其余浅灰）；行满整体上移一行（像素行拷贝滚屏）；
//      每次只提交受影响的行/区域（fb_flip_region）。
//   3) 不停留 / 可跳过：按节奏滚完立即返回（滚屏总时长有界 ≈0.5~1.2s），任意键可提前结束（并吞掉这个键）。
//   4) dmesg 数据源 + 串口证据（终端命令在 kernel/terminal64.cpp）。
//
// 有界性（保证既有验收脚本不超时）：
//   * 回放行数 <= CON64_REPLAY_MAX(320)，每行只画一次；滚屏是像素拷贝，不重新光栅化字形；
//   * 滚屏节奏：总时长目标 CON64_SCROLL_TOTAL_TICKS（≈1000ms），按行数摊到每行（1..CON64_SCROLL_MAX_PER_LINE tick）；
//   * 总屏上时间 = 回放（与日志量成正比、有上限 ≈0.5~1.2s），**没有停留阶段**（实测见 [CON64] replay ... ms=）。
#include "console64.h"
#include "debug64.h"      // dbg64_* + 行锁 + sink 挂钩 + irq save/restore
#include "fb.h"           // 帧缓冲（黑底 + fb_flip_region 局部提交）
#include "font.h"         // 四个字体面（本文件只用 face 2 = 终端等宽）
#include "input.h"        // kbd_has_char / kbd_pop_char（任意键跳过）
#include "x86_64.h"       // g_ticks64 / TICK_MS_64


// ==================== 尺寸常量 ====================
// 两个"少搬像素"的优化常量：
//   * 回放时攒 CON64_SCROLL_BATCH 行才整体上移一次（总像素搬运量 ≈ 1/批大小；屏上依旧连续上滚）；
//   * 行宽表 row_ink 记录每行真的画了多少列 —— 滚屏只搬"有墨"的那段宽度（不是整屏宽）。
#define CON64_ROWS_MAX    64                    // 控制台最大行数（行宽表上限；1280x800 下是 39 行）
#define CON64_SCROLL_BATCH 8                    // 回放时攒 8 行再整体上移（滚动步数适中：屏上明显在滚，整屏拷贝/flip 也不多）
#define CON64_STAMP_CHARS 14                    // "[    0.123456]"（Linux 的 %5lu.%06lu）
#define CON64_REC_HDR     16                    // 每条记录的头：tick(8) + len(2) + level(1) + flags(1) + 保留(4)
#define CON64_REC_MAX     (CON64_REC_HDR + CON64_TEXT_MAX)
#define CON64_MARGIN_X    4
#define CON64_MARGIN_Y    4
#define CON64_C_BG       0xFF000000u            // 黑底
#define CON64_C_INFO     0xFFD0D0D0u            // 浅灰（info）
#define CON64_C_WARN     0xFFFFD75Fu            // 黄（warn）
#define CON64_C_ERROR    0xFFFF5F5Fu            // 红（error/FAILED）
#define CON64_C_STAMP    0xFF7F8C8Du            // 时间戳（暗灰）

// ==================== 环形缓冲（.data，不是 .bss！）====================
struct Con64HeadLine {
    uint64_t tick;
    uint16_t len;
    uint16_t level;
    char     text[CON64_HEAD_TEXT];
};

struct Con64Ring {
    uint8_t  buf[CON64_RING_BYTES];      // 记录区（变长记录：[tail, tail+used) 连续存放）
    uint32_t tail;                       // 最旧记录的偏移
    uint32_t used;                       // 记录区字节数
    uint32_t count;                      // 记录（行）数
    uint32_t dropped;                    // 因满被覆盖丢弃的行数
    uint32_t total;                      // 累计推送的行数（含被丢的）
    uint32_t init_done;                  // 1 = 已初始化
    uint32_t logged_init;                // [CON64] ring init 只打一次
    uint32_t trunc_lines;                // 被截断（>CON64_TEXT_MAX）的行数
    // 行累积器（sink 逐字符攒一行）
    char     cur[CON64_TEXT_MAX + 4];
    uint32_t cur_len;
    uint32_t cur_trunc;
    uint32_t cur_started;
    uint64_t cur_tick;
    // 头部保留区（最早来的 CON64_HEAD_LINES 行；环形覆盖不到它们）
    uint32_t head_n;
    Con64HeadLine head[CON64_HEAD_LINES];
};

// ★ section(".data.con64")：链接脚本的 .data 里有 *(.data.*)，所以它落在 .data（PROGBITS），
//   而 bss_clear_64() 只清 [__bss_start, __bss_end) —— 缓冲不会被清掉。
static Con64Ring g_ring64 __attribute__((section(".data.con64"), aligned(16)));

// ==================== 屏幕控制台状态（这块在 .bss：只在 bss 清零之后使用）====================
struct Con64Scr {
    int  active;
    int  cols;
    int  rows;
    int  line_h;
    int  adv;             // 等宽面 ASCII 推进宽度（8px）
    int  ts_x;            // 时间戳起始 x
    int  text_x;          // 正文起始 x
    int  top_y;
    int  region_w;        // 控制台区域宽（= text_x + cols*adv + 边距；正文绝不超出它）
    int  region_h;
    int  drawn;           // 屏上已有的行数（<= rows）
    int  skip;            // 1 = 用户按键结束
    int  shown_total;     // 已经画上屏的行数（按 ring.total 计数，实时追加用）
    int  pending;         // 攒着还没上移的行数（回放时按 CON64_SCROLL_BATCH 批量滚）
    uint8_t row_ink[CON64_ROWS_MAX];   // 每一行真的画了多少列（0 = 空行）—— 滚屏只搬"有墨"的宽度
    // 批量滚屏时暂存的待画行（最多 CON64_SCROLL_BATCH-1 行；保证屏上不丢行）
    uint64_t pend_tick[CON64_SCROLL_BATCH];
    uint8_t  pend_lv[CON64_SCROLL_BATCH];
    char     pend_text[CON64_SCROLL_BATCH][CON64_TEXT_MAX + 8];
    int      pend_n;
};

static Con64Scr g_scr64;

// ==================== 小工具 ====================
static bool con64_pre64(const char* s, const char* pre) {
    if (!s || !pre) return false;
    while (*pre) { if (*s++ != *pre++) return false; }
    return true;
}

int con64_classify64(const char* s) {
    if (!s) return CON64_LV_INFO;
    bool warn = false;
    for (const char* p = s; *p; p++) {
        if (con64_pre64(p, "PANIC")) return CON64_LV_ERROR;
        if (con64_pre64(p, "FAIL"))  return CON64_LV_ERROR;   // FAIL / FAILED
        if (con64_pre64(p, "WARN"))  warn = true;
    }
    return warn ? CON64_LV_WARN : CON64_LV_INFO;
}

const char* con64_level_name64(int lv) {
    return lv == CON64_LV_ERROR ? "error" : (lv == CON64_LV_WARN ? "warn" : "info");
}

int con64_stamp64(uint64_t tick, char* out, int out_max) {
    if (!out || out_max < 8) return 0;
    const uint64_t ms = tick * (uint64_t)TICK_MS_64;
    uint64_t sec = ms / 1000u;
    const uint32_t usec = (uint32_t)((ms % 1000u) * 1000u);
    char sd[24];
    int sn = 0;
    if (sec == 0) sd[sn++] = '0';
    while (sec > 0 && sn < 20) { sd[sn++] = (char)('0' + (int)(sec % 10)); sec /= 10; }
    int o = 0;
    if (o >= out_max - 1) return 0;
    out[o++] = '[';
    for (int i = sn; i < 5 && o < out_max - 1; i++) out[o++] = ' ';      // 秒右对齐到 5 位
    while (sn > 0 && o < out_max - 1) out[o++] = sd[--sn];
    if (o < out_max - 1) out[o++] = '.';
    // 微秒：6 位，左补 0
    {
        char us[6];
        int un = 0;
        uint32_t u = usec;
        if (u == 0) us[un++] = '0';
        while (u > 0 && un < 6) { us[un++] = (char)('0' + (int)(u % 10)); u /= 10; }
        while (un < 6) us[un++] = '0';
        while (un > 0 && o < out_max - 1) out[o++] = us[--un];
    }
    if (o < out_max - 1) out[o++] = ']';
    out[o] = 0;
    return o;
}

// ==================== 环形缓冲 ====================
static uint32_t con64_rec_len64(uint32_t off) {
    return (uint32_t)g_ring64.buf[off + 8] | ((uint32_t)g_ring64.buf[off + 9] << 8);
}
static uint64_t con64_rec_tick64(uint32_t off) {
    uint64_t t = 0;
    for (int k = 7; k >= 0; k--) t = (t << 8) | g_ring64.buf[off + (uint32_t)k];
    return t;
}
static const char* con64_rec_text64(uint32_t off) { return (const char*)(g_ring64.buf + off + CON64_REC_HDR); }
static int con64_rec_level64(uint32_t off) { return (int)g_ring64.buf[off + 10]; }

static void con64_evict_oldest64() {
    if (g_ring64.count == 0) return;
    const uint32_t step = CON64_REC_HDR + con64_rec_len64(g_ring64.tail);
    g_ring64.tail += step;
    g_ring64.used -= step;
    g_ring64.count--;
    g_ring64.dropped++;
    if (g_ring64.used == 0) { g_ring64.tail = 0; }
}

static void con64_push_line64(uint64_t tick, int level, uint32_t truncated,
                              const char* text, uint32_t len) {
    if (len > CON64_TEXT_MAX) len = CON64_TEXT_MAX;
    const uint32_t need = CON64_REC_HDR + len;
    // 1) 腾空间（环形：丢最旧的；每丢一行 dropped+1）
    while (g_ring64.used + need > CON64_RING_BYTES && g_ring64.count > 0) con64_evict_oldest64();
    if (g_ring64.used + need > CON64_RING_BYTES) return;     // 理论上不会发生（need <= 176）
    // 2) 保证记录区在 [tail, tail+used) 里连续：必要时整体搬到 0
    if (g_ring64.tail + g_ring64.used + need > CON64_RING_BYTES) {
        for (uint32_t i = 0; i < g_ring64.used; i++) g_ring64.buf[i] = g_ring64.buf[g_ring64.tail + i];
        g_ring64.tail = 0;
    }
    const uint32_t off = g_ring64.tail + g_ring64.used;
    for (int k = 0; k < 8; k++) g_ring64.buf[off + (uint32_t)k] = (uint8_t)((tick >> (8 * k)) & 0xFF);
    g_ring64.buf[off + 8] = (uint8_t)(len & 0xFF);
    g_ring64.buf[off + 9] = (uint8_t)((len >> 8) & 0xFF);
    g_ring64.buf[off + 10] = (uint8_t)level;
    g_ring64.buf[off + 11] = (uint8_t)(truncated ? 1 : 0);
    g_ring64.buf[off + 12] = g_ring64.buf[off + 13] = 0;
    g_ring64.buf[off + 14] = g_ring64.buf[off + 15] = 0;
    for (uint32_t i = 0; i < len; i++) g_ring64.buf[off + CON64_REC_HDR + i] = (uint8_t)text[i];
    g_ring64.used += need;
    g_ring64.count++;
    g_ring64.total++;
    if (truncated) g_ring64.trunc_lines++;
    // 3) 头部保留区（最早来的 16 行：长模式/BSS/BootInfo/E820…）
    if (g_ring64.head_n < CON64_HEAD_LINES) {
        Con64HeadLine* h = &g_ring64.head[g_ring64.head_n];
        h->tick = tick;
        h->len = (uint16_t)len;
        h->level = (uint16_t)level;
        for (uint32_t i = 0; i < len; i++) h->text[i] = text[i];
        h->text[len] = 0;
        g_ring64.head_n++;
    }
}

static uint32_t con64_rec_off64(uint32_t idx) {
    uint32_t off = g_ring64.tail;
    for (uint32_t i = 0; i < idx && i < g_ring64.count; i++) off += CON64_REC_HDR + con64_rec_len64(off);
    return off;
}

// 行累积器收尾：把一行推成一条记录
static void con64_finish_line64() {
    if (!g_ring64.cur_started) return;                 // 空行（只有 \r\n）：不记
    uint32_t len = g_ring64.cur_len;
    const uint32_t trunc = g_ring64.cur_trunc;
    if (trunc && len + 3 <= CON64_TEXT_MAX) {          // 截断标记（行内上限 160 = 157 字符 + "..."）
        g_ring64.cur[len++] = '.';
        g_ring64.cur[len++] = '.';
        g_ring64.cur[len++] = '.';
    }
    g_ring64.cur[len] = 0;
    con64_push_line64(g_ring64.cur_tick, con64_classify64(g_ring64.cur), trunc,
                      g_ring64.cur, len);
    g_ring64.cur_len = 0;
    g_ring64.cur_trunc = 0;
    g_ring64.cur_started = 0;
}

// ★ 唯一出口挂钩：dbg64_putc() 每写一个字符都会调到这里（不管是内核哪个模块、哪一行）
static void con64_sink64(char c) {
    if (!g_ring64.init_done) return;
    const uint64_t fl = dbg64_irq_save64();            // 与串口一样：多任务/中断里攒行不互相插行
    if (c == '\r') { dbg64_irq_restore64(fl); return; }
    if (c == '\n') { con64_finish_line64(); dbg64_irq_restore64(fl); return; }
    if (!g_ring64.cur_started) {
        g_ring64.cur_started = 1;
        g_ring64.cur_tick = g_ticks64;                 // Linux 也是"行首"时间戳
    }
    if (g_ring64.cur_len < (uint32_t)(CON64_TEXT_MAX - 3)) {
        g_ring64.cur[g_ring64.cur_len++] = c;
    } else {
        g_ring64.cur_trunc = 1;                        // 超长：丢字符，行尾补 "..."
    }
    dbg64_irq_restore64(fl);
}

void con64_rehook64() { dbg64_set_sink64(&con64_sink64); }

void con64_init64() {
    if (!g_ring64.init_done) {
        g_ring64.tail = 0;
        g_ring64.used = 0;
        g_ring64.count = 0;
        g_ring64.dropped = 0;
        g_ring64.total = 0;
        g_ring64.trunc_lines = 0;
        g_ring64.cur_len = 0;
        g_ring64.cur_trunc = 0;
        g_ring64.cur_started = 0;
        g_ring64.cur_tick = 0;
        g_ring64.head_n = 0;
        g_ring64.init_done = 1;
    }
    con64_rehook64();                                  // 幂等：清 BSS 之后重挂
    if (!g_ring64.logged_init) {
        g_ring64.logged_init = 1;
        dbg64_line_begin64();
        dbg64_str("[CON64] ring init bytes=");
        dbg64_dec((uint64_t)CON64_RING_BYTES);
        dbg64_str(" lines=");
        dbg64_dec((uint64_t)(CON64_RING_BYTES / CON64_REC_MAX));   // 按最长行的行数容量
        dbg64_str(" text_max=");
        dbg64_dec((uint64_t)CON64_TEXT_MAX);
        dbg64_nl();
        dbg64_line_end64();
    }
}

int con64_inited64()          { return g_ring64.init_done ? 1 : 0; }
int con64_ring_bytes64()      { return (int)CON64_RING_BYTES; }
int con64_text_max64()        { return (int)CON64_TEXT_MAX; }
int con64_buffered_lines64()  { return (int)g_ring64.count; }
int con64_dropped_lines64()   { return (int)g_ring64.dropped; }
int con64_head_lines64()      { return (int)g_ring64.head_n; }
int con64_trunc_lines64()     { return (int)g_ring64.trunc_lines; }

static int con64_copy_out64(const char* src, uint32_t len, char* out, int out_max) {
    if (!out || out_max <= 0) return 0;
    uint32_t n = len;
    if (n > (uint32_t)(out_max - 1)) n = (uint32_t)(out_max - 1);
    for (uint32_t i = 0; i < n; i++) out[i] = src[i];
    out[n] = 0;
    return (int)n;
}

int con64_line64(int i, uint64_t* tick, int* level, char* out, int out_max) {
    if (i < 0 || (uint32_t)i >= g_ring64.count) { if (out && out_max > 0) out[0] = 0; return 0; }
    const uint32_t off = con64_rec_off64((uint32_t)i);
    if (tick)  *tick  = con64_rec_tick64(off);
    if (level) *level = con64_rec_level64(off);
    return con64_copy_out64(con64_rec_text64(off), con64_rec_len64(off), out, out_max);
}

int con64_head_line64(int i, uint64_t* tick, int* level, char* out, int out_max) {
    if (i < 0 || (uint32_t)i >= g_ring64.head_n) { if (out && out_max > 0) out[0] = 0; return 0; }
    const Con64HeadLine* h = &g_ring64.head[i];
    if (tick)  *tick  = h->tick;
    if (level) *level = (int)h->level;
    return con64_copy_out64(h->text, h->len, out, out_max);
}

// ==================== dmesg 数据源 ====================
static bool con64_dmesg_has_gap64() { return g_ring64.dropped > 0 && g_ring64.head_n > 0; }

int con64_dmesg_count64() {
    const int ring = (int)g_ring64.count;
    return con64_dmesg_has_gap64() ? (int)g_ring64.head_n + 1 + ring : ring;
}

int con64_dmesg_plain64(int i, char* out, int out_max) {
    if (i < 0) { if (out && out_max > 0) out[0] = 0; return 0; }
    if (con64_dmesg_has_gap64()) {
        const int hn = (int)g_ring64.head_n;
        if (i < hn) return con64_head_line64(i, nullptr, nullptr, out, out_max);
        if (i == hn) {
            // 省略标记行：说明中间这些行已被环形缓冲覆盖（实数 = dropped）
            const char* a = "[CON64] ... ";
            int o = 0;
            while (*a && o < out_max - 1) out[o++] = *a++;
            uint32_t d = g_ring64.dropped;
            char tmp[16]; int n = 0;
            if (d == 0) tmp[n++] = '0';
            while (d > 0) { tmp[n++] = (char)('0' + (int)(d % 10)); d /= 10; }
            while (n > 0 && o < out_max - 1) out[o++] = tmp[--n];
            a = " lines dropped (ring keeps the most recent ones) ...";
            while (*a && o < out_max - 1) out[o++] = *a++;
            out[o] = 0;
            return o;
        }
        return con64_line64(i - hn - 1, nullptr, nullptr, out, out_max);
    }
    return con64_line64(i, nullptr, nullptr, out, out_max);
}

int con64_dmesg_text64(int i, char* out, int out_max) {
    uint64_t tick = 0;
    int level = CON64_LV_INFO;
    char body[CON64_TEXT_MAX + 8];
    const bool gap = con64_dmesg_has_gap64() && i == (int)g_ring64.head_n;
    int blen;
    if (gap) {
        blen = con64_dmesg_plain64(i, body, (int)sizeof body);
    } else if (con64_dmesg_has_gap64() && i < (int)g_ring64.head_n) {
        blen = con64_head_line64(i, &tick, &level, body, (int)sizeof body);
    } else {
        const int ri = con64_dmesg_has_gap64() ? i - (int)g_ring64.head_n - 1 : i;
        blen = con64_line64(ri, &tick, &level, body, (int)sizeof body);
    }
    if (blen <= 0 || !out || out_max < 24) { if (out && out_max > 0) out[0] = 0; return 0; }
    if (gap) {                                  // 标记行不带时间戳
        int o = 0;
        for (int k = 0; k < blen && o < out_max - 1; k++) out[o++] = body[k];
        out[o] = 0;
        return o;
    }
    char st[40];
    const int sl = con64_stamp64(tick, st, (int)sizeof st);
    int o = 0;
    for (int k = 0; k < sl && o < out_max - 1; k++) out[o++] = st[k];
    if (o < out_max - 1) out[o++] = ' ';
    for (int k = 0; k < blen && o < out_max - 1; k++) out[o++] = body[k];
    out[o] = 0;
    return o;
}

void con64_dmesg_dump_serial64(int first_n, int last_n) {
    const int n = con64_dmesg_count64();
    dbg64_line_begin64();
    dbg64_str("[CON64] dmesg lines=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" head=");
    dbg64_dec((uint64_t)con64_head_lines64());
    dbg64_str(" dropped=");
    dbg64_dec((uint64_t)con64_dropped_lines64());
    dbg64_str(" trunc=");
    dbg64_dec((uint64_t)con64_trunc_lines64());
    dbg64_nl();
    dbg64_line_end64();

    const int tail_from = (n - last_n > first_n) ? (n - last_n) : first_n;
    for (int i = 0; i < n; i++) {
        const bool head_part = (i < first_n);
        const bool tail_part = (i >= tail_from);
        if (!head_part && !tail_part) {
            if (i == first_n) {
                dbg64_line_begin64();
                dbg64_str("[CON64] dmesg ... (");
                dbg64_dec((uint64_t)(tail_from - first_n));
                dbg64_str(" lines elided in serial; full list goes to the terminal)");
                dbg64_nl();
                dbg64_line_end64();
            }
            continue;
        }
        char lb[CON64_TEXT_MAX + 64];
        const int len = con64_dmesg_text64(i, lb, (int)sizeof lb);
        if (len <= 0) continue;
        dbg64_line_begin64();
        dbg64_str("[CON64] dmesg[");
        dbg64_dec((uint64_t)i);
        dbg64_str("] ");
        dbg64_str(lb);
        dbg64_nl();
        dbg64_line_end64();
    }
}

// ==================== 屏幕控制台 ====================
static uint32_t con64_level_color64(int lv) {
    return lv == CON64_LV_ERROR ? CON64_C_ERROR : (lv == CON64_LV_WARN ? CON64_C_WARN : CON64_C_INFO);
}

// 画一行正文（UTF-8；ASCII 走等宽面 font_draw_glyph，其它码点走查询链 font_draw_glyph_cp）
// 返回真的画了多少列（供 row_ink 记录"有墨"宽度 —— 滚屏按它裁剪搬运范围）
static int con64_draw_text64(int x, int y, const char* s, uint32_t fg) {
    const int max_x = g_scr64.text_x + g_scr64.cols * g_scr64.adv;
    font_select(FONT_FACE_MONO);
    int cx = x;
    const char* p = s;
    while (*p) {
        int bytes = 0;
        uint32_t cp = font_utf8_decode(p, &bytes);
        if (bytes <= 0) break;
        if (cp == 0) { p += bytes; continue; }
        const int adv = (cp < 0x80) ? font_glyph_advance((char)cp) : font_glyph_advance_cp(cp);
        if (cx + adv > max_x) break;                       // 列数裁剪（正文绝不越出控制台区域）
        if (cp < 0x7F) font_draw_glyph(cx, y, (char)cp, fg);
        else           font_draw_glyph_cp(cx, y, cp, fg);
        cx += adv;
        p += bytes;
    }
    return (cx - x) / (g_scr64.adv > 0 ? g_scr64.adv : 8);
}

// 当前"有墨"宽度（像素）：= 时间戳 + 最宽的一行正文 + 边距；滚屏只搬这一段（不搬整屏宽）
static int con64_scr_ink_w64() {
    int cols = 0;
    const int n = (g_scr64.drawn < g_scr64.rows) ? g_scr64.drawn : g_scr64.rows;
    for (int r = 0; r < n; r++) if (g_scr64.row_ink[r] > cols) cols = g_scr64.row_ink[r];
    if (cols < 1) cols = 1;
    int w = g_scr64.text_x + cols * g_scr64.adv + 8;
    if (w > g_scr64.region_w) w = g_scr64.region_w;
    return w;
}

static void con64_scr_clear_line64(int y, int w) {
    if (w > g_scr64.region_w) w = g_scr64.region_w;
    fb_fill_rect(0, y, w, g_scr64.line_h, CON64_C_BG);
}

// 滚屏：控制台内容整体上移 dy 行（像素行拷贝；底部 dy 行清黑）
// ★ 只搬"有墨"的宽度、只提交控制台区域（fb_flip_region）；不做字形重光栅化（比整屏重画快得多）。
static void con64_scr_scroll64(int dy) {
    const int lh = g_scr64.line_h;
    const int y0 = g_scr64.top_y;
    const int rows = g_scr64.drawn;
    if (dy < 1) dy = 1;
    if (dy >= rows) {                                       // 全滚出去：整块清黑
        for (int r = 0; r < rows; r++) g_scr64.row_ink[r] = 0;
        con64_scr_clear_line64(y0, g_scr64.region_w);
        fb_fill_rect(0, y0, g_scr64.region_w, rows * lh, CON64_C_BG);
        return;
    }
    const int w = con64_scr_ink_w64();
    for (int r = 0; r + dy < rows; r++) {
        const int sy = y0 + (r + dy) * lh;
        const int ty = y0 + r * lh;
        for (int x = 0; x < w; x++) fb_putpixel(x, ty, fb_get_pixel(x, sy));
        g_scr64.row_ink[r] = g_scr64.row_ink[r + dy];
    }
    for (int r = rows - dy; r < rows; r++) {
        con64_scr_clear_line64(y0 + r * lh, w);
        g_scr64.row_ink[r] = 0;
    }
}

// 画一行（含时间戳），并按 batch 批量滚屏：屏满时先攒 batch-1 行，再整体上移 batch 行后一次画上
// （保证屏上不丢行；batch=1 = 逐行滚，实时阶段用）
static void con64_scr_put64(uint64_t tick, int level, const char* text, int batch) {
    if (!g_scr64.active || g_scr64.rows <= 0) return;
    if (batch < 1) batch = 1;
    if (batch > CON64_SCROLL_BATCH) batch = CON64_SCROLL_BATCH;
    bool scrolled = false;
    if (g_scr64.drawn >= g_scr64.rows) {
        if (batch > 1 && g_scr64.pend_n + 1 < batch) {      // 先攒着（未满一批）
            const int k = g_scr64.pend_n;
            g_scr64.pend_tick[k] = tick;
            g_scr64.pend_lv[k] = (uint8_t)level;
            int i = 0;
            while (text[i] && i < CON64_TEXT_MAX + 6) { g_scr64.pend_text[k][i] = text[i]; i++; }
            g_scr64.pend_text[k][i] = 0;
            g_scr64.pend_n++;
            return;
        }
        const int dy = (batch > 1) ? (g_scr64.pend_n + 1) : 1;   // 一次上移这么多行
        con64_scr_scroll64(dy);
        scrolled = true;
        // 把攒下的行按原顺序补画到新腾出的行上
        for (int k = 0; k < g_scr64.pend_n; k++) {
            const int row = g_scr64.rows - dy + k;
            const int y = g_scr64.top_y + row * g_scr64.line_h;
            char st[40];
            if (con64_stamp64(g_scr64.pend_tick[k], st, (int)sizeof st) > 0) {
                font_select(FONT_FACE_MONO);
                font_draw_text(g_scr64.ts_x, y, st, CON64_C_STAMP);
            }
            g_scr64.row_ink[row] =
                (uint8_t)(CON64_STAMP_CHARS + 1 + con64_draw_text64(g_scr64.text_x, y,
                            g_scr64.pend_text[k], con64_level_color64(g_scr64.pend_lv[k])));
        }
        g_scr64.pend_n = 0;
    }
    const int row = (g_scr64.drawn < g_scr64.rows) ? g_scr64.drawn : g_scr64.rows - 1;
    const int y = g_scr64.top_y + row * g_scr64.line_h;
    con64_scr_clear_line64(y, g_scr64.region_w);
    char st[40];
    const int sl = con64_stamp64(tick, st, (int)sizeof st);
    if (sl > 0) {
        font_select(FONT_FACE_MONO);
        font_draw_text(g_scr64.ts_x, y, st, CON64_C_STAMP);
    }
    const int cols = con64_draw_text64(g_scr64.text_x, y, text, con64_level_color64(level));
    g_scr64.row_ink[row] = (uint8_t)(CON64_STAMP_CHARS + 1 + cols);
    if (g_scr64.drawn < g_scr64.rows) g_scr64.drawn++;
    if (scrolled) fb_flip_region(0, g_scr64.top_y, g_scr64.region_w, g_scr64.region_h);
    else          fb_flip_region(0, y, g_scr64.region_w, g_scr64.line_h);
}

static int con64_poll_key64() {                            // 任意键（并吞掉整个队列）
    if (!kbd_has_char()) return 0;
    uint8_t c;
    while (kbd_pop_char(&c)) { }
    return 1;
}

// 实时追加：把"进入实时模式之后新进缓冲的行"画到屏上（滚屏）
static void con64_scr_drain64() {
    if (g_scr64.shown_total > (int)g_ring64.total) g_scr64.shown_total = (int)g_ring64.total;
    while (g_scr64.shown_total < (int)g_ring64.total) {
        const uint32_t back = (uint32_t)g_ring64.total - (uint32_t)g_scr64.shown_total;  // 倒数第几条
        g_scr64.shown_total++;
        if (back > g_ring64.count) continue;               // 这条已被覆盖（丢掉了）
        const int idx = (int)g_ring64.count - (int)back;
        uint64_t tick = 0; int lv = CON64_LV_INFO;
        char lb[CON64_TEXT_MAX + 8];
        if (con64_line64(idx, &tick, &lv, lb, (int)sizeof lb) <= 0) continue;
        con64_scr_put64(tick, lv, lb, 1);              // 实时阶段：逐行滚（延迟最低）
    }
}

int con64_boot_screen64(int verbose) {
    const int face_save = font_current_face();   // 画完恢复现场（不改其它模块对"当前面"的前提）
    g_scr64.active = 0;
    g_scr64.skip = 0;
    g_scr64.drawn = 0;
    g_scr64.cols = 0;
    g_scr64.rows = 0;
    g_scr64.pending = 0;
    g_scr64.pend_n = 0;
    for (int r = 0; r < CON64_ROWS_MAX; r++) g_scr64.row_ink[r] = 0;
    g_scr64.line_h = font_line_height();
    g_scr64.top_y = CON64_MARGIN_Y;
    {
        font_select(FONT_FACE_MONO);
        g_scr64.adv = font_glyph_advance(' ');
        if (g_scr64.adv <= 0) g_scr64.adv = 8;
    }
    const int W = fb_width(), H = fb_height();
    g_scr64.ts_x = CON64_MARGIN_X;
    g_scr64.text_x = CON64_MARGIN_X + CON64_STAMP_CHARS * g_scr64.adv + g_scr64.adv;
    if (W > 0 && g_scr64.line_h > 0) {
        g_scr64.cols = (W - g_scr64.text_x - CON64_MARGIN_X) / g_scr64.adv;
        g_scr64.rows = (H - CON64_MARGIN_Y - 2) / g_scr64.line_h;
    }
    if (g_scr64.cols < 1) g_scr64.cols = 0;
    if (g_scr64.rows < 1) g_scr64.rows = 0;
    g_scr64.region_w = g_scr64.text_x + g_scr64.cols * g_scr64.adv + CON64_MARGIN_X;
    if (g_scr64.region_w > W) g_scr64.region_w = W;
    g_scr64.region_h = g_scr64.rows * g_scr64.line_h + 2;
    if (g_scr64.region_h > H) g_scr64.region_h = H;

    dbg64_line_begin64();
    dbg64_str("[CON64] screen ready cols=");
    dbg64_dec((uint64_t)g_scr64.cols);
    dbg64_str(" rows=");
    dbg64_dec((uint64_t)g_scr64.rows);
    dbg64_str(" line_h=");
    dbg64_dec((uint64_t)g_scr64.line_h);
    dbg64_str(" adv=");
    dbg64_dec((uint64_t)g_scr64.adv);
    dbg64_nl();
    dbg64_line_end64();

    (void)con64_selftest64();                             // [CON64] selftest PASS/FAIL mask=...

    if (!verbose) {
        dbg64_line_begin64();
        dbg64_str("[CON64] verbose=0 skipped lines=");
        dbg64_dec((uint64_t)con64_buffered_lines64());
        dbg64_str(" (boot.verbose=0: boot console off, dmesg still available)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    if (W <= 0 || H <= 0 || g_scr64.line_h <= 0 || g_scr64.rows <= 0 || g_scr64.cols <= 0) {
        dbg64_line_begin64();
        dbg64_str("[CON64] screen skipped (no framebuffer/font)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }

    // ---- 画一屏黑底（Linux 观感：黑底 + 等宽浅色字）----
    // ★ 控制台自己拥有整屏：先清掉可能残留的窗口裁剪矩形 —— fb_putpixel 会尊重裁剪
    //   （gui64/preload64 设置裁剪后又复位；这里保险再复位一次，否则滚屏的像素搬运会被裁剪掉）。
    fb_reset_clip();
    // ★ 整屏一次性提交（否则屏下半部还是上一页的残留像素）；之后只提交控制台区域。
    fb_fill_rect(0, 0, W, H, CON64_C_BG);
    fb_flip_region(0, 0, W, H);

    // ---- 回放缓冲里已有的行：**按节奏逐行滚出**（有界；跑完不停留）----
    //   节奏：总时长目标 CON64_SCROLL_TOTAL_TICKS，按本次要画的**行数**摊到每行
    //   （每行 1..CON64_SCROLL_MAX_PER_LINE tick）：行少 → 每行多等一点（看得清），
    //   行多 → 每行 1 tick（总量仍有上限）。★ 用户要求：滚完**不停留**，直接进系统。
    //   等待用 pause 自旋（不依赖 IF、不需要 hlt），期间继续 drain 与轮询按键。
    g_scr64.active = 1;
    const uint64_t t0 = g_ticks64;
    const int have = con64_buffered_lines64();
    const int first = (have > CON64_REPLAY_MAX) ? (have - CON64_REPLAY_MAX) : 0;
    const int planned = (have > first) ? (have - first) : 0;
    int per_line = (planned > 0) ? (CON64_SCROLL_TOTAL_TICKS / planned) : 1;
    if (per_line < 1) per_line = 1;
    if (per_line > CON64_SCROLL_MAX_PER_LINE) per_line = CON64_SCROLL_MAX_PER_LINE;
    int replayed = 0;
    char lb[CON64_TEXT_MAX + 8];
    for (int i = first; i < have; i++) {
        uint64_t tick = 0; int lv = CON64_LV_INFO;
        if (con64_line64(i, &tick, &lv, lb, (int)sizeof lb) <= 0) continue;
        con64_scr_put64(tick, lv, lb, CON64_SCROLL_BATCH);   // 回放阶段：攒 4 行批量滚（少搬像素）
        replayed++;
        if (con64_poll_key64()) { g_scr64.skip = 1; break; }   // 回放期间按键：立即结束
        // 节奏等待：**按计划时间**等（第 replayed 行的目标时刻 = replayed*per_line），
        //   绘制本身已经比计划慢时就不再额外等待 —— 总时长自动收敛到 max(绘制, 计划)，不会叠加。
        const uint64_t target = (uint64_t)replayed * (uint64_t)per_line;
        while (!g_scr64.skip && (g_ticks64 - t0) < target) {
            // ★ 这里**不要调 con64_scr_drain64()**：drain 会把 pending 里的行**逐行**刷出去，
            //   每行都触发一次整屏拷贝/flip —— 那会让"攒 CON64_SCROLL_BATCH 行再滚"完全失效
            //   （实测：64 行 16 秒 ≈ 每行 250ms）。实时行的刷新交给回放结束后的 drain。
            if (con64_poll_key64()) { g_scr64.skip = 1; break; }
            __asm__ volatile("pause");
        }
        if (g_scr64.skip) break;
    }
    const uint64_t replay_ms = (uint64_t)(g_ticks64 - t0) * (uint64_t)TICK_MS_64;
    dbg64_line_begin64();
    dbg64_str("[CON64] replay lines=");
    dbg64_dec((uint64_t)replayed);
    dbg64_str(" dropped=");
    dbg64_dec((uint64_t)con64_dropped_lines64());
    dbg64_str(" ms=");
    dbg64_dec(replay_ms);
    dbg64_nl();
    dbg64_line_end64();

    // ---- 实时追加模式（停留期间新来的打点继续画）----
    g_scr64.shown_total = (int)g_ring64.total;
    dbg64_line_begin64();
    dbg64_str("[CON64] live lines=");
    dbg64_dec((uint64_t)con64_buffered_lines64());
    dbg64_str(" drawn=");
    dbg64_dec((uint64_t)g_scr64.drawn);
    dbg64_nl();
    dbg64_line_end64();

    // ---- ★ 不停留：日志滚完（或按键跳过）后立即返回，接着进系统 ----
    //   用户要求："跑完后不停留直接进桌面"。此处刻意**没有**停留阶段；
    //   想让屏幕停住看日志的话，进系统后用终端 `dmesg` 看完整日志。
    con64_scr_drain64();
    g_scr64.active = 0;
    if (g_scr64.skip) {
        dbg64_line_begin64();
        dbg64_str("[CON64] skip key=1 (any key ends the boot console)");
        dbg64_nl();
        dbg64_line_end64();
    }
    font_select(face_save);                        // ★ 恢复进入本函数时的字体面
    return g_scr64.skip ? 1 : 0;
}


// ==================== 自检 ====================
int con64_selftest64() {
    int mask = 0;
    // bit0 环形缓冲容量/初始化
    if (!g_ring64.init_done || CON64_RING_BYTES < 8192 || CON64_RING_BYTES > 16384 ||
        CON64_TEXT_MAX < 64 || con64_buffered_lines64() <= 0) mask |= 0x1;
    // bit1 早期行确实被缓存（fb_init 之前的打点 >= 10 行）
    if (con64_head_lines64() < 10) mask |= 0x2;
    // bit2 时间戳格式（Linux：宽度 14、'[' 开头 ']' 结尾、第 7 位是 '.'）
    {
        char st[40];
        const int n0 = con64_stamp64(0, st, (int)sizeof st);
        const int n1 = con64_stamp64(110, st, (int)sizeof st);        // 110 tick = 440ms = [    0.440000]
        if (n0 != CON64_STAMP_CHARS || n1 != CON64_STAMP_CHARS || st[0] != '[' ||
            st[6] != '.' || st[13] != ']') mask |= 0x4;
    }
    // bit3 等级判定（FAIL/PANIC 红、WARN 黄、其余 info）
    if (con64_classify64("selftest FAILED mask=3") != CON64_LV_ERROR) mask |= 0x8;
    if (con64_classify64("[X64] PANIC in isr")    != CON64_LV_ERROR) mask |= 0x8;
    if (con64_classify64("[FS64] WARN carrier")   != CON64_LV_WARN)  mask |= 0x8;
    if (con64_classify64("[LM64] M0 PASS")        != CON64_LV_INFO)  mask |= 0x8;
    // bit4 屏幕几何（有 framebuffer 时才要求）
    if (fb_width() > 0 && (g_scr64.cols < 40 || g_scr64.rows < 10)) mask |= 0x10;

    dbg64_line_begin64();
    if (mask == 0) {
        dbg64_str("[CON64] selftest PASS mask=0x0");
    } else {
        dbg64_str("[CON64] selftest FAIL mask=0x");
        static const char* H = "0123456789abcdef";
        dbg64_putc(H[(mask >> 4) & 0xF]);
        dbg64_putc(H[mask & 0xF]);
    }
    dbg64_nl();
    dbg64_line_end64();
    return mask;
}
