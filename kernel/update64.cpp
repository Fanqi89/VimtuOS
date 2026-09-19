// update64.cpp - 64 位 update 子系统实现（标记文件 -> 应用 -> store/ring log/重启）
//
// 机制（照 32 位、按 64 位现状落地；边界见 update64.h）：
//   1) 标记文件 /update.pending（VimtuFS2 单层路径；内容 "ver=<x.y.z>\n" 的 ASCII 文本）；
//   2) 应用动作 = 校验内容 -> store64 写 "update.applied=<ver>" 并 flush（真落盘）
//        -> ring log 一条 -> 写 /update.done -> 删除 /update.pending（VimtuFS2 没有 rename）
//        -> 打点并软重启（sysstate64_soft_restart64，不返回）；
//   3) 启动检查在 os_boot_path 里（gui64_run 之前）跑一次：有 pending 就在进桌面前完成闭环。
// 幂等：apply 之后 pending 已删除，第二次启动只会看到 /update.done（不再触发重启）。
#include "update64.h"
#include "vfs64.h"       // 单层路径读写/删除
#include "store64.h"     // update.applied 持久化
#include "sysstate64.h"  // ring log + 软重启 + 模块注册
#include "debug64.h"     // 串口打点

static bool g_up_init = false;

// pending 文件最大读取长度（标记文件是小文本；超过就当非法/截断处理，绝不爆缓冲）
static const int UP64_TEXT_MAX = 96;

// ==================== 内容校验 ====================
// 版本串：数字与 '.'，1..16 字符，至少一个数字，首字符是数字（不接受前导/尾随空白）
static bool up64_ver_ok(const char* v) {
    if (!v || !v[0]) return false;
    int n = 0, digits = 0;
    if (v[0] < '0' || v[0] > '9') return false;
    for (int i = 0; v[i]; i++) {
        const char c = v[i];
        if (c >= '0' && c <= '9') { digits++; n++; continue; }
        if (c == '.') { n++; continue; }
        return false;
    }
    return digits > 0 && n >= 1 && n <= 16;
}

// 从 pending 文本里取 "ver=<x>" 的版本串；返回 1 = 取到且合法
static int up64_parse_ver(const char* text, char* out, int max) {
    if (!text || !out || max <= 1) return 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] != 'v' || text[i + 1] != 'e' || text[i + 2] != 'r' || text[i + 3] != '=') continue;
        int n = 0;
        int j = i + 4;
        while (text[j] && text[j] != '\r' && text[j] != '\n' && text[j] != ' ' && text[j] != '\t') {
            if (n < max - 1) out[n++] = text[j];
            j++;
        }
        out[n] = 0;
        return up64_ver_ok(out) ? 1 : 0;
    }
    return 0;
}

// ==================== 应用动作 ====================
// 返回 1 = 应用成功（pending -> done，store 已写并 flush）；0 = 没有/非法。不重启。
int update64_apply64() {
    char text[UP64_TEXT_MAX];
    const int n = vfs64_read(UPDATE64_PENDING_PATH, text, (int)sizeof(text) - 1);
    if (n <= 0) return 0;
    text[n] = 0;

    char ver[24];
    if (!up64_parse_ver(text, ver, (int)sizeof(ver))) {
        dbg64_line_begin64();
        dbg64_str("[UPDATE64] pending invalid ver=? (kept ");
        dbg64_str(UPDATE64_PENDING_PATH);
        dbg64_str(")\n");
        dbg64_line_end64();
        return 0;
    }

    // 1) store64：写已应用版本 + flush（真落盘到 VFS 的 /store.a|b）
    const int src = store64_set64(UPDATE64_STORE_KEY, ver);
    const int frc = store64_flush64();

    // 2) /update.done（VimtuFS2 没有 rename：写新文件说明，再删 pending）
    char done[128];
    int dn = 0;
    const char* p1 = "applied ver=";
    const char* p2 = " from=";
    const char* p3 = "\n";
    for (int i = 0; p1[i] && dn < (int)sizeof(done) - 1; i++) done[dn++] = p1[i];
    for (int i = 0; ver[i] && dn < (int)sizeof(done) - 1; i++) done[dn++] = ver[i];
    for (int i = 0; p2[i] && dn < (int)sizeof(done) - 1; i++) done[dn++] = p2[i];
    for (int i = 0; UPDATE64_VERSION[i] && dn < (int)sizeof(done) - 1; i++) done[dn++] = UPDATE64_VERSION[i];
    for (int i = 0; p3[i] && dn < (int)sizeof(done) - 1; i++) done[dn++] = p3[i];
    const int drc = vfs64_write(UPDATE64_DONE_PATH, done, dn);
    const int urc = vfs64_unlink(UPDATE64_PENDING_PATH);

    // 3) ring log（sysstate64 的 64 条循环日志也在终端 syslog 里可见）
    sys64_logf64("[UPDATE64] applied ver=%s (pending -> store + /update.done)", ver);

    // 4) 打点（自动验收 grep）
    dbg64_line_begin64();
    dbg64_str("[UPDATE64] applied ver=");
    dbg64_str(ver);
    dbg64_str(" from=");
    dbg64_str(UPDATE64_VERSION);
    dbg64_str(" store=");
    dbg64_dec((uint64_t)(src < 0 ? -src : src));
    dbg64_str(" flush=");
    dbg64_dec((uint64_t)(frc < 0 ? -frc : frc));
    dbg64_str(" done_rc=");
    dbg64_dec((uint64_t)(drc < 0 ? -drc : drc));
    dbg64_str(" unlink_rc=");
    dbg64_dec((uint64_t)(urc < 0 ? -urc : urc));
    dbg64_str(" path=");
    dbg64_str(UPDATE64_DONE_PATH);
    dbg64_str(" gen=");
    dbg64_dec(store64_generation64());
    dbg64_nl();
    dbg64_line_end64();
    return 1;
}

// ==================== 启动检查 ====================
int update64_check64() {
    char text[UP64_TEXT_MAX];
    if (!update64_pending_exists64(text, (int)sizeof(text))) {
        dbg64_line_begin64();
        dbg64_str("[UPDATE64] no pending\n");
        dbg64_line_end64();
        return 0;
    }
    char ver[24];
    if (!up64_parse_ver(text, ver, (int)sizeof(ver))) {
        dbg64_line_begin64();
        dbg64_str("[UPDATE64] pending invalid ver=? (kept ");
        dbg64_str(UPDATE64_PENDING_PATH);
        dbg64_str(")\n");
        dbg64_line_end64();
        return 0;
    }
    dbg64_line_begin64();
    dbg64_str("[UPDATE64] apply pending ver=");
    dbg64_str(ver);
    dbg64_str(" at boot\n");
    dbg64_line_end64();

    if (update64_apply64() != 1) return 0;

    dbg64_line_begin64();
    dbg64_str("[UPDATE64] restarting to finish update\n");
    dbg64_line_end64();
    sysstate64_soft_restart64();     // 不返回（优雅停止 + 落盘 -> 硬复位链）
    for (;;) __asm__ volatile("cli; hlt");
}

int update64_pending_exists64(char* out, int max) {
    if (!out || max <= 1) return 0;
    const int n = vfs64_read(UPDATE64_PENDING_PATH, out, max - 1);
    if (n <= 0) { out[0] = 0; return 0; }
    out[n] = 0;
    return 1;
}

int update64_write_pending64(const char* ver) {
    if (!up64_ver_ok(ver)) return -1;
    char buf[64];
    int n = 0;
    const char* p = "ver=";
    for (int i = 0; p[i] && n < (int)sizeof(buf) - 1; i++) buf[n++] = p[i];
    for (int i = 0; ver[i] && n < (int)sizeof(buf) - 1; i++) buf[n++] = ver[i];
    if (n < (int)sizeof(buf) - 1) buf[n++] = '\n';
    const int rc = vfs64_write(UPDATE64_PENDING_PATH, buf, n);
    dbg64_line_begin64();
    dbg64_str("[UPDATE64] stage pending ver=");
    dbg64_str(ver);
    dbg64_str(" path=");
    dbg64_str(UPDATE64_PENDING_PATH);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(rc < 0 ? -rc : rc));
    dbg64_nl();
    dbg64_line_end64();
    return rc < 0 ? -1 : 0;
}

const char* update64_version64()      { return UPDATE64_VERSION; }
const char* update64_pending_path64() { return UPDATE64_PENDING_PATH; }
const char* update64_done_path64()    { return UPDATE64_DONE_PATH; }

int update64_applied64(char* out, int max) {
    return store64_get64(UPDATE64_STORE_KEY, out, max);
}

// ==================== sysstate64 模块挂接（只注册；init = 只读复核） ====================
static int up64_mod_init()   { return 0; }                                  // 复核：模块已编入
static int up64_mod_stop(uint32_t timeout) { (void)timeout; return 0; }
static int up64_mod_health() { return SYS64_MODH_UP; }

void update64_init64() {
    if (g_up_init) return;
    g_up_init = true;

    // ★ 先把所有会写日志的查询做完（store64_get / vfs64_read 在异常路径会自己打行），
    //   再进行锁拼这一条 init 行 —— 否则 VFS 未挂载时 "[VFS64] read: not mounted" 会插进本行中间。
    char applied[32];
    const int n = update64_applied64(applied, (int)sizeof(applied));
    char pend[UP64_TEXT_MAX];
    const int has_pend = update64_pending_exists64(pend, (int)sizeof(pend));

    dbg64_line_begin64();
    dbg64_str("[UPDATE64] init version=");
    dbg64_str(UPDATE64_VERSION);
    dbg64_str(" applied=");
    if (n < 0) dbg64_str("(none)");
    else       dbg64_str(applied);
    dbg64_str(" carrier=");
    dbg64_str(store64_carrier64());
    dbg64_str(" pending=");
    dbg64_str(has_pend ? "yes" : "no");
    dbg64_nl();
    dbg64_line_end64();

    static const Sys64Module kUpdateMod = {
        "update64", up64_mod_init, up64_mod_stop, up64_mod_health, 0, {0, 0, 0}, 1000u
    };
    (void)sysstate64_register64(&kUpdateMod);
}

// ==================== 终端 update status 的一行式报告 ====================
static void up64_app_s(char* out, int* n, int maxlen, const char* s) {
    while (s && *s && *n < maxlen - 1) out[(*n)++] = *s++;
}

void update64_report64(char* out, int maxlen) {
    if (!out || maxlen <= 0) return;
    int n = 0;
    up64_app_s(out, &n, maxlen, "version=");
    up64_app_s(out, &n, maxlen, UPDATE64_VERSION);

    char applied[32];
    const int an = update64_applied64(applied, (int)sizeof(applied));
    up64_app_s(out, &n, maxlen, " applied=");
    up64_app_s(out, &n, maxlen, (an < 0) ? "(none)" : applied);

    up64_app_s(out, &n, maxlen, " pending=");
    char pend[UP64_TEXT_MAX];
    if (update64_pending_exists64(pend, (int)sizeof(pend))) {
        char ver[24];
        if (up64_parse_ver(pend, ver, (int)sizeof(ver))) {
            up64_app_s(out, &n, maxlen, "ver=");
            up64_app_s(out, &n, maxlen, ver);
            up64_app_s(out, &n, maxlen, " (valid)");
        } else {
            up64_app_s(out, &n, maxlen, "invalid");
        }
    } else {
        up64_app_s(out, &n, maxlen, "none");
    }

    uint32_t t = 0, sz = 0;
    up64_app_s(out, &n, maxlen, " done=");
    up64_app_s(out, &n, maxlen, (vfs64_stat(UPDATE64_DONE_PATH, &t, &sz) == 0) ? "yes" : "no");
    up64_app_s(out, &n, maxlen, " carrier=");
    up64_app_s(out, &n, maxlen, store64_carrier64());
    up64_app_s(out, &n, maxlen, " store_gen=");
    {
        char nb[24];
        int m = 0;
        uint64_t g = store64_generation64();
        if (g == 0) nb[m++] = '0';
        while (g > 0) { nb[m++] = (char)('0' + (int)(g % 10)); g /= 10; }
        while (m > 0 && n < maxlen - 1) out[n++] = nb[--m];
    }
    out[n] = 0;
}
