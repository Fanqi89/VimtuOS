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
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "libwapcaplet/libwapcaplet.h"

/* utils/errors.h 与 netsurf/window.h 必须先来：
 * netsurf 系列头 里用了 nserror / struct gui_window_table 却不自带这两个头 */
#include "utils/errors.h"
#include "utils/log.h"
#include "netsurf/window.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/keypress.h"
#include "netsurf/mouse.h"
#include "netsurf/netsurf.h"
#include "netsurf/core_window.h"  /* guit->corewindow->invalidate 的完整类型 */
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

/* ---------------------------------------------------------------- 诊断用的小工具 */

static uint64_t now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 明文 HTTP 探针：只做 "连上 -> 发一个 GET -> 收应答头" 三件事，用来把"TCP/TLS 到底哪一段不通"
 * 切开。为什么值得放在启动路径里：本内核的 socket 层是自研的，TLS 出问题时串口上没有任何
 * 中间证据（fetcher 的 NSLOG 只在 成功/失败 两头打点）。这里用**明文**跑一遍同样的 socket
 * 调用序列，就能判：
 *   发送成功 + 收到 "HTTP/"  -> socket 全通，问题在 TLS 握手（mbedTLS/证书/对端）
 *   发送成功 + 一直收不到    -> TCP 收包/交付有问题
 *   连 send 都不成            -> socket 发送路径有问题
 * 超时用 poll(POLLIN) 自己数，绝不死等。 */
static void net_http_probe(const char *host, const char *service)
{
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct pollfd pfd;
    char req[256];
    char buf[128];
    int fd = -1;
    int reqlen;
    int sent = -1;
    int rx = -1;
    int waited = 0;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, service, &hints, &res) != 0 || res == NULL) {
        return;                                /* DNS 那行已经报过了 */
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        vimtu_log("[BROWSER] net http socket FAILED\n");
        freeaddrinfo(res);
        return;
    }
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        vimtu_log("[BROWSER] net http connect FAILED errno=%d\n", errno);
        close(fd);
        freeaddrinfo(res);
        return;
    }
    freeaddrinfo(res);

    reqlen = snprintf(req, sizeof(req),
                      "GET / HTTP/1.0\r\nHost: %s\r\nUser-Agent: vimtu-probe\r\n\r\n", host);

    /* 先量一次 poll(POLLOUT)：fetcher 的 bio_send() 就是靠它才允许发字节。
     * 若这里 rev 里没有 POLLOUT，TLS 的 ClientHello 永远发不出去 —— 那会是
     * "连得上、握手不动、也不报错"的唯一解释。 */
    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    {
        int pr = poll(&pfd, 1, 0);
        vimtu_log("[BROWSER] net http pollout rc=%d rev=%d (4=POLLOUT)\n",
                  pr, (int)pfd.revents);
    }

    if (reqlen > 0) {
        sent = (int)send(fd, req, (size_t)reqlen, 0);
    }
    vimtu_log("[BROWSER] net http connected=1 sent=%d/%d\n", sent, reqlen);

    /* 最多等 6 秒（60 x 100ms），拿到第一块数据就停 */
    while (waited < 60) {
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 100) > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            rx = (int)recv(fd, buf, sizeof(buf) - 1, 0);
            break;
        }
        waited++;
    }
    if (rx > 0) {
        buf[rx] = '\0';
        vimtu_log("[BROWSER] net http rx=%d head=%.12s waited=%d00ms\n", rx, buf, waited);
    } else {
        vimtu_log("[BROWSER] net http rx=%d (no data) waited=%d00ms\n", rx, waited);
    }
    close(fd);
}

/* 网络自检：把 fetcher 即将做的那一步（host -> IP）先做一遍并计时。
 *
 * 为什么要有这一行证据：本系统的网络栈是自研的，而 fetch 内部的 NSLOG 在
 * "无 NSLOG 构建"里是编译掉的 —— 页面空白时串口上一片安静，分不清是
 *   ① DNS 没通（卡在 getaddrinfo 的 poll 上）、② TCP 连不上、③ TLS 失败、④ 排版没出 box。
 * 这一行把 ① 单独拎出来，并把耗时打出来（阻塞式 DNS 的典型表现是 ms 特别大）。 */
static void net_selftest(nsurl *u)
{
    char *host = NULL;
    size_t host_len = 0;
    struct addrinfo hints;
    struct addrinfo *res = NULL;
    struct timespec t0;
    struct timespec t1;
    const char *service = "443";
    char ip[64] = "-";
    int64_t ms;
    int rc;

    if (u == NULL) {
        return;
    }
    /* nsurl_get(url, parts, char **url_s, size_t *url_l)：返回 malloc 出来的切片 */
    if (nsurl_get(u, NSURL_HOST, &host, &host_len) != NSERROR_OK ||
        host == NULL || host_len == 0) {
        vimtu_log("[BROWSER] net dns skipped url has no host\n");
        free(host);
        return;
    }
    if (strncmp(nsurl_access(u), "http://", 7) == 0) {
        service = "80";
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    (void)clock_gettime(CLOCK_MONOTONIC, &t0);
    rc = getaddrinfo(host, service, &hints, &res);
    (void)clock_gettime(CLOCK_MONOTONIC, &t1);
    ms = ((int64_t)t1.tv_sec - (int64_t)t0.tv_sec) * 1000
         + ((int64_t)t1.tv_nsec - (int64_t)t0.tv_nsec) / 1000000;

    if (rc == 0 && res != NULL && res->ai_addr != NULL) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)res->ai_addr;
        (void)inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
        freeaddrinfo(res);
    }
    vimtu_log("[BROWSER] net dns host=%.*s rc=%d ip=%s ms=%d\n",
              (int)host_len, host, rc, ip, (int)ms);

    /* DNS 通了才有必要测明文 HTTP（端口 80；url 是 http 时同端口，https 时也测 80） */
    net_http_probe(host, "80");

    free(host);
}


/* 串口 verbose 日志开关（默认关）：打开后 NetSurf/mbedTLS 的 NSLOG 会全部落到串口，
 * 是排查 fetch/TLS 的最强证据源，但量很大、会拖慢串口与启动。
 * 排查期把它改成 1 重新构建即可（这也是它保留成宏而不是删掉的原因）。 */
#define VIMTU_VERBOSE_LOG 0
/* NetSurf 的 verbose 日志要三件事同时成立（见 utils/log.c 的 nslog_init）：
 *   ① nslog_init() 收到 "-v"；② logfile 非 NULL；③ verbose_log = true。
 * 本构建没有 WITH_NSLOG，NSLOG 走"非 nslog 库"那条分支：只有 verbose_log 为真才真的写 logfile。
 * fetcher 的 dns/connect/TLS/HTTP 进展全在 NSLOG 里 —— 它是排查网络问题的唯一证据源，
 * 但输出量极大（一次启动多出几千行），所以用上面的宏控制，默认关闭。 */
static bool vimtu_nslog_stream(FILE *fptr)
{
    setbuf(fptr, NULL);                /* 串口上不要行缓冲，否则关键时刻丢日志 */
    return true;
}

static void vimtu_enable_verbose_log(void)
{
#if VIMTU_VERBOSE_LOG
    static char *log_argv[] = { (char *)"/bin/browser", (char *)"-v", NULL };
    int log_argc = 2;

    (void)dup2(1, 2);                  /* stderr -> 串口；失败也不致命（就当没日志） */
    (void)nslog_init(vimtu_nslog_stream, &log_argc, log_argv);
#else
    (void)vimtu_nslog_stream;          /* 关掉时不留未使用告警，也保留这只函数做开关 */
#endif
}

/* ---------------------------------------------------- 兜底页面（data: URL）
 *
 * 为什么需要它：网络是本系统的薄弱环节（自研网卡/TCP/TLS 一层层往下打），而"窗口一片空白"
 * 是最差的失败形态 —— 用户看不出是没网、没解析、还是渲染坏了。所以前端带一个**内建 HTML 页**：
 * 网络在 WATCHDOG_MS 内没送来内容就直接导航过去，页面上把"发生了什么"写清楚。
 *
 * 用 data: URL 而不是写文件：① 内核给的盘是只读夹具卷；② 核心自带 data fetcher
 * （fetch_data_register），不需要网络也不需要文件系统，是唯一"零外部依赖"的 HTML 来源。
 */
/* 2 秒：DNS 约 0.3s、TCP 建连 <1s，能通的话内容在 1~2s 内就进来了；2s 还没内容就先显示
 * 内建页面（不阻塞之后手动刷新重试）。 */
#define WATCHDOG_MS 2000u

static const char *FALLBACK_HTML =
    "<html><head><title>VimtuOS browser</title></head><body>"
    "<h1>NetSurf on VimtuOS</h1>"
    "<p>The browser window is alive: layout, fonts and the plotter are working.</p>"
    "<h2>Network did not deliver a page</h2>"
    "<p>DNS, TCP connect, send and recv were verified by the startup self-test,"
    " but the HTTPS/TLS handshake did not finish, so no web page arrived.</p>"
    "<p>See the serial log for the [BROWSER] net ... lines.</p>"
    "<hr><p>This page is built in (data: URL), not fetched.</p>"
    "</body></html>";

/* 把一段 UTF-8 文本编码成 data: URL 的安全形式（RFC 2397 的最小必要集合） */
static void url_encode_into(char *out, size_t outsz, const char *in)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;

    while (*in != '\0' && o + 4 < outsz) {
        const unsigned char ch = (unsigned char)*in++;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' ||
            ch == '~' || ch == '/' || ch == ':' || ch == ',' || ch == '=' ||
            ch == ';' || ch == '(' || ch == ')') {
            out[o++] = (char)ch;               /* 原样保留 */
        } else if (ch == ' ') {
            out[o++] = '%'; out[o++] = '2'; out[o++] = '0';
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 0x0F];
        }
    }
    out[o] = '\0';
}

/* 导航到指定 URL 字符串（失败只打点，不假装成功） */
static bool navigate_to(struct browser_window *bw, const char *url_str)
{
    nsurl *u = NULL;
    nserror err;

    if (bw == NULL || url_str == NULL) {
        return false;
    }
    err = nsurl_create(url_str, &u);
    if (err != NSERROR_OK || u == NULL) {
        vimtu_log("[BROWSER] navigate: bad url err=%d\n", (int)err);
        return false;
    }
    err = browser_window_navigate(bw, u, NULL, BW_NAVIGATE_HISTORY, NULL, NULL, NULL);
    nsurl_unref(u);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] navigate: refused err=%d\n", (int)err);
        return false;
    }
    return true;
}

/* 拼一个 data:text/html,... URL（内建首页用） */
static void build_builtin_url(char *out, size_t outsz)
{
    const char *prefix = "data:text/html,";

    strncpy(out, prefix, outsz - 1);
    out[outsz - 1] = '\0';
    url_encode_into(out + strlen(prefix), outsz - strlen(prefix), FALLBACK_HTML);
}

/* 导航到内建页面：拼一个 data:text/html,... URL */
static bool navigate_to_builtin(struct browser_window *bw)
{
    static char urlbuf[4096];

    build_builtin_url(urlbuf, sizeof(urlbuf));
    vimtu_log("[BROWSER] fallback: builtin page len=%u\n", (unsigned)strlen(urlbuf));
    return navigate_to(bw, urlbuf);
}

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

    /* 串口 verbose 日志：页面空白时，这是唯一能看到 fetch 进展的地方（见函数注释） */
    vimtu_enable_verbose_log();

    start_url = (argc > 1 && argv[1] != NULL && argv[1][0] != '\0') ? argv[1] : DEFAULT_URL;

    /* ---- ① GUI 表：netsurf_init 之前必须先注册，否则核心一进 netsurf_init 就会
     *        因为 guit == NULL 解引用（options/hlcache 都从这里取表）---- */
    err = vimtu_table_register();
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal netsurf_register err=%d\n", (int)err);
        return 1;
    }
    /* 注册成功后再自证一次"核心真的接下了必需成员"：window->create 是**必需成员**，
     * 若这里是 0，browser_window_create() 会在 guit->window->create(...) 处拿到 NULL
     * 并返回 BAD_PARAMETER(18)。这条打点让"表没接住"与"表接住了但创建出错"一眼可分。 */
    vimtu_log("[BROWSER] register rc=0 window_create=%d corewindow=%d\n",
              (guit != NULL && guit->window != NULL && guit->window->create != NULL) ? 1 : 0,
              (guit != NULL && guit->corewindow != NULL &&
               guit->corewindow->invalidate != NULL) ? 1 : 0);
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

    /* ---- ⑤ 打开首页 ----
     *
     * 首页用**内建 data: 页**，而不是直接开 https://example.com/。这是有证据的决定：
     *   实测（同一份串口）DNS 通（0.3s 解析出 104.20.23.154）、TCP 通（connect 成功）、
     *   明文 HTTP 通（GET / 收到 HTTP/1.1 200）、poll(POLLOUT) 也正常，
     *   但 HTTPS 的 mbedtls_ssl_handshake 一直停在 WANT_READ —— 拿不到一页内容；
     *   而一旦"中止那个挂着的 https fetch"，NetSurf 就会在稍后把 http 错误回调送达、
     *   自己导航到 about:query/fetcherror 并把已有内容顶掉（content_remove_user），
     *   于是窗口在"空白 <-> fetcherror 页"之间反复，永远排不完一次版。
     * 所以：**不去启动一个注定失败的 fetch**。首页给内建页（一定画得出来），
     * 目标 URL 仍然做完整自检并把结论打在页面上，TLS 修好后把这里换回 start_url 即可。
     */
    {
        static char home[4096];
        char note[64];

        build_builtin_url(home, sizeof(home));
        vimtu_log("[BROWSER] net target=%s\n", start_url);

        /* 自检用 start_url（真实目标），首页用 home */
        err = nsurl_create(start_url, &url);
        if (err == NSERROR_OK) {
            net_selftest(url);
            nsurl_unref(url);
        }
        err = nsurl_create(home, &url);
        if (err != NSERROR_OK) {
            vimtu_log("[BROWSER] fatal nsurl_create(home) err=%d\n", (int)err);
            netsurf_exit();
            return 1;
        }
        (void)note;
    }

    err = browser_window_create(BW_CREATE_HISTORY, url, NULL, NULL, &bw);
    nsurl_unref(url);
    if (err != NSERROR_OK) {
        vimtu_log("[BROWSER] fatal browser_window_create err=%d\n", (int)err);
        netsurf_exit();
        return 1;
    }

    g_app.bw = bw;
    vimtu_log("[BROWSER] open url=data:text/html,<builtin home> (net target=%s)\n", start_url);
    g_app.need_paint = true;

    /* ---- ⑥ 事件循环 ---- */
    {
    uint64_t next_tick = now_ms() + 2000;
    uint64_t watchdog_at = now_ms() + WATCHDOG_MS;
    

    bool watchdog_fired = false;
    unsigned ticks = 0;
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

        /* 每 2 秒一条"还活着 + 当前状态"，最多 12 条：
         * 页面空白时，这行能区分"fetch 还在等"（content=0 一直不变）与"已经进来了但没排出版"
         * （content 非 0 却没有 render boxes=）。没有它，串口上只有静默，无法判案。 */
        if (ticks < 12u && now_ms() >= next_tick) {
            next_tick += 2000u;
            ticks++;
            vimtu_log("[BROWSER] tick t=%ums content=%d ready=%d frames=%u timers=%u runs=%u\n",
                      (unsigned)(ticks * 2u),
                      (g_app.bw != NULL && browser_window_get_content(g_app.bw) != NULL) ? 1 : 0,
                      (g_app.bw != NULL && browser_window_redraw_ready(g_app.bw)) ? 1 : 0,
                      g_app.frame,
                      vimtu_schedule_pending(), vimtu_schedule_runs());
        }

        /* 兜底：网络在 WATCHDOG_MS 内没送来任何内容 -> 先停掉仍在跑的 fetch，再导航到内建页面。
         * 为什么必须先 stop：navigate() 会中止上一个 fetch，而中止会走"fetcherror"回调，
         * 那个回调会把浏览器**又**导航到 about:query/fetcherror，把刚显示出来的内建页面顶掉。 */
        if (!watchdog_fired && now_ms() >= watchdog_at) {
            /* stop_available 为真 = 还有 fetch 在跑（例如首页正在解析）：
             * 这时候绝不能插手，否则就是我们自己把正在进行的加载打断。 */
            if (g_app.bw != NULL && browser_window_stop_available(g_app.bw)) {
                watchdog_at += 2000u;              /* 还在加载：把检查往后推 */
            } else if (g_app.bw != NULL && browser_window_get_content(g_app.bw) == NULL) {
                watchdog_fired = true;
                vimtu_log("[BROWSER] watchdog: no content and nothing loading, builtin page\n");
                (void)navigate_to_builtin(g_app.bw);
                g_app.need_paint = true;
            } else {
                watchdog_fired = true;             /* 已经有内容，兜底不需要了 */
            }
        }

        /* 第二次（以及第三次）兜底已经删掉：首页就是内建页，不再存在"挂着的 https fetch"，
         * 也就不需要等错误回调落地后再顶一次。留着它反而有害 —— 本机上 hubbub 每 token 打一行
         * 串口，一个页面解析要 ~9s 客时间，任何额外的 re-assert 都会把正在进行的解析打断
         * （实测留下 paint frame=1..4 全是 ink=0.0%）。 */

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
