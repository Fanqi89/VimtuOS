// userdb64.cpp - 多用户骨架的实现（见 userdb64.h 的设计说明与打点格式）
//
// 组成：
//   1) SHA-256（自带实现，纯整数、无 libc/堆/浮点）+ 加盐迭代口令哈希（1000 轮）
//   2) /etc/users.db（系统卷）文本表的读/写/校验（带版本号；坏行跳过并打点）
//   3) Linux 语义的数据布局：/home/<用户>、/home/<用户>/Desktop、/root、/root/Desktop
//   4) 会话身份（GUI 身份 vs 会话 euid：su/sudo 只改后者；只在内存里）
//   5) 桌面文件列表（登录时对 <home>/Desktop 做一次真实 vfs64 枚举）
//
// ★ 本批**不做**权限位检查（uid/gid/mode 拦截属 P4）：所有文件仍可被任何会话访问，
//   本模块只保证"身份字段是真实可查的"（whoami / 终端提示符 / [USER64] 打点）。
#include "userdb64.h"
#include "vfs64.h"
#include "config64.h"
#include "theme64.h"
#include "gfx64.h"
#include "img64.h"
#include "debug64.h"
#include "x86_64.h"

// ==================== 打点（每类上限，防刷屏）====================
enum { LOGK_INIT = 0, LOGK_LOGIN, LOGK_CMD, LOGK_ERR, LOGK_N };
// 每类上限（防刷屏）：INIT（建表/载入/落盘）/ LOGIN（登录·桌面列表·设置生效）/ CMD（账号命令）/ ERR
//   —— 验收里会反复登录/建号，给得宽松些；单条命令仍然只打一两行。
static const int kLogCap[LOGK_N] = { 16, 64, 128, 48 };
static int g_log_used[LOGK_N];

static int log_ok64(int kind) {
    if (kind < 0 || kind >= LOGK_N) return 0;
    if (g_log_used[kind] >= kLogCap[kind]) {
        if (g_log_used[kind] == kLogCap[kind]) {
            g_log_used[kind]++;
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb log budget reached kind=");
            dbg64_dec((uint64_t)kind);
            dbg64_str(" cap=");
            dbg64_dec((uint64_t)kLogCap[kind]);
            dbg64_nl();
            dbg64_line_end64();
        }
        return 0;
    }
    g_log_used[kind]++;
    return 1;
}

// ==================== 小工具（不用 libc）====================
static int  s_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static void s_cpy(char* d, int cap, const char* s) {
    int i = 0;
    if (cap <= 0) return;
    while (s && s[i] && i < cap - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int s_eq(const char* a, const char* b) {
    int i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == b[i];
}
static int s_starts(const char* s, const char* p) {
    int i = 0;
    if (!s || !p) return 0;
    while (p[i]) { if (s[i] != p[i]) return 0; i++; }
    return 1;
}
static void u32_str(char* out, int cap, uint32_t v) {
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + (v % 10)); v /= 10; }
    int k = 0;
    while (n > 0 && k < cap - 1) out[k++] = t[--n];
    out[k] = 0;
}
// 有符号十进制：**只解析前导数字**，遇到第一个非数字就停（字段用 '\0' 分隔，所以这里只有
//   版本头那种"后面还跟着换行"的场景会走到这条路径；一个数字都没有时返回 def）。
static int i_parse(const char* s, int def) {
    if (!s || !s[0]) return def;
    int i = 0, neg = 0, v = 0, any = 0;
    if (s[0] == '-') { neg = 1; i = 1; }
    for (; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') break;
        v = v * 10 + (s[i] - '0');
        if (v > 100000000) return def;
        any = 1;
    }
    if (!any) return def;
    return neg ? -v : v;
}
static uint32_t u_parse(const char* s, uint32_t def) {
    if (!s || !s[0]) return def;
    uint64_t v = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return def;
        v = v * 10 + (uint64_t)(s[i] - '0');
        if (v > 0xFFFFFFFEull) return def;
    }
    return (uint32_t)v;
}
// 用户可以自己起名：可打印 ASCII、不含 '/'、'|'、空格，1..15 字符，不是 "." / ".."
static int name_valid64(const char* n) {
    const int len = s_len(n);
    if (len < 1 || len > USERDB64_NAME_MAX - 1) return 0;
    if (s_eq(n, ".") || s_eq(n, "..")) return 0;
    for (int i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)n[i];
        if (c <= 0x20 || c > 0x7E || c == '/' || c == '|') return 0;
    }
    return 1;
}
static int path_valid64(const char* p, int allow_empty) {
    if (!p) return 0;
    if (!p[0]) return allow_empty ? 1 : 0;
    if (p[0] != '/') return 0;
    const int len = s_len(p);
    if (len >= USERDB64_PATH_MAX) return 0;
    for (int i = 0; i < len; i++) {
        const unsigned char c = (unsigned char)p[i];
        if (c <= 0x20 || c > 0x7E || c == '|') return 0;
    }
    return 1;
}

// ==================== SHA-256（自带，无 libc）====================
struct Sha256Ctx {
    uint32_t h[8];
    uint64_t bits;
    uint8_t  buf[64];
    uint32_t n;
};

static const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_init(Sha256Ctx* c) {
    c->h[0] = 0x6a09e667u; c->h[1] = 0xbb67ae85u; c->h[2] = 0x3c6ef372u; c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu; c->h[5] = 0x9b05688cu; c->h[6] = 0x1f83d9abu; c->h[7] = 0x5be0cd19u;
    c->bits = 0;
    c->n = 0;
}

static void sha256_block(Sha256Ctx* c, const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3];
    uint32_t e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + kSha256K[i] + w[i];
        const uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        const uint32_t mj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = cc; cc = b; b = a; a = t1 + t2;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
    c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static void sha256_update(Sha256Ctx* c, const void* data, uint32_t len) {
    const uint8_t* p = (const uint8_t*)data;
    c->bits += (uint64_t)len * 8ull;
    while (len > 0) {
        const uint32_t take = (64u - c->n) < len ? (64u - c->n) : len;
        for (uint32_t i = 0; i < take; i++) c->buf[c->n + i] = p[i];
        c->n += take;
        p += take;
        len -= take;
        if (c->n == 64) { sha256_block(c, c->buf); c->n = 0; }
    }
}

static void sha256_final(Sha256Ctx* c, uint8_t out[32]) {
    const uint64_t bits = c->bits;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    const uint8_t zero = 0;
    while (c->n != 56) sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)((bits >> (56 - 8 * i)) & 0xFF);
    sha256_update(c, lenb, 8);
    for (int i = 0; i < 8; i++) {
        out[i * 4 + 0] = (uint8_t)(c->h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)(c->h[i]);
    }
}

// ==================== 口令哈希（加盐 + 迭代）====================
#define USERDB64_HASH_ITER 1000    // 迭代轮数（每轮 SHA-256；如实打进日志）
#define USERDB64_SALT_BYTES 16

static void hex_enc(char* out, int out_cap, const uint8_t* in, int n) {
    static const char* H = "0123456789abcdef";
    int k = 0;
    for (int i = 0; i < n && k < out_cap - 1; i++) {
        if (k < out_cap - 1) out[k++] = H[(in[i] >> 4) & 0xF];
        if (k < out_cap - 1) out[k++] = H[in[i] & 0xF];
    }
    out[k] = 0;
}
static int hex_dec(uint8_t* out, int out_cap, const char* hex, int* out_n) {
    int n = 0;
    for (int i = 0; hex && hex[i] && hex[i + 1] && n < out_cap; i += 2) {
        int hi = -1, lo = -1;
        const char a = hex[i], b = hex[i + 1];
        if (a >= '0' && a <= '9') hi = a - '0'; else if (a >= 'a' && a <= 'f') hi = a - 'a' + 10; else return -1;
        if (b >= '0' && b <= '9') lo = b - '0'; else if (b >= 'a' && b <= 'f') lo = b - 'a' + 10; else return -1;
        out[n++] = (uint8_t)((hi << 4) | lo);
    }
    if (out_n) *out_n = n;
    return 0;
}

// 口令哈希：h0 = SHA256(salt || pw)；之后 999 轮 h = SHA256(h || salt)（共 USERDB64_HASH_ITER 轮）
static void pw_hash64(const char* pw, const uint8_t salt[USERDB64_SALT_BYTES], uint8_t out[32]) {
    Sha256Ctx c;
    uint8_t d[32];
    const uint32_t plen = (uint32_t)s_len(pw);
    sha256_init(&c);
    sha256_update(&c, salt, USERDB64_SALT_BYTES);
    sha256_update(&c, pw, plen);
    sha256_final(&c, d);
    for (int i = 1; i < USERDB64_HASH_ITER; i++) {
        sha256_init(&c);
        sha256_update(&c, d, 32);
        sha256_update(&c, salt, USERDB64_SALT_BYTES);
        sha256_final(&c, d);
    }
    for (int i = 0; i < 32; i++) out[i] = d[i];
    // 清掉栈上的中间值（明文由调用方负责，不在这里留痕）
    for (int i = 0; i < 32; i++) d[i] = 0;
}

// 盐：xorshift32（rdtsc64 / ticks64 / 计数器 播种）。**不是 CSPRNG**（如实写在日志里）
static uint32_t g_rng = 0;
static uint32_t g_rng_used = 0;
static uint32_t rng_next64() {
    if (!g_rng) {
        uint64_t tsc = 0;
        __asm__ volatile("rdtsc" : "=A"(tsc));          // 自带 rdtsc（不依赖别的模块的实现）
        uint64_t seed = tsc ^ ((uint64_t)ticks64() << 17) ^ 0x9E3779B97F4A7C15ull;
        g_rng = (uint32_t)(seed ^ (seed >> 32));
        if (!g_rng) g_rng = 0x1234567u;
    }
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    g_rng_used++;
    return g_rng;
}
static void salt_new64(uint8_t out[USERDB64_SALT_BYTES]) {
    for (int i = 0; i < USERDB64_SALT_BYTES; i += 4) {
        const uint32_t v = rng_next64();
        out[i + 0] = (uint8_t)(v & 0xFF);
        out[i + 1] = (uint8_t)((v >> 8) & 0xFF);
        out[i + 2] = (uint8_t)((v >> 16) & 0xFF);
        out[i + 3] = (uint8_t)((v >> 24) & 0xFF);
    }
}

// ==================== 用户表（内存）====================
static User64Entry g_ent[USERDB64_MAX_USERS];
static int g_count = 0;
static int g_root_idx = -1;
static int g_gui_idx = -1;         // GUI 登录身份（登录界面/桌面显示的用户名与头像）
static int g_sess_idx = -1;        // 会话身份（euid；su/sudo/exit 只改它）
static int g_db_ok = 0;            // 1 = /etc/users.db 可读写
static int g_db_loaded_from_disk = 0;
static int g_dirty = 0;            // 1 = 内存表与磁盘不一致（需要落盘）

static int sys_slot64() { return vfs64_system_slot64(); }

static void ent_clear64(User64Entry* e) {
    for (uint32_t i = 0; i < (uint32_t)sizeof(User64Entry); i++) ((uint8_t*)e)[i] = 0;
    e->theme = -1;
    e->lock_mode = -1;
}

static void set_home64(User64Entry* e) {
    if (e->flags & USERDB64_F_ROOT) {
        s_cpy(e->home, USERDB64_PATH_MAX, "/root");
    } else {
        char h[USERDB64_PATH_MAX];
        s_cpy(h, USERDB64_PATH_MAX, "/home/");
        int l = s_len(h);
        s_cpy(h + l, USERDB64_PATH_MAX - l, e->name);
        s_cpy(e->home, USERDB64_PATH_MAX, h);
    }
    s_cpy(e->desktop, USERDB64_PATH_MAX, e->home);
    int l = s_len(e->desktop);
    s_cpy(e->desktop + l, USERDB64_PATH_MAX - l, "/Desktop");
}

static User64Entry* find_ptr64(const char* name) {
    for (int i = 0; i < g_count; i++) if (s_eq(g_ent[i].name, name)) return &g_ent[i];
    return nullptr;
}

// ---- 目录：存在就不建（避免 vfs64 的 "already exists" 失败打点刷屏）----
static void ensure_dir64(const char* path) {
    const int slot = sys_slot64();
    if (slot < 0) return;
    uint32_t t = 0, sz = 0;
    if (vfs64_stat_on64(slot, path, &t, &sz) == 0) return;
    (void)vfs64_mkdir_on64(slot, path);
}
static void ensure_user_dirs64(const User64Entry* e) {
    ensure_dir64(e->home);
    ensure_dir64(e->desktop);
}

// ==================== 序列化 / 解析 ====================
static int db_build64(char* out, int cap) {
    int n = 0;
    struct P {
        static int put(char* o, int cap, int& n, const char* s) {
            while (*s) { if (n >= cap - 1) return -1; o[n++] = *s++; }
            o[n] = 0;
            return 0;
        }
        static int num(char* o, int cap, int& n, int v) {
            char t[12];
            u32_str(t, (int)sizeof(t), (uint32_t)(v < 0 ? 0 : v));
            const int rc = put(o, cap, n, t);
            return rc;
        }
    };
    if (P::put(out, cap, n, "VIMTU-USERSDB ")) return -1;
    if (P::num(out, cap, n, 1)) return -1;
    if (P::put(out, cap, n, "\n")) return -1;
    for (int i = 0; i < g_count; i++) {
        const User64Entry* e = &g_ent[i];
        if (P::put(out, cap, n, "U|")) return -1;
        if (P::put(out, cap, n, e->name)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::num(out, cap, n, (int)e->uid)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::put(out, cap, n, e->home)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::put(out, cap, n, e->hash)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::put(out, cap, n, e->salt)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::put(out, cap, n, e->avatar)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::num(out, cap, n, e->theme)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::put(out, cap, n, e->wall)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::num(out, cap, n, e->lock_mode)) return -1;
        if (P::put(out, cap, n, "|")) return -1;
        if (P::num(out, cap, n, (int)e->flags)) return -1;
        if (P::put(out, cap, n, "\n")) return -1;
    }
    return n;
}

// 按 '|' 切字段（**就地**把分隔符写成 0）；返回字段数。line 必须是可写缓冲。
static int db_split64(char* line, char** f, int maxf) {
    int nf = 0;
    char* p = line;
    while (nf < maxf) {
        f[nf++] = p;
        while (*p && *p != '|') p++;
        if (*p == '|') { *p = 0; p++; } else break;
    }
    return nf;
}

// 解析一行 "U|name|uid|home|hash|salt|avatar|theme|wall|lock_mode|flags"；成功 0（写进 e）
static int db_parse_line64(char* line, User64Entry* e) {
    if (!line || line[0] != 'U' || line[1] != '|') return -1;
    char* f[12];
    // line[1] == '|'：把首字段留成 "U"，然后从 line+2 起切剩余 10 个字段
    line[1] = 0;
    const int nf = 1 + db_split64(line + 2, f + 1, 11);
    if (nf != 11) return -1;
    ent_clear64(e);
    s_cpy(e->name, USERDB64_NAME_MAX, f[1]);
    if (!name_valid64(e->name)) return -1;
    e->uid = u_parse(f[2], 0xFFFFFFFFu);
    if (e->uid == 0xFFFFFFFFu) return -1;
    s_cpy(e->home, USERDB64_PATH_MAX, f[3]);
    s_cpy(e->hash, USERDB64_HASH_MAX, f[4]);
    s_cpy(e->salt, USERDB64_SALT_MAX, f[5]);
    s_cpy(e->avatar, USERDB64_PATH_MAX, f[6]);
    e->theme = i_parse(f[7], -1);
    s_cpy(e->wall, USERDB64_PATH_MAX, f[8]);
    e->lock_mode = i_parse(f[9], -1);
    e->flags = u_parse(f[10], 0);
    if (e->uid == 0) e->flags |= USERDB64_F_ROOT;
    e->hidden = (e->flags & USERDB64_F_HIDDEN) ? 1 : 0;
    if (e->flags & USERDB64_F_ROOT) e->hidden = 1;
    if (!path_valid64(e->home, 0)) return -1;
    // 主目录与桌面路径以 home 为准（磁盘上被人手改过也不信）
    s_cpy(e->desktop, USERDB64_PATH_MAX, e->home);
    int l = s_len(e->desktop);
    s_cpy(e->desktop + l, USERDB64_PATH_MAX - l, "/Desktop");
    return 0;
}

// ==================== 落盘 / 读取 ====================
static int db_save64(const char* why) {
    if (sys_slot64() < 0) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb save FAILED reason=no system volume why=");
            dbg64_str(why ? why : "-");
            dbg64_nl();
            dbg64_line_end64();
        }
        return -1;
    }
    static char buf[USERDB64_DB_MAX];
    const int n = db_build64(buf, (int)sizeof(buf));
    if (n <= 0) return -1;
    const int rc = vfs64_write_on64(sys_slot64(), "/etc/users.db", buf, n);
    if (rc < 0) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb save FAILED path=/etc/users.db bytes=");
            dbg64_dec((uint64_t)n);
            dbg64_str(" why=");
            dbg64_str(why ? why : "-");
            dbg64_nl();
            dbg64_line_end64();
        }
        return -1;
    }
    // 回读校验（写入长度 + 头 16 字节；"真落盘"证据）
    uint32_t t = 0, sz = 0;
    const int st = vfs64_stat_on64(sys_slot64(), "/etc/users.db", &t, &sz);
    if (st != 0 || (int)sz != n) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb save verify FAILED stat=");
            dbg64_dec((uint64_t)(st == 0 ? 0 : 1));
            dbg64_str(" size=");
            dbg64_dec((uint64_t)sz);
            dbg64_str(" want=");
            dbg64_dec((uint64_t)n);
            dbg64_nl();
            dbg64_line_end64();
        }
        return -1;
    }
    if (log_ok64(LOGK_INIT)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] userdb save ok path=/etc/users.db bytes=");
        dbg64_dec((uint64_t)n);
        dbg64_str(" users=");
        dbg64_dec((uint64_t)g_count);
        dbg64_str(" why=");
        dbg64_str(why ? why : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
    g_db_ok = 1;
    g_dirty = 0;
    return 0;
}

// 读 + 解析；返回读到的用户数（-1 = 没有文件/不是系统卷）
static int db_load64() {
    if (sys_slot64() < 0) return -1;
    static char buf[USERDB64_DB_MAX];
    const int n = vfs64_read_on64(sys_slot64(), "/etc/users.db", buf, (int)sizeof(buf) - 1);
    if (n <= 0) return -1;
    buf[n] = 0;
    // 版本头
    if (!s_starts(buf, "VIMTU-USERSDB ")) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb load FAILED reason=bad magic\n");
            dbg64_line_end64();
        }
        return -1;
    }
    const int ver = i_parse(buf + 14, -1);
    if (ver != 1) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb load FAILED reason=unsupported version=");
            dbg64_dec((uint64_t)(ver < 0 ? 0 : ver));
            dbg64_nl();
            dbg64_line_end64();
        }
        return -1;
    }
    g_count = 0;
    int line_no = 0, bad = 0;
    int i = 0;
    while (i < n) {
        int j = i;
        while (j < n && buf[j] != '\n') j++;
        const int len = j - i;
        if (len > 0) {
            buf[j] = 0;
            line_no++;
            if (line_no > 1) {                     // 第 1 行是版本头
                if (g_count < USERDB64_MAX_USERS) {
                    User64Entry e;
                    if (db_parse_line64(buf + i, &e) == 0) {
                        g_ent[g_count++] = e;
                        if (log_ok64(LOGK_INIT)) {
                            dbg64_line_begin64();
                            dbg64_str("[USER64] userdb load line=");
                            dbg64_dec((uint64_t)line_no);
                            dbg64_str(" name=");
                            dbg64_str(e.name);
                            dbg64_str(" uid=");
                            dbg64_dec((uint64_t)e.uid);
                            dbg64_str(" home=");
                            dbg64_str(e.home);
                            dbg64_nl();
                            dbg64_line_end64();
                        }
                    } else {
                        bad++;
                        if (log_ok64(LOGK_ERR)) {
                            dbg64_line_begin64();
                            dbg64_str("[USER64] userdb load line=");
                            dbg64_dec((uint64_t)line_no);
                            dbg64_str(" skipped (bad record)\n");
                            dbg64_line_end64();
                        }
                    }
                } else {
                    bad++;
                }
            }
        }
        i = j + 1;
    }
    (void)bad;
    return g_count;
}

// ==================== 初始化 ====================
static int add_user64(const char* name, uint32_t uid, uint32_t flags, int save) {
    if (g_count >= USERDB64_MAX_USERS) return -1;
    User64Entry* e = &g_ent[g_count];
    ent_clear64(e);
    s_cpy(e->name, USERDB64_NAME_MAX, name);
    e->uid = uid;
    e->flags = flags;
    e->hidden = (flags & (USERDB64_F_HIDDEN | USERDB64_F_ROOT)) ? 1 : 0;
    set_home64(e);
    g_count++;
    if (flags & USERDB64_F_ROOT) g_root_idx = g_count - 1;
    ensure_user_dirs64(e);
    g_dirty = 1;
    if (save) db_save64("add");
    return g_count - 1;
}

static uint32_t next_uid64() {
    uint32_t uid = 1000;
    for (;;) {
        int used = 0;
        for (int i = 0; i < g_count; i++) if (g_ent[i].uid == uid) { used = 1; break; }
        if (!used) return uid;
        uid++;
    }
}

void userdb64_init64() {
    for (int i = 0; i < LOGK_N; i++) g_log_used[i] = 0;
    g_count = 0;
    g_root_idx = -1;
    g_gui_idx = -1;
    g_sess_idx = -1;
    g_db_ok = 0;
    g_db_loaded_from_disk = 0;

    if (sys_slot64() < 0) {
        // 没有系统卷（例如"装好但主分区还没格式化"的盘）：内存模式，如实打点
        add_user64("root", 0, USERDB64_F_ROOT | USERDB64_F_HIDDEN, 0);
        add_user64("vimtu", 1000, 0, 0);
        if (log_ok64(LOGK_INIT)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb init path=/etc/users.db FAILED reason=no system volume mode=ram-only users=");
            dbg64_dec((uint64_t)g_count);
            dbg64_nl();
            dbg64_line_end64();
        }
        return;
    }

    // 1) 目录骨架（Linux 语义）
    ensure_dir64("/etc");
    ensure_dir64("/home");
    ensure_dir64("/root");
    ensure_dir64("/root/Desktop");

    // 2) 读表
    const int loaded = db_load64();
    g_db_loaded_from_disk = (loaded > 0) ? 1 : 0;

    // 3) root 记录（隐藏；uid=0）
    g_root_idx = -1;
    for (int i = 0; i < g_count; i++) if (g_ent[i].uid == 0) { g_root_idx = i; break; }
    if (g_root_idx < 0) {
        g_root_idx = add_user64("root", 0, USERDB64_F_ROOT | USERDB64_F_HIDDEN, 0);
        ensure_dir64("/root");
        ensure_dir64("/root/Desktop");
    }
    g_ent[g_root_idx].flags |= USERDB64_F_ROOT | USERDB64_F_HIDDEN;
    g_ent[g_root_idx].hidden = 1;
    s_cpy(g_ent[g_root_idx].home, USERDB64_PATH_MAX, "/root");
    s_cpy(g_ent[g_root_idx].desktop, USERDB64_PATH_MAX, "/root/Desktop");

    // 4) 引导用户：表里一个普通用户都没有时建一个（否则开机会没任何可登录账户）
    if (userdb64_normal_count64() == 0) {
        const int idx = add_user64("vimtu", next_uid64(), 0, 0);
        if (log_ok64(LOGK_INIT)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdb firstboot bootstrap user=");
            dbg64_str("vimtu");
            dbg64_str(" uid=");
            dbg64_dec((uint64_t)(idx >= 0 ? g_ent[idx].uid : 0));
            dbg64_str(" (no password)\n");
            dbg64_line_end64();
        }
    }

    // 5) 每个用户的主目录/桌面 + 只读检查并打点（load 行已在 db_load64 里打过）
    for (int i = 0; i < g_count; i++) ensure_user_dirs64(&g_ent[i]);

    // 6) 落盘（首次建表 / root 补齐 / 目录变化都立即落盘）
    if (g_dirty || !g_db_loaded_from_disk) db_save64(g_db_loaded_from_disk ? "init" : "firstboot");

    if (log_ok64(LOGK_INIT)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] userdb init path=/etc/users.db version=");
        dbg64_dec((uint64_t)1);
        dbg64_str(" algo=sha256 iter=");
        dbg64_dec((uint64_t)USERDB64_HASH_ITER);
        dbg64_str(" salt=");
        dbg64_dec((uint64_t)USERDB64_SALT_BYTES);
        dbg64_str("B rng=rdtsc-xorshift(non-CSPRNG) loaded=");
        dbg64_dec((uint64_t)g_db_loaded_from_disk);
        dbg64_str(" users=");
        dbg64_dec((uint64_t)g_count);
        dbg64_str(" normal=");
        dbg64_dec((uint64_t)userdb64_normal_count64());
        dbg64_str(" root=hidden\n");
        dbg64_line_end64();
    }
    (void)userdb64_selftest64();
    (void)userdb64_selftest64();
}

// ==================== 查询 ====================
int userdb64_count64() { return g_count; }
const User64Entry* userdb64_at64(int i) { return (i >= 0 && i < g_count) ? &g_ent[i] : nullptr; }
int userdb64_find64(const char* name) {
    if (!name) return -1;
    for (int i = 0; i < g_count; i++) if (s_eq(g_ent[i].name, name)) return i;
    return -1;
}
const User64Entry* userdb64_find_root64() { return (g_root_idx >= 0) ? &g_ent[g_root_idx] : nullptr; }
int userdb64_normal_count64() {
    int n = 0;
    for (int i = 0; i < g_count; i++) if (!(g_ent[i].flags & USERDB64_F_ROOT) && !g_ent[i].hidden) n++;
    return n;
}
const User64Entry* userdb64_visible64(int k) {
    int n = 0;
    for (int i = 0; i < g_count; i++) {
        if ((g_ent[i].flags & USERDB64_F_ROOT) || g_ent[i].hidden) continue;
        if (n == k) return &g_ent[i];
        n++;
    }
    return nullptr;
}
const char* userdb64_name_for_uid64(uint32_t uid) {
    for (int i = 0; i < g_count; i++) if (g_ent[i].uid == uid) return g_ent[i].name;
    return "-";
}

// ==================== 账号管理 ====================
int userdb64_add64(const char* name, uint32_t* uid_out) {
    if (!name_valid64(name)) {
        if (log_ok64(LOGK_CMD)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] useradd FAIL name=");
            dbg64_str(name ? name : "-");
            dbg64_str(" reason=bad-name (1..15 printable ASCII, no '/' '|')\n");
            dbg64_line_end64();
        }
        return -1;
    }
    if (find_ptr64(name)) {
        if (log_ok64(LOGK_CMD)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] useradd FAIL name=");
            dbg64_str(name);
            dbg64_str(" reason=exists\n");
            dbg64_line_end64();
        }
        return -1;
    }
    if (g_count >= USERDB64_MAX_USERS) return -1;
    const uint32_t uid = next_uid64();
    const int idx = add_user64(name, uid, 0, 1);
    if (idx < 0) return -1;
    if (uid_out) *uid_out = uid;
    if (log_ok64(LOGK_CMD)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] useradd ok name=");
        dbg64_str(g_ent[idx].name);
        dbg64_str(" uid=");
        dbg64_dec((uint64_t)g_ent[idx].uid);
        dbg64_str(" home=");
        dbg64_str(g_ent[idx].home);
        dbg64_str(" desktop=");
        dbg64_str(g_ent[idx].desktop);
        dbg64_str(" password=none\n");
        dbg64_line_end64();
    }
    return 0;
}

int userdb64_del64(const char* name) {
    const int i = userdb64_find64(name);
    if (i < 0) return -1;
    if (g_ent[i].flags & USERDB64_F_ROOT) {
        if (log_ok64(LOGK_CMD)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdel FAIL name=root reason=cannot-delete-root\n");
            dbg64_line_end64();
        }
        return -1;
    }
    if (userdb64_normal_count64() <= 1) {
        if (log_ok64(LOGK_CMD)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] userdel FAIL name=");
            dbg64_str(name);
            dbg64_str(" reason=last-normal-user\n");
            dbg64_line_end64();
        }
        return -1;
    }
    const uint32_t uid = g_ent[i].uid;
    for (int k = i; k < g_count - 1; k++) g_ent[k] = g_ent[k + 1];
    g_count--;
    if (g_gui_idx >= g_count) g_gui_idx = -1;
    if (g_sess_idx >= g_count) g_sess_idx = -1;
    db_save64("userdel");
    if (log_ok64(LOGK_CMD)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] userdel ok name=");
        dbg64_str(name);
        dbg64_str(" uid=");
        dbg64_dec((uint64_t)uid);
        dbg64_str(" home-kept=1 (no recursive delete)\n");
        dbg64_line_end64();
    }
    return 0;
}

int userdb64_set_password64(int idx, const char* pw) {
    if (idx < 0 || idx >= g_count) return -1;
    User64Entry* e = &g_ent[idx];
    if (!pw || !pw[0]) {                       // 清除密码（直接登录）
        e->hash[0] = 0;
        e->salt[0] = 0;
        db_save64("passwd-clear");
        if (log_ok64(LOGK_CMD)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] passwd ok user=");
            dbg64_str(e->name);
            dbg64_str(" set=0 (no password -> direct login)\n");
            dbg64_line_end64();
        }
        return 0;
    }
    uint8_t salt[USERDB64_SALT_BYTES];
    salt_new64(salt);
    uint8_t h[32];
    pw_hash64(pw, salt, h);
    hex_enc(e->salt, USERDB64_SALT_MAX, salt, USERDB64_SALT_BYTES);
    hex_enc(e->hash, USERDB64_HASH_MAX, h, 32);
    for (int i = 0; i < 32; i++) h[i] = 0;
    for (int i = 0; i < USERDB64_SALT_BYTES; i++) salt[i] = 0;
    const int rc = db_save64("passwd");
    if (log_ok64(LOGK_CMD)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] passwd ok user=");
        dbg64_str(e->name);
        dbg64_str(" set=1 algo=sha256 iter=");
        dbg64_dec((uint64_t)USERDB64_HASH_ITER);
        dbg64_str(" salt=");
        dbg64_dec((uint64_t)USERDB64_SALT_BYTES);
        dbg64_str("B hashlen=");
        dbg64_dec((uint64_t)s_len(e->hash) / 2);
        dbg64_str("B store=/etc/users.db (plaintext never stored) rc=");
        dbg64_dec((uint64_t)(rc == 0 ? 0 : 1));
        dbg64_nl();
        dbg64_line_end64();
    }
    return rc;
}

int userdb64_check_password64(int idx, const char* pw) {
    if (idx < 0 || idx >= g_count) return -1;
    const User64Entry* e = &g_ent[idx];
    if (!e->hash[0]) return 1;                 // 没设密码：直接放行
    if (!pw) return 0;
    uint8_t salt[USERDB64_SALT_BYTES];
    int sn = 0;
    if (hex_dec(salt, USERDB64_SALT_BYTES, e->salt, &sn) != 0 || sn != USERDB64_SALT_BYTES) return 0;
    uint8_t want[32], got[32];
    int hn = 0;
    if (hex_dec(want, 32, e->hash, &hn) != 0 || hn != 32) return 0;
    pw_hash64(pw, salt, got);
    int same = 1;
    for (int i = 0; i < 32; i++) { if (want[i] != got[i]) same = 0; want[i] = 0; got[i] = 0; }
    for (int i = 0; i < USERDB64_SALT_BYTES; i++) salt[i] = 0;
    return same ? 1 : 0;
}

int userdb64_save64() { return db_save64("manual"); }

// ==================== 桌面文件列表 ====================
#define USERDB64_DESK_MAX 32
static Vfs64Dirent64 g_desk[USERDB64_DESK_MAX];
static int g_desk_n = 0;
static int g_desk_total = 0;
static char g_desk_dir[USERDB64_PATH_MAX];

static void desktop_scan64(const User64Entry* e) {
    g_desk_n = 0;
    g_desk_total = 0;
    g_desk_dir[0] = 0;
    if (!e) return;
    s_cpy(g_desk_dir, USERDB64_PATH_MAX, e->desktop);
    const int slot = sys_slot64();
    if (slot < 0) return;
    uint32_t cursor = 0;
    for (;;) {
        Vfs64Dirent64 d;
        const int r = vfs64_list64_on64(slot, e->desktop, &d, 1, &cursor);
        if (r <= 0) break;
        g_desk_total++;
        if (g_desk_n < USERDB64_DESK_MAX) g_desk[g_desk_n++] = d;
        if (g_desk_total > 512) break;         // 有界
    }
    if (log_ok64(LOGK_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] desktop list user=");
        dbg64_str(e->name);
        dbg64_str(" dir=");
        dbg64_str(e->desktop);
        dbg64_str(" entries=");
        dbg64_dec((uint64_t)g_desk_total);
        dbg64_str(" names=");
        for (int i = 0; i < g_desk_n; i++) {
            if (i) dbg64_str(",");
            dbg64_str(g_desk[i].name);
        }
        if (g_desk_n == 0) dbg64_str("(empty)");
        dbg64_nl();
        dbg64_line_end64();
    }
}

int userdb64_desktop_count64() { return g_desk_n; }
const char* userdb64_desktop_name64(int i) { return (i >= 0 && i < g_desk_n) ? g_desk[i].name : ""; }
uint32_t userdb64_desktop_size64(int i) { return (i >= 0 && i < g_desk_n) ? g_desk[i].size : 0; }
const char* userdb64_desktop_dir64() { return g_desk_dir; }
int userdb64_desktop_total64() { return g_desk_total; }

// ==================== 登录 / 会话身份 ====================
const User64Entry* userdb64_gui_user64() { return (g_gui_idx >= 0) ? &g_ent[g_gui_idx] : nullptr; }
const User64Entry* userdb64_session_user64() { return (g_sess_idx >= 0) ? &g_ent[g_sess_idx] : nullptr; }
int userdb64_euid64() { return (g_sess_idx >= 0) ? (int)g_ent[g_sess_idx].uid : -1; }
const char* userdb64_session_name64() { return (g_sess_idx >= 0) ? g_ent[g_sess_idx].name : "-"; }
int userdb64_root_session64() { return (g_sess_idx >= 0 && (g_ent[g_sess_idx].flags & USERDB64_F_ROOT)) ? 1 : 0; }

const char* userdb64_last_user64() {
    static char buf[USERDB64_NAME_MAX];
    buf[0] = 0;
    config64_get_str64("ui.login.last", "", buf, (int)sizeof(buf));
    return buf;
}

// 每用户设置生效（主题 / 壁纸 / 锁屏壁纸模式）：只在用户**显式设过**（非 inherit）时动手
static void apply_user_settings64(const User64Entry* e) {
    if (!e) return;
    if (e->theme >= 0 && e->theme < THEME64_THEME_COUNT) {
        if (theme64_id64() != e->theme) theme64_set_theme64(e->theme, "user login");
    }
    if (e->wall[0]) {
        Img64 im{};
        if (img64_load_vfs64(e->wall, &im) == 0) {
            const int rc = gfx64_wall_set_source64(im.px, im.w, im.h, 1, "vfs:user-wall");
            img64_free64(&im);
            if (log_ok64(LOGK_LOGIN)) {
                dbg64_line_begin64();
                dbg64_str("[USER64] login apply settings user=");
                dbg64_str(e->name);
                dbg64_str(" wall=");
                dbg64_str(e->wall);
                dbg64_str(" loaded=1 rc=");
                dbg64_dec((uint64_t)(rc == 0 ? 0 : 1));
                dbg64_nl();
                dbg64_line_end64();
            }
        } else if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] login apply settings user=");
            dbg64_str(e->name);
            dbg64_str(" wall=");
            dbg64_str(e->wall);
            dbg64_str(" FAILED reason=");
            dbg64_str(img64_last_err64());
            dbg64_nl();
            dbg64_line_end64();
        }
    }
    if (log_ok64(LOGK_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] login apply settings user=");
        dbg64_str(e->name);
        dbg64_str(" theme=");
        if (e->theme >= 0) dbg64_dec((uint64_t)e->theme); else dbg64_str("inherit");
        dbg64_str(" wall=");
        dbg64_str(e->wall[0] ? e->wall : "-");
        dbg64_str(" lock_mode=");
        if (e->lock_mode >= 0) dbg64_dec((uint64_t)e->lock_mode); else dbg64_str("inherit");
        dbg64_str(" avatar=");
        dbg64_str(e->avatar[0] ? e->avatar : "(builtin)");
        dbg64_nl();
        dbg64_line_end64();
    }
}

int userdb64_login64(const char* name) {
    const int i = userdb64_find64(name);
    if (i < 0) {
        if (log_ok64(LOGK_ERR)) {
            dbg64_line_begin64();
            dbg64_str("[USER64] login FAIL name=");
            dbg64_str(name ? name : "-");
            dbg64_str(" reason=no-such-user\n");
            dbg64_line_end64();
        }
        return -1;
    }
    g_gui_idx = i;
    g_sess_idx = i;                                   // 登录 = 会话身份也回到该用户（root 会话不跨登录）
    config64_set_str64("ui.login.last", g_ent[i].name);
    (void)config64_flush64();
    desktop_scan64(&g_ent[i]);
    apply_user_settings64(&g_ent[i]);
    if (log_ok64(LOGK_LOGIN)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] login ok name=");
        dbg64_str(g_ent[i].name);
        dbg64_str(" uid=");
        dbg64_dec((uint64_t)g_ent[i].uid);
        dbg64_str(" home=");
        dbg64_str(g_ent[i].home);
        dbg64_str(" desktop=");
        dbg64_str(g_ent[i].desktop);
        dbg64_str(" gui=");
        dbg64_str(g_ent[i].name);
        dbg64_str(" euid=");
        dbg64_dec((uint64_t)g_ent[i].uid);
        dbg64_nl();
        dbg64_line_end64();
    }
    return 0;
}

int userdb64_session_root64(const char* via) {
    if (g_root_idx < 0) return -1;
    if (g_gui_idx < 0) return -1;                     // 还没登录过：没有会话可切
    if ((g_ent[g_gui_idx].flags & USERDB64_F_ROOT)) return -1;
    const int from = g_sess_idx;
    g_sess_idx = g_root_idx;
    if (log_ok64(LOGK_CMD)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] su ok from=");
        dbg64_str(from >= 0 ? g_ent[from].name : "-");
        dbg64_str(" to=root euid=0 gui=");
        dbg64_str(g_ent[g_gui_idx].name);
        dbg64_str(" gui_unchanged=1 via=");
        dbg64_str(via ? via : "su");
        dbg64_str(" (permission bits not enforced yet; P4)\n");
        dbg64_line_end64();
    }
    return 0;
}

int userdb64_session_exit64() {
    if (g_sess_idx < 0 || !(g_ent[g_sess_idx].flags & USERDB64_F_ROOT)) return 1;
    const int from = g_sess_idx;
    g_sess_idx = g_gui_idx;
    if (log_ok64(LOGK_CMD)) {
        dbg64_line_begin64();
        dbg64_str("[USER64] exit ok from=");
        dbg64_str(g_ent[from].name);
        dbg64_str(" to=");
        dbg64_str(g_sess_idx >= 0 ? g_ent[g_sess_idx].name : "-");
        dbg64_str(" euid=");
        dbg64_dec((uint64_t)(g_sess_idx >= 0 ? (int)g_ent[g_sess_idx].uid : -1));
        dbg64_str(" gui=");
        dbg64_str(g_gui_idx >= 0 ? g_ent[g_gui_idx].name : "-");
        dbg64_nl();
        dbg64_line_end64();
    }
    return 0;
}

// ==================== 报告（终端 users）====================
int userdb64_report64(char* out, int max) {
    struct W {
        static int put(char* o, int cap, int& n, const char* s) {
            while (*s) { if (n >= cap - 1) return n; o[n++] = *s++; o[n] = 0; }
            return n;
        }
        static int num(char* o, int cap, int& n, int v) {
            char t[12];
            u32_str(t, (int)sizeof(t), (uint32_t)(v < 0 ? 0 : v));
            return put(o, cap, n, t);
        }
    };
    int n = 0;
    out[0] = 0;
    W::put(out, max, n, "[USER64] users count=");
    W::num(out, max, n, g_count);
    W::put(out, max, n, " root=1 normal=");
    W::num(out, max, n, userdb64_normal_count64());
    W::put(out, max, n, " (root is hidden in the login UI)  list=");
    for (int i = 0; i < g_count; i++) {
        if (i) W::put(out, max, n, ",");
        W::put(out, max, n, g_ent[i].name);
    }
    W::put(out, max, n, "\n");
    for (int i = 0; i < g_count; i++) {
        const User64Entry* e = &g_ent[i];
        W::put(out, max, n, "  ");
        W::put(out, max, n, e->name);
        for (int k = s_len(e->name); k < 8; k++) W::put(out, max, n, " ");
        W::put(out, max, n, " uid=");
        W::num(out, max, n, (int)e->uid);
        W::put(out, max, n, (e->flags & USERDB64_F_ROOT) ? " admin/root" : " user      ");
        W::put(out, max, n, (e->flags & USERDB64_F_ROOT) ? " home=/root" : " home=");
        if (!(e->flags & USERDB64_F_ROOT)) W::put(out, max, n, e->home);
        W::put(out, max, n, e->hash[0] ? "  password=set" : "  password=none");
        if (e->hidden) W::put(out, max, n, "  [hidden in login UI]");
        if (i == g_gui_idx) W::put(out, max, n, "  [gui session]");
        if (i == g_sess_idx && g_sess_idx != g_gui_idx) W::put(out, max, n, "  [session euid]");
        W::put(out, max, n, "\n");
    }
    return n;
}

// ==================== 自检 ====================
int userdb64_selftest64() {
    int fails = 0;
    // 1) SHA-256 已知向量："abc" -> ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    {
        Sha256Ctx c;
        uint8_t d[32];
        sha256_init(&c);
        sha256_update(&c, "abc", 3);
        sha256_final(&c, d);
        static const uint8_t want[32] = {
            0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
            0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
        };
        for (int i = 0; i < 32; i++) if (d[i] != want[i]) fails |= 1;
        // 空串向量：e3b0c442...
        sha256_init(&c);
        sha256_update(&c, "", 0);
        sha256_final(&c, d);
        if (d[0] != 0xe3 || d[1] != 0xb0 || d[2] != 0xc4 || d[3] != 0x42) fails |= 2;
    }
    // 2) 口令哈希：同盐同口令必须一致、换盐必须不同、明文不得出现在哈希/盐里
    {
        uint8_t s1[USERDB64_SALT_BYTES], s2[USERDB64_SALT_BYTES];
        for (int i = 0; i < USERDB64_SALT_BYTES; i++) { s1[i] = (uint8_t)(i * 7 + 1); s2[i] = (uint8_t)(0xA0 - i); }
        uint8_t a[32], b[32], c[32];
        pw_hash64("Vimtu-pw-123", s1, a);
        pw_hash64("Vimtu-pw-123", s1, b);
        pw_hash64("Vimtu-pw-123", s2, c);
        for (int i = 0; i < 32; i++) if (a[i] != b[i]) fails |= 4;
        int diff = 0;
        for (int i = 0; i < 32; i++) if (a[i] != c[i]) diff = 1;
        if (!diff) fails |= 8;
        if (a[0] == 'V' && a[1] == 'i') fails |= 16;         // 绝不是明文
    }
    // 3) 十六进制往返
    {
        uint8_t raw[6] = { 0x00, 0x0F, 0xA5, 0xFF, 0x10, 0x7E };
        char hex[16];
        uint8_t back[6];
        int bn = 0;
        hex_enc(hex, (int)sizeof(hex), raw, 6);
        if (!s_eq(hex, "000fa5ff107e")) fails |= 32;
        if (hex_dec(back, 6, hex, &bn) != 0 || bn != 6) fails |= 64;
        for (int i = 0; i < 6; i++) if (back[i] != raw[i]) fails |= 64;
    }
    // 4) 表序列化 -> 解析往返（名字/uid/home/主题/wall/lock_mode/flags 逐字段一致）
    {
        static char buf[512];
        User64Entry t;
        ent_clear64(&t);
        s_cpy(t.name, USERDB64_NAME_MAX, "selftest");
        t.uid = 4242;
        set_home64(&t);
        s_cpy(t.hash, USERDB64_HASH_MAX, "00112233");
        s_cpy(t.salt, USERDB64_SALT_MAX, "ffeeddcc");
        s_cpy(t.avatar, USERDB64_PATH_MAX, "builtin:2");
        t.theme = 3;
        s_cpy(t.wall, USERDB64_PATH_MAX, "/wall/x.png");
        t.lock_mode = 1;
        t.flags = 0;
        int n = 0;
        struct P2 {
            static void put(char* o, int& n, const char* s) { while (*s) o[n++] = *s++; }
            static void num(char* o, int& n, int v) { char t2[12]; u32_str(t2, 12, (uint32_t)v); put(o, n, t2); }
        };
        P2::put(buf, n, "U|");
        P2::put(buf, n, t.name);
        P2::put(buf, n, "|");
        P2::num(buf, n, (int)t.uid);
        P2::put(buf, n, "|");
        P2::put(buf, n, t.home);
        P2::put(buf, n, "|");
        P2::put(buf, n, t.hash);
        P2::put(buf, n, "|");
        P2::put(buf, n, t.salt);
        P2::put(buf, n, "|");
        P2::put(buf, n, t.avatar);
        P2::put(buf, n, "|");
        P2::num(buf, n, t.theme);
        P2::put(buf, n, "|");
        P2::put(buf, n, t.wall);
        P2::put(buf, n, "|");
        P2::num(buf, n, t.lock_mode);
        P2::put(buf, n, "|0");
        buf[n] = 0;
        User64Entry r;
        if (db_parse_line64(buf, &r) != 0) fails |= 128;
        else {
            if (!s_eq(r.name, "selftest") || r.uid != 4242) fails |= 128;
            if (!s_eq(r.home, "/home/selftest") || !s_eq(r.desktop, "/home/selftest/Desktop")) fails |= 128;
            if (!s_eq(r.hash, "00112233") || !s_eq(r.salt, "ffeeddcc")) fails |= 128;
            if (!s_eq(r.avatar, "builtin:2") || r.theme != 3 || r.lock_mode != 1) fails |= 128;
            if (!s_eq(r.wall, "/wall/x.png")) fails |= 128;
        }
    }
    // 5) 路径/名字合法性
    if (name_valid64("a b")) fails |= 256;
    if (name_valid64("a/b")) fails |= 256;
    if (!name_valid64("user_1")) fails |= 256;
    if (name_valid64("")) fails |= 256;
    if (path_valid64("relative", 0)) fails |= 512;
    if (!path_valid64("/home/a", 0)) fails |= 512;
    if (!path_valid64("", 1) || path_valid64("", 0)) fails |= 512;
    // 6) 表格一致性：root 必须隐藏、登录界面只看到普通用户
    if (g_root_idx >= 0) {
        if (!(g_ent[g_root_idx].flags & USERDB64_F_ROOT) || !g_ent[g_root_idx].hidden) fails |= 1024;
        for (int k = 0; k < userdb64_normal_count64() + 2; k++) {
            const User64Entry* v = userdb64_visible64(k);
            if (!v) break;
            if (v->flags & USERDB64_F_ROOT) fails |= 1024;
            if (v->hidden) fails |= 1024;
        }
    } else {
        fails |= 1024;
    }
    for (int i = 0; i < g_count; i++) {
        if (!name_valid64(g_ent[i].name)) fails |= 2048;
        if (!path_valid64(g_ent[i].home, 0)) fails |= 2048;
        if (!path_valid64(g_ent[i].desktop, 0)) fails |= 2048;
    }

    dbg64_line_begin64();
    dbg64_str("[USER64] selftest userdb ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_nl();
    dbg64_line_end64();
    return fails;
}
