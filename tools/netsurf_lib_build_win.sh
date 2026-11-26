#!/usr/bin/env bash
# tools/netsurf_lib_build_win.sh - ★ v0.12.5：NetSurf 全家桶的**逐个编译**（一套工具链、一张依赖表）
#
# 为什么用一个参数化脚本而不是 13 份几乎相同的脚本：13 个库的编译选项 99% 一样
# （clang + 我们的 musl 头 + -mcmodel=large -fno-pic），区别只有"源码目录 / 头文件目录 / 宏"。
# 写成一张表，加库就是加一行；每个库跑完都打同一口径的判据：
#
#   [<LIB>] cc ok=<n> fail=0 archive=<bytes> symbols=<m>
#
# 用法：
#   bash tools/netsurf_lib_build_win.sh                 # 按依赖顺序编**全部 13 个**
#   bash tools/netsurf_lib_build_win.sh libwapcaplet    # 只编一个（依赖必须已就位）
#   bash tools/netsurf_lib_build_win.sh --list          # 看顺序与产物名
#
# 产物：build64/netsurf/lib/lib<name>.a（与 mbedTLS 的 build64/mbedtls 同一条纪律：只落 build64）
#
# 依赖关系（顺序不能反）：
#   libwapcaplet   （无依赖）
#   libparserutils （无依赖）
#   libhubbub      （libparserutils）
#   libcss         （libwapcaplet, libparserutils）
#   libdom         （libwapcaplet, libparserutils, libhubbub）
#   其余图像/工具库无依赖；expat、zlib 供 NetSurf 的 XML/gzip 用。
set -u
export PATH=/usr/bin:/bin:/mingw64/bin:$PATH
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NS="${NETSURF_DIR:-$ROOT/third_party/netsurf}"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
OUT="${OUT_DIR:-$ROOT/build64/netsurf}"
OBJ="$OUT/obj"
LIBDIR="$OUT/lib"
mkdir -p "$OBJ" "$LIBDIR"
export LC_ALL=C
CC=clang
if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

if [ ! -d "$NS/libwapcaplet" ]; then
  echo "找不到 NetSurf 源码树：$NS（先跑 py -3 tools/fetch_port_sources.py）" >&2
  exit 2
fi
if [ ! -f "$MUSL/include/stdio.h" ] || [ ! -f "$MUSL/lib/libc.a" ]; then
  echo "找不到 musl 头/静态库：$MUSL" >&2
  exit 2
fi
MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
BASE_FLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
 -fno-stack-protector -fno-builtin -ffunction-sections -fdata-sections -O2 -w"

# ---- 依赖表：名字 | 源码目录（相对库根，空格分隔）| 头文件目录（绝对/相对）| 额外宏 ----
lib_cfg() {
  RECURSE=1
  EXTRA_CFLAGS=""
  EXCLUDE=""
  case "$1" in
    libwapcaplet)
      SRC_DIRS="src"
      INCS="$NS/libwapcaplet/include"
      DEFS=""
      ;;
    libparserutils)
      SRC_DIRS="src/charset src/charset/codecs src/charset/encodings src/input src/utils"
      INCS="$NS/libparserutils/include $NS/libparserutils/src"
      DEFS=""
      ;;
    libhubbub)
      SRC_DIRS="src src/tokeniser src/treebuilder src/charset src/utils"
      INCS="$NS/libhubbub/include $NS/libhubbub/src $NS/libparserutils/include $NS/libparserutils/src"
      DEFS=""
      ;;
    libcss)
      # ★ -fcommon：src/stylesheet.h 末尾的 `} _ALIGNED;` 是**变量声明**（上游把它当对齐属性用了），
      #   每个 include 它的 .c 都会定义一个全局 `_ALIGNED`；-fno-common（GCC10+/clang 默认）下
      #   链接期必然 "duplicate symbol: _ALIGNED"（实测 whole-archive 一次炸出 30 行）。
      EXTRA_CFLAGS="-fcommon"
      # ★ 排除生成器程序：src/parse/properties/css_property_parser_gen.c **自带 main()**，
      #   它是"生成属性解析表"的工具，不是库代码。混进 .a 会让链接期 "duplicate symbol: main"。
      EXCLUDE="src/parse/properties/css_property_parser_gen.c"
      SRC_DIRS="src src/parse src/parse/properties src/select src/lex src/bytecode src/charset src/utils"
      INCS="$NS/libcss/include $NS/libcss/src $NS/libwapcaplet/include $NS/libparserutils/include $NS/libparserutils/src"
      DEFS=""
      ;;
    libnsgif)
      SRC_DIRS="src src/lzw"
      INCS="$NS/libnsgif/include $NS/libnsgif/src"
      DEFS=""
      ;;
    libnsbmp)
      SRC_DIRS="src"
      INCS="$NS/libnsbmp/include $NS/libnsbmp/src"
      DEFS=""
      ;;
    libnsutils)
      SRC_DIRS="src"
      INCS="$NS/libnsutils/include $NS/libnsutils/src"
      DEFS=""
      ;;
    libnspsl)
      SRC_DIRS="src"
      INCS="$NS/libnspsl/include $NS/libnspsl/src"
      DEFS=""
      ;;
    libnslog)
      SRC_DIRS="src"
      INCS="$NS/libnslog/include $NS/libnslog/src"
      DEFS=""
      ;;
    libsvgtiny)
      SRC_DIRS="src"
      # ★ 实测：svgtiny_internal.h 里 `#include <dom/dom.h>` —— 它依赖 libdom（以及 dom 的依赖链）
      INCS="$NS/libsvgtiny/include $NS/libsvgtiny/src \
            $NS/libdom/include $NS/libdom/src $NS/libdom \
            $NS/libhubbub/include $NS/libhubbub/src \
            $NS/libparserutils/include $NS/libparserutils/src \
            $NS/libwapcaplet/include"
      DEFS=""
      ;;
    librosprite)
      # ★ 排除工具：src/palette2c.c 自带 main()（把调色板转成 C 表的小工具）
      EXCLUDE="src/palette2c.c"
      SRC_DIRS="src"
      INCS="$NS/librosprite/include $NS/librosprite/src"
      DEFS=""
      ;;
    libdom)
      # ★ 加上 expat 的 XML 绑定：libsvgtiny 需要 `dom_xml_parser_create`（由
      #   bindings/xml/expat_xmlparser.c 提供）。libxml2 那份（libxml_xmlparser.c）要 libxml2，
      #   我们没有 —— 显式排除。
      EXCLUDE="bindings/xml/libxml_xmlparser.c"
      SRC_DIRS="src/core src/events src/html src/namespace src/utils src/abort bindings/hubbub bindings/xml"
      INCS="$NS/libdom/include $NS/libdom/src $NS/libwapcaplet/include \
            $NS/libparserutils/include $NS/libparserutils/src \
            $NS/libhubbub/include $NS/libhubbub/src \
            $ROOT/third_party/expat/expat/lib $OUT/inc"
      DEFS="-DDOM_ENABLE_HTML"
      ;;
    expat)
      # expat 2.6.4：库本体在 third_party/expat/expat/lib
      SRC_DIRS="lib"
      # expat_config.h 由 tools/netsurf_gen_expat_config.py 生成到 build64/netsurf/inc（不进仓）
      INCS="$ROOT/third_party/expat/expat/lib $OUT/inc"
      # XML_STATIC：不生成 __declspec(dllimport) 引用（我们是静态链接）
      DEFS="-DXML_STATIC -DXML_NS -DXML_DTD"
      ;;
    zlib)
      SRC_DIRS="."
      RECURSE=0        # ★ 只编顶层 *.c：contrib/ 里是示例与 Windows 专用（iowin32.c 要 windows.h）
      INCS="$ROOT/third_party/zlib"
      # HAVE_UNISTD_H：zconf.h 靠它决定"POSIX 路径"（否则 gz*.c 里 read/write/lseek/close
      # 全部变成隐式声明 —— iso C99 之后直接编译失败，实测就是这么撞上的）
      DEFS="-DHAVE_UNISTD_H=1"
      ;;
    *)
      echo "未知库：$1" >&2
      return 2
      ;;
  esac
  return 0
}

# ---- 代码生成（perl）：哪些库需要先跑生成步骤 ----
# 本机**有** perl（msys2 /usr/bin/perl 5.42），所以官方 make 里的 .pl 能直接跑；
# 生成的中间文件落在 third_party/ 里（那棵树不进仓，见 .gitignore），不会污染仓库。
#   libparserutils: build/make-aliases.pl -> src/charset/aliases.inc（编码别名表）
lib_gen() {
  case "$1" in
    libparserutils)
      if [ ! -f "$NS/libparserutils/src/charset/aliases.inc" ]; then
        if command -v perl >/dev/null 2>&1; then
          ( cd "$NS/libparserutils" && perl build/make-aliases.pl ) && \
            echo "[libparserutils] gen aliases.inc ok=$(stat -c %s "$NS/libparserutils/src/charset/aliases.inc" 2>/dev/null || echo 0)B"
        else
          echo "[libparserutils] FAIL 缺 aliases.inc 且没有 perl" >&2
          return 1
        fi
      fi
      ;;
    expat)
      # expat_config.h（官方由 cmake 生成；这里由我们的生成器写进 build64/netsurf/inc）
      if [ ! -f "$OUT/inc/expat_config.h" ]; then
        ( cd "$ROOT" && $PY tools/netsurf_gen_expat_config.py ) || return 1
      fi
      ;;
    libcss)
      # ★ 官方 src/parse/properties/Makefile 的流程（必须照做，否则缺 css__parse_<prop> 符号）：
      #   ① 用**宿主**编译器把 css_property_parser_gen.c 编成 gen_parser（它是工具，自带 main，
      #      所以不进 .a —— 见 lib_cfg 的 EXCLUDE）；
      #   ② 对 properties.gen 里每一条 `名字:…` 行，跑
      #        gen_parser -o autogenerated_<名字>.c '<那一整行>'
      #   实测：不做这一步，链接期会报 `undefined symbol: css__parse_outline_style` 等
      #   （whole-archive 门禁抓到的第一批）。
      if [ ! -f "$NS/libcss/src/parse/properties/autogenerated_display.c" ]; then
        mkdir -p "$OUT/gen"
        if ! clang --target=x86_64-w64-windows-gnu -O1 -w \
              -o "$OUT/gen/gen_parser.exe" \
              "$NS/libcss/src/parse/properties/css_property_parser_gen.c" 2>"$OUT/gen/gen.log"; then
          echo "[libcss] FAIL gen_parser 编不过（宿主 mingw）：" >&2
          head -10 "$OUT/gen/gen.log" >&2
          return 1
        fi
        n=0
        ( cd "$NS/libcss/src/parse/properties" || exit 1
          grep -E '^[^#][^:]+:' properties.gen | sed 's/:.*//' | while read -r prop; do
            line=$(grep -m1 "^$prop:" properties.gen)
            "$OUT/gen/gen_parser.exe" -o "autogenerated_$prop.c" "$line" || exit 1
            echo "$prop"
          done > "$OUT/gen/props.txt"
        ) || return 1
        n=$(wc -l < "$OUT/gen/props.txt")
        echo "[libcss] gen autogenerated_*.c ok=$n 个属性解析器"
      fi
      ;;
    libsvgtiny)
      # ① autogenerated_colors.c：官方用 gperf（本机没有）-> 手写等价生成器
      if [ ! -f "$NS/libsvgtiny/src/autogenerated_colors.c" ]; then
        ( cd "$ROOT" && $PY tools/netsurf_gen_svgtiny_colors.py ) || return 1
      fi
      # ② svgtiny 里 include `<dom/bindings/xml/xmlparser.h>` —— 那是 libdom **安装后**的布局
      #    （官方 make install 把 bindings/ 拷进 include/dom/bindings/）。这里照同样布局铺一份，
      #    落在 third_party（不进仓）。
      if [ -d "$NS/libdom/bindings" ] && [ ! -d "$NS/libdom/include/dom/bindings" ]; then
        mkdir -p "$NS/libdom/include/dom/bindings"
        cp -r "$NS/libdom/bindings/." "$NS/libdom/include/dom/bindings/"
        echo "[libsvgtiny] gen dom-include shim ok=$(find "$NS/libdom/include/dom/bindings" -name '*.h' | wc -l) headers"
      fi
      ;;
    libhubbub)
      # ① 实体表（官方就是 perl）：build/make-entities.pl -> src/tokeniser/entities.inc
      if [ ! -f "$NS/libhubbub/src/tokeniser/entities.inc" ]; then
        ( cd "$NS/libhubbub" && perl build/make-entities.pl ) || return 1
        echo "[libhubbub] gen entities.inc ok=$(stat -c %s "$NS/libhubbub/src/tokeniser/entities.inc" 2>/dev/null || echo 0)B"
      fi
      # ② 元素名表：官方用 **gperf**（本机没有）-> 走我们手写的等价生成器
      if [ ! -f "$NS/libhubbub/src/treebuilder/autogenerated-element-type.c" ]; then
        ( cd "$ROOT" && $PY tools/netsurf_gen_hubbub_element_type.py ) || return 1
      fi
      ;;
    libnslog)
      # 官方 src/Makefile：bison 出 filter-parser.{c,h}，flex 出 filter-lexer.{inc,h}，
      # 再拼一个 3 行的 filter-lexer.c 把 .inc 包起来。本机 bison/flex 都在（msys2）。
      # ★ 关键：必须像官方 Makefile 那样传 `--define=api.prefix={filter_}`（bison≥3 的写法）。
      #   不传的话 bison 只生成 `yyparse`，而 filter.c 调的是 `filter_parse` / 词法头也不声明
      #   `filter_lex`，于是出现 "call to undeclared function 'yylex'/'filter_parse'"（实测第一版就踩了）。
      # 无条件重生成（幂等、1 秒级）：第一版用"文件存在就跳过"，结果上一次**没带 api.prefix**
      # 生成的文件让这一步永远被跳过，症状看起来像"补丁没生效"。生成完还要核对头里真的有
      # filter_parse —— 没有就报错，绝不带着半成品往下走。
      if true; then
        ( cd "$NS/libnslog" && \
          rm -f src/filter-parser.c src/filter-parser.h src/filter-lexer.inc src/filter-lexer.h src/filter-lexer.c && \
          bison -d -t --define=api.prefix={filter_} \
                --output=src/filter-parser.c --defines=src/filter-parser.h src/filter-parser.y && \
          flex --outfile=src/filter-lexer.inc --header-file=src/filter-lexer.h src/filter-lexer.l && \
          printf '#ifndef __clang_analyzer__\n#include "filter-lexer.inc"\n#endif\n' > src/filter-lexer.c ) || return 1
        if ! grep -q "filter_parse" "$NS/libnslog/src/filter-parser.h"; then
          echo "[libnslog] FAIL 生成的 filter-parser.h 里没有 filter_parse（api.prefix 没生效？）" >&2
          return 1
        fi
        echo "[libnslog] gen bison+flex(api.prefix=filter_) ok=$(( $(stat -c %s "$NS/libnslog/src/filter-parser.c") + $(stat -c %s "$NS/libnslog/src/filter-lexer.inc") ))B"
      fi
      ;;
    *) : ;;
  esac
  return 0
}

ORDER="libwapcaplet libparserutils libhubbub libcss libnsgif libnsbmp libnsutils libnspsl libnslog libsvgtiny librosprite libdom expat zlib"

if [ "${1:-}" = "--list" ]; then
  for l in $ORDER; do echo "$l"; done
  exit 0
fi

WANT="${1:-all}"
if [ "$WANT" = "all" ]; then
  LIST="$ORDER"
else
  LIST="$WANT"
fi

rc_all=0
for lib in $LIST; do
  lib_cfg "$lib" || { rc_all=1; continue; }
  case "$lib" in
    expat) srcroot="$ROOT/third_party/expat/expat" ;;
    zlib)  srcroot="$ROOT/third_party/zlib" ;;
    *)     srcroot="$NS/$lib" ;;
  esac
  if [ ! -d "$srcroot" ]; then
    echo "[$lib] SKIP 源码树不存在：$srcroot" >&2
    rc_all=1
    continue
  fi
  # 先跑该库的代码生成步骤（缺 .inc 会让整个库 1 个文件都编不过）
  if ! lib_gen "$lib"; then
    rc_all=1
    continue
  fi

  # 收集 .c（跳过 tests/examples/perl 生成的目录）
  # ★ 用 find **递归**收集（libcss 的 src/select 下面还有 4 层子树：只按 `dir/*.c` 收会漏掉
  #   138 个文件里的绝大部分 —— 实测第一版 libcss 只编了 59 个而不是 187 个，"编过了"是假的）。
  #   再 sort -u 去重（SRC_DIRS 里同时列了父目录和子目录时不会重复编译）。
  FILES=""
  for d in $SRC_DIRS; do
    [ -d "$srcroot/$d" ] || continue
    if [ "$RECURSE" = "1" ]; then
      FILES="$FILES $(find "$srcroot/$d" -name '*.c' -type f 2>/dev/null | tr '\n' ' ')"
    else
      FILES="$FILES $(ls "$srcroot/$d"/*.c 2>/dev/null | tr '\n' ' ')"
    fi
  done
  FILES=$(echo $FILES | tr ' ' '\n' | sort -u | tr '\n' ' ')
  # 排除"自带 main 的工具/生成器"（见 lib_cfg 里各库的 EXCLUDE 注释）
  for x in $EXCLUDE; do
    FE=$srcroot/$x
    FILES=$(echo $FILES | tr ' ' '\n' | grep -v -x "$FE" | tr '\n' ' ')
  done
  n_files=$(echo $FILES | wc -w)
  if [ "$n_files" -eq 0 ]; then
    echo "[$lib] FAIL 没找到任何 .c（源码目录 $SRC_DIRS）" >&2
    rc_all=1
    continue
  fi

  INC_FLAGS=""
  for i in $INCS; do INC_FLAGS="$INC_FLAGS -I$i"; done

  libobj="$OBJ/$lib"
  # ★ 每次从干净目录开始：否则"被排除的文件"上一次留下的 .o 还在，照样会被 ar 打进去
  #   （实测：exclude 了 css_property_parser_gen.c，链接期仍报 duplicate symbol: main）。
  rm -rf "$libobj"
  mkdir -p "$libobj"
  ok=0; fail=0
  : > "$libobj/cc.log"
  for f in $FILES; do
    b="$(basename "$f" .c)"
    # 同名不同目录（如 codec_*.c、properties.c）加目录前缀，避免互相覆盖
    # ★ tag 里不许出现路径分隔符、也不许以 "." 开头（实测：zlib 用 `dir/.*.c` 收文件时
    #   文件名变成 "./adler32"，对象就成了隐藏文件 ._adler32.o，`*.o` 匹配不到 -> ar 报
    #   "No such file or directory"，而 cc ok=15 看着像成功）。
    tag=$(echo "$f" | sed "s|^$srcroot/||; s|^\./||; s|/|_|g; s|\.c$||; s|[^A-Za-z0-9_]|_|g")
    if $CC $BASE_FLAGS $EXTRA_CFLAGS $DEFS $INC_FLAGS -c "$f" -o "$libobj/$tag.o" 2>>"$libobj/cc.log"; then
      ok=$((ok+1))
    else
      fail=$((fail+1))
      echo "  FAIL $tag" >>"$libobj/cc.log"
    fi
  done

  arfile="$LIBDIR/lib$lib.a"
  rm -f "$arfile"
  if [ "$fail" -eq 0 ]; then
    ( llvm-ar rcs "$arfile" "$libobj"/*.o 2>/dev/null || ar rcs "$arfile" "$libobj"/*.o )
  fi
  size=$(stat -c %s "$arfile" 2>/dev/null || echo 0)
  syms=0
  if [ "$size" -gt 0 ]; then
    # msys2 里没有 llvm-nm（只有 binutils 的 nm.exe）：两个都试，判据只认 "地址 类型 符号" 这种行
    syms=$({ nm -g --defined-only "$arfile" 2>/dev/null || llvm-nm -g --defined-only "$arfile" 2>/dev/null; } | grep -cE "^[0-9a-f]+ [TDBR] " || true)
  fi
  echo "[$lib] cc ok=$ok fail=$fail archive=$size symbols=$syms src=$n_files"
  if [ "$fail" -gt 0 ]; then
    echo "--- [$lib] 前 15 条编译错误（完整见 $libobj/cc.log）---"
    grep -E "error:" "$libobj/cc.log" | head -15
    rc_all=1
  fi
done

if [ "$rc_all" -eq 0 ]; then
  echo "[NETSURF-LIBS] ALL OK（产物在 $LIBDIR）"
else
  echo "[NETSURF-LIBS] 有失败（见上面的 error 行）" >&2
fi
exit "$rc_all"
