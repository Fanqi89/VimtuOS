/* fb.c - ★ A1 用户态绘图的 C 包装（Vimtu64 用户态 C 运行时）
 *
 * 三条调用全部走**自有 ABI（int 0x80）的 9/10/11**（= user/fbdemo.asm 用的同一套内核接口）：
 *   fb_map     (9)  rdi = &va_out（8 字节）、rsi = &info（32 字节 Fb64Info）-> 0 / -1..-4
 *   fb_flip    (10) rdi/rsi/rdx/r10 = x/y/w/h（**有符号 int**）-> 0 = 已提交（含夹取）/ 1 = 完全越界被拒
 *   fb_present (11) 整屏提交 -> 0
 * 错误码与 kernel/syscall64.h 的 A1 段一一对应；负值同时映射进 errno（见 errno.h 的口径）。 */
#include <errno.h>

#include "fb.h"
#include "vimtu64.h"

static void v64_fb_errno64(int code) {
    switch (code) {
    case V64_FB_EPERM:  errno = EPERM;  break;
    case V64_FB_EFAULT: errno = EFAULT; break;
    case V64_FB_ENODEV: errno = ENODEV; break;
    case V64_FB_ENOMEM: errno = ENOMEM; break;
    default:            errno = EIO;    break;
    }
}

int fb_map(uint32_t** out_va, struct Fb64Info* out_info) {
    if (!out_va || !out_info) { errno = EFAULT; return V64_FB_EFAULT; }
    unsigned long va = 0;                                  /* 内核往这里写 8 字节用户 VA */
    const long r = __v64_int80(V64_NR_FB_MAP, (long)(uintptr_t)&va, (long)(uintptr_t)out_info, 0, 0);
    if (r != 0) {
        const int code = (int)r;
        v64_fb_errno64(code);
        return code;                                       /* 负值原样返回（调用方要如实打印） */
    }
    *out_va = (uint32_t*)(uintptr_t)va;
    return 0;
}

int fb_flip(int x, int y, int w, int h) {
    /* int -> long 是有符号扩展的：x = -40 会变成 0xFFFF...FFD8（与 asm 版 `mov rdi, -40` 一致） */
    const long r = __v64_int80(V64_NR_FB_FLIP, (long)x, (long)y, (long)w, (long)h);
    if (r < 0) { v64_fb_errno64((int)r); return (int)r; }
    return (int)r;                                         /* 0 = 已提交（含夹取）；1 = 完全越界被拒 */
}

int fb_present(void) {
    const long r = __v64_int80(V64_NR_FB_PRESENT, 0, 0, 0, 0);
    if (r < 0) { v64_fb_errno64((int)r); return (int)r; }
    return 0;
}
