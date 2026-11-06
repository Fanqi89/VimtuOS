// drm64.cpp - ★ v0.9.12：DRM/KMS 内核门面 + evdev 门面（实现）
//
// 先读 kernel/drm64.h —— ABI、dumb buffer 怎么让 ring3 可写、混合呈现的分工、打点格式
// 全部写在那个文件头里。这里只记实现上的三件事：
//   1) dumb buffer 的**页帧表**存在本文件的状态里（page_alloc_64 逐页分配，不要求物理连续），
//      跨页拷贝由 d64_copy64() 负责（按页边界切块 + 8 字节字拷贝）。
//   2) ring3 可写靠 MAP_DUMB 把同一批物理页挂进**当前进程**的页表（user64_map_phys_page64），
//      窗口 = USER64_FB_VA64 的后半段（[+8MiB, +40MiB)，4 槽 × 8MiB）。窗口选择理由见头文件。
//   3) 上屏只走既有两条路径：vgpu64_blit64（virtio-gpu 2D 传输 + FLUSH）优先，
//      失败/无设备 -> fb_soft_flip_region64（CPU 逐行写 LFB）。本文件不碰任何 GPU 寄存器。
//
// 体积纪律：日志原语一律 __attribute__((noinline)) 包装（照 kernel/gpu64.cpp 的写法）——
//   dbg64_* 是 static inline，每处展开都带 outb + 串口自旋，几十处打点会白涨几 KB .text。
#include "drm64.h"
#include "debug64.h"
#include "fb.h"            // fb_surface64 / fb_phys_* / fb_soft_flip_region64 / vblank 复用入口
#include "virtio_gpu64.h"  // vgpu64_blit64（GPU 传输路径；没有设备时返回 0）
#include "usermode64.h"    // user64_range_ok64 / user64_map_phys_page64 / USER64_FB_VA64 / PTE_*
#include "mem_64.h"        // page_alloc_64 / page_free_64 / PAGE_SIZE_64 / PTE_*
#include "x86_64.h"        // g_ticks64 / PIT_HZ_64 / TICK_MS_64 / ms_to_ticks64
#include "input64.h"       // Ev64Event / ev64_current_queue64 / ev64_pop64（evdev 的数据源）
#include "task64.h"        // task_sleep64（演示的有界等待）
#include "vfs64.h"         // 卷里有没有 /bin/drmdemo.elf、/etc/drm64_probe
#include "proc64.h"        // 启动期 ring3 验收程序（真进程：proc64_create64/start_elf64）

// ==================== 打点小工具（noinline，见文件头的体积纪律）====================
static const char* const D64_HEXL = "0123456789abcdef";
__attribute__((noinline)) static void d64_puts(const char* s) { dbg64_str(s); }
__attribute__((noinline)) static void d64_ch(char c) { dbg64_putc(c); }
__attribute__((noinline)) static void d64_num(uint64_t v) { dbg64_dec(v); }
__attribute__((noinline)) static void d64_hex(uint64_t v) {      // "0x" + 最短十六进制
    d64_puts("0x");
    if (!v) { d64_ch('0'); return; }
    char b[16]; int n = 0;
    while (v && n < 16) { b[n++] = D64_HEXL[v & 0xFu]; v >>= 4; }
    while (n > 0) d64_ch(b[--n]);
}
// line begin/end 也走 noinline：dbg64_line_begin64/end64 自带 cli + 锁 + 深度记账，
//   30 多处展开纯属体积浪费（本层 .text 的目标是 < 12 KB）。
__attribute__((noinline)) static void d64_begin() { dbg64_line_begin64(); }
__attribute__((noinline)) static void d64_end()   { dbg64_nl(); dbg64_line_end64(); }


// ==================== 用户内存（范围校验由调用方做，这里只搬字节）====================
// 为什么可以裸解引用：ioctl 是在**当前进程的 CR3** 下跑的（syscall64 的 lx64_ioctl64），
// 传进来的 arg 已经过了 user64_range_ok64 —— 与 proc64 的 shm_map 写 out_va 是同一种写法。
__attribute__((noinline)) static void d64_urd64(uint64_t uva, void* dst, uint32_t n) {
    const uint8_t* s = (const uint8_t*)(uintptr_t)uva; uint8_t* d = (uint8_t*)dst;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
__attribute__((noinline)) static void d64_uwr64(uint64_t uva, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)(uintptr_t)uva; const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
__attribute__((noinline)) static void d64_uwr32(uint64_t uva, uint32_t v) { *(uint32_t*)(uintptr_t)uva = v; }

// ==================== 状态（全部 .bss；无动态分配）====================
struct Drm64Dumb {
    uint32_t used;
    uint32_t id;        // handle = 槽号 + 1
    uint32_t w, h, bpp, stride;
    uint32_t pages;
    uint32_t owner;     // 创建它的 pid（0 = 内核/没有进程上下文）
    uint64_t bytes;
    uint64_t uva;       // 映射给 ring3 的 VA（0 = 没映射过；每进程各自映射到同一个 VA）
    uint64_t phys[DRM64_DUMB_MAX_PAGES64];
};
static Drm64Dumb  g_dumb[DRM64_MAX_DUMB64];
static uint32_t   g_handle_on_screen = 0;   // 当前"在屏"的 handle（GETCRTC 的 fb_id）
static uint64_t   g_seq = 0;                // vblank 序列号（单调递增）
static uint64_t   g_presents = 0;           // present 计数
static int        g_inited = 0;
static int        g_vblank_hw = 0;          // 1 = 硬件垂直回扫可用（0 = 软兜底）
static uint8_t    g_rowbuf[DRM64_DUMB_MAX_STRIDE64];   // 自检/图案用的行缓冲（.bss，8 KiB）

// ==================== 跨页安全的拷贝（dumb buffer <-> 连续内存）====================
// to_buf=1：连续内存 -> dumb buffer（ring3 之后会读到的就是这份内容）
// to_buf=0：dumb buffer -> 连续内存（呈现时拷进内核后备缓冲）
// 按页边界切块，块内能用 8 字节字就用（3 MB 的整屏拷贝不能逐字节）。
// 自检用的**屏幕快照**（.bss）：只覆盖自检几何（<= 384x288 = 432 KiB）。
// 为什么不 kmalloc/页池：自检要把开机画面原样翻回来，必须有一块连续、随时可写的暂存；
// .bss 不进内核镜像文件大小，代价只是启动期清零这 432 KiB。
#define D64_SNAP_W64 384u
#define D64_SNAP_H64 288u
static uint32_t g_snapbuf[D64_SNAP_W64 * D64_SNAP_H64];
// 行拷贝（快照 <-> 后备缓冲）：连续内存之间的纯 32 位字拷贝。
__attribute__((noinline)) static void d64_rows64(uint32_t* dst, const uint32_t* src, uint32_t npx) {
    for (uint32_t i = 0; i < npx; i++) dst[i] = src[i];
}
__attribute__((noinline)) static void d64_copy64(const Drm64Dumb* d, uint64_t off, void* contig, uint32_t n, int to_buf) {
    uint8_t* c = (uint8_t*)contig;
    while (n) {
        const uint64_t pi = off >> 12;
        const uint32_t po = (uint32_t)(off & 0xFFFu);
        uint32_t chunk = 4096u - po;
        if (chunk > n) chunk = n;
        uint8_t* p = (uint8_t*)(uintptr_t)(d->phys[pi] + po);      // 0..4GiB 恒等直映
        if (!(((uint64_t)(uintptr_t)p & 7u) == 0 && (chunk & 7u) == 0 && (po & 7u) == 0)) {
            if (to_buf) for (uint32_t i = 0; i < chunk; i++) p[i] = c[i];
            else        for (uint32_t i = 0; i < chunk; i++) c[i] = p[i];
        } else {
            uint64_t* pw = (uint64_t*)p; uint64_t* cw = (uint64_t*)c;
            const uint32_t words = chunk >> 3;
            if (to_buf) for (uint32_t i = 0; i < words; i++) pw[i] = cw[i];
            else        for (uint32_t i = 0; i < words; i++) cw[i] = pw[i];
        }
        c += chunk; off += chunk; n -= chunk;
    }
}

// ==================== 槽位 / 句柄 ====================
static Drm64Dumb* d64_find64(uint32_t handle) {
    if (handle == 0 || handle > DRM64_MAX_DUMB64) return nullptr;
    Drm64Dumb* d = &g_dumb[handle - 1];
    return d->used ? d : nullptr;
}

static void d64_free_pages64(Drm64Dumb* d) {
    for (uint32_t i = 0; i < d->pages; i++) {
        if (d->phys[i]) { page_free_64((void*)(uintptr_t)d->phys[i]); d->phys[i] = 0; }
    }
}

// 建一块 dumb buffer（内核侧入口：ioctl 与自检共用）。成功返回 0，并把 handle 写进 *out。
static int d64_create64(uint32_t w, uint32_t h, uint32_t bpp, uint32_t* out) {
    if (!out) return -DRM64_EINVAL64;
    if (bpp != 32u || w == 0u || h == 0u) return -DRM64_EINVAL64;
    const uint64_t stride = (uint64_t)w * 4ull;
    if (stride > (uint64_t)DRM64_DUMB_MAX_STRIDE64 || h > DRM64_DUMB_MAX_HEIGHT64) return -DRM64_EINVAL64;
    const uint64_t bytes = stride * (uint64_t)h;
    if (bytes == 0ull || bytes > DRM64_DUMB_MAX_BYTES64) return -DRM64_EINVAL64;
    const uint32_t pages = (uint32_t)((bytes + PAGE_SIZE_64 - 1ull) / PAGE_SIZE_64);
    if (pages == 0u || pages > DRM64_DUMB_MAX_PAGES64) return -DRM64_EINVAL64;

    int slot = -1;
    for (uint32_t i = 0; i < DRM64_MAX_DUMB64; i++) if (!g_dumb[i].used) { slot = (int)i; break; }
    if (slot < 0) {
        d64_begin(); d64_puts("[DRM] create FAILED reason=no-free-slot cap=");
        d64_num(DRM64_MAX_DUMB64); d64_puts(" err="); d64_num(DRM64_ENOMEM64); d64_end();
        return -DRM64_ENOMEM64;
    }
    Drm64Dumb* d = &g_dumb[slot];
    for (uint32_t i = 0; i < sizeof(Drm64Dumb); i++) ((uint8_t*)d)[i] = 0;   // 槽必须干净
    for (uint32_t i = 0; i < pages; i++) {
        void* fp = page_alloc_64();
        if (!fp) {
            d64_free_pages64(d);                                    // 已拿到的页还回去（不泄漏）
            for (uint32_t k = 0; k < sizeof(Drm64Dumb); k++) ((uint8_t*)d)[k] = 0;
            d64_begin(); d64_puts("[DRM] create FAILED reason=page-pool-empty pages=");
            d64_num(pages); d64_puts(" err="); d64_num(DRM64_ENOMEM64); d64_end();
            return -DRM64_ENOMEM64;
        }
        d->phys[i] = (uint64_t)(uintptr_t)fp;
        uint64_t* z = (uint64_t*)fp;                                // 页池不清零：不给下一个用户看旧内容
        for (uint32_t k = 0; k < (uint32_t)(PAGE_SIZE_64 / 8u); k++) z[k] = 0;
    }
    d->used = 1u;
    d->id = (uint32_t)slot + 1u;
    d->w = w; d->h = h; d->bpp = bpp;
    d->stride = (uint32_t)stride;
    d->pages = pages;
    d->bytes = bytes;
    d->owner = 0;                                                   // ioctl 侧填真 pid
    *out = d->id;
    d64_begin();
    d64_puts("[DRM] create handle="); d64_num(d->id);
    d64_puts(" w="); d64_num(w); d64_puts(" h="); d64_num(h);
    d64_puts(" pitch="); d64_num(d->stride); d64_puts(" size="); d64_num(bytes);
    d64_puts(" pages="); d64_num(pages);
    d64_end();
    return 0;
}

static int d64_destroy64(uint32_t handle) {
    Drm64Dumb* d = d64_find64(handle);
    if (!d) return -DRM64_EINVAL64;
    const uint32_t pages = d->pages;
    d64_free_pages64(d);
    for (uint32_t k = 0; k < sizeof(Drm64Dumb); k++) ((uint8_t*)d)[k] = 0;
    if (g_handle_on_screen == handle) g_handle_on_screen = 0;
    d64_begin();
    d64_puts("[DRM] destroy handle="); d64_num(handle);
    d64_puts(" pages="); d64_num(pages);
    d64_puts(" freed="); d64_num(pages);
    d64_puts(" free_pages="); d64_num(page_count_free_64());
    d64_end();
    return 0;
}

// ==================== ring3 可写：把物理页挂进当前进程的页表 ====================
// 窗口 = USER64_FB_VA64 + 8MiB + 槽 * 8MiB（见头文件）。返回用户 VA（0 = 失败）。
static uint64_t d64_map_uva64(uint32_t slot, Drm64Dumb* d) {
    if (!user64_available64()) return 0;
    int bw = 0, bh = 0;
    if (!fb_surface64(&bw, &bh)) return 0;
    (void)bh;
    int bytes = 0;
    (void)fb_surface_phys64(&bytes);
    // 后备缓冲占 FB 窗的前 [0, bytes) 段；两者不能重叠（重叠 = 互相踩）。
    if (bytes <= 0 || (uint64_t)bytes > DRM64_UVA_BASE_OFFSET64) return 0;
    const uint64_t va = USER64_FB_VA64 + DRM64_UVA_BASE_OFFSET64 + (uint64_t)slot * DRM64_UVA_SLOT_BYTES64;
    const uint64_t need = (d->bytes + PAGE_SIZE_64 - 1ull) / PAGE_SIZE_64;
    if (need * PAGE_SIZE_64 > DRM64_UVA_SLOT_BYTES64) return 0;
    for (uint64_t i = 0; i < need; i++) {
        if (!user64_map_phys_page64(va + i * PAGE_SIZE_64, d->phys[i],
                                    PTE_USER_64 | PTE_WRITE_64 | PTE_NX_64)) {
            for (uint64_t k = 0; k < i; k++) (void)user64_unmap_page64(va + k * PAGE_SIZE_64);
            user64_paging_sync64();
            return 0;
        }
    }
    user64_paging_sync64();
    d->uva = va;
    return user64_page_is_user_ok64(va) ? va : 0;    // 说好"用户可写"：没做到就如实失败
}

// 上屏的统一入口（GPU 2D 传输优先，否则 CPU 写 LFB）—— 定义在 vblank 段之后
static int d64_present_backbuf64(int w, int h);
// ==================== 呈现（CPU 光栅化 + GPU 传输）====================
// 返回 1 = 走了 virtio-gpu 2D（path=gpu）/ 0 = CPU 写 LFB（path=cpu）/ -1 = 没有可用的屏。
static int d64_present_dumb64(Drm64Dumb* d) {
    int bw = 0, bh = 0;
    uint32_t* bb = fb_surface64(&bw, &bh);
    if (!bb || bw <= 0 || bh <= 0) return -1;
    uint32_t cw = d->w, ch = d->h;
    if (cw > (uint32_t)bw) cw = (uint32_t)bw;
    if (ch > (uint32_t)bh) ch = (uint32_t)bh;
    if (cw == 0u || ch == 0u) return -1;
    // ① CPU 侧：把 ring3 画好的 dumb buffer 逐行拷进内核后备缓冲（既有的绘制目标）
    for (uint32_t y = 0; y < ch; y++) {
        d64_copy64(d, (uint64_t)y * (uint64_t)d->stride, bb + (uint64_t)y * (uint64_t)bw, cw * 4u, 0);
    }
    const int dev = d64_present_backbuf64((int)cw, (int)ch);
    d64_begin();
    d64_puts("[DRM] present id="); d64_num(d->id);
    d64_puts(dev ? " path=gpu bytes=" : " path=cpu bytes=");
    d64_num((uint64_t)cw * 4ull * (uint64_t)ch);
    d64_puts(" seq="); d64_num(g_seq);
    d64_end();
    return dev ? 1 : 0;
}

// ==================== vblank（硬件回扫优先，软兜底）====================
// 返回 1 = 用了硬件回扫 / 0 = 软兜底。序列号（g_seq）在这里单调 +1。
static int d64_vblank_wait64(void) {
    int hw = 0;
    d64_begin(); d64_puts("[DRM] wait enter hw="); d64_ch(g_vblank_hw ? '1' : '0');
    d64_puts(" seq="); d64_num(g_seq); d64_end();
    if (g_vblank_hw) hw = fb64_vblank_wait_export64() ? 1 : 0;
    uint32_t spin = 0;
    if (!hw) {
        const uint64_t t0 = g_ticks64;
        uint32_t want = ms_to_ticks64(16);
        if (want == 0u) want = 1u;
        // ★ 上限从 2亿降到 200万：tick 没走（IF=0 / 时钟没到）时必须**快速返回**，
        //   否则一次等待就把调用方（尤其 ring3 进程）卡在这里。
        while ((g_ticks64 - t0) < (uint64_t)want && ++spin < 2000000u) __asm__ volatile("pause");
    }
    g_seq++;
    d64_begin(); d64_puts("[DRM] wait leave hw="); d64_ch(hw ? '1' : '0');
    d64_puts(" spin="); d64_num((uint64_t)spin); d64_puts(" seq="); d64_num(g_seq); d64_end();
    return hw;
}
// 把后备缓冲的 [0,0,w,h) 提交上屏：GPU 2D 传输优先，否则 CPU 写 LFB（page_flip 与自检还原共用）。
// 返回 1 = 走了设备 / 0 = CPU。
__attribute__((noinline)) static int d64_present_backbuf64(int w, int h) {
    const int dev = vgpu64_blit64(0, 0, w, h);
    if (!dev) fb_soft_flip_region64(0, 0, w, h);
    g_presents++;
    return dev;
}
// tick -> (sec, usec)：4 ms 粒度，如实（与 clock_gettime 同一时基）。
static void d64_time64(uint64_t* sec, uint64_t* usec) {
    const uint64_t t = g_ticks64;
    if (sec)  *sec  = t / (uint64_t)PIT_HZ_64;
    if (usec) *usec = (t % (uint64_t)PIT_HZ_64) * 1000ull * (uint64_t)TICK_MS_64;
}

// ==================== 模式信息（几何的唯一来源 = fb 的既有几何）====================
static void d64_mode_name64(char* out, uint32_t cap, uint32_t w, uint32_t h) {
    uint32_t n = 0;
    char t[12]; int k = 0;
    if (cap < 2u) return;
    uint32_t v = w; do { t[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && k < 11);
    while (k > 0 && n + 1u < cap) out[n++] = t[--k];
    if (n + 1u < cap) out[n++] = 'x';
    v = h; k = 0; do { t[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v && k < 11);
    while (k > 0 && n + 1u < cap) out[n++] = t[--k];
    out[n] = 0;
}
// 模式只算**一次**（init 里），之后 GETCRTC / GETCONNECTOR 直接拷这份静态结构 ——
// 省掉每次调用的清零 + 拼名字（名字依赖几何，而几何在一次启动里不会变）。
static Drm64ModeMode g_mode64;
static void d64_fill_mode64(uint32_t w, uint32_t h) {
    uint8_t* p = (uint8_t*)&g_mode64;
    for (uint32_t i = 0; i < sizeof(Drm64ModeMode); i++) p[i] = 0;
    d64_mode_name64(g_mode64.name, (uint32_t)sizeof(g_mode64.name), w, h);
    g_mode64.hdisplay = (uint16_t)w; g_mode64.vdisplay = (uint16_t)h;
    g_mode64.hsync_start = (uint16_t)(w + 16u); g_mode64.hsync_end = (uint16_t)(w + 48u);
    g_mode64.htotal = (uint16_t)(w + 160u);
    g_mode64.vsync_start = (uint16_t)(h + 3u);  g_mode64.vsync_end = (uint16_t)(h + 6u);
    g_mode64.vtotal = (uint16_t)(h + 32u);
    g_mode64.vrefresh = 60u;                                              // 估计值（没有 EDID 时序表）
    g_mode64.clock = (uint32_t)(((uint64_t)w * (uint64_t)h * 60ull) / 1000ull);  // kHz 估计
}

// ==================== ioctl 分派 ====================
// 把一个常量短字符串写进用户缓冲（cap = 调用方给的字节数）；返回写入字节数（含 NUL；0 = 没写成）。
// 为什么不「取 packed 成员的地址」：那会触发 -Waddress-of-packed-member，且地址可能非对齐。
static uint64_t d64_put_str64(const char* s, uint64_t ptr, uint64_t cap) {
    uint32_t n = 0;
    while (s[n]) n++;
    n++;                                                             // 含 NUL
    if (!ptr || cap == 0ull || !user64_range_ok64(ptr, cap)) return 0ull;
    const uint32_t w = (n <= (uint32_t)cap) ? n : (uint32_t)cap;
    d64_uwr64(ptr, s, w);
    if (w >= (uint32_t)cap) ((char*)(uintptr_t)ptr)[cap - 1ull] = 0;  // 缓冲不够：末字节必须是 NUL
    return (uint64_t)w;
}
static int d64_ioctl_version64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64Version))) return -DRM64_EFAULT64;
    Drm64Version v;
    d64_urd64(arg, &v, sizeof(Drm64Version));
    v.version_major = 1; v.version_minor = 0; v.version_patchlevel = 0;
    // 短字符串写进**调用方给的**用户缓冲：长度 = 含 NUL 的实际字节数（Linux 语义）
    v.name_len = d64_put_str64(DRM64_DEV_NAME64, v.name, v.name_len);
    v.date_len = d64_put_str64(DRM64_DEV_DATE64, v.date, v.date_len);
    v.desc_len = d64_put_str64(DRM64_DEV_DESC64, v.desc, v.desc_len);
    d64_uwr64(arg, &v, sizeof(Drm64Version));
    d64_begin(); d64_puts("[DRM] version name="); d64_puts(DRM64_DEV_NAME64);
    d64_puts(" major="); d64_num((uint64_t)v.version_major); d64_end();
    return 0;
}

static int d64_ioctl_getcap64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64GetCap))) return -DRM64_EFAULT64;
    Drm64GetCap c;
    d64_urd64(arg, &c, sizeof(Drm64GetCap));
    if (c.capability == 1ull || c.capability == 2ull) c.value = 1ull;  // DUMB_BUFFER / VBLANK
    else { c.value = 0ull; d64_begin(); d64_puts("[DRM] get_cap unsupported cap="); d64_num(c.capability); d64_end(); }
    d64_uwr64(arg, &c, sizeof(Drm64GetCap));
    return 0;
}

// 当前可用的 fb id 列表（= 活着的 dumb handle；本内核 fb == dumb buffer）
static uint32_t d64_fb_list64(uint32_t* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < DRM64_MAX_DUMB64 && n < cap; i++) if (g_dumb[i].used) out[n++] = g_dumb[i].id;
    return n;
}

static int d64_ioctl_getresources64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModeResources))) return -DRM64_EFAULT64;
    Drm64ModeResources r;
    d64_urd64(arg, &r, sizeof(Drm64ModeResources));
    uint32_t fbs[DRM64_MAX_DUMB64];
    const uint32_t nf = d64_fb_list64(fbs, DRM64_MAX_DUMB64);
    // crtc / connector / encoder 都是 1 个（如实）
    const uint32_t ncr = DRM64_MAX_CRTC64, ncn = 1u, nen = 1u;
    if ((r.count_fbs && r.count_fbs < nf) || (r.count_crtcs && r.count_crtcs < ncr) ||
        (r.count_connectors && r.count_connectors < ncn) || (r.count_encoders && r.count_encoders < nen)) {
        d64_begin(); d64_puts("[DRM] getresources FAILED reason=count-too-small err=");
        d64_num(DRM64_EINVAL64); d64_end();
        return -DRM64_EINVAL64;
    }
    if (r.fb_id_ptr)         for (uint32_t i = 0; i < nf; i++) d64_uwr32(r.fb_id_ptr + (uint64_t)i * 4ull, fbs[i]);
    if (r.crtc_id_ptr)       for (uint32_t i = 0; i < ncr; i++) d64_uwr32(r.crtc_id_ptr + (uint64_t)i * 4ull, DRM64_CRTC_ID64);
    if (r.connector_id_ptr)  for (uint32_t i = 0; i < ncn; i++) d64_uwr32(r.connector_id_ptr + (uint64_t)i * 4ull, DRM64_CONNECTOR_ID64);
    if (r.encoder_id_ptr)    for (uint32_t i = 0; i < nen; i++) d64_uwr32(r.encoder_id_ptr + (uint64_t)i * 4ull, DRM64_ENCODER_ID64);
    r.count_fbs = nf; r.count_crtcs = ncr; r.count_connectors = ncn; r.count_encoders = nen;
    r.min_width = 16u; r.min_height = 16u;
    r.max_width  = (uint32_t)fb_phys_width();
    r.max_height = (uint32_t)fb_phys_height();
    if (r.max_width > 2048u) r.max_width = 2048u;                    // 单行/单块上限（DRM64_DUMB_MAX_*）
    if (r.max_height > DRM64_DUMB_MAX_HEIGHT64) r.max_height = DRM64_DUMB_MAX_HEIGHT64;
    if (r.min_width > r.max_width) r.min_width = r.max_width;
    if (r.min_height > r.max_height) r.min_height = r.max_height;
    d64_uwr64(arg, &r, sizeof(Drm64ModeResources));
    d64_begin(); d64_puts("[DRM] getresources fbs="); d64_num(nf);
    d64_puts(" crtcs="); d64_num(ncr); d64_puts(" connectors="); d64_num(ncn);
    d64_puts(" max="); d64_num(r.max_width); d64_puts("x"); d64_num(r.max_height); d64_end();
    return 0;
}

static int d64_ioctl_getcrtc64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModeCrtc))) return -DRM64_EFAULT64;
    Drm64ModeCrtc c;
    d64_urd64(arg, &c, sizeof(Drm64ModeCrtc));
    c.crtc_id = DRM64_CRTC_ID64;
    c.fb_id = g_handle_on_screen;
    c.x = 0; c.y = 0; c.gamma_size = 0; c.mode_valid = 1;
    c.mode = g_mode64;
    c.count_connectors = 1u;
    if (c.set_connectors_ptr) d64_uwr32(c.set_connectors_ptr, DRM64_CONNECTOR_ID64);
    d64_uwr64(arg, &c, sizeof(Drm64ModeCrtc));
    d64_begin(); d64_puts("[DRM] getcrtc id="); d64_num(c.crtc_id);
    d64_puts(" fb="); d64_num(c.fb_id); d64_puts(" mode="); d64_puts(c.mode.name); d64_end();
    return 0;
}

static int d64_ioctl_getconnector64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModeConnector))) return -DRM64_EFAULT64;
    Drm64ModeConnector c;
    d64_urd64(arg, &c, sizeof(Drm64ModeConnector));
    if (c.connector_id != 0u && c.connector_id != DRM64_CONNECTOR_ID64) {
        d64_begin(); d64_puts("[DRM] getconnector FAILED reason=no-such-connector id=");
        d64_num(c.connector_id); d64_puts(" err="); d64_num(DRM64_EINVAL64); d64_end();
        return -DRM64_EINVAL64;
    }
    const Drm64ModeMode m = g_mode64;
    if (c.modes_ptr && c.count_modes >= 1u && user64_range_ok64(c.modes_ptr, sizeof(Drm64ModeMode)))
        d64_uwr64(c.modes_ptr, &m, sizeof(Drm64ModeMode));
    if (c.encoders_ptr && c.count_encoders >= 1u) d64_uwr32(c.encoders_ptr, DRM64_ENCODER_ID64);
    c.connector_id = DRM64_CONNECTOR_ID64;
    c.connector_type = 11u;                                          // DRM_MODE_CONNECTOR_HDMIA
    c.connector_type_id = 1u;
    c.connection = 1u;                                               // connected（本内核只有"有屏"这一种）
    c.encoder_id = DRM64_ENCODER_ID64;
    c.count_modes = 1u; c.count_encoders = 1u; c.count_props = 0u;
    c.mm_width = 0u; c.mm_height = 0u; c.subpixel = 0u;              // 没有 EDID：如实 0
    d64_uwr64(arg, &c, sizeof(Drm64ModeConnector));
    d64_begin(); d64_puts("[DRM] getconnector id="); d64_num(c.connector_id);
    d64_puts(" connected=1 mode="); d64_puts(m.name); d64_end();
    return 0;
}

static int d64_ioctl_create_dumb64(uint64_t arg, uint64_t pid) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModeCreateDumb))) return -DRM64_EFAULT64;
    Drm64ModeCreateDumb c;
    d64_urd64(arg, &c, sizeof(Drm64ModeCreateDumb));
    if (c.flags != 0u) { d64_begin(); d64_puts("[DRM] create FAILED reason=flags err="); d64_num(DRM64_EINVAL64); d64_end(); return -DRM64_EINVAL64; }
    uint32_t h = 0;
    const int r = d64_create64(c.width, c.height, c.bpp, &h);
    if (r != 0) return r;
    Drm64Dumb* d = d64_find64(h);
    if (d) d->owner = (uint32_t)(pid > 0xFFFFFFFFull ? 0u : pid);
    c.handle = h;
    c.pitch = d ? d->stride : (c.width * 4u);
    c.size = d ? d->bytes : 0ull;
    d64_uwr64(arg, &c, sizeof(Drm64ModeCreateDumb));
    return 0;
}

static int d64_ioctl_map_dumb64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModeMapDumb))) return -DRM64_EFAULT64;
    Drm64ModeMapDumb m;
    d64_urd64(arg, &m, sizeof(Drm64ModeMapDumb));
    Drm64Dumb* d = d64_find64(m.handle);
    if (!d) { d64_begin(); d64_puts("[DRM] map FAILED handle="); d64_num(m.handle); d64_puts(" reason=no-such-handle err="); d64_num(DRM64_EINVAL64); d64_end(); return -DRM64_EINVAL64; }
    const uint32_t slot = m.handle - 1u;
    const uint64_t va = d64_map_uva64(slot, d);
    if (!va) {
        d64_begin(); d64_puts("[DRM] map FAILED handle="); d64_num(m.handle);
        d64_puts(" reason=no-user-window/page-map err="); d64_num(DRM64_ENOMEM64); d64_end();
        return -DRM64_ENOMEM64;
    }
    m.offset = va;
    d64_uwr64(arg, &m, sizeof(Drm64ModeMapDumb));
    d64_begin();
    d64_puts("[DRM] map handle="); d64_num(m.handle);
    d64_puts(" va="); d64_hex(va);
    d64_puts(" pages="); d64_num(d->pages);
    d64_puts(" phys="); d64_hex(d->phys[0]);
    d64_puts(" u=1");
    d64_end();
    return 0;
}

static int d64_ioctl_page_flip64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64ModePageFlip))) return -DRM64_EFAULT64;
    Drm64ModePageFlip f;
    d64_urd64(arg, &f, sizeof(Drm64ModePageFlip));
    if (f.crtc_id != DRM64_CRTC_ID64 || f.flags != 0u) {
        d64_begin(); d64_puts("[DRM] page_flip FAILED reason=crtc/flags crtc=");
        d64_num(f.crtc_id); d64_puts(" err="); d64_num(DRM64_EINVAL64); d64_end();
        return -DRM64_EINVAL64;
    }
    Drm64Dumb* d = d64_find64(f.fb_id);
    if (!d) { d64_begin(); d64_puts("[DRM] page_flip FAILED reason=no-such-fb id="); d64_num(f.fb_id); d64_puts(" err="); d64_num(DRM64_EINVAL64); d64_end(); return -DRM64_EINVAL64; }
    const int path = d64_present_dumb64(d);
    if (path < 0) {
        d64_begin(); d64_puts("[DRM] present FAILED id="); d64_num(d->id);
        d64_puts(" reason=no-framebuffer err="); d64_num(DRM64_ENODEV64); d64_end();
        return -DRM64_ENODEV64;
    }
    g_handle_on_screen = d->id;
    (void)d64_vblank_wait64();                                       // "翻转后等一次 vblank"（同步语义）
    d64_begin();
    d64_puts("[DRM] page_flip crtc="); d64_num(DRM64_CRTC_ID64);
    d64_puts(" fb="); d64_num(d->id);
    d64_puts(" seq="); d64_num(g_seq);
    d64_puts(" path="); d64_puts(path ? "gpu" : "cpu");
    d64_end();
    return 0;
}

static int d64_ioctl_wait_vblank64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64WaitVblank))) return -DRM64_EFAULT64;
    Drm64WaitVblank v;
    d64_urd64(arg, &v, sizeof(Drm64WaitVblank));
    if (v.crtc_id != DRM64_CRTC_ID64) {
        v.sequence = g_seq; d64_uwr64(arg, &v, sizeof(Drm64WaitVblank));
        return -DRM64_EINVAL64;
    }
    uint64_t hw = 0;
    if (v.request != 0ull) hw = (uint64_t)d64_vblank_wait64();
    v.sequence = g_seq;
    { uint64_t ts = 0, tu = 0; d64_time64(&ts, &tu); v.tval_sec = ts; v.tval_usec = tu; }
    v.flags = (uint32_t)(hw ? 1u : 0u);
    d64_uwr64(arg, &v, sizeof(Drm64WaitVblank));
    d64_begin(); d64_puts("[DRM] wait_vblank seq="); d64_num(v.sequence);
    d64_puts(hw ? " mode=hw ticks=" : " mode=soft ticks="); d64_num(g_ticks64); d64_end();
    return 0;
}

static int d64_ioctl_crtc_seq64(uint64_t arg) {
    if (!user64_range_ok64(arg, sizeof(Drm64CrtcSeq))) return -DRM64_EFAULT64;
    Drm64CrtcSeq s;
    d64_urd64(arg, &s, sizeof(Drm64CrtcSeq));
    if (s.crtc_id != DRM64_CRTC_ID64) return -DRM64_EINVAL64;
    s.sequence = g_seq;
    s.flags = (uint32_t)(g_vblank_hw ? 1u : 0u);
    { uint64_t ts = 0, tu = 0; d64_time64(&ts, &tu); s.tval_sec = ts; s.tval_usec = tu; }
    s.ns = s.tval_sec * 1000000000ull + s.tval_usec * 1000ull;
    d64_uwr64(arg, &s, sizeof(Drm64CrtcSeq));
    return 0;
}

int drm64_ioctl64(uint64_t cmd, uint64_t arg, uint64_t pid) {
    switch (cmd) {
    case DRM64_IOWR(DRM64_NR_VERSION, Drm64Version):               return d64_ioctl_version64(arg);
    case DRM64_IOWR(DRM64_NR_GET_CAP, Drm64GetCap):                return d64_ioctl_getcap64(arg);
    case DRM64_IOWR(DRM64_NR_MODE_GETRESOURCES, Drm64ModeResources): return d64_ioctl_getresources64(arg);
    case DRM64_IOWR(DRM64_NR_MODE_GETCRTC, Drm64ModeCrtc):         return d64_ioctl_getcrtc64(arg);
    case DRM64_IOWR(DRM64_NR_MODE_GETCONNECTOR, Drm64ModeConnector): return d64_ioctl_getconnector64(arg);
    case DRM64_IOWR(DRM64_NR_MODE_CREATE_DUMB, Drm64ModeCreateDumb): return d64_ioctl_create_dumb64(arg, pid);
    case DRM64_IOWR(DRM64_NR_MODE_MAP_DUMB, Drm64ModeMapDumb):     return d64_ioctl_map_dumb64(arg);
    case DRM64_IOWR(DRM64_NR_MODE_DESTROY_DUMB, Drm64ModeDestroyDumb): {
        if (!user64_range_ok64(arg, sizeof(Drm64ModeDestroyDumb))) return -DRM64_EFAULT64;
        Drm64ModeDestroyDumb x;
        d64_urd64(arg, &x, sizeof(Drm64ModeDestroyDumb));
        return d64_destroy64(x.handle);
    }
    case DRM64_IOWR(DRM64_NR_MODE_PAGE_FLIP, Drm64ModePageFlip):   return d64_ioctl_page_flip64(arg);
    case DRM64_IOWR(DRM64_NR_WAIT_VBLANK, Drm64WaitVblank):        return d64_ioctl_wait_vblank64(arg);
    case DRM64_IOWR(DRM64_NR_CRTC_GET_SEQUENCE, Drm64CrtcSeq):     return d64_ioctl_crtc_seq64(arg);
    default:
        d64_begin(); d64_puts("[DRM] ioctl unknown cmd="); d64_hex(cmd);
        d64_puts(" err="); d64_num(DRM64_ENOTTY64); d64_end();
        return -DRM64_ENOTTY64;                                      // 与 Linux 对未知 ioctl 的口径一致
    }
}

// ==================== evdev：事件队列 -> struct input_event ====================
// 为什么要有这个"待取"小缓冲（8 条）：事件队列是**每进程一份**、键鼠混在一起，而
// /dev/input/event0 只该给键盘事件、event1 只给鼠标事件。过滤时不能把不属于本设备的
// 事件丢掉（那是静默丢事件），所以非本设备的先放进这里，下一次另一台设备的 read 先取它们。
static Ev64Event g_ev_pending[8];
static uint32_t  g_ev_pending_n = 0;
static uint32_t  g_ev_reads = 0;              // 打点预算（前 8 条 + 每 64 条一条）

static int ev64_is_key64(uint32_t t)  { return t == EV64_TYPE_KEY_DOWN || t == EV64_TYPE_KEY_UP; }
static int ev64_is_mouse64(uint32_t t) { return t == EV64_TYPE_MOUSE_MOVE || t == EV64_TYPE_MOUSE_DOWN ||
                                               t == EV64_TYPE_MOUSE_UP   || t == EV64_TYPE_WHEEL; }

// 把一条内核事件转成 1~2 条 input_event；返回写出的条数（0 = 这台设备不产这种事件）。
static uint32_t ev64_convert64(const Ev64Event* e, InputEvent64* out, uint32_t cap) {
    if (cap == 0u) return 0;
    const uint64_t sec = e->t_ms / 1000ull;
    const uint64_t usec = (e->t_ms % 1000ull) * 1000ull;
    if (e->type == EV64_TYPE_KEY_DOWN || e->type == EV64_TYPE_KEY_UP) {
        out[0].sec = sec; out[0].usec = usec; out[0].type = EV64_KEY;
        out[0].code = (uint16_t)e->code; out[0].value = (e->type == EV64_TYPE_KEY_DOWN) ? 1 : 0;
        return 1u;
    }
    if (e->type == EV64_TYPE_MOUSE_MOVE) {
        uint32_t n = 0;
        out[0].sec = sec; out[0].usec = usec; out[0].type = EV64_REL;
        out[0].code = (uint16_t)EV64_REL_X; out[0].value = e->dx; n = 1u;
        if (n < cap) {
            out[1].sec = sec; out[1].usec = usec; out[1].type = EV64_REL;
            out[1].code = (uint16_t)EV64_REL_Y; out[1].value = e->dy; n = 2u;
        }
        return n;
    }
    if (e->type == EV64_TYPE_MOUSE_DOWN || e->type == EV64_TYPE_MOUSE_UP) {
        out[0].sec = sec; out[0].usec = usec; out[0].type = EV64_KEY;
        out[0].code = (uint16_t)(e->code ? e->code : e->buttons);    // 键位掩码（左 1/右 2/中 4）
        out[0].value = (e->type == EV64_TYPE_MOUSE_DOWN) ? 1 : 0;
        return 1u;
    }
    if (e->type == EV64_TYPE_WHEEL) {
        out[0].sec = sec; out[0].usec = usec; out[0].type = EV64_REL;
        out[0].code = (uint16_t)EV64_REL_WHEEL; out[0].value = e->dy;
        return 1u;
    }
    return 0u;
}

int64_t evdev64_read64(int which, void* ubuf, uint64_t len) {
    if (!ubuf || len < sizeof(InputEvent64)) {
        d64_begin(); d64_puts("[EVDEV] read FAILED which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" reason=buf-too-small err="); d64_num(DRM64_EINVAL64); d64_end();
        return -DRM64_EINVAL64;
    }
    Ev64Queue* q = ev64_current_queue64();
    if (!q) {                                          // 没有进程上下文 = 这条通道不存在（不假装有）
        d64_begin(); d64_puts("[EVDEV] read FAILED which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" reason=no-process-context err="); d64_num(DRM64_ENODEV64); d64_end();
        return -DRM64_ENODEV64;
    }
    uint32_t maxev = (uint32_t)(len / sizeof(InputEvent64));
    if (maxev > EVDEV64_MAX_EVENTS64) maxev = EVDEV64_MAX_EVENTS64;
    InputEvent64 tmp[EVDEV64_MAX_EVENTS64];
    uint32_t nn = 0;

    // ① 先取上次过滤剩下的（属于本设备的）
    for (uint32_t i = 0; i < g_ev_pending_n; ) {
        const int mine = (which == 1) ? ev64_is_mouse64(g_ev_pending[i].type) : ev64_is_key64(g_ev_pending[i].type);
        if (mine && nn < maxev) {
            nn += ev64_convert64(&g_ev_pending[i], &tmp[nn], maxev - nn);
            g_ev_pending[i] = g_ev_pending[g_ev_pending_n - 1];
            g_ev_pending_n--;
            continue;
        }
        i++;
    }
    // ② 队列里还有就继续取（**一次一条**：内核栈上不放 32 条事件的批次，历史上前车之鉴；
    //   不属于本设备的进待取缓冲 —— 绝不静默丢事件，满了就本轮到此为止）
    while (nn < maxev) {
        Ev64Event one;
        if (ev64_pop64(q, &one, 1u) == 0u) break;
        if ((which == 1) ? ev64_is_mouse64(one.type) : ev64_is_key64(one.type)) {
            nn += ev64_convert64(&one, &tmp[nn], maxev - nn);
        } else if (g_ev_pending_n < 8u) {
            g_ev_pending[g_ev_pending_n++] = one;
        } else {
            break;
        }
    }
    if (nn == 0u) {                                    // 队列空：Linux 的 O_NONBLOCK 读法（不假造事件）
        if (g_ev_reads < 8u || (g_ev_reads % 64u) == 0u) {
            d64_begin(); d64_puts("[EVDEV] read which="); d64_num((uint64_t)(which < 0 ? 0 : which));
            d64_puts(" n=0 pend="); d64_num((uint64_t)g_ev_pending_n);
            d64_puts(" reason=empty err=11");
            d64_end();
        }
        g_ev_reads++;
        return -11;                                    // -EAGAIN（Linux 同一口径：非阻塞时空队列）
    }
    uint32_t bytes = nn * (uint32_t)sizeof(InputEvent64);
    d64_uwr64((uint64_t)(uintptr_t)ubuf, tmp, bytes);
    if (g_ev_reads < 8u || (g_ev_reads % 64u) == 0u) {
        d64_begin(); d64_puts("[EVDEV] read which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" n="); d64_num(nn); d64_end();
    }
    g_ev_reads++;
    return (int64_t)bytes;
}

int evdev64_ioctl64(int which, uint64_t cmd, uint64_t arg) {
    const char* name = (which == 1) ? "vimtu-mouse" : "vimtu-keyboard";
    // ★ 基值掩码必须把**整个 size 字段**（bit14..29）掩掉：只掩 bit16..23 会在不同 len 下算出
    //   不同基值，于是 EVIOCGNAME(32) 落到未知分支→ -ENOTTY（实测真陷进去了一次）。
    const uint32_t base = (uint32_t)cmd & 0xC000FFFFu;
    const uint32_t blen = (uint32_t)((cmd >> 16) & 0x3FFFu);
    if (base == (EVDEV64_IOCTL_VER64 & 0xC000FFFFu)) {                               // EVIOCGVERSION
        if (blen < 4u) return -DRM64_EINVAL64;
        if (!user64_range_ok64(arg, 4)) return -DRM64_EFAULT64;
        const uint32_t ver = 0x00010001u;                            // 1.0.1（与 Linux 的 0x010001 同值）
        d64_uwr32(arg, ver);         // 只回 4 字节版本号（Linux 的 EVIOCGVERSION 语义）
    } else if (base == (EVDEV64_IOCTL_ID64 & 0xC000FFFFu)) {                         // EVIOCGNAME(len)
        if (blen == 0u) return -DRM64_EINVAL64;
        if (!user64_range_ok64(arg, blen)) return -DRM64_EFAULT64;
        uint32_t n = 0; while (name[n]) n++;
        n++;                                                         // 含 NUL
        uint32_t w = (n <= blen) ? n : blen;
        d64_uwr64(arg, name, w);
        if (w >= blen) ((char*)(uintptr_t)arg)[blen - 1u] = 0;
        d64_begin(); d64_puts("[EVDEV] ioctl which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" req=0x"); d64_hex(cmd); d64_puts(" name="); d64_puts(name); d64_end();
        return (int)w;
    } else if (base == (EVDEV64_IOCTL_BIT64 & 0xC000FFFFu)) {                        // EVIOCGBIT(0, len)
        if (blen == 0u) return -DRM64_EINVAL64;
        if (!user64_range_ok64(arg, blen)) return -DRM64_EFAULT64;
        uint8_t bits[8];
        for (uint32_t i = 0; i < 8u; i++) bits[i] = 0;
        bits[0] |= (uint8_t)(1u << EV64_SYN);
        bits[0] |= (uint8_t)(1u << EV64_KEY);
        if (which == 1) bits[0] |= (uint8_t)(1u << EV64_REL);
        uint32_t w = (blen < 8u) ? blen : 8u;
        d64_uwr64(arg, bits, w);
        d64_begin(); d64_puts("[EVDEV] ioctl which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" req=0x"); d64_hex(cmd); d64_puts(" bits=0x"); d64_hex((uint64_t)bits[0]); d64_end();
        return (int)w;
    } else {
        d64_begin(); d64_puts("[EVDEV] ioctl which="); d64_num((uint64_t)(which < 0 ? 0 : which));
        d64_puts(" req=0x"); d64_hex(cmd); d64_puts(" ok=0 err="); d64_num(DRM64_ENOTTY64); d64_end();
        return -DRM64_ENOTTY64;
    }
    return 0;
}

// ==================== 启动期：init + 自检 ====================
void drm64_init64(void) {
    if (g_inited) return;
    g_inited = 1;
    for (uint32_t i = 0; i < DRM64_MAX_DUMB64; i++) {
        for (uint32_t k = 0; k < sizeof(Drm64Dumb); k++) ((uint8_t*)&g_dumb[i])[k] = 0;
    }
    g_handle_on_screen = 0;
    g_seq = 0;
    g_presents = 0;
    g_ev_pending_n = 0;
    g_ev_reads = 0;
    // 模式的唯一计算点（几何此后不会变；GETCRTC / GETCONNECTOR 直接拷 g_mode64）
    d64_fill_mode64((uint32_t)fb_phys_width(), (uint32_t)fb_phys_height());
    // 垂直回扫：复用 fb.cpp 的既有探针（200 ms 实测一轮；25..300 Hz 才算可信）
    uint64_t edges = 0, ms = 0;
    g_vblank_hw = fb64_vblank_probe_export64(&edges, &ms) ? 1 : 0;
    d64_begin(); d64_puts("[DRM] vblank mode="); d64_puts(g_vblank_hw ? "hw" : "soft");
    d64_puts(" edges="); d64_num(edges); d64_puts(" ms="); d64_num(ms); d64_end();
    d64_begin(); d64_puts("[DRM] init crtc="); d64_num(DRM64_MAX_CRTC64);
    d64_puts(" connectors=1 fbs="); d64_num(DRM64_MAX_DUMB64);
    d64_puts(" dumb_max="); d64_num(DRM64_MAX_DUMB64);
    d64_puts(" vblank="); d64_puts(g_vblank_hw ? "hw" : "soft"); d64_end();
}

// 固定色表（自检图案；与 user/apps/drmdemo.c 的同一张表逐值一致 —— 便于人工对屏）
static const uint32_t D64_BARS[8] = {
    0xFFFFFFFFu, 0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu,
    0xFFFFFF00u, 0xFFFF00FFu, 0xFF00FFFFu, 0xFF202020u,
};

// 往 dumb buffer 里画 8 条竖直彩条（条宽 = w/8）。返回 1 = 写成功。
// 按**条**填（每像素一次除法没必要；条数固定 8，边界直接算）。
static int d64_paint_bars64(Drm64Dumb* d) {
    uint32_t barw = d->w / 8u;
    if (barw == 0u) barw = 1u;
    if (d->stride > (uint32_t)sizeof(g_rowbuf)) return 0;
    uint32_t* row = (uint32_t*)g_rowbuf;
    for (uint32_t b = 0; b < 8u; b++) {
        uint32_t x0 = b * barw;
        uint32_t x1 = x0 + barw;
        if (x1 > d->w) x1 = d->w;
        for (uint32_t x = x0; x < x1; x++) row[x] = D64_BARS[b];
    }
    for (uint32_t y = 0; y < d->h; y++)
        d64_copy64(d, (uint64_t)y * (uint64_t)d->stride, g_rowbuf, d->w * 4u, 1);
    return 1;
}

// 自检里的"内容核对"：从 dumb buffer 读回每条彩条中心的像素，必须等于色表 ——
// 这条是"ring3 写的字节真的落在这块缓冲上"的内核侧证据（ring3 那条由 DRMDEMO 打）。
__attribute__((noinline)) static int d64_verify_bars64(const Drm64Dumb* d) {
    uint32_t barw = d->w / 8u;
    if (barw == 0u) return 0;
    const uint32_t y = d->h / 2u;
    uint32_t* row = (uint32_t*)g_rowbuf;
    if (d->stride > (uint32_t)sizeof(g_rowbuf)) return 0;
    d64_copy64(d, (uint64_t)y * (uint64_t)d->stride, g_rowbuf, d->w * 4u, 0);
    int bad = 0;
    for (uint32_t b = 0; b < 8u; b++) {
        uint32_t x = b * barw + barw / 2u;
        if (x >= d->w) x = d->w - 1u;
        if (row[x] != D64_BARS[b]) bad++;
    }
    return bad == 0 ? 1 : 0;
}

int drm64_selftest64(void) {
    if (!g_inited) drm64_init64();
    int bw = 0, bh = 0;
    if (!fb_surface64(&bw, &bh) || bw <= 0 || bh <= 0) {
        d64_begin(); d64_puts("[DRM] selftest FAIL reason=no-framebuffer ok=0"); d64_end();
        return -1;
    }
    // 自检几何：小一点（启动期要快），同时保证能装进 g_snapbuf
    uint32_t w = (uint32_t)bw, h = (uint32_t)bh;
    if (w > D64_SNAP_W64) w = D64_SNAP_W64;
    if (h > D64_SNAP_H64) h = D64_SNAP_H64;
    uint32_t hpat = 0;
    if (d64_create64(w, h, 32u, &hpat) != 0) {
        d64_begin(); d64_puts("[DRM] selftest FAIL reason=dumb-create ok=0"); d64_end();
        return -1;
    }
    const char* reason = "?";
    int ok = 1;
    Drm64Dumb* dp = d64_find64(hpat);
    if (!dp) { ok = 0; reason = "slot"; goto done; }
    if (!d64_paint_bars64(dp)) { ok = 0; reason = "paint"; goto done; }
    {
        const int v = d64_verify_bars64(dp);
        d64_begin(); d64_puts("[DRM] verify bars=8 ok="); d64_num((uint64_t)(v ? 1 : 0)); d64_end();
        if (!v) { ok = 0; reason = "verify"; }
    }
    // 屏幕快照进静态缓冲（.bss）—— 自检跑完要把开机画面**原样翻回来**
    {
        uint32_t* bb = fb_surface64(&bw, &bh);
        if (!bb) { ok = 0; reason = "snapshot-fb"; }
        else for (uint32_t y = 0; y < h; y++) d64_rows64(g_snapbuf + (uint64_t)y * w,
                                                         bb + (uint64_t)y * (uint64_t)bw, w);
    }
    // 翻转 + 等一次 vblank（这就是 ring3 的 PAGE_FLIP 走的同一段代码）
    if (ok) {
        if (d64_present_dumb64(dp) < 0) { ok = 0; reason = "flip"; }
        else {
            const int hw = d64_vblank_wait64();
            d64_begin(); d64_puts("[DRM] wait_vblank seq="); d64_num(g_seq);
            d64_puts(hw ? " mode=hw ticks=" : " mode=soft ticks="); d64_num(g_ticks64); d64_end();
            g_handle_on_screen = dp->id;
        }
    }
    // 还原屏幕（快照写回后备缓冲再提交一次）：自检**不改变**开机画面
    if (ok) {
        uint32_t* bb = fb_surface64(&bw, &bh);
        if (bb) {
            for (uint32_t y = 0; y < h; y++) d64_rows64(bb + (uint64_t)y * (uint64_t)bw,
                                                        g_snapbuf + (uint64_t)y * w, w);
            (void)d64_present_backbuf64((int)w, (int)h);
        }
        g_handle_on_screen = 0;
    }
done:
    if (hpat) (void)d64_destroy64(hpat);
    d64_begin();
    if (ok) {
        d64_puts("[DRM] selftest dumb=ok flip=ok vblank="); d64_num(g_seq);
        d64_puts(g_vblank_hw ? " mode=hw ok=1" : " mode=soft ok=1");
    } else {
        d64_puts("[DRM] selftest FAIL reason="); d64_puts(reason ? reason : "?"); d64_puts(" ok=0");
    }
    d64_end();
    return ok ? 0 : -1;
}

// ==================== 启动期 ring3 验收程序（/bin/drmdemo.elf）====================
// 交付纪律与 /wlclient.elf、/bin/wm.elf 完全相同：**卷里的文件**，内核一个字节都不内嵌。
// 触发条件 = 卷里有 /etc/drm64_probe（与 /etc/wl64_probe、/etc/wm_probe 同一套夹具约定），
// 这样默认启动不多画一个像素、不多建一个进程。
int drm64_demo64(const char* path) {
    static const char DRM64_PROBE_PATH64[] = "/etc/drm64_probe";
    const char* p = (path && path[0] == '/') ? path : "/bin/drmdemo.elf";
    uint32_t t = 0, sz = 0;
    if (vfs64_stat(p, &t, &sz) != 0) {
        d64_begin(); d64_puts("[DRMDEMO] skipped (no elf on vfs) path="); d64_puts(p); d64_end();
        return 0;
    }
    if (vfs64_stat(DRM64_PROBE_PATH64, &t, &sz) != 0) {
        d64_begin(); d64_puts("[DRMDEMO] skipped reason=probe=0 (nothing is drawn on the boot screen) path=");
        d64_puts(p); d64_end();
        return 0;
    }
    const int pid = proc64_create64("drmdemo", 0);
    if (pid < 0) { d64_begin(); d64_puts("[DRMDEMO] skipped (create failed) path="); d64_puts(p); d64_end(); return 0; }
    if (proc64_start_elf64(pid, p) != 0) {
        proc64_destroy64(pid);
        d64_begin(); d64_puts("[DRMDEMO] skipped (start failed) path="); d64_puts(p); d64_end();
        return 0;
    }
    d64_begin(); d64_puts("[DRMDEMO] start pid="); d64_num((uint64_t)pid);
    d64_puts(" path="); d64_puts(p); d64_puts(" pool_free="); d64_num(page_count_free_64()); d64_end();
    // 有界等待（60 s）：演示最多等一次 vblank + 一次输入事件，正常情况下 1~2 s 结束
    const uint64_t t0 = g_ticks64;
    while (proc64_find64(pid) && (g_ticks64 - t0) < (uint64_t)PIT_HZ_64 * 60ull) task_sleep64(2);
    const int alive = proc64_find64(pid) ? 1 : 0;
    if (alive) (void)proc64_kill64(pid, 9);
    if (proc64_find64(pid)) proc64_destroy64(pid);
    d64_begin(); d64_puts("[DRMDEMO] done pid="); d64_num((uint64_t)pid);
    d64_puts(" alive="); d64_num((uint64_t)alive);
    d64_puts(" ticks="); d64_num(g_ticks64 - t0);
    d64_puts(" pool_free="); d64_num(page_count_free_64());
    d64_puts(" presents="); d64_num(g_presents);
    d64_end();
    return alive ? -1 : 0;
}
