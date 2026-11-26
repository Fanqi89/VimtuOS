#!/usr/bin/env bash
# tools/netsurf_core_build_win.sh - v0.12.6：把 **NetSurf 本体（core）** 编成 libnetsurfcore.a
#
# 和 tools/netsurf_lib_build_win.sh 的关系：
#   - 那 14 个依赖库已经就位（build64/netsurf/lib/*.a）；
#   - 本脚本编的是 **NetSurf 自己的 ~230 个 .c**（content/ + desktop/ + utils/ + handlers/），
#     产物 build64/netsurf/lib/libnetsurfcore.a，供 frontends/vimtu（我们的浏览器壳子）链接。
#
# 为什么按"组"编而不是一个巨大 find：出错的组一眼可见，重编单组只要 10 秒
#   bash tools/netsurf_core_build_win.sh handlers
#
# 用法：
#   bash tools/netsurf_core_build_win.sh              # 全部组
#   bash tools/netsurf_core_build_win.sh desktop      # 只编一组
#   bash tools/netsurf_core_build_win.sh --list       # 看组名
#
# 判据（每个组一行）：
#   [core:<group>] cc ok=<n> fail=0 archive=<bytes> symbols=<m>
#   任一组 fail>0 → 打印前 20 条 error 并退出 1
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NS="${NETSURF_DIR:-$ROOT/third_party/netsurf}"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
OUT="${OUT_DIR:-$ROOT/build64/netsurf}"
OBJ="$OUT/obj_core"
LIBDIR="$OUT/lib"
mkdir -p "$OBJ" "$LIBDIR"
export LC_ALL=C
CC=clang

if [ ! -d "$NS/libwapcaplet" ]; then
  echo "找不到 NetSurf 源码树：$NS（先跑 py -3 tools/fetch_port_sources.py）" >&2
  exit 2
fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
  echo "找不到 musl 静态库：$MUSL" >&2
  exit 2
fi

MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
BASE_FLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
 -fno-stack-protector -fno-builtin -ffunction-sections -fdata-sections -O2 -w"

# 构建期生成物目录（nscfg.h / testament.h）
GEN="$OUT/gen"
mkdir -p "$GEN"

# ① nscfg.h：把官方靠 -D 传的三样东西写成真头文件（走 -D 会掉进 shell 引号地狱：
#    实测 `-DNETSURF_BUILTIN_LOG_FILTER='"all"'` 让 options.h 拿到的是多字符常量 -> int）
cat > "$GEN/nscfg.h" <<'NSCFG'
#ifndef VIMTU_NSCFG_H
#define VIMTU_NSCFG_H
#define NETSURF_BUILTIN_LOG_FILTER "all"
#define NETSURF_BUILTIN_VERBOSE_FILTER "nslog,llcache,login,plot"
#define NETSURF_BUILTIN_HOME_URL "https://example.com/"
#define NETSURF_BUILTIN_CA_BUNDLE "/etc/ssl/certs/ca.pem"
#endif
NSCFG

# ② testament.h：官方 tools/git-testament.pl 生成（desktop/version.c 无条件 include 它）
if [ ! -f "$GEN/testament.h" ]; then
  ( cd "$NS" && perl tools/git-testament.pl "$NS" "$GEN/testament.h" ) >/dev/null 2>&1 || true
fi
if [ ! -s "$GEN/testament.h" ]; then
  cat > "$GEN/testament.h" <<'TESTAMENT'
#ifndef NETSURF_TESTAMENT_H
#define NETSURF_TESTAMENT_H
#define WT_ROOT "vimtuos"
#define WT_BRANCH "vimtu"
#define WT_VERSION "0.12.6-vimtu"
#define WT_COMPILEDATE "2026-11-26"
#define WT_COMPILEHOST "vimtuos"
#define WT_COMPILEUSER "vimtu"
#define WT_COMPILER "clang"
#endif
TESTAMENT
fi

# 依赖库的头（14 个库都已编好；头目录就是上游 include/）
INC_FLAGS="-I$NS -I$NS/include -I$NS/content/handlers -I$GEN"
for p in libwapcaplet libparserutils libhubbub libcss libdom libnsgif libnsbmp \
         libnsutils libnspsl libnslog libsvgtiny librosprite; do
  INC_FLAGS="$INC_FLAGS -I$NS/$p/include"
done
# mbedTLS（user/browser/fetchers/vimtu.c 用它做 TLS）
MBED="${MBEDTLS_DIR:-$ROOT/third_party/mbedtls}"
cp "$ROOT/user/lib/mbedtls_vimtu_user_config.h" "$GEN/mbedtls_vimtu_user_config.h"
INC_FLAGS="$INC_FLAGS -I$ROOT/third_party/expat/expat/lib -I$ROOT/third_party/zlib"
INC_FLAGS="$INC_FLAGS -I$MBED/include -I$GEN"

# 前端标识：核心里有 #ifdef ns<frontend> 的地方（少量），按 framebuffer 的做法给一个
DEFS="-Dsmall -Dnsvimtu -DMBEDTLS_USER_CONFIG_FILE=\"mbedtls_vimtu_user_config.h\" -include $GEN/nscfg.h"

# ---- 组表 ----
# 名字 | 源码根（相对 $NS）| 是否递归(1/0) | 额外源文件（空格分隔，相对 $NS）| 排除（相对 $NS）
core_cfg() {
  RECURSE=1
  EXTRA=""
  EXCLUDE=""
  case "$1" in
    utils)      RECURSE=0; SRC_DIRS="utils" ;;
    nsurl)      RECURSE=0; SRC_DIRS="utils/nsurl" ;;
    content)    RECURSE=0; SRC_DIRS="content"
                # about/ 与 file/ 是"子目录里的 fetcher"，fetch.c 会无条件调它们的 register
                EXCLUDE="" ;;
    fetchers)   RECURSE=1
                # about/ 与 file/ 在子目录里（fetch.c 无条件调它们的 register）
                SRC_DIRS="content/fetchers/about content/fetchers/file"
                # curl.c: WITH_CURL 关，不编（我们的 vimtu.c 顶替它）
                EXTRA="content/fetchers/data.c content/fetchers/resource.c $ROOT/user/browser/fetchers/vimtu.c"
                EXCLUDE="" ;;
    handlers)   RECURSE=1; SRC_DIRS="content/handlers/css content/handlers/html content/handlers/image content/handlers/text"
                # javascript/ 要 duktape，不编（fetch_javascript_register 有 stub）
                # 图像解码器按可用库裁剪（handover §4-④：SVG/BMP/GIF/ICO 要；PNG/JPEG/WEBP/RSVG/Video 不要）
                EXCLUDE="content/handlers/image/jpeg.c content/handlers/image/jpegxl.c content/handlers/image/png.c content/handlers/image/rsvg.c content/handlers/image/rsvg246.c content/handlers/image/video.c content/handlers/image/webp.c" ;;
    desktop)    RECURSE=0; SRC_DIRS="desktop"
                # PDF 输出要 libharu（NETSURF_USE_HARU_PDF=0）；haru 字体同理
                EXCLUDE="desktop/save_pdf.c desktop/font_haru.c" ;;
    *) echo "未知组：$1" >&2; return 1 ;;
  esac
  return 0
}

ORDER="utils nsurl content fetchers handlers desktop"

if [ "${1:-}" = "--list" ]; then
  for g in $ORDER; do echo "$g"; done
  exit 0
fi

WANT="${1:-all}"
if [ "$WANT" = "all" ]; then LIST="$ORDER"; else LIST="$WANT"; fi

rc_all=0
for g in $LIST; do
  core_cfg "$g" || { rc_all=1; continue; }

  FILES=""
  for d in ${SRC_DIRS:-}; do
    [ -d "$NS/$d" ] || continue
    if [ "$RECURSE" = "1" ]; then
      FILES="$FILES $(find "$NS/$d" -name '*.c' -type f | tr '\n' ' ')"
    else
      FILES="$FILES $(ls "$NS/$d"/*.c 2>/dev/null | tr '\n' ' ')"
    fi
  done
  for e in $EXTRA; do
    # ★ 绝对路径原样收（我们的 user/browser/fetchers/vimtu.c），相对路径才加 $NS/
    case "$e" in
      /*) p="$e" ;;
      *)  p="$NS/$e" ;;
    esac
    [ -f "$p" ] && FILES="$FILES $p"
  done
  FILES=$(echo $FILES | tr ' ' '\n' | sort -u | tr '\n' ' ')
  for x in $EXCLUDE; do
    FILES=$(echo $FILES | tr ' ' '\n' | grep -v -x "$NS/$x" | tr '\n' ' ')
  done

  gobj="$OBJ/$g"
  rm -rf "$gobj"; mkdir -p "$gobj"
  ok=0; fail=0
  : > "$gobj/cc.log"
  for f in $FILES; do
    b="$(basename "$f" .c)"
    # 同名不同目录加前缀（css.c 在 handlers/css 和 handlers/html 都有！）
    tag=$(echo "$f" | sed "s|^$NS/||; s|/|_|g; s|\.c$||; s|[^A-Za-z0-9_]|_|g")
    if $CC $BASE_FLAGS $DEFS $INC_FLAGS -c "$f" -o "$gobj/$tag.o" 2>>"$gobj/cc.log"; then
      ok=$((ok+1))
    else
      fail=$((fail+1))
      echo "  FAIL $tag" >>"$gobj/cc.log.x"
    fi
  done

  # 每组合并成一个归档片段，最后再合成 libnetsurfcore.a
  part="$LIBDIR/.part_$g.a"
  rm -f "$part"
  if [ "$fail" -eq 0 ] && ls "$gobj"/*.o >/dev/null 2>&1; then
    ( llvm-ar rcs "$part" "$gobj"/*.o 2>/dev/null || ar rcs "$part" "$gobj"/*.o )
  fi
  size=$(stat -c %s "$part" 2>/dev/null || echo 0)
  syms=0
  if [ "$size" -gt 0 ]; then
    syms=$({ nm -g --defined-only "$part" 2>/dev/null || true; } | grep -cE "^[0-9a-f]+ [TDBR] " || true)
  fi
  echo "[core:$g] cc ok=$ok fail=$fail archive=$size symbols=$syms"
  if [ "$fail" -gt 0 ]; then
    echo "--- [core:$g] 前 20 条编译错误（完整见 $gobj/cc.log）---"
    grep -E "error:" "$gobj/cc.log" | head -20
    rc_all=1
  fi
done

if [ "$rc_all" -eq 0 ]; then
  arfile="$LIBDIR/libnetsurfcore.a"
  rm -f "$arfile"
  ( llvm-ar rcs "$arfile" "$LIBDIR"/.part_*.a 2>/dev/null || ar rcs "$arfile" "$LIBDIR"/.part_*.a )
  # ar 收 .a 当成员不是标准做法：改成把 .o 全打进去（上面用法在 GNU ar 下等价于薄归档，这里显式重来一次）
  rm -f "$arfile"
  ( llvm-ar rcs "$arfile" "$OBJ"/*/*.o 2>/dev/null || ar rcs "$arfile" "$OBJ"/*/*.o )
  size=$(stat -c %s "$arfile" 2>/dev/null || echo 0)
  syms=$({ nm -g --defined-only "$arfile" 2>/dev/null || true; } | grep -cE "^[0-9a-f]+ [TDBR] " || true)
  nobj=$(ls "$OBJ"/*/*.o 2>/dev/null | wc -l)
  echo "[NSCORE] libnetsurfcore.a archive=$size symbols=$syms objects=$nobj"
  echo "[NSCORE] ALL OK"
fi

exit "$rc_all"
