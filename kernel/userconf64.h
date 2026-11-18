// userconf64.h - /etc/user.conf（安装向导写入；登录 / 时钟读取）
//
// 为什么单独一个文件而不是只写 /etc/users.db：
//   安装向导要交付的是"这台机器归谁、在哪个时区"，与用户表的多用户语义不是一回事；
//   而且 user.conf 必须**能表达"密码留空"这件事本身**（pass_set=0）——users.db 里
//   "空 hash" 与"没这个用户"在登录界面上是一回事，读了分不清。两张表各管一件事。
//
// 为什么是 header-only：
//   写它的是**安装程序内核**（kernel/setup64.cpp，-DVIMTU_INSTALLER_MEDIA 那一遍编译），
//   读它的是**系统内核**（locklogin64 / panels64）。安装程序那一遍只链一部分目标文件，
//   把这段做成 static 就不必改任何链接表；哈希实现见 kernel/sha256_64.h（同一份，绝不复制）。
//
// 文件格式（纯文本、行式；# 开头是注释；未知键忽略 -> 老配置不会把新内核弄挂）：
//   user=vimtu                       用户名（默认 vimtu）
//   pass_algo=sha256-iter1000        与 users.db 同一套：SHA-256(salt||pw) 再叠 999 轮
//   pass_set=0|1                     0 = 密码留空 -> 登录直接跳过校验进桌面
//   pass_salt=<32 hex>               16 字节盐
//   pass_hash=<64 hex>               口令哈希（**明文永不落盘、也不进串口**）
//   timezone=Asia/Shanghai           标准时区串（如 Asia/Shanghai）
//   tz_offset=480                    相对 UTC 的分钟数（内核没有 tzdata，直接读偏移）
//   lang=0|1                         安装语言（0 中文 / 1 English），供下拉框文案联动
#ifndef USERCONF64_H
#define USERCONF64_H

#include "sha256_64.h"
#include "vfs64.h"       // vfs64_read_on64 / vfs64_system_slot64
#include "x86_64.h"      // rtc_get_time64 / rtc_get_date64
#include "debug64.h"     // dbg64_*（单行打点）

#define USERCONF64_PATH      "/etc/user.conf"
#define USERCONF64_USER_MAX  15     // 与 USERDB64_NAME_MAX 同口径（1..15 个可打印 ASCII）
#define USERCONF64_TZ_MAX    31     // "America/Los_Angeles" 这类串
#define USERCONF64_FILE_MAX  1024
#define USERCONF64_SALT_HEX  33     // 16B -> 32 hex + NUL
#define USERCONF64_HASH_HEX  65     // 32B -> 64 hex + NUL

static char g_uc_user[USERCONF64_USER_MAX + 1] = "vimtu";
static char g_uc_tz[USERCONF64_TZ_MAX + 1]     = "Asia/Shanghai";
static char g_uc_salt[USERCONF64_SALT_HEX]     = "";
static char g_uc_hash[USERCONF64_HASH_HEX]     = "";
static int  g_uc_off      = 480;   // Asia/Shanghai = UTC+8
static int  g_uc_pass     = 0;     // 0 = 密码留空（默认：装完直接进桌面）
static int  g_uc_lang     = 0;     // 0 中文 / 1 English
static int  g_uc_loaded   = 0;     // 1 = 已经尝试读过（幂等）
static int  g_uc_ok       = -1;    // 上次 load 的结果（0 读到了 / -1 用默认值）

static inline int uc_len64(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static inline void uc_cpy64(char* d, int cap, const char* s) {
    int i = 0;
    for (; s && s[i] && i < cap - 1; i++) d[i] = s[i];
    d[i] = 0;
}
static inline int uc_streq64(const char* a, const char* b) {
    if (!a || !b) return 0;
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == 0 && b[i] == 0;
}
// 用户名合法性：1..15 个可打印 ASCII（不含空格/斜杠/冒号/等号/控制符）
static inline int uc_name_ok64(const char* s) {
    const int n = uc_len64(s);
    if (n < 1 || n > USERCONF64_USER_MAX) return 0;
    for (int i = 0; i < n; i++) {
        const char c = s[i];
        if (c <= 0x20 || c == '/' || c == '\\' || c == ':' || c == '=' || c >= 0x7F) return 0;
    }
    return 1;
}
static inline int uc_atoi64(const char* s) {
    int sign = 1, v = 0, i = 0;
    if (s[i] == '-') { sign = -1; i++; }
    for (; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return v * sign;
}

// 读 /etc/user.conf（幂等；没文件/格式坏 -> 保持默认值并如实打点）。0 = 读到了，-1 = 走默认。
static inline int userdb64_userconf_load64(void) {
    if (g_uc_loaded) return g_uc_ok;
    g_uc_loaded = 1;
    const int slot = vfs64_system_slot64();
    static char buf[USERCONF64_FILE_MAX];
    int n = -1;
    if (slot >= 0) n = vfs64_read_on64(slot, USERCONF64_PATH, buf, (int)sizeof(buf) - 1);
    if (n <= 0) {
        g_uc_ok = -1;
        dbg64_line_begin64();
        dbg64_str("[USERCONF] load path=");
        dbg64_str(USERCONF64_PATH);
        dbg64_str(" src=none (defaults: user=vimtu tz=Asia/Shanghai off=480 pass=0)\n");
        dbg64_line_end64();
        return -1;
    }
    buf[n] = 0;
    int bad = 0;
    for (int i = 0; i < n; ) {
        int j = i;
        while (j < n && buf[j] != '\n') j++;
        int end = j;
        while (end > i && (buf[end - 1] == '\r' || buf[end - 1] == ' ' || buf[end - 1] == '\t')) end--;
        if (end > i && buf[i] != '#') {
            int eq = -1;
            for (int k = i; k < end; k++) if (buf[k] == '=') { eq = k; break; }
            if (eq > i) {
                char key[24];
                char val[USERCONF64_FILE_MAX];
                const int klen = eq - i;
                const int vlen = end - eq - 1;
                if (klen > 0 && klen < (int)sizeof(key) && vlen >= 0 && vlen < (int)sizeof(val)) {
                    for (int k = 0; k < klen; k++) key[k] = buf[i + k];
                    key[klen] = 0;
                    for (int k = 0; k < vlen; k++) val[k] = buf[eq + 1 + k];
                    val[vlen] = 0;
                    if (uc_streq64(key, "user")) {
                        if (uc_name_ok64(val)) uc_cpy64(g_uc_user, (int)sizeof(g_uc_user), val);
                        else bad |= 1;
                    } else if (uc_streq64(key, "pass_set")) {
                        g_uc_pass = uc_atoi64(val) ? 1 : 0;
                    } else if (uc_streq64(key, "pass_salt")) {
                        uc_cpy64(g_uc_salt, (int)sizeof(g_uc_salt), val);
                    } else if (uc_streq64(key, "pass_hash")) {
                        uc_cpy64(g_uc_hash, (int)sizeof(g_uc_hash), val);
                    } else if (uc_streq64(key, "timezone")) {
                        if (val[0] && uc_len64(val) <= USERCONF64_TZ_MAX)
                            uc_cpy64(g_uc_tz, (int)sizeof(g_uc_tz), val);
                    } else if (uc_streq64(key, "tz_offset")) {
                        const int v = uc_atoi64(val);
                        if (v >= -12 * 60 && v <= 14 * 60) g_uc_off = v;
                        else bad |= 2;
                    } else if (uc_streq64(key, "lang")) {
                        g_uc_lang = uc_atoi64(val) ? 1 : 0;
                    }
                    // 未知键（含 pass_algo）忽略：老配置不许把新内核弄挂
                } else {
                    bad |= 4;
                }
            } else {
                bad |= 4;
            }
        }
        i = j + 1;
    }
    // pass_set=1 但 salt/hash 缺失 -> 如实降级成"无口令"（否则登录会永远进不去）
    if (g_uc_pass && (!g_uc_salt[0] || !g_uc_hash[0])) {
        g_uc_pass = 0;
        bad |= 8;
    }
    g_uc_ok = 0;
    dbg64_line_begin64();
    dbg64_str("[USERCONF] load path=");
    dbg64_str(USERCONF64_PATH);
    dbg64_str(" user=");
    dbg64_str(g_uc_user);
    dbg64_str(" tz=");
    dbg64_str(g_uc_tz);
    dbg64_str(" off=");
    if (g_uc_off < 0) dbg64_str("-");
    dbg64_dec((uint64_t)(g_uc_off < 0 ? -g_uc_off : g_uc_off));
    dbg64_str(" pass=");
    dbg64_dec((uint64_t)g_uc_pass);
    dbg64_str(" lang=");
    dbg64_dec((uint64_t)g_uc_lang);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" warn=");
    dbg64_dec((uint64_t)bad);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

static inline const char* userdb64_userconf_user64(void)  { return g_uc_user; }
static inline const char* userdb64_userconf_tz64(void)    { return g_uc_tz; }
static inline int  userdb64_userconf_tz_offset64(void)    { return g_uc_off; }
static inline int  userdb64_userconf_pass_set64(void)     { return g_uc_pass; }
static inline int  userdb64_userconf_lang64(void)         { return g_uc_lang; }

// 校验口令：pass_set=0 -> 1（无需口令）；否则按 salt+hash 重算一遍再定长比较
static inline int userdb64_userconf_check_pw64(const char* pw) {
    if (!g_uc_pass) return 1;
    if (!pw) return 0;
    uint8_t salt[USERDB64_SALT_BYTES];
    int sn = 0;
    if (hex_dec(salt, USERDB64_SALT_BYTES, g_uc_salt, &sn) != 0 || sn != (int)USERDB64_SALT_BYTES) return 0;
    uint8_t want[32], got[32];
    int hn = 0;
    if (hex_dec(want, 32, g_uc_hash, &hn) != 0 || hn != 32) return 0;
    pw_hash64(pw, salt, got);
    int same = 1;
    for (int i = 0; i < 32; i++) { if (want[i] != got[i]) same = 0; want[i] = 0; got[i] = 0; }
    for (int i = 0; i < (int)USERDB64_SALT_BYTES; i++) salt[i] = 0;
    return same ? 1 : 0;
}

// ---- 文本拼接（内核没有 libc，不用 snprintf）----
static inline int uc_put64(char* out, int cap, int at, const char* s) {
    for (int i = 0; s && s[i]; i++) {
        if (at >= cap - 1) return -1;
        out[at++] = s[i];
    }
    out[at] = 0;
    return at;
}
static inline int uc_put_int64(char* out, int cap, int at, int v) {
    char t[8];
    int n = 0;
    const int neg = v < 0;
    unsigned u = (unsigned)(neg ? -v : v);
    if (u == 0) t[n++] = '0';
    while (u && n < (int)sizeof(t)) { t[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    if (neg) { if (at >= cap - 1) return -1; out[at++] = '-'; }
    while (n > 0) {
        if (at >= cap - 1) return -1;
        out[at++] = t[--n];
    }
    out[at] = 0;
    return at;
}

// 生成配置文本（安装向导用）。pw 为空 -> pass_set=0 且不写 salt/hash。
// 返回文本长度（不含结尾 NUL）；<0 = 参数非法/缓冲不够。
static inline int userdb64_userconf_build64(char* out, int cap, const char* user, const char* pw,
                                            const char* tz, int off_min, int lang) {
    if (!out || cap < 64) return -1;
    const char* un = (user && uc_name_ok64(user)) ? user : g_uc_user;
    const char* zn = (tz && tz[0] && uc_len64(tz) <= USERCONF64_TZ_MAX) ? tz : g_uc_tz;
    out[0] = 0;
    int at = 0;
    at = uc_put64(out, cap, at, "# VimtuOS /etc/user.conf - written by the installer wizard\n");
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "# plaintext password is never stored; pass_* is salted sha256 (1000 rounds)\n");
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "user=");
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, un);
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "\npass_algo=sha256-iter1000\npass_set=");
    if (at < 0) return -1;
    const int have_pw = (pw && pw[0]) ? 1 : 0;
    at = uc_put_int64(out, cap, at, have_pw);
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "\n");
    if (at < 0) return -1;
    if (have_pw) {
        uint8_t salt[USERDB64_SALT_BYTES];
        uint8_t h[32];
        char salt_hex[USERCONF64_SALT_HEX];
        char hash_hex[USERCONF64_HASH_HEX];
        salt_new64(salt);
        pw_hash64(pw, salt, h);
        hex_enc(salt_hex, (int)sizeof(salt_hex), salt, USERDB64_SALT_BYTES);
        hex_enc(hash_hex, (int)sizeof(hash_hex), h, 32);
        for (int i = 0; i < 32; i++) h[i] = 0;
        for (int i = 0; i < (int)USERDB64_SALT_BYTES; i++) salt[i] = 0;
        at = uc_put64(out, cap, at, "pass_salt=");
        if (at < 0) return -1;
        at = uc_put64(out, cap, at, salt_hex);
        if (at < 0) return -1;
        at = uc_put64(out, cap, at, "\npass_hash=");
        if (at < 0) return -1;
        at = uc_put64(out, cap, at, hash_hex);
        if (at < 0) return -1;
        at = uc_put64(out, cap, at, "\n");
        if (at < 0) return -1;
        for (int i = 0; i < (int)sizeof(salt_hex); i++) salt_hex[i] = 0;
        for (int i = 0; i < (int)sizeof(hash_hex); i++) hash_hex[i] = 0;
    }
    at = uc_put64(out, cap, at, "timezone=");
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, zn);
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "\ntz_offset=");
    if (at < 0) return -1;
    at = uc_put_int64(out, cap, at, off_min);
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "\nlang=");
    if (at < 0) return -1;
    at = uc_put_int64(out, cap, at, lang ? 1 : 0);
    if (at < 0) return -1;
    at = uc_put64(out, cap, at, "\n");
    if (at < 0) return -1;
    return at;
}

// ---- 本地时间/日期：RTC 是 UTC，加 tz 偏移；跨零点连日期一起进位 ----
static inline int uc_is_leap64(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }
static inline int uc_mdays64(int y, int mo) {
    static const int d[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (mo < 1 || mo > 12) return 31;
    return (mo == 2 && uc_is_leap64(y)) ? 29 : d[mo - 1];
}

static inline void userdb64_local_time64(int* h, int* mi, int* s) {
    if (!g_uc_loaded) (void)userdb64_userconf_load64();
    int hh = 0, mm = 0, ss = 0;
    rtc_get_time64(&hh, &mm, &ss);
    int t = hh * 3600 + mm * 60 + ss + g_uc_off * 60;
    t %= 24 * 3600;
    if (t < 0) t += 24 * 3600;                 // C 的 % 向零取整，负数要补一天
    if (h)  *h  = t / 3600;
    if (mi) *mi = (t / 60) % 60;
    if (s)  *s  = t % 60;
}

static inline void userdb64_local_date64(int* y, int* mo, int* d, int* wd) {
    if (!g_uc_loaded) (void)userdb64_userconf_load64();
    int yy = 0, mm = 0, dd = 0, dw = 0;
    rtc_get_date64(&yy, &mm, &dd, &dw);
    int hh = 0, mi = 0, ss = 0;
    rtc_get_time64(&hh, &mi, &ss);
    const int t = hh * 3600 + mi * 60 + ss + g_uc_off * 60;
    int shift = 0;                             // 偏移把时间推过 24h 或退到 0 之前 -> 日期 ±1 天
    if (t >= 24 * 3600) shift = 1;
    else if (t < 0) shift = -1;
    if (shift) {
        dd += shift;
        dw = (dw + shift + 7) % 7;
        if (dd < 1) {
            mm--;
            if (mm < 1) { mm = 12; yy--; }
            dd = uc_mdays64(yy, mm);
        } else if (dd > uc_mdays64(yy, mm)) {
            dd = 1;
            mm++;
            if (mm > 12) { mm = 1; yy++; }
        }
    }
    if (y)  *y  = yy;
    if (mo) *mo = mm;
    if (d)  *d  = dd;
    if (wd) *wd = dw;
}

#endif // USERCONF64_H
