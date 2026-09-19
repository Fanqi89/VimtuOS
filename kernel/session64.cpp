// session64.cpp - 64 位会话 / 应用内容策略实现
//
// 与 gui64 的分工：
//   gui64 负责"窗口"的生命周期（建/销/聚焦），session64 负责"应用内容"的生命周期
//   （关窗后是否清状态 / 停止时清哪些 / 下次启动恢复哪些）。两边只在三个钩子上打交道：
//     gui64_destroy_window() -> session64_app_closed64(app_id)      关窗（可能待清理）
//     gui64 主循环每帧       -> session64_tick64()                  执行真正的 reset
//     桌面起来之后           -> session64_restore64()               按策略恢复应用
#include "session64.h"
#include "config64.h"
#include "gui64.h"        // APP_ID_* / app_*_open64 / app_*_reset64 / gui64_app_windows
#include "debug64.h"      // dbg64_* + 行锁

// ==================== 应用表（按应用 id 登记，与 32 位的"槽位下标"不同）====================
struct Sess64App {
    int  id;                  // gui64.h 的 APP_ID_*
    const char* name;         // ASCII 短名（日志/报告用）
    void (*open)();           // 打开（恢复会话时调用；可为空 = 只登记）
    void (*reset)();          // 清状态（VOLATILE 关窗/停止时调用；可为空 = 无状态可清）
};
static const Sess64App kApps[] = {
    { APP_ID_CALC,     "calculator",  app_calc_open64,     app_calc_reset64     },
    { APP_ID_MINES,    "minesweeper", app_mines_open64,    app_mines_reset64    },
    { APP_ID_TERM,     "terminal",    app_term_open64,     app_term_reset64     },
    { APP_ID_TMGR,     "taskmgr",     app_tmgr_open64,     app_tmgr_reset64     },
    { APP_ID_SETTINGS, "settings",    app_settings_open64, app_settings_reset64 },
    { APP_ID_MONITOR,  "monitor",     app_monitor_open64,  app_monitor_reset64  },
    { APP_ID_MYPC,     "mypc",        app_mypc_open64,     nullptr              },
    { APP_ID_ABOUT,    "about",       app_about_open64,    nullptr              },
    { APP_ID_RECYCLE,  "recycle",     app_recycle_open64,  nullptr              },
};
#define SESS64_APP_N ((int)(sizeof(kApps) / sizeof(kApps[0])))

static int      g_mode = SESS64_VOLATILE;
static bool     g_inited = false;
static bool     g_pending_close[SESS64_APPS_MAX];   // 待清理（关窗时登记，主循环执行）
static int      g_reset_count = 0;
static const char* g_last_reset = "-";

static const Sess64App* app_of(int id) {
    for (int i = 0; i < SESS64_APP_N; i++) if (kApps[i].id == id) return &kApps[i];
    return nullptr;
}

// 小工具（内核里没有 libc）
static void s_copy(char* d, const char* s, int cap) {
    int i = 0;
    if (!d || cap <= 0) return;
    if (!s) s = "";
    while (s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int s_len(const char* s) { int n = 0; if (!s) return 0; while (s[n]) n++; return n; }

// keep 键："sess.keep.<id>"（store 里是 "cfg.sess.keep.<id>"）
static void keep_key(int id, char* buf, int cap) {
    s_copy(buf, "sess.keep.", cap);
    int n = s_len(buf);
    char rev[12]; int m = 0;
    int v = id;
    if (v == 0) rev[m++] = '0';
    while (v > 0) { rev[m++] = (char)('0' + (v % 10)); v /= 10; }
    while (m > 0 && n < cap - 1) buf[n++] = rev[--m];
    buf[n] = 0;
}

// ==================== 初始化 / 策略 ====================
void session64_init64() {
    if (g_inited) return;
    g_inited = true;
    for (int i = 0; i < SESS64_APPS_MAX; i++) g_pending_close[i] = false;
    g_mode = cfg64_session_mode64() ? SESS64_PERSIST : SESS64_VOLATILE;
    int keep_n = 0;
    for (int i = 0; i < SESS64_APP_N; i++) if (session64_app_keep64(kApps[i].id)) keep_n++;
    dbg64_line_begin64();
    dbg64_str("[SESS64] policy=");
    dbg64_str(session64_mode_name64(g_mode));
    dbg64_str(" apps=");
    dbg64_dec((uint64_t)SESS64_APP_N);
    dbg64_str(" keep=");
    dbg64_dec((uint64_t)keep_n);
    dbg64_str(" restore=");
    {
        char list[64];
        cfg64_session_restore64(list, (int)sizeof(list));
        dbg64_str(list[0] ? list : "(none)");
    }
    dbg64_nl();
    dbg64_line_end64();
}

int session64_mode64() { return g_mode; }

const char* session64_mode_name64(int mode) {
    return mode == SESS64_PERSIST ? "PERSIST" : "VOLATILE";
}

void session64_set_mode64(int mode) {
    g_mode = (mode == SESS64_PERSIST) ? SESS64_PERSIST : SESS64_VOLATILE;
    cfg64_set_session_mode64(g_mode);          // 落 config64（-> store64；写盘由 flush/自动落盘完成）
    dbg64_line_begin64();
    dbg64_str("[SESS64] mode=");
    dbg64_str(session64_mode_name64(g_mode));
    dbg64_str(" (persisted via config64)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 应用表 / keep ====================
int session64_app_count64() { return SESS64_APP_N; }

bool session64_app_ok64(int app_id) { return app_of(app_id) != nullptr; }

const char* session64_app_name64(int app_id) {
    const Sess64App* a = app_of(app_id);
    return a ? a->name : "-";
}

bool session64_app_keep64(int app_id) {
    const Sess64App* a = app_of(app_id);
    if (!a) return true;                        // 未登记的应用：不动它（保守）
    char k[CFG64_KEY_MAX];
    keep_key(app_id, k, (int)sizeof(k));
    return config64_get_bool64(k, 0) ? true : false;
}

void session64_set_app_keep64(int app_id, bool keep) {
    const Sess64App* a = app_of(app_id);
    if (!a) return;
    char k[CFG64_KEY_MAX];
    keep_key(app_id, k, (int)sizeof(k));
    config64_set_bool64(k, keep ? 1 : 0);
    dbg64_line_begin64();
    dbg64_str("[SESS64] keep ");
    dbg64_str(a->name);
    dbg64_str("=");
    dbg64_dec(keep ? 1 : 0);
    dbg64_str(" (persisted via config64)");
    dbg64_nl();
    dbg64_line_end64();
}

// ==================== 生命周期 ====================
void session64_app_opened64(int app_id) {
    // 与 32 位一致：打开时不做内容处理（策略在关闭/停止时执行），只把"待清理"标志清掉，
    // 避免"关掉又立刻打开"时把新实例的状态误清。
    if (!app_of(app_id)) return;
    for (int i = 0; i < SESS64_APP_N; i++) {
        if (kApps[i].id == app_id) { g_pending_close[i] = false; return; }
    }
}

void session64_app_closed64(int app_id) {
    const Sess64App* a = app_of(app_id);
    if (!a) return;
    if (g_mode != SESS64_VOLATILE) return;      // PERSIST：关窗不清
    if (session64_app_keep64(app_id)) return;   // 用户显式保留
    for (int i = 0; i < SESS64_APP_N; i++) {
        if (kApps[i].id == app_id) { g_pending_close[i] = true; return; }
    }
}

// 真正的清理（由 GUI 主循环调用，此时没有任何窗口销毁在调用栈上）
static void reset_one(const Sess64App* a, const char* why) {
    if (a->reset) a->reset();
    g_reset_count++;
    g_last_reset = a->name;
    dbg64_line_begin64();
    dbg64_str("[SESS64] reset app=");
    dbg64_str(a->name);
    dbg64_str(" reason=");
    dbg64_str(why);
    dbg64_nl();
    dbg64_line_end64();
}

void session64_tick64() {
    if (!g_inited) return;
    for (int i = 0; i < SESS64_APP_N; i++) {
        if (!g_pending_close[i]) continue;
        g_pending_close[i] = false;
        // 只有在"确实已经没有该应用的窗口"时才清（双击重开的情况由 app_opened 清标志）
        if (gui64_app_windows(kApps[i].id) > 0) continue;
        reset_one(&kApps[i], "close");
    }
}

int session64_reset_all64(const char* why) {
    if (g_mode != SESS64_VOLATILE) {
        dbg64_line_begin64();
        dbg64_str("[SESS64] reset-all skipped (mode=PERSIST)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    int n = 0;
    for (int i = 0; i < SESS64_APP_N; i++) {
        if (session64_app_keep64(kApps[i].id)) {
            dbg64_line_begin64();
            dbg64_str("[SESS64] keep ");
            dbg64_str(kApps[i].name);
            dbg64_str(" (user)");
            dbg64_nl();
            dbg64_line_end64();
            continue;
        }
        reset_one(&kApps[i], why ? why : "stop");
        n++;
    }
    dbg64_line_begin64();
    dbg64_str("[SESS64] reset-all apps=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" reason=");
    dbg64_str(why ? why : "stop");
    dbg64_nl();
    dbg64_line_end64();
    return n;
}

int session64_reset_count64() { return g_reset_count; }
const char* session64_last_reset_name64() { return g_last_reset; }

// ==================== 会话快照 / 恢复 ====================
int session64_open_count64() {
    int n = 0;
    for (int i = 0; i < SESS64_APP_N; i++) n += gui64_app_windows(kApps[i].id);
    return n;
}

int session64_save64() {
    char list[64];
    int pos = 0, n = 0;
    list[0] = 0;
    for (int i = 0; i < SESS64_APP_N; i++) {
        if (gui64_app_windows(kApps[i].id) <= 0) continue;
        if (pos < (int)sizeof(list) - 6) {
            if (n) list[pos++] = ',';
            int v = kApps[i].id;
            char rev[6]; int m = 0;
            if (v == 0) rev[m++] = '0';
            while (v > 0) { rev[m++] = (char)('0' + (v % 10)); v /= 10; }
            while (m > 0) list[pos++] = rev[--m];
        }
        n++;
    }
    list[pos] = 0;
    cfg64_set_session_restore64(list);          // 写 config64（-> store64）；落盘由 flush/自动落盘完成
    cfg64_set_session_mode64(g_mode);
    dbg64_line_begin64();
    dbg64_str("[SESS64] save open=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" list=");
    dbg64_str(list[0] ? list : "(none)");
    dbg64_str(" mode=");
    dbg64_str(session64_mode_name64(g_mode));
    dbg64_nl();
    dbg64_line_end64();
    return n;
}

int session64_restore64() {
    if (g_mode != SESS64_PERSIST) {
        dbg64_line_begin64();
        dbg64_str("[SESS64] restore skipped (mode=VOLATILE)");
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    char list[64];
    cfg64_session_restore64(list, (int)sizeof(list));
    int opened = 0;
    // 解析 "3,2" 形式的 id 列表
    int i = 0;
    while (list[i]) {
        int v = 0;
        bool any = false;
        while (list[i] >= '0' && list[i] <= '9') { v = v * 10 + (list[i] - '0'); i++; any = true; }
        if (any) {
            const Sess64App* a = app_of(v);
            if (a && a->open) { a->open(); opened++; }
        }
        if (list[i] == ',') i++;
        else if (list[i]) i++;
    }
    dbg64_line_begin64();
    dbg64_str("[SESS64] restore open=");
    dbg64_dec((uint64_t)opened);
    dbg64_str(" list=");
    dbg64_str(list[0] ? list : "(none)");
    dbg64_str(" mode=");
    dbg64_str(session64_mode_name64(g_mode));
    dbg64_nl();
    dbg64_line_end64();
    return opened;
}

// ==================== 报告 ====================
static void r_app(char* out, int& pos, int max, const char* s) {
    if (!s) return;
    for (int i = 0; s[i] && pos < max - 1; i++) out[pos++] = s[i];
}
static void r_int(char* out, int& pos, int max, int v) {
    char t[12]; int n = 0;
    bool neg = v < 0;
    uint32_t u = neg ? (uint32_t)(-v) : (uint32_t)v;
    if (u == 0) t[n++] = '0';
    while (u > 0) { t[n++] = (char)('0' + (u % 10)); u /= 10; }
    if (neg && pos < max - 1) out[pos++] = '-';
    while (n > 0 && pos < max - 1) out[pos++] = t[--n];
}

int session64_report64(char* out, int maxlen) {
    if (!out || maxlen <= 0) return 0;
    int pos = 0;
    r_app(out, pos, maxlen, "session policy: ");
    r_app(out, pos, maxlen, session64_mode_name64(g_mode));
    r_app(out, pos, maxlen, "  (0=volatile 1=persist; persisted via config64/store64)\n");
    for (int i = 0; i < SESS64_APP_N; i++) {
        const Sess64App* a = &kApps[i];
        int open_n = gui64_app_windows(a->id);
        r_app(out, pos, maxlen, "  app ");
        r_int(out, pos, maxlen, a->id);
        r_app(out, pos, maxlen, " ");
        r_app(out, pos, maxlen, a->name);
        r_app(out, pos, maxlen, session64_app_keep64(a->id) ? "  keep=on" : "  keep=off");
        r_app(out, pos, maxlen, "  open=");
        r_int(out, pos, maxlen, open_n);
        r_app(out, pos, maxlen, (a->reset ? "  reset=yes\n" : "  reset=-  (no state)\n"));
    }
    r_app(out, pos, maxlen, "  reset events: ");
    r_int(out, pos, maxlen, g_reset_count);
    r_app(out, pos, maxlen, "  last=");
    r_app(out, pos, maxlen, g_last_reset);
    r_app(out, pos, maxlen, "\n");
    out[pos] = 0;
    return pos;
}
