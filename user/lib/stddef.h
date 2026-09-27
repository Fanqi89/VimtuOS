/* stddef.h - size_t / ptrdiff_t / NULL / offsetof（Vimtu64 用户态 C 运行时） */
#ifndef VIMTU64_STDDEF_H
#define VIMTU64_STDDEF_H

typedef __SIZE_TYPE__    size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;
typedef __WCHAR_TYPE__   wchar_t;

#ifndef NULL
#define NULL ((void*)0)
#endif

#define offsetof(type, member) __builtin_offsetof(type, member)

#endif /* VIMTU64_STDDEF_H */
