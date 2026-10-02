#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""user/busybox/bb_config.py - 本移植的 busybox 配置表（**唯一的一份**）

为什么要有这个文件（不直接把 .config 提交进仓库）：
  * busybox 的 `.config` 由 Kconfig 生成，里面 1000+ 个符号的默认值随版本/依赖变化；
    手写一份会立刻过时。这里只写"我们要什么"，由 tools/busybox_build_win.sh 在构建期
    以 `make allnoconfig` 的产物为底、逐条覆盖，再 `make oldconfig` 规范化。
  * 归一化之后**回头断言**（见 REQUIRED），少一个就构建失败 —— 绝不"配错了还以为对"。

用法：py -3 user/busybox/bb_config.py <allnoconfig 生成的 .config>
"""
import sys

# ---------------------------------------------------------------------------
# 1) 编译/链接开关（busybox 的 Kconfig 里就是这些名字）
# ---------------------------------------------------------------------------
SETTINGS = {
    # 静态、非 PIC：VimtuOS 的装载驱动不做任何重定位（见 user/make/makedrv.c 的说明）
    "CONFIG_STATIC": "y",
    "CONFIG_PIE": "n",
    "CONFIG_NOMMU": "n",
    "CONFIG_LFS": "n",
    "CONFIG_BUILD_LIBBUSYBOX": "n",
    "CONFIG_FEATURE_INDIVIDUAL": "n",
    "CONFIG_FEATURE_INSTALLER": "n",
    "CONFIG_DEBUG": "n",
    "CONFIG_FEATURE_CLEAN_UP": "n",
    "CONFIG_DESKTOP": "n",
    "CONFIG_LONG_OPTS": "y",
    "CONFIG_FEATURE_VERBOSE_USAGE": "n",
    "CONFIG_FEATURE_COMPRESS_USAGE": "n",
    "CONFIG_FEATURE_SHOW_SCRIPT": "n",
    "CONFIG_FEATURE_DEVPTS": "n",
    "CONFIG_FEATURE_MOUNT_*": "n",
    "CONFIG_FEATURE_MOUNT_NFS": "n",
    "CONFIG_FEATURE_SYSLOG": "n",
    "CONFIG_FEATURE_UTMP": "n",
    "CONFIG_FEATURE_WTMP": "n",
    "CONFIG_USE_BB_CRYPT": "n",
    "CONFIG_USE_BB_CRYPT_SHA": "n",
    "CONFIG_USE_BB_PWD_GRP": "n",
    "CONFIG_USE_BB_SHADOW": "n",
    "CONFIG_PAM": "n",
    "CONFIG_SELINUX": "n",
    "CONFIG_FEATURE_SECURETTY": "n",
    "CONFIG_LOCALE_SUPPORT": "n",
    "CONFIG_UNICODE_SUPPORT": "n",
    "CONFIG_FEATURE_CHECK_UNICODE_IN_ENV": "n",
    "CONFIG_SUBST_WCHAR": "0",
    "CONFIG_LAST_SUPPORTED_WCHAR": "0",
    "CONFIG_UNICODE_COMBINING_WCHARS": "n",
    "CONFIG_UNICODE_WIDE_WCHARS": "n",
    "CONFIG_UNICODE_BIDI_SUPPORT": "n",
    "CONFIG_EXTRA_CFLAGS": "",     # 构建脚本按需填（见 tools/busybox_build_win.sh）
    "CONFIG_EXTRA_LDLIBS": "",
    "CONFIG_EXTRA_LDFLAGS": "",
    "CONFIG_CROSS_COMPILER_PREFIX": "",
    # 交叉/目标平台的探测结果（不跑宿主测试程序，直接钉死）
    "CONFIG_FEATURE_MOUNT_LOOP_CREATE": "n",
    "CONFIG_FEATURE_MOUNT_HELPERS": "n",
    # ---- shell（ash 当 /bin/sh；管道/重定向/`-c` 都靠它）----
    "CONFIG_ASH": "y",
    "CONFIG_SH_IS_ASH": "y",
    "CONFIG_BASH_IS_ASH": "y",
    "CONFIG_ASH_OPTIMIZE_FOR_SIZE": "y",
    "CONFIG_ASH_INTERNAL_GLOB": "y",
    "CONFIG_ASH_BASH_COMPAT": "y",
    "CONFIG_ASH_JOB_CONTROL": "n",
    "CONFIG_ASH_ALIAS": "y",
    "CONFIG_ASH_EXPAND_PRMT": "n",
    "CONFIG_ASH_ECHO": "y",
    "CONFIG_ASH_PRINTF": "y",
    "CONFIG_ASH_TEST": "y",
    "CONFIG_ASH_SLEEP": "y",
    "CONFIG_ASH_HELP": "y",
    "CONFIG_ASH_CMDCMD": "y",
    "CONFIG_ASH_MAIL": "n",
    "CONFIG_ASH_RANDOM_SUPPORT": "n",
    "CONFIG_ASH_GETOPTS": "n",
    "CONFIG_HUSH": "n",
    "CONFIG_CTTYHACK": "n",
    "CONFIG_FEATURE_SH_MATH": "n",
    "CONFIG_FEATURE_SH_MATH_64": "n",
    "CONFIG_FEATURE_SH_EXTRA_QUIET": "y",
    # 关键：内置 applet 直接在 fork 出来的子进程里跑（不用 execve、不依赖 PATH）
    "CONFIG_FEATURE_SH_STANDALONE": "y",
    "CONFIG_FEATURE_PREFER_APPLETS": "y",
    "CONFIG_FEATURE_SH_NOFORK": "n",
    "CONFIG_FEATURE_SH_READ_FRAC": "n",
    "CONFIG_FEATURE_SH_HISTFILESIZE": "n",
    "CONFIG_FEATURE_SH_EMBEDDED_SCRIPTS": "n",
    "CONFIG_FEATURE_EDITING": "n",
    "CONFIG_FEATURE_TAB_COMPLETION": "n",
    "CONFIG_FEATURE_USERNAME_COMPLETION": "n",
    "CONFIG_FEATURE_EDITING_FANCY_PROMPT": "n",
    "CONFIG_FEATURE_EDITING_SAVEHISTORY": "n",
    # ---- ls -l 要显示的东西（用户名解析不了 -> 数字 uid/gid，如实）----
    "CONFIG_FEATURE_LS_FILETYPES": "y",
    "CONFIG_FEATURE_LS_FOLLOWLINKS": "y",
    "CONFIG_FEATURE_LS_RECURSIVE": "y",
    "CONFIG_FEATURE_LS_SORTFILES": "y",
    "CONFIG_FEATURE_LS_TIMESTAMPS": "y",
    "CONFIG_FEATURE_LS_USERNAME": "n",
    "CONFIG_FEATURE_LS_COLOR": "n",
    "CONFIG_FEATURE_LS_COLOR_IS_DEFAULT": "n",
    # ---- 文本工具 ----
    "CONFIG_FEATURE_AWK_LIBM": "n",
    "CONFIG_FEATURE_AWK_GNU_EXTENSIONS": "y",
    "CONFIG_FEATURE_GREP_CONTEXT": "y",
    "CONFIG_FEATURE_SORT_BIG": "y",
    "CONFIG_FEATURE_SORT_OPTIMIZE_MEMORY": "n",
    "CONFIG_FEATURE_CUT_REGEX": "n",
    "CONFIG_FEATURE_DIFF_DIR": "n",
    "CONFIG_FEATURE_TR_CLASSES": "n",
    "CONFIG_FEATURE_TR_EQUIV": "n",
    "CONFIG_FEATURE_FIND_PRINT0": "y",
    "CONFIG_FEATURE_FIND_TYPE": "y",
    "CONFIG_FEATURE_FIND_MAXDEPTH": "y",
    "CONFIG_FEATURE_FIND_NEWER": "n",
    "CONFIG_FEATURE_FIND_PERM": "y",
    "CONFIG_FEATURE_FIND_MTIME": "n",
    "CONFIG_FEATURE_FIND_EXEC": "y",
    "CONFIG_FEATURE_FIND_PRUNE": "y",
    "CONFIG_FEATURE_FIND_DELETE": "n",
    "CONFIG_FEATURE_FIND_REGEX": "n",
    "CONFIG_FEATURE_FIND_DEPTH": "y",
    "CONFIG_FEATURE_FIND_PAREN": "y",
    "CONFIG_FEATURE_FIND_USER": "n",
    "CONFIG_FEATURE_FIND_GROUP": "n",
    "CONFIG_FEATURE_FIND_SIZE": "n",
    "CONFIG_FEATURE_FIND_LINKS": "n",
    "CONFIG_FEATURE_FIND_INUM": "n",
    "CONFIG_FEATURE_FIND_NOT": "y",
    "CONFIG_FEATURE_XARGS_SUPPORT_CONFIRMATION": "n",
    "CONFIG_FEATURE_XARGS_SUPPORT_QUOTES": "y",
    "CONFIG_FEATURE_XARGS_SUPPORT_TERMOPT": "y",
    "CONFIG_FEATURE_TASKSET_FANCY": "n",
    "CONFIG_FEATURE_TRACEROUTE_VERBOSE": "n",
    "CONFIG_FEATURE_CHOWN_LONG_OPTIONS": "n",
    # ---- 归档 / 压缩 ----
    "CONFIG_FEATURE_TAR_CREATE": "y",
    "CONFIG_FEATURE_TAR_AUTODETECT": "n",
    "CONFIG_FEATURE_TAR_FROM": "y",
    "CONFIG_FEATURE_TAR_GNU_EXTENSIONS": "y",
    "CONFIG_FEATURE_TAR_LONG_OPTIONS": "y",
    "CONFIG_FEATURE_TAR_UNAME_GNAME": "n",
    "CONFIG_FEATURE_TAR_NOPRESERVE_TIME": "n",
    "CONFIG_FEATURE_TAR_SELINUX": "n",
    "CONFIG_FEATURE_TAR_OLDSUN_COMPATIBILITY": "n",
    "CONFIG_FEATURE_GZIP_LONG_OPTIONS": "y",
    "CONFIG_FEATURE_GZIP_DECOMPRESS": "y",
    "CONFIG_FEATURE_SEAMLESS_LZMA": "n",
    "CONFIG_FEATURE_SEAMLESS_BZ2": "n",
    "CONFIG_FEATURE_SEAMLESS_GZ": "y",
    "CONFIG_FEATURE_SEAMLESS_Z": "n",
    "CONFIG_FEATURE_COMPRESS_BBCONFIG": "n",
    "CONFIG_FEATURE_BZIP2_DECOMPRESS": "n",
    "CONFIG_GZIP_FAST": "0",
    "CONFIG_LZMA": "n",
    "CONFIG_XZ": "n",
    "CONFIG_BZIP2": "n",
    "CONFIG_UNXZ": "n",
    # ---- 编辑器 ----
    "CONFIG_FEATURE_VI_MAX_LEN": "4096",
    "CONFIG_FEATURE_VI_8BIT": "n",
    "CONFIG_FEATURE_VI_COLON": "y",
    "CONFIG_FEATURE_VI_YANKMARK": "y",
    "CONFIG_FEATURE_VI_SEARCH": "y",
    "CONFIG_FEATURE_VI_REGEX_SEARCH": "n",
    "CONFIG_FEATURE_VI_USE_SIGNALS": "n",
    "CONFIG_FEATURE_VI_DOT_CMD": "y",
    "CONFIG_FEATURE_VI_READONLY": "y",
    "CONFIG_FEATURE_VI_SETOPTS": "y",
    "CONFIG_FEATURE_VI_SET": "y",
    "CONFIG_FEATURE_VI_WIN_RESIZE": "n",
    "CONFIG_FEATURE_VI_ASK_TERMINAL": "n",
    "CONFIG_FEATURE_VI_UNDO": "n",
    "CONFIG_FEATURE_VI_UNDO_QUEUE": "n",
    "CONFIG_FEATURE_VI_VERBOSE_STATUS": "n",
    "CONFIG_FEATURE_ALLOW_EXEC": "n",
    # ---- 进程 / 系统（注：本内核 ring3 没有 /proc，ps/free/uptime 只能"跑起来但列不出东西"，
    #      报告里如实标 GAP；applet 仍然装进去，好让用户拿到清楚的报错而不是找不到命令）----
    "CONFIG_FEATURE_PS_WIDE": "n",
    "CONFIG_FEATURE_PS_LONG": "n",
    "CONFIG_FEATURE_PS_TIME": "n",
    "CONFIG_FEATURE_PS_ADDITIONAL_COLUMNS": "n",
    "CONFIG_FEATURE_SHOW_THREADS": "n",
    "CONFIG_FEATURE_DF_FANCY": "n",
    "CONFIG_FEATURE_HUMAN_READABLE": "y",
    "CONFIG_FEATURE_MOUNT_FLAGS": "n",
    "CONFIG_FEATURE_KILL_DELAY": "0",
    "CONFIG_FEATURE_KILL_REMOVED": "n",
    "CONFIG_FEATURE_UPTIME_UTMP_SUPPORT": "n",
    "CONFIG_FEATURE_UMOUNT_ALL": "n",
    "CONFIG_DEFAULT_SETFONT_DIR": '""',
    "CONFIG_DEFAULT_DEPMOD_FILE": '""',
    # ---- 其他杂项（关掉省体积、也避开用不到的 ABI）----
    "CONFIG_FEATURE_IPV6": "n",
    "CONFIG_FEATURE_PREFER_IPV4_ADDRESS": "n",
    "CONFIG_FEATURE_UNIX_LOCAL": "n",
    "CONFIG_VERBOSE_RESOLUTION_ERRORS": "n",
    "CONFIG_FEATURE_TELNET_TTYPE": "n",
    "CONFIG_FEATURE_TELNET_AUTOLOGIN": "n",
    "CONFIG_FEATURE_TELNET_WIDTH": "n",
    "CONFIG_FEATURE_TELNETD_STANDALONE": "n",
    "CONFIG_FEATURE_TELNETD_INETD_WAIT": "n",
    "CONFIG_FEATURE_WGET_LONG_OPTIONS": "y",
    "CONFIG_FEATURE_WGET_STATUSBAR": "n",
    "CONFIG_FEATURE_WGET_AUTHENTICATION": "n",
    "CONFIG_FEATURE_WGET_TIMEOUT": "n",
    "CONFIG_FEATURE_WGET_FTP": "n",
    "CONFIG_FEATURE_WGET_OPENSSL": "n",
    "CONFIG_FEATURE_WGET_HTTPS": "n",
    "CONFIG_FEATURE_FANCY_PING": "n",
    "CONFIG_FEATURE_NTPD_SERVER": "n",
    "CONFIG_FEATURE_NTPD_CONF": "n",
    "CONFIG_FEATURE_TFTP_GET": "n",
    "CONFIG_FEATURE_TFTP_PUT": "n",
    "CONFIG_FEATURE_TFTP_BLOCKSIZE": "n",
    "CONFIG_FEATURE_TFTP_PROGRESS_BAR": "n",
    "CONFIG_FEATURE_TFTP_HPA_COMPAT": "n",
    "CONFIG_FEATURE_FTPGETPUT_LONG_OPTIONS": "n",
    "CONFIG_FEATURE_HTTPD_BASIC_AUTH": "n",
    "CONFIG_FEATURE_HWIB": "n",
    "CONFIG_FEATURE_VOLUMEID_*": "n",
}

# ---------------------------------------------------------------------------
# 2) 要编进去的 applet（CONFIG_<大写的名字>）
#    —— 覆盖"日常可用"的那一批；报告里逐条给验证原文。
#    名字 = busybox 的 applet 名（applets.src.h 里 APPLET(...) 的第一个参数）。
# ---------------------------------------------------------------------------
APPLETS = [
    # 文件
    "ls", "cp", "mv", "rm", "mkdir", "rmdir", "cat", "head", "tail", "wc",
    "find", "grep", "sed", "awk", "sort", "uniq", "od", "hexdump", "chmod",
    "chown", "chgrp", "ln", "touch", "stat", "cut", "tr", "diff", "du", "df",
    "dd", "cmp", "split", "tee", "base64", "md5sum", "sha1sum", "sha256sum",
    "sha512sum", "cksum", "comm", "fold", "nl", "tac", "truncate", "expand",
    "unexpand", "paste", "seq", "dirname", "basename", "realpath", "mktemp",
    "readlink", "xargs", "which", "sync", "link", "unlink", "install",
    "mknod", "mkfifo",
    # 文本编辑
    "vi", "ed", "more", "less", "strings", "sum", "xxd",
    # 进程 / 系统
    "ps", "kill", "killall", "uname", "uptime", "free", "id", "whoami",
    "groups", "env", "printenv", "unix2dos", "dos2unix", "usleep",
    "sleep", "date", "time", "timeout", "nohup", "logname", "tty", "pwd",
    "true", "false", "yes", "echo", "printf", "test", "expr", "getopt",
    "setsid", "watch", "start-stop-daemon", "pidof",
    # 归档
    "tar", "gzip", "gunzip", "zcat", "cpio",
    # 网络（本内核 ring3 没有 socket/getaddrinfo -> 如实标 GAP）
    #   `ip` 需要 Linux 内核头（linux/types.h），我们的 musl 头里没有 -> 先不编；
    #   网络栈整体是 GAP（见报告）。
    "wget", "ping", "nc", "netstat", "ifconfig", "route", "arp",
    "hostname", "nslookup",
    # shell（`sh` 这个名字由 SH_IS_ASH 提供，没有独立的 CONFIG_SH）
    "ash",
]

# 归一化之后必须为 "y" 的符号（构建脚本会断言；少一个就直接失败）
REQUIRED = [
    "CONFIG_STATIC",
    "CONFIG_ASH", "CONFIG_SH_IS_ASH", "CONFIG_FEATURE_SH_STANDALONE",
    "CONFIG_FEATURE_PREFER_APPLETS",
    "CONFIG_LS", "CONFIG_CP", "CONFIG_MV", "CONFIG_RM", "CONFIG_MKDIR",
    "CONFIG_CAT", "CONFIG_HEAD", "CONFIG_TAIL", "CONFIG_WC", "CONFIG_FIND",
    "CONFIG_GREP", "CONFIG_SED", "CONFIG_AWK", "CONFIG_SORT", "CONFIG_UNIQ",
    "CONFIG_OD", "CONFIG_HEXDUMP", "CONFIG_CHMOD", "CONFIG_VI", "CONFIG_ED",
    "CONFIG_PS", "CONFIG_KILL", "CONFIG_DF", "CONFIG_UNAME", "CONFIG_ID",
    "CONFIG_WHOAMI", "CONFIG_TAR", "CONFIG_GZIP", "CONFIG_GUNZIP",
    "CONFIG_WGET", "CONFIG_PING",
]

# /bin/<name> 包装程序（= 用 execve 把自己换成 /bin/busybox，
# argv[0] 保留成用户敲的那个名字，busybox 按 argv[0] 的 basename 选 applet）
WRAPPERS = [
    "ls", "cp", "mv", "rm", "mkdir", "rmdir", "cat", "head", "tail", "wc",
    "find", "grep", "sed", "awk", "sort", "uniq", "od", "hexdump", "chmod",
    "chown", "chgrp", "ln", "touch", "stat", "cut", "tr", "diff", "du", "df",
    "dd", "cmp", "tee", "md5sum", "sha256sum", "base64", "basename", "dirname",
    "realpath", "mktemp", "xargs", "vi", "ed", "more", "less", "ps", "kill",
    "killall", "free", "uptime", "uname", "id", "whoami", "env", "date",
    "sleep", "echo", "printf", "seq", "tar", "gzip", "gunzip", "wget", "ping",
]


def _set(line, name, value):
    """把 .config 里的一行 CONFIG_X=.../# CONFIG_X is not set 换掉；返回新行。"""
    if value == "n":
        return "# %s is not set" % name
    return "%s=%s" % (name, value)


def apply(path, strict=False):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        lines = f.read().split("\n")

    wanted = dict(SETTINGS)
    for a in APPLETS:
        wanted["CONFIG_" + a.upper().replace("-", "_")] = "y"
    # 通配的 *_* 名字（如 CONFIG_FEATURE_MOUNT_*）不直接写进 .config
    for k in [k for k in wanted if k.endswith("*")]:
        del wanted[k]

    seen = set()
    out = []
    for ln in lines:
        s = ln.strip()
        name = None
        if s.startswith("# ") and s.endswith(" is not set"):
            name = s[2:-len(" is not set")]
        elif s.startswith("CONFIG_") and "=" in s:
            name = s.split("=", 1)[0]
        if name in wanted:
            seen.add(name)
            out.append(_set(ln, name, wanted[name]))
        else:
            out.append(ln)
    missing = sorted(set(wanted) - seen)
    if missing:
        sys.stderr.write("bb_config: 这一遍 .config 里还没有这些符号（Kconfig 只在父项打开后才显示它们；"
                         "第一遍容忍、第二遍必须齐）：\n")
        for m in missing:
            sys.stderr.write("    %s\n" % m)
        if strict:
            raise SystemExit(2)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print("    .config 覆盖：%d 个符号（%d 个 applet + %d 条设置）"
          % (len(wanted), len(APPLETS), len(SETTINGS)))


def check(path, required):
    bad = []
    values = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            s = ln.strip()
            if "=" in s and s.startswith("CONFIG_"):
                values[s.split("=", 1)[0]] = s.split("=", 1)[1]
    for r in required:
        if values.get(r) != "y":
            bad.append(r)
    return bad


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if a != "--strict"]
    strict = "--strict" in sys.argv[1:]
    if len(args) != 1:
        sys.stderr.write("用法：py -3 user/busybox/bb_config.py <.config> [--strict]\n")
        raise SystemExit(2)
    apply(args[0], strict=strict)
