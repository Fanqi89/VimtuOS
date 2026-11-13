/* peload.c - ★ v0.9.44（W1）：**Windows PE 装载器**（Ring 3，静态 ELF64，`/bin/peload`）
 *
 * ============================ 这一版做了什么 ============================
 * 把一份 PE32+ 装进**当前进程自己的地址空间**，做完 `IMAGE_REL_BASED_DIR64` 重定位、
 * 填好 IAT（用我们自己的极小桩表），然后**直接 `call` 它的 AddressOfEntryPoint**，
 * 把入口的返回值拿回来打进串口。
 *
 *     run /bin/peload /pexcorpus/tiny.exe    -> entry_ret=0   （无导入、无重定位）
 *     run /bin/peload /pexcorpus/reloc.exe   -> entry_ret=42  （重定位真的做对了，自证）
 *     run /bin/peload /pexcorpus/exit.exe    -> exit-stub code=7，进程以 7 退出（IAT 桩自证）
 *     run /bin/peload /pexcorpus/cui.exe     -> ★ W2 起：CRT 真的跑起来，打印它自己那一行
 *                                               "hello from a real Windows console exe" 并退出 0
 *                                               （W1 时这里还是 import MISSING 如实失败）
 *     run /bin/peload /pexcorpus/gsprobe.exe -> entry_ret=42（GS.base/TEB/PEB 自证）
 *
 * ============================ 步骤 0：开工前查证的 3 条内核事实 ============================
 * 交接单要求"读代码，别猜"。三条都查了，**其中一条推翻了交接单的风险假设**：
 *
 * 1) `mmap` 的 hint 支持 —— `kernel/proc64.cpp:1639-1643`：
 *        const uint64_t hint = p64_page64(addr);
 *        if (hint >= USER64_MMAP_VA64 && user64_find_free64(hint, n) == hint) va = hint;
 *        else va = user64_find_free64(p->mmap_next ? p->mmap_next : USER64_MMAP_VA64, n);
 *    → hint **会被尊重**，但只在 `hint >= USER64_MMAP_VA64`（4GiB+576KiB）且那块正好空着时。
 *    我们**刻意传 addr=0**（不放 hint），于是 base 落在 mmap bump 区，
 *    而 mingw 的 ImageBase 是 0x140000000（5GiB）→ **delta 一定非 0**，
 *    重定位是**真活**，不是 no-op。这正好是交接单 §5.5 风险 1 要求的对策。
 *
 * 2) `mprotect` 是否允许 PROT_EXEC —— `kernel/proc64.cpp:1732-1734`：
 *        const uint64_t f = PTE_USER_64 | ((prot & 2u) ? PTE_WRITE_64 : 0u)
 *                                        | ((prot & 4u) ? 0u : PTE_NX_64);
 *    → prot & 4 会**清掉 NX**，即 PROT_EXEC 是允许的；prot & 2 会**加上** PTE_WRITE。
 *    所以 W^X 这条能做严：先 mmap 成 RW 写内容，再 `mprotect(R|X)` 收紧。
 *    **不需要**"干脆映射成 RWX 记个例外"。
 *
 * 3) SSE 可用性 —— `kernel/usermode64.cpp:152-158`（`u64_fpu_enable64`）：
 *        cr0 &= ~(CR0.EM | CR0.TS);   cr4 |= CR4.OSFXSR | CR4.OSXMMEXCPT;
 *    且 `kernel/task64.cpp:206-245` 每个 TCB 一份 512 B 的 fxsave 区，
 *    切走 `fxsave64`、切进 `fxrstor64`。
 *    → **`movaps`/`movdqa` 不会 #UD**，而且 xmm 状态是按任务保存的。
 *    ★ **这条推翻了交接单 §5.3.3 的"cui/gui 大概率 #UD"假设** —— 它们跑不起来的
 *    真实原因会是**缺导入**（W2 的活），不是 SSE。判据 ④ 因此仍然成立，但理由要写对。
 *
 * ============================ 为什么全在 Ring 3、内核 0 行改动 ============================
 * ring3 已经有 mmap(9) / mprotect(10) / munmap(11)，而 PE 装载本质是
 * "拷字节 + 改几个 8 字节整数 + mprotect + 同地址空间 call" —— 全部发生在用户态。
 * 内核不需要认识 PE。这与"格式解析器不拿 ring0 权限"（ext4 ring3 服务同源）是一条纪律。
 *
 * ============================ 输出契约（tests/peload64_test.py 按这些串 grep；格式勿改） ============================
 *   [PELOAD] file=<p> size=<n> read=<n>
 *   [PELOAD] nt machine=0x<n> nsec=<n> sizeimage=0x<n> sizehdr=0x<n> imagebase=0x<n> entry=0x<n> sectalign=0x<n>
 *   [PELOAD] sec idx=<n> name=<8B 去尾零/空格> va=0x<n> vsz=0x<n> raw=0x<n> rawsz=0x<n> perm=<r|-|-><w|-|-><x|-|->
 *   [PELOAD] map base=0x<n> size=0x<n> imagebase=0x<n> delta=0x<n>
 *   [PELOAD] reloc blocks=<n> entries=<n> applied=<n> types=<0xN,…|-> skipped=<n>
 *   [PELOAD] import dll=<name> funcs=<n> ord=<n>
 *   [PELOAD] import fill dll=<name> name=<func> at=0x<n>
 *   [PELOAD] import MISSING dll=<name> name=<func>|ord#<n>
 *   [PELOAD] import summary filled=<n> missing=<n>
 *   [PELOAD] prot idx=<n> name=<..> perm=<rwx>
 *   [PELOAD] enter entry=0x<n> rsp=0x<n>
 *   [PELOAD] ok path=<p> entry_ret=<n> reloc=<n> import=<n>
 *   [PELOAD] exit-stub code=<n>
 *   [PELOAD] reject reason=<dos|nt|opt|magic|trunc|io|pe32>
 * 退出码：0 = PE 跑完并返回；1 = 装载失败（不支持 / 缺导入 / 解析错）；2 = 用法错误 / 不是 PE / 读不出来。
 *
 * ============================ W1 明确**不做**的（留 W2..W5，不许假装） ============================
 *   SEH（`.pdata`/`RtlUnwind`）、TLS 回调（dir[9]）、资源加载、延迟导入（dir[13]）、
 *   bound import、A/W 双份导出、**TEB/PEB + GS.base**、真 DLL 桩库文件、
 *   PE32(0x10B，32 位) 映像。上面每一条都会在遇到时**如实报**（reject/unsupported），不静默跳过。
 *
 * ============================ 行缓冲（★ W0 用血换来的纪律） ============================
 * 内核在**每次** write(1/2) 返回时都打一行 `[SYSCALL] insn nr=1 …`。
 * 所以任何往 fd 1/2 打印的用户态程序必须**一次 write 一整行**，否则整行被内核日志切碎
 * （W0 的 peinfo 第一版 17 条红全是这一个原因）。这里照抄同一套 4 KiB 行缓冲。
 */

/* ★ v0.9.45（W2）：类型与输出函数、桩表结构统一由 peload.h 提供
 *   （peload_crt.c 也要用同一套，所以不能各写一份 typedef）。 */
#include "peload.h"

/* ==================== 系统调用（Linux x86_64 号段） ==================== */
#define NR_READ      0
#define NR_WRITE     1
#define NR_OPEN      2
#define NR_CLOSE     3
#define NR_MMAP      9
#define NR_MPROTECT  10
#define NR_EXITG     231

#define O_RDONLY     0
#define PROT_R       1
#define PROT_W       2
#define PROT_X       4
#define MAP_PRIV_ANON 0x22

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
/* 匿名映射：一律先拿 R|W（要往里写节内容），最后按节权限 mprotect 收紧。 */
static void *x_mmap(u64 len, u64 prot) {
    const i64 r = sc6(NR_MMAP, 0, (i64)len, (i64)prot, (i64)MAP_PRIV_ANON, -1, 0);
    if (r < 0) return 0;
    return (void *)(u64)r;
}
static i64 x_mprotect(u64 addr, u64 len, u64 prot) {
    return sc3(NR_MPROTECT, (i64)addr, (i64)len, (i64)prot);
}
static void x_exit(int code) { (void)sc1(NR_EXITG, (i64)code); for (;;) { } }

/* ==================== 小工具 ==================== */
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

/* ==================== 输出：行级缓冲（一次 write 一整行） ==================== */
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

/* ★ v0.9.45（W2）：把上面这套行级缓冲暴露给 peload_crt.c（PE 的 printf 也走这里，
 *   所以"一次 write 一整行"的纪律在 CRT 输出上同样成立）。 */
void pl_out(int fd, const char *s, u32 n) { outn(fd, s, n); }
void pl_outs(int fd, const char *s) { outs(fd, s); }
void pl_dec(int fd, u64 v) { outdec(fd, v); }
void pl_hex(int fd, u64 v) { outhex(fd, v); }
void pl_flush(void) { ob_flush(); }

/* 小端偏移读（**不定义 PE 结构体**：避免对齐/填充在不同编译器下漂） */
static u16 rd16(const u8 *p) { return (u16)((u32)p[0] | ((u32)p[1] << 8)); }
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) {
    u64 v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | (u64)p[i];
    return v;
}
static void wr64(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) { p[i] = (u8)(v & 0xFFu); v >>= 8; }
}
static void bcopy(u8 *d, const u8 *s, u32 n) {
    for (u32 i = 0; i < n; i++) d[i] = s[i];
}
static void bzero(u8 *d, u32 n) {
    for (u32 i = 0; i < n; i++) d[i] = 0;
}
static int ci_eq(const char *a, const char *b) {          /* 大小写不敏感比较（DLL 名用） */
    for (;;) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* ★ v0.9.45（W2）：桩表在 peload_crt.c（kernel32 + 老式 msvcrt，共 34 条）。
 *   这里只按 (dll, name) 大小写不敏感地查 —— 数据导入与函数导入在**填 IAT 这点上完全一样**：
 *   槽里放的都是"那个东西的地址"（函数 = 跳板地址，变量 = 变量地址）。
 *   `is_data` 只在 GetProcAddress 里用（数据不能当函数返回）。 */
static void *stub_lookup(const char *dll, const char *fn) {
    for (u32 i = 0; i < g_pl_stubs.n; i++) {
        const PlStubEntry *e = &g_pl_stubs.tab[i];
        if (ci_eq(e->dll, dll) && ci_eq(e->name, fn)) return e->addr;
    }
    return 0;
}

/* ==================== 规模上限（都如实报，不静默截断） ==================== */
#define MAX_PE_BYTES  (8u * 1024u * 1024u)   /* = VFS 单文件上限 */
#define MAX_SEC       96u
#define MAX_DLL       32u
#define MAX_THUNKS    4096u                  /* 单个 DLL 的槽数上限 */
#define MAX_MISS_LIST 48u                    /* 明细最多打这么多条，计数照实 */

/* ★ v0.9.45（W2）：W1 的那张"极小桩表"（ExitProcess / GetProcAddress / LoadLibraryA）
 *   与它们的 C 实现已经**整体迁到 peload_crt.c**，并扩到 34 条（kernel32 10 + msvcrt 20 函数
 *   + msvcrt 4 个**数据**导入）。这里不再保留第二份 —— 两份桩表必然会漂。 */

/* ==================== 装载状态 ==================== */
static u8  *g_file;        /* 文件内容（mmap 8 MiB） */
static u32  g_len;
static u8  *g_img;         /* 映像（mmap SizeOfImage） */
static u64  g_base;        /* 映像运行期基址 */
static u64  g_imagebase;   /* 链接期基址（PE 头里的 ImageBase） */
static u64  g_delta;       /* g_base - g_imagebase */
static u32  g_sizeimage, g_sizehdr, g_sectalign, g_sec_off, g_opt_off, g_nsec, g_nrva, g_dd_off;
static u32  g_entry_rva;
static u32  g_n_reloc_applied, g_n_reloc_entries, g_n_reloc_skipped, g_n_reloc_blocks;
static u32  g_n_import_filled, g_n_import_missing;
static u32  g_types_seen;

/* RVA -> 文件偏移。落在任何节之外返回 0（调用方必须当"取不到"，绝不越界读）。 */
static u32 rva2off(u32 rva) {
    for (u32 i = 0; i < g_nsec; i++) {
        const u32 so = g_sec_off + i * 40u;
        const u32 vsz = rd32(g_file + so + 8);
        const u32 va  = rd32(g_file + so + 12);
        const u32 rsz = rd32(g_file + so + 16);
        const u32 raw = rd32(g_file + so + 20);
        const u32 span = vsz > rsz ? vsz : rsz;
        if (rva >= va && rva < va + span) {
            const u32 o = raw + (rva - va);
            if (raw == 0 || o >= g_len) return 0;
            return o;
        }
    }
    return 0;
}

static void out_secname(int fd, const u8 *p) {
    u32 n = 0;
    while (n < 8u && p[n]) n++;
    while (n && (p[n - 1] == ' ')) n--;
    if (!n) { outn(fd, "-", 1); return; }
    for (u32 i = 0; i < n; i++) ob_put(fd, (char)p[i]);
}

/* ==================== 步骤 7：DIR64 重定位 ==================== */
static void do_reloc(void) {
    u32 rva = 0, size = 0;
    if (g_nrva > 5u && g_dd_off + 5u * 8u + 8u <= g_len) {
        rva  = rd32(g_file + g_dd_off + 5u * 8u);
        size = rd32(g_file + g_dd_off + 5u * 8u + 4u);
    }
    if (!rva || !size) {
        outs(1, "[PELOAD] reloc blocks=0 entries=0 applied=0 types=- skipped=0\n");
        return;
    }
    const u32 off = rva2off(rva);
    if (!off || off + size > g_len) {
        outs(1, "[PELOAD] reloc blocks=0 entries=0 applied=0 types=- skipped=0 (dir unreadable)\n");
        return;
    }
    u32 p = 0;
    while (p + 8u <= size) {
        const u32 page = rd32(g_file + off + p);
        const u32 blk  = rd32(g_file + off + p + 4u);
        if (blk < 8u || p + blk > size) break;
        g_n_reloc_blocks++;
        for (u32 q = 8u; q + 2u <= blk; q += 2u) {
            const u16 e = rd16(g_file + off + p + q);
            const u32 type = (u32)(e >> 12);
            const u32 ofs  = (u32)(e & 0xFFFu);
            g_n_reloc_entries++;
            /* ★ `types=` 的语义是"**见到**了哪些类型"，不是"哪些类型没处理"。
             *   第一版写成后者，于是刚应用完一条 DIR64 却打出 `types=-` —— 自己在骗自己。
             *   type 0 = IMAGE_REL_BASED_ABSOLUTE，只是块内对齐填充，不进 types。 */
            if (type) g_types_seen |= (1u << (type & 15u));
            if (type == 0u) continue;                       /* ABSOLUTE：填充项，正常 */
            if (type == 10u) {                              /* IMAGE_REL_BASED_DIR64 */
                const u64 va = g_base + page + ofs;
                if (va + 8u > g_base + g_sizeimage) { g_n_reloc_skipped++; continue; }
                const u64 old = rd64((const u8 *)(u64)va);
                wr64((u8 *)(u64)va, old + g_delta);
                g_n_reloc_applied++;
            } else {
                /* W1 只处理 DIR64(10)。HIGHLOW(3)/HIGH(1)/LOW(2) 属于 PE32，不会出现在 PE32+；
                 * 其余类型如实计入 skipped（类型已在上面记进 types），不假装做过。 */
                g_n_reloc_skipped++;
            }
        }
        p += blk;
    }
    outs(1, "[PELOAD] reloc blocks="); outdec(1, g_n_reloc_blocks);
    outs(1, " entries="); outdec(1, g_n_reloc_entries);
    outs(1, " applied="); outdec(1, g_n_reloc_applied);
    outs(1, " types=");
    if (!g_types_seen) outn(1, "-", 1);
    for (u32 t = 0; t < 16u; t++) {
        if (g_types_seen & (1u << t)) {
            outhex(1, t);
            int more = 0;
            for (u32 q = t + 1u; q < 16u; q++) if (g_types_seen & (1u << q)) { more = 1; break; }
            if (more) outn(1, ",", 1);
        }
    }
    outs(1, " skipped="); outdec(1, g_n_reloc_skipped);
    outn(1, "\n", 1);
}

/* ==================== 步骤 8：IAT 填充 ==================== */
static void do_imports(void) {
    u32 rva = 0, size = 0;
    if (g_nrva > 1u && g_dd_off + 1u * 8u + 8u <= g_len) {
        rva  = rd32(g_file + g_dd_off + 1u * 8u);
        size = rd32(g_file + g_dd_off + 1u * 8u + 4u);
    }
    (void)size;
    if (!rva) { outs(1, "[PELOAD] import summary filled=0 missing=0 (no import dir)\n"); return; }
    const u32 off = rva2off(rva);
    if (!off) { outs(1, "[PELOAD] import summary filled=0 missing=0 (dir unreadable)\n"); return; }

    for (u32 d = 0; d < MAX_DLL; d++) {
        const u32 p = off + d * 20u;
        if (p + 20u > g_len) break;
        const u32 ilt = rd32(g_file + p + 0u);
        const u32 nm  = rd32(g_file + p + 12u);
        const u32 ft  = rd32(g_file + p + 16u);
        if (!nm && !ft && !ilt) break;                       /* 全 0 = 列表结束 */
        const u32 nmp = rva2off(nm);
        if (!nmp) break;
        char dll[44];
        u32 dl = 0;
        while (dl < 43u && nmp + dl < g_len && g_file[nmp + dl]) { dll[dl] = (char)g_file[nmp + dl]; dl++; }
        dll[dl] = 0;

        const u32 thunk_rva = ilt ? ilt : ft;
        const u32 to = rva2off(thunk_rva);
        if (!to || !ft) break;

        u32 fcount = 0, ocount = 0;
        for (u32 i = 0; i < MAX_THUNKS; i++) {
            const u32 tp = to + i * 8u;
            if (tp + 8u > g_len) break;
            const u64 v = rd64(g_file + tp);
            if (!v) break;
            u64 slot = g_base + ft + (u64)i * 8u;
            if (slot + 8u > g_base + g_sizeimage) break;
            if (v & 0x8000000000000000ULL) {                 /* 按序号导入 */
                ocount++;
                g_n_import_missing++;
                if (g_n_import_missing <= MAX_MISS_LIST) {
                    outs(1, "[PELOAD] import MISSING dll="); outs(1, dll);
                    outs(1, " ord#"); outdec(1, v & 0xFFFFULL);
                    outn(1, "\n", 1);
                }
                continue;
            }
            const u32 hn = rva2off((u32)v);
            if (!hn || hn + 2u >= g_len) break;
            char fname[72];
            u32 fl = 0;
            while (fl < 71u && hn + 2u + fl < g_len && g_file[hn + 2u + fl]) {
                fname[fl] = (char)g_file[hn + 2u + fl];
                fl++;
            }
            fname[fl] = 0;
            fcount++;
            const u64 a = (u64)stub_lookup(dll, fname);
            if (a) {
                wr64((u8 *)(u64)slot, a);
                g_n_import_filled++;
                outs(1, "[PELOAD] import fill dll="); outs(1, dll);
                outs(1, " name="); outs(1, fname);
                outs(1, " at="); outhex(1, slot);
                outn(1, "\n", 1);
            } else {
                g_n_import_missing++;
                if (g_n_import_missing <= MAX_MISS_LIST) {
                    outs(1, "[PELOAD] import MISSING dll="); outs(1, dll);
                    outs(1, " name="); outs(1, fname);
                    outn(1, "\n", 1);
                }
            }
        }
        outs(1, "[PELOAD] import dll="); outs(1, dll);
        outs(1, " funcs="); outdec(1, fcount);
        outs(1, " ord=");   outdec(1, ocount);
        outn(1, "\n", 1);
    }
    outs(1, "[PELOAD] import summary filled="); outdec(1, g_n_import_filled);
    outs(1, " missing="); outdec(1, g_n_import_missing);
    outn(1, "\n", 1);
}

/* ==================== 步骤 9：按节权限收紧（W^X：先 RW 写，再 mprotect） ==================== */
static void do_prot(void) {
    for (u32 i = 0; i < g_nsec; i++) {
        const u32 so = g_sec_off + i * 40u;
        const u32 vsz  = rd32(g_file + so + 8);
        const u32 va   = rd32(g_file + so + 12);
        const u32 rsz  = rd32(g_file + so + 16);
        const u32 fl   = rd32(g_file + so + 36);
        const u32 span = vsz ? vsz : rsz;
        if (!span) continue;
        if ((u64)va + span > g_sizeimage) continue;
        u64 prot = 0;
        if (fl & 0x40000000u) prot |= PROT_R;
        if (fl & 0x80000000u) prot |= PROT_W;
        if (fl & 0x20000000u) prot |= PROT_X;
        if (!prot) prot = PROT_R;                            /* 0 权限页：给 R（否则读也 #PF） */
        const u64 s = (g_base + va) & ~0xFFFULL;
        const u64 e = (g_base + (u64)va + span + 0xFFFULL) & ~0xFFFULL;
        if (x_mprotect(s, e - s, prot) != 0) {
            outs(2, "[PELOAD] mprotect FAILED idx="); outdec(2, i); outn(2, "\n", 1);
            continue;
        }
        outs(1, "[PELOAD] prot idx="); outdec(1, i);
        outs(1, " name="); out_secname(1, g_file + so);
        outs(1, " perm=");
        ob_put(1, (prot & PROT_R) ? 'r' : '-');
        ob_put(1, (prot & PROT_W) ? 'w' : '-');
        ob_put(1, (prot & PROT_X) ? 'x' : '-');
        outn(1, "\n", 1);
    }
}

/* ==================== 主流程 ==================== */
int peload_main(u64 *sp) {
    const int argc = (int)sp[0];
    const char **argv = (const char **)(sp + 1);
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        outs(2, "[PELOAD] usage: peload <file.exe>\n");
        return 2;
    }
    const char *path = argv[1];

    /* ---- 1) 读文件（整份读进 mmap；上限 = VFS 单文件上限） ---- */
    g_file = (u8 *)x_mmap(MAX_PE_BYTES, PROT_R | PROT_W);
    if (!g_file) { outs(2, "[PELOAD] reject reason=io (mmap file)\n"); return 2; }
    const i64 fd = sc3(NR_OPEN, (i64)(u64)path, O_RDONLY, 0);
    if (fd < 0) {
        outs(2, "[PELOAD] reject reason=io path="); outs(2, path); outn(2, "\n", 1);
        return 2;
    }
    g_len = 0;
    for (;;) {
        if (g_len >= MAX_PE_BYTES) break;
        const i64 n = sc3(NR_READ, fd, (i64)(u64)(g_file + g_len), (i64)(MAX_PE_BYTES - g_len));
        if (n <= 0) break;
        g_len += (u32)n;
    }
    (void)sc1(NR_CLOSE, fd);
    outs(1, "[PELOAD] file="); outs(1, path);
    outs(1, " size="); outdec(1, g_len);
    outs(1, " read="); outdec(1, g_len);
    outn(1, "\n", 1);

    /* ---- 2) DOS 头 ---- */
    if (g_len < 64u || g_file[0] != 'M' || g_file[1] != 'Z') {
        outs(2, "[PELOAD] reject reason=dos\n");
        return 2;
    }
    const u32 lfanew = rd32(g_file + 0x3C);
    if (lfanew < 64u || lfanew + 24u > g_len ||
        !(g_file[lfanew] == 'P' && g_file[lfanew + 1] == 'E' &&
          g_file[lfanew + 2] == 0 && g_file[lfanew + 3] == 0)) {
        outs(2, "[PELOAD] reject reason=nt\n");
        return 2;
    }
    const u32 coff = lfanew + 4u;
    const u16 machine = rd16(g_file + coff);
    g_nsec = rd16(g_file + coff + 2u);
    const u16 optsz = rd16(g_file + coff + 16u);
    if (g_nsec == 0 || g_nsec > MAX_SEC) { outs(2, "[PELOAD] reject reason=nt (nsec)\n"); return 1; }

    /* ---- 3) 可选头：W1 只做 PE32+（0x20B） ---- */
    g_opt_off = coff + 20u;
    if (g_opt_off + 112u > g_len) { outs(2, "[PELOAD] reject reason=opt\n"); return 2; }
    const u16 magic = rd16(g_file + g_opt_off);
    if (magic != 0x20Bu) {
        outs(2, "[PELOAD] reject reason=magic magic="); outhex(2, magic);
        outs(2, " (W1 only loads PE32+ 0x20b)\n");
        return 1;
    }
    g_imagebase = rd64(g_file + g_opt_off + 24u);
    g_entry_rva = rd32(g_file + g_opt_off + 16u);
    g_sectalign = rd32(g_file + g_opt_off + 32u);
    g_sizeimage = rd32(g_file + g_opt_off + 56u);
    g_sizehdr   = rd32(g_file + g_opt_off + 60u);
    g_nrva      = rd32(g_file + g_opt_off + 108u);
    g_dd_off    = g_opt_off + 112u;
    if (g_nrva > 16u) g_nrva = 16u;
    g_sec_off   = g_opt_off + optsz;

    outs(1, "[PELOAD] nt machine="); outhex(1, machine);
    outs(1, " nsec=");     outdec(1, g_nsec);
    outs(1, " sizeimage="); outhex(1, g_sizeimage);
    outs(1, " sizehdr=");   outhex(1, g_sizehdr);
    outs(1, " imagebase="); outhex(1, g_imagebase);
    outs(1, " entry=");     outhex(1, g_entry_rva);
    outs(1, " sectalign="); outhex(1, g_sectalign);
    outn(1, "\n", 1);

    if (g_sizeimage == 0 || g_sizeimage > MAX_PE_BYTES || g_sizehdr > g_len ||
        g_sizehdr > g_sizeimage) {
        outs(2, "[PELOAD] reject reason=trunc (sizes)\n");
        return 2;
    }
    if ((u64)g_sec_off + (u64)g_nsec * 40u > (u64)g_len) {
        outs(2, "[PELOAD] reject reason=trunc (section table)\n");
        return 2;
    }

    /* ---- 4) 拿映像内存（addr=0：**故意不给 hint**，让 delta 一定非 0 → 重定位是真活） ---- */
    g_img = (u8 *)x_mmap(g_sizeimage, PROT_R | PROT_W);
    if (!g_img) { outs(2, "[PELOAD] reject reason=io (mmap image)\n"); return 1; }
    g_base  = (u64)g_img;
    g_delta = g_base - g_imagebase;
    outs(1, "[PELOAD] map base="); outhex(1, g_base);
    outs(1, " size=");       outhex(1, g_sizeimage);
    outs(1, " imagebase=");  outhex(1, g_imagebase);
    outs(1, " delta=");      outhex(1, g_delta);
    outn(1, "\n", 1);

    /* ---- 5/6) 先清零（不依赖 mmap 是否已经清过），再拷头，再逐节拷 ---- */
    bzero(g_img, g_sizeimage);
    bcopy(g_img, g_file, g_sizehdr);
    for (u32 i = 0; i < g_nsec; i++) {
        const u32 so = g_sec_off + i * 40u;
        const u32 vsz = rd32(g_file + so + 8);
        const u32 va  = rd32(g_file + so + 12);
        const u32 rsz = rd32(g_file + so + 16);
        const u32 raw = rd32(g_file + so + 20);
        const u32 fl  = rd32(g_file + so + 36);
        u32 copied = 0;
        if (rsz && raw && raw <= g_len && (u64)va + rsz <= (u64)g_sizeimage) {
            /* 只拷 min(SizeOfRawData, VirtualSize)：.bss 那段（rawsz..vsz）保持 0 */
            copied = (vsz && vsz < rsz) ? vsz : rsz;
            bcopy(g_img + va, g_file + raw, copied);
        }
        outs(1, "[PELOAD] sec idx="); outdec(1, i);
        outs(1, " name="); out_secname(1, g_file + so);
        outs(1, " va=");   outhex(1, va);
        outs(1, " vsz=");  outhex(1, vsz);
        outs(1, " raw=");  outhex(1, raw);
        outs(1, " rawsz="); outhex(1, rsz);
        outs(1, " perm=");
        ob_put(1, (fl & 0x40000000u) ? 'r' : '-');
        ob_put(1, (fl & 0x80000000u) ? 'w' : '-');
        ob_put(1, (fl & 0x20000000u) ? 'x' : '-');
        outn(1, "\n", 1);
        (void)copied;
    }

    /* ---- 7) 重定位 ---- */
    if (g_delta) do_reloc();
    else outs(1, "[PELOAD] reloc blocks=0 entries=0 applied=0 types=- skipped=0 (delta==0)\n");

    /* ---- 7.5) ★ v0.9.45（W2）：TEB/PEB + GS.base ----
     * 必须在填 IAT 之前：CRT 状态（iob / argv / 堆）先就位，桩实现才敢被调；
     * 而且这一步会**验证** GS.base 真的写进去了（读写回比）。失败就如实失败，
     * 不能带着"%gs 是 0"跳进一个会读 TEB 的 exe —— 那会在 PE 的第一条 gs 指令上 #GP。 */
    if (!pl_crt_setup(g_base, g_sizeimage, path)) {
        outs(2, "[PELOAD] fail reason=crt-setup (teb/peb/gs)\n");
        return 1;
    }

    /* ---- 8) IAT 填充（★ 填充完再收紧权限：IAT 在 .rdata/.idata 里，那时还得可写） ---- */
    do_imports();

    /* 缺导入 = **整体如实失败**。绝不带着空 IAT 跳进去（那会在 PE 第一条 call 上炸 #PF，
     * 看起来像"装载器崩了"，掩盖真正原因）。 */
    if (g_n_import_missing) {
        outs(2, "[PELOAD] fail reason=missing-imports count="); outdec(2, g_n_import_missing);
        outn(2, "\n", 1);
        return 1;
    }

    /* ---- 9) 按节权限收紧 ---- */
    do_prot();

    /* ---- 10/11) 跳进入口并拿回返回值 ---- */
    const u64 entry_va = g_base + g_entry_rva;
    u64 sp_now = 0;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp_now));
    outs(1, "[PELOAD] enter entry="); outhex(1, entry_va);
    outs(1, " rsp="); outhex(1, sp_now);
    outn(1, "\n", 1);

    typedef long (*PeloadEntry)(void);
    PeloadEntry const fn = (PeloadEntry)(u64)entry_va;
    const long ret = fn();
    pl_crt_run_exit_chain();        /* PE 若用 ret 回来的（无 CRT 的语料），atexit 也要跑完 */

    /* ---- 12) 成功 ---- */
    outs(1, "[PELOAD] ok path="); outs(1, path);
    outs(1, " entry_ret="); outdec(1, (u64)ret);
    outs(1, " reloc=");     outdec(1, g_n_reloc_applied);
    outs(1, " import=");    outdec(1, g_n_import_filled);
    outn(1, "\n", 1);
    return 0;
}

/* ==================== 入口（peload_start.S 调它） ====================
 * 返回值交给 exit_group(231)：PE 的 entry_ret 用 `[PELOAD] ok` 行表达，
 * 进程退出码只表达"装载器自己成功与否"（0/1/2），两者不要混。 */
void peload_entry(u64 *sp) {
    g_ofd = 1;
    const int rc = peload_main(sp);
    ob_flush();
    x_exit(rc);
}
