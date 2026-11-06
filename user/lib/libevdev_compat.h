/* libevdev_compat.h - ★ v0.9.21：Linux **libevdev** 的精简适配（wlroots/libinput 的 evdev 后端要用）
 *
 * 定位：把本内核的 evdev 通道（/dev/input/event0|1 + ioctl + read）包装成 libevdev 的**同名 API 子集**，
 *       让按 libevdev 写的代码（包括 libinput 的 evdev 后端）能直接编译运行。
 *
 * 已实现（都有内核侧落地，不是空壳）：
 *   libevdev_new_from_fd()   -> EVIOCGNAME(64) + EVIOCGBIT(0,8)
 *   libevdev_get_name()
 *   libevdev_has_event_type()/has_event_code()
 *   libevdev_grab()          -> **EVIOCGRAB**（本批新加的内核 ioctl，0x40044590）
 *   libevdev_get_abs_info()  -> **EVIOCGABS(axis)**（0xC01845<40+axis>，区间 = 屏幕几何）
 *   libevdev_next_event()    -> read()（24 B/条）+ 批次末尾补一条 SYN_REPORT
 *   libevdev_free()
 *
 * 如实差异（不假装）：
 *   1) **SYN_REPORT 是本适配层合成的**：本内核的 evdev 事件流只有 KEY/REL/ABS 三类，不带 SYN_REPORT，
 *      而 libevdev 的契约是"一帧以 SYN_REPORT 结束"。这里在每批 read 的最后补一条（type=0/code=0/value=0），
 *      不是伪造按键值，只是补齐帧边界。
 *   2) 只有 EVIOCGBIT(0)（事件**类型**位图）；EVIOCGBIT(EV_KEY,…) 的键码位图内核还没做，
 *      所以 has_event_code() 只按类型判断（键/相对轴），不保证具体码位。
 *   3) 没有 EVIOCSKEYCODE / 设备热插拔 / 多设备自动枚举（枚举在本内核是固定的 event0|1 两个节点，
 *      `/sys/class/input` 的合成树还没做 —— 见 docs/2D与3D加速.md §8）。
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include "drm64.h"

/* libevdev 的公开常量（与 Linux libevdev 同值） */
#define LIBEVDEV_GRAB        3
#define LIBEVDEV_UNGRAB      4
#define LIBEVDEV_READ_FLAG_SYNC        1
#define LIBEVDEV_READ_FLAG_NORMAL      0
#define LIBEVDEV_READ_FLAG_FORCE_SYNC  2
#define LIBEVDEV_READ_STATUS_SUCCESS   0
#define LIBEVDEV_READ_STATUS_SYNC      1
#define LIBEVDEV_READ_STATUS_SYNC_DROPPED 2

#define LIBEVDEV_EV_SYN 0u
#define LIBEVDEV_EV_KEY 1u
#define LIBEVDEV_EV_REL 2u
#define LIBEVDEV_EV_ABS 3u

#define LIBEVDEV_MAX_BATCH 8

struct libevdev {
    int           fd;
    int           which;             /* 0 = 键盘 / 1 = 鼠标 */
    unsigned char name[64];
    unsigned long evbits;            /* EVIOCGBIT(0) 的类型位图 */
    struct InputEvent64 buf[LIBEVDEV_MAX_BATCH];
    int           buf_n;             /* 本批有效事件数 */
    int           buf_i;             /* 下一个要吐出的下标 */
    int           syn_pending;       /* 1 = 下一拍吐合成的 SYN_REPORT */
};

static inline int libevdev_new_from_fd(int fd, struct libevdev** out) {
    if (!out || fd < 0) return -22;
    struct libevdev* d = (struct libevdev*)malloc(sizeof(struct libevdev));
    if (!d) return -12;
    for (unsigned i = 0; i < sizeof(struct libevdev); i++) ((unsigned char*)d)[i] = 0;
    d->fd = fd;
    d->which = 1;                    /* 默认按鼠标；调用方可改 */
    long n = drm64_ioctl(fd, (unsigned long)EVDEV64_IOCGNAME(sizeof(d->name)), d->name);
    if (n <= 0) { free(d); return (int)(n < 0 ? n : -5); }
    d->name[sizeof(d->name) - 1] = 0;
    unsigned char bits[8];
    for (unsigned i = 0; i < 8; i++) bits[i] = 0;
    if (drm64_ioctl(fd, (unsigned long)EVDEV64_IOCGBIT(0, 8), bits) > 0) {
        d->evbits = (unsigned long)bits[0];
        d->which = (bits[0] & (1u << LIBEVDEV_EV_REL)) ? 1 : 0;
    }
    *out = d;
    return 0;
}

static inline void libevdev_free(struct libevdev* d) { if (d) free(d); }
static inline const char* libevdev_get_name(struct libevdev* d) { return d ? (const char*)d->name : ""; }
static inline int libevdev_get_fd(struct libevdev* d) { return d ? d->fd : -1; }

static inline int libevdev_has_event_type(struct libevdev* d, unsigned type) {
    if (!d || type > 31u) return 0;
    return (d->evbits >> type) & 1ul ? 1 : 0;
}
/* 只按**类型**判断（原因见文件头第 2 条）：具体码位图内核还没做，不假装能查 */
static inline int libevdev_has_event_code(struct libevdev* d, unsigned type, unsigned code) {
    (void)code;
    return libevdev_has_event_type(d, type);
}

/* EVIOCGRAB：1 = 独占，0 = 释放（内核里存持有者 pid，别的进程 read 会 -EBUSY） */
static inline int libevdev_grab(struct libevdev* d, int grab) {
    if (!d) return -22;
    int v = (grab == LIBEVDEV_GRAB) ? 1 : 0;
    return (int)drm64_ioctl(d->fd, (unsigned long)EVDEV64_IOCTL_GRAB64, &v);
}

/* EVIOCGABS(axis)：区间由内核给（鼠标 = 屏幕几何） */
static inline int libevdev_get_abs_info(struct libevdev* d, unsigned code, struct InputAbsInfo64* out) {
    if (!d || !out) return -22;
    return (int)drm64_ioctl(d->fd, (unsigned long)EVDEV64_IOCTL_GABS64(code), out);
}
static inline int libevdev_set_abs_info(struct libevdev* d, unsigned code, const struct InputAbsInfo64* in) {
    if (!d || !in) return -22;
    return (int)drm64_ioctl(d->fd, (unsigned long)EVDEV64_IOCTL_SABS64(code), (void*)in);
}

/* 取下一条事件：
 *   0  = 成功（ev 已填）
 *   -11= 暂时没有（EAGAIN，调用方应该 poll 再来）
 * 批末尾补一条合成的 SYN_REPORT（见文件头第 1 条）。 */
static inline int libevdev_next_event(struct libevdev* d, unsigned flags, struct InputEvent64* ev) {
    if (!d || !ev) return -22;
    (void)flags;
    if (d->syn_pending) {                       /* 先吐合成的帧边界 */
        d->syn_pending = 0;
        ev->sec = 0; ev->usec = 0;
        ev->type = (uint16_t)LIBEVDEV_EV_SYN; ev->code = 0; ev->value = 0;
        return 0;
    }
    if (d->buf_i >= d->buf_n) {
        const long n = drm64_read(d->fd, d->buf, (long)sizeof(d->buf));
        if (n <= 0) return (int)(n == 0 ? -11 : n);
        d->buf_n = (int)(n / (long)sizeof(struct InputEvent64));
        d->buf_i = 0;
        if (d->buf_n <= 0) return -11;
    }
    *ev = d->buf[d->buf_i++];
    if (d->buf_i >= d->buf_n) d->syn_pending = 1;   /* 本批最后一条：下一拍给 SYN_REPORT */
    return 0;
}
