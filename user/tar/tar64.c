/* tar64.c - ★ B5：Ring 3 的 `tar`（VimtuOS 系统卷里的 /bin/tar）
 *
 * 交付形态：**静态 musl、非 PIC、按 4GiB 装载区定址的 < 64 KiB 主程序**（与 /bin/gzip、/bin/edit
 *   同一条路：内核的主程序装载器直接装，`run /bin/tar …`）。
 *
 * 支持（验收脚本 tests/tar64_test.py 逐条断言）：
 *   动作（三选一）：-c 创建 / -t 列表 / -x 解包
 *   选项串        ：`-cvf x.tar`、`-czf x.tgz`、`-xf x.tar`、`-tvf x.tar`、`-C DIR`、`-f FILE`、`-z`、`-v`、
 *                   **`-T LIST`**（从文件读成员名，一行一个；`-c` 专用）。
 *                   ★ 为什么必须有 `-T`：内核的 execve 只收 **8 个 argv**（kernel/syscall64.cpp 的
 *                     LX64_EXEC_ARGV_MAX=8，本批不许改内核），所以"成员一个个写在命令行上"最多只能带
 *                     3~6 个；要打包更多成员就走 `-T`（tar 自己开发列表读，不受 argv 限制）。
 *   归档格式      ：**ustar（POSIX.1-1988）**：magic `ustar\0` + version `00`，名字 > 100 字节时用
 *                   155 字节的 prefix 字段切分。读的时候额外容忍 GNU longname('L') 与
 *                   pax 扩展头('x'/'g'，跳过)——那是 GNU tar / Python tarfile 的常见产物。
 *   目录          ：-c 对**命令行上给出的目录**只写一条目录项（**不递归**：ring3 没有 getdents，
 *                   见下面"不做什么"）；-x 按成员里的目录项逐级 mkdir -p。
 *   gzip 串联（-z）：**真 fork/exec**：先写到临时 .tar，再 fork 一个子进程跑
 *                   `/bin/gzip -k -o OUT tmp.tar`（解包/列表用 /bin/gunzip）并 wait4。
 *   `-f -`        ：归档流走 fd 1（创建）/ fd 0（解包）—— 本内核的 shell 里**跨进程管道不成立**，
 *                   所以这一项只是接口完整性的如实保留（测试不依赖它）。
 *
 * 不做什么（如实，报告里也列）：
 *   * **不递归目录**（ring3 没有 getdents(217)：内核只在 shell 的邮箱协议里代列目录）。要打包一棵树
 *     就把成员一个个写在命令行上（测试与演示都这么做）。
 *   * 不做硬/软链接、不做权限继承（-x 时按成员 mode 落权限）、不做增量/排除/通配符（shell 不展开 *）。
 *   * 不支持绝对路径与 ".."（-x 时**直接拒绝**并返回 2，不写盘外的东西）。
 * 出错一律打一行 `tar: …` 到 fd 2 并返回 2（不是 0、不是 PANIC）。
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BLK 512
#define NAME_MAX_ 100
#define PREFIX_MAX_ 155

static int g_verbose;

/* ★ 大缓冲一律 **static**：内核给"主程序装载器直接装"的程序只摆 16 KiB 用户栈
 *   （kernel/usermode64.h 的 USER64_STACK_VA64），实测在栈上放 4 KiB 临时数组 + musl 的
 *   fprintf 会把栈踩穿：`[SIG64] fault pid=28 no=14 #PF err=0x6 cr2=0x000000010000FFF0`
 *   （cr2 = 栈底之下 16 字节）-> sig=11/exit=139。这些缓冲只在单线程路径上用，static 安全。 */
static char g_data[BLK];                    /* 512 B：数据块 */
/* ★ 缓冲尺寸的来由（两边都踩过）：
 *   ① 走**内核主程序装载器**的程序，所有 PT_LOAD（含 .bss 的 p_memsz）必须落在用户窗口
 *      低 64 KiB 里 —— 把 16 KiB 缓冲放进 .bss 曾让构建期断言直接红（ved=0x10000d000
 *      msz=0x4e70）。② 改用 malloc 又会让 musl 的 malloc 全量链进来，二进制定位 >64 KiB。
 *   所以：**小静态缓冲 + 如实拒绝超长名字**（ustar 的单成员名上限本来就是 256 B）。 */
#define G_NAME_CAP 512                      /* ustar: prefix(155) + '/' + name(100) 够用 */
#define G_LONG_CAP 512
#define G_TLIST_MAX 12                      /* -T 列表最多 12 个成员（演示/验收用得到 5 个） */
#define G_TNAME_CAP 128
static char g_name[G_NAME_CAP];
static char g_longname[G_LONG_CAP];
static char g_mstore[G_TLIST_MAX][G_TNAME_CAP];

static void errmsg(const char* a, const char* b) {
    fputs("tar: ", stderr);
    if (a) fputs(a, stderr);
    if (b) fputs(b, stderr);
    fputc('\n', stderr);
}
static void err2(const char* a, const char* b) {   /* 即刻退出（不假装成功） */
    errmsg(a, b);
    exit(2);
}

/* ==================== 小工具 ==================== */
static int write_all(int fd, const void* buf, size_t n) {
    const char* p = (const char*)buf;
    size_t off = 0;
    while (off < n) {
        const ssize_t w = write(fd, p + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return 0;
}
static int read_all(int fd, void* buf, size_t n) {          /* 返回实际读到的字节数 */
    char* p = (char*)buf;
    size_t off = 0;
    while (off < n) {
        const ssize_t r = read(fd, p + off, n - off);
        if (r < 0) return -1;
        if (r == 0) break;
        off += (size_t)r;
    }
    return (int)off;
}
/* 八进制字段（ustar 用 NUL 或空格结尾；size 用 12 字节） */
static void put_octal(char* dst, int len, unsigned long long v) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v & 7ull)); v >>= 3; }
    while (n < len - 1) t[n++] = '0';                         /* 前导 0 补齐 */
    for (int i = 0; i < len - 1; i++) dst[i] = t[len - 2 - i];
    dst[len - 1] = 0;
}
static unsigned long long get_octal(const char* src, int len) {
    unsigned long long v = 0;
    for (int i = 0; i < len; i++) {
        const char c = src[i];
        if (c == 0 || c == ' ') { if (v) break; else continue; }
        if (c < '0' || c > '7') break;
        v = v * 8ull + (unsigned long long)(c - '0');
    }
    return v;
}
static int checksum_ok(const char* h) {
    unsigned long sum = 0;
    for (int i = 0; i < BLK; i++) {
        const unsigned char c = (i >= 148 && i < 156) ? (unsigned char)' ' : (unsigned char)h[i];
        sum += c;
    }
    return get_octal(h + 148, 8) == sum;
}
static void set_checksum(char* h) {
    /* ★ 顺序很重要：**先把校验和字段填成 8 个空格**再求和（POSIX：校验和字段自身按空格算）。
     *   踩过的坑：先求和再填空格 -> 总和少了 8*' '=256 -> 宿主 Python tarfile 第一句就
     *   `ReadError: bad checksum`（实测）。 */
    for (int i = 148; i < 156; i++) h[i] = ' ';
    unsigned long sum = 0;
    for (int i = 0; i < BLK; i++) sum += (unsigned char)h[i];
    put_octal(h + 148, 7, sum);                                /* 6 位八进制 + NUL（h[148..153]） */
    h[154] = ' ';                                             /* 经典格式：6 位八进制 + NUL + 空格 */
    h[155] = 0;
}

/* ==================== ustar 头 ==================== */
/* 把 path 拆成 prefix/name（ustar 的单段名上限 100、prefix 上限 155）。
 * 返回 0 成功；-1 = 名字太长（本工具如实拒绝，不做 GNU longname 写出）。 */
static int split_name(const char* path, char* name, char* prefix) {
    const size_t n = strlen(path);
    name[0] = 0;
    prefix[0] = 0;
    if (n <= NAME_MAX_) { memcpy(name, path, n + 1); return 0; }
    /* 找一个切点：让后半段 <= 100 且前半段 <= 155（尽量靠右） */
    for (size_t cut = n - NAME_MAX_; cut > 0; cut--) {
        if (path[cut - 1] != '/') continue;
        if (cut - 1 > PREFIX_MAX_) break;
        if (n - cut > NAME_MAX_) continue;
        memcpy(prefix, path, cut - 1);
        prefix[cut - 1] = 0;
        memcpy(name, path + cut, n - cut + 1);
        return 0;
    }
    return -1;
}

static void emit_header(char* h, const char* path, const struct stat* st, int is_dir, unsigned long long size) {
    memset(h, 0, BLK);
    char name[NAME_MAX_ + 1], prefix[PREFIX_MAX_ + 1];
    if (split_name(path, name, prefix) != 0)
        err2("archive member name too long: ", path);
    memcpy(h + 0, name, strlen(name));
    put_octal(h + 100, 8, (unsigned long long)(st->st_mode & 07777u));
    put_octal(h + 108, 8, (unsigned long long)st->st_uid);
    put_octal(h + 116, 8, (unsigned long long)st->st_gid);
    put_octal(h + 124, 12, size);
    unsigned long long mt = (unsigned long long)st->st_mtime;
    if (mt == 0) {                                            /* 本内核的 stat 不返回 mtime：用当前时间 */
        struct timeval tv;
        if (gettimeofday(&tv, 0) == 0) mt = (unsigned long long)tv.tv_sec;
    }
    put_octal(h + 136, 12, mt);
    h[156] = is_dir ? '5' : '0';                              /* typeflag：目录 / 普通文件 */
    memcpy(h + 257, "ustar", 5);                              /* magic "ustar\0" + version "00" = POSIX ustar */
    h[262] = 0;
    h[263] = '0'; h[264] = '0';
    strncpy(h + 265, "root", 31);                             /* uname/gname：本内核没有 /etc/passwd，如实写 root */
    strncpy(h + 297, "root", 31);
    memcpy(h + 345, prefix, strlen(prefix));
    set_checksum(h);
}

/* ==================== 创建 ==================== */
static void add_one(int out_fd, const char* path, int* nent) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        errmsg("cannot stat ", path);
        exit(2);
    }
    const size_t n = strlen(path);
    const int is_dir = S_ISDIR(st.st_mode);
    if (!is_dir && !S_ISREG(st.st_mode)) {
        errmsg("not a regular file (only files/dirs are supported): ", path);
        exit(2);
    }
    char* name = g_name;
    if (n >= (size_t)G_NAME_CAP - 2) err2("path too long: ", path);
    memcpy(name, path, n + 1);
    if (is_dir && name[n - 1] != '/') { name[n] = '/'; name[n + 1] = 0; }
    if (!is_dir && name[0] == '/' ) {                         /* 归档里存相对名字（与 tar 一致） */
        memmove(name, name + 1, strlen(name));
    }
    if (is_dir && name[0] == '/') memmove(name, name + 1, strlen(name));

    char h[BLK];
    emit_header(h, name, &st, is_dir, is_dir ? 0ull : (unsigned long long)st.st_size);
    if (write_all(out_fd, h, BLK) != 0) err2("write error: ", name);
    (*nent)++;
    if (g_verbose) { fputs(is_dir ? "d " : "a ", stderr); fputs(name, stderr); fputc('\n', stderr); }
    if (is_dir) return;                                       /* **不递归**（见文件头说明） */

    const int in = open(path, O_RDONLY);
    if (in < 0) (void)fprintf(stderr, "tar: cannot open %s (errno=%d)\n", path, errno), exit(2);
    unsigned long long left = (unsigned long long)st.st_size;
    while (left > 0) {
        const size_t want = (left > BLK) ? BLK : (size_t)left;
        const int got = read_all(in, g_data, want);
        if (got < 0 || (size_t)got != want) { close(in); err2("short read: ", path); }
        if (write_all(out_fd, g_data, want) != 0) { close(in); err2("write error: ", path); }
        left -= want;
    }
    close(in);
    /* 数据的 512 补齐 */
    const unsigned long long rem = (unsigned long long)st.st_size % BLK;
    if (rem) {
        static const char zero[BLK];                        /* 全零（BSS） */
        if (write_all(out_fd, zero, BLK - (size_t)rem) != 0) err2("write error: ", path);
    }
}

/* ==================== 列表 / 解包 ==================== */
static void ensure_parents(const char* path) {
    char tmp[4096];
    const size_t n = strlen(path);
    if (n >= sizeof(tmp)) err2("path too long: ", path);
    memcpy(tmp, path, n + 1);
    for (size_t i = 1; i < n; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = 0;
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) { /* 已存在 = 正常 */ }
        tmp[i] = '/';
    }
}
static int name_is_safe(const char* name) {
    if (name[0] == '/') return 0;
    const char* p = name;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == 0)) return 0;
        while (*p && *p != '/') p++;
        if (*p == '/') p++;
    }
    return 1;
}
static void do_list_or_extract(int in_fd, int want_extract) {
    char* const h = g_data;
    char* const longname = g_longname;
    longname[0] = 0;
    int nent = 0, nfile = 0;
    for (;;) {
        const int got = read_all(in_fd, h, BLK);
        if (got == 0) break;
        if (got != BLK) err2("truncated header in archive", "");
        /* 全零块 = 归档结束 */
        int allzero = 1;
        for (int i = 0; i < BLK; i++) if (h[i]) { allzero = 0; break; }
        if (allzero) break;
        if (!checksum_ok(h)) err2("bad header checksum at entry ", NULL);
        const char type = h[156];
        const unsigned long long size = get_octal(h + 124, 12);
        if (type == 'L' || type == 'K') {                      /* GNU longname/longlink */
            if (size >= (unsigned long long)G_LONG_CAP) err2("GNU long name too long for this tar (cap 511 B)", "");
            if (size && read_all(in_fd, longname, (size_t)size) != (int)size) err2("truncated longname", "");
            longname[(int)size] = 0;
            const unsigned long long pad0 = size % BLK;
            if (pad0) { if (read_all(in_fd, g_data, BLK - (size_t)pad0) != (int)(BLK - (size_t)pad0)) err2("truncated longname pad", ""); }
            continue;
        }
        if (type == 'x' || type == 'g') {                       /* pax 扩展头：跳过数据（值不用） */
            unsigned long long left = size;
            while (left) {
                const size_t want = (left > BLK) ? BLK : (size_t)left;
                if (read_all(in_fd, g_data, want) != (int)want) err2("truncated pax header", "");
                left -= want;
            }
            const unsigned long long pad0 = size % BLK;
            if (pad0) { if (read_all(in_fd, g_data, BLK - (size_t)pad0) != (int)(BLK - (size_t)pad0)) err2("truncated pax pad", ""); }
            continue;
        }
        char* name = g_name;
        const char* nm = longname[0] ? longname : h;
        size_t nl = strlen(nm);
        if (nl > NAME_MAX_) nl = NAME_MAX_;
        memcpy(name, nm, nl);
        size_t o = nl;
        char prefix[PREFIX_MAX_ + 1];
        if (!longname[0]) {
            memcpy(prefix, h + 345, PREFIX_MAX_);
            prefix[PREFIX_MAX_] = 0;
            if (prefix[0]) {
                const size_t pl = strlen(prefix);
                memmove(name + pl + 1, name, nl);
                memcpy(name, prefix, pl);
                name[pl] = '/';
                o = pl + 1 + nl;
            }
        }
        name[o] = 0;
        longname[0] = 0;
        nent++;
        if (!name_is_safe(name)) err2("refusing unsafe member name: ", name);
        if (!want_extract) {
            if (g_verbose)
                printf("%s %8llu %s\n", (type == '5') ? "d" : "-", size, name);
            else
                printf("%s\n", name);
            /* 跳过数据 */
            unsigned long long left = (type == '5') ? 0 : size;
            while (left) {
                const size_t want = (left > BLK) ? BLK : (size_t)left;
                if (read_all(in_fd, g_data, want) != (int)want) err2("truncated data for ", name);
                left -= want;
            }
            const unsigned long long pad0 = ((type == '5') ? 0 : size) % BLK;
            if (pad0) { if (read_all(in_fd, g_data, BLK - (size_t)pad0) != (int)(BLK - (size_t)pad0)) err2("truncated pad for ", name); }
            continue;
        }
        /* ---- 解包 ---- */
        /* ★ ustar 的目录名以 '/' 结尾（本工具 -c 写出的、GNU tar/Python tarfile 也是），
         *   而内核的 mkdir 不接受带结尾 '/' 的路径（实测 vfs64 会报 component not found）——
         *   所以这里把结尾的 '/' 去掉再落盘（列表输出保持原样）。 */
        {
            size_t nl2 = strlen(name);
            while (nl2 > 1 && name[nl2 - 1] == '/') name[--nl2] = 0;
        }
        ensure_parents(name);
        if (type == '5') {
            if (mkdir(name, 0755) != 0 && errno != EEXIST) { fprintf(stderr, "tar: cannot mkdir %s (errno=%d)\n", name, errno); exit(2); }
            if (g_verbose) { fputs("x ", stderr); fputs(name, stderr); fputc('\n', stderr); }
            continue;
        }
        if (type != '0' && type != 0 && type != '7') {
            errmsg("unsupported member type (only files/dirs): ", name);
            exit(2);
        }
        const int out = open(name, O_WRONLY | O_CREAT | O_TRUNC, (int)(get_octal(h + 100, 8) & 07777ull));
        if (out < 0) (void)fprintf(stderr, "tar: cannot create %s (errno=%d)\n", name, errno), exit(2);
        unsigned long long left = size;
        while (left) {
            const size_t want = (left > BLK) ? BLK : (size_t)left;
            if (read_all(in_fd, g_data, want) != (int)want) { close(out); err2("truncated data for ", name); }
            if (write_all(out, g_data, want) != 0) { close(out); err2("write error: ", name); }
            left -= want;
        }
        close(out);
        const unsigned long long pad0 = size % BLK;
        if (pad0) { if (read_all(in_fd, g_data, BLK - (size_t)pad0) != (int)(BLK - (size_t)pad0)) err2("truncated pad for ", name); }
        (void)chmod(name, (mode_t)(get_octal(h + 100, 8) & 07777ull));
        nfile++;
        if (g_verbose) { fputs("x ", stderr); fputs(name, stderr); fputc('\n', stderr); }
    }
    if (!want_extract || g_verbose) {
        fprintf(stderr, "tar: %d entries (%d files)\n", nent, nfile);
    }
}

/* ==================== main ==================== */
int main(int argc, char** argv) {
    int mode = 0;                                             /* 'c' / 't' / 'x' */
    int use_z = 0;
    char* archive = NULL;
    char* chdir_to = NULL;
    char* listfile = NULL;
    char* members[256];
    int nmem = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == 0) break;                  /* 不是选项：成员从这里开始 */
        if (a[1] == '-' && a[2] == 0) { i++; break; }         /* "--" 结束选项 */
        for (int k = 1; a[k]; k++) {
            const char c = a[k];
            if (c == 'c' || c == 't' || c == 'x') {
                if (mode && mode != c) err2("conflicting actions (use only one of -c/-t/-x)", "");
                mode = c;
            } else if (c == 'z') use_z = 1;
            else if (c == 'v') g_verbose = 1;
            else if (c == 'f' || c == 'C' || c == 'T') {
                const char* val = a[k + 1] ? (a + k + 1) : ((i + 1 < argc) ? argv[++i] : NULL);
                if (!val) err2((c == 'f') ? "-f needs an archive name"
                                         : ((c == 'C') ? "-C needs a directory" : "-T needs a list file"), "");
                if (c == 'f') archive = (char*)val;
                else if (c == 'C') chdir_to = (char*)val;
                else listfile = (char*)val;
                break;                                        /* 该选项的余下字符是它的值 */
            } else {
                char m[3]; m[0] = '-'; m[1] = c; m[2] = 0;
                errmsg("unknown option ", m);
                fputs("tar: usage: tar -c|-t|-x [-vz] [-C DIR] [-T LIST] -f ARCHIVE [FILE...]\n", stderr);
                return 2;
            }
        }
    }
    for (; i < argc; i++) {
        if (nmem >= (int)(sizeof(members) / sizeof(members[0]))) err2("too many members", "");
        members[nmem++] = argv[i];
    }
    if (listfile) {
        /* ★ -T：从文件读成员名（一行一个；空行与 `#` 开头的行跳过）。
         *   存储放在 **static**（BSS）：主程序的装载窗口只有 64 KiB，32 个名字够演示与验收用，
         *   而且不走栈（内核给直接装载的程序只有 16 KiB 栈）。 */
        char (*mstore)[G_TNAME_CAP] = g_mstore;
        if (mode != 'c') err2("-T is only supported with -c (this target has no readdir)", "");
        /* ★ 用裸 open/read 读列表（不引 musl 的 fopen/fgets：实测那会让 .text 涨 ~10 KiB，
         *   把"PT_LOAD 落在用户窗口低 64 KiB"这条硬约束逼到边缘）。 */
        static char lb[1024];
        const int lfd = open(listfile, O_RDONLY);
        if (lfd < 0) err2("cannot open list file ", listfile);
        const int lgot = read_all(lfd, lb, sizeof(lb) - 1);
        close(lfd);
        if (lgot < 0) err2("cannot read list file ", listfile);
        lb[lgot] = 0;
        char* lp = lb;
        while (*lp) {
            char* e = lp;
            while (*e && *e != '\n') e++;
            const char saved = *e;
            *e = 0;
            size_t n2 = strlen(lp);
            while (n2 && lp[n2 - 1] == '\r') lp[--n2] = 0;
            if (n2 > 0 && lp[0] != '#') {
                if (nmem >= G_TLIST_MAX) err2("-T list too long (max 12 members on this target)", "");
                if (n2 >= (size_t)G_TNAME_CAP) err2("member name too long in -T list: ", lp);
                memcpy(mstore[nmem], lp, n2 + 1);
                members[nmem] = mstore[nmem];
                nmem++;
            }
            if (!saved) break;
            lp = e + 1;
        }
    }
    if (!mode) {
        fputs("tar: usage: tar -c|-t|-x [-vz] [-C DIR] [-T LIST] -f ARCHIVE [FILE...]\n", stderr);
        return 2;
    }
    if (!archive) err2("missing -f ARCHIVE", "");
    if (mode == 'c' && nmem == 0) err2("-c needs at least one member (no recursion: list them explicitly)", "");
    if (mode != 'c' && nmem != 0) err2("only -c takes member arguments", "");
    if (chdir_to && chdir(chdir_to) != 0) err2("cannot chdir to ", chdir_to);

    const int to_stdout = (archive[0] == '-' && archive[1] == 0);
    char tmp[128];
    int used_tmp = 0;

    /* ---- 创建 ---- */
    if (mode == 'c') {
        char* target = archive;
        if (use_z) {                                          /* 先写临时 .tar，再让 gzip 压 */
            snprintf(tmp, sizeof(tmp), "/tmp/tar_%d.tar", (int)getpid());
            target = tmp;
            used_tmp = 1;
        }
        const int fd = to_stdout ? 1 : open(target, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) err2("cannot create archive ", target);
        int nent = 0;
        for (int k = 0; k < nmem; k++) add_one(fd, members[k], &nent);
        static const char zblk[BLK * 2];                    /* 两块全零 = 标准 tar 结束标记 */
        if (write_all(fd, zblk, BLK * 2) != 0) err2("write error (end blocks): ", target);
        if (!to_stdout && close(fd) != 0) err2("close error: ", target);
        if (g_verbose)
            fprintf(stderr, "tar: created %s (%d entries)\n",
                    to_stdout ? "(stdout)" : archive, nent);          /* -z 时也报最终归档名 */
        if (use_z) {
            char* av[7];
            av[0] = (char*)"/bin/gzip";
            av[1] = (char*)"-f";                              /* 覆盖同名输出（幂等） */
            av[2] = (char*)"-k";                              /* 保留输入（下面我们自己删） */
            av[3] = (char*)"-o";
            av[4] = archive;
            av[5] = tmp;
            av[6] = NULL;
            const pid_t pid = fork();
            if (pid < 0) err2("fork failed for ", "/bin/gzip");
            if (pid == 0) { execv(av[0], av); _exit(127); }
            int st = 0;
            if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
                err2("gzip child failed for ", archive);
            (void)unlink(tmp);
        }
        return 0;
    }

    /* ---- 列表 / 解包 ---- */
    int pid = -1;
    int fd;
    if (use_z) {
        int pfd[2];
        if (pipe(pfd) != 0) err2("pipe failed", "");
        pid = (int)fork();
        if (pid < 0) err2("fork failed for ", "/bin/gunzip");
        if (pid == 0) {
            if (dup2(pfd[1], 1) < 0) _exit(127);
            close(pfd[0]);
            close(pfd[1]);
            char* av[4];
            av[0] = (char*)"/bin/gunzip";
            av[1] = (char*)"-c";
            av[2] = archive;
            av[3] = NULL;
            execv(av[0], av);
            _exit(127);
        }
        close(pfd[1]);
        fd = pfd[0];
    } else {
        fd = to_stdout ? 0 : open(archive, O_RDONLY);
    }
    if (fd < 0) err2("cannot open archive ", archive);
    do_list_or_extract(fd, mode == 'x');
    if (pid > 0) {
        int st = 0;
        close(fd);
        if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0)
            err2("gunzip child failed for ", archive);
    } else if (!to_stdout) {
        close(fd);
    }
    (void)used_tmp;
    return 0;
}
