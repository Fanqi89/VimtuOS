/* libfoo.c - ★ A3 下半：动态链接演示用的**共享库**（libfoo.so）
 *
 * 构建（tools/dynlink_build_win.sh）：clang -fPIC -c + ld.lld -shared -T user/ldso/libfoo.ld
 * 三类重定位在这一个文件里都留了"活的"目标（ld.so 必须都能处理，否则程序行为立刻不对）：
 *   * R_X86_64_RELATIVE：foo_name_p（本 .so 自己的 .data 指针 -> .rodata 里的字符串）
 *   * R_X86_64_GLOB_DAT / JUMP_SLOT 的目标：foo_counter（数据）与 foo_add（函数）
 *   * DT_INIT_ARRAY / DT_INIT：foo_ctor（构造函数）+ foo_init（-Wl,-init,foo_init）
 *
 * 为什么不用 libc：本 .so 只演示"重定位 + 依赖加载 + 初始化"，用最小 syscall 直接 write(1)。
 * 输出的每一行都是自动验收（tests/dynlink64_test.py）的断言依据 —— 格式勿改。
 */
typedef unsigned long u64;
typedef long i64;

static i64 sys3(u64 nr, u64 a1, u64 a2, u64 a3) {
    i64 ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}
static void foo_write(const char* s) {
    u64 n = 0;
    while (s[n]) n++;
    sys3(1, 1, (u64)s, n);
}

/* ---- RELATIVE：本 .so 自己的指针（链接期是 0+偏移，加载期必须被 ld.so 修正）---- */
static const char FOO_NAME[] = "libfoo";
static const char* foo_name_p = FOO_NAME;

/* ---- GLOB_DAT 的目标：主程序通过 GOT 读它（导出数据）---- */
int foo_counter = 41;
int foo_ctor_hits = 0;
int foo_init_hits = 0;

/* ---- JUMP_SLOT 的目标：主程序通过 PLT 调它（导出函数）---- */
int foo_add(int a, int b) { return a + b + foo_counter; }
int foo_name_first(void) { return (int)foo_name_p[0]; }        /* 'l' = 108：证明 RELATIVE 生效 */

/* ---- DT_INIT_ARRAY（构造函数）与 DT_INIT（-Wl,-init,foo_init）---- */
__attribute__((constructor)) static void foo_ctor(void) {
    foo_ctor_hits++;
    foo_write("[LDFOO] init_array called\n");
}
void foo_init(void) {
    foo_init_hits++;
    foo_write("[LDFOO] DT_INIT called\n");
}

/* ---- ④ 符号解析顺序（**主程序优先**）用的同名符号 ----
 * 主程序（user/apps/dynhello.c）里**也**定义了 int foo_dup(void)，返回值不同（2 vs 1）。
 * 本文件里对 foo_dup 的引用是"**取地址**"（foo_dup_p），在**不 -Bsymbolic** 的共享库里
 * 这会生成 R_X86_64_GLOB_DAT —— 也就是说该地址由 ld.so 在装载期解析：
 *   正确实现（主程序优先 -> 依赖顺序）必须解析到**主程序**那一份（返回 2）；
 *   解析错了就会拿到本文件这份（返回 1）—— [DYNH] dup main wins call=1 立刻暴露。
 * 注：一开始这里写的是 `foo_dup_via_plt(){ return foo_dup(); }`（想靠 .so 自己的 PLT），
 *   实测 lld 把这种"同模块内的直接调用"就地绑定了（.rela.dyn 里没有 JUMP_SLOT），
 *   所以改成取地址 + GLOB_DAT 这条**可观测**的路径。 */
int foo_dup(void) { return 1; }
static int (*volatile foo_dup_p)(void) = foo_dup;      /* ← GLOB_DAT 的目标（地址引用） */

/* ④ 的直接证据：本 .so 通过那个由 ld.so 解析出来的 GOT 槽调用 foo_dup */
int foo_dup_via_plt(void) { return foo_dup_p(); }
