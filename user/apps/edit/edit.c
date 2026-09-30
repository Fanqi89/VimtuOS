/* edit.c - ★ A4-5：自研极简文本编辑器（Ring 3；/bin/edit；静态 ELF64，走自研 user/lib）
 *
 * 交付方式（与 /bin/shell.bin、/bin/gzip 同一纪律）：
 *   build64.sh -> user/apps/edit/build_edit.sh -> build64/edit（< 64 KiB 的静态 ELF64）
 *   -> tools/edit_pack_win.py 把它写进**系统卷**的 /bin/edit。**内核镜像里一个字节都没有**
 *   （build64.sh 里有一条"内核二进制里搜不到 edit 的字节"的断言）。
 *
 * 怎么跑：终端 shell 里 `run /bin/edit <文件>`（fork + execve 真进程），或启动期演示
 *   （kernel64.cpp 的 edit64 demo）以固定路径拉起它做无键盘的自动验收。
 *
 * ============================ 设计（为什么这么做，逐条）============================
 *   1) **按键从哪来**：本内核的 fd 0 是"空槽 = 虚拟标准流"，read(0,...) 立刻 EOF（没有键盘
 *      输入流、没有行规程）—— 所以按键走**自有 ABI 12 input_poll**（EV64_FLAG_FOCUS 申请键盘
 *      焦点、退出时 EV64_FLAG_RELEASE 释放）。这条路是内核既有的、被 A5 前置验收过的通道。
 *      输出走 fd 1（内核控制台：串口 + 屏幕），用 ANSI 转义做清屏/定位/反显。
 *      ★ 如实：屏幕上 kernel/syscall64.cpp 的控制台输出只在 (8,8) 画**一行**，它不解析 ANSI ——
 *        所以 ANSI 流的"终端证据"在**串口日志**里（验收脚本按 ANSI 转义序列断言），
 *        而不是屏幕像素。像素级渲染属于将来"用户态终端窗口"那一批。
 *   2) **raw 模式**：进入时 TCGETS 保存原 termios、TCSETS 写一份关掉 ICANON/ECHO 的 raw；
 *      退出时 TCSETS 还原 + 再 TCGETS 读回**打印证据**（内核侧 syscall64.cpp 的 ioctl 真存这份
 *      状态，所以"设了/还原了"是可核对的）。本内核没有真行规程（Ctrl+C/tty-int 由内核的
 *      终端路径按前台进程组投递），raw 标志对按键路径没有副作用 —— 如实写在这里。
 *   3) **大文件**：文本缓冲与行索引都用 mmap（9）拿：文本 2 MiB + 行索引 4B×65536 = 256 KiB。
 *      不引动态分配（user/lib 的堆只有 4 KiB）。1 MiB 文本（约 1.7 万行）能开、能上下移动。
 *      行数超过 65536 的部分折在最后一行里显示（如实上限）。
 *   4) **修改模型**：不做 vi 式模式切换 —— 可打印键直接在光标处插入（"直接输入即编辑"），
 *      Esc 进命令模式（`:w` 保存 / `:q` 退出 / `:q!` 强制退出）。Ctrl+S 保存、Ctrl+Q 退出。
 *   5) **Ctrl+C**：走新做的信号投递 —— raise(SIGINT)（= kill(getpid(),2)）-> 内核在系统调用
 *      出口投递 -> handler 里只置标志 -> 主循环**放弃当前改动**（重新从盘上读）、继续运行。
 *      终端窗口的 Ctrl+C（前台进程组 = 本进程，见 setpgid(0,0)）走同一条 handler，语义一致。
 *   6) **重绘**：只在行内容变化时重发那一行的 ANSI（串口 115200 波特，全屏重绘每键 ~1.5 KB
 *      会把验收拖成分钟级）—— 行缓存 g_rows[] 就是为此存在。证据行（[EDIT64] ...）走 **fd 2**
 *      （内核把 fd 2 只送串口），免得它们盖住屏幕上的 ANSI 画。
 *
 * 串口证据（tests/edit64_test.py grep，格式勿改）：
 *   [EDIT64] start file=<p> pid=<n> argc=<n>
 *   [EDIT64] termios raw ok=1 lflag=0x<hex> raw=0x<hex>
 *   [EDIT64] load file=<p> bytes=<n> lines=<n> ms=<n>
 *   [EDIT64] ready file=<p> bytes=<n> lines=<n>
 *   [EDIT64] key=0x<hex> line=<n> col=<n> dirty=<n>          （每个按键一行，有预算）
 *   [EDIT64] save path=<p> bytes=<n>                        （Ctrl+S / :w）
 *   [EDIT64] sigint discard dirty_before=<n> bytes=<n>       （Ctrl+C：放弃改动 -> 回到盘上内容）
 *   [EDIT64] cmd=<text>                                     （命令模式执行的命令）
 *   [EDIT64] termios restored lflag=0x<hex> ic=0x<hex> ok=<n>
 *   [EDIT64] exit code=<n> dirty=<n> bytes=<n>
 */
#include <stdint.h>
#include <stddef.h>

#include "vimtu64.h"
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

/* ==================== 常量 ==================== */
#define ED_TEXT_CAP   (2u * 1024u * 1024u)   /* 文本缓冲上限（mmap） */
#define ED_LINE_CAP   65536u                 /* 行索引上限（4B/项 = 256 KiB） */
#define ED_PATH_MAX   128
#define ED_ROWS       22                     /* 正文行数（1..22） */
#define ED_COLS       78                     /* 每行显示宽（80 列，留 2 列余量） */
#define ED_ROW_STATUS 23                     /* 状态行 */
#define ED_ROW_MSG    24                     /* 消息行 */
#define ED_CMD_MAX    32

/* ioctl 请求号（与 kernel/syscall64.cpp 的 lx64_tty_ioctl64 一致） */
#define ED_TCGETS 0x5401u
#define ED_TCSETS 0x5402u
#define ED_TIOCGWINSZ 0x5413u

/* 键码（与 kernel/input.h / user/lib/vimtu64.h 的键码口径一致） */
#define ED_KEY_ESC      0x1Bu
#define ED_KEY_UP       0xFDu
#define ED_KEY_DOWN     0xFEu
#define ED_KEY_LEFT     0xFBu
#define ED_KEY_RIGHT    0xFCu
#define ED_KEY_DELETE   0xFAu
#define ED_KEY_PAGEUP   0xF8u
#define ED_KEY_PAGEDOWN 0xF7u
#define ED_KEY_CTRL_S   0x13u
#define ED_KEY_CTRL_Q   0x11u
#define ED_KEY_CTRL_C   0x03u

/* termios（36 B；字段偏移与内核一致：iflag/oflag/cflag/lflag + c_line + c_cc[19]） */
struct EdTermios {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[19];
};
struct EdWinsize { uint16_t row, col, xpixel, ypixel; };

/* ==================== 状态 ==================== */
static char*     g_text;                 /* mmap：文本本体（NUL 结尾） */
static uint32_t* g_lidx;                 /* mmap：每行起始偏移（g_nlines + 1 项） */
static uint32_t  g_len;                  /* 文本字节数 */
static uint32_t  g_nlines;               /* 行数（>= 1） */
static int       g_cy, g_cx;             /* 光标：行号（0 基）/ 列号（0 基） */
static int       g_top;                  /* 屏幕第一行对应的行号（滚动） */
static int       g_dirty;                /* 有未保存改动 */
static int       g_quit;                 /* 退出请求 */
static int       g_quit_force;           /* :q! 强制退出 */
static volatile int g_sigint;            /* SIGINT handler 置位（Ctrl+C / 终端 tty-int） */
static char      g_path[ED_PATH_MAX];
static int       g_raw;                  /* 当前是不是 raw 模式 */
static int       g_cmdmode;              /* 命令模式（Esc 之后） */
static char      g_cmd[ED_CMD_MAX];
static int       g_cmdlen;
static int       g_keylog_budget = 24;   /* 按键证据行预算（防刷屏） */
static int       g_ro;                   /* 1 = 只读（打开失败/文件太大：只显示，不保存） */

static struct EdTermios g_ts_saved;      /* 进入时的 termios（退出还原用） */
static char g_rows[ED_ROWS + 3][ED_COLS + 2];   /* 渲染缓存（0..21 正文 / 22 状态 / 23 消息 / 24 命令） */
static char g_msg[ED_COLS + 2];

/* 行缓冲（渲染/证据行共用的一个小格式化缓冲；不引 malloc） */
static char g_fmt[256];

/* ==================== 小工具（不引 libc 的 printf，格式化自己来）==================== */
static int ed_strlen(const char* s) { int n = 0; while (s && s[n]) n++; return n; }
static int ed_streq(const char* a, const char* b) {
    int i = 0;
    for (; a[i] && b[i]; i++) if (a[i] != b[i]) return 0;
    return a[i] == b[i];
}
static char* ed_put(char* p, const char* s) { while (*s) *p++ = *s++; return p; }
static char* ed_putd(char* p, long v) {
    char t[24]; int n = 0; unsigned long u;
    if (v < 0) { *p++ = '-'; u = (unsigned long)(-v); } else u = (unsigned long)v;
    if (!u) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (u % 10u)); u /= 10u; }
    while (n) *p++ = t[--n];
    return p;
}
static char* ed_putx(char* p, unsigned long v) {
    static const char* H = "0123456789abcdef";
    int sh = (int)(sizeof(unsigned long) * 8u) - 4;
    *p++ = '0'; *p++ = 'x';
    while (sh > 0 && ((v >> sh) & 0xFu) == 0) sh -= 4;      /* 去掉前导 0（保留最低一位） */
    for (; sh >= 0; sh -= 4) *p++ = H[(v >> sh) & 0xFu];
    return p;
}
/* 只写串口（内核：fd 2 = 只串口，不画屏）—— 证据行专用 */
static void ed_ser(const char* s, int n) {
    if (n <= 0) return;
    (void)__v64_syscall(1, 2, (long)(uintptr_t)s, (long)n, 0, 0);
}
static void ed_ev1(const char* a, long v) {
    char* p = g_fmt; p = ed_put(p, a); p = ed_putd(p, v); *p++ = '\n';
    ed_ser(g_fmt, (int)(p - g_fmt));
}
/* 只写串口（内核：fd 2 = 只串口，不画屏）—— 证据行专用 */
static void ed_ev_path(const char* a, const char* path, const char* b, long v) {
    char* p = g_fmt; p = ed_put(p, a); p = ed_put(p, path); p = ed_put(p, b); p = ed_putd(p, v); *p++ = '\n';
    ed_ser(g_fmt, (int)(p - g_fmt));
}
/* 屏幕输出（fd 1 = 串口 + 屏幕） */
static void ed_scr(const char* s, int n) {
    if (n <= 0) return;
    (void)__v64_syscall(1, 1, (long)(uintptr_t)s, (long)n, 0, 0);
}
static void ed_scr_str(const char* s) { ed_scr(s, ed_strlen(s)); }

/* ==================== 光标定位 / 屏幕 ==================== */
static void ed_move(int row, int col) {
    char* p = g_fmt;
    p = ed_put(p, "\x1b[");
    p = ed_putd(p, row); *p++ = ';'; p = ed_putd(p, col); *p++ = 'H';
    ed_scr(g_fmt, (int)(p - g_fmt));
}
/* 把一行"想要的内容"画到屏幕（内容变了才发 ANSI；这就是串口流量的控制点）
 * ★ 先把 want 拷进本地缓冲：调用方常常直接传 g_fmt（状态行/命令行就是），而 ed_move() 也写
 *   g_fmt —— 直接传指针会被自己的定位序列覆盖开头 7 个字节（实测症状：状态行的路径前 7 个字符
 *   变成 `\x1b[23;1H`，屏幕上显示成 "dit64_test.txt …"）。 */
static void ed_putrow(int row, const char* want, int reverse) {
    const int idx = (row >= 1 && row <= ED_ROWS + 2) ? row - 1 : ED_ROWS + 2;
    if (ed_streq(g_rows[idx], want)) return;
    char line[ED_COLS + 2];
    int n = 0; while (want[n] && n < ED_COLS) { line[n] = want[n]; n++; }
    line[n] = 0;
    for (int i = 0; i <= n; i++) g_rows[idx][i] = line[i];
    ed_move(row, 1);
    if (reverse) ed_scr_str("\x1b[7m");
    ed_scr(line, n);
    ed_scr_str("\x1b[K");                       /* 清到行尾 */
    if (reverse) ed_scr_str("\x1b[0m");
}
/* 清屏 + 清渲染缓存（缓存清了 = 下一帧全部重画） */
static void ed_clear_screen(void) {
    ed_scr_str("\x1b[2J\x1b[H");
    for (int i = 0; i < ED_ROWS + 3; i++) g_rows[i][0] = 0;
}
static void ed_msg(const char* s) {
    int n = 0; while (s[n] && n < ED_COLS) { g_msg[n] = s[n]; n++; }
    g_msg[n] = 0;
}

/* ==================== 文本/行索引 ==================== */
static uint32_t ed_line_start(int i) { return g_lidx[i]; }
static uint32_t ed_line_len(int i)   { return g_lidx[i + 1] - g_lidx[i]; }
static char*    ed_line_ptr(int i)   { return g_text + g_lidx[i]; }

/* 重建行索引（全文扫一遍：1 MiB 文本 ~ 1ms 量级，改一次重建一次最不容易出错） */
static void ed_reindex(void) {
    uint32_t ln = 0;
    g_lidx[0] = 0;
    for (uint32_t i = 0; i < g_len; i++) {
        if (g_text[i] == '\n') {
            if (ln + 1u < ED_LINE_CAP) { ln++; g_lidx[ln] = i + 1u; }
            else break;                              /* 超上限：剩下的都算最后一行（如实） */
        }
    }
    if (ln + 1u >= ED_LINE_CAP) {
        /* 折行模式：把剩下的字节也算进最后一行（显示会截断，但内容/保存不受影响） */
    } else {
        ln++;
        g_lidx[ln] = g_len;
    }
    g_nlines = ln + 1u;
    g_text[g_len] = 0;
}

/* 从光标行+列算出文本偏移 */
static uint32_t ed_offset(int line, int col) {
    if (line < 0) line = 0;
    if (line >= (int)g_nlines) line = (int)g_nlines - 1;
    const uint32_t ls = ed_line_start(line);
    const uint32_t ll = ed_line_len(line);
    if (col < 0) col = 0;
    if ((uint32_t)col > ll) col = (int)ll;
    return ls + (uint32_t)col;
}

static int ed_clamp_cursor(void) {
    if (g_cy < 0) g_cy = 0;
    if (g_cy >= (int)g_nlines) g_cy = (int)g_nlines - 1;
    const int ll = (int)ed_line_len(g_cy);
    if (g_cx < 0) g_cx = 0;
    if (g_cx > ll) g_cx = ll;
    if (g_cy < g_top) g_top = g_cy;
    if (g_cy >= g_top + ED_ROWS) g_top = g_cy - ED_ROWS + 1;
    if (g_top < 0) g_top = 0;
    if (g_top > (int)g_nlines - 1) g_top = (int)g_nlines > 0 ? (int)g_nlines - 1 : 0;
    return g_cx;
}

/* 在光标处插入 n 字节（文本上限内） */
static int ed_insert(const char* s, uint32_t n) {
    if (g_ro) { ed_msg("read-only (open failed / too big)"); return 0; }
    if (g_len + n > ED_TEXT_CAP - 1u) { ed_msg("file too big (2 MiB cap)"); return 0; }
    const uint32_t off = ed_offset(g_cy, g_cx);
    for (uint32_t i = g_len; i > off; i--) g_text[i + n - 1u] = g_text[i - 1u];
    for (uint32_t i = 0; i < n; i++) g_text[off + i] = s[i];
    g_len += n;
    g_dirty = 1;
    ed_reindex();
    return 1;
}
/* 删掉 [off, off+n) */
static int ed_delete(unsigned n) {
    if (g_ro) { ed_msg("read-only"); return 0; }
    const uint32_t off = ed_offset(g_cy, g_cx);
    if (off + n > g_len) return 0;
    for (uint32_t i = off + n; i <= g_len; i++) g_text[i - n] = g_text[i];
    g_len -= n;
    g_dirty = 1;
    ed_reindex();
    return 1;
}

/* ==================== 文件读写 ==================== */
#define ED_O_RDONLY 0
#define ED_O_WRONLY 1
#define ED_O_CREAT  0100
#define ED_O_TRUNC  01000

/* 内核 tick（250Hz）—— 载入耗时统计与"没键也响应信号"的心跳都用它 */
static unsigned long ed_ticks(void) { return vimtu64_ticks(); }

static int ed_load(const char* path) {
    const unsigned long t0 = ed_ticks();
    const int fd = (int)__v64_syscall(2 /*open*/, (long)(uintptr_t)path, ED_O_RDONLY, 0, 0, 0);
    if (fd < 0) {
        ed_ev1("[EDIT64] open FAILED rc=", (long)fd);
        g_len = 1;                      /* 空文档（一个换行） */
        g_text[0] = '\n';
        ed_reindex();
        ed_msg("new/empty file (open failed)");
        return 0;
    }
    uint32_t total = 0;
    for (;;) {
        if (total + 4096u > ED_TEXT_CAP - 1u) { ed_msg("file > 2 MiB: truncated view"); break; }
        const long r = __v64_syscall(0 /*read*/, (long)fd, (long)(uintptr_t)(g_text + total), 4096, 0, 0);
        if (r < 0) { ed_msg("read error"); break; }
        if (r == 0) break;
        total += (uint32_t)r;
        if (r < 4096) break;            /* 短读 = 到文件尾 */
    }
    (void)__v64_syscall(3 /*close*/, (long)fd, 0, 0, 0, 0);
    g_len = total;
    if (g_len == 0) { g_len = 1; g_text[0] = '\n'; }        /* 空文件：给一行空行（保存时不写它） */
    ed_reindex();
    const unsigned long ms = (ed_ticks() - t0) * 4u;        /* 250Hz -> 4ms */
    char* p = g_fmt;
    p = ed_put(p, "[EDIT64] load file="); p = ed_put(p, path);
    p = ed_put(p, " bytes="); p = ed_putd(p, (long)g_len);
    p = ed_put(p, " lines="); p = ed_putd(p, (long)g_nlines);
    p = ed_put(p, " ms=");    p = ed_putd(p, (long)ms);
    *p++ = '\n';
    ed_ser(g_fmt, (int)(p - g_fmt));
    return 1;
}

/* 保存：整份写回（VimtuFS2 的 fd 写是"写到游标处 + 立刻整体落盘"，见 kernel/fd64.h）。
 * 先 O_TRUNC 打开（清空旧内容）再分块写；写完 fsync + close，并打点字节数给验收脚本比对。 */
static int ed_save(void) {
    if (g_ro) { ed_msg("read-only: cannot save"); ed_ev1("[EDIT64] save FAILED rc=", -1); return 0; }
    /* 空文档（我们合成的那个 '\n'）不写盘：内容为空就写 0 字节 */
    uint32_t n = g_len;
    if (n == 1u && g_text[0] == '\n' && g_nlines == 2u) n = 0;
    const int fd = (int)__v64_syscall(2, (long)(uintptr_t)g_path,
                                     ED_O_WRONLY | ED_O_CREAT | ED_O_TRUNC, 0644, 0, 0);
    if (fd < 0) {
        ed_msg("save: open failed");
        ed_ev1("[EDIT64] save FAILED rc=", (long)fd);
        return 0;
    }
    uint32_t done = 0;
    while (done < n) {
        uint32_t chunk = n - done;
        if (chunk > 4096u) chunk = 4096u;
        const long w = __v64_syscall(1 /*write*/, (long)fd, (long)(uintptr_t)(g_text + done), (long)chunk, 0, 0);
        if (w <= 0) { ed_msg("save: write failed"); break; }
        done += (uint32_t)w;
    }
    (void)__v64_syscall(74 /*fsync*/, (long)fd, 0, 0, 0, 0);
    (void)__v64_syscall(3 /*close*/, (long)fd, 0, 0, 0, 0);
    if (done == n) {
        g_dirty = 0;
        ed_msg("saved");
        ed_ev_path("[EDIT64] save path=", g_path, " bytes=", (long)n);
        return 1;
    }
    ed_msg("save incomplete");
    ed_ev1("[EDIT64] save FAILED bytes=", (long)done);
    return 0;
}

/* 放弃改动：重新从盘上读一遍（Ctrl+C 的语义） */
static void ed_discard(void) {
    const int was = g_dirty;
    ed_ev1("[EDIT64] sigint discard dirty_before=", was);
    g_dirty = 0;
    (void)ed_load(g_path);                      /* 重新装载 = 回到盘上的内容 */
    g_cy = 0; g_cx = 0; g_top = 0;              /* 光标回文件开头（明确语义：从盘上的内容从头继续） */
    ed_clamp_cursor();
    ed_msg("SIGINT: changes discarded, still running");
}

/* ==================== 渲染 ==================== */
static void ed_render(void) {
    char line[ED_COLS + 2];
    for (int r = 0; r < ED_ROWS; r++) {
        const int li = g_top + r;
        if (li < (int)g_nlines) {
            const uint32_t ll = ed_line_len(li);
            uint32_t n = ll;
            if (n > ED_COLS) n = ED_COLS;
            for (uint32_t i = 0; i < n; i++) line[i] = ed_line_ptr(li)[i];
            line[n] = 0;
        } else {
            line[0] = '~'; line[1] = 0;          /* 空行（vi 风格） */
        }
        ed_putrow(r + 1, line, 0);
    }
    /* 状态行（反显）：文件名 行,列 行数 修改标记 raw 标记 */
    {
        char* p = g_fmt;
        p = ed_put(p, " ");
        for (int i = 0; g_path[i] && p - g_fmt < ED_COLS - 40; i++) *p++ = g_path[i];
        p = ed_put(p, "  "); p = ed_putd(p, (long)(g_cy + 1));
        *p++ = ','; p = ed_putd(p, (long)(g_cx + 1));
        p = ed_put(p, "  lines="); p = ed_putd(p, (long)g_nlines);
        p = ed_put(p, "  "); p = ed_put(p, g_dirty ? "MODIFIED" : "saved");
        if (g_ro) p = ed_put(p, "  RO");
        p = ed_put(p, g_raw ? "  [raw]" : "  [cooked]");
        *p = 0;
        ed_putrow(ED_ROW_STATUS, g_fmt, 1);
    }
    ed_putrow(ED_ROW_MSG, g_msg, 0);
    if (g_cmdmode) {
        char* p = g_fmt; *p++ = ':';
        for (int i = 0; i < g_cmdlen; i++) *p++ = g_cmd[i];
        *p = 0;
        ed_putrow(ED_ROW_MSG + 1, g_fmt, 0);
    } else {
        ed_putrow(ED_ROW_MSG + 1, "", 0);
    }
    /* 光标 */
    ed_move(g_cy - g_top + 1, g_cx + 1);
}

/* ==================== 信号 ==================== */
static void ed_on_sigint(int sig) {
    (void)sig;
    g_sigint = 1;                       /* handler 里只置标志：投递点可能在任意指令之间 */
    ed_ser("[EDIT64] sigint handler ran (discard+continue)\n", 48);
}

/* ==================== 命令模式 ==================== */
static void ed_run_cmd(const char* c) {
    ed_ev_path("[EDIT64] cmd=", c, "", 0);
    if (ed_streq(c, "w") || ed_streq(c, "wq")) {
        (void)ed_save();
        if (ed_streq(c, "wq")) g_quit = 1;
    } else if (ed_streq(c, "q")) {
        if (g_dirty) { ed_msg("modified: use :q! or Ctrl+S first"); }
        else g_quit = 1;
    } else if (ed_streq(c, "q!")) {
        g_quit = 1; g_quit_force = 1;
    } else if (ed_streq(c, "e")) {
        ed_discard();
    } else {
        ed_msg("commands: :w :q :q! :wq :e");
    }
}

/* ==================== 按键 ==================== */
static void ed_key(uint32_t code) {
    if (g_keylog_budget > 0) {
        g_keylog_budget--;
        char* p = g_fmt;
        p = ed_put(p, "[EDIT64] key="); p = ed_putx(p, code);
        p = ed_put(p, " line="); p = ed_putd(p, (long)(g_cy + 1));
        p = ed_put(p, " col=");  p = ed_putd(p, (long)(g_cx + 1));
        p = ed_put(p, " dirty="); p = ed_putd(p, (long)g_dirty);
        *p++ = '\n';
        ed_ser(g_fmt, (int)(p - g_fmt));
    }
    if (g_cmdmode) {                                    /* 命令模式：只收可打印键 + 回车/Esc */
        if (code == '\n' || code == '\r') { ed_run_cmd(g_cmd); g_cmdmode = 0; g_cmdlen = 0; return; }
        if (code == ED_KEY_ESC) { g_cmdmode = 0; g_cmdlen = 0; ed_msg("cmd cancelled"); return; }
        if (code == 0x08 || code == 0x7F) { if (g_cmdlen > 0) g_cmd[--g_cmdlen] = 0; return; }
        if (code >= 0x20 && code < 0x7F && g_cmdlen < ED_CMD_MAX - 1) { g_cmd[g_cmdlen++] = (char)code; g_cmd[g_cmdlen] = 0; }
        return;
    }
    switch (code) {
    case ED_KEY_ESC:  g_cmdmode = 1; g_cmdlen = 0; g_cmd[0] = 0; ed_msg(":w save  :q quit  :q! force  :e reload"); return;
    case ED_KEY_UP:   if (g_cy > 0) { g_cy--; ed_clamp_cursor(); } return;
    case ED_KEY_DOWN: if (g_cy + 1 < (int)g_nlines) { g_cy++; ed_clamp_cursor(); } return;
    case ED_KEY_LEFT:
        if (g_cx > 0) g_cx--;
        else if (g_cy > 0) { g_cy--; g_cx = (int)ed_line_len(g_cy); }
        ed_clamp_cursor(); return;
    case ED_KEY_RIGHT:
        if (g_cx < (int)ed_line_len(g_cy)) g_cx++;
        else if (g_cy + 1 < (int)g_nlines) { g_cy++; g_cx = 0; }
        ed_clamp_cursor(); return;
    case ED_KEY_PAGEUP:   for (int i = 0; i < ED_ROWS; i++) if (g_cy > 0) g_cy--; ed_clamp_cursor(); return;
    case ED_KEY_PAGEDOWN: for (int i = 0; i < ED_ROWS; i++) if (g_cy + 1 < (int)g_nlines) g_cy++; ed_clamp_cursor(); return;
    case ED_KEY_DELETE:   /* Delete：删掉光标后的字符（行尾则并下一行） */
        if (g_cx < (int)ed_line_len(g_cy)) { (void)ed_delete(1); }
        else if (g_cy + 1 < (int)g_nlines) { (void)ed_delete(1); }
        ed_clamp_cursor(); return;
    case 0x08: case 0x7F: /* Backspace */
        if (g_cx > 0) { g_cx--; (void)ed_delete(1); }
        else if (g_cy > 0) { g_cy--; g_cx = (int)ed_line_len(g_cy); (void)ed_delete(1); }
        ed_clamp_cursor(); return;
    case '\n': case '\r': (void)ed_insert("\n", 1); g_cy++; g_cx = 0; ed_clamp_cursor(); return;
    case ED_KEY_CTRL_S: (void)ed_save(); return;
    case ED_KEY_CTRL_Q:
        if (g_dirty) ed_msg("modified: Ctrl+S first, or Esc :q!");
        else g_quit = 1;
        return;
    case ED_KEY_CTRL_C:
        /* ★ 走新做的信号投递：给自己发 SIGINT（内核在系统调用出口投递 handler） */
        (void)raise(SIGINT);
        return;
    default:
        if (code >= 0x20 && code < 0x7F) {
            const char ch = (char)code;
            if (ed_insert(&ch, 1)) { g_cx++; ed_clamp_cursor(); }
        }
        return;
    }
}

/* ==================== 主循环 ==================== */
static void ed_restore_term(void) {
    if (!g_raw) return;
    struct EdTermios back;
    (void)ioctl(1, ED_TCSETS, &g_ts_saved);            /* 还原 */
    back = g_ts_saved;
    const int got = (ioctl(1, ED_TCGETS, &back) == 0) ? 1 : 0;
    (void)ioctl(1, ED_TCSETS, &g_ts_saved);            /* 读回会覆盖？不会：读回只写 back；再还原一次幂等 */
    char* p = g_fmt;
    p = ed_put(p, "[EDIT64] termios restored ok="); p = ed_putd(p, got);
    p = ed_put(p, " lflag="); p = ed_putx(p, back.c_lflag);
    p = ed_put(p, " ic=");    p = ed_putx(p, back.c_iflag);
    p = ed_put(p, " want_lflag="); p = ed_putx(p, g_ts_saved.c_lflag);
    *p++ = '\n';
    ed_ser(g_fmt, (int)(p - g_fmt));
    g_raw = 0;
}

int edit_main(uint64_t* sp) {
    const int argc = (sp && sp[0] > 0 && sp[0] < 64) ? (int)sp[0] : 0;
    const char* const* argv = (const char* const*)(sp ? sp + 1 : 0);
    const char* path = (argc >= 2 && argv[1] && argv[1][0]) ? argv[1] : "/etc/edit64_test.txt";
    int i = 0;
    for (; path[i] && i < ED_PATH_MAX - 1; i++) g_path[i] = path[i];
    g_path[i] = 0;

    /* ---- 1) mmap 两块缓冲（文本 + 行索引）---- */
    g_text = (char*)mmap(0, ED_TEXT_CAP, 3 /*PROT_READ|PROT_WRITE*/, 0x22 /*MAP_PRIVATE|MAP_ANON*/, -1, 0);
    g_lidx = (uint32_t*)mmap(0, ED_LINE_CAP * 4u, 3, 0x22, -1, 0);
    if (!g_text || !g_lidx) {
        char* p = g_fmt;
        p = ed_put(p, "[EDIT64] FATAL mmap failed text="); p = ed_putx(p, (unsigned long)(uintptr_t)g_text);
        p = ed_put(p, " lidx="); p = ed_putx(p, (unsigned long)(uintptr_t)g_lidx);
        *p++ = '\n';
        ed_ser(g_fmt, (int)(p - g_fmt));
        return 2;
    }
    g_len = 0; g_nlines = 1; g_lidx[0] = 0; g_lidx[1] = 0;
    g_cy = g_cx = g_top = 0;

    /* ---- 2) 证据行 + 信号 handler（Ctrl+C 之前必须先装好）---- */
    {
        char* p = g_fmt;
        p = ed_put(p, "[EDIT64] start file="); p = ed_put(p, g_path);
        p = ed_put(p, " pid=");  p = ed_putd(p, (long)getpid());
        p = ed_put(p, " argc="); p = ed_putd(p, (long)argc);
        *p++ = '\n';
        ed_ser(g_fmt, (int)(p - g_fmt));
    }
    {
        struct sigaction sa;
        sa.handler = ed_on_sigint; sa.flags = 0; sa.restorer = __v64_sigreturn_stub; sa.mask = 0;
        const int rc = sigaction(SIGINT, &sa, NULL);
        ed_ev1("[EDIT64] sigaction(INT) rc=", (long)rc);
    }
    /* 自立进程组 = 终端前台作业（终端 Ctrl+C 只打我们，不连累 shell；见 kernel/sig64.h） */
    (void)setpgid(0, 0);

    /* ---- 3) 申请键盘焦点（input_poll 的 EV64_FLAG_FOCUS）---- */
    {
        const int r = poll_event(NULL, 0, V64_EV_FLAG_FOCUS);
        ed_ev1("[EDIT64] focus request rc=", (long)r);
    }

    /* ---- 4) raw 模式：保存原 termios，写一份关掉 ICANON/ECHO/ISIG 的 ---- */
    {
        struct EdTermios ts;
        struct EdWinsize ws;
        const int got = (ioctl(1, ED_TCGETS, &ts) == 0) ? 1 : 0;
        if (got) g_ts_saved = ts;
        (void)ioctl(1, ED_TIOCGWINSZ, &ws);
        if (got) {
            struct EdTermios raw = ts;
            raw.c_lflag &= ~(0x1u | 0x2u | 0x8u);        /* 清 ISIG|ICANON|ECHO */
            raw.c_iflag &= ~0x1u;                        /* 清 ICRNL（raw 的惯例） */
            if (ioctl(1, ED_TCSETS, &raw) == 0) {
                g_raw = 1;
                char* p = g_fmt;
                p = ed_put(p, "[EDIT64] termios raw ok=1 lflag="); p = ed_putx(p, ts.c_lflag);
                p = ed_put(p, " raw="); p = ed_putx(p, raw.c_lflag);
                p = ed_put(p, " win="); p = ed_putd(p, (long)ws.col);
                *p++ = 'x'; p = ed_putd(p, (long)ws.row);
                *p++ = '\n';
                ed_ser(g_fmt, (int)(p - g_fmt));
            } else {
                ed_ser("[EDIT64] termios raw ok=0 (TCSETS failed)\n", 40);
            }
        } else {
            ed_ser("[EDIT64] termios raw ok=0 (TCGETS failed)\n", 40);
        }
    }

    /* ---- 5) 装载文件 ---- */
    (void)ed_load(g_path);
    ed_clear_screen();
    ed_msg("Ctrl+S save  Ctrl+Q quit  Ctrl+C discard  arrows/PageUp/Down  Esc :cmd");
    ed_render();
    ed_ev_path("[EDIT64] ready file=", g_path, " bytes=", (long)g_len);

    /* ---- 6) 主循环：input_poll 收键、按需重绘 ---- */
    {
        struct Ev64Event ev[8];
        unsigned long last_blink = ed_ticks();
        while (!g_quit) {
            const int n = poll_event(ev, 8, 0);
            if (n > 0) {
                for (int k = 0; k < n; k++) {
                    if (ev[k].type != V64_EV_KEY_DOWN) continue;      /* 只看按下（抬起忽略） */
                    ed_key(ev[k].code);
                }
            } else if (n < 0) {
                ed_ev1("[EDIT64] input_poll FAILED rc=", (long)n);
                break;
            }
            if (g_sigint) {                                    /* Ctrl+C / 终端 tty-int -> 放弃改动 */
                g_sigint = 0;
                ed_discard();
                ed_render();
            }
            if (n <= 0) {
                if ((ed_ticks() - last_blink) >= 50u) {         /* 200ms 心跳：没键也要有机会响应信号 */
                    last_blink = ed_ticks();
                    if (g_sigint) { g_sigint = 0; ed_discard(); }
                }
                (void)vimtu64_sleep_ms(15);
            }
            ed_render();
        }
    }

    /* ---- 7) 收尾：还原终端、释放焦点、清屏、证据 ---- */
    ed_restore_term();
    (void)poll_event(NULL, 0, V64_EV_FLAG_RELEASE);
    ed_scr_str("\x1b[0m\x1b[2J\x1b[H");
    {
        char* p = g_fmt;
        p = ed_put(p, "[EDIT64] exit code=0 dirty="); p = ed_putd(p, (long)g_dirty);
        p = ed_put(p, " bytes="); p = ed_putd(p, (long)g_len);
        p = ed_put(p, " lines="); p = ed_putd(p, (long)g_nlines);
        p = ed_put(p, " force="); p = ed_putd(p, (long)g_quit_force);
        *p++ = '\n';
        ed_ser(g_fmt, (int)(p - g_fmt));
    }
    return 0;
}
