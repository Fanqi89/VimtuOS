/* tar_start.c - ★ B5：把初始栈交给 musl，再跑 tar 的 main()
 *
 * 参数口径与 musl 的 crt1.o 一致：__libc_start_main(main, argc, argv, init, fini, ldso)，
 * 在 musl 1.2.5 里后三个参数没有被使用，传 0 即可（与 user/apps/tcc/tcc_start.c 同口径）。
 */
typedef unsigned long u64;

extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                             void (*init)(void), void (*fini)(void), void (*ldso)(void));
extern int main(int argc, char** argv);

void tar_vimtu_start(u64* sp) {
    const int argc = (int)sp[0];
    char** argv = (char**)&sp[1];
    __libc_start_main((int (*)(int, char**, char**))(void*)main, argc, argv, 0, 0, 0);
    for (;;) { }                       /* 不该回来 */
}
