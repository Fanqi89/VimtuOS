/* browser.c - VimtuOS 浏览器（ring3 GUI，v0.5.0）
 *
 * 它是什么：一个**真窗口**的网页浏览器骨架 —— 用内核的 wl_surface/shm ABI 拿一块 896x580 的
 *   缓冲（v0.5.0 把 shm 单对象上限从 64 KiB 提到 2 MiB，否则连这个窗口都放不下），用自己的
 *   HTML 子集解析器（user/browser/html.c）把文档压成块，用 user/lib 的 TrueType 栈（font64.c）
 *   排版折行并绘制；输入走 wl_display_dispatch 的 seat 事件。
 *
 * 交付（与 /bin/store、/bin/fontdemo 同一条纪律）：**系统卷里的 /bin/browser**，
 *   tools/browser_pack_win.py 构建期写入并逐字节回读；内核二进制里一个字节都不加。
 *
 * 现在能做什么（v0.5.0 的如实边界）：
 *   * 渲染 HTML 子集：h1/h2/h3/p/div/br/hr/ul/li/pre/a/b/u/center/title + 实体解码 + 内联
 *     style 的 color/background-color/text-align/font-size/font-weight；
 *   * 本地文件：`/www/index.html`（发行版预置的首页）/ 内置欢迎页兜底；
 *   * 滚动：PageUp/PageDown/↑/↓/滚轮；ESC 退出；
 *   * **还没做**：HTTP/HTTPS（TCP 客户端在下一批接 netraw 号 53）、图片/表格/CSS 选择器/JS、
 *     链接点击跳转（只标出链接颜色与下划线）、文本选择/输入框。
 *
 * 打点（tests/browser64_test.py grep；格式别改）：
 *   [BROWSER] win surf=.. w=.. h=.. shm=.. map=..
 *   [BROWSER] fonts faces=.. ok=..
 *   [BROWSER] load url=.. bytes=.. rc=.. blocks=.. text=.. title=..
 *   [BROWSER] render frame=.. scroll=.. lines=.. page_h=.. drawn=..
 *   [BROWSER] key code=.. scroll=..     /  [BROWSER] wheel dy=.. scroll=..
 *   [BROWSER] done frames=.. reason=esc|timeout
 */
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <vimtu64.h>
#include <wl.h>
#include <font64.h>
#include "html.h"

/* user/lib/syscall.c 里有 open() 的定义，但没有头声明（只有 write/read/close 在 unistd.h） */
int open(const char* path, int flags, ...);

/* ---- 窗口几何（与 tests/browser64_test.py 的判据一致；改这里要同步那个脚本）---- */
#define BW            896
#define BH            580
#define BW_BYTES      (BW * BH * 4)
#define BAR_TITLE_H   28
#define BAR_ADDR_H    26
#define PAGE_X        10
#define PAGE_Y        (BAR_TITLE_H + BAR_ADDR_H + 6)          /* 60 */
#define PAGE_W        (BW - 2 * PAGE_X)                       /* 876 */
#define PAGE_H        (BH - PAGE_Y - 24)                      /* 496 */
#define STATUS_Y      (BH - 24)

#define MAX_FRAMES    3600            /* 约 108 s（30 ms/帧） */
#define FRAME_MS      30u
#define EV_MAX        16

/* ---- 调色板（XRGB8888；alpha 恒 0xFF）---- */
#define CO_PAGE    0xFFFFFFFFu
#define CO_TEXT    0xFF1A1A1Au
#define CO_CHROME  0xFF20242Cu
#define CO_CHROME2 0xFF2C3340u
#define CO_LINE    0xFFD8DCE4u
#define CO_LINK    0xFF1E6FD9u
#define CO_DIMTXT  0xFF9AA6B8u
#define CO_BRIGHT  0xFFE8ECF2u

/* mmap 区：源文档 64 KiB + 文本池 56 KiB + 块表 */
#define ARENA_BYTES   (192u * 1024u)

static uint32_t*        g_px;
static struct F64Canvas g_cv;
static struct Font64*   g_font;
static int g_surf = -1, g_shm = -1;
static unsigned char*   g_arena;
static struct HDoc      g_doc;
static char*            g_src;
static int  g_scroll = 0, g_page_h = 0, g_lines = 0;
static int  g_dirty = 1, g_frames = 0;
static char g_url[64] = "/www/index.html";

/* 内置欢迎页（卷里没有 /www/index.html 时用） */
static const char kWelcome[] =
    "<html><head><title>VimtuOS Browser</title></head><body>"
    "<h1>VimtuOS Browser 0.5.0</h1>"
    "<p>这是 ring3 的<b>真窗口</b>浏览器骨架：内核给的 surface + 共享缓冲，"
    "排版与绘制全在用户态。</p>"
    "<hr>"
    "<h2>它现在能做什么</h2>"
    "<ul>"
    "<li>解析 HTML 子集：标题 / 段落 / 列表 / 分隔线 / 预排版 / 实体</li>"
    "<li>TrueType 排版折行（Noto Sans + Noto Sans SC + Unifont 兜底）</li>"
    "<li>滚动：PageUp / PageDown / 上下键 / 滚轮</li>"
    "<li>读本地文件：<a href=\"/www/index.html\">/www/index.html</a></li>"
    "</ul>"
    "<h3>还没做（如实）</h3>"
    "<p>HTTP/HTTPS、图片、表格、CSS 选择器、JavaScript、链接跳转、文本选择。</p>"
    "<pre>  pre 块保留空白：\n    col1    col2\n    A       B</pre>"
    "<p align=center>居中段落 —— &amp; 实体解码：&lt;tag&gt; &quot;quote&quot; &nbsp;nbsp</p>"
    "</body></html>";

static unsigned px(unsigned rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static void fill(int x, int y, int w, int h, unsigned argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > BW) w = BW - x;
    if (y + h > BH) h = BH - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = g_px + (unsigned)yy * BW;
        for (int xx = x; xx < x + w; xx++) row[xx] = argb;
    }
}

static void txt(int x, int y, const char* s, int size, unsigned argb) {
    if (!g_font || !s) return;
    (void)font_draw_text(g_font, &g_cv, x, y, s, size, argb);
}

static void submit(void) {
    (void)wl_surface_attach(g_surf, g_shm, 0);
    (void)wl_surface_damage(g_surf, 0, 0, BW, BH);
    (void)wl_surface_commit(g_surf);
}

/* ==================== 加载 ==================== */
static int read_file(const char* path, char* dst, int cap) {
    const int fd = open(path, 0);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        if (total >= cap - 1) break;
        const int n = (int)read(fd, dst + total, (size_t)(cap - 1 - total));
        if (n <= 0) break;
        total += n;
    }
    (void)close(fd);
    dst[total] = 0;
    return total;
}

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}

/* ==================== 排版/绘制 ==================== */
static int block_size(const struct HBlock* b) {
    switch (b->type) {
        case H_H1: return 30;
        case H_H2: return 25;
        case H_H3: return 21;
        case H_PRE: return 14;
        default: return b->size ? (int)b->size : 16;
    }
}

/* UTF-8 一个字符的字节数与码点 */
static int u8_next(const char* s, int len, int pos, unsigned* cp_out) {
    const unsigned char c = (unsigned char)s[pos];
    int cl = 1;
    unsigned cp = c;
    if (c >= 0xF0) { cl = 4; cp = c & 0x07u; }
    else if (c >= 0xE0) { cl = 3; cp = c & 0x0Fu; }
    else if (c >= 0xC0) { cl = 2; cp = c & 0x1Fu; }
    if (pos + cl > len) cl = len - pos;
    for (int k = 1; k < cl; k++) cp = (cp << 6) | ((unsigned char)s[pos + k] & 0x3Fu);
    *cp_out = cp;
    return cl;
}

/* 折行：返回块用掉的行数；y0 = 该块第一行在**窗口坐标**里的基线（已含滚动）。
 * 只有与页面区相交的行才真画。 */
static int draw_para(const struct HBlock* b, int y0) {
    const int size = block_size(b);
    const int lh = size + 7;
    const char* s = g_doc.textpool + b->off;
    const int len = (int)b->len;
    const unsigned col = px(b->color ? b->color : (CO_TEXT & 0xFFFFFFu));
    int lines = 0, pos = 0;
    char line[512];

    for (;;) {
        int n = 0, li = 0, last_space = -1;
        while (pos + n < len) {
            const unsigned char c = (unsigned char)s[pos + n];
            if (c == '\n') break;
            if (c == ' ') last_space = n;
            unsigned cp = 0;
            const int cl = u8_next(s, len, pos + n, &cp);
            const int adv = g_font ? font64_advance(g_font, cp, size) : (size / 2);
            if (li + adv > PAGE_W && n > 0) {
                if (last_space > 0) n = last_space;
                break;
            }
            if (n + cl > (int)sizeof(line) - 2) break;
            n += cl;
            li += adv;
        }
        if (n > (int)sizeof(line) - 2) n = (int)sizeof(line) - 2;
        for (int k = 0; k < n; k++) line[k] = s[pos + k];
        line[n] = 0;
        while (n > 0 && line[n - 1] == ' ') line[--n] = 0;

        const int y = y0 + lines * lh;
        if (y + lh > PAGE_Y && y < PAGE_Y + PAGE_H) {
            int x = PAGE_X;
            if (b->align != H_ALIGN_LEFT && g_font) {
                const int tw = font64_text_width(g_font, line, size);
                if (tw > 0 && tw < PAGE_W) {
                    x = (b->align == H_ALIGN_CENTER) ? (PAGE_X + (PAGE_W - tw) / 2)
                                                     : (PAGE_X + PAGE_W - tw);
                }
            }
            txt(x, y, line, size, col);
            if (b->underline) fill(x, y + size + 1, li > 0 ? li : 1, 1, px(CO_LINK));
        }
        lines++;
        pos += n;
        if (pos < len && s[pos] == '\n') pos++;
        else if (pos < len && s[pos] == ' ') pos++;
        if (pos >= len || n == 0) break;
    }
    return lines ? lines : 1;
}

static int layout_measure(void) {
    int y = 0;
    for (int i = 0; i < g_doc.nblocks; i++) {
        const struct HBlock* b = &g_doc.blocks[i];
        if (b->type == H_HR) { y += 14; continue; }
        const int size = block_size(b);
        const int lh = size + 7;
        const char* s = g_doc.textpool + b->off;
        const int len = (int)b->len;
        int pos = 0, n_lines = 1, li = 0;
        while (pos < len) {
            const unsigned char c = (unsigned char)s[pos];
            if (c == '\n') { n_lines++; pos++; li = 0; continue; }
            unsigned cp = 0;
            const int cl = u8_next(s, len, pos, &cp);
            const int adv = g_font ? font64_advance(g_font, cp, size) : (size / 2);
            if (li + adv > PAGE_W) { n_lines++; li = 0; continue; }
            li += adv;
            pos += cl;
        }
        y += n_lines * lh + size / 2;
    }
    return y;
}

static int draw_page(int scroll) {
    int y = 0, drawn = 0, lines_total = 0;
    for (int i = 0; i < g_doc.nblocks; i++) {
        const struct HBlock* b = &g_doc.blocks[i];
        if (b->type == H_HR) {
            const int yy = PAGE_Y + y - scroll;
            if (yy >= PAGE_Y && yy < PAGE_Y + PAGE_H) {
                fill(PAGE_X, yy, PAGE_W, 1, px(CO_LINE & 0xFFFFFFu));
                drawn++;
            }
            y += 14;
            continue;
        }
        const int size = block_size(b);
        const int lh = size + 7;
        const int used = draw_para(b, PAGE_Y + y - scroll);
        if ((PAGE_Y + y - scroll) + used * lh > PAGE_Y && (PAGE_Y + y - scroll) < PAGE_Y + PAGE_H) drawn++;
        lines_total += used;
        y += used * lh + size / 2;
    }
    g_lines = lines_total;
    return drawn;
}

static void render(void) {
    fill(0, 0, BW, BH, px(CO_PAGE & 0xFFFFFFu));
    fill(0, 0, BW, BAR_TITLE_H, px(CO_CHROME & 0xFFFFFFu));
    txt(12, 7, "VimtuOS Browser", 15, px(CO_BRIGHT));
    fill(BW - 96, 4, 88, 20, px(0xFFC0392Bu));
    txt(BW - 78, 6, "关闭 X", 15, px(0xFFFFFFFFu));
    txt(BW - 300, 8, "点击页面上下半翻页  点 X 退出", 12, px(CO_DIMTXT));
    fill(0, BAR_TITLE_H, BW, BAR_ADDR_H, px(CO_CHROME2 & 0xFFFFFFu));
    {
        char line[96];
        str_copy(line, "地址: ", 96);
        int n = 0;
        while (line[n]) n++;
        for (int i = 0; g_url[i] && n < 88; i++) line[n++] = g_url[i];
        line[n] = 0;
        txt(12, BAR_TITLE_H + 5, line, 14, px(CO_BRIGHT));
    }
    fill(PAGE_X - 1, PAGE_Y - 1, PAGE_W + 2, PAGE_H + 2, px(0xE4E8EEu));
    fill(PAGE_X, PAGE_Y, PAGE_W, PAGE_H, px(CO_PAGE & 0xFFFFFFu));
    const int drawn = draw_page(g_scroll);
    printf("[BROWSER] render frame=%d scroll=%d lines=%d page_h=%d drawn=%d\n",
           g_frames, g_scroll, g_lines, g_page_h, drawn);
    fill(0, STATUS_Y, BW, BH - STATUS_Y, px(CO_CHROME & 0xFFFFFFu));
    {
        char st[96];
        int n = 0, k = 0, d[10];
        const char* a = "块 ";
        for (int i = 0; a[i]; i++) st[n++] = a[i];
        int v = g_doc.nblocks;
        if (v == 0) d[k++] = 0;
        while (v > 0) { d[k++] = v % 10; v /= 10; }
        while (k) st[n++] = (char)('0' + d[--k]);
        const char* b2 = "  行 ";
        for (int i = 0; b2[i]; i++) st[n++] = b2[i];
        v = g_lines; k = 0;
        if (v == 0) d[k++] = 0;
        while (v > 0) { d[k++] = v % 10; v /= 10; }
        while (k) st[n++] = (char)('0' + d[--k]);
        const char* c2 = "  滚动 ";
        for (int i = 0; c2[i]; i++) st[n++] = c2[i];
        v = g_scroll; k = 0;
        if (v == 0) d[k++] = 0;
        while (v > 0) { d[k++] = v % 10; v /= 10; }
        while (k) st[n++] = (char)('0' + d[--k]);
        st[n] = 0;
        txt(12, STATUS_Y + 4, st, 13, px(CO_DIMTXT));
    }
    submit();
    g_dirty = 0;
}

static void load_doc(const char* path) {
    int bytes = -1, rc = 0;
    if (path && path[0]) bytes = read_file(path, g_src, H_MAX_TEXT);
    if (bytes <= 0) {
        int n = 0;
        while (kWelcome[n] && n < H_MAX_TEXT - 1) { g_src[n] = kWelcome[n]; n++; }
        g_src[n] = 0;
        bytes = n;
        str_copy(g_url, "builtin:index", 64);
        rc = -2;
    } else {
        str_copy(g_url, path, 64);
    }
    g_doc.textpool = (char*)(g_arena + 128 * 1024);
    g_doc.blocks   = (struct HBlock*)(g_arena + 128 * 1024 + H_MAX_TEXT + 1024);
    const int prc = html_parse(g_src, bytes, &g_doc, g_doc.textpool, H_MAX_TEXT,
                               g_doc.blocks, H_MAX_BLOCKS);
    if (prc < 0) rc = prc;
    g_scroll = 0;
    g_page_h = layout_measure();
    printf("[BROWSER] load url=%s bytes=%d rc=%d blocks=%d text=%d title=%s\n",
           g_url[0] ? g_url : "-", bytes, rc, g_doc.nblocks, g_doc.text_used,
           (g_doc.title_len > 0) ? (g_doc.textpool + g_doc.title_off) : "-");
}

/* ==================== 主 ==================== */
int main(void) {
    g_arena = (unsigned char*)mmap(0, ARENA_BYTES, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, -1, 0);
    if (!g_arena) {
        printf("[BROWSER] FAILED arena mmap\n");
        return 1;
    }
    g_src = (char*)g_arena;

    g_font = font_load("/Fonts/NotoSans-Regular.ttf");
    if (g_font) {
        (void)font64_add_face(g_font, "/Fonts-open/NotoSansSC-Regular.ttf", F64_KIND_AUTO);
        (void)font64_add_face(g_font, "/Fonts-open/unifont-14.0.01.ttf", F64_KIND_AUTO);
        (void)font64_set_spacing(g_font, F64_SPACING_NATURAL);
    }
    {
        struct F64FaceInfo fi;
        int ok = 0, bytes = 0;
        if (g_font && font64_face_info(g_font, 0, &fi) == 0) { ok = fi.used; bytes = (int)fi.bytes; }
        printf("[BROWSER] fonts faces=%d ok=%d bytes=%d\n",
               g_font ? font64_face_count(g_font) : 0, ok, bytes);
    }

    g_shm  = shm_create(BW_BYTES);
    g_surf = wl_surface_create(BW, BH, V64_WL_FORMAT_XRGB8888);
    void* va = 0;
    const int mrc = shm_map(g_shm, 0, BW_BYTES, &va);
    printf("[BROWSER] win surf=%d w=%d h=%d shm=%d map=%d\n", g_surf, BW, BH, g_shm, mrc);
    if (g_surf <= 0 || g_shm <= 0 || mrc != 0 || !va) {
        printf("[BROWSER] done frames=0 reason=error\n");
        (void)fflush(0);
        return 1;
    }
    g_px = (uint32_t*)va;
    g_cv.px = g_px;
    g_cv.w = BW;
    g_cv.h = BH;

    load_doc("/www/index.html");
    render();

    int reason = 0;
    for (; g_frames < MAX_FRAMES; g_frames++) {
        struct Wl64SeatEvent ev[EV_MAX];
        const int got = wl_display_dispatch(FRAME_MS, ev, EV_MAX);
        for (int i = 0; i < got; i++) {
            const struct Ev64Event* e = &ev[i].ev;
            const int maxs = (g_page_h > PAGE_H) ? (g_page_h - PAGE_H) : 0;
            if (e->type == V64_EV_KEY_DOWN) {
                const int code = (int)e->code;
                const int before = g_scroll;
                if (code == (int)V64_KEY_ESC) { reason = 1; break; }
                else if (code == (int)V64_KEY_PAGEDOWN) g_scroll += PAGE_H - 20;
                else if (code == (int)V64_KEY_PAGEUP)   g_scroll -= PAGE_H - 20;
                else if (code == (int)V64_KEY_DOWN)     g_scroll += 24;
                else if (code == (int)V64_KEY_UP)       g_scroll -= 24;
                if (g_scroll < 0) g_scroll = 0;
                if (g_scroll > maxs) g_scroll = maxs;
                if (g_scroll != before) {
                    printf("[BROWSER] key code=%d scroll=%d\n", code, g_scroll);
                    g_dirty = 1;
                }
            } else if (e->type == V64_EV_WHEEL) {
                g_scroll += (e->dy > 0) ? 48 : -48;
                if (g_scroll < 0) g_scroll = 0;
                if (g_scroll > maxs) g_scroll = maxs;
                printf("[BROWSER] wheel dy=%d scroll=%d\n", (int)e->dy, g_scroll);
                g_dirty = 1;
            } else if (e->type == V64_EV_MOUSE_DOWN && ev[i].inside && ev[i].sx >= 0) {
                // ★ 鼠标交互：不依赖键盘焦点（内核把键盘事件只投给"指针命中过"的 surface，
                //   而没有真实点击时可能永远没有焦点）。点标题栏右侧 [X] = 退出；
                //   点页面区下半 = 下翻一屏、上半 = 上翻一屏。
                const int mx = (int)ev[i].sx, my = (int)ev[i].sy;
                if (my < BAR_TITLE_H && mx >= BW - 104) {
                    printf("[BROWSER] close click x=%d y=%d -> quit\n", mx, my);
                    reason = 1;
                    break;
                }
                if (my >= PAGE_Y && my < PAGE_Y + PAGE_H) {
                    if (my > PAGE_Y + PAGE_H / 2) g_scroll += PAGE_H - 40;
                    else g_scroll -= PAGE_H - 40;
                    if (g_scroll < 0) g_scroll = 0;
                    if (g_scroll > maxs) g_scroll = maxs;
                    printf("[BROWSER] click x=%d y=%d scroll=%d\n", mx, my, g_scroll);
                    g_dirty = 1;
                }
            }
        }
        if (reason) break;
        if (g_dirty) render();
    }
    if (!reason) reason = 2;
    printf("[BROWSER] done frames=%d reason=%s\n", g_frames, (reason == 1) ? "esc" : "timeout");
    (void)wl_surface_destroy(g_surf);
    (void)fflush(0);
    return 0;
}
