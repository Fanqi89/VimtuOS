#!/bin/bash
# tools/peinfo_build_win.sh - ★ v0.9.43（W0）：构建用户态 PE 只读体检器 /bin/peinfo
#
# 产物（全部落盘 build64/；**一个字节都不进内核镜像**）：
#   build64/peinfo   静态 ELF64，链接在用户窗口低段（4GiB 起）
#                    -> 卷里 /bin/peinfo，用 `run /bin/peinfo <file.exe>` 直接跑
#
# 为什么自足（不引 user/lib / musl）：见 user/win/peinfo.c 文件头 —— 走内核**主程序装载器**，
# 映像（含 .bss）必须很小；文件内容缓冲与 PE 解析全部基于运行期 mmap(9)。
#
# 用法：bash tools/peinfo_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

# ★ PATH 必须**先**设：本脚本可能被 PowerShell 直接 `bash tools/peinfo_build_win.sh` 调用
#   （那时 PATH 是 Windows 的，连 `dirname` 都找不到）。build64.sh 调用时设两次也无害。
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
APPS="user/win"
OBJ="$OUT/peinfo_obj"

CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

mkdir -p "$OUT" "$OBJ"

echo "==> W0 1/3: compile peinfo.c (freestanding, -Wall -Wextra must be silent)"
# -mcmodel=large：映像在 4GiB（small/medium 会产生 R_X86_64_32/32S 越界重定位）
# -mno-sse*：内核与用户态都不开 FP/SSE
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/peinfo.c" -o "$OBJ/peinfo.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/peinfo_start.S" -o "$OBJ/peinfo_start.o"

echo "==> W0 2/3: link (static ELF64 at 4GiB, kernel main-program loader can run it)"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/peinfo64.ld" \
    -o "$OUT/peinfo" "$OBJ/peinfo_start.o" "$OBJ/peinfo.o"

echo "==> W0 3/3: build-time assertions (ELF header / program headers / size)"
$PY - "$OUT/peinfo" <<'PYEOF'
import os, struct, sys

BASE64  = 0x100000000             # USER64_CODE_VA64
STACK64 = 0x100000000 + 0x10000   # USER64_STACK_VA64（装载区上界，P0.1 后实际可到 +998MiB，
                                  #   但本程序是自足小工具，保持"进低 64 KiB"这条更严的旧约束）

path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "peinfo is not ELF64 little-endian"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "peinfo must be ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert 0 < phn <= 16 and phes == 56, "program header table malformed"
first = None
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
    if t == 3: raise SystemExit("ERROR: peinfo has PT_INTERP (kernel only does static load)")
    if t == 2: raise SystemExit("ERROR: peinfo has PT_DYNAMIC")
    if t != 1: continue
    assert va >= BASE64 and va + msz <= STACK64, \
        "PT_LOAD outside 4GiB..4GiB+64KiB (va=%#x msz=%#x)" % (va, msz)
    if first is None:
        first = (off, fsz)
assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
    "program header table not inside first PT_LOAD (auxv AT_PHDR would be 0)"
assert len(d) <= 64 * 1024, "peinfo exceeds 64 KiB"
print("    assertion OK: %s = %d B, entry=%#x phnum=%d (static ET_EXEC, 4GiB load window)"
      % (os.path.basename(path), len(d), entry, phn))
PYEOF

echo
echo "    --- W0 size accounting (goes to system volume /bin/peinfo, not into the kernel) ---"
echo "    build64/peinfo = $(stat -c%s "$OUT/peinfo") B"
