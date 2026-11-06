/* drmdemo.c - ★ v0.9.12：DRM/KMS + evdev 的 ring3 **验收程序**
 *
 * 它只做一件事：证明"用户态能通过 /dev/dri/card0 + /dev/input/event0 拿到 Wayland 需要的那几样
 * 内核态抽象"。每一步都打一行纯 ASCII（tests/drm64_test.py 按行断言，格式勿改）：
 *
 *   [DRMDEMO] open fd=<n>                     open("/dev/dri/card0") == 设备特判对象
 *   [DRMDEMO] version major=1 minor=0 patch=0 name=vimtu-drm date=...
 *   [DRMDEMO] getresources crtcs=1 connectors=1 fbs=<n> max=<w>x<h>
 *   [DRMDEMO] getconnector id=1 connected=1 mode=<w>x<h>
 *   [DRMDEMO] create_dumb handle=<n> <w>x<h>@32 pitch=<n> size=<n>
 *   [DRMDEMO] map_dumb handle=<n> va=0x<hex>                ← CPU 可写的那块内存
 *   [DRMDEMO] paint bars=8 barw=64 va=0x<hex> probe=0x<hex>  ← 8 条 64px 宽的竖直彩条 + 读回自证
 *   [DRMDEMO] page_flip handle=<n> rc=0
 *   [DRMDEMO] wait_vblank seq=<n> hw=<0|1> rc=0
 *   [DRMDEMO] badcmd rc=-25                                  ← 未知 ioctl 必须 -ENOTTY（反例）
 *   [DRMDEMO] evdev open fd=<n> name=vimtu-keyboard
 *   [DRMDEMO] evdev read n=<n> ...  或  [DRMDEMO] evdev read skipped rc=<n>
 *   [DRMDEMO] destroy_dumb handle=<n> rc=0
 *   [DRMDEMO] done ok=1
 *
 * 交付：build64.sh 用自研 user/lib 编成静态 ELF64（链接脚本 user/apps/evshm_demo.ld），
 *   **不内嵌内核**；tools/demo_pack_win.py 写进系统卷 /bin/drmdemo.elf。
 *   启动期由 kernel/drm64.cpp 的 drm64_demo64() 当**真进程**跑（触发条件 = 卷里有
 *   /etc/drm64_probe，与 /etc/wl64_probe、/etc/wm_probe 同一套夹具约定）。
 *
 * 呈现分工（与内核侧头文件一致）：**光栅化在这里（CPU）** —— 直接往 MAP_DUMB 给的 VA 里写像素；
 *   内核 page_flip 只做"拷进后备缓冲 + GPU 2D 传输或 CPU 写 LFB"。
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include "vimtu64.h"
#include "drm64.h"

/* 固定色表：与 kernel/drm64.cpp 的 D64_BARS 逐值一致（便于人工对屏/比对） */
static const uint32_t BARS[8] = {
    0xFFFFFFFFu, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
    0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu, 0xFF202020u,
};

#define DRMDEMO_BARW 64u

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;

    /* ---- ① open /dev/dri/card0 ---- */
    const int fd = drm64_open();
    if (fd < 0) {
        printf("[DRMDEMO] open FAILED rc=%d (no /dev/dri/card0)\n", fd);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] open fd=%d\n", fd);

    /* ---- ② VERSION ---- */
    char name[32], date[16], desc[96];
    for (unsigned i = 0; i < sizeof(name); i++) name[i] = 0;
    for (unsigned i = 0; i < sizeof(date); i++) date[i] = 0;
    for (unsigned i = 0; i < sizeof(desc); i++) desc[i] = 0;
    struct Drm64Version v;
    v.version_major = 0; v.version_minor = 0; v.version_patchlevel = 0;
    v.name_len = sizeof(name); v.name = (uint64_t)(uintptr_t)name;
    v.date_len = sizeof(date); v.date = (uint64_t)(uintptr_t)date;
    v.desc_len = sizeof(desc); v.desc = (uint64_t)(uintptr_t)desc;
    long rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_VERSION, struct Drm64Version), &v);
    if (rc != 0) {
        printf("[DRMDEMO] version FAILED rc=%d\n", (int)rc);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] version major=%d minor=%d patch=%d name=%s date=%s\n",
           v.version_major, v.version_minor, v.version_patchlevel, name, date);

    /* ---- ③ GETRESOURCES（一次调用：数组容量一开始就给足）---- */
    uint32_t fbs[DRM64_MAX_DUMB64], crtcs[1], conns[1], encs[1];
    struct Drm64ModeResources r;
    r.fb_id_ptr = (uint64_t)(uintptr_t)fbs;
    r.crtc_id_ptr = (uint64_t)(uintptr_t)crtcs;
    r.connector_id_ptr = (uint64_t)(uintptr_t)conns;
    r.encoder_id_ptr = (uint64_t)(uintptr_t)encs;
    r.count_fbs = DRM64_MAX_DUMB64; r.count_crtcs = 1; r.count_connectors = 1; r.count_encoders = 1;
    r.min_width = 0; r.max_width = 0; r.min_height = 0; r.max_height = 0;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETRESOURCES, struct Drm64ModeResources), &r);
    if (rc != 0) {
        printf("[DRMDEMO] getresources FAILED rc=%d\n", (int)rc);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] getresources crtcs=%u connectors=%u fbs=%u max=%ux%u\n",
           (unsigned)r.count_crtcs, (unsigned)r.count_connectors, (unsigned)r.count_fbs,
           (unsigned)r.max_width, (unsigned)r.max_height);

    /* ---- ④ GETCONNECTOR：status=connected + 模式名 ---- */
    struct Drm64ModeMode mode;
    struct Drm64ModeConnector c;
    for (unsigned i = 0; i < sizeof(mode); i++) ((uint8_t*)&mode)[i] = 0;
    c.encoders_ptr = (uint64_t)(uintptr_t)encs;
    c.modes_ptr = (uint64_t)(uintptr_t)&mode;
    c.props_ptr = 0; c.prop_values_ptr = 0;
    c.count_modes = 1; c.count_props = 0; c.count_encoders = 1;
    c.connector_id = DRM64_CONNECTOR_ID64;      /* 1 */
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETCONNECTOR, struct Drm64ModeConnector), &c);
    if (rc != 0) {
        printf("[DRMDEMO] getconnector FAILED rc=%d\n", (int)rc);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] getconnector id=%u connected=%u mode=%s\n",
           (unsigned)c.connector_id, (unsigned)c.connection, mode.name);

    /* ---- ⑤ CREATE_DUMB：屏幕几何（上限受内核的 8 MiB / 单行 8192 B 把关）---- */
    unsigned dw = mode.hdisplay ? (unsigned)mode.hdisplay : 640u;
    unsigned dh = mode.vdisplay ? (unsigned)mode.vdisplay : 480u;
    if (dw > 1024u) dw = 1024u;            /* 演示用不着整屏：小一点更快，也验证"局部上屏" */
    if (dh > 768u) dh = 768u;
    struct Drm64ModeCreateDumb cd;
    cd.height = dh; cd.width = dw; cd.bpp = 32; cd.flags = 0;
    cd.handle = 0; cd.pitch = 0; cd.size = 0;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &cd);
    if (rc != 0) {
        printf("[DRMDEMO] create_dumb FAILED rc=%d\n", (int)rc);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] create_dumb handle=%u %ux%u@%u pitch=%u size=%lu\n",
           (unsigned)cd.handle, (unsigned)cd.width, (unsigned)cd.height, (unsigned)cd.bpp,
           (unsigned)cd.pitch, (unsigned long)cd.size);

    /* ---- ⑥ MAP_DUMB：拿到**当前进程可写**的用户 VA（本内核语义，见 user/lib/drm64.h）---- */
    struct Drm64ModeMapDumb md;
    md.handle = cd.handle; md.pad = 0; md.offset = 0;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &md);
    if (rc != 0 || md.offset == 0) {
        printf("[DRMDEMO] map_dumb FAILED rc=%d\n", (int)rc);
        printf("[DRMDEMO] done ok=0\n");
        return 1;
    }
    printf("[DRMDEMO] map_dumb handle=%u va=0x%lx\n", (unsigned)md.handle, (unsigned long)md.offset);

    /* ---- ⑦ CPU 光栅化：8 条 64px 宽的竖直彩条（直接写进那块 VA）---- */
    uint32_t* px = (uint32_t*)(uintptr_t)md.offset;
    const unsigned stride_px = (unsigned)(cd.pitch / 4u);
    for (unsigned y = 0; y < dh; y++) {
        uint32_t* row = px + (size_t)y * (size_t)stride_px;
        for (unsigned x = 0; x < dw; x++) {
            unsigned b = x / DRMDEMO_BARW;
            if (b > 7u) b = 7u;
            row[x] = BARS[b];
        }
    }
    /* 读回一个像素做自证：ring3 写进去的字节，ring3 自己能读回来 */
    const uint32_t probe = px[(size_t)(dh / 2u) * (size_t)stride_px];
    printf("[DRMDEMO] paint bars=8 barw=%u va=0x%lx probe=0x%x\n",
           (unsigned)DRMDEMO_BARW, (unsigned long)md.offset, (unsigned)probe);

    /* ---- ⑧ PAGE_FLIP：整块上屏（内核：拷进后备缓冲 -> GPU 传输 或 CPU 写 LFB）---- */
    struct Drm64ModePageFlip pf;
    pf.crtc_id = DRM64_CRTC_ID64; pf.fb_id = cd.handle;
    pf.flags = 0; pf.reserved = 0; pf.user_data = 0x1234u;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf);
    printf("[DRMDEMO] page_flip handle=%u rc=%d\n", (unsigned)pf.fb_id, (int)rc);

    /* ---- ⑨ CRTC_GET_SEQUENCE（拿 active = 硬件回扫是否真可用，纯只读）---- */
    struct Drm64CrtcSeq cs;
    for (unsigned i = 0; i < sizeof(cs); i++) ((char*)&cs)[i] = 0;
    cs.crtc_id = DRM64_CRTC_ID64;
    long rc_seq = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_CRTC_GET_SEQUENCE, struct Drm64CrtcSeq), &cs);
    printf("[DRMDEMO] crtc_seq active=%u seq=%llu ns=%llu rc=%d\n",
           (unsigned)cs.active, (unsigned long long)cs.sequence,
           (unsigned long long)cs.sequence_ns, (int)rc_seq);

    /* ---- ⑨-b WAIT_VBLANK（Linux 语义：type 的 bit0 = 等下一帧）---- */
    struct Drm64WaitVblank wv;
    wv.type = 1; wv.sequence = 0; wv.tval_sec = 0; wv.tval_usec = 0;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_WAIT_VBLANK, struct Drm64WaitVblank), &wv);
    printf("[DRMDEMO] wait_vblank seq=%lu hw=%u rc=%d\n",
           (unsigned long)wv.sequence, (unsigned)cs.active, (int)rc);

    /* ---- 连续第二帧：同一帧缓冲再翻一次 + 再等一次 vblank。
     *   真正的双缓冲要两块 dumb 交替（下一批配合 buffer 生命周期做），
     *   但"连续两帧都在 vblank 处翻转"这件事已经能证明 page_flip+vblank 的语义。 ---- */
    for (int f = 0; f < 2; f++) {
        rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf);
        if (rc != 0) { printf("[DRMDEMO] page_flip#%d FAILED rc=%d\n", f + 2, (int)rc); }
        struct Drm64WaitVblank wv2;
        for (unsigned k = 0; k < sizeof(wv2); k++) ((char*)&wv2)[k] = 0;
        wv2.type = 1;
        rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_WAIT_VBLANK, struct Drm64WaitVblank), &wv2);
        printf("[DRMDEMO] flip_vblank#%d seq=%llu rc=%d\n", f + 2, (unsigned long long)wv2.sequence, (int)rc);
    }

    /* ---- ⑩ 反例：未知 ioctl 必须 -ENOTTY(-25)（不假装成功）---- */
    rc = drm64_ioctl(fd, 0xDEADBEEFul, &cd);
    printf("[DRMDEMO] badcmd rc=%d\n", (int)rc);

    /* ---- ⑪ evdev：键盘原始事件（event0）。没事件 = -EAGAIN，如实跳过 ---- */
    {
        const int efd = evdev64_open(0);
        if (efd < 0) {
            printf("[DRMDEMO] evdev open FAILED rc=%d\n", efd);
        } else {
            char en[32];
            for (unsigned i = 0; i < sizeof(en); i++) en[i] = 0;
            const long n = drm64_ioctl(efd, (unsigned long)EVDEV64_IOCGNAME(sizeof(en)), en);
            printf("[DRMDEMO] evdev open fd=%d name=%s\n", efd, (n > 0) ? en : "?");
            /* 申请键盘焦点再读（没有焦点 = 队列里一条键事件都不会有，见 kernel/input64.h） */
            (void)poll_event((struct Ev64Event*)0, 0, V64_EV_FLAG_FOCUS);
            struct InputEvent64 ev[4];
            const long got = evdev64_read(efd, ev, (long)sizeof(ev));
            if (got > 0) {
                const long cnt = got / (long)sizeof(struct InputEvent64);
                printf("[DRMDEMO] evdev read n=%ld type=%u code=%u value=%d\n",
                       cnt, (unsigned)ev[0].type, (unsigned)ev[0].code, (int)ev[0].value);
            } else {
                printf("[DRMDEMO] evdev read skipped rc=%d (empty queue)\n", (int)got);
            }
            (void)poll_event((struct Ev64Event*)0, 0, V64_EV_FLAG_RELEASE);
        }
    }

    /* ---- ⑫ DESTROY_DUMB ---- */
    struct Drm64ModeDestroyDumb dd;
    dd.handle = cd.handle;
    rc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &dd);
    printf("[DRMDEMO] destroy_dumb handle=%u rc=%d\n", (unsigned)dd.handle, (int)rc);

    printf("[DRMDEMO] done ok=%d\n", (rc == 0) ? 1 : 0);
    return (rc == 0) ? 0 : 1;
}
