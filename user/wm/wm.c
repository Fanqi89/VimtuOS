/* wm.c - ★ B-wm：**Ring 3 合成器**（/bin/wm）
 *
 * ============================ 这是什么 ============================
 * 把"图形/合成"从内核搬到用户态的第一步：本进程就是**合成器**。
 *   * 内核侧只留：fb_map(9) 映射后备缓冲、fb_flip(10) 提交矩形、input_poll(12) 收输入、
 *     shm_* 与 wl 的 surface 表/提交队列、以及 22..26 号（合成器专属：注册 / 拉提交 / 映射缓冲 /
 *     确认 / 投递事件）。**内核一个像素都不合成**（有合成器时 [WL64] composite 不再出现）。
 *   * 本进程做：fb_map 拿后备缓冲 -> 从内核拉 surface 表（z 序按 id 升序）-> 把各客户端的 shm
 *     缓冲映射进自己地址空间 -> **在用户态 blit/遮挡/alpha 混合**（只重画 damage 矩形）->
 *     fb_flip 上屏 -> 把输入事件命中测试后投回客户端（wl_seat_post）。
 *
 * ============================ 地盘与共存（本批关键约束）============================
 * 内核外壳（gui64/panels64/explorer64 那套 ring0 桌面）**继续是屏幕的主人**；本合成器只在
 * 内核通过 22 号租给它的"**外部面板区**"（默认 320x200，屏幕右下角、避开 Dock）里画：
 *   * 只 fb_flip 自己那一块（矩形提交）——外部面板区之外的像素一个字节都不动；
 *   * 空闲超过 RESYNC_MS 或 surface 出现/消失 -> 整区重画一次（resync）：因为内核外壳可能
 *     在别的时机画进过这块地（窗口拖过来、光标扫过），重画一次就把这块地"收回来"；
 *   * 退出时把这块地**交回**：用区域外采样到的桌面底色回填 + 提交（[WM] exit handback），
 *     所以合成器退出后屏幕上不会留一块花屏（验收 ⑦ 钉的就是这条）。
 *
 * ============================ 性能与设备阈值 ============================
 * 每帧用 rdtsc 量合成耗时（tsc 用 ticks() 标定成微秒）；对"够大的矩形"才值得走设备提交路径：
 *   阈值 >= 64x64 = 4096 像素（virtio-gpu 2D 每次 TRANSFER_TO_HOST_2D + RESOURCE_FLUSH 有固定
 *   开销，小矩形不值）。**本构建的测试台都没有 virtio-gpu 设备**（22 号返回的 gpu 位 = 0），
 *   所以实际全走 CPU/soft-lfb 路径 —— 打点里如实写 path=soft，绝不假装走了设备。
 *
 * 打点（tests/wm64_test.py 按这些串 grep；格式勿改）：
 *   [WM] probe full=<0|1>
 *   [WM] map pid=<n> va=0x<hex> w=<n> h=<n> pitch=<n> bytes=<n> u=1
 *   [WM] composer seat=<n> gpu=<0|1> region=<x>,<y>,<w>,<h>
 *   [WM] calib tsc_per_ms=<n> ms=<n>
 *   [WM] surf id=<n> pid=<n> rect=<x>,<y>,<w>,<h> pix=<n> flags=0x<hex>            （每条 surface 一次）
 *   [WM] surfmap id=<n> shm=<n> va=0x<hex> bytes=<n>
 *   [WM] frame n=<n> surfs=<n> pending=<n> dirty=<x>,<y>,<w>,<h> px=<n> blend=<n> us=<n> flip_us=<n> path=<soft|dev> resync=<0|1>
 *   [WM] ack id=<n> dmg=<x>,<y>,<w>,<h>
 *   [WM] route surf=<n> pid=<n> type=<n> x=<n> y=<n> sx=<n> sy=<n>                （用户态命中测试 + 投递）
 *   [WM] route none type=<n> x=<n> y=<n>
 *   [WM] blitpolicy gpu=<0|1> min_px=4096 rect_px=<n> path=<dev|soft>
 *   [WM] exit handback color=0x<hex> px=<n> frames=<n>
 * 退出：full（卷里有 /etc/wm_probe）= 见过 surface 且它们全没了（客户端退出）或 45 s 上限；
 *       short（没有探针）= 3 帧后退出（~1 s，别的验收脚本几乎不受影响）。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"
#include "fb.h"
#include "wmabi.h"

#define WM_MAX_SURF   8
#define WM_FRAME_MS   30u          /* 空闲轮询间隔（有活就连续跑，没活就睡一下） */
#define WM_PROBE      "/etc/wm_probe"   /* 与 kernel/wl64.cpp 的 WL64_WM_PROBE64 同一个文件 */
#define WM_DEV_MIN_PX 4096u        /* 设备提交路径的阈值：64x64（见文件头说明） */
#define WM_RESYNC_MS  1000u        /* 空闲超过这么久 -> 整区重画一次（把地盘收回来） */
#define WM_FULL_MS    45000u       /* full 模式的最长运行时间（内核侧有界等待 90 s） */
#define WM_SHORT_MS   1200u        /* short 模式跑这么久就退（3 帧左右） */
#define WM_EV_MAX     32u

/* 外部面板区的"chrome"（合成器自己的底色，与内核面板 chrome 相近但不同，便于区分） */
#define WM_BG     0x00161A20u      /* 面板底 */
#define WM_EDGE   0x005A6472u      /* 边框 */
#define WM_BAR    0x00232A34u      /* 顶部条 */
#define WM_EDGE_PX 2
#define WM_BAR_H   22

struct WmSlot {
    int used;
    struct Wm64SurfaceInfo info;
    unsigned char*         px;      /* 该 surface 缓冲的像素基址（= va + shm_off） */
    int                    map_shm; /* 已映射的 shm 对象 id（-1 = 还没映射） */
    int                    logged;  /* 本条 surface 的 [WM] surf 行是否已打 */
};

static struct Fb64Info g_fb;
static uint32_t*       g_fbpx;
static struct Wm64Rect g_rect;                    /* 22 号写回：内核租给我的"外部面板区" */
static int g_rx, g_ry, g_rw, g_rh;                 /* 外部面板区（内核租给我们的） */
static unsigned long long g_tsc_per_ms = 0;
static unsigned long long g_route_logged = 0;
/* 影副本（"地盘"的影子）：只用**每像素最低字节**记一份 320x200 的快照 —— 用来发现"不是我们画的"
 * 像素变化（内核外壳的窗口/光标扫过这块地）。为什么这样做：合成器必须能判断"我的地盘有没有被
 * 别人动过"，否则只能无条件整区重画（那样"只重画 damage"就成了空话）。1 字节/像素 = 64 KiB，
 * 这块内存**走 shm_create/shm_map 要**（用户程序的镜像只有 4GiB..4GiB+64KiB 的装载区，
 * 塞不下一块 64 KiB 的 .bss）；撞色概率 1/256，漏检只影响"什么时候把地盘收回来"，不影响一帧的
 * 正确性 —— 这一条如实写在这里。 */
#define WM_SHADOW_BYTES (64u * 1024u)
static unsigned char* g_shadow;                  /* shm 对象映射进来的影副本 */
static int g_shadow_w = 0, g_shadow_h = 0;       /* 影副本对应的区域尺寸（区域变了就重记） */
static unsigned long long g_foreign_events = 0;  /* 累计"发现外力改动"的次数（诊断） */
static unsigned long long wm_us(unsigned long long tsc) {
    if (!g_tsc_per_ms) return 0;
    return tsc * 1000ULL / g_tsc_per_ms;
}

/* 探针：/etc/wm_probe 存在 = full 模式。走 Linux 兼容路径的 access(21)（与 user/apps/wlclient.c
 * 同一条路子；自有 ABI 的 open(6) 只支持单层路径，这个探针在 /etc 下）。 */
static int wm_probe_exists(void) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)WM_PROBE, 0, 0, 0, 0) == 0 ? 1 : 0;
}

static int wm_min(int a, int b) { return a < b ? a : b; }
static int wm_max(int a, int b) { return a > b ? a : b; }

/* 半开区间矩形并集（空矩形 = w/h <= 0） */
static void wm_union(int* x, int* y, int* w, int* h, int x2, int y2, int w2, int h2) {
    if (*w <= 0 || *h <= 0) { *x = x2; *y = y2; *w = w2; *h = h2; return; }
    if (w2 <= 0 || h2 <= 0) return;
    const int x0 = wm_min(*x, x2), y0 = wm_min(*y, y2);
    const int x1 = wm_max(*x + *w, x2 + w2), y1 = wm_max(*y + *h, y2 + h2);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

/* 画一格：out = 当前 chrome 色（bg/edge/bar 由区域内位置决定） */
static uint32_t wm_chrome_px(int sx, int sy) {
    const int lx = sx - g_rx, ly = sy - g_ry;
    if (lx < WM_EDGE_PX || lx >= g_rw - WM_EDGE_PX || ly < WM_EDGE_PX || ly >= g_rh - WM_EDGE_PX) return WM_EDGE;
    if (ly < WM_BAR_H) return WM_BAR;
    return WM_BG;
}

/* 影副本的单像素写入（定义在下面；blit / chrome 都要用，所以先声明）*/
static inline void wm_shadow_set(int sx, int sy, uint32_t v);

/* 一次 blit：把 surface 矩形 ∩ dirty ∩ 区域 的像素写进后备缓冲。
 *   * z 序 = id 升序：调用方**从下到上**调用，被更高层 surface 盖住的像素跳过（painter's algorithm）；
 *   * alpha：像素 A 通道 = 0xFF 直接覆盖；否则按 A 与底色混合（客户端的 XRGB 缓冲写 0xFF 就是纯拷贝）；
 *   * 返回写出的像素数；*blended 累加混合像素数。 */
static int wm_blit_slot(const struct WmSlot* self, const struct WmSlot* all, int dirty_x, int dirty_y,
                        int dirty_w, int dirty_h, int* blended) {
    if (!self->px || !(self->info.flags & V64_WM_HASBUF)) return 0;
    const struct Wm64SurfaceInfo* si = &self->info;
    int x0 = wm_max(si->x, dirty_x), y0 = wm_max(si->y, dirty_y);
    int x1 = wm_min(si->x + (int)si->w, dirty_x + dirty_w);
    int y1 = wm_min(si->y + (int)si->h, dirty_y + dirty_h);
    x0 = wm_max(x0, g_rx); y0 = wm_max(y0, g_ry);
    x1 = wm_min(x1, g_rx + g_rw); y1 = wm_min(y1, g_ry + g_rh);
    x1 = wm_min(x1, (int)g_fb.width); y1 = wm_min(y1, (int)g_fb.height);
    if (x1 <= x0 || y1 <= y0) return 0;
    const unsigned pitch_px = si->w;
    int written = 0;
    for (int sy = y0; sy < y1; sy++) {
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        for (int sx = x0; sx < x1; sx++) {
            /* 被更高层的 surface 盖住？-> 跳过（上层自己会画） */
            int covered = 0;
            for (int k = 0; k < WM_MAX_SURF; k++) {
                const struct WmSlot* h = &all[k];
                if (h == self || !h->used || !(h->info.flags & V64_WM_CONTENT)) continue;
                if (h->info.id <= si->id) continue;
                if (sx >= h->info.x && sx < h->info.x + (int)h->info.w &&
                    sy >= h->info.y && sy < h->info.y + (int)h->info.h) { covered = 1; break; }
            }
            if (covered) continue;
            const unsigned char* srow = self->px + (unsigned long)(sy - si->y) * (unsigned long)pitch_px * 4u;
            const uint32_t v = *(const uint32_t*)(srow + (unsigned long)(sx - si->x) * 4u);
            const uint32_t a = (v >> 24) & 0xFFu;
            if (a == 0xFFu) {
                drow[sx] = v;
                wm_shadow_set(sx, sy, v);
            } else if (a == 0) {
                wm_shadow_set(sx, sy, drow[sx]);     /* 全透明：不改像素，并把现状认作"我们的" */
            } else {
                const uint32_t d = drow[sx];
                const uint32_t ia = 255u - a;
                const uint32_t r = (((v >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * ia) / 255u;
                const uint32_t g = (((v >> 8) & 0xFFu) * a + ((d >> 8) & 0xFFu) * ia) / 255u;
                const uint32_t b = ((v & 0xFFu) * a + (d & 0xFFu) * ia) / 255u;
                const uint32_t out = 0xFF000000u | (r << 16) | (g << 8) | b;
                drow[sx] = out;
                wm_shadow_set(sx, sy, out);
                (*blended)++;
            }
            written++;
        }
    }
    return written;
}

/* 用户态命中测试：最上层的、有内容的、包含 (x,y) 的 surface（与内核 hit64 同一条规则） */
static struct WmSlot* wm_hit(struct WmSlot* slots, int x, int y) {
    struct WmSlot* best = NULL;
    for (int k = 0; k < WM_MAX_SURF; k++) {
        struct WmSlot* s = &slots[k];
        if (!s->used || !(s->info.flags & V64_WM_CONTENT)) continue;
        if (x < s->info.x || x >= s->info.x + (int)s->info.w) continue;
        if (y < s->info.y || y >= s->info.y + (int)s->info.h) continue;
        if (!best || s->info.id > best->info.id) best = s;
    }
    return best;
}

/* 影副本：把区域当前像素记成 1 字节/像素 */
/* 影副本的写入：**我们画的每个像素**都记进影副本（不是"画完扫一遍后备缓冲"）。
 * 为什么必须是"记我们画的"而不是"扫后备缓冲"：内核外壳会在我们画的**间歇**里把光标轨迹/
 * 窗口底色画进这块地（本批的共存现实），"扫一遍"会把别人的像素当成自己的，之后再也检测不出来。
 * 记法：每像素 1 字节（最低字节）—— 够用来发现"不是我们画的"变化（撞色概率 1/256）。 */
static inline void wm_shadow_set(int sx, int sy, uint32_t v) {
    if (!g_shadow || sx < g_rx || sy < g_ry || sx >= g_rx + g_rw || sy >= g_ry + g_rh) return;
    const unsigned idx = (unsigned)(sy - g_ry) * (unsigned)g_rw + (unsigned)(sx - g_rx);
    if (idx < WM_SHADOW_BYTES) g_shadow[idx] = (unsigned char)(v & 0xFFu);
}
/* 影副本的"维度"登记（在整区重画之前调用一次，让 foreign 检查知道当前区域的尺寸）*/
static void wm_shadow_begin(void) { g_shadow_w = g_rw; g_shadow_h = g_rh; }
/* 影副本比对：返回"不是我们画的"像素的包围盒（0 = 没有外力改动）。
 * 每 WM_FRAME_MS 空转一次就扫一遍（320x200 = 64000 次比较，几十微秒）。 */
static int wm_shadow_foreign(int* fx, int* fy, int* fw, int* fh) {
    if (g_shadow_w != g_rw || g_shadow_h != g_rh) return 0;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30), n = 0;
    unsigned idx = 0;
    for (int y = 0; y < g_rh; y++) {
        const uint32_t* row = (const uint32_t*)((const unsigned char*)g_fbpx + (unsigned long)(g_ry + y) * g_fb.pitch);
        for (int x = 0; x < g_rw; x++, idx++) {
            if (idx >= WM_SHADOW_BYTES) return n ? (void)(*fx = g_rx + x0, *fy = g_ry + y0, *fw = x1 - x0, *fh = y1 - y0), n : 0;
            if ((unsigned char)(row[g_rx + x] & 0xFFu) == g_shadow[idx]) continue;
            n++;
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + 1 > x1) x1 = x + 1;
            if (y + 1 > y1) y1 = y + 1;
        }
    }
    if (!n) return 0;
    *fx = g_rx + x0; *fy = g_ry + y0; *fw = x1 - x0; *fh = y1 - y0;
    return n;
}

int main(void) {
    const int full = wm_probe_exists();
    printf("[WM] probe full=%d\n", full);

    /* ---- ① fb_map：后备缓冲（内核映射同一批物理页，写它就是写显存后备缓冲）---- */
    int rc = fb_map(&g_fbpx, &g_fb);
    if (rc != 0) { printf("[WM] FAILED fb_map rc=%d\n", rc); return 1; }
    printf("[WM] map pid=%d va=0x%x w=%d h=%d pitch=%d bytes=%d u=1\n",
           getpid(), (unsigned)(unsigned long)g_fb.va, (int)g_fb.width, (int)g_fb.height,
           (int)g_fb.pitch, (int)g_fb.size);

    /* ---- ② 注册为合成器 + 拿"外部面板区" ---- */
    int seat_gpu = wl_composer_get(&g_rect);
    if (seat_gpu < 0) { printf("[WM] FAILED composer rc=%d\n", seat_gpu); return 1; }
    const int gpu = (seat_gpu & (int)V64_WM_GPU_BIT) ? 1 : 0;
    const int seat = seat_gpu & (int)V64_WM_SEAT_MASK;
    g_rx = g_rect.x; g_ry = g_rect.y; g_rw = g_rect.w; g_rh = g_rect.h;
    printf("[WM] composer seat=%d gpu=%d region=%d,%d,%d,%d\n", seat, gpu, g_rx, g_ry, g_rw, g_rh);
    printf("[WM] blitpolicy gpu=%d min_px=%d\n", gpu, (int)WM_DEV_MIN_PX);

    /* ---- ③ rdtsc -> us 标定（用 ticks()=250Hz 的 4ms 粒度量 200ms）---- */
    {
        const unsigned long long t0 = wm_rdtsc();
        const unsigned long k0 = vimtu64_ticks();
        vimtu64_sleep_ms(200);
        const unsigned long k1 = vimtu64_ticks();
        const unsigned long long t1 = wm_rdtsc();
        const unsigned long ms = (k1 - k0) * 4ul;
        g_tsc_per_ms = (ms > 0 && t1 > t0) ? (t1 - t0) / (unsigned long long)ms : 0;
        printf("[WM] calib tsc_per_ms=%lu ms=%lu\n", (unsigned long)g_tsc_per_ms, ms);
    }

    static struct WmSlot slots[WM_MAX_SURF];
    for (int i = 0; i < WM_MAX_SURF; i++) { slots[i].map_shm = -1; }
    /* 注意：evs 不再需要（事件走 poll_event 的局部数组） */
    unsigned long n_frame = 0, seen = 0, handback_px = 0;   /* handback_px 在退出回填那段用 */
    /* last_active 已不再需要：外力改动靠影副本比对（见 wm_shadow_foreign），不靠时间阈值 */
    /* 注意单位：ticks 是 PIT 250Hz -> ms 要乘 4（这里曾经漏乘，导致 elapsed 虚高、wm 提前收工） */
    const int t0_ms = (int)vimtu64_ticks() * 4;

    /* 影副本内存：走 shm_create + shm_map（不占镜像的 .bss —— 装载区只有 64 KiB） */
    {
        const int shm = shm_create(WM_SHADOW_BYTES);
        void* va = NULL;
        if (shm < 0 || shm_map(shm, 0, WM_SHADOW_BYTES, &va) != 0 || !va) {
            printf("[WM] FAILED shadow shm rc=%d\n", shm);
            return 1;
        }
        g_shadow = (unsigned char*)va;
        printf("[WM] shadow shm=%d va=0x%x bytes=%d\n", shm, (unsigned)(unsigned long)va, (int)WM_SHADOW_BYTES);
    }
    for (;;) {
        const int now_ms = (int)vimtu64_ticks() * 4;     /* 4ms 粒度的时间（PIT 250Hz） */
        const int elapsed = now_ms - t0_ms;

        int n_used = 0, n_pending = 0, layout = 0;
        int dmg_x = 0, dmg_y = 0, dmg_w = 0, dmg_h = 0;

        /* ---- ④ 拉 surface 表（23 号）：谁有缓冲、谁提交了、damage 多大 ---- */
        for (int i = 0; i < WM_MAX_SURF; i++) {
            struct Wm64SurfaceInfo info;
            memset(&info, 0, sizeof(info));
            const int erc = wl_surface_export(i, &info);
            if (erc == 1) {                                   /* 这个槽空了 */
                if (slots[i].used) { layout = 1; slots[i].used = 0; slots[i].px = NULL; slots[i].map_shm = -1; }
                continue;
            }
            if (erc != 0) continue;                           /* 负错误码：这一轮跳过（不猜） */
            if (!slots[i].used || slots[i].info.id != info.id) layout = 1;
            slots[i].used = 1;
            slots[i].info = info;
            n_used++;
            if (slots[i].map_shm != info.shm_id && (info.flags & V64_WM_HASBUF)) {
                struct Wm64SurfaceMap map;
                memset(&map, 0, sizeof(map));
                const int mrc = wl_surface_map(info.id, &map);
                if (mrc == 0) {
                    slots[i].px = (unsigned char*)(unsigned long)map.va + info.shm_off;
                    slots[i].map_shm = info.shm_id;
                    printf("[WM] surfmap id=%d shm=%d va=0x%x bytes=%d\n",
                           info.id, info.shm_id, (unsigned)(unsigned long)map.va, (int)map.bytes);
                } else {
                    printf("[WM] surfmap FAILED id=%d rc=%d\n", info.id, mrc);
                }
            }
            if (!slots[i].logged) {                           /* 第一条 surface：留证据（便于对账） */
                slots[i].logged = 1;
                printf("[WM] surf id=%d pid=%d rect=%d,%d,%d,%d pix=%d flags=0x%x\n",
                       info.id, info.pid, info.x, info.y, (int)info.w, (int)info.h,
                       (int)(info.w * info.h), (unsigned)info.flags);
            }
            if (info.flags & V64_WM_PENDING) {
                n_pending++;
                /* 这条的 dirty = damage 矩形（局部坐标 -> 屏幕坐标），与区域求交（越界留给合成时裁） */
                int rx = info.x + info.dmg_x, ry = info.y + info.dmg_y;
                int rw = info.dmg_w, rh = info.dmg_h;
                if (rw > (int)info.w - info.dmg_x) rw = (int)info.w - info.dmg_x;   /* 越界夹到面内 */
                if (rh > (int)info.h - info.dmg_y) rh = (int)info.h - info.dmg_y;
                if (rw > 0 && rh > 0) wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h, rx, ry, rw, rh);
            }
        }
        if (n_used) seen = 1;

        /* ---- ⑤ 空闲判定：没有活就不画（一个像素都不动）；顺便查"我的地盘有没有被别人动过" ----
         * resync（整区重画）只在**结构性变化**时做：surface 出现/消失（layout）、第一帧；
         * "外来的像素改动"（内核外壳的窗口/光标扫过这块地）用影副本按**差异包围盒**局部补画，
         * 不做整区重画 —— 这样"只重画 damage"这句话在验收里才是可核对的（③）。 */
        const int idle = (n_pending == 0 && !layout);
        int resync = 0, foreign = 0, full_region = 0;
        if (idle) {
            if (!seen || !g_shadow_w) {          /* 还没画过 / 影副本还没建：整区来一次 */
                resync = 1; full_region = 1;
            } else {
                int fx = 0, fy = 0, fw = 0, fh = 0;
                foreign = wm_shadow_foreign(&fx, &fy, &fw, &fh);
                if (foreign > 0) {               /* 别人动过 -> 只补画那一个包围盒 */
                    dmg_x = fx; dmg_y = fy; dmg_w = fw; dmg_h = fh;
                    g_foreign_events++;
                    resync = 1;
                } else {
                    if (n_used == 0 && seen && full) {
                        printf("[WM] exit reason=no-surfaces n_used=0 seen=1 elapsed_ms=%d\n", elapsed);
                        break;                                     /* 客户端全退了 -> full 模式收工 */
                    }
                    if (!full && elapsed >= (int)WM_SHORT_MS) {
                        printf("[WM] exit reason=short-mode elapsed_ms=%d\n", elapsed);
                        break;
                    }
                    if (full && elapsed >= (int)WM_FULL_MS) {
                        printf("[WM] exit reason=timeout elapsed_ms=%d limit_ms=%d\n", elapsed, (int)WM_FULL_MS);
                        break;
                    }
                    vimtu64_sleep_ms(WM_FRAME_MS);
                    /* 事件照收（空闲时也要把输入投出去） */
                    goto input;
                }
            }
        }
        if (layout) full_region = 1;
        if (full_region) {
            dmg_x = g_rx; dmg_y = g_ry; dmg_w = g_rw; dmg_h = g_rh;
        }
        /* 裁剪：区域 ∩ 屏幕 */
        {
            int x0 = wm_max(dmg_x, g_rx), y0 = wm_max(dmg_y, g_ry);
            int x1 = wm_min(dmg_x + dmg_w, g_rx + g_rw), y1 = wm_min(dmg_y + dmg_h, g_ry + g_rh);
            x0 = wm_max(x0, 0); y0 = wm_max(y0, 0);
            x1 = wm_min(x1, (int)g_fb.width); y1 = wm_min(y1, (int)g_fb.height);
            dmg_x = x0; dmg_y = y0; dmg_w = x1 - x0; dmg_h = y1 - y0;
        }
        if (dmg_w <= 0 || dmg_h <= 0) goto input;

        /* ---- ⑥ 用户态合成：chrome -> 从下到上 blit（遮挡/alpha），只碰 dirty 矩形 ---- */
        const unsigned long long t_c0 = wm_rdtsc();
        int px = 0, blend = 0;
        for (int sy = dmg_y; sy < dmg_y + dmg_h; sy++) {
            uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
            for (int sx = dmg_x; sx < dmg_x + dmg_w; sx++) {
                const uint32_t c = wm_chrome_px(sx, sy);
                drow[sx] = c;
                wm_shadow_set(sx, sy, c);
                px++;
            }
        }
        /* id 升序 = 从下到上（z 序）；内核的 surface 表里 id 越大越在上层 */
        {
            /* 直接按 id 升序排序后逐条画（最多 8 条，冒泡即可） */
            int order[WM_MAX_SURF], n = 0;
            for (int i = 0; i < WM_MAX_SURF; i++) if (slots[i].used) order[n++] = i;
            for (int a = 0; a < n; a++)
                for (int b = a + 1; b < n; b++)
                    if (slots[order[b]].info.id < slots[order[a]].info.id) {
                        const int t = order[a]; order[a] = order[b]; order[b] = t;
                    }
            for (int a = 0; a < n; a++) px += wm_blit_slot(&slots[order[a]], slots, dmg_x, dmg_y, dmg_w, dmg_h, &blend);
        }
        const unsigned long long t_c1 = wm_rdtsc();
        const unsigned long long us = wm_us(t_c1 - t_c0);

        /* ---- ⑦ 上屏（只提交这一块）+ 设备阈值决策 ---- */
        const int use_dev = (gpu && px >= (int)WM_DEV_MIN_PX) ? 1 : 0;
        const unsigned long long t_f0 = wm_rdtsc();
        const int frc = fb_flip(dmg_x, dmg_y, dmg_w, dmg_h);
        const unsigned long long flip_us = wm_us(wm_rdtsc() - t_f0);
        n_frame++;
        printf("[WM] frame n=%lu surfs=%d pending=%d dirty=%d,%d,%d,%d px=%d blend=%d us=%lu flip_us=%lu path=%s full=%d foreign=%d\n",
               n_frame, n_used, n_pending, dmg_x, dmg_y, dmg_w, dmg_h, px, blend, (unsigned long)us,
               (unsigned long)flip_us, use_dev ? "dev" : "soft", full_region ? 1 : 0, foreign);
        if (frc != 0) printf("[WM] frame flip rc=%d\n", frc);
        if (n_frame == 1 || resync)
            printf("[WM] blitpolicy gpu=%d min_px=%d rect_px=%d path=%s\n",
                   gpu, (int)WM_DEV_MIN_PX, px, use_dev ? "dev" : "soft");

        /* ---- ⑧ 确认：告诉内核"这条我合成完了"（清 pending/damage + content=1）---- */
        for (int i = 0; i < WM_MAX_SURF; i++) {
            if (!slots[i].used) continue;
            if (!(slots[i].info.flags & V64_WM_PENDING)) continue;
            if (wl_surface_ack(slots[i].info.id) == 0)
                printf("[WM] ack id=%d dmg=%d,%d,%d,%d\n", slots[i].info.id,
                       slots[i].info.dmg_x, slots[i].info.dmg_y, slots[i].info.dmg_w, slots[i].info.dmg_h);
            slots[i].info.flags &= ~(unsigned)V64_WM_PENDING;
            slots[i].info.flags |= (unsigned)V64_WM_CONTENT;
        }
        /* （这里原来记 last_active：现在外力改动靠影副本比对，不再需要） */
        wm_shadow_begin();         /* 登记影副本的尺寸（first frame / 整区重画都走这里）*/

input:
        /* ---- ⑨ 输入：input_poll 收事件 -> 用户态命中测试 -> wl_seat_post 投回客户端 ---- */
        {
            struct Ev64Event raw[WM_EV_MAX];
            const int got = poll_event(raw, WM_EV_MAX, 0);
            if (got > 0) {
                for (int i = 0; i < got; i++) {
                    struct Wl64SeatEvent se;
                    memset(&se, 0, sizeof(se));
                    se.ev = raw[i];
                    se.seat = (unsigned)seat;
                    se.surf = 0; se.sx = -1; se.sy = -1; se.inside = 0; se.pad = 0;
                    /* 路由打点：MOUSE_DOWN/UP 与键盘事件**一定**打（关键证据）；
                     * MOVE / 没命中 的行按预算节流（鼠标一路扫过会刷屏）。 */
                    const int key_ev = (raw[i].type == V64_EV_MOUSE_DOWN || raw[i].type == V64_EV_MOUSE_UP ||
                                        raw[i].type == V64_EV_KEY_DOWN || raw[i].type == V64_EV_KEY_UP);
                    const int is_key = (raw[i].type == V64_EV_KEY_DOWN || raw[i].type == V64_EV_KEY_UP);
                    if (is_key) {
                        /* 键盘：投给"最近一次指针命中的"surface；没有就投给最上层那块（最小策略，如实） */
                        struct WmSlot* best = NULL;
                        for (int k = 0; k < WM_MAX_SURF; k++)
                            if (slots[k].used && (slots[k].info.flags & V64_WM_CONTENT))
                                if (!best || slots[k].info.id > best->info.id) best = &slots[k];
                        if (best) { se.surf = best->info.id; se.inside = 1; }
                    } else {
                        struct WmSlot* hs = wm_hit(slots, raw[i].x, raw[i].y);
                        if (hs) {
                            se.surf = hs->info.id;
                            se.inside = 1;
                            se.sx = raw[i].x - hs->info.x;
                            se.sy = raw[i].y - hs->info.y;
                        }
                    }
                    if (se.surf) {
                        const int tpid = wl_seat_post(&se);
                        if (key_ev || g_route_logged < 96) {
                            g_route_logged++;
                            printf("[WM] route surf=%d pid=%d type=%d x=%d y=%d sx=%d sy=%d\n",
                                   (int)se.surf, tpid, (int)raw[i].type, (int)raw[i].x, (int)raw[i].y,
                                   (int)se.sx, (int)se.sy);
                        }
                    } else if (key_ev || g_route_logged < 96) {
                        g_route_logged++;
                        printf("[WM] route none type=%d x=%d y=%d\n", (int)raw[i].type, (int)raw[i].x, (int)raw[i].y);
                    }
                }
            }
        }
    }

    /* ---- ⑩ 交回地盘：用区域外的桌面底色回填这块地（不让合成器退出后留花屏）---- */
    {
        uint32_t bg = 0xFF202020u;
        int sxp = g_rx - 6, syp = g_ry + 4;
        if (sxp < 0) { sxp = g_rx + g_rw + 6; syp = g_ry + 4; }
        if (sxp >= 0 && sxp < (int)g_fb.width && syp >= 0 && syp < (int)g_fb.height) {
            const uint32_t* row = (const uint32_t*)((const unsigned char*)g_fbpx + (unsigned long)syp * g_fb.pitch);
            bg = row[sxp] | 0xFF000000u;
        }
        for (int sy = g_ry; sy < g_ry + g_rh && sy < (int)g_fb.height; sy++) {
            if (sy < 0) continue;
            uint32_t* row = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
            for (int sx = g_rx; sx < g_rx + g_rw && sx < (int)g_fb.width; sx++) {
                if (sx < 0) continue;
                row[sx] = bg; handback_px++;
            }
        }
        (void)fb_flip(g_rx, g_ry, g_rw, g_rh);
        printf("[WM] exit handback color=0x%x px=%lu frames=%lu\n", (unsigned)bg, handback_px, n_frame);
    }
    return 0;
}
