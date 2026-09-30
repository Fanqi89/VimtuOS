#!/bin/bash
# build_edit.sh - ★ A4-5：构建 Ring 3 编辑器（/bin/edit；静态 ELF64，走自研 user/lib）
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/edit   静态 ELF64，链接在用户窗口低 64 KiB（4GiB 起）-> 卷里 /bin/edit
#
# 与其它用户程序的差别（为什么不能直接用 user/build_user.sh）：
#   * build_user.sh 产出的是**平铺 blob**（user/lib/crt0.S 合成 argc=1/argv[0]="user64"，
#     给内核的 blob 加载器用）。编辑器是**从系统卷装载的 ELF64**，必须拿到真 argv
#     （要打开的文件名在里面），所以用自己的入口桩 edit_start.S + edit64.ld。
#   * user/lib/crt0.S 因此**不链**（链了会重定义 _start）；其余 lib 文件照链。
#
# 用法：bash user/apps/edit/build_edit.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
OBJ="$OUT/edit_obj"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang --target=x86_64-unknown-none-elf"
LD=ld.lld

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

mkdir -p "$OUT" "$OBJ"

UCFLAGS="--target=x86_64-unknown-none-elf -nostdinc -I user/lib \
 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone -mno-sse -mno-sse2 -mno-mmx -mno-avx \
 -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra"
ASFLAGS="--target=x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
 -ffunction-sections -fdata-sections -Wall -Wextra"

echo "==> ★ A4-5 1/3：编译 user/lib（自研最小 libc；不链 crt0.S —— 见文件头）+ 编辑器"
LIBOBJS=""
for src in syscall.c string.c stdlib.c stdio.c fb.c; do
    $CC $UCFLAGS -c "user/lib/$src" -o "$OBJ/lib_${src%.c}.o"
    LIBOBJS="$LIBOBJS $OBJ/lib_${src%.c}.o"
done
$CC $ASFLAGS -c user/lib/syscall.S -o "$OBJ/lib_syscall_asm.o"       # __v64_int80/__v64_syscall/__v64_sigreturn_stub
LIBOBJS="$LIBOBJS $OBJ/lib_syscall_asm.o"
$CC $ASFLAGS -c user/apps/edit/edit_start.S -o "$OBJ/edit_start.o"   # 入口桩（真 argv）
$CC $UCFLAGS -c user/apps/edit/edit.c -o "$OBJ/edit.o"

echo "==> ★ A4-5 2/3：链接（静态 ELF64，4GiB 起，内核主程序装载器直接装）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/edit/edit64.ld \
    -o "$OUT/edit" "$OBJ/edit_start.o" $LIBOBJS "$OBJ/edit.o"

echo "==> ★ A4-5 3/3：构建期断言（ELF 头/程序头；内核 elf64.cpp 的硬门槛）"
$PY - "$OUT/edit" <<'PYEOF'
import os, struct, sys

BASE64  = 0x100000000             # USER64_CODE_VA64
STACK64 = 0x100000000 + 0x10000   # USER64_STACK_VA64（装载区上界，排他）
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "edit 不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "edit 必须是 ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert 0 < phn <= 16 and phes == 56, "程序头表不合法"
first = None
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
    if t == 3: raise SystemExit("ERROR: edit 出现 PT_INTERP（内核只做静态装载）")
    if t == 2: raise SystemExit("ERROR: edit 出现 PT_DYNAMIC")
    if t != 1: continue
    assert va >= BASE64 and va + msz <= STACK64, "PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x msz=%#x）" % (va, msz)
    if first is None:
        first = (off, fsz)
assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
    "程序头表不在第一个 PT_LOAD 的文件范围内（auxv AT_PHDR 会是 0）"
assert len(d) <= 64 * 1024, "edit 超过 64 KiB（主程序装载窗口）"
print("    断言 OK：%s = %d B，entry=%#x phnum=%d（static ET_EXEC，4GiB 装载区）"
      % (os.path.basename(path), len(d), entry, phn))
PYEOF

echo
echo "    build64/edit = $(stat -c%s "$OUT/edit") B（卷里 /bin/edit；不进内核镜像）"
