// muslhello.c - ★ A3：**musl 静态程序**（musl 的头文件 + musl 的 libc.a）
//
// 与 user/apps/hello.c / libctest.c / fbdemo.c 的区别（别混）：
//   * 那三个用**我们自研的最小 libc**（user/lib/，自有 ABI 包装）；
//   * 本文件用 **musl 1.2.5**（third_party/musl）：include 用 musl 自己的头，
//     链接用 third_party/musl/lib/libc.a —— 这才是"移植 musl libc"的第一块真实里程碑
//     （musl 真在 VimtuOS ring3 里跑起来，而不是"能编出来"）。
//
// 为什么入口是**自写 _start**（而不是 musl 的 crt1.o）：
//   musl 的 crt1.o（crt/crt1.c + arch/x86_64/crt_arch.h）里有一条
//   `lea _DYNAMIC(%rip),%rsi`（非 PIC 的 _DYNAMIC 引用）。链接到用户窗口 4GiB 时，
//   `-static` 下 _DYNAMIC 无处解析 -> `relocation R_X86_64_PC32 out of range`（A2 实测）。
//   所以这里自己写 _start：只把 SP 交给 musl 的 __libc_start_main（SysV 初始栈：
//   argc/argv/envp/auxv 全部由内核 elf64 加载器摆好，见 kernel/elf64.cpp 的 auxv 段）。
//
// 这个程序存在的意义 = **输出可逐字节核对的证据**（验收脚本 tests/musl64_test.py）：
//   ① 固定串 write(1, …)（字节比对）
//   ② argc/argv/argv[0]（证明内核的 argc/argv 摆法被 musl 正确读到）
//   ③ malloc 真分配 + 写入 + 回读（musl mallocng -> brk/mmap/mprotect 这条链）
//   ④ clock_gettime（启动期/运行期调用不崩）
//   ⑤ getrandom（同上）
//   ⑥ errno 真的走 TLS（%fs）—— 这是 "arch_prctl(ARCH_SET_FS) 生效" 的硬证据：
//      errno 失败就会在这里 #PF，而不是"打印个 0"
//   ⑦ 退出码 7（内核 [PROC64] exit … code=7）
//
// 边界（本轮**不做**、也不要假装做了）：动态链接、pthread/TLS 全量、stdio 全量、
// locale、信号投递都不在本轮范围内 —— 见 docs/应用层与系统调用说明.md 的"musl（A3 第一步）"节。
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

// musl 的 __libc_start_main：init/fini/ldso 三个参数在当前 musl 里**没有被使用**
// （见 musl src/env/__libc_start_main.c：它自己走 __init_libc + libc_start_init），
// 所以这里传 0 即可 —— 与 crt1.o 传 _init/_fini 的差别就在这里。
extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                             void (*init_dummy)(void), void (*fini_dummy)(void),
                             void (*ldso_dummy)(void));

static int musl_app_main(int argc, char** argv, char** envp);

// ---------------------------------------------------------------- 极小的输出/格式化助手
// 不引 stdio/printf：本轮只要"字节可核对"的证据，越小越好（内核镜像里嵌的是这个 ELF）。
static size_t m_str(char* d, const char* s) {
    size_t n = 0;
    while (s[n]) { d[n] = s[n]; n++; }
    return n;
}
// width = 0 -> 自然宽度；width > 0 -> 零填充到定宽。
// ★ 为什么要定宽（本批修复）：这个程序的输出会被内核重定向进文件、再由验收脚本按**字节数**核对
//   （tests/a42a64_test.py 的 `cat /tmp/o.txt`、tests/musl64_test.py 的逐字节比对）。变宽字段
//   （malloc 指针的十六进制、clock_gettime 的 sec/nsec 十进制）会让总字节数每次运行都抖
//   （实测首跑 264 / 复跑 265），"总字节数"这类断言于是变成随机假红。定宽 = 同一份源码构建出的
//   输出**逐字节确定**，总字节数只由源码里的模板决定。
//   定宽取值都是**域宽**（不是魔法数）：指针 9 位（用户窗口恒在 0x100000000..0x1FFFFFFFF，
//   即恒 9 位十六进制）、sec 4 位（≤9999s 足够任何一次启动/演示）、nsec 9 位（纳秒域满宽）。
static size_t m_dec(char* d, uint64_t v, int width) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + (int)(v % 10u)); v /= 10u; }
    while (n < width) t[n++] = '0';
    int k = 0;
    while (n > 0) d[k++] = t[--n];
    return (size_t)k;
}
static size_t m_hex(char* d, uint64_t v, int width) {
    char t[24];
    int n = 0;
    if (v == 0) t[n++] = '0';
    while (v) {
        const unsigned r = (unsigned)(v & 0xFu);
        t[n++] = (char)(r < 10u ? ('0' + (int)r) : ('a' + (int)(r - 10u)));
        v >>= 4;
    }
    while (n < width) t[n++] = '0';
    int k = 0;
    while (n > 0) d[k++] = t[--n];
    return (size_t)k;
}
static void m_out(const char* s, size_t n) {
    const ssize_t w = write(1, s, n);
    (void)w;                                   // 只做证据输出，短写在这里不重试（如实、简单）
}

// ---------------------------------------------------------------- 启动
// musl 的 _start 口径（见 musl arch/x86_64/crt_arch.h）：rdi = 初始 SP，rsp 16 字节对齐后 call。
// 内核 elf64 加载器保证 rsp%16==0（见 kernel/elf64.cpp 的初始栈段）。
__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void) {
    __asm__ volatile(
        "xor %rbp, %rbp\n\t"
        "mov %rsp, %rdi\n\t"
        "and $-16, %rsp\n\t"
        "call musl_start_c\n\t"
        "hlt\n\t");
}

// 被 _start 的汇编 call：把初始栈按 SysV 拆成 argc/argv 交给 musl。
// 为什么单独一个函数：naked 函数体里只能写汇编，参数解引用放在普通函数里最干净。
__attribute__((used, noinline))
static void musl_start_c(uint64_t* sp) {
    const int argc = (int)sp[0];
    char** argv = (char**)(sp + 1);
    __libc_start_main(musl_app_main, argc, argv, 0, 0, 0);
    _exit(127);                                // __libc_start_main 不返回；兜底也不让它掉回 hlt
}

// ---------------------------------------------------------------- 程序主体
static int musl_app_main(int argc, char** argv, char** envp) {
    (void)envp;
    char buf[160];
    char* p;

    // ① 固定串：验收脚本按**字节**比对这一行
    {
        static const char HELLO_LINE[] = "[MUSL] hello from musl static ELF\n";
        m_out(HELLO_LINE, sizeof HELLO_LINE - 1u);
    }

    // ② argc / argv[0]：证明内核初始栈的 argc/argv 被 musl 读对了
    p = buf;
    p += m_str(p, "[MUSL] argc=");
    p += m_dec(p, (uint64_t)(argc < 0 ? 0 : argc), 0);        // 0 = 自然宽度（argc 恒 1 位，无需定宽）
    p += m_str(p, " argv0=");
    p += m_str(p, (argc > 0 && argv && argv[0]) ? argv[0] : "(null)");
    p += m_str(p, "\n");
    m_out(buf, (size_t)(p - buf));

    // ③ malloc：musl mallocng（musl 1.2.5 默认）—— 元数据走 brk + mmap(PROT_NONE) + mprotect，
    //    数据页走 mmap(ANON)。写入 + 回读都自己验一遍，失败如实打 FAILED（不假装成功）。
    {
        unsigned char* a = (unsigned char*)malloc(64);
        unsigned char* b = (unsigned char*)malloc(4096);
        int ok = (a != 0 && b != 0);
        if (ok) {
            for (unsigned i = 0; i < 64; i++) a[i] = (unsigned char)(0x11u + (i & 7u));
            for (unsigned i = 0; i < 4096; i++) b[i] = (unsigned char)((i * 7u) & 0xFFu);
            for (unsigned i = 0; i < 64; i++) if (a[i] != (unsigned char)(0x11u + (i & 7u))) ok = 0;
            for (unsigned i = 0; i < 4096; i++) if (b[i] != (unsigned char)((i * 7u) & 0xFFu)) ok = 0;
        }
        if (ok) {
            p = buf;
            p += m_str(p, "[MUSL] malloc ok bytes=64+4096 a=0x");
            p += m_hex(p, (uint64_t)(uintptr_t)a, 9);         // 定宽 9 位（用户窗口恒 9 位十六进制）
            p += m_str(p, " b=0x");
            p += m_hex(p, (uint64_t)(uintptr_t)b, 9);
            p += m_str(p, "\n");
            m_out(buf, (size_t)(p - buf));
        } else {
            static const char MALLOC_FAIL[] = "[MUSL] malloc FAILED\n";
            m_out(MALLOC_FAIL, sizeof MALLOC_FAIL - 1u);
        }
        free(b);
        free(a);
    }

    // ④ clock_gettime：musl 会先找 vDSO（内核没给 AT_SYSINFO_EHDR -> 回落 syscall 228）
    {
        struct timespec ts;
        memset(&ts, 0, sizeof ts);
        if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
            p = buf;
            p += m_str(p, "[MUSL] clock_gettime ok sec=");
            p += m_dec(p, (uint64_t)ts.tv_sec, 4);     // 定宽 4 位（启动/演示用，≤9999s）
            p += m_str(p, " nsec=");
            p += m_dec(p, (uint64_t)ts.tv_nsec, 9);    // 定宽 9 位（纳秒域满宽）
            p += m_str(p, "\n");
            m_out(buf, (size_t)(p - buf));
        } else {
            static const char CT_FAIL[] = "[MUSL] clock_gettime FAILED\n";
            m_out(CT_FAIL, sizeof CT_FAIL - 1u);
        }
    }

    // ⑤ getrandom：内核 318（伪随机，非密码学安全 —— 如实标注见 kernel/syscall64.cpp）
    {
        unsigned char r[16];
        memset(r, 0, sizeof r);
        const ssize_t n = getrandom(r, sizeof r, 0);
        if (n == (ssize_t)sizeof r) {
            p = buf;
            p += m_str(p, "[MUSL] getrandom ok len=16 hex=");
            for (unsigned i = 0; i < sizeof r; i++) {
                static const char H[] = "0123456789abcdef";
                *p++ = H[(r[i] >> 4) & 0xFu];
                *p++ = H[r[i] & 0xFu];
            }
            *p++ = '\n';
            m_out(buf, (size_t)(p - buf));
        } else {
            static const char GR_FAIL[] = "[MUSL] getrandom FAILED\n";
            m_out(GR_FAIL, sizeof GR_FAIL - 1u);
        }
    }

    // ⑥ errno 的硬证据：musl 的 errno 是**每线程**的（%fs 指向的 pthread 结构里的字段），
    //    所以这一条只有在 arch_prctl(ARCH_SET_FS) 真写进 MSR 之后才可能成功。
    {
        errno = 0;
        const int fd = open("/no_such_file_for_musl", O_RDONLY);
        if (fd < 0 && errno == ENOENT) {
            static const char ERRNO_OK[] = "[MUSL] errno ok ENOENT=2\n";
            m_out(ERRNO_OK, sizeof ERRNO_OK - 1u);
        } else {
            p = buf;
            p += m_str(p, "[MUSL] errno FAILED fd=");
            if (fd >= 0) close(fd);
            p += m_dec(p, (uint64_t)(fd < 0 ? (int64_t)errno : (int64_t)fd), 0);   // 0 = 自然宽度
            p += m_str(p, "\n");
            m_out(buf, (size_t)(p - buf));
        }
    }

    // ⑦ 退出码：验收脚本断言内核打 [PROC64] exit … code=7
    return 7;
}
