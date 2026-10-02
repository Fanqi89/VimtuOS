/* vimtu_dirent.c - VimtuOS64：**用户态**的 opendir/readdir/closedir
 *
 * 为什么需要它（本移植最关键的一处边界；报告与 docs/内核边界与架构规则.md 都写了）：
 *   本内核 ring3 **没有 getdents(217)**（kernel/syscall64.cpp 的号段表里没有）。目录枚举在内核
 *   里是有的（vfs64_opendir64/readdir64、fd64_readdir64），但只对内核自己开放 —— 用户态唯一的
 *   通路是终端服务的"代列协议"（/bin/shell.bin 的邮箱 RPC，见 kernel/terminal64.cpp 的
 *   sh64_serve64），而那一页只在 `shell` 命令拉起来的进程里映射，`run` 出来的子进程 execve 之后
 *   就被释放了（proc64_execve64 -> p64_release_area64）。
 *
 *   按本批纪律：**不改内核**。所以目录枚举改用纯用户态方案：构建期由
 *   tools/busybox_pack_win.py 把最终卷的目录树导出成索引 `/etc/vimtu.dirs`
 *   （每行 "<目录绝对路径>" TAB "<名字>"），本文件把 busybox 用的 opendir/readdir 接到它上面。
 *
 *   ★ 如实标注的能力边界（tests/busybox64_test.py 里有断言）：索引是**构建期快照**。本次开机里
 *     新建的文件（mkdir/cp/tar 出来的）不会出现在 ls/find 的输出里。真正的修法就是补一个
 *     getdents64（把内核已有的 fd64_readdir64 暴露给 ring3）—— 那属于宪法里"用户态所必需的 ABI
 *     补齐"，但本批明确不许碰 kernel 目录，所以如实列成 GAP。
 *
 * 链接方式：本文件作为普通目标文件放进 busybox 的链接命令行 —— 目标文件里的强符号优先于静态库
 *   musl/libc.a 里的同名成员，于是 busybox（以及 musl 的 scandir/fts/glob 内部调用）走的就是这份
 *   实现。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VIMTU_DIRIDX   "/etc/vimtu.dirs"
#define VIMTU_NAMEMAX  255
#define VIMTU_CHUNK    4096

/* musl 的 <dirent.h> 只给 `typedef struct __dirstream DIR;`（不完整类型）。这里把它补全：
 * busybox 只把 DIR* 当不透明指针用，所以内部布局由我们说。 */
struct __dirstream {
    struct dirent ent;                       /* readdir 的返回值指向这里 */
    char*  buf;                              /* 整份索引 */
    size_t len;
    size_t pos;                              /* 已扫描到的字节偏移 */
    char   want[512];                        /* 规范化后的目录绝对路径 */
    size_t want_len;
    unsigned long ino;
};

/* ------------------------------------------------------------------ 小工具 */
/* 规范化路径：相对路径前接 getcwd()，合并重复 '/'、去掉结尾 '/'（根目录保留 "/"）。 */
static int vimtu_normalise(const char* in, char* out, size_t cap) {
    char tmp[512];
    size_t j = 0;
    size_t i = 0;
    size_t o = 0;
    if (!in || !*in) return -1;
    if (in[0] != '/') {
        if (!getcwd(tmp, sizeof(tmp))) return -1;
        j = strlen(tmp);
        if (j + 2 >= sizeof(tmp)) return -1;
        tmp[j++] = '/';
    }
    while (in[i] && j + 1 < sizeof(tmp)) tmp[j++] = in[i++];
    tmp[j] = 0;
    i = 0;
    while (tmp[i]) {
        if (tmp[i] == '/') {
            if (o == 0 || out[o - 1] != '/') {
                if (o + 1 >= cap) return -1;
                out[o++] = '/';
            }
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = tmp[i];
        }
        i++;
    }
    while (o > 1 && out[o - 1] == '/') o--;
    if (o == 0) out[o++] = '/';
    out[o] = 0;
    return (int)o;
}

/* 把整个文件读进 malloc 的缓冲（内核单次 read 上限 4096，所以必须循环）。 */
static int vimtu_slurp(const char* path, char** out, size_t* outlen) {
    int fd = open(path, O_RDONLY);
    size_t cap = VIMTU_CHUNK;
    size_t n = 0;
    char* b;
    *out = NULL;
    *outlen = 0;
    if (fd < 0) return -1;
    b = (char*)malloc(cap + 1);
    if (!b) { close(fd); return -1; }
    for (;;) {
        ssize_t r;
        if (n + VIMTU_CHUNK + 1 > cap) {
            char* nb = (char*)realloc(b, cap * 2 + 1);
            if (!nb) { free(b); close(fd); return -1; }
            b = nb;
            cap *= 2;
        }
        r = read(fd, b + n, (size_t)VIMTU_CHUNK);
        if (r < 0) { free(b); close(fd); return -1; }
        if (r == 0) break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = 0;
    *out = b;
    *outlen = n;
    return 0;
}

/* ------------------------------------------------------------------ 入口 */
DIR* opendir(const char* name) {
    struct __dirstream* d;
    char norm[512];
    if (vimtu_normalise(name, norm, sizeof(norm)) < 0) { errno = ENOENT; return NULL; }
    d = (struct __dirstream*)calloc(1, sizeof(*d));
    if (!d) { errno = ENOMEM; return NULL; }
    if (vimtu_slurp(VIMTU_DIRIDX, &d->buf, &d->len) != 0) {
        const int e = errno;
        free(d);
        errno = e;
        return NULL;
    }
    {
        size_t l = strlen(norm);
        if (l >= sizeof(d->want)) l = sizeof(d->want) - 1;
        memcpy(d->want, norm, l);
        d->want[l] = 0;
        d->want_len = l;
    }
    d->ino = 1;
    return (DIR*)d;
}

struct dirent* readdir(DIR* dirp) {
    struct __dirstream* d = (struct __dirstream*)dirp;
    if (!d) { errno = EBADF; return NULL; }
    while (d->pos < d->len) {
        const size_t p = d->pos;
        size_t e = p;
        size_t i, ds, de, ns, nl;
        while (e < d->len && d->buf[e] != '\n') e++;
        d->pos = (e < d->len) ? e + 1 : e;
        if (e == p || d->buf[p] == '#') continue;               /* 空行 / 注释 */
        i = p;
        while (i < e && d->buf[i] != '\t') i++;
        if (i >= e) continue;                                    /* 没有 TAB：不是索引行 */
        ds = p; de = i; ns = i + 1; nl = e - ns;
        if (nl == 0) continue;
        if (de - ds != d->want_len) continue;
        if (memcmp(d->buf + ds, d->want, d->want_len) != 0) continue;
        {
            size_t copy = nl > VIMTU_NAMEMAX ? VIMTU_NAMEMAX : nl;
            memset(&d->ent, 0, sizeof(d->ent));
            d->ent.d_ino = d->ino++;
            d->ent.d_off = (long)d->pos;
            d->ent.d_reclen = (unsigned short)(offsetof(struct dirent, d_name) + copy + 1);
            d->ent.d_type = DT_UNKNOWN;      /* 让调用方自己去 stat（内核 stat 是真的） */
            memcpy(d->ent.d_name, d->buf + ns, copy);
            d->ent.d_name[copy] = 0;
            return &d->ent;
        }
    }
    errno = 0;                                                   /* EOF（POSIX） */
    return NULL;
}

int closedir(DIR* dirp) {
    struct __dirstream* d = (struct __dirstream*)dirp;
    if (!d) { errno = EBADF; return -1; }
    free(d->buf);
    free(d);
    return 0;
}
