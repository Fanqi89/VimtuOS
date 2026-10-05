/* vs_io.c - ★ 应用商店/包管理器的用户态底座（纯 ring3，只用既有 ABI）
 *
 * 里面是四类东西，全部"一行一条 syscall，不夹带策略"：
 *   ① 字符串小工具（本运行时的 string.c 只实现了 memXXX / strXXX 的一部分，这里补 vs_ 前缀的那部分）；
 *   ② 文件 IO：读/写/删/mkdir_p/存在性/大小 —— 走 Linux 兼容号段（open 2 / read 0 / write 1 /
 *      close 3 / stat 4 / unlink 87 / mkdir 83），错误码 = 负 errno；
 *   ③ 卷容量：statfs(137)（x86_64 的 120B struct statfs；f_bsize@+8 / f_blocks@+16 / f_bfree@+24）；
 *   ④ 实时目录枚举：自有 ABI 50/51/52（包装写法与 sdk/software-template/include/vimtu/dir64.c 同款）；
 *      ⑤ sha256（纯标准实现，无外部依赖）+ mmap(Linux 9) 大缓冲。
 *
 * 为什么大缓冲走 mmap：用户窗口只有 16 MiB，但**主程序装载区只有 64 KiB**（kernel/elf64.cpp 的
 * PT_LOAD 硬约束）——静态数组越大，.data 越大，装载区越容易爆。所以解压缓冲一律 mmap，静态数组保持小。
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "vimtu64.h"
#include "vs.h"

/* ==================== ① 字符串 ==================== */
unsigned long vs_strlen(const char* s) {
    unsigned long n = 0;
    if (!s) return 0;
    while (s[n]) n++;
    return n;
}

int vs_streq(const char* a, const char* b) {
    unsigned long i = 0;
    if (!a || !b) return 0;
    while (a[i] && b[i] && a[i] == b[i]) i++;
    return (a[i] == 0 && b[i] == 0) ? 1 : 0;
}

int vs_strcpy(char* dst, int cap, const char* src) {
    int i = 0;
    if (!dst || cap <= 0) return -1;
    if (!src) { dst[0] = 0; return 0; }
    while (src[i]) {
        if (i + 1 >= cap) { dst[0] = 0; return -1; }
        dst[i] = src[i];
        i++;
    }
    dst[i] = 0;
    return 0;
}

int vs_starts(const char* s, const char* pre) {
    int i = 0;
    if (!s || !pre) return 0;
    while (pre[i]) { if (s[i] != pre[i]) return 0; i++; }
    return 1;
}

int vs_find(const char* hay, const char* needle) {
    if (!hay || !needle) return -1;
    const unsigned long nh = vs_strlen(hay), nn = vs_strlen(needle);
    if (nn == 0 || nh < nn) return -1;
    for (unsigned long i = 0; i + nn <= nh; i++) {
        unsigned long k = 0;
        while (k < nn && hay[i + k] == needle[k]) k++;
        if (k == nn) return (int)i;
    }
    return -1;
}

static char vs_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

int vs_icontains(const char* hay, const char* needle) {
    if (!hay || !needle) return 0;
    const unsigned long nh = vs_strlen(hay), nn = vs_strlen(needle);
    if (nn == 0) return 1;
    if (nh < nn) return 0;
    for (unsigned long i = 0; i + nn <= nh; i++) {
        unsigned long k = 0;
        while (k < nn && vs_lower(hay[i + k]) == vs_lower(needle[k])) k++;
        if (k == nn) return 1;
    }
    return 0;
}

int vs_atoi(const char* s) {
    int v = 0, neg = 0;
    if (!s) return 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    return neg ? -v : v;
}

void vs_hex(const unsigned char* b, int n, char* out) {
    static const char* H = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = H[(b[i] >> 4) & 0xF];
        out[i * 2 + 1] = H[b[i] & 0xF];
    }
    out[n * 2] = 0;
}

static int vs_hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int vs_hex2ascii(const char* hex, unsigned char* out, int cap) {
    int n = 0;
    if (!hex) return -1;
    for (int i = 0; hex[i]; i += 2) {
        const int hi = vs_hexval(hex[i]);
        const int lo = vs_hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        if (n >= cap) return -1;
        out[n++] = (unsigned char)((hi << 4) | lo);
    }
    return n;
}

int vs_json_esc(char* dst, int cap, const char* src) {
    int o = 0;
    if (cap <= 0) return -1;
    for (int i = 0; src && src[i]; i++) {
        const char c = src[i];
        if (c == '"' || c == '\\') {
            if (o + 2 >= cap) return -1;
            dst[o++] = '\\';
            dst[o++] = c;
        } else if (c == '\n' || c == '\r' || c == '\t') {
            if (o + 2 >= cap) return -1;
            dst[o++] = ' ';
            dst[o++] = ' ';
        } else {
            if (o + 1 >= cap) return -1;
            dst[o++] = c;
        }
    }
    dst[o] = 0;
    return o;
}

/* ==================== ② 文件 IO ==================== */
int vs_read_file(const char* path, unsigned char* buf, int cap) {
    const int fd = (int)__v64_syscall(V64_LX_OPEN, (long)(unsigned long)path, O_RDONLY, 0, 0, 0);
    if (fd < 0) return -1;
    int total = 0;
    for (;;) {
        if (total >= cap) { (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0); return -2; }
        const long got = __v64_syscall(V64_LX_READ, fd, (long)(unsigned long)(buf + total),
                                      (long)(cap - total), 0, 0);
        if (got < 0) { (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0); return -1; }
        if (got == 0) break;
        total += (int)got;
    }
    (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
    return total;
}

int vs_write_file(const char* path, const unsigned char* buf, int len) {
    const int fd = (int)__v64_syscall(V64_LX_OPEN, (long)(unsigned long)path,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0, 0, 0);
    if (fd < 0) return (int)fd;
    int off = 0;
    while (off < len) {
        int chunk = len - off;
        if (chunk > 1024) chunk = 1024;          /* 与 user/lib write() 的分块上限同口径 */
        const long w = __v64_syscall(V64_LX_WRITE, fd, (long)(unsigned long)(buf + off), chunk, 0, 0);
        if (w <= 0) { (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0); return (int)(w ? w : -5); }
        off += (int)w;
    }
    (void)__v64_syscall(V64_LX_CLOSE, fd, 0, 0, 0, 0);
    return 0;
}

int vs_unlink(const char* path) {
    return (int)__v64_syscall(V64_LX_UNLINK, (long)(unsigned long)path, 0, 0, 0, 0);
}

int vs_mkdir(const char* path) {
    return (int)__v64_syscall(83, (long)(unsigned long)path, 0755, 0, 0, 0);
}

int vs_mkdir_p(const char* dir) {
    char tmp[VS_FILE_MAX];
    if (vs_strcpy(tmp, (int)sizeof(tmp), dir) != 0) return -1;
    const int n = (int)vs_strlen(tmp);
    for (int i = 1; i <= n; i++) {
        if (i == n || tmp[i] == '/') {
            const char save = tmp[i];
            tmp[i] = 0;
            const int rc = vs_mkdir(tmp);
            const int isdir = (rc != 0) ? vs_is_dir(tmp) : 1;
            tmp[i] = save;
            /* ★ 内核的"已存在"不是 -EEXIST（实测 vfs64_mkdir 返回 -1，Linux 层映射成 -2）：
             *   所以判据 = 错误码 + "它其实已经是个目录"两件事都要看。 */
            if (rc != 0 && rc != -17 && !isdir) return rc;
        }
    }
    return 0;
}

/* 这个路径是不是**目录**（Linux stat(4) 的 st_mode 在 +24；x86_64 口径）——
 * 用途：内核 vfs64_mkdir 对"已存在"返回 -1（Linux 层再映射成 -2），**不是** -EEXIST，
 * 所以 mkdir_p 不能只看错误码，必须回头确认"它其实已经是个目录"。 */
int vs_is_dir(const char* path) {
    unsigned char st[144];
    if (__v64_syscall(V64_LX_STAT, (long)(unsigned long)path, (long)(unsigned long)st, 0, 0, 0) < 0)
        return 0;
    const unsigned mode = (unsigned)(st[24] | ((unsigned)st[25] << 8) |
                                     ((unsigned)st[26] << 16) | ((unsigned)st[27] << 24));
    return ((mode & 0xF000u) == 0x4000u) ? 1 : 0;
}

int vs_exists(const char* path) {
    /* access(21)：0 = 存在（权限被拒也算存在，用 F_OK 语义） */
    return (__v64_syscall(21, (long)(unsigned long)path, 0, 0, 0, 0) == 0) ? 1 : 0;
}

static const char* g_last_path;    /* 给 -ENOENT 诊断用（不参与判定） */

int vs_size_of(const char* path) {
    unsigned char st[144];
    g_last_path = path;
    if (__v64_syscall(V64_LX_STAT, (long)(unsigned long)path, (long)(unsigned long)st, 0, 0, 0) < 0)
        return -1;
    return (int)(st[48] | ((unsigned)st[49] << 8) | ((unsigned)st[50] << 16) | ((unsigned)st[51] << 24));
}

long long vs_free_bytes(const char* path) {
    unsigned char st[120];
    for (int i = 0; i < 120; i++) st[i] = 0;
    const long rc = __v64_syscall(137, (long)(unsigned long)path, (long)(unsigned long)st, 0, 0, 0);
    if (rc < 0) return -1;
    const long long bsize = (long long)(st[8] | ((unsigned)st[9] << 8) | ((unsigned)st[10] << 16) |
                                        ((unsigned)st[11] << 24));
    const long long bfree = (long long)(st[24] | ((unsigned)st[25] << 8) | ((unsigned)st[26] << 16) |
                                        ((unsigned)(unsigned long)st[27] << 24));
    if (bsize <= 0) return -1;
    return bsize * bfree;
}

/* ==================== ④ 目录枚举（自有 ABI 50/51/52） ==================== */
#define VS_DIR_ENT_HDR   12
#define VS_DIR_ENT_MAX   44
#define VS_DIR_BATCH     (VS_DIR_ENT_MAX * 5)

static char g_dirbuf[VS_DIR_BATCH];
static int  g_dirlen, g_diroff, g_dirfd = -1;

int vs_dir_open(const char* path) {
    if (g_dirfd >= 0) { (void)__v64_int80(52, g_dirfd, 0, 0, 0); g_dirfd = -1; }
    const long rc = __v64_int80(50, (long)(unsigned long)path, 0, 0, 0);
    if (rc < 0) return (int)rc;
    g_dirfd = (int)rc;
    g_dirlen = 0;
    g_diroff = 0;
    return g_dirfd;
}

int vs_dir_next(int fd, char* name, int cap, int* kind, int* size) {
    if (fd < 0) return -22;
    if (g_diroff >= g_dirlen) {
        const long got = __v64_int80(51, fd, (long)(unsigned long)g_dirbuf, VS_DIR_BATCH, 0);
        if (got < 0) return (int)got;
        if (got == 0) { g_dirlen = 0; g_diroff = 0; return 0; }
        g_dirlen = (int)got;
        g_diroff = 0;
    }
    const char* rec = g_dirbuf + g_diroff;
    const unsigned nl = (unsigned)((unsigned char)rec[0] | ((unsigned char)rec[1] << 8));
    if (nl == 0 || nl > 31) return -22;
    unsigned n = nl;
    if ((int)n > cap - 1) n = (unsigned)(cap - 1);
    for (unsigned i = 0; i < n; i++) name[i] = rec[VS_DIR_ENT_HDR + i];
    name[n] = 0;
    if (kind) *kind = (rec[2] == 2) ? 2 : 1;
    if (size) *size = (int)((unsigned char)rec[4] | ((unsigned char)rec[5] << 8) |
                            ((unsigned char)rec[6] << 16) | ((unsigned char)rec[7] << 24));
    g_diroff += (int)(VS_DIR_ENT_HDR + nl + 1);
    return 1;
}

int vs_dir_close(int fd) {
    if (fd < 0) return -9;
    if (fd == g_dirfd) g_dirfd = -1;
    return (int)__v64_int80(52, fd, 0, 0, 0);
}

/* ==================== ⑤ sha256（标准实现） ==================== */
static const uint32_t SH_K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

static uint32_t sh_ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sh_block(struct VsSha256* s, const unsigned char* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = sh_ror(w[i - 15], 7) ^ sh_ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = sh_ror(w[i - 2], 17) ^ sh_ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3];
    uint32_t e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = sh_ror(e, 6) ^ sh_ror(e, 11) ^ sh_ror(e, 25);
        const uint32_t ch = (e & f) ^ ((~e) & g);
        const uint32_t t1 = h + S1 + ch + SH_K[i] + w[i];
        const uint32_t S0 = sh_ror(a, 2) ^ sh_ror(a, 13) ^ sh_ror(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

void vs_sha256_init(struct VsSha256* s) {
    s->h[0] = 0x6a09e667u; s->h[1] = 0xbb67ae85u; s->h[2] = 0x3c6ef372u; s->h[3] = 0xa54ff53au;
    s->h[4] = 0x510e527fu; s->h[5] = 0x9b05688cu; s->h[6] = 0x1f83d9abu; s->h[7] = 0x5be0cd19u;
    s->bytes = 0;
    s->buflen = 0;
}

void vs_sha256_update(struct VsSha256* s, const unsigned char* p, int n) {
    s->bytes += (uint64_t)n;
    while (n > 0) {
        if (s->buflen == 0 && n >= 64) {                 /* 整块直通 */
            sh_block(s, p);
            p += 64;
            n -= 64;
            continue;
        }
        const int take = (n < 64 - s->buflen) ? n : (64 - s->buflen);
        for (int i = 0; i < take; i++) s->buf[s->buflen + i] = p[i];
        s->buflen += take;
        p += take;
        n -= take;
        if (s->buflen == 64) { sh_block(s, s->buf); s->buflen = 0; }
    }
}

void vs_sha256_final(struct VsSha256* s, char out_hex65[65]) {
    const uint64_t bits = s->bytes * 8ULL;
    unsigned char pad = 0x80;
    vs_sha256_update(s, &pad, 1);
    const unsigned char zero = 0;
    while (s->buflen != 56) vs_sha256_update(s, &zero, 1);
    unsigned char lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)((bits >> (56 - i * 8)) & 0xFFu);
    vs_sha256_update(s, lenb, 8);
    unsigned char raw[32];
    for (int i = 0; i < 8; i++) {
        raw[i * 4] = (unsigned char)(s->h[i] >> 24);
        raw[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
        raw[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
        raw[i * 4 + 3] = (unsigned char)(s->h[i]);
    }
    vs_hex(raw, 32, out_hex65);
}

/* ==================== mmap（Linux 9） ==================== */
void* vs_mmap(int len) {
    /* prot = 3（READ|WRITE）、flags = 0x22（MAP_PRIVATE|MAP_ANONYMOUS）；内核按 len 做 bump 分配 */
    const long rc = __v64_syscall(9, 0, (long)len, 3, 0x22, -1);
    if (rc < 0) return (void*)0;
    return (void*)(unsigned long)rc;
}

void* vs_malloc_stub(int len) { return vs_mmap(len); }

/* ==================== 错误码文案 ==================== */
const char* vs_err_name(int code) {
    switch (code) {
        case VS_OK:          return "ok";
        case VS_E_PATH:      return "path";
        case VS_E_HASH:      return "hash";
        case VS_E_SIZE:      return "size";
        case VS_E_DEPENDS:   return "depends";
        case VS_E_SPACE:     return "space";
        case VS_E_INSTALLED: return "installed";
        case VS_E_NOTINST:   return "not-installed";
        case VS_E_FORMAT:    return "format";
        case VS_E_SCRIPTS:   return "scripts";
        case VS_E_XZ:        return "xz";
        case VS_E_NOTFOUND:  return "notfound";
        case VS_E_IO:        return "io";
        case VS_E_NOMEM:     return "nomem";
        case VS_E_PERM:      return "perm";
        default:             return "unknown";
    }
}

/* ==================== 日志（**一行一次 printf**） ====================
 * ★ 曾经踩过的坑：把前缀与正文拆成两次 printf（或 printf + vprintf），内核的 [SYSCALL] 追踪行
 *   会插在两次 write() 中间 —— 串口上那行就断了（`[VPKG] ` + 追踪行 + 正文），验收脚本按行
 *   grep 全红。所以这里**先把整行拼进缓冲**，最后只调一次 printf。 */
void vs_log(const char* tag, const char* fmt, ...) {
    char line[256];
    int n = vs_fmt(line, (int)sizeof(line), "[%s] ", tag);
    if (n < 0) n = 0;
    va_list ap;
    va_start(ap, fmt);
    n += vs_vfmt(line + n, (int)sizeof(line) - n, fmt, ap);
    va_end(ap);
    /* ★ 为什么直接 write(1) 而不是 printf：user/lib 的 stdout 是 **128 字节行缓冲**，
     *   超过 128 B 的一行会被拆成两次 write()，内核的 [SYSCALL] 追踪行就插进那行中间
     *   （实测 `[VPKG] pkg name=… summary="…"` 与 `[VPKG] info name=… sha256=…` 都被劈开）。
     *   先把 stdout 里已缓冲的字节刷出去，再用**一次** write(1) 落整行 —— 串口上就是原子行。 */
    vimtu64_stdout_flush64();
    if (n > 0) (void)write(1, line, (size_t)vs_strlen(line));
}

/* ==================== 极简格式化输出（本运行时的 stdio 没有 sprintf） ====================
 * 支持：%s %d %u %x %c %% ；够本模块写路径/JSON 片段用。返回写入的字符数（不含 NUL），
 * 截断时按有界写（不越界，也不假装写完）。 */
int vs_vfmt(char* dst, int cap, const char* fmt, va_list ap) {
    int o = 0;
    for (int i = 0; fmt[i] && o < cap - 1; i++) {
        if (fmt[i] != '%') { dst[o++] = fmt[i]; continue; }
        i++;
        if (fmt[i] == 0) break;
        if (fmt[i] == '%') { dst[o++] = '%'; continue; }
        if (fmt[i] == 'c') {
            const int c = va_arg(ap, int);
            dst[o++] = (char)c;
            continue;
        }
        if (fmt[i] == 's') {
            const char* s = va_arg(ap, const char*);
            for (int k = 0; s && s[k] && o < cap - 1; k++) dst[o++] = s[k];
            continue;
        }
        if (fmt[i] == 'd' || fmt[i] == 'u' || fmt[i] == 'x') {
            const int base = (fmt[i] == 'x') ? 16 : 10;
            long v = (long)va_arg(ap, int);
            char t[24];
            int k = 0, neg = 0;
            if (v < 0 && base == 10) { neg = 1; v = -v; }
            if (v == 0) t[k++] = '0';
            while (v > 0) {
                const int d = (int)(v % base);
                t[k++] = (char)((d < 10) ? ('0' + d) : ('a' + d - 10));
                v /= base;
            }
            if (neg && o < cap - 1) dst[o++] = '-';
            while (k > 0 && o < cap - 1) dst[o++] = t[--k];
            continue;
        }
        if (fmt[i] == 'l' && (fmt[i + 1] == 'd' || fmt[i + 1] == 'u')) {
            long v = va_arg(ap, long);
            char t[24];
            int k = 0, neg = 0;
            if (v < 0) { neg = 1; v = -v; }
            if (v == 0) t[k++] = '0';
            while (v > 0) { t[k++] = (char)('0' + (int)(v % 10)); v /= 10; }
            if (neg && o < cap - 1) dst[o++] = '-';
            while (k > 0 && o < cap - 1) dst[o++] = t[--k];
            i++;
            continue;
        }
        dst[o++] = fmt[i];                     /* 不认识的转换：按字面（绝不假装算过） */
    }
    dst[o] = 0;
    return o;
}

/* 变参包装（调用方给 va_list 时用；vs_log 就是靠它做到"整行一次 printf"） */
int vs_fmt(char* dst, int cap, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const int n = vs_vfmt(dst, cap, fmt, ap);
    va_end(ap);
    return n;
}
