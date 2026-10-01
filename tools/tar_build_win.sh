#!/bin/bash
# tools/tar_build_win.sh - ★ B5：在 MSYS2 上构建 VimtuOS 版的 **tar**（/bin/tar）
#
# 与 tools/musl_build_win.sh 同一条路线（静态 musl、非 PIC、4GiB 装载区、自写 _start），
# 但产出的是一份**可以直接被内核主程序装载器装载**的 < 64 KiB 程序（与 /bin/gzip、/bin/edit 同款）：
#   * musl 头：third_party/musl/{include,arch/x86_64,arch/generic,obj/include}
#   * musl 静态库：third_party/musl/lib/libc.a（tools/musl_build_win.sh 会重建它）
#   * 链接脚本：user/tar/tar64.ld（= tools/musl_hello64.ld 的拷贝 + 说明；**.got 不能丢**）
#   * 自写 _start：user/tar/tar_start.S / tar_start.c（musl 的 crt1.o 在 -static 下链接不过）
#   * --gc-sections + -Oz：只把真正用到的 musl 目标文件链进来（libc.a 有 1300+ 个成员）
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/tar   -> 卷里 /bin/tar
#
# 用法：bash tools/tar_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"

MUSL="third_party/musl"
APPS="user/tar"
OBJ="$OUT/tar_obj"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

if [ ! -f "$APPS/tar64.c" ]; then echo "找不到 tar 源码：$APPS/tar64.c" >&2; exit 2; fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
    echo "找不到 musl 静态库：$MUSL/lib/libc.a（先跑 tools/musl_build_win.sh）" >&2
    exit 2
fi

mkdir -p "$OUT"
rm -rf "$OBJ"
mkdir -p "$OBJ"

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 \
 -std=gnu99 -Oz -ffunction-sections -fdata-sections -Wall -Wextra"

echo "==> ★ B5-tar 1/3：编译 tar（musl 头；-Oz；Wall -Wextra 零告警）"
$CC $CFLAGS -c "$APPS/tar64.c" -o "$OBJ/tar.o"
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$APPS/tar_start.S" -o "$OBJ/tar_start.o"
$CC -target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
    -fno-stack-protector -fno-asynchronous-unwind-tables -std=gnu99 -O2 -Wall -Wextra \
    -c "$APPS/tar_start.c" -o "$OBJ/tar_start_c.o"

echo "==> ★ B5-tar 2/3：链接（静态 musl libc.a，4GiB 装载区）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/tar64.ld" \
    -o "$OUT/tar" "$OBJ/tar_start.o" "$OBJ/tar_start_c.o" "$OBJ/tar.o" "$MUSL/lib/libc.a"

echo "==> ★ B5-tar 3/3：构建期断言（内核 elf64.cpp 的硬门槛）"
$PY - "$OUT/tar" <<'PYEOF'
import os, struct, sys

BASE64  = 0x100000000
STACK64 = 0x100000000 + 0x10000
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "tar 不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "tar 必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert 0 < phn <= 16 and phes == 56, "程序头表不合法"
first = None
nload = 0
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
    if t == 3: raise SystemExit("ERROR: tar 出现 PT_INTERP（内核只做静态装载）")
    if t == 2: raise SystemExit("ERROR: tar 出现 PT_DYNAMIC")
    if t != 1: continue
    nload += 1
    assert va >= BASE64 and va + msz <= STACK64, "PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x msz=%#x）" % (va, msz)
    assert (va + msz + 0xFFF) & ~0xFFF <= STACK64, "PT_LOAD 页对齐后压到用户栈"
    if first is None:
        first = (off, fsz)
assert nload > 0
assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
    "程序头表不在第一个 PT_LOAD 的文件范围内（auxv AT_PHDR 会是 0）"
# 内核的**真正**上限（kernel/elf64.h）：读盘/解析缓冲 ELF64_MAX_FILE_BYTES64 = 96 KiB；
# 而"PT_LOAD（含 .bss 的 p_memsz）必须落在用户窗口低 64 KiB"由上面的逐段断言钉住。
assert len(d) <= 96 * 1024, "tar 超过内核读盘缓冲 96 KiB（ELF64_MAX_FILE_BYTES64）"
print("    断言 OK：tar = %d B，entry=%#x phnum=%d segs=%d（静态 musl，4GiB 装载区）"
      % (len(d), entry, phn, nload))
PYEOF

echo
echo "    --- B5-tar 体积记账（进系统卷，不进内核）---"
echo "    build64/tar = $(stat -c%s "$OUT/tar") B   （卷里 /bin/tar）"
