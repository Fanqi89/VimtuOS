/* user/browser/vimtu_abi.h - ★ v0.12.6：浏览器（musl 静态）用的自有 ABI 包装
 *
 * 为什么需要这一份：user/lib/wl.c 是给"自研最小 libc"那一路（--target=x86_64-unknown-none-elf）
 * 用的，它依赖 user/lib/syscall.S 的 __v64_int80。而浏览器必须链接 **musl + mbedTLS + NetSurf**，
 * 走不了那套 -nostdinc 的运行时。
 *
 * 所以这里用 inline asm 直接发 int 0x80（约定与 user/lib/syscall.S 完全一致：
 *   rax = nr, rdi/rsi/rdx/r10 = 参数 1..4，返回值在 rax）。
 * 号段定义见 user/lib/vimtu64.h（10 fb_flip / 12 input_poll / 13 shm_create / 14 shm_map /
 * 15..21 wl_* / 28..33 窗口装饰与图标；错误码见 kernel/wl64.h）。
 */
#ifndef VIMTU_BROWSER_ABI_H
#define VIMTU_BROWSER_ABI_H

#include <stdint.h>

static inline long v64_sys(long nr, long a1, long a2, long a3, long a4)
{
    register long r10 __asm__("r10") = a4;
    long ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "memory", "cc");
    return ret;
}

/* ---------------- 号段（与 user/lib/vimtu64.h / wl.h 逐值一致） ---------------- */
#define V64_NR_INPUT_POLL          12
#define V64_NR_SHM_CREATE          13
#define V64_NR_SHM_MAP             14
#define V64_NR_WL_SURFACE_CREATE   15
#define V64_NR_WL_SURFACE_ATTACH   16
#define V64_NR_WL_SURFACE_DAMAGE   17
#define V64_NR_WL_SURFACE_COMMIT   18
#define V64_NR_WL_SURFACE_DESTROY  19
#define V64_NR_WL_SEAT_GET         20
#define V64_NR_WL_DISPATCH         21
#define V64_NR_WL_SURFACE_DECOR    28
#define V64_NR_WL_SURFACE_TITLE    29
#define V64_NR_WL_SURFACE_CTL      30
#define V64_NR_WL_SURFACE_SET_APP  32
#define V64_NR_EXIT                60   /* 与 Linux 一致的 exit 号（用户侧只用来收尾） */

#define V64_WL_DECOR_WINDOW        0x1u
#define V64_WL_FORMAT_XRGB8888     0
#define V64_SHM_MAX_BYTES          (2u * 1024u * 1024u)
#define V64_WL_MAX_PIXELS          524288u   /* 内核 WL64_MAX_PIXELS64：2 MiB / 4B */

/* ---------------- input_poll 事件（40 字节 POD，与 kernel/input64.h 一致） ---------------- */
struct V64Event {
    uint32_t type;      /* 0 = 键盘, 1 = 鼠标按钮, 2 = 鼠标移动, 3 = 滚轮 */
    uint32_t code;
    int32_t  x, y;
    int32_t  dx, dy;
    uint32_t buttons;
    uint32_t mods;
    uint32_t t_ms;
    uint32_t pad;
};

/* ---------------- seat 事件（64 字节，与 kernel/wl64.h 一致） ---------------- */
struct Wl64SeatEvent {
    struct V64Event ev;   /* +0 */
    uint32_t seat;        /* +40 */
    uint32_t surf;        /* +44 */
    int32_t  sx;          /* +48 */
    int32_t  sy;          /* +52 */
    uint32_t inside;      /* +56 */
    uint32_t pad;         /* +60 */
};

#define V64_WL_MAX_EVENTS 32u

/* ---------------- 薄包装 ---------------- */
static inline int v64_shm_create(unsigned size)
{
    return (int)v64_sys(V64_NR_SHM_CREATE, (long)size, 0, 0, 0);
}

static inline int v64_shm_map(int id, unsigned off, unsigned len, void **out_va)
{
    return (int)v64_sys(V64_NR_SHM_MAP, (long)id, (long)off, (long)len, (long)out_va);
}

static inline int v64_wl_surface_create(int w, int h, int format)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_CREATE, (long)w, (long)h, (long)format, 0);
}

static inline int v64_wl_surface_attach(int surf, int shm_id, int off)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_ATTACH, (long)surf, (long)shm_id, (long)off, 0);
}

static inline long pack2(int lo, int hi)
{
    return (long)((unsigned)lo | ((unsigned long)(unsigned)hi << 32));
}

static inline int v64_wl_surface_damage(int surf, int x, int y, int w, int h)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_DAMAGE, (long)surf, pack2(x, y), pack2(w, h), 0);
}

static inline int v64_wl_surface_commit(int surf)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_COMMIT, (long)surf, 0, 0, 0);
}

static inline int v64_wl_surface_destroy(int surf)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_DESTROY, (long)surf, 0, 0, 0);
}

static inline int v64_wl_seat_get(void)
{
    return (int)v64_sys(V64_NR_WL_SEAT_GET, 0, 0, 0, 0);
}

static inline int v64_wl_dispatch(unsigned timeout_ms, struct Wl64SeatEvent *out, unsigned max)
{
    return (int)v64_sys(V64_NR_WL_DISPATCH, (long)timeout_ms, (long)(uintptr_t)out, (long)max, 0);
}

static inline int v64_wl_set_decor(int surf, unsigned flags, const char *title)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_DECOR, (long)surf, (long)flags,
                        (long)(uintptr_t)title, 0);
}

static inline int v64_wl_ctl(int surf, int action)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_CTL, (long)surf, (long)action, 0, 0);
}

static inline int v64_wl_set_app(int surf, const char *name)
{
    return (int)v64_sys(V64_NR_WL_SURFACE_SET_APP, (long)surf, (long)(uintptr_t)name, 0, 0);
}

static inline int v64_input_poll(struct V64Event *out, unsigned max, unsigned flags)
{
    return (int)v64_sys(V64_NR_INPUT_POLL, (long)(uintptr_t)out, (long)max, (long)flags, 0);
}

#endif /* VIMTU_BROWSER_ABI_H */
