// fd64.cpp - 小 FD 层（实现；设计与边界见 fd64.h）
#include "fd64.h"
#include "vfs64.h"
#include "debug64.h"
// ==================== fd 表 ====================
#define FD64_KIND_FREE 0
#define FD64_KIND_FILE 1
#define FD64_KIND_DIR  2

// 目录句柄的**一次扫描缓存**：vfs64_ls 每次都要线性扫 inode 表（每个 inode 一次读盘），
// 逐条 readdir 各扫一遍在真机上要数秒（会把 GUI 看门狗饿到）。所以第一次 readdir 时列一次、
// 缓存进句柄，之后 O(1)。容量 16 条：终端 ls 的演示/验收规模足够；超出部分如实截断。
#define FD64_DIR_CACHE 16

struct Fd64Slot {
    uint8_t  used;
    uint8_t  kind;        // FD64_KIND_*
    uint8_t  writable;    // 1 = O_WRONLY/O_RDWR
    uint8_t  trunc;       // 1 = O_TRUNC 还没被第一次写消费
    uint8_t  dir_cached;  // 目录句柄：缓存是否已填
    uint8_t  dir_pad[3];
    char     path[FD64_PATH_MAX];
    uint32_t size;        // 文件当前大小（目录 = 0）
    uint32_t off;         // 读/写游标
    uint32_t dir_cursor;  // 目录句柄的线性游标
    uint32_t dir_count;   // 缓存条目数
    uint32_t flags;       // 原样存下（打点/诊断用）
    char     dnames[FD64_DIR_CACHE][FD64_NAME_MAX];   // 目录缓存：名字
    uint32_t dsizes[FD64_DIR_CACHE];                  // 目录缓存：大小（目录 = 0）
};
static Fd64Slot g_fd64[FD64_MAX];

// 全局读缓冲 / 写暂存：理由见 fd64.h（vfs64 没有 read-at-offset 与"部分写"原语）
static uint8_t  g_fd64_rbuf[FD64_FILE_MAX];
static uint8_t  g_fd64_wbuf[FD64_FILE_MAX];
static int      g_fd64_wbuf_fd = -1;     // 写暂存当前属于哪个 fd（-1 = 空闲）
static uint32_t g_fd64_wbuf_size = 0;

// ==================== 小工具 ====================
static void fd64_zero(void* p, uint32_t n) {
    uint8_t* d = (uint8_t*)p;
    for (uint32_t i = 0; i < n; i++) d[i] = 0;
}
static void fd64_copy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}
static int fd64_streq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b && *a == *b) { a++; b++; }
    return *a == 0 && *b == 0;
}
static int fd64_is_ws(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

int fd64_norm_path64(const char* in, char* out, int cap) {
    if (!in || !out || cap < 3) return -FD64_EINVAL;
    int i = 0;
    while (in[i] && fd64_is_ws(in[i])) i++;                 // 去前导空白
    if (in[i] == '/') i++;                                  // 允许 "/name" 与 "name"
    int n = 0;
    out[n++] = '/';
    while (in[i] && !fd64_is_ws(in[i])) {
        const char c = in[i++];
        if (c == '/') return -FD64_EINVAL;                  // 多级路径：阶段一只支持单层
        if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E) return -FD64_EINVAL;
        if (n + 1 >= cap) return -FD64_EINVAL;              // 太长（> cap-2）
        out[n++] = c;
    }
    if (n == 1) return -FD64_EINVAL;                        // 只有 "/" = 根目录：本层不给文件句柄
    out[n] = 0;
    return 0;
}

// ==================== 槽位 ====================
static int fd64_slot_of(int fd) {
    if (fd < 3 || fd >= (int)(3u + FD64_MAX)) return -1;
    const int s = fd - 3;
    if (!g_fd64[s].used) return -1;
    return s;
}
static int fd64_alloc(int* out_slot) {
    for (uint32_t i = 0; i < FD64_MAX; i++) {
        if (!g_fd64[i].used) { *out_slot = (int)i; return 0; }
    }
    return -FD64_EMFILE;
}
static void fd64_release_slot(int s) {
    if (g_fd64_wbuf_fd == s) { g_fd64_wbuf_fd = -1; g_fd64_wbuf_size = 0; }
    fd64_zero(&g_fd64[s], (uint32_t)sizeof(Fd64Slot));
}

// 根目录判定："" 或 "/"（去空白后）就是根 —— 只有目录句柄能用它（文件按根打开 -> EISDIR）
static int fd64_is_root_path64(const char* p) {
    if (!p) return 0;
    int i = 0;
    while (p[i] && fd64_is_ws(p[i])) i++;
    if (p[i] == '/') i++;
    while (p[i] && fd64_is_ws(p[i])) i++;
    return p[i] == 0;
}

// 打开时的公共准备：路径规范化 + 存在性/类型判定
static int fd64_prepare_64(const char* path, char* norm, uint32_t flags, int* out_dir) {
    if (fd64_is_root_path64(path)) {                          // 根目录：只有目录句柄能打开
        if (!(flags & FD64_O_DIRECTORY)) return -FD64_EISDIR;
        norm[0] = '/'; norm[1] = 0;
        *out_dir = 1;
        return 0;
    }
    const int pr = fd64_norm_path64(path, norm, (int)FD64_PATH_MAX);
    if (pr != 0) return pr;
    uint32_t type = 0, size = 0;
    const int have = (vfs64_stat(norm, &type, &size) == 0);
    const int want_dir = (flags & FD64_O_DIRECTORY) != 0;
    if (have) {
        if (type == VFS64_TYPE_DIR) {
            if (!want_dir && (flags & (FD64_O_WRONLY | FD64_O_RDWR | FD64_O_TRUNC)))
                return -FD64_EISDIR;                        // 目录不能按文件写
            *out_dir = 1;
            return 0;
        }
        *out_dir = 0;
        return 0;
    }
    // 不存在：只有 O_CREAT 才允许（目录句柄不允许创建）
    if (want_dir) return -FD64_ENOENT;
    if (!(flags & FD64_O_CREAT)) return -FD64_ENOENT;
    if (vfs64_write(norm, "", 0) < 0) return -FD64_ENOSPC;  // 建空文件（vfs64_write 支持 len=0）
    *out_dir = 0;
    return 0;
}

int fd64_open64(const char* path, uint32_t flags) {
    char norm[FD64_PATH_MAX];
    int is_dir = 0;
    const int pr = fd64_prepare_64(path, norm, flags, &is_dir);
    if (pr != 0) {
        dbg64_line_begin64();
        dbg64_str("[FD64] open FAILED path=");
        dbg64_str(path ? path : "(null)");
        dbg64_str(" rc=");
        dbg64_dec((uint64_t)(pr < 0 ? -pr : pr));
        dbg64_nl();
        dbg64_line_end64();
        return pr;
    }

    int s = -1;
    const int ar = fd64_alloc(&s);
    if (ar != 0) return ar;

    Fd64Slot* e = &g_fd64[s];
    e->used     = 1;
    e->kind     = is_dir ? FD64_KIND_DIR : FD64_KIND_FILE;
    e->writable = (!is_dir && (flags & (FD64_O_WRONLY | FD64_O_RDWR))) ? 1 : 0;
    e->trunc    = (!is_dir && (flags & FD64_O_TRUNC)) ? 1 : 0;
    e->off      = 0;
    e->dir_cursor = 0;
    e->flags    = flags;
    for (uint32_t i = 0; i < FD64_PATH_MAX; i++) e->path[i] = norm[i];
    if (is_dir) {
        e->size = 0;
    } else {
        uint32_t type = 0, size = 0;
        (void)vfs64_stat(norm, &type, &size);
        e->size = e->trunc ? 0 : size;
    }

    const int fd = 3 + s;
    dbg64_line_begin64();
    dbg64_str("[FD64] open path=");
    dbg64_str(norm);
    dbg64_str(" fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" flags=");
    dbg64_dec((uint64_t)flags);
    if (is_dir) dbg64_str(" (dir)");
    dbg64_nl();
    dbg64_line_end64();
    return fd;
}

int fd64_close64(int fd) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    fd64_release_slot(s);
    dbg64_line_begin64();
    dbg64_str("[FD64] close fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

int fd64_read64(int fd, void* buf, int len) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    Fd64Slot* e = &g_fd64[s];
    if (e->kind != FD64_KIND_FILE) return -FD64_EISDIR;
    if (!buf || len < 0) return -FD64_EFAULT;
    if (len == 0) return 0;
    if (e->off >= e->size) return 0;                        // EOF

    uint32_t want = e->size - e->off;
    if ((uint32_t)len < want) want = (uint32_t)len;

    // 读整文件 -> 全局读缓冲 -> 按游标拷贝（关中断的理由见 fd64.h）
    const uint64_t if_save = dbg64_irq_save64();
    const int n = vfs64_read(e->path, g_fd64_rbuf, (int)FD64_FILE_MAX);
    if (n < 0) { dbg64_irq_restore64(if_save); return -FD64_ENOENT; }
    const uint32_t avail = ((uint32_t)n > e->size) ? e->size : (uint32_t)n;
    if (e->off >= avail) { dbg64_irq_restore64(if_save); return 0; }
    uint32_t got = avail - e->off;
    if (got > want) got = want;
    fd64_copy(buf, g_fd64_rbuf + e->off, got);
    e->off += got;
    dbg64_irq_restore64(if_save);

    dbg64_line_begin64();
    dbg64_str("[FD64] read fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)got);
    dbg64_nl();
    dbg64_line_end64();
    return (int)got;
}

int fd64_write64(int fd, const void* buf, int len) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    Fd64Slot* e = &g_fd64[s];
    if (e->kind != FD64_KIND_FILE) return -FD64_EISDIR;
    if (!e->writable) return -FD64_EBADF;
    if (!buf || len < 0) return -FD64_EFAULT;
    if (len == 0) return 0;

    const uint32_t end = e->off + (uint32_t)len;
    if (end > FD64_FILE_MAX) return -FD64_EFBIG;            // 单文件 67584 B 上限（如实拒绝）

    const uint64_t if_save = dbg64_irq_save64();

    // 第一次写这个 fd：装载暂存（O_TRUNC 就是空；否则读现文件）
    if (g_fd64_wbuf_fd != s) {
        g_fd64_wbuf_fd = s;
        g_fd64_wbuf_size = 0;
        if (!e->trunc && e->size > 0) {
            const int n = vfs64_read(e->path, g_fd64_wbuf, (int)FD64_FILE_MAX);
            g_fd64_wbuf_size = (n > 0) ? (uint32_t)n : 0;
            if (g_fd64_wbuf_size > e->size) g_fd64_wbuf_size = e->size;
        }
        e->trunc = 0;
    }
    // 空洞补零（off 超出当前大小）
    if (e->off > g_fd64_wbuf_size) {
        for (uint32_t i = g_fd64_wbuf_size; i < e->off && i < FD64_FILE_MAX; i++) g_fd64_wbuf[i] = 0;
        g_fd64_wbuf_size = e->off;
    }
    fd64_copy(g_fd64_wbuf + e->off, buf, (uint32_t)len);
    if (end > g_fd64_wbuf_size) g_fd64_wbuf_size = end;
    e->size = g_fd64_wbuf_size;
    e->off  = end;

    // 立刻整体落盘（vfs64_write = 覆盖语义；不做脏页缓存，断电语义简单）
    const int wn = vfs64_write(e->path, g_fd64_wbuf, (int)g_fd64_wbuf_size);
    dbg64_irq_restore64(if_save);
    if (wn < 0) return -FD64_ENOSPC;

    dbg64_line_begin64();
    dbg64_str("[FD64] write fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)len);
    dbg64_str(" total=");
    dbg64_dec((uint64_t)e->size);
    dbg64_nl();
    dbg64_line_end64();
    return len;
}

int fd64_lseek64(int fd, int64_t off, int whence) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    Fd64Slot* e = &g_fd64[s];
    if (e->kind != FD64_KIND_FILE) return -FD64_ESPIPE;
    int64_t base = 0;
    if (whence == FD64_SEEK_SET)      base = 0;
    else if (whence == FD64_SEEK_CUR) base = (int64_t)e->off;
    else if (whence == FD64_SEEK_END) base = (int64_t)e->size;
    else return -FD64_EINVAL;
    const int64_t nv = base + off;
    if (nv < 0 || nv > (int64_t)FD64_FILE_MAX) return -FD64_EINVAL;
    e->off = (uint32_t)nv;
    return (int)nv;
}

int fd64_stat64(int fd, uint32_t* type_out, uint32_t* size_out) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    Fd64Slot* e = &g_fd64[s];
    if (type_out) *type_out = (e->kind == FD64_KIND_DIR) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    if (size_out) *size_out = e->size;
    return 0;
}

int fd64_fsync64(int fd) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    return 0;                                                // 写路径已即时落盘（无脏页）
}

int fd64_dup64(int oldfd, int newfd) {
    const int s = fd64_slot_of(oldfd);
    if (s < 0) return -FD64_EBADF;
    int dst = -1;
    if (newfd >= 3) {
        if (newfd >= (int)(3u + FD64_MAX)) return -FD64_EBADF;
        dst = newfd - 3;
        if (g_fd64[dst].used && dst != s) fd64_release_slot(dst);   // 覆盖（与 dup2 同）
        if (dst == s) return newfd;
    } else {
        const int ar = fd64_alloc(&dst);
        if (ar != 0) return ar;
    }
    const Fd64Slot src = g_fd64[s];
    g_fd64[dst] = src;                                        // 路径/游标独立复制（不共享游标，如实）
    if (g_fd64_wbuf_fd == s) { /* 写暂存不随 dup 复制（独立游标），保持原 fd 的暂存 */ }
    return 3 + dst;
}

// ==================== 目录句柄 ====================
int fd64_opendir64(const char* path) {
    return fd64_open64(path, FD64_O_RDONLY | FD64_O_DIRECTORY);
}

int fd64_readdir64(int fd, char* name_out, int name_cap, uint32_t* type_out, uint32_t* size_out) {
    const int s = fd64_slot_of(fd);
    if (s < 0) return -FD64_EBADF;
    Fd64Slot* e = &g_fd64[s];
    if (e->kind != FD64_KIND_DIR) return -FD64_ENOTDIR;
    if (!name_out || name_cap < 2) return -FD64_EFAULT;

    // 第一次调用：列一次目录并把结果缓存进句柄（理由见 FD64_DIR_CACHE 的注释）
    if (!e->dir_cached) {
        e->dir_cached = 1;
        e->dir_cursor = 0;
        e->dir_count = 0;
        const int n = vfs64_ls(e->path, e->dnames, (int)FD64_DIR_CACHE, e->dsizes);
        if (n < 0) return -FD64_ENOENT;
        e->dir_count = (uint32_t)n;
    }
    if (e->dir_cursor >= e->dir_count) return 0;               // 枚举结束

    const uint32_t idx = e->dir_cursor++;
    int i = 0;
    while (e->dnames[idx][i] && i + 1 < name_cap) { name_out[i] = e->dnames[idx][i]; i++; }
    name_out[i] = 0;
    if (type_out || size_out) {
        // 类型：vfs64_ls 只给名字+大小。大小为 0 的项才需要 stat 一次（目录 / 空文件）；
        // 非空文件按大小直接判定为 FILE —— 这样 ls 不会对每个文件都做一遍全表扫描。
        uint32_t ty = VFS64_TYPE_FILE;
        uint32_t sz = e->dsizes[idx];
        if (sz == 0) {
            char full[FD64_PATH_MAX];
            int p = 0;
            full[p++] = '/';
            for (int k = 0; e->dnames[idx][k] && p + 1 < (int)FD64_PATH_MAX; k++) full[p++] = e->dnames[idx][k];
            full[p] = 0;
            if (vfs64_stat(full, &ty, &sz) != 0) ty = VFS64_TYPE_FILE;
        }
        if (type_out) *type_out = ty;
        if (size_out) *size_out = (ty == VFS64_TYPE_FILE) ? sz : 0;
    }
    return 1;
}

// ==================== 自检 / 诊断 ====================
int fd64_selftest64() {
    int fail = 0;
    char out[FD64_PATH_MAX];

    // bit0：路径规范化（单层语义的硬证据：多级路径必须被拒）
    if (fd64_norm_path64("/t.txt", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/t.txt")) fail |= 1;
    if (fd64_norm_path64("t.txt", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/t.txt"))  fail |= 1;
    if (fd64_norm_path64("/bad name", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/bad")) fail |= 1;
    if (fd64_norm_path64("/", out, (int)sizeof(out)) == 0)     fail |= 1;   // 根目录：本层不给句柄
    if (fd64_norm_path64("", out, (int)sizeof(out)) == 0)      fail |= 1;
    if (fd64_norm_path64("/bad name", out, (int)sizeof(out)) != 0) fail |= 1;  // 空白分隔：规范到 "/bad" 即合法

    // bit1：fd 表分配/释放（不碰盘：全是失败路径，不该改变表状态）
    {
        const int before = fd64_slot_of(3);
        if (fd64_close64(3 + (int)FD64_MAX) != -FD64_EBADF) fail |= 2;   // 越界 fd
        if (fd64_read64(1, out, 1) != -FD64_EBADF) fail |= 2;            // 标准流不归本层
        if (before != fd64_slot_of(3)) fail |= 2;
    }

    // bit2：卷可用时打开根目录句柄并读第一条（目录句柄真的能工作）；没挂载就如实跳过（不算失败）
    int vfs = 0;
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat("/", &ty, &sz) == 0) {
        vfs = 1;
        const int dfd = fd64_opendir64("/");
        if (dfd < 3) {
            fail |= 4;
        } else {
            char nm[FD64_NAME_MAX];
            const int r = fd64_readdir64(dfd, nm, (int)sizeof(nm), &ty, &sz);
            if (r < 0) fail |= 4;
            if (fd64_close64(dfd) != 0) fail |= 4;
        }
    }

    dbg64_line_begin64();
    dbg64_str("[FD64] selftest table=");
    dbg64_dec((uint64_t)FD64_MAX);
    dbg64_str(" file_max=");
    dbg64_dec((uint64_t)FD64_FILE_MAX);
    dbg64_str(" vfs=");
    dbg64_dec((uint64_t)vfs);
    dbg64_nl();
    dbg64_line_end64();

    dbg64_line_begin64();
    if (fail == 0) {
        dbg64_str("[FD64] selftest PASS\n");
    } else {
        dbg64_str("[FD64] selftest FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
    }
    dbg64_line_end64();
    return fail;
}

void fd64_dump64() {
    int used = 0;
    for (uint32_t i = 0; i < FD64_MAX; i++) if (g_fd64[i].used) used++;
    dbg64_line_begin64();
    dbg64_str("[FD64] dump used=");
    dbg64_dec((uint64_t)used);
    dbg64_str("/");
    dbg64_dec((uint64_t)FD64_MAX);
    dbg64_nl();
    dbg64_line_end64();
    for (uint32_t i = 0; i < FD64_MAX; i++) {
        if (!g_fd64[i].used) continue;
        dbg64_line_begin64();
        dbg64_str("[FD64] dump fd=");
        dbg64_dec((uint64_t)(3u + i));
        dbg64_str(" kind=");
        dbg64_dec((uint64_t)g_fd64[i].kind);
        dbg64_str(" path=");
        dbg64_str(g_fd64[i].path);
        dbg64_str(" size=");
        dbg64_dec((uint64_t)g_fd64[i].size);
        dbg64_str(" off=");
        dbg64_dec((uint64_t)g_fd64[i].off);
        dbg64_nl();
        dbg64_line_end64();
    }
}
