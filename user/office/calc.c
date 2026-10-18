/* calc.c - 表格公式引擎（递归下降，整数运算，零分配，零全局状态）
 *
 * 语法（见 calc.h）：
 *   expr    := term (('+'|'-') term)*
 *   term    := factor (('*'|'/') factor)*
 *   factor  := ['-'|'+'] primary
 *   primary := NUMBER | REF | 'SUM(' range ')' | '(' expr ')'
 *
 * 所有状态都在栈上：没有 malloc，没有可变全局数组（本仓库带
 * -fno-zero-initialized-in-bss，全零静态数组会进 .data 撑爆装载区）。
 * 整数字面量严格是十进制 0..INT64_MAX；. 和 e 一律当语法错。
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "calc.h"

#define CALC_MAX_DEPTH 32   /* 括号/递归深度上限 */
#define CALC_MAX_CELLS 1024 /* SUM 区域格数上限 */

/* 解析上下文：游标 + 单元格回调（全部放栈上，约 24 字节） */
typedef struct {
    const char* p;
    calc_cell_fn get;
    void* user;
} calc_ctx;

static int calc_ws(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

static int calc_digit(char ch)
{
    return ch >= '0' && ch <= '9';
}

static int calc_alpha(char ch)
{
    return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z');
}

static int calc_lower(char ch)
{
    return (ch >= 'A' && ch <= 'Z') ? ch + 32 : ch;
}

static void calc_skip(calc_ctx* c)
{
    while (calc_ws(*c->p))
        c->p++;
}

/* 单格取值：回调错误码原样返回（含 CALC_ECYCLE / CALC_EREF） */
static int calc_fetch(const calc_ctx* c, int row, int col, int64_t* out)
{
    if (!c->get)
        return CALC_EREF;
    return c->get(row, col, c->user, out);
}

/* 十进制整数字面量（无符号累积再判 INT64_MAX）；'.'/'e' 视为语法错 */
static int calc_number(calc_ctx* c, int64_t* out)
{
    const char* p = c->p;
    uint64_t v = 0;

    if (!calc_digit(*p))
        return CALC_ESYNTAX;
    while (calc_digit(*p)) {
        if (v > (uint64_t)INT64_MAX / 10u)
            return CALC_EOVER;      /* 先判，避免 v*10 在 uint64 里回绕 */
        v = v * 10u + (uint64_t)(*p - '0');
        if (v > (uint64_t)INT64_MAX)
            return CALC_EOVER;
        p++;
    }
    if (*p == '.' || *p == 'e' || *p == 'E')
        return CALC_ESYNTAX;
    c->p = p;
    *out = (int64_t)v;
    return CALC_OK;
}

/* 表达式里的引用：[A-Za-z][1-9][0-9]?，列 A..H、行 1..20；越界/不认识 -> EREF */
static int calc_ref(calc_ctx* c, int* prow, int* pcol)
{
    const char* p = c->p;
    int col, row;

    if (*p >= 'A' && *p <= 'H')
        col = *p - 'A';
    else if (*p >= 'a' && *p <= 'h')
        col = *p - 'a';
    else if (calc_alpha(*p))
        return CALC_EREF;           /* Z1 之类：列越界 */
    else
        return CALC_ESYNTAX;
    p++;
    if (*p < '1' || *p > '9')
        return CALC_EREF;           /* 没有行号，或行号从 0 起 */
    row = *p - '0';
    p++;
    if (calc_digit(*p)) {
        row = row * 10 + (*p - '0');
        p++;
        if (calc_digit(*p))
            return CALC_EREF;       /* 行号 3 位以上 */
    }
    if (row < 1 || row > CALC_ROWS)
        return CALC_EREF;
    c->p = p;
    *prow = row - 1;
    *pcol = col;
    return CALC_OK;
}

static int calc_expr(calc_ctx* c, int depth, int64_t* out);
static int calc_primary(calc_ctx* c, int depth, int64_t* out);

/* SUM( range )，range := REF [':' REF]；单格等价于那格的值 */
static int calc_sum(calc_ctx* c, int64_t* out)
{
    int r1, c1, r2, c2, r, col, rc, rows, cols;
    int64_t acc, v;

    calc_skip(c);
    if (!calc_alpha(*c->p))
        return CALC_ESYNTAX;        /* SUM() / SUM( 之后没东西 */
    rc = calc_ref(c, &r1, &c1);
    if (rc)
        return rc;
    r2 = r1;
    c2 = c1;
    calc_skip(c);
    if (*c->p == ':') {
        c->p++;
        calc_skip(c);
        if (!calc_alpha(*c->p))
            return CALC_ESYNTAX;
        rc = calc_ref(c, &r2, &c2);
        if (rc)
            return rc;
        calc_skip(c);
    }
    if (*c->p != ')')
        return CALC_ESYNTAX;
    c->p++;

    if (r1 > r2) {
        r = r1; r1 = r2; r2 = r;
    }
    if (c1 > c2) {
        col = c1; c1 = c2; c2 = col;
    }
    rows = r2 - r1 + 1;
    cols = c2 - c1 + 1;
    if ((int64_t)rows * (int64_t)cols > CALC_MAX_CELLS)
        return CALC_EOVER;

    acc = 0;
    for (r = r1; r <= r2; r++) {
        for (col = c1; col <= c2; col++) {
            rc = calc_fetch(c, r, col, &v);
            if (rc)
                return rc;
            if (__builtin_add_overflow(acc, v, &acc))
                return CALC_EOVER;
        }
    }
    *out = acc;
    return CALC_OK;
}

static int calc_primary(calc_ctx* c, int depth, int64_t* out)
{
    const char* q;
    char ch;
    int rc, row, col;
    int64_t v;

    calc_skip(c);
    ch = *c->p;
    if (ch == '(') {
        if (depth >= CALC_MAX_DEPTH)
            return CALC_EOVER;
        c->p++;
        rc = calc_expr(c, depth + 1, out);
        if (rc)
            return rc;
        calc_skip(c);
        if (*c->p != ')')
            return CALC_ESYNTAX;
        c->p++;
        return CALC_OK;
    }
    if (calc_digit(ch))
        return calc_number(c, out);
    if (!calc_alpha(ch))
        return CALC_ESYNTAX;

    /* 字母开头：要么是 SUM(，要么是单元格引用 */
    q = c->p;
    while (calc_alpha(*q))
        q++;
    if (q - c->p == 3 && calc_lower(c->p[0]) == 's' && calc_lower(c->p[1]) == 'u' &&
        calc_lower(c->p[2]) == 'm') {
        c->p = q;
        calc_skip(c);
        if (*c->p != '(')
            return CALC_ESYNTAX;
        c->p++;
        return calc_sum(c, out);
    }
    rc = calc_ref(c, &row, &col);
    if (rc)
        return rc;
    rc = calc_fetch(c, row, col, &v);
    if (rc)
        return rc;
    *out = v;
    return CALC_OK;
}

static int calc_factor(calc_ctx* c, int depth, int64_t* out)
{
    int neg = 0, rc;

    calc_skip(c);
    for (;;) {
        if (*c->p == '-') {
            neg = !neg;
        } else if (*c->p != '+') {
            break;
        }
        c->p++;
        calc_skip(c);
    }
    rc = calc_primary(c, depth, out);
    if (rc)
        return rc;
    if (neg) {
        if (*out == INT64_MIN)
            return CALC_EOVER;
        *out = -*out;
    }
    return CALC_OK;
}

static int calc_term(calc_ctx* c, int depth, int64_t* out)
{
    int64_t acc, rhs;
    int rc;
    char op;

    rc = calc_factor(c, depth, &acc);
    if (rc)
        return rc;
    for (;;) {
        calc_skip(c);
        op = *c->p;
        if (op != '*' && op != '/')
            break;
        c->p++;
        rc = calc_factor(c, depth, &rhs);
        if (rc)
            return rc;
        if (op == '*') {
            if (__builtin_mul_overflow(acc, rhs, &acc))
                return CALC_EOVER;
        } else {
            if (rhs == 0)
                return CALC_EDIV0;
            if (acc == INT64_MIN && rhs == -1)
                return CALC_EOVER;
            acc /= rhs;             /* C 的整数除法：向零截断 */
        }
    }
    *out = acc;
    return CALC_OK;
}

static int calc_expr(calc_ctx* c, int depth, int64_t* out)
{
    int64_t acc, rhs;
    int rc;
    char op;

    rc = calc_term(c, depth, &acc);
    if (rc)
        return rc;
    for (;;) {
        calc_skip(c);
        op = *c->p;
        if (op != '+' && op != '-')
            break;
        c->p++;
        rc = calc_term(c, depth, &rhs);
        if (rc)
            return rc;
        if (op == '+') {
            if (__builtin_add_overflow(acc, rhs, &acc))
                return CALC_EOVER;
        } else {
            if (__builtin_sub_overflow(acc, rhs, &acc))
                return CALC_EOVER;
        }
    }
    *out = acc;
    return CALC_OK;
}

int calc_eval(const char* expr, calc_cell_fn get, void* user, int64_t* out)
{
    calc_ctx c;
    int64_t v = 0;
    int rc;

    if (!expr)
        return CALC_ESYNTAX;
    c.p = expr;
    c.get = get;
    c.user = user;
    calc_skip(&c);
    if (*c.p == '=')
        c.p++;
    calc_skip(&c);
    if (*c.p == '\0')
        return CALC_ESYNTAX;    /* "=" 或 "" */
    rc = calc_expr(&c, 0, &v);
    if (rc)
        return rc;
    calc_skip(&c);
    if (*c.p != '\0')
        return CALC_ESYNTAX;    /* 还有剩余字符：1.5、1e3、A1 A2 ... */
    if (out)
        *out = v;
    return CALC_OK;
}

int calc_parse_ref(const char* s, int len, int* row, int* col)
{
    int i, n, r = 0, c;
    char ch;

    if (!s || len <= 0)
        return 0;
    ch = s[0];
    if (ch >= 'A' && ch <= 'H')
        c = ch - 'A';
    else if (ch >= 'a' && ch <= 'h')
        c = ch - 'a';
    else
        return 0;               /* 不是单字母列 A..H */
    i = 1;
    for (n = 0; i < len && n < 2 && calc_digit(s[i]); n++, i++)
        r = r * 10 + (s[i] - '0');
    if (n == 0)
        return 0;               /* A / A0 之外的无行号 */
    if (i < len && calc_digit(s[i]))
        return 0;               /* 超过 2 位数字 */
    while (i < len && calc_ws(s[i]))
        i++;
    if (i < len && s[i] != '\0')
        return 0;               /* 多余字符 */
    if (r < 1 || r > CALC_ROWS)
        return 0;               /* A0 / A21 越界 */
    if (row)
        *row = r - 1;
    if (col)
        *col = c;
    return 1;
}

int calc_ref_name(int row, int col, char* out, int cap)
{
    char buf[4];
    int n, r;

    if (!out || cap < 4)
        return 0;
    if (row < 0 || row >= CALC_ROWS || col < 0 || col >= CALC_COLS)
        return 0;
    r = row + 1;                 /* 行号是 1 基，1..20 */
    buf[0] = (char)('A' + col);
    if (r < 10) {
        buf[1] = (char)('0' + r);
        n = 2;
    } else {
        buf[1] = (char)('0' + r / 10);
        buf[2] = (char)('0' + r % 10);
        n = 3;
    }
    buf[n] = '\0';
    memcpy(out, buf, (size_t)n + 1u);
    return n;
}
