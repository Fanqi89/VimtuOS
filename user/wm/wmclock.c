/* wmclock.c - ★ B-wm 的**客户端 1**：Ring 3 时钟面板（shm surface，经 /bin/wm 上屏）
 *
 * 要证明的事：
 *   * 客户端在**自己的用户态共享缓冲**里画（shm_create + shm_map），提交走 wl_surface_commit；
 *     **它自己不 fb_flip**（屏幕由 /bin/wm 合成 -> 这是"合成器在用户态"的关键分工）；
 *   * **每 5 秒更新一次数字，且只 damage 变化的那几个字形**（帧日志给出 damage 矩形，
 *     tests/wm64_test.py 拿它去断言"帧差 ⊆ damage 矩形、damage 框外逐像素不变"）。
 * 打点（tests/wm64_test.py grep；格式勿改）：
 *   [WMCLOCK] mode full=<0|1>
 *   [WMCLOCK] surf id=<n> w=<n> h=<n> shm=<n> va=0x<hex>        （一次性）
 *   [WMCLOCK] frame f=<n> sec=<n> dmg=<x>,<y>,<w>,<h> ink=0x<hex> bar=0x<hex>
 *   [WMCLOCK] seat n=<n> surf=<n> type=<n> sx=<n> sy=<n> inside=<0|1>
 *   [WMCLOCK] destroy id=<n> rc=<n>
 *   [WMCLOCK] done frames=<n>
 * 说明：只开**一块**缓冲（单缓冲）+ 每次提交后等 dispatch 返回再改下一帧 —— wm 合成完会 ack，
 *   ack 之后内核才把 damage 清掉；客户端在两次提交之间不去写"已经提交过的"那块（见 wl64.h 语义 3）。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"

#define CW          160                      /* surface 宽 */
#define CH          96                       /* surface 高 */
#define CW_BYTES    (CW * CH * 4u)           /* 61440 B（15 页，<= 单对象 64 KiB 上限） */
#define EV_MAX      32u
#define PROBE       "/etc/wm_probe"

#define C_BG   0x101820u                      /* 面板底 */
#define C_BAR  0x283040u                      /* 顶部条 */
#define C_BTMC 0x40C0FFu                      /* 底部色带（固定色 -> 测试的取样点） */
#define C_INK  0xE0F0FFu                      /* 数字笔画色 */
#define INK_X  8                              /* 数字区左上 */
#define INK_Y  30
#define INK_S  4                              /* 字形放大倍数（5x7 * 4 = 20x28） */
#define GLY_W  5
#define GLY_H  7
#define GLY_DX 24                             /* 字形步进（含间距） */

static unsigned char* g_buf;

/* 5x7 数字字形（bit4..bit0 = 从左到右 5 列；10 = ':'） */
static const unsigned char FONT[11][GLY_H] = {
    { 0x1E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x1E },   /* 0 */
    { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   /* 1 */
    { 0x1E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   /* 2 */
    { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   /* 3 */
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   /* 4 */
    { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   /* 5 */
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   /* 6 */
    { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   /* 7 */
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   /* 8 */
    { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   /* 9 */
    { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 }    /* ':' */
};

static inline void put(unsigned x, unsigned y, unsigned rgb) {
    if (x >= CW || y >= CH) return;
    *(unsigned*)(g_buf + (unsigned long)y * CW * 4u + (unsigned long)x * 4u) = 0xFF000000u | (rgb & 0xFFFFFFu);
}

/* 画一个字形（glyph 下标 0..9，10 = ':'），左上角 (x,y)，放大 s 倍 */
static void glyph(int idx, int x, int y, int s, unsigned rgb) {
    for (int r = 0; r < GLY_H; r++) {
        const unsigned char bits = FONT[idx][r];
        for (int c = 0; c < GLY_W; c++) {
            if (!(bits & (1u << (GLY_W - 1 - c)))) continue;
            for (int dy = 0; dy < s; dy++)
                for (int dx = 0; dx < s; dx++) put((unsigned)(x + c * s + dx), (unsigned)(y + r * s + dy), rgb);
        }
    }
}

/* 静态底：底色 + 顶条 + 底部色带（这些每一帧都一样 -> 不进 damage） */
static void draw_static(void) {
    for (int y = 0; y < CH; y++)
        for (int x = 0; x < CW; x++) {
            unsigned c = C_BG;
            if (y < 14) c = C_BAR;
            if (y >= CH - 14) c = C_BTMC;
            put((unsigned)x, (unsigned)y, c);
        }
}

/* 画 MM:SS（digits = {m0,m1,s0,s1}） */
static void draw_digits(const int d[4]) {
    const int x0 = INK_X, y0 = INK_Y, s = INK_S;
    glyph(d[0], x0 + 0 * GLY_DX, y0, s, C_INK);
    glyph(d[1], x0 + 1 * GLY_DX, y0, s, C_INK);
    glyph(10,   x0 + 2 * GLY_DX + 4, y0, s, C_INK);
    glyph(d[2], x0 + 3 * GLY_DX, y0, s, C_INK);
    glyph(d[3], x0 + 4 * GLY_DX, y0, s, C_INK);
}

/* 第 i 个字形的**屏幕局部**矩形（damage 用） */
static void glyph_rect(int i, int* x, int* y, int* w, int* h) {
    *x = INK_X + i * GLY_DX - 2;
    *y = INK_Y - 2;
    *w = GLY_W * INK_S + 4;
    *h = GLY_H * INK_S + 4;
    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
}

static int probe_exists(void) {
    return __v64_syscall(21 /*access*/, (long)(unsigned long)PROBE, 0, 0, 0, 0) == 0 ? 1 : 0;
}

int main(void) {
    const int full = probe_exists();
    printf("[WMCLOCK] mode full=%d\n", full);

    const int surf = wl_surface_create(CW, CH, V64_WL_FORMAT_XRGB8888);
    if (surf < 0) { printf("[WMCLOCK] FAILED surface rc=%d\n", surf); return 1; }
    const int shm = shm_create(CW_BYTES);
    if (shm < 0) { printf("[WMCLOCK] FAILED shm rc=%d\n", shm); return 1; }
    void* va = NULL;
    if (shm_map(shm, 0, CW_BYTES, &va) != 0 || !va) { printf("[WMCLOCK] FAILED shm_map\n"); return 1; }
    g_buf = (unsigned char*)va;
    printf("[WMCLOCK] surf id=%d w=%d h=%d shm=%d va=0x%x\n", surf, CW, CH, shm,
           (unsigned)(unsigned long)va);

    draw_static();
    /* 第一帧：整面提交（屏幕上有内容） */
    int prev[4] = { -1, -1, -1, -1 };
    const int frames = full ? 5 : 2;
    int done = 0;
    for (int f = 0; f < frames; f++) {
        const int sec = f * 5;                       /* 每帧 5 秒 */
        int d[4] = { (sec / 60) / 10 % 10, (sec / 60) % 10, (sec % 60) / 10, sec % 10 };
        /* 擦掉上一帧的笔画再重画（只碰字形格 -> 也就是只碰下面算出来的 damage 矩形） */
        int changed = 0, cx = 0, cy = 0, cw = 0, ch = 0;
        for (int i = 0; i < 4; i++) {
            if (prev[i] == d[i]) continue;
            int gx, gy, gw, gh;
            glyph_rect(i, &gx, &gy, &gw, &gh);
            for (int y = gy; y < gy + gh; y++)
                for (int x = gx; x < gx + gw; x++) put((unsigned)x, (unsigned)y, C_BG);
            changed = 1;
            if (!cw) { cx = gx; cy = gy; cw = gw; ch = gh; }
            else {
                const int x1 = (cx + cw > gx + gw) ? cx + cw : gx + gw;
                const int y1 = (cy + ch > gy + gh) ? cy + ch : gy + gh;
                if (gx < cx) cx = gx;
                if (gy < cy) cy = gy;
                cw = x1 - cx; ch = y1 - cy;
            }
        }
        draw_digits(d);
        if (wl_surface_attach(surf, shm, 0) != 0) { printf("[WMCLOCK] FAILED attach\n"); break; }
        if (changed) {
            if (wl_surface_damage(surf, cx, cy, cw, ch) != 0) { printf("[WMCLOCK] FAILED damage\n"); break; }
        } else {
            if (wl_surface_damage(surf, 0, 0, CW, CH) != 0) { printf("[WMCLOCK] FAILED damage\n"); break; }
            cx = 0; cy = 0; cw = CW; ch = CH;
        }
        if (wl_surface_commit(surf) != 0) { printf("[WMCLOCK] FAILED commit\n"); break; }
        printf("[WMCLOCK] frame f=%d sec=%d dmg=%d,%d,%d,%d ink=0x%x bar=0x%x\n",
               f, sec, cx, cy, cw, ch, (unsigned)C_INK, (unsigned)C_BTMC);
        for (int i = 0; i < 4; i++) prev[i] = d[i];
        done++;
        /* 收 seat 事件（wm 把命中本 surface 的事件投回来）；同时给 wm 时间合成 */
        struct Wl64SeatEvent evs[EV_MAX];
        const unsigned long long t_end = vimtu64_ticks() + (f == 0 ? 500u : 1250u);   /* 2s / 5s（250Hz） */
        while (vimtu64_ticks() < t_end) {
            const int n = wl_display_dispatch(50, evs, EV_MAX);
            for (int i = 0; i < n && i < (int)EV_MAX; i++)
                printf("[WMCLOCK] seat n=%d surf=%d type=%d sx=%d sy=%d inside=%d\n",
                       i, (int)evs[i].surf, (int)evs[i].ev.type, (int)evs[i].sx, (int)evs[i].sy,
                       (int)evs[i].inside);
        }
    }
    const int drc = wl_surface_destroy(surf);
    printf("[WMCLOCK] destroy id=%d rc=%d\n", surf, drc);
    printf("[WMCLOCK] done frames=%d\n", done);
    return 0;
}
