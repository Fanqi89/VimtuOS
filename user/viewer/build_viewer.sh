#!/bin/bash
# user/viewer/build_viewer.sh - ★ v0.10.5：图片查看器 /bin/viewer（纯 ring3，内核 0 字节）
#
# 产物：build64/viewer.elf  ->  tools/viewer_pack_win.py 装进系统卷的 /bin/viewer
#
# 纪律（与 user/store/build_store.sh、user/browser/build_browser.sh 完全同一条）：
#   * clang 一律 -Wall -Wextra，本仓库自己的文件 **零告警**（Werror 收口）；
#     上游 vendored 代码（stb_image.h）单独一个 TU，用 -w 关掉风格告警。
#   * 静态 ELF64（ET_EXEC），无 PT_INTERP/PT_DYNAMIC；
#   * PT_LOAD 必须落在 kernel/elf64.cpp 真正接受的"大映像"窗口
#       [USER64_MMAP_VA64 = 4GiB+576KiB, 4GiB+998MiB)
#     —— 判据抄用户的链接脚本基址 0x100090000，不是放宽（见 viewer64.ld 头部说明）；
#   * font64.c（stb_truetype）与 stb_image_impl.c 必须开 SSE，其余一律 -mno-sse*；
#   * 每页不得同时 W 与 X（本脚本逐段核验）。
set -e
cd "$(dirname "$0")/../.."
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
BUILD=build64
OBJ=$BUILD/viewer
mkdir -p "$OBJ"

CC="clang --target=x86_64-unknown-none-elf"
CFLAGS_COMMON="-nostdinc -I user/lib -I user/viewer -I third_party/stb \
 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 -fno-pic -fno-pie -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
 -std=c11 -O2 -Wall -Wextra -Werror -Wno-unicode-homoglyph"
CFLAGS_NOSSE="-mno-sse -mno-sse2 -mno-mmx -mno-avx"
CFLAGS_SSE="-msse -msse2"
LD="${LD:-ld.lld}"

echo "==> 图片查看器：/bin/viewer（stb_image PNG+JPEG+BMP + wl_surface 窗口壳）"

# ---- 用户态 C 运行时（与 store/browser 同一份源）----
LIB_SRC="user/lib/syscall.c user/lib/string.c user/lib/stdlib.c user/lib/stdio.c"
for f in $LIB_SRC; do
    o="$OBJ/$(basename "$f").o"
    $CC $CFLAGS_COMMON $CFLAGS_NOSSE -c "$f" -o "$o"
done
$CC -nostdinc -ffreestanding -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra \
    -c user/lib/crt0.S -o "$OBJ/crt0.o"
$CC -nostdinc -ffreestanding -fno-pic -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra \
    -c user/lib/syscall.S -o "$OBJ/syscall.S.o"
LIB_OBJS="$OBJ/syscall.c.o $OBJ/string.c.o $OBJ/stdlib.c.o $OBJ/stdio.c.o $OBJ/crt0.o $OBJ/syscall.S.o"

# ---- Wayland 客户端库 + 字体（stb_truetype 是浮点代码 -> 必须开 SSE）----
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/lib/wl.c -o "$OBJ/wl.o"
$CC $CFLAGS_COMMON $CFLAGS_SSE   -c user/lib/font64.c -o "$OBJ/font64.o"

# ---- 解码器（唯一编译 stb_image 的 TU；上游代码用 -w）----
$CC $CFLAGS_COMMON $CFLAGS_SSE -w -c user/viewer/stb_image_impl.c -o "$OBJ/stb_image_impl.o"

# ---- 窗口壳（本仓库自己的代码：零告警要求）----
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/viewer/viewer.c -o "$OBJ/viewer.o"

$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/viewer/viewer64.ld \
    -o "$BUILD/viewer.elf" $OBJ/viewer.o $OBJ/stb_image_impl.o $OBJ/wl.o $OBJ/font64.o $LIB_OBJS

PYBIN="${PY:-python}"
$PYBIN - "$BUILD/viewer.elf" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "不是 ET_EXEC/x86_64"
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phes == 56 and 0 < phn <= 16, "程序头条数/尺寸不对"
BIGLO = 0x100090000                       # USER64_MMAP_VA64 = 4GiB + 576 KiB
BIGHI = 0x100000000 + 998 * 1024 * 1024   # USER64_LOADER_BIG_TOP_VA64 之内
segs = []
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    flags = struct.unpack_from("<I", d, o + 4)[0]
    off = struct.unpack_from("<Q", d, o + 8)[0]
    va = struct.unpack_from("<Q", d, o + 16)[0]
    fsz = struct.unpack_from("<Q", d, o + 32)[0]
    msz = struct.unpack_from("<Q", d, o + 40)[0]
    assert t not in (2, 3), "出现 PT_DYNAMIC/PT_INTERP"
    if t != 1:
        continue
    assert BIGLO <= va and va + msz <= BIGHI, \
        "PT_LOAD 越出大映像窗口（va=%#x msz=%#x）" % (va, msz)
    assert off + fsz <= len(d), "PT_LOAD 文件段越界"
    # ELF p_flags 位序（**不是** R/W/X = 1/2/4！）：PF_X=0x1、PF_W=0x2、PF_R=0x4
    assert (flags & 4) == 4, "PT_LOAD 不可读"
    assert not ((flags & 7) == 7), "单段同时 RWX（W^X 破坏）"
    segs.append((va, msz, flags))
ent = struct.unpack_from("<Q", d, 24)[0]
assert any(va <= ent < va + m for va, m, _ in segs), "入口不在任何 PT_LOAD 内"
# 逐页核验"没有一页同时 W 与 X"（按 4 KiB 页铺开）
pages = {}
for va, msz, flags in segs:
    p0 = va // 4096
    p1 = (va + msz + 4095) // 4096
    for p in range(p0, p1):
        pages[p] = pages.get(p, 0) | flags
for p, fl in pages.items():
    assert not ((fl & 1) and (fl & 2)), "页 %#x 同时 W 与 X" % (p * 4096)
print("    /bin/viewer ELF 检查 OK：%d 个 PT_LOAD，memsz 合计 %d B，文件 %d B（大映像窗口）"
      % (len(segs), sum(m for _, m, _ in segs), len(d)))
PYEOF

echo "    /bin/viewer = $(stat -c%s "$BUILD/viewer.elf") B"
