/* stdlib.c - malloc/free/calloc/realloc（bump + 空闲链表）+ atoi/abs/exit（Vimtu64 用户态 C 运行时）
 *
 * 分配器（规格要求的最小实现，全部落在静态竞技场 g_arena64[]，它在 .bss -> 用户程序 blob 的可写页）：
 *   1) 块格式：16 字节头 { uint32 magic; uint32 size; struct blk* next; }，payload 紧随其后；
 *      magic = V64_BLK_MAGIC_ALLOC（已分配）/ V64_BLK_MAGIC_FREE（空闲）——**这就是越界哨兵**：
 *      写越界踩到相邻块的头 -> magic 崩 -> vimtu64_heap_check() 如实报错；
 *   2) 分配：先扫**空闲链表（按地址升序）首次适配**，够大就切（剩余 >= V64_BLK_MIN 才切）；
 *      没有合适的就从 bump 指针往高位切（bump = 单调递增，天然不重叠）；
 *   3) 释放：顶部块直接归还 bump 指针；其余按地址升序插回空闲链表；
 *   4) **不做相邻空闲块合并**（如实标注：长期"大块-释放-小块"会碎片化 —— 够本运行时的用户程序用，
 *      不是通用分配器；要通用分配器得换 libc）；
 *   5) 竞技场最后 4 字节是 V64_ARENA_GUARD（尾部哨兵），越界写最先踩到它。
 * 所有块在竞技场里是**地址连续**的（bump 单调 + 只在块内切/回收），所以 vimtu64_heap_check()
 * 可以线性走一遍：任何头被踩坏都会被查出来。 */
#include <stdint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "stdio.h"
#include <unistd.h>      /* _exit（exit() 的实现后半段） */

#define V64_HDR_BYTES       16u
#define V64_BLK_MAGIC_ALLOC 0x564D5441u     /* "VMTA"：已分配块头的哨兵 */
#define V64_BLK_MAGIC_FREE  0x564D5446u     /* "VMTF"：空闲块头的哨兵 */
#define V64_ARENA_GUARD     0x564D5447u     /* "VMTG"：竞技场尾部哨兵 */
#define V64_BLK_MIN         48u             /* 切块后剩余块的最小字节数（含头） */
#define V64_ALIGN           16u

typedef struct v64_blk64 {
    uint32_t magic;
    uint32_t size;                          /* payload 字节数（16 对齐，不含头） */
    struct v64_blk64* next;                 /* 只在空闲块里有意义 */
} v64_blk64;

_Static_assert(sizeof(v64_blk64) == V64_HDR_BYTES, "块头必须是 16 字节（ABI/哨兵布局）");

static unsigned char g_arena64[V64_HEAP_BYTES];     /* .bss：竞技场 */
static v64_blk64*    g_free64;                      /* 空闲链表（地址升序） */
static unsigned char* g_top64;                      /* bump 指针 */
static uint32_t*     g_guard64;                     /* 指向竞技场尾部哨兵 */
static unsigned      g_nblk64;                      /* 已分配块数（统计用） */
static int           g_ready64;

static void v64_heap_init64(void) {
    g_top64   = g_arena64;
    g_free64  = 0;
    g_nblk64  = 0;
    g_guard64 = (uint32_t*)(g_arena64 + V64_HEAP_BYTES - sizeof(uint32_t));
    *g_guard64 = V64_ARENA_GUARD;
    g_ready64 = 1;
}

static uint32_t v64_round64(size_t n) {
    return (uint32_t)((n + (V64_ALIGN - 1u)) & ~(size_t)(V64_ALIGN - 1u));
}

void* malloc(size_t n) {
    if (!g_ready64) v64_heap_init64();
    if (n == 0) return 0;
    const uint32_t need = v64_round64(n);

    /* 1) 空闲链表首次适配（顺带校验链表没被踩坏） */
    v64_blk64** pp = &g_free64;
    while (*pp) {
        v64_blk64* b = *pp;
        if (b->magic != V64_BLK_MAGIC_FREE) { errno = EIO; return 0; }   /* 堆被踩坏：如实失败 */
        if (b->size >= need) {
            if (b->size >= need + V64_BLK_MIN) {
                v64_blk64* rest = (v64_blk64*)((unsigned char*)b + V64_HDR_BYTES + need);
                rest->magic = V64_BLK_MAGIC_FREE;
                rest->size  = b->size - need - V64_HDR_BYTES;
                rest->next  = b->next;
                b->size     = need;
                *pp = rest;
            } else {
                *pp = b->next;
            }
            b->magic = V64_BLK_MAGIC_ALLOC;
            b->next  = 0;
            g_nblk64++;
            return (void*)((unsigned char*)b + V64_HDR_BYTES);
        }
        pp = &b->next;
    }

    /* 2) bump（竞技场尾部 4 字节留给哨兵） */
    const unsigned used  = (unsigned)(g_top64 - g_arena64);
    const unsigned avail = (unsigned)(V64_HEAP_BYTES - sizeof(uint32_t));
    if ((size_t)used + V64_HDR_BYTES + need > (size_t)avail) { errno = ENOMEM; return 0; }

    v64_blk64* b = (v64_blk64*)g_top64;
    b->magic = V64_BLK_MAGIC_ALLOC;
    b->size  = need;
    b->next  = 0;
    g_top64 += V64_HDR_BYTES + need;
    g_nblk64++;
    return (void*)((unsigned char*)b + V64_HDR_BYTES);
}

void free(void* p) {
    if (!p) return;
    if (!g_ready64) { errno = EINVAL; return; }
    v64_blk64* b = (v64_blk64*)((unsigned char*)p - V64_HDR_BYTES);
    if ((unsigned char*)b < g_arena64 || (unsigned char*)b >= g_arena64 + V64_HEAP_BYTES) { errno = EINVAL; return; }
    if (b->magic != V64_BLK_MAGIC_ALLOC) { errno = EINVAL; return; }   /* 双重释放/野指针：挡住，不写坏堆 */
    b->magic = V64_BLK_MAGIC_FREE;
    if (g_nblk64) g_nblk64--;

    /* 顶部块：直接归还 bump 指针（这样"分配-释放-再分配同样大小"永远不碎片化） */
    if ((unsigned char*)b + V64_HDR_BYTES + b->size == g_top64) {
        g_top64 = (unsigned char*)b;
        return;
    }
    /* 其余：按地址升序插回空闲链表（链表中相邻块不合并 —— 见文件头第 4 条） */
    v64_blk64** pp = &g_free64;
    while (*pp && *pp < b) pp = &(*pp)->next;
    b->next = *pp;
    *pp = b;
}

void* calloc(size_t n, size_t sz) {
    const size_t total = (sz != 0 && n > (size_t)-1 / sz) ? (size_t)-1 : n * sz;
    void* p = malloc(total);
    if (!p) return 0;
    memset(p, 0, total);
    return p;
}

void* realloc(void* p, size_t n) {
    if (!p) return malloc(n);
    if (n == 0) { free(p); return 0; }
    if (!g_ready64) { errno = EINVAL; return 0; }
    v64_blk64* b = (v64_blk64*)((unsigned char*)p - V64_HDR_BYTES);
    if (b->magic != V64_BLK_MAGIC_ALLOC) { errno = EINVAL; return 0; }

    const uint32_t need = v64_round64(n);
    if (need <= b->size) return p;                       /* 原地够用（不缩容，省一次拷贝） */

    /* 就在 bump 顶部：向后直接扩（不搬） */
    if ((unsigned char*)b + V64_HDR_BYTES + b->size == g_top64) {
        const unsigned used  = (unsigned)(g_top64 - g_arena64);
        const unsigned avail = (unsigned)(V64_HEAP_BYTES - sizeof(uint32_t));
        if ((size_t)used + (need - b->size) <= (size_t)avail) {
            g_top64 += (need - b->size);
            b->size = need;
            return p;
        }
    }
    void* np = malloc(n);
    if (!np) return 0;
    memcpy(np, p, b->size < n ? b->size : n);
    free(p);
    return np;
}

/* ---------------- 自检 / 统计（给用户程序与验收脚本当证据） ---------------- */
unsigned vimtu64_heap_check(void) {
    if (!g_ready64) return 0;                            /* 没用过堆 = 完好 */
    if (*g_guard64 != V64_ARENA_GUARD) return 0xFFFFFFFFu;   /* 尾部哨兵被踩 */

    unsigned idx = 0;
    unsigned char* p = g_arena64;
    while (p < g_top64) {
        v64_blk64* b = (v64_blk64*)p;
        if (b->magic != V64_BLK_MAGIC_ALLOC && b->magic != V64_BLK_MAGIC_FREE) return idx + 1u;
        if (b->size == 0) return idx + 1u;
        if ((size_t)(g_top64 - p) < (size_t)V64_HDR_BYTES + b->size) return idx + 1u;
        p += V64_HDR_BYTES + b->size;
        idx++;
    }
    if (p != g_top64) return 0xFFFFFFFFu;

    /* 空闲链表：地址必须严格升序（插回逻辑一旦写错，这里立刻暴露） */
    const v64_blk64* prev = 0;
    for (const v64_blk64* b = g_free64; b; b = b->next) {
        if (b->magic != V64_BLK_MAGIC_FREE) return 0xFFFFFFFEu;
        if (prev && b <= prev) return 0xFFFFFFFEu;
        prev = b;
    }
    return 0;
}

void vimtu64_heap_stats(unsigned* used, unsigned* total, unsigned* blocks) {
    if (!g_ready64) v64_heap_init64();
    unsigned payload = 0;
    unsigned nblk = 0;
    for (unsigned char* p = g_arena64; p < g_top64; ) {
        const v64_blk64* b = (const v64_blk64*)p;
        if (b->size == 0 || (size_t)(g_top64 - p) < (size_t)V64_HDR_BYTES + b->size) break;
        if (b->magic == V64_BLK_MAGIC_ALLOC) { payload += b->size; nblk++; }
        p += V64_HDR_BYTES + b->size;
    }
    if (used) *used = payload;
    if (total) *total = V64_HEAP_BYTES;
    if (blocks) *blocks = nblk;
}

/* ---------------- 转换 / 退出 ---------------- */
int atoi(const char* s) {
    int sign = 1;
    long v = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n') s++;
    if (*s == '-') { sign = -1; s++; } else if (*s == '+') { s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return (int)(sign * v);
}

int abs(int v) { return (v < 0) ? -v : v; }

/* exit(code)：先把行缓冲刷出去（否则最后一段没换行的输出会丢），再自有 ABI exit(2)。
 * 内核侧：syscall64_dispatch64 的 case 2 会把控制权交回 ring0 蹦床（见 kernel/usermode64.cpp）。 */
void exit(int code) {
    vimtu64_stdout_flush64();
    _exit(code);
}
