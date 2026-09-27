/* tccmini.c - ★ A4-2b：给 **tcc 产物**用的极小 libc（不是给 tcc 自己用的）
 *
 * 为什么必须自己写一份（而不是把 musl 的 libc.a 放进卷里当 -lc）：
 *   tcc 的归档加载（tccelf.c:tcc_load_archive）**不做按符号惰性加载** —— 它把 .a 里**每一个**
 *   成员都读进来（`for(;;) { read(hdr); ... tcc_load_object_file() }`，没有"用到才装"的逻辑）。
 *   把 musl 的 2.3 MB libc.a（1300+ 个成员）交给它，链接出来的映像会是 MB 级 —— 而 VimtuOS
 *   的 ELF 装载区只有 64 KiB（kernel/usermode64.h），必然被内核拒绝。
 *   所以本平台的 "-lc" = 这一份**只包含演示程序真的用到的符号**的小库；系统头仍用 musl 的
 *   （编译期只需要声明，见 /tcc/include）。
 *
 * 这一份是被 `tools/tcc_build_win.sh` 用 clang **预先交叉编好**成 libc.a 放进系统卷的
 * （tcc 只负责链接，不负责编它 —— musl 自己也是这么分发 crt/libc 的）。
 *
 * 系统调用：VimtuOS 的 ring3 与 **Linux x86_64 号段**兼容（syscall 指令）：
 *   1 = write(fd, buf, n)   60 = exit(code)   231 = exit_group(code)
 * 都用内联汇编直接发（不引任何头文件，保持自足）。
 */

typedef unsigned long v64_size_t;

static inline long v64_sys1(long nr, long a) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a) : "rcx", "r11", "memory");
    return r;
}
static inline long v64_sys3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}

long write(int fd, const void* buf, v64_size_t n) {
    return v64_sys3(1, (long)fd, (long)buf, (long)n);
}
int puts(const char* s) {
    v64_size_t n = 0;
    while (s[n]) n++;
    (void)v64_sys3(1, 1, (long)s, (long)n);
    (void)v64_sys3(1, 1, (long)"\n", 1);
    return (int)n;
}
void _exit(int code) {
    v64_sys1(231, code);                       /* exit_group：本内核在 ring3 支持 */
    for (;;) { }                               /* 不该回来 */
}
void exit(int code) {
    _exit(code);
}

/* ★ 下面这几个是**编译器/编译器产物**都会自己发出来的调用（不是可选项）：
 *   * tcc 的代码生成对"结构体赋值/大块拷贝"直接发 memcpy、对大数组清零发 memset；
 *   * clang 在编本文件时也会把 `while (s[n]) n++` 这种循环识别成 strlen —— 实测：
 *     少了 strlen 时宿主 tcc 链接 hello.c 会报 `undefined symbol 'strlen'`（本文件自己的 puts）。
 * 所以把它们放进同一个 libc.a（tcc 的链接器只按符号决议，不需要单独的头）。 */
v64_size_t strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (v64_size_t)(p - s);
}
void* memcpy(void* d, const void* s, v64_size_t n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    for (v64_size_t i = 0; i < n; i++) dp[i] = sp[i];
    return d;
}
void* memmove(void* d, const void* s, v64_size_t n) {
    unsigned char* dp = (unsigned char*)d;
    const unsigned char* sp = (const unsigned char*)s;
    if (dp < sp) { for (v64_size_t i = 0; i < n; i++) dp[i] = sp[i]; }
    else         { for (v64_size_t i = n; i-- > 0; ) dp[i] = sp[i]; }
    return d;
}
void* memset(void* d, int c, v64_size_t n) {
    unsigned char* dp = (unsigned char*)d;
    for (v64_size_t i = 0; i < n; i++) dp[i] = (unsigned char)c;
    return d;
}
int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
