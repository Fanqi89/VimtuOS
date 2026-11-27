/* user/browser/vimtu_shell.c - Edge 风格的窗内工具条（44 px Chrome）与按钮真值
 *
 * 为什么自己画 Chrome 而不是让合成器画：内核的 wl_surface 装饰只给"标题栏 + 三个窗口
 * 按钮"（那是窗口管理），浏览器自己的后退/前进/刷新/地址栏是**应用内容**，必须由客户端
 * 画在客户区里。所以客户区最上面 44 行是自绘的 Chrome，下面是页面区。
 *
 * ★ 三个按钮"真值驱动"：颜色由 g_app.can_back / can_forward / can_reload 决定，而这三个
 *   值只在 refresh_chrome_state()（vimtu_table.c）里由
 *   browser_window_{back,forward,reload}_available() 刷新 —— 也就是说，能不能点、亮不亮，
 *   和 NetSurf 的历史状态是同一份真相，不是"点了以后再判断"。
 *
 * ★ 图标全部是几何图元画的，不是字模：
 *   字模只有 ASCII 32..126，"←"/"⟳"/"⋯" 都不在里面。与其画缺字块，不如用
 *   线/三角/圆点画真图标（这也是所有浏览器的做法）。
 *
 * ★ 坐标：本文件里所有 y 都是**画布坐标**（已经含合成器标题栏的 25 行偏移）。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "netsurf/plot_style.h"

#include "vimtu_app.h"
#include "vimtu_canvas.h"
#include "vimtu_font.h"

/* 配色：取 Edge 亮色主题的近似值（深色图标 + 浅灰底 + 白地址栏） */
#define COL_CHROME_BG   0xF3F3F3u
#define COL_CHROME_LINE 0xD2D4D7u
#define COL_ICON_ON     0x202124u
#define COL_ICON_OFF    0xC3C6C9u
#define COL_ADDR_BG     0xFFFFFFu
#define COL_ADDR_EDGE   0xC7CACDu
#define COL_ADDR_TEXT   0x1B1B1Bu
#define COL_PAGE_BG     0xFFFFFFu

/* 三个按钮 + 右边"⋯"的几何（都是 44x44 的方形热区） */
#define BTN_W       44
#define BTN_BACK_X  0
#define BTN_FWD_X   BTN_W
#define BTN_REL_X   (BTN_W * 2)

static int chrome_top(void)
{
    return VIMTU_CONTENT_Y;                      /* = 25：合成器标题栏下面 */
}

static int chrome_bottom(void)
{
    return VIMTU_CONTENT_Y + VIMTU_CHROME_H;     /* = 69 */
}

/* 地址栏矩形 */
static void addr_rect(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = BTN_W * 3 + 8;
    *y0 = chrome_top() + 6;
    *x1 = VIMTU_CANVAS_W - BTN_W - 8;
    *y1 = chrome_bottom() - 6;
}

/* ---- 图标 ---- */

static void icon_back(struct vimtu_canvas *c, int cx, int cy, uint32_t col, bool on)
{
    int th = on ? 2 : 1;
    int i;

    /* 箭头：一条横杠 + 两条斜线（拼成 "<"） */
    vimtu_canvas_fill(c, cx - 6, cy - th / 2, 11, th, col);
    for (i = 0; i < 6; i++) {
        vimtu_canvas_fill(c, cx - 6 + i, cy - i - th / 2, th, th, col);
        vimtu_canvas_fill(c, cx - 6 + i, cy + i - th / 2, th, th, col);
    }
}

static void icon_forward(struct vimtu_canvas *c, int cx, int cy, uint32_t col, bool on)
{
    int th = on ? 2 : 1;
    int i;

    vimtu_canvas_fill(c, cx - 5, cy - th / 2, 11, th, col);
    for (i = 0; i < 6; i++) {
        vimtu_canvas_fill(c, cx + 5 - i, cy - i - th / 2, th, th, col);
        vimtu_canvas_fill(c, cx + 5 - i, cy + i - th / 2, th, th, col);
    }
}

/* 刷新 = 一个带缺口的圆环 + 一个箭头三角。用 24 段折线描出来（不追求光滑） */
static void icon_reload(struct vimtu_canvas *c, int cx, int cy, uint32_t col, bool on)
{
    static const int dx[12] = { 7, 6, 5, 3, 0, -3, -5, -6, -7, -6, -5, -3 };
    static const int dy[12] = { 0, 4, 6, 7, 8, 7, 6, 4, 0, -4, -6, -7 };
    int i;

    (void)on;
    for (i = 0; i < 12; i++) {
        vimtu_canvas_fill(c, cx + dx[i], cy + dy[i], 2, 2, col);       /* 下半环 */
        if (i > 2 && i < 11) {
            vimtu_canvas_fill(c, cx + dx[i], cy - dy[i], 2, 2, col);   /* 上半环（留缺口） */
        }
    }
    /* 缺口处的箭头 */
    vimtu_canvas_fill(c, cx + 4, cy - 9, 2, 6, col);
    vimtu_canvas_fill(c, cx + 6, cy - 8, 2, 4, col);
    vimtu_canvas_fill(c, cx + 8, cy - 6, 2, 2, col);
}

/* "⋯"：三个点 */
static void icon_more(struct vimtu_canvas *c, int cx, int cy, uint32_t col)
{
    int i;
    for (i = -1; i <= 1; i++) {
        vimtu_canvas_fill(c, cx + i * 7 - 1, cy - 1, 3, 3, col);
    }
}

/* ---- 工具条绘制 ---- */

void vimtu_shell_draw(struct vimtu_app *app)
{
    struct vimtu_canvas *c;
    struct plot_font_style fstyle;
    int ax0, ay0, ax1, ay1;
    int cy;
    uint32_t *url;

    if (app == NULL || !app->canvas_ok) {
        return;
    }
    c = &app->canvas;

    /* 整条 Chrome 只允许画在 [25, 69) 这段里 */
    vimtu_canvas_clip(c, 0, chrome_top(), VIMTU_CANVAS_W, chrome_bottom());

    /* 背景 + 底边 1px 分隔线 */
    vimtu_canvas_fill(c, 0, chrome_top(), VIMTU_CANVAS_W, VIMTU_CHROME_H, COL_CHROME_BG);
    vimtu_canvas_fill(c, 0, chrome_bottom() - 1, VIMTU_CANVAS_W, 1, COL_CHROME_LINE);

    /* 三个按钮的图标（真值决定亮度/粗细）*/
    cy = chrome_top() + VIMTU_CHROME_H / 2;
    icon_back(c, BTN_BACK_X + BTN_W / 2, cy,
              app->can_back ? COL_ICON_ON : COL_ICON_OFF, app->can_back);
    icon_forward(c, BTN_FWD_X + BTN_W / 2, cy,
                 app->can_forward ? COL_ICON_ON : COL_ICON_OFF, app->can_forward);
    icon_reload(c, BTN_REL_X + BTN_W / 2, cy,
                (app->can_reload || app->throbber) ? COL_ICON_ON : COL_ICON_OFF,
                app->can_reload);

    /* 地址栏：白底 + 1px 边 + 当前 URL 文字（放不下就裁在框内，不滚动）*/
    addr_rect(&ax0, &ay0, &ax1, &ay1);
    vimtu_canvas_fill(c, ax0, ay0, ax1 - ax0, ay1 - ay0, COL_ADDR_BG);
    vimtu_canvas_frame(c, ax0, ay0, ax1 - ax0, ay1 - ay0, 1, COL_ADDR_EDGE);

    fstyle.families = NULL;
    fstyle.family = PLOT_FONT_FAMILY_SANS_SERIF;
    /* 12.8pt -> 16px 字模（与页面正文同一把尺子） */
    fstyle.size = plot_style_int_to_fixed(13);
    fstyle.weight = 400;
    fstyle.flags = FONTF_NONE;
    fstyle.background = COL_ADDR_BG;
    fstyle.foreground = COL_ADDR_TEXT;

    {
        const char *txt = (app->url[0] != '\0') ? app->url : "about:blank";
        size_t len = strlen(txt);
        size_t maxchars;
        int baseline = ay0 + (ay1 - ay0) / 2 + VIMTU_FONT_BASELINE / 2;
        int room = (ax1 - ax0) - 12;
        int perchar = vimtu_font_advance(&fstyle);

        if (perchar < 1) {
            perchar = VIMTU_FONT_ADV;
        }
        maxchars = (size_t)(room / perchar);
        if (len > maxchars) {
            len = maxchars;                     /* 右侧截断：地址栏不做横向滚动 */
        }
        vimtu_canvas_text(c, &fstyle, ax0 + 6, baseline, txt, len);
    }

    /* 右边"⋯" */
    icon_more(c, VIMTU_CANVAS_W - BTN_W / 2, cy, COL_ICON_ON);

    /* 加载中：在地址栏底部画一条进度条（没有真进度，就画一段固定长度，
     * 但只在 throbber 为真时出现 —— 这仍然是"真值驱动"） */
    if (app->throbber) {
        int w = (ax1 - ax0) / 3;
        vimtu_canvas_fill(c, ax0 + 1, ay1 - 3, w, 2, 0x0F6CBDu);
    }

    /* 恢复成整面裁剪，别把后面的绘制卡在这条 44 行的带子里 */
    vimtu_canvas_clip(c, 0, 0, c->w, c->h);

    (void)url;
    (void)COL_PAGE_BG;
}

/* ---- 命中测试 + 动作 ---- */

bool vimtu_shell_click(struct vimtu_app *app, int x, int y)
{
    if (app == NULL) {
        return false;
    }
    /* 只有 Chrome 那 44 行归外壳；下面交给页面 */
    if (y < chrome_top() || y >= chrome_bottom()) {
        return false;
    }

    if (x >= BTN_BACK_X && x < BTN_BACK_X + BTN_W) {
        vimtu_log("[BROWSER] chrome back can=%d\n", app->can_back ? 1 : 0);
        vimtu_window_back();
        return true;
    }
    if (x >= BTN_FWD_X && x < BTN_FWD_X + BTN_W) {
        vimtu_log("[BROWSER] chrome forward can=%d\n", app->can_forward ? 1 : 0);
        vimtu_window_forward();
        return true;
    }
    if (x >= BTN_REL_X && x < BTN_REL_X + BTN_W) {
        vimtu_log("[BROWSER] chrome reload\n");
        vimtu_window_reload();
        return true;
    }
    if (x >= VIMTU_CANVAS_W - BTN_W) {
        /* "⋯"：菜单没有做（不是漏了，是没排进这一刀） */
        vimtu_log("[BROWSER] chrome menu UNVERIFIED\n");
        return true;
    }

    /* 地址栏：键盘输入没接（见 [BROWSER] address input UNVERIFIED） */
    {
        int ax0, ay0, ax1, ay1;
        addr_rect(&ax0, &ay0, &ax1, &ay1);
        if (x >= ax0 && x < ax1 && y >= ay0 && y < ay1) {
            app->url_editing = true;
            app->url_fresh = true;          /* 第一个字符把旧 URL 清掉 */
            vimtu_log("[BROWSER] address edit start\n");
            return true;
        }
    }
    return true;                      /* Chrome 上的其它位置：吃掉，别透给页面 */
}

void vimtu_shell_motion(struct vimtu_app *app, int x, int y)
{
    /* 悬停高亮没做（需要重绘整条 Chrome，收益低）。留一个明确的空实现，
     * 而不是让事件循环里出现"这个分支没人管"的空白。 */
    (void)app;
    (void)x;
    (void)y;
}
