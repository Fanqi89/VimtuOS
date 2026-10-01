/* wmpanel.c - ★ B-wm 的**客户端 2**：Ring 3 可交互面板（shm surface，经 /bin/wm 上屏）
 *
 * 要证明的事：
 *   * 输入事件是**用户态路由**的：/bin/wm 用 input_poll 收事件、自己做命中测试、再
 *     wl_seat_post 把事件投进本进程的队列（内核不猜"这条给谁"）；本进程 wl_display_dispatch
 *     取到事件 -> 按点击切换开关状态（像素变化）；
 *   * 收到 seat 事件后**只 damage 自己那一块**（开关矩形 + 状态条），不整面重画；
 *   * 半透明像素（alpha=0x80）真的走了合成器的 **alpha 混合**路径（[WM] frame .. blend=<n>）。
 * 打点（tests/wm64_test.py grep；格式勿改）：
 *   [WMPANEL] mode full=<0|1>
 *   [WMPANEL] surf id=<n> w=<n> h=<n> shm=<n> va=0x<hex>
 *   [WMPANEL] frame f=<n> state=<n> col=0x<hex> dmg=<x>,<y>,<w>,<h>
 *   [WMPANEL] seat n=<n> surf=<n> type=<n> code=0x<hex> sx=<n> sy=<n> inside=<0|1>
 *   [WMPANEL] click surf=<n> sx=<n> sy=<n> state=<n> col=0x<hex> dmg=<x>,<y>,<w>,<h>
 *   [WMPANEL] destroy id=<n> rc=<n> ; [WMPANEL] done clicks=<n>
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"

#define PW          128
#define PH          64
#define PW_BYTES    (PW * PH * 4u)          /* 32768 B（8 页） */
#define PROBE       "/etc/wm_probe"
#define EV_MAX      32u

#define P_BG     0x1A2028u                  /* 底 */
#define P_TITLE  0x303A48u                  /* 标题条 */
#define P_ON     0x30B070u                  /* 开关 开（绿） */
#define P_OFF    0xE0A020u                  /* 开关 关（橙） */
#define P_SW_X   16                         /* 开关矩形（局部坐标） */
#define P_SW_Y   22
#define P_SW_W   48
#define P_SW_H   30
#define P_VEIL   0x80                       /* 半透明覆盖条：alpha=0x80 -> 合成器要混合 */
#define P_VEIL_Y 50                         /* 半透明条所在行（高度 = PH-6-50 = 8 行，便于取样） */

static unsigned char* g_buf;
static int  g_state = 0;
static int  g_frame = 0;

static inline void put_alpha(unsigned x, unsigned y, unsigned rgb, unsigned a) {
    if (x >= PW || y >= PH) return;
    *(unsigned*)(g_buf + (unsigned long)y * PW * 4u + (unsigned long)x * 4u) =
        ((a & 0xFFu) << 24) | (rgb & 0xFFFFFFu);
}
static inline void put(unsigned x, unsigned y, unsigned rgb) { put_alpha(x, y, rgb, 0xFFu); }

/* 整面（开/关两种状态 + 半透明覆盖条） */
static void draw_all(void) {
    for (int y = 0; y < PH; y++)
        for (int x = 0; x < PW; x++) put((unsigned)x, (unsigned)y, (y < 14) ? P_TITLE : P_BG);
    const unsigned col = g_state ? P_ON : P_OFF;
    for (int y = P_SW_Y; y < P_SW_Y + P_SW_H; y++)
        for (int x = P_SW_X; x < P_SW_X + P_SW_W; x++)
            put((unsigned)x, (unsigned)y, ((x < P_SW_X + 2) || (x >= P_SW_X + P_SW_W - 2) ||
                                           (y < P_SW_Y + 2) || (y >= P_SW_Y + P_SW_H - 2)) ? 0xF0F0F0u : col);
    /* 状态条（开关右边）：开 = 亮绿，关 = 亮橙 */
    for (int y = P_SW_Y + 4; y < P_SW_Y + 20; y++)
        for (int x = P_SW_X + P_SW_W + 10; x < P_SW_X + P_SW_W + 22; x++)
            put((unsigned)x, (unsigned)y, col);
    /* 半透明覆盖条（alpha=0x80）：合成时与底色混合 -> [WM] frame .. blend>0 */
    for (int y = P_VEIL_Y; y < PH - 4; y++)
        for (int x = 4; x < PW - 4; x++) put_alpha((unsigned)x, (unsigned)y, 0xFFFFFFu, P_VEIL);
}

static int probe_exists(void) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)PROBE, 0, 0, 0, 0) == 0 ? 1 : 0;
}

int main(void) {
    const int full = probe_exists();
    printf("[WMPANEL] mode full=%d\n", full);

    const int surf = wl_surface_create(PW, PH, V64_WL_FORMAT_XRGB8888);
    if (surf < 0) { printf("[WMPANEL] FAILED surface rc=%d\n", surf); return 1; }
    const int shm = shm_create(PW_BYTES);
    if (shm < 0) { printf("[WMPANEL] FAILED shm rc=%d\n", shm); return 1; }
    void* va = NULL;
    if (shm_map(shm, 0, PW_BYTES, &va) != 0 || !va) { printf("[WMPANEL] FAILED shm_map\n"); return 1; }
    g_buf = (unsigned char*)va;
    printf("[WMPANEL] surf id=%d w=%d h=%d shm=%d va=0x%x\n", surf, PW, PH, shm,
           (unsigned)(unsigned long)va);

    draw_all();
    if (wl_surface_attach(surf, shm, 0) != 0) { printf("[WMPANEL] FAILED attach\n"); return 1; }
    if (wl_surface_damage(surf, 0, 0, PW, PH) != 0) { printf("[WMPANEL] FAILED damage\n"); return 1; }
    if (wl_surface_commit(surf) != 0) { printf("[WMPANEL] FAILED commit\n"); return 1; }
    printf("[WMPANEL] frame f=%d state=%d col=0x%x dmg=%d,%d,%d,%d\n",
           g_frame, g_state, (unsigned)(g_state ? P_ON : P_OFF), 0, 0, PW, PH);
    g_frame++;

    const int clicks_want = full ? 2 : 0;
    int clicks = 0;
    const unsigned long long t0 = vimtu64_ticks();
    const unsigned long long limit = full ? 6000u : 750u;      /* 24 s / 3 s（250Hz） */
    while (clicks < clicks_want && (vimtu64_ticks() - t0) < limit) {
        struct Wl64SeatEvent evs[EV_MAX];
        const int n = wl_display_dispatch(60, evs, EV_MAX);
        for (int i = 0; i < n && i < (int)EV_MAX; i++) {
            printf("[WMPANEL] seat n=%d surf=%d type=%d code=0x%x sx=%d sy=%d inside=%d\n",
                   i, (int)evs[i].surf, (int)evs[i].ev.type, (unsigned)evs[i].ev.code,
                   (int)evs[i].sx, (int)evs[i].sy, (int)evs[i].inside);
            /* 只有"路由到本表面 && 坐标在里面 && 左键按下"才切换（其余事件只看不动） */
            if (evs[i].surf != (unsigned)surf || !evs[i].inside) continue;
            if (evs[i].ev.type != V64_EV_MOUSE_DOWN) continue;
            if (!(evs[i].ev.code & V64_EV_BTN_LEFT)) continue;
            g_state = !g_state;
            clicks++;
            /* 只 damage 开关 + 状态条那一块（其余像素一个字都不动） */
            int dx = P_SW_X, dy = P_SW_Y;
            int dw = (P_SW_X + P_SW_W + 22) - P_SW_X;
            int dh = P_SW_H;
            const unsigned col = g_state ? P_ON : P_OFF;
            for (int y = P_SW_Y; y < P_SW_Y + P_SW_H; y++)
                for (int x = P_SW_X; x < P_SW_X + P_SW_W; x++)
                    put((unsigned)x, (unsigned)y, ((x < P_SW_X + 2) || (x >= P_SW_X + P_SW_W - 2) ||
                                                   (y < P_SW_Y + 2) || (y >= P_SW_Y + P_SW_H - 2)) ? 0xF0F0F0u : col);
            for (int y = P_SW_Y + 4; y < P_SW_Y + 20; y++)
                for (int x = P_SW_X + P_SW_W + 10; x < P_SW_X + P_SW_W + 22; x++)
                    put((unsigned)x, (unsigned)y, col);
            if (wl_surface_attach(surf, shm, 0) != 0) { printf("[WMPANEL] FAILED attach\n"); break; }
            if (wl_surface_damage(surf, dx, dy, dw, dh) != 0) { printf("[WMPANEL] FAILED damage\n"); break; }
            if (wl_surface_commit(surf) != 0) { printf("[WMPANEL] FAILED commit\n"); break; }
            printf("[WMPANEL] click surf=%d sx=%d sy=%d state=%d col=0x%x dmg=%d,%d,%d,%d\n",
                   surf, (int)evs[i].sx, (int)evs[i].sy, g_state, (unsigned)col, dx, dy, dw, dh);
            printf("[WMPANEL] frame f=%d state=%d col=0x%x dmg=%d,%d,%d,%d\n",
                   g_frame, g_state, (unsigned)col, dx, dy, dw, dh);
            g_frame++;
        }
    }
    const int drc = wl_surface_destroy(surf);
    printf("[WMPANEL] destroy id=%d rc=%d\n", surf, drc);
    printf("[WMPANEL] done clicks=%d frames=%d\n", clicks, g_frame);
    return 0;
}
