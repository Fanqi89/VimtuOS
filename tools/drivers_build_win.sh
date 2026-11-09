#!/bin/bash
# tools/drivers_build_win.sh - ★ v0.9.33：编 ring3 驱动框架的两个用户态程序
#   user/drivers/ext4svc.c   -> build64/ext4svc   （ext4 只读驱动服务）
#   user/drivers/drvwatch.c  -> build64/drvwatch  （看门狗观察进程）
# 与 tools/vmtool_build_win.sh 同一套约束：静态 ELF64、无 libc、无 PT_INTERP、
#   -mcmodel=large、主程序窗口 < 64 KiB、-Wall -Wextra 零告警。
set -e
export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
OBJ="$OUT/drv_obj"
CC=clang
LD=ld.lld
mkdir -p "$OUT" "$OBJ"

CFLAGS_COMMON="-target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra"

echo "==> ★ v0.9.33：编 ring3 驱动（ext4svc / drvwatch）"
$CC $CFLAGS_COMMON -c "user/drivers/ext4svc.c"  -o "$OBJ/ext4svc.o"
$CC $CFLAGS_COMMON -c "user/drivers/drvwatch.c" -o "$OBJ/drvwatch.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "user/drivers/r3drv_start.S" -o "$OBJ/r3drv_start.o"

echo "==> 链接（静态 ELF64，4 GiB 起）"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "user/drivers/r3drv64.ld" \
    -o "$OUT/ext4svc"  "$OBJ/r3drv_start.o" "$OBJ/ext4svc.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "user/drivers/r3drv64.ld" \
    -o "$OUT/drvwatch" "$OBJ/r3drv_start.o" "$OBJ/drvwatch.o"

for f in ext4svc drvwatch; do
    SZ=$(stat -c%s "$OUT/$f")
    echo "    build64/$f = $SZ B"
    if [ "$SZ" -gt 65536 ]; then
        echo "ERROR: $f 超过 64 KiB 主程序窗口（$SZ B）" >&2
        exit 1
    fi
done
