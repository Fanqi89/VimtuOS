#!/bin/bash
# tools/make_build_win.sh - ★ B5：在 MSYS2 上**手写构建** VimtuOS 版的 GNU make 4.4.1
#
# 为什么手写（不走上游 configure/Makefile —— 与 tools/tcc_build_win.sh、tools/lua_build_win.sh 同一条路子）：
#   1) 上游 configure 会**运行**一堆测试程序，而我们的目标（x86_64 ELF64 + musl + VimtuOS ring3）
#      在宿主 Windows 上跑不了；实测 MSYS2 上 configure 的结论还是**错**的（它把 fork/waitpid/
#      sys/wait.h 全判成 no）。所以 **config.h 手写**：由 src/config.h.in 的 207 个 #undef 槽位 +
#      一张"musl 有 + 本内核有系统调用"的取值表生成（表在下面 TABLE 里，一条条对应
#      kernel/syscall64.cpp 的号段）。config.h.in 里 gnulib 的无条件定义（_GL_ATTRIBUTE_* 等）
#      原样保留 —— 这是"不跑 configure 也能得到完整 config.h"的关键。
#   2) gnulib 的生成头 glob.h/fnmatch.h 就是 glob.in.h/fnmatch.in.h 改名（无占位符）；
#      取到的是**上游 GNU glob 版 API**（GLOB_ALTDIRFUNC/gl_flags…），make 的 dir.c/read.c 要用。
#   3) **目标平台补丁（1 处，只加不改语义）**：ring3 没有 getdents(217)（内核只在 shell 的
#      邮箱协议里代列目录，见 kernel/terminal64.cpp），musl 的 opendir 能开目录句柄但 readdir
#      一律 -ENOSYS。make 的 dir.c 因此把 file_exists_p 退化为 **stat() 判存在**：
#        * 隐式规则（%.o: %.c）、VPATH 的存在性判定照常工作；
#        * $(wildcard)/glob 不可用（它走 lib/glob.c 的 readdir 接口）—— 报告里如实标注。
#      补丁只打在 **$OUT/make_obj/makesrc 的副本**上，third_party/make/ 里的源码保持原样。
#   4) 需要的东西一个不缺：fork(57)/execve(59)/wait4(61)/pipe(22)/dup2(33)/stat/chdir(80)/
#      getcwd(79)/rename(82)/unlink(87)/mkdir(83)/clock_gettime(228)/gettimeofday(96)。
#      缺的（mkfifo(133)/pselect(270)/select(23)）不定义，make 走自己的回退路径（见报告）。
#
# 产物（全部落在 build64/，**一个字节都不进内核镜像**）：
#   build64/make.bin   真 make（静态 ELF64，钉在 4GiB+0x90000）-> 卷里 /lib/make.bin
#   build64/make       装载驱动（静态 ELF64，4GiB 装载区）    -> 卷里 /bin/make
#
# 用法：bash tools/make_build_win.sh [outdir]        # 缺省 outdir = build64
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
OUT="${1:-build64}"

MKSRC="third_party/make/make-4.4.1"
MUSL="third_party/musl"
APPS="user/make"
OBJ="$OUT/make_obj"
SRCCOPY="$OBJ/makesrc"

export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"
CC="clang"
LD="ld.lld"

if command -v py >/dev/null 2>&1; then PY="py -3"; else PY="${PYTHON:-python}"; fi

if [ ! -f "$MKSRC/src/main.c" ]; then
    echo "找不到 GNU make 源码：$MKSRC（third_party/make/README.vimtu64-b5.md 有来源与许可）" >&2
    exit 2
fi
if [ ! -f "$MUSL/lib/libc.a" ]; then
    echo "找不到 musl 静态库：$MUSL/lib/libc.a（先跑 tools/musl_build_win.sh）" >&2
    exit 2
fi

mkdir -p "$OUT"
rm -rf "$OBJ" "$SRCCOPY"
mkdir -p "$OBJ" "$SRCCOPY/src" "$SRCCOPY/lib"
cp -f "$MKSRC"/src/*.c "$MKSRC"/src/*.h "$SRCCOPY/src/"
cp -f "$MKSRC"/lib/*.c "$MKSRC"/lib/*.h "$SRCCOPY/lib/"
# gnulib 的生成头：glob/fnmatch 的 .in.h 没有占位符，直接改名 = 上游 GNU 版 API
cp -f "$SRCCOPY/lib/glob.in.h" "$SRCCOPY/lib/glob.h"
cp -f "$SRCCOPY/lib/fnmatch.in.h" "$SRCCOPY/lib/fnmatch.h"

echo "==> ★ B5 1/7：生成 config.h（config.h.in 的 207 个槽位 + 目标平台取值表；字节级替换）"
$PY - "$MKSRC/src/config.h.in" "$SRCCOPY/src/config.h" <<'PYCFG'
import re, sys

# name -> 值（不在表里 = 保持未定义）。逐条对应"musl 有 + 本内核有系统调用"。
TABLE = {
    "__x86_64__": "1",
    "PACKAGE": '"make"', "PACKAGE_NAME": '"GNU Make"',
    "PACKAGE_STRING": '"GNU Make 4.4.1"', "PACKAGE_TARNAME": '"make"',
    "PACKAGE_URL": '"https://www.gnu.org/software/make/"',
    "PACKAGE_VERSION": '"4.4.1"', "VERSION": '"4.4.1"',
    "PACKAGE_BUGREPORT": '"bug-make@gnu.org"',
    "MAKE_HOST": '"x86_64-vimtuos"', "MAKE_CXX": '"g++"', "MK_CONFIGURE": "1",
    "PATH_SEPARATOR_CHAR": "':'", "SCCS_GET": '"get"', "STDC_HEADERS": "1",
    "_FILE_OFFSET_BITS": "64", "eaccess": "access",
    # 头文件（musl 全有；sys/timeb.h 没有 -> 不定义）
    "HAVE_DIRENT_H": "1", "HAVE_FCNTL_H": "1", "HAVE_INTTYPES_H": "1",
    "HAVE_LIMITS_H": "1", "HAVE_LOCALE_H": "1", "HAVE_MEMORY_H": "1",
    "HAVE_STDBOOL_H": "1", "HAVE_STDINT_H": "1", "HAVE_STDIO_H": "1",
    "HAVE_STDLIB_H": "1", "HAVE_STRING_H": "1", "HAVE_STRINGS_H": "1",
    "HAVE_SYS_FILE_H": "1", "HAVE_SYS_PARAM_H": "1", "HAVE_SYS_RESOURCE_H": "1",
    "HAVE_SYS_STAT_H": "1", "HAVE_SYS_TIME_H": "1", "HAVE_SYS_TYPES_H": "1",
    "HAVE_SYS_WAIT_H": "1", "HAVE_UNISTD_H": "1", "HAVE_WCHAR_H": "1",
    # 类型
    "HAVE_INTMAX_T": "1", "HAVE_LONG_LONG_INT": "1", "HAVE_SIG_ATOMIC_T": "1",
    "HAVE_UINTMAX_T": "1", "HAVE_UNSIGNED_LONG_LONG_INT": "1", "HAVE_C_BOOL": "1",
    "HAVE_STRUCT_DIRENT_D_TYPE": "1",
    # 函数（内核号段见注释）
    "HAVE_ALLOCA": "1", "HAVE_ALLOCA_H": "1", "HAVE_ATEXIT": "1",
    "HAVE_CLOCK_GETTIME": "1",          # 228
    "HAVE_DUP": "1", "HAVE_DUP2": "1",  # 33
    "HAVE_FDOPEN": "1", "HAVE_FORK": "1",   # 57
    "HAVE_GETCWD": "1",                 # 79
    "HAVE_GETTIMEOFDAY": "1",           # 96
    "HAVE_ISATTY": "1", "HAVE_LSTAT": "1", "HAVE_MEMPCPY": "1", "HAVE_MEMRCHR": "1",
    "HAVE_MKSTEMP": "1", "HAVE_MKTEMP": "1",
    "HAVE_PIPE": "1",                   # 22（真管道）
    "HAVE_READLINK": "1",               # 89（只对 /proc/self/exe 有值）
    "HAVE_REALPATH": "1", "HAVE_SA_RESTART": "1", "HAVE_SETVBUF": "1",
    "HAVE_SIGACTION": "1",              # 13（A4-5 真投递）
    "HAVE_STPCPY": "1", "HAVE_STRCASECMP": "1", "HAVE_STRCOLL": "1",
    "HAVE_STRDUP": "1", "HAVE_STRERROR": "1", "HAVE_STRNCASECMP": "1",
    "HAVE_STRNDUP": "1", "HAVE_STRSIGNAL": "1", "HAVE_STRTOLL": "1",
    "HAVE_TTYNAME": "1", "HAVE_UMASK": "1", "HAVE_WAITPID": "1",   # 61
    "HAVE_WORKING_FORK": "1",
    # decl（musl 里没有这些符号；make 用自己的表）
    "HAVE_DECL_BSD_SIGNAL": "1", "HAVE_DECL_DLERROR": "0", "HAVE_DECL_DLOPEN": "0",
    "HAVE_DECL_DLSYM": "0", "HAVE_DECL_GETLOADAVG": "0",
    "HAVE_DECL_SYS_SIGLIST": "0", "HAVE_DECL__SYS_SIGLIST": "0",
    "HAVE_DECL___SYS_SIGLIST": "0",
    # 本内核的 stat 不返回 mtime -> 高精度只会放大差异
    "FILE_TIMESTAMP_HI_RES": "0",
    # 不定义（内核没有对应系统调用 / 会引到不该引的路径）：
    #   GNULIB_TEST_GETLOADAVG HAVE_GETLOADAVG HAVE_MKFIFO HAVE_PSELECT HAVE_POSIX_SPAWN
    #   HAVE_DOS_PATHS MAKE_JOBSERVER MAKE_LOAD MAKE_SYMLINKS HAVE_GETRLIMIT HAVE_SETRLIMIT
    #   HAVE_GETGROUPS HAVE_GETTEXT HAVE_ICONV HAVE_GUILE HAVE_SYS_TIMEB_H ENABLE_NLS
}

src = open(sys.argv[1], "rb").read().split(b"\n")
out = []
seen = set()
pat = re.compile(rb"^#undef\s+([A-Za-z_][A-Za-z0-9_]*)\s*$")
for line in src:
    m = pat.match(line)
    if m:
        name = m.group(1).decode("ascii")
        seen.add(name)
        if name in TABLE:
            out.append(("#define %s %s" % (name, TABLE[name])).encode("ascii"))
        else:
            out.append(("/* #undef %s */" % name).encode("ascii"))
        continue
    out.append(line)
missing = [k for k in TABLE if k not in seen]
if missing:
    sys.stderr.write("config.h.in 里没有这些名字（表写错了）：%s\n" % " ".join(missing))
    raise SystemExit(1)
open(sys.argv[2], "wb").write(b"\n".join(out))
print("    config.h：%d 个 #undef 槽位，其中 %d 个按目标平台定义为 1/具体值" % (len(seen), len(TABLE)))
PYCFG

echo "==> ★ B5 2a/7：dir.c 目标平台补丁（readdir 不可用 -> stat() 判存在；只改构建副本）"
$PY - "$SRCCOPY/src/dir.c" <<'PYDIR'
import sys

p = sys.argv[1]
d = open(p, "rb").read()

helper = '''#ifdef VIMTU64_NO_READDIR
/* VimtuOS target patch ------------------------------------------------------------
 *   ring3 has no getdents(217): musl's opendir can open a directory handle (the kernel
 *   allows O_DIRECTORY), but the first readdir always returns -ENOSYS.  make's
 *   directory-content cache therefore stays empty forever, so file_exists_p is
 *   degraded to a plain stat():
 *       file_exists_p(name)  ->  stat(dirname/name) succeeded = exists
 *   Implicit rules (%.o: %.c) and VPATH existence tests keep working; $(wildcard)
 *   /glob do NOT (they go through lib/glob.c -> readdir) -- documented in the report.
 *   The patch is applied to a build copy only; third_party/make/ stays untouched. */
static int
vimtu64_dir_stat_exists (const char *dirname, const char *filename)
{
  struct stat st;
  char buf[4096];
  size_t dl, fl;
  if (!filename || !*filename)
    return 0;                           /* "read the whole directory" = unsupported */
  if (!dirname || !*dirname || strcmp (dirname, ".") == 0)
    return stat (filename, &st) == 0;
  dl = strlen (dirname);
  fl = strlen (filename);
  if (dl + fl + 2 > sizeof (buf))
    return 0;
  memcpy (buf, dirname, dl);
  buf[dl] = '/';
  memcpy (buf + dl + 1, filename, fl + 1);
  return stat (buf, &st) == 0;
}
#endif /* VIMTU64_NO_READDIR */

'''.encode("ascii")

anchor = b"/* Find the directory named NAME and return its 'struct directory'.  */"
assert d.count(anchor) == 1, "helper anchor not unique"
d = d.replace(anchor, helper + anchor)

old1 = b"""      dc->counter = command_count;

      ENULLLOOP (dc->dirstream, opendir (name));"""
new1 = b"""      dc->counter = command_count;

#ifdef VIMTU64_NO_READDIR
      /* VimtuOS: no readdir -> never open the directory (see patch note above).  */
      dc->dirfiles.ht_vec = NULL;
#else
      ENULLLOOP (dc->dirstream, opendir (name));"""
assert d.count(old1) == 1, "patch anchor 1 not unique"
d = d.replace(old1, new1)

old2 = b"""  if (dc == NULL || dc->dirfiles.ht_vec == NULL)
    /* The directory could not be stat'd or opened.  */
    return 0;"""
new2 = b"""  if (dc == NULL)
    /* The directory could not be stat'd.  */
    return 0;
#ifdef VIMTU64_NO_READDIR
  if (dc->dirfiles.ht_vec == NULL)
    /* VimtuOS: no readdir -> stat() existence test (see patch note above).  */
    return vimtu64_dir_stat_exists (dir->name, filename);
#else
  if (dc->dirfiles.ht_vec == NULL)
    /* The directory could not be stat'd or opened.  */
    return 0;
#endif"""
assert d.count(old2) == 1, "patch anchor 2 not unique"
d = d.replace(old2, new2)
# ★ 补丁 1b（实测修的一处真错）：**让 stat() 判存在真的生效**。
#   只有上面那处替换不够：make 4.4.1 的 dir_contents_file_exists_p 只在
#   "目录内容已过期重读"（dc->counter != command_count）那一段才被改成不读盘，
#   而第一次调用（command_count 相同、counter 已置位）会**跳过整段**、直接用那份
#   **永远为空的哈希表**判存在 —— 症状就是 `make: *** No rule to make target 'main.o'`，
#   而磁盘上 main.c 明明在（隐式规则 %.o: %.c 因此永远推不出来）。
#   修法：把*入口* dir_file_exists_p 也改成 stat()（helper 就在这个文件的前面，
#   不依赖目录哈希的任何状态）。语义 = "stat 成功即存在"，与上面 helper 的注释一致。
old2b = b"""  return dir_contents_file_exists_p (find_directory (dirname),
                                     filename);
}"""
new2b = b"""#ifdef VIMTU64_NO_READDIR
  /* VimtuOS: no readdir -> existence = stat() (see patch note above).  Doing it here
     (and not only inside dir_contents_file_exists_p) is what makes it work: that
     function only re-reads the directory when its contents look stale, so the very
     first query would otherwise go through the permanently-empty hash table and
     report "not found" for files that do exist.  */
  return vimtu64_dir_stat_exists (dirname, filename);
#else
  return dir_contents_file_exists_p (find_directory (dirname),
                                     filename);
#endif
}"""
assert d.count(old2b) == 1, "patch anchor 2b not unique"
d = d.replace(old2b, new2b)



old3 = b"""          if (open_directories == MAX_OPEN_DIRECTORIES)
            /* We have too many directories open already.
               Read the entire directory and then close it.  */
            dir_contents_file_exists_p (dir, NULL);
        }
    }"""
new3 = b"""          if (open_directories == MAX_OPEN_DIRECTORIES)
            /* We have too many directories open already.
               Read the entire directory and then close it.  */
            dir_contents_file_exists_p (dir, NULL);
        }
#endif /* VIMTU64_NO_READDIR */
    }"""
assert d.count(old3) == 1, "patch anchor 3 not unique"
d = d.replace(old3, new3)

open(p, "wb").write(d)
print("    dir.c 补丁：2 处替换 + 1 个 stat() 兜底辅助函数（%d B）" % len(d))
PYDIR
echo "==> ★ B5 2c/7：job.c 目标平台补丁（wait4 的 EAGAIN 容忍 + ring3 shell 内建命令走 shell）"
$PY - "$SRCCOPY/src/job.c" <<'PYJOB'
import sys

p = sys.argv[1]
d = open(p, "rb").read()

# --- 补丁 1：内核的 wait4(61) 对**还在跑**的子进程 5 秒后返回 -EAGAIN
#     （kernel/proc64.cpp 的 PROC64_WAIT_TIMEOUT_SEC）。make 原来把 wait() 的负返回一律当
#     致命错：`make: *** wait: Resource temporarily unavailable.  Stop.` —— 于是"tcc 链接
#     稍微慢一点"这种正常情况会把整个构建打断（实测原文见报告）。EAGAIN 不是错误，
#     回到循环里继续等这个子进程。
old1 = b"""          if (pid < 0)
            {
              /* The wait*() failed miserably.  Punt.  */
              pfatal_with_name ("wait");
            }"""
new1 = b"""          if (pid < 0)
            {
              /* VimtuOS target patch: the kernel's wait4(61) returns -EAGAIN for a child
                 that is still running (bounded 5 s poll -- kernel/proc64.cpp
                 PROC64_WAIT_TIMEOUT_SEC).  Treating that as fatal broke slow but healthy
                 recipes (observed: `make: *** wait: Resource temporarily unavailable.
                 Stop.` while tcc was linking).  Not an error: wait again. */
              if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
              /* The wait*() failed miserably.  Punt.  */
              pfatal_with_name ("wait");
            }"""
assert d.count(old1) == 1, "job.c wait anchor not unique"
d = d.replace(old1, new1)

# --- 补丁 2：recipe 里 `rm x` / `mkdir y` / `ls` / `cat` / `stat` / `run` 这些**是 ring3 shell
#     的内建命令**（系统卷里根本没有 /bin/rm 这种文件）。GNU make 只有看到 sh_cmds 表里的名字
#     才会走 `/bin/sh -c`；不在表里就走 execvp 直挂 -> 找不到文件 -> `Error 127`（实测：
#     `make: rm: No such file or directory`）。把它们加进 POSIX 那张表即可（语义 = 这些命令
#     必须由 shell 解释）。
old2 = b"""  static const char *sh_cmds[] =
    { ".", ":", "alias", "bg", "break", "case", "cd", "command", "continue",
      "eval", "exec", "exit", "export", "fc", "fg", "for", "getopts", "hash",
      "if", "jobs", "login", "logout", "read", "readonly", "return", "set",
      "shift", "test", "times", "trap", "type", "ulimit", "umask", "unalias",
      "unset", "wait", "while", 0 };"""
new2 = b"""  static const char *sh_cmds[] =
    { ".", ":", "alias", "bg", "break", "case", "cd", "command", "continue",
      "eval", "exec", "exit", "export", "fc", "fg", "for", "getopts", "hash",
      "if", "jobs", "login", "logout", "read", "readonly", "return", "set",
      "shift", "test", "times", "trap", "type", "ulimit", "umask", "unalias",
      "unset", "wait", "while",
      /* VimtuOS: built-ins of the ring3 shell -- there is no /bin/rm, /bin/ls ...
         in the volume, so such recipes must be handed to /bin/sh -c to resolve at
         all.  (Unix make consults THIS table, not sh_cmds_sh.)  */
      "rm", "rmdir", "mkdir", "ls", "cat", "stat", "pwd", "run", 0 };"""
assert d.count(old2) == 1, "job.c sh_cmds anchor not unique"
d = d.replace(old2, new2)

open(p, "wb").write(d)
print("    job.c 补丁：wait 的 EAGAIN 容忍（1 处）+ sh_cmds 加 8 个 ring3 内建（1 处）")
PYJOB


echo "==> ★ B5 2b/6：main.c 目标平台补丁（空的虚拟标准流 close -> EBADF 不算写失败）"
$PY - "$SRCCOPY/src/main.c" <<'PYMAIN'
import sys

p = sys.argv[1]
d = open(p, "rb").read()
old = b'''  int prev_fail = ferror (stdout);
  int fclose_fail = fclose (stdout);

  if (prev_fail || fclose_fail)'''
new = b'''  int prev_fail = ferror (stdout);
  int fclose_fail = fclose (stdout);

#ifdef VIMTU64_VIRTUAL_STDIO
  /* VimtuOS target patch: fd 0/1/2 that are *empty slots* are "virtual console streams"
     with no kernel object behind them, so close(1) returns -EBADF
     (kernel/syscall64.cpp lx64_close64) and musl's fclose reports failure.  That is not a
     write failure -- only ferror() is.  Measured symptom without this: `make -v` printed the
     banner and then `make: write error: stdout: Bad file descriptor`, exit code 1. */
  if (fclose_fail && (errno == EBADF || errno == ENOTTY))
    fclose_fail = 0;
#endif

  if (prev_fail || fclose_fail)'''
assert d.count(old) == 1, "main.c close_stdout anchor not unique"
d = d.replace(old, new)
open(p, "wb").write(d)
print("    main.c 补丁：close_stdout 容忍空的虚拟标准流（EBADF/ENOTTY）")
PYMAIN

echo "==> ★ B5 3/7：编译 gnulib 的 5 个模块 + make 的 28 个翻译单元（-Oz，静态 musl 头）"
MUSLINC="-isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
# 选项理由（与 tools/lua_build_win.sh 同口径）：
#   -target x86_64-linux-gnu 目标 = x86_64 ELF64（VimtuOS 的 ring3 ABI 与 Linux x86_64 一致）
#   -mcmodel=large           映像在 4GiB：small/medium 会产生 R_X86_64_32/32S 越界重定位
#   -fno-pic -fno-pie        非 PIC 定址（装载驱动不做重定位）
#   -fno-stack-protector     不引 __stack_chk_fail
#   -Oz + gc-sections        体积（系统卷也是有限的）
#   -w                       上游代码的告警不当作本项目的交付标准（驱动/装载器才是 -Wextra 零告警）
#   -D_GNU_SOURCE            上游要 GLOB_ALTDIRFUNC / gl_flags 这些 GNU 扩展声明
CFLAGS_COMMON="-target x86_64-linux-gnu -nostdinc $MUSLINC \
 -DHAVE_CONFIG_H -I$SRCCOPY/src -I$SRCCOPY/lib \
 -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
 -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 -D_GNU_SOURCE \
 -std=gnu99 -w -Oz -ffunction-sections -fdata-sections"
CFLAGS_SRC="$CFLAGS_COMMON -DLIBDIR=\"/lib\" -DINCLUDEDIR=\"/include\" -DLOCALEDIR=\"/share/locale\""
# gnulib 的 findprog-in.c / findprog.h 用了 bool 但没 include <stdbool.h>（上游靠 gnulib 生成
# lib/stdbool.h；我们用系统 musl 的那份 -> 统一 -include 一下，1 行等价物）
CFLAGS_LIB="$CFLAGS_COMMON -include stdbool.h"

FAIL=0
for f in concat-filename findprog-in fnmatch getloadavg glob; do
    $CC $CFLAGS_LIB -c "$SRCCOPY/lib/$f.c" -o "$OBJ/lib_$f.o" || { echo "FAILED lib/$f" >&2; FAIL=1; }
done
for f in ar arscan commands default dir expand file function getopt getopt1 guile hash implicit \
         job load loadapi main misc output read remake rule shuffle signame strcache variable \
         version vpath posixos remote-stub; do
    EXTRA=""
    [ "$f" = "dir" ] && EXTRA="-DVIMTU64_NO_READDIR"
    [ "$f" = "main" ] && EXTRA="-DVIMTU64_VIRTUAL_STDIO"
    $CC $CFLAGS_SRC $EXTRA -c "$SRCCOPY/src/$f.c" -o "$OBJ/$f.o" || { echo "FAILED src/$f" >&2; FAIL=1; }
done
[ "$FAIL" = "0" ] || { echo "ERROR: 有翻译单元编不过（见上）" >&2; exit 1; }

echo "==> ★ B5 4/7：链接 make.bin（静态 musl libc.a，钉在 4GiB+0x90000）"
mkdir -p "$OBJ/drv"
$CC -target x86_64-linux-gnu -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -c "$APPS/make_start.S" -o "$OBJ/drv/make_start.o"
$CC -target x86_64-linux-gnu -nostdinc $MUSLINC \
    -mcmodel=large -fno-pic -fno-pie -fno-stack-protector \
    -fno-asynchronous-unwind-tables -std=gnu99 -O2 -Wall -Wextra \
    -c "$APPS/make_start.c" -o "$OBJ/drv/make_start_c.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/make64.ld" \
    -o "$OUT/make.bin" "$OBJ/drv/make_start.o" "$OBJ/drv/make_start_c.o" "$OBJ"/*.o "$MUSL/lib/libc.a"
echo "==> ★ B5 5/7：装载驱动 /bin/make（< 64 KiB，走内核主程序装载器）"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -nostdlib -fno-builtin \
    -fno-stack-protector -fno-pic -fno-pie -mcmodel=large -mno-red-zone \
    -mno-sse -mno-sse2 -mno-mmx -mno-avx -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -ffunction-sections -fdata-sections -std=c11 -O2 -Wall -Wextra \
    -c "$APPS/makedrv.c" -o "$OBJ/makedrv.o"
$CC -target x86_64-unknown-none-elf -nostdinc -ffreestanding -fno-pic -fno-pie \
    -ffunction-sections -fdata-sections -w -c "$APPS/makedrv_start.S" -o "$OBJ/makedrv_start.o"
$LD -m elf_x86_64 -static --gc-sections -z noexecstack -T "$APPS/makedrv64.ld" \
    -o "$OUT/make" "$OBJ/makedrv_start.o" "$OBJ/makedrv.o"

echo "==> ★ B5 6/7：构建期断言（ELF 头/程序头；逐条对应内核加载器与驱动的硬约束）"
$PY - "$OUT/make" "$OUT/make.bin" <<'PYEOF'
import os, struct, sys

BASE64   = 0x100000000             # USER64_CODE_VA64
STACK64  = 0x100000000 + 0x10000   # USER64_STACK_VA64
MMAP_VA  = 0x100000000 + 0x90000   # USER64_MMAP_VA64
WINDOW_TOP = 0x100000000 + 16 * 1024 * 1024
SPAN_MAX = WINDOW_TOP - MMAP_VA

def phdrs(d):
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    out = []
    for i in range(phn):
        q = phoff + i * phes
        t = struct.unpack_from("<I", d, q)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, q + 8)
        out.append((t, off, va, fsz, msz, al))
    return phoff, phes, phn, out

def check_driver(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "驱动不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "驱动必须是 ET_EXEC / x86_64"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    assert 0 < phn <= 16, "驱动 e_phnum 必须 <= 16"
    first = None
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: 驱动出现 PT_INTERP")
        if t == 2: raise SystemExit("ERROR: 驱动出现 PT_DYNAMIC")
        if t != 1: continue
        if first is None:
            first = (off, fsz)
        assert va >= BASE64 and va + msz <= STACK64, "驱动 PT_LOAD 越出 4GiB..4GiB+64KiB（va=%#x memsz=%#x）" % (va, msz)
    assert first is not None and first[0] == 0 and phoff + phn * phes <= first[1], \
        "驱动的程序头表不在第一个 PT_LOAD 里（auxv AT_PHDR 会是 0）"
    assert len(d) <= 96 * 1024, "驱动超过内核读盘缓冲 96 KiB"
    print("    断言 OK：驱动 %s = %d B，entry=%#x phnum=%d" % (os.path.basename(path), len(d), entry, phn))

def check_make(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "make.bin 不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "make.bin 必须是 ET_EXEC / x86_64"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff, phes, phn, ph = phdrs(d)
    assert 0 < phn <= 16, "make.bin e_phnum 必须 <= 16"
    hi = MMAP_VA
    first = None
    for (t, off, va, fsz, msz, al) in ph:
        if t == 3: raise SystemExit("ERROR: make.bin 出现 PT_INTERP（驱动不做重定位）")
        if t != 1: continue
        assert va >= MMAP_VA, "make.bin 的 PT_LOAD 低于 USER64_MMAP_VA64（驱动装载不了）"
        assert va + msz <= WINDOW_TOP, "make.bin 的 PT_LOAD 越出用户窗口"
        if first is None:
            first = (off, va, fsz)
        hi = max(hi, va + msz)
    assert first is not None and first[0] == 0 and first[1] == MMAP_VA, \
        "make.bin 的第一个 PT_LOAD 必须 off=0 / va=USER64_MMAP_VA64（驱动算 AT_PHDR 用）"
    span = hi - MMAP_VA
    assert span <= SPAN_MAX, "make.bin 的 span %d B 超过窗口里的 mmap 区" % span
    assert phoff < first[2], "程序头表不在第一个 PT_LOAD 的文件范围内"
    print("    断言 OK：make %s = %d B，entry=%#x phnum=%d span=%d B（%.1f KiB；mmap 区剩 %d B）"
          % (os.path.basename(path), len(d), entry, phn, span, span / 1024.0, SPAN_MAX - span))

for p in sys.argv[1:]:
    if p.endswith("/make"):
        check_driver(p)
    else:
        check_make(p)
PYEOF

echo
echo "    --- B5 体积记账（全都进系统卷，不进内核）---"
echo "    build64/make.bin = $(stat -c%s "$OUT/make.bin") B   （卷里 /lib/make.bin，静态 musl）"
echo "    build64/make     = $(stat -c%s "$OUT/make") B   （卷里 /bin/make，内核主程序装载器直接装）"
