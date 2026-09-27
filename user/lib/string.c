/* string.c - 字符串/内存实现（Vimtu64 用户态 C 运行时）
 *
 * 全是最朴素的字节循环：**不用任何编译器内建**（构建带 -fno-builtin），也就不可能递归调用
 * 到自己；-O2 下循环会被展开成 inline 的 mov/rep movsb（见构建后 objdump 自查记录）。
 * 语义按 C 标准：memcpy 不处理重叠（重叠用 memmove），strncpy 补零到 n，strstr 找不到返回 NULL。 */
#include <string.h>

void* memcpy(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dst;
}

void* memmove(void* dst, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d == s || n == 0) return dst;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else {
        for (size_t i = n; i != 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}

void* memset(void* dst, int c, size_t n) {
    unsigned char* d = (unsigned char*)dst;
    for (size_t i = 0; i < n; i++) d[i] = (unsigned char)c;
    return dst;
}

int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* x = (const unsigned char*)a;
    const unsigned char* y = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return (int)x[i] - (int)y[i];
    }
    return 0;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int strcmp(const char* a, const char* b) {
    size_t i = 0;
    while (a[i] && a[i] == b[i]) i++;
    return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
}

int strncmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return (int)(unsigned char)a[i] - (int)(unsigned char)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}

char* strcpy(char* dst, const char* src) {
    size_t i = 0;
    for (; src[i]; i++) dst[i] = src[i];
    dst[i] = 0;
    return dst;
}

char* strncpy(char* dst, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) dst[i] = src[i];
    for (; i < n; i++) dst[i] = 0;             /* C 标准：不足 n 的部分补 0 */
    return dst;
}

char* strchr(const char* s, int c) {
    const char ch = (char)c;
    for (;; s++) {
        if (*s == ch) return (char*)s;
        if (*s == 0) return 0;                 /* 也覆盖 c == 0 的情况（返回指向结尾 NUL） */
    }
}

char* strstr(const char* hay, const char* needle) {
    if (!*needle) return (char*)hay;
    for (; *hay; hay++) {
        size_t i = 0;
        while (needle[i] && hay[i] == needle[i]) i++;
        if (!needle[i]) return (char*)hay;
    }
    return 0;
}
