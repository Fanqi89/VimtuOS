/* lua_compat.c - ★ A4-4a：补齐 vendored musl 归档里**缺的两个数学符号**（fabs / fabsf）
 *
 * 为什么需要这一份（如实说明，不掩盖第三方归档的缺口）：
 *   third_party/musl/lib/libc.a（1341 个成员，A2 那次实测构建的产物）里 **没有 fabs.o/fabsf.o**
 *   —— 上游 musl 的 src/math/fabs.c 在宿主 clang 下被识别成"内建 fabs"从而没有落盘成成员。
 *   而 Lua 的 lmathlib/lobject 会用到 fabs（musl 的 pow.o/atan2.o/atan.o 也引用它），
 *   链接期就会 undefined symbol: fabs。
 *
 *   本文件用**同一份语义**把它补回来（IEEE-754 清符号位），并强制 -fno-builtin + -ffreestanding
 *   编译，保证一定产出符号定义（本文件在 tools/lua_build_win.sh 里单独编译）。
 *   ★ 不改 third_party/musl/ 那棵树（A2 的实测证据树，一个字节都不动）。
 */

typedef unsigned long long u64;
typedef unsigned int u32;

double fabs(double x) {
    union { double f; u64 i; } u;
    u.f = x;
    u.i &= 0x7FFFFFFFFFFFFFFFULL;
    return u.f;
}

float fabsf(float x) {
    union { float f; u32 i; } u;
    u.f = x;
    u.i &= 0x7FFFFFFFu;
    return u.f;
}
