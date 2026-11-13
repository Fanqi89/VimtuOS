#!/bin/bash
# tools/peload_build_win.sh - ★ v0.9.44（W1）：构建用户态 PE 装载器 /bin/peload
#
# 产物（全部落盘 build64/；**一个字节都不进内核镜像**）：
#   build64/peload    静态 ELF64，链接在用户窗口低段（4GiB 起）
#                     -> 卷里 /bin/peload，用 `run /bin/peload <file.exe>` 直接跑
#
# 为什么自足（不引 user/lib / musl）：见 user/win/peload.c 文件头 —— 走内核**主程序装载器**，
# 映像（含 .bss）必须很小；PE 文件内容与 PE 映像全都在运行期 mmap(9) 里。
#
# 三个编译单元：
#   user/win/peload.c         主干（解析 + 映射 + 重定位 + IAT + 跳入口）
#   user/win/peload_start.S   _start -> peload_entry(rsp)
#   user/win/peload_stubs.S   ★ IAT 桩的 Win64 -> SysV 跳板（见该文件头的 ABI 说明）
#
# 用法：bash tools/peload_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

# ★ PATH 必须**先**设：本脚本可能被 PowerShell 直接 `bash tools/peload_build_win.sh` 调用
#   （那时 PATH 是 Windows 的，连 `dirname` 都找不到）。build64.sh 调用时设两次也无害。
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
APPS="user/win"
OBJ="$OUT/peload_obj"

CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

mkdir -p "$OUT" "$OBJ"

echo "==> W1 1/3: compile peload.c + peload_stubs.S + peload_start.S (freestanding)"
# -mcmodel=large：映像在 4GiB（small/medium 会产生 R_X86_64_32/32S 越界重定位）
# -mno-sse*：本程序自己不用 FP/SSE（PE 那边能不能用是另一回事，见 peload.c 文件头第 3 条）
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/peload.c" -o "$OBJ/peload.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/peload_stubs.S" -o "$OBJ/peload_stubs.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/peload_start.S" -o "$OBJ/peload_start.o"

echo "==> W1 2/3: link (static ELF64 at 4GiB, kernel main-program loader can run it)"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/peload64.ld" \
    -o "$OUT/peload" "$OBJ/peload_start.o" "$OBJ/peload_stubs.o" "$OBJ/peload.o"

echo "==> W1 3/3: build-time assertions (ELF header / program headers / stubs present / size)"
$PY - "$OUT/peload" <<'PYEOF'
import os, struct, sys

BASE64  = 0x100000000             # USER64_CODE_VA64
STACK64 = 0x100000000 + 0x10000   # USER64_STACK_VA64（本程序是自足小工具，保持"进低 64 KiB"这条严约束）

path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "peload is not ELF64 little-endian"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "peload must be ET_EXEC / x86_64"
entry = struct.unpack_from("<Q", d, 24)[0]
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert 0 < phn <= 16 and phes == 56, "program header table malformed"
first = None
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
    if t == 3: raise SystemExit("ERROR: peload has PT_INTERP (kernel only does static load)")
    if t == 2: raise SystemExit("ERROR: peload has PT_DYNAMIC")
    if t != 1: continue
    assert va >= BASE64 and va + msz <= STACK64, \
        "PT_LOAD outside 4GiB..4GiB+64KiB (va=%#x msz=%#x)" % (va, msz)
    if first is None:
        first = (off, fsz)
assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
    "program header table not inside first PT_LOAD (auxv AT_PHDR would be 0)"
assert len(d) <= 64 * 1024, "peload exceeds 64 KiB"

# ★ 桩跳板必须真的在映像里：'movq %rcx,%rdi' = 48 89 cf ... 这里只查符号串（-s 未 strip，
#   .symtab 在 --gc-sections 下仍保留文本符号）。三道桩缺一条就说明汇编单元没链进来。
for sym in (b"pl_stub_ExitProcess", b"pl_stub_GetProcAddress", b"pl_stub_LoadLibraryA"):
    assert sym in d, "missing stub symbol %r (peload_stubs.S not linked?)" % sym
print("    assertion OK: %s = %d B, entry=%#x phnum=%d (static ET_EXEC, 4GiB window, 3 stubs)"
      % (os.path.basename(path), len(d), entry, phn))
PYEOF

echo
echo "    --- W1 size accounting (goes to system volume /bin/peload, not into the kernel) ---"
echo "    build64/peload = $(stat -c%s "$OUT/peload") B"
