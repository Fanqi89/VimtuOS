/* stdint.h - 自备定长整数类型（Vimtu64 用户态 C 运行时；不依赖任何外部 libc）
 *
 * 为什么自备：本运行时的构建用 `-nostdinc -I user/lib`，编译器自带的头文件一个都不用，
 * 这样"用户程序能用 C 写"这件事不依赖宿主机的任何东西（工具链只当汇编器/优化器用）。
 * x86_64 System V：long = 8 字节。 */
#ifndef VIMTU64_STDINT_H
#define VIMTU64_STDINT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef short              int16_t;
typedef unsigned short     uint16_t;
typedef int                int32_t;
typedef unsigned int       uint32_t;
typedef long               int64_t;
typedef unsigned long      uint64_t;

typedef long               intptr_t;
typedef unsigned long      uintptr_t;
typedef long               intmax_t;
typedef unsigned long      uintmax_t;

#define INT8_MIN   (-128)
#define INT8_MAX   127
#define UINT8_MAX  255u
#define INT16_MIN  (-32768)
#define INT16_MAX  32767
#define UINT16_MAX 65535u
#define INT32_MIN  (-2147483647 - 1)
#define INT32_MAX  2147483647
#define UINT32_MAX 4294967295u
#define INT64_MIN  (-9223372036854775807L - 1)
#define INT64_MAX  9223372036854775807L
#define UINT64_MAX 18446744073709551615uL

#endif /* VIMTU64_STDINT_H */
