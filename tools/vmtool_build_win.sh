#!/bin/bash
# tools/vmtool_build_win.sh - ★ v0.9.30：编 vmtool（ACL/配额/日志/快照的 ring-3 入口）
#   user/vmtool/vmtool.c -> build64/vmtool（静态 ELF64，链接在 4 GiB，主程序窗口 < 64 KiB）
#   与 tools/dpkg_build_win.sh 同一套约束：无 libc、无 PT_INTERP、-mcmodel=large、-Wall -Wextra 零告警。
set -e
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
OBJ="$OUT/vmtool_obj"
CC=clang
LD=ld.lld
mkdir -p "$OUT" "$OBJ"
echo "==> ★ v0.9.30：编 vmtool.c（-Wall -Wextra 零告警）"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "user/vmtool/vmtool.c" -o "$OBJ/vmtool.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "user/vmtool/vmtool_start.S" -o "$OBJ/vmtool_start.o"
echo "==> 链接（静态 ELF64，4 GiB 起）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "user/vmtool/vmtool64.ld" \
    -o "$OUT/vmtool" "$OBJ/vmtool_start.o" "$OBJ/vmtool.o"
SZ=$(stat -c%s "$OUT/vmtool")
echo "    build64/vmtool = $SZ B"
if [ "$SZ" -gt 65536 ]; then echo "ERROR: vmtool 超过 64 KiB 主程序窗口（$SZ B）" >&2; exit 1; fi
