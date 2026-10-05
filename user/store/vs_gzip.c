/* vs_gzip.c - ★ .deb 的 control.tar.gz / data.tar.gz 解压（DEFLATE 全解）
 *
 * 实现来源（**同一实现**，不是另起一套）：user/gzip/gzip.c 的 inflate 那一段
 * （struct Bin / bits_get / huff_build / huff_decode / fixed_tables / inflate_block /
 *  gunzip_member），逐段抄过来，只改了三处：
 *   ① 名字加 vs_ 前缀（避免与 /bin/gzip 同名符号混淆）；
 *   ② 错误码统一成 0 = 成功 / 2 = 坏流（与 gzip.c 同口径）；
 *   ③ 去掉 CRC 校验以外的 CLI 逻辑（本处只要"解出来的字节"）。
 * 为什么抄而不是链 /bin/gzip：本程序是**静态 ELF**，不能 exec 一个"库"；gzip 是独立程序。
 * 如实边界（与 gzip.c 完全一致）：FDICT（预设字典）不支持、多成员只解**第一个**成员、
 * BTYPE=11 保留值拒绝、输出缓冲不够 = 坏流（有界，不会越界写）。
 *
 * ★ xz 不在本文件里：/bin/vpkg 对 .xz 压缩的 deb **明确拒绝**（见 vs_pkg.c 的 VS_E_XZ）。
 */
#include "vs.h"

typedef uint8_t  u8;
typedef uint32_t u32;

#define VS_GZ_MAGIC0  0x1F
#define VS_GZ_MAGIC1  0x8B
#define VS_GZ_DEFLATE 8

static const u32 LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const int LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u32 DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,
                                  3073,4097,6145,8193,12289,16385,24577 };
static const int DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

struct Bin { const u8* in; u32 len; u32 pos; u32 bitbuf; int bitcnt; int err; };

static int bits_get(struct Bin* s, int need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        s->bitbuf |= (u32)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    const int v = (int)(s->bitbuf & ((1u << need) - 1u));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

static void bits_align(struct Bin* s) { s->bitbuf = 0; s->bitcnt = 0; }

struct Huff { short count[16]; short sym[288]; };

static int huff_build(struct Huff* h, const u8* lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    short offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (short)i;
    return left;
}

static int huff_decode(struct Bin* s, const struct Huff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bits_get(s, 1);
        if (s->err) return -1;
        const int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static void fixed_tables(struct Huff* lit, struct Huff* dist) {
    u8 l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    (void)huff_build(lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    (void)huff_build(dist, l, 30);
}

/* 解一个 block；返回 0 = 还有 block，1 = 最后一个 block，2 = 坏流 */
static int inflate_block(struct Bin* s, u8* out, u32 outcap, u32* outlen) {
    u32 o = *outlen;
    const int final = bits_get(s, 1);
    const int type = bits_get(s, 2);
    if (s->err) return 2;
    if (type == 0) {
        bits_align(s);
        if (s->pos + 4u > s->len) return 2;
        const u32 blen = (u32)s->in[s->pos] | ((u32)s->in[s->pos + 1] << 8);
        const u32 nlen = (u32)s->in[s->pos + 2] | ((u32)s->in[s->pos + 3] << 8);
        s->pos += 4;
        if ((blen ^ 0xFFFFu) != nlen) return 2;
        if (s->pos + blen > s->len || blen > outcap - o) return 2;
        for (u32 k = 0; k < blen; k++) out[o++] = s->in[s->pos++];
    } else if (type == 1 || type == 2) {
        struct Huff lit, dist;
        if (type == 1) {
            fixed_tables(&lit, &dist);
        } else {
            const int hlit = bits_get(s, 5) + 257;
            const int hdist = bits_get(s, 5) + 1;
            const int hclen = bits_get(s, 4) + 4;
            if (s->err || hlit > 286 || hdist > 30) return 2;
            static const u8 ORD[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            u8 cl[19];
            for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hclen; i++) cl[ORD[i]] = (u8)bits_get(s, 3);
            if (s->err) return 2;
            struct Huff clh;
            if (huff_build(&clh, cl, 19) < 0) return 2;
            u8 lens[320];
            int n = 0;
            while (n < hlit + hdist) {
                const int sym = huff_decode(s, &clh);
                if (sym < 0 || s->err) return 2;
                if (sym < 16) {
                    lens[n++] = (u8)sym;
                } else if (sym == 16) {
                    if (n == 0) return 2;
                    int rep = bits_get(s, 2) + 3;
                    const u8 prev = lens[n - 1];
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = prev; }
                } else if (sym == 17) {
                    int rep = bits_get(s, 3) + 3;
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = 0; }
                } else {
                    int rep = bits_get(s, 7) + 11;
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = 0; }
                }
                if (s->err) return 2;
            }
            if (lens[256] == 0) return 2;
            if (huff_build(&lit, lens, hlit) < 0) return 2;
            if (huff_build(&dist, lens + hlit, hdist) < 0) return 2;
        }
        for (;;) {
            const int sym = huff_decode(s, &lit);
            if (sym < 0 || s->err) return 2;
            if (sym < 256) {
                if (o >= outcap) return 2;
                out[o++] = (u8)sym;
                continue;
            }
            if (sym == 256) break;
            const int li = sym - 257;
            if (li >= 29) return 2;
            const u32 len = (u32)LEN_BASE[li] + (u32)bits_get(s, LEN_EXTRA[li]);
            const int dsym = huff_decode(s, &dist);
            if (dsym < 0 || dsym >= 30 || s->err) return 2;
            const u32 d = (u32)DIST_BASE[dsym] + (u32)bits_get(s, DIST_EXTRA[dsym]);
            if (s->err || d == 0 || d > o) return 2;
            if (len > outcap - o) return 2;
            for (u32 k = 0; k < len; k++) { out[o] = out[o - d]; o++; }
        }
    } else {
        return 2;
    }
    *outlen = o;
    return final ? 1 : 0;
}

/* ==================== CRC32（gzip 尾部的校验；与内核/zlib 同口径） ==================== */
static unsigned int g_crc_tab[256];
static int g_crc_ready;

static void crc_init(void) {
    for (unsigned int i = 0; i < 256; i++) {
        unsigned int c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
    g_crc_ready = 1;
}

unsigned int vs_crc32(const unsigned char* p, int n) {
    if (!g_crc_ready) crc_init();
    unsigned int c = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) c = g_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* 从一个 gzip 成员解到 out（outcap 上限）；返回 0 = 成功 / 2 = 坏流 */
static int gunzip_at(const u8* in, u32 n, u32 off, u8* out, u32 outcap, int* out_len) {
    if (off >= n) return 2;
    struct Bin s;
    s.in = in; s.len = n; s.pos = off; s.bitbuf = 0; s.bitcnt = 0; s.err = 0;
    u32 o = 0;
    for (;;) {
        const int rc = inflate_block(&s, out, outcap, &o);
        if (rc == 2) return 2;
        if (rc == 1) break;
    }
    if (s.err) return 2;
    if (s.pos + 8u > n) return 2;                     /* 尾部必须还在（ISIZE/CRC 都要读得到） */
    const u32 crc = (u32)in[s.pos] | ((u32)in[s.pos + 1] << 8) | ((u32)in[s.pos + 2] << 16) |
                    ((u32)in[s.pos + 3] << 24);
    const u32 isize = (u32)in[s.pos + 4] | ((u32)in[s.pos + 5] << 8) | ((u32)in[s.pos + 6] << 16) |
                      ((u32)in[s.pos + 7] << 24);
    if (isize != o) return 2;
    if (vs_crc32(out, (int)o) != crc) return 2;
    *out_len = (int)o;
    return 0;
}

int vs_gunzip(const unsigned char* in, int n, unsigned char* out, int outcap, int* out_len) {
    if (n < 18) return 2;
    if (in[0] != VS_GZ_MAGIC0 || in[1] != VS_GZ_MAGIC1 || in[2] != VS_GZ_DEFLATE) return 2;
    const u8 flg = in[3];
    if (flg & 0xE0u) return 2;                        /* 保留位 */
    if (flg & 0x10u) return 2;                        /* FDICT：预设字典不支持（如实拒绝） */
    u32 off = 10u;
    if (flg & 0x04u) {                                /* FEXTRA */
        if ((u32)n < 12u) return 2;
        const u32 xlen = (u32)in[10] | ((u32)in[11] << 8);
        off = 12u + xlen;
        if (off + 8u > (u32)n) return 2;
    }
    if (flg & 0x08u) { while (off < (u32)n && in[off]) off++; off++; if (off >= (u32)n) return 2; }
    if (flg & 0x80u) { while (off < (u32)n && in[off]) off++; off++; if (off >= (u32)n) return 2; }
    if (flg & 0x02u) { off += 2; if (off > (u32)n) return 2; }
    return gunzip_at(in, (u32)n, off, out, (u32)outcap, out_len);
}
