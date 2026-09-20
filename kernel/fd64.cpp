// fd64.cpp - FD 层实现（每进程 fd 表 + 引用计数打开文件对象 + O_APPEND + pipe）
//
// 设计与边界见 kernel/fd64.h（先读那份；这里只记实现上的三个要点）：
//   1) 表在池里（FD64_TABLE_MAX 张），对象在池里（FD64_OPEN_MAX 个）。fd 槽只是**指针**，
//      dup/fork 都是"复制指针 + refs++"，close 是 "refs--（归零才释放）" —— 偏移游标在对象里，
//      所以共享语义是**结构上必然**的，不靠约定。
//   2) 每次读/写都把整个文件过一遍内存（vfs64 没有 read-at-offset / 部分写）。写路径 = 读全文
//      -> 按游标（或 O_APPEND 的"末尾"）打补丁 -> 整体写回。期间关中断（单 CPU 防穿插）。
//   3) 没有进程上下文时（任务 0 桌面/终端、安装介质内核）一律用内核表：终端就是这张表的主人。
#include "fd64.h"
#include "vfs64.h"
#include "debug64.h"

// ---- proc64 的弱引用：进程表（没有进程上下文 -> 返回 nullptr -> 用内核表）----
// proc64.cpp 只在系统内核里链接（安装介质内核没有进程），所以这里必须弱引用 + 判空。
extern "C" FdTable64* proc64_fdtab_of_current64() __attribute__((weak));

// ==================== 对象 ====================
#define FD64_KIND_FREE 0
#define FD64_KIND_FILE 1
#define FD64_KIND_DIR  2
#define FD64_KIND_PIPE_R 3
#define FD64_KIND_PIPE_W 4

// 目录句柄的**一次扫描缓存**：vfs64_ls 每次都要线性扫 inode 表（每个 inode 一次读盘），
// 逐条 readdir 各扫一遍在真机上要数秒（会把 GUI 看门狗饿到）。所以第一次 readdir 时列一次、
// 缓存进对象，之后 O(1)。容量 16 条：终端 ls 的演示/验收规模足够；超出部分如实截断。
#define FD64_DIR_CACHE 16
#define FD64_DIRC_MAX  4          // 目录缓存池（同时最多 4 个目录句柄有缓存）

struct Pipe64 {
    uint8_t  used;
    uint8_t  pad[3];
    uint32_t refs;            // 所有表槽引用（含 dup/fork 出来的）
    uint32_t writers;         // 还开着的写端数（0 = read 返回 EOF）
    uint32_t readers;
    uint32_t rd, wr, n;       // 环形缓冲读/写下标 + 已缓冲字节
    uint8_t  buf[FD64_PIPE_BYTES];
};

struct OpenFile64 {
    uint32_t refs;            // 引用计数（fd 槽数；close/进程退出各 -1）
    uint32_t flags;           // 打开时的 flags（原样存下，打点/诊断用）
    uint32_t off;             // ★ 共享读写游标（dup/fork 共享的就是它）
    uint32_t size;            // 文件大小缓存（每次读/写/lseek 前重新 stat 刷新）
    uint8_t  used;
    uint8_t  kind;            // FD64_KIND_*
    uint8_t  writable;        // 1 = O_WRONLY/O_RDWR
    uint8_t  append;          // 1 = O_APPEND（每次写定位到末尾）
    char     path[FD64_PATH_MAX];
    int32_t  dirc;            // 目录缓存池下标（-1 = 无）
    Pipe64*  pipe;            // pipe 对象（kind = PIPE_R/PIPE_W）
};

struct DirCache64 {
    uint8_t  used;
    uint8_t  pad[3];
    uint32_t count;
    uint32_t cursor;
    char     names[FD64_DIR_CACHE][FD64_NAME_MAX];
    uint32_t sizes[FD64_DIR_CACHE];
};

// ==================== 表 ====================
struct FdTable64 {
    uint8_t     used;
    uint8_t     is_kernel;
    uint8_t     pad[2];
    OpenFile64* slot[FD64_MAX];      // nullptr = 空槽
};

static FdTable64   g_tables[FD64_TABLE_MAX];
static OpenFile64  g_files[FD64_OPEN_MAX];
static Pipe64      g_pipes[FD64_PIPE_MAX];
static DirCache64  g_dirc[FD64_DIRC_MAX];

// 全局读缓冲 / 写暂存：理由见 fd64.h（vfs64 没有 read-at-offset 与部分写原语）
static uint8_t  g_fd64_rbuf[FD64_FILE_MAX];
static uint8_t  g_fd64_wbuf[FD64_FILE_MAX];

FdTable64* fd64_kernel_table64() {
    // 内核表 = 池里第一张（is_kernel 标记，永不释放）
    if (!g_tables[0].used) { g_tables[0].used = 1; g_tables[0].is_kernel = 1; }
    return &g_tables[0];
}
FdTable64* fd64_current_table64() {
    if (proc64_fdtab_of_current64) {
        FdTable64* t = proc64_fdtab_of_current64();
        if (t) return t;
    }
    return fd64_kernel_table64();
}
FdTable64* fd64_table_alloc64() {
    (void)fd64_kernel_table64();                     // 先把 0 号钉死给内核表
    for (uint32_t i = 1; i < FD64_TABLE_MAX; i++) {
        if (!g_tables[i].used) {
            FdTable64* t = &g_tables[i];
            for (uint32_t k = 0; k < FD64_MAX; k++) t->slot[k] = nullptr;
            t->used = 1; t->is_kernel = 0;
            return t;
        }
    }
    return nullptr;
}
void fd64_table_free64(FdTable64* t) {
    if (!t || !t->used || t->is_kernel) return;
    t->used = 0;
}
uint32_t fd64_table_used64(const FdTable64* t) {
    if (!t || !t->used) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < FD64_MAX; i++) if (t->slot[i]) n++;
    return n;
}

// ==================== 对象分配 / 释放 ====================
static OpenFile64* fd64_of_alloc64() {
    for (uint32_t i = 0; i < FD64_OPEN_MAX; i++) {
        if (g_files[i].used) continue;
        OpenFile64* of = &g_files[i];
        uint8_t* p = (uint8_t*)of;
        for (uint32_t k = 0; k < (uint32_t)sizeof(OpenFile64); k++) p[k] = 0;
        of->used = 1; of->refs = 1; of->dirc = -1;
        return of;
    }
    return nullptr;
}
// refs-1；归零才真释放（含目录缓存与 pipe 端）
static void fd64_of_unref64(OpenFile64* of) {
    if (!of || !of->used) return;
    if (of->refs > 1) { of->refs--; return; }
    of->refs = 0;
    if (of->pipe) {
        Pipe64* p = of->pipe;
        if (of->kind == FD64_KIND_PIPE_W) { if (p->writers) p->writers--; }
        else if (of->kind == FD64_KIND_PIPE_R) { if (p->readers) p->readers--; }
        if (p->refs) p->refs--;
        // ★ 两端引用都归零才回收：只剩一端时对象保留（另一端的 EOF 语义要靠 writers/readers）
        if (p->refs == 0) p->used = 0;
        of->pipe = nullptr;
    }
    if (of->dirc >= 0 && of->dirc < (int32_t)FD64_DIRC_MAX) {
        g_dirc[of->dirc].used = 0;
        g_dirc[of->dirc].count = 0;
        g_dirc[of->dirc].cursor = 0;
        of->dirc = -1;
    }
    of->used = 0;
}
static OpenFile64* fd64_slot_obj64(const FdTable64* t, int fd) {
    if (!t || !t->used) return nullptr;
    if (fd < 3 || fd >= (int)(3u + FD64_MAX)) return nullptr;
    return t->slot[fd - 3];
}
static int fd64_table_alloc_slot64(FdTable64* t) {
    if (!t) return -FD64_EMFILE;
    for (uint32_t i = 0; i < FD64_MAX; i++) if (!t->slot[i]) return (int)i;
    return -FD64_EMFILE;
}
// 把对象放进表里第一个空槽（refs 由调用方持有：本函数**不**加引用）
static int fd64_table_put64(FdTable64* t, OpenFile64* of) {
    const int s = fd64_table_alloc_slot64(t);
    if (s < 0) return s;
    t->slot[s] = of;
    return 3 + s;
}

int fd64_table_clone64(FdTable64* dst, const FdTable64* src) {
    if (!dst || !dst->used || !src || !src->used) return -FD64_EINVAL;
    for (uint32_t i = 0; i < FD64_MAX; i++) {
        dst->slot[i] = src->slot[i];
        if (dst->slot[i]) {
            dst->slot[i]->refs++;                       // ★ fork：父子共享同一个 OpenFile64
            Pipe64* p = dst->slot[i]->pipe;
            if (p) {
                p->refs++;
                if (dst->slot[i]->kind == FD64_KIND_PIPE_W) p->writers++;
                else if (dst->slot[i]->kind == FD64_KIND_PIPE_R) p->readers++;
            }
        }
    }
    return 0;
}
void fd64_table_close_all64(FdTable64* t) {
    if (!t || !t->used) return;
    for (uint32_t i = 0; i < FD64_MAX; i++) {
        if (!t->slot[i]) continue;
        fd64_of_unref64(t->slot[i]);
        t->slot[i] = nullptr;
    }
}

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

// 路径规范化：接受 "/a/b/c" 与 "a/b/c"（相对路径当成从根开始 —— VFS 层没有 cwd）。
// v3 起支持**多级路径**：连续的 "//" 折叠成一个；"." 段丢掉；".." 段**保留**交给 vfs64 解析
// （父目录语义只有 VFS 层知道，这里不重复实现）；结尾的 '/' 去掉。
// 仍然拒绝：空串（= 根目录，本层不给文件句柄）、只有 '/'、含空白/控制字符、超过 cap。
// 成功返回 0 并把规范形式（含 '/' 前缀）写进 out；失败返回负错误码。
int fd64_norm_path64(const char* in, char* out, int cap) {
    if (!in || !out || cap < 3) return -FD64_EINVAL;
    int i = 0;
    while (in[i] && fd64_is_ws(in[i])) i++;                 // 去前导空白
    int n = 0;
    out[n++] = '/';
    bool seg_has_char = false;                              // 当前段是否已经有内容
    for (;;) {
        const char c = in[i];
        if (c == 0 || fd64_is_ws(c)) break;                  // 结束 / 空白 -> 这一段到此为止（"a b" 只取 "a"）
        i++;
        if (c == '/') {
            if (!seg_has_char) continue;                     // "//" 与开头的 '/' 都折叠掉
            if (n + 1 >= cap) return -FD64_EINVAL;
            out[n++] = '/';
            seg_has_char = false;
            continue;
        }
        if ((unsigned char)c < 0x21 || (unsigned char)c > 0x7E) return -FD64_EINVAL;
        if (n + 1 >= cap) return -FD64_EINVAL;               // 太长（>= cap-1）
        out[n++] = c;
        seg_has_char = true;
    }
    if (n > 1 && out[n - 1] == '/') n--;                     // 结尾 '/' 去掉（"/a/" -> "/a"）
    if (n == 1) return -FD64_EINVAL;                         // 只有 "/" = 根目录：本层不给文件句柄
    out[n] = 0;
    return 0;
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

// 文件大小刷新：另一个 fd/另一个进程刚写过同一个文件时，本对象缓存的大小会过期（读/seek(END)/
// O_APPEND 都靠它）。每次操作前 stat 一次，代价小（vfs64_stat 是内存里的 inode 查询）。
static void fd64_refresh64(OpenFile64* of) {
    if (!of || of->kind != FD64_KIND_FILE) return;
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat(of->path, &ty, &sz) == 0 && ty != VFS64_TYPE_DIR) of->size = sz;
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

// ==================== open / close ====================
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

    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_of_alloc64();
    if (!of) return -FD64_EMFILE;
    of->flags    = flags;
    of->kind     = is_dir ? FD64_KIND_DIR : FD64_KIND_FILE;
    of->writable = (!is_dir && (flags & (FD64_O_WRONLY | FD64_O_RDWR))) ? 1 : 0;
    of->append   = (!is_dir && (flags & FD64_O_APPEND)) ? 1 : 0;
    of->off      = 0;
    for (uint32_t i = 0; i < FD64_PATH_MAX; i++) of->path[i] = norm[i];
    if (is_dir) {
        of->size = 0;
    } else if (of->writable && (flags & FD64_O_TRUNC)) {
        // ★ O_TRUNC 在 open 时落地（Linux 也是 open 即截断）：写路径每次都是整文件 RMW，
        //   所以不需要"第一次写再截断"的延迟标志了。
        (void)vfs64_write(norm, "", 0);
        of->size = 0;
    } else {
        uint32_t type = 0, size = 0;
        (void)vfs64_stat(norm, &type, &size);
        of->size = size;
    }

    const int fd = fd64_table_put64(t, of);
    if (fd < 0) { fd64_of_unref64(of); return fd; }

    dbg64_line_begin64();
    dbg64_str("[FD64] open path=");
    dbg64_str(norm);
    dbg64_str(" fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" flags=");
    dbg64_dec((uint64_t)flags);
    if (is_dir) dbg64_str(" (dir)");
    if (of->append) dbg64_str(" (append)");
    dbg64_nl();
    dbg64_line_end64();
    return fd;
}

int fd64_close64(int fd) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    const uint32_t left = of->refs - 1u;                      // 打印"还剩几个引用"
    t->slot[fd - 3] = nullptr;
    fd64_of_unref64(of);
    dbg64_line_begin64();
    dbg64_str("[FD64] close fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" refs=");
    dbg64_dec((uint64_t)left);
    if (left == 0) dbg64_str(" (object freed)");
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== pipe ====================
static Pipe64* fd64_pipe_alloc64() {
    for (uint32_t i = 0; i < FD64_PIPE_MAX; i++) {
        fd64_zero(&g_pipes[i], (uint32_t)sizeof(Pipe64));
        g_pipes[i].used = 1;
        return &g_pipes[i];
    }
    return nullptr;
}

// 读：读空且写端还开着 -> -EAGAIN（不阻塞）；写端全关 -> 0（EOF）
static int fd64_pipe_read64(Pipe64* p, void* buf, int len) {
    if (!p || !buf || len < 0) return -FD64_EFAULT;
    const uint64_t fl = dbg64_irq_save64();
    if (p->n == 0) {
        const int eof = (p->writers == 0);
        dbg64_irq_restore64(fl);
        return eof ? 0 : -FD64_EAGAIN;
    }
    uint32_t want = (uint32_t)len;
    if (want > p->n) want = p->n;
    uint8_t* d = (uint8_t*)buf;
    for (uint32_t i = 0; i < want; i++) {
        d[i] = p->buf[p->rd];
        p->rd = (p->rd + 1u) % FD64_PIPE_BYTES;
    }
    p->n -= want;
    dbg64_irq_restore64(fl);
    return (int)want;
}
// 写：写满返回**短写**（能塞多少塞多少）；一点空间都没有 -> -EAGAIN（不阻塞）
static int fd64_pipe_write64(Pipe64* p, const void* buf, int len) {
    if (!p || !buf || len < 0) return -FD64_EFAULT;
    const uint64_t fl = dbg64_irq_save64();
    const uint32_t room = FD64_PIPE_BYTES - p->n;
    if (room == 0) { dbg64_irq_restore64(fl); return -FD64_EAGAIN; }
    uint32_t want = (uint32_t)len;
    if (want > room) want = room;
    const uint8_t* s = (const uint8_t*)buf;
    for (uint32_t i = 0; i < want; i++) {
        p->buf[p->wr] = s[i];
        p->wr = (p->wr + 1u) % FD64_PIPE_BYTES;
    }
    p->n += want;
    dbg64_irq_restore64(fl);
    return (int)want;
}

int fd64_pipe64(int* fd_r, int* fd_w) {
    if (!fd_r || !fd_w) return -FD64_EINVAL;
    FdTable64* t = fd64_current_table64();
    Pipe64* p = fd64_pipe_alloc64();
    if (!p) return -FD64_EMFILE;
    OpenFile64* r = fd64_of_alloc64();
    OpenFile64* w = r ? fd64_of_alloc64() : nullptr;
    if (!r || !w) {
        if (r) fd64_of_unref64(r);
        if (w) fd64_of_unref64(w);
        p->used = 0;
        return -FD64_EMFILE;
    }
    p->refs = 2; p->writers = 1; p->readers = 1;
    r->kind = FD64_KIND_PIPE_R; r->flags = FD64_O_RDONLY; r->pipe = p;
    r->path[0] = '/'; r->path[1] = 'p'; r->path[2] = 'i'; r->path[3] = 'p'; r->path[4] = 'e';
    r->path[5] = '.'; r->path[6] = 'r'; r->path[7] = 0;
    w->kind = FD64_KIND_PIPE_W; w->flags = FD64_O_WRONLY; w->writable = 1; w->pipe = p;
    w->path[0] = '/'; w->path[1] = 'p'; w->path[2] = 'i'; w->path[3] = 'p'; w->path[4] = 'e';
    w->path[5] = '.'; w->path[6] = 'w'; w->path[7] = 0;
    const int fr = fd64_table_put64(t, r);
    int fw = -FD64_EMFILE;
    if (fr >= 0) fw = fd64_table_put64(t, w);
    if (fr < 0 || fw < 0) {
        if (fr >= 0) { t->slot[fr - 3] = nullptr; }
        fd64_of_unref64(r);
        fd64_of_unref64(w);
        p->used = 0;
        return -FD64_EMFILE;
    }
    *fd_r = fr;
    *fd_w = fw;
    dbg64_line_begin64();
    dbg64_str("[FD64] pipe r=");
    dbg64_dec((uint64_t)fr);
    dbg64_str(" w=");
    dbg64_dec((uint64_t)fw);
    dbg64_str(" bytes=");
    dbg64_dec((uint64_t)FD64_PIPE_BYTES);
    dbg64_str(" (non-blocking: full=short-write/empty=-EAGAIN)");
    dbg64_nl();
    dbg64_line_end64();
    return 0;
}

// ==================== read / write / lseek ====================
int fd64_read64(int fd, void* buf, int len) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    if (of->kind == FD64_KIND_DIR) return -FD64_EISDIR;
    if (of->kind == FD64_KIND_PIPE_W) return -FD64_EBADF;       // 写端不能读
    if (!buf || len < 0) return -FD64_EFAULT;
    if (len == 0) return 0;
    if (of->kind == FD64_KIND_PIPE_R) {
        const int n = fd64_pipe_read64(of->pipe, buf, len);
        if (n <= 0) return n;
        // 证据行（验收 grep）：[FD64] pipe read n=<n> data=<s>（最多 64 B，按原样打印）
        dbg64_line_begin64();
        dbg64_str("[FD64] pipe read n=");
        dbg64_dec((uint64_t)n);
        dbg64_str(" data=");
        const uint8_t* d = (const uint8_t*)buf;
        for (int i = 0; i < n; i++) {
            const char c = (char)d[i];
            dbg64_putc(c >= 0x20 && c < 0x7F ? c : '.');
        }
        dbg64_nl();
        dbg64_line_end64();
        return n;
    }
    if ((of->flags & 0x3u) == FD64_O_WRONLY) return -FD64_EBADF; // 只写 fd 不能读（Linux 一致）

    const uint64_t if_save = dbg64_irq_save64();
    fd64_refresh64(of);
    if (of->off >= of->size) { dbg64_irq_restore64(if_save); return 0; }   // EOF
    const int n = vfs64_read(of->path, g_fd64_rbuf, (int)FD64_FILE_MAX);
    if (n < 0) { dbg64_irq_restore64(if_save); return -FD64_ENOENT; }
    const uint32_t avail = ((uint32_t)n > of->size) ? of->size : (uint32_t)n;
    if (of->off >= avail) { dbg64_irq_restore64(if_save); return 0; }
    uint32_t got = avail - of->off;
    if ((uint32_t)len < got) got = (uint32_t)len;
    fd64_copy(buf, g_fd64_rbuf + of->off, got);
    of->off += got;
    dbg64_irq_restore64(if_save);

    dbg64_line_begin64();
    dbg64_str("[FD64] read fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)got);
    dbg64_str(" off=");
    dbg64_dec((uint64_t)of->off);
    dbg64_nl();
    dbg64_line_end64();
    return (int)got;
}

int fd64_write64(int fd, const void* buf, int len) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    if (of->kind == FD64_KIND_DIR) return -FD64_EISDIR;
    if (of->kind == FD64_KIND_PIPE_R) return -FD64_EBADF;       // 读端不能写
    if (!of->writable) return -FD64_EBADF;
    if (!buf || len < 0) return -FD64_EFAULT;
    if (len == 0) return 0;
    if (of->kind == FD64_KIND_PIPE_W) return fd64_pipe_write64(of->pipe, buf, len);

    const uint64_t if_save = dbg64_irq_save64();
    fd64_refresh64(of);
    if (of->append) of->off = of->size;                        // ★ O_APPEND：每次写都定位到末尾
    const uint32_t end = of->off + (uint32_t)len;
    if (end > FD64_FILE_MAX) { dbg64_irq_restore64(if_save); return -FD64_EFBIG; }

    // 整文件 read-modify-write（每次写都读一遍：多个 fd/多个进程同时写时语义才正确）
    uint32_t sz = 0;
    {
        const int n = vfs64_read(of->path, g_fd64_wbuf, (int)FD64_FILE_MAX);
        sz = (n > 0) ? (uint32_t)n : 0;
        if (sz > FD64_FILE_MAX) sz = FD64_FILE_MAX;
    }
    if (of->append) of->off = sz;                              // 以**磁盘上的当前末尾**为准
    if (of->off + (uint32_t)len > FD64_FILE_MAX) { dbg64_irq_restore64(if_save); return -FD64_EFBIG; }
    if (of->off > sz) {                                        // 空洞补零（seek 过末尾再写）
        for (uint32_t i = sz; i < of->off && i < FD64_FILE_MAX; i++) g_fd64_wbuf[i] = 0;
        sz = of->off;
    }
    fd64_copy(g_fd64_wbuf + of->off, buf, (uint32_t)len);
    const uint32_t end2 = of->off + (uint32_t)len;
    if (end2 > sz) sz = end2;
    const int wn = vfs64_write(of->path, g_fd64_wbuf, (int)sz);
    of->size = sz;
    of->off  = end2;                                           // 写后游标 = 末尾（O_APPEND 亦然）
    dbg64_irq_restore64(if_save);
    if (wn < 0) return -FD64_ENOSPC;

    dbg64_line_begin64();
    dbg64_str("[FD64] write fd=");
    dbg64_dec((uint64_t)fd);
    dbg64_str(" n=");
    dbg64_dec((uint64_t)len);
    dbg64_str(" total=");
    dbg64_dec((uint64_t)of->size);
    if (of->append) dbg64_str(" append");
    dbg64_nl();
    dbg64_line_end64();
    return len;
}

int fd64_lseek64(int fd, int64_t off, int whence) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    if (of->kind != FD64_KIND_FILE) return -FD64_ESPIPE;        // 目录/pipe：不可 seek
    fd64_refresh64(of);
    int64_t base = 0;
    if (whence == FD64_SEEK_SET)      base = 0;
    else if (whence == FD64_SEEK_CUR) base = (int64_t)of->off;
    else if (whence == FD64_SEEK_END) base = (int64_t)of->size;
    else return -FD64_EINVAL;
    const int64_t nv = base + off;
    if (nv < 0 || nv > (int64_t)FD64_FILE_MAX) return -FD64_EINVAL;
    of->off = (uint32_t)nv;
    return (int)nv;
}

int fd64_stat64(int fd, uint32_t* type_out, uint32_t* size_out) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    if (type_out) *type_out = (of->kind == FD64_KIND_DIR) ? VFS64_TYPE_DIR : VFS64_TYPE_FILE;
    if (size_out) {
        // pipe：Linux 给 S_IFIFO + 已缓冲字节；本层按普通文件最小字段返回（如实边界）
        if (of->kind == FD64_KIND_FILE) fd64_refresh64(of);
        *size_out = (of->kind == FD64_KIND_FILE) ? of->size : 0;
    }
    return 0;
}

int fd64_fsync64(int fd) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    return 0;                                                // 写路径已即时落盘（无脏页）
}

int fd64_dup64(int oldfd, int newfd) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* src = fd64_slot_obj64(t, oldfd);
    if (!src) return -FD64_EBADF;
    int dst = -1;
    if (newfd >= 3) {
        if (newfd >= (int)(3u + FD64_MAX)) return -FD64_EBADF;
        dst = newfd - 3;
        if (t->slot[dst] == src) return newfd;                // 同一个槽：直接返回（Linux 同）
        if (t->slot[dst]) {                                   // dup2：先关旧目标（引用 -1）
            OpenFile64* old = t->slot[dst];
            t->slot[dst] = nullptr;
            fd64_of_unref64(old);
        }
    } else {
        dst = fd64_table_alloc_slot64(t);
        if (dst < 0) return dst;
    }
    src->refs++;                                              // ★ 共享同一个对象（共享偏移）
    t->slot[dst] = src;
    return 3 + dst;
}

uint64_t fd64_object_id64(int fd) {                            // 自检/证据：对象标识（就是它的地址）
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    return of ? (uint64_t)(uintptr_t)of : 0;
}

// ==================== 目录句柄 ====================
int fd64_opendir64(const char* path) {
    return fd64_open64(path, FD64_O_RDONLY | FD64_O_DIRECTORY);
}

int fd64_readdir64(int fd, char* name_out, int name_cap, uint32_t* type_out, uint32_t* size_out) {
    FdTable64* t = fd64_current_table64();
    OpenFile64* of = fd64_slot_obj64(t, fd);
    if (!of) return -FD64_EBADF;
    if (of->kind != FD64_KIND_DIR) return -FD64_ENOTDIR;
    if (!name_out || name_cap < 2) return -FD64_EFAULT;

    // 第一次调用：列一次目录并把结果缓存起来（理由见 FD64_DIR_CACHE 的注释）
    if (of->dirc < 0) {
        int32_t slot = -1;
        for (uint32_t i = 0; i < FD64_DIRC_MAX; i++) if (!g_dirc[i].used) { slot = (int32_t)i; break; }
        if (slot < 0) return -FD64_EMFILE;
        g_dirc[slot].used = 1;
        g_dirc[slot].cursor = 0;
        const int n = vfs64_ls(of->path, g_dirc[slot].names, (int)FD64_DIR_CACHE, g_dirc[slot].sizes);
        if (n < 0) { g_dirc[slot].used = 0; return -FD64_ENOENT; }
        g_dirc[slot].count = (uint32_t)n;
        of->dirc = slot;
    }
    DirCache64* dc = &g_dirc[of->dirc];
    if (dc->cursor >= dc->count) return 0;                     // 枚举结束

    const uint32_t idx = dc->cursor++;
    int i = 0;
    while (dc->names[idx][i] && i + 1 < name_cap) { name_out[i] = dc->names[idx][i]; i++; }
    name_out[i] = 0;
    if (type_out || size_out) {
        // 类型：vfs64_ls 只给名字+大小。大小为 0 的项才需要 stat 一次（目录 / 空文件）；
        // 非空文件按大小直接判定为 FILE —— 这样 ls 不会对每个文件都做一遍全表扫描。
        uint32_t ty = VFS64_TYPE_FILE;
        uint32_t sz = dc->sizes[idx];
        if (sz == 0) {
            char full[FD64_PATH_MAX];
            int p = 0;
            full[p++] = '/';
            for (int k = 0; dc->names[idx][k] && p + 1 < (int)FD64_PATH_MAX; k++) full[p++] = dc->names[idx][k];
            full[p] = 0;
            if (vfs64_stat(full, &ty, &sz) != 0) ty = VFS64_TYPE_FILE;
        }
        if (type_out) *type_out = ty;
        if (size_out) *size_out = (ty == VFS64_TYPE_FILE) ? sz : 0;
    }
    return 1;
}

// ==================== 语义演示（终端 fdtest）====================
// 位含义：bit0 独立游标、bit1 dup 共享对象/游标、bit2 O_APPEND、bit3 pipe 环回/短写/EOF、
//         bit4 fork 的 fd 继承（表克隆逐槽共享同一对象 + 引用计数 +1）
static int fd64_demo_check64(const char* a, const char* b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}
static void fd64_demo_line64(const char* tag, int ok) {
    dbg64_line_begin64();
    dbg64_str("[FD64] demo ");
    dbg64_str(tag);
    dbg64_str(ok ? " ok=1" : " ok=0");
    dbg64_nl();
    dbg64_line_end64();
}
int fd64_demo64() {
    uint32_t ty = 0, sz = 0;
    if (vfs64_stat("/", &ty, &sz) != 0) {                      // 没卷：如实跳过
        dbg64_line_begin64();
        dbg64_str("[FD64] demo skipped (no volume)\n");
        dbg64_line_end64();
        return 0;
    }
    int fail = 0;
    const char* A = "/fdtest.a";
    const char* B = "/fdtest.b";

    // ---- 造材料：/fdtest.a = "0123456789"（10 B）----
    {
        const int fd = fd64_open64(A, FD64_O_WRONLY | FD64_O_CREAT | FD64_O_TRUNC);
        if (fd < 3) return 4;
        if (fd64_write64(fd, "0123456789", 10) != 10) fail |= 8;
        (void)fd64_close64(fd);
    }

    // ---- bit0：同进程两次 open 同一个文件 = 两个独立游标 ----
    {
        const int fa = fd64_open64(A, FD64_O_RDONLY);
        const int fb = fd64_open64(A, FD64_O_RDONLY);
        char a1[5] = {0}, a2[5] = {0}, b1[5] = {0}, b2[5] = {0};
        if (fa < 3 || fb < 3) {
            fail |= 1;
        } else {
            const int ra1 = fd64_read64(fa, a1, 4);
            const int rb1 = fd64_read64(fb, b1, 4);
            const int rb2 = fd64_read64(fb, b2, 4);
            const int ra2 = fd64_read64(fa, a2, 4);
            if (ra1 != 4 || rb1 != 4 || rb2 != 4 || ra2 != 4) fail |= 1;
            if (!fd64_demo_check64(a1, "0123", 4)) fail |= 1;
            if (!fd64_demo_check64(b1, "0123", 4)) fail |= 1;   // 第二个 fd 从头读（独立）
            if (!fd64_demo_check64(a2, "4567", 4)) fail |= 1;   // 各自接着自己读
            if (!fd64_demo_check64(b2, "4567", 4)) fail |= 1;
            dbg64_line_begin64();
            dbg64_str("[FD64] demo indep fd_a=");
            dbg64_dec((uint64_t)fa);
            dbg64_str(" fd_b=");
            dbg64_dec((uint64_t)fb);
            dbg64_str(" a1=");
            dbg64_str(a1);
            dbg64_str(" b1=");
            dbg64_str(b1);
            dbg64_str(" a2=");
            dbg64_str(a2);
            dbg64_str(" b2=");
            dbg64_str(b2);
            dbg64_str(" same_object=");
            dbg64_dec((fd64_object_id64(fa) == fd64_object_id64(fb)) ? 1 : 0);
            dbg64_nl();
            dbg64_line_end64();
            if (fd64_object_id64(fa) == fd64_object_id64(fb)) fail |= 1;   // 必须是两个对象
        }

        // ---- bit1：dup 后两个 fd 共享同一个对象（共享游标）----
        if (fa >= 3) {
            const int fd = fd64_dup64(fa, -1);
            char d2[3] = {0};
            const int n = (fd >= 3) ? fd64_read64(fd, d2, 2) : -1;
            const int off_fa = fd64_lseek64(fa, 0, FD64_SEEK_CUR);
            const int off_fd = (fd >= 3) ? fd64_lseek64(fd, 0, FD64_SEEK_CUR) : -1;
            const int same = (fd >= 3) && (fd64_object_id64(fa) == fd64_object_id64(fd));
            if (!same || n != 2 || off_fa != off_fd || off_fa != 10) fail |= 2;   // 前两次读各推 4+4
            dbg64_line_begin64();
            dbg64_str("[FD64] demo dup fd_a=");
            dbg64_dec((uint64_t)fa);
            dbg64_str(" fd_dup=");
            dbg64_dec((uint64_t)(fd < 0 ? 0 : fd));
            dbg64_str(" same_object=");
            dbg64_dec(same ? 1 : 0);
            dbg64_str(" off_a=");
            dbg64_dec((uint64_t)(off_fa < 0 ? 0 : off_fa));
            dbg64_str(" off_dup=");
            dbg64_dec((uint64_t)(off_fd < 0 ? 0 : off_fd));
            dbg64_str(" read_dup=");
            dbg64_str(n == 2 ? d2 : "??");
            dbg64_nl();
            dbg64_line_end64();
            if (fd >= 3) (void)fd64_close64(fd);
        }
        if (fa >= 3) (void)fd64_close64(fa);
        if (fb >= 3) (void)fd64_close64(fb);
    }

    // ---- bit2：O_APPEND —— 先 lseek 到 0 再写，内容仍然追到末尾 ----
    {
        const int fd = fd64_open64(A, FD64_O_WRONLY | FD64_O_APPEND);
        char whole[16] = {0};
        const int seekto = (fd >= 3) ? fd64_lseek64(fd, 0, FD64_SEEK_SET) : -1;
        const int wn = (fd >= 3) ? fd64_write64(fd, "!", 1) : -1;
        const int off = (fd >= 3) ? fd64_lseek64(fd, 0, FD64_SEEK_CUR) : -1;
        if (fd >= 3) (void)fd64_close64(fd);
        const int fr = fd64_open64(A, FD64_O_RDONLY);
        const int rn = (fr >= 3) ? fd64_read64(fr, whole, 16) : -1;
        if (fr >= 3) (void)fd64_close64(fr);
        const int ok = (seekto == 0 && wn == 1 && off == 11 && rn == 11 &&
                        whole[10] == '!' && fd64_demo_check64(whole, "0123456789!", 11));
        if (!ok) fail |= 4;
        dbg64_line_begin64();
        dbg64_str("[FD64] demo append lseek=");
        dbg64_dec((uint64_t)(seekto < 0 ? 0 : seekto));
        dbg64_str(" w=");
        dbg64_dec((uint64_t)(wn < 0 ? 0 : wn));
        dbg64_str(" off_after=");
        dbg64_dec((uint64_t)(off < 0 ? 0 : off));
        dbg64_str(" bytes=");
        dbg64_dec((uint64_t)(rn < 0 ? 0 : rn));
        dbg64_str(" content=");
        dbg64_str(whole);
        dbg64_nl();
        dbg64_line_end64();
    }

    // ---- bit3：pipe 环回 + 短写 + EOF（写端关闭后读空 = 0）----
    {
        int r = -1, w = -1;
        const int pr = fd64_pipe64(&r, &w);
        if (pr != 0) {
            fail |= 8;
        } else {
            const int wn = fd64_write64(w, "PIPE-RING", 9);
            char back[16] = {0};
            const int rn = fd64_read64(r, back, 16);
            if (wn != 9 || rn != 9 || !fd64_demo_check64(back, "PIPE-RING", 9)) fail |= 8;
            dbg64_line_begin64();
            dbg64_str("[FD64] demo pipe n=");
            dbg64_dec((uint64_t)(rn < 0 ? 0 : rn));
            dbg64_str(" data=");
            dbg64_str(back);
            dbg64_str(" ok=");
            dbg64_dec((wn == 9 && rn == 9) ? 1 : 0);
            dbg64_nl();
            dbg64_line_end64();
            // 短写：容量 64 B，一次塞 100 B -> 只写进 64（不阻塞）
            uint8_t big[100];
            for (int i = 0; i < 100; i++) big[i] = (uint8_t)('a' + (i % 26));
            const int sw = fd64_write64(w, big, 100);
            if (sw != (int)FD64_PIPE_BYTES) fail |= 8;
            dbg64_line_begin64();
            dbg64_str("[FD64] demo pipe shortwrite=");
            dbg64_dec((uint64_t)(sw < 0 ? 0 : sw));
            dbg64_str("/100 (ring=");
            dbg64_dec((uint64_t)FD64_PIPE_BYTES);
            dbg64_str(")");
            dbg64_nl();
            dbg64_line_end64();
            uint8_t sink[128];
            const int drain = fd64_read64(r, sink, 128);
            if (drain != (int)FD64_PIPE_BYTES) fail |= 8;
            const int empty = fd64_read64(r, sink, 128);       // 写端还开着 -> -EAGAIN（不阻塞）
            if (empty != -FD64_EAGAIN) fail |= 8;
            if (fd64_close64(w) != 0) fail |= 8;               // 写端关闭
            const int eof = fd64_read64(r, sink, 128);         // 写端全关 -> 0（EOF）
            if (eof != 0) fail |= 8;
            dbg64_line_begin64();
            dbg64_str("[FD64] demo pipe empty=");
            dbg64_dec((uint64_t)(empty < 0 ? (uint64_t)(-empty) + 1000u : (uint64_t)empty));
            dbg64_str(" (1000+EAGAIN) eof=");
            dbg64_dec((uint64_t)(eof < 0 ? (uint64_t)(-eof) : (uint64_t)eof));
            dbg64_nl();
            dbg64_line_end64();
            if (fd64_close64(r) != 0) fail |= 8;
        }
    }

    // ---- bit4：fork 的 fd 继承语义（克隆整张表 = 逐槽共享同一个对象 + 引用计数 +1）----
    // 这里直接调 fd64_table_clone64（proc64_fork64 用的就是它）：结构上"父子共享偏移游标"
    // 就是"两个表的槽指向同一个 OpenFile64" —— 所以断言三件事：指针相同、引用计数 +1、
    // 关掉克隆表后原对象还活着（引用计数归零才释放）。
    {
        const int fa = fd64_open64(A, FD64_O_RDONLY);
        FdTable64* mine = fd64_current_table64();
        FdTable64* child = fd64_table_alloc64();
        char seg[5] = {0};
        int n = (fa >= 3) ? fd64_read64(fa, seg, 3) : -1;                  // 先把偏移推到 3
        int same = 0, refs_ok = 0, alive = 0, off_a = -1;
        if (fa >= 3 && child && n == 3) {
            OpenFile64* obj = mine->slot[fa - 3];
            const uint32_t refs0 = obj->refs;
            if (fd64_table_clone64(child, mine) == 0) {
                same = (child->slot[fa - 3] == obj) ? 1 : 0;               // ★ 共享同一个对象
                refs_ok = (obj->refs == refs0 + 1u) ? 1 : 0;               // ★ 引用计数 +1
                fd64_table_close_all64(child);                             // "子进程退出"
                alive = (obj->refs == refs0 && obj->used) ? 1 : 0;         // ★ 原对象还在
                char more[5] = {0};
                const int r2 = fd64_read64(fa, more, 4);                   // 原 fd 继续可用
                if (r2 == 4 && fd64_demo_check64(more, "3456", 4)) alive |= 2;
                off_a = fd64_lseek64(fa, 0, FD64_SEEK_CUR);
            }
        }
        if (!same || !refs_ok || !(alive & 1) || !(alive & 2)) fail |= 16;
        dbg64_line_begin64();
        dbg64_str("[FD64] demo forkinherit fd=");
        dbg64_dec((uint64_t)(fa < 0 ? 0 : fa));
        dbg64_str(" same_object=");
        dbg64_dec((uint64_t)same);
        dbg64_str(" refs_plus1=");
        dbg64_dec((uint64_t)refs_ok);
        dbg64_str(" survived_child_exit=");
        dbg64_dec((uint64_t)(alive & 1));
        dbg64_str(" off_after=");
        dbg64_dec((uint64_t)(off_a < 0 ? 0 : off_a));
        dbg64_str(" ok=");
        dbg64_dec((uint64_t)((same && refs_ok && (alive & 3) == 3) ? 1 : 0));
        dbg64_nl();
        dbg64_line_end64();
        if (fa >= 3) (void)fd64_close64(fa);
        if (child) fd64_table_free64(child);
    }
    // ---- 收尾：删掉材料 ----
    //    B 只是"目录项演示"：先 O_CREAT 建出来，再用 O_RDONLY 打开读一次（证明它是真文件）
    {
        const int fb = fd64_open64(B, FD64_O_WRONLY | FD64_O_CREAT);
        if (fb >= 3) (void)fd64_close64(fb);
        const int fr = fd64_open64(B, FD64_O_RDONLY);
        if (fr >= 3) (void)fd64_close64(fr);
    }
    (void)vfs64_unlink(A);
    (void)vfs64_unlink(B);

    fd64_demo_line64("PASS", fail == 0);
    if (fail != 0) {
        dbg64_line_begin64();
        dbg64_str("[FD64] demo FAIL mask=");
        dbg64_dec((uint64_t)fail);
        dbg64_nl();
        dbg64_line_end64();
    }
    return fail;
}

// ==================== 自检 / 诊断 ====================
int fd64_selftest64() {
    int fail = 0;
    char out[FD64_PATH_MAX];

    // bit0：路径规范化（v3 起支持多级：折叠 "//"、忽略结尾 '/'、保留 ".." 交给 vfs64；
    //       仍然拒绝根目录/空串/空白字符/超长）
    if (fd64_norm_path64("/t.txt", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/t.txt")) fail |= 1;
    if (fd64_norm_path64("t.txt", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/t.txt"))  fail |= 1;
    if (fd64_norm_path64("/bad name", out, (int)sizeof(out)) != 0 || !fd64_streq(out, "/bad")) fail |= 1;
    if (fd64_norm_path64("/", out, (int)sizeof(out)) == 0)     fail |= 1;   // 根目录：本层不给句柄
    if (fd64_norm_path64("", out, (int)sizeof(out)) == 0)      fail |= 1;
    if (fd64_norm_path64("/apps/demo/a.txt", out, (int)sizeof(out)) != 0 ||
        !fd64_streq(out, "/apps/demo/a.txt")) fail |= 1;                    // 多级路径
    if (fd64_norm_path64("//apps///demo//", out, (int)sizeof(out)) != 0 ||
        !fd64_streq(out, "/apps/demo")) fail |= 1;                          // 折叠 '//' 与结尾 '/'
    if (fd64_norm_path64("apps/../apps/x", out, (int)sizeof(out)) != 0 ||
        !fd64_streq(out, "/apps/../apps/x")) fail |= 1;                    // ".." 原样交给 vfs64
    if (fd64_norm_path64("/a/b?", out, (int)sizeof(out)) != 0) fail |= 1;   // '?' 是可打印 ASCII：合法

    // bit1：fd 表分配/释放（不碰盘：全是失败路径，不该改变表状态）
    {
        const int before = fd64_table_used64(fd64_current_table64());
        if (fd64_close64(3 + (int)FD64_MAX) != -FD64_EBADF) fail |= 2;   // 越界 fd
        if (fd64_read64(1, out, 1) != -FD64_EBADF) fail |= 2;            // 标准流不归本层
        if (fd64_table_used64(fd64_current_table64()) != (uint32_t)before) fail |= 2;
    }

    // bit3：fd 表池 + dup 共享同一个 OpenFile64（用根目录句柄，不需要额外文件）
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
            const int d2 = fd64_dup64(dfd, -1);                       // dup：共享对象
            if (d2 < 3 || fd64_object_id64(d2) != fd64_object_id64(dfd)) fail |= 8;
            if (d2 >= 3 && fd64_close64(d2) != 0) fail |= 8;
            if (fd64_object_id64(dfd) == 0) fail |= 8;                // 关了 dup 原对象还在
            if (fd64_close64(dfd) != 0) fail |= 4;
        }
    }

    dbg64_line_begin64();
    dbg64_str("[FD64] selftest table=");
    dbg64_dec((uint64_t)FD64_MAX);
    dbg64_str(" file_max=");
    dbg64_dec((uint64_t)FD64_FILE_MAX);
    dbg64_str(" tables=");
    dbg64_dec((uint64_t)FD64_TABLE_MAX);
    dbg64_str(" opens=");
    dbg64_dec((uint64_t)FD64_OPEN_MAX);
    dbg64_str(" pipes=");
    dbg64_dec((uint64_t)FD64_PIPE_MAX);
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
    FdTable64* t = fd64_current_table64();
    const uint32_t used = fd64_table_used64(t);
    dbg64_line_begin64();
    dbg64_str("[FD64] dump table=");
    dbg64_str((t == fd64_kernel_table64()) ? "kernel" : "proc");
    dbg64_str(" used=");
    dbg64_dec((uint64_t)used);
    dbg64_str("/");
    dbg64_dec((uint64_t)FD64_MAX);
    dbg64_nl();
    dbg64_line_end64();
    if (!t) return;
    for (uint32_t i = 0; i < FD64_MAX; i++) {
        if (!t->slot[i]) continue;
        dbg64_line_begin64();
        dbg64_str("[FD64] dump fd=");
        dbg64_dec((uint64_t)(3u + i));
        dbg64_str(" kind=");
        dbg64_dec((uint64_t)t->slot[i]->kind);
        dbg64_str(" refs=");
        dbg64_dec((uint64_t)t->slot[i]->refs);
        dbg64_str(" path=");
        dbg64_str(t->slot[i]->path);
        dbg64_str(" size=");
        dbg64_dec((uint64_t)t->slot[i]->size);
        dbg64_str(" off=");
        dbg64_dec((uint64_t)t->slot[i]->off);
        dbg64_nl();
        dbg64_line_end64();
    }
}
