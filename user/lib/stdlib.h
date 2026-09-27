/* stdlib.h - 内存分配 / 字符串转换 / 退出（Vimtu64 用户态 C 运行时）
 *
 * 分配器：**bump + 空闲链表**（规格要求的最小实现），全部落在静态竞技场 `V64_HEAP_BYTES` 里：
 *   * 竞技场在 .bss（用户程序 blob 的可写页内），第一次 malloc 时惰性初始化；
 *   * 每块带 16 字节头（magic + size），**magic 就是越界哨兵**：头被踩坏时
 *     vimtu64_heap_check() 会如实报错（不假装没事）；
 *   * 空闲块按地址升序插回链表，分配走**首次适配 + 能切就切**；**不做块合并**（如实标注：
 *     反复"分配大块-释放-分配小块"会碎片化，够用但别当通用分配器）；
 *   * 越界/双重释放不会崩内核：越界写只会踩到别的堆块或竞技场外的 .bss（都是本进程自己的页），
 *     双重释放会被 magic 校验挡住并返回（free 是 void，检测结果在 vimtu64_heap_check()）。 */
#ifndef VIMTU64_STDLIB_H
#define VIMTU64_STDLIB_H

#include <stddef.h>

#define V64_HEAP_BYTES 4096u      /* 竞技场字节数（进 .bss，也进 blob 长度：改它要重新看 blob 大小） */

void* malloc(size_t n);
void  free(void* p);
void* calloc(size_t n, size_t sz);
void* realloc(void* p, size_t n);

int   atoi(const char* s);
int   abs(int v);

void  exit(int code) __attribute__((noreturn));

/* 运行时自检（不进 POSIX；给用户程序/验收脚本当"堆没被踩坏"的证据）：
 *   返回 0 = 竞技场/所有块头/哨兵都完好；非 0 = 出问题的块序号 + 1（或 0xFFFFFFFF）。
 *   哨兵 = 每个已分配块的头部 magic（V64_BLK_MAGIC_ALLOC）与竞技场尾部的 V64_ARENA_GUARD。 */
unsigned vimtu64_heap_check(void);
/* 堆统计：已用字节 / 竞技场字节（供用户程序打印证据） */
void vimtu64_heap_stats(unsigned* used, unsigned* total, unsigned* blocks);

#endif /* VIMTU64_STDLIB_H */
