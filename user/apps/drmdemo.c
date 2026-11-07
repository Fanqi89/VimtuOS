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
 *   [DRMDEMO] libevdev name=<s> absx=<rc> min=<n> max=<n> grab=<rc>/<rc> types=0x<hex>
 *   [DRMDEMO] gbm bo handle=<n> <w>x<h> stride=<n> probe=0x<hex>
 *   [DRMDEMO] gbm surface n=2 front=<i>/<j> stride=<n> rc=0
 *   [DRMDEMO] modeswitch list=<n> pick=<w>x<h> cur=<w>x<h> ilace=0   ← ★ v0.9.23 运行时切模式
 *   [DRMDEMO] setcrtc mode=<w>x<h> rc=0 / setcrtc restore=<w>x<h> rc=0 / modeswitch reflip rc=0
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
#include "libevdev_compat.h"   /* ★ v0.9.21：libevdev 精简适配（grab / 绝对轴 / 事件流） */
#include "gbm_compat.h"        /* ★ v0.9.21：GBM 分配层（dumb + PRIME 之上的最小封装） */

/* 固定色表：与 kernel/drm64.cpp 的 D64_BARS 逐值一致（便于人工对屏/比对） */
static const uint32_t BARS[8] = {
    0xFFFFFFFFu, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
    0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu, 0xFF202020u,
};

#define DRMDEMO_BARW 64u

/* ★ v0.9.20：有界 poll(POLLIN) —— 用户态没有 <poll.h>，这里直接用 Linux 号段 7（poll）。
 * 返回 >0 = 就绪（bit0 = POLLIN）/ 0 = 超时 / 负数 = 内核的 -errno。 */
struct DrmDemoPollFd { int fd; short events; short revents; };
static int drmdemo_poll_in(int fd, int ms) {
    struct DrmDemoPollFd p;
    p.fd = fd; p.events = 1; p.revents = 0;
    const long r = __v64_syscall(7, (long)(uintptr_t)&p, 1, (long)ms, 0, 0);
    return (r > 0) ? (int)p.revents : (int)r;
}
static void drmdemo_zero(void* p, unsigned n) { for (unsigned i = 0; i < n; i++) ((char*)p)[i] = 0; }

int main(int argc, char** argv, char** envp) {
    (void)argc; (void)argv; (void)envp;
    (void)argc; (void)argv; (void)envp;
    uint32_t fb_ok = 0;          /* ⑨-d 建的 fb id，⑨-f 的 atomic 请求要用 */
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

    /* ---- ⑨-c ★ v0.9.15：plane 枚举（wlroots 的非 atomic 路径先走这两步）---- */
    {
        uint32_t planes[2];
        struct Drm64ModeGetPlaneRes pr;
        for (unsigned i = 0; i < sizeof(pr); i++) ((char*)&pr)[i] = 0;
        pr.plane_id_ptr = (uint64_t)(uintptr_t)planes;
        pr.count_planes = 2;
        long prc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPLANERES, struct Drm64ModeGetPlaneRes), &pr);
        printf("[DRMDEMO] plane_res n=%u ids=%u,%u rc=%d\n", (unsigned)pr.count_planes,
               (unsigned)planes[0], (unsigned)(pr.count_planes > 1u ? planes[1] : 0u), (int)prc);

        struct Drm64ModeGetPlane pl;
        uint32_t fmts[2];
        for (unsigned i = 0; i < sizeof(pl); i++) ((char*)&pl)[i] = 0;
        fmts[0] = 0; fmts[1] = 0;
        pl.plane_id = planes[0];
        pl.format_type_ptr = (uint64_t)(uintptr_t)fmts;
        pl.count_format_types = 2;
        long plc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPLANE, struct Drm64ModeGetPlane), &pl);
        printf("[DRMDEMO] plane id=%u crtc=%u fb=%u formats=%u fmt0=0x%x rc=%d\n",
               (unsigned)pl.plane_id, (unsigned)pl.crtc_id, (unsigned)pl.fb_id,
               (unsigned)pl.count_format_types, (unsigned)fmts[0], (int)plc);
    }

    /* ---- ⑨-d ★ v0.9.15：ADDFB2 建 fb 对象 -> SETCRTC 用 fb id 上屏（libdrm 的正路）---- */
    {
        struct Drm64ModeFbCmd2 fb2;
        for (unsigned i = 0; i < sizeof(fb2); i++) ((char*)&fb2)[i] = 0;
        fb2.width = dw; fb2.height = dh;
        fb2.pixel_format = DRM64_FORMAT_XRGB8888;
        fb2.handles[0] = cd.handle;
        fb2.pitches[0] = cd.pitch;
        long arc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ADDFB2, struct Drm64ModeFbCmd2), &fb2);
        printf("[DRMDEMO] addfb2 fb=%u handle=%u rc=%d\n",
               (unsigned)fb2.fb_id, (unsigned)fb2.handles[0], (int)arc);
        if (arc == 0) fb_ok = fb2.fb_id;

        /* 反例：NV12（0x3231564E）本内核不支持 -> 必须 -EINVAL(-22)，不假装成功 */
        struct Drm64ModeFbCmd2 bad;
        for (unsigned i = 0; i < sizeof(bad); i++) ((char*)&bad)[i] = 0;
        bad.width = dw; bad.height = dh;
        bad.pixel_format = 0x3231564Eu;
        bad.handles[0] = cd.handle;
        bad.pitches[0] = cd.pitch;
        long brc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ADDFB2, struct Drm64ModeFbCmd2), &bad);
        printf("[DRMDEMO] addfb2 badfmt rc=%d\n", (int)brc);

        if (arc == 0) {
            struct Drm64ModeCrtc sc;
            for (unsigned i = 0; i < sizeof(sc); i++) ((char*)&sc)[i] = 0;
            sc.crtc_id = DRM64_CRTC_ID64;
            sc.fb_id = fb2.fb_id;
            sc.mode_valid = 1;
            sc.mode = mode;                  /* 用 getconnector 拿到的那唯一一个模式 */
            long src = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_SETCRTC, struct Drm64ModeCrtc), &sc);
            printf("[DRMDEMO] setcrtc crtc=1 fb=%u rc=%d\n", (unsigned)sc.fb_id, (int)src);
        }
    }

    /* ---- ⑨-f ★ v0.9.16：属性系统 + atomic commit（wlroots 的 DRM 后端走的就是这条路）---- */
    {
        struct Drm64ModeGetEncoder en;
        for (unsigned i = 0; i < sizeof(en); i++) ((char*)&en)[i] = 0;
        en.encoder_id = DRM64_ENCODER_ID64;
        long erc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETENCODER, struct Drm64ModeGetEncoder), &en);
        printf("[DRMDEMO] encoder id=%u type=%u crtc=%u possible_crtcs=0x%x rc=%d\n",
               (unsigned)en.encoder_id, (unsigned)en.encoder_type, (unsigned)en.crtc_id,
               (unsigned)en.possible_crtcs, (int)erc);
    }
    {
        /* ① OBJ_GETPROPERTIES：两阶段（先问个数，再取 props/values 数组） */
        struct Drm64ModeObjGetProps og;
        for (unsigned i = 0; i < sizeof(og); i++) ((char*)&og)[i] = 0;
        og.obj_id = DRM64_PLANE_PRIMARY_ID64; og.obj_type = DRM64_OBJ_PLANE;
        long orc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_OBJ_GETPROPS, struct Drm64ModeObjGetProps), &og);
        uint32_t pids[16], pend = og.count_props;
        uint64_t pvals[16];
        for (unsigned i = 0; i < 16; i++) { pids[i] = 0; pvals[i] = 0; }
        og.props_ptr = (uint64_t)(uintptr_t)pids;
        og.prop_values_ptr = (uint64_t)(uintptr_t)pvals;
        og.count_props = 16;
        orc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_OBJ_GETPROPS, struct Drm64ModeObjGetProps), &og);
        printf("[DRMDEMO] obj_props plane n=%u counted=%u first=%u fbval=%llu rc=%d\n",
               (unsigned)og.count_props, (unsigned)pend, (unsigned)pids[0],
               (unsigned long long)pvals[1], (int)orc);

        /* ② GETPROPERTY：FB_ID 的名字/标志 */
        struct Drm64ModeGetProperty gp;
        for (unsigned i = 0; i < sizeof(gp); i++) ((char*)&gp)[i] = 0;
        gp.prop_id = DRM64_PROP_FB_ID;
        long grc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPROPERTY, struct Drm64ModeGetProperty), &gp);
        printf("[DRMDEMO] prop id=%u name=%s flags=0x%x rc=%d\n",
               (unsigned)gp.prop_id, gp.name, (unsigned)gp.flags, (int)grc);

        /* ③ GETPROPBLOB：IN_FORMATS（按 Linux 的 drm_format_modifier_blob 解析） */
        struct Drm64ModeGetBlob gb;
        for (unsigned i = 0; i < sizeof(gb); i++) ((char*)&gb)[i] = 0;
        gb.blob_id = DRM64_BLOB_FMT_PRIMARY;
        long brc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPROPBLOB, struct Drm64ModeGetBlob), &gb);
        uint32_t bloblen = gb.length;
        uint8_t fmtblob[64];
        for (unsigned i = 0; i < sizeof(fmtblob); i++) fmtblob[i] = 0;
        gb.data = (uint64_t)(uintptr_t)fmtblob;
        gb.length = (uint32_t)sizeof(fmtblob);
        brc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPROPBLOB, struct Drm64ModeGetBlob), &gb);
        const uint32_t* ih = (const uint32_t*)fmtblob;
        printf("[DRMDEMO] blob_informats len=%u version=%u formats=%u mods=%u fmt0=0x%x rc=%d\n",
               (unsigned)bloblen, (unsigned)ih[0], (unsigned)ih[2], (unsigned)ih[4],
               (unsigned)((const uint32_t*)(fmtblob + 24))[0], (int)brc);

        /* ④ CREATEBLOB：把当前模式做成 MODE_ID blob（libdrm 的 drmModeCreatePropertyBlob 就是这条） */
        struct Drm64ModeCreateBlob cb;
        for (unsigned i = 0; i < sizeof(cb); i++) ((char*)&cb)[i] = 0;
        cb.data = (uint64_t)(uintptr_t)&mode;
        cb.length = (uint32_t)sizeof(mode);
        long crc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATEBLOB, struct Drm64ModeCreateBlob), &cb);
        printf("[DRMDEMO] blob_mode id=%u len=%u rc=%d\n",
               (unsigned)cb.blob_id, (unsigned)cb.length, (int)crc);

        /* ⑤ atomic 请求：plane(FB_ID/CRTC_ID/SRC/CRTC) + crtc(ACTIVE/MODE_ID) + connector(CRTC_ID) */
        uint32_t aobjs[3] = { DRM64_PLANE_PRIMARY_ID64, DRM64_CRTC_ID64, DRM64_CONNECTOR_ID64 };
        uint32_t acnts[3] = { 7u, 2u, 1u };
        uint32_t aprops[10] = {
            DRM64_PROP_FB_ID, DRM64_PROP_CRTC_ID, DRM64_PROP_SRC_X, DRM64_PROP_SRC_Y,
            DRM64_PROP_SRC_W, DRM64_PROP_SRC_H, DRM64_PROP_CRTC_W,
            DRM64_PROP_ACTIVE, DRM64_PROP_MODE_ID,
            DRM64_PROP_CRTC_ID,
        };
        uint64_t avals[10];
        avals[0] = fb_ok;
        avals[1] = DRM64_CRTC_ID64;
        avals[2] = 0;
        avals[3] = 0;
        avals[4] = (uint64_t)dw << 16;
        avals[5] = (uint64_t)dh << 16;
        avals[6] = dw;
        avals[7] = 1;
        avals[8] = cb.blob_id;
        avals[9] = DRM64_CRTC_ID64;
        struct Drm64ModeAtomic at;
        for (unsigned i = 0; i < sizeof(at); i++) ((char*)&at)[i] = 0;
        at.count_objs = 3;
        at.objs_ptr = (uint64_t)(uintptr_t)aobjs;
        at.count_props_ptr = (uint64_t)(uintptr_t)acnts;
        at.props_ptr = (uint64_t)(uintptr_t)aprops;
        at.prop_values_ptr = (uint64_t)(uintptr_t)avals;
        at.user_data = 0x5150u;

        /* 反例一：缩放（SRC_W = dw/2 而 CRTC_W = dw）-> 必须 -EINVAL(-22) */
        uint64_t save = avals[4];
        avals[4] = (uint64_t)(dw / 2u) << 16;
        at.flags = DRM64_ATOMIC_TEST_ONLY64 | DRM64_ATOMIC_ALLOW_MODESET64;
        long a1 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ATOMIC, struct Drm64ModeAtomic), &at);
        printf("[DRMDEMO] atomic badscale rc=%d\n", (int)a1);
        avals[4] = save;

        /* 反例二：PAGE_FLIP_EVENT 位（本内核不投递事件）-> 必须 -EINVAL */
        at.flags = 0x01u;
        long a2 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ATOMIC, struct Drm64ModeAtomic), &at);
        printf("[DRMDEMO] atomic badevent rc=%d\n", (int)a2);

        /* 正例一：TEST_ONLY（全量校验，**一个像素都不动**） */
        at.flags = DRM64_ATOMIC_TEST_ONLY64 | DRM64_ATOMIC_ALLOW_MODESET64;
        long a3 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ATOMIC, struct Drm64ModeAtomic), &at);
        printf("[DRMDEMO] atomic test_only rc=%d\n", (int)a3);

        /* 正例二：真提交 */
        at.flags = DRM64_ATOMIC_ALLOW_MODESET64;
        long a4 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_ATOMIC, struct Drm64ModeAtomic), &at);
        printf("[DRMDEMO] atomic commit rc=%d user_data=0x%lx\n", (int)a4, (unsigned long)at.user_data);

        /* 复核：GETPLANE 必须报"刚提交的那个 fb"，GETCRTC 也一样 */
        struct Drm64ModeGetPlane pl2;
        for (unsigned i = 0; i < sizeof(pl2); i++) ((char*)&pl2)[i] = 0;
        pl2.plane_id = DRM64_PLANE_PRIMARY_ID64;
        long p2 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETPLANE, struct Drm64ModeGetPlane), &pl2);
        struct Drm64ModeCrtc gc;
        for (unsigned i = 0; i < sizeof(gc); i++) ((char*)&gc)[i] = 0;
        long g2 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETCRTC, struct Drm64ModeCrtc), &gc);
        printf("[DRMDEMO] after_atomic plane_fb=%u crtc_fb=%u valid=%u rc=%d/%d\n",
               (unsigned)pl2.fb_id, (unsigned)gc.fb_id, (unsigned)gc.mode_valid, (int)p2, (int)g2);

        /* 收尾：把 MODE_ID blob 还回去（固定 blob 不能删，用户 blob 可以） */
        if (crc == 0) {
            struct Drm64ModeDestroyBlob db;
            db.blob_id = cb.blob_id;
            long drc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DESTROYBLOB, struct Drm64ModeDestroyBlob), &db);
            printf("[DRMDEMO] blob_destroy id=%u rc=%d\n", (unsigned)db.blob_id, (int)drc);
        }
    }
    /* ---- ⑨-g ★ v0.9.17：2D 加速 —— 脏矩形（DIRTYFB）+ 双缓冲交替 ----
     *   两块 dumb 轮换：只有第一次整块上传，之后**只搬脏矩形**。
     *   证据：内核 [DRM] dirty / [DRM] present ... bytes=<小> / [DRM] flip ... alt=1。 ---- */
    {
        struct Drm64ModeCreateDumb b2;
        for (unsigned i = 0; i < sizeof(b2); i++) ((char*)&b2)[i] = 0;
        b2.height = dh; b2.width = dw; b2.bpp = 32;
        long r1 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &b2);
        struct Drm64ModeMapDumb mb;
        for (unsigned i = 0; i < sizeof(mb); i++) ((char*)&mb)[i] = 0;
        mb.handle = b2.handle;
        long r2 = (r1 == 0) ? drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &mb) : -1;
        printf("[DRMDEMO] dumb_b handle=%u rc=%d/%d\n", (unsigned)b2.handle, (int)r1, (int)r2);
        if (r1 == 0 && r2 == 0 && mb.offset != 0 && b2.pitch == cd.pitch) {
            uint32_t* p2 = (uint32_t*)(uintptr_t)mb.offset;
            const unsigned sp2 = (unsigned)(b2.pitch / 4u);
            for (unsigned y = 0; y < dh; y++) {
                uint32_t* row = p2 + (size_t)y * (size_t)sp2;
                for (unsigned x = 0; x < dw; x++) {
                    unsigned b = x / DRMDEMO_BARW;
                    if (b > 7u) b = 7u;
                    row[x] = BARS[7u - b];              /* B 块反色，肉眼可辨双缓冲 */
                }
            }
            /* 反例：flags 必须 0 */
            struct Drm64ClipRect cr;
            cr.x1 = 0; cr.y1 = 0; cr.x2 = 128; cr.y2 = 64;
            struct Drm64ModeFbDirty dpf;
            for (unsigned i = 0; i < sizeof(dpf); i++) ((char*)&dpf)[i] = 0;
            dpf.fb_id = b2.handle; dpf.num_clips = 1; dpf.clips_ptr = (uint64_t)(uintptr_t)&cr;
            dpf.flags = 1;
            long d0 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DIRTYFB, struct Drm64ModeFbDirty), &dpf);
            printf("[DRMDEMO] dirtyfb badflags rc=%d\n", (int)d0);
            dpf.flags = 0;
            long d1 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DIRTYFB, struct Drm64ModeFbDirty), &dpf);
            printf("[DRMDEMO] dirtyfb handle=%u clips=1 rc=%d\n", (unsigned)b2.handle, (int)d1);
            /* 翻到 B：这次只该搬 128x64 = 32 KiB */
            struct Drm64ModePageFlip pf2;
            for (unsigned i = 0; i < sizeof(pf2); i++) ((char*)&pf2)[i] = 0;
            pf2.crtc_id = DRM64_CRTC_ID64; pf2.fb_id = b2.handle;
            long f1 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf2);
            printf("[DRMDEMO] flip_b rc=%d\n", (int)f1);
            /* 再翻回 A（没有脏矩形 -> 整块 3 MiB） */
            pf.fb_id = cd.handle;
            long f2 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf);
            printf("[DRMDEMO] flip_a rc=%d\n", (int)f2);
            struct Drm64ModeDestroyDumb db2;
            db2.handle = b2.handle;
            long r3 = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &db2);
            printf("[DRMDEMO] destroy_dumb_b rc=%d\n", (int)r3);
        }
    }

    /* ---- ⑨-e ★ v0.9.15：**故意不销毁**的一块 dumb（64x64，占 4 页）----
     *   它不 DESTROY_DUMB 就退出，靠内核的进程回收 hook 收走 ——
     *   串口应出现 [DRM] release pid=<本进程> fbs=1 pages=4，页池水位回原位。 ---- */
    {
        struct Drm64ModeCreateDumb leak;
        leak.height = 64; leak.width = 64; leak.bpp = 32; leak.flags = 0;
        leak.handle = 0; leak.pitch = 0; leak.size = 0;
        long lrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &leak);
        printf("[DRMDEMO] leak_dumb handle=%u pages_hint=%lu rc=%d\n",
               (unsigned)leak.handle, (unsigned long)(leak.size / 4096u), (int)lrc);
    }

    /* ---- ⑬ ★ v0.9.20：PRIME 跨进程 —— A 导出，子进程导入并读回 A 写进去的像素 ----
     *   证据：内核两条 [DRM] map 行 **phys 相同、pid 不同**（同一批物理页挂进两个进程）。 ---- */
    {
        struct Drm64PrimeHandle ph;
        drmdemo_zero(&ph, (unsigned)sizeof(ph));
        ph.handle = cd.handle;
        const long exrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_PRIME_HANDLE_TO_FD, struct Drm64PrimeHandle), &ph);
        printf("[DRMDEMO] prime export handle=%u id=0x%x rc=%ld\n",
               (unsigned)cd.handle, (unsigned)ph.fd, (long)exrc);
        /* 反例：不存在的 handle 必须 -EINVAL */
        struct Drm64PrimeHandle bx;
        drmdemo_zero(&bx, (unsigned)sizeof(bx));
        bx.handle = 999;
        const long bxrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_PRIME_HANDLE_TO_FD, struct Drm64PrimeHandle), &bx);
        printf("[DRMDEMO] prime export bad-handle rc=%ld\n", (long)bxrc);
        /* 反例：负数 id 不是 prime id（更不是 fd）—— 必须 -EINVAL */
        struct Drm64PrimeHandle bn;
        drmdemo_zero(&bn, (unsigned)sizeof(bn));
        bn.fd = -1;
        const long bnrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_PRIME_FD_TO_HANDLE, struct Drm64PrimeHandle), &bn);
        printf("[DRMDEMO] prime import bad-id rc=%ld\n", (long)bnrc);
        if (exrc == 0) {
            const int kid = fork();
            if (kid == 0) {
                /* 子进程：自己的 DRM fd -> FD_TO_HANDLE -> MAP_DUMB -> 读 A 写进去的白色条 */
                const int cfd = drm64_open();
                struct Drm64PrimeHandle ih;
                drmdemo_zero(&ih, (unsigned)sizeof(ih));
                ih.fd = ph.fd;
                const long irc = (cfd >= 0)
                    ? drm64_ioctl(cfd, DRM64_IOWR(DRM64_NR_PRIME_FD_TO_HANDLE, struct Drm64PrimeHandle), &ih) : -1;
                struct Drm64ModeMapDumb imd;
                drmdemo_zero(&imd, (unsigned)sizeof(imd));
                imd.handle = ih.handle;
                const long mrc = (irc == 0)
                    ? drm64_ioctl(cfd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &imd) : -1;
                uint32_t probe = 0;
                if (mrc == 0 && imd.offset != 0) probe = *(volatile uint32_t*)(uintptr_t)imd.offset;
                /* 收工：把自己那份引用还掉（refs 2 -> 1，页**不能**被释放 —— 导出方还在用） */
                struct Drm64ModeDestroyDumb idb;
                drmdemo_zero(&idb, (unsigned)sizeof(idb));
                idb.handle = ih.handle;
                const long drc = (irc == 0)
                    ? drm64_ioctl(cfd, DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, struct Drm64ModeDestroyDumb), &idb) : -1;
                printf("[DRMDEMO] prime import id=0x%x handle=%u rc=%ld/%ld probe=0x%x release=%ld\n",
                       (unsigned)ph.fd, (unsigned)ih.handle, (long)irc, (long)mrc,
                       (unsigned)probe, (long)drc);
                _exit((mrc == 0 && probe == BARS[0] && drc == 0) ? 0 : 1);
            }
            int st = 0;
            const int wr = wait4(kid, &st, 0, 0);
            printf("[DRMDEMO] prime child pid=%d wait=%d status=%d\n", kid, wr, st);
        }
    }

    /* ---- ⑭ ★ v0.9.20：事件投递 —— PAGE_FLIP(EVENT) 立即返回 -> poll(POLLIN) -> read 32 B 事件 ---- */
    {
        struct Drm64ModePageFlip pe;
        drmdemo_zero(&pe, (unsigned)sizeof(pe));
        pe.crtc_id = DRM64_CRTC_ID64; pe.fb_id = cd.handle;
        pe.flags = DRM64_PAGE_FLIP_EVENT64; pe.user_data = 0xAB12CD34ull;
        const long erc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pe);
        const int pin = drmdemo_poll_in(fd, 500);
        struct Drm64EventVblank evb;
        drmdemo_zero(&evb, (unsigned)sizeof(evb));
        const long rd = drm64_read(fd, &evb, (long)sizeof(evb));
        printf("[DRMDEMO] event flip_rc=%ld poll=%d read=%ld type=%u len=%u seq=%u crtc=%u user_data=0x%lx\n",
               (long)erc, pin, (long)rd, (unsigned)evb.type, (unsigned)evb.length,
               (unsigned)evb.sequence, (unsigned)evb.crtc_id, (unsigned long)evb.user_data);
        /* 反例：不认识的 flag 位（不是 PAGE_FLIP_EVENT）必须 -EINVAL */
        struct Drm64ModePageFlip px;
        drmdemo_zero(&px, (unsigned)sizeof(px));
        px.crtc_id = DRM64_CRTC_ID64; px.fb_id = cd.handle; px.flags = 0x80u;
        const long pxrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &px);
        printf("[DRMDEMO] event badflag rc=%ld\n", (long)pxrc);
        /* 同步语义没退化：不带 flag 的 flip 仍然返回 0 且内核仍打 [DRM] page_flip 行 */
        struct Drm64ModePageFlip ps;
        drmdemo_zero(&ps, (unsigned)sizeof(ps));
        ps.crtc_id = DRM64_CRTC_ID64; ps.fb_id = cd.handle; ps.flags = 0;
        const long psrc = drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &ps);
        printf("[DRMDEMO] event sync_flip rc=%ld\n", (long)psrc);
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

    /* ---- ⑮ ★ v0.9.21：libevdev 精简适配（libinput 的 evdev 后端要的这几条）----
     *   证据：内核侧 [EVDEV] ioctl … abs=0 min=0 max=<屏宽-1>（EVIOCGABS）与 grab=1/0（EVIOCGRAB）。 ---- */
    {
        const int mfd = evdev64_open(1);                 /* event1 = 鼠标（有 REL/ABS） */
        struct libevdev* lev = 0;
        const int nrc = (mfd >= 0) ? libevdev_new_from_fd(mfd, &lev) : -1;
        if (nrc == 0 && lev) {
            struct InputAbsInfo64 ai;
            for (unsigned i = 0; i < sizeof(ai); i++) ((char*)&ai)[i] = 0;
            const int arc = libevdev_get_abs_info(lev, EVDEV64_ABS_X64, &ai);
            const int g1 = libevdev_grab(lev, LIBEVDEV_GRAB);       /* EVIOCGRAB = 1 */
            const int g0 = libevdev_grab(lev, LIBEVDEV_UNGRAB);     /* EVIOCGRAB = 0 */
            printf("[DRMDEMO] libevdev name=%s absx=%d min=%d max=%d grab=%d/%d types=0x%lx\n",
                   libevdev_get_name(lev), arc, (int)ai.minimum, (int)ai.maximum,
                   g1, g0, (unsigned long)lev->evbits);
            libevdev_free(lev);
        } else {
            printf("[DRMDEMO] libevdev open FAILED rc=%d\n", nrc);
        }
    }

    /* ---- ⑯ ★ v0.9.21：GBM 分配层（真 BO / 真映射 / 双缓冲轮换；**EGL/GLES 仍未做**）----
     *   证据：内核侧 [DRM] create/map/destroy 行（gbm_bo_create 就是 CREATE_DUMB + MAP_DUMB）。 ---- */
    {
        struct gbm_device* gdev = gbm_create_device(fd);
        if (!gdev) {
            printf("[DRMDEMO] gbm device FAILED\n");
        } else {
            struct gbm_bo* bo = gbm_bo_create(gdev, 64u, 64u, GBM_FORMAT_XRGB8888, GBM_BO_USE_SCANOUT);
            if (bo) {
                volatile uint32_t* p = (volatile uint32_t*)gbm_bo_map(bo);
                if (p) p[0] = 0x5AFE1234u;
                const unsigned probe = p ? (unsigned)p[0] : 0u;
                printf("[DRMDEMO] gbm bo handle=%u %ux%u stride=%u probe=0x%x\n",
                       (unsigned)gbm_bo_get_handle(bo), (unsigned)gbm_bo_get_width(bo),
                       (unsigned)gbm_bo_get_height(bo), (unsigned)gbm_bo_get_stride(bo), probe);
                gbm_bo_destroy(gdev, bo);
            } else {
                printf("[DRMDEMO] gbm bo FAILED (dumb 槽位不够或格式不支持)\n");
            }
            struct gbm_surface* gs = gbm_surface_create(gdev, 32u, 32u, GBM_FORMAT_XRGB8888,
                                                        GBM_BO_USE_SCANOUT);
            if (gs) {
                const unsigned h1 = (unsigned)gbm_bo_get_handle(gbm_surface_lock_front_buffer(gs));
                const unsigned h2 = (unsigned)gbm_bo_get_handle(gbm_surface_lock_front_buffer(gs));
                printf("[DRMDEMO] gbm surface n=%d front=%u/%u stride=%u rc=0\n",
                       gs->n, h1, h2, (unsigned)gbm_bo_get_stride(gs->bo[0]));
                gbm_surface_destroy(gs);
            } else {
                printf("[DRMDEMO] gbm surface FAILED\n");
            }
            gbm_device_destroy(gdev);
        }
    }

    /* ---- ⑰ ★ v0.9.23：运行时真切分辨率（SETCRTC -> Bochs DISPI）----
     *   为什么要这一步：v0.9.22 之前 SETCRTC 只接受"当前几何"，换了就 -EINVAL，
     *   也就是"用户态根本没法改模式"。这里真切一次、再切回来：
     *   证据 = 内核 [DRM] setcrtc mode-switch … ok=1 path=dispi（换成功）
     *        + 换回原模式也 ok=1 + 换回后 ring3 还能上屏（下面的 present 行）。
     *   切不过去就如实 ok=0（内核会给 path=not-in-edid / path=unsupported 的原因，
     *   比如 VMware SVGA / virtio-gpu / GOP 没有改模式的命令通道）。 ---- */
    {
        static struct Drm64ModeMode ms_modes[16];
        struct Drm64ModeConnector q;
        for (unsigned i = 0; i < sizeof(q); i++) ((char*)&q)[i] = 0;
        q.connector_id = DRM64_CONNECTOR_ID64;
        q.modes_ptr = (uint64_t)(uintptr_t)ms_modes;
        q.count_modes = 16;
        q.count_props = 0; q.count_encoders = 0;
        const int qrc = (int)drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_GETCONNECTOR,
                                                        struct Drm64ModeConnector), &q);
        unsigned tw = 0, th = 0, n_il = 0, iflags = 0;
        if (qrc == 0) {
            const unsigned nm = (q.count_modes > 16u) ? 16u : (unsigned)q.count_modes;
            for (unsigned i = 1; i < nm; i++) {
                const struct Drm64ModeMode* m = &ms_modes[i];
                if (!m->hdisplay || !m->vdisplay) continue;
                if (m->flags & 0x10u) {                                /* 隔行：DRM_MODE_FLAG_INTERLACE */
                    n_il++;
                    if (!iflags) iflags = m->flags;
                    continue;                                          /* 隔行模式 DISPI 切不过去 */
                }
                if (m->hdisplay > dw || m->vdisplay > dh) continue;     /* 比演示 dumb 大：跳过 */
                if (m->hdisplay < 640u || m->vdisplay < 480u) continue;
                if (m->hdisplay == mode.hdisplay && m->vdisplay == mode.vdisplay) continue;
                if (m->hdisplay > tw) { tw = m->hdisplay; th = m->vdisplay; }  /* 取最大的那个 */
            }
        }
        printf("[DRMDEMO] modeswitch list=%u ilace=%u iflags=0x%x pick=%ux%u cur=%ux%u\n",
               (unsigned)q.count_modes, n_il, iflags, tw, th,
               (unsigned)mode.hdisplay, (unsigned)mode.vdisplay);
        if (qrc != 0 || tw == 0u) {
            printf("[DRMDEMO] modeswitch skipped (no usable EDID mode smaller than dumb)\n");
        } else {
            struct Drm64ModeCrtc sc;
            for (unsigned i = 0; i < sizeof(sc); i++) ((char*)&sc)[i] = 0;
            sc.crtc_id = DRM64_CRTC_ID64; sc.fb_id = cd.handle;
            sc.x = 0; sc.y = 0; sc.mode_valid = 1;
            sc.mode = mode;                                            /* 先拷当前模式结构 */
            sc.mode.hdisplay = (uint16_t)tw; sc.mode.vdisplay = (uint16_t)th;
            const int r1 = (int)drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_SETCRTC,
                                                           struct Drm64ModeCrtc), &sc);
            printf("[DRMDEMO] setcrtc mode=%ux%u rc=%d\n", tw, th, r1);
            /* 换回原模式：绝不把"屏幕被演示程序改小"这种副作用留在系统里 */
            sc.mode = mode;
            const int r2 = (int)drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_SETCRTC,
                                                           struct Drm64ModeCrtc), &sc);
            printf("[DRMDEMO] setcrtc restore=%ux%u rc=%d\n",
                   (unsigned)mode.hdisplay, (unsigned)mode.vdisplay, r2);
            /* 换回来之后再画一帧：证明往返之后 ring3 仍然能上屏 */
            struct Drm64ModePageFlip pf3;
            pf3.crtc_id = DRM64_CRTC_ID64; pf3.fb_id = cd.handle;
            pf3.flags = 0; pf3.reserved = 0; pf3.user_data = 0x5678u;
            const int r3 = (int)drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP,
                                                           struct Drm64ModePageFlip), &pf3);
            printf("[DRMDEMO] modeswitch reflip rc=%d\n", r3);
            /* 反例：EDID 模式表里**没有**的分辨率必须被拒绝（内核 path=not-in-edid，不假装切成功） */
            sc.mode = mode;
            sc.mode.hdisplay = 1111; sc.mode.vdisplay = 777;
            const int r4 = (int)drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_SETCRTC,
                                                           struct Drm64ModeCrtc), &sc);
            printf("[DRMDEMO] setcrtc badmode=1111x777 rc=%d\n", r4);
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
