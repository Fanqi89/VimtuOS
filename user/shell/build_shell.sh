#!/bin/bash
# user/shell/build_shell.sh - ★ A4-1：把 ring3 shell 编成**静态 ELF64**
#
# 用法：bash user/shell/build_shell.sh [outdir]        # 缺省 outdir = build64
# 产出：<outdir>/shell.elf  静态 ELF64（链接在用户窗口 4GiB 起；无 PT_INTERP/无重定位）
#       <outdir>/shell.bin  与 shell.elf **逐字节相同**的载荷（= 装进系统卷 /bin/shell.bin 的字节）
#
# 为什么 shell 用**我们自研的 user/lib**（而不是 musl）：见 docs/应用层与系统调用说明.md
# 的 "A4-1：Ring 3 shell" 节 —— 一句话：shell 的逻辑 + 自有 ABI 包装只需要极小 libc，
# 而 musl 静态链接的同类程序实测 58 KB（装载窗口只有 64 KiB）；user/lib 版本更小，
# 装载窗口、VimtuFS2 单文件、内核装载缓冲三个上限全都离得很远。
#
# 链接与装载的硬约束（本脚本**提前断言**，别让内核在 ring3 前才报错）：
#   1) 所有 PT_LOAD 必须落在 4GiB..4GiB+64KiB（kernel/usermode64.h 的装载区，上界是
#      USER64_STACK_VA64）；elf64.cpp 会拒绝越界的段；
#   2) 无 PT_INTERP / 无 PT_DYNAMIC（内核只做静态装载）；
#   3) 入口落在某个 PT_LOAD 内、程序头表 <= 16 项（ELF64_MAX_PHDR64）且在第一个 PT_LOAD 里；
#   4) 文件长度 <= 96 KiB（kernel/elf64.h 的 ELF64_MAX_FILE_BYTES64 读盘缓冲）。
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
SRCDIR="user/shell"
LDSCRIPT="$SRCDIR/shell64.ld"
OBJ="$OUT/ush"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang --target=x86_64-unknown-none-elf"
LD=ld.lld

# 与 user/build_user.sh 逐条一致的编译选项（理由见那个脚本的头部）：
# -mcmodel=large 是**必须**的（映像在 4GiB，small 模型会生成 R_X86_64_32S 越界重定位）
# ★ 这里**没有** -fno-zero-initialized-in-bss：那一项是给"平铺 blob"路径用的（blob 长度要把
#   .bss 的零算进去）。本程序走 ELF64 装载器，.bss 由装载器按 p_memsz 映射并清零
#   （kernel/usermode64.cpp 的 user64_map_page64 会 zero），所以让 .bss 保持 NOBITS
#   —— 镜像少 12 KB，装载窗口更宽裕。
UCFLAGS="--target=x86_64-unknown-none-elf -nostdinc -I user/lib \
 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
 -mcmodel=large -mno-red-zone \
 -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
 -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra"
ASFLAGS="--target=x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
 -ffunction-sections -fdata-sections -Wall -Wextra"

mkdir -p "$OUT" "$OBJ"

# ---- 1) 运行时（user/lib 的源码里挑 shell 真的用到的那几个）----
#   syscall.c = 自有 ABI + Linux 号段的 POSIX 包装（open/read/write/close/stat/lseek/unlink）
#   string.c  = memcpy/memset/strlen/strcmp…（abi.c 与 main.c 用）
#   stdlib.c  = exit()（crt0.S 调它）
#   stdio.c   = exit() 会调 vimtu64_stdout_flush64()（在 stdio.c 里），所以必须链上
LIBSRC="syscall.c string.c stdlib.c stdio.c"
LIB_S="crt0.S syscall.S"
LIBOBJS=""
for src in $LIBSRC; do
    o="$OBJ/lib_${src%.c}.o"
    $CC $UCFLAGS -c "user/lib/$src" -o "$o"
    LIBOBJS="$LIBOBJS $o"
done
for src in $LIB_S; do
    o="$OBJ/lib_${src%.S}_asm.o"
    $CC $ASFLAGS -c "user/lib/$src" -o "$o"
    LIBOBJS="$LIBOBJS $o"
done

# ---- 2) shell 自己（main.c = 逻辑；abi.c = 系统调用包装）----
APPOBJS=""
for src in abi.c main.c; do
    o="$OBJ/sh_${src%.c}.o"
    $CC $UCFLAGS -I "$SRCDIR" -c "$SRCDIR/$src" -o "$o"
    APPOBJS="$APPOBJS $o"
done

# ---- 3) 链接（静态、基址 4GiB、gc-sections）----
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$LDSCRIPT" \
    -o "$OUT/shell.elf" $LIBOBJS $APPOBJS
cp "$OUT/shell.elf" "$OUT/shell.bin"          # 载荷字节（装进系统卷的 /bin/shell.bin）

# ---- 4) 自检（纯 Python 解析 ELF 头/程序头，不依赖 binutils）----
#    python 入口：优先 py -3（Windows 原生），退回 python（MSYS2）
if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi
$PY - "$OUT/shell.elf" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF", "不是 ELF"
assert d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2, "e_type 必须是 ET_EXEC（静态，不做重定位）"
assert machine == 0x3E, "e_machine 必须是 x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phentsize == 56, "e_phentsize 必须是 56"
assert 0 < phnum <= 16, "e_phnum 必须 <= 16（ELF64_MAX_PHDR64）"
LO, HI = 0x100000000, 0x100000000 + 0x10000           # 4GiB..4GiB+64KiB
segs, nload = [], 0
first_off = first_filesz = None
for i in range(phnum):
    p = phoff + i * phentsize
    ptype = struct.unpack_from("<I", d, p)[0]
    poff, pva, _ppa, pfsz, pmsz, _al = struct.unpack_from("<QQQQQQ", d, p + 8)
    if ptype == 3:
        raise SystemExit("ERROR: 出现 PT_INTERP（内核只做静态装载）")
    if ptype == 2:
        raise SystemExit("ERROR: 出现 PT_DYNAMIC（静态链接不该有）")
    if ptype != 1:
        continue
    nload += 1
    if first_off is None:
        first_off, first_filesz = poff, pfsz
    assert pmsz >= pfsz, "p_memsz < p_filesz"
    assert poff + pfsz <= len(d), "段文件范围越界"
    assert pva >= LO and pva + pmsz <= HI, "PT_LOAD 越出装载区（va=0x%x memsz=0x%x）" % (pva, pmsz)
    assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "PT_LOAD 页对齐后压到用户栈区"
    segs.append((pva, pmsz))
assert nload and nload <= 16, "PT_LOAD 数量不合法"
assert any(va <= entry < va + msz for va, msz in segs), "入口不在任何 PT_LOAD 内"
assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, \
    "程序头表不在第一个 PT_LOAD 内（auxv AT_PHDR 会是 0）"
assert len(d) <= 96 * 1024, "文件超过内核读盘缓冲 96 KiB"
print("    shell ELF 自检 OK：entry=0x%x phnum=%d segs=%d size=%d B span=0x%x..0x%x" %
      (entry, phnum, nload, len(d), segs[0][0], segs[-1][0] + segs[-1][1]))
PYEOF

echo "    shell = $OUT/shell.elf / $OUT/shell.bin（$(stat -c%s "$OUT/shell.elf") B；静态 ELF64，装载区 4GiB）"
