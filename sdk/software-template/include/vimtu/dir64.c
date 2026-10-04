/* sdk/software-template/include/vimtu/dir64.c - dir64.h 的实现（薄转发，纯用户态）
 *
 * 只用 `user/lib/vimtu64.h` 里的 `__v64_int80`（int 0x80；rdi/rsi/rdx/r10 = 参数 1..4；
 * 返回值负数 = 内核错误码）。不引任何其它东西 —— 可以直接：
 *     clang ... -I user/lib -I sdk/software-template/include/vimtu myapp.c dir64.c
 *
 * 内存：一个 VimtuDir 里带 **一批** 目录记录的缓冲（VIMTU_DIR_BATCH 条，栈上/静态分配由调用方
 * 的 malloc 决定 —— 本运行时的堆只有 4 KiB，所以这里用固定大小，不做动态增长）。
 */
#include "dir64.h"

#include "vimtu64.h"      /* -I user/lib：__v64_int80 */

#define VIMTU_DIR_BATCH_BYTES  ((int)(VIMTU_DIR_ENT_MAX * 8))   /* 一次最多读 8 条 */

struct VimtuDir {
    int   fd;
    int   off;                                /* 缓冲里已消费到的字节偏移 */
    int   len;                                /* 缓冲里有效字节数 */
    char  buf[VIMTU_DIR_BATCH_BYTES];
};

static struct VimtuDir g_dir_pool[4];         /* 模板级：最多同时开 4 个（内核 fd 表上限 8） */
static int             g_dir_used[4];

static unsigned rd_u16(const char* p) { return (unsigned)((unsigned char)p[0] | ((unsigned char)p[1] << 8)); }
static unsigned rd_u32(const char* p) {
    return (unsigned)((unsigned char)p[0] | ((unsigned char)p[1] << 8) |
                      ((unsigned char)p[2] << 16) | ((unsigned char)p[3] << 24));
}

struct VimtuDir* vimtu_dir_open(const char* path, int* err_out) {
    const long rc = __v64_int80(VIMTU_DIR64_OPEN_NR, (long)(unsigned long)path, 0, 0, 0);
    if (rc < 0) {
        if (err_out) *err_out = (int)rc;
        return (struct VimtuDir*)0;
    }
    for (int i = 0; i < 4; i++) {
        if (!g_dir_used[i]) {
            g_dir_used[i] = 1;
            g_dir_pool[i].fd = (int)rc;
            g_dir_pool[i].off = 0;
            g_dir_pool[i].len = 0;
            return &g_dir_pool[i];
        }
    }
    (void)__v64_int80(VIMTU_DIR64_CLOSE_NR, rc, 0, 0, 0);   /* 池满：别泄漏内核句柄 */
    if (err_out) *err_out = -24;                            /* EMFILE，如实报 */
    return (struct VimtuDir*)0;
}

int vimtu_dir_read(struct VimtuDir* d, struct VimtuDirent* out) {
    if (!d || !out) return -22;
    if (d->off >= d->len) {                                 /* 缓冲空了 -> 再问内核要一批 */
        const long got = __v64_int80(VIMTU_DIR64_READ_NR, d->fd,
                                     (long)(unsigned long)d->buf, VIMTU_DIR_BATCH_BYTES, 0);
        if (got < 0) return (int)got;
        if (got == 0) { d->len = 0; d->off = 0; return 0; }  /* EOF */
        d->len = (int)got;
        d->off = 0;
    }
    const char* rec = d->buf + d->off;
    const unsigned name_len = rd_u16(rec);
    if (name_len == 0 || name_len > 31) return -22;          /* 内核布局不会这样：如实报错，不猜 */
    unsigned n = name_len;
    if (n > sizeof(out->name) - 1) n = sizeof(out->name) - 1;
    for (unsigned i = 0; i < n; i++) out->name[i] = rec[VIMTU_DIR_ENT_HDR + i];
    out->name[n] = 0;
    out->kind  = (rec[2] == (char)VIMTU_DIR_KIND_DIR) ? VIMTU_DIR_KIND_DIR : VIMTU_DIR_KIND_FILE;
    out->size  = rd_u32(rec + 4);
    out->mtime = rd_u32(rec + 8);
    d->off += (int)(VIMTU_DIR_ENT_HDR + name_len + 1);       /* 记录 = 12 + name_len + 1 */
    return 1;
}

int vimtu_dir_close(struct VimtuDir* d) {
    if (!d) return -9;
    const long rc = __v64_int80(VIMTU_DIR64_CLOSE_NR, d->fd, 0, 0, 0);
    for (int i = 0; i < 4; i++) if (&g_dir_pool[i] == d) g_dir_used[i] = 0;
    return (int)rc;
}
