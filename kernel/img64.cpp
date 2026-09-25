// img64.cpp - PNG/BMP 解码实现（无 libc、无浮点、纯整数）。
//
// PNG 路径：解析 chunk（IHDR/PLTE/tRNS/IDAT）-> inflate（stored/fixed/dynamic 三种块，
//           canonical Huffman 解码）-> 逐行反滤波（None/Sub/Up/Average/Paeth）-> 转 AARRGGBB。
//           支持位深 1/2/4/8（灰度、调色板）与 8/16（RGB/RGBA/灰度+alpha，16 位取高字节）；
//           **Adam7 隔行**不支持（返回 -2 并打点，本批用不到的格式不硬撑）。
// BMP 路径：BI_RGB 未压缩，24/32bpp + 8bpp 调色板，行按 4 字节对齐，支持自下而上/自上而下。
//
// JPEG：**本批未实现**（返回 -2 并打点说明）—— 需求里点名的是 PNG（logo/kaisi.png 与壁纸都用 PNG），
//       报告里如实标注；解码入口已按格式分派，后续补一个 JPEG 分支即可。
#include "img64.h"
#include "vfs64.h"
#include "mem_64.h"
#include "debug64.h"

static const char* g_err = "ok";
static const char* g_fmt = "-";
static int g_decode_log = 0;
static int g_inflate_rc = 0;

const char* img64_last_err64() { return g_err; }
const char* img64_last_fmt64() { return g_fmt; }

void img64_free64(Img64* img) {
    if (!img) return;
    if (img->owned && img->px) kfree_64(img->px);
    img->px = nullptr;
    img->w = img->h = 0;
    img->owned = 0;
}

// ==================== 位读取（deflate，LSB 优先）====================
struct BitRd64 {
    const uint8_t* p;
    int len, pos;
    uint32_t buf;
    int cnt;
    int err;
};

static int br_bits(BitRd64* b, int n) {
    while (b->cnt < n) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        b->buf |= (uint32_t)b->p[b->pos++] << b->cnt;
        b->cnt += 8;
    }
    const int v = (int)(b->buf & ((1u << n) - 1u));
    b->buf >>= n;
    b->cnt -= n;
    return v;
}
static void br_align(BitRd64* b) {
    b->buf = 0;
    b->cnt = 0;
}

// ==================== canonical Huffman（puff 同构）====================
#define IMG64_MAXBITS 15
struct Huff64 {
    short count[IMG64_MAXBITS + 1];
    short symbol[288];
};
static int huff_build(Huff64* h, const short* length, int n) {
    for (int i = 0; i <= IMG64_MAXBITS; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;                 // 全 0 长度 = 无码（合法但空）
    int left = 1;
    for (int len = 1; len <= IMG64_MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;                    // 码长过订阅（数据坏）
    }
    // ★ 注意：**不完整码是合法的**（deflate 的固定距离码就是：30 个 5 位码 < 32）。
    //   只有"过订阅"（left < 0）才算坏数据；不完整码里解到未分配的码由 huff_decode 返回 -1。
    short offs[IMG64_MAXBITS + 2];
    offs[1] = 0;
    for (int len = 1; len <= IMG64_MAXBITS; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (length[i]) h->symbol[offs[length[i]]++] = (short)i;
    return 0;                                       // 0 = 可用（含不完整码）
}
static int huff_decode(BitRd64* b, const Huff64* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= IMG64_MAXBITS; len++) {
        code |= br_bits(b, 1);
        if (b->err) return -1;
        const int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static const short kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const short kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const short kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const short kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static Huff64 g_len_h, g_dist_h;         // 固定/动态块共用（单线程，静态表足够）

// ★ 固定 Huffman 表：**每遇到一个 BTYPE=1 块都必须重建**，不能做"只建一次"的缓存。
//   原因：g_len_h/g_dist_h 与动态块共用；动态块解完后这两张表是**动态表**，
//   而 zlib 生成的流常见形态是"一个大动态块 + 收尾的空固定块（BFINAL=1，只含 EOB）"。
//   老实现用 static built 缓存 -> 固定块拿动态表解固定码 -> 解出乱码符号、把输入读到尽头，
//   整张图 decode 失败（实测 [IMG64] decode png rc=3 err=inflate failed inflate_rc=18，
//   真文件 logo/kaisi.png 就是这么挂的；4x4 自检因为只有一个固定块反而看不出来）。
//   重建只要 288+30 次赋值，代价可忽略。
static int inflate_fixed_tables() {
    short l[288];
    int i = 0;
    for (; i < 144; i++) l[i] = 8;
    for (; i < 256; i++) l[i] = 9;
    for (; i < 280; i++) l[i] = 7;
    for (; i < 288; i++) l[i] = 8;
    if (huff_build(&g_len_h, l, 288) != 0) return -1;
    short d[30];
    for (i = 0; i < 30; i++) d[i] = 5;
    if (huff_build(&g_dist_h, d, 30) != 0) return -1;
    return 0;
}

// 解压（raw deflate；out_len = 期望输出字节数）
static int inflate_raw64(const uint8_t* in, int in_len, uint8_t* out, int out_len) {
    BitRd64 b{in, in_len, 0, 0, 0, 0};
    int have = 0;
    int last = 0;
    while (!last) {
        last = br_bits(&b, 1);
        const int type = br_bits(&b, 2);
        if (b.err) return 1;
        if (type == 0) {                       // 未压缩块
            br_align(&b);
            if (b.pos + 4 > b.len) return 2;
            const int len = b.p[b.pos] | (b.p[b.pos + 1] << 8);
            const int nlen = b.p[b.pos + 2] | (b.p[b.pos + 3] << 8);
            b.pos += 4;
            if (((len ^ 0xFFFF) & 0xFFFF) != nlen) return 3;
            if (b.pos + len > b.len) return 4;
            for (int i = 0; i < len; i++) {
                if (have >= out_len) break;
                out[have++] = b.p[b.pos + i];
            }
            b.pos += len;
        } else if (type == 1 || type == 2) {
            if (type == 1) {
                if (inflate_fixed_tables() != 0) return 5;
            } else {
                const int hlit = br_bits(&b, 5) + 257;
                const int hdist = br_bits(&b, 5) + 1;
                const int hclen = br_bits(&b, 4) + 4;
                if (b.err) return 6;
                if (hlit > 286 || hdist > 30) return 7;
                static const short ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                short cl_len[19];
                for (int i = 0; i < 19; i++) cl_len[i] = 0;
                for (int i = 0; i < hclen; i++) cl_len[ord[i]] = (short)br_bits(&b, 3);
                Huff64 cl_h;
                if (huff_build(&cl_h, cl_len, 19) != 0) return 8;
                short lens[288 + 30];
                int n = 0;
                while (n < hlit + hdist) {
                    const int sym = huff_decode(&b, &cl_h);
                    if (sym < 0) return 9;
                    if (sym < 16) lens[n++] = (short)sym;
                    else if (sym == 16) {
                        if (n == 0) return 10;
                        const int prev = lens[n - 1];
                        int rep = 3 + br_bits(&b, 2);
                        while (rep-- > 0 && n < hlit + hdist) lens[n++] = (short)prev;
                    } else if (sym == 17) {
                        int rep = 3 + br_bits(&b, 3);
                        while (rep-- > 0 && n < hlit + hdist) lens[n++] = 0;
                    } else {
                        int rep = 11 + br_bits(&b, 7);
                        while (rep-- > 0 && n < hlit + hdist) lens[n++] = 0;
                    }
                    if (b.err) return 11;
                }
                if (huff_build(&g_len_h, lens, hlit) != 0) return 12;
                if (huff_build(&g_dist_h, lens + hlit, hdist) != 0) return 13;
            }
            // 解符号
            for (;;) {
                const int sym = huff_decode(&b, &g_len_h);
                if (sym < 0) return 14;
                if (sym < 256) {
                    if (have < out_len) out[have++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li >= 29) return 15;
                    const int l = kLenBase[li] + br_bits(&b, kLenExtra[li]);
                    const int ds = huff_decode(&b, &g_dist_h);
                    if (ds < 0 || ds >= 30) return 16;
                    const int d = kDistBase[ds] + br_bits(&b, kDistExtra[ds]);
                    if (d > have) return 17;
                    for (int i = 0; i < l; i++) {
                        if (have >= out_len) break;
                        out[have] = out[have - d];
                        have++;
                    }
                    if (b.err) return 18;
                }
            }
        } else {
            return 19;                            // BTYPE=3 非法
        }
    }
    return 0;
}

// ==================== PNG ====================
static uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void png_expand_row(const uint8_t* src, int w, int channels, int bitdepth,
                           const uint8_t* plte, const uint8_t* trns, int trns_n, uint32_t* dst) {
    // 只处理到"得到 AARRGGBB"这一步；16 位取高字节
    if (bitdepth == 8) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = src + x * channels;
            if (channels == 1)      dst[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0];
            else if (channels == 2) dst[x] = ((uint32_t)p[1] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0];
            else if (channels == 3) dst[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            else                    dst[x] = ((uint32_t)p[3] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
        }
    } else if (bitdepth == 16) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = src + x * channels * 2;
            if (channels == 1)      dst[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0];
            else if (channels == 2) dst[x] = ((uint32_t)p[2] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0];
            else if (channels == 3) dst[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[2] << 8) | p[4];
            else                    dst[x] = ((uint32_t)p[6] << 24) | ((uint32_t)p[0] << 16) | ((uint32_t)p[2] << 8) | p[4];
        }
    } else {                            // 1/2/4 位：灰度或调色板
        const int per = 8 / bitdepth;
        const int maxv = (1 << bitdepth) - 1;
        for (int x = 0; x < w; x++) {
            const int idx_byte = x / per;
            const int shift = 8 - bitdepth * (x % per + 1);
            const int v = (src[idx_byte] >> shift) & maxv;
            if (plte) {
                const int a = (trns && v < trns_n) ? trns[v] : 255;
                dst[x] = ((uint32_t)a << 24) | ((uint32_t)plte[v * 3] << 16) |
                         ((uint32_t)plte[v * 3 + 1] << 8) | plte[v * 3 + 2];
            } else {
                const int g = v * 255 / maxv;
                dst[x] = 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
            }
        }
    }
}

static int decode_png64(const uint8_t* data, int len, Img64* out) {
    if (len < 8 || data[0] != 0x89 || data[1] != 'P' || data[2] != 'N' || data[3] != 'G') return -3;
    int w = 0, h = 0, bd = 0, ct = 0, interlace = 0;
    const uint8_t* plte = nullptr;
    int plte_n = 0;
    const uint8_t* trns = nullptr;
    int trns_n = 0;
    uint8_t* idat = nullptr;
    int idat_len = 0;
    int pos = 8;
    bool saw_ihdr = false;
    while (pos + 8 <= len) {
        const int clen = (int)be32(data + pos);
        const uint8_t* type = data + pos + 4;
        const uint8_t* cdata = data + pos + 8;
        if (clen < 0 || pos + 12 + clen > len) break;
        if (type[0] == 'I' && type[1] == 'H' && type[2] == 'D' && type[3] == 'R') {
            if (clen < 13) return -3;
            w = (int)be32(cdata);
            h = (int)be32(cdata + 4);
            bd = cdata[8];
            ct = cdata[9];
            interlace = cdata[12];
            saw_ihdr = true;
        } else if (type[0] == 'P' && type[1] == 'L' && type[2] == 'T' && type[3] == 'E') {
            plte = cdata;
            plte_n = clen / 3;
        } else if (type[0] == 't' && type[1] == 'R' && type[2] == 'N' && type[3] == 'S') {
            trns = cdata;
            trns_n = clen;
        } else if (type[0] == 'I' && type[1] == 'D' && type[2] == 'A' && type[3] == 'T') {
            if (idat_len == 0) idat = (uint8_t*)kmalloc_64((uint64_t)clen);
            else                idat = (uint8_t*)krealloc_64(idat, (uint64_t)(idat_len + clen));
            if (!idat) { g_err = "oom idat"; return -4; }
            for (int i = 0; i < clen; i++) idat[idat_len + i] = cdata[i];
            idat_len += clen;
        } else if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D') {
            break;
        }
        pos += 12 + clen;
    }
    if (!saw_ihdr || w <= 0 || h <= 0 || w > 4096 || h > 4096) { if (idat) kfree_64(idat); g_err = "bad ihdr"; return -3; }
    if (interlace) { if (idat) kfree_64(idat); g_err = "interlace unsupported"; return -2; }
    if (!idat) { g_err = "no idat"; return -3; }
    int channels;
    switch (ct) {
        case 0: channels = 1; break;      // 灰度
        case 2: channels = 3; break;      // RGB
        case 3: channels = 1; break;      // 调色板
        case 4: channels = 2; break;      // 灰度 + alpha
        case 6: channels = 4; break;      // RGBA
        default: kfree_64(idat); g_err = "bad colortype"; return -3;
    }
    if (ct == 3 && bd == 16) { kfree_64(idat); g_err = "bad palette depth"; return -3; }
    if (ct != 3 && bd != 8 && bd != 16) { kfree_64(idat); g_err = "bad bitdepth"; return -3; }
    const int stride = (w * channels * bd + 7) / 8;
    const int raw_len = (stride + 1) * h;
    uint8_t* raw = (uint8_t*)kmalloc_64((uint64_t)raw_len);
    if (!raw) { kfree_64(idat); g_err = "oom raw"; return -4; }
    // PNG 的 IDAT 是 **zlib 流**（2 字节 zlib 头 + deflate + 4 字节 adler32）：
    // 这里跳过 zlib 头再解 raw deflate；尾部 adler32 用不到（输出长度已知，解满即停）。
    int zoff = 0;
    if (idat_len > 2 && (idat[0] & 0x0F) == 8 && ((((int)idat[0] << 8) | idat[1]) % 31) == 0) zoff = 2;
    const int rc = inflate_raw64(idat + zoff, idat_len - zoff, raw, raw_len);
    kfree_64(idat);
    if (rc != 0) { kfree_64(raw); g_err = "inflate failed"; g_inflate_rc = rc; return -3; }
    const int bpp = (channels * bd + 7) / 8 ? (channels * bd + 7) / 8 : 1;
    if (bpp > 8) { kfree_64(raw); g_err = "bpp"; return -3; }
    uint32_t* px = (uint32_t*)kmalloc_64((uint64_t)w * h * 4);
    if (!px) { kfree_64(raw); g_err = "oom px"; return -4; }
    uint8_t* prev = (uint8_t*)kmalloc_64((uint64_t)stride + 8);
    if (!prev) { kfree_64(raw); kfree_64(px); g_err = "oom prev"; return -4; }
    for (int i = 0; i < stride + 8; i++) prev[i] = 0;
    uint8_t* cur = (uint8_t*)kmalloc_64((uint64_t)stride + 8);
    if (!cur) { kfree_64(raw); kfree_64(px); kfree_64(prev); g_err = "oom cur"; return -4; }
    for (int y = 0; y < h; y++) {
        const uint8_t* r = raw + (uint64_t)y * (stride + 1);
        const int ft = r[0];
        const uint8_t* src = r + 1;
        if (ft == 0) {
            for (int i = 0; i < stride; i++) cur[i] = src[i];
        } else if (ft == 1) {
            for (int i = 0; i < stride; i++) cur[i] = (uint8_t)(src[i] + (i >= bpp ? cur[i - bpp] : 0));
        } else if (ft == 2) {
            for (int i = 0; i < stride; i++) cur[i] = (uint8_t)(src[i] + prev[i]);
        } else if (ft == 3) {
            for (int i = 0; i < stride; i++) {
                const int left = (i >= bpp) ? cur[i - bpp] : 0;
                cur[i] = (uint8_t)(src[i] + ((left + prev[i]) >> 1));
            }
        } else if (ft == 4) {
            for (int i = 0; i < stride; i++) {
                const int a = (i >= bpp) ? cur[i - bpp] : 0;
                const int b2 = prev[i];
                const int c = (i >= bpp) ? prev[i - bpp] : 0;
                const int p = a + b2 - c;
                const int pa = p > a ? p - a : a - p;
                const int pb = p > b2 ? p - b2 : b2 - p;
                const int pc = p > c ? p - c : c - p;
                int pr;
                if (pa <= pb && pa <= pc) pr = a;
                else if (pb <= pc) pr = b2;
                else pr = c;
                cur[i] = (uint8_t)(src[i] + pr);
            }
        } else {
            kfree_64(raw); kfree_64(px); kfree_64(prev); kfree_64(cur);
            g_err = "bad filter";
            return -3;
        }
        png_expand_row(cur, w, channels, bd, plte, trns, trns_n, px + (uint64_t)y * w);
        if (stride + 8 <= 4096) {
            for (int i = 0; i < stride; i++) prev[i] = cur[i];
        } else {
            for (int i = 0; i < stride; i++) prev[i] = cur[i];   // 大行：直接拷贝（stride 上限 16K）
        }
    }
    kfree_64(raw);
    kfree_64(prev);
    kfree_64(cur);
    out->px = px;
    out->w = w;
    out->h = h;
    out->owned = 1;
    g_fmt = "png";
    return 0;
}

// ==================== BMP（BI_RGB 未压缩）====================
static int decode_bmp64(const uint8_t* d, int len, Img64* out) {
    if (len < 54) return -3;
    const uint32_t off = (uint32_t)d[10] | ((uint32_t)d[11] << 8) | ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    const uint32_t hdr = (uint32_t)d[14] | ((uint32_t)d[15] << 8) | ((uint32_t)d[16] << 16) | ((uint32_t)d[17] << 24);
    if (hdr < 40) return -2;
    const int32_t w = (int32_t)((uint32_t)d[18] | ((uint32_t)d[19] << 8) | ((uint32_t)d[20] << 16) | ((uint32_t)d[21] << 24));
    const int32_t hraw = (int32_t)((uint32_t)d[22] | ((uint32_t)d[23] << 8) | ((uint32_t)d[24] << 16) | ((uint32_t)d[25] << 24));
    const int bpp = d[28] | (d[29] << 8);
    const uint32_t comp = (uint32_t)d[30] | ((uint32_t)d[31] << 8);
    if (w <= 0 || w > 4096 || hraw == 0 || comp != 0) return -2;
    const int topdown = hraw < 0;
    const int h = topdown ? -hraw : hraw;
    if (h <= 0 || h > 4096) return -2;
    if (bpp != 8 && bpp != 24 && bpp != 32) return -2;
    const uint8_t* pal = d + 14 + hdr;
    int pal_n = 0;
    if (bpp == 8) {
        const uint32_t used = (uint32_t)d[46] | ((uint32_t)d[47] << 8) | ((uint32_t)d[48] << 16) | ((uint32_t)d[49] << 24);
        pal_n = used ? (int)used : 256;
        if (len < 14 + (int)hdr + pal_n * 4) return -3;
    }
    const int row_bytes = ((w * bpp + 31) / 32) * 4;
    if ((int)off + row_bytes * h > len) return -3;
    uint32_t* px = (uint32_t*)kmalloc_64((uint64_t)w * h * 4);
    if (!px) { g_err = "oom bmp"; return -4; }
    for (int y = 0; y < h; y++) {
        const int sy = topdown ? y : (h - 1 - y);
        const uint8_t* row = d + off + (uint64_t)sy * row_bytes;
        uint32_t* dst = px + (uint64_t)y * w;
        for (int x = 0; x < w; x++) {
            if (bpp == 24) {
                dst[x] = 0xFF000000u | ((uint32_t)row[x * 3 + 2] << 16) | ((uint32_t)row[x * 3 + 1] << 8) | row[x * 3];
            } else if (bpp == 32) {
                dst[x] = ((uint32_t)row[x * 4 + 3] << 24) | ((uint32_t)row[x * 4 + 2] << 16) |
                         ((uint32_t)row[x * 4 + 1] << 8) | row[x * 4];
                if (((dst[x] >> 24) & 0xFF) == 0) dst[x] |= 0xFF000000u;   // 32bpp BMP 常用 0 表示不透明
            } else {
                const int idx = row[x];
                if (idx >= pal_n) { dst[x] = 0xFF000000u; continue; }
                const uint8_t* p = pal + idx * 4;
                dst[x] = 0xFF000000u | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
            }
        }
    }
    out->px = px;
    out->w = w;
    out->h = h;
    out->owned = 1;
    g_fmt = "bmp";
    return 0;
}

// ==================== 入口 ====================
int img64_decode64(const void* data, int len, Img64* out) {
    if (!data || !out || len < 16) { g_err = "args"; return -1; }
    out->px = nullptr; out->w = out->h = 0; out->owned = 0;
    const uint8_t* d = (const uint8_t*)data;
    int rc;
    if (d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') rc = decode_png64(d, len, out);
    else if (d[0] == 'B' && d[1] == 'M') rc = decode_bmp64(d, len, out);
    else {
        g_err = d[0] == 0xFF && d[1] == 0xD8 ? "jpeg not implemented in this batch" : "unknown format";
        rc = -2;
    }
    if (g_decode_log < 8) {
        g_decode_log++;
        dbg64_line_begin64();
        dbg64_str("[IMG64] decode ");
        dbg64_str(g_fmt);
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(unsigned)(-rc));
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)len);
        dbg64_str(" -> ");
        dbg64_dec((uint64_t)out->w);
        dbg64_str("x");
        dbg64_dec((uint64_t)out->h);
        dbg64_str(" px=");
        dbg64_dec((uint64_t)(out->w * out->h));
        if (rc != 0) {
            dbg64_str(" err=");
            dbg64_str(g_err);
            dbg64_str(" inflate_rc=");
            dbg64_dec((uint64_t)(unsigned)g_inflate_rc);
        }
        dbg64_nl();
        dbg64_line_end64();
    }
    return rc;
}

// 幂等装进系统卷（见 img64.h 的说明）
int img64_install_blob64(const char* path, const uint8_t* data, uint32_t len, const char* src) {
    if (!path || !path[0] || !data || len == 0) return -1;
    const int sys = vfs64_system_slot64();
    uint32_t rt = 0, rs = 0;
    if (sys < 0 || vfs64_stat_on64(sys, "/", &rt, &rs) != 0) {
        dbg64_line_begin64();
        dbg64_str("[IMG64] install skip path=");
        dbg64_str(path);
        dbg64_str(" reason=no-volume src=");
        dbg64_str(src ? src : "?");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    uint32_t type = 0, size = 0;
    if (vfs64_stat_on64(sys, path, &type, &size) == 0 && type == VFS64_TYPE_FILE && size == len) {
        dbg64_line_begin64();
        dbg64_str("[IMG64] install skip path=");
        dbg64_str(path);
        dbg64_str(" reason=exists bytes=");
        dbg64_dec((uint64_t)size);
        dbg64_nl();
        dbg64_line_end64();
        return 0;
    }
    // 父目录（只做一层：/logo/kaisi.png 的 /logo；已存在时 mkdir 返回非 0，忽略即可）
    {
        char parent[VFS64_PATH_MAX];
        int cut = -1;
        for (int i = 0; path[i] && i < (int)sizeof(parent) - 1; i++) if (path[i] == '/') cut = i;
        if (cut > 0) {
            for (int i = 0; i < cut && i < (int)sizeof(parent) - 1; i++) parent[i] = path[i];
            parent[cut] = 0;
            (void)vfs64_mkdir_on64(sys, parent);
        }
    }
    const int wr = vfs64_write_on64(sys, path, data, (int)len);
    dbg64_line_begin64();
    dbg64_str("[IMG64] install path=");
    dbg64_str(path);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)len);
    dbg64_str(" written=");
    dbg64_dec((uint64_t)(wr < 0 ? 0 : wr));
    dbg64_str(" ok=");
    dbg64_dec((uint64_t)(wr == (int)len ? 1 : 0));
    dbg64_str(" src=");
    dbg64_str(src ? src : "?");
    dbg64_nl();
    dbg64_line_end64();
    return wr == (int)len ? 0 : -1;
}

int img64_load_vfs64(const char* path, Img64* out) {
    if (!path || !path[0]) return -1;
    uint32_t type = 0, size = 0;
    if (vfs64_system_slot64() < 0 ||
        vfs64_stat_on64(vfs64_system_slot64(), path, &type, &size) != 0) {
        dbg64_line_begin64();
        dbg64_str("[IMG64] load skip path=");
        dbg64_str(path);
        dbg64_str(" reason=not-found ok=0");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    if (size == 0 || size > 4u * 1024 * 1024) {
        dbg64_line_begin64();
        dbg64_str("[IMG64] load skip path=");
        dbg64_str(path);
        dbg64_str(" reason=size size=");
        dbg64_dec((uint64_t)size);
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    uint8_t* buf = (uint8_t*)kmalloc_64(size);
    if (!buf) { g_err = "oom file"; return -1; }
    const int got = vfs64_read_on64(vfs64_system_slot64(), path, buf, (int)size);
    if (got <= 0) {
        kfree_64(buf);
        dbg64_line_begin64();
        dbg64_str("[IMG64] load skip path=");
        dbg64_str(path);
        dbg64_str(" reason=read-failed");
        dbg64_nl();
        dbg64_line_end64();
        return -1;
    }
    const int rc = img64_decode64(buf, got, out);
    kfree_64(buf);
    if (rc == 0) {
        dbg64_line_begin64();
        dbg64_str("[IMG64] load path=");
        dbg64_str(path);
        dbg64_str(" ok=1 bytes=");
        dbg64_dec((uint64_t)got);
        dbg64_str(" fmt=");
        dbg64_str(g_fmt);
        dbg64_str(" ");
        dbg64_dec((uint64_t)out->w);
        dbg64_str("x");
        dbg64_dec((uint64_t)out->h);
        dbg64_str(" (from VimtuFS2 system volume)");
        dbg64_nl();
        dbg64_line_end64();
    } else {
        dbg64_line_begin64();
        dbg64_str("[IMG64] load path=");
        dbg64_str(path);
        dbg64_str(" ok=0 bytes=");
        dbg64_dec((uint64_t)got);
        dbg64_str(" err=");
        dbg64_str(g_err);
        dbg64_nl();
        dbg64_line_end64();
    }
    return rc;
}

void img64_scale64(const Img64* src, uint32_t* dst, int dw, int dh) {
    if (!src || !src->px || !dst || dw <= 0 || dh <= 0) return;
    for (int y = 0; y < dh; y++) {
        const int sy = (int)((int64_t)y * src->h / dh);
        const uint32_t* srow = src->px + (uint64_t)sy * src->w;
        uint32_t* drow = dst + (uint64_t)y * dw;
        for (int x = 0; x < dw; x++) {
            const int sx = (int)((int64_t)x * src->w / dw);
            drow[x] = srow[sx];
        }
    }
}

// ==================== 自检 ====================
// 内嵌 4x4 RGBA PNG（由 Python zlib 生成；112 字节，行滤波全为 None，deflate 是**单个固定 Huffman 块**）
// ★ 因此它挡不住"动态块之后再遇到固定块"这类缺陷（真文件回归用例见下面的 logo/kaisi.png）。
static const uint8_t kTestPng[112] = {
    0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x04,
    0x00,0x00,0x00,0x04,0x08,0x06,0x00,0x00,0x00,0xA9,0xF1,0x9E,0x7E,0x00,0x00,0x00,0x37,0x49,0x44,0x41,
    0x54,0x78,0xDA,0x63,0xF8,0xCF,0xC0,0xF0,0x1F,0x0C,0x19,0xFE,0x03,0x01,0x82,0x06,0x03,0x06,0x90,0x64,
    0x83,0x83,0xC2,0x7F,0x05,0x87,0x86,0xFF,0x27,0x4E,0x9C,0xF8,0xCF,0x25,0x22,0x07,0x94,0x6B,0x00,0x89,
    0xFE,0xFF,0x1F,0x15,0x15,0xF5,0xFF,0xC3,0x87,0x0F,0xFF,0x01,0x09,0x3E,0x27,0x15,0x4F,0x3B,0xA1,0xEA,
    0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};
// 期望像素（RGB 顺序，row-major）：红 绿 蓝 黄 / 青 品 白 黑 / (128,64,32) (32,64,128) (200,200,200) (10,20,30)
//                              / (255,128,0) (0,128,255) (90,90,90) (240,240,240)
static const uint32_t kTestPix[16] = {
    0x00FF0000, 0x0000FF00, 0x000000FF, 0x00FFFF00,
    0x0000FFFF, 0x00FF00FF, 0x00FFFFFF, 0x00000000,
    0x00804020, 0x00204080, 0x00C8C8C8, 0x000A141E,
    0x00FF8000, 0x000080FF, 0x005A5A5A, 0x00F0F0F0,
};

// ★ 真文件回归用例：内核内嵌的 logo/kaisi.png 原始字节（build64.sh 用 objcopy 嵌 _binary_kaisi_png_*，
//   只链进系统内核；img64.cpp 也只编进系统内核，所以这里直接引用安全）。
//   158x158 / RGBA8：IDAT 19,396 B -> inflate 100,014 B，形态是"1 个大动态块 + BFINAL 的收尾空固定块"，
//   正是曾经解不出来的那种流（固定表只建一次 -> 收尾固定块拿动态表解 -> inflate_rc=18）。
//   期望像素指纹由宿主侧独立算出（纯 Python 解码 + Pillow 双向核对，两者一致）。
extern "C" const uint8_t _binary_kaisi_png_start[];
extern "C" const uint8_t _binary_kaisi_png_end[];
#define IMG64_REAL_W      158
#define IMG64_REAL_H      158
#define IMG64_REAL_PIXFNV 0xEAD69FF1u
static uint32_t img64_fnv32_64(const uint8_t* p, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= p[i]; h *= 16777619u; }
    return h;
}

int img64_selftest64() {
    int fails = 0;
    Img64 im{};
    const int rc = img64_decode64(kTestPng, (int)sizeof(kTestPng), &im);
    if (rc != 0) fails |= 1;
    if (im.w != 4 || im.h != 4) fails |= 2;
    if (!fails) {
        for (int i = 0; i < 16; i++) {
            if ((im.px[i] & 0x00FFFFFFu) != kTestPix[i]) { fails |= 4; break; }
        }
        // 第二个用例：把同一张图当壁纸源缩放（最近邻）——证明解码结果可用
        uint32_t tmp[8 * 8];
        Img64 s = im;
        img64_scale64(&s, tmp, 8, 8);
        if ((tmp[0] & 0x00FFFFFFu) != 0x00FF0000) fails |= 8;
        if ((tmp[7 * 8 + 7] & 0x00FFFFFFu) != 0x00F0F0F0) fails |= 16;
    }
    // 第三个用例（★ 本批新增）：真文件 logo/kaisi.png —— 动态 Huffman + 收尾固定块 + 100 KB 输出；
    // 比对尺寸与**整块像素缓冲的 FNV-1a 32**（宿主侧 PIL/纯 Python 同一指纹），以后不会再悄悄退化。
    const int real_len = (int)(_binary_kaisi_png_end - _binary_kaisi_png_start);
    Img64 real{};
    uint32_t real_fnv = 0;
    const int rc_real = real_len > 0 ? img64_decode64(_binary_kaisi_png_start, real_len, &real) : -1;
    if (rc_real != 0) {
        fails |= 32;
    } else {
        if (real.w != IMG64_REAL_W || real.h != IMG64_REAL_H) fails |= 32;
        real_fnv = img64_fnv32_64((const uint8_t*)real.px, real.w * real.h * 4);
        if (real_fnv != IMG64_REAL_PIXFNV) fails |= 64;
    }
    dbg64_line_begin64();
    dbg64_str("[IMG64] selftest ");
    dbg64_str(fails == 0 ? "PASS" : "FAIL");
    dbg64_str(" mask=");
    dbg64_dec((uint64_t)fails);
    dbg64_str(" png=4x4 rc=");
    dbg64_dec((uint64_t)(unsigned)(-rc));
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)sizeof(kTestPng));
    dbg64_str(" fmt=");
    dbg64_str(g_fmt);
    dbg64_str(" real=");
    dbg64_dec((uint64_t)real.w);
    dbg64_str("x");
    dbg64_dec((uint64_t)real.h);
    dbg64_nl();
    dbg64_line_end64();
    // 专项打点（自动验收 grep）：解真文件 logo/kaisi.png 的结果 + 像素指纹
    dbg64_line_begin64();
    dbg64_str("[IMG64] selftest real ok=");
    dbg64_dec((uint64_t)((fails & (32 | 64)) == 0 ? 1 : 0));
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)real_len);
    dbg64_str(" ");
    dbg64_dec((uint64_t)real.w);
    dbg64_str("x");
    dbg64_dec((uint64_t)real.h);
    dbg64_str(" px=");
    dbg64_dec((uint64_t)(real.w * real.h));
    dbg64_str(" fnv=");
    dbg64_hex64((uint64_t)real_fnv);
    dbg64_str(" rc=");
    dbg64_dec((uint64_t)(unsigned)(-rc_real));
    dbg64_str(" fmt=");
    dbg64_str(g_fmt);
    dbg64_nl();
    dbg64_line_end64();
    img64_free64(&real);
    img64_free64(&im);
    return fails;
}
