/* office.c - VimtuOS 办公套件（ring3 真窗口，v0.5.3）
 *
 * 三合一：**文档 / 表格 / 演示**，一个进程、一套窗口外壳（复用 /bin/browser 验证过的做法：
 * shm + wl_surface + wl_seat_get + font64 排版）。表格公式引擎在 user/office/calc.c（独立验收）。
 *
 * 交付（与 /bin/browser 同一条纪律）：**系统卷里的 /bin/office**，tools/office_pack_win.py
 * 构建期写入并逐字节回读；内核二进制里一个字节都不加。
 *
 * 诚实边界（别当成 Office）：
 *   * 文档是**纯文本行编辑**（没有富文本/字体/图片/表格内嵌）；UTF-8 按字节移动光标但
 *     插入/删除会对齐字符边界（不会切坏半个汉字）；不支持输入法（键盘只给 ASCII）。
 *   * 表格是 **8 列 x 20 行**的定点整数网格（公式见 calc.h：四则/括号/引用/SUM/环检测），
 *     没有小数、没有函数库、没有跨表引用。
 *   * 演示是 **3 页 x 8 行**的标题+要点，放映模式只做"白底大字 + 点击/空格翻页"。
 *   * 文件格式都是自己的纯文本（.vwd / .vts / .vpt），不兼容任何外部格式。
 *
 * 打点（tests/office64_test.py grep；格式别改）：
 *   [OFFICE] win surf=.. w=896 h=580 shm=.. map=..
 *   [OFFICE] fonts faces=.. ok=..
 *   [OFFICE] seek mode=.. path=.. rc=.. bytes=..
 *   [OFFICE] doc lines=.. bytes=..
 *   [OFFICE] key code=.. mode=.. line=.. col=..   （只在位置真的变了时打）
 *   [OFFICE] save mode=.. path=.. bytes=.. rc=..
 *   [OFFICE] sheet set cell=A1 expr=.. value=.. err=..
 *   [OFFICE] render mode=.. lines=.. scroll=..
 *   [OFFICE] done frames=.. reason=esc
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
#include "calc.h"

int open(const char* path, int flags, ...);

/* ---- 窗口几何（与 tests/office64_test.py 一致）---- */
#define OW            896
#define OH            580
#define OW_BYTES      (OW * OH * 4)
#define TAB_H         28
#define BODY_Y        (TAB_H + 6)
#define BODY_H        (OH - BODY_Y - 24)
#define STATUS_Y      (OH - 24)
#define LINE_H        18
#define DOC_X         12
#define DOC_W         (OW - 24)
#define VIS_LINES     (BODY_H / LINE_H)

#define MAX_FRAMES    3600
#define FRAME_MS      30u
#define EV_MAX        16

/* 文档：字符流 + 行索引（都在 mmap 竞技场里） */
#define DOC_CAP       (32 * 1024)
#define DOC_MAX_LINES 256
/* 表格：8x20 格，每格表达式 <= 23 字节 */
#define CELL_TXT      24
/* 演示：3 页 x 8 行，每行 <= 63 字节 */
#define SLIDES        3
#define SLIDE_LINES   8
#define SLIDE_TXT     64

/* 存盘路径（/tmp 在系统卷里是 0777，普通会话可写） */
#define DOC_PATH      "/tmp/doc.vwd"
#define SHEET_PATH    "/tmp/sheet.vts"
#define DECK_PATH     "/tmp/deck.vpt"

#define ARENA_BYTES   (96u * 1024u)

/* 调色板 */
#define CO_BG      0xFFFFFFFFu
#define CO_TEXT    0xFF1A1A1Au
#define CO_CHROME  0xFF20242Cu
#define CO_CHROME2 0xFF2C3340u
#define CO_ACCENT  0xFF1E6FD9u
#define CO_GRID    0xFFD0D6E0u
#define CO_CURSOR  0xFFE8F0FEu
#define CO_DIMTXT  0xFF9AA6B8u
#define CO_BRIGHT  0xFFE8ECF2u
#define CO_ERR     0xFFC0392Bu

enum { M_DOC = 0, M_SHEET = 1, M_SLIDES = 2, M_PLAY = 3 };

static uint32_t*        g_px;
static struct F64Canvas g_cv;
static struct Font64*   g_font;
static int g_surf = -1, g_shm = -1;
static unsigned char*   g_arena;
static int g_dirty = 1, g_frames = 0;
static int g_mode = M_DOC;
static int g_scroll = 0;

/* 文档状态 */
static char* g_text;
static int   g_len;
static int   g_line_off[DOC_MAX_LINES];
static int   g_nlines;
static int   g_cur_line, g_cur_col;      /* col = 字节偏移（行内） */

/* 表格状态 */
static char* g_cells;                          /* CALC_ROWS*CALC_COLS*CELL_TXT 平铺 */
static int64_t* g_vals;
static int8_t*  g_errs;
static int8_t*  g_visiting;
static int64_t* g_cache;
static int8_t*  g_cached;
static int g_sr, g_sc;                    /* 表格光标 */

/* 演示状态 */
static char* g_slide_txt;                 /* SLIDES*SLIDE_LINES*SLIDE_TXT */
static int g_slide, g_play;

static unsigned px(unsigned rgb) { return 0xFF000000u | (rgb & 0xFFFFFFu); }

static void fill(int x, int y, int w, int h, unsigned argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > OW) w = OW - x;
    if (y + h > OH) h = OH - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = y; yy < y + h; yy++) {
        uint32_t* row = g_px + (unsigned)yy * OW;
        for (int xx = x; xx < x + w; xx++) row[xx] = argb;
    }
}

static void txt(int x, int y, const char* s, int size, unsigned argb) {
    if (!g_font || !s) return;
    (void)font_draw_text(g_font, &g_cv, x, y, s, size, argb);
}

static void submit(void) {
    (void)wl_surface_attach(g_surf, g_shm, 0);
    (void)wl_surface_damage(g_surf, 0, 0, OW, OH);
    (void)wl_surface_commit(g_surf);
}

static void str_copy(char* dst, const char* src, int cap) {
    int i = 0;
    for (; src[i] && i < cap - 1; i++) dst[i] = src[i];
    dst[i] = 0;
}
static int str_len(const char* s) { int n = 0; while (s[n]) n++; return n; }

/* ==================== 文档模型 ==================== */
static void doc_reindex(void) {
    g_nlines = 1;
    g_line_off[0] = 0;
    for (int i = 0; i < g_len && g_nlines < DOC_MAX_LINES; i++) {
        if (g_text[i] == '\n') g_line_off[g_nlines++] = i + 1;
    }
}
static int doc_line_len(int ln) {
    if (ln < 0 || ln >= g_nlines) return 0;
    const int s = g_line_off[ln];
    const int e = (ln + 1 < g_nlines) ? g_line_off[ln + 1] - 1 : g_len;
    return (e > s) ? (e - s) : 0;
}
static const char* doc_line_ptr(int ln) {
    if (ln < 0 || ln >= g_nlines) return "";
    return g_text + g_line_off[ln];
}
static void doc_clamp_cursor(void) {
    if (g_cur_line < 0) g_cur_line = 0;
    if (g_cur_line >= g_nlines) g_cur_line = g_nlines ? g_nlines - 1 : 0;
    const int ll = doc_line_len(g_cur_line);
    if (g_cur_col < 0) g_cur_col = 0;
    if (g_cur_col > ll) g_cur_col = ll;
    /* 对齐 UTF-8 字符边界（不切半个汉字） */
    while (g_cur_col > 0 && g_cur_col < ll &&
           ((unsigned char)doc_line_ptr(g_cur_line)[g_cur_col] & 0xC0u) == 0x80u) g_cur_col--;
}
static int doc_byte_at(void) { return g_line_off[g_cur_line] + g_cur_col; }
static void doc_insert(char ch) {
    if (g_len >= DOC_CAP - 2) return;
    const int at = doc_byte_at();
    for (int i = g_len; i > at; i--) g_text[i] = g_text[i - 1];
    g_text[at] = ch;
    g_len++;
    g_text[g_len] = 0;
    doc_reindex();
    if (ch == '\n') { g_cur_line++; g_cur_col = 0; }
    else g_cur_col++;
    doc_clamp_cursor();
    g_dirty = 1;
}
static void doc_backspace(void) {
    if (g_len <= 0) return;
    const int at = doc_byte_at();
    if (at <= 0) return;
    /* 往前退一个完整的 UTF-8 字符 */
    int n = 1;
    while (at - n > 0 && ((unsigned char)g_text[at - n] & 0xC0u) == 0x80u) n++;
    const int was_nl = (g_text[at - 1] == '\n');
    for (int i = at - n; i + n <= g_len; i++) g_text[i] = g_text[i + n];
    g_len -= n;
    g_text[g_len] = 0;
    doc_reindex();
    if (was_nl && g_cur_line > 0) { g_cur_line--; g_cur_col = doc_line_len(g_cur_line); }
    else if (g_cur_col > 0) g_cur_col -= n;
    doc_clamp_cursor();
    g_dirty = 1;
}

/* ==================== 表格模型 ==================== */
static char* cell_txt(int r, int c) {
    return g_cells + ((r * CALC_COLS + c) * CELL_TXT);
}
static int cell_idx(int r, int c) { return r * CALC_COLS + c; }

/* 求值回调：给 calc.c 用；带环检测（visiting 标记）与缓存 */
static int sheet_value(int r, int c, int64_t* out);
static int sheet_cb(int r, int c, void* user, int64_t* out) {
    (void)user;
    return sheet_value(r, c, out);
}
static int sheet_value(int r, int c, int64_t* out) {
    if (r < 0 || r >= CALC_ROWS || c < 0 || c >= CALC_COLS) return CALC_EREF;
    const int k = cell_idx(r, c);
    if (g_cached[k]) { *out = g_cache[k]; return g_errs[k] ? g_errs[k] : CALC_OK; }
    if (g_visiting[k]) return CALC_ECYCLE;
    g_visiting[k] = 1;
    int rc = CALC_OK;
    int64_t v = 0;
    const char* e = cell_txt(r, c);
    if (e[0] == '=') {
        rc = calc_eval(e, sheet_cb, 0, &v);
    } else if (e[0]) {
        /* 纯数字：手写解析（不引入 strtoll） */
        int neg = 0, i = 0;
        if (e[0] == '-') { neg = 1; i = 1; }
        int any = 0;
        while (e[i] >= '0' && e[i] <= '9') { v = v * 10 + (e[i] - '0'); i++; any = 1; }
        if (!any || e[i]) rc = CALC_ESYNTAX;
        if (neg) v = -v;
    } else {
        v = 0;
    }
    g_visiting[k] = 0;
    g_vals[k] = v;
    g_errs[k] = (int8_t)(rc ? rc : 0);
    g_cache[k] = v;
    g_cached[k] = 1;
    *out = v;
    return rc;
}
static void sheet_flush_cache(void) {
    for (int i = 0; i < CALC_ROWS * CALC_COLS; i++) { g_cached[i] = 0; g_visiting[i] = 0; }
}
static const char* sheet_err_text(int rc) {
    switch (rc) {
        case CALC_ESYNTAX: return "#SYNTAX";
        case CALC_ECYCLE:  return "#CIRC";
        case CALC_EDIV0:   return "#DIV0";
        case CALC_EREF:    return "#REF";
        case CALC_EOVER:   return "#OVER";
        default: return "";
    }
}
static void i64_str(int64_t v, char* out) {
    char t[24];
    int k = 0, n = 0;
    unsigned long long x;
    if (v < 0) { out[n++] = '-'; x = (unsigned long long)(-(v + 1)) + 1ULL; }
    else x = (unsigned long long)v;
    if (x == 0) t[k++] = '0';
    while (x) { t[k++] = (char)('0' + (int)(x % 10)); x /= 10; }
    while (k) out[n++] = t[--k];
    out[n] = 0;
}

/* ==================== 演示模型 ==================== */
static char* slide_line(int s, int l) {
    return g_slide_txt + ((s * SLIDE_LINES + l) * SLIDE_TXT);
}

/* ==================== 文件 IO ==================== */
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

/* 写文件：走 Linux 号段 openat/write/close 太绕；这里用自有 ABI 没有"写文件"号，
 * 所以走 /tmp 下已有文件的"整文件覆盖"路径 —— 见 save_file() 的注释。 */
static int save_file(const char* path, const char* data, int len) {
    /* 用 Linux 号段：openat(257, AT_FDCWD=-100, path, O_WRONLY|O_CREAT|O_TRUNC=0x241, 0644) */
    long fd = -1;
    {
        register long rax __asm__("rax") = 257;
        register long rdi __asm__("rdi") = -100;
        register long rsi __asm__("rsi") = (long)(uintptr_t)path;
        register long rdx __asm__("rdx") = 0x241;
        register long r10 __asm__("r10") = 0644;
        __asm__ __volatile__("syscall" : "+r"(rax)
                             : "r"(rdi), "r"(rsi), "r"(rdx), "r"(r10)
                             : "rcx", "r11", "memory");
        fd = rax;
    }
    if (fd < 0) return (int)fd;
    int done = 0;
    while (done < len) {
        long r;
        {
            register long rax __asm__("rax") = 1;          /* write */
            register long rdi __asm__("rdi") = fd;
            register long rsi __asm__("rsi") = (long)(uintptr_t)(data + done);
            register long rdx __asm__("rdx") = (long)(len - done);
            __asm__ __volatile__("syscall" : "+r"(rax)
                                 : "r"(rdi), "r"(rsi), "r"(rdx) : "rcx", "r11", "memory");
            r = rax;
        }
        if (r <= 0) break;
        done += (int)r;
    }
    {
        register long rax __asm__("rax") = 3;              /* close */
        register long rdi __asm__("rdi") = fd;
        __asm__ __volatile__("syscall" : "+r"(rax) : "r"(rdi) : "rcx", "r11", "memory");
    }
    return done;
}

/* ==================== 加载/保存 ==================== */
static void doc_load(void) {
    const int n = read_file(DOC_PATH, g_text, DOC_CAP);
    if (n > 0) { g_len = n; g_text[g_len] = 0; }
    else {
        const char* d = "VimtuOS Office —— 文档模式\n\n"
                        "F1 文档 / F2 表格 / F3 演示   Ctrl+S 保存   ESC 退出\n"
                        "方向键移动，PageUp/PageDown 翻页，直接敲字插入。\n\n"
                        "这里是纯文本行编辑：没有富文本、没有图片、没有输入法。\n";
        g_len = 0;
        for (int i = 0; d[i] && g_len < DOC_CAP - 2; i++) g_text[g_len++] = d[i];
        g_text[g_len] = 0;
    }
    doc_reindex();
    g_cur_line = 0;
    g_cur_col = 0;
    printf("[OFFICE] seek mode=doc path=%s rc=%d bytes=%d\n", DOC_PATH, n > 0 ? 0 : -1, g_len);
    printf("[OFFICE] doc lines=%d bytes=%d\n", g_nlines, g_len);
}

static void sheet_load(void) {
    char tmp[1024];
    const int n = read_file(SHEET_PATH, tmp, (int)sizeof(tmp));
    for (int i = 0; i < CALC_ROWS * CALC_COLS * CELL_TXT; i++) g_cells[i] = 0;
    if (n > 0) {
        /* 格式：每行 "r c 文本\n"（r/c 0 基） */
        int i = 0;
        while (i < n) {
            int r = 0, c = 0, k = 0;
            while (i < n && tmp[i] >= '0' && tmp[i] <= '9') { r = r * 10 + (tmp[i] - '0'); i++; }
            if (i < n && tmp[i] == ' ') i++;
            while (i < n && tmp[i] >= '0' && tmp[i] <= '9') { c = c * 10 + (tmp[i] - '0'); i++; }
            if (i < n && tmp[i] == ' ') i++;
            char* dst = cell_txt(r, c);
            while (i < n && tmp[i] != '\n' && k < CELL_TXT - 1) dst[k++] = tmp[i++];
            dst[k] = 0;
            if (i < n && tmp[i] == '\n') i++;
            if (r < 0 || r >= CALC_ROWS || c < 0 || c >= CALC_COLS) break;
        }
    } else {
        /* 默认样例：A1=10 A2=20 A3=30 B1=2 C1==A1+B1*2 D1==SUM(A1:A3) */
        str_copy(cell_txt(0, 0), "10", CELL_TXT);
        str_copy(cell_txt(1, 0), "20", CELL_TXT);
        str_copy(cell_txt(2, 0), "30", CELL_TXT);
        str_copy(cell_txt(0, 1), "2", CELL_TXT);
        str_copy(cell_txt(0, 2), "=A1+B1*2", CELL_TXT);
        str_copy(cell_txt(0, 3), "=SUM(A1:A3)", CELL_TXT);
    }
    sheet_flush_cache();
    g_sr = 0; g_sc = 0;
    printf("[OFFICE] seek mode=sheet path=%s rc=%d bytes=%d\n", SHEET_PATH, n > 0 ? 0 : -1, n);
}

static void slides_load(void) {
    const char* d[3][3] = {
        { "VimtuOS 办公套件", "三合一：文档 / 表格 / 演示", "ring3 真窗口 · 纯用户态" },
        { "这一版能做什么", "文档：纯文本行编辑 + 存盘", "表格：8x20 定点整数 + 四则/SUM/环检测" },
        { "诚实边界", "没有富文本 / 图片 / 输入法", "没有小数 / 函数库 / 跨表引用" },
    };
    for (int s = 0; s < SLIDES; s++) {
        for (int l = 0; l < SLIDE_LINES; l++) slide_line(s, l)[0] = 0;
        for (int l = 0; l < 3; l++) str_copy(slide_line(s, l + (l == 0 ? 0 : 1)), d[s][l], SLIDE_TXT);
    }
    g_slide = 0; g_play = 0;
    printf("[OFFICE] seek mode=slides path=%s rc=-1 bytes=0\n", DECK_PATH);
}

/* ==================== 渲染 ==================== */
static void render_tabs(void) {
    fill(0, 0, OW, TAB_H, px(CO_CHROME & 0xFFFFFFu));
    const char* names[3] = { "文档 F1", "表格 F2", "演示 F3" };
    for (int i = 0; i < 3; i++) {
        const int x = 10 + i * 96;
        const int on = (g_mode == i) || (g_mode == M_PLAY && i == M_SLIDES);
        if (on) fill(x - 4, 3, 92, TAB_H - 6, px(CO_ACCENT));
        txt(x, 6, names[i], 14, px(on ? CO_BRIGHT : CO_DIMTXT));
    }
    txt(OW - 300, 7, "Ctrl+S 保存   ESC 退出", 12, px(CO_DIMTXT));
}

static void render_doc(void) {
    int first = g_scroll;
    for (int i = 0; i < VIS_LINES; i++) {
        const int ln = first + i;
        if (ln >= g_nlines) break;
        const int y = BODY_Y + i * LINE_H;
        if (ln == g_cur_line) fill(0, y - 2, OW, LINE_H, px(CO_CURSOR & 0xFFFFFFu));
        txt(DOC_X, y, doc_line_ptr(ln), 16, px(CO_TEXT & 0xFFFFFFu));
    }
    /* 状态栏 */
    fill(0, STATUS_Y, OW, OH - STATUS_Y, px(CO_CHROME & 0xFFFFFFu));
    {
        char st[120];
        char n1[24], n2[24], n3[24];
        i64_str(g_cur_line + 1, n1);
        i64_str(g_cur_col + 1, n2);
        i64_str(g_nlines, n3);
        str_copy(st, "文档  行 ", 120);
        { int k = str_len(st); for (int i = 0; n1[i] && k < 110; i++) st[k++] = n1[i];
          st[k++] = '/'; for (int i = 0; n3[i] && k < 114; i++) st[k++] = n3[i];
          const char* c = "  列 "; for (int i = 0; c[i] && k < 116; i++) st[k++] = c[i];
          for (int i = 0; n2[i] && k < 118; i++) st[k++] = n2[i]; st[k] = 0; }
        txt(12, STATUS_Y + 4, st, 13, px(CO_DIMTXT));
    }
    printf("[OFFICE] render mode=doc lines=%d scroll=%d\n", g_nlines, g_scroll);
}

static void render_sheet(void) {
    const int cw = (OW - 24) / CALC_COLS;
    const int rh = 22;
    /* 列头 */
    for (int c = 0; c < CALC_COLS; c++) {
        char nm[8];
        (void)calc_ref_name(0, c, nm, sizeof(nm));
        nm[1] = 0;                                  /* 只要字母 */
        fill(12 + c * cw, BODY_Y, cw - 1, 18, px(0xF0F2F6u));
        txt(16 + c * cw, BODY_Y + 1, nm, 13, px(CO_TEXT & 0xFFFFFFu));
    }
    /* 行 */
    for (int r = 0; r < CALC_ROWS; r++) {
        const int y = BODY_Y + 18 + r * rh;
        if (y + rh > STATUS_Y) break;
        char nm[8];
        (void)calc_ref_name(r, 0, nm, sizeof(nm));
        fill(0, y, 20, rh - 1, px(0xF0F2F6u));
        txt(2, y + 1, nm, 12, px(CO_DIMTXT));
        for (int c = 0; c < CALC_COLS; c++) {
            const int x = 12 + c * cw;
            fill(x, y, cw - 1, rh - 1, px((r == g_sr && c == g_sc) ? (CO_CURSOR & 0xFFFFFFu) : 0xFFFFFFu));
            const char* e = cell_txt(r, c);
            char shown[32];
            if (e[0]) {
                int64_t v = 0;
                const int rc = sheet_value(r, c, &v);
                if (rc != CALC_OK) str_copy(shown, sheet_err_text(rc), sizeof(shown));
                else i64_str(v, shown);
            } else shown[0] = 0;
            txt(x + 3, y + 2, shown, 13, px(e[0] == '=' ? CO_ACCENT : (CO_TEXT & 0xFFFFFFu)));
        }
    }
    fill(0, STATUS_Y, OW, OH - STATUS_Y, px(CO_CHROME & 0xFFFFFFu));
    {
        char st[160], nm[8], v[32];
        (void)calc_ref_name(g_sr, g_sc, nm, sizeof(nm));
        int64_t val = 0;
        const int rc = sheet_value(g_sr, g_sc, &val);
        if (rc != CALC_OK) str_copy(v, sheet_err_text(rc), sizeof(v));
        else i64_str(val, v);
        str_copy(st, "表格  ", 160);
        { int k = str_len(st); const char* a = "单元格 "; for (int i = 0; a[i]; i++) st[k++] = a[i];
          for (int i = 0; nm[i]; i++) st[k++] = nm[i];
          const char* b = "  = "; for (int i = 0; b[i]; i++) st[k++] = b[i];
          for (int i = 0; v[i]; i++) st[k++] = v[i];
          const char* c = "   输入：数字或 =公式（四则/A1/SUM(A1:A3)）"; for (int i = 0; c[i] && k < 150; i++) st[k++] = c[i];
          st[k] = 0; }
        txt(12, STATUS_Y + 4, st, 13, px(CO_DIMTXT));
    }
    printf("[OFFICE] render mode=sheet lines=%d scroll=%d\n", CALC_ROWS, 0);
}

static void render_slides(void) {
    const int big = g_mode == M_PLAY;
    if (big) {
        fill(0, 0, OW, OH, px(0xFFFFFFu));
        txt(60, 80, slide_line(g_slide, 0), 40, px(CO_TEXT & 0xFFFFFFu));
        for (int l = 1; l < SLIDE_LINES; l++) {
            const char* s = slide_line(g_slide, l);
            if (!s[0]) continue;
            txt(60, 170 + (l - 1) * 48, s, 24, px(0x333333u));
        }
        txt(OW - 180, OH - 36, "点击/空格 翻页  ESC 返回", 14, px(CO_DIMTXT));
    } else {
        txt(20, BODY_Y, "演示（3 页）：F5 放映，左右键切换", 15, px(CO_DIMTXT));
        for (int s = 0; s < SLIDES; s++) {
            const int x = 20 + s * 286;
            const int y = BODY_Y + 40;
            fill(x, y, 266, 200, px(s == g_slide ? (CO_CURSOR & 0xFFFFFFu) : 0xF6F8FBu));
            txt(x + 10, y + 8, slide_line(s, 0), 18, px(CO_TEXT & 0xFFFFFFu));
            txt(x + 10, y + 44, slide_line(s, 1), 13, px(0x444444u));
            txt(x + 10, y + 68, slide_line(s, 2), 13, px(0x444444u));
        }
    }
    fill(0, STATUS_Y, OW, OH - STATUS_Y, px(CO_CHROME & 0xFFFFFFu));
    {
        char st[80], n[8];
        i64_str(g_slide + 1, n);
        str_copy(st, big ? "放映  第 " : "演示  第 ", 80);
        int k = str_len(st);
        for (int i = 0; n[i]; i++) st[k++] = n[i];
        const char* b = big ? " / 3   空格/点击 翻页   ESC 返回" : " / 3   左右键切换   F5 放映";
        for (int i = 0; b[i]; i++) st[k++] = b[i];
        st[k] = 0;
        txt(12, STATUS_Y + 4, st, 13, px(CO_DIMTXT));
    }
    printf("[OFFICE] render mode=%s lines=%d scroll=0\n", big ? "play" : "slides", SLIDE_LINES);
}

static void render(void) {
    fill(0, 0, OW, OH, px(CO_BG & 0xFFFFFFu));
    render_tabs();
    switch (g_mode) {
        case M_DOC:    render_doc(); break;
        case M_SHEET:  render_sheet(); break;
        case M_SLIDES:
        case M_PLAY:   render_slides(); break;
        default: break;
    }
    submit();
    g_dirty = 0;
}

/* ==================== 保存 ==================== */
static void save_current(void) {
    if (g_mode == M_SHEET) {
        /* 只有非空格才写；格式 "r c 文本\n" */
        static char buf[2048];
        int n = 0;
        for (int r = 0; r < CALC_ROWS; r++) {
            for (int c = 0; c < CALC_COLS; c++) {
                const char* e = cell_txt(r, c);
                if (!e[0]) continue;
                char a[8], b[8];
                i64_str(r, a); i64_str(c, b);
                for (int i = 0; a[i] && n < 2000; i++) buf[n++] = a[i];
                if (n < 2000) buf[n++] = ' ';
                for (int i = 0; b[i] && n < 2000; i++) buf[n++] = b[i];
                if (n < 2000) buf[n++] = ' ';
                for (int i = 0; e[i] && n < 2000; i++) buf[n++] = e[i];
                if (n < 2000) buf[n++] = '\n';
            }
        }
        const int rc = save_file(SHEET_PATH, buf, n);
        printf("[OFFICE] save mode=sheet path=%s bytes=%d rc=%d\n", SHEET_PATH, n, rc);
        return;
    }
    if (g_mode == M_SLIDES || g_mode == M_PLAY) {
        const int rc = save_file(DECK_PATH, g_slide_txt, SLIDES * SLIDE_LINES * SLIDE_TXT);
        printf("[OFFICE] save mode=slides path=%s bytes=%d rc=%d\n",
               DECK_PATH, SLIDES * SLIDE_LINES * SLIDE_TXT, rc);
        return;
    }
    const int rc = save_file(DOC_PATH, g_text, g_len);
    printf("[OFFICE] save mode=doc path=%s bytes=%d rc=%d\n", DOC_PATH, g_len, rc);
}

/* ==================== 输入 ==================== */
static void key_doc(int code) {
    const int line_before = g_cur_line, col_before = g_cur_col;
    if (code == (int)V64_KEY_UP) { if (g_cur_line > 0) { g_cur_line--; g_cur_col = doc_line_len(g_cur_line) < g_cur_col ? doc_line_len(g_cur_line) : g_cur_col; } }
    else if (code == (int)V64_KEY_DOWN) { if (g_cur_line + 1 < g_nlines) { g_cur_line++; g_cur_col = doc_line_len(g_cur_line) < g_cur_col ? doc_line_len(g_cur_line) : g_cur_col; } }
    else if (code == (int)V64_KEY_LEFT) { if (g_cur_col > 0) g_cur_col--; else if (g_cur_line > 0) { g_cur_line--; g_cur_col = doc_line_len(g_cur_line); } }
    else if (code == (int)V64_KEY_RIGHT) { if (g_cur_col < doc_line_len(g_cur_line)) g_cur_col++; else if (g_cur_line + 1 < g_nlines) { g_cur_line++; g_cur_col = 0; } }
    else if (code == (int)V64_KEY_PAGEDOWN) { g_cur_line += VIS_LINES; if (g_cur_line >= g_nlines) g_cur_line = g_nlines ? g_nlines - 1 : 0; }
    else if (code == (int)V64_KEY_PAGEUP) { g_cur_line -= VIS_LINES; if (g_cur_line < 0) g_cur_line = 0; }
    else if (code == 0x0D || code == 0x0A) doc_insert('\n');
    else if (code == 0x08 || code == 0x7F) doc_backspace();
    else if (code >= 0x20 && code < 0x7F) doc_insert((char)code);
    doc_clamp_cursor();
    /* 滚动跟随光标 */
    if (g_cur_line < g_scroll) g_scroll = g_cur_line;
    if (g_cur_line >= g_scroll + VIS_LINES) g_scroll = g_cur_line - VIS_LINES + 1;
    if (g_scroll < 0) g_scroll = 0;
    if (line_before != g_cur_line || col_before != g_cur_col) {
        printf("[OFFICE] key code=%d mode=0 line=%d col=%d\n", code, g_cur_line, g_cur_col);
        g_dirty = 1;
    } else if (code >= 0x20 || code == 0x0D || code == 0x08) {
        g_dirty = 1;
    }
}

static void key_sheet(int code) {
    if (code == (int)V64_KEY_UP)    { if (g_sr > 0) g_sr--; }
    else if (code == (int)V64_KEY_DOWN)  { if (g_sr + 1 < CALC_ROWS) g_sr++; }
    else if (code == (int)V64_KEY_LEFT)  { if (g_sc > 0) g_sc--; }
    else if (code == (int)V64_KEY_RIGHT) { if (g_sc + 1 < CALC_COLS) g_sc++; }
    else if (code == 0x09) { if (g_sc + 1 < CALC_COLS) g_sc++; else { g_sc = 0; if (g_sr + 1 < CALC_ROWS) g_sr++; } }
    else if (code == 0x0D) { if (g_sr + 1 < CALC_ROWS) g_sr++; }
    else if (code == 0x08) {
        char* e = cell_txt(g_sr, g_sc);
        const int n = str_len(e);
        if (n > 0) e[n - 1] = 0;
        sheet_flush_cache();
        g_dirty = 1;
    } else if (code >= 0x20 && code < 0x7F) {
        char* e = cell_txt(g_sr, g_sc);
        int n = str_len(e);
        if (n < CELL_TXT - 1) { e[n] = (char)code; e[n + 1] = 0; }
        sheet_flush_cache();
        int64_t v = 0;
        const int rc = sheet_value(g_sr, g_sc, &v);
        char nm[8], vs[32];
        (void)calc_ref_name(g_sr, g_sc, nm, sizeof(nm));
        if (rc != CALC_OK) str_copy(vs, sheet_err_text(rc), sizeof(vs));
        else i64_str(v, vs);
        printf("[OFFICE] sheet set cell=%s expr=%s value=%s err=%d\n", nm, e, vs, rc);
        g_dirty = 1;
    }
    printf("[OFFICE] key code=%d mode=1 line=%d col=%d\n", code, g_sr, g_sc);
    g_dirty = 1;
}

static void key_slides(int code) {
    if (code == (int)V64_KEY_LEFT)  { if (g_slide > 0) g_slide--; }
    else if (code == (int)V64_KEY_RIGHT) { if (g_slide + 1 < SLIDES) g_slide++; }
    else if (code == 0x20) { if (g_slide + 1 < SLIDES) g_slide++; }
    printf("[OFFICE] key code=%d mode=%d line=%d col=0\n", code, g_mode, g_slide);
    g_dirty = 1;
}

/* ==================== 主 ==================== */
int main(int argc, char** argv) {
    g_arena = (unsigned char*)mmap(0, ARENA_BYTES, 3, 0x22, -1, 0);
    if (!g_arena) { printf("[OFFICE] FAILED arena mmap\n"); return 1; }
    g_text      = (char*)(g_arena);
    g_cells     = (char*)(g_arena + 36 * 1024);
    g_vals      = (int64_t*)(g_arena + 36 * 1024 + CALC_ROWS * CALC_COLS * CELL_TXT);
    g_errs      = (int8_t*)(g_vals + CALC_ROWS * CALC_COLS);
    g_visiting  = g_errs + CALC_ROWS * CALC_COLS;
    g_cache     = (int64_t*)(g_arena + 56 * 1024);
    g_cached    = (int8_t*)(g_cache + CALC_ROWS * CALC_COLS);
    g_slide_txt = (char*)(g_arena + 64 * 1024);

    /* 启动参数：`run /bin/office [doc|sheet|slides]` */
    if (argc > 1 && argv && argv[1]) {
        if (argv[1][0] == 's' && argv[1][1] == 'h') g_mode = M_SHEET;
        else if (argv[1][0] == 's') g_mode = M_SLIDES;
        else g_mode = M_DOC;
    }

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
        printf("[OFFICE] fonts faces=%d ok=%d bytes=%d\n", g_font ? font64_face_count(g_font) : 0, ok, bytes);
    }

    g_shm  = shm_create(OW_BYTES);
    g_surf = wl_surface_create(OW, OH, V64_WL_FORMAT_XRGB8888);
    void* va = 0;
    const int mrc = shm_map(g_shm, 0, OW_BYTES, &va);
    printf("[OFFICE] win surf=%d w=%d h=%d shm=%d map=%d\n", g_surf, OW, OH, g_shm, mrc);
    if (g_surf <= 0 || g_shm <= 0 || mrc != 0 || !va) {
        printf("[OFFICE] done frames=0 reason=error\n");
        (void)fflush(0);
        return 1;
    }
    g_px = (uint32_t*)va;
    g_cv.px = g_px; g_cv.w = OW; g_cv.h = OH;

    /* 键盘焦点 + 指针捕获（与 /bin/browser 同一条：不申请就收不到任何事件） */
    { const int seat = wl_seat_get(); printf("[OFFICE] seat id=%d\n", seat); }

    doc_load();
    sheet_load();
    slides_load();
    render();

    int reason = 0;
    for (; g_frames < MAX_FRAMES; g_frames++) {
        struct Wl64SeatEvent ev[EV_MAX];
        const int got = wl_display_dispatch(FRAME_MS, ev, EV_MAX);
        for (int i = 0; i < got; i++) {
            const struct Ev64Event* e = &ev[i].ev;
            if (e->type == V64_EV_KEY_DOWN) {
                const int code = (int)e->code;
                const int ctrl = (e->mods & V64_EV_MOD_CTRL) != 0;
                if (code == (int)V64_KEY_ESC) {
                    if (g_mode == M_PLAY) { g_mode = M_SLIDES; g_dirty = 1; continue; }
                    if (g_mode != M_PLAY) save_current();   /* 退出前落盘（有改动也写，便于验收） */
                    reason = 1; break;
                }
                if (ctrl && (code == 's' || code == 'S')) { save_current(); continue; }
                if (code == 0xF9) {                       /* F2：循环切模式（内核只特判了 F2=0xF9） */
                    save_current();                       /* 切走前把当前模式落盘 */
                    g_mode = (g_mode + 1) % 3;
                    g_scroll = 0;
                    printf("[OFFICE] mode=%d\n", g_mode);
                    g_dirty = 1;
                    continue;
                }
                if (code == 0xF5) { g_mode = M_PLAY; g_dirty = 1; continue; }  /* F5 放映 */
                if (g_mode == M_DOC) key_doc(code);
                else if (g_mode == M_SHEET) key_sheet(code);
                else key_slides(code);
            } else if (e->type == V64_EV_MOUSE_DOWN && ev[i].inside && ev[i].sx >= 0) {
                const int mx = (int)ev[i].sx, my = (int)ev[i].sy;
                if (my < TAB_H) {                       /* 点标签切换 */
                    const int idx = (mx - 10) / 96;
                    if (idx >= 0 && idx <= 2) {
                        g_mode = idx;
                        printf("[OFFICE] mode=%d (click)\n", g_mode);
                        g_dirty = 1;
                    }
                } else if (g_mode == M_PLAY) {          /* 放映：点击翻页 */
                    if (g_slide + 1 < SLIDES) g_slide++;
                    g_dirty = 1;
                } else if (g_mode == M_SHEET && my < STATUS_Y) {
                    const int cw = (OW - 24) / CALC_COLS;
                    const int c = (mx - 12) / cw;
                    const int r = (my - BODY_Y - 18) / 22;
                    if (c >= 0 && c < CALC_COLS && r >= 0 && r < CALC_ROWS) {
                        g_sc = c; g_sr = r;
                        g_dirty = 1;
                    }
                }
            }
        }
        if (reason) break;
        if (g_dirty) render();
    }
    if (!reason) reason = 2;
    printf("[OFFICE] done frames=%d reason=%s\n", g_frames, (reason == 1) ? "esc" : "timeout");
    (void)wl_surface_destroy(g_surf);
    (void)fflush(0);
    (void)argc; (void)argv;
    return 0;
}
