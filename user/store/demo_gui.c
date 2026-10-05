/* demo_gui.c - ★ 应用商店里的**示例 GUI 包**（.vap64 的 payload；被 vpkg 装成 /bin/demo-gui）
 *
 * 它证明的是"图形包装完也能启动"：在**有合成器的会话**里（/bin/store 已经拉起 /lib/wm.elf），
 * 这个程序会自己建一块 shm 画布 -> 画几帧彩色方块 -> 提交上屏 -> 打印打点。
 *
 * 刻意的取舍（如实写）：
 *   * **不链 user/lib/font64**（stb_truetype 一进来 ELF 就 50 KiB 上下，超过 VAP64 的
 *     code 段上限 32768 B）—— 所以这个示例只画色块，不画字；商店自己的界面才用 font64。
 *   * **不自己 fork 合成器**：它假定"已经有合成器"（store 起的那个）。没有合成器时内核的
 *     内部合成路径会兜底（[WL64] composite …），程序本身照样跑完。
 *
 * 打点（tests/vpkg64_test.py 按这些串 grep；格式勿改）：
 *   [DEMO-GUI] ver=1 pid=<n>
 *   [DEMO-GUI] surf id=<n> w=160 h=40 shm=<n>
 *   [DEMO-GUI] frame f=<n> col=0x<hex> rc=<n>
 *   [DEMO-GUI] done frames=3
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"

#define DW 160                  /* 160x40 = 6400 px（<= 单 surface 上限 16384 px） */
#define DH 40
#define DBYTES (DW * DH * 4)    /* 25600 B（<= 单 shm 对象上限 64 KiB） */

static const unsigned COLS[3] = { 0xE05A3Cu, 0x3CE05Au, 0x3C7BE0u };

int main(void) {
    printf("[DEMO-GUI] ver=1 pid=%d\n", getpid());

    const int surf = wl_surface_create(DW, DH, V64_WL_FORMAT_XRGB8888);
    const int shm = shm_create(DBYTES);
    void* va = 0;
    const int mrc = shm_map(shm, 0, DBYTES, &va);
    printf("[DEMO-GUI] surf id=%d w=%d h=%d shm=%d map=%d\n", surf, DW, DH, shm, mrc);
    if (surf <= 0 || shm <= 0 || mrc != 0 || !va) {
        printf("[DEMO-GUI] FAILED rc=%d/%d/%d\n", surf, shm, mrc);
        (void)fflush(0);
        return 1;
    }
    unsigned* px = (unsigned*)va;
    for (int f = 0; f < 3; f++) {
        const unsigned c = 0xFF000000u | COLS[f];
        for (int y = 0; y < DH; y++)
            for (int x = 0; x < DW; x++)
                px[y * DW + x] = ((x / 16 + y / 16 + f) & 1) ? c : 0xFF101010u;
        (void)wl_surface_attach(surf, shm, 0);
        (void)wl_surface_damage(surf, 0, 0, DW, DH);
        const int crc = wl_surface_commit(surf);
        const int drc = wl_display_dispatch(0, 0, 0);
        printf("[DEMO-GUI] frame f=%d col=0x%x rc=%d/%d\n", f, COLS[f], crc, drc);
        vimtu64_sleep_ms(200);
    }
    printf("[DEMO-GUI] done frames=3\n");
    (void)wl_surface_destroy(surf);
    (void)fflush(0);
    return 0;
}
