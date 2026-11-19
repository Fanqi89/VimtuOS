/* mphalport.h - ★ v0.11.0：VimtuOS 端的 HAL 声明（py/mphal.h 用 <mphalport.h> 找它）
 *
 * 只做三件事：fd 0 阻塞读一个字节、fd 1 写、以及把“引脚/中断字符”这类
 * 本内核没有的东西定义成空壳，免得 py/mphal.h 去 include extmod/virtpin.h。
 */

#ifndef MICROPY_INCLUDED_VIMTUOS_MPHALPORT_H
#define MICROPY_INCLUDED_VIMTUOS_MPHALPORT_H

#include "py/mpconfig.h"

// 没有任何 GPIO：把 pin 抽象定成整型，避免 py/mphal.h 拉 extmod/virtpin.h
typedef int mp_hal_pin_obj_t;

// 内核控制台没有“中断字符”概念：进程收到 Ctrl-C 就是普通字节 0x03。
// pyexec.c 会调用 mp_hal_set_interrupt_char(CHAR_CTRL_C)，这里做成空操作。
static inline void mp_hal_set_interrupt_char(char c) {
    (void)c;
}

// 本内核没有实现 clock_gettime，也没有 modtime 模块；给个常量即可。
static inline mp_uint_t mp_hal_ticks_ms(void) {
    return 0;
}

#endif // MICROPY_INCLUDED_VIMTUOS_MPHALPORT_H
