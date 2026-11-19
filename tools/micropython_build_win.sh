#!/bin/bash
# tools/micropython_build_win.sh - ★ v0.11.0：在 MSYS2 上**手写构建** VimtuOS 版的 MicroPython
#
# 为什么手写（不走上游 ports/*/Makefile）：
#   * 上游 Makefile 用 mpy-cross 生成 qstr/genhdr，还要 host gcc + glibc；我们要的是
#     **静态 ELF64、非 PIC、按 4GiB+0x90000 定址**、链接我们自己的 musl libc.a ——
#     这套工具链与上游任何 target 都不同；
#   * 所以 qstr/genhdr 流水线（makeversionhdr / makeqstrdefs pp|split|cat /
#     makeqstrdata / makemoduledefs / make_root_pointers）在这里**逐条显式调用**，
#     不用 make、不用 mpy-cross。每一步的注释都写了"为什么必须这个顺序"。
#
# 产物（全部落 build64/，一个字节都不进内核镜像）：
#   build64/micropython.bin  真解释器（静态 ELF64，钉在 4GiB+0x90000）-> 卷里 /lib/micropython.bin
#   build64/mp               装载驱动（静态 ELF64，用户窗口低 64 KiB）  -> 卷里 /bin/mp
#   打包由 tools/micropython_pack_win.py 做（--drv build64/mp --bin build64/micropython.bin）。
#
# 用法：bash tools/micropython_build_win.sh [outdir]        # 缺省 outdir = build64
#       FLOAT_IMPL=none bash tools/micropython_build_win.sh # 关掉浮点（省 ~50 KiB）
set -e

# ★ PATH 必须在**任何外部命令之前**导好：调用方可能是一个没有 /usr/bin 的非登录 shell
#   （本脚本要自己扛住，不依赖 build64.sh 的调用环境）。
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"

MPSRC="third_party/micropython"
MUSL="third_party/musl"
PORT="user/micropython/port"
APPS="user/micropython"
OBJ="$OUT/micropython_obj"
GEN="$OBJ/genhdr"

# （PATH 已在文件头导出，这里不再重复）
CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

FLOAT_IMPL="${FLOAT_IMPL:-double}"        # double | none

if [ ! -f "$MPSRC/py/mpconfig.h" ]; then
    echo "找不到 MicroPython 源码：$MPSRC（third_party/micropython 是只读依赖）" >&2
    exit 2
fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
    echo "找不到 vendored musl：$MUSL/lib/libc.a" >&2
    exit 2
fi

case "$FLOAT_IMPL" in
    double) FLOAT_DEF="-DMICROPY_FLOAT_IMPL=2" ;;   # 2 = MICROPY_FLOAT_IMPL_DOUBLE
    none)   FLOAT_DEF="-DMICROPY_FLOAT_IMPL=0" ;;   # 0 = MICROPY_FLOAT_IMPL_NONE
    *) echo "FLOAT_IMPL 只能是 double 或 none（给的是 $FLOAT_IMPL）" >&2; exit 2 ;;
esac

mkdir -p "$OUT" "$OBJ" "$GEN"

# ---------------------------------------------------------------------------
# 1) 编译选项（与 tools/lua_build_win.sh:51-55 同一串，逐条理由照抄）：
#   -target x86_64-linux-gnu  目标 = x86_64 ELF64；VimtuOS 的 ring3 ABI 与 Linux x86_64 一致
#   -nostdinc + musl 头       我们自己的 musl 头（third_party/musl）
#   -mcmodel=large            映像在 4GiB：small/medium 会产生 R_X86_64_32/32S 绝对重定位，
#                             lld 直接报 out of range
#   -fno-pic -fno-pie         非 PIC 定址（装载驱动不做重定位）
#   -fno-stack-protector      不引 __stack_chk_fail
#   -Oz + gc-sections         体积（系统卷也是有限的）
#   -w                        上游代码的告警不当作本项目的交付标准
# ---------------------------------------------------------------------------
INC="-I$PORT -I$MPSRC -I$OBJ"
MUSL_INC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CFLAGS_MP="-target x86_64-linux-gnu -nostdinc $MUSL_INC $INC \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -D_XOPEN_SOURCE=700 -DNDEBUG -std=gnu99 -w -Oz -ffunction-sections -fdata-sections \
 $FLOAT_DEF"
# qstr 抽取阶段：同一套 CFLAGS + -DNO_QSTR（upstream 的 QSTR_GEN_FLAGS）
QSTR_CFLAGS="$CFLAGS_MP -DNO_QSTR"

# ---------------------------------------------------------------------------
# 2) 源文件清单
#    py/*.c  = py/py.mk 的 PY_CORE_O_BASENAME（132 个翻译单元，逐条同名，清单在 user/micropython/py_core_sources.txt）
#    额外    = shared/runtime（pyexec/stdout_helpers）+ 端口层 + 入口桩
#    ★ extmod 一个都不编：REPL 不需要；pyexec.c 只 include extmod/modplatform.h 这个**头**。
# ---------------------------------------------------------------------------
PY_CORE=$(cat "$APPS/py_core_sources.txt")
PY_SRCS=""        # 相对 $MPSRC：编译时写 "$MPSRC/$src"
QSTR_SRCS=""      # 相对 $ROOT：makeqstrdefs 直接从仓库根跑，路径一目了然
for n in $PY_CORE; do
    PY_SRCS="$PY_SRCS py/$n.c"
    case "$n" in
        nlr*) ;;                                   # py.mk 的 SRC_QSTR_IGNORE：nlr 里没有 qstr
        *)    QSTR_SRCS="$QSTR_SRCS $MPSRC/py/$n.c" ;;
    esac
done
SHARED_SRCS="shared/runtime/pyexec.c shared/runtime/stdout_helpers.c"
PORT_SRCS_C="$PORT/main.c $PORT/mphalport.c $PORT/readline_port.c"
QSTR_SRCS="$QSTR_SRCS $MPSRC/shared/runtime/pyexec.c $MPSRC/shared/runtime/stdout_helpers.c $PORT_SRCS_C"

objname() { echo "$1" | sed -e 's|.*/||' -e 's|\.c$|.o|' -e 's|\.S$|.o|'; }

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 1/7：生成 genhdr/mpversion.h（py/makeversionhdr.py）"
#   必须最先：py/modsys.c 与 shared/runtime/pyexec.c **无条件** include 它，而下一步就要
#   对这些 .c 做预处理（upstream 把它放在 QSTR_GLOBAL_REQUIREMENTS 里）。
# ---------------------------------------------------------------------------
$PY "$MPSRC/py/makeversionhdr.py" "$GEN/mpversion.h"
cat "$GEN/mpversion.h"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 2/7：qstr 抽取 —— clang -E -DNO_QSTR 全量预处理 -> qstr.i.last"
#   -DNO_QSTR 让 py/qstr.h / py/mpstate.h / py/objmodule.h 跳过 genhdr/*.h
#   （那三个头正是本阶段要生成的东西；先有鸡还是先有蛋的问题就靠这个宏解开）。
# ---------------------------------------------------------------------------
$PY "$MPSRC/py/makeqstrdefs.py" pp $CC -E \
    output "$OBJ/qstr.i.last" \
    cflags $QSTR_CFLAGS \
    sources $QSTR_SRCS \
    cxxflags $QSTR_CFLAGS \
    dependencies "$MPSRC/py/mpconfig.h" "$PORT/mpconfigport.h" \
    changed_sources
echo "    qstr.i.last = $(wc -l < "$OBJ/qstr.i.last") 行"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 3/7：切分 + 合并（makeqstrdefs.py split / cat）：qstr / module / root_pointer"
#   ★ 必须先清掉上一次的 split 目录：makeqstrdefs.py split 只给“非空结果”写文件，换配置
#     （例如 FLOAT_IMPL=none 让 py/modmath.c 整体被 #if 关掉）时旧文件会留在原地，cat 又把
#     上一次的模块注册拼回来 —— 链接期就报 undefined symbol: mp_module_math。
#     upstream 对同一件事的说明在 py/mpconfig.h:82（“切换配置必须 make -B / 从头重建”）。
# ---------------------------------------------------------------------------
rm -rf "$OBJ/qstr" "$OBJ/module" "$OBJ/root_pointer"
rm -f "$OBJ/qstrdefs.collected.h" "$OBJ/qstrdefs.collected.h.hash" \
      "$OBJ/moduledefs.collected" "$OBJ/moduledefs.collected.hash" \
      "$OBJ/root_pointers.collected" "$OBJ/root_pointers.collected.hash"
$PY "$MPSRC/py/makeqstrdefs.py" split qstr         "$OBJ/qstr.i.last" "$OBJ/qstr"         _
$PY "$MPSRC/py/makeqstrdefs.py" cat   qstr         _  "$OBJ/qstr"         "$OBJ/qstrdefs.collected.h"
$PY "$MPSRC/py/makeqstrdefs.py" split module       "$OBJ/qstr.i.last" "$OBJ/module"       _
$PY "$MPSRC/py/makeqstrdefs.py" cat   module       _  "$OBJ/module"       "$OBJ/moduledefs.collected"
$PY "$MPSRC/py/makeqstrdefs.py" split root_pointer "$OBJ/qstr.i.last" "$OBJ/root_pointer" _
$PY "$MPSRC/py/makeqstrdefs.py" cat   root_pointer _  "$OBJ/root_pointer" "$OBJ/root_pointers.collected"
echo "    qstrdefs.collected.h    = $(wc -l < "$OBJ/qstrdefs.collected.h") 行 Q()"
echo "    moduledefs.collected    = $(wc -l < "$OBJ/moduledefs.collected") 条 MP_REGISTER_MODULE"
echo "    root_pointers.collected = $(wc -l < "$OBJ/root_pointers.collected") 条 MP_REGISTER_ROOT_POINTER"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 4/7：生成 qstrdefs.generated.h / moduledefs.h / root_pointers.h"
#   qstrdefs.generated.h 的两段 sed 是 upstream 的“保护 / 还原”技巧：
#     1) 把每个 Q(xxx) 包成 "Q(xxx)" 字符串 -> 让 clang -E 只做条件编译、不动内容
#     2) 预处理后再把引号剥掉 -> makeqstrdata.py 只认 Q(...) 行
#   端口私有的 qstr 走 user/micropython/port/qstrdefsport.h（见该文件的注释）。
# ---------------------------------------------------------------------------
cat "$MPSRC/py/qstrdefs.h" "$PORT/qstrdefsport.h" "$OBJ/qstrdefs.collected.h" \
    | sed 's/^Q(.*)/"&"/' \
    | $CC -E $CFLAGS_MP - \
    | sed 's/^"\(Q(.*)\)"/\1/' \
    > "$GEN/qstrdefs.preprocessed.h"
$PY "$MPSRC/py/makeqstrdata.py" "$GEN/qstrdefs.preprocessed.h" > "$GEN/qstrdefs.generated.h"
$PY "$MPSRC/py/makemoduledefs.py"     "$OBJ/moduledefs.collected"     > "$GEN/moduledefs.h"
$PY "$MPSRC/py/make_root_pointers.py" "$OBJ/root_pointers.collected" > "$GEN/root_pointers.h"
echo "    qstrdefs.generated.h    = $(grep -c '^QDEF' "$GEN/qstrdefs.generated.h") 条 qstr"
echo "    moduledefs.h            = $(wc -l < "$GEN/moduledefs.h") 行"
echo "    root_pointers.h         = $(wc -l < "$GEN/root_pointers.h") 行"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 5/7：编译 py 核心 + shared/runtime + 端口层（clang -Oz）"
# ---------------------------------------------------------------------------
# 入口桩：musl 的 __libc_start_main（理由见 port/mp_start.S）
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$PORT/mp_start.S" -o "$OBJ/mp_start.o"
$CC $CFLAGS_MP -c "$PORT/mp_start.c" -o "$OBJ/mp_start_c.o"
# vendored musl 缺 fabs/fabsf、compiler-rt 的 half-float 内建也不在 libc.a 里：
# 必须 -ffreestanding -fno-builtin 单独编，否则编译器把 fabs 识别成内建、不产出符号。
$CC -target x86_64-linux-gnu -nostdinc -ffreestanding -fno-builtin -fno-pic -fno-pie \
    -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=gnu99 -O2 \
    -c "$PORT/mp_compat.c" -o "$OBJ/mp_compat.o"
ALL_OBJS="$OBJ/mp_start.o $OBJ/mp_start_c.o $OBJ/mp_compat.o"
n=0
for src in $PY_SRCS $SHARED_SRCS; do
    o="$OBJ/$(objname "$src")"
    extra=""
    case "$o" in
        */nlrx64.o) extra="-Os" ;;   # py.mk: nlr 固定 -Os，保证前导代码形状可预测
    esac
    $CC $CFLAGS_MP $extra -c "$MPSRC/$src" -o "$o"
    ALL_OBJS="$ALL_OBJS $o"
    n=$((n + 1))
done
for src in $PORT_SRCS_C; do
    o="$OBJ/$(objname "$src")"
    $CC $CFLAGS_MP -c "$src" -o "$o"
    ALL_OBJS="$ALL_OBJS $o"
    n=$((n + 1))
done
echo "    编译了 $n 个 .c（+ 1 个 .S 入口桩 + mp_compat.o）"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 6/7：链接解释器（静态 musl libc.a，钉在 4GiB+0x90000）+ 装载驱动"
# ---------------------------------------------------------------------------
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$PORT/mp64.ld" \
    -o "$OUT/micropython.bin" $ALL_OBJS "$MUSL/lib/libc.a"

# 驱动：-nostdlib、不引 libc（系统调用全部内联汇编；与 /bin/lua 同一套编译口径）
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/mpdrv.c" -o "$OBJ/mpdrv.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/mpdrv_start.S" -o "$OBJ/mpdrv_start.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/mpdrv64.ld" \
    -o "$OUT/mp" "$OBJ/mpdrv_start.o" "$OBJ/mpdrv.o"

# ---------------------------------------------------------------------------
echo "==> ★ v0.11.0 7/7：构建期硬断言（两个 ELF，逐条对应内核装载器与驱动的约束）"
# ---------------------------------------------------------------------------
$PY tools/micropython_check_elf.py "$OUT/mp" "$OUT/micropython.bin"

echo
echo "    --- 体积记账（全都进系统卷，不进内核）---"
echo "    $OUT/micropython.bin = $(stat -c%s "$OUT/micropython.bin") B   （卷里 /lib/micropython.bin，静态 musl）"
echo "    $OUT/mp              = $(stat -c%s "$OUT/mp") B   （卷里 /bin/mp，内核主程序装载器直接装）"
echo "    打包：py -3 tools/micropython_pack_win.py --vol-in <卷> --vol-out <卷> --drv $OUT/mp --bin $OUT/micropython.bin"
