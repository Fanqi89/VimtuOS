/* user/lib/compiler_rt_shim.c - ★ v0.12.4：补上静态链接时缺的 128 位整数内建函数
 *
 * 为什么需要：mbedTLS 的 library/bignum.c 用 `unsigned __int128` 做除法/取余
 * （mbedtls_mpi_div_mpi 里的双字长除法），clang 把它编成对 __udivti3 / __umodti3 的调用。
 * 这两个符号通常在 libgcc 或 compiler-rt 的 builtins 库里；我们这套 musl 静态工具链**没有**：
 *   * musl 的 libc.a 不含 builtins；
 *   * msys2 的 mingw64/lib/clang/<ver>/lib/windows/libclang_rt.builtins-x86_64.a 是 COFF 目标，
 *     ld.lld 直接拒收（实测：`archive member 'ashrdi3.c.obj' is neither ET_REL nor LLVM bitcode`）。
 * 于是自己写：**只用移位/比较/减法**（不写 `a / b`），避免"实现里又调回自己"的无限递归。
 *
 * 算法：恢复式长除法，每次从被除数取最高位、商左移、够减就减。
 *   128 次迭代、全是**常量**位移（`>> 127` / `<< 1`），所以本身不会再生成 128 位除法调用。
 *   代价是慢（几百拍），但这条路径只在 RSA/大数除法里出现，不是热路径。
 *
 * 契约（gcc/clang 的 ABI，x86_64 SysV）：
 *   __int128 参数/返回值走 rdi:rsi（低:高）与 rdx:rax；这里用 C 签名让编译器自己拼。
 */
#include <stdint.h>

typedef unsigned __int128 u128;

/* n / d 与 n % d 一次算完（d == 0 时按"未定义"处理，返回全 0 —— 调用方不该传 0） */
static void shim_udivmod128(u128 n, u128 d, u128* q, u128* r)
{
    u128 Q = 0, R = 0;
    for (int i = 0; i < 128; i++) {
        R = (R << 1) | ((n >> 127) & 1u);
        n = n << 1;
        Q = Q << 1;
        if (R >= d) { R -= d; Q |= 1u; }
    }
    if (q) *q = Q;
    if (r) *r = R;
}

unsigned __int128 __udivti3(unsigned __int128 n, unsigned __int128 d)
{
    u128 q = 0;
    shim_udivmod128(n, d, &q, (u128*)0);
    return q;
}

unsigned __int128 __umodti3(unsigned __int128 n, unsigned __int128 d)
{
    u128 r = 0;
    shim_udivmod128(n, d, (u128*)0, &r);
    return r;
}

/* 有符号版本：mbedTLS 里目前只用无符号两个；一并给出，免得换个编译选项又缺符号。
 * 语义照 gcc：先算绝对值再做无符号除法，商/余数的符号按"向零取整"。 */
static unsigned __int128 shim_abs128(__int128 v) { return (v < 0) ? (u128)(-(v + 1)) + 1u : (u128)v; }

__int128 __divti3(__int128 n, __int128 d)
{
    const int neg = ((n < 0) != (d < 0));
    u128 q = 0;
    shim_udivmod128(shim_abs128(n), shim_abs128(d), &q, (u128*)0);
    return neg ? -(__int128)q : (__int128)q;
}

__int128 __modti3(__int128 n, __int128 d)
{
    u128 r = 0;
    shim_udivmod128(shim_abs128(n), shim_abs128(d), (u128*)0, &r);
    return (n < 0) ? -(__int128)r : (__int128)r;
}
