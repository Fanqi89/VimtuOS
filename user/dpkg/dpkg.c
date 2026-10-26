/* dpkg.c - ★ v0.7.5：最小 dpkg / apt（Ring 3，静态 ELF64，走内核主程序装载器）
 *
 * 为什么**不移植 Debian 的 dpkg/apt 二进制**（如实说明技术门槛，有依据）：
 *   * apt 依赖 libapt-pkg(C++) + libstdc++ + gnutls + lz4/zstd/xz + libgcrypt …，
 *     需要完整的动态链接 glibc 生态与 /proc、/dev、文件锁、信号栈；
 *   * dpkg 本身虽是 C，但它 fork 出 dpkg-deb，且依赖 tar/xz/zstd 的全套命令行与多种
 *     POSIX 语义（hardlink、flock、/var/lib/dpkg/lock*）。
 *   在本内核当前能力下移植它们不现实。**所以这里做的是"兼容工具"**：
 *   同一套命令、同一套 .deb 格式、同一个 /var/lib/dpkg/status 状态文件 —— 程序自己写，行为对齐。
 *
 * 支持的 .deb 结构（ar 归档）：
 *   !<arch>\n
 *     debian-binary        "2.0\n"
 *     control.tar.gz       元数据（./control 里有 Package/Version/Architecture…）
 *     data.tar.gz          文件树（./usr/bin/... 等）
 *   只支持 **.tar.gz** 成员（xz/zst 成员如实拒绝并提示 —— 本内核还没有 xz/zstd 解码器）。
 *
 * 用法：
 *   dpkg -i PKG.deb     安装：解 control 取元数据 -> 解 data 到 / -> 追加 /var/lib/dpkg/status
 *   dpkg -l             列出已安装（读 /var/lib/dpkg/status）
 *   dpkg -r NAME        移除记录（**不删文件**：本工具没有记录文件清单，如实说明）
 *   dpkg --version
 *   apt  install ./X.deb    = dpkg -i
 *   apt  list --installed   = dpkg -l
 *   apt  update / apt install NAME
 *                       远程仓库需要"HTTP + 仓库索引"，当前如实报未启用（不假装成功）
 *
 * 依赖的系统调用全部是 Linux x86_64 号段（与 shell/musl 同一套 ABI）：
 *   read/write/open/close/lseek/mkdir/fork/execve/wait4/unlink/getpid/exit_group
 */
typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

#define NR_READ    0
#define NR_WRITE   1
#define NR_OPEN    2
#define NR_CLOSE   3
#define NR_LSEEK   8
#define NR_FORK    57
#define NR_EXECVE  59
#define NR_WAIT4   61
#define NR_MKDIR   83
#define NR_UNLINK  87
#define NR_GETPID  39
#define NR_EXITG   231

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x200

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

static u32 slen(const char* s) { u32 n = 0; while (s[n]) n++; return n; }
static int seq(const char* a, const char* b) {
    u32 i = 0;
    while (a[i] && b[i]) { if (a[i] != b[i]) return 0; i++; }
    return a[i] == 0 && b[i] == 0;
}
static int starts(const char* s, const char* p) {
    u32 i = 0;
    while (p[i]) { if (s[i] != p[i]) return 0; i++; }
    return 1;
}
static int has(const char* s, const char* p) {          /* 子串 */
    u32 n = slen(s), m = slen(p);
    if (m == 0 || n < m) return 0;
    for (u32 i = 0; i + m <= n; i++) {
        u32 k = 0;
        while (k < m && s[i + k] == p[k]) k++;
        if (k == m) return 1;
    }
    return 0;
}
static void scat(char* d, const char* s, u32 cap) {
    u32 n = slen(d);
    u32 i = 0;
    while (s[i] && n + 1 < cap) d[n++] = s[i++];
    d[n] = 0;
}
static void scpy(char* d, const char* s, u32 cap) {
    u32 i = 0;
    while (s[i] && i + 1 < cap) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static void out(const char* s) { (void)sc3(NR_WRITE, 2, (i64)s, (i64)slen(s)); }

/* ---------------- 文件 ---------------- */
static i64 f_open(const char* p, i64 fl, i64 mode) { return sc3(NR_OPEN, (i64)p, fl, mode); }
static i64 f_read(i64 fd, void* b, i64 n) { return sc3(NR_READ, fd, (i64)b, n); }
static i64 f_write(i64 fd, const void* b, i64 n) { return sc3(NR_WRITE, fd, (i64)b, n); }
static void f_close(i64 fd) { (void)sc1(NR_CLOSE, fd); }

/* mkdir -p（只对给定时逐级建；已存在不算错） */
static void mkdir_p(const char* path) {
    char buf[256];
    scpy(buf, path, sizeof(buf) - 1);
    for (u32 i = 1; buf[i]; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        (void)sc2(NR_MKDIR, (i64)buf, 0755);
        buf[i] = '/';
    }
    (void)sc2(NR_MKDIR, (i64)buf, 0755);
}

/* fork + execve + wait4（返回子进程退出码；<0 = 起不来） */
static int run(char* const argv[]) {
    const i64 pid = sc1(NR_FORK, 0);
    if (pid < 0) return -1;
    if (pid == 0) {
        (void)sc3(NR_EXECVE, (i64)argv[0], (i64)argv, 0);
        out("dpkg: exec failed: ");
        out(argv[0]);
        out("\n");
        (void)sc1(NR_EXITG, 127);
    }
    int st = 0;
    const i64 w = sc4(NR_WAIT4, pid, (i64)&st, 0, 0);
    if (w < 0) return -1;
    return (st >> 8) & 0xFF;                                /* exit status 在 bit 8..15 */
}

/* ---------------- ar 归档解析 ---------------- */
/* 返回 0 = 成功提取所有成员到 outdir；-2 = 有不支持的成员（xz/zst） */
static int ar_extract(const char* deb, const char* outdir, char* ctl_name, u32 ctl_cap,
                      char* data_name, u32 data_cap, int* saw_debian_binary) {
    const i64 fd = f_open(deb, O_RDONLY, 0);
    if (fd < 0) { out("dpkg: 打不开 .deb\n"); return -1; }
    char magic[8];
    if (f_read(fd, magic, 8) != 8 || magic[0] != '!' || magic[1] != '<' ||
        magic[2] != 'a' || magic[3] != 'r' || magic[4] != 'c' || magic[5] != 'h' || magic[6] != '>') {
        out("dpkg: 不是 ar 归档（.deb 必须以 !<arch> 开头）\n");
        f_close(fd);
        return -1;
    }
    int rc = 0;
    char hdr[60];
    // ★ v0.7.6（真缺陷）：静态缓冲总量必须让 .bss 进得下 execve 的
//   **64 KiB 主程序窗口**（上界 = 4 GiB + 64 KiB = 用户栈起点 0x100010000）。
//   原来 io[32768] + 4096 + 8192 + 16384 + 16384 = 77824 B，.bss 一直压到 0x100017000 ->
//   装载器直接拒：`[ELF64] exec reject segment va=0000000100000000 reason=overlaps-user-region`，
//   于是 dpkg/apt 根本起不来。现在总量 40960 B（.bss 结束 0x10000E000，留 8 KiB 余）。
//   代价（如实）：ar 流拷贝从 32 KiB/次 降到 16 KiB/次（只影响掐速度）。
static char io[16384];
    for (;;) {
        const i64 n = f_read(fd, hdr, 60);
        if (n == 0) break;                                  /* 正常结束 */
        if (n != 60) { rc = -1; break; }
        if (hdr[58] != '`' || hdr[59] != '\n') { out("dpkg: ar 成员头损坏\n"); rc = -1; break; }
        char name[17];
        u32 k = 0;
        while (k < 16 && hdr[k] != ' ' && hdr[k] != '/') { name[k] = hdr[k]; k++; }
        name[k] = 0;
        u64 size = 0;
        for (u32 i = 48; i < 58; i++) {
            if (hdr[i] == ' ') continue;
            if (hdr[i] < '0' || hdr[i] > '9') break;
            size = size * 10u + (u64)(hdr[i] - '0');
        }
        if (seq(name, "debian-binary")) { if (saw_debian_binary) *saw_debian_binary = 1; }
        else if (starts(name, "control.tar")) scpy(ctl_name, name, ctl_cap);
        else if (starts(name, "data.tar")) scpy(data_name, name, data_cap);
        /* xz / zstd 成员：如实拒绝 */
        if ((starts(name, "control.tar") || starts(name, "data.tar")) &&
            !(name[slen(name) - 2] == 'g' && name[slen(name) - 1] == 'z')) {
            out("dpkg: 该 .deb 用 xz/zst 压缩（本内核暂无解码器），请用 data.tar.gz 的包\n");
            rc = -2;
        }
        /* 提取这个成员到 outdir/<name> */
        if ((starts(name, "control.tar") || starts(name, "data.tar") ||
             seq(name, "debian-binary")) && rc == 0) {
            char dst[256];
            scpy(dst, outdir, sizeof(dst) - 1);
            scat(dst, "/", sizeof(dst));
            scat(dst, name, sizeof(dst));
            const i64 ofd = f_open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (ofd < 0) { out("dpkg: 写临时文件失败\n"); rc = -1; }
            u64 left = size;
            while (left > 0 && rc == 0) {
                const u64 want = left > sizeof(io) ? (u64)sizeof(io) : left;
                const i64 got = f_read(fd, io, (i64)want);
                if (got <= 0) { rc = -1; break; }
                if (f_write(ofd, io, got) != got) { rc = -1; break; }
                left -= (u64)got;
            }
            if (ofd >= 0) f_close(ofd);
        } else {
            /* 不提取的成员：跳过 size 字节 */
            u64 left = size;
            while (left > 0) {
                const u64 want = left > sizeof(io) ? (u64)sizeof(io) : left;
                const i64 got = f_read(fd, io, (i64)want);
                if (got <= 0) { rc = -1; break; }
                left -= (u64)got;
            }
        }
        if (size & 1u) { char pad; (void)f_read(fd, &pad, 1); }   /* 2 字节对齐 */
        if (rc != 0) break;
    }
    f_close(fd);
    return rc;
}

/* 从 ./control 里取 "Package:" 与 "Version:" */
static void parse_control(const char* ctl_path, char* pkg, u32 pc, char* ver, u32 vc) {
    pkg[0] = 0;
    ver[0] = 0;
    const i64 fd = f_open(ctl_path, O_RDONLY, 0);
    if (fd < 0) return;
    static char buf[4096];
    const i64 n = f_read(fd, buf, (i64)sizeof(buf) - 1);
    f_close(fd);
    if (n <= 0) return;
    buf[n] = 0;
    char* p = buf;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }
        if (starts(line, "Package:")) {
            char* v = line + 8;
            while (*v == ' ') v++;
            scpy(pkg, v, pc);
        } else if (starts(line, "Version:")) {
            char* v = line + 8;
            while (*v == ' ') v++;
            scpy(ver, v, vc);
        }
    }
}

/* ---------------- /var/lib/dpkg/status ---------------- */
#define STATUS_PATH "/var/lib/dpkg/status"

static int status_append(const char* pkg, const char* ver, const char* deb_path) {
    mkdir_p("/var/lib/dpkg");
    const i64 fd = f_open(STATUS_PATH, O_WRONLY | O_CREAT, 0644);
    if (fd < 0) { out("dpkg: 打不开 " STATUS_PATH "\n"); return -1; }
    (void)sc3(NR_LSEEK, fd, 0, 2);                          /* 追加到末尾 */
    char b[1024];
    scpy(b, "Package: ", sizeof(b)); scat(b, pkg, sizeof(b));
    scat(b, "\nStatus: install ok installed\nVersion: ", sizeof(b)); scat(b, ver, sizeof(b));
    scat(b, "\nSource: ", sizeof(b)); scat(b, deb_path, sizeof(b));
    scat(b, "\nInstalled-By: vimtu-dpkg 0.7.5\n\n", sizeof(b));
    const i64 w = f_write(fd, b, (i64)slen(b));
    f_close(fd);
    return (w > 0) ? 0 : -1;
}

static void status_list(void) {
    const i64 fd = f_open(STATUS_PATH, O_RDONLY, 0);
    if (fd < 0) { out("dpkg: 还没有安装任何包（" STATUS_PATH " 不存在）\n"); return; }
    static char buf[4096];                                // 状态文件列表（每包 ~120 B，4 KiB 约 34 个包）
    const i64 n = f_read(fd, buf, (i64)sizeof(buf) - 1);
    f_close(fd);
    if (n <= 0) { out("dpkg: 还没有安装任何包\n"); return; }
    buf[n] = 0;
    out("Desired=Unknown/Install/Remove/Purge/Hold\n");
    out("||/ Name                 Version              Status\n");
    out("+++-====================-====================-===================\n");
    char* p = buf;
    char name[128], ver[128];
    int have_name = 0, have_ver = 0;
    name[0] = 0; ver[0] = 0;
    while (*p) {
        char* line = p;
        while (*p && *p != '\n') p++;
        if (*p == '\n') { *p = 0; p++; }
        if (starts(line, "Package:")) {
            char* v = line + 8; while (*v == ' ') v++;
            scpy(name, v, sizeof(name)); have_name = 1;
        } else if (starts(line, "Version:")) {
            char* v = line + 8; while (*v == ' ') v++;
            scpy(ver, v, sizeof(ver)); have_ver = 1;
        } else if (line[0] == 0) {                          /* 段落结束 */
            if (have_name) {
                out("ii  ");
                out(name);
                for (u32 i = slen(name); i < 20; i++) out(" ");
                out(" ");
                out(have_ver ? ver : "-");
                out("\n");
            }
            have_name = 0; have_ver = 0; name[0] = 0; ver[0] = 0;
        }
    }
    if (have_name) {
        out("ii  ");
        out(name);
        out("\n");
    }
}

/* 从 status 里删掉某个包的段落，写回 */
static int status_remove(const char* pkg) {
    const i64 fd = f_open(STATUS_PATH, O_RDONLY, 0);
    if (fd < 0) { out("dpkg: 没有安装记录\n"); return -1; }
    static char buf[8192];
    const i64 n = f_read(fd, buf, (i64)sizeof(buf) - 1);
    if (n >= (i64)sizeof(buf) - 1) {                      // 如实拒绝，不静默截断状态文件
        out("dpkg: /var/lib/dpkg/status 太大（本工具缓冲 8 KiB），请先手工清理\n");
        f_close(fd);
        return -1;
    }
    f_close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    static char keep[8192];
    keep[0] = 0;
    char* p = buf;
    char para[2048];
    int removed = 0;
    while (*p) {
        u32 k = 0;
        para[0] = 0;
        while (*p) {                                         /* 读一个段落（到空行为止） */
            if (*p == '\n') {
                if (p[1] == '\n' || p[1] == 0) { p++; if (*p) p++; break; }
            }
            if (k + 1 < sizeof(para)) para[k++] = *p;
            p++;
        }
        para[k] = 0;
        if (starts(para, "Package: ")) {
            const char* v = para + 9;
            u32 m = 0;
            while (v[m] && v[m] != '\n') m++;
            if (m == slen(pkg)) {
                u32 i = 0;
                while (i < m && v[i] == pkg[i]) i++;
                if (i == m) { removed = 1; continue; }        /* 整段丢弃 */
            }
        }
        if (keep[0]) scat(keep, "\n", sizeof(keep));
        scat(keep, para, sizeof(keep));
    }
    const i64 wfd = f_open(STATUS_PATH, O_WRONLY | O_TRUNC, 0644);
    if (wfd < 0) return -1;
    if (keep[0]) (void)f_write(wfd, keep, (i64)slen(keep));
    f_close(wfd);
    if (!removed) { out("dpkg: 没有这个包\n"); return -1; }
    out("dpkg: 已移除记录（**文件未删除**：本工具未记录文件清单，如实说明）\n");
    return 0;
}

/* ---------------- 安装 ---------------- */
static int install_deb(const char* deb) {
    const char* tmpdir = "/tmp/dpkg-work";
    (void)sc2(NR_MKDIR, (i64)"/tmp", 0777);
    (void)sc2(NR_MKDIR, (i64)tmpdir, 0755);
    (void)sc2(NR_MKDIR, (i64)"/tmp/dpkg-work/ctl", 0755);

    char ctl[64], data[64];
    ctl[0] = 0; data[0] = 0;
    int saw_db = 0;
    const int ar = ar_extract(deb, tmpdir, ctl, sizeof(ctl), data, sizeof(data), &saw_db);
    (void)saw_db;                                          /* debian-binary 存在与否不影响安装，只作证据 */
    if (ar != 0) return (ar == -2) ? 3 : 1;
    if (!ctl[0] || !data[0]) { out("dpkg: .deb 缺 control.tar.gz 或 data.tar.gz\n"); return 1; }

    /* 1) 解 control */
    {
        char p[256];
        scpy(p, tmpdir, sizeof(p) - 1); scat(p, "/", sizeof(p)); scat(p, ctl, sizeof(p));
        char d[128];
        scpy(d, tmpdir, sizeof(d) - 1); scat(d, "/ctl", sizeof(d));
        char* av[] = { (char*)"/usr/bin/tar", (char*)"-x", (char*)"-z", (char*)"-C", d, (char*)"-f", p, 0 };
        const int rc = run(av);
        if (rc != 0) { out("dpkg: 解 control 失败（tar 非 0 退出）\n"); return 1; }
    }

    char pkg[64], ver[64];
    {
        char cp[256];
        scpy(cp, tmpdir, sizeof(cp) - 1); scat(cp, "/ctl/control", sizeof(cp));
        parse_control(cp, pkg, sizeof(pkg), ver, sizeof(ver));
    }
    if (!pkg[0]) { out("dpkg: control 文件里没有 Package: 字段\n"); return 1; }

    /* 2) 解 data 到 / */
    {
        char p[256];
        scpy(p, tmpdir, sizeof(p) - 1); scat(p, "/", sizeof(p)); scat(p, data, sizeof(p));
        char* av[] = { (char*)"/usr/bin/tar", (char*)"-x", (char*)"-z", (char*)"-C", (char*)"/", (char*)"-f", p, 0 };
        const int rc = run(av);
        if (rc != 0) { out("dpkg: 解 data 失败（文件未完全落地）\n"); return 1; }
    }

    /* 3) 记状态 */
    if (status_append(pkg, ver, deb) != 0) return 1;

    out("dpkg: 已安装 ");
    out(pkg);
    out(" ");
    out(ver[0] ? ver : "(no version)");
    out("\n");
    return 0;
}

static void usage(void) {
    out("用法:\n");
    out("  dpkg -i PKG.deb      安装（只支持 data.tar.gz / control.tar.gz 的包）\n");
    out("  dpkg -l              列出已安装\n");
    out("  dpkg -r NAME         移除安装记录（不删文件，如实说明）\n");
    out("  dpkg --version\n");
    out("  apt install ./X.deb | apt list --installed | apt update\n");
}

static int is_apt(const char* a0) { return has(a0, "apt"); }

int main(int argc, char** argv) {
    const char* a0 = (argc > 0 && argv[0]) ? argv[0] : "dpkg";
    if (argc >= 2 && (seq(argv[1], "--version") || seq(argv[1], "-V"))) {
        out("dpkg 0.7.5 (vimtu-dpkg; .deb = ar + control.tar.gz + data.tar.gz)\n");
        out("apt  0.7.5 (vimtu-apt; install ./X.deb 走 dpkg，远程仓库未启用)\n");
        return 0;
    }
    if (is_apt(a0)) {
        if (argc >= 2 && seq(argv[1], "install")) {
            if (argc < 3) { out("apt: install 需要 .deb 路径\n"); return 1; }
            if (!has(argv[2], ".deb")) {
                out("apt: 远程仓库安装（apt install NAME）需要 HTTP + 仓库索引 —— 当前未启用\n");
                out("apt: 本地包请用：apt install ./包名.deb\n");
                return 2;
            }
            return install_deb(argv[2]);
        }
        if (argc >= 2 && seq(argv[1], "list")) { status_list(); return 0; }
        if (argc >= 2 && seq(argv[1], "update")) {
            out("apt: update 需要访问远程仓库（HTTP + Release/InRelease 索引）—— 当前未启用\n");
            out("apt: 已如实返回，不假装成功\n");
            return 2;
        }
        usage();
        return 1;
    }
    if (argc >= 3 && seq(argv[1], "-i")) return install_deb(argv[2]);
    if (argc >= 2 && (seq(argv[1], "-l") || seq(argv[1], "--list"))) { status_list(); return 0; }
    if (argc >= 3 && (seq(argv[1], "-r") || seq(argv[1], "--remove"))) return status_remove(argv[2]);
    usage();
    return 1;
}
