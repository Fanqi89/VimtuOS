// font.cpp - TrueType 字体子系统（最小实现，**四个字体面** + UTF-8/CJK + 码点查询链）
// 嵌入字体（构建期子集化，全部是开源字体，来源/版本/许可见 FONTS.md 与 docs/字体许可说明.md）：
//   [0] build/font_bahnschrift.ttf  西文 UI    （Noto Sans 子集，SIL OFL 1.1）
//   [1] build/font_simhei.ttf       中文       （Noto Sans SC 子集，SIL OFL 1.1）
//   [2] build/font_mono.ttf         终端等宽   （Sarasa Mono SC 子集，SIL OFL 1.1；ASCII 宽 = 汉字宽/2）
//   [3] build/font_fallback.ttf     缺字兜底   （GNU Unifont 子集，只收前三个面都没有、界面/终端字面量里出现的码点）
// 特性：cmap format 4（BMP）、hmtx 推进宽度、简单字形扫描线光栅化、
//       ASCII 固定缓存 + CJK LRU 缓存、UTF-8 文本解码、
//       码点查询链（当前面 -> 中文面 -> 兜底面 -> 缺字占位+打点）。
// 中文（CJK）后续：cmap format 12 + 扩展码点。
// 全程无堆分配、无浮点、无 64 位除法（int32 足够）。
#include "font.h"
#include "fb.h"
#include "debug64.h"     // [FONT64] 打点（行锁：dbg64_line_begin64/end64）
#include "demo64.h"      // ★ 中文面外置：原始区 blob 读取（demo64_blob_find64，见本文件顶部取舍说明）
#include "vfs64.h"       // ★ 中文面外置的第二来源：系统卷里可读的 /Fonts-open/NotoSansSC-Regular.ttf
#include <stdint.h>

#define FONT64_LOG_N 16  // [FONT64] fallback hit / glyph miss 的去重表大小（同一码点只打一行）

// ★ A4-4 内核预算：四份字体改成**构建期 deflate 压缩内嵌、启动期解到 .bss**。
//   为什么这样搬（而不是"放卷里、启动期从卷读"）：font_init() 在 kmain 的图形栈初始化里就要用
//   （kernel64.cpp 的 fb_init -> font_init 一段），而系统卷要到 os_boot_path64() 才挂载；更要紧的
//   是验收夹具里有大量"无卷 / 空卷"的盘（desktop64_test 直接引导 system.img；musl64_test /
//   dynlink64_test / ipc64_test / sh64_test 只挂一块**只装了 shell** 的 VimtuFS2 卷）——字体改成
//   "卷里没有就没字"会让这些路径全部退化成豆腐块/无字。压缩内嵌是**行为等价**的搬法：
//   解出来的就是同一份 TTF 字节，face_init 的解析/渲染路径一个字没改。
//   构建期（build64.sh）：build/font_*.ttf -> zlib -9 raw deflate（wbits=-15）+ 8B 未压缩长度头
//   = build64/font_*.z -> objcopy -> _binary_font_*_z_start/_end。
//   体积账（实测）：1,164,272 B -> 755,944 B，**省 408,328 B**（内核 4,037,328 -> 3,629,000 B）。
//   解压缓冲放 .bss：objcopy -O binary 只收 PROGBITS，.bss **不进 kernel64_os.bin**；页池从 128MB
//   起（见 kernel/mem64.cpp 顶部），内核 .bss 本来就含 33MB 后备缓冲，这 ~1.3MB 不影响内存账。
// ★ 本批（内核预算）：**中文面（face 1）改成"可外置"** —— 系统内核不再链 font_simhei_z.o，
//   它的符号是 **weak**：没链进来时地址读回 0，font_init() 就把它标成"外置待装载"
//   （见下面的 g_cjk_ext_*），运行期由 font_external_load64() 从**原始区**（夹具无关的交付位置，
//   见 tools/demo_pack_win.py 的 RAW_EXTRA）把同一份 deflate blob 读回来解到 .bss。
//   为什么这样搬（取舍，如实写清）：
//     * font_init() 在 kmain 的图形栈初始化里就要用 —— 那时**磁盘/vfs 都还没起来**
//       （os_boot_path 里的 ata64_init64 更晚），所以**启动必需的三个面（西文/等宽/兜底 = 24,216 B）
//       必须仍在内核里**（引导日志/早期屏幕就是靠它们画的，ASCII 一个字都不能少）；
//     * 中文面（733,584 B）是唯一的大块：它只在**中文界面**里用得到，而那些界面全部在
//       os_boot_path 的磁盘/vfs 起来之后（桌面/终端/向导）。所以它可以是"磁盘可用后立刻装载"的 ——
//       装载点见 kernel64.cpp 的 ata64_init64() 之后那一行（那一行之后才有屏幕画中文）；
//     * 装载失败（裸盘无原始区、也没卷）**不是失败**：那一档仍然启动，只是中文面缺席
//       （[FONT64] faces=… cjk=0 + selftest 里与中文面相依的三项如实标 na），绝不 PANIC、绝不假装成功。
//   安装程序内核照旧内嵌四份（build64.sh 里安装程序链接行的 font_*_z.o 通配不动）——
//   它的向导在磁盘/卷成型之前就要画中文。
extern "C" const uint8_t _binary_font_bahnschrift_z_start[];
extern "C" const uint8_t _binary_font_bahnschrift_z_end[];
extern "C" const uint8_t _binary_font_simhei_z_start[] __attribute__((weak));
extern "C" const uint8_t _binary_font_simhei_z_end[]   __attribute__((weak));
extern "C" const uint8_t _binary_font_mono_z_start[];
extern "C" const uint8_t _binary_font_mono_z_end[];
extern "C" const uint8_t _binary_font_fallback_z_start[];
extern "C" const uint8_t _binary_font_fallback_z_end[];
// 解压缓冲（.bss）。容量 = 当前子集产物 + 余量：超出即构建期换了更大的字体子集，这里如实拒绝
// （该面 font_ok=false、[FONT64] inflate FAIL），并且 build64.sh 的压缩自检会先把这种改动拦下。
#define FONT_RAM_ASCII_CAP (16u * 1024u)     // 现产物 12,220 B
#define FONT_RAM_CJK_CAP   (1200u * 1024u)   // 现产物 1,119,440 B
#define FONT_RAM_MONO_CAP  (64u * 1024u)     // 现产物 30,344 B
#define FONT_RAM_FALL_CAP  (8u * 1024u)      // 现产物 2,268 B
static uint8_t g_font_ram_ascii[FONT_RAM_ASCII_CAP];
static uint8_t g_font_ram_cjk[FONT_RAM_CJK_CAP];
static uint8_t g_font_ram_mono[FONT_RAM_MONO_CAP];
static uint8_t g_font_ram_fall[FONT_RAM_FALL_CAP];

// ==================== ★ 中文面外置：状态（见上面 extern 段的取舍说明）====================
//   pending  = 外置且还没装载（系统内核、还没到装载点）
//   resolved = 已经"解决"（装载成功，或确认取不到 —— 之后不再重试、selftest 照常打点）
static bool g_cjk_ext_pending  = false;   // 中文面 = 外置待装载
static bool g_cjk_ext_resolved = false;   // 已装载成功 或 已确认取不到
static bool g_cjk_ext_tried    = false;   // 已经尝试过（每次装载点只试一次）
static bool g_selftest_hold    = false;   // font_selftest 被推迟（避免在中文面还没装载时报 cjk=0）

// ---------- raw deflate 解压（stored / fixed / dynamic 三种块）----------
// 算法与 user/gzip/gzip.c 的 inflate 同源（那份在宿主侧与 Python zlib 做过逐字节互操作，
// 并在本文件改动的**宿主自检**里用四份真字体回放过：解出来与 build64/font_*.ttf 逐字节一致）。
// 这里只做"长度已知的整块解压"：不做 gzip 外壳 / CRC 校验，长度不符一律判失败。
struct FontZipIn { const uint8_t* in; uint32_t len; uint32_t pos; uint32_t bitbuf; int bitcnt; int err; };

static int fz_bits(FontZipIn* s, int need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        s->bitbuf |= (uint32_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    const int v = (int)(s->bitbuf & ((1u << need) - 1u));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}
static void fz_align(FontZipIn* s) { s->bitbuf = 0; s->bitcnt = 0; }

struct FontHuff { int16_t count[16]; int16_t sym[288]; };

static int fz_huff_build(FontHuff* h, const uint8_t* lens, int n) {
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    if (h->count[0] == n) return 0;                     // 全 0：合法的"空表"
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;                        // 过完备
    }
    int16_t offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = (int16_t)(offs[len] + h->count[len]);
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = (int16_t)i;
    return left;
}
static int fz_huff_decode(FontZipIn* s, const FontHuff* h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= fz_bits(s, 1);
        if (s->err) return -1;
        const int count = h->count[len];
        if (code - first < count) return h->sym[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}
static void fz_fixed_tables(FontHuff* lit, FontHuff* dist) {
    uint8_t l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    (void)fz_huff_build(lit, l, 288);
    for (int i = 0; i < 30; i++) l[i] = 5;
    (void)fz_huff_build(dist, l, 30);
}
// RFC1951 的长度/距离基值与额外位（与 gzip.c 同表）
static const uint16_t FZ_LEN_BASE[29] = { 3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258 };
static const uint8_t  FZ_LEN_EXTRA[29] = { 0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0 };
static const uint16_t FZ_DIST_BASE[30] = { 1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577 };
static const uint8_t  FZ_DIST_EXTRA[30] = { 0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13 };

// 解一个 block：返回 0 = 还有 block，1 = 最后一个 block，2 = 坏流
static int fz_block(FontZipIn* s, uint8_t* out, uint32_t outcap, uint32_t* outlen) {
    uint32_t o = *outlen;
    const int final = fz_bits(s, 1);
    const int type  = fz_bits(s, 2);
    if (s->err) return 2;
    if (type == 0) {
        fz_align(s);
        if (s->pos + 4u > s->len) return 2;
        const uint32_t blen = (uint32_t)s->in[s->pos] | ((uint32_t)s->in[s->pos + 1] << 8);
        const uint32_t nlen = (uint32_t)s->in[s->pos + 2] | ((uint32_t)s->in[s->pos + 3] << 8);
        s->pos += 4;
        if ((blen ^ 0xFFFFu) != nlen) return 2;
        if (s->pos + blen > s->len || blen > outcap - o) return 2;
        for (uint32_t k = 0; k < blen; k++) out[o++] = s->in[s->pos++];
    } else if (type == 1 || type == 2) {
        FontHuff lit, dist;
        if (type == 1) {
            fz_fixed_tables(&lit, &dist);
        } else {
            const int hlit  = fz_bits(s, 5) + 257;
            const int hdist = fz_bits(s, 5) + 1;
            const int hclen = fz_bits(s, 4) + 4;
            if (s->err || hlit > 286 || hdist > 30) return 2;
            static const uint8_t ORD[19] = { 16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15 };
            uint8_t cl[19];
            for (int i = 0; i < 19; i++) cl[i] = 0;
            for (int i = 0; i < hclen; i++) cl[ORD[i]] = (uint8_t)fz_bits(s, 3);
            if (s->err) return 2;
            FontHuff clh;
            if (fz_huff_build(&clh, cl, 19) < 0) return 2;
            uint8_t lens[320];
            int n = 0;
            while (n < hlit + hdist) {
                const int sym = fz_huff_decode(s, &clh);
                if (sym < 0 || s->err) return 2;
                if (sym < 16) {
                    lens[n++] = (uint8_t)sym;
                } else if (sym == 16) {
                    if (n == 0) return 2;
                    int rep = fz_bits(s, 2) + 3;
                    const uint8_t prev = lens[n - 1];
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = prev; }
                } else if (sym == 17) {
                    int rep = fz_bits(s, 3) + 3;
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = 0; }
                } else {
                    int rep = fz_bits(s, 7) + 11;
                    while (rep-- > 0) { if (n >= hlit + hdist) return 2; lens[n++] = 0; }
                }
                if (s->err) return 2;
            }
            if (lens[256] == 0) return 2;               // 没有 end-of-block 码
            if (fz_huff_build(&lit, lens, hlit) < 0) return 2;
            if (fz_huff_build(&dist, lens + hlit, hdist) < 0) return 2;
        }
        for (;;) {
            const int sym = fz_huff_decode(s, &lit);
            if (sym < 0 || s->err) return 2;
            if (sym < 256) {
                if (o >= outcap) return 2;
                out[o++] = (uint8_t)sym;
                continue;
            }
            if (sym == 256) break;
            const int li = sym - 257;
            if (li >= 29) return 2;
            const uint32_t len = (uint32_t)FZ_LEN_BASE[li] + (uint32_t)fz_bits(s, FZ_LEN_EXTRA[li]);
            const int dsym = fz_huff_decode(s, &dist);
            if (dsym < 0 || dsym >= 30 || s->err) return 2;
            const uint32_t d = (uint32_t)FZ_DIST_BASE[dsym] + (uint32_t)fz_bits(s, FZ_DIST_EXTRA[dsym]);
            if (s->err || d == 0 || d > o) return 2;    // 回溯越界 = 坏流
            if (len > outcap - o) return 2;
            for (uint32_t k = 0; k < len; k++) { out[o] = out[o - d]; o++; }
        }
    } else {
        return 2;                                        // BTYPE=11 保留
    }
    *outlen = o;
    return final ? 1 : 0;
}

// 解一份 font_*.z（[0..7] = 未压缩长度（小端 u64）+ raw deflate 流）到 dst。
// 成功返回 0 并把实际长度写到 *out_len；任何不一致（截断/坏表/长度不符/超容量）都返回 -1。
static int font_unpack64(const uint8_t* src, const uint8_t* end, uint8_t* dst, uint32_t cap, uint32_t* out_len) {
    if (!src || !end || end - src < 9) return -1;
    const uint32_t want = (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
    for (int i = 4; i < 8; i++) if (src[i] != 0) return -1;    // 字体 < 4 GiB：高 4 字节必须为 0
    if (want == 0 || want > cap) return -1;
    FontZipIn s;
    s.in = src + 8; s.len = (uint32_t)(end - (src + 8)); s.pos = 0; s.bitbuf = 0; s.bitcnt = 0; s.err = 0;
    uint32_t o = 0;
    for (;;) {
        const int rc = fz_block(&s, dst, want, &o);
        if (rc == 2) return -1;
        if (rc == 1) break;
    }
    if (s.err || o != want) return -1;
    *out_len = o;
    return 0;
}

#define FONT_PX 20            // 光栅化缓冲（em 高像素 + descender 余量）
#define FONT_SIZE_PX 16       // 字号（em 高度，像素）—— ★ P3 起只是**默认档**，运行期用 g_em_px
#define FONT_SIZE_MIN 12      // font_set_size64() 的下限
#define FONT_SIZE_MAX 18      // 上限 18：光栅缓冲 FONT_PX=20，再大字形会被裁（如实不让选）
static int g_em_px = FONT_SIZE_PX;    // 当前 em 高度（font_set_size64 改它；默认 16 = 历史值）
#define FONT_LINE_HEIGHT 20   // 历史行高常量（em=16 时的值；运行期行高见 font_line_height()）
#define FONT_CACHE_N 128      // ASCII 固定缓存 0x20..0x7F
// 非 ASCII（CJK）LRU 缓存槽数。
// 256 而不是 192：系统级预加载（preload.cpp）一次预热 193 个界面常用汉字，
// 容量若小于预热集，每轮预热都会互相淘汰 → `preload run` 无法幂等（每次重装 ~190 字）。
// 内存代价：每槽 FONT_PX*FONT_PX = 400B，+64 槽 = +25.6KB/face（可接受）。
#define FONT_LRU_N 256
#define MAX_GLYPH_PTS 512     // 单字形最大点数（CJK 字形较大）
#define MAX_EDGES 4096        // 扁平化后最大线段数
#define MAX_XS 1024           // 每行最多交点

struct FontFace {
    const uint8_t* F;
    uint32_t Fsize;
    bool font_ok;
    uint16_t unitsPerEm;
    int32_t  scaleFix;        // FONT_PX*1024 / unitsPerEm（1/1024 px 定点）
    int32_t  ascFix;          // ascender * scaleFix
    uint16_t numGlyphs;
    uint16_t indexToLocFormat;
    uint32_t offHead, offHhea, offHmtx, offMaxp, offLoca, offGlyf, offCmap;
    uint16_t numHMetrics;
    const uint8_t* cmap4;
    uint16_t segCount;
    int advPx[FONT_CACHE_N];
    bool cacheOk[FONT_CACHE_N];
    uint8_t cache[FONT_CACHE_N][FONT_PX * FONT_PX];
    int cacheW[FONT_CACHE_N], cacheH[FONT_CACHE_N];
    int cacheOX[FONT_CACHE_N], cacheOY[FONT_CACHE_N];
    // CJK LRU 缓存
    uint32_t lru_key[FONT_LRU_N];      // 0=空
    uint32_t lru_age[FONT_LRU_N];
    uint8_t lru_bm[FONT_LRU_N][FONT_PX * FONT_PX];
    int lruW[FONT_LRU_N], lruH[FONT_LRU_N];
    int lruOX[FONT_LRU_N], lruOY[FONT_LRU_N];
    uint32_t lru_tick;
};

static FontFace g_face[FONT_FACE_COUNT];   // [0]=西文 [1]=中文 [2]=终端等宽 [3]=缺字兜底
static FontFace* cur = &g_face[FONT_FACE_ASCII];

// 当前字形扁平化边表（渲染临时区，单线程，可共享）
static int32_t ex0[MAX_EDGES], ey0[MAX_EDGES], ex1[MAX_EDGES], ey1[MAX_EDGES];
static int ne;
struct GPoint { int32_t x, y; bool on; };
static GPoint cpts[MAX_GLYPH_PTS * 2];

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline int16_t be16s(const uint8_t* p) { return (int16_t)be16(p); }
static inline uint32_t be32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---------------- UTF-8 解码 ----------------
// 返回码点（0=非法），adv 为消耗字节数
static uint32_t utf8_decode(const char* s, int* adv) {
    const uint8_t* p = (const uint8_t*)s;
    uint8_t b0 = p[0];
    if (b0 < 0x80) { *adv = 1; return b0; }
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        if (!p[1]) { *adv = 1; return 0; }
        *adv = 2;
        return ((uint32_t)(b0 & 0x1F) << 6) | (p[1] & 0x3F);
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (!p[1] || !p[2]) { *adv = 1; return 0; }
        *adv = 3;
        return ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    }
    *adv = 1;
    return 0;
}

// ---------------- cmap format 4 ----------------
static uint16_t cmap_lookup(FontFace* fc, uint16_t cp) {
    if (!fc->cmap4 || fc->segCount == 0) return cp;
    const uint8_t* cmap4 = fc->cmap4;
    for (uint16_t s = 0; s < fc->segCount; s++) {
        uint16_t end = be16(cmap4 + 14 + 2 * s);
        if (cp <= end) {
            uint16_t start = be16(cmap4 + 14 + 2 * fc->segCount + 2 + 2 * s);
            if (cp >= start) {
                uint16_t delta = be16(cmap4 + 14 + 4 * fc->segCount + 2 + 2 * s);
                uint16_t ro = be16(cmap4 + 14 + 6 * fc->segCount + 2 + 2 * s);
                if (ro == 0) return (uint16_t)(cp + delta);
                uint16_t gi = be16(cmap4 + 14 + 6 * fc->segCount + 2 + 2 * s + ro + 2 * (cp - start));
                return (gi == 0) ? 0 : (uint16_t)(gi + delta);
            }
            break;
        }
    }
    return 0;
}

static uint32_t loca_offset(FontFace* fc, uint16_t g) {
    if (fc->indexToLocFormat == 0) return (uint32_t)be16(fc->F + fc->offLoca + 2 * g) * 2;
    return be32(fc->F + fc->offLoca + 4 * g);
}

// ---- ★ P3：字号档（em 像素高）→ 面的 scaleFix / ascFix / ASCII 推进宽度 ----
// face_init 与 font_set_size64() 共用；只依赖 unitsPerEm / hhea.ascender / hmtx（cmap 只在
// face_init 的第二次调用时才就绪 —— 第一次调用只算 scale/asc，advPx 留给第二次）。
static void face_metrics(FontFace* fc) {
    const int16_t asc = be16s(fc->F + fc->offHhea + 4);
    fc->scaleFix = (g_em_px * 1024) / fc->unitsPerEm;
    if (fc->scaleFix < 1) fc->scaleFix = 1;
    fc->ascFix = (int32_t)asc * fc->scaleFix;
    if (!fc->cmap4) return;                       // cmap 还没解析：只算 scale
    for (int i = 0; i < FONT_CACHE_N; i++) {
        uint16_t g = cmap_lookup(fc, (uint16_t)i);
        if (g >= fc->numGlyphs) g = 0;
        if (g >= fc->numHMetrics) g = (uint16_t)(fc->numHMetrics - 1);
        const uint16_t aw = be16(fc->F + fc->offHmtx + 4 * g);
        int px = (int)(((int32_t)aw * fc->scaleFix + 512) >> 10);
        if (px < 1) px = 1;
        fc->advPx[i] = px;
    }
}

// ---------------- 初始化 ----------------
static void face_init(FontFace* fc, const uint8_t* start, const uint8_t* end) {
    fc->F = start;
    fc->Fsize = (uint32_t)(end - start);
    fc->font_ok = false;
    fc->cmap4 = nullptr;
    fc->segCount = 0;
    fc->lru_tick = 0;
    for (int i = 0; i < FONT_CACHE_N; i++) {
        fc->cacheOk[i] = false;
        fc->advPx[i] = 1;
    }
    for (int i = 0; i < FONT_LRU_N; i++) fc->lru_key[i] = 0;
    if (!fc->F || fc->Fsize < 12) return;
    uint32_t ver = be32(fc->F);
    if (ver != 0x00010000 && ver != 0x74727565 && ver != 0x4F54544F) return;  // 1.0 / true / OTTO
    uint16_t nTables = be16(fc->F + 4);
    if (nTables > 60) return;
    fc->offHead = fc->offHhea = fc->offHmtx = fc->offMaxp = 0;
    fc->offLoca = fc->offGlyf = fc->offCmap = 0;
    for (uint16_t i = 0; i < nTables; i++) {
        const uint8_t* rec = fc->F + 12 + 16 * i;
        uint32_t tag = be32(rec);
        uint32_t off = be32(rec + 8);
        if (off > fc->Fsize) continue;
        if (tag == 0x68656164) fc->offHead = off;          // head
        else if (tag == 0x68686561) fc->offHhea = off;     // hhea
        else if (tag == 0x686D7478) fc->offHmtx = off;     // hmtx
        else if (tag == 0x6D617870) fc->offMaxp = off;     // maxp
        else if (tag == 0x6C6F6361) fc->offLoca = off;     // loca
        else if (tag == 0x676C7966) fc->offGlyf = off;     // glyf
        else if (tag == 0x636D6170) fc->offCmap = off;     // cmap
    }
    if (!fc->offHead || !fc->offHhea || !fc->offHmtx || !fc->offMaxp || !fc->offLoca || !fc->offGlyf || !fc->offCmap) return;

    fc->unitsPerEm = be16(fc->F + fc->offHead + 18);
    fc->indexToLocFormat = be16(fc->F + fc->offHead + 50);
    fc->numHMetrics = be16(fc->F + fc->offHhea + 34);
    if (fc->numHMetrics == 0) fc->numHMetrics = 1;
    fc->numGlyphs = be16(fc->F + fc->offMaxp + 4);
    if (fc->numGlyphs == 0) return;

    int16_t asc = be16s(fc->F + fc->offHhea + 4);
    (void)asc;
    fc->cmap4 = nullptr;
    face_metrics(fc);

    // cmap：优先 Windows Unicode BMP（platform 3 / encoding 1）的 format 4
    uint16_t nTabs = be16(fc->F + fc->offCmap + 2);
    for (uint16_t i = 0; i < nTabs; i++) {
        const uint8_t* t = fc->F + fc->offCmap + 4 + 8 * i;
        uint16_t plat = be16(t), enc = be16(t + 2);
        uint32_t off = be32(t + 4);
        if (off + 2 <= fc->Fsize && be16(fc->F + fc->offCmap + off) == 4) {
            if (plat == 3 && enc == 1) { fc->cmap4 = fc->F + fc->offCmap + off; break; }
            if (!fc->cmap4) fc->cmap4 = fc->F + fc->offCmap + off;   // 备用：任意 format 4
        }
    }
    if (!fc->cmap4) fc->cmap4 = fc->F + fc->offCmap;  // 退化：恒等映射
    fc->segCount = (be16(fc->cmap4 + 6) >= 2) ? be16(fc->cmap4 + 6) / 2 : 0;

    // 预计算 ASCII 推进宽度（cmap 就绪后才能查；见 face_metrics）
    face_metrics(fc);
    fc->font_ok = true;
}

void font_init() {
    // ★ 内核预算：四份字体是 deflate 压缩内嵌（见文件顶部）。这里解到 .bss 缓冲再交给 face_init ——
    //   解压失败的面按"字体加载失败"处理（空指针 + 长度 0 -> font_ok=false），绝不拿半截数据解析。
    struct FontBlob { const uint8_t* zs; const uint8_t* ze; uint8_t* ram; uint32_t cap; };
    const FontBlob blobs[FONT_FACE_COUNT] = {
        { _binary_font_bahnschrift_z_start, _binary_font_bahnschrift_z_end, g_font_ram_ascii, FONT_RAM_ASCII_CAP },
        { _binary_font_simhei_z_start,      _binary_font_simhei_z_end,      g_font_ram_cjk,   FONT_RAM_CJK_CAP   },
        { _binary_font_mono_z_start,        _binary_font_mono_z_end,        g_font_ram_mono,  FONT_RAM_MONO_CAP  },
        { _binary_font_fallback_z_start,    _binary_font_fallback_z_end,    g_font_ram_fall,  FONT_RAM_FALL_CAP  },
    };
    uint32_t total_raw = 0, total_z = 0;
    int failed = 0;
    for (int i = 0; i < FONT_FACE_COUNT; i++) {
        // ★ 中文面外置（系统内核没链 font_simhei_z.o，符号是 weak）：标成"待装载"，
        //   不算 failed、也不在这里打 cjk=0 —— 由 font_external_load64() 在磁盘起来后解决。
        if (i == FONT_FACE_CJK && blobs[i].zs == nullptr) {
            g_cjk_ext_pending  = true;
            g_cjk_ext_resolved = false;
            face_init(&g_face[i], blobs[i].ram, blobs[i].ram);      // 空：font_ok=false（等外置装载）
            continue;
        }
        total_z += (uint32_t)(blobs[i].ze - blobs[i].zs);
        uint32_t n = 0;
        if (font_unpack64(blobs[i].zs, blobs[i].ze, blobs[i].ram, blobs[i].cap, &n) != 0) {
            failed++;
            face_init(&g_face[i], blobs[i].ram, blobs[i].ram);      // 空：font_ok=false（selftest 会如实报）
            continue;
        }
        total_raw += n;
        face_init(&g_face[i], blobs[i].ram, blobs[i].ram + n);
    }
    cur = &g_face[FONT_FACE_ASCII];
    // 打点（体积账的实测证据：raw = 解出来的 TTF 总字节，packed = 内核里内嵌的压缩字节）
    dbg64_line_begin64();
    dbg64_str(failed ? "[FONT64] inflate FAIL faces=" : "[FONT64] inflate ok faces=");
    // ★ 中文面外置时 faces= 只数"真的在核里解出来的面"（外置那面由 ext=1 表达，别把 4 报成 3 或反过来）
    dbg64_dec((uint64_t)(FONT_FACE_COUNT - failed - (g_cjk_ext_pending ? 1 : 0)));
    dbg64_str(" raw=");
    dbg64_dec(total_raw);
    if (g_cjk_ext_pending) dbg64_str(" ext=1(cjk)");
    dbg64_str(" packed=");
    dbg64_dec(total_z);
    dbg64_str(" saved=");
    dbg64_dec(total_raw > total_z ? (uint64_t)(total_raw - total_z) : 0);
    dbg64_nl();
    dbg64_line_end64();
    if (g_cjk_ext_pending) {
        // 如实打点：中文面是外置的、还没装载（装载点 = kernel64.cpp 里磁盘/卷起来之后那两行）。
        // 这里**不**打 cjk=0 / FAIL —— 那不是失败，只是"还没到装载点"。
        dbg64_line_begin64();
        dbg64_str("[FONT64] cjk face external pending (not embedded; load point = after disk/vfs ready)\n");
        dbg64_line_end64();
    }
}

// ==================== ★ 中文面外置装载（幂等；kernel64.cpp 在磁盘/vfs 起来之后调用）====================
// 两个来源，按"夹具无关"的优先级：
//   ① **原始区 blob**（/etc/font_simhei.z，deflate：8B 小端未压缩长度 + raw deflate）——
//      夹具盘都是"system.img 的字节 + 空卷"，只有原始区在每个夹具盘里都有（见 tools/demo_pack_win.py）；
//   ② 系统卷里的**可读 TTF**（/Fonts-open/NotoSansSC-Regular.ttf）—— 全量演示盘（sysdisk.img）里有，
//      用户自建卷里没有；UEFI/ESP 夹具这种"引导盘不是 system.img"的档只能走这条。
// 两个都取不到 = 中文面缺席（g_cjk_ext_resolved=true 后不再重试；selftest 照常打点、与中文面相依的
// 三项如实标 na）—— 系统照常启动，绝不 PANIC、绝不假装成功。
#define FONT_CJK_RAW_PATH "/etc/font_simhei.z"
#define FONT_CJK_VFS_PATH "/Fonts-open/NotoSansSC-Regular.ttf"

static int font_cjk_apply64(const uint8_t* src, uint32_t src_len, bool deflated, const char* what) {
    if (!src || src_len < 16u) return -1;
    uint32_t n = 0;
    if (deflated) {
        if (font_unpack64(src, src + src_len, g_font_ram_cjk, FONT_RAM_CJK_CAP, &n) != 0) return -1;
    } else {
        if (src_len > FONT_RAM_CJK_CAP) return -1;
        if (!(src[0] == 0x00u && src[1] == 0x01u && src[2] == 0x00u && src[3] == 0x00u)) return -1;   // TTF magic
        for (uint32_t i = 0; i < src_len; i++) g_font_ram_cjk[i] = src[i];   // 已是可读 TTF：拷进同一块 .bss 面缓冲
        n = src_len;
    }
    if (n < 1024u) return -1;
    face_init(&g_face[FONT_FACE_CJK], g_font_ram_cjk, g_font_ram_cjk + n);
    dbg64_line_begin64();
    dbg64_str("[FONT64] cjk external load src=");
    dbg64_str(what);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)n);
    dbg64_str(" face_ok=");
    dbg64_dec(g_face[FONT_FACE_CJK].font_ok ? 1u : 0u);
    dbg64_nl();
    dbg64_line_end64();
    return g_face[FONT_FACE_CJK].font_ok ? 0 : -1;
}

// 返回 0 = 中文面已就绪；1 = 这次没成功（可能是"磁盘还没起来"，可再调）；2 = 已确认取不到（不再重试）
int font_external_load64() {
    if (!g_cjk_ext_pending) return 0;                       // 内嵌（安装程序内核 / 老构建）：什么都不用做
    if (g_face[FONT_FACE_CJK].font_ok) { g_cjk_ext_resolved = true; return 0; }
    if (g_cjk_ext_resolved) return 2;                       // 已确认取不到：不再磨盘
    g_cjk_ext_tried = true;

    // ① 原始区 blob（defalte；夹具无关）
    {
        uint32_t zlen = 0;
        const uint8_t* z = demo64_blob_find64(FONT_CJK_RAW_PATH, &zlen);
        if (z && font_cjk_apply64(z, zlen, true, "raw") == 0) { g_cjk_ext_resolved = true; if (g_selftest_hold) font_selftest(); return 0; }
    }
    // ② 系统卷里的可读 TTF
    {
        const int sys = vfs64_system_slot64();
        if (sys >= 0 && vfs64_slot_used64(sys)) {
            uint32_t ty = 0, sz = 0;
            if (vfs64_stat_on64(sys, FONT_CJK_VFS_PATH, &ty, &sz) == 0 && ty == VFS64_TYPE_FILE &&
                sz > 1024u && sz <= FONT_RAM_CJK_CAP) {
                const int got = vfs64_read_on64(sys, FONT_CJK_VFS_PATH, g_font_ram_cjk, (int)sz);
                if (got > 1024 && font_cjk_apply64(g_font_ram_cjk, (uint32_t)got, false, "vfs") == 0) {
                    g_cjk_ext_resolved = true;
                    if (g_selftest_hold) font_selftest();
                    return 0;
                }
            }
        }
    }
    return 1;                                               // 这次没到手（下次装载点还可以再试）
}

// 装载点收尾：两个装载点（早/晚）都跑完还是取不到 —— 如实把中文面标成"缺席"，
// 让 selftest 能打点（cjk=0 + 与中文面相依的三项标 na）。语义："不假装成功、也不假装失败"。
void font_external_give_up64() {
    if (!g_cjk_ext_pending || g_cjk_ext_resolved) return;
    g_cjk_ext_resolved = true;
    dbg64_line_begin64();
    dbg64_str("[FONT64] cjk face unavailable (no raw blob and no volume font): CJK will render as boxes; not a failure)\n");
    dbg64_line_end64();
    if (g_selftest_hold) font_selftest();
}

void font_select(int face) {
    if (face < 0) face = 0;
    if (face >= FONT_FACE_COUNT) face = FONT_FACE_COUNT - 1;
    cur = &g_face[face];
}

int font_face_count() { return FONT_FACE_COUNT; }

// ★ 只读：当前面（console64 的引导日志画完之后把面恢复回去；不改渲染语义）
int font_current_face() {
    for (int f = 0; f < FONT_FACE_COUNT; f++) if (cur == &g_face[f]) return f;
    return FONT_FACE_ASCII;
}

// ---------------- 扁平化 ----------------
static void add_line(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    if (ne < MAX_EDGES) { ex0[ne] = x0; ey0[ne] = y0; ex1[ne] = x1; ey1[ne] = y1; ne++; }
}

static void add_quad(int32_t x0, int32_t y0, int32_t cx, int32_t cy, int32_t x1, int32_t y1) {
    // 固定 6 段细分：相邻采样点连成直线
    int32_t px = x0, py = y0;
    for (int i = 1; i <= 6; i++) {
        int32_t t = i, om = 6 - i;
        int32_t bx = (om * om * x0 + 2 * om * t * cx + t * t * x1) / 36;
        int32_t by = (om * om * y0 + 2 * om * t * cy + t * t * y1) / 36;
        add_line(px, py, bx, by);
        px = bx; py = by;
    }
}

// ---------------- 字形光栅化 ----------------
// 输出：bm[FONT_PX*FONT_PX]（1=前景），并给出 bbox（OX,OY,W,H）
// cp: Unicode 码点（BMP）
static bool rasterize_glyph(FontFace* fc, uint32_t cp, uint8_t* bm, int* ox, int* oy, int* w, int* h) {
    *w = 0; *h = 0; *ox = 0; *oy = 0;
    if (!fc->font_ok) return false;
    if (cp > 0xFFFF) return false;
    uint16_t g = cmap_lookup(fc, (uint16_t)cp);
    if (g >= fc->numGlyphs) return false;
    uint32_t l0 = loca_offset(fc, g), l1 = loca_offset(fc, (uint16_t)(g + 1));
    if (l1 <= l0) return true;                       // 空白字形
    const uint8_t* gp = fc->F + fc->offGlyf + l0;
    if (l0 + 10 > fc->Fsize) return false;
    int16_t contours = be16s(gp);
    if (contours < 0) return false;                  // 复合字形暂不支持
    if (contours == 0) return true;

    uint16_t endPtsOff = 10;                 // 头部: contours(2)+bbox(8)
    uint16_t nPts = be16(gp + endPtsOff + 2 * (contours - 1)) + 1;   // 末端点 + 1
    if (nPts == 0 || nPts > MAX_GLYPH_PTS) return false;
    if (l0 + endPtsOff + 2 * contours + 2 > fc->Fsize) return false;

    // 读取 flags / x / y
    uint32_t p = endPtsOff + 2 * contours + 2;
    uint16_t insLen = be16(gp + endPtsOff + 2 * contours);
    p += insLen;

    uint8_t flags[MAX_GLYPH_PTS];
    int fi = 0;
    while (fi < nPts) {
        if (fc->offGlyf + l0 + p >= fc->Fsize) return false;
        uint8_t fl = fc->F[fc->offGlyf + l0 + p++];
        flags[fi++] = fl;
        if (fl & 0x08) {
            if (fc->offGlyf + l0 + p >= fc->Fsize) return false;
            int rep = fc->F[fc->offGlyf + l0 + p++];
            while (rep-- && fi < nPts) flags[fi++] = fl;
        }
    }
    int32_t xs[MAX_GLYPH_PTS], ys[MAX_GLYPH_PTS];
    int32_t v = 0;
    for (uint16_t i = 0; i < nPts; i++) {
        if (flags[i] & 0x02) {
            // 短向量：1 字节无符号幅值，符号由 bit4 决定（bit4=1 正）
            if (fc->offGlyf + l0 + p >= fc->Fsize) return false;
            uint8_t d = fc->F[fc->offGlyf + l0 + p++];
            xs[i] = v + (flags[i] & 0x10 ? (int32_t)d : -(int32_t)d);
        } else if (flags[i] & 0x10) {
            xs[i] = v;
        } else {
            if (fc->offGlyf + l0 + p + 2 > fc->Fsize) return false;
            xs[i] = v + (int16_t)be16(fc->F + fc->offGlyf + l0 + p);
            p += 2;
        }
        v = xs[i];
    }
    v = 0;
    for (uint16_t i = 0; i < nPts; i++) {
        if (flags[i] & 0x04) {
            // 短向量：1 字节无符号幅值，符号由 bit5 决定（bit5=1 正）
            if (fc->offGlyf + l0 + p >= fc->Fsize) return false;
            uint8_t d = fc->F[fc->offGlyf + l0 + p++];
            ys[i] = v + (flags[i] & 0x20 ? (int32_t)d : -(int32_t)d);
        } else if (flags[i] & 0x20) {
            ys[i] = v;
        } else {
            if (fc->offGlyf + l0 + p + 2 > fc->Fsize) return false;
            ys[i] = v + (int16_t)be16(fc->F + fc->offGlyf + l0 + p);
            p += 2;
        }
        v = ys[i];
    }

    // 缩放为 1/1024 px 定点
    for (uint16_t i = 0; i < nPts; i++) {
        xs[i] = xs[i] * fc->scaleFix;
        ys[i] = ys[i] * fc->scaleFix;
    }

    // 逐轮廓构建（从最后一个 on 点开始，含隐式中点）
    ne = 0;
    for (uint16_t ci = 0; ci < (uint16_t)contours; ci++) {
        uint16_t endIdx = be16(gp + endPtsOff + 2 * ci);
        if (endIdx >= nPts) return false;
        int startIdx = (ci == 0) ? 0 : (be16(gp + endPtsOff + 2 * (ci - 1)) + 1);
        int cnt = endIdx - startIdx + 1;
        if (cnt <= 0) continue;

        // 找最后一个 on 点
        int lastOn = -1;
        for (int i = 0; i < cnt; i++) {
            if (flags[startIdx + i] & 1) lastOn = i;
        }
        if (lastOn < 0) continue;   // 全 off，忽略该轮廓

        int cn = 0;
        for (int k = 0; k < cnt; k++) {
            int i = startIdx + (lastOn + k) % cnt;
            cpts[cn].x = xs[i]; cpts[cn].y = ys[i];
            cpts[cn].on = (flags[i] & 1) != 0;
            cn++;
        }
        int32_t sx = cpts[0].x, sy = cpts[0].y;
        int32_t px = sx, py = sy;
        int32_t cxc = 0, cyc = 0; bool haveC = false;
        for (int k = 1; k < cn; k++) {
            if (cpts[k].on) {
                if (haveC) add_quad(px, py, cxc, cyc, cpts[k].x, cpts[k].y);
                else add_line(px, py, cpts[k].x, cpts[k].y);
                px = cpts[k].x; py = cpts[k].y; haveC = false;
            } else {
                if (haveC) {
                    int32_t mx = (cxc + cpts[k].x) / 2, my = (cyc + cpts[k].y) / 2;
                    add_quad(px, py, cxc, cyc, mx, my);
                    px = mx; py = my;
                }
                cxc = cpts[k].x; cyc = cpts[k].y; haveC = true;
            }
        }
        if (haveC) add_quad(px, py, cxc, cyc, sx, sy);
        else add_line(px, py, sx, sy);
    }

    if (ne == 0) return true;

    // 边表 bbox（1/1024 px）
    int32_t minX = INT32_MAX, maxX = -INT32_MAX, minY = INT32_MAX, maxY = -INT32_MAX;
    for (int i = 0; i < ne; i++) {
        int32_t a = ex0[i], b = ex1[i];
        if (a < minX) minX = a;
        if (b > maxX) maxX = b;
        if (a > maxX) maxX = a;
        if (b < minX) minX = b;
        a = ey0[i]; b = ey1[i];
        if (a < minY) minY = a;
        if (b > maxY) maxY = b;
        if (a > maxY) maxY = a;
        if (b < minY) minY = b;
    }
    // 像素 bbox（钳制到 0..FONT_PX-1）。
    // 注意：字形坐标 y 向上为正（TrueType 原点在基线），行号必须从 ascFix 换算：
    //   yc = ascFix - row*1024 - 512  →  row = (ascFix - y - 512) >> 10
    int px0 = (int)(minX >> 10); if (px0 < 0) px0 = 0;
    int px1 = (int)((maxX + 1023) >> 10); if (px1 > FONT_PX - 1) px1 = FONT_PX - 1;
    int py0 = (int)((fc->ascFix - maxY - 512) >> 10); if (py0 < 0) py0 = 0;
    int py1 = (int)((fc->ascFix - minY - 512) >> 10); if (py1 > FONT_PX - 1) py1 = FONT_PX - 1;
    if (py0 > FONT_PX - 1) py0 = FONT_PX - 1;
    if (px1 < px0 || py1 < py0) return true;
    *ox = px0; *oy = py0; *w = px1 - px0 + 1; *h = py1 - py0 + 1;
    // ★ P7a ④ 修复（文字渲染重叠错乱）：本函数是**累加**语义（同一字形的轮廓自重叠要靠
    //   `old + a` 叠加覆盖率，见下面扫描线填充），所以填充前必须把目标位图的 bbox 区域清零。
    //   调用方的位图缓冲是**复用**的，而且从来没被清过（本文件里没有任何 memset）：
    //     * font_draw_glyph -> cur->cache[idx]：换字号（font_set_size64）把 cacheOk 全置 false
    //       却没清位图 -> 重光栅化时把**旧字号的同一字形**叠进来（字变粗/重影）；
    //     * lru_get_slot    -> fc->lru_bm[victim]：CJK LRU 槽换出后装的是**另一个字形**，
    //       叠上来就是"上一个字残影"= 用户实测的偶发文字重叠错乱（取决于换出顺序，所以偶发）。
    //   只清 bbox（下面填充只会写这个范围），不动别处、不改首次渲染的像素（fonts64 逐像素不变）。
    for (int row = py0; row <= py1; row++) {
        uint8_t* dst = bm + row * FONT_PX;
        for (int xx = px0; xx <= px1; xx++) dst[xx] = 0;
    }


    // 扫描线填充（偶奇规则）
    int32_t xs_cross[MAX_XS];
    for (int row = py0; row <= py1; row++) {
        int32_t yc = fc->ascFix - ((int32_t)row * 1024 + 512);   // 像素中心在字形坐标系的 y
        int nxs = 0;
        for (int i = 0; i < ne; i++) {
            int32_t y0 = ey0[i], y1 = ey1[i];
            if (y0 == y1) continue;
            // 判断 yc 是否在 [min(y0,y1), max(y0,y1)) 内；
            // 注意：插值必须用原始 y0/y1（不交换坐标，否则 x 会算错）
            int32_t ylo = (y0 < y1) ? y0 : y1;
            int32_t yhi = (y0 > y1) ? y0 : y1;
            if (yc < ylo || yc >= yhi) continue;
            int32_t dx = ex1[i] - ex0[i];
            int32_t x = ex0[i] + (yc - y0) * dx / (y1 - y0);
            if (nxs < MAX_XS) xs_cross[nxs++] = x;
        }
        // 排序（插入排序，交点通常很少）
        for (int i = 1; i < nxs; i++) {
            int32_t key = xs_cross[i];
            int j = i - 1;
            while (j >= 0 && xs_cross[j] > key) { xs_cross[j + 1] = xs_cross[j]; j--; }
            xs_cross[j + 1] = key;
        }
        for (int k = 0; k + 1 < nxs; k += 2) {
            int32_t xa = xs_cross[k], xb = xs_cross[k + 1];
            int x0 = (int)((xa + 1023) >> 10);  // ceil：第一个被覆盖像素
            int x1 = (int)(xb >> 10);           // floor：最后一个可能被覆盖像素
            if (x0 < 0) x0 = 0;
            if (x1 > FONT_PX - 1) x1 = FONT_PX - 1;
            for (int xx = x0; xx <= x1; xx++) {
                // 覆盖率灰度：像素 [xx*1024, (xx+1)*1024) 与 [xa,xb) 的交集长度 → 0..255
                int32_t p0 = (int32_t)xx * 1024;
                int32_t p1 = p0 + 1024;
                int32_t cs = (xa > p0) ? xa : p0;
                int32_t ce = (xb < p1) ? xb : p1;
                if (ce > cs) {
                    int a = (int)((ce - cs) * 255 / 1024);
                    int old = bm[row * FONT_PX + xx] + a;
                    if (old > 255) old = 255;
                    bm[row * FONT_PX + xx] = (uint8_t)old;
                }
            }
        }
    }
    return true;
}

// ---------------- CJK LRU 缓存 ----------------
// 返回槽索引（-1=失败），key 缓存命中时直接复用
static int lru_get_slot(FontFace* fc, uint32_t cp) {
    // 查找命中
    for (int i = 0; i < FONT_LRU_N; i++) {
        if (fc->lru_key[i] == cp) {
            fc->lru_age[i] = ++fc->lru_tick;
            return i;
        }
    }
    // 找最旧槽（最小 age）
    int victim = 0;
    uint32_t min_age = fc->lru_age[0];
    for (int i = 1; i < FONT_LRU_N; i++) {
        if (fc->lru_age[i] < min_age) { min_age = fc->lru_age[i]; victim = i; }
    }
    // 光栅化到 victim
    int ox, oy, w, h;
    if (!rasterize_glyph(fc, cp, fc->lru_bm[victim], &ox, &oy, &w, &h)) return -1;
    fc->lru_key[victim] = cp;
    fc->lruOX[victim] = ox; fc->lruOY[victim] = oy;
    fc->lruW[victim] = w; fc->lruH[victim] = h;
    fc->lru_age[victim] = ++fc->lru_tick;
    return victim;
}

// ---------------- 公共 API ----------------
// 行高：em + 4（em=16 时 = 20 = 历史 FONT_LINE_HEIGHT，默认档下逐像素不变）
int font_line_height() { return g_em_px + 4; }

// ★ P3：字体大小档（设置页"字体大小"）。改完立刻影响**全部**后续绘制（advPx/缓存全部按新
// scaleFix 重算；已缓存的字形位图全部失效重光栅化）。返回实际生效值（被钳制过）。
void font_set_size64(int px) {
    if (px < FONT_SIZE_MIN) px = FONT_SIZE_MIN;
    if (px > FONT_SIZE_MAX) px = FONT_SIZE_MAX;
    if (px == g_em_px) return;
    g_em_px = px;
    for (int f = 0; f < FONT_FACE_COUNT; f++) {
        FontFace* fc = &g_face[f];
        if (!fc->F) continue;
        face_metrics(fc);                        // 重算 scaleFix/ascFix + ASCII 推进宽度
        for (int i = 0; i < FONT_CACHE_N; i++) {  // ASCII 缓存全部作废（位图与推进宽都变了）
            fc->cacheOk[i] = false;
            fc->cacheW[i] = fc->cacheH[i] = 0;
        }
        for (int i = 0; i < FONT_LRU_N; i++) fc->lru_key[i] = 0;   // CJK LRU 全部作废
        fc->lru_tick = 0;
    }
    dbg64_line_begin64();
    dbg64_str("[FONT64] size px=");
    dbg64_dec((uint64_t)g_em_px);
    dbg64_str(" line=");
    dbg64_dec((uint64_t)(g_em_px + 4));
    dbg64_str(" faces=");
    dbg64_dec((uint64_t)FONT_FACE_COUNT);
    dbg64_str(" caches_rebuilt=1 (scaleFix/advance recomputed; glyph bitmaps invalidated)");
    dbg64_nl();
    dbg64_line_end64();
}
int font_get_size64() { return g_em_px; }

int font_glyph_advance(char c) {
    int idx = (unsigned char)c;
    if (idx < 32 || idx >= FONT_CACHE_N) idx = 32;
    return cur->advPx[idx];
}

// ---------------- 码点查询链：当前面 -> 中文面 -> 兜底面 ----------------
// 四个面的码点集按需求表分工（见 _subset_fonts.py / _subsetsimhei.py）：ASCII+界面符号（面 0）、
// GB2312 一级汉字（面 1）、制表符/块元素/几何图形（面 2）、"前三个面都没有的界面/终端字面量码点"（面 3）。
// 链保证：只要四个面里任何一个有该字形就画得出来；都没有才画缺字占位（1px 空心方框）+ 计数 + 打点。
static uint32_t g_fb_logged[FONT64_LOG_N];
static int g_fb_logged_n = 0;
static uint32_t g_miss_logged[FONT64_LOG_N];
static int g_miss_logged_n = 0;
static uint32_t g_fallback_hits = 0;
static uint32_t g_glyph_miss = 0;

static void font64_hex4(uint32_t cp) {
    static const char* H = "0123456789abcdef";
    char b[5];
    b[0] = H[(cp >> 12) & 0xF]; b[1] = H[(cp >> 8) & 0xF];
    b[2] = H[(cp >> 4) & 0xF];  b[3] = H[cp & 0xF]; b[4] = 0;
    dbg64_str(b);
}

// 同一个码点只打一行（刷屏会把串口塞满；计数照常累加）
static bool font64_log_once(uint32_t* tab, int* n, uint32_t cp) {
    for (int i = 0; i < *n; i++) if (tab[i] == cp) return false;
    if (*n < FONT64_LOG_N) tab[(*n)++] = cp;
    return true;
}

static bool face_has_cp(FontFace* fc, uint32_t cp) {
    if (!fc || !fc->font_ok || cp > 0xFFFF) return false;
    uint16_t g = cmap_lookup(fc, (uint16_t)cp);
    return g != 0 && g < fc->numGlyphs;
}

static FontFace* font_resolve_cp(uint32_t cp) {
    if (face_has_cp(cur, cp)) return cur;
    if (face_has_cp(&g_face[FONT_FACE_CJK], cp)) return &g_face[FONT_FACE_CJK];
    if (face_has_cp(&g_face[FONT_FACE_FALLBACK], cp)) {
        g_fallback_hits++;
        if (font64_log_once(g_fb_logged, &g_fb_logged_n, cp)) {
            dbg64_line_begin64();
            dbg64_str("[FONT64] fallback hit cp=0x");
            font64_hex4(cp);
            dbg64_str(" face=3");
            dbg64_nl();
            dbg64_line_end64();
        }
        return &g_face[FONT_FACE_FALLBACK];
    }
    return nullptr;                 // 四个面都没有 -> 调用方画缺字占位
}

int font_glyph_advance_cp(uint32_t cp) {
    if (cp < FONT_CACHE_N) return font_glyph_advance((char)cp);
    FontFace* fc = font_resolve_cp(cp);
    if (!fc) return g_em_px;             // 缺字占位按一个 em 宽推进
    uint16_t g = cmap_lookup(fc, (uint16_t)cp);
    if (g >= fc->numGlyphs) return g_em_px;
    if (g >= fc->numHMetrics) g = (uint16_t)(fc->numHMetrics - 1);
    uint16_t aw = be16(fc->F + fc->offHmtx + 4 * g);
    int px = (int)(((int32_t)aw * fc->scaleFix + 512) >> 10);
    if (px < 1) px = 1;
    return px;
}

uint32_t font_utf8_decode(const char* s, int* adv) {
    return utf8_decode(s, adv);
}

int font_text_width(const char* s) {
    int w = 0;
    while (*s) {
        int adv;
        uint32_t cp = utf8_decode(s, &adv);
        s += adv;
        w += (cp < FONT_CACHE_N) ? font_glyph_advance((char)cp) : font_glyph_advance_cp(cp);
    }
    return w;
}

// 抗锯齿混合：fg 按 alpha(0-255) 与后备缓冲背景混合
static inline void draw_blend(int x, int y, uint32_t fg, int a) {
    if (a <= 0) return;
    if (a >= 255) { fb_putpixel(x, y, fg); return; }
    uint32_t bg = fb_get_pixel(x, y);
    uint8_t fr = (fg >> 16) & 0xFF, fgc = (fg >> 8) & 0xFF, fgb = fg & 0xFF;
    uint8_t br = (bg >> 16) & 0xFF, bgc = (bg >> 8) & 0xFF, bgb = bg & 0xFF;
    uint8_t r = (uint8_t)((fr * a + br * (255 - a)) / 255);
    uint8_t g = (uint8_t)((fgc * a + bgc * (255 - a)) / 255);
    uint8_t b = (uint8_t)((fgb * a + bgb * (255 - a)) / 255);
    fb_putpixel(x, y, 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b);
}

bool font_draw_glyph(int x, int y, char c, uint32_t fg) {
    int idx = (unsigned char)c;
    if (idx < 32 || idx >= FONT_CACHE_N) return false;
    if (!cur->font_ok) return false;
    if (!cur->cacheOk[idx]) {
        int ox, oy, w, h;
        if (!rasterize_glyph(cur, (uint32_t)idx, cur->cache[idx], &ox, &oy, &w, &h)) {
            cur->cacheOk[idx] = 2;  // 失败
        } else {
            cur->cacheOX[idx] = ox; cur->cacheOY[idx] = oy;
            cur->cacheW[idx] = w; cur->cacheH[idx] = h;
            cur->cacheOk[idx] = 1;
        }
    }
    if (cur->cacheOk[idx] != 1) return false;
    if (cur->cacheW[idx] <= 0) return true;   // 空白字形
    // 注意：字形 bbox 顶部在光栅化缓冲的 cacheOY 行（行 0 = ascender 顶部）。
    // 绘制必须把 bbox 放到 y + cacheOY，否则所有字形顶部对齐、基线错乱。
    for (int yy = 0; yy < cur->cacheH[idx]; yy++) {
        const uint8_t* row = &cur->cache[idx][(cur->cacheOY[idx] + yy) * FONT_PX + cur->cacheOX[idx]];
        for (int xx = 0; xx < cur->cacheW[idx]; xx++) {
            if (row[xx]) draw_blend(x + xx, y + cur->cacheOY[idx] + yy, fg, row[xx]);
        }
    }
    return true;
}

// 缺字占位：四个面都没有这个码点时画一个 1px 空心方框（并计数 + 打点，见 font_draw_glyph_cp）
static void font_draw_missing_box(int x, int y, uint32_t fg) {
    int w = g_em_px - 6, h = g_em_px - 4;
    for (int i = 0; i < w; i++) {
        fb_putpixel(x + i, y + 2, fg);
        fb_putpixel(x + i, y + 2 + h - 1, fg);
    }
    for (int j = 0; j < h; j++) {
        fb_putpixel(x, y + 2 + j, fg);
        fb_putpixel(x + w - 1, y + 2 + j, fg);
    }
}

// 按 Unicode 码点绘制（CJK 走 LRU 缓存；查询链：当前面 -> 中文面 -> 兜底面；都没有 = 缺字占位）
bool font_draw_glyph_cp(int x, int y, uint32_t cp, uint32_t fg) {
    if (cp < FONT_CACHE_N) return font_draw_glyph(x, y, (char)cp, fg);
    FontFace* fc = font_resolve_cp(cp);
    if (!fc) {
        // ★ 中文面外置的**过渡窗口**（kmain 的"屏幕硬件检查报告"等早期屏幕早于第一个装载点）：
        //   这里的缺字**不计入** [FONT64] glyph miss —— 计数的语义是"四个面都没有这个码点
        //   （字体覆盖缺陷）"，而这一档是"中文面还没装载"（装载点之后照常计数）。否则每次启动
        //   都会报一屏假缺字（实测：硬件报告那屏的汉字），把"覆盖缺陷"这条信号淹掉。
        const bool transition = (g_cjk_ext_pending && !g_cjk_ext_resolved);
        if (!transition) {
            g_glyph_miss++;
            if (font64_log_once(g_miss_logged, &g_miss_logged_n, cp)) {
                dbg64_line_begin64();
                dbg64_str("[FONT64] glyph miss cp=0x");
                font64_hex4(cp);
                dbg64_nl();
                dbg64_line_end64();
            }
        }
        font_draw_missing_box(x, y, fg);
        return false;
    }
    int slot = lru_get_slot(fc, cp);
    if (slot < 0) return false;
    if (fc->lruW[slot] <= 0) return true;   // 空白字形
    for (int yy = 0; yy < fc->lruH[slot]; yy++) {
        const uint8_t* row = &fc->lru_bm[slot][(fc->lruOY[slot] + yy) * FONT_PX + fc->lruOX[slot]];
        for (int xx = 0; xx < fc->lruW[slot]; xx++) {
            if (row[xx]) draw_blend(x + xx, y + fc->lruOY[slot] + yy, fg, row[xx]);
        }
    }
    return true;
}

void font_draw_text(int x, int y, const char* s, uint32_t fg) {
    int cx = x;
    while (*s) {
        int adv;
        uint32_t cp = utf8_decode(s, &adv);
        if (cp == 0 && adv == 1 && (uint8_t)s[0] == '\n') { cx = x; y += font_line_height(); s++; continue; }
        if (cp == 0 && adv == 1) { s++; continue; }  // 非法字节跳过
        if (cp < FONT_CACHE_N) {
            font_draw_glyph(cx, y, (char)cp, fg);
            cx += font_glyph_advance((char)cp);
        } else {
            font_draw_glyph_cp(cx, y, cp, fg);
            cx += font_glyph_advance_cp(cp);
        }
        s += adv;
    }
}

// ==================== 字形预加载（系统级预加载） ====================
// 只"填充缓存"，不写任何屏幕像素（把 font_draw_glyph/font_draw_glyph_cp 的缓存填充部分
// 抽出来复用），所以预热与绘制结果完全一致、只是提前做。

static bool prewarm_one_ascii(FontFace* fc, int idx) {
    if (fc->cacheOk[idx]) return false;
    int ox, oy, w, h;
    if (!rasterize_glyph(fc, (uint32_t)idx, fc->cache[idx], &ox, &oy, &w, &h)) {
        fc->cacheOk[idx] = 2;          // 失败（与绘制路径一致地标记）
        return false;
    }
    fc->cacheOX[idx] = ox; fc->cacheOY[idx] = oy;
    fc->cacheW[idx] = w;   fc->cacheH[idx] = h;
    fc->cacheOk[idx] = 1;
    return true;
}

// LRU 容量（每 face）—— 系统级预加载据此自检"预热集能否全部驻留"（幂等的前提）
int font_lru_capacity() { return FONT_LRU_N; }

int font_prewarm_ascii() {
    int n = 0;
    for (int idx = 32; idx < FONT_CACHE_N; idx++)
        if (prewarm_one_ascii(cur, idx)) n++;
    return n;
}

int font_prewarm_ascii_all() {
    FontFace* save = cur;
    int n = 0;
    for (int f = 0; f < FONT_FACE_COUNT; f++) {        // 四个面全预热（含等宽面/兜底面）
        if (!g_face[f].font_ok) continue;
        cur = &g_face[f];
        n += font_prewarm_ascii();
    }
    cur = save;
    return n;
}

int font_prewarm_text(const char* s) {
    if (!s) return 0;
    int n = 0;
    while (*s) {
        int adv = 1;
        uint32_t cp = utf8_decode(s, &adv);
        if (adv <= 0) adv = 1;
        if (cp != 0) {
            if (cp < FONT_CACHE_N) {
                FontFace* fc = cur;
                if (prewarm_one_ascii(fc, (int)cp)) n++;
            } else {
                FontFace* fc = font_resolve_cp(cp);
                if (fc && fc->font_ok) {
                    bool hit = false;
                    for (int i = 0; i < FONT_LRU_N; i++)
                        if (fc->lru_key[i] == cp) { hit = true; break; }
                    if (!hit && lru_get_slot(fc, cp) >= 0) n++;   // 未命中 → 光栅化进 LRU 槽
                }
            }
        }
        s += adv;
    }
    return n;
}

int font_cache_used() {
    int n = 0;
    for (int f = 0; f < FONT_FACE_COUNT; f++) {        // 四个面都算
        FontFace* fc = &g_face[f];
        if (!fc->font_ok) continue;
        for (int i = 32; i < FONT_CACHE_N; i++)
            if (fc->cacheOk[i] == 1) n++;
        for (int i = 0; i < FONT_LRU_N; i++)
            if (fc->lru_key[i] != 0) n++;
    }
    return n;
}

int font_cache_capacity() {
    int n = 0;
    for (int f = 0; f < FONT_FACE_COUNT; f++) {
        if (!g_face[f].font_ok) continue;
        n += FONT_CACHE_N - 32 + FONT_LRU_N;
    }
    return n;
}

// ==================== 四面的自检 / 打点（行锁；kernel64.cpp 在 font_init 之后调用）====================
uint32_t font_fallback_hits() { return g_fallback_hits; }
uint32_t font_glyph_miss_count() { return g_glyph_miss; }

// 某个面里一个码点的推进宽度（像素）；0 = 这个面没有该字形
static int face_adv_px(FontFace* fc, uint32_t cp) {
    if (!fc || !fc->font_ok) return 0;
    uint16_t g = cmap_lookup(fc, (uint16_t)cp);
    if (g == 0 || g >= fc->numGlyphs) return 0;
    if (g >= fc->numHMetrics) g = (uint16_t)(fc->numHMetrics - 1);
    uint16_t aw = be16(fc->F + fc->offHmtx + 4 * g);
    int px = (int)(((int32_t)aw * fc->scaleFix + 512) >> 10);
    return px < 1 ? 1 : px;
}

void font_selftest() {
    // ★ 中文面外置：**还没到装载点**（磁盘/vfs 没起来）—— 推迟这份打点，别把"还没装载"报成 cjk=0/FAIL。
    //   装载点（font_external_load64 / font_external_give_up64）解决之后会回调这里补打点。
    if (g_cjk_ext_pending && !g_cjk_ext_resolved) {
        g_selftest_hold = true;
        dbg64_line_begin64();
        dbg64_str("[FONT64] selftest held (cjk face external; waiting for disk/vfs load point)\n");
        dbg64_line_end64();
        return;
    }
    uint32_t mask = 0;
    // ★ 中文面"外置但确认取不到"（裸盘没原始区、卷里也没字体）= **不适用**，不是失败：
    //   与中文面相依的三项（bit0 全四面 / bit1 中英 1:2 / bit2 查询链）按 na 记，下面的打点里会写出 na=1。
    const bool cjk_na = (g_cjk_ext_pending && g_cjk_ext_resolved && !g_face[FONT_FACE_CJK].font_ok);
    dbg64_line_begin64();
    dbg64_str("[FONT64] faces=");
    dbg64_dec(FONT_FACE_COUNT);
    dbg64_str(" ascii=");    dbg64_dec(g_face[FONT_FACE_ASCII].font_ok ? 1 : 0);
    dbg64_str(" cjk=");      dbg64_dec(g_face[FONT_FACE_CJK].font_ok ? 1 : 0);
    dbg64_str(" mono=");     dbg64_dec(g_face[FONT_FACE_MONO].font_ok ? 1 : 0);
    dbg64_str(" fallback="); dbg64_dec(g_face[FONT_FACE_FALLBACK].font_ok ? 1 : 0);
    dbg64_str(" cjk_ext_na="); dbg64_dec(cjk_na ? 1u : 0u);
    dbg64_nl();
    dbg64_line_end64();

    // bit0：四个面都加载成功（中文面外置取不到时按 na 通过）
    int loaded = 0;
    for (int f = 0; f < FONT_FACE_COUNT; f++) if (g_face[f].font_ok) loaded++;
    if (loaded == FONT_FACE_COUNT) mask |= 1;
    else if (cjk_na && loaded == FONT_FACE_COUNT - 1) mask |= 1;       // na
    // bit1：等宽面 ASCII 宽 *2 == 中文面汉字宽（中英 1:2；终端就是按这个混排的）
    int ma = face_adv_px(&g_face[FONT_FACE_MONO], (uint32_t)'A');
    int ca = face_adv_px(&g_face[FONT_FACE_CJK], 0x4E00u);
    if (ma > 0 && ca == ma * 2) mask |= 2;
    else if (cjk_na) mask |= 2;                                       // na（这一项与中文面相依）
    dbg64_line_begin64();
    dbg64_str("[FONT64] mono ascii="); dbg64_dec((uint64_t)ma);
    dbg64_str(" cjk=");                dbg64_dec((uint64_t)ca);
    dbg64_str(" ratio=");              dbg64_dec(ma > 0 ? (uint64_t)(ca / ma) : 0);
    dbg64_nl();
    dbg64_line_end64();
    // bit2：查询链 —— 从任何一个面发起画汉字，都要落到中文面
    bool chain_ok = true;
    for (int f = 0; f < FONT_FACE_COUNT; f++) {
        if (!g_face[f].font_ok) continue;
        cur = &g_face[f];
        if (font_resolve_cp(0x4E00u) != &g_face[FONT_FACE_CJK]) chain_ok = false;
    }
    cur = &g_face[FONT_FACE_ASCII];
    if (chain_ok) mask |= 4;
    else if (cjk_na) mask |= 4;                                       // na（查询链的判据是"落到中文面"）
    // bit3：兜底面命中 —— 只在前三个面缺席、由 Unifont 补的码点（界面/终端字面量里出现，见 _subset_fonts.py）
    static const uint32_t fb_cand[] = { 0x2229u, 0x6D4Fu, 0x6E32u };   // ∩ / 浏 / 渲
    bool fb_ok = false;
    for (unsigned i = 0; i < sizeof(fb_cand) / sizeof(fb_cand[0]); i++) {
        if (font_resolve_cp(fb_cand[i]) == &g_face[FONT_FACE_FALLBACK]) { fb_ok = true; break; }
    }
    if (fb_ok) mask |= 8;
    // bit4：面 0/1/2 的 ASCII 推进宽度都算得出来（ASCII 是这三面的硬要求；面 3 只收"前三面没有的码点"）
    bool adv_ok = true;
    for (int f = 0; f <= FONT_FACE_MONO; f++)
        if (face_adv_px(&g_face[f], (uint32_t)'A') <= 0) adv_ok = false;
    if (adv_ok) mask |= 16;

    dbg64_line_begin64();
    dbg64_str("[FONT64] selftest ");
    dbg64_str((mask == 0x1Fu) ? "PASS mask=0x" : "FAIL mask=0x");
    dbg64_hex64((uint64_t)mask);
    dbg64_nl();
    dbg64_line_end64();
}
