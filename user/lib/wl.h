/* wl.h - ★ A5：Wayland 基础骨架的**用户态包装**（Vimtu64 用户态 C 运行时）
 *
 * 与内核侧 kernel/wl64.h 是同一份约定（调用号/结构布局/错误码/语义），本文件只是把它包成 C。
 * 用户程序写的是 Wayland 术语（surface / buffer / damage / commit / seat / dispatch），
 * 底下走的是 int 0x80 自有 ABI 的 15..21 号（**不是** Unix domain socket + wl_display 协议）。
 *
 *   int wl_surface_create(int w, int h, int format);              -> surface id (>=1) / 负错误码
 *   int wl_surface_attach(int surf, int shm_id, int offset);      -> 0 / 负错误码
 *   int wl_surface_damage(int surf, int x, int y, int w, int h);  -> 0 / 负错误码（内部打包成两个寄存器）
 *   int wl_surface_commit(int surf);                              -> 0 / 负错误码（入提交队列）
 *   int wl_surface_destroy(int surf);                             -> 0 / 负错误码
 *   int wl_seat_get(void);                                        -> seat id (>=1) / 负错误码
 *   int wl_display_dispatch(unsigned timeout_ms, struct Wl64SeatEvent* out, unsigned max);
 *         -> 本次投递的 seat 事件条数（>=0）/ 负错误码；同一轮里内核把提交队列合成上屏
 *
 * 语义要点（详细版在 kernel/wl64.h 的注释里，这里是用户侧要记住的部分）：
 *   * **未提交不显示**：往共享缓冲里写、却不 commit，屏幕上不会有任何变化；
 *   * commit 之后要**调一次 wl_display_dispatch**（哪怕 timeout_ms=0）才会真的合成上屏；
 *   * **damage 为空 = 整面**；两次 damage 之间并成包围盒；框外的屏幕像素不动；
 *   * 一个 surface 同时只 attach 一块缓冲；换缓冲/销毁时内核会还给 shm 对象一个引用，
 *     所以"提交（+ dispatch）返回之后，那块缓冲就可以重画了"——可以 ping-pong 两块缓冲；
 *   * seat 事件 = input_poll 的 40 字节事件 + surf/sx/sy/inside（见下面结构体）。 */
#ifndef VIMTU64_WL_H
#define VIMTU64_WL_H

#include <stdint.h>
#include "vimtu64.h"

/* ---------------- 自有 ABI 号段（★ A5：15..21；内核侧定义见 kernel/wl64.h） ---------------- */
#define V64_NR_WL_SURFACE_CREATE  15
#define V64_NR_WL_SURFACE_ATTACH  16
#define V64_NR_WL_SURFACE_DAMAGE  17
#define V64_NR_WL_SURFACE_COMMIT  18
#define V64_NR_WL_SURFACE_DESTROY 19
#define V64_NR_WL_SEAT_GET        20
#define V64_NR_WL_DISPATCH        21

/* 错误码（与 kernel/wl64.h 的 WL64_* 逐值一致） */
#define V64_WL_EPERM    (-1)   /* 没有进程上下文 / surface 不属于本进程 */
#define V64_WL_EFAULT   (-2)   /* 用户指针非法 */
#define V64_WL_ENOENT   (-3)   /* 没有这个 surface / shm 对象 / 句柄 */
#define V64_WL_EINVAL   (-4)   /* 参数非法（尺寸/格式/offset/越界/max 超上限） */
#define V64_WL_ENODEV   (-5)   /* 没有可用的显示（fb 未初始化 / 面板放不下） */
#define V64_WL_ENOSPC   (-6)   /* surface 表满 / 引用计数 */

/* 像素格式（与 A1 的 Fb64Info.format 同一口径）：0 = XRGB8888 */
#define V64_WL_FORMAT_XRGB8888  0

/* 上限（与内核常量一致；超了内核用错误码拒绝，用户侧提前判更省事） */
#define V64_WL_MAX_PIXELS   16384u   /* 单 surface 像素上限 = 64 KiB 缓冲 / 4 字节 */
#define V64_WL_MAX_EVENTS   32u      /* 一次 dispatch 最多取多少条 seat 事件 */

/* ---------------- seat 事件（ABI：64 字节 POD，与内核 Wl64SeatEvent 逐字节一致）----------------
 * 前 40 字节就是 input_poll 的 struct Ev64Event（见 vimtu64.h），所以 ev 字段可以直接当事件用。 */
struct Wl64SeatEvent {
    struct Ev64Event ev;   /* +0  输入事件本体（type/code/x/y/dx/dy/buttons/mods/t_ms） */
    uint32_t seat;         /* +40 座位 id（wl_seat_get 的返回值） */
    uint32_t surf;         /* +44 路由到的 surface id；0 = 没有（指针不在自己任何 surface 上） */
    int32_t  sx;           /* +48 surface 局部坐标 x；inside=0 或键盘事件 = -1 */
    int32_t  sy;           /* +52 surface 局部坐标 y */
    uint32_t inside;       /* +56 1 = 事件坐标落在该 surface 矩形内（键盘事件恒 1） */
    uint32_t pad;          /* +60 对齐填充 */
};
#define V64_WL_SEAT_EVENT_SIZE 64

int wl_surface_create(int w, int h, int format);
int wl_surface_attach(int surf, int shm_id, int offset);
int wl_surface_damage(int surf, int x, int y, int w, int h);
int wl_surface_commit(int surf);
int wl_surface_destroy(int surf);
int wl_seat_get(void);
int wl_display_dispatch(unsigned timeout_ms, struct Wl64SeatEvent* out, unsigned max);

#endif /* VIMTU64_WL_H */
