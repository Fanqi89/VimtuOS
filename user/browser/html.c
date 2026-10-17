/* html.c - VimtuOS Browser 的 HTML 子集解析器（纯用户态、零依赖、不 malloc）
 *
 * 语义（与 user/browser/html.h 的约定一一对应）：
 *   * 单遍扫描：'<' 进标签态、'>' 出；其余字节是文本；标签名大小写不敏感。
 *   * 整段跳过：<script>…</script>、<style>…</style>、<!-- … -->。
 *   * 块级切分：p/div/h1/h2/h3/pre/li/hr/body/head（title 单走标题态）；
 *     其余标签（span/b/i/em/…）不切块，只改"当前块"的属性。
 *   * 实体解码、空白折叠、常见属性（align/style 里的 color/background-color/
 *     text-align/font-size/font-weight）、<a href> 的链接色与下划线。
 *
 * 纪律（本仓库的硬约束，改代码前先读）：
 *   * 不用 malloc：只用调用方给的 textpool / blocks 两块缓冲。
 *   * 没有全局可变数组（-fno-zero-initialized-in-bss 会把全零静态数组塞进 .data）；
 *     解析状态全在 html_parse 的栈帧里（struct HP ≈ 120 B，没有大数组）。
 *   * 用户栈 16 KiB：所有局部变量都是标量，最大的是 parse_color 的 int d[6]。
 *
 * 已知的"简化"（有意为之，不是漏做）：
 *   1. 样式只做到"块"这一粒度：<b>/<a> 影响所在块与后继块，不做字符级 run；
 *      因此 <p>前<b>中</b>后</p> 整块都是粗体。
 *   2. 内联样式（style=）没有出栈：<span style="color:#f00"> 的颜色会一直留到
 *      下一个块级标签（块级标签会重置块内样式状态）。只有 <a>/<b>/<u>/<center>
 *      这些"语义标签"按嵌套计数正确回退。
 *   3. style="color:#000" 与"未设颜色"都落到 color = 0（头文件约定 0 = 主题默认），
 *      无法区分纯黑。background-color 同理。
 *   4. 未知标签（table/form/noscript/…）只按属性影响当前块，不产生语义；
 *      扫不到 '>' 的 '<' 退回成普通文本；<!DOCTYPE …>/<!CDATA[…]>/<?xml …> 整段忽略。
 *   5. </title> 处会在池里补一个 '\0'（计入 text_used，不计入 title_len），
 *      这样 doc->textpool + doc->title_off 可以直接当 C 字符串用（browser.c 依赖它）。
 */

#include <stdint.h>
#include <stddef.h>

#include "html.h"

#define H_LINK_COLOR 0x1E6FD9u
#define H_DEF_SIZE   16
#define H_POOL_KEEP  8   /* 池尾必须留出来的字节数（NUL + 余量） */

/* 解析器状态（只在 html_parse 的栈上活一次） */
struct HP {
    /* 调用方的缓冲 */
    char*          pool;
    int            cap;      /* textcap */
    int            used;
    struct HBlock* blocks;
    int            maxb;
    int            nb;
    struct HDoc*   doc;

    /* 当前块 */
    int cur;            /* 块下标；-1 = 当前没有块 */
    int cur_has_text;   /* 当前块是否已经写过正文（决定块首空格丢不丢） */
    int pend_space;     /* 待发的折叠空格 */

    /* 模式 */
    int pre;            /* <pre> 里：不折叠空白 */
    int in_title;       /* <title> 里：文本进 title_off/title_len */
    int title_has_text;
    int title_pend_space;

    /* 内联语义标签的嵌套计数（正确回退） */
    int bold_depth;
    int a_depth;
    int u_depth;
    int center_hold;

    /* 块内样式（遇到块级标签重置） */
    int st_color;   /* 0 = 未设 */
    int st_bg;      /* 0 = 未设 */
    int st_size;    /* 0 = 未设 */
    int st_bold;
    int tag_align;  /* -1 = 未设 */
};

/* 前向声明（写文本与开块互相调用） */
static int     push_block(struct HP* p, int type, int dsize, int dbold, int make_cur);
static void    close_block(struct HP* p);
static uint8_t eff_align(struct HP* p);
static int     put_byte(struct HP* p, unsigned char c);
static int     text_byte(struct HP* p, unsigned char c);

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static int h_is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int h_is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int h_low(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

/* s[0..n) 是否等于 lit（lit 以 NUL 结尾；大小写不敏感） */
static int ci_eq(const char* s, int n, const char* lit)
{
    int i;
    if (n <= 0) return 0;
    for (i = 0; i < n; i++) {
        if (lit[i] == '\0') return 0;
        if (h_low((unsigned char)s[i]) != h_low((unsigned char)lit[i])) return 0;
    }
    return lit[i] == '\0';
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* left/center/right（justify 退化成 left）；不认识返回 -1 */
static int parse_align(const char* v, int n)
{
    if (ci_eq(v, n, "left"))    return H_ALIGN_LEFT;
    if (ci_eq(v, n, "center"))  return H_ALIGN_CENTER;
    if (ci_eq(v, n, "right"))   return H_ALIGN_RIGHT;
    if (ci_eq(v, n, "justify")) return H_ALIGN_LEFT;
    return -1;
}

/* "#RRGGBB" / "#RGB" -> 0xRRGGBB */
static int parse_color(const char* v, int n, uint32_t* out)
{
    int i = 0, nd = 0, d[6];
    uint32_t r, g, b;
    while (i < n && h_is_ws((unsigned char)v[i])) i++;
    if (i >= n || v[i] != '#') return 0;
    i++;
    while (i < n && nd < 6) {
        int h = hexval((unsigned char)v[i]);
        if (h < 0) break;
        d[nd++] = h;
        i++;
    }
    while (i < n && h_is_ws((unsigned char)v[i])) i++;
    if (i < n) return 0;          /* 尾巴上还有别的东西（如 "!important"）→ 不认 */
    if (nd == 3) {
        r = (uint32_t)(d[0] * 17);
        g = (uint32_t)(d[1] * 17);
        b = (uint32_t)(d[2] * 17);
    } else if (nd == 6) {
        r = (uint32_t)(d[0] * 16 + d[1]);
        g = (uint32_t)(d[2] * 16 + d[3]);
        b = (uint32_t)(d[4] * 16 + d[5]);
    } else {
        return 0;
    }
    *out = (r << 16) | (g << 8) | b;
    return 1;
}

/* "NN" / "NNpx" -> NN；没有数字返回 0 */
static int parse_px(const char* v, int n)
{
    int i = 0, val = 0, nd = 0;
    while (i < n && h_is_ws((unsigned char)v[i])) i++;
    while (i < n && v[i] >= '0' && v[i] <= '9' && nd < 4) {
        val = val * 10 + (v[i] - '0');
        i++;
        nd++;
    }
    return nd ? val : 0;
}

/* 一个码位编码成 UTF-8；超出 BMP / 代理区 / 0 一律 U+FFFD */
static int utf8_put(unsigned int cp, unsigned char* o)
{
    if (cp == 0 || cp > 0xFFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) cp = 0xFFFDu;
    if (cp < 0x80u) {
        o[0] = (unsigned char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        o[0] = (unsigned char)(0xC0u | (cp >> 6));
        o[1] = (unsigned char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    o[0] = (unsigned char)(0xE0u | (cp >> 12));
    o[1] = (unsigned char)(0x80u | ((cp >> 6) & 0x3Fu));
    o[2] = (unsigned char)(0x80u | (cp & 0x3Fu));
    return 3;
}

/* 解码 s[0] == '&' 开头的实体。
 *   返回 1 = 命中了（*used 字节被吃掉，out/outn 是输出）；0 = 不认识（按普通 '&' 处理）。 */
static int dec_entity(const char* s, int n, int* used, unsigned char* out, int* outn)
{
    int e = 1, nl, i;
    unsigned int cp = 0;
    int ok = 0;
    const char* nm;

    *outn = 0;
    while (e < n && e <= 12 && s[e] != ';') e++;
    if (e >= n || s[e] != ';') return 0;
    nl = e - 1;
    nm = s + 1;

    if (nl == 2 && ci_eq(nm, 2, "lt"))        { out[0] = '<';  *outn = 1; }
    else if (nl == 2 && ci_eq(nm, 2, "gt"))   { out[0] = '>';  *outn = 1; }
    else if (nl == 3 && ci_eq(nm, 3, "amp"))  { out[0] = '&';  *outn = 1; }
    else if (nl == 4 && ci_eq(nm, 4, "quot")) { out[0] = '"';  *outn = 1; }
    else if (nl == 4 && ci_eq(nm, 4, "apos")) { out[0] = '\''; *outn = 1; }
    else if (nl == 4 && ci_eq(nm, 4, "nbsp")) { out[0] = ' ';  *outn = 1; }
    else if (nl >= 2 && nm[0] == '#') {
        if (nm[1] == 'x' || nm[1] == 'X') {
            i = 2;
            if (i < nl) {
                ok = 1;
                for (; i < nl; i++) {
                    int h = hexval((unsigned char)nm[i]);
                    if (h < 0) { ok = 0; break; }
                    cp = cp * 16u + (unsigned int)h;
                    if (cp > 0x10FFFFu) cp = 0x110000u;
                }
            }
        } else {
            i = 1;
            if (i < nl) {
                ok = 1;
                for (; i < nl; i++) {
                    if (nm[i] < '0' || nm[i] > '9') { ok = 0; break; }
                    cp = cp * 10u + (unsigned int)(nm[i] - '0');
                    if (cp > 0x10FFFFu) cp = 0x110000u;
                }
            }
        }
        if (!ok) return 0;
        *outn = utf8_put(cp, out);
    }

    if (*outn == 0) return 0;
    *used = e + 1;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 文本池 / 块                                                         */
/* ------------------------------------------------------------------ */

/* 池里还能不能再放 n 字节（尾部留 H_POOL_KEEP） */
static int room(struct HP* p, int n)
{
    int limit = p->cap - H_POOL_KEEP;
    if (limit < 0) return 0;
    return (p->used + n <= limit);
}

/* 链接位置记录：只认第一处 <a href> 的文本，且必须与上次记录的区间连续 */
static void link_note(struct HP* p, int before, int n)
{
    if (p->a_depth <= 0) return;
    if (p->doc->link_len == 0) {
        p->doc->link_off = (uint32_t)before;
        p->doc->link_len = (uint32_t)n;
    } else if ((long)p->doc->link_off + (long)p->doc->link_len == (long)before) {
        p->doc->link_len += (uint32_t)n;
    }
}

/* 往池里写 1 字节；in_title 时记给 title，否则记给当前块（调用方保证 cur >= 0） */
static int put_byte(struct HP* p, unsigned char c)
{
    int before;
    if (!room(p, 1)) return -2;
    before = p->used;
    p->pool[p->used] = (char)c;
    p->used++;
    if (p->in_title) {
        p->doc->title_len = p->used - p->doc->title_off;
        return 0;
    }
    if (p->cur >= 0) {
        p->blocks[p->cur].len++;
        link_note(p, before, 1);
    }
    return 0;
}

/* 写 1 字节正文；没有当前块就先开一个（<pre> 里开 H_PRE） */
static int text_byte(struct HP* p, unsigned char c)
{
    int r;
    if (p->in_title) return put_byte(p, c);
    if (p->cur < 0) {
        r = push_block(p, p->pre ? H_PRE : H_P, p->pre ? 14 : 0, 0, 1);
        if (r) return r;
    }
    return put_byte(p, c);
}

static uint8_t eff_align(struct HP* p)
{
    if (p->tag_align >= 0) return (uint8_t)p->tag_align;
    if (p->center_hold) return H_ALIGN_CENTER;
    return H_ALIGN_LEFT;
}

/* 收尾当前块；空块（非 H_HR）直接丢弃 —— 于是 </p>、<p></p> 不会产生空块 */
static void close_block(struct HP* p)
{
    if (p->cur >= 0) {
        struct HBlock* b = &p->blocks[p->cur];
        if (b->type != H_HR && b->len == 0u && p->cur == p->nb - 1) p->nb--;
        p->cur = -1;
    }
    p->cur_has_text = 0;
    p->pend_space = 0;
}

/* 开新块（先把当前块收尾）。make_cur = 0 用于 <hr>：块产出但不接收后续文本。 */
static int push_block(struct HP* p, int type, int dsize, int dbold, int make_cur)
{
    struct HBlock* b;
    int size;
    close_block(p);
    if (p->nb >= p->maxb) return -1;
    size = p->st_size ? p->st_size : (dsize ? dsize : H_DEF_SIZE);
    b = &p->blocks[p->nb];
    b->off = (uint32_t)p->used;
    b->len = 0;
    b->type = (uint8_t)type;
    b->align = eff_align(p);
    b->bold = (uint8_t)(dbold || p->bold_depth > 0 || p->st_bold);
    b->underline = (uint8_t)(p->a_depth > 0 || p->u_depth > 0);
    b->size = (uint16_t)size;
    b->color = (uint32_t)(p->st_color ? p->st_color : (p->a_depth > 0 ? (int)H_LINK_COLOR : 0));
    b->bg = (uint32_t)p->st_bg;
    p->nb++;
    p->cur = make_cur ? (p->nb - 1) : -1;
    p->cur_has_text = 0;
    p->pend_space = 0;
    return 0;
}

/* 内联样式/语义标签变化后，把它同步到"当前已经开出来的块"上
 * （不然 <p><a href=x>链接</a></p> 的段落拿不到下划线） */
static void sync_style(struct HP* p)
{
    struct HBlock* b;
    if (p->cur < 0) return;
    b = &p->blocks[p->cur];
    if (p->bold_depth > 0 || p->st_bold) b->bold = 1;
    if (p->a_depth > 0 || p->u_depth > 0) b->underline = 1;
    if (p->st_color) b->color = (uint32_t)p->st_color;
    else if (p->a_depth > 0) b->color = (uint32_t)H_LINK_COLOR;
    if (p->st_bg) b->bg = (uint32_t)p->st_bg;
    if (p->st_size) b->size = (uint16_t)p->st_size;
    b->align = eff_align(p);
}

static void reset_block_style(struct HP* p)
{
    p->st_color = 0;
    p->st_bg = 0;
    p->st_size = 0;
    p->st_bold = 0;
    p->tag_align = -1;
}

/* ------------------------------------------------------------------ */
/* 文本喂入（空白折叠 / 实体直出）                                     */
/* ------------------------------------------------------------------ */

/* 普通文本字节：非 <pre> 折叠空白（块首空格丢弃） */
static int feed_char(struct HP* p, unsigned char c)
{
    int r;
    if (p->in_title) {
        if (h_is_ws((int)c)) {
            if (p->title_has_text) p->title_pend_space = 1;
            return 0;
        }
        if (p->title_pend_space) {
            p->title_pend_space = 0;
            if (p->title_has_text) {
                r = put_byte(p, (unsigned char)' ');
                if (r) return r;
            }
        }
        p->title_has_text = 1;
        return put_byte(p, c);
    }
    if (p->pre) {
        if (!h_is_ws((int)c)) p->cur_has_text = 1;
        return text_byte(p, c);
    }
    if (h_is_ws((int)c)) {
        if (p->cur_has_text) p->pend_space = 1;
        return 0;
    }
    if (p->pend_space) {
        p->pend_space = 0;
        if (p->cur_has_text) {
            r = text_byte(p, (unsigned char)' ');
            if (r) return r;
        }
    }
    p->cur_has_text = 1;
    return text_byte(p, c);
}

/* 实体解码出来的字节：不再参与空白折叠（&nbsp; 才真的能顶出一个空格） */
static int feed_lit(struct HP* p, unsigned char c)
{
    int r;
    if (p->in_title) {
        if (p->title_pend_space) {
            p->title_pend_space = 0;
            if (p->title_has_text) {
                r = put_byte(p, (unsigned char)' ');
                if (r) return r;
            }
        }
        p->title_has_text = 1;
        return put_byte(p, c);
    }
    if (p->pre) {
        p->cur_has_text = 1;
        return text_byte(p, c);
    }
    if (p->pend_space) {
        p->pend_space = 0;
        if (p->cur_has_text) {
            r = text_byte(p, (unsigned char)' ');
            if (r) return r;
        }
    }
    p->cur_has_text = 1;
    return text_byte(p, c);
}

/* ------------------------------------------------------------------ */
/* 标签                                                                */
/* ------------------------------------------------------------------ */

/* 块级标签表：返回 1 表示是块级标签，type/size/bold 是其默认值 */
static int block_tag(const char* name, int nl, int* type, int* size, int* bold)
{
    if (ci_eq(name, nl, "p") || ci_eq(name, nl, "div") ||
        ci_eq(name, nl, "body") || ci_eq(name, nl, "head")) {
        *type = H_P; *size = H_DEF_SIZE; *bold = 0; return 1;
    }
    if (ci_eq(name, nl, "h1")) { *type = H_H1; *size = 28; *bold = 1; return 1; }
    if (ci_eq(name, nl, "h2")) { *type = H_H2; *size = 24; *bold = 1; return 1; }
    if (ci_eq(name, nl, "h3")) { *type = H_H3; *size = 20; *bold = 1; return 1; }
    if (ci_eq(name, nl, "pre")) { *type = H_PRE; *size = 14; *bold = 0; return 1; }
    if (ci_eq(name, nl, "li")) { *type = H_LI; *size = H_DEF_SIZE; *bold = 0; return 1; }
    if (ci_eq(name, nl, "hr")) { *type = H_HR; *size = H_DEF_SIZE; *bold = 0; return 1; }
    return 0;
}

/* style="a:b;c:d" 里认那五条属性 */
static void parse_style(struct HP* p, const char* v, int n)
{
    int i = 0;
    while (i < n) {
        int ds = i, de, c, ps, pe, vs, ve;
        uint32_t col = 0;
        int a, sz;
        while (i < n && v[i] != ';') i++;
        de = i;
        if (i < n) i++;                       /* 吃掉 ';' */
        c = ds;
        while (c < de && v[c] != ':') c++;
        if (c >= de) continue;                /* 没有 ':' 的声明丢掉 */
        ps = ds; pe = c;
        vs = c + 1; ve = de;
        while (ps < pe && h_is_ws((unsigned char)v[ps])) ps++;
        while (pe > ps && h_is_ws((unsigned char)v[pe - 1])) pe--;
        while (vs < ve && h_is_ws((unsigned char)v[vs])) vs++;
        while (ve > vs && h_is_ws((unsigned char)v[ve - 1])) ve--;

        if (ci_eq(v + ps, pe - ps, "color")) {
            if (parse_color(v + vs, ve - vs, &col)) p->st_color = (int)col;
        } else if (ci_eq(v + ps, pe - ps, "background-color")) {
            if (parse_color(v + vs, ve - vs, &col)) p->st_bg = (int)col;
        } else if (ci_eq(v + ps, pe - ps, "text-align")) {
            a = parse_align(v + vs, ve - vs);
            if (a >= 0) p->tag_align = a;
        } else if (ci_eq(v + ps, pe - ps, "font-size")) {
            sz = parse_px(v + vs, ve - vs);
            if (sz >= 4 && sz <= 256) p->st_size = sz;
        } else if (ci_eq(v + ps, pe - ps, "font-weight")) {
            if (ci_eq(v + vs, ve - vs, "bold") || ci_eq(v + vs, ve - vs, "700") ||
                ci_eq(v + vs, ve - vs, "800") || ci_eq(v + vs, ve - vs, "900")) {
                p->st_bold = 1;
            }
        }
    }
}

/* 扫 tag[apos..tn) 上的属性；只认 align / href / style，其余跳过 */
static void parse_attrs(struct HP* p, const char* tag, int tn, int apos, int* has_href)
{
    int pos = apos;
    while (pos < tn) {
        int ns, nl, vs = 0, vl = 0, hasv = 0, save;
        while (pos < tn && (h_is_ws((unsigned char)tag[pos]) || tag[pos] == '/')) pos++;
        if (pos >= tn) break;
        ns = pos;
        while (pos < tn && !h_is_ws((unsigned char)tag[pos]) &&
               tag[pos] != '=' && tag[pos] != '/') pos++;
        nl = pos - ns;
        if (nl <= 0) { pos++; continue; }
        save = pos;
        while (pos < tn && h_is_ws((unsigned char)tag[pos])) pos++;
        if (pos < tn && tag[pos] == '=') {
            pos++;
            while (pos < tn && h_is_ws((unsigned char)tag[pos])) pos++;
            if (pos < tn && (tag[pos] == '"' || tag[pos] == '\'')) {
                char q = tag[pos];
                pos++;
                vs = pos;
                while (pos < tn && tag[pos] != q) pos++;
                vl = pos - vs;
                if (pos < tn) pos++;          /* 收尾引号 */
            } else {
                vs = pos;
                while (pos < tn && !h_is_ws((unsigned char)tag[pos])) pos++;
                vl = pos - vs;
            }
            hasv = 1;
        } else {
            pos = save;
        }
        if (!hasv) continue;

        if (ci_eq(tag + ns, nl, "align")) {
            int a = parse_align(tag + vs, vl);
            if (a >= 0) p->tag_align = a;
        } else if (ci_eq(tag + ns, nl, "href")) {
            *has_href = 1;
        } else if (ci_eq(tag + ns, nl, "style")) {
            parse_style(p, tag + vs, vl);
        }
    }
}

/* 找 </name ...>，返回 '<' 的下标；找不到返回 -1 */
static int find_close_tag(const char* src, int len, int from, const char* name)
{
    int nlen = 0, i;
    while (name[nlen] != '\0') nlen++;
    for (i = from; i + 2 + nlen <= len; i++) {
        if (src[i] != '<' || src[i + 1] != '/') continue;
        if (!ci_eq(src + i + 2, nlen, name)) continue;
        if (i + 2 + nlen == len) return i;
        if (h_is_ws((unsigned char)src[i + 2 + nlen]) || src[i + 2 + nlen] == '>') return i;
    }
    return -1;
}

/* <br>：当前块里追加 '\n'（没有块就先开 H_P） */
static int do_br(struct HP* p)
{
    int r;
    if (p->cur < 0) {
        r = push_block(p, p->pre ? H_PRE : H_P, p->pre ? 14 : 0, 0, 1);
        if (r) return r;
    }
    r = put_byte(p, (unsigned char)'\n');
    if (r) return r;
    p->cur_has_text = 0;   /* 换行后的前导空格照样丢 */
    p->pend_space = 0;
    return 0;
}

/* <li> 的 "• " 前缀（0xE2 0x80 0xA2 0x20），不算"已有正文"，故 li 后的前导空格仍被丢 */
static int li_prefix(struct HP* p)
{
    int r;
    r = put_byte(p, 0xE2u); if (r) return r;
    r = put_byte(p, 0x80u); if (r) return r;
    r = put_byte(p, 0xA2u); if (r) return r;
    return put_byte(p, 0x20u);
}

/* ------------------------------------------------------------------ */
/* 主入口                                                              */
/* ------------------------------------------------------------------ */

static int is_tag_open(const char* src, int len, int i)
{
    int c;
    if (i + 1 >= len) return 0;
    c = (unsigned char)src[i + 1];
    return h_is_alpha(c) || c == '/' || c == '!' || c == '?';
}

int html_parse(const char* src, int len, struct HDoc* doc, char* textpool, int textcap,
               struct HBlock* blocks, int maxblocks)
{
    struct HP st;
    struct HP* p = &st;
    int i = 0, rc = 0;

    if (doc == NULL || textpool == NULL || blocks == NULL || src == NULL) return -3;
    if (textcap <= 0 || maxblocks <= 0) return -3;
    if (len < 0) return -3;

    doc->nblocks = 0;
    doc->title_off = 0;
    doc->title_len = 0;
    doc->text_used = 0;
    doc->link_off = 0;
    doc->link_len = 0;
    doc->textpool = textpool;
    doc->blocks = blocks;

    p->pool = textpool;
    p->cap = textcap;
    p->used = 0;
    p->blocks = blocks;
    p->maxb = maxblocks;
    p->nb = 0;
    p->doc = doc;
    p->cur = -1;
    p->cur_has_text = 0;
    p->pend_space = 0;
    p->pre = 0;
    p->in_title = 0;
    p->title_has_text = 0;
    p->title_pend_space = 0;
    p->bold_depth = 0;
    p->a_depth = 0;
    p->u_depth = 0;
    p->center_hold = 0;
    reset_block_style(p);

    while (i < len && rc == 0) {
        const char* tag;
        int ts, te, tn, k, nl, ns, closing, apos;
        int type, dsize, dbold, isblk, has_href;

        if (src[i] != '<' || !is_tag_open(src, len, i)) {
            /* ---- 文本 / 实体 ---- */
            if (src[i] == '&') {
                int used = 0, outn = 0, j;
                unsigned char out[4];
                if (dec_entity(src + i, len - i, &used, out, &outn)) {
                    for (j = 0; j < outn && rc == 0; j++) rc = feed_lit(p, out[j]);
                    if (rc == 0) i += used;
                    continue;
                }
            }
            rc = feed_char(p, (unsigned char)src[i]);
            i++;
            continue;
        }

        /* ---- 注释 <!-- ... --> ---- */
        if (i + 3 < len && src[i + 1] == '!' && src[i + 2] == '-' && src[i + 3] == '-') {
            k = i + 4;
            while (k + 2 < len && !(src[k] == '-' && src[k + 1] == '-' && src[k + 2] == '>')) k++;
            i = (k + 2 < len) ? (k + 3) : len;
            continue;
        }

        /* ---- 扫标签本体（引号里的 '>' 不算结束） ---- */
        ts = i + 1;
        {
            int j = ts, q = 0;
            while (j < len) {
                char c = src[j];
                if (q) {
                    if (c == (char)q) q = 0;
                } else if (c == '"' || c == '\'') {
                    q = c;
                } else if (c == '>') {
                    break;
                }
                j++;
            }
            if (j >= len) {           /* 扫不到 '>'：把这个 '<' 当普通文本，别把后面吞掉 */
                rc = feed_char(p, (unsigned char)src[i]);
                i++;
                continue;
            }
            te = j;
            i = j + 1;                /* 文本从 '>' 之后继续 */
        }
        tag = src + ts;
        tn = te - ts;

        /* ---- 标签名 ---- */
        k = 0;
        while (k < tn && h_is_ws((unsigned char)tag[k])) k++;
        closing = 0;
        if (k < tn && tag[k] == '/') { closing = 1; k++; }
        ns = k;
        while (k < tn && !h_is_ws((unsigned char)tag[k]) &&
               tag[k] != '/' && tag[k] != '=') k++;
        nl = k - ns;
        apos = k;
        if (nl <= 0) continue;

        if (closing) {
            /* ---------------- 结束标签 ---------------- */
            if (ci_eq(tag + ns, nl, "title")) {
                if (p->in_title) {
                    p->in_title = 0;
                    p->title_pend_space = 0;
                    /* 给 title 补一个 '\0'：这样 textpool+title_off 就是 C 字符串 */
                    if (p->doc->title_len > 0 && room(p, 1)) p->pool[p->used++] = '\0';
                }
            } else if (ci_eq(tag + ns, nl, "pre")) {
                close_block(p);
                p->pre = 0;
            } else if (ci_eq(tag + ns, nl, "a")) {
                if (p->a_depth > 0) p->a_depth--;
            } else if (ci_eq(tag + ns, nl, "b") || ci_eq(tag + ns, nl, "strong")) {
                if (p->bold_depth > 0) p->bold_depth--;
            } else if (ci_eq(tag + ns, nl, "u")) {
                if (p->u_depth > 0) p->u_depth--;
            } else if (ci_eq(tag + ns, nl, "center")) {
                p->center_hold = 0;
            } else {
                /* </p> </div> </h1..3> </li> </body> </head> </hr> … */
                int t2 = 0, s2 = 0, b2 = 0;
                if (block_tag(tag + ns, nl, &t2, &s2, &b2)) close_block(p);
            }
            continue;
        }

        /* ---------------- 开始标签 ---------------- */
        if (ci_eq(tag + ns, nl, "script") || ci_eq(tag + ns, nl, "style")) {
            /* 整段跳过（大小写不敏感地找 </script> / </style>） */
            int from = i;
            int at = ci_eq(tag + ns, nl, "script")
                         ? find_close_tag(src, len, from, "script")
                         : find_close_tag(src, len, from, "style");
            if (at < 0) { i = len; break; }
            k = at;
            while (k < len && src[k] != '>') k++;
            i = (k < len) ? (k + 1) : len;
            continue;
        }

        if (ci_eq(tag + ns, nl, "title")) {
            /* 标题态：文本进 textpool 的一段，不进正文块 */
            close_block(p);
            p->in_title = 1;
            p->title_has_text = 0;
            p->title_pend_space = 0;
            p->doc->title_off = p->used;
            p->doc->title_len = 0;
            continue;
        }

        type = -1; dsize = 0; dbold = 0;
        isblk = block_tag(tag + ns, nl, &type, &dsize, &dbold);
        if (isblk) {
            reset_block_style(p);       /* 块级标签重置块内样式；属性在下面重新给 */
            if (type == H_PRE) p->pre = 1;
        }
        has_href = 0;
        parse_attrs(p, tag, tn, apos, &has_href);

        if (isblk) {
            rc = push_block(p, type, dsize, dbold, type != H_HR);
            if (rc == 0 && type == H_LI) rc = li_prefix(p);
        } else if (ci_eq(tag + ns, nl, "br")) {
            rc = do_br(p);
        } else {
            if (ci_eq(tag + ns, nl, "a")) {
                if (has_href) p->a_depth++;
            } else if (ci_eq(tag + ns, nl, "b") || ci_eq(tag + ns, nl, "strong")) {
                p->bold_depth++;
            } else if (ci_eq(tag + ns, nl, "u")) {
                p->u_depth++;
            } else if (ci_eq(tag + ns, nl, "center")) {
                p->center_hold = 1;
            }
            /* span / i / em / font / 其它一切：不切块，只让属性生效 */
            sync_style(p);
        }
    }

    close_block(p);

    if (p->used < p->cap) p->pool[p->used] = '\0';
    doc->nblocks = p->nb;
    doc->text_used = p->used;
    return rc;
}
