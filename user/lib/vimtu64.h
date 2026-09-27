/* vimtu64.h - Vimtu64 自有 ABI（int 0x80）号段 + 原始桩 + A1 绘图结构（用户态 C 运行时）
 *
 * ★ 号段与结构体的**唯一定义点在内核**（kernel/syscall64.h / kernel/usermode64.h）；
 *   这里只是同一份约定在用户侧的一份拷贝，改任一侧都要同步另一侧 + docs/应用层与系统调用说明.md。
 *
 * 寄存器约定（自有 ABI）：rax = 调用号；rdi/rsi/rdx/r10 = 参数 1..4；rax = 返回值（负数 = 错误）。
 * 本文件里的 C 函数全部按这个约定包一层，**不做任何猜测**：拿不到就是负值/errno。 */
#ifndef VIMTU64_VIMTU64_H
#define VIMTU64_VIMTU64_H

#include <stdint.h>

/* ---------------- 自有 ABI 号段（int 0x80） ---------------- */
#define V64_NR_WRITE       1   /* write(fd=1, buf, len)  -> 已写字节数 / -1 */
#define V64_NR_EXIT        2   /* exit(code)             -> 不返回 */
#define V64_NR_GETPID      3   /* getpid()               -> pid */
#define V64_NR_TICKS       4   /* ticks()                -> PIT tick 数（250Hz） */
#define V64_NR_SLEEP_MS    5   /* sleep_ms(ms)           -> 0 */
#define V64_NR_OPEN        6   /* open(path) 只读        -> fd / -1 */
#define V64_NR_READ        7   /* read(fd, buf, len)     -> 已读字节数 / -1 */
#define V64_NR_CLOSE       8   /* close(fd)              -> 0 / -1 */
#define V64_NR_FB_MAP      9   /* fb_map(&va, &info)     -> 0 / -1..-4（A1） */
#define V64_NR_FB_FLIP     10  /* fb_flip(x, y, w, h)    -> 0 = 已提交(含夹取) / 1 = 完全越界被拒 */
#define V64_NR_FB_PRESENT  11  /* fb_present()           -> 0/负 */

/* write(1,...) 单次上限：内核 SYSCALL64_WRITE_MAX = 1024（超了算参数错误并打 [SYSCALL] deny）。
   write() 包装按这个值**自动分块**，调用方不用管。 */
#define V64_WRITE_MAX      1024

/* fb_map 的错误码（负数，见 kernel/syscall64.h） */
#define V64_FB_EPERM      (-1)   /* 用户窗口不可用（UEFI 固件页表） */
#define V64_FB_EFAULT     (-2)   /* 用户指针非法 */
#define V64_FB_ENODEV     (-3)   /* 没有帧缓冲/后备缓冲 */
#define V64_FB_ENOMEM     (-4)   /* 页表页不足 */

/* fb_flip 的返回码 */
#define V64_FB_FLIPPED     0     /* 已提交（可能被夹取） */
#define V64_FB_REJECT      1     /* 完全越界被拒（内核打点 clip=reject） */

/* ---------------- A1：后备缓冲几何（与内核 Fb64Info 逐字段一致） ---------------- */
struct Fb64Info {
    uint32_t width;    /* +0  后备缓冲宽 */
    uint32_t height;   /* +4  高 */
    uint32_t pitch;    /* +8  每行字节数（= width*4） */
    uint32_t format;   /* +12 0 = XRGB8888（0x00RRGGBB，内存里低字节是蓝） */
    uint64_t size;     /* +16 总字节数 */
    uint64_t va;       /* +24 用户可读写映射基址 */
};
#define V64_FB_FMT_XRGB8888 0

/* ---------------- Linux 兼容号段（syscall 指令；自有 ABI 没有的号从这里走）---------------- */
#define V64_LX_READ        0
#define V64_LX_WRITE       1
#define V64_LX_OPEN        2
#define V64_LX_CLOSE       3
#define V64_LX_STAT        4
#define V64_LX_FSTAT       5
#define V64_LX_LSEEK       8
#define V64_LX_GETCWD     79
#define V64_LX_UNLINK     87
#define V64_INSN_MARK     0x180u  /* 只作说明：内核帧里的路径标记（用户侧看不到） */

/* ---------------- 原始桩（user/lib/syscall.S）----------------
 * __v64_int80：自有 ABI。nr + 最多 4 个参数（第 4 个放 r10 —— fb_flip 的 h 用它）。
 * __v64_syscall：syscall 指令（Linux 号段），返回 -errno。 */
long __v64_int80(long nr, long a1, long a2, long a3, long a4);
long __v64_syscall(long nr, long a1, long a2, long a3, long a4, long a5);

/* 编译期常量：程序 blob 的链接基址（= kernel/usermode64.h 的 USER64_CODE_VA64）。
 * 用户程序**不要**拿它算地址（代码是链接期定死的绝对地址），只用来打印/自检。 */
#define V64_CODE_VA64 0x0000000100000000ul

#endif /* VIMTU64_VIMTU64_H */
