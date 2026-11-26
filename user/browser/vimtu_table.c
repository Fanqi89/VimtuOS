/* user/browser/vimtu_table.c - 把 NetSurf 的 netsurf_table 填起来（窗口/调度/杂项）
 *
 * 为什么这个文件是"承重墙"：gui_factory.c 的 verify_*_register() 会逐条检查"必需成员"，
 * 少一个 netsurf_register() 就返回 BAD_PARAMETER，浏览器连 netsurf_init() 都进不去。
 * 必需的是：
 *   misc.schedule                     <- 整台发动机：fetch / hlcache / 重排全靠它驱动
 *   window.{create,destroy,invalidate,get_scroll,set_scroll,get_dimensions,event}
 *   corewindow.{invalidate,set_extent,set_scroll,get_scroll,get_dimensions,drag_status}
 *   fetch.filetype                    <- 给 file:// 路径猜 MIME
 *   bitmap.*（见 vimtu_bitmap.c）     layout.*（见 vimtu_layout.c）
 * 其余（file / llcache / download / clipboard / utf8 / search / search_web）留 NULL，
 * gui_factory 会填上它自己的默认实现：
 *   * file  -> default_file_table（POSIX 语义，正是我们要的）
 *   * llcache-> null_llcache_table（**不落盘**：本系统没有可写的持久缓存目录，
 *              走 fs_backing_store 会一路 mkdir 失败；内存缓存照样工作）
 *
 * ★ 调度为什么必须自己写、而且用 CLOCK_MONOTONIC：
 *   NetSurf 的核心是"事件驱动 + 定时器"结构（fetch.c 每 10ms 让 fetcher_poll 跑一轮、
 *   hlcache 定期清理、重排请求延后执行）。这些全部只通过 guit->misc->schedule 表达。
 *   所以"浏览器卡住不刷新"的头号原因就是这张表填错/没跑。
 *   PIT 时钟在 VimtuOS 里是 250Hz（4ms 粒度），CLOCK_MONOTONIC 由内核 228 号系统调用给出，
 *   和 QEMU 的墙钟一致 —— 这就是"该什么时候跑回调"的唯一依据。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* utils/errors.h 必须先来：netsurf 系列头 里用了 nserror 却不自带这个头 */
#include "utils/errors.h"
#include "netsurf/bitmap.h"
#include "netsurf/browser_window.h"
#include "netsurf/core_window.h"
#include "netsurf/fetch.h"
#include "netsurf/layout.h"
#include "netsurf/misc.h"
#include "netsurf/netsurf.h"
#include "netsurf/plotters.h"
#include "netsurf/types.h"
#include "netsurf/utf8.h"
#include "netsurf/window.h"
#include "utils/errors.h"
#include "utils/nsurl.h"

#include "desktop/browser_history.h"   /* browser_window_history_back/forward */

#include "vimtu_abi.h"
#include "vimtu_app.h"
#include "vimtu_bitmap.h"
#include "vimtu_canvas.h"
#include "vimtu_font.h"

/* 全局笔（vimtu_plot.c） */
extern const struct plotter_table plot;

/* 位图/布局表由各自文件提供 */
struct gui_bitmap_table *vimtu_get_bitmap_table(void);
struct gui_layout_table *vimtu_get_layout_table(void);

struct vimtu_app g_app;

/* ==================================================================== 调度器 */

struct vimtu_timer {
    struct vimtu_timer *next;
    uint64_t due_ms;                 /* CLOCK_MONOTONIC 毫秒 */
    void (*callback)(void *p);
    void *p;
};

static struct vimtu_timer *g_timers;
static unsigned g_sched_runs;           /* 累计执行过的回调数（诊断用） */

/* 诊断出口：还有多少定时器挂着 / 已经跑过多少回调。
 * 为什么要暴露这两个数：fetch 的推进完全依赖 "guit->misc->schedule -> fetcher_poll" 这条链。
 * 页面空白时，"timers=0 runs=0" 说明链子没接上；"runs 暴涨" 说明链子在转、卡的是 fetcher 内部。 */
unsigned vimtu_schedule_pending(void)
{
    unsigned n = 0;
    struct vimtu_timer *t;

    for (t = g_timers; t != NULL; t = t->next) {
        n++;
    }
    return n;
}

unsigned vimtu_schedule_runs(void)
{
    return g_sched_runs;
}

static uint64_t mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

/* 摘掉 (callback,p) 对应的定时器；返回是否摘掉了 */
static bool timer_remove(void (*callback)(void *p), void *p)
{
    struct vimtu_timer **pp = &g_timers;

    while (*pp != NULL) {
        struct vimtu_timer *t = *pp;
        if (t->callback == callback && t->p == p) {
            *pp = t->next;
            free(t);
            return true;
        }
        pp = &t->next;
    }
    return false;
}

static nserror vimtu_misc_schedule(int t, void (*callback)(void *p), void *p)
{
    struct vimtu_timer *nt;

    if (callback == NULL) {
        return NSERROR_BAD_PARAMETER;
    }

    /* 同 (callback,p) 只允许一份挂着的：先摘再挂（NetSurf 的契约：
     * "Additional calls with the same callback and user parameter will reset
     *  the callback time"） */
    (void)timer_remove(callback, p);

    if (t < 0) {
        return NSERROR_OK;               /* 负值 = 取消 */
    }

    nt = (struct vimtu_timer *)calloc(1, sizeof(*nt));
    if (nt == NULL) {
        return NSERROR_NOMEM;
    }
    nt->callback = callback;
    nt->p = p;
    nt->due_ms = mono_ms() + (uint64_t)t;

    /* 无序插入即可：run 的时候扫一遍并且只在**快照**上执行，
     * 这样回调里再 schedule/取消 都不会踩到正在遍历的链表 */
    nt->next = g_timers;
    g_timers = nt;
    return NSERROR_OK;
}

/* 跑一轮到期回调，返回"离下一个到期还有多少毫秒"（没有定时器 -> -1，用于阻塞等待） */
int vimtu_schedule_run(void)
{
    uint64_t now = mono_ms();
    struct vimtu_timer *run = NULL;
    struct vimtu_timer *next;

    /* ① 把到期的全部摘到一个临时链表里（摘的时候已经从 g_timers 移除，
     *    回调里再 schedule 同一个 (cb,p) 不会和自己打架） */
    {
        struct vimtu_timer **pp = &g_timers;
        while (*pp != NULL) {
            struct vimtu_timer *t = *pp;
            if (t->due_ms <= now) {
                *pp = t->next;
                t->next = run;
                run = t;
            } else {
                pp = &t->next;
            }
        }
    }

    /* ② 逐个执行（执行期间链表已经独立） */
    while (run != NULL) {
        next = run->next;
        run->callback(run->p);
        g_sched_runs++;
        free(run);
        run = next;
    }

    /* ③ 算下一次到期 */
    if (g_timers == NULL) {
        return -1;
    }
    now = mono_ms();
    {
        uint64_t soonest = (uint64_t)-1;
        struct vimtu_timer *t;
        for (t = g_timers; t != NULL; t = t->next) {
            if (t->due_ms < soonest) {
                soonest = t->due_ms;
            }
        }
        if (soonest <= now) {
            return 0;
        }
        if (soonest - now > 1000u) {
            return 1000;                 /* 别睡太久：输入/脏区还要照看 */
        }
        return (int)(soonest - now);
    }
}

static void vimtu_misc_quit(void)
{
    g_app.quit = true;
}

static struct gui_misc_table vimtu_misc_table = {
    .schedule = vimtu_misc_schedule,
    .quit = vimtu_misc_quit,
};

/* ==================================================================== 窗口 */

struct gui_window {
    struct gui_window *next;             /* 链表（g_windows） */
    struct browser_window *bw;
    struct vimtu_canvas *canvas;         /* NULL = 附加窗口（本前端只有一块 surface） */
    bool primary;

    int scrollx;
    int scrolly;
    int extent_w;
    int extent_h;

    int caret_x;
    int caret_y;
    int caret_h;
    bool caret;

    int drag_status;
};

static struct gui_window *g_windows;     /* 全部 gui_window（释放/退出时清理） */

void vimtu_window_mark_dirty(void)
{
    g_app.need_paint = true;
}

/* 刷新三个按钮的真值（每个可能改变历史的动作之后都要调） */
static void refresh_chrome_state(void)
{
    struct browser_window *bw = g_app.bw;

    if (bw == NULL) {
        g_app.can_back = false;
        g_app.can_forward = false;
        g_app.can_reload = false;
        return;
    }
    g_app.can_back = browser_window_back_available(bw);
    g_app.can_forward = browser_window_forward_available(bw);
    g_app.can_reload = browser_window_reload_available(bw);
}

static struct gui_window *vimtu_window_create(struct browser_window *bw,
                                             struct gui_window *existing,
                                             gui_window_create_flags flags)
{
    struct gui_window *gw;
    int rc;

    (void)flags;

    gw = (struct gui_window *)calloc(1, sizeof(*gw));
    if (gw == NULL) {
        return NULL;
    }
    gw->bw = bw;
    gw->extent_w = VIMTU_PAGE_W;
    gw->extent_h = VIMTU_PAGE_H;
    gw->drag_status = CORE_WINDOW_DRAG_NONE;
    if (existing != NULL) {
        gw->scrollx = existing->scrollx;
        gw->scrolly = existing->scrolly;
    }
    gw->next = NULL;                     /* 见下面的链表挂接 */

    if (!g_app.canvas_ok) {
        /* 画布在这里建（而不是 main 里）：窗口尺寸/装饰都是"窗口"的属性，
         * 而且核心只有在 create 回调里才保证 gui_window 是活的。 */
        rc = vimtu_canvas_open(&g_app.canvas, VIMTU_CANVAS_W, VIMTU_CANVAS_H);
        if (rc != 0) {
            vimtu_log("[BROWSER] fatal canvas open failed rc=%d\n", rc);
            free(gw);
            return NULL;
        }
        g_app.canvas_ok = true;

        /* 声明成"窗口"：标题栏/圆角/三按钮由合成器画，客户区从第 25 行开始 */
        (void)v64_wl_set_decor(g_app.canvas.surf, V64_WL_DECOR_WINDOW, "Vimtu 浏览器");
        (void)v64_wl_set_app(g_app.canvas.surf, "browser");

        gw->canvas = &g_app.canvas;
        gw->primary = true;
        g_app.win = gw;
        g_app.bw = bw;
    } else {
        /* 第二扇窗口（target=_blank 之类）：没有第二块 surface 可用（像素预算只够一块），
         * 于是只做逻辑窗口，不绘制。这是**故意的**降级，打 UNVERIFIED 让验收看得见。 */
        gw->canvas = NULL;
        gw->primary = false;
        vimtu_log("[BROWSER] window create secondary detached UNVERIFIED\n");
    }

    gw->next = (struct gui_window *)g_windows;
    g_windows = gw;
    return gw;
}

/* ---- 窗口销毁 ---- */
static void vimtu_window_destroy(struct gui_window *gw)
{
    struct gui_window **pp = &g_windows;

    if (gw == NULL) {
        return;
    }
    while (*pp != NULL && *pp != gw) {
        pp = &(*pp)->next;
    }
    if (*pp != NULL) {
        *pp = gw->next;
    }
    if (g_app.win == gw) {
        g_app.win = NULL;
        g_app.bw = NULL;
    }
    free(gw);
}

static nserror vimtu_window_invalidate(struct gui_window *gw, const struct rect *rect)
{
    (void)rect;
    /* 核心说"这块脏了"。真重绘放到事件循环里做：invalidate 可能在 redraw 过程中被调，
     * 这里立刻重画就是递归重绘（NetSurf 文档也明确要求不要在这条回调里开始画）。 */
    if (gw != NULL && gw->primary) {
        g_app.need_paint = true;
    }
    return NSERROR_OK;
}

static bool vimtu_window_get_scroll(struct gui_window *gw, int *sx, int *sy)
{
    if (gw == NULL || sx == NULL || sy == NULL) {
        return false;
    }
    *sx = gw->scrollx;
    *sy = gw->scrolly;
    return true;
}

static nserror vimtu_window_set_scroll(struct gui_window *gw, const struct rect *rect)
{
    int maxx;
    int maxy;

    if (gw == NULL || rect == NULL) {
        return NSERROR_OK;
    }
    /* NetSurf 的语义："保证 rect 可见"；最简单的合法实现是把指定点滚到左上角。
     * 再按内容范围夹一下，避免滚到页面外的空白里。 */
    maxx = gw->extent_w - VIMTU_PAGE_W;
    maxy = gw->extent_h - VIMTU_PAGE_H;
    if (maxx < 0) {
        maxx = 0;
    }
    if (maxy < 0) {
        maxy = 0;
    }

    gw->scrollx = rect->x0;
    gw->scrolly = rect->y0;
    if (gw->scrollx < 0) {
        gw->scrollx = 0;
    }
    if (gw->scrollx > maxx) {
        gw->scrollx = maxx;
    }
    if (gw->scrolly < 0) {
        gw->scrolly = 0;
    }
    if (gw->scrolly > maxy) {
        gw->scrolly = maxy;
    }

    g_app.need_paint = true;
    return NSERROR_OK;
}

static nserror vimtu_window_get_dimensions(struct gui_window *gw, int *width, int *height)
{
    (void)gw;
    if (width == NULL || height == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    /* 视口 = 页面区（Chrome 和合成器标题栏已经让开了） */
    *width = VIMTU_PAGE_W;
    *height = VIMTU_PAGE_H;
    return NSERROR_OK;
}

static nserror vimtu_window_event(struct gui_window *gw, enum gui_window_event event)
{
    switch (event) {
    case GW_EVENT_UPDATE_EXTENT:
        if (gw != NULL && gw->bw != NULL) {
            (void)browser_window_get_extents(gw->bw, true, &gw->extent_w, &gw->extent_h);
        }
        g_app.need_paint = true;
        break;

    case GW_EVENT_REMOVE_CARET:
        if (gw != NULL) {
            gw->caret = false;
        }
        g_app.need_paint = true;
        break;

    case GW_EVENT_START_THROBBER:
        g_app.throbber = true;
        refresh_chrome_state();
        g_app.need_paint = true;
        break;

    case GW_EVENT_STOP_THROBBER:
        g_app.throbber = false;
        refresh_chrome_state();
        g_app.need_paint = true;
        break;

    default:
        break;
    }
    return NSERROR_OK;
}

static void vimtu_window_set_title(struct gui_window *gw, const char *title)
{
    (void)gw;
    if (title == NULL) {
        return;
    }
    strncpy(g_app.title, title, sizeof(g_app.title) - 1);
    g_app.title[sizeof(g_app.title) - 1] = '\0';

    /* 标题跟着页面标题走：合成器画标题栏（内核打点只打变化），这里顺手同步 */
    if (g_app.canvas_ok) {
        (void)v64_wl_set_decor(g_app.canvas.surf, V64_WL_DECOR_WINDOW, g_app.title);
    }
}

static nserror vimtu_window_set_url(struct gui_window *gw, struct nsurl *url)
{
    const char *s;

    (void)gw;
    if (url == NULL) {
        return NSERROR_OK;
    }
    s = nsurl_access(url);
    if (s == NULL) {
        return NSERROR_OK;
    }
    strncpy(g_app.url, s, sizeof(g_app.url) - 1);
    g_app.url[sizeof(g_app.url) - 1] = '\0';

    refresh_chrome_state();
    g_app.need_paint = true;
    return NSERROR_OK;
}

static void vimtu_window_set_status(struct gui_window *gw, const char *text)
{
    (void)gw;
    if (text == NULL) {
        g_app.status[0] = '\0';
        return;
    }
    strncpy(g_app.status, text, sizeof(g_app.status) - 1);
    g_app.status[sizeof(g_app.status) - 1] = '\0';
}

static void vimtu_window_set_pointer(struct gui_window *gw, enum gui_pointer_shape shape)
{
    /* 指针形状由内核/合成器管（本 ABI 没有"设置光标形状"的号段） */
    (void)gw;
    (void)shape;
}

static void vimtu_window_place_caret(struct gui_window *gw, int x, int y, int height,
                                     const struct rect *clip)
{
    (void)clip;
    if (gw == NULL) {
        return;
    }
    gw->caret_x = x;
    gw->caret_y = y;
    gw->caret_h = height;
    gw->caret = true;
    g_app.need_paint = true;
}

static struct gui_window_table vimtu_window_table = {
    .create = vimtu_window_create,
    .destroy = vimtu_window_destroy,
    .invalidate = vimtu_window_invalidate,
    .get_scroll = vimtu_window_get_scroll,
    .set_scroll = vimtu_window_set_scroll,
    .get_dimensions = vimtu_window_get_dimensions,
    .event = vimtu_window_event,

    /* 可选成员：给了就有窗口标题/地址栏/状态栏，没给核心会用空实现 */
    .set_title = vimtu_window_set_title,
    .set_url = vimtu_window_set_url,
    .set_status = vimtu_window_set_status,
    .set_pointer = vimtu_window_set_pointer,
    .place_caret = vimtu_window_place_caret,
};

/* ============================================================ core window 表
 *
 * 本前端的 cookie 管理器/全局历史/本地历史这些 core window 界面都没做，核心只用它
 * （如果将来做的话）来画/滚动。所以这里给一份"能回答尺寸与滚动、但永不 invalidate"
 * 的真实实现：至少 core 内部那些 helper（cw_helper_scroll_visible）不会踩空。
 */
static nserror vimtu_corewindow_invalidate(struct core_window *cw, const struct rect *rect)
{
    (void)cw;
    (void)rect;
    return NSERROR_OK;
}

static nserror vimtu_corewindow_set_extent(struct core_window *cw, int width, int height)
{
    (void)cw;
    (void)width;
    (void)height;
    return NSERROR_OK;
}

static nserror vimtu_corewindow_set_scroll(struct core_window *cw, int x, int y)
{
    (void)cw;
    (void)x;
    (void)y;
    return NSERROR_OK;
}

static nserror vimtu_corewindow_get_scroll(const struct core_window *cw, int *x, int *y)
{
    (void)cw;
    if (x != NULL) {
        *x = 0;
    }
    if (y != NULL) {
        *y = 0;
    }
    return NSERROR_OK;
}

static nserror vimtu_corewindow_get_dimensions(const struct core_window *cw, int *width, int *height)
{
    (void)cw;
    if (width != NULL) {
        *width = VIMTU_PAGE_W;
    }
    if (height != NULL) {
        *height = VIMTU_PAGE_H;
    }
    return NSERROR_OK;
}

static nserror vimtu_corewindow_drag_status(struct core_window *cw, core_window_drag_status ds)
{
    (void)cw;
    (void)ds;
    return NSERROR_OK;
}

static struct core_window_table vimtu_corewindow_table = {
    .invalidate = vimtu_corewindow_invalidate,
    .set_extent = vimtu_corewindow_set_extent,
    .set_scroll = vimtu_corewindow_set_scroll,
    .get_scroll = vimtu_corewindow_get_scroll,
    .get_dimensions = vimtu_corewindow_get_dimensions,
    .drag_status = vimtu_corewindow_drag_status,
};

/* ================================================================ fetch 表 */

/* file:// 的 MIME 猜测。核心只在 file: 协议和表单文件控件里用它，
 * 但 filetype 是**必需成员**（verify_fetch_register 硬性检查）。 */
static const char *vimtu_fetch_filetype(const char *unix_path)
{
    static char mime[64];
    const char *dot;

    if (unix_path == NULL) {
        return "application/octet-stream";
    }
    dot = strrchr(unix_path, '.');
    if (dot == NULL) {
        return "text/plain";
    }

    if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
        return "text/html";
    }
    if (strcasecmp(dot, ".css") == 0) {
        return "text/css";
    }
    if (strcasecmp(dot, ".txt") == 0) {
        return "text/plain";
    }
    if (strcasecmp(dot, ".js") == 0) {
        return "text/javascript";
    }
    if (strcasecmp(dot, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(dot, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(dot, ".bmp") == 0) {
        return "image/bmp";
    }
    if (strcasecmp(dot, ".ico") == 0) {
        return "image/vnd.microsoft.icon";
    }
    if (strcasecmp(dot, ".svg") == 0) {
        return "image/svg+xml";
    }
    if (strcasecmp(dot, ".webp") == 0) {
        return "image/webp";
    }

    /* 未知扩展名：把扩展名本身当一个私有 MIME 返回（比 "text/plain" 更诚实：
     * 核心会按未知类型处理，不会把二进制当文本渲染成乱码页） */
    strncpy(mime, "application/x.", sizeof(mime) - 1);
    mime[sizeof(mime) - 1] = '\0';
    strncat(mime, dot + 1, sizeof(mime) - strlen(mime) - 1);
    return mime;
}

/* 内建资源（resource: 协议）：NetSurf 每载入一页都要读四张内建样式表
 *   resource:default.css / quirks.css / internal.css / user.css
 * 前端必须通过 **get_resource_data 交出这些字节**。本前端此前留 NULL（=核心默认
 * "NOT_FOUND"），实测后果非常隐蔽：每次页面加载都报
 *   html_convert_css_callback: stylesheet resource:default.css failed: UnacceptableType
 * 四张全失败之后 html_finish_conversion 走错误路径 → 浏览器被导航到
 *   about:query/fetcherror → 那张页又触发同样的失败 → 自持循环，窗口永远排不完一次版
 * （串口表现是 "Completing parse" 每秒一条、地址各不相同，paint 一直 ink=0.0%）。
 *
 * 现在给出**合法的空样式表**：空 CSS 完全合法，布局照常进行（只是没有默认字体着色/边距
 * 微调）。这比"没有样式表就报错"诚实，也是 TLS 修好之前让页面能显示的最小一步。
 * 后续要把真正的 default.css 打包进来，只需要把这里的字符串换成真实内容即可。 */
static nserror vimtu_fetch_get_resource_data(const char *path, const uint8_t **data, size_t *data_len)
{
    static const char empty_css[] = "/* vimtu: built-in stylesheet placeholder */\n";

    if (path == NULL || data == NULL || data_len == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    if (strstr(path, ".css") != NULL) {
        *data = (const uint8_t *)empty_css;
        *data_len = sizeof(empty_css) - 1;
        return NSERROR_OK;
    }
    return NSERROR_NOT_FOUND;
}

static nserror vimtu_fetch_release_resource_data(const uint8_t *data)
{
    (void)data;
    return NSERROR_OK;               /* 指向静态字符串，不需要释放 */
}

static struct gui_fetch_table vimtu_fetch_table = {
    .filetype = vimtu_fetch_filetype,
    .get_resource_data = vimtu_fetch_get_resource_data,
    .release_resource_data = vimtu_fetch_release_resource_data,
};

/* ================================================================= utf8 表
 *
 * 本系统的"本地编码"就是 UTF-8（页面、文件名、标题栏都是），所以转换 = 拷贝。
 * 两个成员都是**必需**的（verify_utf8_register 硬性检查）。
 */
static nserror vimtu_utf8_to_local(const char *string, size_t len, char **result)
{
    if (string == NULL || result == NULL) {
        return NSERROR_BAD_PARAMETER;
    }
    if (len == 0) {
        len = strlen(string);
    }
    *result = strndup(string, len);
    return (*result != NULL) ? NSERROR_OK : NSERROR_NOMEM;
}

static struct gui_utf8_table vimtu_utf8_table = {
    .utf8_to_local = vimtu_utf8_to_local,
    .local_to_utf8 = vimtu_utf8_to_local,
};

/* ============================================================ 总表 + 注册 */

nserror vimtu_table_register(void)
{
    static struct netsurf_table table;

    memset(&table, 0, sizeof(table));
    table.misc = &vimtu_misc_table;
    table.window = &vimtu_window_table;
    table.corewindow = &vimtu_corewindow_table;
    table.bitmap = vimtu_get_bitmap_table();
    table.layout = vimtu_get_layout_table();
    table.fetch = &vimtu_fetch_table;
    table.utf8 = &vimtu_utf8_table;

    /* clipboard / search / search_web / download / file / llcache 留 NULL：
     * gui_factory.c 会给它们填默认实现（剪贴板=空、搜索=空、file=POSIX、
     * llcache=不落盘的 null 表）。本前端没有可视的剪贴板/搜索界面，
     * 自行实现一遍只会多一份和核心语义漂移的代码。 */

    return netsurf_register(&table);
}

/* =============================================================== 绘制一帧 */

void vimtu_window_paint(void)
{
    struct gui_window *gw = g_app.win;
    struct vimtu_canvas *c = &g_app.canvas;
    struct rect clip;
    struct redraw_context ctx;
    unsigned ink;
    unsigned total;
    unsigned permille;

    if (!g_app.canvas_ok || gw == NULL || c->px == NULL) {
        g_app.need_paint = false;
        return;
    }

    /* ---- ① 页面区：先刷白底，再把文档画进去 ---- */
    vimtu_canvas_clip(c, 0, VIMTU_PAGE_Y, VIMTU_PAGE_W, VIMTU_PAGE_Y + VIMTU_PAGE_H);
    vimtu_canvas_fill(c, 0, VIMTU_PAGE_Y, VIMTU_PAGE_W, VIMTU_PAGE_H, 0xFFFFFFu);

    clip.x0 = 0;
    clip.y0 = VIMTU_PAGE_Y;
    clip.x1 = VIMTU_PAGE_W;
    clip.y1 = VIMTU_PAGE_Y + VIMTU_PAGE_H;

    ctx.interactive = true;
    ctx.background_images = true;
    ctx.plot = &plot;
    ctx.priv = c;

    if (g_app.bw != NULL) {
        (void)browser_window_redraw(g_app.bw,
                                   -gw->scrollx,                    /* 文档原点的画布坐标 */
                                   VIMTU_PAGE_Y - gw->scrolly,
                                   &clip, &ctx);
    }

    /* ---- ② 光标（文本框里的竖线）---- */
    if (gw->caret) {
        int cx = gw->caret_x - gw->scrollx;
        int cy = VIMTU_PAGE_Y + gw->caret_y - gw->scrolly;
        vimtu_canvas_clip(c, 0, VIMTU_PAGE_Y, VIMTU_PAGE_W, VIMTU_PAGE_Y + VIMTU_PAGE_H);
        vimtu_canvas_fill(c, cx, cy, 1, gw->caret_h, 0x000000u);
    }

    /* ---- ③ Chrome 最后画（永远压在页面之上，不会被页面内容盖住）---- */
    vimtu_shell_draw(&g_app);

    /* ---- ④ 证据：这一帧到底有没有画出东西 ---- */
    g_app.frame++;
    total = (unsigned)VIMTU_PAGE_W * (unsigned)VIMTU_PAGE_H;
    ink = vimtu_canvas_count_ink(c, 0, VIMTU_PAGE_Y, VIMTU_PAGE_W, VIMTU_PAGE_H, 0xFFFFFFu);
    permille = (total != 0) ? (unsigned)(((uint64_t)ink * 1000u) / total) : 0u;

    /* 前 10 帧逐帧打，之后每 30 帧一条（串口是 115200，别把证据刷成噪音） */
    if (g_app.frame <= 10 || (g_app.frame % 30u) == 0) {
        vimtu_log("[BROWSER] paint frame=%u ink=%u.%u%%\n",
                  g_app.frame, permille / 10u, permille % 10u);
    }

    /* ---- ⑤ 上屏（damage + commit + dispatch(0)）---- */
    vimtu_canvas_flush(c, 0, VIMTU_CONTENT_Y, VIMTU_CANVAS_W, VIMTU_CANVAS_H - VIMTU_CONTENT_Y);

    g_app.need_paint = false;
}

/* ========================================================== 导航（给外壳用） */

void vimtu_window_back(void)
{
    if (g_app.bw == NULL) {
        return;
    }
    if (!browser_window_back_available(g_app.bw)) {
        return;                       /* 真值驱动：不能退就不动 */
    }
    (void)browser_window_history_back(g_app.bw, false);
    refresh_chrome_state();
    g_app.need_paint = true;
}

void vimtu_window_forward(void)
{
    if (g_app.bw == NULL) {
        return;
    }
    if (!browser_window_forward_available(g_app.bw)) {
        return;
    }
    (void)browser_window_history_forward(g_app.bw, false);
    refresh_chrome_state();
    g_app.need_paint = true;
}

void vimtu_window_reload(void)
{
    if (g_app.bw == NULL) {
        return;
    }
    (void)browser_window_reload(g_app.bw, true);
    g_app.throbber = true;
    refresh_chrome_state();
    g_app.need_paint = true;
}
