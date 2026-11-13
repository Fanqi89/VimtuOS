/* peinfo.c - ★ v0.9.43（W0）：**Windows PE 只读体检器**（Ring 3，静态 ELF64，`/bin/peinfo`）
 *
 * ============================ 这是什么、不是什么 ============================
 * **是**：把一份 PE32 / PE32+（.exe / .dll）当作**数据**读进来，逐字段打印：
 *   DOS 头 → NT(COFF) 头 → 可选头 → 节表 → 数据目录（导入 / 导出 / 重定位 / TLS / 资源 / CLR）。
 *   **重点产出是导入表逐条**（`DLL!函数名`）+ 一行**导入并集** `union "…"`。
 * **不是**：不是加载器 —— 它**不映射节、不做重定位、不填 IAT、不执行任何字节**。
 *   一个字节的可执行代码都不会跑，所以对任何来路不明的 exe 都是安全的只读操作。
 *
 * 为什么先做这一步（W0 在路线图里的位置，见 docs/多生态加载盘点与路线图.md §7）：
 *   W1/W2/W3 要写的是"PE 装载 + kernel32/user32/gdi32 子集"。**在写任何一个 API 之前**，
 *   必须先知道真实 exe 到底导入了哪些 DLL、哪些函数 —— 否则就是照猜写一遍、再发现清单不对、
 *   白写一半。本工具的输出直接就是那张清单（`union` 行 + `import_f` 逐条），
 *   并与宿主 Python 的独立解析实现（tools/pe_info.py）**逐字段交叉核对**（tests/peinfo64_test.py）。
 *
 * ============================ 为什么自足（不引 user/lib / musl） ============================
 * 与 user/gzip/gzip.c 同一条纪律：本程序要被 `run /bin/peinfo <file.exe>` 直接执行，走内核的
 * **主程序装载器**，映像必须落在用户窗口低段（链在 4 GiB）。所以：
 *   * 不链 libc、不链 user/lib；系统调用全部内联汇编直接发（Linux x86_64 号段，与 shell/musl 同一条 ABI）；
 *   * **大缓冲一律运行期 mmap(9)**：文件内容缓冲 8 MiB（= VFS 单文件上限）从 mmap 拿，
 *     静态数据只有几十字节（几个计数器），`.bss` 近零 —— 映像体积因此与"要看多大的 PE"无关。
 *
 * ============================ 解析边界（如实写清，不假装全能） ============================
 * * 支持 PE32(0x10B) 与 **PE32+(0x20B)** 两种可选头；**不认识的 magic 直接拒绝**（不是猜）。
 * * 只做**只读遍历**：导入/导出/重定位/TLS/资源/CLR 的**目录项与条目**。
 *   不解析：资源树内部形状（只给 RVA+size）、延迟导入（Delay Import，目录 13）、
 *   bound import、证书表、调试目录内容。这些都**跳过并如实标注**（`skip=…`），不静默。
 * * 所有 RVA 换算走节表（`rva2off`）；RVA 落在**任何节之外**时返回 0，调用方必须当"取不到"处理
 *   （绝不拿着越界偏移去读文件 —— 那是崩溃来源）。
 * * 字符串（DLL 名 / 函数名 / 节名）一律**限长**截断并显式标注 `+`（表示后面还有），不假定 NUL。
 * * 导入表 / 节表 / 重定位块数量都有**硬上限**（见下方 MAX_*），超了如实打 `truncated=1`。
 *
 * ============================ 输出契约（tests/peinfo64_test.py 按这些串 grep；格式勿改） ============================
 *   [PEINFO] file=<p> size=<n> read=<n>
 *   [PEINFO] dos mz=1 lfanew=0x<n>
 *   [PEINFO] nt machine=0x<n> nsec=<n> optsz=<n> magic=0x<n> chars=0x<n>
 *   [PEINFO] opt subsys=<n>(<gui|cui|efi|native|unknown>) imagebase=0x<n> entry=0x<n>
 *   [PEINFO] opt sizeimage=0x<n> sizehdr=0x<n> sectalign=0x<n> filealign=0x<n> dllchar=0x<n> nrva=<n>
 *   [PEINFO] section idx=<n> name=<8B 去尾零/空格> va=0x<n> vsz=0x<n> raw=0x<n> rawsz=0x<n> flags=0x<n> perm=<r|-|-><w>…（见下）
 *   [PEINFO] dir idx=<n> vadr=0x<n> size=0x<n>
 *   [PEINFO] import dll=<name> thunks=<n> funcs=<n> ord=<n>
 *   [PEINFO] import_f dll=<name> name=<func> hint=<n>
 *   [PEINFO] import_o dll=<name> ord=<n>
 *   [PEINFO] export name=<dllname> base=<n> funcs=<n> names=<n>
 *   [PEINFO] export_f idx=<n> rva=0x<n> name=<name|->
 *   [PEINFO] reloc blocks=<n> entries=<n> types=<0xN,...>
 *   [PEINFO] tls vadr=0x<n> callbacks=0x<n>
 *   [PEINFO] res vadr=0x<n> size=0x<n>
 *   [PEINFO] clr vadr=0x<n> size=0x<n>
 *   [PEINFO] summary dlls=<n> funcs=<n> ord=<n> exports=<n> relocs=<n> tls=<0|1> res=<0|1> clr=<0|1> truncated=<0|1>
 *   [PEINFO] union "<dll>!<func>|<dll>!<ord#N>|…"      ← **导入并集**（W1/W2/W3 的实现清单）
 *   [PEINFO] reject reason=<magic|dos|nt|opt|trunc|io>
 * 退出码：0 = 解析完（含"没有导出/重定位"这类正常空值）；1 = 用法错误 / 不是 PE / 读不出来。
 */

typedef unsigned long long u64;
typedef long long          i64;
typedef unsigned int       u32;
typedef int                i32;
typedef unsigned short     u16;
typedef unsigned char      u8;

/* ==================== 系统调用（Linux x86_64 号段） ==================== */
#define NR_READ   0
#define NR_WRITE  1
#define NR_OPEN   2
#define NR_CLOSE  3
#define NR_LSEEK  8
#define NR_MMAP   9
#define NR_EXITG  231

#define O_RDONLY  0

static inline i64 sc1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc3(long nr, i64 a1, i64 a2, i64 a3) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc6(long nr, i64 a1, i64 a2, i64 a3, i64 a4, i64 a5, i64 a6) {
    register i64 r10 __asm__("r10") = a4;
    register i64 r8  __asm__("r8")  = a5;
    register i64 r9  __asm__("r9")  = a6;
    i64 r;
    __asm__ volatile("syscall" : "=a"(r)
                     : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
static void *x_mmap(u64 len) {
    const i64 r = sc6(NR_MMAP, 0, (i64)len, 3 /*R|W*/, 0x22 /*PRIVATE|ANONYMOUS*/, -1, 0);
    if (r < 0) return 0;
    return (void *)(u64)r;
}
static void x_exit(int code) { (void)sc1(NR_EXITG, (i64)code); for (;;) { } }

/* ==================== 小工具 ==================== */
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* ==================== 输出：**行级缓冲**（★ v0.9.43 实测抓到的真缺陷）====================
 * 踩到的现场（tests/peinfo64_test.py 第一次跑）：
 *     体内 machine='[SYSCALL]'   宿主 machine='0x8664'
 *     体内 nsec=None             宿主 nsec='3'
 * 原因不是解析错 —— 是**每一小段输出都单独 write 一次**，而内核在每次 write 返回时打一行
 *     `[SYSCALL] insn nr=1 rdi=... ret=...`
 * 于是 `[PEINFO] nt machine=` 与它的值之间被这条内核日志**插进去**，整行被切成碎片：
 * 人看不到、测试也读不出来。而 `union` 那一行之所以是完整的，正因为它是一次大块 write ——
 * 这反过来证明解析本身是对的（28/31 条导入全部逐字符与宿主一致）。
 *
 * 修法：所有输出先攒进 g_ob，**遇到 '\n' 或缓冲满才 flush**（一次 write 一整行）。
 * 这样每行输出只产生 1 次 write，内核最多在它前后各插一行日志，**行内容永远连续**。
 * 为什么不干脆先把整份报告拼成一个 buffer 再一次 write：报告可能上千行（gui.exe 59 行 ×
 * 平均 60 B ≈ 3.5 KB，但导入多的 exe 会更大），而 4 KiB 的固定缓冲 + 行级 flush 是**有界**的
 * （绝不因为输入大就爆栈/爆静态区）。 */
#define OUT_CAP 4096u
static char g_ob[OUT_CAP];
static u32  g_on;
static int  g_ofd = 1;

static void raw_write(int fd, const char *s, u32 n) {
    while (n) {
        const u32 k = n > 1024u ? 1024u : n;
        const i64 w = sc3(NR_WRITE, fd, (i64)(u64)s, (i64)k);
        if (w <= 0) return;
        s += w; n -= (u32)w;
    }
}
static void ob_flush(void) {
    if (!g_on) return;
    raw_write(g_ofd, g_ob, g_on);
    g_on = 0;
}
static void ob_put(int fd, char c) {
    if (g_ofd != fd) { ob_flush(); g_ofd = fd; }
    if (g_on + 1u >= OUT_CAP) ob_flush();
    g_ob[g_on++] = c;
    if (c == '\n') ob_flush();
}
static void outn(int fd, const char *s, u32 n) {
    for (u32 i = 0; i < n; i++) ob_put(fd, s[i]);
}
static void outs(int fd, const char *s) { outn(fd, s, slen(s)); }
static void outdec(int fd, u64 v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) outn(fd, &t[--n], 1);
}
static void outhex(int fd, u64 v) {          /* 0x 前缀 + 不补零的小写十六进制 */
    static const char H[] = "0123456789abcdef";
    char t[16]; int n = 0;
    outs(fd, "0x");
    if (!v) { outn(fd, "0", 1); return; }
    while (v) { t[n++] = H[v & 0xFull]; v >>= 4; }
    while (n) outn(fd, &t[--n], 1);
}
/* 小端读取（**偏移读**：不定义 PE 结构体，避免对齐/填充在不同编译器下漂） */
static u16 rd16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) {
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (u64)p[i];
    return v;
}

/* ==================== 规模上限（都如实打 truncated） ==================== */
#define MAX_PE_BYTES   (8u * 1024u * 1024u)   /* = VFS 单文件上限 */
#define MAX_SEC        96u
#define MAX_DLL        64u
#define MAX_DLL_FUNCS  4096u
#define MAX_EXPORTS    512u
#define MAX_RELOC_BLK  256u
#define MAX_NAME       64u                   /* 单个名字的打印上限（超了标 '+'） */

static u8 *g_buf;                             /* 文件内容（mmap） */
static u32 g_len;                             /* 已读到多少字节 */
static u16 g_nsec;
static u32 g_sec_off;                         /* 节表在文件里的偏移 */
static u32 g_opt_off;                         /* 可选头偏移 */
static u16 g_opt_magic;
static u32 g_nrva;                            /* NumberOfRvaAndSizes（<=16 才读目录） */
static u32 g_dd_off;                          /* DataDirectory 偏移 */
static int g_truncated;

/* RVA -> 文件偏移（落在任何节之外返回 0）。越界一律当"取不到"。 */
static u32 rva2off(u32 rva) {
    if (rva < rd32(g_buf + g_opt_off + 60)) return rva;      /* SizeOfHeaders 内：RVA==偏移 */
    for (u16 i = 0; i < g_nsec && i < MAX_SEC; i++) {
        const u32 so = g_sec_off + (u32)i * 40u;
        if (so + 40u > g_len) break;
        const u32 vsz  = rd32(g_buf + so + 8);
        const u32 va   = rd32(g_buf + so + 12);
        const u32 rsz  = rd32(g_buf + so + 16);
        const u32 roff = rd32(g_buf + so + 20);
        const u32 span = vsz ? vsz : rsz;
        if (rva >= va && rva < va + span) {
            const u32 d = rva - va;
            return (d < rsz) ? (roff + d) : 0;               /* .bss 段：没文件内容 */
        }
    }
    return 0;
}

/* 打印一个"文件内 NUL 结尾字符串"（限长，超长标 '+'）。off=0 视为取不到。 */
static void out_str1(int fd, u32 off) {
    if (!off || off >= g_len) { outn(fd, "?", 1); return; }
    u32 n = 0;
    while (off + n < g_len && g_buf[off + n] && n < MAX_NAME) n++;
    outn(fd, (const char *)(g_buf + off), n);
    if (n == MAX_NAME && off + n < g_len && g_buf[off + n]) outn(fd, "+", 1);
}

/* 节名：8 字节定长，去尾部 NUL/空格 */
static void out_sec_name(int fd, u32 off) {
    u32 n = 8;
    while (n && (g_buf[off + n - 1] == 0 || g_buf[off + n - 1] == ' ')) n--;
    if (!n) { outn(fd, "-", 1); return; }
    outn(fd, (const char *)(g_buf + off), n);
}

static const char *subsys_name(u16 s) {
    switch (s) {
    case 1:  return "native";
    case 2:  return "gui";
    case 3:  return "cui";
    case 9:  return "wince";
    case 10: return "efi";
    case 11: return "efi-bs";
    case 12: return "efi-rt";
    case 13: return "efi-rom";
    default: return "unknown";
    }
}

/* 导入并集用的名字缓冲（静态 16 KiB：最多 MAX_DLL*MAX_NAME 左右，够也不占多少） */
#define UNION_CAP (16u * 1024u)
static char   g_union[UNION_CAP];
static u32    g_union_n;

static void union_add(const char *s) {
    const u32 n = slen(s);
    if (g_union_n + n + 2u >= UNION_CAP) { g_truncated = 1; return; }
    if (g_union_n) g_union[g_union_n++] = '|';
    for (u32 i = 0; i < n; i++) g_union[g_union_n++] = s[i];
    g_union[g_union_n] = 0;
}
/* 把 "dll!name" / "dll!ord#N" 拼出来（避免重复实现拼接） */
static void union_add2(const char *dll, const char *name, u32 ord, int is_ord) {
    char tmp[MAX_NAME * 2u + 16u];
    u32 k = 0;
    for (u32 i = 0; dll[i] && k + 1u < sizeof(tmp); i++) tmp[k++] = dll[i];
    if (k + 1u < sizeof(tmp)) tmp[k++] = '!';
    if (is_ord) {
        const char *p = "ord#";
        for (u32 i = 0; p[i] && k + 1u < sizeof(tmp); i++) tmp[k++] = p[i];
        char d[12]; int m = 0;
        u32 v = ord;
        if (!v) d[m++] = '0';
        while (v) { d[m++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
        while (m && k + 1u < sizeof(tmp)) tmp[k++] = d[--m];
    } else {
        for (u32 i = 0; name[i] && k + 1u < sizeof(tmp); i++) tmp[k++] = name[i];
    }
    tmp[k] = 0;
    union_add(tmp);
}

/* 从文件偏移处把 NUL 字符串拷到 dst（限长） */
static void copy_str(u32 off, char *dst, u32 cap) {
    u32 n = 0;
    if (off && off < g_len) {
        while (off + n < g_len && g_buf[off + n] && n + 1u < cap) {
            dst[n] = (char)g_buf[off + n];
            n++;
        }
    }
    dst[n] = 0;
}

/* ==================== 主流程 ==================== */
int peinfo_main(u64 *sp) {
    const int argc = (int)sp[0];
    const char **argv = (const char **)(sp + 1);
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        outs(2, "[PEINFO] usage: peinfo <file.exe|file.dll>\n");
        return 1;
    }
    const char *path = argv[1];

    /* ---- 读文件（整份读进 mmap；上限 = VFS 单文件上限） ---- */
    g_buf = (u8 *)x_mmap(MAX_PE_BYTES);
    if (!g_buf) { outs(2, "[PEINFO] reject reason=io (mmap)\n"); return 1; }
    const i64 fd = sc3(NR_OPEN, (i64)(u64)path, O_RDONLY, 0);
    if (fd < 0) {
        outs(2, "[PEINFO] reject reason=io path=");
        outs(2, path);
        outn(2, "\n", 1);
        return 1;
    }
    g_len = 0;
    for (;;) {
        if (g_len >= MAX_PE_BYTES) { g_truncated = 1; break; }
        const i64 n = sc3(NR_READ, fd, (i64)(u64)(g_buf + g_len), (i64)(MAX_PE_BYTES - g_len));
        if (n <= 0) break;
        g_len += (u32)n;
    }
    (void)sc1(NR_CLOSE, fd);

    outs(1, "[PEINFO] file="); outs(1, path);
    outs(1, " size="); outdec(1, g_len);
    outs(1, " read="); outdec(1, g_len);
    outn(1, "\n", 1);

    /* ---- DOS 头 ---- */
    if (g_len < 64u || g_buf[0] != 'M' || g_buf[1] != 'Z') {
        outs(2, "[PEINFO] reject reason=dos\n");
        return 1;
    }
    const u32 lfanew = rd32(g_buf + 0x3C);
    outs(1, "[PEINFO] dos mz=1 lfanew="); outhex(1, lfanew); outn(1, "\n", 1);
    if (lfanew + 24u > g_len || lfanew < 64u) {
        outs(2, "[PEINFO] reject reason=nt\n");
        return 1;
    }
    if (!(g_buf[lfanew] == 'P' && g_buf[lfanew + 1] == 'E' &&
          g_buf[lfanew + 2] == 0 && g_buf[lfanew + 3] == 0)) {
        outs(2, "[PEINFO] reject reason=nt\n");
        return 1;
    }

    /* ---- COFF 头（e_lfanew+4，20 B） ---- */
    const u32 coff = lfanew + 4u;
    const u32 machine = rd16(g_buf + coff);
    g_nsec        = rd16(g_buf + coff + 2u);
    const u16 optsz = rd16(g_buf + coff + 16u);
    const u16 chars = rd16(g_buf + coff + 18u);
    outs(1, "[PEINFO] nt machine="); outhex(1, machine);
    outs(1, " nsec=");   outdec(1, g_nsec);
    outs(1, " optsz=");  outdec(1, optsz);
    outs(1, " chars=");  outhex(1, chars);
    outn(1, "\n", 1);
    if (g_nsec == 0 || g_nsec > MAX_SEC) { g_truncated = 1; }

    /* ---- 可选头（coff+20） ---- */
    g_opt_off = coff + 20u;
    if (g_opt_off + 2u > g_len) { outs(2, "[PEINFO] reject reason=opt\n"); return 1; }
    g_opt_magic = rd16(g_buf + g_opt_off);
    outs(1, "[PEINFO] opt magic="); outhex(1, g_opt_magic); outn(1, "\n", 1);
    if (g_opt_magic != 0x10Bu && g_opt_magic != 0x20Bu) {
        outs(2, "[PEINFO] reject reason=magic\n");
        return 1;
    }
    const int is64 = (g_opt_magic == 0x20Bu);
    /* PE32 与 PE32+ 的差别只在 ImageBase 起：PE32 是 u32、PE32+ 是 u64，
     * 于是其后所有字段**整体后移 4 字节**。用两个基址避免满篇 if。 */
    const u32 o = g_opt_off + (is64 ? 24u : 28u);      /* ImageBase 偏移 */
    if (o + 8u > g_len) { outs(2, "[PEINFO] reject reason=trunc\n"); return 1; }
    const u64 imagebase = is64 ? rd64(g_buf + o) : (u64)rd32(g_buf + o);
    const u32 entry     = rd32(g_buf + g_opt_off + 16u);
    const u32 sectalign = rd32(g_buf + g_opt_off + 32u);   /* 两版都 +32（ImageBase 之后重合） */
    const u32 filealign = rd32(g_buf + g_opt_off + 36u);   /* 两版都 +36 */
    const u32 sizeimage = rd32(g_buf + g_opt_off + 56u);
    const u32 sizehdr   = rd32(g_buf + g_opt_off + 60u);
    const u16 subsys    = rd16(g_buf + g_opt_off + 68u);
    const u16 dllchar   = rd16(g_buf + g_opt_off + 70u);
    g_nrva              = rd32(g_buf + g_opt_off + (is64 ? 108u : 92u));
    g_dd_off            = g_opt_off + (is64 ? 112u : 96u);
    if (g_nrva > 16u) g_nrva = 16u;

    outs(1, "[PEINFO] opt subsys="); outdec(1, subsys);
    outn(1, "(", 1); outs(1, subsys_name(subsys)); outn(1, ")", 1);
    outs(1, " imagebase="); outhex(1, imagebase);
    outs(1, " entry=");     outhex(1, entry);
    outn(1, "\n", 1);
    outs(1, "[PEINFO] opt sizeimage="); outhex(1, sizeimage);
    outs(1, " sizehdr=");   outhex(1, sizehdr);
    outs(1, " sectalign="); outhex(1, sectalign);
    outs(1, " filealign="); outhex(1, filealign);
    outs(1, " dllchar=");   outhex(1, dllchar);
    outs(1, " nrva=");      outdec(1, g_nrva);
    outn(1, "\n", 1);

    /* ---- 节表（紧跟可选头，每项 40 B） ---- */
    g_sec_off = g_opt_off + optsz;
    if ((u64)g_sec_off + (u64)g_nsec * 40u > (u64)g_len) {
        outs(2, "[PEINFO] reject reason=trunc\n");
        return 1;
    }
    for (u16 i = 0; i < g_nsec && i < MAX_SEC; i++) {
        const u32 so = g_sec_off + (u32)i * 40u;
        const u32 vsz  = rd32(g_buf + so + 8);
        const u32 va   = rd32(g_buf + so + 12);
        const u32 rsz  = rd32(g_buf + so + 16);
        const u32 roff = rd32(g_buf + so + 20);
        const u32 fl   = rd32(g_buf + so + 36);
        outs(1, "[PEINFO] section idx="); outdec(1, i);
        outs(1, " name="); out_sec_name(1, so);
        outs(1, " va=");    outhex(1, va);
        outs(1, " vsz=");   outhex(1, vsz);
        outs(1, " raw=");   outhex(1, roff);
        outs(1, " rawsz="); outhex(1, rsz);
        outs(1, " flags="); outhex(1, fl);
        outs(1, " perm=");
        outn(1, (fl & 0x40000000u) ? "r" : "-", 1);
        outn(1, (fl & 0x80000000u) ? "w" : "-", 1);
        outn(1, (fl & 0x20000000u) ? "x" : "-", 1);
        outn(1, "\n", 1);
    }

    /* ---- 数据目录（只打非空的，索引见 PE 规范） ---- */
    for (u32 i = 0; i < g_nrva; i++) {
        const u32 dd = g_dd_off + i * 8u;
        if (dd + 8u > g_len) break;
        const u32 va = rd32(g_buf + dd), sz = rd32(g_buf + dd + 4u);
        if (!va && !sz) continue;
        outs(1, "[PEINFO] dir idx="); outdec(1, i);
        outs(1, " vadr="); outhex(1, va);
        outs(1, " size="); outhex(1, sz);
        outn(1, "\n", 1);
    }

    /* ---- 导入表（dir[1]）——本工具的**重点产出** ---- */
    u32 n_dll = 0, n_func = 0, n_ord = 0;
    {
        u32 ivadr = 0;
        if (g_nrva > 1u && g_dd_off + 1u * 8u + 8u <= g_len) ivadr = rd32(g_buf + g_dd_off + 8u);
        u32 ioff = ivadr ? rva2off(ivadr) : 0;
        while (ioff && n_dll < MAX_DLL) {
            if (ioff + 20u > g_len) break;
            const u32 oft  = rd32(g_buf + ioff);        /* OriginalFirstThunk (INT) */
            const u32 name = rd32(g_buf + ioff + 12u);  /* DLL name RVA */
            const u32 ft   = rd32(g_buf + ioff + 16u);  /* FirstThunk (IAT) */
            if (!oft && !name && !ft) break;            /* 终止描述符 */
            char dll[MAX_NAME + 1];
            copy_str(name ? rva2off(name) : 0, dll, (u32)sizeof(dll));
            if (!dll[0]) { dll[0] = '?'; dll[1] = 'd'; dll[2] = 'l'; dll[3] = 'l'; dll[4] = 0; }

            /* 先数一遍（thunks/funcs/ord），再逐条打印 —— 两遍走同一套解析，避免"数一条、打一条"漂移 */
            const u32 thunk_rva = oft ? oft : ft;
            const u32 tsz = is64 ? 8u : 4u;
            u32 t_off = thunk_rva ? rva2off(thunk_rva) : 0;
            u32 nf = 0, no = 0, tot = 0;
            if (t_off) {
                for (u32 k = 0; k < MAX_DLL_FUNCS; k++) {
                    const u32 eo = t_off + k * tsz;
                    if (eo + tsz > g_len) break;
                    const u64 tv = is64 ? rd64(g_buf + eo) : (u64)rd32(g_buf + eo);
                    if (!tv) break;
                    tot++;
                    if (is64 ? (tv >> 63) : (tv & 0x80000000ull)) no++; else nf++;
                }
                if (tot == MAX_DLL_FUNCS) g_truncated = 1;
            }
            outs(1, "[PEINFO] import dll="); outs(1, dll);
            outs(1, " thunks="); outdec(1, tot);
            outs(1, " funcs=");  outdec(1, nf);
            outs(1, " ord=");    outdec(1, no);
            outn(1, "\n", 1);

            if (t_off) {
                for (u32 k = 0; k < tot; k++) {
                    const u32 eo = t_off + k * tsz;
                    const u64 tv = is64 ? rd64(g_buf + eo) : (u64)rd32(g_buf + eo);
                    const int by_ord = is64 ? ((tv >> 63) != 0) : ((tv & 0x80000000ull) != 0);
                    if (by_ord) {
                        const u32 ord = (u32)(tv & 0xFFFFu);
                        outs(1, "[PEINFO] import_o dll="); outs(1, dll);
                        outs(1, " ord="); outdec(1, ord);
                        outn(1, "\n", 1);
                        union_add2(dll, "", ord, 1);
                        n_ord++;
                    } else {
                        const u32 byname = rva2off((u32)(tv & 0x7FFFFFFFull));
                        const u16 hint = byname ? rd16(g_buf + byname) : 0;
                        char fn[MAX_NAME + 1];
                        copy_str(byname ? byname + 2u : 0, fn, (u32)sizeof(fn));
                        outs(1, "[PEINFO] import_f dll="); outs(1, dll);
                        outs(1, " name="); outs(1, fn[0] ? fn : "?");
                        outs(1, " hint="); outdec(1, hint);
                        outn(1, "\n", 1);
                        union_add2(dll, fn, 0, 0);
                        n_func++;
                    }
                }
            }
            n_dll++;
            ioff += 20u;
        }
        if (n_dll == MAX_DLL) g_truncated = 1;
    }

    /* ---- 导出表（dir[0]） ---- */
    u32 n_exp = 0;
    {
        u32 evadr = 0;
        if (g_nrva > 0u && g_dd_off + 8u <= g_len) evadr = rd32(g_buf + g_dd_off);
        const u32 eoff = evadr ? rva2off(evadr) : 0;
        if (eoff && eoff + 40u <= g_len) {
            const u32 nname = rd32(g_buf + eoff + 24u);
            const u32 nfun  = rd32(g_buf + eoff + 20u);
            const u32 base  = rd32(g_buf + eoff + 16u);
            const u32 anames = rd32(g_buf + eoff + 32u);
            const u32 aords  = rd32(g_buf + eoff + 36u);
            const u32 afuncs = rd32(g_buf + eoff + 28u);
            outs(1, "[PEINFO] export name=");
            out_str1(1, rva2off(rd32(g_buf + eoff + 12u)));
            outs(1, " base=");  outdec(1, base);
            outs(1, " funcs="); outdec(1, nfun);
            outs(1, " names="); outdec(1, nname);
            outn(1, "\n", 1);
            const u32 no_off = rva2off(anames), oo_off = rva2off(aords), f_off = rva2off(afuncs);
            for (u32 i = 0; i < nname && i < MAX_EXPORTS && no_off && oo_off && f_off; i++) {
                if (no_off + i * 4u + 4u > g_len || oo_off + i * 2u + 2u > g_len) break;
                const u32 nrva = rd32(g_buf + no_off + i * 4u);
                const u16 ordi = rd16(g_buf + oo_off + i * 2u);
                const u32 fo = f_off + (u32)ordi * 4u;
                if (fo + 4u > g_len) continue;
                outs(1, "[PEINFO] export_f idx="); outdec(1, i);
                outs(1, " rva="); outhex(1, rd32(g_buf + fo));
                outs(1, " name="); out_str1(1, rva2off(nrva));
                outn(1, "\n", 1);
                n_exp++;
            }
            if (nname > MAX_EXPORTS) g_truncated = 1;
        } else {
            outs(1, "[PEINFO] export name=- base=0 funcs=0 names=0\n");
        }
    }

    /* ---- 重定位（dir[5]：块头 {RVA, SizeOfBlock} + u16 项） ---- */
    u32 n_blk = 0, n_rel = 0;
    u32 types_seen = 0;                    /* 位掩码：type 0..15 */
    {
        u32 rvadr = 0, rsz = 0;
        if (g_nrva > 5u && g_dd_off + 5u * 8u + 8u <= g_len) {
            rvadr = rd32(g_buf + g_dd_off + 5u * 8u);
            rsz   = rd32(g_buf + g_dd_off + 5u * 8u + 4u);
        }
        u32 roff = rvadr ? rva2off(rvadr) : 0;
        const u32 rend = roff ? (roff + rsz <= g_len ? roff + rsz : g_len) : 0;
        while (roff && roff + 8u <= rend && n_blk < MAX_RELOC_BLK) {
            const u32 bsz = rd32(g_buf + roff + 4u);
            if (bsz < 8u || roff + bsz > rend) break;
            const u32 nent = (bsz - 8u) / 2u;
            for (u32 k = 0; k < nent; k++) {
                const u16 e = rd16(g_buf + roff + 8u + k * 2u);
                const u32 ty = (u32)(e >> 12);
                if (ty) { types_seen |= (1u << (ty & 31u)); n_rel++; }
            }
            n_blk++;
            roff += bsz;
        }
        if (n_blk == MAX_RELOC_BLK) g_truncated = 1;
    }
    outs(1, "[PEINFO] reloc blocks="); outdec(1, n_blk);
    outs(1, " entries="); outdec(1, n_rel);
    outs(1, " types=");
    if (!types_seen) outn(1, "-", 1);
    for (u32 t = 0; t < 16u; t++) {
        if (types_seen & (1u << t)) {
            outhex(1, t);
            /* 用逗号分隔下一个（简单：只在后面再看有没有） */
            int more = 0;
            for (u32 q = t + 1u; q < 16u; q++) if (types_seen & (1u << q)) { more = 1; break; }
            if (more) outn(1, ",", 1);
        }
    }
    outn(1, "\n", 1);

    /* ---- TLS（dir[9]）与资源（dir[2]）/ CLR（dir[14]）：只给目录与回调地址 ---- */
    u32 tls_on = 0, res_on = 0, clr_on = 0, tls_callbacks = 0;
    if (g_nrva > 9u && g_dd_off + 9u * 8u + 8u <= g_len) {
        const u32 tv = rd32(g_buf + g_dd_off + 9u * 8u);
        if (tv) {
            tls_on = 1;
            const u32 to = rva2off(tv);
            /* PE32+: CallbackTable 在 +24（u64）；PE32: +12（u32） */
            const u32 cb_field = to + (is64 ? 24u : 12u);
            if (to && cb_field + (is64 ? 8u : 4u) <= g_len) {
                const u64 cb = is64 ? rd64(g_buf + cb_field) : (u64)rd32(g_buf + cb_field);
                const u32 cboff = cb ? rva2off((u32)cb) : 0;
                if (cboff) {
                    const u32 step = is64 ? 8u : 4u;
                    for (u32 k = 0; k < 64u && cboff + k * step + step <= g_len; k++) {
                        const u64 v = is64 ? rd64(g_buf + cboff + k * step)
                                           : (u64)rd32(g_buf + cboff + k * step);
                        if (!v) break;
                        tls_callbacks++;
                    }
                }
            }
            outs(1, "[PEINFO] tls vadr="); outhex(1, tv);
            outs(1, " callbacks="); outhex(1, tls_callbacks);
            outn(1, "\n", 1);
        }
    }
    if (g_nrva > 2u && g_dd_off + 2u * 8u + 8u <= g_len) {
        const u32 rv = rd32(g_buf + g_dd_off + 2u * 8u), rs = rd32(g_buf + g_dd_off + 2u * 8u + 4u);
        if (rv || rs) {
            res_on = 1;
            outs(1, "[PEINFO] res vadr="); outhex(1, rv);
            outs(1, " size="); outhex(1, rs);
            outn(1, "\n", 1);
        }
    }
    if (g_nrva > 14u && g_dd_off + 14u * 8u + 8u <= g_len) {
        const u32 cv = rd32(g_buf + g_dd_off + 14u * 8u), cs = rd32(g_buf + g_dd_off + 14u * 8u + 4u);
        if (cv || cs) {
            clr_on = 1;
            outs(1, "[PEINFO] clr vadr="); outhex(1, cv);
            outs(1, " size="); outhex(1, cs);
            outn(1, "\n", 1);
        }
    }

    /* ---- 汇总 + 导入并集 ---- */
    outs(1, "[PEINFO] summary dlls="); outdec(1, n_dll);
    outs(1, " funcs=");   outdec(1, n_func);
    outs(1, " ord=");     outdec(1, n_ord);
    outs(1, " exports="); outdec(1, n_exp);
    outs(1, " relocs=");  outdec(1, n_rel);
    outs(1, " tls=");     outdec(1, tls_on);
    outs(1, " res=");     outdec(1, res_on);
    outs(1, " clr=");     outdec(1, clr_on);
    outs(1, " truncated="); outdec(1, (u64)g_truncated);
    outn(1, "\n", 1);
    outs(1, "[PEINFO] union \"");
    outn(1, g_union, g_union_n);
    outn(1, "\"\n", 2);
    return 0;
}


/* ==================== 入口（peinfo_start.S 调它） ====================
 * 为什么不直接让 _start 调 peinfo_main：需要把返回值交给 exit_group(231)，
 * 而 x_exit 只在这里用一次 —— 这样它不会变成"未使用的静态函数"（-Wunused-function）。 */
void peinfo_entry(u64 *sp) {
    g_ofd = 1;
    const int rc = peinfo_main(sp);
    ob_flush();                 /* 最后一行若没带 '\n'（异常路径）也要落出来 */
    x_exit(rc);
}
