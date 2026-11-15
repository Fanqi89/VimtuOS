/* stb_image_impl.c - ★ v0.10.5：vendored stb_image 的唯一编译单元 + 内存竞技场
 *
 * 为什么"移植"而不是"手搓"（本轮的第一条决策）：
 *   内核侧的 kernel/img64.cpp 已经有 PNG/BMP 的**纯整数**解码器（零依赖、无浮点），
 *   但它是 ring0 的、而且 **JPEG 明确未实现**。本批要的是 ring3 图片查看器 + 三种格式
 *   （PNG/JPG/BMP），所以走"把单头文件库搬进用户态"这条路：
 *     * stb_image v2.32（public domain / MIT 双许可，见 third_party/stb/README.md）；
 *     * 只保留 PNG + JPEG + BMP 三个解码器（其余格式用 STBI_NO_* 编掉，省体积）；
 *     * 零 stdio（STBI_NO_STDIO）：文件由 viewer.c 读成内存，再 stbi_load_from_memory；
 *     * 零 math.h（STBI_NO_HDR + STBI_NO_LINEAR 之后 `pow`/`ldexp` 整段被编掉）。
 *
 * 为什么要自己实现分配器（不是"忘了用 malloc"）：
 *   `user/lib/stdlib.c` 的堆只有 **4 KiB**（V64_HEAP_BYTES），装不下任何一张真图。
 *   这里用 mmap(9) 拿 12 MiB、16 字节对齐 bump 分配，STBI_FREE 是空操作。
 *   本进程是**一次性**的（看完图退出，内核回收整片地址空间），所以不回收是正确的口径，
 *   而不是偷懒 —— 同一个理由写在 vi_img.h 里。
 *
 * 编译纪律：本文件必须带 `-msse -msse2`（stb 里有 float 路径，例如 JPEG 的通用重采样），
 *   与 user/lib/font64.c（stb_truetype）同一条规矩；窗口壳 viewer.c 仍然 -mno-sse。
 *   同时 `-w` 关掉上游代码的风格告警（本仓库自己的文件仍 -Wall -Wextra -Werror）。
 */
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "vi_img.h"

/* ==================== ① 竞技场（mmap + bump） ==================== */
/* 内核 mmap(9)：prot = 3（READ|WRITE）、flags = 0x22（MAP_PRIVATE|MAP_ANONYMOUS）。
 * 与 user/store/vs_io.c 的 vs_mmap 同一写法（那条路已经在上千次装包里跑过）。 */
static unsigned char* g_arena;
static unsigned int   g_arena_used;
static unsigned int   g_arena_cap;

int vi_arena_init(void) {
    if (g_arena) return 1;
    const long rc = __v64_syscall(9, 0, (long)VI_ARENA_BYTES, 3, 0x22, -1);
    if (rc < 0) return 0;
    g_arena = (unsigned char*)(unsigned long)rc;
    g_arena_used = 0;
    g_arena_cap = VI_ARENA_BYTES;
    return 1;
}

void vi_arena_destroy(void) {
    /* 不做 munmap：进程马上就退出了，内核在 exit 时整片回收。故意留空并写明，
     * 免得以后有人以为"漏了"。 */
    g_arena = (unsigned char*)0;
    g_arena_used = 0;
    g_arena_cap = 0;
}

static void* vi_arena_alloc(unsigned long n) {
    if (!g_arena) return (void*)0;
    if (n == 0) n = 16;
    n = (n + 15u) & ~15u;                       /* 16 字节对齐（stb 里 float 数组要它） */
    if ((unsigned long)g_arena_used + n > (unsigned long)g_arena_cap) return (void*)0;
    void* p = g_arena + g_arena_used;
    g_arena_used += (unsigned int)n;
    return p;
}

/* STBI_REALLOC 只需要"变大的那段还能用"：bump 分配器里新块永远是新的，
 * 老内容原样拷过去即可。stb 只用它来按需放大 token 缓冲。 */
static void* vi_arena_realloc(void* p, unsigned long n) {
    if (!p) return vi_arena_alloc(n);
    if (!g_arena) return (void*)0;
    /* 老块多大？stb 没告诉我们 —— 但它的用法是"同一个缓冲变大"。
     * 保守做法：把**已用区从 p 起**的整段拷过去（p 之后只会是同一逻辑块的旧尾部或空闲，
     * 拷多不会越界：上限就是 g_arena_used）。*/
    const unsigned long have = (unsigned long)(g_arena + g_arena_used - (unsigned char*)p);
    void* q = vi_arena_alloc(n);
    if (!q) return (void*)0;
    const unsigned long cp = (n < have) ? n : have;
    memcpy(q, p, (size_t)cp);
    return q;
}

/* ==================== ② stb_image 配置（唯一的实现点） ==================== */
#define STBI_NO_STDIO                    /* 文件由 viewer.c 读成内存 */
#define STBI_NO_HDR                      /* 不做 Radiance HDR（省体积 + 去掉 pow/ldexp） */
#define STBI_NO_LINEAR                   /* 不做 HDR->LDR 的线性转换（同上） */
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_GIF
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_THREAD_LOCALS            /* 不假设 __thread 可用 */
#define STBI_NO_FAILURE_STRINGS          /* 失败原因用我们自己的静态串，见 vi_err */
#define STBI_NO_SIMD                     /* 不用 SSE intrinsic，走纯 C 路径（更可复现） */
#define STBI_ASSERT(x) ((void)0)         /* 环 3 里 assert 只能整进程死；解码失败走返回值 */
/* 分配全部指向竞技场（默认是 4 KiB 堆，装不下任何真图）。 */
#define STBI_MALLOC(sz)            vi_arena_alloc((unsigned long)(sz))
#define STBI_REALLOC(p, sz)        vi_arena_realloc((p), (unsigned long)(sz))
#define STBI_FREE(p)               ((void)(p))
#define STBI_MEMCPY(d, s, n)       memcpy((d), (s), (size_t)(n))
#define STBI_MEMSET(d, c, n)       memset((d), (c), (size_t)(n))

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

/* ==================== ③ 对外壳 ==================== */
static const unsigned char* g_pixels;
static const char*          g_err = "";

const unsigned char* vi_pixels(void) { return g_pixels; }
const char*          vi_err(void)    { return g_err; }

int vi_decode(const unsigned char* data, int len, int* out_w, int* out_h, int* out_comp) {
    g_pixels = (const unsigned char*)0;
    g_err = "";
    if (!data || len <= 0) { g_err = "no-data"; return 0; }
    if (!g_arena) { g_err = "no-arena"; return 0; }
    if ((unsigned)len > VI_MAX_FILE) { g_err = "file-too-big"; return 0; }

    int w = 0, h = 0, comp = 0;
    /* 先只读文件头拿尺寸：这样超大图可以在**解码前**就拒绝（否则竞技场先被撑爆）。 */
    if (!stbi_info_from_memory(data, len, &w, &h, &comp)) {
        g_err = stbi_failure_reason();
        if (!g_err || !g_err[0]) g_err = "probe";
        return 0;
    }
    if (w <= 0 || h <= 0) { g_err = "bad-size"; return 0; }
    if ((unsigned long)w * (unsigned long)h > (unsigned long)VI_MAX_PIXELS) {
        g_err = "too-many-pixels";
        return 0;
    }

    /* 请求 3 通道（RGB）：画布会自己补 alpha=0xFF，省 1/4 内存。 */
    unsigned char* px = stbi_load_from_memory(data, len, &w, &h, &comp, 3);
    if (!px) {
        g_err = stbi_failure_reason();
        if (!g_err || !g_err[0]) g_err = "decode";
        return 0;
    }
    g_pixels = px;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    if (out_comp) *out_comp = 3;
    return 1;
}
