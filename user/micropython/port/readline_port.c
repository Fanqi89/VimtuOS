/* readline_port.c - ★ v0.11.0：端口自己的 readline()（不做行编辑）
 *
 * 为什么不用 upstream 的 shared/readline/readline.c：
 *   * 它是完整的 emacs 行编辑器（MICROPY_REPL_EMACS_KEYS），会往 stdout 发
 *     ANSI 光标控制序列（MICROPY_HAL_HAS_VT100）。本内核的口径是
 *     “行编辑归终端侧，MicroPython 只逐字符读”（见 mpconfigport.h 里
 *     MICROPY_REPL_READLINE_SUPPORTED 0）。
 *   * 它还会 MP_REGISTER_ROOT_POINTER(readline_hist[...])，凭空给 GC 根加一个 8×指针表。
 *
 * 这里只做 upstream `readline()` 契约要求的最小集合：
 *     int readline(vstr_t *line, const char *prompt);
 *   返回  0                        -> 读到一整行（已写进 line，不含行尾）
 *         CHAR_CTRL_C (3)          -> 用户按了 Ctrl-C
 *         CHAR_CTRL_D (4)          -> 空行上按了 Ctrl-D（EOF 也映射成它）
 *
 * 支持的编辑：可见字符、退格/Delete(0x7f/0x08) 删一个字节、Ctrl-U 清行、
 * CR/LF 结束行（连续的 CRLF 只算一次结束）。
 * 不做：历史、光标左右移动、Tab 补全、自动缩进 —— 这些由内核侧的行编辑承担。
 */

#include <string.h>

#include "py/mpconfig.h"
#include "py/obj.h"
// vstr_t / vstr_add_byte / vstr_cut_tail_bytes 都在 py/misc.h 里（不是 py/vstr.h）
#include "py/misc.h"
#include "py/mphal.h"

#include "shared/readline/readline.h"

// 上一次 readline 是因为 CR 结束的；如果紧接着来的是 LF，要吞掉它，
// 否则内核把 "\r\n" 原样送上来时 REPL 会多出一个空行。
static bool last_end_was_cr;

int readline(vstr_t *line, const char *prompt) {
    mp_hal_stdout_tx_str(prompt);

    for (;;) {
        int c = mp_hal_stdin_rx_chr();

        if (c == '\r' || c == '\n') {
            if (c == '\n' && last_end_was_cr) {
                last_end_was_cr = false;
                continue; // 吞掉 CRLF 里的 LF
            }
            last_end_was_cr = (c == '\r');
            mp_hal_stdout_tx_str("\r\n");
            return 0;
        }
        last_end_was_cr = false;

        if (c == CHAR_CTRL_C) {
            mp_hal_stdout_tx_str("^C\r\n");
            return CHAR_CTRL_C;
        }
        if (c == CHAR_CTRL_D) {
            if (vstr_len(line) == 0) {
                return CHAR_CTRL_D; // 空行 EOF -> REPL 退出
            }
            continue; // 非空行上的 Ctrl-D：忽略（upstream 也是这个行为）
        }
        if (c == CHAR_CTRL_U) {
            while (vstr_len(line) > 0) {
                vstr_cut_tail_bytes(line, 1);
                mp_hal_stdout_tx_str("\b \b");
            }
            continue;
        }
        if (c == 0x7f || c == 0x08) { // Delete / Backspace
            if (vstr_len(line) > 0) {
                vstr_cut_tail_bytes(line, 1);
                mp_hal_stdout_tx_str("\b \b");
            }
            continue;
        }
        if (c < 0x20 || c == 0x7f) {
            continue; // 其它控制字符：丢弃（不往行里塞）
        }

        vstr_add_byte(line, (byte)c);
        char echo = (char)c;
        mp_hal_stdout_tx_strn(&echo, 1);
    }
}

// pyexec.c 之外还有两个 upstream 接口：这里给最小实现，保持符号齐全
// （pyexec 只在 friendly_repl 里用 readline()，其余没人调用）。
void readline_init0(void) {
    last_end_was_cr = false;
}

void readline_push_history(const char *line) {
    (void)line;
}
