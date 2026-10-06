#!/bin/bash
# user/store/build_store.sh - ★ 应用商店 + 包管理器：编出四个用户态程序（纯 ring3，内核零改动）
#
#   build64/store/vpkg.elf      /bin/vpkg    （命令行包管理器；CLI 路）
#   build64/store/store.elf     /bin/store   （GUI 商店；走 /bin/wm 合成器 + font64）
#   build64/store/demo_cli.elf  示例 CLI 包（.vap64 的 payload -> /bin/demo-cli）
#   build64/store/demo_gui.elf  示例 GUI 包（.vap64 的 payload -> /bin/demo-gui）
#
# 纪律（与 user/shell/build_shell.sh、sdk/software-template 的 Makefile 同一条）：
#   * clang 一律带 -Wall -Wextra 且**零告警**（-Werror 收口）；
#   * 静态 ELF64（ET_EXEC），PT_LOAD 必须落在 4GiB..4GiB+64KiB（内核装载器硬约束，
#     见 kernel/elf64.cpp 的 e64_lo64/e64_hi64；本脚本自己按程序头表复算这条）；
#   * **font64.c 必须开 SSE**（stb_truetype 是浮点代码），其余一律 -mno-sse*；
#   * 产物只落 build64/：由 tools/store_pack_win.py 装进系统卷（内核里一个字节都没有）。
#
# 用法：bash user/store/build_store.sh           （或由 build64.sh 调用）
set -e
cd "$(dirname "$0")/../.."
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
BUILD=build64/store
OBJ=$BUILD/obj
mkdir -p "$OBJ" "$BUILD"

CC="clang --target=x86_64-unknown-none-elf"
CFLAGS_COMMON="-nostdinc -I user/lib -ffreestanding -nostdlib -fno-builtin -fno-stack-protector \
 -fno-pic -fno-pie -fno-zero-initialized-in-bss -mcmodel=large -mno-red-zone \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
 -std=c11 -O2 -Wall -Wextra -Werror -Wno-unicode-homoglyph"
CFLAGS_NOSSE="-mno-sse -mno-sse2 -mno-mmx -mno-avx"
LD="${LD:-ld.lld}"

echo "==> ★ 应用商店/包管理器：编译用户态程序（四个产物，全部静态 ELF64）"

# ---- user/lib 的公共部分（与 SDK 模板同一条链接口径）----
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

# ---- CLI 版引擎对象（含 deb 的 ar/tar/gzip/**xz**：只有 /bin/vpkg 带这段）----
# ★ 体积（kernel/elf64.cpp 的 64 KiB PT_LOAD 硬约束，本脚本末尾自检）：
#   这一批把 xz 解码核（third_party/lzma，~12 KB 机器码）+ vs_xz.c 编进 /bin/vpkg 之后，
#   装载区从"余 18 KB"变成"只剩 2 KB 出头"，所以 **整个 CLI 版都改 -Os**
#   （4 个引擎对象 + vpkg.c；-O2 会再涨 ~6 KB，直接越界）。
#   行为完全一样，只是代码更小；GUI 版本来就是 -Os。
VSFLAGS="$CFLAGS_COMMON $CFLAGS_NOSSE -Os -I third_party/lzma"
$CC $VSFLAGS -c user/store/vs_io.c   -o "$OBJ/vs_io.c.o"
$CC $VSFLAGS -c user/store/vs_gzip.c -o "$OBJ/vs_gzip.c.o"
$CC $VSFLAGS -c user/store/vs_xz.c   -o "$OBJ/vs_xz.c.o"
$CC $VSFLAGS -c user/store/vs_pkg.c  -o "$OBJ/vs_pkg.c.o"
# ★ vendored 解码核（third_party/lzma：Igor Pavlov，public domain；见那里的 README/LICENSE）：
#   编进 /bin/vpkg，用 -w 关掉上游代码的风格告警（本仓库自己的文件仍然 -Wall -Wextra -Werror）。
VWFLAGS="$VSFLAGS -w"
$CC $VWFLAGS -c third_party/lzma/LzmaDec.c  -o "$OBJ/lzma-LzmaDec.c.o"
$CC $VWFLAGS -c third_party/lzma/Lzma2Dec.c -o "$OBJ/lzma-Lzma2Dec.c.o"
VS_OBJS="$OBJ/vs_io.c.o $OBJ/vs_gzip.c.o $OBJ/vs_xz.c.o $OBJ/vs_pkg.c.o \
 $OBJ/lzma-LzmaDec.c.o $OBJ/lzma-Lzma2Dec.c.o"
# ---- GUI 版（-DVS_STORE_GUI **不编** deb 的 inflate/ar/tar/xz）——★ 全部用 -Os：
#   256x60 窗口 + stb_truetype（font64）已经把 64 KiB 主程序装载区吃满，只能靠体积换功能。
#   GUI 对 deb 走 `/bin/vpkg install <name>` 子进程（同一份引擎），见 vs_pkg.c 与 store.c 的说明。
for f in user/store/vs_io.c user/store/vs_gzip.c user/store/vs_pkg.c; do
    o="$OBJ/gui-$(basename "$f").o"
    $CC $CFLAGS_COMMON $CFLAGS_NOSSE -DVS_STORE_GUI -Os -c "$f" -o "$o"
done
VS_OBJS_GUI="$OBJ/gui-vs_io.c.o $OBJ/gui-vs_gzip.c.o $OBJ/gui-vs_pkg.c.o"

# ---- ① /bin/vpkg（命令行）----
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c user/store/vpkg.c -o "$OBJ/vpkg.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/vpkg.elf" $OBJ/vpkg.o $VS_OBJS $LIB_OBJS

# ---- ② /bin/store（GUI：合成器客户端 + font64；-Os 换体积）----
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -Os -c user/lib/wl.c -o "$OBJ/wl.o"
$CC $CFLAGS_COMMON -msse -msse2 -Os -c user/lib/font64.c -o "$OBJ/font64.o"
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -DVS_STORE_GUI -Os -c user/store/store.c -o "$OBJ/store.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/store.elf" $OBJ/store.o $VS_OBJS_GUI $OBJ/wl.o $OBJ/font64.o $LIB_OBJS

# ---- ③④ 示例包载荷（.vap64 的 payload；ELF 且 <= 32768 B）----
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/store/demo_cli.c -o "$OBJ/demo_cli.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/demo_cli.elf" $OBJ/demo_cli.o $LIB_OBJS
$CC $CFLAGS_COMMON $CFLAGS_NOSSE -c user/store/demo_gui.c -o "$OBJ/demo_gui.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T user/apps/evshm_demo.ld \
    -o "$BUILD/demo_gui.elf" $OBJ/demo_gui.o $OBJ/wl.o $LIB_OBJS
PYBIN="${PY:-py -3}"
# ---- 自检：静态 ELF64 + PT_LOAD 落在 4GiB..4GiB+64KiB + 入口在段内 ----
$PYBIN - "$BUILD/vpkg.elf" "$BUILD/store.elf" "$BUILD/demo_cli.elf" "$BUILD/demo_gui.elf" <<'PYEOF'
import struct, sys
lo, hi = 0x100000000, 0x100000000 + 0x10000
for path in sys.argv[1:]:
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "%s 不是 ELF64 小端" % path
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "%s 不是 ET_EXEC/x86_64" % path
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    segs = []
    for i in range(phn):
        o = phoff + i * phes
        t = struct.unpack_from("<I", d, o)[0]
        va = struct.unpack_from("<Q", d, o + 16)[0]
        msz = struct.unpack_from("<Q", d, o + 40)[0]
        assert t not in (2, 3), "%s 出现 PT_DYNAMIC/PT_INTERP" % path
        if t == 1:
            assert lo <= va and va + msz <= hi, \
                "%s PT_LOAD 越出主程序装载区（va=%#x msz=%#x <= %#x）" % (path, va, msz, hi - lo)
            segs.append((va, msz))
    ent = struct.unpack_from("<Q", d, 24)[0]
    assert any(va <= ent < va + m for va, m in segs), "%s 入口不在任何 PT_LOAD 内" % path
    assert len(d) <= 96 * 1024, "%s 超过内核读盘缓冲 96 KiB" % path
    print("    %-42s %7d B  PT_LOAD=%d memsz=%d B（装载区 <= 65536）"
          % (path.replace("\\", "/"), len(d), len(segs), sum(m for _v, m in segs)))
    assert sum(m for _v, m in segs) <= 0x10000, "%s PT_LOAD memsz 超过 64 KiB" % path
PYEOF

echo "==> ★ 应用商店/包管理器：四个产物都在 build64/store/（不内嵌内核；由 tools/store_pack_win.py 装卷）"
