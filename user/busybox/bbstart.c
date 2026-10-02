/* bbstart.c - VimtuOS64：把初始栈交给 musl，再跑 busybox 的 main()
 *
 * 参数口径与 musl 的 crt1.o 一致：__libc_start_main(main, argc, argv, init, fini, ldso)，
 * musl 1.2.5 里后三个参数没有被使用，传 0 即可（同 user/make/make_start.c）。
 * busybox 的 main 是 int main(int argc, char **argv)（applets/appletlib.c），多出来的
 * 第三个参数被忽略 —— 与 libc 直接调 main 完全一样。
 */
typedef unsigned long u64;

extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                             void (*init)(void), void (*fini)(void), void (*ldso)(void));
extern int main(int argc, char** argv);

void bb_vimtu_start(u64* sp) {
    const int argc = (int)sp[0];
    char** argv = (char**)&sp[1];
    __libc_start_main((int (*)(int, char**, char**))(void*)main, argc, argv, 0, 0, 0);
    for (;;) { }                       /* 不该回来 */
}
