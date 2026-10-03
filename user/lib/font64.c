/* font64.c - ★ Ring 3 用户态文字/字体栈（实现；规则与依据见 font64.h 顶部）
 *
 * 结构：
 *   ① stb_truetype（单头文件，public domain/MIT 双许可，见 user/lib/stb_truetype.h 末尾原文）
 *      —— 只用来把 TTF 轮廓光栅化成覆盖率位图；对外 API 全是我们的（font64.h）。
 *      我们的 libc 没有 math.h/assert.h，也没有 4 KiB 以上的堆，所以：
 *        * STBTT_ifloor/iceil/sqrt/fabs/cos/acos/pow 全部换成下面的整数/自实现版本；
 *        * STBTT_malloc/free 指向本文件里的**光栅化临时区**（mmap 来的 1 MiB，LIFO 回退）；
 *        * STBTT_assert 关掉（裸机没有 assert；失败路径由我们自己的返回值如实表达）。
 *      CFF/Type2 那几条路（cos/acos/pow）只有非 glyf 字库才走得到，而 font_load 会**先拒绝**
 *      没有 glyf 表的字库（与内核 face_init 同口径），所以那几条路在本项目里是死代码。
 *   ② 从**系统卷**读字库：open + lseek(算大小) + read（内核单次 read 上限 4096，所以要分块）
 *      -> 拷进 mmap 的大缓冲（用户窗口的 mmap 区 4GiB+576KiB..4GiB+16MiB）。
 *   ③ 字形缓存：按 (面, 字号, 码点) 命中/缺失计数 + rdtsc 计时（首次 vs 命中）。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include "vimtu64.h"
#include "font64.h"

/* ==================== ① stb 替身（整数/自实现数学 + 竞技场分配） ==================== */

/* sqrt：牛顿迭代（float 硬件指令，不是 libm 调用） */
static float f64_sqrtf(float x) {
    if (!(x > 0.0f)) return 0.0f;
    float r = (x > 1.0f) ? x : 1.0f;
    for (int i = 0; i < 24; i++) r = 0.5f * (r + x / r);
    return r;
}
static float f64_fabsf(float x) { return (x < 0.0f) ? -x : x; }
/* pow 只有 CFF 路径用（指数 1/3），这里给一个自足的立方根牛顿迭代；其它指数如实返回 x。 */
static float f64_powf(float x, float y) {
    if (y == 0.0f) return 1.0f;
    if (y > 0.9f && y < 1.1f) return x;
    if (y > 0.3f && y < 0.34f) {                  /* 1/3：立方根 */
        float r = (x > 1.0f) ? x : 1.0f;
        for (int i = 0; i < 30; i++) r = r - (r * r * r - x) / (3.0f * r * r);
        return r;
    }
    return x;
}
/* cos/acos：CFF 路径专用（glyf 字库用不到）。泰勒级数，够那几条死代码用。 */
static float f64_cosf(float x) {
    const float PI2 = 6.28318530718f;
    while (x > 3.14159265f) x -= PI2;
    while (x < -3.14159265f) x += PI2;
    const float x2 = x * x;
    float t = 1.0f, s = 1.0f;
    for (int i = 1; i <= 8; i++) { t = -t * x2 / (float)((2 * i - 1) * (2 * i)); s += t; }
    return s;
}
static float f64_acosf(float x) {
    if (x >= 1.0f) return 0.0f;
    if (x <= -1.0f) return 3.14159265f;
    /* acos(x) = pi/2 - asin(x)；asin 用泰勒（|x|<=1 时收敛慢，但这条路径不会被调用） */
    const float x2 = x * x;
    float t = x, s = x;
    for (int n = 1; n <= 40; n++) {
        t = t * x2 * (float)(2 * n - 1) / (float)(2 * n);
        s += t / (float)(2 * n + 1);
    }
    return 1.57079632f - s;
}

/* fmod：只有 SDF（有向距离场）那条路用，本项目不调用；给个自足实现免得引 libm。 */
static float f64_fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    const float q = (float)(long long)(x / y);
    return x - q * y;
}
#define STBTT_ifloor(x)      ((int)(x) - (((x) < 0.0f && (float)(int)(x) != (x)) ? 1 : 0))
#define STBTT_iceil(x)       ((int)(x) + (((x) > 0.0f && (float)(int)(x) != (x)) ? 1 : 0))
#define STBTT_sqrt(x)        f64_sqrtf(x)
#define STBTT_pow(x, y)      f64_powf((x), (y))
#define STBTT_fmod(x, y)     f64_fmodf((x), (y))
#define STBTT_fabs(x)        f64_fabsf(x)
#define STBTT_cos(x)         f64_cosf(x)
#define STBTT_acos(x)        f64_acosf(x)
#define STBTT_assert(x)      ((void)0)
#define STBTT_strlen(x)      f64_strlen(x)

static size_t f64_strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static void* f64_stb_alloc(size_t n);
static void  f64_stb_free(void* p);
#define STBTT_malloc(x, u)   ((void)(u), f64_stb_alloc(x))
#define STBTT_free(x, u)     ((void)(u), f64_stb_free(x))

#include "stb_truetype.h"

/* ---- 光栅化临时区（LIFO 回退的 bump 分配器）----
 * 为什么需要它：stb 的 v2 光栅器每个字形要 vertices/edges/scanline 三块临时内存
 * （汉字在 16px 上可能到几十 KB），而我们的 libc 堆只有 4 KiB。
 * 为什么能省：stb 的分配/释放是 LIFO 的，释放栈顶块时直接回退水位 —— 稳定态水位接近 0。
 * 水位与分配失败的次数都记在 F64Stats 里（如实暴露，不假装成功）。 */
/* ★ 单头文件库：实现段必须在本 TU 里展开一次（别处只 include 声明段）；
 *   位置必须在上面那些 STBTT_* 替身之后，且全项目只展开这一次。 */
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define F64_ARENA_BYTES (1024u * 1024u)
struct F64Arena {
    unsigned char* base;
    unsigned       size;
    unsigned       used;      /* 当前水位 */
    unsigned       peak;      /* 历史最高水位 */
    unsigned       top_off;   /* 最近一次分配（栈顶） */
    unsigned       top_size;
    unsigned       fails;
};
static struct F64Arena g_arena;

static void* f64_stb_alloc(size_t n) {
    if (!g_arena.base) return 0;
    unsigned need = (unsigned)((n + 15u) & ~(size_t)15u);
    if (need == 0) need = 16u;
    if (g_arena.used + need > g_arena.size) { g_arena.fails++; return 0; }
    unsigned char* p = g_arena.base + g_arena.used;
    g_arena.top_off = g_arena.used;
    g_arena.top_size = need;
    g_arena.used += need;
    if (g_arena.used > g_arena.peak) g_arena.peak = g_arena.used;
    return (void*)p;
}
static void f64_stb_free(void* p) {
    if (!p || !g_arena.base) return;
    const unsigned off = (unsigned)((unsigned char*)p - g_arena.base);
    if (off == g_arena.top_off) {                 /* 栈顶释放 -> 水位回退 */
        g_arena.used = off;
        g_arena.top_off = 0;
        g_arena.top_size = 0;
    }
}

/* ==================== 共用小工具 ==================== */

unsigned long long font64_rdtsc(void) {
    unsigned int lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

unsigned long long font64_calibrate_tsc_per_ms(unsigned sleep_ms) {
    if (sleep_ms == 0) sleep_ms = 20;
    const unsigned long long t0 = vimtu64_ticks();
    const unsigned long long c0 = font64_rdtsc();
    (void)vimtu64_sleep_ms(sleep_ms);
    const unsigned long long c1 = font64_rdtsc();
    const unsigned long long t1 = vimtu64_ticks();
    const unsigned long long dt = t1 - t0;              /* PIT ticks：250Hz = 4ms 粒度 */
    if (dt == 0) return 0;
    return ((c1 - c0) * 4ULL) / dt;                     /* 每 tick = 4 ms -> tsc/ms */
}

int font64_utf8_decode(const char* s, int* adv) {
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char b0 = p[0];
    if (b0 < 0x80u) { *adv = 1; return (int)b0; }
    if (b0 >= 0xC2u && b0 <= 0xDFu) {
        if (!p[1]) { *adv = 1; return 0; }
        *adv = 2;
        return (int)((((uint32_t)(b0 & 0x1Fu)) << 6) | (uint32_t)(p[1] & 0x3Fu));
    }
    if (b0 >= 0xE0u && b0 <= 0xEFu) {
        if (!p[1] || !p[2]) { *adv = 1; return 0; }
        *adv = 3;
        return (int)((((uint32_t)(b0 & 0x0Fu)) << 12) | ((uint32_t)(p[1] & 0x3Fu) << 6) |
                     (uint32_t)(p[2] & 0x3Fu));
    }
    *adv = 1;
    return 0;
}

/* 全角（East Asian Wide/Fullwidth）——GRID12 的 1:2 判定用。
 * 表按 Unicode EastAsianWidth 的 W/F 段抄（只列常用段；未列的按半角）。 */
struct F64Wide { uint32_t lo, hi; };
static const struct F64Wide F64_WIDE[] = {
    { 0x1100u,  0x115Fu }, { 0x2E80u,  0x303Eu }, { 0x3041u,  0x33FFu },
    { 0x3400u,  0x4DBFu }, { 0x4E00u,  0x9FFFu }, { 0xA000u,  0xA4CFu },
    { 0xA960u,  0xA97Fu }, { 0xAC00u,  0xD7A3u }, { 0xF900u,  0xFAFFu },
    { 0xFE10u,  0xFE19u }, { 0xFE30u,  0xFE6Fu }, { 0xFF00u,  0xFF60u },
    { 0xFFE0u,  0xFFE6u }, { 0x1F300u, 0x1F64Fu }, { 0x1F900u, 0x1F9FFu },
    { 0x20000u, 0x3FFFDfu },
};
int font64_is_wide(uint32_t cp) {
    for (unsigned i = 0; i < sizeof(F64_WIDE) / sizeof(F64_WIDE[0]); i++)
        if (cp >= F64_WIDE[i].lo && cp <= F64_WIDE[i].hi) return 1;
    return 0;
}

/* ==================== 字库文件（从系统卷读） ==================== */

/* 从卷里读一份文件到 mmap 缓冲：返回缓冲指针并把长度写到 *out_len（失败 0）。
 *
 * ★ 走**内核的 Linux 兼容路径**（syscall 指令 nr=2/8/0/3），不走自有 ABI 的 open(6)/read(7)。
 *   实测依据（第一次跑 tests/font64user_test.py 的串口日志）：
 *     * "/Fonts/NotoSans-Regular.ttf"（28 字符）open 成功、read 回 0 B（打开到的是目录）；
 *     * "/Fonts-open/<四个中文面>.ttf"（35 字符）直接 open 返回 -1。
 *   原因是自有 ABI 号的路径缓冲只有 32 B（kernel/syscall64.cpp:559 LX64_PATH_MAX=32，
 *   调用点 :2093）且只解析单层；Linux 号的缓冲是 128 B（同文件 :561 LX64_PATHR_MAX），
 *   多层路径也是 tcc/编辑器一直在用的那条（fd64/vfs64 的多层目录支持）。
 *   Linux 单次 read 上限 4096（LX64_READ_MAX），所以这里分块读。 */
static unsigned char* f64_read_volume(const char* path, unsigned cap, unsigned* out_len) {
    const long fd = __v64_syscall(V64_LX_OPEN, (long)(unsigned long)path, 0 /*O_RDONLY*/, 0, 0, 0);
    if (fd < 0) {
        printf("[FONT64U] open FAIL path=%s ret=%d\n", path, (int)fd);
        return 0;
    }
    const long sz = __v64_syscall(V64_LX_LSEEK, fd, 0, 2 /*SEEK_END*/, 0, 0);
    if (sz <= 0 || sz > (long)cap) {
        printf("[FONT64U] size FAIL path=%s bytes=%d cap=%u\n", path, (int)sz, cap);
        (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
        return 0;
    }
    /* ★ 量完大小必须**倒回文件头**：lseek(SEEK_END) 会把偏移留在文件尾，
     *   接着 read 只会得到 0（实测过：read ret=0，[FD64] read 一行都没有）。 */
    if (__v64_syscall(V64_LX_LSEEK, fd, 0, 0 /*SEEK_SET*/, 0, 0) != 0) {
        printf("[FONT64U] rewind FAIL path=%s\n", path);
        (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
        return 0;
    }
    unsigned char* buf = (unsigned char*)mmap(0, (size_t)sz, 3 /*READ|WRITE*/,
                                              0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0);
    if (!buf) {
        printf("[FONT64U] mmap FAIL path=%s bytes=%d\n", path, (int)sz);
        (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
        return 0;
    }
    unsigned got = 0;
    while (got < (unsigned)sz) {
        unsigned chunk = (unsigned)sz - got;
        if (chunk > 4096u) chunk = 4096u;            /* 内核单次 read 上限 4096（LX64_READ_MAX） */
        const long r = __v64_syscall(V64_LX_READ, fd, (long)(unsigned long)(buf + got),
                                     (long)chunk, 0, 0);
        if (r <= 0) break;
        got += (unsigned)r;
    }
    (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
    if (got != (unsigned)sz) {
        printf("[FONT64U] read FAIL path=%s want=%d got=%u\n", path, (int)sz, got);
        return 0;
    }
    *out_len = got;
    return buf;
}

/* 最小的 sfnt 表目录扫描（只为了"是不是 glyf 字库"这一条判断 —— 与内核 face_init 同口径：
 * 内核要求 head/hhea/hmtx/maxp/loca/glyf/cmap 七个表都在，OTTO/CFF 直接拒绝）。 */
static uint32_t f64_be32(const unsigned char* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint16_t f64_be16(const unsigned char* p) { return (uint16_t)(((uint16_t)p[0] << 8) | p[1]); }
static int f64_find_table(const unsigned char* d, unsigned len, const char* tag, uint32_t* off) {
    if (len < 12u) return 0;
    const uint16_t nt = f64_be16(d + 4);
    for (uint16_t i = 0; i < nt; i++) {
        const unsigned char* rec = d + 12u + 16u * i;
        if ((unsigned)(rec - d) + 16u > len) return 0;
        if (rec[0] == (unsigned char)tag[0] && rec[1] == (unsigned char)tag[1] &&
            rec[2] == (unsigned char)tag[2] && rec[3] == (unsigned char)tag[3]) {
            *off = f64_be32(rec + 8);
            return *off != 0u && *off < len;
        }
    }
    return 0;
}

/* ==================== 面（face） ==================== */

struct F64Face {
    int                used;
    int                kind;
    char               path[72];
    unsigned           bytes;
    unsigned char*     data;
    stbtt_fontinfo     tt;
    int                upem, asc_fu, desc_fu, nglyph;
};

#define F64_GLYPH_PX  26      /* 单字形位图缓冲边长（em<=18 + descender 余量） */
#define F64_CACHE_N   128     /* 字形缓存槽数 */

struct F64Glyph {
    uint64_t      key;                       /* (face<<40)|(size<<32)|cp；0 = 空槽 */
    unsigned      age;
    unsigned char bm[F64_GLYPH_PX * F64_GLYPH_PX];
    signed char   w, h, ox, oy;
    unsigned char ok;                        /* 1 = 有位图（w/h 可能为 0 = 空白字形） */
};

struct Font64 {
    struct F64Face  face[F64_MAX_FACES];
    int             nface;
    int             primary;
    int             spacing;
    int             shift_half;
    unsigned        tick;
    unsigned        miss_n, hit_n;
    unsigned long long miss_tsc, hit_tsc;
    unsigned        glyph_ok, glyph_empty, glyph_fail;
    unsigned        miss_logged;             /* 缺字去重打印计数 */
    uint32_t        miss_cp[8];
    struct F64Glyph cache[F64_CACHE_N];
};

static struct Font64* f64_alloc_handle(void) {
    struct Font64* f = (struct Font64*)mmap(0, sizeof(struct Font64), 3, 0x22, -1, 0);
    if (!f) return 0;
    memset(f, 0, sizeof(*f));
    f->primary = -1;
    f->spacing = F64_SPACING_NATURAL;
    f->shift_half = 0;
    return f;
}

static int f64_add_face_slot(struct Font64* f, const char* path, int kind, int verbose) {
    if (!f) return -V64_SHM_EINVAL;
    if (f->nface >= F64_MAX_FACES) return -V64_SHM_ENOMEM;
    struct F64Face* fc = &f->face[f->nface];
    unsigned n = 0;
    unsigned char* d = f64_read_volume(path, 24u * 1024u * 1024u, &n);
    fc->path[0] = 0;
    for (int i = 0; i < (int)sizeof(fc->path) - 1 && path[i]; i++) fc->path[i] = path[i];
    if (!d) {
        if (verbose) printf("[FONT64U] face FAIL kind=%d path=%s why=read\n", kind, path);
        return -V64_SHM_EINVAL;
    }
    /* 表目录门槛（与内核 face_init 同口径）：七个必需表 + 不许是 OTTO/CFF */
    uint32_t t_head = 0, t_hhea = 0, t_hmtx = 0, t_maxp = 0, t_loca = 0, t_glyf = 0, t_cmap = 0;
    const uint32_t ver = f64_be32(d);
    const int has_glyf = f64_find_table(d, n, "glyf", &t_glyf);
    const int ok_tabs = has_glyf && f64_find_table(d, n, "head", &t_head) &&
                        f64_find_table(d, n, "hhea", &t_hhea) && f64_find_table(d, n, "hmtx", &t_hmtx) &&
                        f64_find_table(d, n, "maxp", &t_maxp) && f64_find_table(d, n, "loca", &t_loca) &&
                        f64_find_table(d, n, "cmap", &t_cmap);
    if (!ok_tabs || (ver != 0x00010000u && ver != 0x74727565u /* 'true' */)) {
        if (verbose)
            printf("[FONT64U] face FAIL kind=%d path=%s why=%s ver=0x%x bytes=%u\n", kind, path,
                   has_glyf ? "tables" : "no-glyf(cff/otto)", (unsigned)ver, n);
        return -V64_SHM_EINVAL;
    }
    if (!stbtt_InitFont(&fc->tt, d, 0)) {
        if (verbose) printf("[FONT64U] face FAIL kind=%d path=%s why=stbtt_InitFont\n", kind, path);
        return -V64_SHM_EINVAL;
    }
    fc->tt.userdata = 0;
    fc->data = d;
    fc->bytes = n;
    fc->upem = (int)f64_be16(d + t_head + 18);
    fc->asc_fu = (int)(int16_t)f64_be16(d + t_hhea + 4);
    fc->desc_fu = (int)(int16_t)f64_be16(d + t_hhea + 6);
    fc->nglyph = (int)f64_be16(d + t_maxp + 4);
    /* kind 自动判定：有 U+4E00 的当中文面（内核的分工：face 1 收 GB2312 一级汉字），
     * 等宽靠文件名（*mono*）判 —— 与 _subset_fonts.py 的产物命名一致。 */
    if (kind == F64_KIND_AUTO) {
        kind = stbtt_FindGlyphIndex(&fc->tt, 0x4E00u) ? F64_KIND_CJK : F64_KIND_LATIN;
        for (int i = 0; path[i]; i++)
            if ((path[i] == 'm' || path[i] == 'M') && (path[i + 1] == 'o' || path[i + 1] == 'O'))
                kind = F64_KIND_MONO;
    }
    fc->kind = kind;
    fc->used = 1;
    f->nface++;
    if (f->primary < 0) f->primary = 0;
    if (verbose)
        printf("[FONT64U] face ok kind=%d path=%s bytes=%u upem=%d asc=%d desc=%d nglyph=%d\n",
               kind, path, n, fc->upem, fc->asc_fu, fc->desc_fu, fc->nglyph);
    return 0;
}

struct Font64* font_load(const char* path) {
    if (!path) return 0;
    if (!g_arena.base) {                       /* 第一次加载才要这块临时区 */
        g_arena.base = (unsigned char*)mmap(0, F64_ARENA_BYTES, 3, 0x22, -1, 0);
        g_arena.size = F64_ARENA_BYTES;
        g_arena.used = g_arena.top_off = g_arena.top_size = 0;
        g_arena.peak = 0;
        g_arena.fails = 0;
        if (!g_arena.base) {
            printf("[FONT64U] arena mmap FAIL bytes=%u\n", (unsigned)F64_ARENA_BYTES);
            return 0;
        }
    }
    struct Font64* f = f64_alloc_handle();
    if (!f) {
        printf("[FONT64U] handle mmap FAIL bytes=%u\n", (unsigned)sizeof(struct Font64));
        return 0;
    }
    (void)f64_add_face_slot(f, path, F64_KIND_AUTO, 1);
    return f;
}

int font64_add_face(struct Font64* f, const char* path, int kind) {
    if (!f || !path) return -V64_SHM_EINVAL;
    if (!g_arena.base) font_load(path);        /* 幂等：确保临时区在 */
    return f64_add_face_slot(f, path, kind, 1);
}

int font64_face_count(struct Font64* f) { return f ? f->nface : 0; }

int font64_face_info(struct Font64* f, int idx, struct F64FaceInfo* out) {
    if (!f || !out || idx < 0 || idx >= F64_MAX_FACES) return -V64_SHM_EINVAL;
    struct F64Face* fc = &f->face[idx];
    out->used = fc->used;
    out->kind = fc->kind;
    out->path = fc->path;
    out->bytes = fc->bytes;
    out->upem = fc->upem;
    out->asc_fu = fc->asc_fu;
    out->desc_fu = fc->desc_fu;
    out->nglyph = fc->nglyph;
    out->has_glyf = fc->used;
    return 0;
}

int font64_set_primary(struct Font64* f, int kind) {
    if (!f) return -V64_SHM_EINVAL;
    for (int i = 0; i < f->nface; i++)
        if (f->face[i].kind == kind) { f->primary = i; return i; }
    return -1;
}
int font64_set_spacing(struct Font64* f, int mode) {
    if (!f) return -V64_SHM_EINVAL;
    f->spacing = (mode == F64_SPACING_GRID12) ? F64_SPACING_GRID12 : F64_SPACING_NATURAL;
    return f->spacing;
}
int font64_set_shift(struct Font64* f, int half_px) {
    if (!f) return -V64_SHM_EINVAL;
    f->shift_half = half_px ? 1 : 0;
    return f->shift_half;
}
int font64_shift(struct Font64* f) { return f ? f->shift_half : 0; }

/* ==================== 面选择 / 度量（口径见 font64.h） ==================== */

static int f64_kind_index(struct Font64* f, int kind) {
    for (int i = 0; i < f->nface; i++) if (f->face[i].used && f->face[i].kind == kind) return i;
    return -1;
}

static int f64_face_has(struct Font64* f, int idx, uint32_t cp) {
    if (idx < 0 || idx >= f->nface || !f->face[idx].used) return 0;
    if (cp > 0x10FFFFu) return 0;
    return stbtt_FindGlyphIndex(&f->face[idx].tt, (int)cp) != 0;
}

/* 内核的查询链（kernel/font.cpp:787 font_resolve_cp）：主面 -> 中文面 -> 兜底面。
 * GRID12 下**半角字符优先等宽面** —— 这正是内核终端的样子（终端 font_select(MONO)，
 * 汉字由查询链落到中文面）：见 kernel/terminal64.cpp:512/519/619。 */
static int f64_pick_face(struct Font64* f, uint32_t cp) {
    if (f->spacing == F64_SPACING_GRID12 && !font64_is_wide(cp)) {
        const int m = f64_kind_index(f, F64_KIND_MONO);
        if (f64_face_has(f, m, cp)) return m;
    }
    if (f64_face_has(f, f->primary, cp)) return f->primary;
    const int cj = f64_kind_index(f, F64_KIND_CJK);
    if (f64_face_has(f, cj, cp)) return cj;
    const int fb = f64_kind_index(f, F64_KIND_FALLBK);
    if (f64_face_has(f, fb, cp)) return fb;
    for (int i = 0; i < f->nface; i++) if (f64_face_has(f, i, cp)) return i;
    return -1;
}

int font64_face_of(struct Font64* f, uint32_t cp) { return f ? f64_pick_face(f, cp) : -1; }

int font64_line_height(struct Font64* f, int size_px) {
    (void)f;
    return size_px + 4;                          /* kernel/font.cpp:716 */
}

/* 面自身的 hmtx 推进（内核公式：kernel/font.cpp:327 scaleFix + :336 推进） */
static int f64_natural_advance(struct F64Face* fc, uint32_t cp, int size_px, int* gi_out) {
    const int gi = stbtt_FindGlyphIndex(&fc->tt, (int)cp);
    if (gi_out) *gi_out = gi;
    if (!gi) return -1;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&fc->tt, gi, &adv, &lsb);
    const int scale1024 = (size_px * 1024) / fc->upem;
    int px = (adv * scale1024 + 512) >> 10;
    if (px < 1) px = 1;
    return px;
}

int font64_advance(struct Font64* f, uint32_t cp, int size_px) {
    if (!f) return size_px;
    const int fi = f64_pick_face(f, cp);
    if (fi < 0) return size_px;                  /* 缺字占位按一个 em 推进（kernel/font.cpp:808） */
    if (f->spacing == F64_SPACING_GRID12) {      /* 内核终端列网格：半角 em/2、全角 em */
        const int cell = font64_is_wide(cp) ? size_px : (size_px / 2);
        return cell > 0 ? cell : 1;
    }
    int nat = f64_natural_advance(&f->face[fi], cp, size_px, 0);
    if (nat < 0) return size_px;
    return nat;
}

int font64_text_width(struct Font64* f, const char* utf8, int size_px) {
    if (!f || !utf8) return 0;
    int w = 0;
    const char* s = utf8;
    while (*s) {
        int adv = 1;
        const uint32_t cp = (uint32_t)font64_utf8_decode(s, &adv);
        s += adv;
        if (cp == 0) continue;
        w += font64_advance(f, cp, size_px);
    }
    return w;
}

int font64_pen_series(struct Font64* f, const char* utf8, int size_px, int* out, int cap) {
    if (!f || !utf8 || !out) return -1;
    int n = 0, pen = 0;
    const char* s = utf8;
    while (*s) {
        int adv = 1;
        const uint32_t cp = (uint32_t)font64_utf8_decode(s, &adv);
        s += adv;
        if (cp == 0) continue;
        if (n >= cap) return -1;
        out[n++] = pen;
        pen += font64_advance(f, cp, size_px);
        if (n + 1 >= cap) break;
    }
    if (n < cap) out[n] = pen;                  /* 末尾笔位（总宽） */
    return n;
}

/* ==================== 字形位图缓存 ==================== */

static unsigned f64_face_asc_px(struct F64Face* fc, int size_px) {
    const int scale1024 = (size_px * 1024) / fc->upem;   /* 内核 scaleFix */
    int ascfix = fc->asc_fu * scale1024;                 /* 内核 ascFix（kernel/font.cpp:329） */
    if (ascfix < 0) ascfix = 0;
    return (unsigned)(ascfix >> 10);                     /* 基线相对锚点的像素行 */
}

/* 取字形位图（缓存命中直接返回；未命中就光栅化）。返回 0 = 有位图（含空白），-1 = 失败。 */
static int f64_glyph_get(struct Font64* f, int fi, uint32_t cp, int size_px, struct F64Glyph** out) {
    const uint64_t key = ((uint64_t)fi << 40) | ((uint64_t)(size_px & 0xFF) << 32) | (uint64_t)cp;
    const unsigned long long t0 = font64_rdtsc();
    unsigned victim = 0, min_age = 0xFFFFFFFFu;
    for (unsigned i = 0; i < F64_CACHE_N; i++) {
        if (f->cache[i].key == key) {                      /* 命中 */
            f->cache[i].age = ++f->tick;
            f->hit_n++;
            f->hit_tsc += font64_rdtsc() - t0;
            *out = &f->cache[i];
            return f->cache[i].ok ? 0 : -1;
        }
        if (f->cache[i].age < min_age) { min_age = f->cache[i].age; victim = i; }
    }
    struct F64Face* fc = &f->face[fi];
    struct F64Glyph* g = &f->cache[victim];
    g->key = key;
    g->age = ++f->tick;
    g->ok = 0;
    g->w = g->h = 0;
    g->ox = g->oy = 0;
    const int gi = stbtt_FindGlyphIndex(&fc->tt, (int)cp);
    if (!gi) { f->glyph_fail++; return -1; }
    const float scale = (float)size_px / (float)fc->upem;   /* 内核尺度：em = size_px 像素 */
    const float shift = f->shift_half ? 0.5f : 0.0f;
    int ix0 = 0, iy0 = 0, ix1 = 0, iy1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&fc->tt, gi, scale, scale, shift, shift, &ix0, &iy0, &ix1, &iy1);
    int w = ix1 - ix0, h = iy1 - iy0;
    if (w > F64_GLYPH_PX) w = F64_GLYPH_PX;
    if (h > F64_GLYPH_PX) h = F64_GLYPH_PX;
    if (w <= 0 || h <= 0) {                                 /* 空白字形（空格等） */
        g->ok = 1;
        f->glyph_empty++;
        f->miss_n++;
        f->miss_tsc += font64_rdtsc() - t0;
        *out = g;
        return 0;
    }
    memset(g->bm, 0, sizeof(g->bm));
    stbtt_MakeGlyphBitmapSubpixel(&fc->tt, g->bm, w, h, F64_GLYPH_PX, scale, scale, shift, shift, gi);
    g->w = (signed char)w;
    g->h = (signed char)h;
    g->ox = (signed char)((ix0 < -128 || ix0 > 127) ? 0 : ix0);
    g->oy = (signed char)((iy0 < -128 || iy0 > 127) ? 0 : iy0);
    g->ok = 1;
    f->glyph_ok++;
    f->miss_n++;
    f->miss_tsc += font64_rdtsc() - t0;
    *out = g;
    return 0;
}

void font64_cache_reset(struct Font64* f) {
    if (!f) return;
    for (unsigned i = 0; i < F64_CACHE_N; i++) {
        f->cache[i].key = 0;
        f->cache[i].age = 0;
        f->cache[i].ok = 0;
    }
    f->tick = 0;
    g_arena.used = g_arena.top_off = g_arena.top_size = 0;   /* 临时区一起回到起点 */
}

struct F64Stats font64_stats(struct Font64* f) {
    struct F64Stats s;
    memset(&s, 0, sizeof(s));
    if (!f) return s;
    s.miss_n = f->miss_n;
    s.hit_n = f->hit_n;
    s.miss_tsc = f->miss_tsc;
    s.hit_tsc = f->hit_tsc;
    s.arena_peak = g_arena.peak;
    s.arena_fail = g_arena.fails;
    s.glyph_ok = f->glyph_ok;
    s.glyph_empty = f->glyph_empty;
    s.glyph_fail = f->glyph_fail;
    return s;
}

/* ==================== 绘制 ==================== */

void font64_fill(struct F64Canvas* c, uint32_t color) {
    if (!c || !c->px) return;
    const uint32_t v = 0xFF000000u | (color & 0xFFFFFFu);
    for (int y = 0; y < c->h; y++) {
        uint32_t* row = c->px + (unsigned)y * (unsigned)c->w;
        for (int x = 0; x < c->w; x++) row[x] = v;
    }
}

/* 把一像素按"覆盖率 * 颜色 alpha"混进画布（内核 draw_blend 的同一套口径，见 kernel/font.cpp:834） */
static void f64_blend(struct F64Canvas* c, int x, int y, uint32_t color, unsigned cov) {
    if (!c || !c->px || x < 0 || y < 0 || x >= c->w || y >= c->h) return;
    const unsigned ca = (color >> 24) & 0xFFu;
    unsigned a = (cov * ca + 127u) / 255u;
    if (a == 0) return;
    uint32_t* p = c->px + (unsigned)y * (unsigned)c->w + (unsigned)x;
    const uint32_t s = *p;
    const unsigned dr = (color >> 16) & 0xFFu, dg = (color >> 8) & 0xFFu, db = color & 0xFFu;
    if (a >= 255u) { *p = 0xFF000000u | (dr << 16) | (dg << 8) | db; return; }
    const unsigned sr = (s >> 16) & 0xFFu, sg = (s >> 8) & 0xFFu, sb = s & 0xFFu;
    const unsigned r = (dr * a + sr * (255u - a)) / 255u;
    const unsigned g = (dg * a + sg * (255u - a)) / 255u;
    const unsigned b = (db * a + sb * (255u - a)) / 255u;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}


void font64_blend_px(struct F64Canvas* c, int x, int y, uint32_t color, unsigned cov) {
    f64_blend(c, x, y, color, cov);
}
static void f64_blit_glyph(struct F64Canvas* c, const struct F64Glyph* g, int px, int py, uint32_t color) {
    for (int j = 0; j < (int)g->h; j++) {
        const unsigned char* row = g->bm + (unsigned)j * F64_GLYPH_PX;
        for (int i = 0; i < (int)g->w; i++) {
            const unsigned cov = row[i];
            if (cov) f64_blend(c, px + i, py + j, color, cov);
        }
    }
}

int font_draw_text(struct Font64* f, struct F64Canvas* c, int x, int y,
                   const char* utf8, int size_px, uint32_t color) {
    if (!f || !c || !utf8) return 0;
    int cx = x, cy = y, drawn = 0;
    const char* s = utf8;
    while (*s) {
        int adv = 1;
        const uint32_t cp = (uint32_t)font64_utf8_decode(s, &adv);
        if (cp == 0u && adv == 1 && (unsigned char)s[0] == (unsigned char)'\n') {
            cx = x;
            cy += font64_line_height(f, size_px);
            s++;
            continue;
        }
        if (cp == 0u) { s++; continue; }                  /* 非法字节跳过（与内核一致） */
        const int fi = f64_pick_face(f, cp);
        if (fi < 0) {                                     /* 缺字占位：1px 空心方框（内核 font.cpp:874） */
            if (f->miss_logged < 8) {
                int seen = 0;
                for (unsigned k = 0; k < f->miss_logged; k++) if (f->miss_cp[k] == cp) seen = 1;
                if (!seen) {
                    f->miss_cp[f->miss_logged++] = cp;
                    printf("[FONT64U] glyph miss cp=0x%x (no face covers it; box placeholder)\n", cp);
                }
            }
            const int bw = size_px - 6, bh = size_px - 4;
            for (int i = 0; i < bw; i++) { f64_blend(c, cx + i, cy + 2, color, 255); f64_blend(c, cx + i, cy + 2 + bh - 1, color, 255); }
            for (int j = 0; j < bh; j++) { f64_blend(c, cx, cy + 2 + j, color, 255); f64_blend(c, cx + bw - 1, cy + 2 + j, color, 255); }
            cx += size_px;
            drawn++;
            s += adv;
            continue;
        }
        struct F64Glyph* g = 0;
        if (f64_glyph_get(f, fi, cp, size_px, &g) == 0 && g) {
            const unsigned asc = f64_face_asc_px(&f->face[fi], size_px);
            const int baseline = cy + (int)asc;           /* 锚点 = 升部顶端，基线在下面 */
            if (g->w > 0 && g->h > 0) f64_blit_glyph(c, g, cx + g->ox, baseline + g->oy, color);
            drawn++;
        }
        cx += font64_advance(f, cp, size_px);
        s += adv;
    }
    return drawn;
}

int font64_ink_stats(const struct F64Canvas* c, uint32_t bg, int x0, int y0, int w, int h,
                     int tol, int* bbox) {
    if (bbox) { bbox[0] = bbox[1] = bbox[2] = bbox[3] = 0; }
    if (!c || !c->px) return 0;
    const int br = (int)((bg >> 16) & 0xFFu), bgg = (int)((bg >> 8) & 0xFFu), bb = (int)(bg & 0xFFu);
    int n = 0, mx0 = 1 << 30, my0 = 1 << 30, mx1 = -(1 << 30), my1 = -(1 << 30);
    for (int y = y0; y < y0 + h; y++) {
        if (y < 0 || y >= c->h) continue;
        const uint32_t* row = c->px + (unsigned)y * (unsigned)c->w;
        for (int x = x0; x < x0 + w; x++) {
            if (x < 0 || x >= c->w) continue;
            const uint32_t p = row[x];
            const int r = (int)((p >> 16) & 0xFFu), g = (int)((p >> 8) & 0xFFu), b = (int)(p & 0xFFu);
            const int dr = r > br ? r - br : br - r;
            const int dg = g > bgg ? g - bgg : bgg - g;
            const int db = b > bb ? b - bb : bb - b;
            if (dr > tol || dg > tol || db > tol) {
                n++;
                if (x < mx0) mx0 = x;
                if (y < my0) my0 = y;
                if (x + 1 > mx1) mx1 = x + 1;
                if (y + 1 > my1) my1 = y + 1;
            }
        }
    }
    if (n && bbox) { bbox[0] = mx0; bbox[1] = my0; bbox[2] = mx1 - mx0; bbox[3] = my1 - my0; }
    return n;
}
