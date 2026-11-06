/* gbm_compat.h - ★ v0.9.21：Linux **GBM** 的精简适配（分配 + 映射 + 轮换），不含 EGL/GLES
 *
 * 定位：wlroots 的 DRM 后端要 GBM 才能建扫描输出缓冲（gbm_surface/gbm_bo）。本内核没有 Mesa，
 *       所以我们不做 EGL/GLES —— 但 **GBM 这一层的语义是可以真做的**：GBM 的 BO 就是
 *       "一块能交给 DRM 扫描输出的像素缓冲"。本文件把 GBM 的 API 子集直接落在
 *       既有的 DRM dumb buffer（CREATE_DUMB + MAP_DUMB）上，**不新增任何内核 ABI**。
 *
 * 已实现：
 *   gbm_create_device(fd) / gbm_device_destroy / gbm_device_get_fd
 *   gbm_bo_create(dev, w, h, format, flags)      -> CREATE_DUMB + MAP_DUMB
 *   gbm_bo_map / gbm_bo_unmap                    -> 用户 VA（本内核的 dumb 映射）
 *   gbm_bo_get_handle / get_stride / get_width / get_height / get_format
 *   gbm_bo_destroy                               -> DESTROY_DUMB
 *   gbm_surface_create / gbm_surface_lock_front_buffer / gbm_surface_release_buffer
 *   gbm_surface_has_free_buffers / gbm_surface_destroy
 *
 * 如实差异（不假装）：
 *   1) **没有 EGL/GLES**：gmr/eglCreateWindowSurface/gl* 一行都没有（需要 Mesa，见 docs §8）。
 *      所以 gbm_surface_lock_front_buffer() 返回的 BO 可以被 DRM 直接 page_flip 上屏，
 *      但**不能**被 GL 渲染 —— 客户端只能自己往 gbm_bo_map() 拿到的内存里写像素。
 *   2) gbm_bo_get_handle 返回的是**本内核的 dumb handle**（uint32_t，Linux 那边是 union gbm_bo_handle），
 *      所以按 Linux 代码 `bo->handle.u32` 写的地方要改成直接拿返回值。
 *   3) 没有 modifier / tiling / dmabuf 导入导出（PRIME 的 handle<->id 在 DRM 侧有，GBM 侧没包）。
 */
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include "drm64.h"

#define GBM_FORMAT_XRGB8888 0x34325258u     /* 与 DRM64_FORMAT_XRGB8888 / Linux GBM_FORMAT_XRGB8888 同值 */
#define GBM_FORMAT_ARGB8888 0x34325241u
#define GBM_BO_USE_SCANOUT  (1u << 0)
#define GBM_BO_USE_RENDERING (1u << 2)
#define GBM_BO_USE_LINEAR   (1u << 4)

struct gbm_device { int fd; };

struct gbm_bo {
    uint32_t handle;        /* 本内核的 dumb handle（见文件头第 2 条） */
    uint32_t width, height;
    uint32_t stride;
    uint32_t format;
    uint64_t size;
    uint64_t uva;           /* MAP_DUMB 拿到的用户 VA（0 = 没映射） */
};

struct gbm_surface {
    struct gbm_device* dev;
    struct gbm_bo* bo[2];
    int n;
    int front;              /* 0/1 = front 下标；-1 = 还没锁定过 */
};

static inline struct gbm_device* gbm_create_device(int fd) {
    if (fd < 0) return 0;
    struct gbm_device* d = (struct gbm_device*)malloc(sizeof(struct gbm_device));
    if (!d) return 0;
    d->fd = fd;
    return d;
}
static inline void gbm_device_destroy(struct gbm_device* d) { if (d) free(d); }
static inline int gbm_device_get_fd(struct gbm_device* d) { return d ? d->fd : -1; }

static inline struct gbm_bo* gbm_bo_create(struct gbm_device* d, uint32_t w, uint32_t h,
                                           uint32_t format, uint32_t flags) {
    (void)flags;
    if (!d || w == 0u || h == 0u) return 0;
    if (format != GBM_FORMAT_XRGB8888 && format != GBM_FORMAT_ARGB8888) return 0;  /* 如实只支持这两种 */
    struct Drm64ModeCreateDumb cd;
    for (unsigned i = 0; i < sizeof(cd); i++) ((char*)&cd)[i] = 0;
    cd.width = w; cd.height = h; cd.bpp = 32u;
    if (drm64_ioctl(d->fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &cd) != 0) return 0;
    struct Drm64ModeMapDumb md;
    for (unsigned i = 0; i < sizeof(md); i++) ((char*)&md)[i] = 0;
    md.handle = cd.handle;
    if (drm64_ioctl(d->fd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &md) != 0) {
        struct Drm64ModeDestroyDumb dd;
        dd.handle = cd.handle;
        (void)drm64_ioctl(d->fd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &dd);
        return 0;
    }
    struct gbm_bo* bo = (struct gbm_bo*)malloc(sizeof(struct gbm_bo));
    if (!bo) return 0;
    bo->handle = cd.handle; bo->width = w; bo->height = h;
    bo->stride = cd.pitch; bo->format = format; bo->size = cd.size;
    bo->uva = md.offset;
    return bo;
}
static inline void gbm_bo_destroy(struct gbm_device* d, struct gbm_bo* bo) {
    if (!bo) return;
    if (d && bo->handle) {
        struct Drm64ModeDestroyDumb dd;
        dd.handle = bo->handle;
        (void)drm64_ioctl(d->fd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &dd);
    }
    free(bo);
}
static inline void* gbm_bo_map(struct gbm_bo* bo) { return (bo && bo->uva) ? (void*)(uintptr_t)bo->uva : 0; }
static inline void  gbm_bo_unmap(struct gbm_bo* bo) { (void)bo; }   /* 映射随进程页表，没有单独 unmap 语义 */
static inline uint32_t gbm_bo_get_handle(struct gbm_bo* bo) { return bo ? bo->handle : 0u; }
static inline uint32_t gbm_bo_get_stride(struct gbm_bo* bo) { return bo ? bo->stride : 0u; }
static inline uint32_t gbm_bo_get_width(struct gbm_bo* bo) { return bo ? bo->width : 0u; }
static inline uint32_t gbm_bo_get_height(struct gbm_bo* bo) { return bo ? bo->height : 0u; }
static inline uint32_t gbm_bo_get_format(struct gbm_bo* bo) { return bo ? bo->format : 0u; }

static inline struct gbm_surface* gbm_surface_create(struct gbm_device* d, uint32_t w, uint32_t h,
                                                     uint32_t format, uint32_t flags) {
    if (!d) return 0;
    struct gbm_surface* s = (struct gbm_surface*)malloc(sizeof(struct gbm_surface));
    if (!s) return 0;
    s->dev = d; s->n = 0; s->front = -1;
    for (int i = 0; i < 2; i++) {
        s->bo[i] = gbm_bo_create(d, w, h, format, flags);
        if (!s->bo[i]) {                       /* 建不出两块就如实失败（不返回半残的 surface） */
            for (int k = 0; k < i; k++) { gbm_bo_destroy(d, s->bo[k]); s->bo[k] = 0; }
            free(s);
            return 0;
        }
        s->n++;
    }
    return s;
}
/* 双缓冲轮换：返回**可被 DRM page_flip 上屏**的那块 BO（GBM 语义：lock = 拿一帧给 compositor 用） */
static inline struct gbm_bo* gbm_surface_lock_front_buffer(struct gbm_surface* s) {
    if (!s || s->n <= 0) return 0;
    s->front = (s->front + 1) % s->n;
    return s->bo[s->front];
}
static inline void gbm_surface_release_buffer(struct gbm_surface* s, struct gbm_bo* bo) { (void)s; (void)bo; }
static inline int  gbm_surface_has_free_buffers(struct gbm_surface* s) { return (s && s->n > 0) ? 1 : 0; }
static inline void gbm_surface_destroy(struct gbm_surface* s) {
    if (!s) return;
    for (int i = 0; i < s->n; i++) gbm_bo_destroy(s->dev, s->bo[i]);
    free(s);
}
