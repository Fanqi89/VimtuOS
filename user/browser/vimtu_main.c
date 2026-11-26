/* user/browser/vimtu_main.c - 入口、初始化、事件循环
 *
 * 为什么自己写 _start（而不是用 musl 的 crt1.o）：
 *   musl 的 crt_arch.h 里有 `lea _DYNAMIC(%rip),%rsi`，本映像静态链接在 4 GiB+576 KiB
 *   （user/tls/httpsprobe64.ld），_DYNAMIC 无处解析 -> 链接期 R_X86_64_PC32 out of range，
 *   或者更糟：ld.lld 直接报 "cannot find entry symbol _start"、产物没有入口。
 *   user/tls/httpsprobe.c 已经踩过这个坑并给出了口径：自己定义 _start，把内核摆好的
 *   SysV 初始栈（argc/argv/envp/auxv）交给 musl 的 __libc_start_main。
 *
 * 为什么事件循环长这样：
 *   NetSurf 的核心是"定时器驱动的状态机"，一切推进（取数据 / 排版 / 重绘请求）都发生在
 *   guit->misc->schedule 排的回调里。所以循环只做三件事，顺序不能换：
 *     ① vimtu_schedule_run()  跑到期回调，拿到"下一个到期还有多少 ms"
 *     ② wl_dispatch(...)      等输入（同时是唯一真正把已 commit 的帧合成上屏的地方）
 *     ③ 处理事件 -> 该导航的导航、该滚的滚；有脏区就 vimtu_window_paint() 画一帧
 *   把 ② 的 timeout 交给 ① 算出来的值，才不会出现"定时器已经到期但循环还在睡"。
 *
 * 串口证据（stdout 是全缓冲的，所以一律 write(1,...)，不用 printf）：
 *   [BROWSER] init ok           —— 表注册 + netsurf_init 全过了
 *   [BROWSER] open url=...      —— browser_window_create 真的带着 URL 起来了
 *   [BROWSER] render boxes=<n>  —— 页面排版出了多少个 box（真数，走 html_get_box_tree）
 *   [BROWSER] paint frame=<n> ink=<x.x>%  —— 这一帧页面区里非背景像素的占比
 *   [BROWSER] fatal ...         —— 不可恢复错误，打完就退出
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* utils/errors.h 与 netsurf/window.h 必须先来：
 * netsurf 系列头 里用了 nserror / struct gui_window_table 却不自带这两个头 */
#include "utils/errors.h"
#include "netsurf/window.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/netsurf.h"
#include "netsurf/content.h"     /* content_get_type：先确认是 HTML 才敢走 box 树 */
#include "netsurf/plotters.h"
#include "utils/errors.h"
#include "utils/nsoption.h"
#include "utils/nsurl.h"

#include "html/box.h"          /* struct box（统计 boxes= 用） */

#include "vimtu_abi.h"
#include "vimtu_app.h"
#include "vimtu_canvas.h"

/* 内核 input64.h 的事件类型（vimtu_abi.h 里那段注释是旧的 0..3 口径，以这里为准） */
#define EV_KEY_DOWN   1u
#define EV_KEY_UP     2u
#define EV_MOUSE_MOVE 3u
#define EV_MOUSE_DOWN 4u
#define EV_MOUSE_UP   5u
#define EV_WHEEL      6u
#define EV_WINDOW_CLOSE 7u          /* WL64_EV_CLOSE64：合成器关掉了我们这扇窗 */

/* kernel/input.cpp 的方向键导航码（复用键盘缓冲） */
#define NAV_UP    0xFDu
#define NAV_DOWN  0xFEu
#define NAV_LEFT  0xFBu
#define NAV_RIGHT 0xFCu

/* 全局表（desktop/gui_factory.c 定义）；输入换算要借 window->get_scroll 读滚动量 */
extern struct netsurf_table *guit;

/* 手写原型：避免把 libdom 的头整条拖进来（html_save.h 里还有 dom_document 签名） */
struct hlcache_handle;
struct box *html_get_box_tree(struct hlcache_handle *h);

/* 滚轮一格滚多少像素（NetSurf 的 dy>0 = 向上/远离用户） */
#define WHEEL_STEP 48

/* ================================================================= 串口输出 */

void vimtu_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return;
    }
    if (n > (int)sizeof(buf) - 1) {
        n = (int)sizeof(buf) - 1;
    }
    /* 一次 write 一行：串口上"半行"是最难读的证据形态 */
    (void)write(1, buf, (size_t)n);
}

/* ============================================================== 启动（_start） */

extern int __libc_start_main(int (*main_fn)(int, char **, char **), int argc, char **argv,
                             void (*init_dummy)(void), void (*fini_dummy)(void),
                             void (*ldso_dummy)(void));

static int vimtu_main(int argc, char **argv, char **envp);

static uint64_t *g_start_sp;

__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void)
{
    __asm__ volatile(
        "xor %rbp, %rbp\n\t"
        "mov %rsp, %rdi\n\t"
        "and $-16, %rsp\n\t"
        "call vimtu_start_c\n\t"
        "hlt\n\t");
}

__attribute__((used, noinline))
static void vimtu_start_c(uint64_t *sp)
{
    int argc;
    char **argv;

    g_start_sp = sp;
    argc = (int)sp[0];
    argv = (char **)(sp + 1);
    __libc_start_main(vimtu_main, argc, argv, 0, 0, 0);
    _exit(127);                       /* __libc_start_main 不返回；兜底也不掉回 hlt */
}

/* ============================================================== 输入 -> 页面 */

/* 页面区左上角是 (0, VIMTU_PAGE_Y)，因此：
 *   文档坐标 = 画布坐标 - 页面区原点 + 滚动量
 * 这是"点击落在哪个链接上"唯一需要的一次坐标换算，写在一处，别散到三处去。 */
static void handle_page_pointer(int x, int y, browser_mouse_state state, bool click)
{
    int scrollx = 0;
    int scrolly = 0;
    int docx;
    int docy;

    if (g_app.win != NULL && guit != NULL && guit->window != NULL &&
        guit->window->get_scroll != NULL) {
        (void)guit->window->get_scroll(g_app.win, &scrollx, &scrolly);
    }
    if (g_app.bw == NULL) {
        return;
    }

    docx = x + scrollx;
    docy = (y - VIMTU_PAGE_Y) + scrolly;

    if (click) {
        browser_window_mouse_click(g_app.bw, state, docx, docy);
    } else {
        browser_window_mouse_track(g_app.bw, state, docx, docy);
    }
}

static uint32_t map_key(uint32_t code)
{
    switch (code) {
    case NAV_UP:
        return NS_KEY_UP;
    case NAV_DOWN:
        return NS_KEY_DOWN;
    case NAV_LEFT:
        return NS_KEY_LEFT;
    case NAV_RIGHT:
        return NS_KEY_RIGHT;
    case 0x0Du:
        return NS_KEY_CR;
    case 0x1Bu:
        return NS_KEY_ESCAPE;
    case 0x08u:
        return NS_KEY_DELETE_LEFT;
    case 0x7Fu:
        return NS_KEY_DELETE_RIGHT;
    case 0x09u:
        return NS_KEY_TAB;
    default:
        return code;                  /* 可打印 ASCII 原样当 UCS4 */
    }
}

static void handle_seat_event(const struct Wl64SeatEvent *se)
{
    int x;
    int y;

    if (se->surf == 0 || se->inside == 0) {
        return;                       /* 指针不在我们这扇窗口上 */
    }
    x = se->sx;
    y = se->sy;                       /* 已经含合成器标题栏的 25 行偏移 */

    switch (se->ev.type) {
    case EV_MOUSE_DOWN:
        if (y < VIMTU_CONTENT_Y) {
            return;                   /* 标题栏归合成器，事件本来也不该投进来 */
        }
        if (y >= VIMTU_PAGE_Y) {
            if (g_app.bw != NULL) {
                handle_page_pointer(x, y, BROWSER_MOUSE_PRESS_1, true);
            }
        } else {
            (void)vimtu_shell_click(&g_app, x, y);
        }
        g_app.need_paint = true;
        break;

    case EV_MOUSE_UP:
        if (y >= VIMTU_PAGE_Y && g_app.bw != NULL) {
            handle_page_pointer(x, y, BROWSER_MOUSE_CLICK_1, true);
            g_app.need_paint = true;
        }
        break;

    case EV_MOUSE_MOVE:
        if (y >= VIMTU_PAGE_Y) {
            if (g_app.bw != NULL) {
                handle_page_pointer(x, y, BROWSER_MOUSE_HOVER, false);
            }
        } else {
            vimtu_shell_motion(&g_app, x, y);
        }
        break;

    case EV_WHEEL: {
        int scrollx = 0;
        int scrolly = 0;
        int maxy;
        int extent_h = VIMTU_PAGE_H;

        if (g_app.win == NULL) {
            break;
        }
        if (guit != NULL && guit->window != NULL && guit->window->get_scroll != NULL) {
            (void)guit->window->get_scroll(g_app.win, &scrollx, &scrolly);
        }
        /* browser_window_get_extents 给的是内容总高，用它夹住滚动范围 */
        if (g_app.bw != NULL) {
            int ew = 0;
            if (browser_window_get_extents(g_app.bw, true, &ew, &extent_h) != NSERROR_OK) {
                extent_h = VIMTU_PAGE_H;
            }
        }
        maxy = extent_h - VIMTU_PAGE_H;
        if (maxy < 0) {
            maxy = 0;
        }
        scrolly -= (int)se->ev.dy * WHEEL_STEP;
        if (scrolly < 0) {
            scrolly = 0;
        }
        if (scrolly > maxy) {
            scrolly = maxy;
        }
        {
            struct rect r;
            r.x0 = scrollx;
            r.y0 = scrolly;
            r.x1 = scrollx;
            r.y1 = scrolly;
            if (guit != NULL && guit->window != NULL && guit->window->set_scroll != NULL) {
                (void)guit->window->set_scroll(g_app.win, &r);
            }
        }
        g_app.need_paint = true;
        break;
    }

    case EV_KEY_DOWN: {
        bool handled = false;
        uint32_t code = se->ev.code;

        if (g_app.url_editing) {
            /* 地址栏编辑态：键盘输入**没有接**（见报告里的 UNVERIFIED 清单）。
             * ESC 先退出这个状态，免得用户被"键盘没反应"困住。 */
            if (code == 0x1Bu) {
                g_app.url_editing = false;
            }
            vimtu_log("[BROWSER] address input UNVERIFIED code=0x%x\n", (unsigned)code);
            break;
        }
        if (g_app.bw != NULL && code < 0x100u) {
            handled = browser_window_key_press(g_app.bw, map_key(code));
        }
        if (handled) {
            g_app.need_paint = true;
        }
        break;
    }

    case EV_WINDOW_CLOSE:
        vimtu_log("[BROWSER] window close requested\n");
        g_app.quit = true;
        break;

    default:
        break;
    }
}

/* ================================================================ boxes 统计 */

static unsigned count_boxes(const struct box *b)
{
    unsigned n = 0;

    while (b != NULL) {
        n++;
        if (b->children != NULL) {
            n += count_boxes(b->children);
        }
        b = b->next;
    }
    return n;
}

/* ===================================================================== 主流程 */

static const char *DEFAULT_URL = "https://example.com/";

static int vimtu_main(int argc, char **argv, char **envp)
{
    const char *start_url;
    nsurl *url = NULL;
    nserror err;
    struct browser_window *bw = NULL;
    bitmap_fmt_t bitmap_fmt;
    struct Wl64SeatEvent events[V64_WL_MAX_EVENTS];
    struct hlcache_handle *last_content = NULL;
    bool logged_render = false;
    unsigned frames_idle = 0;

    (void)envp;

    start_url = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') ? argv[1] : DEFAULT_URL;

    /* ---- ① GUI 表：netsurf_init 之前必须先注册，否则核心一进 netsurf_init 就会
     *        因为 guit == NULL 解引用（options/hlcache 都从这里取表）---- */
    err = vimtu_table_register();
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal netsurf_register err=%d\n", (int)err);
        return 1;
    }

    /* ---- ② 选项：不读 Choices 文件（本系统没有配置文件），只用编译期默认值 ---- */
    err = nsoption_init(NULL, &nsoptions, &nsoptions_default);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal nsoption_init err=%d\n", (int)err);
        return 1;
    }
    nsoption_set_bool(core_select_menu, true);      /* 没有原生下拉菜单，用核心内建的 */

    /* ---- ③ 位图格式：先声明，再让任何图像解码器开工 ---- */
    bitmap_fmt.layout = BITMAP_LAYOUT_B8G8R8A8;
    bitmap_fmt.pma = false;
    bitmap_set_format(&bitmap_fmt);

    /* ---- ④ 核心初始化 ---- */
    err = netsurf_init(NULL);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal netsurf_init err=%d\n", (int)err);
        return 1;
    }
    vimtu_log("[BROWSER] init ok\n");

    /* ---- ⑤ 打开首页 ---- */
    err = nsurl_create(start_url, &url);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal nsurl_create url=%s err=%d\n", start_url, (int)err);
        netsurf_exit();
        return 1;
    }
    err = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
    nsurl_unref(url);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal browser_window_create err=%d\n", (int)err);
        netsurf_exit();
        return 1;
    }

    g_app.bw = bw;
    vimtu_log("[BROWSER] open url=%s\n", start_url);
    g_app.need_paint = true;

    /* ---- ⑥ 事件循环 ---- */
    while (!g_app.quit) {
        int timeout;
        int got;

        timeout = vimtu_schedule_run();
        if (g_app.need_paint) {
            timeout = 0;                       /* 有脏区：不等输入，先画 */
        }
        if (timeout < 0) {
            timeout = 200;                     /* 没有定时器也要定期醒来看看 */
        }
        if (timeout > 1000) {
            timeout = 1000;
        }

        /* wl_dispatch 同时做两件事：把已 commit 的帧合成上屏、取 seat 事件。
         * max = 0 时它不等（timeout 由内核按 tick 处理），这里给满 32 条一批。 */
        got = v64_wl_dispatch((unsigned)timeout, events, V64_WL_MAX_EVENTS);
        if (got > 0) {
            int i;
            for (i = 0; i < got && i < (int)V64_WL_MAX_EVENTS; i++) {
                handle_seat_event(&events[i]);
            }
        }

        /* 内容变了就补一条 boxes= 证据（每次换内容打一条，不刷屏） */
        if (g_app.bw != NULL) {
            struct hlcache_handle *h = browser_window_get_content(g_app.bw);
            if (h != NULL && h != last_content) {
                last_content = h;
                logged_render = true;
                /* html_get_box_tree() 会把 content 硬转成 html_content（上游里没有类型检查），
                 * 所以非 HTML 内容（图片/纯文本）必须先挡住，否则就是野指针遍历。 */
                if (content_get_type(h) == CONTENT_HTML) {
                    vimtu_log("[BROWSER] render boxes=%u\n", count_boxes(html_get_box_tree(h)));
                } else {
                    vimtu_log("[BROWSER] render boxes=0 type=%d\n", (int)content_get_type(h));
                }
                g_app.need_paint = true;
            } else if (h == NULL && last_content != NULL) {
                last_content = NULL;
            }
        }

        if (g_app.need_paint) {
            vimtu_window_paint();
            frames_idle = 0;
        } else {
            frames_idle++;
        }

        /* 空转太久也没内容：不做任何事，只是让循环继续（保留 quit 的响应性） */
        if (frames_idle > 0 && g_app.quit) {
            break;
        }
    }

    /* ---- ⑦ 收尾 ---- */
    if (g_app.bw != NULL) {
        browser_window_destroy(g_app.bw);
        g_app.bw = NULL;
    }
    vimtu_canvas_close(&g_app.canvas);
    g_app.canvas_ok = false;
    netsurf_exit();
    vimtu_log("[BROWSER] exit\n");
    (void)logged_render;
    return 0;
}
