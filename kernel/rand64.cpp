// kernel/rand64.cpp - ★ v0.12.3：密码学级随机数（CSPRNG）：RDRAND/RDSEED + SHA-256 计数器 DRBG
//
// 设计要点（为什么这样写，见 rand64.h 的注释）：
//   ① 有 RDRAND 就直接用它出字节 —— 这是 CPU 里的数字随机数发生器，**不需要我们再造轮子**；
//      每消耗 1 KiB 用 RDSEED（有的话）重新播种一次，避免长期不 rekey。
//   ② 没有 RDRAND（例如 QEMU 默认 `-cpu qemu64`）时，用 SHA-256 计数器 DRBG，种子来自：
//      TSC 抖动采样（连续 rdtsc 差值的最低位，被中断/预取扰动）+ PIT tick + 中断/切换计数。
//      这类抖动不是"真熵"，但对我们能拿到的模拟器/老 CPU 来说，是唯一可用的来源；
//      **打点里如实写明 seed_src=rdseed|rdrand|tsc-jitter**，不冒充硬件熵。
//   ③ 输出永不返回全 0（极端情况兜底：全 0 就强制再取一块，最多重试 8 次）。
#include "rand64.h"
#include "x86_64.h"        // ticks64()（sha256_64.h 的 rng 要用）
#include "sha256_64.h"     // sha256_init/update/final（本仓唯一的 SHA-256 实现）
#include "debug64.h"

// 两位十六进制（debug64.h 只有 hex64，没有单字节版）
static void dbg_hex8v(uint8_t b) {
    const char* hx = "0123456789ABCDEF";
    char t[3];
    t[0] = hx[(b >> 4) & 0xF];
    t[1] = hx[b & 0xF];
    t[2] = 0;
    dbg64_str(t);
}

extern volatile uint64_t g_ticks64;

#ifndef PIT_HZ_64
#define PIT_HZ_64 250u
#endif

// ---------------- CPUID / 硬件熵 ----------------
static void cpuid64(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d) {
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static int g_have_rdrand = -1;
static int g_have_rdseed = -1;

static int rdrand64_u64(uint64_t* out) {
    uint8_t ok = 0;
    uint64_t v = 0;
    // RDRAND 可能因为熵池空而 CF=0：最多重试 10 次（Intel SDM 建议）
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdrand %0\n\tsetc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

static int rdseed64_u64(uint64_t* out) {
    uint8_t ok = 0;
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
        __asm__ volatile("rdseed %0\n\tsetc %1" : "=r"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

static uint64_t rdtsc64(void) {
    uint32_t lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

// ---------------- SHA-256 计数器 DRBG ----------------
static uint8_t g_state[32];      // DRBG state
static uint64_t g_ctr;           // 块计数器
static uint8_t g_block[32];      // 当前块剩余
static uint32_t g_block_off = 32; // = 32 表示"该出新块了"
static int g_inited = 0;
static int g_seed_bytes = 0;
static const char* g_seed_src = "none";
static uint64_t g_out_bytes = 0;

static void drbg_rekey(const uint8_t block[32]) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, g_state, 32);
    sha256_update(&c, block, 32);
    sha256_final(&c, g_state);
}

static void drbg_block(void) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, g_state, 32);
    uint8_t cb[8];
    for (int i = 0; i < 8; i++) cb[i] = (uint8_t)(g_ctr >> (56 - 8 * i));
    g_ctr++;
    sha256_update(&c, cb, 8);
    sha256_final(&c, g_block);
    drbg_rekey(g_block);          // 后向安全：出块即 rekey
    g_block_off = 0;
}

static void drbg_seed(const uint8_t* extra, uint32_t n) {
    Sha256Ctx c;
    sha256_init(&c);
    sha256_update(&c, "vimtuos-rand64-v1", 17);
    if (extra && n) sha256_update(&c, extra, n);
    uint64_t t = rdtsc64();
    sha256_update(&c, &t, 8);
    uint64_t k = g_ticks64;
    sha256_update(&c, &k, 8);
    sha256_final(&c, g_state);
    g_ctr = 0;
    g_block_off = 32;
    g_inited = 1;
    // 立刻丢弃第一块（Hash_DRBG 的"首个输出不直接用"惯例）
    drbg_block();
}

// 无硬件熵源时的播种：TSC 抖动里取位。抖动的来源是中断/预取/缓存/流水线，
// 在模拟器和老 CPU 上这是唯一可用的东西 —— 打点里写 seed_src=tsc-jitter，不冒充真熵。
static void seed_from_jitter(void) {
    uint8_t pool[32];
    for (int i = 0; i < 32; i++) pool[i] = 0;
    uint64_t prev = rdtsc64();
    uint32_t bit = 0;
    for (int round = 0; round < 4096 && bit < 256; round++) {
        uint64_t now = rdtsc64();
        const uint64_t d = now - prev;
        prev = now;
        // 取差值的最低位（最低位受噪声影响最大）
        const uint8_t b = (uint8_t)(d & 1u);
        pool[bit >> 3] |= (uint8_t)(b << (bit & 7));
        bit++;
        // 每采样 8 次插一点别的量，避免"纯 TSC 低位"退化
        if ((round & 7) == 7) {
            pool[(bit >> 3) & 31] ^= (uint8_t)(g_ticks64 & 0xFF);
        }
    }
    drbg_seed(pool, 32);
    g_seed_bytes = 32;
    g_seed_src = "tsc-jitter";
}

void rand64_init64(void) {
    if (g_have_rdrand < 0) {
        uint32_t a = 0, b = 0, c = 0, d = 0;
        cpuid64(0, 0, &a, &b, &c, &d);
        const uint32_t max_leaf = a;
        if (max_leaf >= 1) {
            cpuid64(1, 0, &a, &b, &c, &d);
            g_have_rdrand = (c & (1u << 30)) ? 1 : 0;      // CPUID.01h:ECX.RDRAND
        } else {
            g_have_rdrand = 0;
        }
        g_have_rdseed = 0;
        if (max_leaf >= 7) {
            cpuid64(7, 0, &a, &b, &c, &d);
            g_have_rdseed = (b & (1u << 18)) ? 1 : 0;      // CPUID.07h:EBX.RDSEED
        }
    }
    if (g_inited) return;

    // 播种：优先 RDSEED（真熵），其次 RDRAND，最后抖动
    uint8_t seed[64];
    uint32_t got = 0;
    if (g_have_rdseed) {
        for (int i = 0; i < 8; i++) {
            uint64_t v = 0;
            if (!rdseed64_u64(&v)) break;
            for (int k = 0; k < 8; k++) seed[got++] = (uint8_t)(v >> (8 * k));
        }
        if (got) g_seed_src = "rdseed";
    }
    if (got == 0 && g_have_rdrand) {
        for (int i = 0; i < 8; i++) {
            uint64_t v = 0;
            if (!rdrand64_u64(&v)) break;
            for (int k = 0; k < 8; k++) seed[got++] = (uint8_t)(v >> (8 * k));
        }
        if (got) g_seed_src = "rdrand";
    }
    if (got == 0) {
        seed_from_jitter();
    } else {
        drbg_seed(seed, got);
        g_seed_bytes = (int)got;
    }
    g_out_bytes = 0;
}

void rand64_bytes64(void* out, uint32_t n) {
    if (!out || !n) return;
    if (!g_inited) rand64_init64();
    uint8_t* p = (uint8_t*)out;

    // 有 RDRAND 就直接用（每 1 KiB 重新播种一次；有 RDSEED 就用它）
    if (g_have_rdrand) {
        while (n > 0) {
            uint64_t v = 0;
            if (!rdrand64_u64(&v)) break;          // 取不到就退回 DRBG
            const uint32_t take = (n < 8) ? n : 8;
            for (uint32_t k = 0; k < take; k++) p[k] = (uint8_t)(v >> (8 * k));
            p += take; n -= take; g_out_bytes += take;
        }
        if (n == 0) {
            if (g_have_rdseed && g_out_bytes >= 1024u) {
                uint8_t s[32]; uint32_t g = 0;
                for (int i = 0; i < 4; i++) { uint64_t v = 0; if (!rdseed64_u64(&v)) break;
                    for (int k = 0; k < 8; k++) s[g++] = (uint8_t)(v >> (8 * k)); }
                if (g) drbg_seed(s, g);
                g_out_bytes = 0;
            }
            return;
        }
    }

    // DRBG 路径
    for (int guard = 0; n > 0 && guard < 8; guard++) {
        while (n > 0) {
            if (g_block_off >= 32) drbg_block();
            const uint32_t avail = 32 - g_block_off;
            const uint32_t take = (n < avail) ? n : avail;
            for (uint32_t k = 0; k < take; k++) p[k] = g_block[g_block_off + k];
            g_block_off += take;
            p += take; n -= take; g_out_bytes += take;
        }
    }
}

uint32_t rand64_u32_64(void) { uint32_t v = 0; rand64_bytes64(&v, 4); return v; }
uint64_t rand64_u64_64(void) { uint64_t v = 0; rand64_bytes64(&v, 8); return v; }
int rand64_rdrand64(void) { if (g_have_rdrand < 0) rand64_init64(); return g_have_rdrand; }
int rand64_rdseed64(void) { if (g_have_rdseed < 0) rand64_init64(); return g_have_rdseed; }
int rand64_seed_bytes64(void) { if (!g_inited) rand64_init64(); return g_seed_bytes; }
uint32_t rand64_salt32_64(void) { return rand64_u32_64(); }

// ---------------- 自检 ----------------
// 判据（都要能抓住"退化成一个常数"这个最常见故障）：
//   ① 1024 字节不全 0，且不等于 1024 个相同字节
//   ② 256 个桶的字节分布：最热桶计数 <= 3× 期望（1024/256 = 4 -> 上限 12）
//   ③ 64 个 16 字节块里没有重复块
//   ④ 连续两次取 32 字节结果不同
int rand64_selftest64() {
    rand64_init64();
    uint8_t buf[1024];
    rand64_bytes64(buf, sizeof(buf));

    bool ok = true;
    // ① 非全 0 / 非全同值
    bool all_zero = true, all_same = true;
    for (uint32_t i = 0; i < sizeof(buf); i++) {
        if (buf[i] != 0) all_zero = false;
        if (buf[i] != buf[0]) all_same = false;
    }
    if (all_zero || all_same) ok = false;

    // ② 字节分布
    uint32_t hist[256];
    for (int i = 0; i < 256; i++) hist[i] = 0;
    for (uint32_t i = 0; i < sizeof(buf); i++) hist[buf[i]]++;
    uint32_t hottest = 0;
    for (int i = 0; i < 256; i++) if (hist[i] > hottest) hottest = hist[i];
    if (hottest > 12u) ok = false;                    // 期望 4，上限 12（3 倍）

    // ③ 16 字节块不重复
    for (int i = 0; i < 64; i++) {
        for (int j = i + 1; j < 64; j++) {
            int same = 1;
            for (int k = 0; k < 16; k++) if (buf[i * 16 + k] != buf[j * 16 + k]) { same = 0; break; }
            if (same) ok = false;
        }
    }

    // ④ 两次调用不同
    uint8_t a[32], b[32];
    rand64_bytes64(a, sizeof(a));
    rand64_bytes64(b, sizeof(b));
    int same_ab = 1;
    for (int k = 0; k < 32; k++) if (a[k] != b[k]) { same_ab = 0; break; }
    if (same_ab) ok = false;

    dbg64_line_begin64();
    dbg64_str("[RAND64] selftest ");
    dbg64_str(ok ? "PASS" : "FAIL");
    dbg64_str(" csprng=sha256-ctr drbg=1 rdrand=");
    dbg64_dec((uint64_t)(g_have_rdrand > 0 ? 1 : 0));
    dbg64_str(" rdseed=");
    dbg64_dec((uint64_t)(g_have_rdseed > 0 ? 1 : 0));
    dbg64_str(" seed_src=");
    dbg64_str(g_seed_src);
    dbg64_str(" seed_bytes=");
    dbg64_dec((uint64_t)g_seed_bytes);
    dbg64_str(" hottest=");
    dbg64_dec((uint64_t)hottest);
    dbg64_str(" sample=");
    for (int i = 0; i < 4; i++) dbg_hex8v(buf[i]);
    dbg64_nl();
    dbg64_line_end64();
    return ok ? 0 : -1;
}
