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
#define WM_FRAME_MS     16u          /* ★ v0.10.2：空闲轮询 = 一帧预算（原来是 40 ms，
                                      *   空闲时光标/窗口修复要等 40 ms 才跟得上） */
#define WM_PACE_MS      16           /* 帧节拍目标：16 ms ≈ 62.5 Hz（PIT 250 Hz = 4 ms 粒度，
                                      *   16 是最接近 60 Hz 的整数倍；每帧画完还早就把余量睡掉） */
#define WM_PROBE        "/etc/wm_probe"   /* 与 kernel/wl64.cpp 的 WL64_WM_PROBE64 同一个文件 */
#define WM_DEV_MIN_PX   4096u        /* 设备提交路径的阈值：64x64（本台都没有 virtio-gpu -> 恒 soft） */
#define WM_FULL_MS      45000u       /* full 模式最长运行时间 */
#define WM_SHORT_MS     180000u      /* short 模式：**一个 surface 都没见过**时的等待窗口（见文件头） */
#define WM_SEEN_MS      1800000u     /* ★ W3 第三批：short 模式"见过 surface 之后的最长寿命"。
                                      *   原来是 60000 —— 实测 `[WM] exit reason=timeout elapsed_ms=60032`
                                      *   会在**窗口还在屏上**时把合成器自己收掉：拖动/点击验收跑不到
                                      *   60 s 就没有合成器了（E 段全红、capture 被释放、事件改由内核
                                      *   直接投给焦点进程）。合成器是**长驻服务**，不该有 60 s 自裁；
                                      *   真正的收工判据是上面那条 `no-surfaces`（见过的 surface 全退光）。 */
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
    /* ★ v0.11.2：装饰用的标题缓存（通过 29 号从内核读回，每个 surface 只读一次） */
    /* ★ v0.11.4：32 -> 64 B（中文标题 3 B/字；"图片查看器 1280x800" 才放得下） */
    char                   title[64];
    int                    title_got;
};

static struct Fb64Info  g_fb;
static uint32_t*        g_fbpx;
static struct Wm64Rect  g_rect;                 /* 22 号写回：整屏（本批起） */
static int g_rx, g_ry, g_rw, g_rh;
static unsigned long long g_tsc_per_ms = 0;
static unsigned long long g_route_logged = 0;
// ★ 单独给"路由到窗口"一份预算：早期从 (0,0) 走向控件的一大堆 route none
//   会把共用预算吃光，真按到窗口上时反而打不出来（E/F 的取证就靠这一行）。
static unsigned long long g_route_surf_logged = 0;
static unsigned g_place_seq = 0;
static int      g_seat = 1;                      /* 22 号给的座位 id（事件投递用） */
static uint32_t g_frame_log = 0;                 /* [WM] frame 行的预算（纯修复帧不打，见下） */
static const uint32_t WM_FRAME_LOG_MAX = 64u;
/* ★ v0.10.2（帧预算）：拖动/路由/确认三处的逐事件打点预算。
 *   为什么：本机日志出口是内核终端的 COM1，每行 ≈1.4 ms（QEMU TCG 实测），而拖动时
 *   每个鼠标事件曾经要打 3 行（drag move / route / ack）+ 内核侧 8 行 —— 单帧 175 ms
 *   里约 170 ms 是打点，合成器被自己的日志锁在 ~6 fps。证据口径不变：前 N 条原文照打。 */
static uint32_t g_dragmove_log = 0;              /* [WM] drag move：前 12 条 */
static uint32_t g_ack_log = 0;                   /* [WM] ack：前 16 条 */
static const uint32_t WM_ROUTE_LOG_MAX = 16u;    /* 指针路由行（原来 240） */

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
    // ★ v0.10.1：**全部客户端刚销毁时不清备忘录**。
    //   现场证据（tests/wm64_test.py ⑦ 红）：
    //     [WM] exit handback color=0xff202020 px=0 frames=139 bg=1 area=0 wins=0
    //   wins=0 说明 g_lastn 被清成了 0：最后一个客户端销毁后还会再画一帧（surfs=0），
    //   那一帧的 wm_remember() 把"我们画过的窗口矩形"抹掉了 —— 于是收工回填时
    //   手里一个矩形都没有，px/area 只能是 0，"把地还回去"这句就成了假话。
    //   语义：备忘录记录的是**最近一次真正画上屏的窗口矩形**，空列表不是新状态，
    //   不覆盖上一次。
    // ★ v0.10.1：这里的语义是**并集**，不是"最后一帧的活窗口集合"。
    //   现场（tests/wm64_test.py ⑦ 的 px=8192 want=23552）：两个客户端先后退出，
    //   先退的那个的矩形被后一帧的"活窗口只有 1 个"覆盖掉 —— 于是收工回填只还了
    //   wmpanel 的 128x64 = 8192 px，而 wm 实际画过 160x96 + 128x64 = 23552 px。
    //   并集才对：wm 画过的每一块地，收工时都要还回去（窗口挪过就两块都还）。
    for (int i = 0; i < WM_MAX_SURF; i++) {
        if (!slots[i].used) continue;
        const int x = (int)slots[i].info.x, y = (int)slots[i].info.y;
        const int w = (int)slots[i].info.w, h = (int)slots[i].info.h;
        if (w <= 0 || h <= 0) continue;
        int dup = 0;
        for (int k = 0; k < g_lastn; k++) {
            if (g_lastx[k] == x && g_lasty[k] == y && g_lastw[k] == w && g_lasth[k] == h) { dup = 1; break; }
        }
        if (dup || g_lastn >= WM_LAST_MAX) continue;
        g_lastx[g_lastn] = x;
        g_lasty[g_lastn] = y;
        g_lastw[g_lastn] = w;
        g_lasth[g_lastn] = h;
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

#define WM_BENCH "/etc/wm_bench"   /* ★ v0.10.2：验收基准只在有这个文件时跑（发版路径零副作用） */

/* 探针：任意路径的 access(21) 探测（Linux 兼容路径）。 */
static int wm_probe_named(const char* p) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)p, 0, 0, 0, 0) == 0 ? 1 : 0;
}

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

/* ★ v0.10.2（帧预算）：本文件专用的 8 字节字拷贝。
 *   为什么不改 user/lib/string.c：那份 libc 也会链进"原始 blob 区"里的 12 份演示程序，
 *   而原始区是 1,799 扇区、**零扇区余量**（实测 921,536 B > 上限 921,088 B 会让整条构建
 *   在 demo_pack 那一关口失败）。合成器的拷贝只影响 /lib/wm.elf（卷交付，不受该预算约束）。
 *   也不用 rep movsb：QEMU TCG 对 REP 串操作是"每字节回一次 helper"，实测慢约 100 倍。 */
static void* wm_copy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    while (n >= 8) { *(unsigned long long*)d = *(const unsigned long long*)s; d += 8; s += 8; n -= 8; }
    while (n--) *d++ = *s++;
    return dst;
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
/* ★ v0.10.2（帧预算）：按行整段 memcpy，不再逐像素走 wm_bg_at()。
 *   为什么：wm_bg_at 每像素要算一遍字节偏移、判一次"跨没跨 2 个 shm 对象"、再取一次基址；
 *   一帧回填上万像素时这部分和真正的搬运一样贵。现在只在**对象边界**切一次段。 */
static int wm_bg_restore(int x0, int y0, int x1, int y1) {
    if (!g_have_bg) return 0;
    int n = 0;
    int cx0 = x0 < 0 ? 0 : x0;
    int cx1 = x1 > (int)g_fb.width ? (int)g_fb.width : x1;
    if (cx0 >= cx1) return 0;
    for (int sy = y0; sy < y1; sy++) {
        if (sy < 0 || sy >= (int)g_fb.height) continue;
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        int sx = cx0;
        while (sx < cx1) {
            const unsigned off = ((unsigned)sy * (unsigned)g_fb.width + (unsigned)sx) * 4u;
            const int h = (off < WM_BG_OBJ_BYTES) ? 0 : 1;
            const unsigned base = h ? WM_BG_OBJ_BYTES : 0u;
            const unsigned seg = h ? (g_bg_need - base) : WM_BG_OBJ_BYTES;
            if (off < base || off >= seg + base) break;        /* 偏移越界：如实放弃这一行 */
            unsigned room = seg - (off - base);
            unsigned want = (unsigned)(cx1 - sx) * 4u;
            if (want > room) want = room;
            if (want == 0) break;
            wm_copy(drow + sx, g_bg[h] + (off - base), want);
            const unsigned cnt = want / 4u;
            n += (int)cnt;
            sx += (int)cnt;
        }
    }
    return n;
}


/* ==================== \u2605 v0.11.2\uff1a\u7a97\u53e3\u88c5\u9970\uff08Ring 3 \u5408\u6210\u5668\u63a5\u7ba1\uff09 ====================
 * \u5efa\u6a21\u53e3\u5f84\u4e0e\u5185\u6838\u8def**\u9010\u9879\u5bf9\u9f50**\uff08kernel/gui64.cpp \u7684 gui64_deco_frame64 / gui64_deco_edge64\uff09\uff1a
 *   \u6807\u9898\u5e26 24 + \u4e0a\u8fb9\u6846 1 = 25 \u884c\u3001\u5706\u89d2\u534a\u5f84 14\u3001\u4e09\u6309\u94ae\u69fd 30px / \u5757 24x16\u30011px \u73bb\u7483\u8fb9\u6846\u3002
 * \u548c\u5185\u6838\u8def\u4e0d\u540c\u7684\u4e24\u5904\uff08\u5982\u5b9e\u6807\u6ce8\uff09\uff1a
 *   \u2460 \u5b57\u4f53\uff1a\u2605 v0.11.3 \u8d77\u4e5f\u662f TTF \u5b57\u6a21\uff08wm_ttf.h\uff0c\u540c\u4e00\u4efd\u5b57\u5e93 em=16 \u9884\u5149\u6805\u5316\uff09\uff0c\u4e0e\u5185\u6838\u7a97\u53e3\u6807\u9898\u540c\u6e90\uff1b \u5b57\u5f62\u4e0d\u540c\uff0c\u5b57\u9ad8/\u4f4d\u7f6e\u4e00\u81f4\uff1b
 *   \u2461 \u56db\u89d2\u5706\u89d2\u7528**wm \u81ea\u5df1\u7684\u6574\u5c4f\u80cc\u666f\u5feb\u7167**\u8865\u50cf\u7d20\uff08\u5185\u6838\u8def\u662f\u6a21\u7cca\u58c1\u7eb8\u91c7\u6837\uff09\uff0c
 *      \u6240\u4ee5\u62d6\u52a8\u540e\u5706\u89d2\u91cc\u770b\u5230\u7684\u662f\u771f\u5f53\u65f6\u7684\u684c\u9762\u3002
 * \u7a97\u53e3\u72b6\u6001\uff08\u6298\u53e0/\u6700\u5927\u5316\uff09\u4e0d\u5b58\u5728\u8fd9\u91cc\uff1a\u5b83\u5c5e\u4e8e\u5185\u6838\u7684 surface \u8868\uff0823 \u53f7\u5bfc\u51fa\u7684 flags\uff09\uff0c
 * \u4e09\u6309\u94ae\u7684\u52a8\u4f5c\u8d70 30 \u53f7 wl_surface_ctl \u2014\u2014 \u8fd9\u6837\u4e24\u6761\u5408\u6210\u8def\u770b\u5230\u7684\u662f**\u540c\u4e00\u4efd\u72b6\u6001**\u3002 */
#include "wm_ttf.h"      /* ★ v0.11.3：TTF 字模（替代 v0.11.2 的 wm_font8.h 8x8 位图） */

#define WM_DECO_R      14
#define WM_DECO_HDR    25            /* \u6807\u9898\u680f 24 + \u4e0a\u8fb9\u6846 1\uff08= kernel \u7684 WL64_DECOR_H64\uff09 */
#define WM_BTN_SLOT    30
#define WM_BTN_W       24
#define WM_BTN_H       16
#define WM_C_TITLE     0xFFF2F4F8u   /* \u4e0e\u4e3b\u9898 light \u7684 title_bg \u540c\u503c\uff08\u5b9e\u6d4b\u5185\u6838\u8def\u50cf\u7d20 242,244,248\uff09 */
#define WM_C_CLIENT    0xFFF2F2F2u   /* \u62ff\u4e0d\u5230 23 \u53f7 pad \u65f6\u7684\u515c\u5e95\u5ba2\u6237\u533a\u5e95\u8272 */
#define WM_C_BORDER    0xFFBFC6D0u
#define WM_C_BTN       0xFFE6E8ECu
#define WM_C_CLOSE     0xFFE81123u
#define WM_C_GLYPH     0xFF3A4048u

static int g_clip_x0, g_clip_y0, g_clip_x1, g_clip_y1;

static inline uint32_t* wm_rowpx(int y) {
    return (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)y * g_fb.pitch);
}
static void wm_put(int x, int y, uint32_t c) {
    if (x < g_clip_x0 || x >= g_clip_x1 || y < g_clip_y0 || y >= g_clip_y1) return;
    wm_rowpx(y)[x] = c;
}
static void wm_fill(int x, int y, int w, int h, uint32_t c) {
    const int x0 = wm_max(x, g_clip_x0), x1 = wm_min(x + w, g_clip_x1);
    const int y0 = wm_max(y, g_clip_y0), y1 = wm_min(y + h, g_clip_y1);
    for (int j = y0; j < y1; j++) {
        uint32_t* r = wm_rowpx(j);
        for (int i = x0; i < x1; i++) r[i] = c;
    }
}
/* 圆角裁切（按位选角）：mask bit0 左上 / bit1 右上 / bit2 左下 / bit3 右下。
 * ★ v0.11.6：**程序窗口只切上面两角**（mask=0x3）—— 用户点名的规范原文是"左上右上为 90 度弧、
 *   底边保持全宽直线"。旧版切四角 + WM_DECO_R=14（比标题栏半高 12.5 还大）→ 左右看着像半圆端帽。
 *   系统栏（Dock/任务栏）不走这个函数，所以它们的形状一个像素都没动。 */
static void wm_corner_cut_mask(int x, int y, int w, int h, int r, int mask) {
    if (r <= 0 || w <= 2 * r || h <= 2 * r) return;
    const int r2 = r * r;
    for (int k = 0; k < 4; k++) {
        if (!(mask & (1 << k))) continue;
        const int ox = (k & 1) ? (x + w - r) : x;
        const int oy = (k & 2) ? (y + h - r) : y;
        for (int j = 0; j < r; j++) {
            for (int i = 0; i < r; i++) {
                const int cx = (k & 1) ? (r - 1 - i) : i;
                const int cy = (k & 2) ? (r - 1 - j) : j;
                const int dx = r - 1 - cx, dy = r - 1 - cy;
                if (dx * dx + dy * dy <= r2) continue;          /* \u5706\u5185\uff1a\u4fdd\u7559 */
                const int px = ox + i, py = oy + j;
                if (px < g_clip_x0 || px >= g_clip_x1 || py < g_clip_y0 || py >= g_clip_y1) continue;
                const uint32_t* bp = wm_bg_at(px, py);
                wm_rowpx(py)[px] = bp ? *bp : 0xFF202020u;
            }
        }
    }
}
/* ==================== ★ v0.11.3：标题字形 = TTF 字模（不再用 8x8 位图）====================
 * 由来：v0.11.2 的 wm 标题用 user/wm/wm_font8.h 的 8x8 位图，而内核窗口标题用 TTF ——
 *   同一块屏幕上两套字形，"统一装饰"就只统一到边框为止。要把 stb_truetype 直接链进 /lib/wm.elf
 *   又塞不下（ELF64 装载窗口只有 64 KiB，font64.o 一个文件 94,800 B）。
 * 做法：tools/wm_titlefont_win.py 用**同一份字库、同一个 em(16px)** 预光栅化 ASCII 32..126，
 *   生成 4-bit alpha 字模（wm_ttf.h，alpha 4,718 B）。这里按字模做 alpha 混合 ——
 *   字形来源与内核标题完全同源，字高/基线也用"最高字形顶端 = 0"对齐（见生成器注释）。
 * 如实边界：预光栅化只有 Regular 一份、只有 ASCII；非 ASCII 字符按 8px 空白跳过。 */
static void wm_blend(int x, int y, uint32_t fg, int a) {
    if (a <= 0) return;
    if (x < g_clip_x0 || x >= g_clip_x1 || y < g_clip_y0 || y >= g_clip_y1) return;
    uint32_t* p = &wm_rowpx(y)[x];
    if (a >= 15) { *p = fg; return; }
    const uint32_t d = *p, ia = (uint32_t)(15 - a), aa = (uint32_t)a;
    const uint32_t r = ((((fg >> 16) & 0xFFu) * aa + ((d >> 16) & 0xFFu) * ia) + 7u) / 15u;
    const uint32_t g = ((((fg >> 8) & 0xFFu) * aa + ((d >> 8) & 0xFFu) * ia) + 7u) / 15u;
    const uint32_t b = (((fg & 0xFFu) * aa + (d & 0xFFu) * ia) + 7u) / 15u;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}
static void wm_text_ttf(int x, int y, const char* s, uint32_t fg, int max_x) {
    if (!s) return;
    int cx = x;
    for (unsigned i = 0; s[i]; i++) {
        const unsigned char ch = (unsigned char)s[i];
        if (ch < WM_TF_FIRST || ch > WM_TF_LAST) { cx += 8; continue; }
        const struct WmTfGlyph* gl = &g_wm_tf[ch - WM_TF_FIRST];
        if (gl->w && gl->h) {
            if (cx + (int)gl->bx + (int)gl->w > max_x) return;
            const int rowb = ((int)gl->w + 1) / 2;
            for (int gy = 0; gy < (int)gl->h; gy++) {
                for (int gx = 0; gx < (int)gl->w; gx++) {
                    const unsigned char byte = g_wm_tf_alpha[(int)gl->off + gy * rowb + (gx >> 1)];
                    const int a = (gx & 1) ? (byte & 0xF) : (byte >> 4);
                    if (a) wm_blend(cx + (int)gl->bx + gx, y + (int)gl->by + gy, fg, a);
                }
            }
        }
        cx += gl->adv;
    }
}

/* ==================== ★ v0.11.4：标题字形与图标都向**内核**要 ====================
 * 来由（本版要解决的两条边界）：
 *   ① v0.11.3 的 wm 标题用预光栅化 ASCII 字模（wm_ttf.h）—— **中文标题画不出来**
 *      （非 ASCII 按 8px 空白跳过）。要自己光栅化 CJK 就得链 stb_truetype，而 /lib/wm.elf 的
 *      ELF64 装载窗口只有 64 KiB（font64.o 94,800 B），塞不进去 —— 这不是"懒得做"，是装不下。
 *   ② 标题栏/任务栏只有默认图标，没有应用身份。
 * 结论：字形/图标的**唯一来源是内核**（与"装饰单一真源"同一思路），合成器只做 alpha 混合：
 *   31 wl_surface_title_alpha() -> 8-bit 覆盖率位图（320x24，stride=320，左上角 = em 框顶）
 *   33 wl_surface_app_icon()    -> 16x16 RGBA（品牌色 + 覆盖率 alpha）
 * 这样两条合成路（内核窗口 / Ring 3 合成器）的**像素来自同一套字形缓存与图标包**，
 * 中文标题不需要在 wm 侧多一个字节的字模，图标也不需要 wm 自己解析 /etc/iconpack.bin。 */
#define WM_TITLE_BMP_W  V64_WL_TITLE_BMP_W      /* 320；与内核 31 号的 stride 同一口径 */
#define WM_TITLE_BMP_H  V64_WL_TITLE_BMP_H      /* 24 */
#define WM_ICON_PX      V64_WL_APP_ICON_PX      /* 16 */
static unsigned char g_title_bmp[WM_TITLE_BMP_W * WM_TITLE_BMP_H];
static unsigned char g_icon_rgba[WM_ICON_PX * WM_ICON_PX * 4];
static int g_title_bmp_calls = 0;               /* 31 号调用次数（[WM] title bmp 打点用） */
static int g_title_bmp_ok = 0;                  /* 1 = 最近一次标题是用内核位图画出来的 */
static int g_title_icon_kind = -1;              /* 最近一次拿到的图标 kind（-1 = 没有） */
static int g_wm_title_log = 0;                  /* [WM] title bmp 行的预算（防刷屏） */
static void wm_title_log64(const struct WmSlot* s, int w, int h) {
    if (g_wm_title_log >= 6) return;            /* 每帧都会画标题：前 6 条够取证，之后闭嘴 */
    g_wm_title_log++;
    printf("[WM] title bmp surf=%d w=%d h=%d calls=%d ok=%d icon=%d\n",
           (int)s->info.id, w, h, g_title_bmp_calls, g_title_bmp_ok, g_title_icon_kind);
}

/* 8-bit 覆盖率的 alpha 混合（0..255）。数学与内核 font.cpp 的 blend_px 同式，
 * 这样"同一份覆盖率 + 同一个颜色"在两路得到**同一个像素**（不是"看起来差不多"）。 */
static void wm_blend8(int x, int y, uint32_t fg, int a) {
    if (a <= 0) return;
    if (x < g_clip_x0 || x >= g_clip_x1 || y < g_clip_y0 || y >= g_clip_y1) return;
    uint32_t* p = &wm_rowpx(y)[x];
    if (a >= 255) { *p = fg; return; }
    const uint32_t d = *p, ia = (uint32_t)(255 - a);
    const uint32_t r = ((((fg >> 16) & 0xFFu) * (uint32_t)a + ((d >> 16) & 0xFFu) * ia) + 127u) / 255u;
    const uint32_t g = ((((fg >> 8) & 0xFFu) * (uint32_t)a + ((d >> 8) & 0xFFu) * ia) + 127u) / 255u;
    const uint32_t b = (((fg & 0xFFu) * (uint32_t)a + (d & 0xFFu) * ia) + 127u) / 255u;
    *p = 0xFF000000u | (r << 16) | (g << 8) | b;
}
/* 标题文字：内核 31 号给覆盖率，这里只混颜色。返回实际画的宽度（<0 = 内核没给，调用方回落字模） */
static int wm_title_text(const struct WmSlot* s, int x, int y, int max_x) {
    const int rc = wl_surface_title_alpha((int)s->info.id, g_title_bmp, (unsigned)sizeof(g_title_bmp));
    g_title_bmp_calls++;
    if (rc < 0) { g_title_bmp_ok = 0; return rc; }
    const int w = (rc >> 16) & 0xFFFF;
    const int h = rc & 0xFFFF;
    if (w <= 0 || h <= 0) { g_title_bmp_ok = 1; return 0; }
    g_title_bmp_ok = 1;
    wm_title_log64(s, w, h);                    /* 打点：前 6 条（含 icon kind，取证用） */
    int cw = w;
    if (max_x > x && cw > max_x - x) cw = max_x - x;
    for (int yy = 0; yy < h; yy++) {
        const unsigned char* row = g_title_bmp + yy * WM_TITLE_BMP_W;
        for (int xx = 0; xx < cw; xx++) {
            const int a = row[xx];
            if (a) wm_blend8(x + xx, y + yy, WM_C_GLYPH, a);
        }
    }
    return cw;
}
/* 应用图标：内核 33 号给 16x16 RGBA（品牌色 + alpha）。返回 kind（<0 = 没有图标） */
static int wm_title_icon(const struct WmSlot* s, int x, int y) {
    const int kind = wl_surface_app_icon((int)s->info.id, g_icon_rgba, (unsigned)sizeof(g_icon_rgba));
    g_title_icon_kind = kind;
    if (kind >= 0) wm_title_log64(s, 0, WM_ICON_PX);
    if (kind < 0) return kind;
    for (int j = 0; j < WM_ICON_PX; j++) {
        for (int i = 0; i < WM_ICON_PX; i++) {
            const unsigned char* p = g_icon_rgba + ((j * WM_ICON_PX) + i) * 4;
            const int a = (int)p[3];
            if (!a) continue;
            const uint32_t fg = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
            wm_blend8(x + i, y + j, fg, a);
        }
    }
    return kind;
}

/* \u88c5\u9970\u6846\uff1a\u6b63\u5e38 = surface \u77e9\u5f62\uff1b\u6298\u53e0 = \u53ea\u7559 25px\uff1b\u6700\u5927\u5316 = \u6574\u5c4f\uff08\u4e0e\u5185\u6838 wl64_frame64 \u540c\u4e00\u628a\u5c3a\u5b50\uff09 */
static void wm_frame(const struct WmSlot* s, int* fx, int* fy, int* fw, int* fh) {
    if (s->info.flags & V64_WM_MINIMIZED) {         /* ★ v0.11.3：Dock 收容 = 整窗摘掉（高 0） */
        *fx = (int)s->info.x; *fy = (int)s->info.y; *fw = (int)s->info.w; *fh = 0; return;
    }
    if (s->info.flags & V64_WM_COLLAPSED) {
        *fx = (int)s->info.x; *fy = (int)s->info.y; *fw = (int)s->info.w; *fh = WM_DECO_HDR; return;
    }
    if (s->info.flags & V64_WM_MAXED) {
        *fx = 0; *fy = 0; *fw = (int)g_fb.width; *fh = (int)g_fb.height; return;
    }
    *fx = (int)s->info.x; *fy = (int)s->info.y; *fw = (int)s->info.w; *fh = (int)s->info.h;
}
/* \u4e09\u6309\u94ae\u547d\u4e2d\uff1a0 = \u6700\u5c0f\u5316 / 1 = \u6700\u5927\u5316 / 2 = \u5173\u95ed / -1 = \u4e0d\u662f\u6309\u94ae */
static int wm_btn_hit(const struct WmSlot* s, int x, int y) {
    if (!(s->info.flags & V64_WM_DECOR)) return -1;
    int fx, fy, fw, fh;
    wm_frame(s, &fx, &fy, &fw, &fh);
    const int bx = fx + fw - 1 - WM_BTN_SLOT * 3;
    for (int b = 0; b < 3; b++) {
        const int bx0 = bx + b * WM_BTN_SLOT;
        if (x >= bx0 && x < bx0 + WM_BTN_SLOT && y >= fy + 1 && y < fy + WM_DECO_HDR - 1) return b;
    }
    return -1;
}
/* \u88c5\u9970\u524d\u534a\uff1a\u6807\u9898\u5e26 + \u6807\u9898\u6587\u5b57 + \u4e09\u6309\u94ae\uff08+\u6700\u5927\u5316\u65f6\u7684\u5ba2\u6237\u533a\u5e95\u8272\uff09 */
static void wm_deco_frame(const struct WmSlot* s) {
    int fx, fy, fw, fh;
    wm_frame(s, &fx, &fy, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;
    wm_fill(fx, fy, fw, WM_DECO_HDR, WM_C_TITLE);
    /* ★ v0.11.4：标题栏左侧先画**应用图标**（内核 33 号给 16x16 RGBA），再画标题文字。
     *   几何与内核 gui64_deco_frame64 **逐像素同一把尺子**：
     *     图标 16x16 @ (fx+6, fy+5)（= 内核的 x+6 / y+BORDER+4）；有图标 -> 文字左移到 fx+26。
     *   标题字形走内核 31 号（覆盖率位图）——**中文标题终于画得出来**；
     *   内核 31 号不可用时才回落 v0.11.3 的 ASCII 字模（wm_ttf.h），如实保留降级路径。 */
    int tx = fx + 8;
    if (s->info.flags & V64_WM_DECOR) {
        if (wm_title_icon(s, fx + 6, fy + 5) >= 0) tx = fx + 26;
    }
    if (s->title[0]) {
        if (wm_title_text(s, tx, fy + 6, fx + fw - 100) < 0)
            wm_text_ttf(tx, fy + 6, s->title, WM_C_GLYPH, fx + fw - 100);
    }
    const int bx = fx + fw - 1 - WM_BTN_SLOT * 3;
    const int by = fy + (WM_DECO_HDR - WM_BTN_H) / 2;
    for (int b = 0; b < 3; b++) {
        const int x = bx + b * WM_BTN_SLOT + (WM_BTN_SLOT - WM_BTN_W) / 2;
        wm_fill(x, by, WM_BTN_W, WM_BTN_H, (b == 2) ? WM_C_CLOSE : WM_C_BTN);
        const int mx = x + WM_BTN_W / 2, my = by + WM_BTN_H / 2;
        if (b == 0) {
            wm_fill(mx - 5, my + 2, 11, 2, WM_C_GLYPH);                       /* \u6700\u5c0f\u5316\uff1a\u77ed\u6a2a\u7ebf */
        } else if (b == 1) {
            wm_fill(mx - 5, my - 5, 11, 1, WM_C_GLYPH);                        /* \u6700\u5927\u5316\uff1a\u7a7a\u5fc3\u65b9\u6846 */
            wm_fill(mx - 5, my + 5, 11, 1, WM_C_GLYPH);
            wm_fill(mx - 5, my - 5, 1, 11, WM_C_GLYPH);
            wm_fill(mx + 5, my - 5, 1, 11, WM_C_GLYPH);
        } else {
            for (int i = -4; i <= 4; i++) {                                    /* 关闭：白✕ */
                wm_put(mx + i, my + i, 0xFFFFFFFFu);
                wm_put(mx - i, my + i, 0xFFFFFFFFu);
            }
        }
    }
    if ((s->info.flags & V64_WM_MAXED) && !(s->info.flags & V64_WM_COLLAPSED)) {
        const uint32_t cb = 0xFF000000u | (s->info.pad & 0x00FFFFFFu);
        wm_fill(fx + 1, fy + WM_DECO_HDR, fw - 2, fh - WM_DECO_HDR - 1, cb);
    }
}
/* \u88c5\u9970\u540e\u534a\uff1a1px \u8fb9\u6846 + \u56db\u89d2\u5706\u89d2\u88c1\u5207\uff08\u5fc5\u987b\u753b\u5728\u5185\u5bb9**\u4e4b\u540e**\uff09 */
static void wm_deco_edge(const struct WmSlot* s) {
    int fx, fy, fw, fh;
    wm_frame(s, &fx, &fy, &fw, &fh);
    if (fw <= 0 || fh <= 0) return;
    wm_fill(fx, fy, fw, 1, WM_C_BORDER);
    wm_fill(fx, fy + fh - 1, fw, 1, WM_C_BORDER);
    wm_fill(fx, fy, 1, fh, WM_C_BORDER);
    wm_fill(fx + fw - 1, fy, 1, fh, WM_C_BORDER);
    /* ★ v0.11.6：程序窗口**只切上面两角**（mask=0x3）—— 底边保持全宽直线（用户规范原文）。 */
    wm_corner_cut_mask(fx, fy, fw, fh, WM_DECO_R, 0x3);
}

/* ==================== ★ v0.11.3：指针由合成器自己画 ====================
 * 为什么（取证见 kernel/gui64.cpp 的 draw_cursor 注释）：合成器每收到一个输入事件就把
 * 窗口矩形重画一遍，而软件光标是后备缓冲里的一层像素 —— 谁后画谁留下。实测悬停在
 * 图片查看器上时，`perf cur=638,398` 那个位置**一个指针像素都没有**（16x16 箭头被整块抹掉），
 * 用户看到的就是"鼠标悬停变成一个透明窟窿"。内核那一层在合成器注册后已经停手
 * （[GUI64] cursor skip …），这里就是指针的**唯一写者**：
 *   * 位置来自同一个输入事件流（EV64 的 x/y 就是屏幕绝对坐标）；
 *   * 每一帧画在**所有窗口/装饰之上**（wm_compose_rect 末尾），永远压在最上层；
 *   * 位置变化的旧矩形 + 新矩形都并进本轮脏区，否则旧箭头会留在屏幕上。
 * 字形与内核软件光标逐行同一份（14 行箭头 + 描边），所以指针外观不会因为
 * "换谁画"而变化 —— 变的只是"谁后画"。 */
#define WM_PTR_W 12
#define WM_PTR_H 16
static const char* const g_wm_ptr[WM_PTR_H] = {
    "X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
    "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
    "X.....XXXXX", "X..X..X    ", "X.X.X..X   ", "XX..X..X   ", "X....X..X  ",
    "X.....X..X "
};
static int g_ptr_x = 0, g_ptr_y = 0, g_ptr_ok = 0;
static void wm_ptr_paint(void) {
    if (!g_ptr_ok) return;
    for (int j = 0; j < WM_PTR_H; j++) {
        const char* r = g_wm_ptr[j];
        for (int i = 0; r[i]; i++) {
            if (r[i] == ' ') break;
            wm_put(g_ptr_x + i, g_ptr_y + j, (r[i] == 'X') ? 0xFF000000u : 0xFFFFFFFFu);
        }
    }
}
/* 指针挪到 (x,y)：旧位置与新位置的包围盒并进脏矩形（-1/+1 给描边留边） */
static void wm_ptr_move(int x, int y, int* ox, int* oy, int* ow, int* oh) {
    if (g_ptr_ok && g_ptr_x == x && g_ptr_y == y) return;
    if (g_ptr_ok) wm_union(ox, oy, ow, oh, g_ptr_x - 1, g_ptr_y - 1, WM_PTR_W + 2, WM_PTR_H + 2);
    g_ptr_x = x; g_ptr_y = y; g_ptr_ok = 1;
    wm_union(ox, oy, ow, oh, g_ptr_x - 1, g_ptr_y - 1, WM_PTR_W + 2, WM_PTR_H + 2);
}
/* 用户态命中测试：最上层的、有内容的、包含 (x,y) 的 surface（与内核 hit64 同一条规则） */
static struct WmSlot* wm_hit(struct WmSlot* slots, int x, int y) {
    struct WmSlot* best = NULL;
    for (int k = 0; k < WM_MAX_SURF; k++) {
        struct WmSlot* s = &slots[k];
        if (!s->used || !(s->info.flags & V64_WM_CONTENT)) continue;
        /* ★ v0.11.2：命中测试用**装饰框**（折叠后只剩 25px；最大化后是整屏） */
        int fx, fy, fw, fh;
        wm_frame(s, &fx, &fy, &fw, &fh);
        if (x < fx || x >= fx + fw) continue;
        if (y < fy || y >= fy + fh) continue;
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
/* ★ v0.11.3：最大化时的缩放合成（与内核 kernel/wl64.cpp 的 wl64_blit_scaled64 **逐式相同**）：
 *   fit + 居中（letterbox）；源 pitch = si->w；仍然做 alpha 混合（与 1:1 路径同一条规则）。 */
static int wm_blit_scaled(const struct WmSlot* self, int x0, int y0, int x1, int y1, int* blended) {
    const struct Wm64SurfaceInfo* si = &self->info;
    int fx, fy, fw, fh;
    wm_frame(self, &fx, &fy, &fw, &fh);
    const int cx0 = fx + 1, cy0 = fy + WM_DECO_HDR;
    const int cw = fw - 2, ch = fh - WM_DECO_HDR - 1;
    const int sw = (int)si->w, sh = (int)si->h;
    if (cw <= 2 || ch <= 2 || sw <= 0 || sh <= 0 || !self->px) return 0;
    const int sxs = cw * 1024 / sw, sys = ch * 1024 / sh;
    const int sc = (sxs < sys) ? sxs : sys;
    int ndw = sw * sc / 1024, ndh = sh * sc / 1024;
    if (ndw < 1) ndw = 1;
    if (ndh < 1) ndh = 1;
    const int ox = cx0 + (cw - ndw) / 2, oy = cy0 + (ch - ndh) / 2;
    const uint32_t* spx = (const uint32_t*)self->px;
    const int ax0 = wm_max(ox, x0), ay0 = wm_max(oy, y0);
    const int ax1 = wm_min(ox + ndw, x1), ay1 = wm_min(oy + ndh, y1);
    if (ax1 <= ax0 || ay1 <= ay0) return 0;
    int written = 0;
    for (int sy = ay0; sy < ay1; sy++) {
        const int src_y = (sy - oy) * sh / ndh;
        const uint32_t* srow = spx + (unsigned long)src_y * (unsigned long)sw;
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        for (int sx = ax0; sx < ax1; sx++) {
            const int src_x = (sx - ox) * sw / ndw;
            const uint32_t v = srow[src_x];
            const uint32_t a = (v >> 24) & 0xFFu;
            if (a == 0xFFu) drow[sx] = v;
            else if (a == 0u) continue;
            else {
                const uint32_t d = drow[sx], ia = 255u - a;
                const uint32_t r = (((((v >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * ia) * 257u) + 257u) >> 16;
                const uint32_t gg = (((((v >> 8) & 0xFFu) * a + ((d >> 8) & 0xFFu) * ia) * 257u) + 257u) >> 16;
                const uint32_t b = ((((v & 0xFFu) * a + (d & 0xFFu) * ia) * 257u) + 257u) >> 16;
                drow[sx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
                (*blended)++;
            }
            written++;
        }
    }
    return written;
}
/* 一块 surface 往脏矩形里 blit（clipped）；调用方保证**从下到上**（id 升序）调用 */
static int wm_blit_slot(const struct WmSlot* self, int x0, int y0, int x1, int y1, int* blended) {
    if (!self->px || !(self->info.flags & V64_WM_HASBUF)) return 0;
    const struct Wm64SurfaceInfo* si = &self->info;
    /* ★ v0.11.2：装饰窗口的客户区从装饰框的 (0, 25) 开始；折叠时一个像素都不合成。
     *   正常框（fx,fy = si->x,si->y）下这两个偏移都是 0 -> 裸 surface 老路径一个像素不变。 */
    int fx, fy, fw, fh;
    wm_frame(self, &fx, &fy, &fw, &fh);
    const int deco = (si->flags & V64_WM_DECOR) != 0;
    if (deco && (si->flags & V64_WM_COLLAPSED)) return 0;
    if (deco && (si->flags & V64_WM_MINIMIZED)) return 0;      // ★ v0.11.3：Dock 收容 -> 不合成
    if (deco && (si->flags & V64_WM_MAXED))                    // ★ v0.11.3：最大化 = 缩放铺满
        return wm_blit_scaled(self, x0, y0, x1, y1, blended);
    const int cy0 = fy + (deco ? WM_DECO_HDR : 0);
    const int chh = (int)si->h - (deco ? WM_DECO_HDR : 0);
    int ax0 = wm_max(fx, x0), ay0 = wm_max(cy0, y0);
    int ax1 = wm_min(fx + (int)si->w, x1), ay1 = wm_min(cy0 + chh, y1);
    if (ax1 <= ax0 || ay1 <= ay0 || chh <= 0) return 0;
    const unsigned pitch_px = si->w;
    const uint32_t* spx = (const uint32_t*)self->px;      /* ★ v0.10.2：uint32 下标代替逐像素指针算术 */
    int written = 0;
    for (int sy = ay0; sy < ay1; sy++) {
        uint32_t* drow = (uint32_t*)((unsigned char*)g_fbpx + (unsigned long)sy * g_fb.pitch);
        const uint32_t* srow = spx + (unsigned long)((sy - cy0) + (deco ? WM_DECO_HDR : 0)) * (unsigned long)pitch_px;
        for (int sx = ax0; sx < ax1; sx++) {
            const uint32_t v = srow[sx - fx];
            const uint32_t a = (v >> 24) & 0xFFu;
            if (a == 0xFFu) {
                drow[sx] = v;
            } else if (a == 0) {
                continue;                                  /* 全透明：保留背景 */
            } else {
                const uint32_t d = drow[sx];
                const uint32_t ia = 255u - a;
                /* /255 -> ((t * 257) + 257) >> 16：t <= 255*255 时与原式逐值相等，省 3 次除法 */
                const uint32_t r = (((((v >> 16) & 0xFFu) * a + ((d >> 16) & 0xFFu) * ia) * 257u) + 257u) >> 16;
                const uint32_t gg = (((((v >> 8) & 0xFFu) * a + ((d >> 8) & 0xFFu) * ia) * 257u) + 257u) >> 16;
                const uint32_t b = ((((v & 0xFFu) * a + (d & 0xFFu) * ia) * 257u) + 257u) >> 16;
                drow[sx] = 0xFF000000u | (r << 16) | (gg << 8) | b;
                (*blended)++;
            }
            written++;
        }
    }
    return written;
}

/* 把一个脏矩形重新合成：② 按 id 升序（从下到上）重画所有窗口（① 的快照回填在外面做，
 * 这样可以分别计时：`[WM] frame .. bg_us=`）*/
static int wm_blit_rect(struct WmSlot* slots, int x0, int y0, int x1, int y1, int* blended) {
    if (x1 <= x0 || y1 <= y0) return 0;
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
/* ★ v0.11.2：**合成一段矩形**（不回填背景）—— 装饰前半 -> 客户区内容 -> 装饰后半。
 *   与内核窗口 draw_window 的顺序一致；层级仍是 id 升序（后创建的在上）。
 *   为什么单独抽出来：主合成路径（⑨）与"输入后补画"路径必须走同一段代码，
 *   否则会出现"主路径画了内容、补画路径画了装饰"的半截状态（本批实测踩过：
 *   第一版只改了 wm_paint_rect，主路径仍直调 wm_blit_rect —— 屏幕上标题带全黑）。 */
static int wm_compose_rect(struct WmSlot* slots, int x0, int y0, int x1, int y1, int* blended) {
    if (x1 <= x0 || y1 <= y0) return 0;
    int order[WM_MAX_SURF], n = 0;
    for (int i = 0; i < WM_MAX_SURF; i++) if (slots[i].used) order[n++] = i;
    for (int a = 0; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (slots[order[b]].info.id < slots[order[a]].info.id) {
                const int t = order[a]; order[a] = order[b]; order[b] = t;
            }
    g_clip_x0 = wm_max(x0, 0); g_clip_y0 = wm_max(y0, 0);
    g_clip_x1 = wm_min(x1, (int)g_fb.width); g_clip_y1 = wm_min(y1, (int)g_fb.height);
    int px = 0;
    for (int a = 0; a < n; a++) {
        struct WmSlot* s = &slots[order[a]];
        const int deco = (s->info.flags & V64_WM_DECOR) != 0;
        if (deco) wm_deco_frame(s);
        px += wm_blit_slot(s, x0, y0, x1, y1, blended);
        if (deco) wm_deco_edge(s);
    }
    wm_ptr_paint();          /* ★ v0.11.3：指针永远压在本帧最上层（clipped 到本矩形） */
    return px;
}
static int wm_paint_rect(struct WmSlot* slots, int x0, int y0, int x1, int y1, int* blended) {
    if (x1 <= x0 || y1 <= y0) return 0;
    wm_bg_restore(x0, y0, x1, y1);
    return wm_compose_rect(slots, x0, y0, x1, y1, blended);
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
        /* ★ v0.10.2（帧预算）：验收基准（每次启动一行）。
           判据：整屏 4 MiB 快照回填 bgfull_us < 20,000 µs。
           4 KiB 页时实测 519,000~565,000 µs（跨 1000 个页），2 MiB 大页后应为个位数毫秒。 */
        if (g_have_bg && wm_probe_named(WM_BENCH)) {
            static unsigned char b4k[4096];
            unsigned long long c4k_min = ~0ULL, c4k_max = 0, bf_min = ~0ULL, bf_max = 0;
            unsigned cks = 0;
            int npx = 0;
            for (int rep = 0; rep < 8; rep++) {
                unsigned long long t0 = wm_rdtsc();
                wm_copy(b4k, g_bg[0], sizeof(b4k));
                const unsigned long long d = wm_us(wm_rdtsc() - t0);
                if (d < c4k_min) c4k_min = d;
                if (d > c4k_max) c4k_max = d;
                cks += b4k[rep];
                t0 = wm_rdtsc();
                npx = wm_bg_restore(0, 0, (int)g_fb.width, (int)g_fb.height);
                const unsigned long long d2 = wm_us(wm_rdtsc() - t0);
                if (d2 < bf_min) bf_min = d2;
                if (d2 > bf_max) bf_max = d2;
            }
            printf("[WM] bench bgfull_px=%d bgfull_us_min=%lu bgfull_us_max=%lu "
                   "copy4k_us_min=%lu copy4k_us_max=%lu cks=%u\n",
                   npx, (unsigned long)bf_min, (unsigned long)bf_max,
                   (unsigned long)c4k_min, (unsigned long)c4k_max, cks);
            /* ★ 足迹扫描（每档 6 次取最小）：4 KiB / 64 KiB / 256 KiB / 1 MiB / 3 MiB。
               目的：把"每 MB 成本随足迹超线性"这件事钉在证据上（见 docs/桌面帧预算.md）。 */
            {
                static const unsigned kSz[5] = { 4096u, 65536u, 262144u, 1048576u, 3145728u };
                unsigned c4[5];
                unsigned sum = 0;
                for (int k = 0; k < 5; k++) {
                    unsigned long long best = ~0ULL;
                    for (int rep = 0; rep < 6; rep++) {
                        const unsigned long long s0 = wm_rdtsc();
                        wm_copy(g_fbpx, g_bg[0], kSz[k]);      /* 快照 -> 帧缓冲（真实路径的方向） */
                        const unsigned long long d = wm_us(wm_rdtsc() - s0);
                        if (d < best) best = d;
                        sum += g_fbpx[rep];
                    }
                    c4[k] = (unsigned)best;
                }
                printf("[WM] bench scale us_4k=%u us_64k=%u us_256k=%u us_1m=%u us_3m=%u cks=%u\n",
                       c4[0], c4[1], c4[2], c4[3], c4[4], sum);
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
    /* ★ v0.10.6（拖拽残影根治）：⑪（输入）阶段算出的脏矩形发生在 ⑨（合成）之后，而 dmg_*
     *   是**本轮局部变量**。只要本轮 ⑨ 已经画过一帧（客户端刚好提交了），这批脏矩形就会被
     *   静默丢掉 —— 拖动时"旧位置还给桌面"那一步永远回不来，屏幕上留下一条 **24 px**
     *   （= 一次鼠标步长）宽的旧标题栏拖影（wmfull64_test H② 的 1/6 概率残留）。
     *   所以 ⑪ 的脏矩形走独立的 odmg_*；本轮画不掉就存进 carry_*，下一轮 ⑨ 必定画掉。 */
    int carry_x = 0, carry_y = 0, carry_w = 0, carry_h = 0;

    for (;;) {
        const int now_ms = (int)vimtu64_ticks() * 4;
        const int elapsed = now_ms - t0_ms;

        int n_used = 0, n_pending = 0;
        int dmg_x = carry_x, dmg_y = carry_y, dmg_w = carry_w, dmg_h = carry_h;
        const int from_carry = (carry_w > 0 && carry_h > 0);
        carry_x = carry_y = carry_w = carry_h = 0;
        int layout = 0;
        int painted_this_iter = 0;      /* ★ v0.10.2：本轮的 ⑨ 是否已经画过一帧 */
        int odmg_x = 0, odmg_y = 0, odmg_w = 0, odmg_h = 0;   /* ★ v0.10.6：⑪ 输入阶段算出的脏矩形 */

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
            if (is_new) { slots[i].placed = 0; slots[i].logged = 0; slots[i].map_shm = -1;
                          slots[i].title_got = 0; slots[i].title[0] = 0; }
            n_used++;

            /* ★ v0.11.2：装饰窗口的标题由合成器画 —— 用 29 号从内核读回来（每个 surface 只读一次） */
            if ((info.flags & V64_WM_DECOR) && !slots[i].title_got) {
                if (wl_surface_title((int)info.id, slots[i].title, sizeof(slots[i].title)) > 0) {
                    slots[i].title_got = 1;
                    printf("[WM] title id=%d text=\"%s\"\n", (int)info.id, slots[i].title);
                }
            }

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

        /* ★ v0.11.3：第一帧整屏重画一次 —— 内核在合成器注册后不再画指针，它留在后备缓冲里的
         *   最后一把箭头（以及启动期没画过的桌面底色）需要一次整屏合成来清干净。
         *   只做一次（painted_any 之后不再强制整屏），不改变帧预算的任何稳态数字。 */
        if (!painted_any) wm_union(&dmg_x, &dmg_y, &dmg_w, &dmg_h, 0, 0, (int)g_fb.width, (int)g_fb.height);
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
            const unsigned long long t_bg = wm_rdtsc();
            const int bgpx = wm_bg_restore(dmg_x, dmg_y, dmg_x + dmg_w, dmg_y + dmg_h);
            const unsigned long long bg_us = wm_us(wm_rdtsc() - t_bg);
            const unsigned long long t_c0 = wm_rdtsc();
            int blend = 0;
            const int px = wm_compose_rect(slots, dmg_x, dmg_y, dmg_x + dmg_w, dmg_y + dmg_h, &blend);
            const unsigned long long us = wm_us(wm_rdtsc() - t_c0);

            const int use_dev = 0;      /* 22 号的 gpu 位为 0 -> 恒 soft，如实写 */
            const unsigned long long t_f0 = wm_rdtsc();
            const int frc = fb_flip(dmg_x, dmg_y, dmg_w, dmg_h);
            const unsigned long long flip_us = wm_us(wm_rdtsc() - t_f0);
            n_frame++;
            painted_any = 1;
            painted_this_iter = 1;
            last_repair_ms = now_ms;
            /* ★ 只给"有内容变化"的帧留证：纯修复帧（每 150 ms 一次，没有任何客户端提交）不打 ——
             *   每一行都会让 ring0 终端滚动重绘一次，几百行就把客户机拖到 1 fps，进而把夹具的
             *   光标闭环反馈拖死（实测）。layout / pending / 拖动中 / 第一帧 仍然照打。 */
            const int want_frame_log = (n_frame == 1 || layout || n_pending || g_drag_id
                                        || from_carry);   /* ★ v0.10.6：补画 carry 的帧必须留证 */
            g_frame_log++;
            /* 前 WM_FRAME_LOG_MAX 帧逐帧留证；之后每 16 帧一条（60 fps 下 ~4 行/秒） */
            const int do_frame_log = from_carry || (want_frame_log &&
                (g_frame_log <= WM_FRAME_LOG_MAX || (g_frame_log & 15u) == 0u));
            /* ★ v0.10.6：from_carry 的帧**不采样**（一轮最多 1~2 条）—— 它正是"旧位置被补画
             *   回去"的唯一日志证据，被采样吞掉的话未来回归无法自证。 */
            if (do_frame_log) {
                printf("[WM] frame n=%lu surfs=%d pending=%d dirty=%d,%d,%d,%d px=%d blend=%d us=%lu flip_us=%lu path=%s full=%d foreign=0 bg_us=%lu t_ms=%lu\n",
                       n_frame, n_used, n_pending, dmg_x, dmg_y, dmg_w, dmg_h, px, blend,
                       (unsigned long)us, (unsigned long)flip_us, use_dev ? "dev" : "soft", layout ? 1 : 0,
                       (unsigned long)bg_us, (unsigned long)(vimtu64_ticks() * 4));
                if (g_frame_log <= WM_FRAME_LOG_MAX)
                    printf("[WM] bg px=%d bg_us=%lu us=%lu flip_us=%lu\n",
                           bgpx, (unsigned long)bg_us, (unsigned long)us, (unsigned long)flip_us);
            }
            if (frc != 0) printf("[WM] frame flip rc=%d\n", frc);
            if (n_frame == 1)
                printf("[WM] blitpolicy gpu=0 min_px=%d rect_px=%d path=soft\n", (int)WM_DEV_MIN_PX, px);

            /* ---- ⑩ 确认：告诉内核"这条我合成完了"（清 pending/damage + content=1）---- */
            for (int i = 0; i < WM_MAX_SURF; i++) {
                if (!slots[i].used) continue;
                if (!(slots[i].info.flags & V64_WM_PENDING)) continue;
                if (wl_surface_ack(slots[i].info.id) == 0 && g_ack_log++ < 16u)
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
                    /* ★ v0.11.3：指针位置跟着同一个事件流走。旧/新两个 16x16 矩形都并进
                     *   odmg_*（否则旧箭头留在屏上、新位置又被窗口重画抹掉 —— 那正是
                     *   "悬停变透明窟窿"的成因）。滚轮事件也带坐标，一并跟进。 */
                    if (is_move || is_down || is_up || raw[i].type == V64_EV_WHEEL)
                        wm_ptr_move((int)raw[i].x, (int)raw[i].y,
                                    &odmg_x, &odmg_y, &odmg_w, &odmg_h);

                    int consumed = 0;
                    if (is_down && (raw[i].code & 1u)) {
                        /* ★ v0.11.2：先判**三按钮**（真实语义走 30 号：关闭 / 折叠 / 最大化），
                         *   不是按钮才当拖动。两者都**不投给客户端**（否则 PE 的 OK 判定会被
                         *   标题栏上的一下点击弄困惑）。 */
                        struct WmSlot* hs = wm_hit(slots, raw[i].x, raw[i].y);
                        if (hs) {
                            const int b = wm_btn_hit(hs, raw[i].x, raw[i].y);
                            if (b >= 0) {
                                /* ★ v0.11.3：最小化按钮 = 收容到 Dock（整窗摘掉；Dock 那条运行项
                                 *   出现"最小化横杠"，点 Dock 项走 RESTORE 逐像素还原）。 */
                                const int act = (b == 0) ? V64_WL_CTL_MIN2DOCK
                                              : (b == 1) ? V64_WL_CTL_MAX : V64_WL_CTL_CLOSE;
                                const int rc = wl_surface_ctl((int)hs->info.id, act);
                                printf("[WM] btn id=%d which=%s action=%s rc=%d\n", (int)hs->info.id,
                                       (b == 0) ? "min" : (b == 1) ? "max" : "close",
                                       (b == 0) ? "minimize" : (b == 1) ? "maximize" : "close", rc);
                                int fx, fy, fw, fh;
                                wm_frame(hs, &fx, &fy, &fw, &fh);
                                wm_union(&odmg_x, &odmg_y, &odmg_w, &odmg_h, fx, fy, fw, fh);
                                consumed = 1;
                            }
                        }
                        if (!consumed && hs && (raw[i].y - (int)hs->info.y) < WM_DRAG_BAR) {
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
                                    if (g_dragmove_log++ < 12u)
                                      printf("[WM] drag move id=%d pos=%d,%d dx=%d dy=%d\n",
                                             g_drag_id, nx, ny, nx - ox, ny - oy);
                                    /* 旧位置还给桌面 + 新位置重画（★ v0.10.6 -> odmg_*，
                                     *   本轮 ⑨ 已画过就交给 carry_*，绝不丢） */
                                    wm_union(&odmg_x, &odmg_y, &odmg_w, &odmg_h,
                                             ox, oy, (int)ds->info.w, (int)ds->info.h);
                                    wm_union(&odmg_x, &odmg_y, &odmg_w, &odmg_h,
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

                    /* 事件照旧投回客户端（拖动期间也投：客户端的按钮判定只看 UP 落在哪）。
                     * ★ v0.11.2：标题栏按钮那一下已经被合成器吃掉（consumed），不再投。 */
                    if (consumed) {
                        /* 合成器自己处理完了 */
                    } else if (is_key) {
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
                        if (!is_key && g_route_surf_logged < WM_ROUTE_LOG_MAX) {
                            g_route_surf_logged++;
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
            /* 收到输入：外壳很可能刚把光标画进我们的窗口里 -> 立刻整窗重画一遍。
             * ★ v0.10.6：本轮 ⑨ 已经画过一帧时**不能再画**（会把这一轮的帧预算翻倍），
             *   但 odmg_* 也**绝不能丢** —— 交给 carry_*，下一轮 ⑨ 一定画掉。 */
            if (got_any) {
                if (!painted_this_iter) {
                    for (int i = 0; i < WM_MAX_SURF; i++) {
                        if (!slots[i].used) continue;
                        wm_union(&odmg_x, &odmg_y, &odmg_w, &odmg_h,
                                 slots[i].info.x, slots[i].info.y, (int)slots[i].info.w, (int)slots[i].info.h);
                    }
                    if (odmg_w > 0 && odmg_h > 0) {
                        int x0 = wm_max(odmg_x, 0), y0 = wm_max(odmg_y, 0);
                        int x1 = wm_min(odmg_x + odmg_w, (int)g_fb.width), y1 = wm_min(odmg_y + odmg_h, (int)g_fb.height);
                        if (x1 > x0 && y1 > y0) {
                            int blend2 = 0;
                            const int px2 = wm_paint_rect(slots, x0, y0, x1, y1, &blend2);
                            (void)fb_flip(x0, y0, x1 - x0, y1 - y0);
                            n_frame++;
                            painted_this_iter = 1;
                            last_repair_ms = now_ms;
                            printf("[WM] frame n=%lu surfs=%d pending=0 dirty=%d,%d,%d,%d px=%d blend=%d us=0 flip_us=0 path=soft full=0 foreign=0 input=1 t_ms=%lu\n",
                                   n_frame, n_used, x0, y0, x1 - x0, y1 - y0, px2, blend2,
                                   (unsigned long)(vimtu64_ticks() * 4));
                            wm_remember(slots);
                            odmg_x = odmg_y = odmg_w = odmg_h = 0;   /* 已经画掉了，别再 carry */
                        }
                    }
                }
                if (odmg_w > 0 && odmg_h > 0) {                 /* 本轮画不掉 -> 下一轮必画 */
                    carry_x = odmg_x; carry_y = odmg_y;
                    carry_w = odmg_w; carry_h = odmg_h;
                }
            }
        }
        /* ---- ⑬ 帧节拍：一帧画完还早 -> 把余量睡掉（4 ms 粒度）。
         *   不做这件事的话，合成器会把 100% CPU 烧在轮询上（实测拖动时每 6 ms 就刷一帧），
         *   既抢掉客户端的时间片，也让输入事件无法聚合到同一帧。 */
        if (painted_this_iter && WM_PACE_MS > 0) {
            const int spend_ms = (int)(vimtu64_ticks() * 4) - now_ms;
            if (spend_ms >= 0 && spend_ms < WM_PACE_MS) {
                const int left = (WM_PACE_MS - spend_ms) & ~3;
                if (left > 0) vimtu64_sleep_ms((unsigned long)left);
            }
        } else if (!dmg_w && n_pending == 0) {
            vimtu64_sleep_ms(WM_FRAME_MS);
        }
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
