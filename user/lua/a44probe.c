/* a44probe.c - ★ A4-4b：**两个新系统调用**（utime(132) 显式时间 / rename(82) 跨目录）的 ring3 探针
 *
 * 为什么单独写一个探针（而不是改 tests/a42a64_test.py 的那份）：
 *   a42a 那份探针（build64.sh 生成）是 A4-2a 的**既有验收**，它在断言"显式时间 = -ENOSYS、
 *   跨目录 = -ENOSYS"——那是 A4-2a 时点的如实行为。A4-4b 把这两条**实现**了，旧断言自然过期；
 *   而 tests/a42a64_test.py 不在本批可改范围。所以本批用这个自足探针取证（A4-4b 的两条语义：
 *   合法路径成功 + 非法路径错误码 + 目录移进自己子树 = EINVAL），由 tests/lua64_test.py 断言。
 *
 * 口径：与 a42a 探针一致 —— **syscall 指令 + Linux 号段**、自包含（不 include 任何头、不依赖 libc）、
 * 每行 `A44 <tag>=<value>`（value = 系统调用返回值，负数 = -errno）。
 */
typedef long i64;
typedef unsigned long u64;

static i64 sc3(i64 n, i64 a, i64 b, i64 c) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return r;
}
static i64 sc0(i64 n) { return sc3(n, 0, 0, 0); }
static i64 sc1(i64 n, i64 a) { return sc3(n, a, 0, 0); }
static i64 sc2(i64 n, i64 a, i64 b) { return sc3(n, a, b, 0); }

static void out_str(const char* s) {
    int n = 0;
    while (s[n]) n++;
    (void)sc3(1, 1, (i64)(u64)s, n);
}
static void out_num(i64 v) {
    char b[24];
    int n = 0;
    u64 x;
    if (v < 0) { b[n++] = '-'; x = (u64)(-v); } else { x = (u64)v; }
    char t[24];
    int k = 0;
    if (x == 0) t[k++] = '0';
    while (x) { t[k++] = (char)('0' + (int)(x % 10u)); x /= 10u; }
    while (k) b[n++] = t[--k];
    b[n] = 0;
    out_str(b);
}
static void ev(const char* tag, i64 v) { out_str("A44 "); out_str(tag); out_str("="); out_num(v); out_str("\n"); }

/* struct utimbuf { time_t actime; time_t modtime; } —— 两个 i64 */
static long g_times[2] = { 1700000000, 1700000000 };   /* 2023-11-14T22:13:20Z（UTC 口径） */
static long g_bad[2]   = { 0, 0 };                     /* 1970-01-01：超出 2000..2063 -> EINVAL */

__attribute__((section(".text.start"), noreturn)) void _start(void) {
    /* 准备素材（都在世界可写的 /tmp 下） */
    const i64 f1 = sc3(2, (i64)(u64)"/tmp/a44probe_a.txt", 0x241 /*O_WRONLY|O_CREAT|O_TRUNC*/, 0644);
    if (f1 >= 0) { (void)sc3(1, f1, (i64)(u64)"probe\n", 6); (void)sc1(3, f1); }

    /* ---- 132）utime：显式时间 ---- */
    ev("utime_null",    sc2(132, (i64)(u64)"/tmp/a44probe_a.txt", 0));                            /* 0 */
    ev("utime_explicit", sc2(132, (i64)(u64)"/tmp/a44probe_a.txt", (i64)(u64)g_times));           /* 0 */
    ev("utime_missing", sc2(132, (i64)(u64)"/tmp/a44probe_nope.txt", (i64)(u64)g_times));         /* -ENOENT */
    ev("utime_badtime", sc2(132, (i64)(u64)"/tmp/a44probe_a.txt", (i64)(u64)g_bad));              /* -EINVAL（超出可表示范围） */

    /* ---- 82）rename：同目录 / 跨目录 / 失败路径 ---- */
    ev("rename_same",   sc2(82, (i64)(u64)"/tmp/a44probe_a.txt", (i64)(u64)"/tmp/a44probe_b.txt")); /* 0 */
    ev("rename_cross",  sc2(82, (i64)(u64)"/tmp/a44probe_b.txt", (i64)(u64)"/tmp/sub/a44probe_b.txt")); /* 0（跨目录移动） */
    ev("rename_back",   sc2(82, (i64)(u64)"/tmp/sub/a44probe_b.txt", (i64)(u64)"/tmp/a44probe_c.txt")); /* 0（移回 /tmp） */
    ev("rename_missing", sc2(82, (i64)(u64)"/tmp/a44probe_nope.txt", (i64)(u64)"/tmp/a44probe_x.txt")); /* -ENOENT */
    /* 目标已存在：先造一个目标 */
    const i64 f2 = sc3(2, (i64)(u64)"/tmp/a44probe_target.txt", 0x241, 0644);
    if (f2 >= 0) (void)sc1(3, f2);
    ev("rename_exists", sc2(82, (i64)(u64)"/tmp/a44probe_c.txt", (i64)(u64)"/tmp/a44probe_target.txt")); /* -EEXIST */
    /* 目标父目录是文件：/tmp/a44probe_target.txt/xx -> -ENOTDIR（syscall 层统一给 -ENOENT，见下注释） */
    ev("rename_parent_file", sc2(82, (i64)(u64)"/tmp/a44probe_c.txt", (i64)(u64)"/tmp/a44probe_target.txt/xx")); /* -ENOENT */
    /* 只读/无权限目标目录（/proc 不存在 -> ENOENT；这里用一个 root 属主的目录 /etc 试写 -> -EACCES） */
    ev("rename_perm_denied", sc2(82, (i64)(u64)"/tmp/a44probe_c.txt", (i64)(u64)"/etc/a44probe.txt")); /* -EACCES */

    /* ---- 目录移进自己的子树：EINVAL（-22） ---- */
    (void)sc1(83, (i64)(u64)"/tmp/sub/a44probe_dir");                 /* mkdir */
    ev("rename_dir_into_self", sc2(82, (i64)(u64)"/tmp/sub/a44probe_dir", (i64)(u64)"/tmp/sub/a44probe_dir/inner"));
    ev("rmdir_dir", sc1(84, (i64)(u64)"/tmp/sub/a44probe_dir"));

    out_str("A44 done=0\n");
    (void)sc1(60, 0);                                                  /* exit(0) */
    for (;;) { }
}
