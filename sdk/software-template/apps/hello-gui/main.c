/* sdk/software-template/apps/hello-gui/main.c - 模板示例②：图形程序（/bin/wm 合成器 + shm surface）
 *
 * 推荐路线（本文档 README 的"GUI 两路"里那条）：**不自己碰帧缓冲**，而是
 *   ① 自己 fork + execve 拉起 Ring 3 合成器 `/lib/wm.elf`（内核侧的 /bin/wm.elf 只在启动早期注册，
 *      桌面期看不到它；这条与 user/apps/fontdemo.c 的取舍一致，"客户端与合成器同时在跑"才是真的）；
 *   ② shm_create + shm_map 拿一块**跨进程共享**的画布；
 *   ③ wl_surface_create / attach / damage / commit 提交，**提交后必须 dispatch 一次**才上屏；
 *   ④ 文字用 `user/lib/font64.h`（stb_truetype）画在自己的画布上 —— 字库从系统卷读，不编进程序。
 *
 * 打点（tests/hello_gui_test.py 按这些串 grep + 抓屏像素；格式别改）：
 *   [HELLO-GUI] ver=1 pid=<n>
 *   [HELLO-GUI] wm fork pid=<n> exec=/lib/wm.elf present=<0|1>
 *   [HELLO-GUI] font path=/Fonts/NotoSans-Regular.ttf ok=<0|1> bytes=<n> upem=<n> nglyph=<n>
 *   [HELLO-GUI] surf id=<n> w=<h> h=<h> shm=<n> va=0x<n>
 *   [HELLO-GUI] marker color=0xffff0000 x=<n> y=<n> w=<n> h=<n> px=<n>
 *   [HELLO-GUI] text size=18 width=<px> lh=<px>
 *   [HELLO-GUI] frame f=<n> rc=<n> ink=<n> (band ...)
 *   [HELLO-GUI] destroy rc=<n>
 *   [HELLO-GUI] done frames=<n> bytes=<n>
 *
 * 如实边界（这段代码**故意**没做的事）：
 *   * 不处理 seat 事件（不做交互）：模板只演示"画 + 提交"，输入另见 user/lib/wl.h 的 seat 事件；
 *   * 不做多缓冲 ping-pong（单缓冲 + 每帧重画）；要 ping-pong 就再 shm_create 一块轮流 attach；
 *   * 单 surface <= 16384 px、单 shm <= 64 KiB（内核上限，超了内核用负错误码拒绝）。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "vimtu64.h"
#include "wl.h"
#include "font64.h"

#define W 256
#define H 56                    /* 256*56 = 14336 px / 57344 B（都在内核上限内） */
#define FRAMES 3
#define WM_ELF "/lib/wm.elf"
#define FONT_PRIMARY "/Fonts/NotoSans-Regular.ttf"
#define FONT_CJK     "/Fonts-open/NotoSansSC-Regular.ttf"

#define CO_BG     0xFF101014u
#define CO_WHITE  0xFFFFFFFFu
#define CO_MARKER 0xFFFF0000u   /* 纯红 24x24 方块：给抓屏脚本一个**好判**的像素证据 */

static const char* TEXT = "VimtuOS SDK hello-gui";

/* 一次提交 = attach + damage + commit + dispatch（dispatch 不取事件，只把提交合成上屏） */
static int submit(int surf, int shm, int w, int h) {
    int rc = wl_surface_attach(surf, shm, 0);
    if (rc != 0) return rc;
    rc = wl_surface_damage(surf, 0, 0, w, h);
    if (rc != 0) return rc;
    rc = wl_surface_commit(surf);
    if (rc != 0) return rc;
    return wl_display_dispatch(0, 0, 0);
}

/* 把一个矩形刷成实色（XRGB8888：内存里低字节是蓝，与内核 fb 同口径） */
static void fill_rect(uint32_t* px, int pitch_px, int x, int y, int w, int h, uint32_t color) {
    for (int j = 0; j < h; j++) {
        for (int i = 0; i < w; i++) px[(y + j) * pitch_px + (x + i)] = color;
    }
}

int main(void) {
    printf("[HELLO-GUI] ver=1 pid=%d\n", getpid());

    /* ---- ① 拉起 Ring 3 合成器（卷里有才拉；没有就吃内核内部合成器）---- */
    int wm_pid = 0, wm_present = 0;
    {
        const int probe = open(WM_ELF, O_RDONLY);
        if (probe >= 0) {
            (void)close(probe);
            wm_present = 1;
            static char* argv[2];
            static char* envp[1];
            argv[0] = (char*)(unsigned long)WM_ELF;
            argv[1] = 0;
            envp[0] = 0;
            const int pid = fork();
            if (pid == 0) {
                (void)__v64_syscall(59 /*execve*/, (long)(unsigned long)WM_ELF,
                                    (long)(unsigned long)argv, (long)(unsigned long)envp, 0, 0);
                (void)__v64_int80(V64_NR_EXIT, 127, 0, 0, 0);
                for (;;) { }
            }
            wm_pid = pid;
        }
        printf("[HELLO-GUI] wm fork pid=%d exec=%s present=%d\n", wm_pid, WM_ELF, wm_present);
    }
    /* 合成器要映射它自己的后备缓冲 + 标定 rdtsc，实测约 1.5~2 s；等不够会被内核内部合成器吃掉提交。
     * 没有合成器时不必等（内核内部合成器立刻可用）。 */
    if (wm_present) vimtu64_sleep_ms(2500);
    else            vimtu64_sleep_ms(300);

    /* ---- ② 字库：从**系统卷**读，不编进程序 ---- */
    struct Font64* font = font_load(FONT_PRIMARY);
    {
        int ok = 0, bytes = 0, upem = 0, nglyph = 0;
        if (font) {
            (void)font64_add_face(font, FONT_CJK, F64_KIND_CJK);   /* 有就加中文面，没有不影响 */
            struct F64FaceInfo fi;
            if (font64_face_info(font, 0, &fi) == 0) {
                ok = fi.used; bytes = (int)fi.bytes; upem = fi.upem; nglyph = fi.nglyph;
            }
            (void)font64_set_spacing(font, F64_SPACING_GRID12);
            (void)font64_set_primary(font, F64_KIND_LATIN);
        }
        printf("[HELLO-GUI] font path=%s ok=%d bytes=%d upem=%d nglyph=%d\n",
               FONT_PRIMARY, ok, bytes, upem, nglyph);
    }

    /* ---- ③ surface + shm 画布 ---- */
    const int shm = shm_create((unsigned)(W * H * 4));
    const int surf = wl_surface_create(W, H, V64_WL_FORMAT_XRGB8888);
    void* va = 0;
    const int map_rc = (shm > 0) ? shm_map(shm, 0, (unsigned)(W * H * 4), &va) : -1;
    printf("[HELLO-GUI] surf id=%d w=%d h=%d shm=%d va=0x%x\n",
           surf, W, H, shm, (unsigned)(unsigned long)va);
    if (surf <= 0 || shm <= 0 || map_rc != 0 || !va) {
        printf("[HELLO-GUI] FAILED surface/shm (surf=%d shm=%d map=%d)\n", surf, shm, map_rc);
        return 1;
    }
    uint32_t* px = (uint32_t*)va;
    struct F64Canvas canvas = { px, W, H };

    /* ---- ④ 画：底色 + 一个纯红标记块 + 一行字 ---- */
    font64_fill(&canvas, CO_BG);
    fill_rect(px, W, 4, 4, 24, 24, CO_MARKER);
    printf("[HELLO-GUI] marker color=0x%x x=4 y=4 w=24 h=24 px=%d\n", CO_MARKER, 24 * 24);
    int drawn = 0;
    if (font) drawn = font_draw_text(font, &canvas, 36, 8, TEXT, 18, CO_WHITE);
    {
        int bbox[4] = { 0, 0, 0, 0 };
        const int ink = font64_ink_stats(&canvas, CO_BG, 0, 0, W, H, 16, bbox);
        const int tw = font ? font64_text_width(font, TEXT, 18) : -1;
        const int lh = font ? font64_line_height(font, 18) : -1;
        printf("[HELLO-GUI] text size=18 glyphs=%d width=%d lh=%d ink=%d bbox=%d,%d,%d,%d\n",
               drawn, tw, lh, ink, bbox[0], bbox[1], bbox[2], bbox[3]);
    }

    /* ---- ⑤ 帧循环：提交 + 合成上屏（每帧重画，内容一致） ---- */
    for (int f = 0; f < FRAMES; f++) {
        const int rc = submit(surf, shm, W, H);
        int bbox[4] = { 0, 0, 0, 0 };
        const int ink = font64_ink_stats(&canvas, CO_BG, 0, 0, W, H, 16, bbox);
        printf("[HELLO-GUI] frame f=%d rc=%d ink=%d\n", f, rc, ink);
        (void)fflush(0);
        if (f + 1 < FRAMES) {
            vimtu64_sleep_ms(700);
            font64_fill(&canvas, CO_BG);
            fill_rect(px, W, 4, 4, 24, 24, CO_MARKER);
            (void)font_draw_text(font, &canvas, 36, 8, TEXT, 18, CO_WHITE);
        }
    }

    /* ---- ⑥ 收尾：销毁 surface（合成器见"客户端全退"自己收工）---- */
    const int drc = wl_surface_destroy(surf);
    printf("[HELLO-GUI] destroy rc=%d\n", drc);
    vimtu64_sleep_ms(600);
    printf("[HELLO-GUI] done frames=%d bytes=%d\n", FRAMES, FRAMES * (W * H * 4));
    (void)fflush(0);
    return 0;
}
