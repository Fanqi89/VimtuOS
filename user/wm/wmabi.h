/* wmabi.h - ★ B-wm：Ring 3 合成器的用户态 ABI 包装（自有 ABI 22..26）
 *
 * 与 kernel/wl64.h 的 "B-wm" 段是**同一份约定**（调用号/结构布局/错误码/语义），这里只是包成 C。
 * 本文件属于 user/wm 目录（合成器与它的两个客户端共用），**不改 user/lib** 的既有 ABI 包装。
 *
 *   22 wl_composer_get()                -> (seat id) | (gpu<<8) / 负错误码
 *   23 wl_surface_export(idx, out)      -> 0 = 填好一条（64 B）；1 = 该下标没有 surface；负 = 错误
 *   24 wl_surface_map(surf, out)        -> 0 = 缓冲页已映射（16 B 出参）；负 = 错误
 *   25 wl_surface_ack(surf)             -> 0；合成器声明"这条我合成完了"
 *   26 wl_seat_post(ev)                 -> 投递到的 pid（>=1）/ 0 = 目标没了 / 负 = 错误
 *
 * 只有**注册过的合成器进程**能调 22..26（别的进程拿 -1 EPERM）。客户端只用 15..21（wl.h）——
 * 它们不知道 22..26 的存在：提交走 commit(18)，收事件走 dispatch(21)（内核把 wm 投回来的事件
 * 放进自己的每进程队列，路由字段由命中测试算，语义与 A5 完全一致）。
 */
#ifndef VIMTU64_WMABI_H
#define VIMTU64_WMABI_H

#include <stdint.h>
#include "vimtu64.h"
#include "wl.h"                 /* struct Wl64SeatEvent（64 B：40 B 事件 + 路由字段） */

/* ---- 自有 ABI 号段（★ B-wm：22..26；内核侧定义见 kernel/wl64.h） ---- */
#define V64_NR_WL_COMPOSER_GET    22
#define V64_NR_WL_SURFACE_EXPORT  23
#define V64_NR_WL_SURFACE_MAP     24
#define V64_NR_WL_SURFACE_ACK     25
#define V64_NR_WL_SEAT_POST       26

/* 22 号的返回值：低 8 位 = seat id，bit8 = 显示后端是 virtio-gpu 2D 设备（0 = soft-lfb） */
#define V64_WM_GPU_BIT            0x100u
#define V64_WM_SEAT_MASK          0xFFu

/* 错误码（与 kernel/wl64.h 的 WL64_* 逐值一致） */
#define V64_WM_EPERM    (-1)    /* 没有进程上下文 / 调用者不是注册过的合成器 */
#define V64_WM_EFAULT   (-2)    /* 用户指针非法 */
#define V64_WM_ENOENT   (-3)    /* 没有这个 surface / 还没有缓冲 */
#define V64_WM_EINVAL   (-4)    /* 参数非法（idx 越界） */
#define V64_WM_ENODEV   (-5)    /* 没有可用的显示（fb 未初始化 / 外部面板区放不下） */
#define V64_WM_ENOSPC   (-6)    /* 资源满 / 页表页不足（映射失败） */

/* ---- 23 的出参：一条 surface 的只读快照（64 B POD；字段与 Wl64SurfaceInfo 逐字节一致）---- */
struct Wm64SurfaceInfo {
    uint32_t id;        /* +0  surface id */
    int32_t  pid;       /* +4  拥有者进程 */
    uint32_t w, h;      /* +8,+12  逻辑尺寸 */
    int32_t  x, y;      /* +16,+20 屏幕坐标 */
    int32_t  shm_id;    /* +24 attach 的 shm 对象（0 = 还没有缓冲） */
    uint32_t shm_off;   /* +28 缓冲在对象里的字节偏移 */
    uint32_t shm_bytes; /* +32 缓冲字节数 */
    uint32_t refs;      /* +36 对象引用数（诊断） */
    int32_t  dmg_x, dmg_y, dmg_w, dmg_h;   /* +40..+52 damage 包围盒（surface 局部坐标） */
    uint32_t flags;     /* +56 bit0 pending / bit1 content / bit2 has_buf */
    uint32_t pad;       /* +60 */
};
#define V64_WM_INFO_SIZE     64u
#define V64_WM_PENDING       0x1u
#define V64_WM_CONTENT       0x2u
#define V64_WM_HASBUF        0x4u

/* ---- 24 的出参：缓冲页的映射结果（16 B POD）。
 * 像素地址 = va + shm_off + y*(w*4) + x*4（pitch = surface 宽 * 4，格式 XRGB8888）。 ---- */
struct Wm64SurfaceMap {
    uint64_t va;        /* +0  用户可读 VA（对象页 0 的映射基址） */
    uint32_t bytes;     /* +8  已映射字节数（= pages * 4096） */
    uint32_t pages;     /* +12 页数 */
};
#define V64_WM_MAP_SIZE      16u

/* ---- 22 的出参：内核租给合成器的"外部面板区"（16 B POD；out 传 0 表示不要）---- */
struct Wm64Rect { int32_t x, y, w, h; };
#define V64_WM_RECT_SIZE     16u

int wl_composer_get(struct Wm64Rect* out_rect);
int wl_surface_export(int idx, struct Wm64SurfaceInfo* out);
int wl_surface_map(int surf, struct Wm64SurfaceMap* out);
int wl_surface_ack(int surf);
int wl_seat_post(struct Wl64SeatEvent* ev);

/* ---- rdtsc（用户态计时）----
 * 内核从不设 CR4.TSD（见 kernel/usermode64.cpp 的 CR4 只开 OSFXSR/OSXMMEXCPT），所以 ring3 的
 * rdtsc 是合法指令；合成器用它量"合成一帧"的周期数，再按自己标定的 tsc/ms 换成微秒。
 * 为什么不用 ticks()(自有 ABI 4)：PIT 250 Hz = 4 ms 粒度，量一帧（几十微秒）根本不够。 */
static inline unsigned long long wm_rdtsc(void) {
    unsigned int lo = 0, hi = 0;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((unsigned long long)hi << 32) | (unsigned long long)lo;
}

#endif /* VIMTU64_WMABI_H */
