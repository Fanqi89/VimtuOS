/* vmtool.c - ★ v0.9.30：ACL / 配额 / 日志 / 快照 的 **ring-3 入口**（静态 ELF64，走内核主程序装载器）
 *
 * 为什么要有这个程序（一句话）：v0.9.28 把这四项做进了内核与 VimtuFS2 v5，但**没有系统调用、
 * 没有终端命令** —— 对用户而言等于不存在。这里是最小但真的用户态入口，全部走真系统调用。
 *
 * 号段（如实，绝不混口径）：
 *   * 188 setxattr / 191 getxattr —— **Linux x86_64 的真号**。
 *     ACL 的字节按 Linux 的 posix_acl_xattr 规范拼：`{u32 a_version = 2}` + N×8 字节条目
 *     `{u16 e_tag; u16 e_perm; u32 e_id}`；内核把 4 B 版本头剥掉后落盘（盘上仍是裸条目数组）。
 *   * 1024 snap_take / 1025 snap_mount / 1026 quota_set / 1027 quota_get / 1028 feature_enable
 *     —— **VimtuOS 私有号**（Linux 没有对应调用）。
 *
 * 用法（每个子命令都返回 0 = 成功 / 1 = 失败，便于脚本判定）：
 *   vmtool features                      看 /proc/vimtu/features
 *   vmtool journal                       看 /proc/vimtu/journal
 *   vmtool snapshots                     看 /proc/vimtu/snapshots
 *   vmtool quota-all                     看 /proc/vimtu/quota
 *   vmtool feature on <acl|quota|journal|snapshot|all>    开特性（root）
 *   vmtool acl grant <path> <uid> <perm3>   给 uid 授 rwx（写 Linux 规范 44 B，带版本头）
 *   vmtool acl deny  <path>                把属主/组/其他人全锁死（写 36 B，带版本头）
 *   vmtool acl get   <path>                读回 ACL 并逐项打印
 *   vmtool acl clear <path>                删掉 ACL（size=0）
 *   vmtool quota set <uid> <blocks|unlimited>
 *   vmtool quota get <uid>
 *   vmtool snap-take [want_blocks]         拍快照，打印新 id
 *   vmtool snap-mount <id>                 把快照挂成只读卷，打印卷槽号
 *   vmtool write <path> <bytes>            写文件（用来触发 ACL/配额拒绝，字节按 'A' 填充）
 *
 * 这一步刻意**不**做：listxattr（内核没有枚举接口，如实缺）、fsetxattr（fd 版）。
 */
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

#define NR_READ        0
#define NR_WRITE       1
#define NR_OPEN        2
#define NR_CLOSE       3
#define NR_SETXATTR    188
#define NR_GETXATTR    191
#define NR_EXITG       231
#define NR_SNAP_TAKE   1024
#define NR_SNAP_MOUNT  1025
#define NR_QUOTA_SET   1026
#define NR_QUOTA_GET   1027
#define NR_FEAT_ENABLE 1028

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

#define XA_VERSION      2u
#define XA_TAG_USER_OBJ 0x01u
#define XA_TAG_USER     0x02u
#define XA_TAG_GROUP_OBJ 0x04u
#define XA_TAG_GROUP    0x08u
#define XA_TAG_MASK     0x10u
#define XA_TAG_OTHER    0x20u
#define XA_UNDEF        0xFFFFFFFFu
#define ACL_NAME        "system.posix_acl_access"
#define VMT_FEAT_ALL    15u

static inline i64 sc1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc2(long nr, i64 a1, i64 a2) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc3(long nr, i64 a1, i64 a2, i64 a3) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc4(long nr, i64 a1, i64 a2, i64 a3, i64 a4) {
    register i64 r10 __asm__("r10") = a4;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
                     : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc5(long nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5) {
    register i64 r10 __asm__("r10") = a4;
    register i64 r8v __asm__("r8") = a5;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8v)
                     : "rcx", "r11", "memory");
    return r;
}

/* ---------------- 小工具（无 libc） ---------------- */
static u32 slen(const char* s) { u32 n = 0; while (s && s[n]) n++; return n; }
static int seq(const char* a, const char* b) {
    u32 i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == 0 && b[i] == 0;
}
static void out1(const char* s) { (void)sc3(NR_WRITE, 1, (i64)s, (i64)slen(s)); }
static void out1u(u64 v) {
    char t[24];
    int n = 0;
    if (!v) t[n++] = '0';
    while (v && n < 24) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    char o[24];
    int k = 0;
    while (n > 0) o[k++] = t[--n];
    o[k] = 0;
    out1(o);
}
static void out1x(u32 v) {
    const char* h = "0123456789abcdef";
    char o[11];
    o[0] = '0'; o[1] = 'x';
    for (int i = 0; i < 8; i++) o[2 + i] = h[(v >> (28 - 4 * i)) & 0xFu];
    o[10] = 0;
    out1(o);
}
static int parse_u(const char* s, u32* out) {
    if (!s || !s[0]) return 0;
    if (s[0] == 'u' && s[1] == 'n') { *out = 0xFFFFFFFFu; return 1; }   /* "unlimited" */
    u64 v = 0;
    for (u32 i = 0; s[i]; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
        v = v * 10u + (u64)(s[i] - '0');
        if (v > 0xFFFFFFFFull) return 0;
    }
    *out = (u32)v;
    return 1;
}
static i64 f_open(const char* p, i64 fl, i64 mode) { return sc3(NR_OPEN, (i64)p, fl, mode); }

/* 把一个 /proc 合成文件打到 stdout；返回 0/1 */
static int cat_file(const char* path) {
    const i64 fd = f_open(path, O_RDONLY, 0);
    if (fd < 0) {
        out1("vmtool: cannot open ");
        out1(path);
        out1(" (rc=");
        out1u((u64)(-fd));
        out1(")\n");
        return 1;
    }
    char b[512];
    for (;;) {
        const i64 n = sc3(NR_READ, fd, (i64)b, (i64)sizeof(b));
        if (n <= 0) break;
        (void)sc3(NR_WRITE, 1, (i64)b, n);
    }
    (void)sc1(NR_CLOSE, fd);
    return 0;
}

/* ---------------- ACL 字节拼装（Linux 规范：4 B 版本头 + 8 B/项） ---------------- */
static void put16(u8* p, u16 v) { p[0] = (u8)(v & 0xFFu); p[1] = (u8)((v >> 8) & 0xFFu); }
static void put32(u8* p, u32 v) {
    p[0] = (u8)(v & 0xFFu); p[1] = (u8)((v >> 8) & 0xFFu);
    p[2] = (u8)((v >> 16) & 0xFFu); p[3] = (u8)((v >> 24) & 0xFFu);
}
static u16 get16(const u8* p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static u32 get32(const u8* p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u32 acl_ent(u8* p, u16 tag, u16 perm, u32 id) {
    put16(p + 0, tag);
    put16(p + 2, perm);
    put32(p + 4, id);
    return 8;
}
/* grant：{ver=2} + USER_OBJ(7) + USER(uid,perm) + GROUP_OBJ(0) + MASK(perm) + OTHER(0) = 44 B */
static u32 acl_build_grant(u8* v, u32 uid, u32 perm3) {
    u32 n = 0;
    put32(v, XA_VERSION);
    n = 4;
    n += acl_ent(v + n, XA_TAG_USER_OBJ, 7u, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_USER, (u16)perm3, uid);
    n += acl_ent(v + n, XA_TAG_GROUP_OBJ, 0u, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_MASK, (u16)perm3, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_OTHER, 0u, XA_UNDEF);
    return n;
}
/* deny：{ver=2} + USER_OBJ(0) + GROUP_OBJ(0) + MASK(0) + OTHER(0) = 36 B（属主也锁死） */
static u32 acl_build_deny(u8* v) {
    u32 n = 4;
    put32(v, XA_VERSION);
    n += acl_ent(v + n, XA_TAG_USER_OBJ, 0u, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_GROUP_OBJ, 0u, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_MASK, 0u, XA_UNDEF);
    n += acl_ent(v + n, XA_TAG_OTHER, 0u, XA_UNDEF);
    return n;
}

static int cmd_acl(int argc, char** argv) {
    if (argc < 3) { out1("usage: vmtool acl grant|deny|get|clear <path> [args]\n"); return 1; }
    const char* sub = argv[2];
    const char* path = argv[3];
    u8 v[64];
    if (seq(sub, "grant")) {
        u32 uid = 0, perm = 0;
        if (argc < 6 || !parse_u(argv[4], &uid) || !parse_u(argv[5], &perm) || perm > 7u) {
            out1("usage: vmtool acl grant <path> <uid> <perm3>\n");
            return 1;
        }
        const u32 n = acl_build_grant(v, uid, perm);
        const i64 rc = sc5(NR_SETXATTR, (i64)path, (i64)ACL_NAME, (i64)v, (i64)n, 0);
        out1("vmtool: acl grant uid=");
        out1u(uid);
        out1(" perm=");
        out1u(perm);
        out1(" bytes=");
        out1u(n);
        out1(rc == 0 ? " ok\n" : " FAILED\n");
        if (rc != 0) { out1("vmtool: rc="); out1u((u64)(-rc)); out1("\n"); }
        return rc == 0 ? 0 : 1;
    }
    if (seq(sub, "deny")) {
        const u32 n = acl_build_deny(v);
        const i64 rc = sc5(NR_SETXATTR, (i64)path, (i64)ACL_NAME, (i64)v, (i64)n, 0);
        out1("vmtool: acl deny bytes=");
        out1u(n);
        out1(rc == 0 ? " ok\n" : " FAILED\n");
        return rc == 0 ? 0 : 1;
    }
    if (seq(sub, "clear")) {
        const i64 rc = sc5(NR_SETXATTR, (i64)path, (i64)ACL_NAME, 0, 0, 0);
        out1(rc == 0 ? "vmtool: acl clear ok\n" : "vmtool: acl clear FAILED\n");
        return rc == 0 ? 0 : 1;
    }
    if (seq(sub, "get")) {
        const i64 n = sc4(NR_GETXATTR, (i64)path, (i64)ACL_NAME, (i64)v, (i64)sizeof(v));
        if (n < 0) { out1("vmtool: acl get: none\n"); return 1; }
        out1("vmtool: acl get bytes=");
        out1u((u64)n);
        out1("\n");
        /* 内核返回**裸条目数组**（Linux 的 4 B 版本头已在写入时剥掉），8 B 一项 */
        for (i64 o = 0; o + 8 <= n; o += 8) {
            out1("  tag=");
            out1x((u32)get16(v + o));
            out1(" perm=");
            out1u((u64)get16(v + o + 2));
            out1(" id=");
            out1u((u64)get32(v + o + 4));
            out1("\n");
        }
        return 0;
    }
    out1("vmtool: unknown acl subcommand\n");
    return 1;
}

static int cmd_quota(int argc, char** argv) {
    if (argc < 3) { out1("usage: vmtool quota set|get <uid> [blocks]\n"); return 1; }
    u32 uid = 0;
    if (!parse_u(argv[3], &uid)) { out1("vmtool: bad uid\n"); return 1; }
    if (seq(argv[2], "set")) {
        u32 lim = 0;
        if (argc < 5 || !parse_u(argv[4], &lim)) { out1("usage: vmtool quota set <uid> <blocks|unlimited>\n"); return 1; }
        const i64 rc = sc2(NR_QUOTA_SET, (i64)uid, (i64)lim);
        out1("vmtool: quota set uid=");
        out1u(uid);
        out1(" limit=");
        if (lim == 0xFFFFFFFFu) out1("unlimited"); else out1u(lim);
        out1(rc == 0 ? " ok\n" : " FAILED\n");
        if (rc != 0) { out1("vmtool: rc="); out1u((u64)(-rc)); out1("\n"); }
        return rc == 0 ? 0 : 1;
    }
    if (seq(argv[2], "get")) {
        u32 lim = 0, used = 0;
        const i64 rc = sc3(NR_QUOTA_GET, (i64)uid, (i64)&lim, (i64)&used);
        if (rc != 0) { out1("vmtool: quota get FAILED\n"); return 1; }
        out1("vmtool: quota uid=");
        out1u(uid);
        out1(" limit=");
        if (lim == 0xFFFFFFFFu) out1("unlimited"); else out1u(lim);
        out1(" used=");
        out1u(used);
        out1("\n");
        return 0;
    }
    out1("vmtool: unknown quota subcommand\n");
    return 1;
}

static int cmd_feature(int argc, char** argv) {
    if (argc < 4 || !seq(argv[2], "on")) { out1("usage: vmtool feature on <acl|quota|journal|snapshot|all>\n"); return 1; }
    u32 bits = 0;
    if (seq(argv[3], "all")) bits = VMT_FEAT_ALL;
    else if (seq(argv[3], "journal")) bits = 1u;
    else if (seq(argv[3], "quota")) bits = 2u;
    else if (seq(argv[3], "acl")) bits = 4u;
    else if (seq(argv[3], "snapshot")) bits = 8u;
    else { out1("vmtool: unknown feature name\n"); return 1; }
    const i64 rc = sc1(NR_FEAT_ENABLE, (i64)bits);
    out1("vmtool: feature on bits=");
    out1u(bits);
    out1(rc == 0 ? " ok\n" : " FAILED\n");
    if (rc != 0) { out1("vmtool: rc="); out1u((u64)(-rc)); out1("\n"); }
    return rc == 0 ? 0 : 1;
}

static int cmd_write(int argc, char** argv) {
    if (argc < 4) { out1("usage: vmtool write <path> <bytes>\n"); return 1; }
    u32 nbytes = 0;
    if (!parse_u(argv[3], &nbytes) || nbytes == 0) { out1("vmtool: bad size\n"); return 1; }
    const i64 fd = f_open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        out1("vmtool: open FAILED rc=");
        out1u((u64)(-fd));
        out1("\n");
        return 1;
    }
    static char buf[512];
    for (u32 i = 0; i < sizeof(buf); i++) buf[i] = 'A';
    u32 left = nbytes, done = 0;
    while (left) {
        const u32 chunk = (left > sizeof(buf)) ? (u32)sizeof(buf) : left;
        const i64 w = sc3(NR_WRITE, fd, (i64)buf, (i64)chunk);
        if (w <= 0) {
            out1("vmtool: write FAILED rc=");
            out1u((u64)(-w));
            out1(" after ");
            out1u(done);
            out1(" bytes\n");
            (void)sc1(NR_CLOSE, fd);
            return 1;
        }
        done += (u32)w;
        left -= (u32)w;
    }
    (void)sc1(NR_CLOSE, fd);
    out1("vmtool: wrote ");
    out1u(done);
    out1(" bytes to ");
    out1(argv[2]);
    out1("\n");
    return 0;
}

static int cmd_snap(int argc, char** argv) {
    if (argc >= 2 && seq(argv[1], "snap-take")) {
        u32 want = 0;
        if (argc >= 3 && !parse_u(argv[2], &want)) { out1("vmtool: bad size\n"); return 1; }
        const i64 id = sc1(NR_SNAP_TAKE, (i64)want);
        if (id < 0) { out1("vmtool: snap-take FAILED\n"); return 1; }
        out1("vmtool: snap-take id=");
        out1u((u64)id);
        out1("\n");
        return 0;
    }
    if (argc >= 3 && seq(argv[1], "snap-mount")) {
        u32 id = 0;
        if (!parse_u(argv[2], &id)) { out1("vmtool: bad id\n"); return 1; }
        const i64 slot = sc1(NR_SNAP_MOUNT, (i64)id);
        if (slot < 0) { out1("vmtool: snap-mount FAILED\n"); return 1; }
        out1("vmtool: snap-mount id=");
        out1u(id);
        out1(" slot=");
        out1u((u64)slot);
        out1("\n");
        return 0;
    }
    out1("usage: vmtool snap-take [want_blocks] | snap-mount <id>\n");
    return 1;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        out1("vmtool - ACL/quota/journal/snapshot ring-3 entry (VimtuOS v0.9.30)\n"
             "  features | journal | snapshots | quota-all\n"
             "  feature on <acl|quota|journal|snapshot|all>\n"
             "  acl grant <path> <uid> <perm3> | acl deny <path> | acl get <path> | acl clear <path>\n"
             "  quota set <uid> <blocks|unlimited> | quota get <uid>\n"
             "  snap-take [want_blocks] | snap-mount <id>\n"
             "  write <path> <bytes>\n");
        return 1;
    }
    const char* c = argv[1];
    if (seq(c, "features")) return cat_file("/proc/vimtu/features");
    if (seq(c, "journal")) return cat_file("/proc/vimtu/journal");
    if (seq(c, "snapshots")) return cat_file("/proc/vimtu/snapshots");
    if (seq(c, "quota-all")) return cat_file("/proc/vimtu/quota");
    if (seq(c, "acl")) return cmd_acl(argc, argv);
    if (seq(c, "quota")) return cmd_quota(argc, argv);
    if (seq(c, "feature")) return cmd_feature(argc, argv);
    if (seq(c, "write")) return cmd_write(argc, argv);
    if (seq(c, "snap-take") || seq(c, "snap-mount")) return cmd_snap(argc, argv);
    out1("vmtool: unknown command '");
    out1(c);
    out1("'\n");
    return 1;
}
