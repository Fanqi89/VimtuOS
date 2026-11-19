/* mphalport.c - ★ v0.11.0：VimtuOS 端的 MicroPython HAL
 *
 * 全部语义就是“fd 0 阻塞读一个字节 / fd 1 写一串字节”，没有 termios、没有 select。
 * 走 musl 的 read()/write()（= VimtuOS 的 syscall 0/1，与 /bin/lua 驱动同一条路）。
 *
 * 失败模式：
 *   * read() 返回 0（fd 0 到了 EOF / 内核读语义是“没数据就返回 0”）：
 *     当作 Ctrl-D（=4）交回去，REPL 会优雅退出而不是死循环空转。
 *     这一点在报告里单独列为“需要内核侧确认”的接口（fd 0 必须**阻塞**到至少 1 字节）。
 *   * read() 出错（非 EINTR）：同上，Ctrl-D。
 */

#include <errno.h>
#include <unistd.h>

#include "py/mpconfig.h"
#include "py/mphal.h"

#include "shared/readline/readline.h"

int mp_hal_stdin_rx_chr(void) {
    unsigned char c = 0;
    for (;;) {
        ssize_t n = read(0, &c, 1);
        if (n == 1) {
            return (int)c;
        }
        if (n == 0) {
            // fd 0 的“空读”：内核没有数据时返回 0。REPL 不能忙等 —— 交回 Ctrl-D。
            return CHAR_CTRL_D;
        }
        if (errno != EINTR) {
            return CHAR_CTRL_D;
        }
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    size_t left = len;
    while (left) {
        ssize_t n = write(1, str, left);
        if (n > 0) {
            str += n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break; // 真错误：放弃剩余字节（REPL 不能卡死）
    }
    return (mp_uint_t)(len - left);
}

// 本内核没有 modtime 模块调用它；给一个安全的空转实现，避免链接期再拉 nanosleep。
void mp_hal_delay_ms(mp_uint_t ms) {
    (void)ms;
}
