/* sh64.h - ★ A4-1：ring3 shell 与内核终端之间的**共享邮箱** ABI（用户侧这一份）
 *
 * 为什么需要它（数据流的唯一定义点，内核侧同一份在 kernel/terminal64.cpp 的
 * "A4-1: ring3 shell" 段，**改一处必须同步另一处**）：
 *   终端窗口的按键 -> 内核 terminal64 的 term_key -> 邮箱 in 环 -> shell 的 stdin；
 *   shell 的 stdout -> 邮箱 out 环 -> 内核 term_tick 抽干 -> 终端窗口 + 串口。
 *
 * 内存从哪来：内核在 `shell` 命令里 page_alloc_64() 拿一页物理页，把它按 U|W 映射进
 * **这个 shell 进程**的地址空间，固定 VA = SH64_MAIL_VA64。shell 侧只是按这个固定 VA 直接读写
 * （不需要任何系统调用）。
 *
 * 环语义（单生产者/单消费者，单核，无锁）：
 *   in  ：内核写 in_w、shell 读 in_r；空 = in_r == in_w；满 = (w + 1) % CAP == r
 *   out ：shell 写 out_w、内核读 out_r；同上
 *   magic：shell 把整页清零后写 SH64_MAGIC 表示"就绪"；内核只在这个值对时才动环。
 *
 * ★ 邮箱 VA 为什么在**用户窗口**（4GiB+384KiB）而不是 A1 的 FB 区（5GiB）：
 *   内核的 user64_range_ok64() 只认 1MiB 用户窗口（usermode64.cpp:500），而 shell 用
 *   `read(3, VA, 1)` 探"邮箱在不在"这一招**必须**落在窗口里才行 —— 落在 5GiB 上 probe 一定
 *   返回 -EFAULT（哪怕页真映射着），直接读未映射页又是 ring3 #PF（= 内核 PANIC）。
 *   4GiB+384KiB 在窗口内的空闲缝里（ELF 装载区 / 用户栈 / brk / mmap 都不碰它）。
 * ★ 这一页在**进程自己的用户区**里，所以随进程回收一起 page_free（内核侧不再释放一次）。
 */
#include <stddef.h>    /* offsetof（下面的布局自检用） */
#include <stdint.h>

#ifndef VIMTU64_SH64_H
#define VIMTU64_SH64_H

#define SH64_MAIL_VA64   0x0000000100060000ULL   /* 4GiB + 384KiB（用户窗口内的空闲缝，见上面的说明） */
#define SH64_MAGIC       0x53483634u             /* 'SH64'：shell 写完 = 邮箱就绪 */
#define SH64_IN_CAP      512                     /* in 环（内核 -> shell）字节数 */
#define SH64_OUT_CAP     2048                    /* out 环（shell -> 内核）字节数 */

/* 行首控制字节（只有**行首**才有意义；正文里的同值字节是普通数据）：
 *   0x01 = shell -> 内核的**服务请求**（如 "LS <path>"）
 *   0x02 = 内核 -> shell 的**服务应答**行（如 "D <name>" / "F <size> <name>" / "E <n>"） */

#define SH64_REQ_LS      0x01
#define SH64_RSP_LS      0x02

/* 服务应答的三种行（去掉行首的 0x02 之后的内容）：
 *   "D <name>"            子目录
 *   "F <size> <name>"     普通文件 + 字节数
 *   "E <n>"               结束：n = 0 正常；n != 0 = 失败（1 = 打不开/不是目录） */
#define SH64_LS_DIR      'D'
#define SH64_LS_FILE     'F'
#define SH64_LS_END      'E'

struct Sh64Mail {
    volatile uint32_t magic;      /* +0    见上 */
    volatile uint32_t in_w;       /* +4    内核写 */
    volatile uint32_t in_r;       /* +8    shell 读 */
    volatile uint32_t out_w;      /* +12   shell 写 */
    volatile uint32_t out_r;      /* +16   内核读 */
    volatile uint32_t pad[3];     /* +20..+31 保留（结构体布局固定，别改顺序） */
    char inb[SH64_IN_CAP];        /* +32   in 环 */
    char outb[SH64_OUT_CAP];      /* +544  out 环 */
};
/* 布局自检（用户侧编译期断言；内核侧的 static_assert 在 terminal64.cpp） */
typedef char sh64_layout_in_ok[(offsetof(struct Sh64Mail, inb) == 32) ? 1 : -1];
typedef char sh64_layout_out_ok[(offsetof(struct Sh64Mail, outb) == 544) ? 1 : -1];
typedef char sh64_layout_size_ok[(sizeof(struct Sh64Mail) <= 4096) ? 1 : -1];

#endif /* VIMTU64_SH64_H */
