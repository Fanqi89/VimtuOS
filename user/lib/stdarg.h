/* stdarg.h - 变参与编译器内建挂钩（Vimtu64 用户态 C 运行时）
 *
 * 只做映射，不引入任何浮点/复杂实现：printf 子集**不带浮点**（本内核与运行时都不开 FP，
 * 见 build64.sh 的用户态编译段 -mno-sse -mno-sse2）。 */
#ifndef VIMTU64_STDARG_H
#define VIMTU64_STDARG_H

typedef __builtin_va_list va_list;

#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_copy(dst, src)  __builtin_va_copy(dst, src)

#endif /* VIMTU64_STDARG_H */
