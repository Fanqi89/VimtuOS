#!/bin/bash
# tools/busybox_build_win.sh - ★ 本批：用 **clang + 我们的 musl** 交叉构建**静态** busybox
# （busybox 1.36.1），产出三件交付物（全部只进**系统卷**，内核镜像里一个字节都不加）：
#
#   build64/busybox.bin   busybox 本体（静态 musl ELF64，钉在 4GiB+0x90000）-> 卷里 /lib/busybox.bin
#   build64/busybox       装载驱动（静态 ELF64，4GiB 装载区）             -> 卷里 /bin/busybox
#   build64/bbwrap        applet 包装程序（< 4 KiB，静态 ELF64）          -> 卷里 /bin/<applet>（每个名字一份）
#
# ── 为什么不走上游的 buildroot/configure ────────────────────────────────────────────
# busybox 用的是 Kconfig（不是 autoconf），但上游 `make defconfig` 会**运行宿主测试程序**、
# 而且宿主工具链在本机是 mingw（没有 POSIX regex / mmap / sys/utsname.h）。实测踩到的坑一
# 条条列在本文件下面的 HOST PATCH 段里，做法统一为"只补宿主工具，不动第三方源码树"：
# 构建脚本每次都从 third_party/busybox/busybox-1.36.1 **全新拷一份**到 build64/ 里再打补丁，
# 第三方源码保持原样。
#
# ── 配置是怎么来的（可复现的确切命令行见本文件末尾的 echo）──────────────────────────
#   make allnoconfig      （宿主 conf 工具，已补丁；产出全 'n' 的 .config）
#   py -3 user/busybox/bb_config.py .config      （覆盖成我们要的 applet/开关表）
#   make oldconfig </dev/null                    （Kconfig 归一化，按依赖裁剪）
#   然后**断言** REQUIRED 里每个符号都真的是 y —— 少一个就退出非零。
#
# 用法：bash tools/busybox_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"
OUT="$(cd "$ROOT" && mkdir -p "$OUT" && cd "$OUT" && pwd)"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
export LC_ALL=C      # conf 把 AUTOCONF_TIMESTAMP 写成"本地化时区名"会让 clang 报非法字符编码
CC=clang
LD=ld.lld
OBJCOPY=objcopy

BSRC="$ROOT/third_party/busybox/busybox-1.36.1"
BTAR="$ROOT/third_party/busybox/busybox-1.36.1.tar.bz2"
MUSL="${MUSL_DIR:-$ROOT/third_party/musl}"
APPS="$ROOT/user/busybox"
OBJ="$OUT/busybox_obj"
SRC="$OBJ/src"
HC="$OBJ/hostcompat"
BOBJ="$OBJ/vimtu"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

# ---------------------------------------------------------------- 0) 源码
if [ ! -f "$BSRC/Makefile" ] && [ -f "$BTAR" ]; then
    echo "==> 解压 busybox 源码 tarball（$BTAR）"
    ( cd "$ROOT/third_party/busybox" && tar -xjf "$(basename "$BTAR")" )
fi
if [ ! -f "$BSRC/Makefile" ]; then
    echo "找不到 busybox 源码：$BSRC" >&2
    echo "  来源/许可见 third_party/busybox/README.vimtu64.md（busybox.net 的 busybox-1.36.1.tar.bz2）" >&2
    exit 2
fi
if [ ! -f "$MUSL/include/stdio.h" ] || [ ! -f "$MUSL/lib/libc.a" ]; then
    echo "找不到 musl 头/静态库：$MUSL（先跑 tools/musl_build_win.sh）" >&2
    exit 2
fi

# ---------------------------------------------------------------- 1) 全新副本
echo "==> 1/8 从第三方源码树**全新**拷一份（第三方目录一个字节都不动）"
rm -rf "$OBJ"
mkdir -p "$OBJ" "$HC" "$BOBJ"
cp -a "$BSRC" "$SRC"

# ---------------------------------------------------------------- 2) 宿主工具补丁
# 本机 HOSTCC 只能是 mingw64 的 gcc/clang，它没有 POSIX 的那几样东西。补丁全部只落在
# build64/busybox_obj/src 这份副本上，并且都标了 "VIMTU64 HOST PATCH"。
echo "==> 2/8 宿主工具补丁（fixdep / split-include / kconfig / symbol）"
mkdir -p "$HC/arpa" "$HC/sys"
cat > "$HC/hostcompat.h" <<'EOF'
#ifndef VIMTU64_HOSTCOMPAT_H
#define VIMTU64_HOSTCOMPAT_H
/* hostcompat.h - 强制 include 进每个 busybox 宿主工具（-include）。
 * MSYS2/mingw 缺的 POSIX 位，都在这里补成"能在宿主上跑出正确结果"的等价物。 */
#if defined(_WIN32)
# include <malloc.h>
# include <stdlib.h>
# include <stdint.h>
/* alloca()：busybox 的宿主源码只在"宿主有 <alloca.h>"时才 include 它；缺声明时代码按
 * int 返回值用 -> 指针被截断 -> 宿主工具段错误（实测 fixdep 在真 depfile 上 rc=139）。 */
# ifndef alloca
#  define alloca(x) __builtin_alloca(x)
# endif
/* kconfig 的 conf.c 用 BSD 的 random()/srandom()；mingw 只有 rand()/srand()。 */
# ifndef random
#  define random() ((long)rand())
# endif
# ifndef srandom
#  define srandom(seed) srand((unsigned)(seed))
# endif
#endif
#endif
EOF
cat > "$HC/arpa/inet.h" <<'EOF'
/* hostcompat/arpa/inet.h - mingw 没有 arpa/inet.h；宿主工具只用到字节序宏（fixdep: ntohl）。*/
#ifndef VIMTU64_HOSTCOMPAT_ARPA_INET_H
#define VIMTU64_HOSTCOMPAT_ARPA_INET_H
#include <stdint.h>
static inline uint32_t ntohl(uint32_t x) {
    return ((x & 0x000000ffu) << 24) | ((x & 0x0000ff00u) << 8) |
           ((x & 0x00ff0000u) >> 8)  | ((x & 0xff000000u) >> 24);
}
static inline uint32_t htonl(uint32_t x) { return ntohl(x); }
static inline uint16_t ntohs(uint16_t x) { return (uint16_t)((x >> 8) | (x << 8)); }
static inline uint16_t htons(uint16_t x) { return ntohs(x); }
#endif
EOF
cat > "$HC/sys/utsname.h" <<'EOF'
/* hostcompat/sys/utsname.h - mingw 没有 <sys/utsname.h>；kconfig 的 symbol.c 用它问宿主
 * 机器名（我们钉成 x86_64）。 */
#ifndef VIMTU64_HOSTCOMPAT_SYS_UTSNAME_H
#define VIMTU64_HOSTCOMPAT_SYS_UTSNAME_H
#include <string.h>
struct utsname {
    char sysname[65]; char nodename[65]; char release[65]; char version[65]; char machine[65];
};
static inline int uname(struct utsname *u) {
    memset(u, 0, sizeof(*u));
    strcpy(u->sysname, "Windows"); strcpy(u->nodename, "vimtu64-host");
    strcpy(u->release, "0"); strcpy(u->version, "0"); strcpy(u->machine, "x86_64");
    return 0;
}
#endif
EOF

$PY - "$SRC" <<'PYHOST'
import os, sys
src = sys.argv[1]

def edit(rel, fn):
    p = os.path.join(src, rel)
    d = open(p, "rb").read()
    d2 = fn(d)
    if d2 != d:
        open(p, "wb").write(d2)
    print("    HOST PATCH %s" % rel)

def fixdep(d):
    d = d.replace(b"#include <sys/mman.h>\n",
                  b"/* VIMTU64 HOST PATCH: MSYS2/mingw has no sys/mman.h; use read() instead. */\n")
    old = b"""\tmap = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
\tif ((long) map == -1) {
\t\tperror("fixdep: mmap");
\t\tclose(fd);
\t\treturn;
\t}
"""
    new = b"""\t/* VIMTU64 HOST PATCH: read() the whole file (no mmap on Windows) */
\tmap = malloc(st.st_size + 1);
\t{
\t\tsize_t got = 0;
\t\tif (map != NULL) {
\t\t\twhile (got < (size_t)st.st_size) {
\t\t\t\tssize_t r = read(fd, (char *)map + got, (size_t)st.st_size - got);
\t\t\t\tif (r <= 0) break;
\t\t\t\tgot += (size_t)r;
\t\t\t}
\t\t}
\t\tif (map == NULL || got != (size_t)st.st_size) {
\t\t\tperror("fixdep: read");
\t\t\tclose(fd);
\t\t\treturn;
\t\t}
\t\t((char *)map)[st.st_size] = 0;
\t}
"""
    assert d.count(old) == 2, d.count(old)
    d = d.replace(old, new)
    assert d.count(b"\tmunmap(map, st.st_size);\n") == 2
    d = d.replace(b"\tmunmap(map, st.st_size);\n", b"\tfree(map);\n")
    d = d.replace(b"\tfd = open(filename, O_RDONLY);\n",
                  b"\tfd = open(filename, O_RDONLY | O_BINARY);   /* VIMTU64 HOST PATCH */\n")
    d = d.replace(b"\tfd = open(depfile, O_RDONLY);\n",
                  b"\tfd = open(depfile, O_RDONLY | O_BINARY);    /* VIMTU64 HOST PATCH */\n")
    return d

def splitinc(d):
    assert d.count(b"mkdir(str_dir_config, 0755)") == 1
    d = d.replace(b"mkdir(str_dir_config, 0755)",
                  b"mkdir(str_dir_config) /* VIMTU64 HOST PATCH: mingw mkdir takes 1 arg */")
    assert d.count(b"mkdir(ptarget, 0755)") == 1
    d = d.replace(b"mkdir(ptarget, 0755)",
                  b"mkdir(ptarget)         /* VIMTU64 HOST PATCH: mingw mkdir takes 1 arg */")
    return d

def ksym(d):
    a = b"struct symbol **sym_re_search(const char *pattern)\n{\n\tstruct symbol *sym, **sym_arr = NULL;\n"
    assert d.count(a) == 1
    d = d.replace(a, b"""struct symbol **sym_re_search(const char *pattern)
{
#ifdef _WIN32
\t/* VIMTU64 HOST PATCH: MSYS2/mingw has no POSIX regex (regcomp/regexec/regfree).
\t *   This is the interactive "search symbol" helper; allnoconfig/oldconfig never
\t *   call it, so "no match" is correct for this port. */
\t(void)pattern;
\treturn NULL;
#else
\tstruct symbol *sym, **sym_arr = NULL;
""")
    b = b"\tregfree(&re);\n\n\treturn sym_arr;\n}\n"
    assert d.count(b) == 1
    d = d.replace(b, b"\tregfree(&re);\n\n\treturn sym_arr;\n#endif\n}\n")
    return d

def confdata(d):
    """MSYS2/mingw: rename() does NOT replace an existing destination, so
    conf_write()'s three renames fail as soon as .config/.config.old/autoconf.h
    already exist (measured: `*** Error during writing of the configuration.`
    on the second oldconfig).  Unlink the destination first. """
    helper = b"""/* VIMTU64 HOST PATCH: on Windows rename() fails when the destination exists
 * (MSYS2/mingw maps it to a non-replacing move).  Remove it first. */
static int vimtu_rename_over(const char *from, const char *to)
{
#ifdef _WIN32
\tremove(to);
#endif
\treturn rename(from, to);
}

"""
    a = b"int conf_write(const char *name)\n"
    assert d.count(a) == 1
    d = d.replace(a, helper + a)
    for old, new in ((b'rename(".tmpconfig.h", "include/autoconf.h");',
                      b'vimtu_rename_over(".tmpconfig.h", "include/autoconf.h");'),
                     (b'rename(name, tmpname);', b'vimtu_rename_over(name, tmpname);'),
                     (b'if (rename(newname, tmpname))', b'if (vimtu_rename_over(newname, tmpname))')):
        assert d.count(old) == 1, old
        d = d.replace(old, new)
    return d

def applettables(d):
    """Two Windows facts about this host tool:
    1) Windows cannot rename() a file that still has an open handle.  Upstream does
       `i = open(tmp1); dup2(i, 1);` and later `fclose(stdout); rename(tmp1, ...)` --
       on MSYS2 the fd `i` is still open on tmp1, so the rename fails.
    2) Windows rename() does not replace an existing destination, and make runs
       applet_tables once per target (include/applet_tables.h / include/NUM_APPLETS.h),
       so the second run must overwrite.
    Both made the tool exit 1 with no message at all (measured). """
    a = b"\tdup2(i, 1);\n"
    assert d.count(a) == 1, d.count(a)
    d = d.replace(a, b"\tdup2(i, 1);\n\tclose(i);   /* VIMTU64 HOST PATCH: Windows cannot rename an open file */\n")

    def over(call):
        return (b"#ifdef _WIN32\n"
                b"\tremove(argv[1]);   /* VIMTU64 HOST PATCH: rename() does not replace on Windows */\n"
                b"#endif\n" + call)

    a1 = b"\tif (rename(tmp1, argv[1]))\n"
    a2 = b"\tif (rename(tmp2, argv[2]))\n"
    assert d.count(a1) == 1 and d.count(a2) == 1
    d = d.replace(a1, over(a1))
    d = d.replace(a2, over(a2).replace(b"remove(argv[1]);", b"remove(argv[2]);"))
    return d


edit("scripts/basic/fixdep.c", fixdep)
edit("applets/applet_tables.c", applettables)
edit("scripts/basic/split-include.c", splitinc)
edit("scripts/kconfig/symbol.c", ksym)
edit("scripts/kconfig/confdata.c", confdata)

# scripts/basic/Makefile：不再编 fixdep / docproc（前者跑不了，后者只有 docs 目标用），只留 split-include
p = os.path.join(src, "scripts/basic/Makefile")
open(p, "wb").write(b"""# VIMTU64 HOST PATCH:
#   Upstream builds three native host tools here (fixdep, split-include, docproc).
#   * fixdep  - its dependency-header scan segfaults as a native mingw64 binary
#               (measured rc=139 on a real depfile).  We ship a POSIX-shell replacement
#               (scripts/basic/fixdep) that emits the same makefile fragment -- the scan
#               only affects incremental rebuilds, and this build always starts from a
#               pristine copy of the sources.
#   * docproc - only used by the `docs` target, which this port never builds.
#   split-include IS needed (it slices include/autoconf.h into include/config/*.h).
hostprogs-y\t:= split-include
always\t\t:= $(hostprogs-y)
""")
print("    HOST PATCH scripts/basic/Makefile")

open(os.path.join(src, "scripts/basic/fixdep"), "wb").write(b"""#!/bin/sh
# VIMTU64 HOST PATCH: shell replacement for the native fixdep host tool.
# Usage: fixdep <depfile> <target> <cmdline>
# Emits the makefile fragment that scripts/Kbuild.include's cmd_and_fixdep consumes.
depfile="$1"; target="$2"; cmdline="$3"
printf 'cmd_%s := %s\\n\\n' "$target" "$cmdline"
printf 'deps_%s := \\\\\\n' "$target"
sed -e 's/^[^:]*://' "$depfile" \\
  | tr -s ' \\t\\r\\n' ' ' | tr ' ' '\\n' | grep -v '^$' \\
  | while read -r d; do printf '  %s \\\\\\n' "$d"; done
printf '\\n%s: $(deps_%s)\\n\\n$(deps_%s):\\n' "$target" "$target" "$target"
""")
os.chmod(os.path.join(src, "scripts/basic/fixdep"), 0o755)
print("    HOST PATCH scripts/basic/fixdep (POSIX shell)")

# kconfig/Makefile：只编 conf（mconf 要 sys/ioctl.h，qconf/gconf 要 Qt/GTK）
p = os.path.join(src, "scripts/kconfig/Makefile")
d = open(p, "rb").read()
old = b"hostprogs-y\t:= conf mconf qconf gconf kxgettext\n"
assert d.count(old) == 1
d = d.replace(old, b"# VIMTU64 HOST PATCH: only `conf` (allnoconfig/oldconfig) is needed;\n"
                   b"#   mconf needs sys/ioctl.h (absent on MSYS2/mingw), qconf/gconf need Qt/GTK.\n"
                   b"hostprogs-y\t:= conf\n")
open(p, "wb").write(d)
print("    HOST PATCH scripts/kconfig/Makefile")

# Makefile 的链接步骤：改成我们自己的 ld.lld + 链接脚本（busybox 原链接走 scripts/trylink，
# 目标平台那一套（crt/libc 探测）在交叉环境里必然失败）。core-y/libs-y 是上游算好的目标文件列表。
p = os.path.join(src, "Makefile")
d = open(p, "rb").read()
old = b"""      cmd_busybox__ ?= $(srctree)/scripts/trylink \\
      "$@" \\
      "$(CC)" \\
      "$(CFLAGS) $(CFLAGS_busybox)" \\
      "$(LDFLAGS) $(EXTRA_LDFLAGS)" \\
      "$(core-y)" \\
      "$(libs-y)" \\
      "$(LDLIBS)" \\
      "$(CONFIG_EXTRA_LDLIBS)" \\
      && $(srctree)/scripts/generate_BUFSIZ.sh --post include/common_bufsiz.h
"""
new = b"""      cmd_busybox__ ?= $(VIMTU_LINK) -o "$@" $(VIMTU_PREOBJS) $(core-y) $(libs-y) $(VIMTU_POSTLIBS) \\
      && $(srctree)/scripts/generate_BUFSIZ.sh --post include/common_bufsiz.h
"""
assert d.count(old) == 1, d.count(old)
d = d.replace(old, new)
open(p, "wb").write(d)

# Makefile：把 gen_bbconfigopts 从 include/config/MARKER 的 recipe 里摘掉
p = os.path.join(src, "Makefile")
d = open(p, "rb").read()
old = b"\t$(call cmd,gen_bbconfigopts)\n"
assert d.count(old) == 1, ("gen_bbconfigopts", d.count(old))
d = d.replace(old, b"\t# VIMTU64 HOST PATCH: bbconfigopts is generated by tools/busybox_build_win.sh (no bzip2 on MSYS2)\n")
open(p, "wb").write(d)
print("    HOST PATCH Makefile (drop gen_bbconfigopts)")

print("    HOST PATCH Makefile (link step -> ld.lld + user/busybox/busybox64.ld)")
PYHOST

# ---------------------------------------------------------------- 3) 配置
echo "==> 3/8 配置：make allnoconfig -> bb_config.py 覆盖 -> make oldconfig（全部离线，不跑目标程序）"
HOSTCFLAGS="-I$HC -O2 -w -include $HC/hostcompat.h"
( cd "$SRC" && make allnoconfig HOSTCFLAGS="$HOSTCFLAGS" >>"$OBJ/oldconfig.log" 2>&1 )
# 两遍：Kconfig 只在父项打开后才显示子项，所以第一遍可能有一部分符号还不在 .config 里
# （bb_config.py 第一遍容忍并列出名字），跑一次 oldconfig 让它们出现，第二遍必须全齐。
$PY "$APPS/bb_config.py" "$SRC/.config"
# oldconfig 会为新出现的符号提问（字符串型如 CROSS_COMPILER_PREFIX 缺省空）-> yes "" 一律取缺省
( cd "$SRC" && yes "" | make oldconfig HOSTCFLAGS="$HOSTCFLAGS" >>"$OBJ/oldconfig.log" 2>&1 )
$PY "$APPS/bb_config.py" "$SRC/.config" --strict
# oldconfig 会为新出现的符号提问（字符串型如 CROSS_COMPILER_PREFIX 缺省空）-> yes "" 一律取缺省
( cd "$SRC" && yes "" | make oldconfig HOSTCFLAGS="$HOSTCFLAGS" >>"$OBJ/oldconfig.log" 2>&1 )
$PY - "$SRC/.config" "$APPS/bb_config.py" <<'PYCHK'
import importlib.util, sys
cfg, mod = sys.argv[1], sys.argv[2]
spec = importlib.util.spec_from_file_location("bb_config", mod)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
bad = m.check(cfg, m.REQUIRED)
if bad:
    sys.stderr.write("ERROR: oldconfig 之后这些符号不是 y（依赖被裁掉了？）：\n")
    for b in bad:
        sys.stderr.write("    %s\n" % b)
    raise SystemExit(1)
vals = {}
for ln in open(cfg, "r", errors="replace"):
    s = ln.strip()
    if s.startswith("CONFIG_") and "=" in s:
        k, v = s.split("=", 1)
        vals[k] = v
n_y = sum(1 for v in vals.values() if v == "y")
print("    .config 归一化 OK：%d 个 =y（REQUIRED %d 个全部为 y）" % (n_y, len(m.REQUIRED)))
PYCHK

# ---------------------------------------------------------------- 4) 编译我们的三个目标文件
# bbconfigopts 由我们直接生成（等价于 scripts/mkconfigs 的输出，但不依赖 bzip2/od）
# autoconf.h 的 AUTOCONF_TIMESTAMP 由 conf 用本地化 %c 写出来（clang 会对里面的
# 非 ASCII 报 -Winvalid-source-encoding，而且每次构建都不同）-> 钉成固定 ASCII。
$PY - "$SRC/include/autoconf.h" <<'PYTS'
import io, re, sys
p = sys.argv[1]
d = io.open(p, encoding="utf-8", errors="replace").read()
d2 = re.sub(r'#define AUTOCONF_TIMESTAMP "[^"]*"',
            '#define AUTOCONF_TIMESTAMP "2026-01-01 00:00:00 UTC (pinned by tools/busybox_build_win.sh)"', d)
io.open(p, "w", encoding="utf-8", newline="\n").write(d2)
print("    autoconf.h AUTOCONF_TIMESTAMP -> 固定 ASCII")
PYTS

echo "==> 3b/8 生成 include/bbconfigopts.h / include/bbconfigopts_bz2.h（不依赖 bzip2）"
$PY - "$SRC" <<'PYBBOPTS'
import sys, os
src = sys.argv[1]
lines = []
for ln in open(os.path.join(src, ".config"), "r", errors="replace"):
    s = ln.rstrip("\n")
    if s.startswith("CONFIG_") or s.startswith("# CONFIG_"):
        lines.append(s)
body = "".join('"%s\\n"\n' % s.replace("\\", "\\\\").replace('"', '\\"') for s in lines)
open(os.path.join(src, "include/bbconfigopts.h"), "w", newline="\n").write(
    "#ifndef _BBCONFIGOPTS_H\n#define _BBCONFIGOPTS_H\n"
    "/*\n * busybox configuration settings.\n *\n"
    " * Licensed under GPLv2 or later, see file LICENSE in this source tree.\n *\n"
    " * Generated by tools/busybox_build_win.sh (scripts/mkconfigs needs bzip2).\n */\n"
    "static const char bbconfig_config[] ALIGN1 =\n" + body + ";\n#endif\n")
open(os.path.join(src, "include/bbconfigopts_bz2.h"), "w", newline="\n").write(
    "#ifndef _BBCONFIGOPTS_BZ2_H\n#define _BBCONFIGOPTS_BZ2_H\n"
    "/*\n * busybox configuration settings.\n *\n"
    " * Licensed under GPLv2 or later, see file LICENSE in this source tree.\n *\n"
    " * CONFIG_FEATURE_COMPRESS_BBCONFIG=n -> the compressed blob is never used,\n"
    " * so it is emitted empty rather than requiring bzip2 at build time.\n */\n"
    "static const char bbconfig_config_bz2[] ALIGN1 = {\n};\n#endif\n")
print("    bbconfigopts.h：%d 行配置" % len(lines))
PYBBOPTS

echo "==> 4/8 编译本移植自己的目标文件（bbstart / vimtu_dirent；-Wall -Wextra 零告警）"
MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CROSSCFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \
 -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -ffunction-sections \
 -fdata-sections -std=gnu11 -O2 -Wall -Wextra -D_GNU_SOURCE"
$CC $CROSSCFLAGS -c "$APPS/bbstart.c" -o "$BOBJ/bbstart_c.o"
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$APPS/bbstart.S" -o "$BOBJ/bbstart.o"
$CC $CROSSCFLAGS -c "$APPS/vimtu_dirent.c" -o "$BOBJ/vimtu_dirent.o"
# fcntl 的最小用户态实现（musl stdio 需要 F_GETFD/F_DUPFD_CLOEXEC；内核没有 fcntl(72)）
$CC $CROSSCFLAGS -c "$APPS/vimtu_fcntl.c" -o "$BOBJ/vimtu_fcntl.o"

# ---------------------------------------------------------------- 5) 编 busybox 本体
echo "==> 5/8 编 busybox 本体（-Oz，静态 musl 头；clang $($CC --version | head -1 | sed 's/.*version //;s/ .*//')）"
export CFLAGS="-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie -fno-stack-protector"
# busybox 的 LD = $(CC) -nostdlib：built-in.o 的 `-r` 也要同一个目标三元组，
# 否则 clang 按宿主（Windows / COFF）出 .o，最后 ld.lld 报 "unknown file type"
export LDFLAGS="-target x86_64-linux-gnu -fuse-ld=lld"
VIMTU_LINK="$LD -m elf_x86_64 -static -z noexecstack -T $APPS/busybox64.ld"
VIMTU_PREOBJS="$BOBJ/bbstart.o $BOBJ/bbstart_c.o $BOBJ/vimtu_dirent.o $BOBJ/vimtu_fcntl.o"
VIMTU_POSTLIBS="$MUSL/lib/libc.a"
( cd "$SRC" && make -j4 busybox HOSTCFLAGS="$HOSTCFLAGS" \
      VIMTU_LINK="$VIMTU_LINK" VIMTU_PREOBJS="$VIMTU_PREOBJS" VIMTU_POSTLIBS="$VIMTU_POSTLIBS" \
      CC="$CC" 2>&1 | tail -25 )
if [ ! -f "$SRC/busybox" ]; then
    echo "ERROR: busybox 没编出来（见上面的编译输出）" >&2
    exit 1
fi
cp -f "$SRC/busybox" "$OUT/busybox.bin"
$OBJCOPY --strip-all "$OUT/busybox.bin" 2>/dev/null || true

# ---------------------------------------------------------------- 6) 构建期断言
echo "==> 6/8 busybox.bin 自检（ELF 头/程序头 + 内核装载器的硬约束）"
$PY - "$OUT/busybox.bin" <<'PYELF'
import struct, sys
BASE64 = 0x100000000
MMAP_VA = BASE64 + 0x90000
WINDOW_TOP = BASE64 + 16 * 1024 * 1024
p = sys.argv[1]
d = open(p, "rb").read()
assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
etype, machine = struct.unpack_from("<HH", d, 16)
assert etype == 2 and machine == 0x3E, "必须是 ET_EXEC / x86_64"
entry, phoff = struct.unpack_from("<QQ", d, 24)[0], struct.unpack_from("<Q", d, 32)[0]
phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
assert phes == 56 and 0 < phn <= 16, "e_phentsize/e_phnum 不合法"
first = None
seg = []
for i in range(phn):
    q = phoff + i * phes
    t = struct.unpack_from("<I", d, q)[0]
    off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, q + 8)
    if t == 3:
        raise SystemExit("ERROR: 出现 PT_INTERP（驱动只做静态装载）")
    if t == 2:
        raise SystemExit("ERROR: 出现 PT_DYNAMIC（驱动不做重定位）")
    if t != 1:
        continue
    assert va >= MMAP_VA, "PT_LOAD 低于 USER64_MMAP_VA64：va=%#x" % va
    assert va + msz <= WINDOW_TOP, "PT_LOAD 越出用户窗口：va=%#x+%#x" % (va, msz)
    if first is None:
        first = (off, va, fsz)
    seg.append((va, msz))
assert first is not None and first[0] == 0 and first[1] == MMAP_VA, \
    "第一个 PT_LOAD 必须 off=0 / va=USER64_MMAP_VA64（驱动用它算 auxv 的 AT_PHDR）"
assert phoff < first[2], "程序头表不在第一个 PT_LOAD 的文件范围内"
assert any(va <= entry < va + msz for va, msz in seg), "入口不在任何 PT_LOAD 内"
print("    断言 OK：busybox.bin = %d B（%.1f KiB）entry=%#x phnum=%d PT_LOAD=%d span=%#x..%#x"
      % (len(d), len(d) / 1024.0, entry, phn, len(seg), seg[0][0], seg[-1][0] + seg[-1][1]))
PYELF

# ---------------------------------------------------------------- 7) 驱动 + 包装程序
echo "==> 7/8 编 /bin/busybox（装载驱动）与 /bin/<applet>（包装程序）"
NONE_FLAGS="-target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
 -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone -mno-sse -mno-sse2 -mno-mmx \
 -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables -std=c11 -O2 -Wall -Wextra"
$CC $NONE_FLAGS -c "$APPS/bbdrv.c" -o "$BOBJ/bbdrv.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie -w \
    -c "$APPS/bbdrv_start.S" -o "$BOBJ/bbdrv_start.o"
$LD -m elf_x86_64 -static -z noexecstack -T "$APPS/bbdrv64.ld" \
    -o "$OUT/busybox" "$BOBJ/bbdrv_start.o" "$BOBJ/bbdrv.o"

$CC $NONE_FLAGS -c "$APPS/bbwrap.c" -o "$BOBJ/bbwrap.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie -w \
    -c "$APPS/bbwrap_start.S" -o "$BOBJ/bbwrap_start.o"
$LD -m elf_x86_64 -static -z noexecstack -T "$APPS/bbwrap64.ld" \
    -o "$OUT/bbwrap" "$BOBJ/bbwrap_start.o" "$BOBJ/bbwrap.o"

echo "==> 8/8 驱动/包装程序自检（必须落在用户窗口低 64 KiB、无 INTERP/DYNAMIC、< 96 KiB）"
$PY - "$OUT/busybox" "$OUT/bbwrap" <<'PYSMALL'
import os, struct, sys
STACK64 = 0x100000000 + 0x10000

def check(p, maxb):
    d = open(p, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "%s 不是 ET_EXEC/x86_64" % p
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    assert 0 < phn <= 16
    first = None
    for i in range(phn):
        q = phoff + i * phes
        t = struct.unpack_from("<I", d, q)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, q + 8)
        if t == 3:
            raise SystemExit("ERROR: %s 出现 PT_INTERP" % p)
        if t == 2:
            raise SystemExit("ERROR: %s 出现 PT_DYNAMIC" % p)
        if t != 1:
            continue
        if first is None:
            first = (off, fsz)
        assert va >= 0x100000000 and va + msz <= STACK64, \
            "%s 的 PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x memsz=%#x）" % (p, va, msz)
    assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
        "%s 的程序头表不在第一个 PT_LOAD 里（auxv 的 AT_PHDR 会是 0）" % p
    assert len(d) <= maxb, "%s 太大：%d B > %d B" % (p, len(d), maxb)
    print("    断言 OK：%s = %d B" % (os.path.basename(p), len(d)))

check(sys.argv[1], 96 * 1024)
check(sys.argv[2], 8 * 1024)
PYSMALL

echo
echo "    --- busybox 体积账（全部进系统卷，不进内核）---"
echo "    build64/busybox.bin = $(stat -c%s "$OUT/busybox.bin") B   -> 卷里 /lib/busybox.bin"
echo "    build64/busybox     = $(stat -c%s "$OUT/busybox") B   -> 卷里 /bin/busybox（装载驱动）"
echo "    build64/bbwrap      = $(stat -c%s "$OUT/bbwrap") B   -> 卷里 /bin/<applet>（每个名字一份）"
echo
echo "    复现用的确切命令行："
echo "      cd $SRC"
echo "      make allnoconfig HOSTCFLAGS=\"-I<hostcompat> -O2 -w -include <hostcompat>/hostcompat.h\""
echo "      py -3 user/busybox/bb_config.py .config"
echo "      make oldconfig HOSTCFLAGS=... </dev/null"
echo "      clang -target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie \\"
echo "            -fno-stack-protector -O2 -c user/busybox/bbstart.c user/busybox/vimtu_dirent.c"
echo "      make -j4 busybox CFLAGS=\"-target x86_64-linux-gnu -nostdinc $MUSLINC -mcmodel=large -fno-pic -fno-pie -fno-stack-protector\" \\"
echo "            CC=clang VIMTU_LINK=\"$LD -m elf_x86_64 -static -z noexecstack -T user/busybox/busybox64.ld\" \\"
echo "            VIMTU_PREOBJS=\"<bbstart.o> <bbstart_c.o> <vimtu_dirent.o>\" VIMTU_POSTLIBS=third_party/musl/lib/libc.a"
