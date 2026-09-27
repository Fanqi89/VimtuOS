/* main.c - ★ A4-1：**Ring 3 shell**（Vimtu64；静态链接，用 user/lib 的自研最小 libc + 自有 ABI 包装）
 *
 * 这个程序从哪来（交付方式，**不内嵌内核**）：
 *   它是**系统卷里的一个文件** /bin/shell.bin（build64.sh 用 tools/make_shellvol.py 把它写进
 *   VimtuFS2 卷；内核只负责"按请求把它当 ELF 装载进用户态"）。内核镜像里一个字节的 shell
 *   代码都没有 —— 见 kernel/terminal64.cpp 的 cmd_shell / sh64_start64。
 *
 * 谁来跑它：终端窗口里敲 `shell`（别名 sh）-> 内核 proc64_create64 + proc64_start_elf64
 *   真进程 + 独立 CR3。本进程的 stdin/stdout **不是 fd 0/1**，而是"终端服务"共享邮箱
 *   （struct Sh64Mail，见 user/shell/sh64.h）—— 数据流：
 *     按键  : 终端窗口 -> term_key -> 邮箱 in 环 -> 本程序 read_line（回显也走 out 环）
 *     输出  : 本程序 out_* -> 邮箱 out 环 -> 内核 term_tick 抽干 -> 终端窗口 + 串口
 *     服务  : 本程序 rpc_send_ls -> out 环（行首 0x01）-> 内核 fd64 目录原语 ->
 *             应答行走 in 环（行首 0x02）-> 本程序 rpc_line
 *   外部命令（run）走 fork(57) + execve(59) + wait4(61) 真进程；它的 stdout 是**内核控制台**
 *   （串口 + 屏幕左上角覆盖层），不是这个窗口 —— 本轮如实边界（内核 dup2 到 0/1 是 -ENOSYS，
 *   见 abi.c 的说明），报告里列明。
 *
 * 内置命令：help echo pwd cd ls cat stat mkdir rm run exit（未知命令按外部程序试跑）
 * 重定向  ：> >> <（内置命令；外部命令的 > / < 不做 —— 同一个 dup2 边界）
 * 管道    ：|（内置命令之间；实现 = 上一段的输出捕获进内存缓冲，作为下一段的输入）
 * 不做（如实）：'~' 展开（没有 home 查询的口子）、引号/转义、通配符、后台任务、$?/$变量。
 */
#include <stddef.h>
#include <stdint.h>

#include "abi.h"
#include "sh64.h"

/* ==================== 常量 ==================== */
#define SH_LINE_MAX   128     /* 命令行缓冲（内核把整行按键灌进 in 环，这里只按行收） */
#define SH_PATH_MAX   128     /* 与内核 VFS64_PATH_MAX 一致 */
#define SH_CAP_MAX    2048    /* 管道/`<` 每一步的捕获缓冲（三块各 2KB：装载窗口 64KiB 内要留余量） */
#define SH_PEND_MAX   256     /* 按键挂起队列（内核 in 环满时会丢键，这里也做一次上界） */
#define SH_ARGV_MAX   12
#define SH_STAGES_MAX 4

/* Linux 的 open 标志（与 user/lib/fcntl.h、kernel/fd64.h 的取值一致） */
#define SH_O_RDONLY 00000000
#define SH_O_WRONLY 00000001
#define SH_O_CREAT  00000100
#define SH_O_TRUNC  00001000
#define SH_O_APPEND 00002000

/* ==================== 状态 ==================== */
static struct Sh64Mail* g_mb;                 /* 共享邮箱（固定 VA，见 sh64.h） */
/* shell 自己的 cwd：内核没有 chdir(80)（syscall64.cpp 的表里没有这个号），所以相对路径
 * 由本程序解析成绝对路径 —— 这是"cd 生效"的**唯一**实现点，如实写在报告里。 */
static char g_cwd[SH_PATH_MAX] = "/";

/* 输出汇（一次 run_stage 期间唯一）：
 *   g_cap != 0    -> 捕获进内存（管道的前几段）
 *   g_out_fd >= 0 -> 写这个文件 fd（`>` / `>>`）
 *   否则          -> 邮箱（-> 终端窗口 + 串口） */
static char* g_cap;
static int   g_cap_len;
static int   g_cap_ovf;
static int   g_out_fd = -1;

/* 输入汇（`<` 文件 或 上一段捕获；只有 cat 会消费） */
static const char* g_in;
static int         g_in_len;

/* 按键挂起队列 + 服务应答行收集 */
static char g_pend[SH_PEND_MAX];
static int  g_pend_n;
static int  g_ctrl;                            /* 1 = 正在收一行 0x02 开头的服务应答 */
static char g_ctrl_buf[192];
static int  g_ctrl_n;
static int  g_rpc_state;                       /* 0 = 空闲、1 = 等应答、2 = 已完成 */
static int  g_rpc_err;                         /* 应答 "E <n>" 里的 n */

/* 捕获缓冲（两个交替：当前段写 A 时，输入来自 B，绝不重叠） */
static char g_cap_a[SH_CAP_MAX];
static char g_cap_b[SH_CAP_MAX];
static char g_in_slot[SH_CAP_MAX];
static char* g_cur_slot = g_cap_a;             /* exec_line 每段之前设置 */

/* ==================== 邮箱（共享页）==================== */
static int  mb_wait_drain(void);
static int  mb_out_bytes(const char* p, int n);
static void rpc_line(const char* s, int n);

/* 探测邮箱在不在：read(3, 邮箱VA, 1) —— 内核 read 先做 user64_range_ok64（未映射 -> -EFAULT），
 * 再走 fd 层（fd 3 不存在 -> -EBADF）。所以：
 *   -14（EFAULT）= 没映射 = **不是从终端服务启动的**（例如手工 elfrun /bin/shell.bin）
 *   其它值       = 映射好了
 * 价值：没有邮箱时**不崩**（不撞 #PF），而是给一句清楚的话然后退出。 */
static int mb_probe(void) {
    const long r = sh_read(3, (void*)(uintptr_t)SH64_MAIL_VA64, 1);
    return (r == -14) ? 0 : 1;
}

static void mb_init(void) {
    char* p = (char*)(uintptr_t)SH64_MAIL_VA64;
    for (unsigned i = 0; i < 4096u; i++) p[i] = 0;       /* 整页清零（内核只保证"物理页已映射"）*/
    g_mb = (struct Sh64Mail*)(uintptr_t)SH64_MAIL_VA64;
    g_mb->in_w = 0; g_mb->in_r = 0; g_mb->out_w = 0; g_mb->out_r = 0;
    g_mb->magic = SH64_MAGIC;                            /* 最后一笔：宣布"就绪" */
}

/* 等内核把 out 环抽空：最多 ~3 秒（250Hz tick -> 750 tick） */
static int mb_wait_drain(void) {
    const unsigned long t0 = sh_ticks();
    while (g_mb->out_r == g_mb->out_w) {
        if (sh_ticks() - t0 > 750ul) return 0;
        sh_sleep_ms(2);
    }
    return 1;
}

/* out 环写 n 字节：环满就等内核抽干（有界；真的超时就丢并返回 -1，绝不假装写进去了） */
static int mb_out_bytes(const char* p, int n) {
    int sent = 0;
    while (sent < n) {
        volatile struct Sh64Mail* m = g_mb;
        const unsigned w = m->out_w, r = m->out_r;
        unsigned space = (r + SH64_OUT_CAP - 1u - w) % SH64_OUT_CAP;   /* 留一格区分空/满 */
        if (space == 0) {
            if (!mb_wait_drain()) return -1;
            continue;
        }
        unsigned chunk = (unsigned)(n - sent);
        if (chunk > space) chunk = space;
        for (unsigned i = 0; i < chunk; i++) m->outb[(w + i) % SH64_OUT_CAP] = p[sent + (int)i];
        m->out_w = (w + chunk) % SH64_OUT_CAP;
        sent += (int)chunk;
    }
    return 0;
}

/* in 环读一字节：-1 = 暂时没有 */
static int mb_in_get(void) {
    volatile struct Sh64Mail* m = g_mb;
    if (m->in_r == m->in_w) return -1;
    const int c = (unsigned char)m->inb[m->in_r];
    m->in_r = (m->in_r + 1u) % SH64_IN_CAP;
    return c;
}

/* ==================== 输出 ==================== */
static void out_bytes(const char* p, int n);
static void out_str(const char* s);
static void out_dec(long v);
static void out_hex(unsigned long v);
static void out_ch(char c);

static void out_bytes(const char* p, int n) {
    if (n <= 0) return;
    if (g_cap) {                                     /* 捕获（管道的前几段） */
        for (int i = 0; i < n; i++) {
            if (g_cap_len < SH_CAP_MAX) g_cap[g_cap_len++] = p[i];
            else { g_cap_ovf = 1; break; }
        }
        return;
    }
    if (g_out_fd >= 0) {                             /* 重定向到文件 */
        int off = 0;
        while (off < n) {
            const long w = sh_write(g_out_fd, p + off, (unsigned long)(n - off));
            if (w <= 0) return;                      /* 写失败：停止（不假装写完） */
            off += (int)w;
        }
        return;
    }
    (void)mb_out_bytes(p, n);                        /* 正常：邮箱 -> 终端窗口 */
}

static void out_str(const char* s) {
    int n = 0;
    while (s[n]) n++;
    out_bytes(s, n);
}

static void out_dec(long v) {
    char t[24];
    int n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) out_ch('-');
    if (u == 0) t[n++] = '0';
    while (u) { t[n++] = (char)('0' + (int)(u % 10ul)); u /= 10ul; }
    while (n > 0) out_ch(t[--n]);
}

static void out_hex(unsigned long v) {
    static const char H[] = "0123456789abcdef";
    char t[17];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = H[v & 0xful]; v >>= 4; }
    while (n > 0) out_ch(t[--n]);
}

static void out_ch(char c) { out_bytes(&c, 1); }

/* ==================== 输入泵 ==================== */
static int pump_mailbox(void) {
    int got = 0;
    for (;;) {
        const int c = mb_in_get();
        if (c < 0) break;
        got = 1;
        if (g_ctrl) {                                /* 收一行服务应答（0x02 开头，到 '\n'） */
            if (c == '\n') { g_ctrl = 0; rpc_line(g_ctrl_buf, g_ctrl_n); g_ctrl_n = 0; }
            else if (g_ctrl_n < (int)sizeof(g_ctrl_buf) - 1) g_ctrl_buf[g_ctrl_n++] = (char)c;
            continue;
        }
        if (c == SH64_RSP_LS && g_rpc_state == 1) { g_ctrl = 1; g_ctrl_n = 0; continue; }
        if (g_pend_n < (int)sizeof(g_pend)) g_pend[g_pend_n++] = (char)c;   /* 普通按键 */
    }
    return got;
}


static int pend_get(void) {
    for (;;) {
        if (g_pend_n > 0) {
            const int c = (unsigned char)g_pend[0];
            for (int i = 1; i < g_pend_n; i++) g_pend[i - 1] = g_pend[i];
            g_pend_n--;
            return c;
        }
        if (!pump_mailbox()) sh_sleep_ms(3);
    }
}

/* ==================== 服务：目录列表（内核没有 getdents(217)，见报告）==================== */
static int rpc_wait(unsigned timeout_ms) {
    const unsigned long t0 = sh_ticks();
    while (g_rpc_state == 1) {
        if (!pump_mailbox()) sh_sleep_ms(2);
        if ((sh_ticks() - t0) * 4ul > timeout_ms) { g_rpc_state = 0; g_rpc_err = 9; break; }
    }
    return g_rpc_err;
}

/* 发一条服务请求（**直接进邮箱**，不经过输出汇：重定向时输出进文件，请求仍走服务） */
static void rpc_send_ls(const char* path) {
    char req[192];
    int n = 0;
    req[n++] = (char)SH64_REQ_LS;
    req[n++] = 'L';
    req[n++] = 'S';
    req[n++] = ' ';
    for (int i = 0; path[i] && n < (int)sizeof(req) - 2; i++) req[n++] = path[i];
    req[n++] = '\n';
    (void)mb_out_bytes(req, n);
}

/* 应答行（行首的 0x02 已在泵里吃掉）：'D <name>' / 'F <size> <name>' / 'E <n>' */
static void rpc_line(const char* s, int n) {
    if (g_rpc_state != 1 || n < 1) return;
    if (s[0] == SH64_LS_END) {
        int v = 0, i = 1;
        while (i < n && s[i] == ' ') i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        g_rpc_err = v;
        g_rpc_state = 2;
        return;
    }
    /* 条目：直接打印（'D <name>' -> "name/"；'F <size> <name>' -> "name  N bytes"）。
     * ★ 已知边界（内核侧既有缺陷，fd64.cpp 本轮不许改，已写进报告）：fd64_readdir64 对
     *   "大小 0"的条目用 "斜杠+名字"（相对卷根，而不是相对被列目录）去 stat 判类型 —— 所以
     *   子目录里的子目录会被报成 FILE（显示成 "sub  0 bytes" 而不是 "sub/"）。根目录不受影响。*/
    if (s[0] == SH64_LS_DIR && n > 2) {              /* "D <name>" */
        out_str("  ");
        out_bytes(s + 2, n - 2);
        out_str("/\n");
        return;
    }
    if (s[0] == SH64_LS_FILE && n > 2) {             /* "F <size> <name>" */
        int i = 1;
        long sz = 0;
        while (i < n && s[i] == ' ') i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') { sz = sz * 10 + (s[i] - '0'); i++; }
        while (i < n && s[i] == ' ') i++;
        out_str("  ");
        out_bytes(s + i, n - i);
        out_str("  ");
        out_dec(sz);
        out_str(" bytes\n");
    }
}

/* ==================== 路径解析 ====================
 * 支持：绝对/相对、'.'、'..'（根的 .. 还是根）、连续 '/'、结尾 '/'（忽略）。
 * 不支持（如实）：'~'（内核没有给 ring3 的 home 查询口子）、符号链接（文件系统没有）。 */
static int path_resolve(const char* in, char* out, int cap) {
    const char* segs[24];
    int seg_len[24];
    int nseg = 0;
    if (!in || !in[0]) return -1;

    int i = 0;
    if (in[0] != '/') {                              /* 相对：从 cwd 起步 */
        if (g_cwd[0] != '/') return -1;
        for (;;) {
            while (g_cwd[i] == '/') i++;
            if (!g_cwd[i]) break;
            const int s0 = i;
            while (g_cwd[i] && g_cwd[i] != '/') i++;
            if (nseg < 24) { segs[nseg] = g_cwd + s0; seg_len[nseg] = i - s0; nseg++; }
        }
        i = 0;
    }
    if (in[0] == '~') return -1;
    while (in[i]) {
        while (in[i] == '/') i++;
        if (!in[i]) break;
        const int s0 = i;
        while (in[i] && in[i] != '/') i++;
        const int len = i - s0;
        if (len == 1 && in[s0] == '.') continue;
        if (len == 2 && in[s0] == '.' && in[s0 + 1] == '.') {
            if (nseg > 0) nseg--;
            continue;
        }
        if (len > 31) return -1;                     /* VimtuFS2 单段名字上限 31 */
        for (int k = 0; k < len; k++) {
            const unsigned char ch = (unsigned char)in[s0 + k];
            if (ch < 0x21 || ch > 0x7E) return -1;
        }
        if (nseg >= 24) return -1;                   /* 层数上界（内核 VFS64_PATH_DEPTH_MAX=16） */
        segs[nseg] = in + s0;
        seg_len[nseg] = len;
        nseg++;
    }

    int o = 0;
    if (nseg == 0) {
        if (cap < 2) return -1;
        out[o++] = '/';
    } else {
        for (int k = 0; k < nseg; k++) {
            if (o >= cap - 1) return -1;
            out[o++] = '/';
            for (int m = 0; m < seg_len[k]; m++) {
                if (o >= cap - 1) return -1;
                out[o++] = segs[k][m];
            }
        }
    }
    out[o] = 0;
    return 0;
}

/* ==================== 内置命令 ==================== */
static int bi_help(int argc, char** argv);
static int bi_echo(int argc, char** argv);
static int bi_pwd(int argc, char** argv);
static int bi_cd(int argc, char** argv);
static int bi_ls(int argc, char** argv);
static int bi_cat(int argc, char** argv);
static int bi_stat(int argc, char** argv);
static int bi_mkdir(int argc, char** argv);
static int bi_rm(int argc, char** argv);
static int run_external(int argc, char** argv);

static int bi_ls(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc > 2) { out_str("ls: usage: ls [path]\n"); return 2; }
    const char* want = (argc > 1) ? argv[1] : g_cwd;
    if (path_resolve(want, path, (int)sizeof(path)) != 0) {
        out_str("ls: bad path: "); out_str(want); out_str("\n");
        return 2;
    }
    out_str(path); out_str(":\n");
    g_rpc_err = 0;
    g_rpc_state = 1;
    rpc_send_ls(path);                            /* 应答行由 rpc_line 直接打印（见那里的说明） */
    const int err = rpc_wait(4000);               /* 等应答（期间按键进挂起队列） */
    g_rpc_state = 0;
    if (err == 9) { out_str("ls: no reply from the terminal service\n"); return 9; }
    if (err != 0) {
        out_str("ls: cannot list "); out_str(path);
        out_str(" (service err="); out_dec(err); out_str(")\n");
        return 1;
    }
    return 0;                                        /* 空目录：只打目录名（与 Linux 一致） */
}

static int bi_cat(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc == 1) {                                 /* 无参数：消费输入汇（管道或 `<`） */
        if (g_in && g_in_len > 0) { out_bytes(g_in, g_in_len); return 0; }
        out_str("cat: no input (not a pipeline stage and no '<' redirect)\n");
        return 2;
    }
    int rc = 0;
    for (int a = 1; a < argc; a++) {
        if (path_resolve(argv[a], path, (int)sizeof(path)) != 0) {
            out_str("cat: bad path: "); out_str(argv[a]); out_str("\n"); rc = 2; continue;
        }
        const int fd = sh_open(path, SH_O_RDONLY);
        if (fd < 0) { out_str("cat: cannot open "); out_str(path); out_str("\n"); rc = 1; continue; }
        char buf[512];
        for (;;) {
            const long n = sh_read(fd, buf, sizeof buf);
            if (n <= 0) break;
            out_bytes(buf, (int)n);
        }
        (void)sh_close(fd);
    }
    return rc;
}

static int bi_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) out_ch(' ');
        out_str(argv[i]);
    }
    out_ch('\n');
    return 0;
}

static int bi_pwd(int argc, char** argv) {
    (void)argc; (void)argv;
    out_str(g_cwd); out_ch('\n');
    return 0;
}

static int bi_cd(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc > 2) { out_str("cd: usage: cd [path]\n"); return 2; }
    if (path_resolve((argc > 1) ? argv[1] : "/", path, (int)sizeof(path)) != 0) {
        out_str("cd: bad path\n"); return 2;
    }
    struct { long v[18]; } st;                       /* 144 B = Linux struct stat 的尺寸 */
    if (sh_stat(path, &st) != 0) {
        out_str("cd: no such directory: "); out_str(path); out_str("\n"); return 1;
    }
    const unsigned mode = (unsigned)(st.v[3] & 0xFFFFl);     /* st_mode 在 +24 */
    if ((mode & 0170000u) != 0040000u) {
        out_str("cd: not a directory: "); out_str(path); out_str("\n"); return 1;
    }
    int i = 0;
    while (path[i] && i < SH_PATH_MAX - 1) { g_cwd[i] = path[i]; i++; }
    g_cwd[i] = 0;
    return 0;
}

static int bi_mkdir(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc != 2) { out_str("mkdir: usage: mkdir <path>\n"); return 2; }
    if (path_resolve(argv[1], path, (int)sizeof(path)) != 0) { out_str("mkdir: bad path\n"); return 2; }
    const int r = sh_mkdir(path, 0755);
    if (r != 0) {
        out_str("mkdir: failed "); out_str(path);
        out_str(" (err="); out_dec(-r); out_str(")\n");
        return 1;
    }
    out_str("mkdir: ok "); out_str(path); out_ch('\n');
    return 0;
}

static int bi_rm(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc != 2) { out_str("rm: usage: rm <file>\n"); return 2; }
    if (path_resolve(argv[1], path, (int)sizeof(path)) != 0) { out_str("rm: bad path\n"); return 2; }
    const int r = sh_unlink(path);
    if (r != 0) {
        out_str("rm: failed "); out_str(path);
        out_str(" (err="); out_dec(-r); out_str(")\n");
        return 1;
    }
    out_str("rm: ok "); out_str(path); out_ch('\n');
    return 0;
}

static int bi_stat(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (argc != 2) { out_str("stat: usage: stat <path>\n"); return 2; }
    if (path_resolve(argv[1], path, (int)sizeof(path)) != 0) { out_str("stat: bad path\n"); return 2; }
    struct { long v[18]; } st;
    for (int i = 0; i < 18; i++) st.v[i] = 0;
    if (sh_stat(path, &st) != 0) {
        out_str("stat: no such file: "); out_str(path); out_str("\n"); return 1;
    }
    const unsigned long mode = (unsigned long)(st.v[3] & 0xFFFFl);
    const unsigned long ifmt = mode & 0170000ul;
    out_str("  file: "); out_str(path); out_ch('\n');
    out_str("  type: ");
    out_str((ifmt == 0040000ul) ? "directory" : ((ifmt == 0100000ul) ? "regular" : "other"));
    out_ch('\n');
    out_str("  size: "); out_dec(st.v[6]); out_ch('\n');           /* st_size 在 +48 */
    out_str("  mode: 0"); out_hex(mode & 07777ul); out_ch('\n');
    out_str("  uid: "); out_dec((long)((unsigned long)st.v[3] >> 32) & 0xFFFFl); out_ch('\n');
    out_str("  gid: "); out_dec((long)((unsigned long)st.v[3] >> 48) & 0xFFFFl); out_ch('\n');
    return 0;
}

static int bi_help(int argc, char** argv) {
    (void)argc; (void)argv;
    out_str("VimtuOS ring3 shell (sh64) - built-in commands:\n"
            "  help                 this help\n"
            "  echo TEXT...         print TEXT\n"
            "  pwd                  print the shell cwd\n"
            "  cd [PATH]            change the shell cwd (kernel has no chdir(80))\n"
            "  ls [PATH]            list a directory (terminal service -> kernel fd64 readdir)\n"
            "  cat FILE...          print files ('cat' alone reads a pipeline/'<' input)\n"
            "  stat PATH            file status (type/size/mode/uid/gid)\n"
            "  mkdir PATH           create a directory\n"
            "  rm FILE              remove a file\n"
            "  run PATH [ARG...]    run an external program (fork+execve+wait4)\n"
            "  exit [CODE]          leave the shell\n"
            "redirection: > >> < (built-ins)    pipeline: a | b (built-ins)\n"
            "external programs get the kernel console for stdout (dup2 to fd 0/1 = -ENOSYS)\n");
    return 0;
}

/* 外部程序：fork + execve + wait4（本内核的真进程路径；未实现的号会被内核如实拒绝）
 * ★ 先 stat 一次再 fork：本内核的 **execve 失败路径有个已知缺陷**（proc64.cpp：装载失败时旧映像
 *   已经被释放、进程却还在 ring3 取指 -> #PF -> PANIC，见报告"A4-1 发现的既有缺陷"）。shell 不碰
 *   内核代码，就用**用户态预检**把它绕开：目标不存在/不是普通文件 -> 只打一行错误，不 fork。 */
static int run_external(int argc, char** argv) {
    char path[SH_PATH_MAX];
    if (path_resolve(argv[0], path, (int)sizeof(path)) != 0) {
        out_str("run: bad path: "); out_str(argv[0]); out_str("\n");
        return 2;
    }
    struct { long v[18]; } st;
    for (int i = 0; i < 18; i++) st.v[i] = 0;
    if (sh_stat(path, &st) != 0) {
        out_str("run: no such program: "); out_str(path); out_str("\n");
        return 1;
    }
    if (((unsigned long)st.v[3] & 0170000ul) == 0040000ul) {      /* 目录 */
        out_str("run: is a directory: "); out_str(path); out_str("\n");
        return 1;
    }
    const int pid = sh_fork();
    if (pid < 0) {
        out_str("run: fork failed (err="); out_dec(-pid);
        out_str(") - shared address space mode? see the serial log\n");
        return 2;
    }
    if (pid == 0) {
        char* av[SH_ARGV_MAX + 1];
        int n = 0;
        av[n++] = path;
        for (int i = 1; i < argc && n < SH_ARGV_MAX; i++) av[n++] = argv[i];
        av[n] = 0;
        /* ★ A4-2b：`run PATH > FILE` 的重定向落地在**子进程**里 —— dup2(fd, 1) 之后再 execve。
         *   为什么现在才做：批次 D 才把内核的 dup2 修成"目标 0/1/2 也走 FD 层"（
         *   kernel/syscall64.cpp:567 的注释："槽被 dup2 绑了对象 -> FD 层（文件/pipe/tty），
         *   这正是\"外部命令 > 文件\"的落地方式"）；在那之前，目标 1 是 -ENOSYS，shell 只能对
         *   内置命令做「把输出写进文件 fd」的替代实现（见 out_bytes）。子进程 fork 出来的 fd
         *   表是**复制**的，execve 也保留（proc64.cpp 的 fds_kept），所以这里绑好 fd 1 就行。 */
        if (g_out_fd >= 0) {
            const int r = sh_dup(g_out_fd, 1);
            if (r < 0) {
                const int saved = g_out_fd;
                g_out_fd = -1;
                out_str("run: dup2 to fd 1 failed (err="); out_dec(-r); out_str(")\n");
                g_out_fd = saved;
            }
        }
        (void)sh_execve(path, av, 0);
        sh_exit_group(127);                          /* 装载失败：本内核按 127 终止（如实） */
    }
    int st2 = 0;
    /* ★ A4-2b：父进程的"run: … exited code=N"这一行要回**终端**而不是重定向文件
     *   （真 shell 也是这个语义：重定向只作用在那个命令的 stdout 上）。 */
    const int saved_out_fd = g_out_fd;
    g_out_fd = -1;
    const int r = sh_wait4(pid, &st2, 0);
    if (r < 0) {
        out_str("run: wait4 failed (err="); out_dec(-r); out_str(")\n");
        g_out_fd = saved_out_fd;
        return 2;
    }
    g_out_fd = saved_out_fd;
    const int code = (st2 >> 8) & 0xFF;
    out_str("run: "); out_str(path);
    out_str(" pid="); out_dec(r);
    out_str(" exited code="); out_dec(code); out_ch('\n');
    return code;
}

/* run 内置命令：argv[1] 才是程序路径（argv[0] = "run"）——不带这个包装的话，
 * `run /musl_hello.elf` 会被当成"执行一个叫 run 的程序"（本轮实测踩到过）。 */
static int bi_run(int argc, char** argv) {
    if (argc < 2) { out_str("run: usage: run <path> [ARG...]\n"); return 2; }
    return run_external(argc - 1, argv + 1);
}

static int is_builtin(const char* name) {
    static const char* N[] = { "help", "echo", "pwd", "cd", "ls", "cat", "stat", "mkdir", "rm", "run" };
    for (unsigned i = 0; i < sizeof N / sizeof N[0]; i++) {
        const char* a = N[i];
        int k = 0;
        while (a[k] && name[k] && a[k] == name[k]) k++;
        if (a[k] == 0 && name[k] == 0) return (int)i;
    }
    return -1;
}

static int call_builtin(int which, int argc, char** argv) {
    switch (which) {
        case 0: return bi_help(argc, argv);
        case 1: return bi_echo(argc, argv);
        case 2: return bi_pwd(argc, argv);
        case 3: return bi_cd(argc, argv);
        case 4: return bi_ls(argc, argv);
        case 5: return bi_cat(argc, argv);
        case 6: return bi_stat(argc, argv);
        case 7: return bi_mkdir(argc, argv);
        case 8: return bi_rm(argc, argv);
        case 9: return bi_run(argc, argv);
        default: return 2;
    }
}

/* 切一个以空白分隔的 token（原地改 s）；返回 token 起始指针，*pp 指向下一个位置 */
static char* tok(char** pp) {
    char* s = *pp;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) { *pp = s; return 0; }
    char* b = s;
    while (*s && *s != ' ' && *s != '\t') s++;
    if (*s) *s++ = 0;
    *pp = s;
    return b;
}

/* 字符串相等 / 十进制解析（只给 exit 的退出码用；非法一律当 0） */
static int s_eq(const char* a, const char* b) {
    int k = 0;
    while (a[k] && b[k] && a[k] == b[k]) k++;
    return (a[k] == 0 && b[k] == 0) ? 1 : 0;
}

static int dec_of(const char* s) {
    int v = 0;
    for (int i = 0; s[i] >= '0' && s[i] <= '9'; i++) v = v * 10 + (s[i] - '0');
    return v;
}

/* ==================== 一段命令：解析重定向 + 派发 ==================== */
static int run_stage(char* stage, const char* in_buf, int in_len, int last) {
    char* argv[SH_ARGV_MAX + 1];
    int argc = 0;
    char* out_file = 0;
    char* in_file = 0;
    int   append = 0;

    char* p = stage;
    for (;;) {
        char* t = tok(&p);
        if (!t) break;
        if (t[0] == '>' && !out_file) {
            append = (t[1] == '>');
            char* f = (t[1] == '>') ? (t + 2) : (t + 1);
            if (!*f) f = tok(&p);
            if (!f) { out_str("sh: syntax error: '>' without a file name\n"); return 2; }
            out_file = f;
            continue;
        }
        if (t[0] == '<' && !in_file) {
            char* f = t + 1;
            if (!*f) f = tok(&p);
            if (!f) { out_str("sh: syntax error: '<' without a file name\n"); return 2; }
            in_file = f;
            continue;
        }
        if (argc < SH_ARGV_MAX) argv[argc++] = t;
    }
    argv[argc] = 0;
    if (argc == 0) {
        if (!in_file && !out_file) return 0;                       /* 空段（如 "a | | b"）*/
        out_str("sh: syntax error: no command name\n");
        return 2;
    }

    /* 输入汇：`<` 优先，其次上一段的捕获 */
    g_in = 0; g_in_len = 0;
    if (in_file) {
        char path[SH_PATH_MAX];
        if (path_resolve(in_file, path, (int)sizeof(path)) != 0) { out_str("sh: bad path\n"); return 2; }
        const int fd = sh_open(path, SH_O_RDONLY);
        if (fd < 0) { out_str("sh: cannot open "); out_str(path); out_str("\n"); return 1; }
        g_in = g_in_slot; g_in_len = 0;
        for (;;) {
            const long n = sh_read(fd, g_in_slot + g_in_len, (unsigned long)(SH_CAP_MAX - g_in_len));
            if (n <= 0) break;
            g_in_len += (int)n;
            if (g_in_len >= SH_CAP_MAX) break;
        }
        (void)sh_close(fd);
    } else if (in_len > 0 && in_buf) {
        g_in = in_buf; g_in_len = in_len;
    }

    /* exit 先处理（不走输出汇/文件重定向：它只结束本进程） */
    if (s_eq(argv[0], "exit")) {
        sh_exit_group((argc > 1) ? dec_of(argv[1]) : 0);
    }

    const int which = is_builtin(argv[0]);

    /* 输出汇 */
    g_cap = 0; g_cap_len = 0; g_cap_ovf = 0; g_out_fd = -1;
    if (last && out_file) {
        /* ★ A4-2b：`>` / `>>` 现在对**外部程序**也生效 —— 内置命令走 out_bytes 的"直接写文件
         *   fd"，外部程序走子进程里的 dup2(fd, 1) + execve（见 run_external）。两条路都不依赖
         *   内核的"标准流可替换"（批次 D 之后 dup2 到 0/1/2 已经走 FD 层，见上面那段注释）。 */
        char path[SH_PATH_MAX];
        if (path_resolve(out_file, path, (int)sizeof(path)) != 0) { out_str("sh: bad path\n"); return 2; }
        const int fd = sh_open(path, SH_O_WRONLY | SH_O_CREAT | (append ? SH_O_APPEND : SH_O_TRUNC));
        if (fd < 0) { out_str("sh: cannot create "); out_str(path); out_str("\n"); return 1; }
        g_out_fd = fd;
    } else if (!last) {
        g_cap = g_cur_slot;                          /* 捕获给下一段当输入 */
    }

    int rc;
    if (which >= 0) {
        rc = call_builtin(which, argc, argv);
    } else if (!last) {
        out_str("sh: external programs cannot be a pipeline stage (no process-to-process pipe)\n");
        rc = 2;
    } else if (g_in) {
        out_str("sh: external programs cannot take '<' (kernel dup2 to fd 0 = -ENOSYS)\n");
        rc = 2;
    } else {
        rc = run_external(argc, argv);
    }

    if (g_out_fd >= 0) { (void)sh_close(g_out_fd); g_out_fd = -1; }
    g_cap = 0;
    if (g_cap_ovf) out_str("sh: pipeline buffer full (4 KiB) - output truncated\n");
    return rc;
}

/* ==================== 一行：按 '|' 切段，顺序执行（前段输出 = 后段输入）==================== */
static int exec_line(char* line) {
    char* stages[SH_STAGES_MAX];
    int nstage = 0;
    stages[nstage++] = line;
    for (char* p = line; *p; p++) {
        if (*p == '|') {
            *p = 0;
            if (nstage >= SH_STAGES_MAX) {
                out_str("sh: too many pipeline stages (max 4)\n");
                return 2;
            }
            stages[nstage++] = p + 1;
        }
    }
    int rc = 0;
    const char* prev = 0;
    int prev_len = 0;
    for (int i = 0; i < nstage; i++) {
        const int last = (i == nstage - 1);
        g_cur_slot = (i & 1) ? g_cap_b : g_cap_a;    /* 与输入缓冲交替，绝不重叠 */
        rc = run_stage(stages[i], prev, prev_len, last);
        if (!last) {
            prev = g_cur_slot;
            prev_len = g_cap_len;                    /* run_stage 刚写完的捕获长度 */
        }
    }
    return rc;
}

/* ==================== 行编辑 + 主循环 ==================== */
static int read_line(char* buf, int max) {
    int n = 0;
    for (;;) {
        const int c = pend_get();
        if (c < 0) continue;
        if (c == '\n' || c == '\r') { out_ch('\n'); break; }
        if (c == 0x08 || c == 0x7F) {                /* 退格：本地回显 "\b \b" */
            if (n > 0) { n--; out_str("\b \b"); }
            continue;
        }
        if (c >= 0x20 && c < 0x7F) {
            if (n < max - 1) {
                buf[n++] = (char)c;
                out_ch((char)c);                     /* 回显（经邮箱 -> 终端窗口）*/
            }
            continue;
        }
        /* 其它控制字节（含内核的 0x01/0x02 之类）：丢掉 */
    }
    buf[n] = 0;
    return n;
}

static void prompt(void) {
    out_str("sh64:");
    out_str(g_cwd);
    out_str("$ ");
}

int main(int argc, char** argv) {
    (void)argc; (void)argv;
    if (!mb_probe()) {
        static const char MSG[] =
            "[sh64] no terminal mailbox at 0x100060000: this shell must be started by the\n"
            "       terminal command 'shell' (kernel terminal service), not by elfrun.\n";
        (void)sh_write(1, MSG, sizeof MSG - 1u);
        return 2;
    }
    mb_init();
    out_str("VimtuOS ring3 shell (sh64) - /bin/shell.bin, built-ins only, no fork for these\n");
    out_str("type 'help' for the command list\n");
    for (;;) {
        prompt();
        char line[SH_LINE_MAX];
        const int n = read_line(line, (int)sizeof(line));
        if (n > 0) (void)exec_line(line);
    }
}
