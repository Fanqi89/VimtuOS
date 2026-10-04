/* vimtu_dirent.c - VimtuOS64：**用户态**的 opendir/readdir/closedir
 *
 * ★ 本批：目录枚举改走**内核的实时目录** —— 自有 ABI（int 0x80）号 50/51/52
 *   dir_open/dir_read/dir_close（语义 / 记录布局 / 错误码 / 打点的唯一说明见
 *   kernel/syscall64.h 的"目录枚举 ABI（50/51/52）"段）。于是 busybox 的 ls/find/du 能看到
 *   **本次开机里** mkdir/cp/tar 出来的文件 —— 以前走构建期快照 /etc/vimtu.dirs，新文件看不见。
 *
 * 为什么"内核加这一组号"是合规的（架构宪法四条件怎么落地在这里）：
 *   ① 内核**已有**目录枚举能力：vfs64_opendir64/readdir64/closedir64（游标式遍历）+ fs64 分派 +
 *      fd64 的**每进程目录 fd**（fd64_opendir64 / fd64_readdir64 / fd64_close64）。内核侧这三号
 *      只是把这个**既有**入口暴露给 ring3，没有新写一份枚举器。
 *   ② ring3 之前**没有任何等价通路**：目录枚举在内核里只对内核自己开放，ring3 唯一的旁路是终端
 *      服务的"代列协议"（内核邮箱 RPC，见 kernel/terminal64.cpp 的 sh64_serve64），而那一页只在
 *      `shell` 命令拉起来的进程里映射，`run` 出来的子进程 execve 之后就被释放了。busybox 是普通
 *      ring3 进程 —— 新 ABI 之前只能吃构建期快照。
 *   ③ 内核侧**只加校验 + 转发**：dir_open -> fd64_opendir64（路径解析 + vfs64 权限判定），
 *      dir_read -> fd64_readdir64（+ fs64_stat64 取 mtime，与 shell `ls -l` 同一口径），
 *      dir_close -> fd64_close64。内核不做排序/过滤/递归/格式化。
 *   ④ **策略全在用户态**：本文件只负责"把内核的定长记录流包成 busybox 认的 DIR/readdir 语义"；
 *      排序 / 过滤 / 递归（find）/ 人类可读格式化都是 busybox 自己的事。
 *
 * 构建期快照 /etc/vimtu.dirs 作为**兜底**保留：只在"新 ABI 探测不可用"时退回（打点
 *   `[DIR64] fallback snapshot reason=...`）。探测 = 第一次 opendir 时对 "/" 调一次 dir_open，
 *   失败（-1 = 内核没有这三号）就整进程退回快照；普通错误（ENOENT/EACCES/...）**不**触发兜底。
 *   验收用 /tmp/.dir64-off 强制关闭新 ABI 来走这条分支（见 tests/dir64_test.py 的第 ⑥ 项）。
 *
 * 链接方式：本文件作为普通目标文件放进 busybox 的链接命令行 —— 目标文件里的强符号优先于静态库
 *   musl/libc.a 里的同名成员，于是 busybox（以及 musl 的 scandir 内部调用）走的就是这份实现。
 *
 * 与内核的字节级契约（改一处必须同时改 kernel/syscall64.h 的同一段）：
 *   int $0x80 自有 ABI：rax = 号；rdi/rsi/rdx = 参数 1/2/3；返回值在 rax（负数 = -errno）。
 *     50 dir_open(path)          -> fd(>=3) | -errno
 *     51 dir_read(fd,out,cap)    -> 写入字节数(>0) | 0(EOF) | -errno
 *     52 dir_close(fd)           -> 0 | -errno
 *   记录（缓冲里连续排布）：{u16 name_len; u8 kind; u8 rsvd; u32 size; u32 mtime; char name[]}
 *     长度 = 12 + name_len + 1（name 有结尾 NUL）；kind: 1 = 普通文件 / 2 = 目录。
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VIMTU_DIRIDX    "/etc/vimtu.dirs"     /* 构建期快照（只在兜底分支用） */
#define VIMTU_OFFFILE   "/tmp/.dir64-off"     /* 验收钩子：存在 = 强制走兜底快照 */
#define VIMTU_DUMPFILE  "/tmp/.dir64-dump"    /* 验收钩子：存在 = 把每条内核记录打到 fd 2 */
#define VIMTU_NAMEMAX   255
#define VIMTU_CHUNK     4096
#define VIMTU_PAGE      2048                  /* 一批内核记录的缓冲 */

/* 自有 ABI（int 0x80）：号 50/51/52；记录布局见上面的契约。 */
#define VIMTU_DIR64_OPEN    50
#define VIMTU_DIR64_READ    51
#define VIMTU_DIR64_CLOSE   52
#define VIMTU_DIR64_HDR     12                /* 定长头字节数（name 从 +12 起） */
#define VIMTU_DIR64_KIND_DIR 2                /* kind == 2 = 目录 */

/* musl 的 <dirent.h> 只给 `typedef struct __dirstream DIR;`（不完整类型）。这里把它补全：
 * busybox 只把 DIR* 当不透明指针用（opendir/readdir/closedir 三个入口都在本文件），所以内部
 * 布局由我们说。**第一个字段故意是 int fd**：musl 的 dirfd() 读的就是偏移 0（这里给真值 / -1）。 */
struct __dirstream {
    int            fd;                        /* 新 ABI 的目录句柄（<0 = 兜底模式） */
    int            use64;                     /* 1 = 走新 ABI；0 = 走构建期快照 */
    struct dirent  ent;                       /* readdir 的返回值指向这里 */
    unsigned long  ino;
    unsigned char  page[VIMTU_PAGE];          /* 新 ABI：一批记录的缓冲 */
    int            page_len;
    int            page_off;
    char*          buf;                       /* 兜底：整份 /etc/vimtu.dirs（堆） */
    size_t         buf_len;
    size_t         pos;
    char           want[512];                 /* 兜底：规范化后的目录绝对路径 */
    size_t         want_len;
};

/* ------------------------------------------------------------------ 打点（fd 2 = 只串口） */
struct vimtu_line { char b[224]; int n; };

static void vl_put(struct vimtu_line* l, const char* s) {
    while (*s && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = *s++;
}
static void vl_num(struct vimtu_line* l, long v) {
    char t[24];
    int i = 0;
    unsigned long u;
    if (v < 0) { vl_put(l, "-"); u = (unsigned long)(-v); } else { u = (unsigned long)v; }
    do { t[i++] = (char)('0' + (int)(u % 10u)); u /= 10u; } while (u != 0 && i < (int)sizeof(t));
    while (i > 0 && l->n < (int)sizeof(l->b) - 1) l->b[l->n++] = t[--i];
}
static void vl_flush(struct vimtu_line* l) {
    if (l->n <= 0) return;
    (void)write(2, l->b, (size_t)l->n);
}

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

static int vimtu_file_exists64(const char* p) {
    const int fd = open(p, O_RDONLY);
    if (fd < 0) return 0;
    (void)close(fd);
    return 1;
}

/* ------------------------------------------------------------------ 新 ABI 的探测 / 开关 */
static int g_dir64_state = -1;    /* -1 = 未知；0 = 不可用（走兜底）；1 = 可用 */
static int g_dir64_dump  = -1;    /* -1 = 未知；0 = 关；1 = 开（验收钩子） */

/* 自有 ABI（int 0x80）：rax=nr，rdi/rsi/rdx = 参数 1/2/3，返回值在 rax（负数 = -errno）。
 * 用显式 clobber + 固定寄存器，不让编译器自己挑（与 user/svc/drvdemo.c 同一套写法）。 */
static long vimtu_sc3(long nr, long a, long b, long c) {
    long r;
    __asm__ volatile("int $0x80"
                     : "=a"(r)
                     : "a"(nr), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

/* 新 ABI 可用吗？不可用时**整进程**退回构建期快照并打点（只打一次，防刷屏）。 */
static int vimtu_dir64_ready(void) {
    struct vimtu_line l = {{0}, 0};
    if (g_dir64_state >= 0) return g_dir64_state;
    g_dir64_state = 0;
    if (vimtu_file_exists64(VIMTU_OFFFILE)) {
        vl_put(&l, "[DIR64] fallback snapshot reason=disabled(");
        vl_put(&l, VIMTU_OFFFILE);
        vl_put(&l, ") use64=0\n");
        vl_flush(&l);
        return 0;
    }
    {
        const long fd = vimtu_sc3(VIMTU_DIR64_OPEN, (long)"/", 0, 0);
        if (fd >= 3) {
            (void)vimtu_sc3(VIMTU_DIR64_CLOSE, fd, 0, 0);
            g_dir64_state = 1;
        } else {
            vl_put(&l, "[DIR64] fallback snapshot reason=probe-failed rc=");
            vl_num(&l, fd);
            vl_put(&l, " use64=0\n");
            vl_flush(&l);
        }
    }
    return g_dir64_state;
}

static int vimtu_dump_on(void) {
    if (g_dir64_dump < 0) g_dir64_dump = vimtu_file_exists64(VIMTU_DUMPFILE) ? 1 : 0;
    return g_dir64_dump;
}
/* 验收钩子：把内核给的**原始记录字段**打出来（证明布局：名字/类型/大小/mtime 都来自内核）。 */
static void vimtu_dump_ent64(const char* nm, unsigned int kind, uint32_t size, uint32_t mtime) {
    static int budget = 64;
    struct vimtu_line l = {{0}, 0};
    if (budget <= 0) return;
    budget--;
    vl_put(&l, "[DIR64] ent name=");
    vl_put(&l, nm);
    vl_put(&l, " kind=");
    vl_num(&l, (long)kind);
    vl_put(&l, " size=");
    vl_num(&l, (long)size);
    vl_put(&l, " mtime=");
    vl_num(&l, (long)mtime);
    vl_put(&l, "\n");
    vl_flush(&l);
}

/* ------------------------------------------------------------------ 入口 */
DIR* opendir(const char* name) {
    struct __dirstream* d;
    char norm[512];
    if (vimtu_normalise(name, norm, sizeof(norm)) < 0) { errno = ENOENT; return NULL; }
    if (vimtu_dir64_ready()) {
        const long fd = vimtu_sc3(VIMTU_DIR64_OPEN, (long)norm, 0, 0);
        if (fd < 0) { errno = (int)(-fd); return NULL; }     /* 明确错误码：-ENOENT/-EACCES/-ENOTDIR/... */
        d = (struct __dirstream*)calloc(1, sizeof(*d));
        if (!d) {
            (void)vimtu_sc3(VIMTU_DIR64_CLOSE, fd, 0, 0);
            errno = ENOMEM;
            return NULL;
        }
        d->fd = (int)fd;
        d->use64 = 1;
        d->ino = 1;
        return (DIR*)d;
    }
    /* ---- 兜底：构建期快照 /etc/vimtu.dirs ---- */
    d = (struct __dirstream*)calloc(1, sizeof(*d));
    if (!d) { errno = ENOMEM; return NULL; }
    d->fd = -1;
    d->use64 = 0;
    if (vimtu_slurp(VIMTU_DIRIDX, &d->buf, &d->buf_len) != 0) {
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

/* 新 ABI：从内核的记录流里取一条（会自动补下一批）。 */
static struct dirent* vimtu_readdir64(struct __dirstream* d) {
    for (;;) {
        if (d->page_off < d->page_len) {
            const unsigned char* p = d->page + d->page_off;
            const int left = d->page_len - d->page_off;
            unsigned int nl;
            unsigned int kind;
            unsigned int maxn;
            uint32_t size;
            uint32_t mtime;
            const char* nm;
            size_t copy;
            if (left < VIMTU_DIR64_HDR + 1) { d->page_off = d->page_len; break; }   /* 残片：丢弃 */
            nl = (unsigned int)p[0] | ((unsigned int)p[1] << 8);
            kind = (unsigned int)p[2];
            maxn = (unsigned int)(left - VIMTU_DIR64_HDR - 1);
            if (nl > maxn) nl = maxn;
            memcpy(&size, p + 4, 4);
            memcpy(&mtime, p + 8, 4);
            nm = (const char*)(p + VIMTU_DIR64_HDR);
            copy = nl > VIMTU_NAMEMAX ? VIMTU_NAMEMAX : nl;
            memset(&d->ent, 0, sizeof(d->ent));
            d->ent.d_ino = d->ino++;
            d->ent.d_off = (long)d->page_off;
            d->ent.d_reclen = (unsigned short)(offsetof(struct dirent, d_name) + copy + 1);
            d->ent.d_type = (kind == VIMTU_DIR64_KIND_DIR) ? DT_DIR : DT_REG;
            memcpy(d->ent.d_name, nm, copy);
            d->ent.d_name[copy] = 0;
            if (vimtu_dump_on()) vimtu_dump_ent64(d->ent.d_name, kind, size, mtime);
            d->page_off += VIMTU_DIR64_HDR + (int)nl + 1;
            return &d->ent;
        }
        {
            const long n = vimtu_sc3(VIMTU_DIR64_READ, (long)d->fd, (long)d->page, (long)sizeof(d->page));
            if (n < 0) { errno = (int)(-n); return NULL; }
            if (n == 0) { errno = 0; return NULL; }             /* EOF（POSIX） */
            d->page_len = (int)n;
            d->page_off = 0;
        }
    }
    errno = 0;
    return NULL;
}

/* 兜底：线性扫构建期快照（与上一版逐字节相同的行为）。 */
static struct dirent* vimtu_readdir_snap(struct __dirstream* d) {
    while (d->pos < d->buf_len) {
        const size_t p = d->pos;
        size_t e = p;
        size_t i, ds, de, ns, nl;
        while (e < d->buf_len && d->buf[e] != '\n') e++;
        d->pos = (e < d->buf_len) ? e + 1 : e;
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
            d->ent.d_type = DT_UNKNOWN;      /* 兜底快照没有类型：让调用方自己去 stat（内核 stat 是真的） */
            memcpy(d->ent.d_name, d->buf + ns, copy);
            d->ent.d_name[copy] = 0;
            return &d->ent;
        }
    }
    errno = 0;                                                   /* EOF（POSIX） */
    return NULL;
}

struct dirent* readdir(DIR* dirp) {
    struct __dirstream* d = (struct __dirstream*)dirp;
    if (!d) { errno = EBADF; return NULL; }
    if (d->use64) return vimtu_readdir64(d);
    return vimtu_readdir_snap(d);
}

int closedir(DIR* dirp) {
    struct __dirstream* d = (struct __dirstream*)dirp;
    int rc = 0;
    if (!d) { errno = EBADF; return -1; }
    if (d->use64 && d->fd >= 0) {
        const long r = vimtu_sc3(VIMTU_DIR64_CLOSE, (long)d->fd, 0, 0);
        if (r < 0) { errno = (int)(-r); rc = -1; }
    }
    if (d->buf) free(d->buf);
    free(d);
    return rc;
}
