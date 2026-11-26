/* user/netsurf/libm_shim.c - ★ v0.12.5：补上 `fabs` / `fabsf`（本机 musl 归档里缺这两个对象）
 *
 * 事实现场（实测，不是猜测）：
 *   third_party/musl/lib/libc.a  里只有 `fabsl.o`，**没有** `fabs.o` / `fabsf.o`；
 *   third_party/musl/lib/libm.a  里一个 fabs 相关对象都没有。
 * 而 libsvgtiny 的 `svgtiny_path.c` / `svgtiny_gradient.c` 真的调用 `fabs()` 与 `fabsf()`
 * （它在算路径包围盒与渐变点），于是 whole-archive 链接必然：
 *   `ld.lld: error: undefined symbol: fabsf / fabs`
 *
 * 处理方式：按 IEEE-754 语义**清掉符号位**实现这两个函数。这与 C 标准对 `fabs` 的定义逐位一致
 * （含 ±0、±inf、NaN 的符号位处理；不产生浮点异常、不依赖 libm）。
 * 为什么不用 -Dfabs=__builtin_fabs 糊过去：库源码里写的是 `fabs(...)`，在 -fno-builtin 下
 * 编译器生成的是**外部调用**，用宏替换等于改上游源码——不如老实补一个符号。
 */
#include <stdint.h>

double fabs(double x)
{
    union { double d; uint64_t u; } v;
    v.d = x;
    v.u &= 0x7FFFFFFFFFFFFFFFULL;      /* 清掉 double 的符号位（bit63） */
    return v.d;
}

float fabsf(float x)
{
    union { float f; uint32_t u; } v;
    v.f = x;
    v.u &= 0x7FFFFFFFU;                /* 清掉 float 的符号位（bit31） */
    return v.f;
}
