/* peload_crt.c - ★ v0.9.45（W2）：**Windows 运行期桩**（TEB/PEB + kernel32 子集 + 老式 msvcrt 子集）
 *
 * ============================ 这一版要证明什么 ============================
 * W1 能跳进"没有任何导入"的 PE。W2 要让**带 CRT 的真 exe** 跑起来：
 *     run /bin/peload /pexcorpus/cui.exe
 *     -> [PELOAD] ok path=/pexcorpus/cui.exe entry_ret=0 …
 *     并且终端里真的出现 "hello from a real Windows console exe"
 *
 * 也就是说：**msvcrt 的 printf 在我们自己的桩上，把字符串写进了 VimtuOS 的终端**。
 *
 * ============================ 三块地基本身 ============================
 * 1) **TEB/PEB + GS.base**：Windows 的线程环境块是经 `%gs:0x30`（TEB 自身）/ `%gs:0x60`（PEB）
 *    访问的。Linux 生态用 FS.base（158/ARCH_SET_FS），Windows 生态用 **GS.base**。
 *    本内核不用 swapgs、没有 per-CPU gs（见 kernel/syscall_entry64.asm 文件头），
 *    所以 IA32_GS_BASE(0xC0000101) 是空闲的 → 新增私有 syscall **1037/1038**（set/get）。
 *    探针自证：`gsprobe.exe` 读 `gs:[0x60]`→PEB→+0x10 的 ImageBaseAddress，
 *    检查它指向的确实是**本 exe 自己的 PE 头（MZ）**且 entry 对得上 —— 全对才返回 42。
 *
 * 2) **Win64 调用约定 vs SysV**：见 peload_stubs.S 文件头。桩表里存的是**汇编跳板**地址。
 *
 * 3) **数据导入 vs 函数导入**：msvcrt 的 `_commode` / `_fmode` / `_acmdln` / `__initenv` 是
 *    **变量**，IAT 槽里必须放"变量地址"，不能放函数地址。桩表的 `is_data` 就是干这个的
 *    （W1 的桩表没有这个字段，因为 W1 只有函数导入）。
 *
 * ============================ 如实边界（W2 明确不做的） ============================
 * * SEH（`__C_specific_handler` 的真展开）、TLS 回调（`.tls` 段的 DllMain 式回调）、
 *   资源、延迟导入、DLL 加载（`LoadLibraryA` 只认三个名字并返回假句柄）、
 *   `user32`/`gdi32`（W3 的活）。
 * * `printf` 家族只实现**老式 msvcrt 的 vfprintf/fprintf/puts/fflush**；
 *   格式化支持 `%s %c %d %i %u %x %X %p %%` + 标志 `-`/`0` + 十进制宽度；
 *   其余格式符**原样输出并把次数记进 crt unsupported**（不假装格式化过）。
 * * `malloc` 是**只增不减的 bump 分配器**（无 free/realloc —— 本 exe 的导入表里没有它们）。
 * * `Sleep` 走 `nanosleep`；内核对它没有实现时就是一次 ENOSYS（如实，不假装睡过）。
 */

#include "peload.h"

/* ==================== 系统调用 ==================== */
#define NR_READ     0
#define NR_WRITE    1
#define NR_MMAP     9
#define NR_MPROTECT 10
#define NR_GETPID   39
#define NR_NANOSLEEP 35
#define NR_EXITG    231

#define PROT_R  1
#define PROT_W  2
#define PROT_X  4
#define MAP_PRIV_ANON 0x22

static inline i64 sc1(long nr, i64 a1) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1) : "rcx", "r11", "memory");
    return r;
}
static inline i64 sc2(long nr, i64 a1, i64 a2) {
    i64 r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
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
    const i64 r = sc6(NR_MMAP, 0, (i64)len, (i64)(PROT_R | PROT_W), (i64)MAP_PRIV_ANON, -1, 0);
    return (r < 0) ? 0 : (void *)(u64)r;
}
static void x_exit(int code) { (void)sc1(NR_EXITG, (i64)code); for (;;) { } }

/* ==================== 小工具 ==================== */
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void bzero_(void *d, u32 n) { u8 *p = (u8 *)d; for (u32 i = 0; i < n; i++) p[i] = 0; }
static void bcopy_(void *d, const void *s, u32 n) {
    u8 *p = (u8 *)d; const u8 *q = (const u8 *)s;
    for (u32 i = 0; i < n; i++) p[i] = q[i];
}
static void w64(void *p, u64 v) { u8 *b = (u8 *)p; for (int i = 0; i < 8; i++) { b[i] = (u8)(v & 0xFF); v >>= 8; } }
static void w32(void *p, u32 v) { u8 *b = (u8 *)p; for (int i = 0; i < 4; i++) { b[i] = (u8)(v & 0xFF); v >>= 8; } }
static void w16(void *p, u16 v) { u8 *b = (u8 *)p; b[0] = (u8)(v & 0xFF); b[1] = (u8)(v >> 8); }
static int ci_eq(const char *a, const char *b) {
    for (;;) {
        char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
        if (!x) return 1;
    }
}

/* ==================== 状态 ==================== */
static u64 g_image_base;      /* 本次装载的映像基址（填 PEB->ImageBaseAddress） */
static u32 g_image_size;
static u8 *g_teb;             /* TEB（0x400 B，GS.base 指向它） */
static u8 *g_peb;             /* PEB（0x400 B，TEB+0x60 指向它） */
static char g_exe_path[256];
static u32 g_unsupported_fmt; /* 不认识的格式符计数（如实统计） */

/* ==================== 1) TEB / PEB ====================
 * 布局严格按 Windows x64（这是 CRT 与 SEH 唯一认的偏移，不能自定义）：
 *   TEB 0x00 NT_TIB：ExceptionList / StackBase / StackLimit / SubSystemTib /
 *                    FiberData / ArbitraryUserPointer / **Self@0x30**
 *   TEB 0x40 ClientId.UniqueProcess / 0x48 UniqueThread
 *   TEB 0x58 ThreadLocalStoragePointer
 *   TEB **0x60 ProcessEnvironmentBlock**  ← `%gs:0x60` 拿 PEB
 *   PEB 0x02 BeingDebugged / **0x10 ImageBaseAddress** / 0x18 Ldr / 0x20 ProcessParameters
 *   PEB 0x118 OS 版本三项（有些 CRT 会读）
 * StackBase/StackLimit 是**近似值**（本内核没有把用户栈边界告诉 ring3）：
 *   取当前 rsp 所在页上界为 StackBase、再减 64 KiB 为 StackLimit —— 与 ELF 装载器的
 *   64 KiB 用户栈一致。它只影响 SEH 的"栈展开到哪"，不影响本批判据。
 */
static int build_teb_peb(void) {
    g_teb = (u8 *)x_mmap(0x400);
    g_peb = (u8 *)x_mmap(0x400);
    if (!g_teb || !g_peb) return 0;
    bzero_(g_teb, 0x400);
    bzero_(g_peb, 0x400);

    u64 sp_now = 0;
    __asm__ volatile("movq %%rsp, %0" : "=r"(sp_now));
    const u64 stack_base  = (sp_now + 0xFFFULL) & ~0xFFFULL;
    const u64 stack_limit = stack_base - 0x10000ULL;
    const i64 pid = sc1(NR_GETPID, 0);
    const u64 pidv = (pid > 0) ? (u64)pid : 1u;

    w64(g_teb + 0x00, 0);                 /* ExceptionList */
    w64(g_teb + 0x08, stack_base);        /* StackBase */
    w64(g_teb + 0x10, stack_limit);       /* StackLimit */
    w64(g_teb + 0x18, 0);                 /* SubSystemTib */
    w64(g_teb + 0x20, 0);                 /* FiberData / Version */
    w64(g_teb + 0x28, 0);                 /* ArbitraryUserPointer */
    w64(g_teb + 0x30, (u64)(u64)g_teb);   /* ★ Self */
    w64(g_teb + 0x38, 0);                 /* EnvironmentPointer */
    w64(g_teb + 0x40, pidv);              /* ClientId.UniqueProcess */
    w64(g_teb + 0x48, pidv);              /* ClientId.UniqueThread */
    w64(g_teb + 0x50, 0);                 /* ActiveRpcHandle */
    w64(g_teb + 0x58, 0);                 /* ThreadLocalStoragePointer */
    w64(g_teb + 0x60, (u64)(u64)g_peb);   /* ★ PEB */

    w32(g_peb + 0x00, 0);                 /* InheritedAddressSpace / ReadImageFileExecOptions */
    w32(g_peb + 0x02, 0);                 /* BeingDebugged = 0 */
    w64(g_peb + 0x08, 0);                 /* Mutant */
    w64(g_peb + 0x10, g_image_base);      /* ★ ImageBaseAddress */
    w64(g_peb + 0x18, 0);                 /* Ldr（W2 没有真 loader data） */
    w64(g_peb + 0x20, 0);                 /* ProcessParameters */
    w64(g_peb + 0x30, 0);                 /* ProcessHeap */
    w32(g_peb + 0x118, 6);                /* OSMajorVersion */
    w32(g_peb + 0x11C, 1);                /* OSMinorVersion */
    w16(g_peb + 0x120, 7601);             /* OSBuildNumber */
    return 1;
}

/* ==================== 2) mini-CRT 的堆（bump 分配器） ====================
 * 为什么不做 free：本批的导入面里**没有** free/realloc（cui/gui 的 union 实测），
 * 所以"能分配"就够；做一套真堆只会引入自己的 bug。上限 1 MiB，用尽时如实返回 0。
 * 注意：如果将来某个 exe 的导入面里有 free，这里必须升级 —— 桩表会让它 MISSING 而不是静默错。 */
#define HEAP_BYTES (1024u * 1024u)
static u8 *g_heap;
static u32 g_heap_used;
static u32 g_heap_cap;

static void *crt_alloc(u32 n) {
    if (!g_heap) {
        g_heap = (u8 *)x_mmap(HEAP_BYTES);
        if (!g_heap) return 0;
        g_heap_cap = HEAP_BYTES;
        g_heap_used = 0;
    }
    const u32 a = (n + 15u) & ~15u;
    if (!a || g_heap_used + a > g_heap_cap) return 0;
    void *p = g_heap + g_heap_used;
    g_heap_used += a;
    return p;
}

/* ==================== 3) FILE 模型（msvcrt 的 FILE 是 32 字节，必须对齐这个步长） ====================
 * ★ 为什么步长要 32：mingw 那边 `stdout` 是 `(&__iob_func()[1])` —— 它按 **msvcrt 的 sizeof(FILE)**
 *   做指针算术。如果我们的数组步长不是 32，`__iob_func()[1]` 就落到我们自己数组的中间，
 *   于是"往 stdout 写"变成"往别的槽写"。本批只有我们自己解释这 32 字节，所以内部布局随便，
 *   但**步长必须是 32**。 */
typedef struct { i32 fd; i32 mode; u8 pad[24]; } PlFile;
static PlFile g_iob[3];

static int fd_of_stream(u64 stream) {
    if (!stream) return 1;
    PlFile *f = (PlFile *)(u64)stream;
    if (f->fd < 0 || f->fd > 2) return 1;
    return f->fd;
}

/* ==================== 4) Win64 va_list 遍历 ====================
 * x64 Windows 的 va_list 是个**结构体指针**：{ gp_offset, fp_offset, overflow_arg_area,
 * reg_save_area }。整数/指针参数在 reg_save_area 的 gp_offset 处（前 4 个），用完转到
 * overflow_arg_area（栈上）。**本文件只处理整数/指针类**（%s %c %d %u %x %p）。 */
typedef struct { u32 gp_off; u32 fp_off; u64 overflow; u64 reg_save; } PlVa;

static u64 va_gp(PlVa *va) {
    u64 v;
    if (va->gp_off < 48u) { v = *(const u64 *)(u64)(va->reg_save + va->gp_off); va->gp_off += 8u; }
    else { v = *(const u64 *)va->overflow; va->overflow += 8u; }
    return v;
}

/* ==================== 5) 格式化引擎（老式 msvcrt 的 vfprintf 那一条路） ====================
 * 支持：`%%`、`%s`、`%c`、`%d/%i`（含负数）、`%u`、`%x/%X`、`%p`，
 *       外加标志 `-`（左对齐）/`0`（补零）与十进制宽度。
 * 不支持的格式符：**原样输出**（连同 % 与标志），并把 g_unsupported_fmt 加一 ——
 *   这样"看起来打印了"不会掩盖"其实没格式化"。 */
static void emit_pad(int fd, char c, i32 n) { for (i32 i = 0; i < n; i++) pl_out(fd, &c, 1); }

static i32 fmt_uint(char *t, u64 v, u32 base, int upper) {
    const char *D = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    i32 n = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = D[v % base]; v /= base; }
    for (i32 i = 0; i < n; i++) t[i] = tmp[n - 1 - i];
    return n;
}

static i32 fmt_core(int fd, const char *fmt, PlVa *va) {
    i32 total = 0;
    for (u32 i = 0; fmt[i]; i++) {
        if (fmt[i] != '%') { pl_out(fd, fmt + i, 1); total++; continue; }
        u32 j = i + 1;
        char left = 0, zero = 0;
        while (fmt[j] == '-' || fmt[j] == '0') { if (fmt[j] == '-') left = 1; else zero = 1; j++; }
        i32 width = 0;
        while (fmt[j] >= '0' && fmt[j] <= '9') { width = width * 10 + (fmt[j] - '0'); j++; }
        if (fmt[j] == 'l' || fmt[j] == 'h' || fmt[j] == 'z') {          /* 长度修饰：本批按 64 位整数处理 */
            j++;
            if (fmt[j] == 'l') j++;
        }
        const char conv = fmt[j];
        if (!conv) break;
        char buf[72];
        i32 n = 0;
        buf[0] = 0;
        switch (conv) {
        case '%': buf[n++] = '%'; break;
        case 'c': buf[n++] = (char)(va_gp(va) & 0xFF); break;
        case 's': {
            const char *s = (const char *)(u64)va_gp(va);
            if (!s) s = "(null)";
            i32 pad;
            i32 len = (i32)slen(s);
            pad = (width > len) ? width - len : 0;
            if (!left) emit_pad(fd, ' ', pad);
            pl_outs(fd, s);
            if (left) emit_pad(fd, ' ', pad);
            total += len + pad;
            i = j;
            continue;
        }
        case 'd': case 'i': {
            i64 v = (i64)va_gp(va);
            u64 m;
            if (v < 0) { buf[n++] = '-'; m = (u64)(-v); } else m = (u64)v;
            n += fmt_uint(buf + n, m, 10u, 0);
            break;
        }
        case 'u': n += fmt_uint(buf, va_gp(va), 10u, 0); break;
        case 'x': n += fmt_uint(buf, va_gp(va), 16u, 0); break;
        case 'X': n += fmt_uint(buf, va_gp(va), 16u, 1); break;
        case 'p': {
            u64 v = va_gp(va);
            buf[n++] = '0';
            buf[n++] = 'x';
            n += fmt_uint(buf + n, v, 16u, 0);
            break;
        }
        default: {
            /* 不认识的转换：**原样输出**整个 spec，并计数。绝不假装格式化过。 */
            g_unsupported_fmt++;
            for (u32 k = i; k <= j; k++) { pl_out(fd, fmt + k, 1); total++; }
            i = j;
            continue;
        }
        }
        const i32 pad = (width > n) ? width - n : 0;
        if (!left) emit_pad(fd, zero ? '0' : ' ', pad);
        for (i32 k = 0; k < n; k++) pl_out(fd, buf + k, 1);
        if (left) emit_pad(fd, ' ', pad);
        total += n + pad;
        i = j;
    }
    pl_flush();          /* 最后可能没有 '\n'（不吞输出）：flush 后内核的 [SYSCALL] 行才在它之后 */
    return total;
}

/* ==================== 6) atexit 链 ==================== */
#define PL_ATEXIT_MAX 32
static void (*g_atexit[PL_ATEXIT_MAX])(void);
static u32 g_atexit_n;
static int g_exit_chain_ran;

void pl_crt_run_exit_chain(void) {
    if (g_exit_chain_ran) return;
    g_exit_chain_ran = 1;
    while (g_atexit_n) {                       /* LIFO，与 C 标准一致 */
        void (*f)(void) = g_atexit[--g_atexit_n];
        if (f) f();
    }
}

/* ==================== 7) 桩实现（被 peload_stubs.S 以 SysV 约定调用） ====================
 * 注意：这些函数是**SysV**（参数 rdi/rsi/rdx/rcx）。Win64 的 rcx/rdx/r8/r9 已经由跳板搬过来了。
 * 参数个数不足时多出来的形参是垃圾值 —— 只要实现不读它就行。 */

/* ---- kernel32 ---- */
void pl_do_ExitProcess(u64 code) {
    pl_outs(1, "[PELOAD] exit-stub code=");
    pl_dec(1, code);
    pl_out(1, "\n", 1);
    pl_crt_run_exit_chain();
    x_exit((int)(code & 0xFFu));
}
void *pl_do_GetProcAddress(void *h, const char *name) {
    (void)h;
    if (!name) return 0;
    for (u32 i = 0; i < g_pl_stubs.n; i++)
        if (!g_pl_stubs.tab[i].is_data && ci_eq(g_pl_stubs.tab[i].name, name))
            return g_pl_stubs.tab[i].addr;
    return 0;
}
void *pl_do_LoadLibraryA(const char *name) {
    static const char *const known[] = { "kernel32.dll", "msvcrt.dll", "user32.dll" };
    if (!name) return 0;
    for (u32 i = 0; i < 3u; i++) if (ci_eq(known[i], name)) return (void *)(u64)(0x1000u + i);
    return 0;
}
u64 pl_do_GetLastError(void) { return 0; }                       /* 本批没有错误跟踪：恒 = ERROR_SUCCESS(0) */
u64 pl_do_SetUnhandledExceptionFilter(u64 f) { (void)f; return 0; }
u64 pl_do_Sleep(u64 ms) {
    if (ms) {
        /* nanosleep(35) 的 timespec = { tv_sec, tv_nsec }；内核没实现时就是一次 ENOSYS（如实）。 */
        u64 ts[2];
        ts[0] = ms / 1000u;
        ts[1] = (ms % 1000u) * 1000000ULL;
        (void)sc2(NR_NANOSLEEP, (i64)(u64)ts, 0);
    }
    return 0;
}
u64 pl_do_VirtualProtect(u64 addr, u64 len, u64 newprot, u64 oldptr) {
    /* ★ 必须真改页权限：mingw 的 _pei386_runtime_relocator 会用 VirtualProtect 把 .rdata
     *   临时改成可写、写完再改回去。若这里假装成功，写 .rdata 会直接 #PF。 */
    const u64 np = newprot & 0xFFu;
    u64 prot = 0;
    if (np != 0x01u) {                                   /* PAGE_NOACCESS 之外都可读 */
        prot |= PROT_R;
        if (np & 0x04u) prot |= PROT_W;                   /* PAGE_READWRITE / WRITECOPY */
        if (np & 0x40u) prot |= PROT_W;                   /* PAGE_EXECUTE_READWRITE */
        if (np & 0x10u) prot |= PROT_X;                   /* EXECUTE 家族 */
        if (np & 0x20u) prot |= PROT_X;                   /* EXECUTE_READ */
        if (np & 0x80u) { prot |= PROT_X | PROT_W; }      /* EXECUTE_WRITECOPY */
    }
    if (oldptr) *(u32 *)(u64)oldptr = 0x04u;              /* 旧的当 PAGE_READWRITE 报回去 */
    if (sc3(NR_MPROTECT, (i64)addr, (i64)len, (i64)prot) != 0) return 0;
    return 1;
}
u64 pl_do_VirtualQuery(u64 addr, u64 buf, u64 buflen) {
    if (!buf || buflen < 48u) return 0;
    u8 *p = (u8 *)(u64)buf;
    bzero_(p, 48);
    const u64 b = addr & ~0xFFFULL;
    w64(p + 0x00, b);            /* BaseAddress */
    w64(p + 0x08, b);            /* AllocationBase */
    w32(p + 0x10, 0x04u);        /* AllocationProtect = PAGE_READWRITE */
    w64(p + 0x18, 0x1000u);      /* RegionSize */
    w32(p + 0x20, 0x1000u);      /* State = MEM_COMMIT */
    w32(p + 0x24, 0x04u);        /* Protect = PAGE_READWRITE */
    w32(p + 0x28, 0x20000u);     /* Type = MEM_PRIVATE */
    return 48u;                  /* 返回写入的字节数（Windows 同） */
}
u64 pl_do_GetStartupInfoA(u64 p_si) {
    if (p_si) {
        bzero_((void *)(u64)p_si, 104u);
        w32((void *)(u64)p_si, 104u);       /* cb = sizeof(STARTUPINFOA) x64 */
    }
    return 0;
}
u64 pl_do_IsDBCSLeadByte(u64 c) { (void)c; return 0; }

/* ---- msvcrt：进程启动 ---- */
u64 pl_do_set_app_type(u64 t) { (void)t; return 0; }
u64 pl_do_setusermatherr(u64 f) { (void)f; return 0; }
u64 pl_do_XcptFilter(u64 code, u64 ptr) { (void)code; (void)ptr; return 0; }   /* EXCEPTION_CONTINUE_SEARCH */

/* __getmainargs(int *argc, char ***argv, char ***envp, int dowild, STARTUPINFO *si)
 * ★ 5 个参数：第 5 个在**栈上**（Win64：4 个寄存器 + 影子空间），所以它有专用汇编跳板
 *   （见 peload_stubs.S 的 pl_stub___getmainargs）。 */
static char  g_argv0[256];
static char *g_argv[2];
u64 pl_do_getmainargs(u64 p_argc, u64 p_argv, u64 p_env, u64 dowild, u64 p_si) {
    (void)dowild;
    u32 n = slen(g_exe_path);
    if (n > 255u) n = 255u;
    for (u32 i = 0; i < n; i++) g_argv0[i] = g_exe_path[i];
    g_argv0[n] = 0;
    g_argv[0] = g_argv0;
    g_argv[1] = 0;
    if (p_argc) *(u32 *)(u64)p_argc = 1;
    if (p_argv) *(u64 *)(u64)p_argv = (u64)(u64)g_argv;
    if (p_env)  *(u64 *)(u64)p_env = 0;                    /* 没有环境变量（如实） */
    if (p_si) { bzero_((void *)p_si, 104u); w32((void *)p_si, 104u); }
    return 0;
}

/* _initterm(start, end)：逐个调 8 字节函数指针，这是 CRT 的静态构造链 */
u64 pl_do_initterm(u64 start, u64 end) {
    u32 called = 0;
    for (u64 p = start; p + 8u <= end; p += 8u) {
        const u64 f = *(const u64 *)(u64)p;
        if (f) { ((void (*)(void))(u64)f)(); called++; }
    }
    return called;
}
u64 pl_do_atexit(u64 f) {
    if (g_atexit_n >= PL_ATEXIT_MAX) return 1;
    g_atexit[g_atexit_n++] = (void (*)(void))(u64)f;
    return 0;
}
u64 pl_do_cexit(void) { pl_crt_run_exit_chain(); pl_flush(); return 0; }
void pl_do_exit(u64 code) { pl_crt_run_exit_chain(); pl_flush(); x_exit((int)(code & 0xFFu)); }
void pl_do__exit(u64 code) { pl_flush(); x_exit((int)(code & 0xFFu)); }
void pl_do_abort(void) {
    pl_outs(2, "[PELOAD] crt abort\n");
    pl_flush();
    x_exit(3);
}
void pl_do_amsg_exit(u64 code) {
    pl_outs(2, "[PELOAD] crt runtime-error code=");
    pl_dec(2, code);
    pl_out(2, "\n", 1);
    pl_flush();
    x_exit(255);
}

/* ---- msvcrt：流与格式化 ---- */
u64 pl_do_iob_func(void) { return (u64)(u64)g_iob; }
u64 pl_do_fflush(u64 stream) { (void)stream; pl_flush(); return 0; }
u64 pl_do_setvbuf(u64 stream, u64 buf, u64 mode, u64 size) {
    (void)stream; (void)buf; (void)mode; (void)size;
    return 0;                                              /* 我们的输出本来就是行缓冲 */
}
u64 pl_do_vfprintf(u64 stream, u64 fmt, u64 va) {
    if (!fmt || !va) return (u64)-1;
    return (u64)(i64)fmt_core(fd_of_stream(stream), (const char *)(u64)fmt, (PlVa *)(u64)va);
}
u64 pl_do_fprintf(u64 stream, u64 fmt, u64 va) {           /* 专用跳板负责把可变参数打包成 va_list */
    return pl_do_vfprintf(stream, fmt, va);
}
u64 pl_do_puts(u64 s) {
    if (!s) return (u64)-1;
    pl_outs(1, (const char *)(u64)s);
    pl_out(1, "\n", 1);
    pl_flush();
    return 0;
}

/* ---- msvcrt：内存与字符串 ---- */
u64 pl_do_malloc(u64 n) { return (u64)(u64)crt_alloc((u32)n); }
u64 pl_do_memcpy(u64 d, u64 s, u64 n) {
    if (d && s && n) bcopy_((void *)(u64)d, (const void *)(u64)s, (u32)n);
    return d;
}
u64 pl_do_strlen(u64 s) { return s ? (u64)slen((const char *)(u64)s) : 0; }

/* ==================== 8) 桩表 ====================
 * 表里存的是 **peload_stubs.S 的汇编跳板地址**（Win64 -> SysV）。数据导入直接给变量地址。 */
extern void pl_stub_ExitProcess(void);
extern void pl_stub_GetProcAddress(void);
extern void pl_stub_LoadLibraryA(void);
extern void pl_stub_GetLastError(void);
extern void pl_stub_SetUnhandledExceptionFilter(void);
extern void pl_stub_Sleep(void);
extern void pl_stub_VirtualProtect(void);
extern void pl_stub_VirtualQuery(void);
extern void pl_stub_GetStartupInfoA(void);
extern void pl_stub_IsDBCSLeadByte(void);
extern void pl_stub__XcptFilter(void);
extern void pl_stub___getmainargs(void);
extern void pl_stub___iob_func(void);
extern void pl_stub___set_app_type(void);
extern void pl_stub___setusermatherr(void);
extern void pl_stub__amsg_exit(void);
extern void pl_stub__cexit(void);
extern void pl_stub__exit(void);
extern void pl_stub__initterm(void);
extern void pl_stub_atexit(void);
extern void pl_stub_abort(void);
extern void pl_stub_exit(void);
extern void pl_stub_fflush(void);
extern void pl_stub_fprintf(void);
extern void pl_stub_malloc(void);
extern void pl_stub_memcpy(void);
extern void pl_stub_puts(void);
extern void pl_stub_setvbuf(void);
extern void pl_stub_strlen(void);
extern void pl_stub_vfprintf(void);

/* 数据导入的实体（PE 侧 `mov rax,[__imp_x]` 拿到的就是这里的地址） */
static char **g_initenv_var;          /* __initenv：char***  */
static i32    g_commode_var;          /* _commode：int       */
static i32    g_fmode_var;            /* _fmode：int         */
static char  *g_acmdln_var;           /* _acmdln：char*      */

static const PlStubEntry g_stub_tab[] = {
    /* ---- kernel32（W0 实测：这批图形 exe 只用 A 版，只要 5~7 个） ---- */
    { "kernel32.dll", "ExitProcess",                (void *)pl_stub_ExitProcess,  0 },
    { "kernel32.dll", "GetProcAddress",             (void *)pl_stub_GetProcAddress, 0 },
    { "kernel32.dll", "LoadLibraryA",               (void *)pl_stub_LoadLibraryA, 0 },
    { "kernel32.dll", "GetLastError",               (void *)pl_stub_GetLastError, 0 },
    { "kernel32.dll", "SetUnhandledExceptionFilter",(void *)pl_stub_SetUnhandledExceptionFilter, 0 },
    { "kernel32.dll", "Sleep",                      (void *)pl_stub_Sleep,        0 },
    { "kernel32.dll", "VirtualProtect",             (void *)pl_stub_VirtualProtect, 0 },
    { "kernel32.dll", "VirtualQuery",               (void *)pl_stub_VirtualQuery, 0 },
    { "kernel32.dll", "GetStartupInfoA",            (void *)pl_stub_GetStartupInfoA, 0 },
    { "kernel32.dll", "IsDBCSLeadByte",             (void *)pl_stub_IsDBCSLeadByte, 0 },

    /* ---- msvcrt（★ 老式 1998 版 ABI，不是 UCRT：__getmainargs/_initterm/_iob_func 这套） ---- */
    { "msvcrt.dll", "_XcptFilter",       (void *)pl_stub__XcptFilter,       0 },
    { "msvcrt.dll", "__getmainargs",     (void *)pl_stub___getmainargs,     0 },
    { "msvcrt.dll", "__iob_func",        (void *)pl_stub___iob_func,        0 },
    { "msvcrt.dll", "__set_app_type",    (void *)pl_stub___set_app_type,    0 },
    { "msvcrt.dll", "__setusermatherr",  (void *)pl_stub___setusermatherr,  0 },
    { "msvcrt.dll", "_amsg_exit",        (void *)pl_stub__amsg_exit,        0 },
    { "msvcrt.dll", "_cexit",            (void *)pl_stub__cexit,            0 },
    { "msvcrt.dll", "_exit",             (void *)pl_stub__exit,             0 },
    { "msvcrt.dll", "_initterm",         (void *)pl_stub__initterm,         0 },
    { "msvcrt.dll", "atexit",            (void *)pl_stub_atexit,            0 },
    { "msvcrt.dll", "abort",             (void *)pl_stub_abort,             0 },
    { "msvcrt.dll", "exit",              (void *)pl_stub_exit,              0 },
    { "msvcrt.dll", "fflush",            (void *)pl_stub_fflush,            0 },
    { "msvcrt.dll", "fprintf",           (void *)pl_stub_fprintf,           0 },
    { "msvcrt.dll", "malloc",            (void *)pl_stub_malloc,            0 },
    { "msvcrt.dll", "memcpy",            (void *)pl_stub_memcpy,            0 },
    { "msvcrt.dll", "puts",              (void *)pl_stub_puts,              0 },
    { "msvcrt.dll", "setvbuf",           (void *)pl_stub_setvbuf,           0 },
    { "msvcrt.dll", "strlen",            (void *)pl_stub_strlen,            0 },
    { "msvcrt.dll", "vfprintf",          (void *)pl_stub_vfprintf,          0 },

    /* ---- msvcrt：★ 数据导入（IAT 里放的是**变量地址**，不是函数） ---- */
    { "msvcrt.dll", "__initenv",         (void *)&g_initenv_var,            1 },
    { "msvcrt.dll", "_commode",          (void *)&g_commode_var,            1 },
    { "msvcrt.dll", "_fmode",            (void *)&g_fmode_var,              1 },
    { "msvcrt.dll", "_acmdln",           (void *)&g_acmdln_var,             1 },
};
const PlStubTable g_pl_stubs = { g_stub_tab, (u32)(sizeof(g_stub_tab) / sizeof(g_stub_tab[0])) };

/* ==================== 9) 入口：建 TEB/PEB + 设 GS.base ==================== */
int pl_crt_setup(u64 image_base, u32 image_size, const char *exe_path) {
    g_image_base = image_base;
    g_image_size = image_size;
    if (exe_path) {
        u32 n = slen(exe_path);
        if (n > 255u) n = 255u;
        for (u32 i = 0; i < n; i++) g_exe_path[i] = exe_path[i];
        g_exe_path[n] = 0;
    }
    g_acmdln_var = g_exe_path;
    g_fmode_var = 0;          /* _O_TEXT */
    g_commode_var = 0;

    for (u32 i = 0; i < 3u; i++) {
        g_iob[i].fd = (i32)i;
        g_iob[i].mode = 0;
        bzero_(g_iob[i].pad, 24);
    }

    if (!build_teb_peb()) {
        pl_outs(2, "[PELOAD] crt FAILED teb/peb mmap\n");
        return 0;
    }
    const i64 rc = sc1(PL_NR_SET_GS, (i64)(u64)g_teb);
    u64 rb = 0;
    const i64 rc2 = sc1(PL_NR_GET_GS, (i64)(u64)&rb);

    pl_outs(1, "[PELOAD] teb base="); pl_hex(1, (u64)(u64)g_teb);
    pl_outs(1, " self=");            pl_hex(1, *(u64 *)(g_teb + 0x30));
    pl_outs(1, " peb=");             pl_hex(1, (u64)(u64)g_peb);
    pl_outs(1, " imagebase=");       pl_hex(1, image_base);
    pl_outs(1, " gs_set_rc=");       pl_dec(1, (u64)rc);
    pl_outs(1, " gs_read_rc=");      pl_dec(1, (u64)rc2);
    pl_outs(1, " gs_read=");         pl_hex(1, rb);
    pl_outs(1, " stubs=");           pl_dec(1, g_pl_stubs.n);
    pl_out(1, "\n", 1);
    return (rc == 0 && rc2 == 0 && rb == (u64)(u64)g_teb) ? 1 : 0;
}
