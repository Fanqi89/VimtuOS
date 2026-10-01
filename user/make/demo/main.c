/* main.c - ★ B5：make 演示工程的入口（用 tcc 的极小 libc：只有 write/puts/strlen/memcpy…） */
extern long write(int fd, const void* buf, unsigned long n);
extern int  util_add(int a, int b);
extern int  util_mul(int a, int b);

static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void put_dec(long v) {
    char t[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) (void)write(1, "-", 1);
    if (u == 0) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (int)(u % 10u)); u /= 10u; }
    while (n) (void)write(1, &t[--n], 1);
}

int main(void) {
    const char* p = "vimtuos-make-demo: 6*7=";
    (void)write(1, p, slen(p));
    put_dec(util_mul(util_add(3, 3), 7));
    (void)write(1, "\n", 1);
    return 0;
}
