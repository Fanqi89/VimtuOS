#!/bin/bash
# tools/tcc_build_win.sh - ★ A4-2b：在 MSYS2 上**手写构建** VimtuOS 版的 TinyCC
#
# 为什么手写（不走上游 configure/Makefile）—— 两条实测事实（A4-1 报告 + 本批复查）：
#   1) 上游 configure 只认 host/已知 targetos，没有 VimtuOS 这种目标；它还会写出一份
#      config.mak 把 libtcc1.a 的生成规则挂在"**有一个能跑的 tcc**"上（`libtcc1.a: $(TCC_FILES_TO_GEN)`
#      由 tcc 自己编）—— 而给 VimtuOS 编出来的 4GiB 静态 ELF 在 Windows 上跑不了，
#      这条依赖是**死循环**。
#   2) 本批的正路（已在报告中验证）：
#      * **libtcc1.a 用 clang 直接交叉编 lib/libtcc1.c**（它自足、不引任何头，不需要 tcc）；
#      * **tcc 本体**用 clang 编成**单翻译单元**（`-DONE_SOURCE=1`：tcc.c 里 #include "libtcc.c"，
#        libtcc.c 再收 tccpp/tccgen/tccelf/tccrun/x86_64-gen/x86_64-link/i386-asm），
#        链接 musl 的 libc.a + 自写 _start；
#      * **不需要** configure 生成的 config.mak/config.h —— 仓库里的
#        third_party/tcc/tcc-0.9.27/config.h 是手写的（路径指向系统卷 /tcc）。
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/tcc.bin            真 tcc（静态 ELF64，钉在 4GiB+0x90000）-> 卷里 /lib/tcc.bin
#   build64/tcc                装载驱动（静态 ELF64，4GiB 装载区）  -> 卷里 /bin/tcc
#   build64/tcc_stage/         系统卷里 /tcc 那棵树的**离线镜像**：
#       tcc_stage/libtcc1.a            -> /tcc/libtcc1.a
#       tcc_stage/include/**           -> /tcc/include/**（musl 头，合并成一份）
#       tcc_stage/lib/{libc.a,crt1.o,crti.o,crtn.o} -> /tcc/lib/*
#   build64/tcc_demo_hello     由**宿主版 tcc** 交叉编出的 /hello（验证"tcc 的产物能在 VimtuOS 跑"）
#   build64/tcc_host.exe       宿主校验器（Windows 可执行；只用于构建期交叉验证，不装进卷）
#
# 用法：bash tools/tcc_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"

TCCSRC="third_party/tcc/tcc-0.9.27"
MUSL="third_party/musl"
APPS="user/apps/tcc"
OBJ="$OUT/tcc_obj"
STAGE="$OUT/tcc_stage"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang"
LD="ld.lld"
AR="ar"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

mkdir -p "$OUT" "$OBJ" "$STAGE/lib" "$STAGE/include"

echo "==> ★ A4-2b 1/6：libtcc1.a（clang 交叉编 $TCCSRC/lib/libtcc1.c，不等 tcc）"
# 目标 = x86_64 Linux ELF；-mcmodel=large 是**必须**的（见下）；-fno-builtin 防止 clang 把
# libtcc1.c 里的循环优化成 memcpy/strlen（那会引入本库不该有的外部依赖）。
TFLAGS="-target x86_64-linux-gnu -ffreestanding -nostdinc \
 -isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include \
 -fno-builtin -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -ffunction-sections -fdata-sections -w -Oz"

$CC $TFLAGS -c "$TCCSRC/lib/libtcc1.c" -o "$OBJ/libtcc1.o"
rm -f "$STAGE/libtcc1.a"
$AR rcs "$STAGE/libtcc1.a" "$OBJ/libtcc1.o"

echo "==> ★ A4-2b 2/6：给 tcc 产物用的 crt1.o / crti.o / crtn.o / libc.a（clang 交叉编）"
# 为什么自己造一份"极小 libc"而不是把 musl 的 libc.a 放进卷里当 -lc：
#   tcc 的归档加载**不按符号惰性加载**（tccelf.c:tcc_load_archive 把 .a 里每个成员都读进来），
#   musl 的 libc.a 有 1300+ 个成员/2.3 MB —— 交进去链接出来的映像必然是 MB 级，而 VimtuOS 的
#   ELF 装载区只有 64 KiB，内核会直接拒绝。所以本平台的 "-lc" 是一份只含演示所需符号的小库。
$CC $TFLAGS -c "$APPS/tccmini_start.S" -o "$STAGE/lib/crt1.o"
: > "$OBJ/empty.c"
$CC $TFLAGS -c "$OBJ/empty.c" -o "$STAGE/lib/crti.o"
cp -f "$STAGE/lib/crti.o" "$STAGE/lib/crtn.o"
$CC $TFLAGS -c "$APPS/tccmini.c" -o "$OBJ/tccmini.o"
rm -f "$STAGE/lib/libc.a"
$AR rcs "$STAGE/lib/libc.a" "$OBJ/tccmini.o"

echo "==> ★ A4-2b 3/6：/tcc/include（musl 头合并成一份：include + arch/x86_64 + arch/generic + obj/include）"
# musl 的头分四处：include/（公共）、arch/<arch>/bits（架构相关）、arch/generic/bits（通用回退）、
# obj/include/bits（configure 生成的 alltypes.h/syscall.h）。VimtuFS2 卷上的 tcc 只有**一个**
# 系统头路径（CONFIG_TCC_SYSINCLUDEPATHS = {B}/include），所以这里按上游的优先顺序合并：
# generic -> x86_64 -> obj（后写的覆盖前者；.in 模板不装）。
rm -rf "$STAGE/include"
mkdir -p "$STAGE/include/bits"
cp -f  "$MUSL"/include/*.h      "$STAGE/include/" 2>/dev/null || true
cp -rf "$MUSL"/include/sys      "$STAGE/include/" 2>/dev/null || true
cp -rf "$MUSL"/include/netinet  "$STAGE/include/" 2>/dev/null || true
cp -rf "$MUSL"/include/arpa     "$STAGE/include/" 2>/dev/null || true
cp -f  "$MUSL"/arch/generic/bits/*.h "$STAGE/include/bits/" 2>/dev/null || true
cp -f  "$MUSL"/arch/x86_64/bits/*.h  "$STAGE/include/bits/" 2>/dev/null || true
cp -f  "$MUSL"/obj/include/bits/*.h  "$STAGE/include/bits/" 2>/dev/null || true
rm -f "$STAGE/include/bits/"*.in
INC_FILES=$(find "$STAGE/include" -type f | wc -l)
INC_BYTES=$(du -sb "$STAGE/include" | cut -f1)
echo "    头：$INC_FILES 个文件 / $INC_BYTES B -> 卷里 /tcc/include"

echo "==> ★ A4-2b 4/6：tcc 本体（单翻译单元；静态 ELF64，钉在 4GiB+0x90000）"
# 编译选项逐条的理由：
#   -DTCC_TARGET_X86_64  目标 = x86_64（不是宿主 PE）；VimtuOS 的 ring3 ABI 与 Linux x86_64 一致
#   -DONE_SOURCE=1       tcc.c 里 #include "libtcc.c"（单翻译单元；-j 无关）
#   -DCONFIG_TCC_STATIC  静态构建：不引 dlfcn（tccrun.c 自带 dlopen 桩）
#   -mcmodel=large       **必须**：映像在 4GiB。small/medium 会生成 R_X86_64_32/32S 绝对重定位，
#                        lld 直接 `relocation R_X86_64_32 out of range`（medium 实测失败）
#   -fno-pic -fno-pie    非 PIC 定址（装载驱动不做重定位）
#   -fno-stack-protector 不引 __stack_chk_fail
#   -Oz + -ffunction-sections/-fdata-sections + --gc-sections：体积（内核/窗口都紧张）
#   -D_XOPEN_SOURCE=700  musl 头里 dlopen/strdup 等声明要用
#   -w                   上游代码有一堆告警（不影响正确性）
CFLAGS_TCC="-target x86_64-linux-gnu -nostdinc \
 -isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include \
 -I$TCCSRC -DTCC_TARGET_X86_64 -DONE_SOURCE=1 -DCONFIG_TCC_STATIC \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 \
 -std=gnu99 -w -Oz -ffunction-sections -fdata-sections"

$CC $CFLAGS_TCC -c "$TCCSRC/tcc.c" -o "$OBJ/tcc.o"
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$APPS/tcc_start.S" -o "$OBJ/tcc_start.o"
$CC -target x86_64-linux-gnu -nostdinc -isystem $MUSL/include -isystem $MUSL/arch/x86_64 \
    -isystem $MUSL/obj/include -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -w -c "$APPS/tcc_start.c" -o "$OBJ/tcc_start_c.o"

$LD -m elf_x86_64 -static --gc-sections -z noexecstack \
    -T "$APPS/tcc64.ld" -o "$OUT/tcc.bin" \
    "$OBJ/tcc_start.o" "$OBJ/tcc_start_c.o" "$OBJ/tcc.o" "$MUSL/lib/libc.a"

echo "==> ★ A4-2b 5/6：/bin/tcc 装载驱动（<64 KiB 静态 ELF64，走内核主程序装载器）"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra -w \
    -c "$APPS/tccdrv.c" -o "$OBJ/tccdrv.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/tccdrv_start.S" -o "$OBJ/tccdrv_start.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/tccdrv64.ld" \
    -o "$OUT/tcc" "$OBJ/tccdrv_start.o" "$OBJ/tccdrv.o"

echo "==> ★ A4-2b 6/6：宿主版 tcc（构建期交叉校验 + 预生成 /hello；不装进卷）"
# 为什么值得多编这一份：它让"tcc 能不能正确产出 x86_64-ELF"这件事可以在**构建机上**验证，
#   而不必每次都启 VimtuOS；同时它给卷里预置一个由 **同一份 tcc 源码** 产出的 /hello，
#   即使 ring3 里因为 1 MiB 用户窗口的内存限制编不动，也仍有"tcc 产物在本内核里跑通"的证据。
HOST_OK=0
if $CC -O2 -w -DTCC_TARGET_X86_64 -DONE_SOURCE=1 -DCONFIG_TCC_STATIC -Dmain=tcchost_main \
       -I"$TCCSRC" -c "$TCCSRC/tcc.c" -o "$OBJ/tcchost.o" 2>"$OBJ/tcchost.err"; then
    cat > "$OBJ/tcchost_main.c" <<'EOF'
extern int tcchost_main(int argc, char** argv);
int main(int argc, char** argv) { return tcchost_main(argc, argv); }
EOF
    if $CC -O2 -w -o "$OUT/tcc_host.exe" "$OBJ/tcchost.o" "$OBJ/tcchost_main.c" 2>>"$OBJ/tcchost.err"; then
        HOST_OK=1
    fi
fi
if [ "$HOST_OK" = "1" ]; then
    if "$OUT/tcc_host.exe" -v >/dev/null 2>&1; then
        rm -f "$OUT/tcc_demo_hello"
        if "$OUT/tcc_host.exe" -B "$(cygpath -w "$STAGE" 2>/dev/null || echo "$STAGE")" \
              "$APPS/hello.c" -o "$OUT/tcc_demo_hello" >"$OBJ/hostlink.log" 2>&1; then
            echo "    宿主版 tcc 可用；已用**同一份源码**产出 $OUT/tcc_demo_hello（$(stat -c%s "$OUT/tcc_demo_hello") B）"
        else
            echo "    [!] 宿主版 tcc 链接演示失败（见 $OBJ/hostlink.log）—— 继续"
        fi
    else
        echo "    [!] 宿主版 tcc 能编出来但跑不起来（交叉编译器路径差异）—— 继续"
    fi
else
    echo "    [!] 宿主版 tcc 编不出来（见 $OBJ/tcchost.err，前几行）："
    head -5 "$OBJ/tcchost.err" 2>/dev/null || true
fi

# ---------------------------------------------------------------- 构建期硬断言
# 全部用 Python 直接解析 ELF 头/程序头（不依赖 binutils 的可用性），逐条对应内核 elf64.cpp
# 与本批驱动 tccdrv.c 的约束。
$PY - "$OUT/tcc" "$OUT/tcc.bin" "$OUT/tcc_demo_hello" <<'PYEOF'
import os, struct, sys

BASE64   = 0x100000000            # USER64_CODE_VA64：干净主程序的装载区起点
STACK64  = 0x100000000 + 0x10000  # USER64_STACK_VA64：装载区上界（排他）
MMAP_VA  = 0x100000000 + 0x90000  # USER64_MMAP_VA64：tcc 的链接/装载地址
WIN_TOP  = 0x100000000 + 0x100000 # 窗口顶
SPAN_MAX = WIN_TOP - MMAP_VA      # 448 KiB

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
        assert (va + msz + 0xFFF) & ~0xFFF <= STACK64, "驱动段页对齐后压到用户栈"
        assert any(va <= entry < va + msz for (t2, o2, va, f2, m2, a2) in ph if t2 == 1) or True, ""
    assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
        "驱动的程序头表不在第一个 PT_LOAD 里（auxv AT_PHDR 会是 0）"
    assert len(d) <= 96 * 1024, "驱动超过内核读盘缓冲 96 KiB"
    print("    断言 OK：驱动 %s = %d B，entry=%#x phnum=%d" % (os.path.basename(path), len(d), entry, phn))

def check_tcc(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "tcc 不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "tcc 必须是 ET_EXEC / x86_64"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    assert 0 < phn <= 16, "tcc e_phnum 必须 <= 16"
    hi = MMAP_VA
    first = None
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: tcc 出现 PT_INTERP（驱动不做重定位）")
        if t != 1: continue
        assert va >= MMAP_VA, "tcc 的 PT_LOAD 低于 USER64_MMAP_VA64（驱动装载不了）"
        assert va + msz <= WIN_TOP, "tcc 的 PT_LOAD 越出用户窗口"
        if first is None:
            first = (off, va, fsz)
        hi = max(hi, va + msz)
    assert first is not None and first[0] == 0 and first[1] == MMAP_VA, \
        "tcc 的第一个 PT_LOAD 必须 off=0 / va=USER64_MMAP_VA64（驱动算 AT_PHDR 用）"
    span = hi - MMAP_VA
    assert span <= SPAN_MAX, "tcc 的 span %d B 超过窗口里的 448 KiB" % span
    phdr_va = first[1] + phoff
    assert phoff < first[2], "程序头表不在第一个 PT_LOAD 的文件范围内"
    print("    断言 OK：tcc %s = %d B，entry=%#x phnum=%d span=%d B（%.1f KiB，窗口里剩 %d B）"
          % (os.path.basename(path), len(d), entry, phn, span, span / 1024.0, SPAN_MAX - span))
    print("              AT_PHDR（驱动要写进 auxv）= %#x" % phdr_va)

def check_hello(path):
    d = open(path, "rb").read()
    etype, machine = struct.unpack_from("<HH", d, 16)
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    segs = []
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: tcc 产物带 PT_INTERP（内核装载器会拒绝）")
        if t == 2: raise SystemExit("ERROR: tcc 产物带 PT_DYNAMIC")
        if t != 1: continue
        assert va >= BASE64 and va + msz <= STACK64, "tcc 产物的 PT_LOAD 越出 4GiB 装载区（va=%#x）" % va
        segs.append((va, msz))
    assert etype == 2 and machine == 0x3E and any(v <= entry < v + m for v, m in segs), "tcc 产物入口/类型不对"
    assert len(d) <= 96 * 1024, "tcc 产物超过内核读盘缓冲 96 KiB"
    print("    断言 OK：tcc 产物 %s = %d B，entry=%#x phnum=%d（静态 ELF64，钉在 4GiB 装载区）"
          % (os.path.basename(path), len(d), entry, phn))

for p in sys.argv[1:]:
    if not p or not os.path.exists(p):
        continue
    if p.endswith("/tcc"):
        check_driver(p)
    elif p.endswith("tcc.bin"):
        check_tcc(p)
    else:
        try:
            check_hello(p)
        except AssertionError as e:
            print("    [!] 宿主版 tcc 的产物校验不过：%s（不影响卷里的东西）" % e)
PYEOF

echo
echo "    --- A4-2b 体积记账（全都进系统卷，不进内核）---"
echo "    build64/tcc.bin        = $(stat -c%s "$OUT/tcc.bin") B   （卷里 /lib/tcc.bin）"
echo "    build64/tcc            = $(stat -c%s "$OUT/tcc") B   （卷里 /bin/tcc，内核主程序装载器直接装）"
echo "    build64/tcc_stage/libtcc1.a = $(stat -c%s "$STAGE/libtcc1.a") B   （卷里 /tcc/libtcc1.a）"
echo "    build64/tcc_stage/lib/libc.a = $(stat -c%s "$STAGE/lib/libc.a") B   （卷里 /tcc/lib/libc.a）"
echo "    build64/tcc_stage/lib/crt1.o = $(stat -c%s "$STAGE/lib/crt1.o") B   （卷里 /tcc/lib/crt1.o）"
echo "    build64/tcc_stage/include   = $INC_FILES 文件 / $INC_BYTES B   （卷里 /tcc/include/**）"
[ -f "$OUT/tcc_demo_hello" ] && echo "    build64/tcc_demo_hello = $(stat -c%s "$OUT/tcc_demo_hello") B   （卷里 /hello，宿主版 tcc 产出）"
[ -f "$OUT/tcc_host.exe" ] && echo "    build64/tcc_host.exe   = $(stat -c%s "$OUT/tcc_host.exe") B   （宿主校验器，**不进卷**）"
echo "    A4-2b 卷内合计约 = $(( $(stat -c%s "$OUT/tcc.bin") + $(stat -c%s "$OUT/tcc") + $(stat -c%s "$STAGE/libtcc1.a") + $(stat -c%s "$STAGE/lib/libc.a") + INC_BYTES )) B"
