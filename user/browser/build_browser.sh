#!/usr/bin/env bash
# build_browser.sh - 编 /bin/browser（VimtuOS 浏览器，v0.5.0）
#
# 产物：$BUILD/browser.elf（静态 ELF64，落在用户窗口 4GiB..4GiB+64KiB 主装载区）
# 纪律（与 user/store/build_store.sh 同一条）：
#   * clang 一律 -Wall -Wextra；
#   * **font64.c 必须开 SSE**（stb_truetype 是浮点代码），其余一律 -mno-sse*；
#   * -Os 换体积：主装载区只有 64 KiB（kernel/elf64.cpp 的硬约束），浏览器 + 字体栈已经很紧；
#   * 不在内核里加一个字节 —— 交付 = 卷里的 /bin/browser（tools/browser_pack_win.py 写入）。
set -e
cd "$(dirname "$0")/../.."
BUILD="${1:-build64}"
OBJ="$BUILD/uapps/browser"
mkdir -p "$OBJ"

CC="clang --target=x86_64-unknown-none-elf"
CFLAGS_COMMON="-nostdinc -I user/lib -I user/browser -ffreestanding -nostdlib -fno-builtin \
 -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections -std=c11"
CFLAGS_NOSSE="-mno-sse -mno-sse2 -mno-mmx -mno-avx"
LD="${LD:-ld.lld}"

echo "==> /bin/browser：编译用户库 + 浏览器"
for f in user/lib/syscall.c user/lib/string.c user/lib/stdlib.c user/lib/stdio.c; do
    o="$OBJ/$(basename "${f%.c}").o"
    $CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c "$f" -o "$o"
done
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/lib/crt0.S -o "$OBJ/crt0.o"
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/lib/syscall.S -o "$OBJ/syscallS.o"
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c user/lib/wl.c -o "$OBJ/wl.o"
$CC $CFLAGS_COMMON -msse -msse2 -Os -c user/lib/font64.c -o "$OBJ/font64.o"
LIB_OBJS="$OBJ/syscall.o $OBJ/string.o $OBJ/stdlib.o $OBJ/stdio.o $OBJ/crt0.o $OBJ/syscallS.o"

echo "==> /bin/browser：编译 html.c + browser.c"
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c user/browser/html.c    -o "$OBJ/html.o"
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c user/browser/browser.c -o "$OBJ/browser.o"

$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/store/store64.ld \
    -o "$BUILD/browser.elf" "$OBJ/browser.o" "$OBJ/html.o" "$OBJ/wl.o" "$OBJ/font64.o" $LIB_OBJS

SIZE=$(stat -c%s "$BUILD/browser.elf")
echo "    /bin/browser = $SIZE B（静态 ELF64；交付 = 系统卷 /bin/browser）"
# 自检：PT_LOAD 必须落在 4GiB..4GiB+64KiB（kernel/elf64.cpp 的硬门槛），且没有 PT_INTERP
python - "$BUILD/browser.elf" <<'PYBROWSER'
import struct, sys
p = sys.argv[1]
d = open(p, "rb").read()
if d[:4] != b"\x7fELF" or d[4] != 2:
    print("    ERROR: 不是 ELF64"); sys.exit(1)
e_phoff = struct.unpack_from("<Q", d, 32)[0]
e_phentsize = struct.unpack_from("<H", d, 54)[0]
e_phnum = struct.unpack_from("<H", d, 56)[0]
lo, hi, nload, interp = None, None, 0, False
for i in range(e_phnum):
    off = e_phoff + i * e_phentsize
    t, _fl, _o, va, _pa, fsz, msz, _al = struct.unpack_from("<IIQQQQQQ", d, off)
    if t == 3:
        interp = True
    if t != 1:
        continue
    nload += 1
    a, b = va, va + max(fsz, msz)
    lo = a if lo is None else min(lo, a)
    hi = b if hi is None else max(hi, b)
OK_LO = 0x0000000100000000
if interp:
    print("    ERROR: 有 PT_INTERP（本内核只做静态装载）"); sys.exit(1)
if nload == 0 or lo < OK_LO or hi > OK_LO + 64 * 1024:
    print("    ERROR: PT_LOAD 落在 [0x%x, 0x%x)，不在 4GiB..4GiB+64KiB" % (lo or 0, hi or 0)); sys.exit(1)
print("    ELF64 自检 OK：segs=%d 装载区 [0x%x, 0x%x) 大小 %d B" % (nload, lo, hi, hi - lo))
PYBROWSER
