/* vs_zstd.c - ★ v0.10.3：.deb 的 zstd 解压（RFC 8878 帧解码器，纯 ring3，内核零改动）
 *
 * 为什么要有它：Ubuntu 21.10 / Debian 12 起 dpkg 默认用 `control.tar.zst` +
 *   `data.tar.zst`（zstd）。仓库原本只有 gzip（vs_gzip.c）与 xz（vs_xz.c），于是发行版
 *   **原样**包在 ar 扫描阶段就 `VS_E_FORMAT`（成员名根本匹配不上）。本文件补上第三条路。
 *
 * 为什么自己写而不 vendored：
 *   zstd 官方的 decompress-only 单文件（zstddeclib）也是几百 KB 源码，而 vpkg 的装载区
 *   在 v0.10.2 之前只有 64 KiB（kernel/elf64.cpp 的 e64_hi64）。所以按 RFC 8878 写一个
 *   **只覆盖 .deb 需要**的实现：
 *     支持：多帧拼接、skippable 帧、Raw/RLE/Compressed 块、预定义/自定义/重复 FSE 表、
 *           Huffman 字面量（1 流与 4 流）与 Treeless、repeat offset、xxh64 内容校验（真验）。
 *     不支持（在 *why 里如实说出来，不假装成功）：
 *           * 字典帧（Dictionary_ID != 0）：.deb 不用字典；
 *           * 跨帧复用状态（每帧重新开始，符合规范）。
 *
 * 内存口径（与 xz 那条路一致）：
 *   我们**不按 Window_Size 分配环形窗口**，而是把调用方给的输出缓冲本身当历史窗口
 *   （`dst[0..out)`）。依据：zstd 的匹配距离只能引用**本帧已产出**的字节，所以
 *   "产出 ≤ dst_cap"时永不回绕；代价是某个成员解出来 > dst_cap 会被如实拒掉（why=output-overflow）。
 *   **字面量必须单独一块**（lit/litcap）：序列执行时要"边取字面量边把输出写到同一个位置上"，
 *   就地解会彼此覆盖。zstd 单个块解压后 ≤ 128 KiB，所以 litcap >= 128 KiB 就够。
 *
 * 工作区一律**外置**（vs.h 的 vs_mmap）：本仓库全体用户态都编
 *   -fno-zero-initialized-in-bss，十几 KB 的静态解码表会进 .data 把映像顶出装载区。
 *
 * 打点：本文件**不打点**（保持零依赖，可在宿主上单文件自测，见 tools/vs_zstd_hosttest.c）。
 *   失败原因通过 *why 返回静态 ASCII 串，由 vs_pkg.c 打成
 *   `[VPKG] deb zstd member=<名> at=<偏移> reason=<...>`。
 *
 * 依赖：**一个头文件都不包含**（不用 vs.h、不用 libc）。这样同一份源码能被
 *   tools/vs_zstd_hosttest.c 用宿主 gcc 直接编译，拿真 zstd 造的 .zst 做交叉验证
 *   （见 docs/zstd解码器.md 的验收口径）。它需要的东西只有 int/unsigned/long long。
 */

/* ==================== 码本（RFC 8878 §3.1.1.3.2.2） ==================== */

#define ZS_LL_MAX 35
#define ZS_ML_MAX 52
#define ZS_OF_MAX 31

#define ZS_LL_LOG 6
#define ZS_ML_LOG 6
#define ZS_OF_LOG 5

#define ZS_HUF_MAXBITS 11
#define ZS_FSE_MAXLOG  9

static const unsigned ZS_LL_BASE[36] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
    16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536
};
static const unsigned char ZS_LL_BITS[36] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16
};
static const unsigned ZS_ML_BASE[53] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
    19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33, 34,
    35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 131, 259, 515, 1027, 2051,
    4099, 8195, 16387, 32771, 65539
};
static const unsigned char ZS_ML_BITS[53] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11,
    12, 13, 14, 15, 16
};

/* 预定义分布（规范化计数；-1 = "less than 1" 的低概率符号）
 *   三个数组的 |值| 之和都恰好等于 2^tableLog（LL/ML = 64，OF = 32），这是构建器的自检依据。 */
static const short ZS_LL_NORM[36] = {
    4, 3, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 3, 2, 1, 1, 1, 1, 1,
    -1, -1, -1, -1
};
static const short ZS_ML_NORM[53] = {
    1, 4, 3, 2, 2, 2, 2, 2,
    2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, -1, -1,
    -1, -1, -1, -1, -1
};
/* 预定义 Offset 表只规范到 code 28，29..31 概率 0（预定义模式下不会出现） */
static const short ZS_OF_NORM[32] = {
    1, 1, 1, 1, 1, 1, 2, 2,
    2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    -1, -1, -1, -1, -1, 0, 0, 0
};

/* 宿主自测用的追踪（-DZS_TRACE）：产品构建里 ZS_TRACE 一定未定义，零开销 */
#ifdef ZS_TRACE
#include <stdio.h>
#define ZTR(...) do { fprintf(stderr, __VA_ARGS__); } while (0)
#else
#define ZTR(...) do { } while (0)
#endif

/* ==================== 小工具 ==================== */

static unsigned zs_rd16(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}
static unsigned zs_rd24(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16);
}
static unsigned zs_rd32(const unsigned char* p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
/* 小端读 k 字节（k <= 5）到 u64 */
static unsigned long long zs_rdle(const unsigned char* p, int k) {
    unsigned long long v = 0;
    for (int i = 0; i < k; i++) v |= (unsigned long long)p[i] << (8 * i);
    return v;
}
static int zs_highbit(unsigned v) {              /* 最高置位下标；0 -> 0 */
    int r = 0;
    while (v >>= 1) r++;
    return r;
}

/* ==================== XXH64（zstd 的内容校验；只取低 32 位） ==================== */

#define ZS_P1 11400714785074694791ULL
#define ZS_P2 14029467366897019727ULL
#define ZS_P3  1609587929392839161ULL
#define ZS_P4  9650029242287828579ULL
#define ZS_P5  2870177450012600261ULL

static unsigned long long zs_rotl64(unsigned long long x, int r) {
    return (x << r) | (x >> (64 - r));
}
static unsigned long long zs_rd64(const unsigned char* p) {
    return (unsigned long long)zs_rd32(p) | ((unsigned long long)zs_rd32(p + 4) << 32);
}
static unsigned long long zs_round64(unsigned long long acc, unsigned long long in) {
    acc += in * ZS_P2;
    acc = zs_rotl64(acc, 31);
    return acc * ZS_P1;
}
static unsigned long long zs_merge64(unsigned long long acc, unsigned long long v) {
    acc ^= zs_round64(0, v);
    return acc * ZS_P1 + ZS_P4;
}
/* 返回低 32 位（zstd 的 Content_Checksum 就是 XXH64(0, 数据) 的低 32 位） */
static unsigned zs_xxh64_32(const unsigned char* p, int n, unsigned long long seed) {
    const unsigned char* end = p + n;
    unsigned long long h;
    if (n >= 32) {
        const unsigned char* lim = end - 32;
        unsigned long long v1 = seed + ZS_P1 + ZS_P2;
        unsigned long long v2 = seed + ZS_P2;
        unsigned long long v3 = seed;
        unsigned long long v4 = seed - ZS_P1;
        do {
            v1 = zs_round64(v1, zs_rd64(p)); p += 8;
            v2 = zs_round64(v2, zs_rd64(p)); p += 8;
            v3 = zs_round64(v3, zs_rd64(p)); p += 8;
            v4 = zs_round64(v4, zs_rd64(p)); p += 8;
        } while (p <= lim);
        h = zs_rotl64(v1, 1) + zs_rotl64(v2, 7) + zs_rotl64(v3, 12) + zs_rotl64(v4, 18);
        h = zs_merge64(h, v1);
        h = zs_merge64(h, v2);
        h = zs_merge64(h, v3);
        h = zs_merge64(h, v4);
    } else {
        h = seed + ZS_P5;
    }
    h += (unsigned long long)n;
    while (p + 8 <= end) {
        h ^= zs_round64(0, zs_rd64(p));
        h = zs_rotl64(h, 27) * ZS_P1 + ZS_P4;
        p += 8;
    }
    if (p + 4 <= end) {
        h ^= (unsigned long long)zs_rd32(p) * ZS_P1;
        h = zs_rotl64(h, 23) * ZS_P2 + ZS_P3;
        p += 4;
    }
    while (p < end) {
        h ^= (unsigned long long)(*p) * ZS_P5;
        h = zs_rotl64(h, 11) * ZS_P1;
        p++;
    }
    h ^= h >> 33;
    h *= ZS_P2;
    h ^= h >> 29;
    h *= ZS_P3;
    h ^= h >> 32;
    return (unsigned)(h & 0xFFFFFFFFu);
}

/* ==================== 位读取器 ①：前向 LSB-first ==================== */
/* FSE 表描述（"normalized counters"）是**正着写、低位数在先**的 —— 与压缩数据流相反。 */

struct ZsFwd {
    const unsigned char* p;
    int n;              /* 字节数 */
    int bitpos;         /* 已读位数 */
};

static void zs_fwd_init(struct ZsFwd* f, const unsigned char* p, int n) {
    f->p = p; f->n = n; f->bitpos = 0;
}
static unsigned zs_fwd_peek(struct ZsFwd* f, int nb) {
    unsigned v = 0;
    for (int i = 0; i < nb; i++) {
        const int bp = f->bitpos + i;
        unsigned bit = 0;
        if (bp >= 0 && bp < f->n * 8) bit = ((unsigned)f->p[bp >> 3] >> (bp & 7)) & 1u;
        v |= bit << i;
    }
    return v;
}
static unsigned zs_fwd_read(struct ZsFwd* f, int nb) {
    const unsigned v = zs_fwd_peek(f, nb);
    f->bitpos += nb;
    return v;
}
static void zs_fwd_skip(struct ZsFwd* f, int nb) { f->bitpos += nb; }

/* ==================== 位读取器 ②：反向 MSB-first ==================== */
/* 压缩数据流（Huffman 字面量 / FSE 序列）：第 0 位 = 最后一个字节的最高位，
 * 往后走 = 往缓冲区开头走。一次最多读 32 位（offset 的额外位最多 31 位）。 */

struct ZsBwd {
    const unsigned char* p;
    int n;              /* 字节数 */
    long long nbits;    /* = n*8 */
    long long pos;      /* 已消费位数 */
    int over;           /* 读到缓冲区开头之前 */
};

static void zs_bwd_init(struct ZsBwd* b, const unsigned char* p, int n) {
    b->p = p; b->n = n; b->nbits = (long long)n * 8; b->pos = 0; b->over = 0;
}
/* ★ zstd 的**数据位流**以一个 stop bit（值 1）收尾，它上面还有 0 填充。
 *   解码器从最后一个字节的**最高位**往下读，所以真数据从 stop bit 下面开始。
 *   不跳它 = 整体错位（实测踩过：200 个 0 的块读出 llc=0/ofc=8/mlc=0）。
 *   对应参考实现 BIT_initDStream 的 bitsConsumed = 8 - highbit32(lastByte)。 */
static int zs_bwd_init_stream(struct ZsBwd* b, const unsigned char* p, int n) {
    unsigned last;
    zs_bwd_init(b, p, n);
    if (n <= 0) { b->over = 1; return -1; }
    last = p[n - 1];
    if (last == 0) { b->over = 1; return -1; }   /* 没有 stop bit = 流坏了 */
    b->pos = 8 - zs_highbit(last);
    return 0;
}
/* 看 nb 位（不前进）。越界部分按 0 补齐。 */
static unsigned zs_bwd_peek(struct ZsBwd* b, int nb) {
    unsigned long long v = 0;
    const long long byte_of = b->pos >> 3;
    const int bit_of = (int)(b->pos & 7);
    if (nb <= 0) return 0;
    if (b->pos + nb > b->nbits) b->over = 1;
    for (int i = 0; i < 5; i++) {                 /* 5 字节 = 40 位 >= bit_of(7) + nb(32) */
        const long long idx = byte_of + i;
        unsigned c = 0;
        if (idx >= 0 && idx < b->n) c = b->p[b->n - 1 - idx];
        v = (v << 8) | c;
    }
    v >>= (40 - bit_of - nb);
    return (unsigned)(v & ((nb >= 32) ? 0xFFFFFFFFull : ((1ull << nb) - 1ull)));
}
static void zs_bwd_skip(struct ZsBwd* b, int nb) {
    if (nb <= 0) return;
    if (b->pos + nb > b->nbits) b->over = 1;
    b->pos += nb;
}
static unsigned zs_bwd_read(struct ZsBwd* b, int nb) {
    const unsigned v = zs_bwd_peek(b, nb);
    zs_bwd_skip(b, nb);
    return v;
}

/* ==================== 工作区（外置） ==================== */

struct ZsFse {
    unsigned short base;    /* 下一状态 = base + 读进来的 nb 位 */
    unsigned char  sym;
    unsigned char  nb;      /* 要读几位 */
};

struct ZsWork {
    struct ZsFse ll[1 << ZS_FSE_MAXLOG];
    struct ZsFse of[1 << ZS_FSE_MAXLOG];
    struct ZsFse ml[1 << ZS_FSE_MAXLOG];
    struct ZsFse hw[64];                /* Huffman 权重流用（tableLog <= 6） */
    unsigned short hsym[1 << ZS_HUF_MAXBITS];
    unsigned char  hlen[1 << ZS_HUF_MAXBITS];
    int huf_maxbits;                    /* 上一块的 Huffman Max_Number_of_Bits */
    int ll_log, of_log, ml_log;         /* 上一块的表日志（"repeat" 模式要用） */
    int have_tables;
};

#define VS_ZSTD_WORK_BYTES ((int)sizeof(struct ZsWork))

/* ==================== FSE 表构建 ==================== */

static unsigned zs_step(unsigned table_size) {
    return (table_size >> 1) + (table_size >> 3) + 3;
}

/* 由规范化计数建解码表。成功 0 / 失败 -1。 */
static int zs_build_fse(struct ZsFse* t, int table_log, const short* norm, int nsym) {
    const unsigned size = 1u << table_log;
    const unsigned mask = size - 1;
    const unsigned step = zs_step(size);
    unsigned char syms[1024];
    unsigned next[512];
    unsigned high, pos, u;
    int s, i;

    if (table_log < 5 || table_log > ZS_FSE_MAXLOG) return -1;
    if (nsym <= 0 || nsym > 512) return -1;

    /* ① 低概率符号（-1）从表尾往前占位 */
    high = size - 1;
    for (s = 0; s < nsym; s++) {
        if (norm[s] == -1) {
            syms[high--] = (unsigned char)s;
            next[s] = 1;
        } else if (norm[s] < -1) {
            return -1;
        } else {
            next[s] = (unsigned)norm[s];
        }
    }
    /* ② 按 step 散布其余符号 */
    pos = 0;
    for (s = 0; s < nsym; s++) {
        const int n = norm[s];
        if (n <= 0) continue;
        for (i = 0; i < n; i++) {
            syms[pos] = (unsigned char)s;
            pos = (pos + step) & mask;
            while (pos > high) pos = (pos + step) & mask;
        }
    }
    if (pos != 0) return -1;                /* 散布没回到起点 = 计数之和不等于 2^log */
    /* ③ 由符号位置反推状态转移 */
    for (u = 0; u < size; u++) {
        const unsigned sym = syms[u];
        const unsigned ns = next[sym]++;
        int nb = 0;
        unsigned v = ns;
        while (v >>= 1) nb++;
        t[u].sym  = (unsigned char)sym;
        t[u].nb   = (unsigned char)(table_log - nb);
        t[u].base = (unsigned short)((ns << (table_log - nb)) - size);
    }
    return 0;
}

/* RLE 模式：整张表只有一个符号，且**不读任何位**（tableLog = 0） */
static void zs_fse_rle(struct ZsFse* t, int sym) {
    t[0].sym = (unsigned char)sym;
    t[0].nb = 0;
    t[0].base = 0;
}

/* ==================== FSE 表描述（前向位流） ==================== */

/* 解一段 normalized counters（RFC 8878 §3.1.1.3.2.1）。
 * 成功返回消耗字节数（>0）；失败返回 <=0。 */
static int zs_read_ncount(struct ZsFwd* f, short* norm, int nsym_max, int* out_log, int* out_nsym) {
    unsigned nb, threshold;
    int remaining, ch = 0, previous0 = 0;
    const int start_bit = f->bitpos;

    for (int i = 0; i < nsym_max; i++) norm[i] = 0;

    nb = zs_fwd_read(f, 4) + 5;                       /* Accuracy_Log = 5..20 */
    if (nb > (unsigned)ZS_FSE_MAXLOG) return 0;        /* 本实现的表上限 */
    *out_log = (int)nb;
    remaining = (int)((1u << nb) + 1);
    threshold = 1u << nb;
    nb++;

    while (remaining > 1 && ch < nsym_max) {
        if (previous0) {                              /* 连续 0 的游程 */
            unsigned n0 = (unsigned)ch;
            while (zs_fwd_peek(f, 16) == 0xFFFFu) { n0 += 24; zs_fwd_skip(f, 16); }
            while (zs_fwd_peek(f, 2) == 3u) { n0 += 3; zs_fwd_skip(f, 2); }
            n0 += zs_fwd_read(f, 2);
            if ((int)n0 >= nsym_max) return 0;
            while ((unsigned)ch < n0) norm[ch++] = 0;
            previous0 = 0;
        }
        {
            const int mx = (int)((2 * threshold - 1) - (unsigned)remaining);
            int count;
            if ((int)zs_fwd_peek(f, (int)(nb - 1)) < mx) {
                count = (int)zs_fwd_read(f, (int)(nb - 1));
            } else {
                count = (int)zs_fwd_read(f, (int)nb);
                if (count >= (int)threshold) count -= mx;
            }
            count--;                                  /* 归一化里多出来的 1 格 */
            remaining -= (count < 0) ? -count : count;
            norm[ch++] = (short)count;
            previous0 = (count == 0);
            while (remaining < (int)threshold) { nb--; threshold >>= 1; }
        }
    }
    if (remaining != 1) return 0;
    *out_nsym = ch;
    return (f->bitpos - start_bit + 7) / 8;
}

/* ==================== Huffman ==================== */

/* 权重表 -> 解码表。ns = **存储下来的**权重个数，最后一个符号的权重按规范推导
 * （sum(2^(w-1)) 必须补足到 2^Max_Number_of_Bits）。成功 0 / 失败 -1。 */
static int zs_build_huf(struct ZsWork* w, const unsigned char* wt, int ns, int* out_maxbits) {
    unsigned char wv[258];
    unsigned total = 0;
    int i, maxbits, nsym;

    if (ns < 1 || ns > 256) return -1;
    for (i = 0; i < ns; i++) {
        if (wt[i] > ZS_HUF_MAXBITS) return -1;
        wv[i] = wt[i];
        if (wt[i]) total += 1u << (wt[i] - 1);
    }
    if (total == 0) return -1;
    maxbits = zs_highbit(total) + 1;
    if (maxbits > ZS_HUF_MAXBITS) return -1;
    {
        const unsigned rest = (1u << maxbits) - total;
        if (rest == 0) return -1;
        wv[ns] = (unsigned char)(zs_highbit(rest) + 1);
        if (wv[ns] == 0 || wv[ns] > ZS_HUF_MAXBITS) return -1;
    }
    nsym = ns + 1;

    for (i = 0; i < nsym; i++) {
        if (wv[i] > maxbits) return -1;
    }

    /* 官方（Conversion from weights to Huffman prefix codes）：
     *   Number_of_Bits = maxbits + 1 - Weight；
     *   符号按 Weight 升序（= 码长降序）、同 Weight 内按符号升序，
     *   依次占用**连续的** 2^(Weight-1) 个解码表项。
     *   这正是位流 MSB-first 下的正确布局（不是经典 canonical 的短码优先）。 */
    for (i = 0; i < (1 << ZS_HUF_MAXBITS); i++) { w->hlen[i] = 0; w->hsym[i] = 0; }
    {
        unsigned at = 0;
        const unsigned lim = 1u << maxbits;
        for (int wt2 = 1; wt2 <= maxbits; wt2++) {
            const int L = maxbits + 1 - wt2;
            const unsigned span = 1u << (wt2 - 1);
            for (i = 0; i < nsym; i++) {
                unsigned k;
                if ((int)wv[i] != wt2) continue;
                if (at + span > lim) return -1;
                for (k = 0; k < span; k++) {
                    w->hsym[at + k] = (unsigned short)i;
                    w->hlen[at + k] = (unsigned char)L;
                }
                at += span;
            }
        }
        if (at != lim) return -1;      /* 权重之和必须正好切满 2^Max_Number_of_Bits */
    }
    *out_maxbits = maxbits;
    return 0;
}

/* 解 Huffman 树描述（1 字节头 + 4 位权重，或 FSE 压缩的权重）。成功 0 / 失败 -1。 */
static int zs_read_huf_tree(struct ZsWork* w, const unsigned char* p, int n,
                            int* used, int* out_maxbits) {
    unsigned char weights[258];
    if (n < 1) return -1;
    if (p[0] < 128) {                               /* ★ 官方：< 128 = **FSE 压缩**的权重序列，长度 = p[0] */
        const int sz = (int)p[0];
        struct ZsFwd f;
        struct ZsBwd b;
        short norm[512];
        int tlog = 0, nsym = 0, csz, produced = 0;
        unsigned s1, s2;
        struct ZsFse* d;

        if (sz <= 0 || sz + 1 > n) return -1;   /* 长度字节 + 至少 1 字节位流 */
        zs_fwd_init(&f, p + 1, sz);
        csz = zs_read_ncount(&f, norm, 512, &tlog, &nsym);
        if (csz <= 0 || csz >= sz) return -1;
        if (tlog > 6) return -1;                    /* HUF_readStats 的硬件上限 */
        if (zs_build_fse(w->hw, tlog, norm, nsym) != 0) return -1;
        d = w->hw;
        if (zs_bwd_init_stream(&b, p + 1 + csz, sz - csz) != 0) return -1;
        s1 = (unsigned)zs_bwd_read(&b, tlog);
        s2 = (unsigned)zs_bwd_read(&b, tlog);
        for (;;) {
            const unsigned m = (1u << tlog) - 1u;
            if (produced >= 256) return -1;
            weights[produced++] = d[s1 & m].sym;
            s1 = d[s1 & m].base + (unsigned)zs_bwd_read(&b, d[s1 & m].nb);
            if (b.over) {
                if (produced >= 256) return -1;
                weights[produced++] = d[s2 & m].sym;
                break;
            }
            if (produced >= 256) return -1;
            weights[produced++] = d[s2 & m].sym;
            s2 = d[s2 & m].base + (unsigned)zs_bwd_read(&b, d[s2 & m].nb);
            if (b.over) {
                if (produced >= 256) return -1;
                weights[produced++] = d[s1 & m].sym;
                break;
            }
        }
        *used = sz + 1;
        return zs_build_huf(w, weights, produced, out_maxbits);
    }
    {
        /* ★ 官方：>= 128 = **4 位直存**，Number_of_Weights = p[0] - 127 */
        const int ns = (int)p[0] - 127;
        const int nb = (ns + 1) / 2;
        if (ns < 1 || 1 + nb > n) return -1;
        for (int i = 0; i < ns; i++) {
            const unsigned byte = p[1 + (i >> 1)];
            weights[i] = (unsigned char)((i & 1) ? (byte & 0xFu) : (byte >> 4));
        }
        *used = 1 + nb;
        return zs_build_huf(w, weights, ns, out_maxbits);
    }
}

/* 解一条 Huffman 流，正好产出 want 字节。成功返回 want / 失败 -1。 */
static int zs_huf_stream(struct ZsWork* w, int maxbits, const unsigned char* p, int n,
                         unsigned char* out, int want) {
    struct ZsBwd b;
    if (want == 0) return 0;
    if (maxbits <= 0 || maxbits > ZS_HUF_MAXBITS) return -1;
    if (zs_bwd_init_stream(&b, p, n) != 0) return -1;
    for (int i = 0; i < want; i++) {
        const unsigned idx = zs_bwd_peek(&b, maxbits);
        const unsigned L = w->hlen[idx];
        if (L == 0) return -1;
        out[i] = (unsigned char)w->hsym[idx];
        zs_bwd_skip(&b, (int)L);
    }
    return want;
}

/* ==================== 主解码 ==================== */

int vs_zstd_decode(const unsigned char* src, int src_len,
                   unsigned char* dst, int dst_cap,
                   unsigned char* lit, int lit_cap,
                   void* work, int* out_len, const char** why, int* at_off)
{
    struct ZsWork* w = (struct ZsWork*)work;
    int ip = 0;
    long long out = 0;

    if (!src || !dst || !lit || !work || !out_len) return -1;
    *out_len = 0;
    if (why) *why = "none";
    if (at_off) *at_off = 0;
    if (lit_cap < 128 * 1024) { if (why) *why = "literals-buffer-too-small"; return -1; }

    w->ll_log = w->of_log = w->ml_log = 0;
    w->have_tables = 0;

    for (;;) {
        unsigned magic, fhd;
        long long fcs = -1;
        unsigned rep[3];

        if (ip == src_len) break;
        if (ip + 4 > src_len) { if (why) *why = "frame-header-truncated"; return -1; }
        magic = zs_rd32(src + ip);
        if ((magic & 0xFFFFFFF0u) == 0x184D2A50u) {         /* skippable 帧 */
            if (ip + 8 > src_len) { if (why) *why = "skippable-truncated"; return -1; }
            ip += 8 + (int)zs_rd32(src + ip + 4);
            if (ip > src_len) { if (why) *why = "skippable-truncated"; return -1; }
            continue;
        }
        if (magic != 0xFD2FB528u) { if (why) *why = "bad-magic"; if (at_off) *at_off = ip; return -1; }
        ip += 4;
        if (ip >= src_len) { if (why) *why = "frame-header-truncated"; return -1; }
        fhd = src[ip++];
        if (fhd & 0x08) { if (why) *why = "reserved-bit-set"; return -1; }
        rep[0] = 1; rep[1] = 4; rep[2] = 8;                 /* 每帧重置 repeat offset */

        {
            const int fcs_flag = (int)(fhd >> 6) & 3;
            const int single = (int)(fhd >> 5) & 1;
            const int did_flag = (int)(fhd & 3);
            const int did_size = (did_flag == 0) ? 0 : (did_flag == 1) ? 1 : (did_flag == 2) ? 2 : 4;
            int fcs_size;
            if (did_size != 0) { if (why) *why = "dictionary-unsupported"; return -1; }
            if (!single) {
                if (ip >= src_len) { if (why) *why = "frame-header-truncated"; return -1; }
                ip++;                                       /* Window_Descriptor：不分配环形窗口 */
            }
            fcs_size = (fcs_flag == 0) ? (single ? 1 : 0)
                     : (fcs_flag == 1) ? 2 : (fcs_flag == 2) ? 4 : 8;
            if (fcs_size) {
                if (ip + fcs_size > src_len) { if (why) *why = "frame-header-truncated"; return -1; }
                if (fcs_size == 1) fcs = src[ip];
                else if (fcs_size == 2) fcs = (long long)zs_rd16(src + ip) + 256;
                else if (fcs_size == 4) fcs = (long long)zs_rd32(src + ip);
                else fcs = (long long)zs_rdle(src + ip, 8);
                ip += fcs_size;
                if (fcs < 0 || fcs > (long long)dst_cap) {
                    if (why) *why = "output-too-small-for-frame";
                    return -1;
                }
            }

            /* ---- 块循环 ---- */
            for (;;) {
                unsigned bh;
                int last, btype, bsize;
                if (ip + 3 > src_len) { if (why) *why = "block-header-truncated"; if (at_off) *at_off = ip; return -1; }
                bh = zs_rd24(src + ip); ip += 3;
                last = (int)(bh & 1u);
                btype = (int)((bh >> 1) & 3u);
                bsize = (int)(bh >> 3);
                if (btype == 3) { if (why) *why = "reserved-block-type"; if (at_off) *at_off = ip - 3; return -1; }
                ZTR("[blk] last=%d type=%d size=%d ip=%d out=%lld\n", last, btype, bsize, ip, out);

                if (btype == 0) {                           /* Raw */
                    if (ip + bsize > src_len) { if (why) *why = "raw-block-truncated"; return -1; }
                    if (out + bsize > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }
                    for (int i = 0; i < bsize; i++) dst[out + i] = src[ip + i];
                    out += bsize; ip += bsize;
                } else if (btype == 1) {                    /* RLE */
                    if (ip + 1 > src_len) { if (why) *why = "rle-block-truncated"; return -1; }
                    if (out + bsize > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }
                    for (int i = 0; i < bsize; i++) dst[out + i] = src[ip];
                    out += bsize; ip += 1;
                } else {                                    /* Compressed */
                    const int bend = ip + bsize;
                    int lpos = ip, ltype, hdr_size, lreg = 0, lcomp = 0;
                    int lstart, nseq = 0;
                    long long lit_pos = 0;
                    int ll_log = 0, of_log = 0, ml_log = 0;
                    int mb = 0;
                    short norm[512];
                    int nsym = 0;

                    if (bend > src_len) { if (why) *why = "block-truncated"; if (at_off) *at_off = ip; return -1; }
                    if (bsize < 1) { if (why) *why = "block-empty"; return -1; }

                    /* ① 字面量段头（RFC 8878 §3.1.1.3.1.1） */
                    ltype = src[lpos] & 3;
                    if (ltype == 0 || ltype == 1) {          /* Raw / RLE：Size_Format 1 位 */
                        if (((src[lpos] >> 2) & 1) == 0) {
                            lreg = src[lpos] >> 3;           /* 5 位 */
                            hdr_size = 1;
                        } else {
                            if (lpos + 2 > bend) { if (why) *why = "literals-header"; return -1; }
                            lreg = (int)((zs_rdle(src + lpos, 2) >> 4) & 0xFFF);   /* 12 位 */
                            hdr_size = 2;
                        }
                        lcomp = (ltype == 1) ? 1 : lreg;
                    } else {                                /* Compressed / Treeless：Size_Format 2 位 */
                        const int sf = (int)((src[lpos] >> 2) & 3);
                        const int bits = (sf <= 1) ? 10 : (sf == 2) ? 14 : 18;
                        unsigned long long v;
                        hdr_size = (sf <= 1) ? 3 : (sf == 2) ? 4 : 5;
                        if (lpos + hdr_size > bend) { if (why) *why = "literals-header"; return -1; }
                        v = zs_rdle(src + lpos, hdr_size);
                        lreg  = (int)((v >> 4) & ((1ull << bits) - 1ull));
                        lcomp = (int)((v >> (4 + bits)) & ((1ull << bits) - 1ull));
                        /* ★ 关键：Size_Format 0 才是**单流**；1/2/3 都是 4 流。
                         *   （zstd 编码器写的是 lhc = type + (!singleStream)<<2 + …） */
                        if (sf == 0) hdr_size |= 0x100;      /* 标记"单流"，下面用它分流 */
                    }
                    lstart = lpos + (hdr_size & 0xFF);
                    ZTR("[lit] ltype=%d hdr=%d reg=%d comp=%d lstart=%d\n",
                        ltype, hdr_size & 0xFF, lreg, lcomp, lstart);
                    if (lstart + lcomp > bend) { if (why) *why = "literals-truncated"; if (at_off) *at_off = lpos; return -1; }
                    if (lreg < 0 || lreg > lit_cap) { if (why) *why = "literals-block-too-big"; return -1; }
                    if (out + lreg > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }

                    /* ② 解出字面量（落进独立的 lit 缓冲） */
                    if (ltype == 0) {
                        for (int i = 0; i < lreg; i++) lit[i] = src[lstart + i];
                    } else if (ltype == 1) {
                        if (lcomp != 1) { if (why) *why = "rle-literals-size"; return -1; }
                        for (int i = 0; i < lreg; i++) lit[i] = src[lstart];
                    } else {
                        int used = 0;
                        if (ltype == 2) {                    /* 自带 Huffman 树 */
                            if (zs_read_huf_tree(w, src + lstart, lcomp, &used, &mb) != 0) {
                                if (why) *why = "huffman-tree";
                                if (at_off) *at_off = lstart;
                                return -1;
                            }
                            lstart += used;
                            lcomp -= used;
                        } else {                             /* Treeless：沿用上一块的表 */
                            if (w->have_tables == 0) { if (why) *why = "treeless-without-table"; return -1; }
                            mb = w->huf_maxbits;
                        }
                        if (lreg > 0) {
                            if (hdr_size & 0x100) {          /* 单流 */
                                if (zs_huf_stream(w, mb, src + lstart, lcomp, lit, lreg) != lreg) {
                                    if (why) *why = "literals-huffman";
                                    if (at_off) *at_off = lstart;
                                    return -1;
                                }
                            } else {                         /* 4 流 */
                                int ssz[4], part, off4, p0, produced = 0;
                                if (lcomp < 6) { if (why) *why = "literals-4streams-header"; return -1; }
                                ssz[0] = (int)zs_rd16(src + lstart);
                                ssz[1] = (int)zs_rd16(src + lstart + 2);
                                ssz[2] = (int)zs_rd16(src + lstart + 4);
                                ssz[3] = lcomp - 6 - ssz[0] - ssz[1] - ssz[2];
                                if (ssz[3] < 0) { if (why) *why = "literals-4streams-size"; return -1; }
                                part = (lreg + 3) / 4;
                                off4 = part * 3;
                                p0 = lstart + 6;
                                {
                                    int want4[4];
                                    want4[0] = part; want4[1] = part; want4[2] = part;
                                    want4[3] = lreg - off4;
                                    if (want4[3] < 0) { if (why) *why = "literals-4streams-split"; return -1; }
                                    for (int s2 = 0; s2 < 4; s2++) {
                                        if (zs_huf_stream(w, mb, src + p0, ssz[s2], lit + produced, want4[s2]) != want4[s2]) {
                                            if (why) *why = "literals-huffman";
                                            if (at_off) *at_off = p0;
                                            return -1;
                                        }
                                        produced += want4[s2];
                                        p0 += ssz[s2];
                                    }
                                }
                            }
                        }
                    }
                    if (ltype == 2) {                            /* 记住这一块的树，供 Treeless 复用 */
                        w->huf_maxbits = mb;
                        w->have_tables |= 2;
                    } else if (ltype == 3) {
                        mb = w->huf_maxbits;
                    }

                    /* ③ 序列段 */
                    {
                        int sp = lstart + lcomp;
                        unsigned modes;
                        struct ZsBwd b;
                        unsigned sll, sof, sml;

                        if (sp >= bend) { if (why) *why = "sequences-missing"; return -1; }
                        {
                            const unsigned b0 = src[sp++];
                            if (b0 < 128) nseq = (int)b0;
                            else if (b0 < 255) {
                                if (sp >= bend) { if (why) *why = "sequences-truncated"; return -1; }
                                nseq = (int)((b0 - 128) << 8) + src[sp++];
                            } else {
                                if (sp + 2 > bend) { if (why) *why = "sequences-truncated"; return -1; }
                                nseq = 0x7F00 + (int)src[sp] + ((int)src[sp + 1] << 8);
                                sp += 2;
                            }
                        }
                        if (nseq > 0) {
                            if (sp >= bend) { if (why) *why = "sequences-truncated"; return -1; }
                            modes = src[sp++];
                            ZTR("[seq] n=%d modes=0x%02X (ll=%d of=%d ml=%d) sp=%d bend=%d\n",
                                nseq, modes, (modes >> 6) & 3, (modes >> 4) & 3, (modes >> 2) & 3, sp, bend);
                            if ((modes & 3) != 0) { if (why) *why = "sequences-mode-reserved"; return -1; }

                            /* ---- LL 表 ---- */
                            switch ((modes >> 6) & 3) {          /* ★ LL = bits 7-6 */
                            case 0:
                                ll_log = ZS_LL_LOG;
                                if (zs_build_fse(w->ll, ZS_LL_LOG, ZS_LL_NORM, 36) != 0) { if (why) *why = "fse-ll-predefined"; return -1; }
                                break;
                            case 1:
                                if (sp + 1 > bend) { if (why) *why = "sequences-truncated"; return -1; }
                                zs_fse_rle(w->ll, src[sp++]); ll_log = 0;
                                break;
                            case 2: {
                                struct ZsFwd f;
                                int nb;
                                zs_fwd_init(&f, src + sp, bend - sp);
                                nb = zs_read_ncount(&f, norm, 36, &ll_log, &nsym);
                                if (nb <= 0) { if (why) *why = "fse-ll-description"; if (at_off) *at_off = sp; return -1; }
                                if (zs_build_fse(w->ll, ll_log, norm, nsym) != 0) { if (why) *why = "fse-ll-table"; return -1; }
                                sp += nb;
                                break;
                            }
                            default:
                                if (!(w->have_tables & 4)) { if (why) *why = "fse-ll-repeat-without-table"; return -1; }
                                ll_log = w->ll_log;
                                break;
                            }
                            /* ---- OF 表 ---- */
                            switch ((modes >> 4) & 3) {          /* ★ OF = bits 5-4 */
                            case 0:
                                of_log = ZS_OF_LOG;
                                if (zs_build_fse(w->of, ZS_OF_LOG, ZS_OF_NORM, 32) != 0) { if (why) *why = "fse-of-predefined"; return -1; }
                                break;
                            case 1:
                                if (sp + 1 > bend) { if (why) *why = "sequences-truncated"; return -1; }
                                zs_fse_rle(w->of, src[sp++]); of_log = 0;
                                break;
                            case 2: {
                                struct ZsFwd f;
                                int nb;
                                zs_fwd_init(&f, src + sp, bend - sp);
                                nb = zs_read_ncount(&f, norm, 32, &of_log, &nsym);
                                if (nb <= 0) { if (why) *why = "fse-of-description"; return -1; }
                                if (zs_build_fse(w->of, of_log, norm, nsym) != 0) { if (why) *why = "fse-of-table"; return -1; }
                                sp += nb;
                                break;
                            }
                            default:
                                if (!(w->have_tables & 4)) { if (why) *why = "fse-of-repeat-without-table"; return -1; }
                                of_log = w->of_log;
                                break;
                            }
                            /* ---- ML 表 ---- */
                            switch ((modes >> 2) & 3) {          /* ★ ML = bits 3-2 */
                            case 0:
                                ml_log = ZS_ML_LOG;
                                if (zs_build_fse(w->ml, ZS_ML_LOG, ZS_ML_NORM, 53) != 0) { if (why) *why = "fse-ml-predefined"; return -1; }
                                break;
                            case 1:
                                if (sp + 1 > bend) { if (why) *why = "sequences-truncated"; return -1; }
                                zs_fse_rle(w->ml, src[sp++]); ml_log = 0;
                                break;
                            case 2: {
                                struct ZsFwd f;
                                int nb;
                                zs_fwd_init(&f, src + sp, bend - sp);
                                nb = zs_read_ncount(&f, norm, 53, &ml_log, &nsym);
                                if (nb <= 0) { if (why) *why = "fse-ml-description"; return -1; }
                                if (zs_build_fse(w->ml, ml_log, norm, nsym) != 0) { if (why) *why = "fse-ml-table"; return -1; }
                                sp += nb;
                                break;
                            }
                            default:
                                if (!(w->have_tables & 4)) { if (why) *why = "fse-ml-repeat-without-table"; return -1; }
                                ml_log = w->ml_log;
                                break;
                            }
                            /* 本块的表留在 work 里，供下一块 "repeat" 使用 */
                            ZTR("[fse] ll_log=%d of_log=%d ml_log=%d nsym=%d\n", ll_log, of_log, ml_log, nsym);
                            {
                                int q;
                                ZTR("[tbl-ll]");
                                for (q = 0; q < 32; q++) ZTR(" %u", (unsigned)w->ll[q].sym);
                                ZTR("\n[tbl-ml]");
                                for (q = 0; q < 32; q++) ZTR(" %u", (unsigned)w->ml[q].sym);
                                ZTR("\n[tbl-of]");
                                for (q = 0; q < 32; q++) ZTR(" %u", (unsigned)w->of[q].sym);
                                ZTR("\n");
                            }
                            {
                                int q;
                                ZTR("[norm]");
                                for (q = 0; q < nsym && q < 12; q++) ZTR(" %d", (int)norm[q]);
                                ZTR("\n");
                            }
                            w->ll_log = ll_log; w->of_log = of_log; w->ml_log = ml_log;
                            w->have_tables |= 4;

                            /* ---- 序列位流 ---- */
                            if (zs_bwd_init_stream(&b, src + sp, bend - sp) != 0) {
                                if (why) *why = "sequences-no-stopbit";
                                return -1;
                            }
                            sll = zs_bwd_read(&b, ll_log);
                            sof = zs_bwd_read(&b, of_log);
                            sml = zs_bwd_read(&b, ml_log);
                            if (b.over) { if (why) *why = "sequences-init"; return -1; }
                            ZTR("[init] sll=%u sof=%u sml=%u bits=%lld\n",
                                sll, sof, sml, b.nbits);

                            for (int k = 0; k < nseq; k++) {
                                const unsigned llm = (1u << ll_log) - 1u;
                                const unsigned ofm = (1u << of_log) - 1u;
                                const unsigned mlm = (1u << ml_log) - 1u;
                                const struct ZsFse ell = w->ll[sll & llm];
                                const struct ZsFse eof = w->of[sof & ofm];
                                const struct ZsFse eml = w->ml[sml & mlm];
                                const unsigned llc = ell.sym, ofc = eof.sym, mlc = eml.sym;
                                unsigned llv, mlv, ofv, offsetv;
                                if (llc > ZS_LL_MAX || mlc > ZS_ML_MAX || ofc > (unsigned)ZS_OF_MAX) {
                                    if (why) *why = "sequence-code-range";
                                    return -1;
                                }
                                /* 额外位读取顺序（与参考实现一致）：Offset -> Match_Length -> Literals_Length */
                                ofv = zs_bwd_read(&b, (int)ofc);
                                mlv = zs_bwd_read(&b, ZS_ML_BITS[mlc]);
                                llv = zs_bwd_read(&b, ZS_LL_BITS[llc]);
                                llv += ZS_LL_BASE[llc];
                                mlv += ZS_ML_BASE[mlc];
                                offsetv = (1u << ofc) + ofv;
                                if (b.over && k + 1 < nseq) { if (why) *why = "sequences-underflow"; return -1; }
                                if (k < 8) ZTR("[s%d] llc=%u ofc=%u mlc=%u -> ll=%u ofv=%u ml=%u pos=%lld\n",
                                               k, llc, ofc, mlc, llv, offsetv, mlv, b.pos);
                                offsetv = (1u << ofc) + ofv;
                                if (b.over && k + 1 < nseq) { if (why) *why = "sequences-underflow"; return -1; }
                                /* repeat offset（官方格式文档 Repeat offsets）：
                                 *   ll != 0: ov 1 -> rep1, 2 -> rep2, 3 -> rep3
                                 *   ll == 0: ov 1 -> rep2, 2 -> rep3, 3 -> **rep1 - 1 字节**
                                 *   同一式子：repCode = ov - 1 + (ll==0)                          */
                                if (offsetv > 3) {
                                    const unsigned real = offsetv - 3;
                                    rep[2] = rep[1]; rep[1] = rep[0]; rep[0] = real;
                                } else {
                                    const unsigned repcode = offsetv - 1u + (llc == 0u ? 1u : 0u);
                                    if (repcode > 0u) {
                                        const unsigned real = (repcode == 3u) ? (rep[0] - 1u) : rep[repcode];
                                        rep[2] = (repcode >= 2u) ? rep[1] : rep[2];
                                        rep[1] = rep[0];
                                        rep[0] = real;
                                    }
                                }
                                /* 状态推进（顺序与参考实现一致：LL -> ML -> OF） */
                                sll = ell.base + zs_bwd_read(&b, ell.nb);
                                sml = eml.base + zs_bwd_read(&b, eml.nb);
                                sof = eof.base + zs_bwd_read(&b, eof.nb);

                                /* 执行：先搬 ll 个字面量，再从 rep[0] 处拷 ml 字节 */
                                if (lit_pos + (long long)llv > (long long)lreg) { if (why) *why = "literal-overrun"; return -1; }
                                if (out + (long long)llv > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }
                                for (unsigned i2 = 0; i2 < llv; i2++) dst[out + i2] = lit[lit_pos + i2];
                                out += llv; lit_pos += llv;
                                {
                                    const unsigned off = rep[0];
                                    if (off == 0 || (long long)off > out) { if (why) *why = "match-offset-out-of-range"; return -1; }
                                    if (out + (long long)mlv > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }
                                    for (unsigned i2 = 0; i2 < mlv; i2++) dst[out + i2] = dst[out - off + i2];
                                    out += mlv;
                                }
                            }
                        }
                        /* 剩下的字面量原样搬完 */
                        {
                            const long long rest = (long long)lreg - lit_pos;
                            if (rest < 0) { if (why) *why = "literal-overrun"; return -1; }
                            if (out + rest > (long long)dst_cap) { if (why) *why = "output-overflow"; return -1; }
                            for (long long i2 = 0; i2 < rest; i2++) dst[out + i2] = lit[lit_pos + i2];
                            out += rest;
                        }
                    }
                    ip = bend;
                }
                if (last) break;
            }
            /* 内容校验（可选字段，有就真验） */
            if (fhd & 0x04) {
                unsigned want, got;
                if (ip + 4 > src_len) { if (why) *why = "checksum-truncated"; return -1; }
                want = zs_rd32(src + ip);
                got = zs_xxh64_32(dst, (int)out, 0);
                ip += 4;
                if (want != got) { if (why) *why = "checksum-mismatch"; if (at_off) *at_off = ip - 4; return -1; }
            }
            if (fcs >= 0 && out != fcs) { if (why) *why = "content-size-mismatch"; return -1; }
        }
    }
    *out_len = (int)out;
    return 0;
}
