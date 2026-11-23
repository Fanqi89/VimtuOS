/* viewer.c - ★ v0.10.5：图片查看器（ring3 wl_surface 窗口壳 + vendored stb_image 解码）
 *
 * 为什么有这个程序：
 *   用户反馈"双击任何非原生文件都被强行用文本编辑器打开（满屏乱码）"，而且系统里
 *   **没有独立的图片查看器**。本文件就是那个查看器；文件关联路由表在
 *   kernel/explorer64.cpp（.png/.jpg/.jpeg/.bmp -> /bin/viewer <路径>）。
 *
 * 设计（"极简窗口壳"到底极简在哪）：
 *   * 一块 shm 画布 + 一个 wl_surface（与 user/store/store.c 同一套 syscall 契约：
 *     shm_create(13) / shm_map(14) / wl_surface_create(15) / damage(17) / commit(18)）；
 *   * 窗口尺寸 = **图片按比例缩到 IW_MAX x IH_MAX 以内**，不铺满全屏、不做缩放交互；
 *   * 缩放入口是整数最近邻（`sx = x*sw/dw`），**不用浮点**（本文件 -mno-sse）；
 *   * ESC 退出；有界帧循环（3 分钟上限），退出打点 reason=esc|timeout|error。
 *
 * 单 surface 像素上限：kernel/wl64.h 的 WL64_MAX_PIXELS64 = 524288（= 2 MiB 缓冲 / 4B）。
 *   本程序最大画布 762 x 541 = 412,242 px，留足余量（user/lib/wl.h 里那个 16384 是
 *   旧注释，内核早就是 524288，见该处注释；判据以 kernel/wl64.h 为准）。
 *
 * 打点契约（tests/viewer64_test.py 按这些串 grep，**格式勿改**）：
 *   [VIEWER] ver=1 pid=<n> file=<path>
 *   [VIEWER] read ok bytes=<n> magic=<png|jpeg|bmp|unknown>
 *   [VIEWER] decode ok w=<n> h=<n> comp=<n>
 *   [VIEWER] decode FAILED reason=<r> w=<n> h=<n>
 *   [VIEWER] win w=<n> h=<n> img=<w>x<h> mode=<1:1|fit>
 *   [VIEWER] fonts face=<path> bytes=<n> ok=<0|1>
 *   [VIEWER] surf id=<n> w=<n> h=<n> shm=<n> map=<n>
 *   [VIEWER] frame f=<n>
 *   [VIEWER] done frames=<n> reason=<esc|timeout|error>
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>     /* kill()：退出时收掉自己拉起的 /lib/wm.elf，别留一个吃键盘的常驻进程 */

#include "vimtu64.h"
#include "wl.h"
#include "font64.h"
#include "vi_img.h"

/* ---- 画布上限：单 surface 524288 px（kernel/wl64.h），这里留足余量 ---- */
#define IW_MAX        760
#define IH_MAX        520
/* ★ v0.11.3：查看器不再自绘标题栏 —— 走 28 号 wl_surface_set_decor，由合成器（内核路或
 *   /lib/wm.elf）按**唯一一份规格**画 25px 标题带 + 三个真按钮（最小化/最大化/关闭）。
 *   因此画布 = **客户区**：宽 = 显示图宽，高 = 显示图高，不再留 TITLE_H / PAD。
 *   （上一版自绘 20px 标题栏 + 1px 边框，所以窗口比图大 2x21；这一版 +0。）*/
#define CO_BG         0xFF14181Fu     /* 客户区底色（图片之外的留白） */

/* 缩放：zoom = 千分比，**1000 = 适配画布**（初始态）。滚轮每格 ±125‰，上限 4000（4x）。
 * 画布尺寸固定（surface 创建后不改），放大 = 在画布内取样一部分图像（裁剪视图），
 * 缩小下限就是 1000 —— 不做"比适配更小"，否则四周全是底色，等于没内容。 */
#define ZOOM_MIN      1000
#define ZOOM_MAX      4000
#define ZOOM_STEP     125

#define FRAME_MS      50u
#define MAX_FRAMES    3600            /* ≈ 3 分钟上限（人看够了就 ESC） */
#define EV_MAX        32

/* 合成器路径：内核只认 /bin/wm.elf，但桌面期卷里是 /lib/wm.elf（/bin 无该文件）。 */
static const char* const WmPath = "/lib/wm.elf";

/* 本进程拉起的 /lib/wm.elf 的 pid（<=0 = 没拉起来）。退出时杀掉它：
 * 它是**全屏合成器**，活着就会接管键盘 —— 留着它会让调用方（外壳/文件管理器）收不到按键。*/
static int g_wmpid = -1;

static void viewer_stop_wm(void) {
    if (g_wmpid > 0) {
        (void)kill(g_wmpid, 9);
        g_wmpid = -1;
        (void)vimtu64_sleep_ms(200);      /* 让内核走完 [WL64] composer release */
    }
}

/* main 里任何返回都先收掉合成器（否则留一个吃键盘的常驻进程） */
#define VI_FAIL(code) do { viewer_stop_wm(); return (code); } while (0)

/* 只判断"在不在"：open 成功即可（不读内容，省一次大读盘）。 */
static int vs_file_exists(const char* path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    (void)close(fd);
    return 1;
}

static int g_w, g_h;                  /* 画布尺寸 */
static uint32_t* g_px;
static int g_surf = -1, g_shm = -1;
static struct Font64* g_font;

static unsigned rgb(unsigned v) { return 0xFF000000u | (v & 0xFFFFFFu); }

static void fill(int x, int y, int w, int h, unsigned v) {
    const uint32_t c = rgb(v);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = g_px + (unsigned)yy * (unsigned)g_w;
        for (int xx = x; xx < x + w; xx++) row[xx] = c;
    }
}

static void text_at(int x, int y, const char* s, int size, unsigned v) {
    if (!g_font) return;
    struct F64Canvas cv;
    cv.px = g_px;
    cv.w = g_w;
    cv.h = g_h;
    (void)font_draw_text(g_font, &cv, x, y, s, size, rgb(v));
}

/* 按比例塞进 maxw x maxh（纯整数：64 位交叉乘比较，不用浮点） */
static void fit_sz(int w, int h, int maxw, int maxh, int* ow, int* oh) {
    if (w <= maxw && h <= maxh) { *ow = w; *oh = h; return; }
    const long long a = (long long)maxw * (long long)h;    /* 宽度打满时的高度分子 */
    const long long b = (long long)maxh * (long long)w;    /* 高度打满时的宽度分子 */
    if (a <= b) { *ow = maxw; *oh = (int)(a / (long long)w); }
    else        { *oh = maxh; *ow = (int)(b / (long long)h); }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

/* 魔数嗅探：只用来打点（"这是 png 还是 jpeg"），**不参与解码**（解码交给 stb_image）。 */
static const char* magic_of(const unsigned char* p, int n) {
    if (n >= 8 && p[0] == 0x89 && p[1] == 'P' && p[2] == 'N' && p[3] == 'G') return "png";
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return "jpeg";
    if (n >= 2 && p[0] == 'B' && p[1] == 'M') return "bmp";
    return "unknown";
}

/* ---- 缩放状态（★ v0.11.3 滚轮缩放）：zoom 千分比，g_ox/g_oy = 视图在"显示图"里的左上偏移 ---- */
static int g_zoom = ZOOM_MIN;      /* 1000 = 适配画布 */
static int g_ox = 0, g_oy = 0;     /* 视图偏移（显示图坐标系），<= 0 会被夹到 0 */

/* 偏移夹取：显示图比画布大才允许偏移（放大后才可能 > 0） */
static void zoom_clamp(int dw0, int dh0) {
    const int disp_w = (int)((long long)dw0 * (long long)g_zoom / 1000);
    const int disp_h = (int)((long long)dh0 * (long long)g_zoom / 1000);
    const int mx = disp_w - g_w, my = disp_h - g_h;
    if (g_ox > mx) g_ox = mx;
    if (g_oy > my) g_oy = my;
    if (g_ox < 0) g_ox = 0;
    if (g_oy < 0) g_oy = 0;
}

/* 以画布内 (cx,cy) 为锚点缩放一格 dir=±1：保住"锚点下的那个图像点"不动。
 * 整数口径：新偏移 = 锚点对应的显示图坐标 - 锚点左右位置。 */
static void zoom_at(int dir, int cx, int cy, int iw, int ih, int dw0, int dh0) {
    int nz = g_zoom + dir * ZOOM_STEP;
    if (nz < ZOOM_MIN) nz = ZOOM_MIN;
    if (nz > ZOOM_MAX) nz = ZOOM_MAX;
    if (nz == g_zoom) return;
    const int odw = (int)((long long)dw0 * (long long)g_zoom / 1000);
    const int odh = (int)((long long)dh0 * (long long)g_zoom / 1000);
    const int ndw = (int)((long long)dw0 * (long long)nz / 1000);
    const int ndh = (int)((long long)dh0 * (long long)nz / 1000);
    /* 锚点 -> 图像坐标 -> 新显示图坐标（除法只在 odw/odh >= 1 时发生，zoom >= 1000 恒成立） */
    const int ix = (int)(((long long)(cx + g_ox) * (long long)iw) / (odw > 0 ? odw : 1));
    const int iy = (int)(((long long)(cy + g_oy) * (long long)ih) / (odh > 0 ? odh : 1));
    g_zoom = nz;
    g_ox = (int)(((long long)ix * (long long)ndw) / (iw > 0 ? iw : 1)) - cx;
    g_oy = (int)(((long long)iy * (long long)ndh) / (ih > 0 ? ih : 1)) - cy;
    zoom_clamp(dw0, dh0);
}

/* 画布 -> 内核对这块 shm 的映射（w*h*4）；整幅重画，调用方随后 commit 一次。
 * ★ v0.11.3：标题栏不在这里画（合成器画）；这里只画**客户区**（图片 + 留白底色）。 */
static void paint(const unsigned char* src, int iw, int ih, int dw0, int dh0) {
    fill(0, 0, g_w, g_h, CO_BG);
    const int disp_w = (int)((long long)dw0 * (long long)g_zoom / 1000);
    const int disp_h = (int)((long long)dh0 * (long long)g_zoom / 1000);
    if (disp_w <= 0 || disp_h <= 0 || iw <= 0 || ih <= 0) return;
    /* 图：最近邻取样（放大 = 裁剪视图；缩小 = 1000‰ 时的适配）。整数运算，无浮点。 */
    for (int y = 0; y < g_h; y++) {
        long long sy = ((long long)(y + g_oy) * (long long)ih) / (long long)disp_h;
        if (sy >= (long long)ih) sy = (long long)ih - 1;
        const unsigned char* srow = src + (unsigned long)sy * (unsigned long)iw * 3u;
        uint32_t* drow = g_px + (unsigned long)y * (unsigned long)g_w;
        for (int x = 0; x < g_w; x++) {
            long long sx = ((long long)(x + g_ox) * (long long)iw) / (long long)disp_w;
            if (sx >= (long long)iw) sx = (long long)iw - 1;
            const unsigned char* p = srow + (unsigned long)sx * 3u;
            drow[x] = 0xFF000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
        }
    }
    /* 缩放非默认时在左上角标一个百分比（字体栈已在卷里，顺手把它用上 —— 也避免
     * text_at 变成未使用函数；百分比是用户判断"现在几倍"的唯一线索）。 */
    if (g_zoom != ZOOM_MIN) {
        char z[16];
        int k = 0, v = g_zoom / 10;
        char t[8];
        int tn = 0;
        if (v == 0) t[tn++] = '0';
        while (v > 0) { t[tn++] = (char)('0' + v % 10); v /= 10; }
        while (tn > 0) z[k++] = t[--tn];
        z[k++] = '%';
        z[k] = 0;
        text_at(4, 4, z, 11, 0xFFE8ECF2u);
    }
}

int main(int argc, char** argv) {
    const char* path = (argc >= 2 && argv[1]) ? argv[1] : "";
    printf("[VIEWER] ver=1 pid=%d file=%s\n", getpid(), path[0] ? path : "-");
    if (!path[0]) {
        printf("[VIEWER] done frames=0 reason=error\n");
        return 2;
    }

    /* ---- ⓪ 拉起全屏用户态合成器 /lib/wm.elf（必须在任何大 mmap 之前！）----
     * 为什么：内核内部合成只在面板区"画一次 + flip 一次"，桌面任何一次重绘都会把它盖掉
     * （实测：`[WL64] blit surf=1 … dst=964,532` 明明打出来了，screendump 那块却是终端文字）。
     * /bin/store 与 SDK 的 hello-gui 都是同一条做法：客户端自己 fork 合成器。
     * ★ fork 一定要在 vi_arena_init()/mmap 之前：proc64 的 fork 是整页物理复制，
     *   上限 256 页（≈1 MiB），本程序后面要拿 4 MiB + 12 MiB。*/
    {
        int wmpid = -1;
        if (vs_file_exists(WmPath)) {
            wmpid = fork();
            if (wmpid == 0) {
                char* av[2];
                char* env[1];
                av[0] = (char*)WmPath;
                av[1] = 0;
                env[0] = 0;
                (void)__v64_syscall(59 /*execve*/, (long)(unsigned long)WmPath,
                                    (long)(unsigned long)av, (long)(unsigned long)env, 0, 0);
                _exit(127);
            }
            g_wmpid = wmpid;
            printf("[VIEWER] wm path=%s pid=%d\n", WmPath, wmpid);
            if (wmpid > 0) vimtu64_sleep_ms(2500);   /* 合成器映射 33 MB 后备缓冲 + 标定 rdtsc */
        } else {
            printf("[VIEWER] wm missing (%s) - fall back to kernel compositor\n", WmPath);
        }
    }

    if (!vi_arena_init()) {
        printf("[VIEWER] decode FAILED reason=alloc w=0 h=0\n");
        printf("[VIEWER] done frames=0 reason=error\n");
        VI_FAIL(3);
    }

    /* ---- ① 把文件读进内存（自己的 ABI：open(6)/read(7)/close(8)，只读）---- */
    unsigned char* buf = (unsigned char*)mmap((void*)0, VI_MAX_FILE, 3, 0x22, -1, 0);
    if (!buf) {
        printf("[VIEWER] decode FAILED reason=alloc-buf w=0 h=0\n");
        printf("[VIEWER] done frames=0 reason=error\n");
        VI_FAIL(3);
    }
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[VIEWER] decode FAILED reason=open w=0 h=0\n");
        printf("[VIEWER] done frames=0 reason=error\n");
        VI_FAIL(4);
    }
    int len = 0;
    for (;;) {
        if ((unsigned)len >= VI_MAX_FILE) break;
        const long r = (long)read(fd, buf + len, VI_MAX_FILE - (unsigned)len);
        if (r <= 0) break;
        len += (int)r;
    }
    (void)close(fd);
    const char* magic = magic_of(buf, len);
    printf("[VIEWER] read ok bytes=%d magic=%s\n", len, magic);
    if (len <= 0) {
        printf("[VIEWER] decode FAILED reason=empty w=0 h=0\n");
        printf("[VIEWER] done frames=0 reason=error\n");
        VI_FAIL(4);
    }

    /* ---- ② 解码（stb_image；PNG / JPEG / BMP）---- */
    int iw = 0, ih = 0, comp = 0;
    if (!vi_decode(buf, len, &iw, &ih, &comp)) {
        printf("[VIEWER] decode FAILED reason=%s w=%d h=%d\n", vi_err(), iw, ih);
        printf("[VIEWER] done frames=0 reason=error\n");
        VI_FAIL(5);
    }
    const unsigned char* px = vi_pixels();
    printf("[VIEWER] decode ok w=%d h=%d comp=%d\n", iw, ih, comp);

    /* ---- ③ 窗口几何（按比例塞进 IW_MAX x IH_MAX）----
     * ★ v0.11.3：画布 = **客户区**（标题栏由合成器画，占 25px 在画布之外），所以 +0/+0。 */
    int dw = 0, dh = 0;
    fit_sz(iw, ih, IW_MAX, IH_MAX, &dw, &dh);
    g_w = dw;
    g_h = dh;
    const char* mode = (dw == iw && dh == ih) ? "1:1" : "fit";
    printf("[VIEWER] win w=%d h=%d img=%dx%d mode=%s\n", g_w, g_h, iw, ih, mode);

    /* ---- ④ 字体（从系统卷读；读不到就只画色块，如实报 ok=0）---- */
    g_font = font_load("/Fonts/NotoSans-Regular.ttf");
    {
        int ok = 0, bytes = 0;
        struct F64FaceInfo fi;
        if (g_font && font64_face_info(g_font, 0, &fi) == 0) { ok = fi.used; bytes = (int)fi.bytes; }
        printf("[VIEWER] fonts face=%s bytes=%d ok=%d\n", "/Fonts/NotoSans-Regular.ttf", bytes, ok);
        if (g_font) (void)font64_set_spacing(g_font, F64_SPACING_NATURAL);
    }

    /* ---- ⑤ 画布 + surface ---- */
    /* ---- ⑤ 画布 + surface + 装饰（28 号）---- */
    const unsigned bytes_needed = (unsigned)g_w * (unsigned)g_h * 4u;
    g_shm = shm_create(bytes_needed);
    g_surf = wl_surface_create(g_w, g_h, V64_WL_FORMAT_XRGB8888);
    void* va = 0;
    const int mrc = shm_map(g_shm, 0, bytes_needed, &va);
    printf("[VIEWER] surf id=%d w=%d h=%d shm=%d map=%d\n", g_surf, g_w, g_h, g_shm, mrc);
    if (g_surf <= 0 || g_shm <= 0 || mrc != 0 || !va) {
        printf("[VIEWER] done frames=0 reason=error\n");
        (void)fflush(0);
        VI_FAIL(6);
    }
    g_px = (uint32_t*)va;

    /* ★ v0.11.3：装饰 = 合成器画（25px 标题带 + 三按钮）。标题 = 文件名 + 尺寸 + 格式，
     *   与上一版自绘标题栏同口径（用户看到的信息不变），只是**画家换了**：
     *   /lib/wm.elf 在跑就它画（按钮可点、真实语义走 30 号），没有合成器就内核路画。 */
    {
        char tt[64];
        int k = 0;
        const char* base = path;
        for (int i = 0; path[i]; i++) if (path[i] == '/' || path[i] == '\\') base = path + i + 1;
        for (int i = 0; base[i] && k < (int)sizeof(tt) - 24; i++) tt[k++] = base[i];
        tt[k++] = ' ';
        {
            char num[16];
            int m = 0, v = iw;
            char t[12];
            int tn = 0;
            if (v == 0) t[tn++] = '0';
            while (v > 0) { t[tn++] = (char)('0' + v % 10); v /= 10; }
            while (tn > 0) num[m++] = t[--tn];
            num[m++] = 'x';
            v = ih; tn = 0;
            if (v == 0) t[tn++] = '0';
            while (v > 0) { t[tn++] = (char)('0' + v % 10); v /= 10; }
            while (tn > 0) num[m++] = t[--tn];
            num[m] = 0;
            for (int i = 0; num[i] && k < (int)sizeof(tt) - 6; i++) tt[k++] = num[i];
        }
        tt[k] = 0;
        const int drc = wl_surface_set_decor(g_surf, V64_WL_DECOR_WINDOW, tt);
        printf("[VIEWER] decor rc=%d title=%s\n", drc, tt);
    }

    /* ---- ⑤.5 绑定 seat：**不做这一步就一条事件也收不到** ----
     * 内核 kernel/wl64.cpp 的 seat 是"先 attach 才有 users"的模型
     * （`[WL64] seat attach/focus … users=N`）；user/browser/browser.c 同样是先
     * `wl_seat_get()` 再 dispatch，user/store/store.c 还多一次空热身 dispatch。
     * 漏了它：surface 照样上屏（像素对），但 ESC/鼠标全收不到（实测）。*/
    {
        const int seat = wl_seat_get();
        const int drc = wl_display_dispatch(0, 0, 0);
        printf("[VIEWER] seat=%d warm=%d\n", seat, drc);
    }

    /* ---- ⑥ 画一帧 + 帧循环（ESC 退出；有界）---- */
    /* ★ 顺序不能改：attach（挂上这块 shm 缓冲）-> damage（声明重画区）-> commit（上屏）。
     *   漏掉 attach 的话 surface 上**没有缓冲**，内核只会画出面板自己的底色，
     *   图一个像素都不会出现（实测踩过：surf/map 都报成功，屏上却是空的）。*/
    const int arc = wl_surface_attach(g_surf, g_shm, 0);
    printf("[VIEWER] attach rc=%d\n", arc);
    paint(px, iw, ih, dw, dh);
    (void)wl_surface_damage(g_surf, 0, 0, g_w, g_h);
    (void)wl_surface_commit(g_surf);

    int frame = 0, reason = 0;          /* 0 超时 / 1 esc / 2 close(合成器三按钮) */
    for (; frame < MAX_FRAMES; frame++) {
        if (frame == 0) printf("[VIEWER] frame f=%d\n", frame);
        struct Wl64SeatEvent ev[EV_MAX];
        const int got = wl_display_dispatch(FRAME_MS, ev, EV_MAX);
        int quit = 0, zdir = 0, zcx = -1, zcy = -1;
        for (int i = 0; i < got; i++) {
            const struct Ev64Event* e = &ev[i].ev;
            if (e->type == V64_EV_KEY_DOWN && e->code == V64_KEY_ESC) quit = 1;
            /* ★ v0.11.3：合成器的关闭按钮（30 号 ctl=CLOSE）先投 type=7 再销毁 surface */
            if (e->type == V64_WL_EV_CLOSE) { reason = 2; quit = 2; }
            /* ★ v0.11.3：滚轮缩放。dy > 0 = 向上（放大）；锚点 = 指针在客户区里的位置 */
            if (e->type == V64_EV_WHEEL && e->dy != 0) {
                zdir = (e->dy > 0) ? +1 : -1;
                zcx = ev[i].sx;
                zcy = ev[i].sy;
            }
        }
        if (quit) { if (quit == 1) reason = 1; break; }
        if (zdir) {
            if (zcx < 0 || zcx >= g_w) zcx = g_w / 2;
            if (zcy < 0 || zcy >= g_h) zcy = g_h / 2;
            const int before = g_zoom;
            zoom_at(zdir, zcx, zcy, iw, ih, dw, dh);
            if (g_zoom != before) {
                printf("[VIEWER] zoom now=%d before=%d at=%d,%d off=%d,%d\n",
                       g_zoom, before, zcx, zcy, g_ox, g_oy);
                paint(px, iw, ih, dw, dh);
                (void)wl_surface_damage(g_surf, 0, 0, g_w, g_h);
                (void)wl_surface_commit(g_surf);
            }
        }
    }
    printf("[VIEWER] done frames=%d reason=%s\n", frame,
           reason == 2 ? "close" : (reason ? "esc" : "timeout"));
    (void)wl_surface_destroy(g_surf);
    (void)fflush(0);
    viewer_stop_wm();
    return 0;
}
