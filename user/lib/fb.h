/* fb.h - ★ A1 用户态绘图的自有 ABI 包装（Vimtu64 用户态 C 运行时）
 *
 * 与 user/fbdemo.asm 用的是**同一套**内核接口（int 0x80 的 9/10/11），只是包成了 C：
 *   fb_map    -> 内核把后备缓冲按用户可读写页映射进本进程，返回用户 VA + 几何
 *   fb_flip   -> 把刚画好的矩形从后备缓冲提交到屏幕（0 = 已提交（含夹取）/ 1 = 完全越界被拒）
 *   fb_present-> 整屏提交
 * 语义/错误码与 kernel/syscall64.h 的 A1 段一字不差（本文件不发明新语义）。 */
#ifndef VIMTU64_FB_H
#define VIMTU64_FB_H

#include <stdint.h>
#include "vimtu64.h"

/* 一次搞定：内部调 fb_map，成功返回 0，失败返回内核的负错误码（-1..-4，见 vimtu64.h）。
 * 成功时 *out_va = 后备缓冲的用户 VA，*out_info = 几何（与内核同一份约定）。 */
int fb_map(uint32_t** out_va, struct Fb64Info* out_info);
/* 提交矩形（有符号坐标）：0 = 已提交（可能被夹取）、1 = 完全越界被拒（内核打点 clip=reject） */
int fb_flip(int x, int y, int w, int h);
int fb_present(void);

/* 便捷：把 32 位像素写进后备缓冲（调用方自己保证坐标在界内；越界写会踩到映射外的页） */
static inline void fb_poke(struct Fb64Info* info, uint32_t* base, int x, int y, uint32_t color) {
    uint32_t* row = (uint32_t*)((unsigned char*)base + (unsigned long)(unsigned)y * info->pitch);
    row[x] = color;
}

#endif /* VIMTU64_FB_H */
