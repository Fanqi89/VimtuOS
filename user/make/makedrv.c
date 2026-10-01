/* makedrv.c - ★ B5：Ring 3 的 GNU make 装载驱动（= 系统卷里的 /bin/make）
 *
 * 为什么需要它（与 /bin/tcc、/bin/lua 同一条理由，见 user/apps/tcc/tccdrv.c 的说明）：
 *   VimtuOS 的内核 ELF64 装载器只装"主程序"，而主程序的 PT_LOAD 必须完整落在**用户窗口低
 *   64 KiB**（kernel/usermode64.h 的 4GiB..USER64_STACK_VA64；elf64.cpp 的 e64_parse_ex64
 *   会把越界的段拒掉）。静态链接的 GNU make 是几百 KB，装不进 64 KiB —— 直接 `run /bin/make`
 *   只会得到一条装载拒绝。
 *
 *   本内核另一条能装"大映像"的路是 PT_INTERP 解释器槽，但那要求映像是 ET_DYN/位置无关
 *   （内核给解释器挑基址、且**不做任何重定位**）。make 是静态非 PIC 的 4GiB 定址 ELF。
 *
 *   所以本驱动走第三条路（与 /bin/tcc、/bin/lua 完全同构）：
 *     1) 读 /lib/make.bin 的 ELF 头/程序头；
 *     2) 用 mmap(9) 在 **USER64_MMAP_VA64 = 4GiB+0x90000** 申请 span 字节（本进程第一次
 *        mmap 恰好落在这个地址 —— 内核的每进程 bump 分配器起点就是它）；
 *     3) 按 p_offset/p_filesz 把各段搬进 mmap 区（make 就是按这个地址静态链接的，不需要重定位）；
 *     4) 把内核压在初始栈上的 auxv 里 AT_PHDR/AT_PHENT/AT_PHNUM/AT_ENTRY 改成 **make 自己的**
 *        （musl 的 static_init_tls/__init_libc 靠 AT_PHDR 遍历程序头表找 PT_TLS）；
 *     5) 恢复初始 rsp，直接 jmp 到 make 的 e_entry。
 *   于是 make 看到的 argc/argv/envp/auxv 与"内核直接装载它"完全一样（argv[0] 仍然是
 *   /bin/make，后面跟着用户写的参数），**用户命令行不受任何影响**：
 *       run /bin/make -v
 *       run /bin/make -C /tmp/proj all
 *
 * 栈：内核给的 16 KiB 用户栈对 musl 的 vfprintf / make 的递归展开（variable_expand /
 *   eval）都不够，这里在映像之后再 mmap 一块 **256 KiB** 的栈（顺序不能反：映像必须落在
 *   固定的 4GiB+0x90000），把初始栈上的那一段（argc/argv/envp/auxv + 参数字符串）原样搬过去。
 *   ★ 实测修（本批）：原来这里是 512 KiB。内核的 fork 是**整页复制**父进程的用户区，
 *   上限 kernel/proc64.cpp 的 PROC64_FORK_MAX_PAGES = 256 页（≈1 MiB）—— 512 KiB 的栈
 *   （128 页）会被 recipe 的 `/bin/sh -c` 子进程整份继承过去，于是"shell 再 fork 出 /bin/tcc"
 *   这一步直接超限：`[PROC64] fork FAILED reason=copy-or-limit`，make 只能报
 *   `make: *** [<builtin>: main.o] Error 127`（一条 tcc 命令都没起来）。降到 256 KiB
 *   （64 页）后，make + 它的 recipe 子进程合计仍在上限内。**注意**：子进程 execve 之后
 *   拿到的是内核给的新 16 KiB 栈，本驱动这块栈只是"被继承的页数"变少了，语义没变。
 *
 * 本驱动不引任何 libc（系统调用全部内联汇编直接发）；串口/控制台打点单次 ≤ 1024 字节。
 * 出错一律打 `[MAKEDRV] FAIL reason=...` 后 exit_group(127)，绝不"假装成功"。
 */

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned int u32;
typedef unsigned short u16;
typedef short i16;

/* ---- 与本内核对齐的常量（用户态看不到 kernel 的头文件，这里是同一份约定的拷贝）---- */
#define DRV_STACK_BYTES 262144ULL               /* 给 make 的栈：256 KiB（512 KiB 会撑爆 fork 的 256 页上限，见文件头说明） */
#define U64_STACK_VA64  0x0000000100010000ULL   /* USER64_STACK_VA64：内核给的用户栈底 */
#define U64_MMAP_VA64   0x0000000100090000ULL   /* USER64_MMAP_VA64：mmap bump 起点 */
#define U64_WINDOW_TOP  0x0000000101000000ULL   /* 4GiB + 16MiB（窗口顶） */
#define MAKE_SPAN_MAX   (U64_WINDOW_TOP - U64_MMAP_VA64)   /* ~15.4 MiB */
#define MAKE_PATH       "/lib/make.bin"
#define ELF64_PHDR_SIZE 56
#define E64_MAX_PHDR    16

/* ---- Linux x86_64 号段（本内核实现的那几个）---- */
#define NR_READ   0
#define NR_WRITE  1
#define NR_OPEN   2
#define NR_CLOSE  3
#define NR_LSEEK  8
#define NR_MMAP     9
#define NR_MPROTECT 10
#define NR_EXITG    231

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

/* ---- 输出（fd 1 = 内核控制台；shell 的 `>` 重定向后就是文件）---- */
#define OUT_BUF_BYTES 1024
static char g_outbuf[OUT_BUF_BYTES];
static unsigned long g_outlen = 0;

static void out_flush(void) {
    unsigned long off = 0;
    while (off < g_outlen) {
        const i64 w = sc3(NR_WRITE, 1, (i64)(g_outbuf + off), (i64)(g_outlen - off));
        if (w <= 0) break;                       /* 控制台写失败：丢弃，绝不死循环 */
        off += (unsigned long)w;
    }
    g_outlen = 0;
}
static void out_n(const char* s, unsigned long n) {
    while (n) {
        unsigned long room = (unsigned long)OUT_BUF_BYTES - g_outlen;
        if (!room) { out_flush(); room = (unsigned long)OUT_BUF_BYTES; }
        const unsigned long k = (n < room) ? n : room;
        for (unsigned long i = 0; i < k; i++) g_outbuf[g_outlen + i] = s[i];
        g_outlen += k;
        s += k; n -= k;
    }
}
static unsigned long slen(const char* s) { unsigned long n = 0; while (s[n]) n++; return n; }
static void out_s(const char* s) { out_n(s, slen(s)); }
static void out_dec(u64 v) {
    char t[24]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n) out_n(&t[--n], 1);
}
static void out_hex(u64 v) {
    static const char H[] = "0123456789abcdef";
    char t[18]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = H[v & 0xFu]; v >>= 4; }
    out_s("0x");
    while (n) out_n(&t[--n], 1);
}
static void out_eol(void) { out_n("\n", 1); out_flush(); }
static void die(const char* why) {
    out_s("[MAKEDRV] FAIL reason=");
    out_s(why);
    out_eol();
    sc1(NR_EXITG, 127);
    for (;;) { }
}

/* ---- ELF64 结构（只看要用的字段；字段顺序与 elf.h 一致）---- */
struct Ehdr {
    unsigned char e_ident[16];
    u16 e_type, e_machine;
    u32 e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct Phdr {
    u32 p_type, p_flags;
    u64 p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};

#define PT_LOAD 1u

/* ---- 初始栈布局（内核 elf64.cpp 的 e64_build_stack_argv64）----
 *   [0]=argc，[1..argc]=argv，[argc+1]=NULL，[argc+2]=envp 终止 NULL，然后是 auxv 键值对，
 *   最后 AT_NULL(0)。本内核没有环境变量，所以 envp 直接是 NULL。
 */
static const u64 AT_PHDR = 3, AT_PHENT = 4, AT_PHNUM = 5, AT_ENTRY = 9, AT_BASE = 7;

static u64* auxv_of(u64* sp) {
    const u64 argc = sp[0];
    u64* p = sp + 1 + argc + 1;          /* 跳过 argc / argv / argv 终止 */
    while (*p) p++;                      /* 跳过 envp（本内核为空 -> 不循环） */
    return p + 1;                        /* auxv 的第一对 */
}

/* ---- make 的入口：恢复初始 rsp 后 jmp（C 里做不到"jmp 到不返回的入口且不压栈"，
 *      所以要一小段内联汇编；make 的 _start 只读 rsp）---- */
static void jump_to(u64* sp, u64 entry) {
    __asm__ volatile("movq %0, %%rsp\n\tjmp *%1" :: "r"(sp), "r"(entry) : "memory");
    for (;;) { }
}

/* ---- 装载 ---- */
static unsigned char g_hdrbuf[4096];      /* ELF 头 + 16 个程序头（64 + 16*56 = 960 B）用得很少 */

void drv_main(u64* sp) {
    const u64 argc = sp[0];
    const char* argv0 = (const char*)(sp[1]);

    out_s("[MAKEDRV] start path=/bin/make argc=");
    out_dec(argc);
    out_s(" argv0=");
    out_s(argv0 ? argv0 : "(null)");
    out_eol();

    const i64 fd = sc3(NR_OPEN, (i64)MAKE_PATH, 0, 0);
    if (fd < 0) die("open-make");
    /* 头 + 程序头表一次读进来（4096 B 一次读满：内核单次 read 上限 4096） */
    const i64 got = sc3(NR_READ, fd, (i64)g_hdrbuf, 4096);
    if (got < 64) die("read-hdr");
    struct Ehdr* eh = (struct Ehdr*)g_hdrbuf;
    if (!(eh->e_ident[0] == 0x7F && eh->e_ident[1] == 'E' && eh->e_ident[2] == 'L' && eh->e_ident[3] == 'F'))
        die("not-elf");
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) die("not-elf64-le");
    if (eh->e_type != 2) die("not-et-exec");           /* 静态定址映像必须是 ET_EXEC */
    if (eh->e_machine != 0x3E) die("not-x86_64");
    if (eh->e_phentsize != ELF64_PHDR_SIZE || eh->e_phnum == 0 || eh->e_phnum > E64_MAX_PHDR)
        die("bad-phdr");
    if (eh->e_phoff + (u64)eh->e_phnum * ELF64_PHDR_SIZE > (u64)got) die("phdr-not-in-read");

    struct Phdr* ph = (struct Phdr*)(g_hdrbuf + eh->e_phoff);
    u64 span = 0;
    unsigned segs = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        segs++;
        if (ph[i].p_vaddr < U64_MMAP_VA64) die("seg-below-mmap");
        const u64 end = ph[i].p_vaddr + ph[i].p_memsz;
        if (end < ph[i].p_vaddr) die("seg-overflow");
        if (end - U64_MMAP_VA64 > MAKE_SPAN_MAX) die("seg-too-big");
        if (end - U64_MMAP_VA64 > span) span = end - U64_MMAP_VA64;
    }
    if (!segs) die("no-load-seg");
    span = (span + 0xFFFu) & ~0xFFFu;

    out_s("[MAKEDRV] load path=/lib/make.bin hdr=");
    out_dec((u64)got);
    out_s(" segs=");
    out_dec(segs);
    out_s(" span=");
    out_dec(span);
    out_s(" entry=");
    out_hex(eh->e_entry);
    out_eol();

    /* 本进程第一次 mmap：内核 bump 分配器的起点就是 USER64_MMAP_VA64（= make 的链接基址） */
    const i64 va = sc6(NR_MMAP, 0, (i64)span, 3 /*R|W*/, 0x22 /*MAP_PRIVATE|MAP_ANONYMOUS*/, -1, 0);
    if (va < 0) die("mmap");
    out_s("[MAKEDRV] mmap va=");
    out_hex((u64)va);
    out_s(" len=");
    out_dec(span);
    out_s(" expected=");
    out_hex(U64_MMAP_VA64);
    out_eol();
    if ((u64)va != U64_MMAP_VA64) die("mmap-va-mismatch");

    /* 逐段搬：p_offset -> p_vaddr（分块读，内核单次 read 上限 4096） */
    static unsigned char chunk[4096];
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        u64 off = 0;
        if (sc3(NR_LSEEK, fd, (i64)ph[i].p_offset, 0) < 0) die("lseek");
        while (off < ph[i].p_filesz) {
            u64 want = ph[i].p_filesz - off;
            if (want > sizeof(chunk)) want = sizeof(chunk);
            const i64 n = sc3(NR_READ, fd, (i64)chunk, (i64)want);
            if (n <= 0) die("read-seg");
            unsigned char* dst = (unsigned char*)(ph[i].p_vaddr + off);
            for (i64 k = 0; k < n; k++) dst[k] = chunk[k];
            off += (u64)n;
        }
        /* p_memsz > p_filesz 的尾部（.bss）：内核 mmap 的页是零页，这里再显式清一遍 */
        for (u64 k = ph[i].p_filesz; k < ph[i].p_memsz; k++)
            ((unsigned char*)(ph[i].p_vaddr + k))[0] = 0;
    }
    (void)sc1(NR_CLOSE, fd);

    /* ★ 必须 mprotect：内核的 mmap(9) 把新页映射成 P|U|W|**NX**，而我们要 jmp 进去执行代码
     *   —— 直接跳就是 ring3 取指页错误（tcc 驱动实测过）。mprotect(10) 带 PROT_EXEC(4) 会清 NX
     *   （proc64_mprotect64）。整段一起给 RWX：make 的 .text/.rodata/.data/.bss 都在这一段里。 */
    const i64 mrc = sc3(NR_MPROTECT, (i64)U64_MMAP_VA64, (i64)span, 7 /*R|W|X*/);
    out_s("[MAKEDRV] mprotect va=");
    out_hex(U64_MMAP_VA64);
    out_s(" len=");
    out_dec(span);
    out_s(" prot=7 rc=");
    out_dec((u64)(mrc < 0 ? (i64)(-mrc) : mrc));
    out_eol();
    if (mrc < 0) die("mprotect");

    /* ★ 给 make 换一块更大的栈（内核给的 16 KiB 不够：musl 的 vfprintf 与 make 的递归展开
     *   都可能掉到栈底之下 -> ring3 #PF）。这里在**映像之后**再 mmap 一块栈（顺序不能反：
     *   映像必须落在固定的 4GiB+0x90000），把内核压在初始栈上的那一小段（argc/argv/envp/auxv
     *   + 参数字符串）原样搬到新栈顶。 */
    const i64 stk = sc6(NR_MMAP, 0, (i64)DRV_STACK_BYTES, 3, 0x22, -1, 0);
    if (stk < 0) die("mmap-stack");
    {
        const u64 old_top = U64_STACK_VA64 + 0x4000ULL;          /* 内核给的用户栈顶 */
        const u64 len = old_top - (u64)sp;
        const u64 new_sp = (u64)stk + DRV_STACK_BYTES - len;
        unsigned char* d = (unsigned char*)new_sp;
        const unsigned char* s = (const unsigned char*)sp;
        for (u64 i = 0; i < len; i++) d[i] = s[i];
        sp = (u64*)new_sp;                                       /* 之后一切都在新栈上 */
        out_s("[MAKEDRV] stack mmap va=");
        out_hex((u64)stk);
        out_s(" size=");
        out_dec(DRV_STACK_BYTES);
        out_s(" copied=");
        out_dec(len);
        out_s(" new_rsp=");
        out_hex(new_sp);
        out_eol();
    }

    /* auxv 改写成 make 自己的（musl 的 static_init_tls 要按 AT_PHDR 遍历程序头表） */
    u64* a = auxv_of(sp);
    u64 phdr_va = 0;
    for (unsigned i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type != PT_LOAD) continue;
        if (eh->e_phoff >= ph[i].p_offset && eh->e_phoff < ph[i].p_offset + ph[i].p_filesz)
            phdr_va = ph[i].p_vaddr + (eh->e_phoff - ph[i].p_offset);
        break;
    }
    if (!phdr_va) die("phdr-va");
    for (; a[0] != 0; a += 2) {
        if (a[0] == AT_PHDR)  a[1] = phdr_va;
        else if (a[0] == AT_PHNUM) a[1] = eh->e_phnum;
        else if (a[0] == AT_PHENT) a[1] = ELF64_PHDR_SIZE;
        else if (a[0] == AT_ENTRY) a[1] = eh->e_entry;
        else if (a[0] == AT_BASE)  a[1] = 0;
    }
    out_s("[MAKEDRV] auxv phdr=");
    out_hex(phdr_va);
    out_s(" phnum=");
    out_dec(eh->e_phnum);
    out_s(" entry=");
    out_hex(eh->e_entry);
    out_eol();

    out_s("[MAKEDRV] jmp entry=");
    out_hex(eh->e_entry);
    out_s(" rsp=");
    out_hex((u64)sp);
    out_eol();
    jump_to(sp, eh->e_entry);
}
