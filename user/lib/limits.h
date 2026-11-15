/* limits.h - 整数极值（Vimtu64 用户态 C 运行时）
 *
 * 为什么需要它：`third_party/stb/stb_image.h` 无条件 `#include <limits.h>`
 * （用 INT_MAX / UINT_MAX 做尺寸上界检查）。本仓库一律 `-nostdinc -I user/lib`，
 * 所以这份头文件必须由我们自己提供，否则 stb_image 编不过。
 *
 * 口径：与 x86_64 Linux/musl 的 64 位模型一致（long = 64 位、int = 32 位）。
 */
#ifndef VIMTU64_LIMITS_H
#define VIMTU64_LIMITS_H

#define CHAR_BIT   8
#define SCHAR_MIN  (-128)
#define SCHAR_MAX  127
#define UCHAR_MAX  255

#define CHAR_MIN   0
#define CHAR_MAX   UCHAR_MAX

#define MB_LEN_MAX 1

#define SHRT_MIN   (-32768)
#define SHRT_MAX   32767
#define USHRT_MAX  65535u

#define INT_MIN    (-2147483647 - 1)
#define INT_MAX    2147483647
#define UINT_MAX   4294967295u

#define LONG_MIN   (-9223372036854775807L - 1L)
#define LONG_MAX   9223372036854775807L
#define ULONG_MAX  18446744073709551615uL

#define LLONG_MIN  (-9223372036854775807LL - 1LL)
#define LLONG_MAX  9223372036854775807LL
#define ULLONG_MAX 18446744073709551615uLL

#endif /* VIMTU64_LIMITS_H */
