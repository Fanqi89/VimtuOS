/* mpconfigport.h - ★ v0.11.0：VimtuOS 上的 MicroPython 端口配置
 *
 * 口径：ports/minimal/mpconfigport.h 为蓝本，但
 *   * ROM 级别从 MINIMUM 提到 CORE_FEATURES（MINIMUM 会关掉 MICROPY_ENABLE_COMPILER，
 *     那样就没有 REPL/编译器了；CORE 层给的正是本端口要的那套：print / for / int / str /
 *     list / dict / range / def / class / import(builtin)）；
 *   * REPL 打开（MICROPY_HELPER_REPL 1），但**不做行编辑**：行编辑归内核侧，
 *     端口自己实现的 readline()（port/readline_port.c）只逐字符读 + 回显。
 *
 * 注意 include 时机：本文件在 py/mpconfig.h:100 就被 include，那时
 * MICROPY_FLOAT_IMPL_DOUBLE 等符号还没定义（它们在 mpconfig.h:975 之后），
 * 所以这里只能用字面量 0/2 —— 宏体是延迟展开的，写符号名也能工作，但字面量更明确。
 */

#ifndef MICROPY_INCLUDED_VIMTUOS_MPCONFIGPORT_H
#define MICROPY_INCLUDED_VIMTUOS_MPCONFIGPORT_H

#include <stdint.h>

// ---------------------------------------------------------------------------
// ROM 级别
// ---------------------------------------------------------------------------
#define MICROPY_CONFIG_ROM_LEVEL (MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES)

// ---------------------------------------------------------------------------
// 必须打开的
// ---------------------------------------------------------------------------
#define MICROPY_ENABLE_COMPILER (1)          // 没有编译器就没有 REPL
#define MICROPY_ENABLE_GC (1)
#define MICROPY_HELPER_REPL (1)              // py/repl.c（>>> 提示符/续行判定）
#define MICROPY_REPL_READLINE_SUPPORTED (0)  // 行编辑归内核侧；见 port/readline_port.c
#define MICROPY_REPL_EMACS_KEYS (0)
#define MICROPY_REPL_EMACS_WORDS_MOVE (0)
#define MICROPY_REPL_EMACS_EXTRA_WORDS_MOVE (0)
#define MICROPY_REPL_AUTO_INDENT (0)
#define MICROPY_HAL_HAS_VT100 (0)            // 端口不发 ANSI 光标控制序列

// 没有文件系统/没有 frozen 字节码（第三种 import 路径全部关掉）
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)
#define MICROPY_MODULE_FROZEN_MPY (0)
#define MICROPY_MODULE_FROZEN_STR (0)

// 错误字符串压缩：关掉可以整整省掉 makecompresseddata.py 这一段流水线，
// 代价是 MP_ERROR_TEXT 字符串在 ROM 里是原文（体积略大，实测只有几 KiB）。
#define MICROPY_ROM_TEXT_COMPRESSION (0)

// ---------------------------------------------------------------------------
// 明确关掉的（体积 + 依赖面）
// 这些模块的实现都在 extmod/ 里，本端口**不编译任何 extmod**，
// 这里再显式写一遍，避免以后有人把 extmod 加进来时“悄悄变大”。
// ---------------------------------------------------------------------------
#define MICROPY_PY_THREAD (0)
#define MICROPY_PY_USELECT (0)
#define MICROPY_PY_FFI (0)
#define MICROPY_PY_BTREE (0)
#define MICROPY_PY_WEBSOCKET (0)
#define MICROPY_PY_UCRYPTOLIB (0)
#define MICROPY_PY_MACHINE (0)
#define MICROPY_PY_UOS (0)

// sys 模块：跟 minimal 端口一样全关（没有文件系统，路径/argv 没有意义）
#define MICROPY_PY_SYS_MODULES (0)
#define MICROPY_PY_SYS_EXIT (0)
#define MICROPY_PY_SYS_PATH (0)
#define MICROPY_PY_SYS_ARGV (0)
#define MICROPY_PY_SYS_PS1_PS2 (0)
#define MICROPY_PY_SYS_STDFILES (0)
#define MICROPY_PY_SYS_STDIO_BUFFER (0)

#define MICROPY_PY_BUILTINS_HELP (0)

// 需要 libm 的“花哨”数学函数（CORE 层本来就没开，写死以稳定体积）
#define MICROPY_PY_MATH_SPECIAL_FUNCTIONS (0)
#define MICROPY_PY_MATH_CONSTANTS (0)
#define MICROPY_PY_MATH_ISCLOSE (0)
#define MICROPY_PY_MATH_FACTORIAL (0)

// 不要 finaliser / 栈检查（后者需要内核给栈边界，本端口不给）
#define MICROPY_ENABLE_FINALISER (0)
#define MICROPY_STACK_CHECK (0)

// ---------------------------------------------------------------------------
// 浮点
//   2 = MICROPY_FLOAT_IMPL_DOUBLE（默认）
//   0 = MICROPY_FLOAT_IMPL_NONE  （构建脚本 FLOAT_IMPL=none 时用 -D 覆盖）
// 依据：vendored musl 的 libc.a 里 pow/sqrt/fmod/floor/ceil/exp/log/sin/cos/atan2/
// modf/frexp/tgamma 都在，只缺 fabs/fabsf（port/mp_compat.c 补）。
// ---------------------------------------------------------------------------
#ifndef MICROPY_FLOAT_IMPL
#define MICROPY_FLOAT_IMPL (2)
#endif

// 无浮点（FLOAT_IMPL=none）时必须同时关掉 math/cmath：
//   py/modmath.c / py/modcmath.c 的“实体”整体在 MICROPY_PY_BUILTINS_FLOAT 下，
//   而 MP_REGISTER_MODULE(MODULE_MATH, mp_module_math) 在 MICROPY_PY_MATH 下 ——
//   只关浮点不关 math，链接期 objmodule.o 会报 undefined symbol: mp_module_math。
//   （CORE 层默认 MICROPY_PY_MATH=1 —— 这是固化过程中实测踩到的坑之一。）
#if MICROPY_FLOAT_IMPL == 0
#undef MICROPY_PY_MATH
#define MICROPY_PY_MATH (0)
#undef MICROPY_PY_CMATH
#define MICROPY_PY_CMATH (0)
#endif

// ---------------------------------------------------------------------------
// 机器相关
// ---------------------------------------------------------------------------
typedef long mp_off_t;

// MicroPython 的 py/mpconfig.h 用到 alloca（mp_vla 之类）
#include <alloca.h>

#define MICROPY_HW_BOARD_NAME "vimtu64-mp"
#define MICROPY_HW_MCU_NAME "x86_64"

// GC 堆：静态 .bss 数组（见 port/main.c）。4 MiB 是“好用”和“窗口够”的折中：
// 用户窗口 16 MiB = 映像 span(含这块 .bss) + 512 KiB 栈（驱动里 mmap）；
// 映像 4.5 MiB + 栈 0.5 MiB 还剩 ~10.9 MiB 给 musl 的 malloc（MicroPython 的
// qstr/vstr 走 libc malloc）。
#define MICROPY_HEAP_SIZE (4 * 1024 * 1024)

// MicroPython 的 C 栈保护上限（MICROPY_STACK_CHECK=0 时是空操作，这里留个口径）
#define MICROPY_STACK_LIMIT_BYTES (448 * 1024)

#define MP_STATE_PORT MP_STATE_VM

#endif // MICROPY_INCLUDED_VIMTUOS_MPCONFIGPORT_H
