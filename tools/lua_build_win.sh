#!/bin/bash
# tools/lua_build_win.sh - ★ A4-4a：在 MSYS2 上**手写构建** VimtuOS 版的 Lua 5.4.7
#
# 为什么手写（不走上游 src/Makefile）：
#   * 上游 Makefile 的 "linux" 目标假设宿主 GCC + glibc，而且会编出宿主可执行文件；我们要的是
#     **静态 ELF64、非 PIC、按 4GiB+0x90000 定址**、链接我们自己的 musl libc.a —— 这套工具链
#     与上游任何目标都不同；
#   * 上游目标会在 x86_64 上链 readline（LUA_USE_READLINE）/ dlopen（LUA_USE_DLOPEN）——
#     VimtuOS 都没有，必须走**默认配置**（不加 LUA_USE_LINUX/POSIX）：核心只用 C89 libc，
#     这正好落在 musl 的能力范围内（见 docs 的 A4-4 节"缺什么/怎么规避"）。
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/lua.bin   真解释器（静态 ELF64，钉在 4GiB+0x90000）-> 卷里 /lib/lua.bin
#   build64/lua       装载驱动（静态 ELF64，用户窗口低 64 KiB）  -> 卷里 /bin/lua
#
# 用法：bash tools/lua_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"

LUASRC="third_party/lua/lua-5.4.7/src"
MUSL="third_party/musl"
APPS="user/lua"
OBJ="$OUT/lua_obj"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

if [ ! -f "$LUASRC/lua.c" ]; then
    echo "找不到 Lua 源码：$LUASRC（third_party/lua/README-vimtu64-a4.md 有来源与许可）" >&2
    exit 2
fi

mkdir -p "$OUT" "$OBJ"

# ---- 1) 解释器各翻译单元 ----
# 逐条选项的理由：
#   -target x86_64-linux-gnu  目标 = x86_64 ELF64；VimtuOS 的 ring3 ABI 与 Linux x86_64 一致
#   -nostdinc + musl 头       我们自己的 musl 头（third_party/musl）
#   -mcmodel=large            映像在 4GiB：small/medium 会产生 R_X86_64_32/32S 绝对重定位，lld
#                             直接报 out of range（tcc 的同一课）
#   -fno-pic -fno-pie         非 PIC 定址（装载驱动不做重定位）
#   -fno-stack-protector      不引 __stack_chk_fail
#   -Oz + gc-sections         体积（系统卷也是有限的）
#   -w                        上游代码的告警不当作本项目的交付标准（驱动/装载器才是 -Wextra 零告警）
CFLAGS_LUA="-target x86_64-linux-gnu -nostdinc \
 -isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include \
 -I$LUASRC -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 \
 -std=gnu99 -w -Oz -ffunction-sections -fdata-sections"

LUA_TUS="lapi.c lauxlib.c lbaselib.c lcode.c lcorolib.c lctype.c ldblib.c ldebug.c ldo.c ldump.c \
 lfunc.c lgc.c linit.c liolib.c llex.c lmathlib.c lmem.c loadlib.c lobject.c lopcodes.c loslib.c \
 lparser.c lstate.c lstring.c lstrlib.c ltable.c ltablib.c ltm.c lundump.c lutf8lib.c lvm.c lzio.c \
 lua.c"

echo "==> ★ A4-4a 1/4：编译 Lua 5.4.7（$(echo $LUA_TUS | wc -w) 个翻译单元；默认配置，无 readline/dlopen）"
OBJS=""
for tu in $LUA_TUS; do
    o="$OBJ/${tu%.c}.o"
    $CC $CFLAGS_LUA -c "$LUASRC/$tu" -o "$o"
    OBJS="$OBJS $o"
done
# 入口桩：musl 的 __libc_start_main（自写 _start，理由见 user/lua/lua_start.S）
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$APPS/lua_start.S" -o "$OBJ/lua_start.o"
$CC -target x86_64-linux-gnu -nostdinc -isystem $MUSL/include -isystem $MUSL/arch/x86_64 \
    -isystem $MUSL/obj/include -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -std=gnu99 -Wall -Wextra -c "$APPS/lua_start.c" -o "$OBJ/lua_start_c.o"

 echo "==> ★ A4-4a 2/4：链接解释器（静态 musl libc.a，钉在 4GiB+0x90000）"
 # vendored musl 归档缺 fabs/fabsf（上游 fabs.c 在宿主 clang 下被当内建、没落盘成成员）——
 # 用自写的同语义实现补上（理由与实现见 user/lua/lua_compat.c）。必须 -fno-builtin，
 # 否则编译器又把它识别成内建、不产出符号。
 $CC -target x86_64-linux-gnu -nostdinc -ffreestanding -fno-builtin -fno-pic -fno-pie \
     -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
     -ffunction-sections -fdata-sections -std=gnu99 -O2 -Wall -Wextra \
     -c "$APPS/lua_compat.c" -o "$OBJ/lua_compat.o"
 $LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/lua64.ld" \
     -o "$OUT/lua.bin" "$OBJ/lua_start.o" "$OBJ/lua_start_c.o" "$OBJ/lua_compat.o" $OBJS "$MUSL/lib/libc.a"
 
 echo "==> ★ A4-4a 3/4：装载驱动 /bin/lua（< 64 KiB，走内核主程序装载器）"
 $CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
     -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
     -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
     -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
     -c "$APPS/luadrv.c" -o "$OBJ/luadrv.o"
 $CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
     -ffunction-sections -fdata-sections -w -c "$APPS/luadrv_start.S" -o "$OBJ/luadrv_start.o"
 $LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/luadrv64.ld" \
     -o "$OUT/lua" "$OBJ/luadrv_start.o" "$OBJ/luadrv.o"
# ---- 4) 构建期硬断言（直接解析 ELF 头/程序头，逐条对应内核 elf64.cpp 与驱动的约束）----
echo "==> ★ A4-4a 4/4：构建期断言（ELF 头/程序头；逐条对应内核加载器与驱动的硬约束）"
$PY - "$OUT/lua" "$OUT/lua.bin" <<'PYEOF'
import os, struct, sys

BASE64   = 0x100000000             # USER64_CODE_VA64
STACK64  = 0x100000000 + 0x10000   # USER64_STACK_VA64
MMAP_VA  = 0x100000000 + 0x90000   # USER64_MMAP_VA64
WINDOW_TOP = 0x100000000 + 16 * 1024 * 1024
SPAN_MAX = WINDOW_TOP - MMAP_VA

def phdrs(d):
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    out = []
    for i in range(phn):
        q = phoff + i * phes
        t = struct.unpack_from("<I", d, q)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, q + 8)
        out.append((t, off, va, fsz, msz, al))
    return phoff, phes, phn, out

def check_driver(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "驱动不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "驱动必须是 ET_EXEC / x86_64"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    assert 0 < phn <= 16, "驱动 e_phnum 必须 <= 16"
    first = None
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: 驱动出现 PT_INTERP")
        if t == 2: raise SystemExit("ERROR: 驱动出现 PT_DYNAMIC")
        if t != 1: continue
        if first is None:
            first = (off, fsz)
        assert va >= BASE64 and va + msz <= STACK64, "驱动 PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x memsz=%#x）" % (va, msz)
    assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
        "驱动的程序头表不在第一个 PT_LOAD 里（auxv AT_PHDR 会是 0）"
    assert len(d) <= 96 * 1024, "驱动超过内核读盘缓冲 96 KiB"
    print("    断言 OK：驱动 %s = %d B，entry=%#x phnum=%d" % (os.path.basename(path), len(d), entry, phn))

def check_lua(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "lua.bin 不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "lua.bin 必须是 ET_EXEC / x86_64"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    assert 0 < phn <= 16, "lua.bin e_phnum 必须 <= 16"
    hi = MMAP_VA
    first = None
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: lua.bin 出现 PT_INTERP（驱动不做重定位）")
        if t != 1: continue
        assert va >= MMAP_VA, "lua.bin 的 PT_LOAD 低于 USER64_MMAP_VA64（驱动装载不了）"
        assert va + msz <= WINDOW_TOP, "lua.bin 的 PT_LOAD 越出用户窗口"
        if first is None:
            first = (off, va, fsz)
        hi = max(hi, va + msz)
    assert first is not None and first[0] == 0 and first[1] == MMAP_VA, \
        "lua.bin 的第一个 PT_LOAD 必须 off=0 / va=USER64_MMAP_VA64（驱动算 AT_PHDR 用）"
    span = hi - MMAP_VA
    assert span <= SPAN_MAX, "lua.bin 的 span %d B 超过窗口里的 mmap 区" % span
    assert phoff < first[2], "程序头表不在第一个 PT_LOAD 的文件范围内"
    print("    断言 OK：lua %s = %d B，entry=%#x phnum=%d span=%d B（%.1f KiB；mmap 区剩 %d B）"
          % (os.path.basename(path), len(d), entry, phn, span, span / 1024.0, SPAN_MAX - span))

for p in sys.argv[1:]:
    if p.endswith("/lua"):
        check_driver(p)
    else:
        check_lua(p)
PYEOF

echo
echo "    --- A4-4a 体积记账（全都进系统卷，不进内核）---"
echo "    build64/lua.bin = $(stat -c%s "$OUT/lua.bin") B   （卷里 /lib/lua.bin，静态 musl）"
echo "    build64/lua     = $(stat -c%s "$OUT/lua") B   （卷里 /bin/lua，内核主程序装载器直接装）"

# ---------------------------------------------------------------------------
# ★ A4-4b：两个**新系统调用**的 ring3 探针（utime(132) 显式时间 / rename(82) 跨目录）——
#   /bin/a44probe（< 64 KiB，走内核主程序装载器；由 tests/lua64_test.py 装进夹具卷并断言）。
# 口径与 build64.sh 里的 a42a 探针一致：syscall 指令 + Linux 号段 + 自足（不依赖 libc）。
# ---------------------------------------------------------------------------
echo "==> ★ A4-4b：编译两个新系统调用的 ring3 探针（a44probe）"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/a44probe.c" -o "$OBJ/a44probe.o"
$LD -m elf_x86_64 -T user/lib/user64.ld --gc-sections -o "$OUT/a44probe" "$OBJ/a44probe.o"
A44SZ=$(stat -c%s "$OUT/a44probe")
if [ "$A44SZ" -gt 65536 ]; then
    echo "ERROR: a44probe $A44SZ B 超过 64KiB（用户窗口装载区上限）" >&2
    exit 1
fi
echo "    build64/a44probe = $A44SZ B（卷里 /bin/a44probe；由 tests/lua64_test.py 断言两条新系统调用）"
