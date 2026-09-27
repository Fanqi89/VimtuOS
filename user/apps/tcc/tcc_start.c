/* tcc_start.c - ★ A4-2b：把初始栈交给 musl，再跑 tcc 的 main()
 *
 * 链在 tcc_start.S 后面（tcc 的 _start 只负责把初始 rsp 递进来）。
 * 参数口径与 musl 的 crt1.o 一致：__libc_start_main(main, argc, argv, init, fini, ldso)，
 * 在 musl 1.2.5 里后三个参数**没有被使用**（见 third_party/musl/src/env/__libc_start_main.c
 * 的 __init_libc/libc_start_init），所以传 0 即可 —— 这与 user/apps/muslhello.c 的做法一致。
 *
 * tcc 的 main 返回后 __libc_start_main 走 exit()，不会回到这里。
 */
typedef unsigned long u64;

extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                             void (*init)(void), void (*fini)(void), void (*ldso)(void));
extern int main(int argc, char** argv);

void tcc_vimtu_start(u64* sp) {
    const int argc = (int)sp[0];
    char** argv = (char**)&sp[1];
    __libc_start_main((int (*)(int, char**, char**))(void*)main, argc, argv, 0, 0, 0);
    for (;;) { }                       /* 不该回来 */
}
