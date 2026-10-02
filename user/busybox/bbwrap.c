/* bbwrap.c - VimtuOS64：/bin/<applet> 包装程序（卷里每个 applet 名一份**同样的字节**）
 *
 * 为什么需要它（卷里没有 symlink）：
 *   busybox 的多入口靠 argv[0] 的 basename 选 applet。内核的 execve 原样搬运 argv，
 *   所以只要"把进程换成 /bin/busybox"、别的什么都不动，busybox 就会看到
 *       argv[0] = "/bin/ls"  -> basename "ls" -> 跑 ls
 *   于是同一个 < 2 KiB 的包装程序**拷成多少份就多出多少条命令**（/bin/ls、/bin/cp …）。
 *
 *   （另一条更省事的路是"把驱动本体拷成 20 份"，那样每份 ~17 KiB；本方案每份 ~2 KiB。）
 *
 * 本文件不引任何 libc：execve(59) 直接内联汇编发。失败一律打 fd 2 + exit_group(127)，
 * 绝不静默继续（静默继续的话用户会看到"命令什么也没干"）。
 */
typedef unsigned long long u64;
typedef long long i64;

#define NR_WRITE  1
#define NR_EXECVE 59
#define NR_EXITG  231
#define BB_PATH   "/bin/busybox"

static i64 sc3(long nr, i64 a1, i64 a2, i64 a3) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}
static i64 sc1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}

void wrap_main(u64* sp) {
    const long argc = (long)sp[0];
    char** argv = (char**)&sp[1];
    char** envp = (char**)&sp[1 + argc + 1];      /* 内核摆的 [argc+1] 就是 envp 终止 NULL */

    const i64 r = sc3(NR_EXECVE, (i64)(unsigned long)(const char*)BB_PATH, (i64)(unsigned long)argv,
                      (i64)(unsigned long)envp);
    /* 走不到这里除非 execve 失败（Linux 语义：失败返回 -errno，成功不返回） */
    {
        char buf[96];
        const char* p = "[BBWRAP] FAIL execve /bin/busybox rc=";
        int n = 0;
        while (p[n]) n++;
        u64 v = (u64)(r < 0 ? -r : r);
        char t[24]; int k = 0;
        if (!v) t[k++] = '0';
        while (v) { t[k++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
        int i = 0;
        while (p[i]) { buf[i] = p[i]; i++; }
        while (k) buf[i++] = t[--k];
        buf[i++] = '\n';
        (void)sc3(NR_WRITE, 2, (i64)(unsigned long)buf, i);
        (void)sc1(NR_EXITG, 127);
    }
    for (;;) { }
}
