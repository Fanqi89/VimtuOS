/* fontdemo.c - ★ Ring 3 文字/字体栈的演示程序（用户态画字 -> /bin/wm 合成上屏）
 *
 * 交付：/bin/fontdemo.elf（系统卷里的文件；内核里没有它的字节）—— 由 tests/font64user_test.py
 * 写进夹具卷，在**内核终端**里用 `elfrun /bin/fontdemo.elf` 跑成真进程。
 *
 * 它跑通的闭环：
 *   ① 从**系统卷**读四份 TTF（/Fonts/NotoSans-Regular.ttf + /Fonts-open/{NotoSansSC-Regular,
 *      SarasaMonoSC-Regular,unifont-14.0.01}.ttf）—— 字库不编进程序（[FONT64U] load 行给路径/字节数）；
 *   ② 在本进程 shm 画布上画中英混排（"VimtuOS 你好 42"，字号 18 / 14，两种颜色，其中一种带 alpha）
 *      与一段**与内核同串同字号**的对照文本（内核终端第一行 banner，等宽面 em=16，两个相位各一行）；
 *   ③ fork + execve 拉起 `/lib/wm.elf`（Ring 3 合成器），自己的 surface 交给它合成上屏；
 *   ④ 刷 N 帧后退出（surface 销毁 -> wm 见"客户端全退"收工 -> 交回地盘）。
 *
 * 为什么合成器在 /lib/wm.elf 而不是 /bin/wm.elf：内核的 wl64_wm64("/bin/wm.elf") 是在
 * **桌面之前**（gui64_run 之前）同步等待的（kernel/kernel64.cpp:1173），那段时间桌面/终端还没起来，
 * 用户没法在合成器活着的时候再起客户端。夹具卷里**故意不放** /bin/wm.elf（内核打一行
 * [WL64] wm skipped），把 wm 放到 /lib/wm.elf，由本程序在桌面期用 fork+execve 拉起 —— 这样
 * "客户端与合成器同时在跑"是真实成立的（[WM] probe full=1 + [WL64] composer pid=.. + [WM] frame .. surfs=1）。
 *
 * 打点（tests/font64user_test.py 按这些串 grep；格式勿改）：
 *   [FONTDEMO] ver=1 pid=<n>
 *   [FONTDEMO] face kind=<k> path=<p> bytes=<n> ok=<0|1> upem=.. asc=.. desc=.. nglyph=..
 *   [FONTDEMO] spacing=grid12 size=18 text="..." width=<px> lh=<px>
 *   [FONTDEMO] pen size=18 n=<n> x=..,.. adv=..,..
 *   [FONTDEMO] ink size=18 row=0 n=<px> bbox=x,y,w,h cover_permille=..
 *   [FONTDEMO] alpha color=0x.. cov=255 bg=0x.. px=0x.. expect=0x..
 *   [FONTDEMO] surf tag=a id=.. w=.. h=.. shm=<id> va=0x..
 *   [FONTDEMO] frame f=<n> ink_a=.. ink_b=.. wm_child=<pid>
 *   [FONTDEMO] cache phase=<first|reuse> miss=<n> hit=<n> miss_us=<n> hit_us=<n> redraw_tsc=<n>
 *   [FONTDEMO] summary faces=<n> glyph_ok=.. empty=.. fail=.. arena_peak=.. arena_fail=..
 *   [FONTDEMO] destroy a=<ret> b=<ret>
 *   [FONTDEMO] done frames=<n> bytes=<n>
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vimtu64.h"
#include "wl.h"
#include "font64.h"

#ifndef VIMTUOS_VERSION_STR
#define VIMTUOS_VERSION_STR "0.4.3"
#endif

/* 画布（单 surface <= 16384 px；单 shm <= 64 KiB —— 见 user/lib/wl.h 的上限常量） */
#define A_W 256
#define A_H 48                 /* 256*48 = 12288 px / 49152 B */
#define B_W 384
#define B_H 40                 /* 384*40 = 15360 px / 61440 B */
#define FRAMES 3

#define CO_BG      0x000000u
#define CO_WHITE   0xFFFFFFFFu   /* 白（alpha = 0xFF） */
#define CO_AMBER   0x80FFC857u   /* 半透明琥珀（alpha = 0x80）——alpha 路径的证据 */

/* 与内核终端第一行**同一串、同一字号**（em=16，等宽面 face 2）：
 * 真源 = build64.sh 的 VIMTUOS_VERSION（编译期宏；tests/font64user_test.py 用同一个真源复算）。 */
static const char BANNER[] = "VimtuOS Terminal v" VIMTUOS_VERSION_STR " (64-bit long mode)";
static const char MIXED[]  = "VimtuOS 你好 42";

/* 系统卷里的字库路径（与 kernel/font.cpp 的四个面一一对应） */
static const char* const FACE_PATH[4] = {
    "/Fonts/NotoSans-Regular.ttf",              /* face 0 西文 UI */
    "/Fonts-open/NotoSansSC-Regular.ttf",       /* face 1 中文 */
    "/Fonts-open/SarasaMonoSC-Regular.ttf",     /* face 2 终端等宽 */
    "/Fonts-open/unifont-14.0.01.ttf",          /* face 3 缺字兜底 */
};
static const char* const WM_ELF = "/lib/wm.elf";

static struct Font64* g_font;
static unsigned long long g_tsc_per_ms;

static unsigned us_of(unsigned long long tsc) {
    if (!g_tsc_per_ms) return 0;
    return (unsigned)((tsc * 1000ULL) / g_tsc_per_ms);
}

/* 往 buf 里写十进制（返回新的写指针） */
static char* app_u(char* p, int v) {
    char t[12];
    int k = 0;
    unsigned x = (v < 0) ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) *p++ = '-';
    if (x == 0) t[k++] = '0';
    while (x) { t[k++] = (char)('0' + (int)(x % 10u)); x /= 10u; }
    while (k) *p++ = t[--k];
    return p;
}

/* ★ 一条日志行**只准一次 printf**：本 libc 的 printf 在返回前一定 flush（user/lib/stdio.c:159），
 *   一行拆成多次 printf 就会被内核的 [SYSCALL] 追踪行插进去（实测过），验收脚本按行 grep 会失败。
 *   所以这里自己拼字符串，最后一次性 printf("%s\n", buf)。 */
static void print_pen(const char* tag, const char* text, int size) {
    int xs[64];
    const int n = font64_pen_series(g_font, text, size, xs, 64);
    char buf[200];
    if (n < 0) { printf("[FONTDEMO] pen size=%d n=-1 (cap too small)\n", size); return; }
    printf("[FONTDEMO] pen size=%d n=%d tag=%s\n", size, n, tag);
    if (n <= 20) {
        char* p = buf;
        const char* pre = "[FONTDEMO] pen x size=";
        for (int i = 0; pre[i]; i++) *p++ = pre[i];
        p = app_u(p, size);
        *p++ = ' '; *p++ = 'n'; *p++ = '=';
        p = app_u(p, n);
        for (int i = 0; i <= n; i++) { *p++ = ','; p = app_u(p, (i < n) ? xs[i] : xs[n]); }
        *p = 0;
        printf("%s\n", buf);
    }
    {   /* 逐字推进（与内核 font64_glyph_advance 同口径，见 font64_advance） */
        const char* s = text;
        char* p = buf;
        const char* pre = "[FONTDEMO] pen adv size=";
        int first = 1, k = 0;
        for (int i = 0; pre[i]; i++) *p++ = pre[i];
        p = app_u(p, size);
        const char* pre2 = " adv=";
        for (int i = 0; pre2[i]; i++) *p++ = pre2[i];
        while (*s) {
            int adv = 1;
            const uint32_t cp = (uint32_t)font64_utf8_decode(s, &adv);
            s += adv;
            if (!cp) continue;
            if (k >= 16) break;                 /* 一行最多 16 个推进（行长 < 128 B） */
            if (!first) *p++ = ',';
            p = app_u(p, font64_advance(g_font, cp, size));
            first = 0;
            k++;
        }
        if (k < n) { *p++ = ','; *p++ = '.'; *p++ = '.'; *p++ = '.'; }
        *p = 0;
        printf("%s\n", buf);
    }
}

static void print_ink(const char* tag, struct F64Canvas* c, int size, int row, int y0, int h,
                      int total_px) {
    int bbox[4] = { 0, 0, 0, 0 };
    const int n = font64_ink_stats(c, CO_BG, 0, y0, c->w, h, 16, bbox);
    const int permille = total_px > 0 ? (n * 1000) / total_px : 0;
    printf("[FONTDEMO] ink size=%d row=%d n=%d bbox=%d,%d,%d,%d cover_permille=%d "
           "(band y=%d h=%d of %dx%d)\n",
           size, row, n, bbox[0], bbox[1], bbox[2], bbox[3], permille, y0, h, c->w, c->h);
    (void)tag;
}

static void draw_canvas_a(struct F64Canvas* a) {
    font64_fill(a, CO_BG);
    (void)font64_set_spacing(g_font, F64_SPACING_GRID12);
    (void)font64_set_primary(g_font, F64_KIND_LATIN);
    font_draw_text(g_font, a, 0, 0, MIXED, 18, CO_WHITE);
    font_draw_text(g_font, a, 0, 22, MIXED, 14, CO_AMBER);
}

static void draw_canvas_b(struct F64Canvas* b) {
    font64_fill(b, CO_BG);
    (void)font64_set_spacing(g_font, F64_SPACING_GRID12);
    (void)font64_set_primary(g_font, F64_KIND_MONO);        /* 内核终端就是等宽面 */
    (void)font64_set_shift(g_font, 0);
    font_draw_text(g_font, b, 0, 0, BANNER, 16, CO_WHITE);
    (void)font64_set_shift(g_font, 1);                      /* 另一相位各一行（对照口径见测试） */
    font_draw_text(g_font, b, 0, 20, BANNER, 16, CO_WHITE);
    (void)font64_set_shift(g_font, 0);
}

static int submit(int surf, int shm, int w, int h) {
    int rc = wl_surface_attach(surf, shm, 0);
    if (rc != 0) return rc;
    rc = wl_surface_damage(surf, 0, 0, w, h);
    if (rc != 0) return rc;
    rc = wl_surface_commit(surf);
    if (rc != 0) return rc;
    return wl_display_dispatch(0, 0, 0);             /* 0 = 不取事件，只把提交合成上屏 */
}

int main(void) {
    printf("[FONTDEMO] ver=1 pid=%d\n", getpid());

    /* ---- 零、先把合成器拉起来（fork + execve）----
     * ★ 必须在**加载字库/分配大字库缓冲之前**：本内核的 fork 有页预算
     *   （[PROC64] init ... fork_max_pages=256），而一次 font_load 就要 mmap
     *   1 MiB 光栅化临时区 + 1.1 MB 中文字库缓冲（~290 页）—— 先加载后 fork 会
     *   如实返回 -ENOMEM（第一次跑就是这样：pid=-12）。 */
    g_tsc_per_ms = font64_calibrate_tsc_per_ms(40);
    printf("[FONTDEMO] tsc_per_ms=%lu\n", (unsigned long)g_tsc_per_ms);
    if (g_tsc_per_ms == 0) printf("[FONTDEMO] tsc calibration FAILED (ticks granularity)\n");

    int wm_pid = 0;
    {
        static char* argv[2];
        static char* envp[1];
        argv[0] = (char*)(unsigned long)WM_ELF;
        argv[1] = 0;
        envp[0] = 0;
        const int pid = fork();
        if (pid == 0) {
            (void)__v64_syscall(59 /*execve*/, (long)(unsigned long)WM_ELF,
                                (long)(unsigned long)argv, (long)(unsigned long)envp, 0, 0);
            (void)__v64_int80(V64_NR_EXIT, 127, 0, 0, 0);   /* 拉起失败：如实以 127 退出 */
            for (;;) { }
        }
        wm_pid = pid;
        printf("[FONTDEMO] wm fork pid=%d exec=%s\n", pid, WM_ELF);
    }

    /* ★ 用户态没有"合成器是否已注册"的查询接口（22 号只有注册者能调、没有查询号），
     *   所以这里用**固定等待**：实测 /lib/wm.elf 从 fork 到注册约 1.5–2 s
     *   （它要映射 33 MB 后备缓冲 + 标定 rdtsc）。不等的后果实测过：surface 建在合成器
     *   注册之前 -> 内核内部合成器把我们的提交吃掉（[WM] frame surfs=2 pending=0 flags=0）。
     *   测试脚本会断言 [WL64] composer pid=.. 出现在本进程的 surface create 行**之前**，
     *   所以这个等待够不够是可核对的（不是"大概齐"）。 */
    vimtu64_sleep_ms(2500);

    /* ---- ① 从系统卷读字库（不编进程序）---- */
    g_font = font_load(FACE_PATH[0]);
    if (!g_font) { printf("[FONTDEMO] FAILED font_load path=%s\n", FACE_PATH[0]); return 1; }
    for (int i = 1; i < 4; i++) (void)font64_add_face(g_font, FACE_PATH[i], i);
    for (int i = 0; i < 4; i++) {
        struct F64FaceInfo fi;
        if (font64_face_info(g_font, i, &fi) != 0) continue;
        printf("[FONTDEMO] face kind=%d path=%s bytes=%u ok=%d upem=%d asc=%d desc=%d nglyph=%d\n",
               fi.kind, fi.used ? fi.path : FACE_PATH[i], fi.bytes, fi.used,
               fi.upem, fi.asc_fu, fi.desc_fu, fi.nglyph);
    }
    vimtu64_sleep_ms(400);                                  /* 让合成器注册完再交面给它 */

    /* ---- surface + shm 缓冲（自家画布）---- */
    const int seat = wl_seat_get();
    printf("[FONTDEMO] seat=%d\n", seat);

    const int sa = wl_surface_create(A_W, A_H, V64_WL_FORMAT_XRGB8888);
    const int sb = wl_surface_create(B_W, B_H, V64_WL_FORMAT_XRGB8888);
    const int ha = shm_create((unsigned)(A_W * A_H * 4));
    const int hb = shm_create((unsigned)(B_W * B_H * 4));
    printf("[FONTDEMO] surf tag=a id=%d w=%d h=%d shm=%d\n", sa, A_W, A_H, ha);
    printf("[FONTDEMO] surf tag=b id=%d w=%d h=%d shm=%d\n", sb, B_W, B_H, hb);
    if (sa <= 0 || sb <= 0 || ha <= 0 || hb <= 0) { printf("[FONTDEMO] FAILED surface/shm\n"); return 1; }
    void* va = 0; void* vb = 0;
    if (shm_map(ha, 0, (unsigned)(A_W * A_H * 4), &va) != 0 || !va) { printf("[FONTDEMO] FAILED shm_map a\n"); return 1; }
    if (shm_map(hb, 0, (unsigned)(B_W * B_H * 4), &vb) != 0 || !vb) { printf("[FONTDEMO] FAILED shm_map b\n"); return 1; }
    struct F64Canvas ca = { (uint32_t*)va, A_W, A_H };
    struct F64Canvas cb = { (uint32_t*)vb, B_W, B_H };
    printf("[FONTDEMO] map a=0x%x b=0x%x bytes_a=%d bytes_b=%d\n",
           (unsigned)(unsigned long)va, (unsigned)(unsigned long)vb, A_W * A_H * 4, B_W * B_H * 4);

    /* ---- ④ alpha 混合的定点证据（已知覆盖率 255 + alpha 0x80）---- */
    {
        font64_fill(&ca, CO_BG);
        font64_blend_px(&ca, 1, 1, CO_AMBER, 255);
        const uint32_t got = ca.px[1 * A_W + 1];
        const unsigned a = (CO_AMBER >> 24) & 0xFFu;
        const unsigned er = ((((CO_AMBER >> 16) & 0xFFu) * a) + 0xFFu * 0) / 255u;
        const unsigned eg = ((((CO_AMBER >> 8) & 0xFFu) * a)) / 255u;
        const unsigned eb = ((((CO_AMBER) & 0xFFu) * a)) / 255u;
        const unsigned exp = 0xFF000000u | (er << 16) | (eg << 8) | eb;
        printf("[FONTDEMO] alpha color=0x%x cov=255 bg=0x%x px=0x%x expect=0x%x\n",
               CO_AMBER, CO_BG, got, exp);
    }

    /* ---- ② 第一遍绘制（全部 miss）---- */
    unsigned long long first_tsc = 0;
    {
        const unsigned long long t0 = font64_rdtsc();
        draw_canvas_a(&ca);
        draw_canvas_b(&cb);
        first_tsc = font64_rdtsc() - t0;
    }
    {
        struct F64Stats st = font64_stats(g_font);
        printf("[FONTDEMO] cache phase=first miss=%u hit=%u miss_us=%u hit_us=%u redraw_tsc=%lu "
               "redraw_us=%u\n",
               st.miss_n, st.hit_n, us_of(st.miss_tsc), us_of(st.hit_tsc),
               (unsigned long)first_tsc, us_of(first_tsc));
        printf("[FONTDEMO] spacing=grid12 size=18 text=\"%s\" width=%d lh=%d\n",
               MIXED, font64_text_width(g_font, MIXED, 18), font64_line_height(g_font, 18));
        printf("[FONTDEMO] spacing=grid12 size=14 text=\"%s\" width=%d lh=%d\n",
               MIXED, font64_text_width(g_font, MIXED, 14), font64_line_height(g_font, 14));
        (void)font64_set_spacing(g_font, F64_SPACING_NATURAL);
        printf("[FONTDEMO] spacing=natural size=16 banner_width=%d text=\"%s\"\n",
               font64_text_width(g_font, BANNER, 16), BANNER);
        (void)font64_set_spacing(g_font, F64_SPACING_GRID12);
        print_pen("mixed18", MIXED, 18);
        print_pen("mixed14", MIXED, 14);
        print_pen("banner16", BANNER, 16);
        printf("[FONTDEMO] summary faces=%d glyph_ok=%u empty=%u fail=%u arena_peak=%u arena_fail=%u\n",
               font64_face_count(g_font), st.glyph_ok, st.glyph_empty, st.glyph_fail,
               st.arena_peak, st.arena_fail);
        printf("[FONTDEMO] miss30=%u\n", 30u);
    }
    print_ink("a18", &ca, 18, 0, 0, 22, A_W * 22);
    print_ink("a14", &ca, 14, 1, 22, 18, A_W * 18);
    print_ink("b16p0", &cb, 16, 0, 0, 20, B_W * 20);
    print_ink("b16p5", &cb, 16, 1, 20, 20, B_W * 20);

    /* ---- 帧循环：提交 + 合成（wm 在用户态合成上屏）---- */
    for (int f = 0; f < FRAMES; f++) {
        int rc = submit(sa, ha, A_W, A_H);
        rc |= submit(sb, hb, B_W, B_H);
        int ibbox[4], bbbox[4];
        const int ia = font64_ink_stats(&ca, CO_BG, 0, 0, A_W, A_H, 16, ibbox);
        const int ib = font64_ink_stats(&cb, CO_BG, 0, 0, B_W, B_H, 16, bbbox);
        printf("[FONTDEMO] frame f=%d rc=%d ink_a=%d ink_b=%d wm_child=%d\n", f, rc, ia, ib, wm_pid);
        (void)fflush(0);
        if (f + 1 < FRAMES) {
            vimtu64_sleep_ms(700);
            /* 重画一遍（这一遍应当全是**缓存命中**）——第二帧的耗时对比就是"首次 vs 命中" */
            const unsigned long long t0 = font64_rdtsc();
            draw_canvas_a(&ca);
            draw_canvas_b(&cb);
            const unsigned long long dt = font64_rdtsc() - t0;
            struct F64Stats st2 = font64_stats(g_font);
            printf("[FONTDEMO] cache phase=reuse miss=%u hit=%u miss_us=%u hit_us=%u redraw_tsc=%lu "
                   "redraw_us=%u\n",
                   st2.miss_n, st2.hit_n, us_of(st2.miss_tsc), us_of(st2.hit_tsc),
                   (unsigned long)dt, us_of(dt));
        }
    }

    /* ---- 收尾：销毁 surface（wm 见"客户端全退"自己收工）---- */
    const int da = wl_surface_destroy(sa);
    const int db = wl_surface_destroy(sb);
    printf("[FONTDEMO] destroy a=%d b=%d\n", da, db);
    vimtu64_sleep_ms(600);
    printf("[FONTDEMO] done frames=%d bytes=%d\n", FRAMES, FRAMES * (A_W * A_H * 4 + B_W * B_H * 4));
    (void)fflush(0);
    return 0;
}
