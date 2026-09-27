/* dynhello.c - ★ A3 下半：**依赖共享库的动态程序**（主程序，由 ld.so 带起来）
 *
 * 内核装载它时看到 PT_INTERP=/lib/ldvimtu.so，于是先装 ld.so、把控制权交给 ld.so；
 * ld.so 读它的 PT_DYNAMIC（在 AT_PHDR 指着的那张程序头表里找），按 DT_NEEDED 加载
 * libfoo.so，做三类重定位，调用双方的 DT_INIT/DT_INIT_ARRAY，最后跳到本文件的 _start。
 * 所以本文件里**每一行输出都是"ld.so 真的做对了"的证据**：
 *   [DYNH] hello from PIE + libfoo.so            —— 入口真的到了（ld.so 跳转成功）
 *   [DYNH] interp base=0x… entry=0x…             —— auxv 的 AT_BASE/AT_ENTRY（与内核 [ELF64] interp 对得上）
 *   [DYNH] glob_dat foo_counter=41 ptr_ok=1      —— GLOB_DAT 生效（通过 GOT 读 .so 的导出数据）
 *   [DYNH] abs64 ptr_ok=1                        —— R_X86_64_64 生效（.data 里的绝对指针）
 *   [DYNH] jump_slot foo_add(1,2)=44             —— JUMP_SLOT 生效（通过 PLT 调 .so 的导出函数）
 *   [DYNH] foo_name_first=108                    —— .so 自己的 RELATIVE 生效（'l'=108）
 *   [DYNH] init hits foo_init_array=1 foo_init=1 main_init=1
 *                                                —— ld.so 调过 .so 的 DT_INIT_ARRAY/DT_INIT
 *                                                   与主程序自己的 DT_INIT
 *   [DYNH] argv0=/dynhello.elf argc=1            —— 初始栈原样交给主程序（ld.so 没破坏它）
 * 退出码也是证据：重定位/初始化任何一步没做对，下面的校验就会打印 FAIL 并 exit(9)。
 *
 * 链接（tools/dynlink_build_win.sh）：clang -fPIC -c + ld.lld -pie --image-base=0x100000000
 *   * ET_DYN（PIE，靠 R_X86_64_RELATIVE 自定位），映像钉在 4GiB（内核装载区）；
 *   * 用 lld 的**默认脚本** + --image-base（不用自定义脚本）：实测自定义脚本会把主程序的
 *     .data.rel.ro/.init_array 丢掉（重定位也没了）—— 见 tools/dynlink_build_win.sh 的记录；
 *   * 头/程序头表进第一个 PT_LOAD（off=0 / va=4GiB）—— 内核的 AT_PHDR 才算得出来
 *     （ld.so 靠它 + PT_PHDR 反推主程序的加载偏移，本程序为 0）。
 */
typedef unsigned char u8;
typedef unsigned int  u32;
typedef unsigned long u64;
typedef long i64;

static i64 sys3(u64 nr, u64 a1, u64 a2, u64 a3) {
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}
static void dh_write(const char* s) {
    u64 n = 0;
    while (s[n]) n++;
    sys3(1, 1, (u64)s, n);
}
static void dh_hex(u64 v) {
    static const char H[] = "0123456789abcdef";
    char b[19];
    b[0] = '0'; b[1] = 'x';
    for (u32 i = 0; i < 16; i++) b[2 + i] = H[(v >> (60 - 4 * i)) & 0xF];
    b[18] = 0;
    dh_write(b);
}
static void dh_dec(u64 v) {
    char b[24];
    u32 i = 23;
    b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + (u32)(v % 10u)); v /= 10u; }
    dh_write(&b[i]);
}

/* ---- 共享库导出的符号（三类重定位的目标）---- */
extern int foo_add(int, int);            /* JUMP_SLOT（PLT） */
extern int foo_counter;                  /* GLOB_DAT（GOT） */
extern int foo_ctor_hits;                /* GLOB_DAT */
extern int foo_init_hits;                /* GLOB_DAT */
extern int foo_name_first(void);         /* JUMP_SLOT */
extern int foo_dup_via_plt(void);        /* .so 内部经 PLT 调 foo_dup（④ 符号解析顺序） */
static int* volatile dh_abs_ptr = &foo_counter;      /* R_X86_64_64（.data 里的绝对指针） */
static const char DH_MSG[] = "[DYNH] hello from PIE + libfoo.so\n";
static const char* volatile dh_msg_p = DH_MSG;             /* R_X86_64_RELATIVE（本程序自己的指针） */

/* ---- ④ 与 libfoo.so 里的**同名符号**（那边返回 1，这里返回 2）----
 * 契约：ELF 的符号解析顺序是"主程序优先 -> 依赖顺序"。libfoo.so 内部的 foo_dup_via_plt()
 * 走它自己的 PLT，所以装载期由 ld.so 解析 foo_dup：正确实现必须解析到**本文件这一份**
 * （地址在 4GiB 装载区），于是返回 2；解析错了会拿到 .so 自己那份（返回 1）。
 * 这个符号必须进本程序的 .dynsym（链接时加 -E/--export-dynamic），否则 .so 找不到它。 */
int foo_dup(void) { return 2; }

/* 主程序自己的初始化函数：ld.so 必须调用它（DT_INIT，链接参数 -init dh_init）。
 * ★ 实测注解：这里**不用** __attribute__((constructor))/DT_INIT_ARRAY —— lld 对本项目这套
 *   "自定义基址 + 默认脚本" 的 PIE 会把主程序的 .init_array 输出段丢掉（连 DT_INIT_ARRAY
 *   都不给，见 tools/dynlink_build_win.sh 的实测记录）；-init 的 DT_INIT 路径稳定可用，
 *   而 DT_INIT_ARRAY 这条路径由 libfoo.so 负责验证（它的 .init_array 保留且被调用）。 */
static int dh_init_hits = 0;
void dh_init(void) { dh_init_hits++; }

/* ★ A3 下半：入口桩在 user/apps/dynhello_start.S（汇编 4 条指令）—— 为什么不能用 C 读 %rsp
 *   见那个文件的说明：ld.so 是 jmp 过来的，rsp 指向初始栈，C 函数序言的 push 会把 rsp 挪到
 *   argc 槽下面（实测读成野值、随后 #PF cr2=0x90001EA20）。这里只写 C 主体。 */
void dh_main(u64* sp) {
    /* 从初始栈读 argc/argv（并读 auxv 的 AT_BASE/AT_ENTRY 与内核打点对照） */
    const u64 argc = sp[0];
    const char* argv0 = (const char*)sp[1];
    u64 at_base = 0, at_entry = 0;
    u64* p = sp + 1 + argc + 1;
    while (*p) p++;                                     /* envp */
    p++;
    for (; p[0] != 0; p += 2) {
        if (p[0] == 7) at_base = p[1];
        if (p[0] == 9) at_entry = p[1];
    }

    dh_write(dh_msg_p);                                  /* RELATIVE 生效才有这行 */

    dh_write("[DYNH] interp base="); dh_hex(at_base);
    dh_write(" entry="); dh_hex(at_entry);
    dh_write("\n");

    dh_write("[DYNH] glob_dat foo_counter=");
    dh_dec((u64)foo_counter);                            /* GLOB_DAT：GOT 槽里的地址 */
    dh_write(" ptr_ok=");
    dh_dec(dh_abs_ptr == &foo_counter ? 1u : 0u);        /* R_X86_64_64：绝对指针 */
    dh_write("\n");

    const int sum = foo_add(1, 2);                       /* JUMP_SLOT：PLT -> foo_add */
    dh_write("[DYNH] jump_slot foo_add(1,2)=");
    dh_dec((u64)sum);
    dh_write(" name_first=");
    dh_dec((u64)foo_name_first());                       /* .so 自己的 RELATIVE */
    dh_write("\n");

    const int dup = foo_dup_via_plt();                   /* ④ 符号解析顺序（主程序优先） */
    dh_write("[DYNH] dup main wins call=");
    dh_dec((u64)dup);
    dh_write("\n");

    dh_write("[DYNH] init hits foo_init_array=");
    dh_dec((u64)foo_ctor_hits);
    dh_write(" foo_init=");
    dh_dec((u64)foo_init_hits);
    dh_write(" main_init=");
    dh_dec((u64)dh_init_hits);
    dh_write("\n");
    dh_write("[DYNH] argv0=");
    dh_write(argv0 ? argv0 : "?");
    dh_write(" argc=");
    dh_dec(argc);
    dh_write("\n");

    /* 校验（任何一条不对 -> FAIL + exit(9)，自动验收能看到）：
       foo_counter=41、foo_add(1,2)=1+2+41=44、foo_name_first='l'=108、
       foo_dup_via_plt()=2（**主程序**那一份，不是 .so 里的 1）、
       两边的构造函数都跑过、argv0 是 /dynhello.elf、auxv 里 base/entry 都有值。 */
    const int ok = (foo_counter == 41) && (sum == 44) && (foo_name_first() == 108) &&
                   (dup == 2) &&
                   (foo_ctor_hits == 1) && (foo_init_hits == 1) && (dh_init_hits == 1) &&
                   at_base != 0 && at_entry != 0 && argv0 && argv0[0] == '/';
    if (ok) {
        dh_write("[DYNH] PASS exit=0\n");
    } else {
        dh_write("[DYNH] FAIL checks (see values above) exit=9\n");
    }
    sys3(60, ok ? 0u : 9u, 0, 0);
    for (;;) { }                                        /* exit 不返回；这里只是兜底 */
}
