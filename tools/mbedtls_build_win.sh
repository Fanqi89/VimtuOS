#!/usr/bin/env bash
# tools/mbedtls_build_win.sh - ★ v0.12.3：用**我们自己的 musl 交叉工具链**把 mbedTLS 的
# 库源码编成 `build64/mbedtls/libmbedtls.a`（静态，给 ring3 的 HTTPS 客户端用）。
#
# 为什么先只编 library/：TLS 客户端只用得到 library/*.c（ssl / x509 / 大数 / AES-GCM / SHA /
# ECDHE…）。programs/ 与 tests/ 需要 POSIX 进程与文件系统细节，不是第一优先级。
#
# 判据（脚本自己打，验收脚本 grep）：
#   [MBEDTLS] cc ok=<n> fail=<n> objs=<n> archive=<bytes>B
# 失败时逐条打印**前 20 条**编译错误（不静默跳过：fail>0 就 exit 1）。
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH   # clang / llvm-ar / llvm-* 都在 mingw64/bin
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
OUT="$ROOT/build64/mbedtls"
OBJ="$OUT/obj"
mkdir -p "$OBJ"
export LC_ALL=C
CC=clang

if [ ! -f "$SRC/library/ssl_tls.c" ]; then
  echo "找不到 mbedTLS 源码：$SRC（先跑 py -3 tools/fetch_port_sources.py）" >&2
  exit 2
fi
if [ ! -f "$MUSL/include/stdio.h" ] || [ ! -f "$MUSL/lib/libc.a" ]; then
  echo "找不到 musl 头/静态库：$MUSL（先跑 tools/musl_build_win.sh）" >&2
  exit 2
fi

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
 -fno-stack-protector -fno-builtin -O2 \
 -I$SRC/include -I$SRC/library -w"

ok=0
fail=0
: >"$OUT/cc_fail.log"
for f in "$SRC"/library/*.c; do
  b="$(basename "$f" .c)"
  if $CC $CFLAGS -c "$f" -o "$OBJ/$b.o" 2>>"$OUT/cc_fail.log"; then
    ok=$((ok+1))
  else
    fail=$((fail+1))
    echo "  FAIL $b" >>"$OUT/cc_fail.log"
  fi
done

objs=$(ls "$OBJ"/*.o 2>/dev/null | wc -l)
rm -f "$OUT/libmbedtls.a"
if [ "$objs" -gt 0 ]; then
  llvm-ar rcs "$OUT/libmbedtls.a" "$OBJ"/*.o 2>/dev/null || ar rcs "$OUT/libmbedtls.a" "$OBJ"/*.o
fi
size=$(stat -c %s "$OUT/libmbedtls.a" 2>/dev/null || echo 0)

echo "[MBEDTLS] cc ok=$ok fail=$fail objs=$objs archive=$size"
if [ "$fail" -gt 0 ]; then
  echo "--- 前 20 条编译错误（完整见 build64/mbedtls/cc_fail.log）---"
  grep -E "error:" "$OUT/cc_fail.log" | head -20
  exit 1
fi
exit 0
