#!/bin/bash
# L1-DEPRECATED (v0.9.42): build64.sh no longer calls this script.
#
# Why: L1 makes /usr/bin/dpkg and /usr/bin/apt the SAME BYTES as /bin/vpkg
# (see tools/dpkg_pack_win.py + the header of user/store/vpkg.c, which picks
# the front-end from basename(argv[0])).  user/dpkg/dpkg.c is a SECOND .deb
# engine (gzip only, shells out to /usr/bin/tar, no Depends, remove keeps files)
# and is no longer compiled or shipped.  Kept for reference / archaeology.
# Running it still works but build64/dpkg is unused: dpkg_pack_win.py now
# defaults to build64/store/vpkg.elf.
# tools/dpkg_build_win.sh - ? v0.7.5?????? dpkg / apt?Ring 3??? ELF64?< 64 KiB ????
set -e
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
OBJ="$OUT/dpkg_obj"
CC=clang
LD=ld.lld
mkdir -p "$OUT" "$OBJ"
echo "==> ? v0.7.5??? dpkg.c????-Wall -Wextra?"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "user/dpkg/dpkg.c" -o "$OBJ/dpkg.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "user/dpkg/dpkg_start.S" -o "$OBJ/dpkg_start.o"
echo "==> ★ v0.7.5：链接（静态 ELF64，4 GiB 起，内核主程序装载器直接装）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "user/dpkg/dpkg64.ld" \
    -o "$OUT/dpkg" "$OBJ/dpkg_start.o" "$OBJ/dpkg.o"
SZ=$(stat -c%s "$OUT/dpkg")
echo "    build64/dpkg = $SZ B"
if [ "$SZ" -gt 65536 ]; then echo "ERROR: dpkg 超过 64 KiB 主程序窗口（$SZ B）" >&2; exit 1; fi
