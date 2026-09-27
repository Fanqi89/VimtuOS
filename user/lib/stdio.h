/* stdio.h - 极简标准输出（Vimtu64 用户态 C 运行时）
 *
 * printf 子集（**不带浮点**：本内核与运行时都不开 FP）：
 *   转换：%s %c %d %i %u %x %X %p %%
 *   长度修饰：l / ll / z（按 64 位处理；不带修饰时按 32 位）
 *   宽度：十进制位数；标志：'-'（左对齐）、'0'（数字左侧补 0）
 *   其它（%f/%e/%g/精度/星号宽度）**没做** —— 遇到就按字面输出 "%?"，绝不假装算过。
 * 输出路径：128 字节**行缓冲** -> write(1, ...)（自有 ABI 单次上限 1024，write() 会分块），
 *   换行或缓冲满就落一次 write —— 这样每行只对应一条 [SYSCALL] 打点，串口日志好读也好 grep。 */
#ifndef VIMTU64_STDIO_H
#define VIMTU64_STDIO_H

#include <stddef.h>
#include <stdarg.h>

int  putchar(int c);
int  puts(const char* s);
int  printf(const char* fmt, ...);
int  vprintf(const char* fmt, va_list ap);

/* 行缓冲：把当前缓冲里的字节立刻 write 出去（exit()/崩溃前想保住输出就调它） */
void vimtu64_stdout_flush64(void);
int  fflush(void* stream);          /* stream 参数只为兼容调用写法，本运行时有 stdout（fd 1） */

#endif /* VIMTU64_STDIO_H */
