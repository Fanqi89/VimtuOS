// sha256_64.h - 内核共用的 SHA-256 + 十六进制 + 加盐迭代口令哈希（header-only）
//
// 谁在用：
//   * kernel/userdb64.cpp  -> /etc/users.db 的口令字段（useradd/passwd/su）
//   * kernel/userconf64.h  -> 安装向导写 /etc/user.conf 的 pass_salt/pass_hash
//   * 登录界面（locklogin64）校验 /etc/user.conf 里的口令
//
// 为什么是 header-only：安装程序内核（-DVIMTU_INSTALLER_MEDIA）与系统内核是**两遍编译**，
// 安装程序那一遍只链一部分目标文件；把这段做成 static inline 就不用改任何链接表，
// 也保证两份内核的哈希**逐字节一致**（口径不一致会让"装的时候设的密码，开机验不过"）。
//
// 算法口径（与 docs / [USER64] 打点一致，别改）：
//   h0 = SHA256(salt(16B) || pw)；之后 (ITER-1) 轮 h = SHA256(h || salt)；ITER = 1000
//   落盘只存 salt 的 hex 与 h 的 hex —— **明文永不落盘、也永不进串口**。
#ifndef SHA256_64_H
#define SHA256_64_H

#include <stdint.h>

static inline int sha64_len(const char* s) { int n = 0; while (s && s[n]) n++; return n; }

// ==================== SHA-256（自带，无 libc）====================
// ★ 放进头文件的原因：安装程序内核也要算口令哈希（写 /etc/user.conf），但它**不链接**
//   userdb64.cpp（那个模块依赖 theme64/gfx64/img64）。把这一小段做成 header-only，
//   两份内核共用同一份实现 —— 绝不复制第二套 SHA-256。
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
    const uint32_t plen = (uint32_t)sha64_len(pw);
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

#endif // SHA256_64_H
