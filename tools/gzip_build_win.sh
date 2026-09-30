#!/bin/bash
# tools/gzip_build_win.sh - ★ A4-4c：构建用户态 gzip/gunzip（Ring 3，静态 ELF64）
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/gzip   <64 KiB 的静态 ELF64，链接在用户窗口低 64 KiB（4GiB 起）
#                  -> 卷里 /bin/gzip 与 /bin/gunzip（**同一份字节两个名字**，busybox 风格：
#                     程序按 argv[0] 里是否含 "gunzip" 决定默认方向）
#
# 为什么自足、不引 user/lib 或 musl：见 user/gzip/gzip.c 头部（主程序装载器 = 64 KiB 上限）。
# 用法：bash tools/gzip_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
APPS="user/gzip"
OBJ="$OUT/gzip_obj"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

mkdir -p "$OUT" "$OBJ"

echo "==> ★ A4-4c 1/3：编译 gzip.c（裸机目标，-Wall -Wextra 零告警）"
# -nostdinc -ffreestanding -nostdlib：不引任何头/库（系统调用内联汇编，字符串函数自写）
# -mcmodel=large：映像在 4GiB（small/medium 会产生 R_X86_64_32/32S 越界重定位）
# -mno-sse*/mno-mmx/mno-avx：内核与用户态都不开 FP/SSE（别让编译器偷偷用 xmm）
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/gzip.c" -o "$OBJ/gzip.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/gzip_start.S" -o "$OBJ/gzip_start.o"

echo "==> ★ A4-4c 2/3：链接（静态 ELF64，4GiB 起，内核主程序装载器直接装）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/gzip64.ld" \
    -o "$OUT/gzip" "$OBJ/gzip_start.o" "$OBJ/gzip.o"

echo "==> ★ A4-4c 3/3：构建期断言（ELF 头/程序头 + 宿主 Python zlib 互操作自检）"
$PY - "$OUT/gzip" "$OUT/gzip_obj/gzip.o" <<'PYEOF'
import os, struct, subprocess, sys

BASE64  = 0x100000000             # USER64_CODE_VA64
STACK64 = 0x100000000 + 0x10000   # USER64_STACK_VA64（装载区上界，排他）

path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "gzip 不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "gzip 必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert 0 < phn <= 16 and phes == 56, "程序头表不合法"
first = None
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
    if t == 3: raise SystemExit("ERROR: gzip 出现 PT_INTERP（内核只做静态装载）")
    if t == 2: raise SystemExit("ERROR: gzip 出现 PT_DYNAMIC")
    if t != 1: continue
    assert va >= BASE64 and va + msz <= STACK64, "PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x msz=%#x）" % (va, msz)
    if first is None:
        first = (off, fsz)
assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
    "程序头表不在第一个 PT_LOAD 的文件范围内（auxv AT_PHDR 会是 0）"
assert len(d) <= 64 * 1024, "gzip 超过 64 KiB（主程序装载窗口）"
print("    断言 OK：%s = %d B，entry=%#x phnum=%d（static ET_EXEC，4GiB 装载区）"
      % (os.path.basename(path), len(d), entry, phn))
PYEOF

echo
echo "    --- A4-4c 体积记账（进系统卷 /bin/gzip 与 /bin/gunzip，两份同名，不进内核）---"
echo "    build64/gzip = $(stat -c%s "$OUT/gzip") B"
