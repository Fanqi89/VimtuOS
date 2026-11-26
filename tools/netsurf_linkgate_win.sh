#!/usr/bin/env bash
# tools/netsurf_linkgate_win.sh - ★ v0.12.5：NetSurf 库栈的**链接门禁**（"库能编 ≠ 能用"）
#
# 两道判据：
#   ① **whole-archive**：把 14 个 .a 整个塞进链接器。静态库默认只拉"被引用到的成员"，
#      所以"能链上"其实什么都证明不了；whole-archive 会强制每个 .o 进链接，
#      任何库内部引用了我们没编的东西（典型：libsvgtiny 要 `dom_xml_parser_create`、
#      它来自 libdom 的 expat 绑定）都会立刻变成 undefined symbol —— 这正是我们要它抓的。
#   ② 真程序：user/netsurf/smoke.c 调用 libwapcaplet + libdom(+hubbub 绑定) 把一段 HTML 解成 DOM
#      并读出根元素名。产出与 httpsprobe 同构的静态 ELF（基址 0x100090000），
#      所以将来它可以直接被装进系统卷在客人里跑（本轮只做链接+符号判据）。
#
# 判据（脚本自己打）：
#   [NSLINK] whole-archive ok=1 archives=14
#   [NSLINK] smoke elf=<bytes> PT_LOAD=<n> base=0x100090000 symbols=<n>
# 失败时 exit 1 并把前 30 行链接错误打出来（不静默）。
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT_DIR:-$ROOT/build64/netsurf}"
LIBDIR="$OUT/lib"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
OBJ="$OUT/smoke"
LDSCRIPT="$ROOT/user/tls/httpsprobe64.ld"
CC=clang
LD=ld.lld

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

LIBS="liblibwapcaplet liblibparserutils liblibhubbub liblibcss liblibdom \
liblibnsgif liblibnsbmp liblibnsutils liblibnspsl liblibnslog liblibsvgtiny liblibrosprite \
libexpat libzlib"

missing=0
for l in $LIBS; do
  [ -f "$LIBDIR/$l.a" ] || { echo "[NSLINK] 缺 $LIBDIR/$l.a（先跑 tools/netsurf_lib_build_win.sh）" >&2; missing=1; }
done
[ "$missing" = "0" ] || exit 2

nf=$(echo $LIBS | wc -w)
mkdir -p "$OBJ"

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
NS="$ROOT/third_party/netsurf"
CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
 -fno-stack-protector -fno-builtin -O2 -w \
 -I$NS/libwapcaplet/include -I$NS/libparserutils/include -I$NS/libhubbub/include \
 -I$NS/libcss/include -I$NS/libdom/include -I$NS/libdom -I$NS/libnsutils/include"

# ---------------- ② 先编 smoke（语法/接口层面就能抓错） ----------------
if ! $CC $CFLAGS -c "$ROOT/user/netsurf/smoke.c" -o "$OBJ/smoke.o" 2>"$OBJ/cc.log"; then
  echo "[NSLINK] FAIL smoke.c 编不过（前 30 行）："
  head -30 "$OBJ/cc.log"
  exit 1
fi

# ---------------- ⓪ 先编 libm_shim（fabs/fabsf：musl 归档里缺这两个对象） ----------------
if ! $CC $CFLAGS -c "$ROOT/user/netsurf/libm_shim.c" -o "$OBJ/libm_shim.o" 2>"$OBJ/cc_m.log"; then
  echo "[NSLINK] FAIL libm_shim.c 编不过："
  head -20 "$OBJ/cc_m.log"
  exit 1
fi

# ---------------- ① whole-archive 链接（不带 smoke 的 main，只验证库之间自洽） ----------------
WANT="$OBJ/whole.txt"
: > "$WANT"
for l in $LIBS; do echo "--whole-archive $LIBDIR/$l.a --no-whole-archive" >> "$WANT"; done
# 用一个空 main 强制产出可执行文件；库内部互相引用的符号必须全部能解析
cat > "$OBJ/empty.c" <<'EOF'
int main(void) { return 0; }
EOF
$CC $CFLAGS -c "$OBJ/empty.c" -o "$OBJ/empty.o" 2>/dev/null
if $LD -m elf_x86_64 -static -z noexecstack -T "$LDSCRIPT" \
      -o "$OBJ/whole.elf" "$OBJ/empty.o" "$OBJ/libm_shim.o" \
      --whole-archive $LIBDIR/liblibwapcaplet.a $LIBDIR/liblibparserutils.a \
      $LIBDIR/liblibhubbub.a $LIBDIR/liblibcss.a $LIBDIR/liblibdom.a \
      $LIBDIR/liblibnsgif.a $LIBDIR/liblibnsbmp.a $LIBDIR/liblibnsutils.a \
      $LIBDIR/liblibnspsl.a $LIBDIR/liblibnslog.a $LIBDIR/liblibsvgtiny.a \
      $LIBDIR/liblibrosprite.a $LIBDIR/libexpat.a $LIBDIR/libzlib.a \
      --no-whole-archive "$MUSL/lib/libc.a" "$MUSL/lib/libm.a" 2>"$OBJ/whole.log"; then
  echo "[NSLINK] whole-archive ok=1 archives=$nf"
else
  echo "[NSLINK] whole-archive FAIL（前 30 行）："
  head -30 "$OBJ/whole.log"
  exit 1
fi

# ---------------- ② smoke 真链接 ----------------
# 注意：这里**不用** whole-archive（那会把每个 .o 都拉进来，包括只有测试才用的符号）；
# smoke 只引用 wapcaplet + dom + hubbub，其余库按需拉取。
if $LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$LDSCRIPT" \
      -o "$OBJ/smoke.elf" "$OBJ/smoke.o" "$OBJ/libm_shim.o" \
      $LIBDIR/liblibdom.a $LIBDIR/liblibhubbub.a $LIBDIR/liblibparserutils.a \
      $LIBDIR/liblibwapcaplet.a $LIBDIR/libexpat.a $LIBDIR/libzlib.a \
      "$MUSL/lib/libc.a" "$MUSL/lib/libm.a" 2>"$OBJ/link.log"; then
  :
else
  echo "[NSLINK] smoke 链接 FAIL（前 30 行）："
  head -30 "$OBJ/link.log"
  exit 1
fi

SZ=$(stat -c %s "$OBJ/smoke.elf" 2>/dev/null || echo 0)
SYMS=$({ nm -g --defined-only "$OBJ/smoke.elf" 2>/dev/null; } | grep -cE "^[0-9a-f]+ [TDBR] " || true)
$PY - "$OBJ/smoke.elf" "$SZ" "$SYMS" <<'PYEOF'
import struct, sys
path, size, syms = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
d = open(path, "rb").read()
BASE = 0x100090000
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
n, lo = 0, None
for i in range(phn):
    o = phoff + i * phes
    t = struct.unpack_from("<I", d, o)[0]
    va = struct.unpack_from("<Q", d, o + 16)[0]
    if t != 1:
        continue
    n += 1
    lo = va if lo is None else min(lo, va)
assert n >= 1 and lo == BASE, "PT_LOAD 基址错：%s（应为 %#x）" % (hex(lo) if lo else None, BASE)
print("[NSLINK] smoke elf=%d PT_LOAD=%d base=%#x symbols=%d" % (size, n, BASE, syms))
PYEOF
rc=$?
[ "$rc" -eq 0 ] || { echo "[NSLINK] ELF 自检 FAILED" >&2; exit 1; }
echo "[NSLINK] ALL OK（whole-archive 14 个库 + smoke 真链接）"
exit 0
