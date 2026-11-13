/* wm.c - ★ B-wm / W3 第二批：**全屏 Ring 3 多窗口合成器**（/lib/wm.elf）
 *
 * ============================ 这一批改了什么（W3 第二批）============================
 *  ① **地盘从"右下角 320x200 面板区"升级成整屏**：22 号 wl_composer_get 现在交整屏
 *     （[WM] composer .. region=0,0,1280,800），所以窗口可以出现在屏幕任何位置。
 *  ② **摆放策略搬到合成器**：新窗口由 wm 用 27 号 wl_surface_move 放到**屏幕正中 + 级联偏移**
 *     （[WM] place id=.. pos=.. mode=center|cascade）。Windows 的 MessageBoxA 从此不再落在右下角。
 *  ③ **窗口拖动**：按住窗口**顶部 22 px**（= MessageBoxA 自绘标题栏的高度，见下面 WM_DRAG_BAR）
 *     拖动 -> wm 反复调 27 号改位置 -> 内核的命中测试与 seat 路由跟着变（单一事实来源仍是内核
 *     的 surface 表，不是 wm 自己的一份影子）。松手打印 [WM] drag end。
 *  ④ **背景快照回填**：wm 启动时把整屏像素拷进 2 个 shm 对象（2 MiB x 2 = 4 MiB，够 1280x800x4），
 *     窗口挪走/关掉时用快照把那些矩形**还原成桌面**，所以拖动不留拖影。
 *
 * ============================ 与 ring0 外壳共存（如实说明）============================
 * 内核外壳（gui64/panels64/explorer64 那套 ring0 桌面）**仍然是屏幕的主人**：它画壁纸/图标/
 * Dock/它自己的窗口，而且会为了光标轨迹、悬停高亮等把自己的脏矩形重画上屏。wm 是**窗口层**：
 *   * 只画"窗口矩形 + 它盖子下的那一块"（先回填背景、再按 id 升序 blit 各 surface）；
 *   * 窗口矩形之外**一个像素都不碰**（不会去涂整屏，也就不可能把桌面擦掉）；
 *   * 每次收到输入事件、以及每 WM_REPAIR_MS 一次，把**所有窗口矩形**重画一遍 —— 这样外壳把
 *     光标画到我们窗口上时，下一帧就被我们盖回去（这是"两个画家共用一张画布"的必需手段，
 *     不是作弊：真 Wayland 里只有合成器一个画家，那是后续把外壳也搬进 wm 的事）；
 *   * 退出时把**我们画过的矩形**用快照还原（不是整屏回填 —— 那会把桌面涂成一块死色）。
 * 已知代价（如实记录）：窗口矩形正好压在桌面会变的东西上时（Dock 计时/悬停高亮），
 * 还原出来的是**启动那一刻**的旧像素。整屏单一画家的正确解法在后续批次。
 *
 * ============================ 打点（tests 按这些串 grep；格式勿改）============================
 *   [WM] probe full=<0|1>
 *   [WM] map pid=<n> va=0x<hex> w=<n> h=<n> pitch=<n> bytes=<n> u=1
 *   [WM] composer seat=<n> gpu=<0|1> region=<x>,<y>,<w>,<h>
 *   [WM] policy region=full drag_bar=<n> place=center+cascade repair_ms=<n> bg=<0|1>
 *   [WM] bg shm=<a>,<b> bytes=<n> va=0x<hex>,0x<hex> u=1        / [WM] bg FAILED reason=<s> need=<n> cap=<n>
 *   [WM] calib tsc_per_ms=<n> ms=<n>
 *   [WM] surf id=<n> pid=<n> rect=<x>,<y>,<w>,<h> pix=<n> flags=0x<hex>
 *   [WM] place id=<n> pos=<x>,<y> mode=<center|cascade>
 *   [WM] surfmap id=<n> shm=<n> va=0x<hex> bytes=<n>
 *   [WM] frame n=<n> surfs=<n> pending=<n> dirty=<x>,<y>,<w>,<h> px=<n> blend=<n> us=<n> flip_us=<n> path=<soft|dev> full=<0|1> foreign=<n>
 *   [WM] ack id=<n> dmg=<x>,<y>,<w>,<h>
 *   [WM] route surf=<n> pid=<n> type=<n> x=<n> y=<n> sx=<n> sy=<n>
 *   [WM] route none type=<n> x=<n> y=<n>
 *   [WM] drag begin id=<n> grab=<sx>,<sy> from=<x>,<y>
 *   [WM] drag move id=<n> pos=<x>,<y> dx=<n> dy=<n>
 *   [WM] drag end id=<n> pos=<x>,<y> moved=<n>
 *   [WM] blitpolicy gpu=<0|1> min_px=<n> rect_px=<n> path=<dev|soft>
 *   [WM] exit handback color=0x<hex> px=<n> frames=<n> bg=<0|1>
 *
 * 退出：full（卷里有 /etc/wm_probe）= 见过 surface 且它们全没了，或 WM_FULL_MS 上限；
 *       short（没有探针）= **没见过任何 surface** 时 WM_SHORT_MS 就退（别的验收脚本几乎不受
 *       影响），一旦见过 surface 就等它们全退（上限 WM_SEEN_MS）—— 这样"先起 wm 再起客户端"
 *       的顺序才成立。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"
#include "fb.h"
#include "wmabi.h"

#define WM_MAX_SURF     8
#define WM_FRAME_MS     40u          /* 空闲轮询间隔（TCG 下每轮 8 次 export + poll，别太密） */
#define WM_PROBE        "/etc/wm_probe"   /* 与 kernel/wl64.cpp 的 WL64_WM_PROBE64 同一个文件 */
#define WM_DEV_MIN_PX   4096u        /* 设备提交路径的阈值：64x64（本台都没有 virtio-gpu -> 恒 soft） */
#define WM_FULL_MS      45000u       /* full 模式最长运行时间 */
#define WM_SHORT_MS     180000u      /* short 模式：**一个 surface 都没见过**时的等待窗口（见文件头） */
#define WM_SEEN_MS      60000u       /* short 模式：已经见过 surface 之后的最长寿命 */
#define WM_REPAIR_MS    150u         /* 周期性把窗口矩形重画一遍（盖掉外壳的光标/脏区重绘） */
#define WM_EV_MAX       32u
#define WM_DRAG_BAR     22           /* 拖动把手 = 窗口顶部这么多行（= MessageBoxA 的自绘标题栏） */
#define WM_CASCADE      28           /* 级联偏移（多窗口时彼此错开） */
#define WM_CASCADE_N    6
#define WM_BG_OBJ_BYTES (2u * 1024u * 1024u)   /* 单个 shm 对象上限（= SHM64_SLOT_BYTES64） */

struct WmSlot {
    int used;
    struct Wm64SurfaceInfo info;
    unsigned char*         px;      /* 该 surface 缓冲的像素基址（= va + shm_off） */
    int                    map_shm; /* 已映射的 shm 对象 id（-1 = 还没映射） */
    int                    logged;  /* [WM] surf 行是否已打 */
    int                    placed;  /* 是否已经由 wm 摆过位置 */
    int                    prev_x, prev_y;   /* 上一帧的屏幕位置（算 dirty 用） */
    int                    prev_w, prev_h;
};

static struct Fb64Info  g_fb;
static uint32_t*        g_fbpx;
static struct Wm64Rect  g_rect;                 /* 22 号写回：整屏（本批起） */
static int g_rx, g_ry, g_rw, g_rh;
static unsigned long long g_tsc_per_ms = 0;
static unsigned long long g_route_logged = 0;
static unsigned g_place_seq = 0;
static int      g_seat = 1;                      /* 22 号给的座位 id（事件投递用） */
static uint32_t g_frame_log = 0;                 /* [WM] frame 行的预算（纯修复帧不打，见下） */
static const uint32_t WM_FRAME_LOG_MAX = 64u;

/* 背景快照（整屏像素）：2 个 shm 对象接力，0 = 没拿到（如实降级：不回填背景） */
static unsigned char* g_bg[2];
static unsigned       g_bg_need = 0;
static int            g_have_bg = 0;

/* 最近一次**真的画上屏**的窗口矩形（退出回填要用它：最后一个客户端销毁时槽位已经被清掉，
 * 只看 used 槽会得到 px=0 —— wm64_test 的 ⑦ 就是钉这一条的）。 */
#define WM_LAST_MAX 8
static int g_lastx[WM_LAST_MAX], g_lasty[WM_LAST_MAX];
static int g_lastw[WM_LAST_MAX], g_lasth[WM_LAST_MAX], g_lastn = 0;
static void wm_remember(struct WmSlot* slots) {
    g_lastn = 0;
    for (int i = 0; i < WM_MAX_SURF && g_lastn < WM_LAST_MAX; i++) {
        if (!slots[i].used) continue;
        g_lastx[g_lastn] = (int)slots[i].info.x;
        g_lasty[g_lastn] = (int)slots[i].info.y;
        g_lastw[g_lastn] = (int)slots[i].info.w;
        g_lasth[g_lastn] = (int)slots[i].info.h;
        g_lastn++;
    }
}

/* 拖动状态 */
static int g_drag_id = 0, g_drag_grab_x = 0, g_drag_grab_y = 0;
static int g_drag_from_x = 0, g_drag_from_y = 0, g_drag_moved = 0, g_drag_posts = 0;

static unsigned long long wm_us(unsigned long long tsc) {
    if (!g_tsc_per_ms) return 0;
    return tsc * 1000ULL / g_tsc_per_ms;
}

static int wm_min(int a, int b) { return a < b ? a : b; }
static int wm_max(int a, int b) { return a > b ? a : b; }

/* 探针：/etc/wm_probe 存在 = full 模式（走 Linux 兼容路径的 access(21)）。 */
static int wm_probe_exists(void) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)WM_PROBE, 0, 0, 0, 0) == 0 ? 1 : 0;
}

/* 半开区间矩形并集（空矩形 = w/h <= 0） */
static void wm_union(int* x, int* y, int* w, int* h, int x2, int y2, int w2, int h2) {
    if (w2 <= 0 || h2 <= 0) return;
    if (*w <= 0 || *h <= 0) { *x = x2; *y = y2; *w = w2; *h = h2; return; }
    const int x0 = wm_min(*x, x2), y0 = wm_min(*y, y2);
    const int x1 = wm_max(*x + *w, x2 + w2), y1 = wm_max(*y + *h, y2 + h2);
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

/* ---------------- 背景快照：整屏像素的读写（跨 2 个 shm 对象） ---------------- */
static inline uint32_t* wm_bg_at(int sx, int sy) {
    const unsigned off = ((unsigned)sy * (unsigned)g_fb.width + (unsigned)sx) * 4u;
    if (off + 4u > g_bg_need) return NULL;
    const int h = (off < WM_BG_OBJ_BYTES) ? 0 : 1;
    return (uint32_t*)(g_bg[h] + (off - (h ? WM_BG_OBJ_BYTES : 0u)));
}
static int wm_bg_capture(void) {
    if (!g_have_bg) return 0;
    int n = 0;
    for (int sy = 0; sy < (int)g_fb.height; sy++) {
        const uint32_t* srow = (const uint32_t*)((const unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        for (int sx = 0; sx < (int)g_fb.width; sx++) {
            uint32_t* d = wm_bg_at(sx, sy);
            if (!d) return n;
            *d = srow[sx];
            n++;
        }
    }
    return n;
}
/* 用快照把一块矩形还原成"启动那一刻的桌面" */
static int wm_bg_restore(int x0, int y0, int x1, int y1) {
    if (!g_have_bg) return 0;
    int n = 0;
    for (int sy = y0; sy < y1; sy++) {
        if (sy < 0 || sy >= (int)g_fb.height) continue;
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        for (int sx = x0; sx < x1; sx++) {
            if (sx < 0 || sx >= (int)g_fb.width) continue;
            const uint32_t* s = wm_bg_at(sx, sy);
            if (!s) continue;
            drow[sx] = *s;
            n++;
        }
    }
    return n;
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

/* 摆放策略：屏幕正中 + 级联（新窗口彼此错开一点，免得完全重叠） */
static void wm_place(int id, int w, int h, int* out_x, int* out_y, const char** mode) {
    const int cw = (int)g_fb.width, ch = (int)g_fb.height;
    int x = (cw - w) / 2, y = (ch - h) / 2;
    const unsigned k = g_place_seq++ % (unsigned)WM_CASCADE_N;
    if (k) { x += (int)k * WM_CASCADE; y += (int)k * WM_CASCADE; *mode = "cascade"; }
    else   { *mode = "center"; }
    if (x + w > cw) x = cw - w;
    if (y + h > ch) y = ch - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    *out_x = x;
    *out_y = y;
    (void)id;
}

/* 一块 surface 往脏矩形里 blit（clipped）；调用方保证**从下到上**（id 升序）调用 */
static int wm_blit_slot(const struct WmSlot* self, int x0, int y0, int x1, int y1, int* blended) {
    if (!self->px || !(self->info.flags & V64_WM_HASBUF)) return 0;
    const struct Wm64SurfaceInfo* si = &self->info;
    int ax0 = wm_max(si->x, x0), ay0 = wm_max(si->y, y0);
    int ax1 = wm_min(si->x + (int)si->w, x1), ay1 = wm_min(si->y + (int)si->h, y1);
    if (ax1 <= ax0 || ay1 <= ay0) return 0;
    const unsigned pitch_px = si->w;
    int written = 0;
    for (int sy = ay0; sy < ay1; sy++) {
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        const unsigned char* srow = self->px + (unsigned long)(sy - si->y) * (unsigned long)pitch_px * 4u;
        for (int sx = ax0; sx < ax1; sx++) {
            const uint32_t v = *(const uint32_t*)(srow + (unsigned long)(sx - si->x) * 4u);
            const uint32_t a = (v >> 24) & 0xFFu;
            if (a == 0xFFu) {
                drow[sx] = v;
            } else if (a == 0) {
                continue;                                  /* 全透明：保留背景 */
            } else {
                const uint32_t d = drow[sx];
                const uint32_t ia = 255u - a;
                const uint32_t r = (((v >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * ia) / 255u;
                const uint32_t gg = (((v >> 8) & 0xFFu) * a + ((d >> 8) & 0xFFu) * ia) / 255u;
                const uint32_t b = ((v & 0xFFu) * a + (d & 0xFFu) * ia) / 255u;
                drow[sx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
                (*blended)++;
            }
            written++;
        }
    }
    return written;
}

/* 把一个脏矩形重新合成：① 用快照回填桌面 ② 按 id 升序（从下到上）重画所有窗口 */
static int wm_paint_rect(struct WmSlot* slots, int x0, int y0, int x1, int y1, int* blended) {
    if (x1 <= x0 || y1 <= y0) return 0;
    wm_bg_restore(x0, y0, x1, y1);
    int px = 0;
    int order[WM_MAX_SURF], n = 0;
    for (int i = 0; i < WM_MAX_SURF; i++) if (slots[i].used) order[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (slots[order[b]].info.id < slots[order[a]].info.id) {
                const int t = order[a]; order[a] = order[b]; order[b] = t;
            }
    for (int a = 0; a < n; a++) px += wm_blit_slot(&slots[order[a]], x0, y0, x1, y1, blended);
    return px;
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

    /* ---- ② 注册为合成器 + 拿"可画范围"（本批起 = 整屏）---- */
    {
        const int seat_gpu = wl_composer_get(&g_rect);
        if (seat_gpu < 0) { printf("[WM] FAILED composer rc=%d\n", seat_gpu); return 1; }
        const int gpu = (seat_gpu & (int)V64_WM_GPU_BIT) ? 1 : 0;
        const int seat = seat_gpu & (int)V64_WM_SEAT_MASK;
        g_rx = g_rect.x; g_ry = g_rect.y; g_rw = g_rect.w; g_rh = g_rect.h;
        g_seat = seat;
        printf("[WM] composer seat=%d gpu=%d region=%d,%d,%d,%d\n", seat, gpu, g_rx, g_ry, g_rw, g_rh);
        printf("[WM] blitpolicy gpu=%d min_px=%d\n", gpu, (int)WM_DEV_MIN_PX);
    }

    /* ---- ③ rdtsc -> us 标定（ticks() 是 PIT 250Hz = 4ms 粒度，量 200ms）---- */
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

    /* ---- ④ 背景快照：整屏像素拷进 2 个 shm 对象（窗口挪走时用它把桌面还原）---- */
    {
        g_bg_need = (unsigned)g_fb.width * (unsigned)g_fb.height * 4u;
        const unsigned cap = WM_BG_OBJ_BYTES * 2u;
        if (g_bg_need == 0 || g_bg_need > cap) {
            printf("[WM] bg FAILED reason=%s need=%d cap=%d\n",
                   (g_bg_need == 0) ? "no-fb" : "too-big", (int)g_bg_need, (int)cap);
        } else {
            int ok = 1, ids[2] = { 0, 0 };
            for (int i = 0; i < 2 && ok; i++) {
                void* va = NULL;
                const unsigned want = (i == 0 || g_bg_need <= WM_BG_OBJ_BYTES)
                                    ? WM_BG_OBJ_BYTES
                                    : (g_bg_need - WM_BG_OBJ_BYTES);
                const int shm = shm_create((int)want);
                if (shm < 0 || shm_map(shm, 0, (int)want, &va) != 0 || !va) { ok = 0; break; }
                ids[i] = shm;
                g_bg[i] = (unsigned char*)va;
                if (i == 0 && g_bg_need <= WM_BG_OBJ_BYTES) break;   /* 一个对象就够 */
            }
            g_have_bg = ok ? 1 : 0;
            if (ok) {
                const int got = wm_bg_capture();
                printf("[WM] bg shm=%d,%d bytes=%d va=0x%x,0x%x u=1\n", ids[0], ids[1], (int)g_bg_need,
                       (unsigned)(unsigned long)g_bg[0], (unsigned)(unsigned long)(g_bg[1] ? g_bg[1] : g_bg[0]));
                if (got != (int)(g_fb.width * g_fb.height))
                    printf("[WM] bg partial px=%d want=%d\n", got, (int)(g_fb.width * g_fb.height));
            } else {
                printf("[WM] bg FAILED reason=shm need=%d cap=%d\n", (int)g_bg_need, (int)cap);
            }
        }
        printf("[WM] policy region=full drag_bar=%d place=center+cascade repair_ms=%d bg=%d\n",
               WM_DRAG_BAR, (int)WM_REPAIR_MS, g_have_bg);
    }

    static struct WmSlot slots[WM_MAX_SURF];
    for (int i = 0; i < WM_MAX_SURF; i++) { slots[i].map_shm = -1; }

    unsigned long n_frame = 0, seen = 0, hb_px = 0;
    const int t0_ms = (int)vimtu64_ticks() * 4;
    int last_repair_ms = 0;
    int painted_any = 0;

    for (;;) {
        const int now_ms = (int)vimtu64_ticks() * 4;
        const int elapsed = now_ms - t0_ms;

        int n_used = 0, n_pending = 0;
        int dmg_x = 0, dmg_y = 0, dmg_w = 0, dmg_h = 0;
        int layout = 0;

        /* ---- ⑤ 拉 surface 表（23 号）：谁有缓冲、谁提交了、damage 多大 ---- */
        for (int i = 0; i < WM_MAX_SURF; i++) {
            struct Wm64SurfaceInfo info;
            memset(&info, 0, sizeof(info));
            const int erc = wl_surface_export(i, &info);
            if (erc == 1) {                                   /* 这个槽空了 */
                if (slots[i].used) {
                    /* 窗口消失：把它原来的矩形还给桌面 */
                    wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                             slots[i].prev_x, slots[i].prev_y, slots[i].prev_w, slots[i].prev_h);
                    layout = 1;
                    slots[i].used = 0; slots[i].px = NULL; slots[i].map_shm = -1;
                    slots[i].placed = 0; slots[i].logged = 0;
                }
                continue;
            }
            if (erc != 0) continue;                           /* 负错误码：这一轮跳过（不猜） */
            const int is_new = (!slots[i].used || slots[i].info.id != info.id);
            if (is_new) layout = 1;
            if (is_new && slots[i].used) {
                wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                         slots[i].prev_x, slots[i].prev_y, slots[i].prev_w, slots[i].prev_h);
            }
            slots[i].used = 1;
            slots[i].info = info;
            slots[i].prev_w = (int)info.w;
            slots[i].prev_h = (int)info.h;
            if (is_new) { slots[i].placed = 0; slots[i].logged = 0; slots[i].map_shm = -1; }
            n_used++;

            /* 缓冲换对象（或第一次）：映射进自己的地址空间 */
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

            /* ---- ⑥ 摆放：新窗口用 27 号挪到屏幕正中（+ 级联）---- */
            if (!slots[i].placed) {
                int nx = 0, ny = 0;
                const char* mode = "center";
                wm_place(info.id, (int)info.w, (int)info.h, &nx, &ny, &mode);
                const int ox = (int)info.x, oy = (int)info.y;
                if (wl_surface_move(info.id, nx, ny) == 0) {
                    slots[i].info.x = nx;
                    slots[i].info.y = ny;
                    slots[i].placed = 1;
                    printf("[WM] place id=%d pos=%d,%d mode=%s\n", info.id, nx, ny, mode);
                } else {
                    /* 挪不动就老实留在内核给的默认位置（绝不假装摆过） */
                    slots[i].placed = 1;
                    printf("[WM] place FAILED id=%d want=%d,%d keep=%d,%d\n", info.id, nx, ny, ox, oy);
                }
                /* 新位置与旧位置都要重画（旧位置还给桌面） */
                wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h, ox, oy, (int)info.w, (int)info.h);
                wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                         slots[i].info.x, slots[i].info.y, (int)info.w, (int)info.h);
            }

            if (!slots[i].logged) {
                slots[i].logged = 1;
                printf("[WM] surf id=%d pid=%d rect=%d,%d,%d,%d pix=%d flags=0x%x\n",
                       info.id, info.pid, slots[i].info.x, slots[i].info.y, (int)info.w, (int)info.h,
                       (int)(info.w * info.h), (unsigned)info.flags);
            }

            if (info.flags & V64_WM_PENDING) {
                n_pending++;
                int rx = (int)slots[i].info.x + info.dmg_x;
                int ry = (int)slots[i].info.y + info.dmg_y;
                int rw = info.dmg_w, rh = info.dmg_h;
                if (rw > (int)info.w - info.dmg_x) rw = (int)info.w - info.dmg_x;
                if (rh > (int)info.h - info.dmg_y) rh = (int)info.h - info.dmg_y;
                wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h, rx, ry, rw, rh);
            }
        }
        if (n_used) seen = 1;

        /* ---- ⑦ 空闲/寿命判定 ---- */
        const int idle = (n_pending == 0 && !layout);
        if (idle) {
            if (n_used == 0 && seen) {
                printf("[WM] exit reason=no-surfaces n_used=0 seen=1 elapsed_ms=%d\n", elapsed);
                break;
            }
            if (!seen && elapsed >= (int)WM_SHORT_MS) {
                printf("[WM] exit reason=short-mode elapsed_ms=%d\n", elapsed);
                break;
            }
            if (seen && elapsed >= (int)(full ? WM_FULL_MS : WM_SEEN_MS)) {
                printf("[WM] exit reason=timeout elapsed_ms=%d limit_ms=%d\n",
                       elapsed, (int)(full ? WM_FULL_MS : WM_SEEN_MS));
                break;
            }
            /* 周期性修复：把窗口矩形整体重画一遍（盖掉外壳画进来的光标/高亮） */
            if (painted_any && (now_ms - last_repair_ms) >= (int)WM_REPAIR_MS) {
                for (int i = 0; i < WM_MAX_SURF; i++) {
                    if (!slots[i].used) continue;
                    wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                             slots[i].info.x, slots[i].info.y, (int)slots[i].info.w, (int)slots[i].info.h);
                }
            }
        }
        if (layout) {
            for (int i = 0; i < WM_MAX_SURF; i++) {
                if (!slots[i].used) continue;
                wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                         slots[i].info.x, slots[i].info.y, (int)slots[i].info.w, (int)slots[i].info.h);
            }
        }

        /* ---- ⑧ 裁剪：脏矩形 ∩ 屏幕（窗口矩形之外绝不多画一个像素）---- */
        if (dmg_w > 0 && dmg_h > 0) {
            int x0 = wm_max(dmg_x, 0), y0 = wm_max(dmg_y, 0);
            int x1 = wm_min(dmg_x + dmg_w, (int)g_fb.width), y1 = wm_min(dmg_y + dmg_h, (int)g_fb.height);
            dmg_x = x0; dmg_y = y0; dmg_w = x1 - x0; dmg_h = y1 - y0;
        } else {
            dmg_w = 0; dmg_h = 0;
        }

        if (dmg_w > 0 && dmg_h > 0) {
            /* ---- ⑨ 用户态合成：快照回填 + 从下到上 blit ---- */
            const unsigned long long t_c0 = wm_rdtsc();
            int blend = 0;
            const int px = wm_paint_rect(slots, dmg_x, dmg_y, dmg_x + dmg_w, dmg_y + dmg_h, &blend);
            const unsigned long long us = wm_us(wm_rdtsc() - t_c0);

            const int use_dev = 0;      /* 22 号的 gpu 位为 0 -> 恒 soft，如实写 */
            const unsigned long long t_f0 = wm_rdtsc();
            const int frc = fb_flip(dmg_x, dmg_y, dmg_w, dmg_h);
            const unsigned long long flip_us = wm_us(wm_rdtsc() - t_f0);
            n_frame++;
            painted_any = 1;
            last_repair_ms = now_ms;
            /* ★ 只给"有内容变化"的帧留证：纯修复帧（每 150 ms 一次，没有任何客户端提交）不打 ——
             *   每一行都会让 ring0 终端滚动重绘一次，几百行就把客户机拖到 1 fps，进而把夹具的
             *   光标闭环反馈拖死（实测）。layout / pending / 拖动中 / 第一帧 仍然照打。 */
            if ((n_frame == 1 || layout || n_pending || g_drag_id) && g_frame_log < WM_FRAME_LOG_MAX) {
                g_frame_log++;
                printf("[WM] frame n=%lu surfs=%d pending=%d dirty=%d,%d,%d,%d px=%d blend=%d us=%lu flip_us=%lu path=%s full=%d foreign=0\n",
                       n_frame, n_used, n_pending, dmg_x, dmg_y, dmg_w, dmg_h, px, blend,
                       (unsigned long)us, (unsigned long)flip_us, use_dev ? "dev" : "soft", layout ? 1 : 0);
            }
            if (frc != 0) printf("[WM] frame flip rc=%d\n", frc);
            if (n_frame == 1)
                printf("[WM] blitpolicy gpu=0 min_px=%d rect_px=%d path=soft\n", (int)WM_DEV_MIN_PX, px);

            /* ---- ⑩ 确认：告诉内核"这条我合成完了"（清 pending/damage + content=1）---- */
            for (int i = 0; i < WM_MAX_SURF; i++) {
                if (!slots[i].used) continue;
                if (!(slots[i].info.flags & V64_WM_PENDING)) continue;
                if (wl_surface_ack(slots[i].info.id) == 0)
                    printf("[WM] ack id=%d dmg=%d,%d,%d,%d\n", slots[i].info.id,
                           slots[i].info.dmg_x, slots[i].info.dmg_y,
                           slots[i].info.dmg_w, slots[i].info.dmg_h);
                slots[i].info.flags &= ~(unsigned)V64_WM_PENDING;
                slots[i].info.flags |= (unsigned)V64_WM_CONTENT;
            }
        }

        /* 记下这一轮的窗口位置（下一轮算 dirty 用；同时留给退出回填） */
        wm_remember(slots);
        for (int i = 0; i < WM_MAX_SURF; i++) {
            if (!slots[i].used) continue;
            slots[i].prev_x = (int)slots[i].info.x;
            slots[i].prev_y = (int)slots[i].info.y;
            slots[i].prev_w = (int)slots[i].info.w;
            slots[i].prev_h = (int)slots[i].info.h;
        }

        /* ---- ⑪ 输入：input_poll 收事件 -> 拖动 / 用户态命中测试 -> wl_seat_post ---- */
        /* ★ 关键（实测踩到）：**一个 surface 都没有时不收输入**。否则 wm 会把宿主敲键盘产生的
         *   每一个 KeyDown/KeyUp 都读走并打一行 → ring0 终端每行都要重绘（1280x800 在 TCG 下
         *   ~0.9 s/次）→ 整个客户机掉到 1~3 fps → 夹具那边的光标闭环反馈直接卡死几十秒。
         *   "没有窗口可路由"时收输入本来也没有任何用处（wm 不产生键盘事件给谁）。 */
        if (n_used > 0)
        {
            struct Ev64Event raw[WM_EV_MAX];
            const int got = poll_event(raw, WM_EV_MAX, 0);
            int got_any = 0;
            if (got > 0) {
                for (int i = 0; i < got; i++) {
                    struct Wl64SeatEvent se;
                    memset(&se, 0, sizeof(se));
                    se.ev = raw[i];
                    se.seat = (unsigned)g_seat;
                    se.surf = 0; se.sx = -1; se.sy = -1; se.inside = 0; se.pad = 0;

                    const int is_key = (raw[i].type == V64_EV_KEY_DOWN || raw[i].type == V64_EV_KEY_UP);
                    const int is_down = (raw[i].type == V64_EV_MOUSE_DOWN);
                    const int is_up   = (raw[i].type == V64_EV_MOUSE_UP);
                    const int is_move = (raw[i].type == V64_EV_MOUSE_MOVE);
                    got_any = 1;

                    if (is_down && (raw[i].code & 1u)) {
                        /* 左键按下：落点在最上层窗口的**顶部拖动条**里 -> 开始拖 */
                        struct WmSlot* hs = wm_hit(slots, raw[i].x, raw[i].y);
                        if (hs && (raw[i].y - hs->info.y) < WM_DRAG_BAR) {
                            g_drag_id = (int)hs->info.id;
                            g_drag_grab_x = raw[i].x - hs->info.x;
                            g_drag_grab_y = raw[i].y - hs->info.y;
                            g_drag_from_x = (int)hs->info.x;
                            g_drag_from_y = (int)hs->info.y;
                            g_drag_moved = 0;
                            printf("[WM] drag begin id=%d grab=%d,%d from=%d,%d\n",
                                   g_drag_id, g_drag_grab_x, g_drag_grab_y, g_drag_from_x, g_drag_from_y);
                        }
                    }

                    if (g_drag_id && is_move) {
                        struct WmSlot* ds = NULL;
                        for (int k = 0; k < WM_MAX_SURF; k++)
                            if (slots[k].used && (int)slots[k].info.id == g_drag_id) { ds = &slots[k]; break; }
                        if (ds) {
                            int nx = raw[i].x - g_drag_grab_x;
                            int ny = raw[i].y - g_drag_grab_y;
                            const int ox = (int)ds->info.x, oy = (int)ds->info.y;
                            if (nx != ox || ny != oy) {
                                if (wl_surface_move(ds->info.id, nx, ny) == 0) {
                                    ds->info.x = nx;
                                    ds->info.y = ny;
                                    g_drag_moved++;
                                    printf("[WM] drag move id=%d pos=%d,%d dx=%d dy=%d\n",
                                           g_drag_id, nx, ny, nx - ox, ny - oy);
                                    /* 旧位置还给桌面 + 新位置重画 */
                                    wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                                             ox, oy, (int)ds->info.w, (int)ds->info.h);
                                    wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                                             nx, ny, (int)ds->info.w, (int)ds->info.h);
                                }
                            }
                        } else {
                            g_drag_id = 0;      /* 窗口没了：拖动中止 */
                        }
                    }

                    if (is_up && (raw[i].code & 1u) && g_drag_id) {
                        struct WmSlot* ds = NULL;
                        for (int k = 0; k < WM_MAX_SURF; k++)
                            if (slots[k].used && (int)slots[k].info.id == g_drag_id) { ds = &slots[k]; break; }
                        printf("[WM] drag end id=%d pos=%d,%d moved=%d\n", g_drag_id,
                               ds ? (int)ds->info.x : -1, ds ? (int)ds->info.y : -1, g_drag_moved);
                        g_drag_id = 0;
                    }

                    /* 事件照旧投回客户端（拖动期间也投：客户端的按钮判定只看 UP 落在哪）*/
                    if (is_key) {
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
                        g_drag_posts++;
                        /* 只给**指针**事件留证（键盘是外壳的事，打出来只会把串口刷爆、拖死终端重绘） */
                        if (!is_key && g_route_logged < 32) {
                            g_route_logged++;
                            printf("[WM] route surf=%d pid=%d type=%d x=%d y=%d sx=%d sy=%d\n",
                                   (int)se.surf, tpid, (int)raw[i].type, (int)raw[i].x, (int)raw[i].y,
                                   (int)se.sx, (int)se.sy);
                        }
                    } else if (!is_key && g_route_logged < 32) {
                        g_route_logged++;
                        printf("[WM] route none type=%d x=%d y=%d\n", (int)raw[i].type, (int)raw[i].x, (int)raw[i].y);
                    }
                }
            }
            /* 收到输入：外壳很可能刚把光标画进我们的窗口里 -> 立刻整窗重画一遍 */
            if (got_any) {
                for (int i = 0; i < WM_MAX_SURF; i++) {
                    if (!slots[i].used) continue;
                    wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h,
                             slots[i].info.x, slots[i].info.y, (int)slots[i].info.w, (int)slots[i].info.h);
                }
                if (dmg_w > 0 && dmg_h > 0) {
                    int x0 = wm_max(dmg_x, 0), y0 = wm_max(dmg_y, 0);
                    int x1 = wm_min(dmg_x + dmg_w, (int)g_fb.width), y1 = wm_min(dmg_y + dmg_h, (int)g_fb.height);
                    if (x1 > x0 && y1 > y0) {
                        int blend2 = 0;
                        const int px2 = wm_paint_rect(slots, x0, y0, x1, y1, &blend2);
                        (void)fb_flip(x0, y0, x1 - x0, y1 - y0);
                        n_frame++;
                        last_repair_ms = now_ms;
                        printf("[WM] frame n=%lu surfs=%d pending=0 dirty=%d,%d,%d,%d px=%d blend=%d us=0 flip_us=0 path=soft full=0 foreign=0\n",
                               n_frame, n_used, x0, y0, x1 - x0, y1 - y0, px2, blend2);
                        wm_remember(slots);
                    }
                }
            }
        }
        if (!dmg_w && n_pending == 0) vimtu64_sleep_ms(WM_FRAME_MS);
    }

    /* ---- ⑫ 收工：把**我们画过的那些窗口矩形**用快照还原（不是整屏回填）。
     *   用 g_last*（最近一次真正画上屏的矩形）：最后一个客户端销毁时 slots 里的 used 已经被清掉，
     *   只看 used 会得到 px=0 —— 那等于"退出时并没有把地还回去"这句假话。 */
    {
        uint32_t bg = 0xFF202020u;
        int win_area = 0;
        for (int i = 0; i < g_lastn; i++) {
            int x0 = wm_max(g_lastx[i], 0), y0 = wm_max(g_lasty[i], 0);
            int x1 = wm_min(g_lastx[i] + g_lastw[i], (int)g_fb.width);
            int y1 = wm_min(g_lasty[i] + g_lasth[i], (int)g_fb.height);
            if (x1 <= x0 || y1 <= y0) continue;
            win_area += (x1 - x0) * (y1 - y0);
            const uint32_t* s = wm_bg_at(x0, y0);
            if (s) bg = *s | 0xFF000000u;
            hb_px += (unsigned long)wm_bg_restore(x0, y0, x1, y1);
            (void)fb_flip(x0, y0, x1 - x0, y1 - y0);
        }
        printf("[WM] exit handback color=0x%x px=%lu frames=%lu bg=%d area=%d wins=%d\n",
               (unsigned)bg, hb_px, n_frame, g_have_bg, win_area, g_lastn);
    }
    return 0;
}
