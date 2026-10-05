/* vs.h - ★ 应用商店 + 包管理器（/bin/vpkg 与 /bin/store）的共用户头（纯用户态，ring3）
 *
 * 这一层只放**约定**：错误码、结构、路径、以及三个模块的对外函数。
 * 实现分工（每个 .c 都有一句"这是什么"）：
 *   vs_io.c     文件读写/mkdir_p/statfs/目录枚举（走 Linux 号段与自有 ABI 50/51/52）+
 *               sha256 + 错误码文案 + 小工具（十六进制/十进制）
 *   vs_gzip.c   gzip/DEFLATE 解压（**同一实现**：逐段抄自 user/gzip/gzip.c 的 inflate，
 *               见该文件头部；不是"另起一套"）
 *   vs_pkg.c    仓库索引 /opt/vpkg/index.json、已装库 /var/lib/vpkg/installed.json、
 *               .vap64（kernel/app64.h 布局）与 .deb（ar + control.tar.gz + data.tar.gz）
 *               的解析与安装/卸载引擎（CLI 与 GUI **共用同一份**）
 *   vpkg.c      /bin/vpkg 命令行（list / info / install / remove / search / update）
 *   store.c     /bin/store GUI 商店（/lib/wm.elf 合成器 + shm surface + user/lib/font64.h）
 *
 * 纪律（与仓库其它用户态程序同一条）：
 *   * 一行日志**只准一次 printf**（本 libc 换行即 flush；否则 [SYSCALL] 追踪行会插进中间）；
 *   * 拿不到证据就如实报错，**绝不假装成功**；
 *   * 内核零改动：本目录所有程序都只用既有 ABI（自有 int 0x80 与 Linux 兼容号段）。
 */
#ifndef VIMTU_STORE_VS_H
#define VIMTU_STORE_VS_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>


/* ---------------- 错误码（★ 每条都要在日志里**分得开**） ---------------- */
#define VS_OK          0
#define VS_E_PATH      1    /* 路径/名字非法（含 /、.. 、空格、超长） */
#define VS_E_HASH      2    /* sha256 与索引不符 */
#define VS_E_SIZE      3    /* 字节数与索引声明不符 */
#define VS_E_DEPENDS   4    /* 缺依赖 */
#define VS_E_SPACE     5    /* 空间不足 */
#define VS_E_INSTALLED 6    /* 已安装（同版本） */
#define VS_E_NOTINST   7    /* 没装过（卸载/更新时） */
#define VS_E_FORMAT    8    /* 包容器坏（VAP64 头/ar/tar/gzip） */
#define VS_E_SCRIPTS   9    /* deb 带维护者脚本（preinst/postinst/prerm/postrm） */
#define VS_E_XZ       10    /* deb 用 xz 压缩（本实现不支持） */
#define VS_E_NOTFOUND 11    /* 仓库/已装库里没有这个名字 */
#define VS_E_IO       12    /* 读写失败 */
#define VS_E_NOMEM    13    /* mmap/缓冲不够 */
#define VS_E_PERM     14    /* 目标目录本会话不可写（P4 权限真拦截：-EACCES；装系统目录要 root） */

const char* vs_err_name(int code);      /* "hash" / "depends" / ...（日志与退出码都用它） */

/* ---------------- 上限（全部静态分配；用户窗口只有 16 MiB） ---------------- */
#define VS_MAX_PKGS     8         /* 索引/已装库最多几条（超出如实报错，不静默截断） */
#define VS_NAME_MAX     32        /* VimtuFS2 名字上限 31 + NUL */
#define VS_VER_MAX      16
#define VS_ARCH_MAX     12
#define VS_TYPE_MAX     8
#define VS_FILE_MAX     128
#define VS_DESC_MAX     72
#define VS_SUM_MAX      56
#define VS_DEPENDS_MAX  2         /* 依赖解析深度：一层（见 vs_pkg.c 的如实边界） */
#define VS_FILES_MAX    12        /* 一个包最多记几个落盘文件 */
#define VS_KEY_MAX      24

#define VS_REPO_DIR     "/opt/vpkg/repo"
#define VS_INDEX_PATH   "/opt/vpkg/index.json"
#define VS_DB_DIR       "/var/lib/vpkg"
#define VS_DB_PATH      "/var/lib/vpkg/installed.json"
#define VS_BIN_DIR      "/bin"
#define VS_SHARE_DIR    "/usr/share"
#define VS_LIB_DIR      "/lib"
#define VS_WM_PATH      "/lib/wm.elf"          /* Ring 3 合成器（内核只认 /bin/wm.elf，桌面期由客户端 fork） */
#define VS_WM_ALT_PATH  "/bin/wm.elf"

/* 安装/卸载一次的日志前缀：CLI = "VPKG"，GUI = "STORE"（两份证据长一样，便于对账） */
#define VS_TAG_CLI      "VPKG"
#define VS_TAG_GUI      "STORE"

struct VsPkg {                    /* 仓库索引里的一条（= 一份可安装的包） */
    char  name[VS_NAME_MAX];
    char  version[VS_VER_MAX];
    char  arch[VS_ARCH_MAX];
    char  type[VS_TYPE_MAX];      /* "vap64" | "deb" */
    char  file[VS_FILE_MAX];      /* 相对 /opt/vpkg/repo 的文件名 */
    char  summary[VS_SUM_MAX];
    char  desc[VS_DESC_MAX];
    int   size;                   /* 索引声明的字节数（与盘上文件**必须**一致） */
    char  sha256[65];
    int   ndep;
    char  depends[VS_DEPENDS_MAX][VS_NAME_MAX];
};

struct VsInstalled {              /* /var/lib/vpkg/installed.json 里的一条 */
    char  name[VS_NAME_MAX];
    char  version[VS_VER_MAX];
    char  arch[VS_ARCH_MAX];
    char  type[VS_TYPE_MAX];
    int   bytes;
    char  sha256[65];
    int   nfiles;
    char  files[VS_FILES_MAX][VS_FILE_MAX];
};

/* ---------------- vs_io.c ---------------- */
unsigned long vs_strlen(const char* s);
int  vs_streq(const char* a, const char* b);
int  vs_strcpy(char* dst, int cap, const char* src);          /* 0 = 成功，-1 = 装不下 */
int  vs_starts(const char* s, const char* pre);
int  vs_find(const char* hay, const char* needle);            /* 子串下标 / -1 */
int  vs_icontains(const char* hay, const char* needle);       /* 大小写不敏感的 contains */

/* 读整个文件到 buf（cap 上限；返回实际字节数，-1 = 打开失败，-2 = 比 cap 大） */
int  vs_read_file(const char* path, unsigned char* buf, int cap);
/* 写整个文件（O_WRONLY|O_CREAT|O_TRUNC）；返回 0 / -errno */
int  vs_write_file(const char* path, const unsigned char* buf, int len);
int  vs_unlink(const char* path);                             /* 0 / -errno */
int  vs_mkdir(const char* path);                              /* 0 / -errno */
int  vs_mkdir_p(const char* dir);                             /* 逐级 mkdir；0 / -errno（已存在算成功） */
int  vs_exists(const char* path);                             /* 1 / 0 */
int  vs_is_dir(const char* path);
int  vs_size_of(const char* path);                            /* >=0 / -1 */
long long vs_free_bytes(const char* path);                    /* statfs(137) 的 f_bfree*512；<0 = 失败 */
/* 打开目录句柄（自有 ABI 50）：>=0 / 负错误码；vs_dir_next 返回 1/0/<0 */
int  vs_dir_open(const char* path);
int  vs_dir_next(int fd, char* name, int cap, int* kind, int* size);
int  vs_dir_close(int fd);

void vs_hex(const unsigned char* b, int n, char* out);        /* 小写十六进制 + NUL */
int  vs_hex2ascii(const char* hex, unsigned char* out, int cap); /* -1 = 非法 */
int  vs_atoi(const char* s);
int  vs_json_esc(char* dst, int cap, const char* src);        /* JSON 字符串转义（读用） */

/* sha256 */
struct VsSha256 { uint32_t h[8]; uint64_t bytes; unsigned char buf[64]; int buflen; };
void vs_sha256_init(struct VsSha256* s);
void vs_sha256_update(struct VsSha256* s, const unsigned char* p, int n);
void vs_sha256_final(struct VsSha256* s, char out_hex65[65]);

/* ---------------- vs_gzip.c（同一个 inflate 实现，源 = user/gzip/gzip.c） ---------------- */
int  vs_gunzip(const unsigned char* in, int n, unsigned char* out, int outcap, int* out_len);
unsigned int vs_crc32(const unsigned char* p, int n);

/* ---------------- mmap（Linux 号段 9）：大缓冲一律走它，静态缓冲保持小 ---------------- */
void* vs_mmap(int len);
void* vs_malloc_stub(int len);       /* = vs_mmap（没有 free：本次运行期的如实边界） */

/* ---------------- vs_pkg.c：仓库 / 已装库 / 安装引擎 ---------------- */
/* 读索引（失败返回负错误码；*n 写条数） */
int  vs_index_load(const char* path, struct VsPkg* out, int cap, int* n);
/* 读已装库（不存在 = 0 条，返回 0） */
int  vs_db_load(const char* path, struct VsInstalled* out, int cap, int* n);
int  vs_db_write(const char* path, const struct VsInstalled* in, int n, int* out_bytes);
/* 在索引/已装库里的查找（-1 = 没有） */
int  vs_index_find(const struct VsPkg* a, int n, const char* name);
int  vs_db_find(const struct VsInstalled* a, int n, const char* name);

/* 安装/卸载（CLI 与 GUI 共用；tag = "VPKG" / "STORE"，日志前缀）。
 * arg 可以是索引里的名字，也可以是 /path/x.vap64 或 /path/x.deb（含 '/' 时按文件走）。
 * 返回 VS_OK 或 VS_E_*；返回值同时决定 CLI 的退出码（非 0 = 失败）。 */
int  vs_install(const char* tag, const char* arg, int do_progress);
int  vs_remove(const char* tag, const char* name);
int  vs_list(const char* tag);                       /* 仓库 + 安装状态 */
int  vs_info(const char* tag, const char* name);
int  vs_search(const char* tag, const char* kw);
int  vs_update(const char* tag);                     /* 按索引版本重装不一致的包 */

/* 已装库的微内存缓存（GUI 每次重画要用；避免每帧读盘） */
int  vs_cached_installed(struct VsInstalled* out, int cap, int* n);
int  vs_cached_repo(struct VsPkg* out, int cap, int* n);
void vs_cache_drop(void);

/* ---------------- 通用日志（一行一次 printf） ---------------- */
void vs_log(const char* tag, const char* fmt, ...);

/* 极简格式化输出（本运行时的 stdio 没有 sprintf）：%s %d %u %x %c %% %ld %lu */

/* 指针版表访问（GUI 用；表本体在 mmap 区，见 vs_pkg.c 的体积说明） */
const struct VsInstalled* vs_inst_table(int* n);

/* 变参版（vs_log 用它拼"整行"；见 vs_io.c 的日志说明） */
int  vs_vfmt(char* dst, int cap, const char* fmt, va_list ap);
const struct VsPkg*       vs_repo_table(int* n);
int  vs_fmt(char* dst, int cap, const char* fmt, ...);

#endif /* VIMTU_STORE_VS_H */
