/* drm64.h - ★ v0.9.12：DRM/KMS + evdev 的用户侧 ABI（Vimtu64 用户态 C 运行时）
 *
 * ★★ 这个文件是 kernel/drm64.h 的**逐值拷贝**。改这里必须同步 kernel/drm64.h，
 *    tests/drm64_test.py 会逐值比对两份头文件（#define 的数值 + 每个结构体的字段序列），
 *    不一致就直接判失败 —— 别指望两边"差不多"。
 *
 * 与内核侧完全一致的部分（照抄）：
 *   * ioctl 号：DRM64_IOWR(nr, type) = dir(W|R)<<30 | sizeof(type)<<16 | 'd'<<8 | nr
 *     v0.9.14 起 11 个号的数值与 Linux `include/uapi/drm/drm.h` **逐个相等**，
 *     结构体 sizeof 也与 Linux 逐字节相同（version 64 / modeinfo 68 / crtc 104 /
 *     wait_vblank 24 / crtc_get_sequence 24）—— 下一批接真实 libdrm 不用再改 ABI。
 *   * 结构体：全部 packed（Drm64Version 除外，它靠自然对齐拿到 Linux 的 4 B padding），
 *     字段顺序固定，每个都有一条 sizeof 断言
 *   * 上限：4 个 dumb buffer / 4 个 connector / 1 个 CRTC / 2 个 plane（越界 -EINVAL）
 *   * evdev：struct input_event64 = 24 B（与 Linux x86_64 的 struct input_event 同尺寸）
 *
 * 用法（user/lib/drm64.c 里有薄封装 drm64_open/drm64_ioctl/evdev64_open/evdev64_read）：
 *   int fd = drm64_open();                                        // open("/dev/dri/card0")
 *   struct Drm64ModeCreateDumb cd = { 0 };                        // height/width/bpp 由调用方填
 *   drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, struct Drm64ModeCreateDumb), &cd);
 *   struct Drm64ModeMapDumb md = { 0 };
 *   md.handle = cd.handle;
 *   drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, struct Drm64ModeMapDumb), &md);
 *                                                 // -> md.offset = **用户 VA**（CPU 可写）
 *   ...在 (uint32_t*)md.offset 里画图...
 *   struct Drm64ModePageFlip pf = { 1, cd.handle, 0, 0, 0 };
 *   drm64_ioctl(fd, DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, struct Drm64ModePageFlip), &pf);
 *                                                 // -> 上屏 + 等一次 vblank
 * 说明（如实）：本内核的 MAP_DUMB 返回的是"这块缓冲在当前进程里的用户 VA"
 *   （Linux 那边返回的是要给 mmap 的文件偏移）—— 我们没有文件 mmap，语义差异写死在这里。
 */
#ifndef VIMTU64_DRM64_H
#define VIMTU64_DRM64_H

#include <stdint.h>

#if defined(__cplusplus)
#define DRM64_STATIC_ASSERT(c, m) static_assert(c, m)
#else
#define DRM64_STATIC_ASSERT(c, m) _Static_assert(c, m)
#endif

/* ==================== ioctl 编码（Linux DRM 风格，size 用我们的 struct）==================== */
#define DRM64_IOC_NRBITS64   8
#define DRM64_IOC_TYPEBITS64 8
#define DRM64_IOC_SIZEBITS64 14
#define DRM64_IOC_DIRBITS64  2
#define DRM64_IOC_NONE64     0u
#define DRM64_IOC_WRITE64    1u
#define DRM64_IOC_READ64     2u
#define DRM64_IOC_BASE64     ((uint32_t)'d')      /* 0x64：与 Linux DRM 同一个基字符 */
/* dir(W|R) << 30 | size << 16 | 'd' << 8 | nr     —— 与 Linux _IOWR 的位序完全一致 */
#define DRM64_IOWR(nr, type) \
    ((uint32_t)((DRM64_IOC_READ64 | DRM64_IOC_WRITE64) << 30) | \
     ((uint32_t)sizeof(type) << 16) | (DRM64_IOC_BASE64 << 8) | ((uint32_t)(nr) & 0xFFu))

/* ---- 支持的 nr（Linux DRM 的编号风格；只列本批实现了的）---- */
#define DRM64_NR_VERSION           0x00u
#define DRM64_NR_GET_CAP           0x0cu
#define DRM64_NR_WAIT_VBLANK       0x3au
#define DRM64_NR_CRTC_GET_SEQUENCE 0x3bu
#define DRM64_NR_MODE_GETRESOURCES 0xA0u
#define DRM64_NR_MODE_GETCRTC      0xA1u
#define DRM64_NR_MODE_SETCRTC      0xA2u
#define DRM64_NR_MODE_GETENCODER   0xA6u
#define DRM64_NR_MODE_GETCONNECTOR 0xA7u
#define DRM64_NR_MODE_GETPROPERTY  0xAAu
#define DRM64_NR_MODE_SETPROPERTY  0xABu
#define DRM64_NR_MODE_GETPROPBLOB  0xACu
#define DRM64_NR_MODE_ADDFB        0xADu
#define DRM64_NR_MODE_ADDFB2       0xAEu
#define DRM64_NR_MODE_OBJ_GETPROPS 0xB9u
#define DRM64_NR_MODE_OBJ_SETPROP  0xBAu
#define DRM64_NR_MODE_ATOMIC       0xBCu
#define DRM64_NR_MODE_CREATEBLOB   0xBDu
#define DRM64_NR_MODE_DESTROYBLOB  0xBEu
#define DRM64_NR_MODE_PAGE_FLIP    0xB0u
#define DRM64_NR_MODE_DIRTYFB      0xB1u
#define DRM64_NR_MODE_GETPLANE     0xB5u
#define DRM64_NR_MODE_GETPLANERES  0xB6u
#define DRM64_NR_MODE_CREATE_DUMB  0xB2u
#define DRM64_NR_MODE_MAP_DUMB     0xB3u
#define DRM64_NR_MODE_DESTROY_DUMB 0xB4u

/* ==================== 上限（如实小于等于；越界一律 -EINVAL）==================== */
#define DRM64_MAX_DUMB64       4u        /* 同时存在的 dumb buffer 数（= 用户映射窗槽数） */
#define DRM64_MAX_CONNECTOR64  4u        /* connector 数（实际只有 1 个：HDMI-A-1） */
#define DRM64_MAX_CRTC64       1u        /* CRTC 数（本内核只有一块屏） */
#define DRM64_MAX_PLANE64      2u        /* plane 数（primary + cursor，本批只实现 primary） */
#define DRM64_DUMB_MAX_BYTES64 (8ull * 1024ull * 1024ull)   /* 单块上限 8 MiB（1080p@32bpp 够用） */
#define DRM64_DUMB_MAX_PAGES64 2048u                        /* = DRM64_DUMB_MAX_BYTES64 / 4KiB */
#define DRM64_DUMB_MAX_STRIDE64 8192u                       /* 单行字节上限（w <= 2048 @32bpp） */
#define DRM64_DUMB_MAX_HEIGHT64 4096u
#define DRM64_DEV_NAME64       "vimtu-drm"
#define DRM64_DEV_DATE64       "20250101"
#define DRM64_DEV_DESC64       "Vimtu64 DRM/KMS minimal facade (dumb buffers + page flip + vblank)"
#define DRM64_CONNECTOR_NAME64 "HDMI-A-1"
/* CRTC / connector / encoder 的 id 从 1 开始（0 = 没有）；fb id 与 dumb handle 同一套号。 */
#define DRM64_CRTC_ID64        1u
#define DRM64_CONNECTOR_ID64   1u
#define DRM64_ENCODER_ID64     1u
/* v0.9.15：fb 对象（ADDFB/ADDFB2）与 plane。fb id 与 dumb handle 不是同一套号（Linux 也是）。 */
#define DRM64_MAX_FB64         8u
#define DRM64_FB_ID_BASE64     0x100u
#define DRM64_PLANE_PRIMARY_ID64 1u          /* primary plane（真能上屏） */
#define DRM64_PLANE_CURSOR_ID64  2u          /* cursor plane（只报告，不改） */
/* 只支持这两个 fourcc（= Linux DRM_FORMAT_XRGB8888 / DRM_FORMAT_ARGB8888） */
#define DRM64_FORMAT_XRGB8888  0x34325258u
#define DRM64_FORMAT_ARGB8888  0x34325241u

/* ==== ★ v0.9.16：属性系统（atomic commit 的地基）==== */
#define DRM64_OBJ_CRTC      0xccccccccu
#define DRM64_OBJ_CONNECTOR 0xc0c0c0c0u
#define DRM64_OBJ_ENCODER   0xe0e0e0e0u
#define DRM64_OBJ_PLANE     0xeeeeeeeeu
#define DRM64_OBJ_FB        0xfbfbfbfbu
#define DRM64_PROP_ACTIVE     1u
#define DRM64_PROP_MODE_ID    2u
#define DRM64_PROP_TYPE       3u
#define DRM64_PROP_FB_ID      4u
#define DRM64_PROP_CRTC_ID    5u
#define DRM64_PROP_SRC_X      6u
#define DRM64_PROP_SRC_Y      7u
#define DRM64_PROP_SRC_W      8u
#define DRM64_PROP_SRC_H      9u
#define DRM64_PROP_CRTC_X     10u
#define DRM64_PROP_CRTC_Y     11u
#define DRM64_PROP_CRTC_W     12u
#define DRM64_PROP_CRTC_H     13u
#define DRM64_PROP_IN_FORMATS 14u
#define DRM64_PROP_COUNT64    14u
#define DRM64_PROP_RANGE64     0x02u
#define DRM64_PROP_ENUM64      0x08u
#define DRM64_PROP_OBJECT64    0x10u
#define DRM64_PROP_IMMUTABLE64 0x20u
#define DRM64_PROP_BLOB64      0x40u
#define DRM64_BLOB_NONE        0u
#define DRM64_BLOB_MODE        0x200u
#define DRM64_BLOB_FMT_PRIMARY 0x201u
#define DRM64_BLOB_FMT_CURSOR  0x202u
#define DRM64_BLOB_USER_BASE64 0x300u
#define DRM64_MAX_USERBLOB64   4u
#define DRM64_USERBLOB_BYTES64 128u
#define DRM64_IN_FORMATS_VERSION64 1u
#define DRM64_PLANE_TYPE_OVERLAY 0u
#define DRM64_PLANE_TYPE_PRIMARY 1u
#define DRM64_PLANE_TYPE_CURSOR  2u
#define DRM64_ENCODER_TYPE_TMDS64 2u
#define DRM64_ATOMIC_TEST_ONLY64     0x0100u
#define DRM64_ATOMIC_NONBLOCK64      0x0200u
#define DRM64_ATOMIC_ALLOW_MODESET64 0x0400u
#define DRM64_ATOMIC_FLAGS_OK64 \
    (DRM64_ATOMIC_TEST_ONLY64 | DRM64_ATOMIC_NONBLOCK64 | DRM64_ATOMIC_ALLOW_MODESET64)
#define DRM64_ATOMIC_MAX_OBJS64   8u
#define DRM64_ATOMIC_MAX_PROPS64  32u
/* 用户映射窗（见内核侧头文件）：A1 的 FB 区后半段，4 槽 × 8 MiB */
#define DRM64_UVA_BASE_OFFSET64 (8ull * 1024ull * 1024ull)  /* USER64_FB_VA64 + 8 MiB */
#define DRM64_UVA_SLOT_BYTES64  (8ull * 1024ull * 1024ull)

/* ==================== 错误码（Linux errno 的正值；函数返回它的相反数）==================== */
#define DRM64_EPERM64   1
#define DRM64_EIO64     5
#define DRM64_ENOMEM64  12
#define DRM64_EFAULT64  14
#define DRM64_EBUSY64   16
#define DRM64_ENODEV64  19
#define DRM64_EINVAL64  22
#define DRM64_ENOTTY64  25
#define DRM64_ENOSYS64  38

/* ==================== 结构体（全部 packed；字段顺序 = ABI，别动）==================== */
/* ① VERSION（= Linux struct drm_version，64 B）：不 packed，+12..15 那 4 B padding 是 Linux 的布局。 */
struct Drm64Version {
    int32_t  version_major;      /* +0   = 1 */
    int32_t  version_minor;      /* +4   = 0 */
    int32_t  version_patchlevel; /* +8   = 0 */
                                 /* +12  padding（自然对齐，勿加字段） */
    uint64_t name_len;           /* +16  in/out：name 缓冲字节数（写入含 NUL 的实际长度） */
    uint64_t name;               /* +24  用户缓冲指针（"vimtu-drm"） */
    uint64_t date_len;           /* +32  in/out：date 缓冲字节数 */
    uint64_t date;               /* +40  用户缓冲指针（"20250101"） */
    uint64_t desc_len;           /* +48  in/out：desc 缓冲字节数 */
    uint64_t desc;               /* +56  用户缓冲指针（一行说明） */
};                               /* = 64 B（= Linux drm_version） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64Version) == 64, "Drm64Version 必须是 64 B（= Linux drm_version）");

/* ② GET_CAP：只支持 DRM_CAP_DUMB_BUFFER(1) 与 DRM_CAP_VBLANK(2)（值 1 = 支持）。 */
struct __attribute__((packed)) Drm64GetCap {
    uint64_t capability;         /* +0  in：1 = DUMB_BUFFER / 2 = VBLANK */
    uint64_t value;              /* +8  out：1 = 支持 / 0 = 不支持 */
};                               /* = 16 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64GetCap) == 16, "Drm64GetCap 必须是 16 B");

/* ③ GETRESOURCES：两个方向用同一个结构（count_* 是 in/out；ptr 是用户数组指针）。 */
struct __attribute__((packed)) Drm64ModeResources {
    uint64_t fb_id_ptr;          /* +0   用户 u32 数组（fb id） */
    uint64_t crtc_id_ptr;        /* +8   用户 u32 数组（crtc id） */
    uint64_t connector_id_ptr;   /* +16  用户 u32 数组（connector id） */
    uint64_t encoder_id_ptr;     /* +24  用户 u32 数组（encoder id） */
    uint32_t count_fbs;          /* +32  in/out */
    uint32_t count_crtcs;        /* +36  in/out */
    uint32_t count_connectors;   /* +40  in/out */
    uint32_t count_encoders;     /* +44  in/out */
    uint32_t min_width;          /* +48  out */
    uint32_t max_width;          /* +52  out */
    uint32_t min_height;         /* +56  out */
    uint32_t max_height;         /* +60  out */
};                               /* = 64 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeResources) == 64, "Drm64ModeResources 必须是 64 B");

/* ③-b 显示模式（= Linux struct drm_mode_modeinfo，68 B —— name[32] 在最后） */
struct __attribute__((packed)) Drm64ModeMode {
    uint32_t clock;              /* +0   kHz（按 60 Hz 估计，如实标注为估计值） */
    uint16_t hdisplay;           /* +4 */
    uint16_t hsync_start;        /* +6 */
    uint16_t hsync_end;          /* +8 */
    uint16_t htotal;             /* +10 */
    uint16_t hskew;              /* +12  恒 0 */
    uint16_t vdisplay;           /* +14 */
    uint16_t vsync_start;        /* +16 */
    uint16_t vsync_end;          /* +18 */
    uint16_t vtotal;             /* +20 */
    uint16_t vscan;              /* +22  恒 0 */
    uint32_t vrefresh;           /* +24  Hz（估计值） */
    uint32_t flags;              /* +28  0 */
    uint32_t type;               /* +32  0 = 非首选之外的普通模式（本批恒 0） */
    char     name[32];           /* +36  "<w>x<h>"（NUL 结尾） */
};                               /* = 68 B（= Linux drm_mode_modeinfo） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeMode) == 68, "Drm64ModeMode 必须是 68 B（= Linux drm_mode_modeinfo）");

/* ④ GETCRTC（= Linux struct drm_mode_crtc，104 B）：读当前 CRTC 状态（1 个 CRTC）。 */
struct __attribute__((packed)) Drm64ModeCrtc {
    uint64_t       set_connectors_ptr;  /* +0   用户 u32 数组（out） */
    uint32_t       count_connectors;    /* +8   in/out */
    uint32_t       crtc_id;             /* +12  out = 1 */
    uint32_t       fb_id;               /* +16  out = 当前在屏的 dumb handle（0 = 没有） */
    uint32_t       x;                   /* +20  out = 0 */
    uint32_t       y;                   /* +24  out = 0 */
    uint32_t       gamma_size;          /* +28  out = 0（没有 gamma LUT） */
    uint32_t       mode_valid;          /* +32  out = 1 */
    struct Drm64ModeMode  mode;         /* +36  out = 当前模式（68 B） */
};                                      /* = 104 B（= Linux drm_mode_crtc） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeCrtc) == 104, "Drm64ModeCrtc 必须是 104 B（= Linux drm_mode_crtc）");

/* ⑤ GETCONNECTOR：1 个 connector（HDMI-A-1，status=connected）。 */
struct __attribute__((packed)) Drm64ModeConnector {
    uint64_t encoders_ptr;       /* +0   用户 u32 数组 */
    uint64_t modes_ptr;          /* +8   用户 Drm64ModeMode 数组 */
    uint64_t props_ptr;          /* +16  用户 u32 数组（本批恒 0 个属性） */
    uint64_t prop_values_ptr;    /* +24  用户 u64 数组（本批恒 0 个） */
    uint32_t count_modes;        /* +32  in/out */
    uint32_t count_props;        /* +36  in/out（恒 0） */
    uint32_t count_encoders;     /* +40  in/out */
    uint32_t encoder_id;         /* +44  out = 1 */
    uint32_t connector_id;       /* +48  out = 1 */
    uint32_t connector_type;     /* +52  out = 11（DRM_MODE_CONNECTOR_HDMIA） */
    uint32_t connector_type_id;  /* +56  out = 1 */
    uint32_t connection;         /* +60  out = 1（connected） */
    uint32_t mm_width;           /* +64  out = 0（没有 EDID 物理尺寸时如实 0） */
    uint32_t mm_height;          /* +68  out = 0 */
    uint32_t subpixel;           /* +72  out = 0（unknown） */
    uint32_t pad;                /* +76 */
};                               /* = 80 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeConnector) == 80, "Drm64ModeConnector 必须是 80 B");

/* ⑥ CREATE_DUMB（bpp 只支持 32；pitch = width*4；handle = 1..4） */
struct __attribute__((packed)) Drm64ModeCreateDumb {
    uint32_t height;             /* +0   in */
    uint32_t width;              /* +4   in */
    uint32_t bpp;                /* +8   in（必须是 32） */
    uint32_t flags;              /* +12  必须 0 */
    uint32_t handle;             /* +16  out（1..DRM64_MAX_DUMB64） */
    uint32_t pitch;              /* +20  out（= width*4） */
    uint64_t size;               /* +24  out（= pitch*height） */
};                               /* = 32 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeCreateDumb) == 32, "Drm64ModeCreateDumb 必须是 32 B");

/* ⑦ MAP_DUMB：**offset 是本内核的用户 VA**（语义差异，见文件头；Linux 那边是文件偏移）。 */
struct __attribute__((packed)) Drm64ModeMapDumb {
    uint32_t handle;   /* +0  in */
    uint32_t pad;      /* +4 */
    uint64_t offset;   /* +8  out = 这块缓冲在当前进程里的用户 VA（0 = 没有映射） */
};                     /* = 16 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeMapDumb) == 16, "Drm64ModeMapDumb 必须是 16 B");

/* ⑧ DESTROY_DUMB */
struct __attribute__((packed)) Drm64ModeDestroyDumb {
    uint32_t handle;   /* +0  in */
};                     /* = 4 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeDestroyDumb) == 4, "Drm64ModeDestroyDumb 必须是 4 B");

/* ⑨ PAGE_FLIP：fb_id 就是 dumb handle；同步翻转（返回 0 = 已上屏并等过一次 vblank） */
struct __attribute__((packed)) Drm64ModePageFlip {
    uint32_t crtc_id;    /* +0   in（必须是 1） */
    uint32_t fb_id;      /* +4   in（dumb handle） */
    uint32_t flags;      /* +8   必须 0（没有异步翻页 / 事件） */
    uint32_t reserved;   /* +12 */
    uint64_t user_data;  /* +16  原样带回（本批不回任何事件，仅日志） */
};                       /* = 24 B */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModePageFlip) == 24, "Drm64ModePageFlip 必须是 24 B");

/* ⑩ WAIT_VBLANK（= Linux union drm_wait_vblank 的 reply 布局，24 B）
 *   in  : type 的 bit0 = 1（DRM_VBLANK_RELATIVE）= 等下一个 vblank；0 = 只读序列号
 *   out : type 原样回显、sequence = 当前序列号、tval_sec/tval_usec = tick 换算的时间
 *   注意：没有 crtc_id 字段（Linux 用 type 的高位带 CRTC 选择；本内核只有 1 个 CRTC，不校验）。 */
struct __attribute__((packed)) Drm64WaitVblank {
    uint32_t type;        /* +0   in：1 = 等下一个 vblank / 0 = 只读；out：回显 */
    uint32_t sequence;    /* +4   in：相对偏移（本批只用 0）；out：实际序列号 */
    int64_t  tval_sec;    /* +8   out：tick 换算的秒 */
    int64_t  tval_usec;   /* +16  out：微秒（4 ms tick 粒度，如实） */
};                        /* = 24 B（= Linux drm_wait_vblank_reply） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64WaitVblank) == 24, "Drm64WaitVblank 必须是 24 B（= Linux union drm_wait_vblank）");

/* ⑪ CRTC_GET_SEQUENCE（= Linux struct drm_crtc_get_sequence，24 B） */
struct __attribute__((packed)) Drm64CrtcSeq {
    uint32_t crtc_id;      /* +0   in */
    uint32_t active;       /* +4   out：1 = vblank 由硬件扫描驱动 / 0 = 软兜底 */
    uint64_t sequence;     /* +8   out：当前序列号 */
    uint64_t sequence_ns;  /* +16  out：sequence 对应的纳秒（tick 粒度，如实） */
};                         /* = 24 B（= Linux drm_crtc_get_sequence） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64CrtcSeq) == 24, "Drm64CrtcSeq 必须是 24 B（= Linux drm_crtc_get_sequence）");

/* ⑫ ADDFB2（= Linux struct drm_mode_fb_cmd2，104 B）：modifier[4] 要对齐到 8，不能 packed */
struct Drm64ModeFbCmd2 {
    uint32_t fb_id;              /* +0   out = 0x100..0x107 */
    uint32_t width;              /* +4   in */
    uint32_t height;             /* +8   in */
    uint32_t pixel_format;       /* +12  in：只认 XRGB8888 / ARGB8888 */
    uint32_t flags;              /* +16  必须 0 */
    uint32_t handles[4];         /* +20  in：handles[0] = dumb handle（其余必须 0） */
    uint32_t pitches[4];         /* +36  in：pitches[0] 必须等于该 dumb 的 stride */
    uint32_t offsets[4];         /* +52  in：offsets[0] 必须 0 */
    uint64_t modifier[4];        /* +72  in：必须全 0（没有 tiling/modifier） */
};                               /* = 104 B（= Linux drm_mode_fb_cmd2） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeFbCmd2) == 104, "Drm64ModeFbCmd2 必须是 104 B（= Linux drm_mode_fb_cmd2）");

/* ⑬ ADDFB（legacy；= Linux struct drm_mode_fb_cmd，28 B） */
struct __attribute__((packed)) Drm64ModeFbCmd {
    uint32_t fb_id;              /* +0   out */
    uint32_t width;              /* +4   in */
    uint32_t height;             /* +8   in */
    uint32_t pitch;              /* +12  in（必须等于 dumb 的 stride） */
    uint32_t bpp;                /* +16  in（必须是 32） */
    uint32_t depth;              /* +20  in（24 或 32） */
    uint32_t handle;             /* +24  in（dumb handle） */
};                               /* = 28 B（= Linux drm_mode_fb_cmd） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeFbCmd) == 28, "Drm64ModeFbCmd 必须是 28 B（= Linux drm_mode_fb_cmd）");

/* ⑭ GETPLANERESOURCES（= Linux struct drm_mode_get_plane_res，16 B）：u64 + u32，不能 packed */
struct Drm64ModeGetPlaneRes {
    uint64_t plane_id_ptr;       /* +0   用户 u32 数组 */
    uint32_t count_planes;       /* +8   in/out（本内核恒 2） */
};                               /* = 16 B（= Linux drm_mode_get_plane_res） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeGetPlaneRes) == 16, "Drm64ModeGetPlaneRes 必须是 16 B（= Linux drm_mode_get_plane_res）");

/* ⑮ GETPLANE（= Linux struct drm_mode_get_plane，32 B） */
struct Drm64ModeGetPlane {
    uint32_t plane_id;           /* +0   in（1 = primary / 2 = cursor） */
    uint32_t crtc_id;            /* +4   out */
    uint32_t fb_id;              /* +8   out = 该 plane 正在扫出的 fb（0 = 没有） */
    uint32_t possible_crtcs;     /* +12  out = 位掩码（本内核恒 1） */
    uint32_t gamma_size;         /* +16  out = 0（没有 gamma LUT） */
    uint32_t count_format_types; /* +20  in/out（本内核恒 2） */
    uint64_t format_type_ptr;    /* +24  用户 u32 数组（fourcc） */
};                               /* = 32 B（= Linux drm_mode_get_plane） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeGetPlane) == 32, "Drm64ModeGetPlane 必须是 32 B（= Linux drm_mode_get_plane）");

/* ⑯ GETENCODER（= Linux struct drm_mode_get_encoder，20 B） */
struct Drm64ModeGetEncoder {
    uint32_t encoder_id;       /* +0   in */
    uint32_t encoder_type;     /* +4   out（2 = TMDS） */
    uint32_t crtc_id;          /* +8   out */
    uint32_t possible_crtcs;   /* +12  out（位掩码，恒 1） */
    uint32_t possible_clones;  /* +16  out（恒 0） */
};                             /* = 20 B（= Linux drm_mode_get_encoder） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeGetEncoder) == 20, "Drm64ModeGetEncoder 必须是 20 B（= Linux drm_mode_get_encoder）");

/* ⑰ GETPROPERTY（= Linux struct drm_mode_get_property，64 B） */
struct Drm64ModeGetProperty {
    uint64_t values_ptr;       /* +0   用户 u64 数组（枚举值 / 位掩码） */
    uint64_t enum_blob_ptr;    /* +8   用户 u32 数组（枚举 blob id） */
    uint32_t prop_id;          /* +16  in */
    uint32_t flags;            /* +20  out（RANGE/OBJECT/IMMUTABLE/BLOB…） */
    char     name[32];         /* +24  out（NUL 结尾） */
    uint32_t count_values;     /* +56  out */
    uint32_t count_enum_blobs; /* +60  out（本内核恒 0） */
};                             /* = 64 B（= Linux drm_mode_get_property） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeGetProperty) == 64, "Drm64ModeGetProperty 必须是 64 B（= Linux drm_mode_get_property）");

/* ⑱ GETPROPBLOB（= Linux struct drm_mode_get_blob，16 B） */
struct Drm64ModeGetBlob {
    uint32_t blob_id;          /* +0   in */
    uint32_t length;           /* +4   in/out */
    uint64_t data;             /* +8   用户缓冲 */
};                             /* = 16 B（= Linux drm_mode_get_blob） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeGetBlob) == 16, "Drm64ModeGetBlob 必须是 16 B（= Linux drm_mode_get_blob）");

/* ⑲ OBJ_GETPROPERTIES（= Linux struct drm_mode_obj_get_properties，32 B） */
struct Drm64ModeObjGetProps {
    uint64_t props_ptr;        /* +0   用户 u32 数组（属性 id） */
    uint64_t prop_values_ptr;  /* +8   用户 u64 数组（属性值） */
    uint32_t count_props;      /* +16  in/out */
    uint32_t obj_id;           /* +20  in */
    uint32_t obj_type;         /* +24  in（DRM64_OBJ_*） */
};                             /* = 32 B（= Linux drm_mode_obj_get_properties） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeObjGetProps) == 32, "Drm64ModeObjGetProps 必须是 32 B（= Linux drm_mode_obj_get_properties）");

/* ⑳ OBJ_SETPROPERTY（= Linux struct drm_mode_obj_set_property，24 B） */
struct Drm64ModeObjSetProp {
    uint64_t value;            /* +0   in */
    uint32_t prop_id;          /* +8   in */
    uint32_t obj_id;           /* +12  in */
    uint32_t obj_type;         /* +16  in */
};                             /* = 24 B（= Linux drm_mode_obj_set_property） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeObjSetProp) == 24, "Drm64ModeObjSetProp 必须是 24 B（= Linux drm_mode_obj_set_property）");

/* ㉑ ATOMIC（= Linux struct drm_mode_atomic，56 B） */
struct Drm64ModeAtomic {
    uint32_t flags;               /* +0   in */
    uint32_t count_objs;          /* +4   in */
    uint64_t objs_ptr;            /* +8   用户 u32 数组（对象 id） */
    uint64_t count_props_ptr;     /* +16  用户 u32 数组（每个对象几个属性） */
    uint64_t props_ptr;           /* +24  用户 u32 数组（属性 id，按对象顺序拼在一起） */
    uint64_t prop_values_ptr;     /* +32  用户 u64 数组（值，与 props 一一对应） */
    uint64_t reserved;            /* +40  必须 0 */
    uint64_t user_data;           /* +48  原样带回（本内核不投递事件，仅日志） */
};                                /* = 56 B（= Linux drm_mode_atomic） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeAtomic) == 56, "Drm64ModeAtomic 必须是 56 B（= Linux drm_mode_atomic）");

/* ㉒ CREATEBLOB（= Linux struct drm_mode_create_blob，16 B） */
struct Drm64ModeCreateBlob {
    uint64_t data;             /* +0   用户缓冲 */
    uint32_t length;           /* +8   in（<= 128） */
    uint32_t blob_id;          /* +12  out（0x300..0x303） */
};                             /* = 16 B（= Linux drm_mode_create_blob） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeCreateBlob) == 16, "Drm64ModeCreateBlob 必须是 16 B（= Linux drm_mode_create_blob）");

/* ㉓ DESTROYBLOB（= Linux struct drm_mode_destroy_blob，4 B） */
struct Drm64ModeDestroyBlob {
    uint32_t blob_id;          /* +0   in */
};                             /* = 4 B（= Linux drm_mode_destroy_blob） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeDestroyBlob) == 4, "Drm64ModeDestroyBlob 必须是 4 B（= Linux drm_mode_destroy_blob）");

/* ㉔ DIRTYFB（= Linux struct drm_mode_fb_dirty_cmd，24 B）：告诉内核哪些矩形变了，
 *   之后的 PAGE_FLIP 只传这部分（2D 加速的真实来源：搬运量变小）。 */
struct Drm64ModeFbDirty {
    uint32_t fb_id;              /* +0   in（dumb handle 或 fb id） */
    uint32_t flags;              /* +4   必须 0 */
    uint32_t color;              /* +8   fbdev 仿真用；本内核忽略 */
    uint32_t num_clips;          /* +12  in */
    uint64_t clips_ptr;          /* +16  用户 u16[4] 数组 */
};                               /* = 24 B（= Linux drm_mode_fb_dirty_cmd） */
DRM64_STATIC_ASSERT(sizeof(struct Drm64ModeFbDirty) == 24, "Drm64ModeFbDirty 必须是 24 B（= Linux drm_mode_fb_dirty_cmd）");
struct Drm64ClipRect {
    uint16_t x1, y1, x2, y2;     /* = Linux struct drm_clip_rect（8 B） */
};
DRM64_STATIC_ASSERT(sizeof(struct Drm64ClipRect) == 8, "Drm64ClipRect 必须是 8 B（= Linux drm_clip_rect）");
#define DRM64_MAX_CLIPS64 8u

// ==================== ★ v0.9.20：PRIME（跨进程共享同一批物理页）====================
// Linux ABI：DRM_IOCTL_PRIME_HANDLE_TO_FD(0x2D) / DRM_IOCTL_PRIME_FD_TO_HANDLE(0x2E)，
// 结构体 struct drm_prime_handle { __u32 handle; __u32 flags; __s32 fd; } = 12 B。
// **语义差异（如实写在这里，别在别处猜）**：Linux 用 UDS + SCM_RIGHTS 把真 fd 传给另一个进程；
// 本内核没有"传 fd"这条路，所以：
//   * HANDLE_TO_FD 返回的 fd 字段是一个**全局 prime id**（0x400 + 槽号），不是文件描述符；
//   * 任何进程都能拿这个 id 调 FD_TO_HANDLE 换回 handle（本内核的 dumb handle 本来就是全局的
//     1..4，所以换回来的号跟导出方相同 —— Linux 那边是每个 fd 一套号，这是第二处差异）；
//   * 真正的价值是**页共享 + 引用计数**：两个进程 MAP_DUMB 同一个 handle 拿到的是同一批物理页，
//     DESTROY_DUMB 只减引用，引用归零才把页还回页池（[DRM] map 行尾的 pid= 是宿主侧的证据）。
#define DRM64_NR_PRIME_HANDLE_TO_FD 0x2Du
#define DRM64_NR_PRIME_FD_TO_HANDLE 0x2Eu
#define DRM64_MAX_PRIME64     4u
#define DRM64_PRIME_ID_BASE64 0x400u
struct __attribute__((packed)) Drm64PrimeHandle {
    uint32_t handle;   // +0  in：dumb handle（导出方填）
    uint32_t flags;    // +4  in：必须 0（不支持 DRM_CLOEXEC）
    int32_t  fd;       // +8  out：全局 prime id（**不是 fd**，见上）
};                     // = 12 B（= Linux struct drm_prime_handle）
DRM64_STATIC_ASSERT(sizeof(struct Drm64PrimeHandle) == 12, "Drm64PrimeHandle 必须是 12 B（= Linux drm_prime_handle）");

// ==================== ★ v0.9.20：事件投递（vblank 事件 + read/poll）====================
// Linux：struct drm_event { u32 type; u32 length; } 后跟 struct drm_event_vblank 的其余字段，
// 合计 32 B，type = DRM_EVENT_VBLANK(0x02)。PAGE_FLIP 带 DRM_MODE_PAGE_FLIP_EVENT(0x1) 时
// 提交后**立即返回**，事件由 PIT tick（4 ms 粒度）入队，read() 从 /dev/dri/card0 取出来。
// 如实边界：本内核没有独立的 vblank 中断源，所以"入队时刻"是 tick 而不是硬件回扫边沿；
// 同一时刻只允许一个未入队的 flip（第二个返回 -EBUSY），队列满则丢最老的一条。
#define DRM64_EVENT_VBLANK      0x02u
#define DRM64_PAGE_FLIP_EVENT64 0x01u
#define DRM64_EVQ_MAX64         8u
#define DRM64_EV_OWNERS64       2u
struct Drm64EventVblank {
    uint32_t type;      // +0   = 0x02（DRM_EVENT_VBLANK）
    uint32_t length;    // +4   = 32
    uint64_t user_data; // +8   原样带回（PAGE_FLIP 的 user_data）
    uint32_t tv_sec;    // +16
    uint32_t tv_usec;   // +20  （4 ms 粒度，如实）
    uint32_t sequence;  // +24  单调递增
    uint32_t crtc_id;   // +28  = 1
};                      // = 32 B（= Linux struct drm_event_vblank）
DRM64_STATIC_ASSERT(sizeof(struct Drm64EventVblank) == 32, "Drm64EventVblank 必须是 32 B（= Linux drm_event_vblank）");

/* ==================== evdev ABI ==================== */
/* 24 B，与 Linux x86_64 的 struct input_event 同尺寸（时间戳 2×u64 + 2×u16 + i32）。 */
struct __attribute__((packed)) InputEvent64 {
    uint64_t sec;     /* +0 */
    uint64_t usec;    /* +8 */
    uint16_t type;    /* +16  EV_* */
    uint16_t code;    /* +18  键码 / REL_* 轴号 / BTN_* */
    int32_t  value;   /* +20 */
};                    /* = 24 B */
DRM64_STATIC_ASSERT(sizeof(struct InputEvent64) == 24, "InputEvent64 必须是 24 B（与 Linux 同尺寸）");

#define EV64_SYN 0u
#define EV64_KEY 1u
#define EV64_REL 2u
#define EV64_ABS 3u
/* 本批的键码口径 = 既有 input64.h 的键码（可打印 ASCII / 控制字符 / NAV_* 扩展），
 * 鼠标键位直接用掩码（0x1 = 左、0x2 = 右、0x4 = 中，与 EV64_BTN_* 一致 —— 不是 Linux 的 BTN_*）。 */
#define EV64_REL_X      0u
#define EV64_REL_Y      1u
#define EV64_REL_WHEEL  8u
#define EVDEV64_MAX_EVENTS64 32u     /* 单次 read 最多返回的事件数（= 进程队列容量） */

/* ioctl（Linux evdev 的编码，size 位是"缓冲区字节数"，见 _IOC 的 size 语义） */
#define EVDEV64_IOCTL_ID64     0x80084506u    /* EVIOCGNAME(len)（len 由调用方填进 size 位） */
#define EVDEV64_IOCTL_VER64    0x80044501u    /* EVIOCGVERSION */
#define EVDEV64_IOCTL_BIT64    0x80084520u    /* EVIOCGBIT(0, len)：支持的事件类型位图 */

// ==================== ★ v0.9.21：evdev 的 grab / abs 三件套 ====================
// Linux `include/uapi/linux/input.h` 的同一套编码（size 位 = struct 尺寸），逐值相等：
//   EVIOCGRAB      = _IOW('E', 0x90, int)          = 0x40044590
//   EVIOCGABS(ax)  = _IOR('E', 0x40+ax, absinfo)   = 0xC01845<40+ax>
//   EVIOCSABS(ax)  = _IOW('E', 0xC0+ax, absinfo)   = 0x401845<C0+ax>
// 为什么需要：libinput 的 evdev 后端**必须**能 grab（独占设备）与读绝对轴范围，
//   否则 wlroots 的 libinput 后端拿到鼠标后无法拿到"屏幕坐标 → 绝对轴"的映射区间。
#define EVDEV64_IOCTL_GRAB64       0x40044590u
#define EVDEV64_IOCTL_GABS64(axis) (0xC0184500u + ((uint32_t)(axis) & 7u))
#define EVDEV64_IOCTL_SABS64(axis) (0x401845C0u + ((uint32_t)(axis) & 7u))
#define EVDEV64_ABS_X64            0u
#define EVDEV64_ABS_Y64            1u
#define EVDEV64_MAX_ABS64          2u
struct __attribute__((packed)) InputAbsInfo64 {
    int32_t value;       // +0   当前值（本内核不跟踪，如实置 0）
    int32_t minimum;     // +4   ABS_X: 0 / ABS_Y: 0
    int32_t maximum;     // +8   = 屏幕宽-1 / 高-1（鼠标绝对轴的真实区间）
    int32_t fuzz;        // +12  恒 0（没有硬件去抖）
    int32_t flat;        // +16  恒 0
    int32_t resolution;  // +20  恒 0
};                      // = 24 B（= Linux struct input_absinfo）
DRM64_STATIC_ASSERT(sizeof(struct InputAbsInfo64) == 24, "InputAbsInfo64 必须是 24 B（= Linux input_absinfo）");
/* EVIOCGNAME(len) / EVIOCGBIT(ev, len) 的具体命令字：在基值上按 len 填 size 位 */
#define EVDEV64_IOCGNAME(len)  (0x80080000u | (((uint32_t)(len) & 0x3FFFu) << 16) | 0x4506u)
#define EVDEV64_IOCGBIT(ev, len) \
    (0x80080000u | (((uint32_t)(len) & 0x3FFFu) << 16) | 0x4520u | (((uint32_t)(ev) & 0x3FFFu) << 8))

/* ==================== 薄封装（user/lib/drm64.c）====================
 * drm64_open()       -> fd（/dev/dri/card0）/ 负数 errno
 * drm64_ioctl()      -> 内核返回值（0 或负 errno；不做 errno 翻译，调用方如实打印）
 * evdev64_open(which)-> fd（which = 0: /dev/input/event0 键盘 / 1: event1 鼠标）
 * evdev64_read()     -> 读到的字节数（24 的倍数）/ 负数（-EAGAIN = 队列空）
 */
int  drm64_open(void);
long drm64_ioctl(int fd, unsigned long cmd, void* arg);
int  evdev64_open(int which);
long evdev64_read(int fd, void* buf, long len);
long drm64_read(int fd, void* buf, long len);      /* ★ v0.9.20：读 DRM 事件（32 B/条，空队列 -EAGAIN） */

#endif /* VIMTU64_DRM64_H */
