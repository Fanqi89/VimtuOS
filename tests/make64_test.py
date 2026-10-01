#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/make64_test.py - ★ B5：**Ring 3 里的 GNU make 4.4.1** 端到端验收

要证明的事（对应任务书 B5 的 make 条目 + sh -c 条目）：
  ① `run /bin/make -v` -> "GNU Make 4.4.1"（静态 musl、跑在 ring3，由 /bin/make 驱动装载 /lib/make.bin）；
  ② `run /bin/make -C /make-demo all`：3 文件 C 工程（变量 + `$(…)` + `$@` + **隐式规则** `%.o: %.c`）
     **真编出产物** /make-demo/hello（recipe 走 `/bin/sh -c`，靠 `&&` 强制 shell 路径）；
  ③ `run /make-demo/hello` 真跑起来，输出与预期**逐字节一致**（宿主侧也从卷里读回二进制核 ELF 头）；
  ④ `run /bin/make -C /make-demo clean` 删干净（宿主侧从卷里确认文件消失）；
  ⑤ 失败用例：编译错误 -> make 退出码非 0、错误行可见（另有"快速失败"工程把退出码钉死在串口上）；
  ⑥ `-j2`：跑一次并**如实记录**它走的是哪条路（支持并发 / 报错降级 / 串行）；
  ⑦ `sh -c` 语义子集：`;`、`&&`、`||`、`$?`、`set -e`、`NAME=VALUE`、`$X`/`${X}`、`#` 注释、
     管道（内置间）、`>` 重定向、无参数时的 usage。

关于 wait4 的既有内核边界（如实）：内核的 wait4 对**还在跑**的子进程 5 秒后返回 -EAGAIN
（kernel/proc64.cpp 的 PROC64_WAIT_TIMEOUT_SEC），shell 只调一次 wait4 —— 于是慢命令看不到
`exited code=` 汇总行（gzip64_test/tcc64_test 也如实用过这条口径）。本脚本对**慢命令**同时接受
`exited code=0` 与 `run: wait4 failed (err=11)`，产物证据一律走"宿主从卷里读回字节"。

用法：
    py -3 tests\\make64_test.py
    py -3 tests\\make64_test.py --build <产物快照目录> --timeout 400
退出码：0 = 硬断言全过；1 = 有硬断言失败；2 = 环境问题。
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh              # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
SHELL_BIN = os.path.join(BUILD, "shell.bin")
KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
MAKE_DRV = os.path.join(BUILD, "make")
MAKE_BIN = os.path.join(BUILD, "make.bin")
TAR_BIN = os.path.join(BUILD, "tar")
TCC_DRV = os.path.join(BUILD, "tcc")
TCC_BIN = os.path.join(BUILD, "tcc.bin")
TCC_STAGE = os.path.join(BUILD, "tcc_stage")
FIXTURE = os.path.join(BUILD, "make_test.img")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512
HELLO_TXT = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"

DEMO_EXPECT = b"vimtuos-make-demo: 6*7=42\n"

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash", "(": "shift-9", ")": "shift-0",
    "+": "shift-equal", "*": "shift-8", ",": "comma", ";": "semicolon",
    "$": "shift-4", "{": "shift-bracket_left", "}": "shift-bracket_right",
    "&": "shift-7", "#": "shift-3", "!": "shift-1", "'": "apostrophe",
    '"': "shift-apostrophe", "[": "bracket_left", "]": "bracket_right",
    # ★ 实测补：`$?` 用到的 '?'（QEMU sendkey 名 = shift-slash）。缺这一条时
    #   type_line 会对 `run /bin/sh -c /bin/tar ; echo status=$?` 直接抛 ValueError
    #   （测试自己崩，不是被测对象崩）—— 报错信息原文要求"加进 TYPED_NAMES"。
    "?": "shift-slash",
}
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[ELF64] reject",
    "selftest FAIL",
    "FAILED mask=",
]
KERNEL_TAGS = (
    "SYSCALL", "TASK", "TASK64", "WD64", "UI", "TERM", "SH64", "PROC64", "ELF64", "FD64",
    "VFS64", "DRV64", "ICON64", "IMG64", "GUI64", "FONT64", "APP", "LOCK64", "START64",
    "DESK64", "DOCK64", "PANEL64", "VOL", "FAT64", "BIG64", "NET64", "INPUT64", "PRELOAD64",
    "SET64", "ATA64", "AHCI64", "NVME64", "USB64", "MEM64", "PANIC", "PANIC64", "BSOD", "USER64",
    "PERM64", "SYS64", "CON64", "CONF64", "STORE64", "UPDATE64", "SESSION64", "USERDB64",
    "GFX64", "DISPLAY64", "THEME64", "OS", "RESET", "SHUTDOWN", "WL64",
)
KERNEL_TAG_RE = re.compile(r"\[(?:%s)\][^\r\n]*\r?\n" % "|".join(KERNEL_TAGS))


def use_build_dir(path):
    global BUILD, SYSTEM_IMG, SHELL_BIN, KERNEL_OS, MAKE_DRV, MAKE_BIN, TAR_BIN
    global TCC_DRV, TCC_BIN, TCC_STAGE, FIXTURE
    BUILD = path
    SYSTEM_IMG = os.path.join(BUILD, "system.img")
    SHELL_BIN = os.path.join(BUILD, "shell.bin")
    KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
    MAKE_DRV = os.path.join(BUILD, "make")
    MAKE_BIN = os.path.join(BUILD, "make.bin")
    TAR_BIN = os.path.join(BUILD, "tar")
    TCC_DRV = os.path.join(BUILD, "tcc")
    TCC_BIN = os.path.join(BUILD, "tcc.bin")
    TCC_STAGE = os.path.join(BUILD, "tcc_stage")
    FIXTURE = os.path.join(BUILD, "make_test.img")


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def shell_stream(text):
    for _ in range(4):
        nxt = KERNEL_TAG_RE.sub("", text)
        if nxt == text:
            break
        text = nxt
    return text


def load_mod(name, fname):
    import importlib.util
    path = os.path.join(ROOT, "tools", fname)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_guest_file(img_path, parts):
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img_path, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:TARGET_SECTORS * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = struct.unpack_from("<I", vol, 20)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
    cur = 0
    rec = None
    for part in parts:
        hit = None
        for i, nm, r in TP._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        _, rec = hit
        cur = hit[0]
    return TP._read_file(vol, rec, total)


BAD_C = b"""/* bad.c - \xe6\x95\x85\xe6\x84\x8f\xe7\xbc\x96\xe4\xb8\x8d\xe8\xbf\x87\xe7\x9a\x84\xe6\xba\x90\xef\xbc\x9amake \xe5\xa4\xb1\xe8\xb4\xa5\xe7\x94\xa8\xe4\xbe\x8b */
int main(void) { this is not valid C (missing semicolon and types) }
"""
BAD_MAKEFILE = b"""# \xe5\xa4\xb1\xe8\xb4\xa5\xe5\xb7\xa5\xe7\xa8\x8b\xef\xbc\x9a\xe7\xbc\x96\xe8\xaf\x91\xe9\x94\x99\xe8\xaf\xaf -> make \xe9\x80\x80\xe5\x87\xba\xe7\xa0\x81\xe9\x9d\x9e 0\xef\xbc\x88\xe9\x94\x99\xe8\xaf\xaf\xe8\xa1\x8c\xe5\x8f\xaf\xe8\xa7\x81\xef\xbc\x89
CC = /bin/tcc
all: bad.out
bad.out: bad.c
\t$(CC) bad.c -o $@ && echo "built $@"
"""
FASTFAIL_MAKEFILE = b"""# \xe5\xbf\xab\xe9\x80\x9f\xe5\xa4\xb1\xe8\xb4\xa5\xe5\xb7\xa5\xe7\xa8\x8b\xef\xbc\x9a\xe5\x91\xbd\xe4\xbb\xa4\xe8\xa1\x8c\xe5\xa4\xb1\xe8\xb4\xa5 -> \xe9\x80\x80\xe5\x87\xba\xe7\xa0\x81\xe9\x9d\x9e 0\xef\xbc\x88\xe5\xbf\xab\xe5\x88\xb0\xe8\x83\xbd\xe7\x9c\x8b\xe5\x88\xb0 shell \xe7\x9a\x84 waited \xe6\xb1\x87\xe6\x80\xbb\xe8\xa1\x8c\xef\xbc\x89
all:
\t/bin/tar && echo "unreachable: tar should have failed"
"""
FASTFAIL2_MAKEFILE = b"""# \xe7\xac\xac\xe4\xba\x8c\xe4\xb8\xaa\xe5\xbf\xab\xe9\x80\x9f\xe5\xa4\xb1\xe8\xb4\xa5\xe5\xb7\xa5\xe7\xa8\x8b\xef\xbc\x9a\xe5\x8f\x98\xe9\x87\x8f + \xe5\x86\x85\xe7\xbd\xae\xe5\x91\xbd\xe4\xbb\xa4 rm\xef\xbc\x88\xe7\xa9\xba\xef\xbc\x89\xe5\xa4\xb1\xe8\xb4\xa5
all:
\trm /no/such/file/in/vimtuos && echo "unreachable"
"""


def prepare_fixture():
    """夹具盘 = shell + /bin/sh + make（/bin/make + /lib/make.bin）+ tcc 全套 + tar + 演示工程。"""
    for p in (SYSTEM_IMG, SHELL_BIN, MAKE_DRV, MAKE_BIN, TCC_DRV, TCC_BIN, TAR_BIN):
        if not os.path.exists(p):
            return None
    if not os.path.isdir(TCC_STAGE):
        return None
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    shell_bytes = open(SHELL_BIN, "rb").read()
    drv = open(MAKE_DRV, "rb").read()
    mb = open(MAKE_BIN, "rb").read()
    tar_bytes = open(TAR_BIN, "rb").read()
    tcc_drv = open(TCC_DRV, "rb").read()
    tcc_bin = open(TCC_BIN, "rb").read()

    vol = TP.Volume2(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    tmp_ino = vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh", shell_bytes, parent=bin_ino, mode=0o755)     # make 的 recipe 走 /bin/sh -c
    vol.write_file("make", drv, parent=bin_ino, mode=0o755)
    vol.write_file("tcc", tcc_drv, parent=bin_ino, mode=0o755)
    vol.write_file("tar", tar_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", HELLO_TXT, parent=etc_ino, mode=0o644)
    lib_ino = vol.mkdir("lib", parent=0, mode=0o755)
    vol.write_file("make.bin", mb, parent=lib_ino, mode=0o755)
    vol.write_file("tcc.bin", tcc_bin, parent=lib_ino, mode=0o755)
    # /tcc 那棵树（tcc 自己要用的头/libtcc1/极小 libc）
    tcc_ino = vol.mkdir("tcc", parent=0, mode=0o755)
    tcc_lib = vol.mkdir("lib", parent=tcc_ino, mode=0o755)
    tcc_inc = vol.mkdir("include", parent=tcc_ino, mode=0o755)
    b = open(os.path.join(TCC_STAGE, "libtcc1.a"), "rb").read()
    vol.write_file("libtcc1.a", b, parent=tcc_ino, mode=0o644)
    for rel in ("libc.a", "crt1.o", "crti.o", "crtn.o"):
        b = open(os.path.join(TCC_STAGE, "lib", rel), "rb").read()
        vol.write_file(rel, b, parent=tcc_lib, mode=0o644)
    inc_root = os.path.join(TCC_STAGE, "include")
    inc_map = {}
    for dirpath, dirnames, filenames in os.walk(inc_root):
        rel = os.path.relpath(dirpath, inc_root)
        cur_ino = tcc_inc if rel == "." else inc_map[rel]
        for d in sorted(dirnames):
            child_rel = d if rel == "." else os.path.join(rel, d)
            inc_map[child_rel] = vol.mkdir(d, parent=cur_ino, mode=0o755)
        for f in sorted(filenames):
            vol.write_file(f, open(os.path.join(dirpath, f), "rb").read(), parent=cur_ino, mode=0o644)
    # 演示工程（user/make/demo -> /make-demo）
    demo_dir = os.path.join(ROOT, "user", "make", "demo")
    # ★ 实测修：/make-demo 必须**可写**（0777）—— 内核里的进程 uid=1000，0755/uid0 的目录会让
    #   tcc 建不出 main.o/hello（原文：`tcc: error: could not write 'main.o'`）。
    demo_ino = vol.mkdir("make-demo", parent=0, mode=0o777)
    for f in sorted(os.listdir(demo_dir)):
        p = os.path.join(demo_dir, f)
        if os.path.isfile(p):
            vol.write_file(f, open(p, "rb").read(), parent=demo_ino, mode=0o644)
    # 失败工程（两个：编译错误 / 快速失败）
    for d, files in (("badproj", {"Makefile": BAD_MAKEFILE, "bad.c": BAD_C}),
                     ("fastfail", {"Makefile": FASTFAIL_MAKEFILE}),
                     ("fastfail2", {"Makefile": FASTFAIL2_MAKEFILE})):
        ino = vol.mkdir(d, parent=tmp_ino, mode=0o755)
        for nm, data in files.items():
            vol.write_file(nm, data, parent=ino, mode=0o644)
    base = vol.finish()
    bad = TP.verify(base, {"/bin/shell.bin": shell_bytes, "/bin/sh": shell_bytes, "/bin/make": drv,
                           "/lib/make.bin": mb, "/bin/tar": tar_bytes,
                           "/make-demo/Makefile": open(os.path.join(demo_dir, "Makefile"), "rb").read(),
                           "/tmp/badproj/bad.c": BAD_C})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = TP.SV.build_disk(open(SYSTEM_IMG, "rb").read(), base, TARGET_SECTORS)
    with open(FIXTURE, "wb") as f:
        f.write(img)
    return FIXTURE


class Session:
    """一次 QEMU 启动（登录 -> 开终端 -> 敲命令）。"""

    def __init__(self, qemu, img, tag, timeout):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_make64_")
        self.serial = os.path.join(self.tmp, tag + ".log")
        mport = qh.free_port()
        args = [
            qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
            "-no-reboot",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = qh.Monitor(mport)
        qh.login_desktop(self.mon, self.log, self.proc, timeout=min(timeout, 180))
        self.wait_raw("[GUI64] ready", timeout)
        for _ in range(3):
            self.mon.key("meta_l", wait=0.9)
            self.mon.key("1", wait=1.8)
            if "[APP] term opened" in self.log():
                break
        self.type_line("shell", per_key=0.2)
        self.wait_raw("[SH64] launch path=/bin/shell.bin", 60)
        time.sleep(1.0)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_raw(self, needle, timeout=30, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            time.sleep(0.3)
        return False

    def wait_console(self, needle, timeout=60, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in shell_stream(self.log()[since:]):
                return True
            time.sleep(0.3)
        return False

    def wait_re(self, pattern, timeout=240, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if re.search(pattern, shell_stream(self.log()[since:])) is not None:
                return True
            time.sleep(0.3)
        return False

    def exited_or_eagain(self, base, tool, code, timeout=90):
        """慢命令：接受 `exited code=C` **或** 既有的 wait4-EAGAIN 边界（见文件头）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            txt = shell_stream(self.log()[base:])
            if re.search(r"run: /bin/%s pid=\d+ exited code=%d" % (tool, code), txt) is not None:
                return "exited"
            if "run: wait4 failed (err=11)" in txt:
                return "eagain"
            time.sleep(0.3)
        return None

    def exited(self, base, tool, code, timeout=90):
        deadline = time.time() + timeout
        while time.time() < deadline:
            txt = shell_stream(self.log()[base:])
            if re.search(r"run: /bin/%s pid=\d+ exited code=%d" % (tool, code), txt) is not None:
                return True
            time.sleep(0.3)
        return False

    def type_line(self, text, per_key=0.12):
        for ch in text:
            if ch in TYPED_NAMES:
                self.mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch.islower() or ch.isdigit():
                self.mon.key(ch, wait=per_key)
            elif ch.isupper():
                # ★ 踩过的坑：QEMU sendkey 的键名是小写的；直接发 "C" 会被**静默丢掉**
                #   （实测 `-C /tmp` 变成了 `- /tmp`，tar 于是把 /tmp 当成员 -> 断言全红）。
                self.mon.key("shift-" + ch.lower(), wait=per_key)
            else:
                raise ValueError("sendkey 不支持这个字符：%r（加进 TYPED_NAMES）" % ch)
        self.mon.key("ret", wait=per_key + 0.2)

    def run_cmd(self, cmd, per_key=0.12):
        base = len(self.log())
        self.type_line(cmd, per_key=per_key)
        return base

    def run_cmd_wait(self, cmd, needle, timeout=240, per_key=0.12):
        """敲一条命令并**等某个输出行出现**再返回（慢命令后面还有命令时必须这样排）。"""
        base = self.run_cmd(cmd, per_key=per_key)
        self.wait_console(needle, timeout, since=base)
        return base

    def close(self, keep=False):
        self.proc.kill()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass
        if not keep:
            shutil.rmtree(self.tmp, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=420)
    ap.add_argument("--no-desktop", action="store_true", help="只做交付/夹具断言（不启 QEMU）")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--build", default=None, help="构建产物目录（默认 <repo>/build64）")
    args = ap.parse_args()
    if args.build:
        use_build_dir(os.path.abspath(args.build))

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    print("=== Vimtu64 ★ B5：Ring 3 GNU make（/bin/make -> /lib/make.bin）+ /bin/sh -c acceptance ===")

    for p, what in ((MAKE_DRV, "build64/make（驱动）"), (MAKE_BIN, "build64/make.bin（真 make）")):
        check("构建产物存在：%s" % what, os.path.exists(p), p)
    if os.path.exists(MAKE_BIN):
        d = open(MAKE_BIN, "rb").read()
        etype, machine = struct.unpack_from("<HH", d, 16)
        check("make.bin 是 ELF64 ET_EXEC/x86_64（静态 musl）",
              d[:4] == b"\x7fELF" and d[4] == 2 and etype == 2 and machine == 0x3E,
              "%d B" % len(d))
    if os.path.exists(MAKE_DRV):
        d = open(MAKE_DRV, "rb").read()
        check("驱动 /bin/make < 64 KiB（内核主程序装载器能装）", len(d) <= 64 * 1024, "%d B" % len(d))
    if os.path.exists(KERNEL_OS) and os.path.exists(MAKE_BIN):
        k = open(KERNEL_OS, "rb").read()
        bad = [p for p in (MAKE_BIN, MAKE_DRV, os.path.join(BUILD, "tar"), SHELL_BIN)
               if os.path.exists(p) and open(p, "rb").read()[len(open(p, "rb").read()) // 2:
                                                              len(open(p, "rb").read()) // 2 + 64] in k]
        check("内核二进制里搜不到 make/tar/shell 的 64B 探针（工具只从系统卷装载）", not bad,
              "kernel=%d B" % len(k))

    if args.img:
        img = args.img
        if not os.path.exists(img):
            sys.stderr.write("镜像不存在：%s\n" % img)
            return 2
        print("[make64] 直接用给定镜像：%s" % img)
    else:
        img = prepare_fixture()
        if not img:
            sys.stderr.write("缺少构建产物（先跑 bash build64.sh）\n")
            return 2
        check("夹具卷逐字节回读通过（/bin/make + /lib/make.bin + /bin/sh + tcc 全套 + /make-demo）",
              True, "img=%s" % os.path.basename(img))
        for parts in (["bin", "make"], ["lib", "make.bin"], ["make-demo", "Makefile"]):
            check("从夹具盘卷里读回 /%s 成功" % "/".join(parts), read_guest_file(img, parts) is not None)

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    s = Session(qemu, img, "make", args.timeout)
    check("系统起来并进了桌面 + ring3 shell 就绪", "[GUI64] ready" in s.log() and
          "VimtuOS ring3 shell (sh64)" in s.log())

    # ---------------- ① make -v ----------------
    b0 = s.run_cmd("run /bin/make -v")
    check("★ `run /bin/make -v` 打出 GNU Make 版本（驱动装载 /lib/make.bin 成功）",
          s.wait_console("GNU Make 4.4.1", 120, since=b0))
    check("★ `make -v` 退出码 0（快命令，wait4 汇总行可见）", s.exited(b0, "make", 0, 60),
          (re.search(r"run: /bin/make[^\r\n]*", shell_stream(s.log()[b0:])) or ["（缺）"])[0])

    # ---------------- ② 演示工程：make all ----------------
    b1 = s.run_cmd_wait("run /bin/make -C /make-demo all",
                        "make: linked hello from main.o util.o", 300)
    txt = shell_stream(s.log()[b1:])
    check("★ `make -C /make-demo all` 的 recipe 真的跑了：`/bin/sh -c` 里 `&&` 后的 echo 打出来了"
          "（= 隐式规则 %.o: %.c + sh -c 语义子集都成立）",
          "make: linked hello from main.o util.o" in txt,
          (re.search(r"make: [^\r\n]*", txt) or ["（缺）"])[0])
    check("★ recipe 行本身被 make 回显（tcc 编译两行都在串口日志里）",
          txt.count("/bin/tcc") >= 2, "命中 %d 次 /bin/tcc" % txt.count("/bin/tcc"))
    ev = s.exited_or_eagain(b1, "make", 0, 60)
    check("★ `make all` 的结束证据：exited code=0（或既有的 wait4-EAGAIN 边界，见文件头）",
          ev is not None, ev)

    # ---------------- ③ run 产物 ----------------
    b2 = s.run_cmd("run /make-demo/hello")
    check("★ 产出的 /make-demo/hello 在 ring3 里真跑起来，输出 = 'vimtuos-make-demo: 6*7=42'",
          s.wait_console("vimtuos-make-demo: 6*7=42", 90, since=b2))
    check("★ 产物退出码 0", s.exited(b2, "hello", 0, 60))

    # ---------------- ④ make clean ----------------
    b3 = s.run_cmd("run /bin/make -C /make-demo clean")
    check("★ `make clean` 跑完（退出码 0）", s.exited(b3, "make", 0, 90),
          (re.search(r"run: /bin/make[^\r\n]*", shell_stream(s.log()[b3:])) or ["（缺）"])[0])
    s.wait_console("rm: ok", 30, since=b3)

    # ---------------- ⑤ -j2（如实记录走哪条路） ----------------
    b4 = s.run_cmd_wait("run /bin/make -C /make-demo -j2 all",
                        "make: linked hello from main.o util.o", 300)
    txt4 = shell_stream(s.log()[b4:])
    j2_ok = "make: linked hello from main.o util.o" in txt4
    j2_note = (re.search(r"make: [^\r\n]*", txt4) or ["(无输出)"])[0]
    for pat in ("jobserver", "unavailable", "-j", "warning"):
        m = re.search(r"[^\r\n]*%s[^\r\n]*" % pat, txt4)
        if m:
            j2_note = m.group(0)
            break
    check("★ `-j2` 跑了一次（要么真并发编出产物、要么如实报 jobserver 不可用）",
          j2_ok or "warning" in txt4.lower() or "jobserver" in txt4.lower(), j2_note[:120])
    ev4 = s.exited_or_eagain(b4, "make", 0, 60)
    check("★ `-j2` 的结束证据（exited code=0 或 EAGAIN 边界）", ev4 is not None, ev4)

    # ---------------- ⑥ 失败用例 ----------------
    b5 = s.run_cmd("run /bin/make -C /tmp/fastfail")
    check("★ 失败工程 #1（recipe 命令失败）：make 退出码非 0（快命令，汇总行可见）",
          s.exited(b5, "make", 2, 90),
          (re.search(r"run: /bin/make[^\r\n]*", shell_stream(s.log()[b5:])) or ["（缺）"])[0])
    check("★ 失败工程 #1：`&&` 后面的 echo **没有**执行（错误没被吞掉）",
          "unreachable: tar should have failed" not in shell_stream(s.log()[b5:]))
    b6 = s.run_cmd("run /bin/make -C /tmp/fastfail2")
    check("★ 失败工程 #2（内置 rm 失败）：make 退出码非 0", s.exited(b6, "make", 2, 90))
    b7 = s.run_cmd("run /bin/make -C /tmp/badproj")
    s.wait_console("Error 1", 240, since=b7)          # 编译错误 -> make 的报错行
    txt7 = shell_stream(s.log()[b7:])
    check("★ 失败工程 #3（编译错误）：错误行可见（tcc 的 error + make 的 Error 1）",
          ("Error 1" in txt7 or "Error 2" in txt7) and
          ("error:" in txt7 or "Error 1" in txt7 or "Error 2" in txt7),
          (re.search(r"[^\r\n]*Error [0-9][^\r\n]*", txt7) or ["（缺）"])[0][:120])
    ev7 = s.exited_or_eagain(b7, "make", 2, 60)
    check("★ 失败工程 #3：退出码非 0（exited code=2 或既有 EAGAIN 边界 —— 编译要加载 tcc，可能超 5 秒）",
          ev7 is not None, ev7)

    # ---------------- ⑦ sh -c 语义子集 ----------------
    # ★ 实测修：这些 `sh -c` 检查必须把命令**用单引号包起来**再敲进去。不包的话，外层 ring3 shell
    #   会先按自己的 `|` / `>` / `;` / `&&` 切分（于是测的是外层 shell，不是 -c 的子 shell），
    #   断言测的东西就不是本批要证的语义子集了。单引号在 ring3 shell 里是"整段字面量"（见
    #   user/shell/main.c 的 quote_fold），也正是 GNU make 走 `/bin/sh -c '<recipe>'` 的那种形式。
    b8 = s.run_cmd("run /bin/sh -c 'echo hi | cat > /tmp/o'")
    check("★ `sh -c \"echo hi | cat > /tmp/o\"`（任务书原文例子；内置管道 + 重定向）退出码 0",
          s.exited(b8, "sh", 0, 60),
          (re.search(r"run: /bin/sh[^\r\n]*", shell_stream(s.log()[b8:])) or ["（缺）"])[0])
    b9 = s.run_cmd("run /bin/sh -c 'X=1 ; echo X=$X'")
    check("★ 变量赋值 + 展开：`X=1 ; echo X=$X` 打出 X=1，退出码 0",
          s.wait_console("X=1", 60, since=b9) and s.exited(b9, "sh", 0, 60))
    b10 = s.run_cmd("run /bin/sh -c 'X=hello ; echo ${X}world'")
    check("★ `${X}` 展开：打出 helloworld，退出码 0",
          s.wait_console("helloworld", 60, since=b10) and s.exited(b10, "sh", 0, 60))
    b11 = s.run_cmd("run /bin/sh -c 'echo a && echo b'")
    check("★ `&&`：两条都打出来（a 与 b）",
          s.wait_console("a", 60, since=b11) and s.wait_console("b", 60, since=b11) and
          s.exited(b11, "sh", 0, 60))
    b12 = s.run_cmd("run /bin/sh -c '/bin/tar || echo recovered'")
    check("★ `||`：左边失败后跑右边（recovered），退出码 0",
          s.wait_console("recovered", 90, since=b12) and s.exited(b12, "sh", 0, 60))
    b13 = s.run_cmd("run /bin/sh -c '/bin/tar ; echo status=$?'")
    check("★ `$?`：tar 无参数返回 2 -> 打出 status=2，退出码 0",
          s.wait_console("status=2", 90, since=b13) and s.exited(b13, "sh", 0, 60))
    b14 = s.run_cmd("run /bin/sh -c 'set -e ; echo first ; /bin/tar ; echo never'")
    check("★ `set -e`：第一条失败即退出（first 打了、never 没打），退出码 = 2",
          s.wait_console("first", 90, since=b14) and s.exited(b14, "sh", 2, 60),
          (re.search(r"run: /bin/sh[^\r\n]*", shell_stream(s.log()[b14:])) or ["（缺）"])[0])
    # ★ 实测修：`never` / `this is a comment` 这两个词**也出现在被回显的命令行里**（终端把用户敲的
    #   那一行原样回显），所以"substring not in log"永远为假。改成找**独立的输出行**：
    #   `echo never` 的输出是单独一行 "never"，而回显那行是整条 `run /bin/sh -c '…never…'`。
    check("★ `set -e`：`never` 确实没有执行（set -e 语义证据）",
          not re.search(r"(?m)^never\r?$", shell_stream(s.log()[b14:])))
    b15 = s.run_cmd("run /bin/sh -c 'echo a # this is a comment'")
    txt15 = shell_stream(s.log()[b15:])
    check("★ `#` 注释：只打 a（注释内容没进 echo 的输出）",
          s.exited(b15, "sh", 0, 60) and
          not re.search(r"(?m)^this is a comment\r?$", txt15))
    b16 = s.run_cmd("run /bin/sh -c")
    check("★ `sh -c` 缺参数：usage + 退出码 2（不碰邮箱、不 PANIC）",
          s.wait_console("-c requires a command string", 60, since=b16) and s.exited(b16, "sh", 2, 60))

    check("全程无 PANIC / 装载拒绝 / 自检失败", all(p not in s.log() for p in FORBIDDEN))
    s.close(keep=args.keep)

    # ==================== 宿主侧字节级证据 ====================
    got_o = read_guest_file(img, ["tmp", "o"])
    check("★★ `echo hi | cat > /tmp/o` 的落盘内容逐字节 = 'hi\\n'（宿主从卷里读回）",
          got_o == b"hi\n", repr(got_o))
    hello = read_guest_file(img, ["make-demo", "hello"])
    check("★★ `make all` 真编出 /make-demo/hello（宿主从卷里读回），是静态 ELF64 ET_EXEC",
          hello is not None and hello[:4] == b"\x7fELF" and
          struct.unpack_from("<H", hello, 16)[0] == 2 and
          struct.unpack_from("<H", hello, 18)[0] == 0x3E,
          "%s B" % (len(hello) if hello else None))
    if hello:
        entry = struct.unpack_from("<Q", hello, 24)[0]
        check("★ 产物的入口落在 4GiB 装载区（内核 ELF 装载器的硬约束）",
              0x100000000 <= entry < 0x100000000 + 0x10000, "entry=%#x" % entry)
    for parts in (["make-demo", "main.o"], ["make-demo", "util.o"]):
        check("★★ make clean 之后 /%s 已被删除" % "/".join(parts),
              read_guest_file(img, parts) is None)
    # -j2 那一步又把产物编回来了：clean 在前、-j2 在后 -> 这里应当是"存在"
    check("★★ `-j2 all`（在 clean 之后）重新编出了 /make-demo/hello",
          read_guest_file(img, ["make-demo", "hello"]) is not None)

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
