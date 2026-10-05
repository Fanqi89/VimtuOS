#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/lxcorpus_build.py - ★ Linux 应用语料（lxcorpus）构建器 —— 宿主侧，只读仓库、只写 build64/

目标（用户计划：**VimtuOS 要能安装并运行 Linux 上的软件**）：造出一份**可复现**的语料，
按"今天能不能跑"分成四类（每类都有可判定的期望输出），供 tests/linuxapp64_test.py 在 QEMU 里
逐类跑、把失败点钉死：

  ① 静态 musl（对照组）      stathello.elf   —— 期望**今天就能跑**（内核只做静态装载）
  ①c 静态 musl（> 96 KiB）   bighello.elf    —— 期望今天失败：内核读盘缓冲 96 KiB（elf64.h:80）
  ② 动态 musl                dynhello.elf + ld-musl-x86_64.so.1 + libc.so   —— 期望今天失败
  ②b 动态程序 + glibc 解释器  dynhello_gnuinterp.elf（只为把内核带到"解释器那一步"）
  ③ 动态 glibc（真发行版）    gnuhello（= Debian hello 包里的 /usr/bin/hello）+ ld-linux + libc.so.6
  ④ 真 .deb                 hello_2.10-2_amd64.deb（Debian 原样，xz）与 _gz.deb（宿主重压成 gzip）

产物（**不提交**，build64/ 已 gitignore）：build64/lxcorpus/
    src/            本次语料用到的 C 源码（从本文件写出去的，构建可复现）
    *.elf / gnuhello / *.so* / *.deb / CORPUS.json（清单：卷内路径、size、sha256、期望输出）
    dl/             下载缓存（离线可重建）
    .sh/            实际执行的 bash 脚本（留证）

为什么要自己编 musl 的**动态**形态（②）：仓库里 third_party/musl/lib 只有 libc.a，
A3 那轮如实记了"musl 自带的 ld-musl（动态形态）没通"，缺的正是 lib/libc.so。
musl 的共享 libc.so **就是**它的动态链接器 ld-musl-x86_64.so.1（同一个文件两个名字），
本脚本用 musl 自己的 Makefile 规则手工链接它（见 build_musl_shared 的注释：Windows 上
make 的归档/链接命令行会超长，所以用 ld.lld + 响应文件，绕开命令行长度限制）。

为什么 ③ 的 main 程序跑不起来还要放进来：这正是本批次要钉死的**头号缺口** ——
kernel/elf64.cpp:75 把**主程序**的映像区间硬编码在 [4GiB, 4GiB+64KiB)（e64_lo64/e64_hi64），
`base=0`，没有 load bias/PIE 支持 → 发行版二进制（PIE，p_vaddr=0）与老式非 PIE（0x400000）
一律 [ELF64] reject reason=outside-user-window。

用法（**必须 Windows 原生 Python**：MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tools/lxcorpus_build.py                # 全量构建（缺缓存时才联网）
    py -3 tools/lxcorpus_build.py --offline      # 只用 dl/ 缓存，绝不联网
    py -3 tools/lxcorpus_build.py --reuse-shared # 不动 musl 共享库（调试用）
退出码：0 = 全建成；1 = 有硬失败；2 = 环境问题（缺 MSYS2 clang/ld.lld、缺下载缓存）。
"""
import argparse
import gzip
import hashlib
import json
import lzma
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import io

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MSYS_BASH = r"C:\msys64\usr\bin\bash.exe"
MUSL = os.path.join(ROOT, "third_party", "musl")
OUT = os.path.join(ROOT, "build64", "lxcorpus")
DL = os.path.join(OUT, "dl")
SRC = os.path.join(OUT, "src")
SH = os.path.join(OUT, ".sh")
LIBDIR = os.path.join(OUT, "libs")            # 链接期用的 -L（= 卷里 /lib 的镜像）

# 期望输出（**测试按逐字节比对**；改这里就要同步改 tests/linuxapp64_test.py 的断言）
STATIC_MARK = "[LXSTATIC] hello from static musl ELF"
DYN_MARK = "[LXDYN] hello from dynamic musl ELF"

# ③ 的 glibc 运行时来自 Debian bookworm 的真包（**下载缓存**，不进仓库）
DEB_BASE = "http://deb.debian.org/debian/pool/main"
DEB_HELLO = DEB_BASE + "/h/hello/hello_2.10-2_amd64.deb"
DEB_LIBC6_LIST = DEB_BASE + "/g/glibc/"                      # 目录列表里挑最新的 libc6 2.36
DEB_LIBC6_RE = r"libc6_2\.36-[0-9]+\+deb12u[0-9]+_amd64\.deb"
DEB_LIBC6_FALLBACK = DEB_BASE + "/g/glibc/libc6_2.36-9+deb12u14_amd64.deb"

# 内核侧硬约束（**只读引用**，唯一真源在 kernel/；这里只用于报告/自检，别当权威）
KLIMITS = {
    "elf_max_file_bytes": 96 * 1024,              # kernel/elf64.h:80 ELF64_MAX_FILE_BYTES64
    "main_lo": 0x100000000,                       # kernel/elf64.cpp:75 e64_lo64
    "main_hi": 0x100010000,                       # kernel/elf64.cpp:76 e64_hi64（64 KiB 窗口）
    "interp_lo": 0x100090000,                     # USER64_MMAP_VA64
    "interp_top": 0x100100000,                    # USER64_INTERP_TOP_VA64
    "max_phdr": 16,                               # ELF64_MAX_PHDR64
    "vfs_max_file_bytes": 8 * 1024 * 1024,
}


def q(p):
    return p.replace("\\", "/")


def win_to_msys(p):
    """"C:/Users/x" -> "/c/Users/x"（MSYS2 口径）"""
    p = os.path.abspath(p).replace("\\", "/")
    m = re.match(r"^([A-Za-z]):/(.*)$", p)
    return ("/%s/%s" % (m.group(1).lower(), m.group(2))) if m else p


def sha256_of(b):
    return hashlib.sha256(b).hexdigest()


def run_bash(name, script, keep=True):
    """跑一段 bash（MSYS2 登录 shell + mingw64 的 clang/lld 进 PATH），脚本落盘留证。"""
    os.makedirs(SH, exist_ok=True)
    os.makedirs(os.path.dirname(os.path.join(SH, name)), exist_ok=True)
    sp = os.path.join(SH, name + ".sh")
    with open(sp, "w", encoding="utf-8", newline="\n") as f:
        f.write("set -e\n")
        f.write('export PATH="/c/msys64/mingw64/bin:/c/msys64/usr/bin:$PATH"\n')
        f.write("cd %s\n" % win_to_msys(ROOT))
        f.write(script)
        if not script.endswith("\n"):
            f.write("\n")
    cmd = [MSYS_BASH, "-l", "-c", "bash %s" % win_to_msys(sp)]
    r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    if r.returncode != 0:
        sys.stderr.write("---- %s.sh 失败（rc=%d）----\n%s\n%s\n" % (name, r.returncode,
                                                                  r.stdout[-2000:], r.stderr[-4000:]))
        raise RuntimeError("bash 步骤失败：%s" % name)
    for line in (r.stdout or "").splitlines():
        print("    " + line)
    return r.stdout


def curl(url, dst, offline=False):
    if os.path.exists(dst) and os.path.getsize(dst) > 0:
        return True
    if offline:
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    r = subprocess.run(["curl.exe", "-sS", "-L", "--max-time", "120", "-o", dst, url],
                       capture_output=True, text=True, errors="replace")
    ok = r.returncode == 0 and os.path.exists(dst) and os.path.getsize(dst) > 0
    if not ok:
        sys.stderr.write("下载失败 %s：%s\n" % (url, (r.stderr or "").strip()[:200]))
        if os.path.exists(dst):
            os.remove(dst)
    return ok


def curl_text(url, offline=False):
    if offline:
        return ""
    r = subprocess.run(["curl.exe", "-sS", "-L", "--max-time", "60", url],
                       capture_output=True, text=True, errors="replace")
    return r.stdout if r.returncode == 0 else ""


# ---------------------------------------------------------------------------
# ELF 只读检查（纯 Python：不依赖 binutils；与 kernel/elf64.cpp 的判据同向）
# ---------------------------------------------------------------------------
def elf_info(b):
    if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
        raise ValueError("不是 ELF64 小端")
    etype = struct.unpack_from("<H", b, 16)[0]
    machine = struct.unpack_from("<H", b, 18)[0]
    entry = struct.unpack_from("<Q", b, 24)[0]
    phoff = struct.unpack_from("<Q", b, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", b, 54)
    out = {"type": etype, "machine": machine, "entry": entry, "phnum": phnum,
           "interp": None, "loads": [], "dynamic": False, "tls": False, "span": 0,
           "file_bytes": len(b), "needed": [], "phdr_in_first_load": False}
    for i in range(phnum):
        o = phoff + i * phentsize
        ptype, pflags = struct.unpack_from("<II", b, o)
        poff, pva, _ppa, pfsz, pmsz = struct.unpack_from("<QQQQQ", b, o + 8)
        if ptype == 3:
            j = b.index(b"\x00", poff)
            out["interp"] = b[poff:j].decode("ascii", "replace")
        elif ptype == 2:
            out["dynamic"] = True
        elif ptype == 7:
            out["tls"] = True
        elif ptype == 1:
            out["loads"].append({"va": pva, "memsz": pmsz, "filesz": pfsz, "off": poff,
                                 "flags": pflags})
            out["span"] = max(out["span"], pva + pmsz)
            if poff == 0 and phoff + phnum * phentsize <= pfsz:
                out["phdr_in_first_load"] = True
    if out["dynamic"]:
        for i in range(phnum):
            o = phoff + i * phentsize
            if struct.unpack_from("<I", b, o)[0] != 2:
                continue
            doff, dsz = struct.unpack_from("<QQ", b, o + 8)
            p = doff
            while p + 16 <= doff + dsz:
                tag, val = struct.unpack_from("<QQ", b, p)
                if tag == 0:
                    break
                if tag == 1:                                # DT_NEEDED（strtab 在里面）
                    for k in range(phnum):                  # 找 .dynstr：从 PT_LOAD 里扫太脏，
                        pass                                # 只在下面按整文件扫字符串表
                p += 16
            # DT_NEEDED 的字符串偏移是相对 .dynstr；这里够用的近似：拿整文件里的
            # "libc.so\0" / "libc.so.6\0" 字面量判断（只用于报告）
            for cand in (b"libc.so\x00", b"libc.so.6\x00"):
                if cand in b:
                    out["needed"].append(cand[:-1].decode())
    return out


def elf_note(inf):
    return ("type=%s entry=0x%x phnum=%d loads=%d span=0x%x file=%d B interp=%s"
            % ({2: "EXEC", 3: "DYN"}.get(inf["type"], inf["type"]), inf["entry"], inf["phnum"],
               len(inf["loads"]), inf["span"], inf["file_bytes"], inf["interp"] or "-"))

# ===========================================================================
# ① 静态 musl（对照 + 96 KiB 上限反例）
# ===========================================================================
STATIC_C = r"""/* ① 静态 musl 语料（由 tools/lxcorpus_build.py 生成，别手改 build64 里的副本）
 * 期望输出（宿主测试逐字节比对）：见 tools/lxcorpus_build.py 的 STATIC_MARK
 * 链接：自写 _start + lib/libc.a + tools/musl_hello64.ld（钉在 4GiB、无 PT_INTERP）
 *
 * 为什么入口是**自写 _start**（不是 musl 的 crt1.o）：与 user/apps/muslhello.c 同因 ——
 *   crt1.o 里有 `lea _DYNAMIC(%rip),%rsi`（非 PIC 的 _DYNAMIC 引用），静态链接到 4GiB 时
 *   _DYNAMIC 解析成 0 -> PC32 重定位超范围（ld.lld: relocation R_X86_64_PC32 out of range）。
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                            void (*init_dummy)(void), void (*fini_dummy)(void),
                            void (*ldso_dummy)(void));
static int lx_main(int argc, char** argv, char** envp);
static void lx_start_c(uint64_t* sp);

__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void) {
    __asm__ volatile("xor %rbp, %rbp\n\t"
                     "mov %rsp, %rdi\n\t"
                     "and $-16, %rsp\n\t"
                     "call lx_start_c\n\t"
                     "hlt\n\t");
}

__attribute__((used, noinline))
static void lx_start_c(uint64_t* sp) {
    __libc_start_main(lx_main, (int)sp[0], (char**)(sp + 1), 0, 0, 0);
    _exit(127);
}

static int lx_main(int argc, char** argv, char** envp) {
    static const char mark[] = "[LXSTATIC] hello from static musl ELF\n";
    char buf[160];
    int n = 0;
    (void)argv; (void)envp;
    write(1, mark, sizeof(mark) - 1);
    /* 不用 printf：用 libc 的 snprintf + write 就该看到 musl 的 stdio/字符串都活着 */
    n += (int)(unsigned long)snprintf(buf, sizeof(buf), "[LXSTATIC] snprintf argc=%d errno=%d\n",
                                      argc, ENOENT);
    if (n > 0) write(1, buf, (unsigned)n);
    return 0;
}
"""

BIG_C = r"""/* ①c 静态 musl，但**故意 > 96 KiB**（kernel/elf64.h:80 的读盘缓冲上限）
 * 期望：内核在"读文件之前"就按 size 拒绝 —— [ELF64] exec reject reason=size
 * 为什么这条重要：真实发行版程序（busybox 动态 2 MB、coreutils 100~200 KB/个）全部先撞这一条。
 */
#include <stdint.h>
#include <unistd.h>
static const char mark[] = "[LXBIG] hello (should never print)\n";
#include "big_blob.h"
extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                            void (*init_dummy)(void), void (*fini_dummy)(void),
                            void (*ldso_dummy)(void));
static int lx_main(int argc, char** argv, char** envp);
static void lx_start_c(uint64_t* sp);
__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void) {
    __asm__ volatile("xor %rbp, %rbp\n\t"
                     "mov %rsp, %rdi\n\t"
                     "and $-16, %rsp\n\t"
                     "call lx_start_c\n\t"
                     "hlt\n\t");
}
__attribute__((used, noinline))
static void lx_start_c(uint64_t* sp) {
    __libc_start_main(lx_main, (int)sp[0], (char**)(sp + 1), 0, 0, 0);
    _exit(127);
}
static int lx_main(int argc, char** argv, char** envp) {
    /* 用 argc 算下标（编译期折不掉）引用一下，别被 --gc-sections 丢掉 */
    unsigned i = (unsigned)argc & (sizeof(big_blob) - 1u);
    if (big_blob[i] == 0x7f) write(1, mark, sizeof(mark) - 1);
    (void)argv; (void)envp;
    return 0;
}
"""


def gen_big_blob(path, size=400 * 1024):
    """生成一个 400 KiB 的 const 数组头（语料源码；让 ELF 本体远超 96 KiB 上限）。"""
    rows = []
    total = size // 16
    for i in range(total):
        rows.append(",".join("0x%02x" % ((i * 7 + k) & 0xFF) for k in range(16)))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("/* generated by tools/lxcorpus_build.py: %d B of read-only data\n"
                " * (purpose: push the ELF file size past the kernel 96 KiB read-buffer limit) */\n"
                % size)
        f.write("static const unsigned char big_blob[%d] = {\n" % size)
        for i in range(0, len(rows), 8):
            f.write(",".join(rows[i:i + 8]) + ",\n")
        f.write("};\n")


def build_static_hello():
    os.makedirs(SRC, exist_ok=True)
    with open(os.path.join(SRC, "stathello.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(STATIC_C)
    with open(os.path.join(SRC, "bighello.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(BIG_C)
    gen_big_blob(os.path.join(SRC, "big_blob.h"))
    script = """
MUSL="third_party/musl"
INC="-nostdinc -isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CF="$INC -O2 -fno-pic -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 -mcmodel=large -mno-sse -mno-sse2 -mno-mmx -mno-avx -ffunction-sections -fdata-sections -std=c11 -I build64/lxcorpus/src"
mkdir -p build64/lxcorpus/.obj
LIBC_A="$MUSL/lib/libc.a"
[ -f "$LIBC_A" ] || { echo "缺 $LIBC_A（先跑 bash build64.sh）"; exit 2; }
echo "    musl 静态库：$LIBC_A（$(stat -c%s "$LIBC_A") B）"
for n in stathello bighello; do
  clang --target=x86_64-linux-gnu $CF -c "build64/lxcorpus/src/$n.c" -o "build64/lxcorpus/.obj/$n.o"
  # 链接顺序与 musl 的规格一致：自写 _start 在程序对象里 -> 程序 -> libc.a
  ld.lld -m elf_x86_64 -static --gc-sections -z noexecstack \\
         -T tools/musl_hello64.ld -o "build64/lxcorpus/$n.elf" \\
         "build64/lxcorpus/.obj/$n.o" "$LIBC_A"
  echo "    ① $n.elf = $(stat -c%s "build64/lxcorpus/$n.elf") B"
done
"""
    run_bash("01_static", script)


# ===========================================================================
# ② 动态 musl：lib/libc.so（= ld-musl-x86_64.so.1）+ 动态 hello
# ===========================================================================
DYN_C = r"""/* ② 动态 musl 语料（由 tools/lxcorpus_build.py 生成）
 * PT_INTERP = /lib/ld-musl-x86_64.so.1（同一份字节也在 /lib/libc.so，VimtuFS2 没有符号链接）
 * 期望输出：见 tools/lxcorpus_build.py 的 DYN_MARK
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

/* 与静态那份同一套自写 _start（理由见 ① 的注释）；动态链接时 printf/snprintf 来自
 * /lib/libc.so（= ld-musl-x86_64.so.1）—— 调用要走 PLT/GOT + 解释器重定位。 */
extern int __libc_start_main(int (*main)(int, char**, char**), int argc, char** argv,
                            void (*init_dummy)(void), void (*fini_dummy)(void),
                            void (*ldso_dummy)(void));
static int lx_main(int argc, char** argv, char** envp);
static void lx_start_c(uint64_t* sp);

__attribute__((naked, noreturn, used, section(".text.start")))
void _start(void) {
    __asm__ volatile("xor %rbp, %rbp\n\t"
                     "mov %rsp, %rdi\n\t"
                     "and $-16, %rsp\n\t"
                     "call lx_start_c\n\t"
                     "hlt\n\t");
}

__attribute__((used, noinline))
static void lx_start_c(uint64_t* sp) {
    __libc_start_main(lx_main, (int)sp[0], (char**)(sp + 1), 0, 0, 0);
    _exit(127);
}

static int lx_main(int argc, char** argv, char** envp) {
    static const char mark[] = "[LXDYN] hello from dynamic musl ELF\n";
    char buf[160];
    int n;
    (void)argv; (void)envp;
    write(1, mark, sizeof(mark) - 1);
    n = (int)(unsigned long)snprintf(buf, sizeof(buf), "[LXDYN] libc via PLT argc=%d\n", argc);
    if (n > 0) write(1, buf, (unsigned)n);
    return 0;
}
"""


def musl_shared_objs():
    """"obj/src/**/*.lo + obj/compat/**/*.lo + obj/ldso/{dlstart,dynlink}.lo"
    —— 与 musl 自己的 Makefile 同一口径（LOBJS + LDSO_OBJS）。返回相对 musl 目录的路径。"""
    out = []
    for sub in ("obj/src", "obj/compat"):
        base = os.path.join(MUSL, sub)
        for dirpath, _dn, files in os.walk(base):
            for fn in files:
                if fn.endswith(".lo"):
                    out.append(os.path.relpath(os.path.join(dirpath, fn), MUSL).replace("\\", "/"))
    for fn in ("obj/ldso/dlstart.lo", "obj/ldso/dynlink.lo"):
        if os.path.exists(os.path.join(MUSL, fn)):
            out.append(fn)
    return sorted(out)


def build_musl_shared(reuse=False):
    spec = musl_shared_objs()
    if len(spec) < 100:
        return None, "musl 的 PIC 目标文件（obj/**/*.lo）只有 %d 个 —— 先在 third_party/musl 里编一遍" % len(spec)
    objs = os.path.join(OUT, "musl_libc_objs.rsp")
    with open(objs, "w", encoding="ascii", newline="\n") as f:
        for p in spec:                                     # 一行一个参数（lld 的响应文件）
            f.write(p + "\n")
    dst = os.path.join(OUT, "libc.so")
    if not (reuse and os.path.exists(dst)):
        # ★ Windows 上 musl 的 $(CC) -shared 会因命令行超长失败（1300+ 个目标文件），
        #   所以这里直接调 ld.lld + 响应文件：与 musl 的 lib/libc.so 规则同参数
        #   （$(CC) ... -nostdlib -shared -Wl,-e,_dlstart -o lib/libc.so $(LOBJS) $(LDSO_OBJS)）。
        run_bash("02_musl_shared", """
cd third_party/musl
OBJS=$(cygpath -w "$(pwd)/../.." 2>/dev/null || true)
ld.lld -m elf_x86_64 -shared -e _dlstart -soname libc.so -z noexecstack \\
       -o "../../build64/lxcorpus/libc.so" "@../../build64/lxcorpus/musl_libc_objs.rsp"
""")
    b = open(dst, "rb").read()
    inf = elf_info(b)
    os.makedirs(LIBDIR, exist_ok=True)
    shutil.copyfile(dst, os.path.join(LIBDIR, "libc.so"))
    shutil.copyfile(dst, os.path.join(OUT, "ld-musl-x86_64.so.1"))
    note = elf_note(inf)
    print("    ② musl 共享 libc.so = %d B（%s）；= ld-musl-x86_64.so.1" % (len(b), note))
    return {"libc_so": dst, "info": inf, "note": note, "objs": len(spec)}, None


def patch_interp(path, new_interp):
    """"把 PT_INTERP 字符串换成 new_interp —— 必须这么做，因为 MSYS2 的 bash 会把
    `/lib/ld-musl-x86_64.so.1` 这种绝对 POSIX 路径**改写成 Windows 路径**再交给原生
    ld.lld.exe（实测产物里写着 `C:/msys64/lib/ld-musl-x86_64.so.1`），而内核要的是
    卷内绝对路径。脚本里同时设了 MSYS2_ARG_CONV_EXCL='*'，这里是**兜底 + 校验**。"""
    b = bytearray(open(path, "rb").read())
    phoff = struct.unpack_from("<Q", b, 32)[0]
    phentsize, phnum = struct.unpack_from("<HH", b, 54)
    want = new_interp.encode("ascii") + b"\x00"
    for i in range(phnum):
        o = phoff + i * phentsize
        if struct.unpack_from("<I", b, o)[0] != 3:
            continue
        poff, _va, _pa, pfsz = struct.unpack_from("<QQQQ", b, o + 8)[0:4]
        cur = bytes(b[poff:poff + pfsz]).split(b"\x00")[0].decode("ascii", "replace")
        if len(want) > pfsz:
            raise RuntimeError("PT_INTERP 缓冲太小：%r > %d" % (new_interp, pfsz))
        b[poff:poff + pfsz] = want + b"\x00" * (pfsz - len(want))
        open(path, "wb").write(b)
        return cur
    raise RuntimeError("产物里没有 PT_INTERP：%s" % path)


def build_dyn_hello():
    with open(os.path.join(SRC, "dynhello.c"), "w", encoding="utf-8", newline="\n") as f:
        f.write(DYN_C)
    script = """
export MSYS2_ARG_CONV_EXCL='*'     # 别让 MSYS2 改写 /lib/... 这类绝对路径（见 patch_interp）
MUSL="third_party/musl"
INC="-nostdinc -isystem $MUSL/include -isystem $MUSL/arch/x86_64 -isystem $MUSL/arch/generic -isystem $MUSL/obj/include"
CF="$INC -O2 -fPIC -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -D_XOPEN_SOURCE=700 -std=c11"
L="build64/lxcorpus/libs"
mkdir -p build64/lxcorpus/.obj
clang --target=x86_64-linux-gnu $CF -c build64/lxcorpus/src/dynhello.c -o build64/lxcorpus/.obj/dynhello.o
for spec in "dynhello.elf:/lib/ld-musl-x86_64.so.1" "dynhello_gnuinterp.elf:/lib64/ld-linux-x86-64.so.2"; do
  out="${spec%%:*}"; interp="${spec##*:}"
  # 主程序必须钉在 4GiB（kernel/elf64.cpp:75 的 e64_lo64；base=0、没有 load bias）
  ld.lld -m elf_x86_64 -no-pie --image-base 0x100000000 -e _start -z noexecstack \\
         --allow-shlib-undefined \\
         --dynamic-linker "$interp" \\
         -o "build64/lxcorpus/$out" \\
         build64/lxcorpus/.obj/dynhello.o -L"$L" -lc
  echo "    ② $out = $(stat -c%s "build64/lxcorpus/$out") B（PT_INTERP=$interp）"
done
"""
    run_bash("03_dynhello", script)
    # 兜底：MSYS2 的路径改写可能已经把 interp 写歪了（见 patch_interp 的注释），这里校正并验一遍
    for out, interp in (("dynhello.elf", "/lib/ld-musl-x86_64.so.1"),
                        ("dynhello_gnuinterp.elf", "/lib64/ld-linux-x86-64.so.2")):
        p = os.path.join(OUT, out)
        if not os.path.exists(p):
            continue
        cur = patch_interp(p, interp)
        inf = elf_info(open(p, "rb").read())
        ok = inf["interp"] == interp and inf["dynamic"] and inf["phdr_in_first_load"]
        print("    ② %s：PT_INTERP 修正 %r -> %r（dynamic=%s needed=%s %s）"
              % (out, cur, inf["interp"], inf["dynamic"], inf["needed"],
                 "OK" if ok else "★ 自检失败"))
        if not ok:
            raise RuntimeError("%s 的 PT_INTERP/PT_DYNAMIC 自检失败" % out)


# ===========================================================================
# ③ 真发行版（glibc）语料：Debian 的 hello 包 + libc6 里的运行时
# ===========================================================================
def ar_members(deb):
    assert deb[:8] == b"!<arch>\n", "不是 ar 归档"
    off, out = 8, []
    while off + 60 <= len(deb):
        h = deb[off:off + 60]
        name = h[0:16].decode("ascii").split("/")[0].strip()
        size = int(h[48:58].decode("ascii").strip() or "0")
        out.append((name, deb[off + 60:off + 60 + size]))
        off = off + 60 + size + (size & 1)
    return out


def ar_make(members):
    """members: [(name, bytes)] -> ar 归档（debian 的 .deb 就是这种 ar）。"""
    out = b"!<arch>\n"
    for name, data in members:
        hdr = (name.ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6) +
               "100644".ljust(8) + str(len(data)).ljust(10) + "`\n")
        assert len(hdr) == 60
        out += hdr.encode("ascii") + data + (b"\n" if len(data) & 1 else b"")
    return out


def unpack_tar_member(data, pattern):
    """从 (control|data).tar.{gz,xz} 里取第一个匹配的普通文件字节。"""
    if data[:2] == b"\x1f\x8b":
        raw = gzip.decompress(data)
    elif data[:6] == b"\xfd7zXZ\x00":
        raw = lzma.decompress(data)
    else:
        raise ValueError("不认识的 tar 压缩格式")
    rx = re.compile(pattern)
    with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
        for ti in tf.getmembers():
            nm = ti.name[2:] if ti.name.startswith("./") else ti.name
            if ti.isfile() and rx.match(nm):
                return nm, tf.extractfile(ti).read()
    return None, None


def deb_repack_gz(deb, dst, strip_depends=False):
    """把 control/data.tar.xz 换成 .tar.gz（**payload 内容不变**）——/bin/vpkg 只支持 gzip。
    这是为了把"发行版的包格式（xz）"与"发行版的程序（动态 glibc）"两个缺口分开测。
    strip_depends=True 时再把 control 里的 `Depends:` 行删掉（Debian 的 hello 声明
    `Depends: libc6 (>= 2.34)`，而 VimtuOS 的 /var/lib/vpkg 里没有 libc6 这个包——
    这一条本身也是缺口，见 tests/linuxapp64_test.py 的 ④）。"""
    ms = dict(ar_members(deb))
    out = [("debian-binary", ms["debian-binary"])]
    for nm in ("control", "data"):
        for ext in ("gz", "xz"):
            k = "%s.tar.%s" % (nm, ext)
            if k not in ms:
                continue
            raw = gzip.decompress(ms[k]) if ext == "gz" else lzma.decompress(ms[k])
            if nm == "control" and strip_depends:
                raw = strip_depends_lines(raw)
            out.append(("%s.tar.gz" % nm, gzip.compress(raw, 9, mtime=0)))
            break
    blob = ar_make(out)
    with open(dst, "wb") as f:
        f.write(blob)
    return blob


def deb_repack_minimal(deb, dst, keep=("usr/bin/hello",)):
    """再进一步：只保留 data.tar 里的那一个可执行文件（+ 去掉 Depends）。
    为什么要这一层：/bin/vpkg 的 **VS_FILES_MAX = 12**（user/store/vs.h），而真实的 Debian
    hello 包 data.tar 里有 49 个条目 -> rc=8(VS_E_FORMAT, 日志原文 `deb files=49 cap=12`)。
    这一变体把"包太大/太复杂"这一层也剥掉，好让「装」与「跑」两个缺口分开测。"""
    ms = dict(ar_members(deb))
    ctl_raw = gzip.decompress(ms["control.tar.gz"]) if "control.tar.gz" in ms \
        else lzma.decompress(ms["control.tar.xz"])
    dat_raw = gzip.decompress(ms["data.tar.gz"]) if "data.tar.gz" in ms \
        else lzma.decompress(ms["data.tar.xz"])
    ctl = strip_depends_lines(ctl_raw)
    buf = io.BytesIO()
    with tarfile.open(fileobj=io.BytesIO(dat_raw)) as tf:
        picked = []
        for ti in tf.getmembers():
            nm = ti.name[2:] if ti.name.startswith("./") else ti.name
            if ti.isfile() and nm in keep:
                picked.append((ti, tf.extractfile(ti).read()))
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for ti, data in picked:
            ti.name = "./" + (ti.name[2:] if ti.name.startswith("./") else ti.name)
            ti.mode = 0o755
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    blob = ar_make([("debian-binary", ms["debian-binary"]),
                    ("control.tar.gz", gzip.compress(ctl, 9, mtime=0)),
                    ("data.tar.gz", gzip.compress(buf.getvalue(), 9, mtime=0))])
    with open(dst, "wb") as f:
        f.write(blob)
    return blob


def strip_depends_lines(tar_bytes):
    """把 control.tar 里 ./control 的 Depends:/Pre-Depends:/Recommends: 行删掉，其余原样。"""
    with tarfile.open(fileobj=io.BytesIO(tar_bytes)) as tf:
        members = []
        for ti in tf.getmembers():
            data = tf.extractfile(ti).read() if ti.isfile() else b""
            members.append((ti, data))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as tf:
        for ti, data in members:
            if ti.isfile() and ti.name.lstrip("./") == "control":
                keep = [ln for ln in data.decode("utf-8", "replace").splitlines()
                        if not ln.startswith(("Depends:", "Pre-Depends:", "Recommends:"))]
                data = ("\n".join(keep) + "\n").encode("utf-8")
            ti.size = len(data)
            tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def build_glibc_corpus(offline):
    os.makedirs(DL, exist_ok=True)
    hello_deb = os.path.join(DL, "hello_2.10-2_amd64.deb")
    if not curl(DEB_HELLO, hello_deb, offline):
        return None, "取不到 Debian 的 hello 包（离线且无缓存：%s）" % hello_deb
    # libc6：先挑目录列表里最新的 2.36（书签），列表拿不到就用 fallback
    libc6_deb = None
    cands = sorted(set(re.findall(DEB_LIBC6_RE, curl_text(DEB_LIBC6_LIST, offline)))) if not offline else []
    for cand in reversed(cands):
        p = os.path.join(DL, cand)
        if curl(DEB_LIBC6_LIST + cand, p, offline):
            libc6_deb = p
            break
    if libc6_deb is None:
        p = os.path.join(DL, os.path.basename(DEB_LIBC6_FALLBACK))
        if os.path.exists(p) or curl(DEB_LIBC6_FALLBACK, p, offline):
            libc6_deb = p
    if libc6_deb is None or not os.path.exists(libc6_deb):
        return None, "取不到 Debian 的 libc6 包（glibc 运行时：ld-linux + libc.so.6）"

    hb = open(hello_deb, "rb").read()
    hm = dict(ar_members(hb))
    member = "data.tar.xz" if "data.tar.xz" in hm else "data.tar.gz"
    _nm, gnuhello = unpack_tar_member(hm[member], r"^usr/bin/hello$")
    if gnuhello is None:
        return None, "hello 包里没有 usr/bin/hello"
    lb = open(libc6_deb, "rb").read()
    lm = dict(ar_members(lb))
    member = "data.tar.xz" if "data.tar.xz" in lm else "data.tar.gz"
    raw = lzma.decompress(lm[member]) if member.endswith("xz") else gzip.decompress(lm[member])
    # ★ 卷内路径 ← 包内路径。注意 ld-linux：Debian 里 `/lib64/ld-linux-x86-64.so.2` 是**符号链接**
    #   （-> /lib/x86_64-linux-gnu/ld-linux-x86-64.so.2），而 VimtuFS2 没有符号链接，
    #   所以这里把**真文件**放一份到 /lib64/（PT_INTERP 用的就是这条路径）。
    want = {"lib/x86_64-linux-gnu/ld-linux-x86-64.so.2": "/lib64/ld-linux-x86-64.so.2",
            "lib/x86_64-linux-gnu/libc.so.6": "/lib/x86_64-linux-gnu/libc.so.6"}
    got = {}
    with tarfile.open(fileobj=io.BytesIO(raw)) as tf:
        for ti in tf.getmembers():
            nm = ti.name[2:] if ti.name.startswith("./") else ti.name
            if ti.isfile() and nm in want:
                got[nm] = tf.extractfile(ti).read()
    for k in want:
        if k not in got:
            return None, "libc6 包里没有 %s" % k

    with open(os.path.join(OUT, "gnuhello"), "wb") as f:
        f.write(gnuhello)
    with open(os.path.join(OUT, "hello_2.10-2_amd64.deb"), "wb") as f:
        f.write(hb)
    deb_repack_gz(hb, os.path.join(OUT, "hello_2.10-2_gz.deb"))
    # 名字短一点：VimtuFS2 的名字上限 31 B（vfs64.h VFS64_NAME_MAX）——这本身也是一条真实约束
    deb_repack_gz(hb, os.path.join(OUT, "hello_2.10-2_nodeps.deb"), strip_depends=True)
    deb_repack_minimal(hb, os.path.join(OUT, "hello_2.10-2_min.deb"))
    os.makedirs(os.path.join(OUT, "rt64"), exist_ok=True)
    for k, v in got.items():
        base = os.path.basename(k)
        dst = os.path.join(OUT, "rt64", base)
        with open(dst, "wb") as f:
            f.write(v)
    print("    ③ Debian hello/usr/bin/hello = %d B（%s）" % (len(gnuhello), elf_note(elf_info(gnuhello))))
    for k, v in sorted(got.items()):
        print("    ③ %s -> %s = %d B（%s）" % (k, want[k], len(v), elf_note(elf_info(v))))
    return {"gnuhello": gnuhello, "interp": got["lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"],
            "libc6": got["lib/x86_64-linux-gnu/libc.so.6"],
            "interp_vol": want["lib/x86_64-linux-gnu/ld-linux-x86-64.so.2"],
            "libc6_vol": want["lib/x86_64-linux-gnu/libc.so.6"],
            "deb": hb, "deb_xz_name": os.path.basename(hello_deb),
            "libc6_deb": os.path.basename(libc6_deb)}, None


# ===========================================================================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--offline", action="store_true", help="只用 dl/ 缓存，绝不联网")
    ap.add_argument("--reuse-shared", action="store_true", help="不重链 musl 共享库")
    args = ap.parse_args()

    if not os.path.exists(MSYS_BASH):
        sys.stderr.write("找不到 MSYS2 bash：%s\n" % MSYS_BASH)
        return 2
    os.makedirs(OUT, exist_ok=True)
    items = []
    problems = []

    def item(iid, cls, host, vol, mode, kind, expect, note, expect_out=None, today="run"):
        items.append({"id": iid, "class": cls, "host": host, "vol": vol, "mode": mode,
                      "kind": kind, "expect": expect, "note": note,
                      "expect_out": expect_out, "today": today, "size": os.path.getsize(host),
                      "sha256": sha256_of(open(host, "rb").read())})

    for stream in (sys.stdout, sys.stderr):           # 控制台可能是 GBK：遇到解不出的字符别炸
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except Exception:                             # noqa: BLE001
            pass
    print("== lxcorpus：Linux 应用语料（build64/lxcorpus/）==")

    # ---- ① 静态 musl ----------------------------------------------------
    print("== ① 静态 musl（对照组 + 96 KiB 上限反例）==")
    try:
        build_static_hello()
    except RuntimeError as ex:
        problems.append("① 静态 musl 编译失败：%s" % ex)
    if os.path.exists(os.path.join(OUT, "stathello.elf")):
        item("static_musl", "① 静态 musl", os.path.join(OUT, "stathello.elf"),
             "/lxcorpus/stathello.elf", "0755", "exe", "run",
             "musl 静态、钉在 4GiB、无 PT_INTERP", STATIC_MARK, "run")
    if os.path.exists(os.path.join(OUT, "bighello.elf")):
        item("static_musl_big", "①c 静态 > 96 KiB", os.path.join(OUT, "bighello.elf"),
             "/lxcorpus/bighello.elf", "0755", "exe", "run",
             "故意超过内核 96 KiB 读盘上限（elf64.h:80）", "[LXBIG]", "fail")
    mh = os.path.join(ROOT, "build64", "musl_hello.elf")
    if os.path.exists(mh):
        item("static_musl_repo", "① 静态 musl（仓库既有）", mh, "/lxcorpus/musl_hello.elf",
             "0755", "exe", "run", "build64.sh 的 musl_hello.elf（既有对照，证明夹具本身没问题）",
             "[MUSL] hello from musl static ELF", "run")
    else:
        problems.append("缺 build64/musl_hello.elf（先跑 bash build64.sh）；跳过这条对照")

    # ---- ② 动态 musl ----------------------------------------------------
    print("== ② 动态 musl（libc.so = ld-musl-x86_64.so.1）==")
    sinfo, err = build_musl_shared(args.reuse_shared)
    if err:
        problems.append("② musl 共享 libc.so：%s" % err)
    else:
        item("musl_ldso", "② musl 动态链接器", os.path.join(OUT, "ld-musl-x86_64.so.1"),
             "/lib/ld-musl-x86_64.so.1", "0755", "lib", "lib",
             "musl 的 libc.so 本身就是 ld-musl-x86_64.so.1（%s）" % sinfo["note"], None, "n/a")
        item("musl_libc_so", "② musl 共享 libc", os.path.join(OUT, "libc.so"),
             "/lib/libc.so", "0755", "lib", "lib", "动态 hello 的 DT_NEEDED（%s）" % sinfo["note"],
             None, "n/a")
    try:
        build_dyn_hello()
    except RuntimeError as ex:
        problems.append("② 动态 hello 链接失败：%s" % ex)
    if os.path.exists(os.path.join(OUT, "dynhello.elf")):
        item("musl_dyn", "② 动态 musl 程序", os.path.join(OUT, "dynhello.elf"),
             "/lxcorpus/dynhello.elf", "0755", "exe", "run",
             "PT_INTERP=/lib/ld-musl-x86_64.so.1", DYN_MARK, "fail")
    if os.path.exists(os.path.join(OUT, "dynhello_gnuinterp.elf")):
        item("gnu_interp_probe", "②b 动态程序 + glibc 解释器", os.path.join(OUT, "dynhello_gnuinterp.elf"),
             "/lxcorpus/dynglibc.elf", "0755", "exe", "run",
             "同一个程序，PT_INTERP 换成 /lib64/ld-linux-x86-64.so.2：把内核带到「解释器」那一步",
             DYN_MARK, "fail")
    if os.path.exists(os.path.join(OUT, "dynhello.elf")):
        # ★ ⑤：**路径长度**这一条单独测 —— 内核 execve(59) 的路径缓冲只有 32 B
        #   （kernel/syscall64.cpp:579 LX64_PATH_MAX = 32；lx64_user_str64 要求 NUL 在 31 字节内），
        #   超过就 [SYSCALL] deny nr=59 + -EFAULT。同一个二进制换个长路径就能复现。
        item("dynhello_longpath", "⑤ 长路径（execve 路径缓冲 32 B）", os.path.join(OUT, "dynhello.elf"),
             "/lxcorpus/dynhello-with-a-long-name.elf", "0755", "exe", "run",
             "同一个二进制，只是卷内路径 38 字节 > 内核 execve 的 31 字节上限", DYN_MARK, "fail")

    # ---- ③ 真发行版（glibc）--------------------------------------------
    print("== ③ 真发行版动态 glibc（Debian bookworm 的 hello + libc6）==")
    g, err = build_glibc_corpus(args.offline)
    if err:
        problems.append("③ glibc 语料：%s" % err)
    else:
        item("glibc_bin", "③ 动态 glibc 程序", os.path.join(OUT, "gnuhello"), "/lxcorpus/gnuhello",
             "0755", "exe", "run", "Debian hello 包里的真 /usr/bin/hello（PIE + glibc）",
             "Hello, world!", "fail")
        item("glibc_ldso", "③ glibc 动态链接器", os.path.join(OUT, "rt64", "ld-linux-x86-64.so.2"),
             "/lib64/ld-linux-x86-64.so.2", "0755", "lib", "lib",
             "来自 Debian libc6（%s）" % g["libc6_deb"], None, "n/a")
        item("glibc_libc6", "③ glibc 共享 libc", os.path.join(OUT, "rt64", "libc.so.6"),
             "/lib/x86_64-linux-gnu/libc.so.6", "0755", "lib", "lib",
             "glibc 的默认搜索路径 /lib/x86_64-linux-gnu（没有 /etc/ld.so.cache）", None, "n/a")

    # ---- ④ 真 .deb（/bin/vpkg）-----------------------------------------
    print("== ④ 真 .deb（Debian 原样 xz + 宿主重压 gzip）==")
    if g:
        deb_xz = os.path.join(OUT, "hello_2.10-2_amd64.deb")
        deb_gz = os.path.join(OUT, "hello_2.10-2_gz.deb")
        deb_ok = os.path.join(OUT, "hello_2.10-2_nodeps.deb")
        deb_min = os.path.join(OUT, "hello_2.10-2_min.deb")
        items.append({"id": "deb_real_xz", "class": "④ 真 deb（原样 xz）", "host": deb_xz,
                      "vol": "/opt/vpkg/repo/" + os.path.basename(deb_xz), "mode": "0644",
                      "kind": "deb", "pkg": "hello-xz", "payload": "/usr/bin/hello",
                      "expect": "install", "today": "fail",
                      "note": "Debian bookworm 原样包：control/data.tar.xz —— /bin/vpkg 只支持 gzip",
                      "expect_out": None, "size": os.path.getsize(deb_xz),
                      "sha256": sha256_of(open(deb_xz, "rb").read())})
        items.append({"id": "deb_real_gz_dep", "class": "④ 真 deb（gzip 重压，控制文件带 Depends）",
                      "host": deb_gz, "vol": "/opt/vpkg/repo/" + os.path.basename(deb_gz),
                      "mode": "0644", "kind": "deb", "pkg": "hello-dep",
                      "payload": "/usr/bin/hello", "expect": "install", "today": "fail",
                      "note": "同一份 payload + gzip：control 里的 `Depends: libc6 (>= 2.34)` 挡住安装"
                              "（/var/lib/vpkg 里没有 libc6 这个包 —— 发行版包的依赖模型缺口）",
                      "expect_out": "Hello, world!", "size": os.path.getsize(deb_gz),
                      "sha256": sha256_of(open(deb_gz, "rb").read())})
        items.append({"id": "deb_real_gz", "class": "④ 真 deb（gzip 重压 + 去掉 Depends）",
                      "host": deb_ok, "vol": "/opt/vpkg/repo/" + os.path.basename(deb_ok),
                      "mode": "0644", "kind": "deb", "pkg": "hello",
                      "payload": "/usr/bin/hello", "expect": "install+run", "today": "run-fail",
                      "note": "宿主把 xz 重压成 gzip 并删掉 Depends 行；data.tar 仍有 49 个条目"
                              "（> vpkg 的 VS_FILES_MAX=12 -> rc=8 VS_E_FORMAT）",
                      "expect_out": "Hello, world!", "size": os.path.getsize(deb_ok),
                      "sha256": sha256_of(open(deb_ok, "rb").read()),
                      "payload_sha256": sha256_of(g["gnuhello"]) if g else None})
        items.append({"id": "deb_real_min", "class": "④ 真 deb（gzip + 去 Depends + 只留 1 个文件）",
                      "host": deb_min, "vol": "/opt/vpkg/repo/" + os.path.basename(deb_min),
                      "mode": "0644", "kind": "deb", "pkg": "hello-min",
                      "payload": "/usr/bin/hello", "expect": "install+run", "today": "run-fail",
                      "note": "同一份 Debian payload，剥到 vpkg 能吞下的最小形态（只为把「装」与「跑」分开测）",
                      "expect_out": "Hello, world!", "size": os.path.getsize(deb_min),
                      "sha256": sha256_of(open(deb_min, "rb").read()),
                      "payload_sha256": sha256_of(g["gnuhello"]) if g else None})

    man = {"generated_by": "tools/lxcorpus_build.py",
           "marks": {"static": STATIC_MARK, "dyn": DYN_MARK},
           "kernel_limits": KLIMITS,
           "items": items, "problems": problems}
    with open(os.path.join(OUT, "CORPUS.json"), "w", encoding="utf-8") as f:
        json.dump(man, f, ensure_ascii=False, indent=1)
        f.write("\n")

    print("== 语料清单（%d 项；%s）==" % (len(items), os.path.join(OUT, "CORPUS.json")))
    for it in items:
        print("   %-18s %-28s %9d B  %s  today=%s"
              % (it["id"], it["vol"], it["size"], it["class"], it["today"]))
    if problems:
        print("== 硬问题（%d 条）==" % len(problems))
        for p in problems:
            print("   [!] " + p)
        return 1
    print("== OK ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
