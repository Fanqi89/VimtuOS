#!/usr/bin/env bash
# tools/httpsprobe_build_win.sh - ★ v0.12.4：编 ring3 的 HTTPS 探针（/bin/httpsprobe）
#
# 产物：build64/httpsprobe.elf（静态 ELF64，基址 4GiB+576KiB）
#   = user/tls/httpsprobe.c              探针主体（socket BIO + 握手 + 证书信息 + GET）
#   + user/lib/mbedtls_vimtu_port.c      mbedtls_hardware_poll -> getrandom(318) -> kernel/rand64.cpp
#   + build64/mbedtls/libmbedtls.a       mbedTLS 3.6.2（tools/mbedtls_build_win.sh 的产物）
#   + third_party/musl/lib/libc.a        musl（getaddrinfo / malloc / snprintf / time …）
#
# 为什么链接脚本是 user/tls/httpsprobe64.ld 而不是 tools/musl_hello64.ld：
#   musl_hello 那份把映像钉在 4GiB（老装载区，硬限 64 KiB），本程序 + mbedTLS 一定超；
#   httpsprobe64.ld 钉在 4GiB+576KiB（USER64_MMAP_VA64）= 内核 elf64.cpp 的"大映像"区起点。
#
# 判据（脚本自己打，验收脚本 grep）：
#   [HTTPSPROBE] cc ok=<n> fail=0 libmbedtls=<bytes> elf=<bytes> text=<bytes> phdr=<n> base=0x100090000
#   [HTTPSPROBE] elfcheck ok=1 PT_LOAD=<n> span=<bytes> entry_in_load=1 no_interp=1 no_dynamic=1
# 任何一步失败都 exit 非 0（不静默跳过）。
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${1:-$ROOT/build64}"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
MBED="${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}"
OBJ="$BUILD/httpsprobe_obj"
OUT="$BUILD/httpsprobe.elf"
LDSCRIPT="$ROOT/user/tls/httpsprobe64.ld"
LIBMBED="$BUILD/mbedtls/libmbedtls.a"
LIBC_A="$MUSL/lib/libc.a"
CC=clang
LD=ld.lld

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

for f in "$ROOT/user/tls/httpsprobe.c" "$ROOT/user/lib/mbedtls_vimtu_port.c" "$LDSCRIPT"; do
    [ -f "$f" ] || { echo "缺少源文件：$f" >&2; exit 2; }
done
[ -f "$MBED/include/mbedtls/ssl.h" ] || { echo "缺少 mbedTLS 源码：$MBED" >&2; exit 2; }
[ -f "$LIBMBED" ] || { echo "缺少 $LIBMBED（先跑 tools/mbedtls_build_win.sh）" >&2; exit 2; }
[ -f "$LIBC_A" ]  || { echo "缺少 musl 静态库：$LIBC_A" >&2; exit 2; }

# ★ 用户覆盖配置必须与 libmbedtls.a **同一份**（配置影响结构体布局；不一致 = 运行期随机出鬼）。
#   照 tools/mbedtls_build_win.sh 的做法：只把这一个头放进独立 include 目录，
#   不把 user/lib 整个加进去（那会遮住 musl 的 stdio.h -> unknown type name 'FILE'）。
CFGDIR="$OBJ/cfginc"
mkdir -p "$OBJ" "$CFGDIR"
cp "$ROOT/user/lib/mbedtls_vimtu_user_config.h" "$CFGDIR/mbedtls_vimtu_user_config.h"
CFG_DEF="-DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_vimtu_user_config.h\""

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
# 注意：**不要** -mno-sse —— libmbedtls.a 是按默认（有 SSE2）编的，这里必须一致。
CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -I$MBED/include -I$CFGDIR $CFG_DEF \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector -fno-builtin \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections -fdata-sections \
 -std=gnu11 -O2 -Wall -Wextra"

echo "==> HTTPS 探针：$ROOT/user/tls/httpsprobe.c -> $OUT"
echo "    链接库：libmbedtls.a=$(stat -c%s "$LIBMBED") B  musl libc.a=$(stat -c%s "$LIBC_A") B"

ok=0
fail=0
: >"$OBJ/cc.log"
for src in "$ROOT/user/tls/httpsprobe.c" "$ROOT/user/lib/mbedtls_vimtu_port.c" "$ROOT/user/lib/compiler_rt_shim.c"; do
    o="$OBJ/$(basename "${src%.c}").o"
    if $CC $CFLAGS -c "$src" -o "$o" 2>>"$OBJ/cc.log"; then
        ok=$((ok+1))
    else
        fail=$((fail+1)); echo "  FAIL $(basename "$src")"
    fi
done
echo "[HTTPSPROBE] cc ok=$ok fail=$fail libmbedtls=$(stat -c%s "$LIBMBED")"
if [ "$fail" -gt 0 ]; then
    echo "--- 编译错误（前 40 行，完整见 $OBJ/cc.log）---"
    head -40 "$OBJ/cc.log"
    exit 1
fi

# ---- 128 位整数内建函数（__udivti3 等）：**我们自己补**，不用工具的 builtins ----
# mbedTLS 的 bignum.c 用 `unsigned __int128` 除法 -> 链接期实测
#   `undefined symbol: __udivti3 referenced by bignum.o:(mbedtls_mpi_div_mpi)`。
# 两条都试过、都不行才写的替身：
#   ① musl 的 libc.a 里没有 builtins；
#   ② msys2 的 mingw64/lib/clang/*/lib/windows/libclang_rt.builtins-x86_64.a 是 COFF，
#      ld.lld 直接拒收（实测 `archive member 'ashrdi3.c.obj' is neither ET_REL nor LLVM bitcode`）。
# 替身在 user/lib/compiler_rt_shim.c（只用移位/比较/减法，不会递归回自己）。
rm -f "$OUT"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$LDSCRIPT" \
    -o "$OUT" "$OBJ/httpsprobe.o" "$OBJ/mbedtls_vimtu_port.o" "$OBJ/compiler_rt_shim.o" \
    "$LIBMBED" "$LIBC_A" 2>&1 | head -40
if [ ! -f "$OUT" ]; then
    echo "[HTTPSPROBE] link FAILED（见上面的链接器输出）" >&2
    exit 1
fi

ELF_SZ=$(stat -c%s "$OUT")
# ★ 不打印 text 段字节数：llvm-size 在这台 msys 上对 ELF 的输出列序不同，
#   早先那版 `awk 'NR==2{print $1}'` 打出来恒为 0 —— 一个假数字比没有数字更糟。
#   真值由下面的 elfcheck（纯 Python 解析程序头）给出：PT_LOAD 个数 + 映像 span。
echo "[HTTPSPROBE] elf=$ELF_SZ"

# ---- 内核装载路径的硬约束自检（纯 Python 解析 ELF 头/程序头，不依赖 binutils）----
$PY - "$OUT" <<'PYEOF'
import struct, sys
path = sys.argv[1]
d = open(path, "rb").read()
assert d[:4] == b"\x7fELF", "不是 ELF"
assert d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "不是 ET_EXEC/x86_64"
BASE = 0x100090000
phoff = struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
entry = struct.unpack_from("<Q", d, 24)[0]
segs = []
span_lo, span_hi = None, None
first = None
for i in range(phn):
    o = phoff + i * phes
    t, flags = struct.unpack_from("<II", d, o)
    off, va, pa, fsz, msz = struct.unpack_from("<QQQQQ", d, o + 8)
    assert t not in (2, 3), "出现 PT_DYNAMIC/PT_INTERP（静态非 PIE 不该有）"
    if t != 1:
        continue
    if first is None:
        first = (off, va)
    segs.append((va, msz))
    assert va >= BASE, "PT_LOAD 落在 USER64_MMAP_VA64 之下（va=%#x）" % va
    span_lo = va if span_lo is None else min(span_lo, va)
    span_hi = va + msz if span_hi is None else max(span_hi, va + msz)
assert segs, "没有 PT_LOAD"
assert first[0] == 0 and first[1] == BASE, \
    "第一个 PT_LOAD 必须 offset=0 / va=USER64_MMAP_VA64（实测 off=%#x va=%#x）" % (first[0], first[1])
assert span_lo == BASE, "min(p_vaddr)=%#x 必须正好是 USER64_MMAP_VA64（否则内核装载 base != 0，绝对地址错位）" % span_lo
assert any(va <= entry < va + m for va, m in segs), "入口不在任何 PT_LOAD 内"
assert len(d) <= 8 * 1024 * 1024, "超过文件系统/装载器的 8 MiB 上限"
print("[HTTPSPROBE] elfcheck ok=1 PT_LOAD=%d span=%d entry_in_load=1 no_interp=1 no_dynamic=1 base=%#x"
      % (len(segs), span_hi - span_lo, BASE))
PYEOF
rc=$?
if [ "$rc" -ne 0 ]; then
    echo "[HTTPSPROBE] ELF 自检 FAILED" >&2
    exit 1
fi

# ★ 交付纪律：内核二进制里**不许**出现探针的字节（探针只从系统卷装载）。
#   这里不直接查内核（内核可能还没编），由 build64.sh 末尾的 probe64.py 统一查；
#   本脚本只保证产物落点正确。
echo "[HTTPSPROBE] done elf=$ELF_SZ -> $OUT"
exit 0
