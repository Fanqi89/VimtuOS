/* gzip.c - ★ A4-4c：用户态 gzip / gunzip（Ring 3，静态 ELF64，走内核主程序装载器直接跑）
 *
 * 为什么自足（不引 user/lib、不引 musl）：
 *   内核的主程序装载器要求 PT_LOAD 完整落在**用户窗口低 64 KiB**（4GiB..USER64_STACK_VA64），
 *   本工具要能被 `run /bin/gzip ...` 直接执行，所以映像（含 .bss）必须很小 —— 这里连
 *   head[]/prev[]（256 KB）与输入/输出缓冲都在运行期用 mmap(9) 拿，静态数据只有 CRC 表
 *   （1 KB）与几个小缓冲。系统调用全部内联汇编直接发（Linux x86_64 号段，与 shell/musl 同一条 ABI）。
 *
 * deflate 的取舍（**如实写明**，见 docs 的 A4-4 节）：
 *   * 压缩端只做 **fixed Huffman（BTYPE=01）+ stored（BTYPE=00）** 二选一：
 *       - LZ77 贪婪匹配（32 KiB 窗口 / 3..258 字节、hash 链 64 步上限）；
 *       - 单个 deflate block 覆盖整份输入（不做 dynamic Huffman、不做压缩等级、不做多线程）；
 *       - 若 fixed 编码结果不小于输入（不可压缩数据），退化成 stored block（每块 <= 65535 B）。
 *     不做 dynamic Huffman 的代价：文本上比 zlib -6 大一些，但**完全符合 RFC1951/1952**，
 *     宿主 Python 的 zlib/gzip 能逐字节解开（tests/gzip64_test.py 的互操作证据）。
 *   * 解压端是**完整 inflate**：stored / fixed / dynamic、多 block、32 KiB 回溯窗口、
 *     gzip 头（FEXTRA/FNAME/FCOMMENT/FHCRC 全部按长度跳过）、CRC32 + ISIZE 校验。
 *     不做：多成员（concat）gzip（第一个成员之后的字节会判错）、FDICT（字典 -> 明确拒绝）。
 *   * 内存模式：整份输入 + 整份输出都驻留内存（输入上限 4 MiB；超限明确报错而不是偷偷截断）。
 *
 * 用法（-d 或不带 -d 由 argv[0] 名字里的 "gunzip" 决定）：
 *     gzip   [-c] [-k] [-f] [-v] [-o OUT] FILE...        FILE -> FILE.gz（默认删源文件，-k 保留）
 *     gunzip [-c] [-k] [-f] [-v] [-t] [-o OUT] FILE.gz   FILE.gz -> FILE
 *     gunzip -t FILE.gz                                  只校验（打印 OK crc32=0x… size=… 到 stderr）
 *     -o OUT  解压/压缩结果写到 OUT（本工具的扩展：测试里做往返比对用）
 * 状态/统计一律写 **fd 2**（stderr = 内核控制台），所以 `> 文件` 重定向不会污染数据。
 * 退出码：0 = 成功；1 = 用法/IO 错误；2 = 数据损坏（坏 magic/坏 CRC/坏 ISIZE/截断）。
 */

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ==================== 系统调用（Linux x86_64 号段） ==================== */
#define NR_READ      0
#define NR_WRITE     1
#define NR_OPEN      2
#define NR_CLOSE     3
#define NR_STAT      4
#define NR_LSEEK     8
#define NR_MMAP      9
#define NR_UNLINK    87
#define NR_EXITG     231

#define O_RDONLY     0
#define O_WRONLY     1
#define O_CREAT      0x40
#define O_TRUNC      0x200

static inline i64 sc1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc3(long nr, i64 a1, i64 a2, i64 a3) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc6(long nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    register i64 r9  __asm__("r9")  = a6;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
static void *x_mmap(u64 len) {
    const i64 r = sc6(NR_MMAP, 0, (i64)len, 3 /*R|W*/, 0x22 /*PRIVATE|ANONYMOUS*/, -1, 0);
    if (r < 0) return 0;
    return (void *)(u64)r;
}

/* ==================== 小工具：字符串 / 输出 ==================== */
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) {
    u32 i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == 0 && b[i] == 0;
}
static int ends_with(const char *s, const char *suf) {
    const u32 n = slen(s), m = slen(suf);
    if (n < m) return 0;
    return seq(s + (n - m), suf);
}
static void outn(int fd, const char *s, u32 n) {
    while (n) {
        u32 k = n > 1024u ? 1024u : n;
        const i64 w = sc3(NR_WRITE, fd, (i64)(u64)s, (i64)k);
        if (w <= 0) return;
        s += w; n -= (u32)w;
    }
}
static void outs(int fd, const char *s) { outn(fd, s, slen(s)); }
static void outdec(int fd, u64 v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) outn(fd, &t[--n], 1);
}
static void outhex8(int fd, u32 v) {
    static const char H[] = "0123456789abcdef";
    char t[8];
    for (int i = 7; i >= 0; i--) { t[i] = H[v & 0xFu]; v >>= 4; }
    outs(fd, "0x");
    outn(fd, t, 8);
}
static void err2(const char *a, const char *b) { outs(2, a); if (b) outs(2, b); outs(2, "\n"); }

/* ==================== CRC32（IEEE，zlib 同多项式 0xEDB88320） ==================== */
static u32 g_crc_tab[256];

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        g_crc_tab[i] = c;
    }
}
static u32 crc32_of(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) c = g_crc_tab[(c ^ p[i]) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ==================== 文件读写（内核单次 read/write 上限 4096） ==================== */
static i64 file_size(int fd) {
    const i64 end = sc3(NR_LSEEK, fd, 0, 2 /*SEEK_END*/);
    if (end < 0) return -1;
    if (sc3(NR_LSEEK, fd, 0, 0 /*SEEK_SET*/) < 0) return -1;
    return end;
}
static i64 read_all(int fd, u8 *dst, u32 cap, u32 *out_len) {
    u32 got = 0;
    while (got < cap) {
        u32 want = cap - got;
        if (want > 4096u) want = 4096u;
        const i64 n = sc3(NR_READ, fd, (i64)(u64)(dst + got), (i64)want);
        if (n < 0) return -1;
        if (n == 0) break;
        got += (u32)n;
    }
    if (out_len) *out_len = got;
    return 0;
}
static int write_all(int fd, const u8 *src, u32 n) {
    u32 off = 0;
    while (off < n) {
        u32 want = n - off;
        if (want > 4096u) want = 4096u;
        const i64 w = sc3(NR_WRITE, fd, (i64)(u64)(src + off), (i64)want);
        if (w <= 0) return -1;
        off += (u32)w;
    }
    return 0;
}
/* 目标是否存在（用 stat 的 st_mode 判定；内核 stat 缓冲 = Linux x86_64 struct stat 前 144 B） */
static int path_exists(const char *path) {
    static u8 stbuf[160];
    for (u32 i = 0; i < sizeof(stbuf); i++) stbuf[i] = 0;
    return sc3(NR_STAT, (i64)(u64)path, (i64)(u64)stbuf, 0) == 0;
}

/* ==================== deflate（fixed Huffman + stored） ==================== */
#define HBITS 15
#define HSIZE (1u << HBITS)
#define WMASK 32767u

static const u16 LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const u8  LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const u16 DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const u8  DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

struct Bw { u8 *out; u32 cap; u32 len; u32 bitbuf; int bitcnt; };

static void bw_put(struct Bw *w, u32 bits, int n) {
    w->bitbuf |= (bits & ((1u << n) - 1u)) << w->bitcnt;
    w->bitcnt += n;
    while (w->bitcnt >= 8) {
        if (w->len < w->cap) w->out[w->len] = (u8)(w->bitbuf & 0xFFu);
        w->len++;
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}
static void bw_flush(struct Bw *w) {
    while (w->bitcnt > 0) {
        if (w->len < w->cap) w->out[w->len] = (u8)(w->bitbuf & 0xFFu);
        w->len++;
        w->bitbuf >>= 8;
        w->bitcnt -= 8;
    }
}
/* ★ 关键约定（RFC1951 3.1.1）：Huffman 码按"**码字的最高位先发**"进码流，而 bw_put 是 LSB 先写的
 *   （与 DEFLATE 的字节内位序一致）—— 所以发码字之前必须先按位反转。这一点搞错会得到
 *   "解压端报 invalid distance too far back"的坏流（本文件在宿主自检里踩过一次）。 */
static u32 rev_bits(u32 v, int n) {
    u32 r = 0;
    for (int i = 0; i < n; i++) { r = (r << 1) | (v & 1u); v >>= 1; }
    return r;
}
static void bw_code(struct Bw *w, u32 code, int n) { bw_put(w, rev_bits(code, n), n); }
/* fixed Huffman 的字面/长度码（RFC1951 的 3.2.6 表；下面写的是**规范码字**，发送前反转） */
static void emit_lit(struct Bw *w, int sym) {
    if (sym <= 143)      bw_code(w, (u32)(0x30 + sym), 8);
    else if (sym <= 255) bw_code(w, (u32)(0x190 + (sym - 144)), 9);
    else if (sym <= 279) bw_code(w, (u32)(sym - 256), 7);
    else                 bw_code(w, (u32)(0xC0 + (sym - 280)), 8);
}
static void emit_len_dist(struct Bw *w, u32 len, u32 dist) {
    int li = 28;
    while (li > 0 && LEN_BASE[li] > len) li--;
    emit_lit(w, 257 + li);
    if (LEN_EXTRA[li]) bw_put(w, len - LEN_BASE[li], LEN_EXTRA[li]);   /* 额外位：LSB 先（原样） */
    int di = 29;
    while (di > 0 && DIST_BASE[di] > dist) di--;
    bw_code(w, (u32)di, 5);                                            /* 距离码同样是"规范码字" */
    if (DIST_EXTRA[di]) bw_put(w, dist - DIST_BASE[di], DIST_EXTRA[di]);
}
static u32 hash3(const u8 *p) {
    return (((u32)p[0] << 10) ^ ((u32)p[1] << 5) ^ (u32)p[2]) & (HSIZE - 1u);
}
/* 把 [0, n) 用 fixed Huffman 编成一个 block（bfinal 决定 BFINAL）；head/prev 由调用方 mmap 好。
 * 返回输出字节数（不会超过 cap —— 调用方按最坏情况开缓冲）。 */
static u32 deflate_fixed(const u8 *src, u32 n, u8 *dst, u32 cap, i32 *head, i32 *prev, int bfinal) {
    struct Bw w;
    w.out = dst; w.cap = cap; w.len = 0; w.bitbuf = 0; w.bitcnt = 0;
    bw_put(&w, (u32)bfinal, 1);
    bw_put(&w, 1u, 2);                                   /* BTYPE=01 = fixed Huffman */
    for (u32 i = 0; i < HSIZE; i++) head[i] = -1;
    for (u32 i = 0; i < 32768u; i++) prev[i] = -1;
    u32 i = 0;
    while (i < n) {
        u32 best_len = 0, best_dist = 0;
        const u32 rem = n - i;
        const u32 lim = (rem < 258u) ? rem : 258u;
        if (rem >= 3u) {
            const u32 h = hash3(src + i);
            i32 cur = head[h];
            int chain = 64;
            while (cur >= 0 && chain-- > 0) {
                const u32 dist = i - (u32)cur;
                if (dist > 32768u) break;
                const u8 *a = src + (u32)cur, *b = src + i;
                /* 先花 O(1) 排除明显不匹配（best_len 处的字节 + 头两个字节）；best_len < lim 恒成立 */
                if (a[best_len] == b[best_len] && a[0] == b[0] && a[1] == b[1]) {
                    u32 l = 0;
                    while (l < lim && a[l] == b[l]) l++;
                    if (l > best_len) { best_len = l; best_dist = dist; if (l >= lim) break; }
                }
                cur = prev[(u32)cur & WMASK];
            }
        }
        u32 adv;
        if (best_len >= 3u) {
            emit_len_dist(&w, best_len, best_dist);
            adv = best_len;
        } else {
            emit_lit(&w, src[i]);
            adv = 1;
        }
        for (u32 k = 0; k < adv; k++) {
            const u32 p = i + k;
            if (p + 3u <= n) {
                const u32 h = hash3(src + p);
                prev[p & WMASK] = head[h];
                head[h] = (i32)p;
            }
        }
        i += adv;
    }
    emit_lit(&w, 256);                                   /* end-of-block */
    bw_flush(&w);
    return w.len;
}
/* stored block（每块 <= 65535 字节；空输入也要发一个 final 空 stored 块） */
static u32 deflate_stored(const u8 *src, u32 n, u8 *dst, u32 cap) {
    u32 off = 0, out = 0;
    do {
        u32 blk = n - off;
        if (blk > 65535u) blk = 65535u;
        const int final = (off + blk >= n) ? 1 : 0;
        if (out + 5u + blk > cap) return 0xFFFFFFFFu;     /* 不该发生：调用方按最坏情况开缓冲 */
        dst[out++] = (u8)final;                          /* BFINAL + BTYPE=00，按字节对齐 */
        dst[out++] = (u8)(blk & 0xFFu);
        dst[out++] = (u8)(blk >> 8);
        dst[out++] = (u8)((~blk) & 0xFFu);
        dst[out++] = (u8)(((~blk) >> 8) & 0xFFu);
        for (u32 k = 0; k < blk; k++) dst[out++] = src[off + k];
        off += blk;
    } while (off < n);
    return out;
}

/* ==================== inflate（stored / fixed / dynamic；多 block） ==================== */
struct Bin { const u8 *in; u32 len; u32 pos; u32 bitbuf; int bitcnt; int err; };

static int bits_get(struct Bin *s, int need) {
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
static void bits_align(struct Bin *s) { s->bitbuf = 0; s->bitcnt = 0; }

struct Huff { short count[16]; short sym[288]; };

static int huff_build(struct Huff *h, const u8 *lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;                      /* 全 0：允许（表示"没有这个表"） */
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;                         /* 过完备 */
    }
    short offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (short)i;
    return left;                                         /* >0 = 不完整码（允许） */
}
static int huff_decode(struct Bin *s, const struct Huff *h) {
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
static void fixed_tables(struct Huff *lit, struct Huff *dist) {
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
static int inflate_block(struct Bin *s, u8 *out, u32 outcap, u32 *outlen) {
    u32 o = *outlen;
    const int final = bits_get(s, 1);
    const int type  = bits_get(s, 2);
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
            const int hlit  = bits_get(s, 5) + 257;
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
            if (lens[256] == 0) return 2;                /* 没有 end-of-block 码 */
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
            if (s->err || d == 0 || d > o) return 2;     /* 回溯越界 = 坏流 */
            if (len > outcap - o) return 2;
            for (u32 k = 0; k < len; k++) { out[o] = out[o - d]; o++; }
        }
    } else {
        return 2;                                        /* BTYPE=11 保留 */
    }
    *outlen = o;
    return final ? 1 : 0;
}

/* ==================== gzip 外壳 ==================== */
#define GZ_MAGIC0 0x1F
#define GZ_MAGIC1 0x8B
#define GZ_DEFLATE 8

/* 前向声明：头部跳过逻辑会调用「从某个偏移开始解 inflate」的实现 */
static int gunzip_member_at(const u8 *in, u32 n, u32 off, u8 *out, u32 outcap, u32 *out_len, u32 *out_crc);

/* 返回 0 = 成功；2 = 坏头/坏 CRC/坏 ISIZE/截断；*out_len 与 *out_crc 是解出来的字节数与 CRC32 */
static int gunzip_member(const u8 *in, u32 n, u8 *out, u32 outcap, u32 *out_len, u32 *out_crc) {
    if (n < 18u) return 2;                                        /* 头 10 + 最小流 + 尾 8 */
    if (in[0] != GZ_MAGIC0 || in[1] != GZ_MAGIC1 || in[2] != GZ_DEFLATE) return 2;
    const u8 flg = in[3];
    if (flg & 0xE0u) return 2;                                    /* 保留位必须为 0 */
    if (flg & 0x10u) return 2;                                    /* FDICT：需要预设字典，不支持（如实） */
    if (flg & 0x04u) {                                            /* FEXTRA：跳过 XLEN 字节 */
        if (n < 12u) return 2;
        const u32 xlen = (u32)in[10] | ((u32)in[11] << 8);
        if (12u + xlen + 8u > n) return 2;
        return gunzip_member_at(in, n, 12u + xlen, out, outcap, out_len, out_crc);
    }
    /* FNAME/FCOMMENT/FHCRC 只是"跳过到 NUL / 固定长度" */
    {
        u32 off = 10u;
        if (flg & 0x08u) { while (off < n && in[off]) off++; off++; if (off >= n) return 2; }
        if (flg & 0x80u) { while (off < n && in[off]) off++; off++; if (off >= n) return 2; }
        if (flg & 0x02u) { off += 2; if (off > n) return 2; }
        return gunzip_member_at(in, n, off, out, outcap, out_len, out_crc);
    }
}
static int gunzip_member_at(const u8 *in, u32 n, u32 off, u8 *out, u32 outcap, u32 *out_len, u32 *out_crc) {
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
    if (s.pos + 8u != n) return 2;                                /* 尾 8 字节必须正好在流结束处 */
    const u32 crc = (u32)in[s.pos] | ((u32)in[s.pos + 1] << 8) | ((u32)in[s.pos + 2] << 16) | ((u32)in[s.pos + 3] << 24);
    const u32 isize = (u32)in[s.pos + 4] | ((u32)in[s.pos + 5] << 8) | ((u32)in[s.pos + 6] << 16) | ((u32)in[s.pos + 7] << 24);
    if (isize != o) return 2;                                     /* 长度不符 = 坏流 */
    const u32 got = crc32_of(out, o);
    if (got != crc) return 2;                                     /* CRC32 不符 = 坏流 */
    *out_len = o;
    if (out_crc) *out_crc = crc;
    return 0;
}

/* ==================== 命令行 ==================== */
struct Opt {
    int decompress;
    int to_stdout;
    int keep;
    int force;
    int verbose;
    int test;
    const char *out_path;       /* -o OUT */
};

static const char *base_name(const char *p) {
    const char *b = p;
    for (u32 i = 0; p[i]; i++) if (p[i] == '/') b = p + i + 1;
    return b;
}
static void usage(void) {
    err2("usage: gzip   [-c] [-k] [-f] [-v] [-o OUT] FILE...", 0);
    err2("       gunzip [-c] [-k] [-f] [-v] [-t] [-o OUT] FILE.gz...", 0);
    err2("       (gzip: fixed-Huffman deflate + stored fallback; gunzip: full inflate)", 0);
}

static int process_one(const struct Opt *op, const char *path) {
    const int compressing = !op->decompress;
    /* ---- 输入名 / 输出名 ---- */
    char in_name[128];
    const u32 il = slen(path);
    if (il >= sizeof(in_name)) { err2("gzip: path too long: ", path); return 1; }
    for (u32 i = 0; i <= il; i++) in_name[i] = path[i];

    char out_name[140];
    const char *outp = op->out_path;
    if (!outp && !op->to_stdout) {
        if (compressing) {
            if (il + 3u >= sizeof(out_name)) { err2("gzip: path too long: ", path); return 1; }
            for (u32 i = 0; i <= il; i++) out_name[i] = path[i];
            out_name[il] = '.'; out_name[il + 1] = 'g'; out_name[il + 2] = 'z'; out_name[il + 3] = 0;
            outp = out_name;
        } else {
            if (!ends_with(path, ".gz")) { err2("gzip: unknown suffix -- ignored: ", path); return 1; }
            for (u32 i = 0; i + 3u < il; i++) out_name[i] = path[i];
            out_name[il - 3u] = 0;
            outp = out_name;
        }
    }

    /* ---- 读输入 ---- */
    const i64 ifd = sc3(NR_OPEN, (i64)(u64)in_name, O_RDONLY, 0);
    if (ifd < 0) { err2("gzip: cannot open: ", in_name); return 1; }
    const i64 isz = file_size((int)ifd);
    if (isz < 0 || isz > 4 * 1024 * 1024) {
        err2("gzip: bad size (in-memory mode caps at 4 MiB): ", in_name);
        sc1(NR_CLOSE, ifd);
        return 1;
    }
    const u32 in_len = (u32)isz;
    u8 *inbuf = (u8 *)x_mmap(in_len ? in_len : 1u);
    if (!inbuf) { err2("gzip: mmap input failed", 0); sc1(NR_CLOSE, ifd); return 1; }
    u32 got = 0;
    if (read_all((int)ifd, inbuf, in_len, &got) != 0 || got != in_len) {
        err2("gzip: read failed: ", in_name);
        sc1(NR_CLOSE, ifd);
        return 1;
    }
    sc1(NR_CLOSE, ifd);

    u8 *outbuf;
    u32 crc;
    if (compressing) {
        const u32 cap = in_len + in_len / 4u + 4096u;
        outbuf = (u8 *)x_mmap(cap ? cap : 1u);
        i32 *head = (i32 *)x_mmap((u64)HSIZE * 4u);
        i32 *prev = (i32 *)x_mmap(32768u * 4u);
        if (!outbuf || !head || !prev) { err2("gzip: mmap failed", 0); return 1; }
        /* 先按 fixed Huffman 编；若结果不小于输入（不可压缩），退化成 stored（RFC1951 允许） */
        u8 *tmp = (u8 *)x_mmap(cap);
        if (!tmp) { err2("gzip: mmap failed", 0); return 1; }
        const u32 fixed_len = deflate_fixed(inbuf, in_len, tmp, cap, head, prev, 1);
        u32 stream_len;
        if (fixed_len != 0xFFFFFFFFu && fixed_len < in_len) {
            for (u32 i = 0; i < fixed_len; i++) outbuf[i] = tmp[i];
            stream_len = fixed_len;
        } else {
            stream_len = deflate_stored(inbuf, in_len, outbuf, cap);
            if (stream_len == 0xFFFFFFFFu) { err2("gzip: internal buffer too small", 0); return 1; }
        }
        /* gzip 成员头（10B：magic/CM/FLG/MTIME=0/XFL/OS）+ 流 + CRC32 + ISIZE */
        u8 hdr[10];
        hdr[0] = GZ_MAGIC0; hdr[1] = GZ_MAGIC1; hdr[2] = GZ_DEFLATE; hdr[3] = 0;
        hdr[4] = hdr[5] = hdr[6] = hdr[7] = 0;   /* MTIME=0：本内核没有可靠墙上时间，不写假时间 */
        hdr[8] = 0; hdr[9] = 3;                  /* XFL=0 / OS=3 (Unix) */
        crc = crc32_of(inbuf, in_len);
        u8 tail[8];
        tail[0] = (u8)crc; tail[1] = (u8)(crc >> 8); tail[2] = (u8)(crc >> 16); tail[3] = (u8)(crc >> 24);
        tail[4] = (u8)in_len; tail[5] = (u8)(in_len >> 8); tail[6] = (u8)(in_len >> 16); tail[7] = (u8)(in_len >> 24);
        if ((op->to_stdout && !op->out_path) || op->test) {   /* -o 优先于 -c（-o 是明确的落盘目标）*/
            if (!op->test) {
                if (write_all(1, hdr, 10) != 0 || write_all(1, outbuf, stream_len) != 0 || write_all(1, tail, 8) != 0) {
                    err2("gzip: write failed", 0);
                    return 1;
                }
            }
        } else {
            if (path_exists(outp) && !op->force) { err2("gzip: target exists (use -f): ", outp); return 1; }
            const i64 ofd = sc3(NR_OPEN, (i64)(u64)outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (ofd < 0) { err2("gzip: cannot create: ", outp); return 1; }
            int bad = (write_all((int)ofd, hdr, 10) != 0) || (write_all((int)ofd, outbuf, stream_len) != 0);
            if (!bad) bad = (write_all((int)ofd, tail, 8) != 0);
            sc1(NR_CLOSE, ofd);
            if (bad) { err2("gzip: write failed: ", outp); return 1; }
            if (!op->keep) (void)sc3(NR_UNLINK, (i64)(u64)in_name, 0, 0);
        }
        if (op->verbose) {
            outs(2, "gzip: "); outs(2, path); outs(2, ": ");
            outdec(2, in_len); outs(2, " -> "); outdec(2, stream_len + 18u);
            outs(2, " bytes, crc32="); outhex8(2, crc);
            outs(2, (stream_len < in_len) ? ", mode=fixed\n" : ", mode=stored\n");
        }
        return 0;
    }

    /* ---- 解压 ---- */
    /* 输出缓冲怎么定大小：gzip 成员的最后 4 字节就是 ISIZE（未压缩长度 mod 2^32）——
     * 直接用它开缓冲，而不是"压缩比的猜测"（★ 实测踩过：本工具把 1 MiB 文本压到 39 KB，
     * 按 4x 开缓冲会装不下解压结果，于是**自己的 inflate 报坏流**）。ISIZE 被改坏/超上限
     * （内存模式上限 4 MiB）时明确报错，不猜。 */
    u32 cap = 0;
    if (in_len >= 18u && inbuf[0] == GZ_MAGIC0 && inbuf[1] == GZ_MAGIC1) {
        const u32 isize = (u32)inbuf[in_len - 4] | ((u32)inbuf[in_len - 3] << 8) |
                          ((u32)inbuf[in_len - 2] << 16) | ((u32)inbuf[in_len - 1] << 24);
        if (isize > 4u * 1024u * 1024u) {
            /* ISIZE 声明 > 4 MiB：内存模式服务不了 —— 如实拒绝，并且**用与"坏流"同一句主消息**
             * （退出码 2 + 原因后缀），这样调用方/脚本只需要认一条错误前缀。 */
            err2("gzip: invalid or corrupted input: ", path);
            err2("gzip: declared ISIZE > 4 MiB (in-memory mode caps at 4 MiB)", 0);
            return 2;
        }
        cap = isize + 1024u;
    }
    if (cap == 0) cap = in_len * 16u + 65536u;            /* 没有可信 ISIZE：按最坏情况开 */
    outbuf = (u8 *)x_mmap(cap);
    if (!outbuf) { err2("gzip: mmap output failed", 0); return 1; }
    u32 olen = 0;
    const int rc = gunzip_member(inbuf, in_len, outbuf, cap, &olen, &crc);
    if (rc != 0) { err2("gzip: invalid or corrupted input: ", path); return 2; }
    if (op->test) {
        outs(2, "gunzip: "); outs(2, path);
        outs(2, ": OK crc32="); outhex8(2, crc);
        outs(2, " size="); outdec(2, olen); outs(2, "\n");
        return 0;
    }
    if (op->to_stdout && !op->out_path) {                 /* -o 优先于 -c（见压缩端同一说明） */
        if (write_all(1, outbuf, olen) != 0) { err2("gzip: write failed", 0); return 1; }
    } else {
        if (path_exists(outp) && !op->force) { err2("gzip: target exists (use -f): ", outp); return 1; }
        const i64 ofd = sc3(NR_OPEN, (i64)(u64)outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (ofd < 0) { err2("gzip: cannot create: ", outp); return 1; }
        const int bad = write_all((int)ofd, outbuf, olen) != 0;
        sc1(NR_CLOSE, ofd);
        if (bad) { err2("gzip: write failed: ", outp); return 1; }
        if (!op->keep) (void)sc3(NR_UNLINK, (i64)(u64)in_name, 0, 0);
    }
    if (op->verbose) {
        outs(2, "gunzip: "); outs(2, path); outs(2, ": ");
        outdec(2, in_len); outs(2, " -> "); outdec(2, olen);
        outs(2, " bytes, crc32="); outhex8(2, crc); outs(2, "\n");
    }
    return 0;
}

/* 真正的入口：_start 把初始 rsp 递进来（SysV：argc 在 [rsp]） */
void gzip_main(u64 *sp) {
    const int argc = (int)sp[0];
    char **argv = (char **)&sp[1];
    crc_init();

    struct Opt op;
    op.decompress = 0; op.to_stdout = 0; op.keep = 0; op.force = 0; op.verbose = 0; op.test = 0;
    op.out_path = 0;
    if (argc >= 1 && argv[0] && ends_with(base_name(argv[0]), "gunzip")) op.decompress = 1;

    int nfiles = 0;
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] == '-' && a[1] != 0) {
            for (int k = 1; a[k]; k++) {
                const char c = a[k];
                if (c == 'c') op.to_stdout = 1;
                else if (c == 'k') op.keep = 1;
                else if (c == 'f') op.force = 1;
                else if (c == 'v') op.verbose = 1;
                else if (c == 'd') op.decompress = 1;
                else if (c == 't') { op.test = 1; op.decompress = 1; }
                else if (c == 'o') {
                    if (a[k + 1]) { op.out_path = a + k + 1; }
                    else if (i + 1 < argc) { op.out_path = argv[++i]; }
                    else { usage(); sc1(NR_EXITG, 1); }
                    break;
                } else {
                    err2("gzip: unknown option: -", 0);
                    usage();
                    sc1(NR_EXITG, 1);
                }
            }
            continue;
        }
        nfiles++;
        const int r = process_one(&op, a);
        if (r != 0 && rc == 0) rc = r;                    /* 第一个错误码就是退出码（2 = 损坏优先） */
    }
    if (nfiles == 0) { usage(); sc1(NR_EXITG, 1); }
    sc1(NR_EXITG, rc);
    for (;;) { }
}
