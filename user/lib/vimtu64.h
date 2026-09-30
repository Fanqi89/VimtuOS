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
/* ---- ★ A5 前置：用户态输入事件投递 + 共享内存缓冲（内核侧定义见 kernel/input64.h 与
 *      kernel/proc64.h 的 A5 段；改这里必须同步那两份 + docs/应用层与系统调用说明.md） ---- */
#define V64_NR_INPUT_POLL  12  /* input_poll(Event*, max, flags) -> 事件数 / 待取数(max=0) / 负错误码 */
#define V64_NR_SHM_CREATE  13  /* shm_create(size)               -> 跨进程共享的对象 id(>=1) / 负错误码 */
#define V64_NR_SHM_MAP     14  /* shm_map(id, off, len, &va)     -> 0 / 负错误码（第 4 参数走 r10） */

/* input_poll 的错误码（与 kernel/input64.h 的 EV64_* 一致） */
#define V64_EV_EPERM    (-1)   /* 没有进程上下文（任务 0 / 共享地址空间模式） */
#define V64_EV_EFAULT   (-2)   /* out 指针非法 */
#define V64_EV_ENODEV   (-3)   /* 没有事件层（理论上到不了） */
#define V64_EV_ENOMEM   (-4)   /* max 超过队列容量（32） */
#define V64_EV_EINVAL   (-5)   /* flags 有未知位 */

/* shm 的错误码（与 kernel/proc64.h 的 SHM64_* 一致） */
#define V64_SHM_EPERM   (-1)   /* 没有进程上下文 / 本进程没有这个对象的句柄 */
#define V64_SHM_EFAULT  (-2)   /* out_va 指针非法 */
#define V64_SHM_EINVAL  (-3)   /* size/offset/len 非法 */
#define V64_SHM_ENOMEM  (-4)   /* 对象表/页池/句柄表满；映射窗被占用 */

/* ---------------- ★ A5 前置：事件结构（与内核 Ev64Event **逐字节**一致：40 B，POD） ---------------- */
struct Ev64Event {
    uint32_t type;     /* +0  EV64_TYPE_* */
    uint32_t code;     /* +4  键码 / 鼠标键位掩码 / 0 */
    int32_t  x;        /* +8  光标 x */
    int32_t  y;        /* +12 光标 y（向下为正） */
    int32_t  dx;       /* +16 本次实际生效的 x 位移 */
    int32_t  dy;       /* +20 本次实际生效的 y 位移（WHEEL 时 = 滚轮增量） */
    uint32_t buttons;  /* +24 鼠标键位掩码 */
    uint32_t mods;     /* +28 修饰键掩码 */
    uint64_t t_ms;     /* +32 毫秒时间戳（250Hz PIT，4ms 粒度） */
};
#define V64_EV_SIZE        40

#define V64_EV_KEY_DOWN    1u
#define V64_EV_KEY_UP      2u
#define V64_EV_MOUSE_MOVE  3u
#define V64_EV_MOUSE_DOWN  4u
#define V64_EV_MOUSE_UP    5u
#define V64_EV_WHEEL       6u

/* input_poll 的 flags：bit0 申请键盘焦点、bit1 申请指针捕获、bit2 释放两者 */
#define V64_EV_FLAG_FOCUS    0x1u
#define V64_EV_FLAG_CAPTURE  0x2u
#define V64_EV_FLAG_RELEASE  0x4u

/* mods 位 */
#define V64_EV_MOD_SHIFT   0x1u
#define V64_EV_MOD_CTRL    0x2u
#define V64_EV_MOD_ALT     0x4u
#define V64_EV_MOD_CAPS    0x8u

/* 鼠标键位（MOUSE_DOWN/UP 的 code + buttons 的位） */
#define V64_EV_BTN_LEFT    0x1u
#define V64_EV_BTN_RIGHT   0x2u
#define V64_EV_BTN_MIDDLE  0x4u

/* 键码口径（与内核 input.h / input.cpp 的键盘缓冲一致；验收脚本按这些值断言） */
#define V64_KEY_ESC        0x1Bu
#define V64_KEY_UP         0xFDu
#define V64_KEY_DOWN       0xFEu
#define V64_KEY_LEFT       0xFBu
#define V64_KEY_RIGHT      0xFCu
#define V64_KEY_DELETE     0xFAu
#define V64_KEY_F2         0xF9u
#define V64_KEY_PAGEUP     0xF8u
#define V64_KEY_PAGEDOWN   0xF7u

/* 单对象上限（= kernel/proc64.h 的 SHM64_MAX_PAGES64 * 4KiB） */
#define V64_SHM_MAX_BYTES  (64u * 1024u)

/* ---------------- ★ A5 前置：包装（user/lib/syscall.c） ----------------
 * poll_event(out, max, flags)：out != NULL 时最多取 max 条事件（40 B/条）并返回条数；
 *   max == 0 时只查询\"有多少待取\"（out 忽略）；返回负数 = 内核错误码（V64_EV_*）。
 * shm_create(size)：返回跨进程可共享的对象 id（>= 1）；负数 = V64_SHM_*。
 * shm_map(id, offset, len, out_va)：把对象的一段映射进**当前进程**，*out_va 写回基址；
 *   offset 必须 4KiB 对齐；返回 0 = 成功。 */
int poll_event(struct Ev64Event* out, unsigned max, unsigned flags);
int shm_create(unsigned size);
int shm_map(int id, unsigned offset, unsigned len, void** out_va);

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
