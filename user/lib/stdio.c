/* stdio.c - putchar / puts / printf（最小子集，无浮点）+ 行缓冲（Vimtu64 用户态 C 运行时）
 *
 * 支持：%s %c %d %i %u %x %X %p %%；长度修饰 l/ll/z（64 位）；宽度（十进制位数）；
 *       标志 '-'（左对齐）、'0'（数字左侧补 0）。
 * 不支持（**遇到就按原样打出去，不假装算过**）：%f/%e/%g（本内核与运行时不带 FP）、精度 "."、星号宽度 '*'。
 *
 * 缓冲：128 字节行缓冲（.bss），换行/缓冲满 -> write(1, ...)（write() 自己按 1024 分块）。
 * 每个 printf 调用只产生 **1 条（或很少几条）[SYSCALL] nr=1 打点**，串口日志干净、好 grep。 */
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#define V64_OUT_BUF 128u

static char     g_out64[V64_OUT_BUF];
static unsigned g_outn64;
static int      g_out_err64;      /* 1 = 至少一次 write 失败（printf 返回负值） */

void vimtu64_stdout_flush64(void) {
    if (g_outn64 == 0) return;
    if (write(STDOUT_FILENO, g_out64, g_outn64) < 0) g_out_err64 = 1;
    g_outn64 = 0;
}

static void out_ch(int c) {
    g_out64[g_outn64++] = (char)c;
    if (g_outn64 >= V64_OUT_BUF || c == '\n') vimtu64_stdout_flush64();
}

static void out_str(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) out_ch((unsigned char)s[i]);
}

/* 按宽度输出一段字符（pad = ' ' 或 '0'，left = 左对齐） */
static void out_padded(const char* s, size_t n, int width, int left, char pad) {
    int padn = width - (int)n;
    if (padn < 0) padn = 0;
    if (!left) while (padn-- > 0) out_ch(pad);
    out_str(s, n);
    if (left) while (padn-- > 0) out_ch(' ');
}

/* 无符号数 -> 字符串（base 10/16/8），返回长度；buf 至少 24 字节 */
static size_t out_num(unsigned long v, int base, int upper, char* buf) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    size_t n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = digits[v % (unsigned long)base]; v /= (unsigned long)base; }
    for (size_t i = 0; i < n; i++) buf[i] = tmp[n - 1 - i];
    buf[n] = 0;
    return n;
}

int vprintf(const char* fmt, va_list ap) {
    int written = 0;

    for (const char* p = fmt; *p; p++) {
        if (*p != '%') { out_ch((unsigned char)*p); written++; continue; }

        const char* start = p;
        p++;
        int left = 0;
        char pad = ' ';
        int zero_len_mod = 0;               /* 1 = 见过 l/ll/z（按 64 位读） */
        for (;; p++) {
            if (*p == '-') { left = 1; continue; }
            if (*p == '+' || *p == ' ' || *p == '#') continue;      /* 忽略：本子集不实现这些效果 */
            if (*p == '0') { pad = '0'; continue; }
            break;
        }
        int width = 0;
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }
        while (*p == 'l' || *p == 'z') { zero_len_mod = 1; p++; }

        char buf[24];
        size_t n = 0;
        int neg = 0;
        switch (*p) {
        case 'd':
        case 'i': {
            long v = zero_len_mod ? va_arg(ap, long) : (long)va_arg(ap, int);
            unsigned long uv;
            if (v < 0) { neg = 1; uv = (unsigned long)(-(v + 1)) + 1ul; } else { uv = (unsigned long)v; }
            n = out_num(uv, 10, 0, buf);
            if (neg) {                       /* '-' 占宽度：先把负号塞进缓冲 */
                size_t k = n;
                for (size_t i = k; i > 0; i--) buf[i] = buf[i - 1];
                buf[0] = '-';
                buf[k + 1] = 0;
                n = k + 1;
            }
            out_padded(buf, n, width, left, pad);
            written += (int)n;
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            const int base = (*p == 'u') ? 10 : ((*p == 'o') ? 8 : 16);
            const unsigned long v = zero_len_mod ? va_arg(ap, unsigned long)
                                                 : (unsigned long)va_arg(ap, unsigned int);
            n = out_num(v, base, (*p == 'X'), buf);
            out_padded(buf, n, width, left, pad);
            written += (int)n;
            break;
        }
        case 'p': {
            const unsigned long v = (unsigned long)(uintptr_t)va_arg(ap, void*);
            buf[0] = '0'; buf[1] = 'x';
            n = 2 + out_num(v, 16, 0, buf + 2);
            out_padded(buf, n, width, left, pad);
            written += (int)n;
            break;
        }
        case 'c': {
            const int ch = va_arg(ap, int);
            char c = (char)ch;
            out_padded(&c, 1, width, left, ' ');
            written++;
            break;
        }
        case 's': {
            const char* s = va_arg(ap, const char*);
            if (!s) s = "(null)";
            const size_t sn = strlen(s);
            out_padded(s, sn, width, left, ' ');
            written += (int)sn;
            break;
        }
        case '%':
            out_ch('%');
            written++;
            break;
        case 0:
            p = start;                        /* 格式串在 '%' 处结束：按字面把 '%' 打出去 */
            out_ch('%');
            written++;
            break;
        default:                              /* 不支持的转换：**如实**原样输出，别假装算过 */
            out_ch((unsigned char)*p);
            written++;
            break;
        }
    }
    return g_out_err64 ? -1 : written;
}

int printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int r = vprintf(fmt, ap);
    va_end(ap);
    vimtu64_stdout_flush64();                 /* 行缓冲：printf 返回前一定落盘（一致、好断言） */
    return r;
}

int putchar(int c) {
    out_ch((unsigned char)c);
    return (unsigned char)c;
}

int puts(const char* s) {
    if (!s) s = "(null)";
    out_str(s, strlen(s));
    out_ch('\n');
    return 0;
}

/* 只有 stdout（fd 1）。stream 参数是给调用方写法兼容用的，本运行时不看它。 */
int fflush(void* stream) {
    (void)stream;
    vimtu64_stdout_flush64();
    return g_out_err64 ? -1 : 0;
}
