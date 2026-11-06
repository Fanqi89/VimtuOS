/* libdrm_compat.h - ★ v0.9.13：libdrm 风格的**用户态兼容封装**（跑在我们自己的 DRM ioctl 上）
 *
 * 定位（先读，别误期待）：
 *   * 这一层是**名字与用法**对齐 libdrm（`drmModeGetResources` / `drmModeCreateDumbBuffer` /
 *     `drmModePageFlip` / `drmWaitVBlank` …），底层仍是我们自洽的 `kernel/drm64.h` 那套 ioctl。
 *     也就是说：**今天是"用法兼容"，不是"二进制兼容"** —— 真 libdrm 本体要跑起来还差
 *     "struct 尺寸与 drm.h 逐字节一致"（差值见 docs/Wayland基础抽象层.md 的表）。
 *   * 为什么不直接抄 libdrm 的 struct：那要求 VERSION/GETCRTC/WAIT_VBLANK/GET_SEQUENCE 四处
 *     struct 布局与 Linux 一字不差（含 `__kernel_size_t` + 用户指针），属于一次独立的、需要
 *     真 libdrm 源码做对照的改造。这一层先把"调用形态"固定下来，下一批替换实现即可。
 *
 * 用法（与 libdrm 一样：先拿资源，再建 buffer，最后 flip + 等 vblank）：
 *
 *     int fd = libdrm_open();
 *     struct libdrm_res res;            libdrm_mode_get_resources(fd, &res);
 *     struct libdrm_conn c;             libdrm_mode_get_connector(fd, 1, &c);
 *     struct libdrm_dumb d;             libdrm_mode_create_dumb(fd, 1024, 768, 32, &d);
 *     void* px = libdrm_mode_map_dumb(fd, &d);      // CPU 直接写这些像素
 *     libdrm_mode_page_flip(fd, d.handle, 0);
 *     uint64_t seq = 0;                 libdrm_wait_vblank(fd, &seq);
 *     libdrm_mode_destroy_dumb(fd, d.handle);
 *
 * 全部返回 0 = 成功；负数 = 内核 errno 原样（不翻译，验收程序要原样打印）。
 */
#pragma once
#include <stdint.h>
#include "drm64.h"          /* ABI 唯一真源（kernel/drm64.h 的逐值副本） */

/* ---- 资源 / 连接器 ---- */
struct libdrm_res {
    uint32_t crtcs;
    uint32_t connectors;
    uint32_t fbs;
    uint32_t max_width;
    uint32_t max_height;
};

struct libdrm_conn {
    uint32_t id;
    uint32_t connected;
    uint32_t width;
    uint32_t height;
};

struct libdrm_dumb {
    uint32_t handle;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint64_t size;
    void*    pixels;        /* map_dumb 之后由内核挂进本进程页表的那块 VA */
};

static inline int libdrm_open(void) { return drm64_open(); }

static inline long libdrm_ioctl(int fd, unsigned long cmd, void* arg) {
    return drm64_ioctl(fd, cmd, arg);
}

static inline int libdrm_mode_get_resources(int fd, struct libdrm_res* out) {
    if (!out) return -22;
    struct Drm64ModeResources r;
    for (unsigned i = 0; i < sizeof(r); i++) ((char*)&r)[i] = 0;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETRESOURCES, struct Drm64ModeResources), &r);
    if (rc < 0) return (int)rc;
    out->crtcs      = r.count_crtcs;
    out->connectors = r.count_connectors;
    out->fbs        = r.count_fbs;
    out->max_width  = r.max_width;
    out->max_height = r.max_height;
    return 0;
}

static inline int libdrm_mode_get_connector(int fd, uint32_t id, struct libdrm_conn* out) {
    if (!out) return -22;
    /* 注意：连接器的模式不是内聚在 connector 结构里，而是由调用方给
     * `modes_ptr` + `count_modes` 的用户数组（与 Linux drmModeGetConnector 同一个形式）。 */
    struct Drm64ModeConnector c;
    for (unsigned i = 0; i < sizeof(c); i++) ((char*)&c)[i] = 0;
    struct Drm64ModeMode modes[8];
    for (unsigned i = 0; i < sizeof(modes); i++) ((char*)&modes)[i] = 0;
    c.connector_id = id;
    c.modes_ptr    = (uint64_t)(uintptr_t)modes;
    c.count_modes  = 8;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETCONNECTOR, struct Drm64ModeConnector), &c);
    if (rc < 0) return (int)rc;
    out->id        = c.connector_id;
    out->connected = c.connection ? 1u : 0u;
    out->width     = (c.count_modes > 0) ? modes[0].hdisplay : 0u;
    out->height    = (c.count_modes > 0) ? modes[0].vdisplay : 0u;
    return 0;
}

static inline int libdrm_mode_create_dumb(int fd, uint32_t w, uint32_t h, uint32_t bpp,
                                          struct libdrm_dumb* out) {
    if (!out) return -22;
    struct Drm64ModeCreateDumb cd;
    for (unsigned i = 0; i < sizeof(cd); i++) ((char*)&cd)[i] = 0;
    cd.width = w; cd.height = h; cd.bpp = bpp;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &cd);
    if (rc < 0) return (int)rc;
    out->handle = cd.handle;
    out->width  = w; out->height = h;
    out->pitch  = cd.pitch;
    out->size   = cd.size;
    out->pixels = (void*)0;
    return 0;
}

/* 把这块缓冲挂进本进程并返回可写的 VA（就是 libdrm 的 drmModeMapDumbBuffer 的等价物） */
static inline void* libdrm_mode_map_dumb(int fd, struct libdrm_dumb* d) {
    if (!d) return (void*)0;
    struct Drm64ModeMapDumb md;
    for (unsigned i = 0; i < sizeof(md); i++) ((char*)&md)[i] = 0;
    md.handle = d->handle;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &md);
    if (rc < 0) return (void*)0;
    d->pixels = (void*)(uintptr_t)md.offset;
    return d->pixels;
}

static inline int libdrm_mode_page_flip(int fd, uint32_t fb_id, uint64_t user_data) {
    struct Drm64ModePageFlip pf;
    for (unsigned i = 0; i < sizeof(pf); i++) ((char*)&pf)[i] = 0;
    pf.crtc_id = DRM64_CRTC_ID64;
    pf.fb_id   = fb_id;
    pf.user_data = user_data;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf);
    return (int)rc;
}

static inline int libdrm_wait_vblank(int fd, uint64_t* seq_out) {
    struct Drm64WaitVblank wv;
    for (unsigned i = 0; i < sizeof(wv); i++) ((char*)&wv)[i] = 0;
    wv.crtc_id = DRM64_CRTC_ID64;
    wv.request = 1;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_WAIT_VBLANK, struct Drm64WaitVblank), &wv);
    if (rc < 0) return (int)rc;
    if (seq_out) *seq_out = wv.sequence;
    return 0;
}

static inline int libdrm_mode_destroy_dumb(int fd, uint32_t handle) {
    struct Drm64ModeDestroyDumb dd;
    for (unsigned i = 0; i < sizeof(dd); i++) ((char*)&dd)[i] = 0;
    dd.handle = handle;
    const long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &dd);
    return (int)rc;
}

/* 未知命令反例（验收用）：必须拿到 -ENOTTY(-25)，不许被"包装成成功" */
static inline long libdrm_bad_cmd(int fd) {
    return drm64_ioctl(fd, 0xDEADBEEFul, (void*)0);
}
