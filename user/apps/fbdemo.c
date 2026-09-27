/* fbdemo.c - ★ A2：把 A1 的汇编演示（user/fbdemo.asm）**用 C 重写**，可观测行为逐条对齐
 *
 * 判据对照（tests/fbmap64_test.py 就是按这些串口打点 + 抓屏像素断言的）：
 *   [FBDEMO] map va=0x.. w=.. h=.. pitch=.. fmt=..       —— fb_map 拿到的用户 VA + 几何
 *   [FBDEMO] frame=<n> flip=<ret> va=0x.. pid=<n>        —— 每帧一次 fb_flip(0, band_y, w, band_h)
 *   [FBDEMO] oob ret=1                                   —— 第 3 帧故意喂完全越界的 flip，被内核拒绝
 *   [FBDEMO] clamp ret=0                                 —— 第 4 帧喂部分越界（x=-40），被夹取后提交
 *   [FBDEMO] done frames=5                               —— 收尾（其间调过一次 fb_present）
 * 画的东西与 asm 版一致：横贯全宽的**渐变带**（8 像素一段，颜色随帧平移 17）+ 从左到右移动的
 *   240x40 实心矩形 + 固定位置的状态像素条；提交区域只有那条带（fb_flip 局部提交）。
 * 与 asm 版的差别（只列事实）：帧数/帧长/矩形尺寸/配色公式**完全相同**；C 版用的是
 *   printf（我们自己的行缓冲）而不是手写十进制/十六进制转换，因此每帧只有 1~2 次 write(1)。
 * 帧数/帧长别随便拉长：本演示跑在**进桌面之前**的启动路径上（见 user/fbdemo.asm 顶部的实测说明）。 */
#include <stdio.h>
#include <stdlib.h>

#include <unistd.h>

#include "fb.h"
#include "vimtu64.h"

#define FRAMES   5        /* 总帧数 */
#define FRAME_MS 700      /* 单帧停留（验收脚本按 [FBDEMO] frame= 同步抓屏） */
#define RECT_W   240      /* 移动矩形宽 */
#define RECT_H   40       /* 移动矩形高 */

int main(void) {
    uint32_t*      fb   = 0;
    struct Fb64Info info;

    const int rc = fb_map(&fb, &info);                    /* 自有 ABI 9：内核映射后备缓冲 */
    if (rc != 0) {
        printf("[FBDEMO] fb_map FAILED ret=%d\n", rc);    /* 如实打印负错误码，绝不假装画成功 */
        return 1;
    }

    const unsigned width  = info.width;
    const unsigned height = info.height;
    const unsigned pitch  = info.pitch;


    printf("[FBDEMO] map va=0x%lx w=%u h=%u pitch=%u fmt=%u\n",
           (unsigned long)info.va, width, height, pitch, info.format);

    const unsigned band_y = height / 5;                   /* 色带 y = height/5 */
    const unsigned band_h = height / 6;                   /* 色带高 = height/6 */
    const int      pid    = getpid();

    for (unsigned frame = 0; frame < FRAMES; frame++) {
        /* ---- (a) 背景：整条色带按 8 像素一段填渐变（段的颜色随帧平移 17）---- */
        for (unsigned y = band_y; y < band_y + band_h; y++) {
            uint32_t* row = (uint32_t*)((unsigned char*)fb + (unsigned long)y * pitch);
            for (unsigned x = 0; x < width; x += 8) {
                const unsigned t = (x + frame * 17u) & 0xFFu;
                const uint32_t color = 0xFF000000u | ((uint32_t)t << 16) |
                                       ((((uint32_t)t ^ 0x55u)) << 8) | (255u - t);
                unsigned k = 0;
                for (; k < 8 && x + k < width; k++) row[x + k] = color;
            }
        }

        /* ---- (b) 移动矩形：240x40，x = frame*(width-240)/(FRAMES-1)，y = band_y + 16 ---- */
        {
            const unsigned rx = (unsigned)((unsigned long)(width - RECT_W) * frame / (FRAMES - 1));
            const uint32_t color = 0xFF000000u | (((uint32_t)((frame * 29u + 80u) & 0xFFu)) << 16) |
                                   (((uint32_t)((200u - frame * 11u) & 0xFFu)) << 8) |
                                   ((uint32_t)(frame * 53u) & 0xFFu);
            for (unsigned y = band_y + 16; y < band_y + 16 + RECT_H; y++) {
                uint32_t* row = (uint32_t*)((unsigned char*)fb + (unsigned long)y * pitch);
                for (unsigned k = 0; k < RECT_W && rx + k < width; k++) row[rx + k] = color;
            }
        }

        /* ---- (c) 状态像素条：固定位置，宽度 = (frame+1)*(width/10)，高 10 ---- */
        {
            const unsigned bar_w = (width / 10u) * (frame + 1u);
            const unsigned bar_y = band_y + band_h - 12u;
            for (unsigned y = bar_y; y < bar_y + 10u; y++) {
                uint32_t* row = (uint32_t*)((unsigned char*)fb + (unsigned long)y * pitch);
                unsigned k = 0;
                for (; k < bar_w && 8u + k < width; k++) row[8u + k] = 0xFFFFFFFFu;
            }
        }

        /* ---- 提交区域：只有这条带上屏（fb_flip 局部提交）---- */
        const int flip = fb_flip(0, (int)band_y, (int)width, (int)band_h);

        /* ---- 第 3 帧：故意喂**完全越界**的区域（内核必须拒绝且不崩）---- */
        if (frame == 2) {
            printf("[FBDEMO] oob ret=%d\n", fb_flip((int)width + 100, (int)band_y, 32, 32));
        }
        /* ---- 第 4 帧：**部分越界**（x = -40）——内核夹取后提交，返回 0 ---- */
        if (frame == 3) {
            printf("[FBDEMO] clamp ret=%d\n", fb_flip(-40, (int)band_y + 8, 80, RECT_H));
        }

        printf("[FBDEMO] frame=%u flip=%d va=0x%lx pid=%d\n", frame, flip, (unsigned long)info.va, pid);
        vimtu64_sleep_ms(FRAME_MS);                       /* 自有 ABI 5：让测试按帧抓屏 */
    }

    fb_present();                                          /* 自有 ABI 11：整屏提交一次 */
    printf("[FBDEMO] done frames=%d\n", FRAMES);
    return 0;
}
