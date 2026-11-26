#!/usr/bin/env bash
# tools/browser_build_win.sh - ★ v0.12.6：把 NetSurf 核心 + 我们自己的 fetcher/frontend 链成 /bin/browser
#
# 交付物：build64/browser.elf（静态 ELF64，基址 0x100090000 = USER64_MMAP_VA64，内核 mode-2 装载）
#
# 链接组成（顺序不能乱）：
#   user/browser/*.c                前端（gui/plot/layout/bitmap/corewindow/外壳/main）
#   user/browser/fetchers/vimtu.c   已在 libnetsurfcore.a 里（tools/netsurf_core_build_win.sh）
#   libnetsurfcore.a                99+ 个 .c（含 vimtu fetcher）
#   14 个依赖库                      wapcaplet/parserutils/hubbub/css/dom/nsgif/nsbmp/nsutils/nspsl/nslog/svgtiny/rosprite/expat/zlib
#   libmbedtls.a                    TLS
#   musl libc.a + compiler_rt_shim  运行时（NetSurf 要 malloc/stdio/qsort/strtod）
#
# 判据（脚本自己打）：
#   [BROWSER] cc ok=N fail=0
#   [BROWSER] link ok elf=<bytes> PT_LOAD=<n> span=<bytes> base=0x100090000
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build64}"
NS="${NETSURF_DIR:-$ROOT/third_party/netsurf}"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
MBED="${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}"
NSCORE="$BUILD/netsurf"
GEN="$NSCORE/gen"
OBJ="$BUILD/browser_obj"
OUT="$BUILD/browser.elf"
LDSCRIPT="$ROOT/user/tls/httpsprobe64.ld"
CC=clang
LD=ld.lld

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

[ -f "$NSCORE/lib/libnetsurfcore.a" ] || { echo "缺少 $NSCORE/lib/libnetsurfcore.a（先跑 tools/netsurf_core_build_win.sh）" >&2; exit 2; }
[ -f "$BUILD/mbedtls/libmbedtls.a" ] || { echo "缺少 libmbedtls.a（先跑 tools/mbedtls_build_win.sh）" >&2; exit 2; }
[ -f "$MUSL/lib/libc.a" ]           || { echo "缺少 musl libc.a" >&2; exit 2; }
[ -f "$GEN/nscfg.h" ]               || { echo "缺少 $GEN/nscfg.h（先跑一次 tools/netsurf_core_build_win.sh）" >&2; exit 2; }

mkdir -p "$OBJ"
cp "$ROOT/user/lib/mbedtls_vimtu_user_config.h" "$GEN/mbedtls_vimtu_user_config.h" 2>/dev/null || true

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
INCS="-I$ROOT/user/browser -I$NS -I$NS/include -I$NS/content/handlers -I$GEN -I$MBED/include"
for p in libwapcaplet libparserutils libhubbub libcss libdom libnsgif libnsbmp \
         libnsutils libnspsl libnslog libsvgtiny librosprite; do
  INCS="$INCS -I$NS/$p/include"
done
INCS="$INCS -I$ROOT/third_party/expat/expat/lib -I$ROOT/third_party/zlib"

CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC $INCS \
 -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_vimtu_user_config.h\" -Dsmall -Dnsvimtu -include $GEN/nscfg.h \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
 -std=gnu11 -O2 -Wall"

SRCS=$(ls "$ROOT/user/browser"/*.c 2>/dev/null)
if [ -z "$SRCS" ]; then
  echo "没找到前端源码（$ROOT/user/browser/*.c）" >&2
  exit 2
fi

ok=0; fail=0
: >"$OBJ/cc.log"
rm -f "$OBJ"/*.o
for src in $SRCS; do
  o="$OBJ/$(basename "${src%.c}").o"
  if $CC $CFLAGS -c "$src" -o "$o" 2>>"$OBJ/cc.log"; then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "  FAIL $(basename "$src")"
  fi
done
# 运行时胶水（mbedTLS 熵源 + 128 位内建）
for src in "$ROOT/user/lib/mbedtls_vimtu_port.c" "$ROOT/user/lib/compiler_rt_shim.c"; do
  o="$OBJ/$(basename "${src%.c}").o"
  if $CC $CFLAGS -c "$src" -o "$o" 2>>"$OBJ/cc.log"; then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "  FAIL $(basename "$src")"
  fi
done

echo "[BROWSER] cc ok=$ok fail=$fail"
if [ "$fail" -gt 0 ]; then
  echo "--- 编译错误（前 40 行，完整见 $OBJ/cc.log）---"
  grep -E "error:" "$OBJ/cc.log" | head -40
  exit 1
fi

# ---- 链接 ----
LIBS="$NSCORE/lib/libnetsurfcore.a"
for l in liblibcss liblibdom liblibhubbub liblibparserutils liblibwapcaplet \
         liblibnsgif liblibnsbmp liblibnsutils liblibnspsl liblibnslog \
         liblibsvgtiny liblibrosprite libexpat libzlib; do
  [ -f "$NSCORE/lib/$l.a" ] && LIBS="$LIBS $NSCORE/lib/$l.a"
done
LIBS="$LIBS $BUILD/mbedtls/libmbedtls.a"

rm -f "$OUT"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$LDSCRIPT" \
    -o "$OUT" "$OBJ"/*.o \
    --start-group $LIBS --end-group \
    "$MUSL/lib/libc.a" 2>&1 | head -40
if [ ! -f "$OUT" ]; then
  echo "[BROWSER] link FAILED（见上面的链接器输出）" >&2
  exit 1
fi

ELF_SZ=$(stat -c%s "$OUT")
$PY - "$OUT" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF", "不是 ELF"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "不是 ET_EXEC/x86_64"
BASE = 0x100090000
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
entry = struct.unpack_from("<Q", d, 24)[0]
segs, lo, hi, first = [], None, None, None
for i in range(phn):
    o = phoff + i * phes
    t, flags = struct.unpack_from("<II", d, o)
    off, va, pa, fsz, msz = struct.unpack_from("<QQQQQ", d, o + 8)
    assert t not in (2, 3), "出现 PT_DYNAMIC/PT_INTERP"
    if t != 1:
        continue
    if first is None:
        first = (off, va)
    segs.append((va, msz))
    assert va >= BASE, "PT_LOAD 落在 USER64_MMAP_VA64 之下"
    lo = va if lo is None else min(lo, va)
    hi = va + msz if hi is None else max(hi, va + msz)
assert segs and first[0] == 0 and first[1] == BASE, "首个 PT_LOAD 必须 offset=0/va=BASE"
assert lo == BASE, "min(p_vaddr) 必须正好是 BASE"
assert any(va <= entry < va + m for va, m in segs), "入口不在任何 PT_LOAD 内"
print("[BROWSER] link ok elf=%d PT_LOAD=%d span=%d base=%#x"
      % (len(d), len(segs), hi - lo, BASE))
PYEOF
rc=$?
if [ "$rc" -ne 0 ]; then
  echo "[BROWSER] ELF 自检 FAILED" >&2
  exit 1
fi

echo "[BROWSER] done elf=$ELF_SZ -> $OUT"
exit 0
