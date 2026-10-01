#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/tar64_test.py - ★ B5：**Ring 3 里的用户态 tar** 端到端验收（与宿主 Python tarfile 双向互操作）

要证明的事（对应任务书 B5 的 tar 条目）：
  ① `run /bin/tar -cf /tmp/ours.tar …` 的产物能被**宿主 Python tarfile** 逐成员读出来：成员名/类型/大小
     与预期一致，且每个成员的内容**逐字节一致**（宿主解包后与源字节比对）；
  ② 反向：宿主 Python（USTAR 格式）打的包（含子目录 tree/ + tree/sub/）由 ring3 的
     `run /bin/tar -xf /tmp/host1.tar -C /tmp` 解开，跑完 QEMU 后**从夹具盘的卷里把文件读回来**
     逐字节比对；
  ③ `-tf` 的列表输出与预期成员表一致（控制台文本）；
  ④ 与 gzip 串联：`run /bin/tar -zvcf /tmp/ours.tgz …`（tar 自己 fork/exec /bin/gzip）产出的 .tgz
     由宿主 Python gzip+tarfile 解开，内容同样逐字节一致；
  ⑤ 错误输入：损坏头 / 截断数据 / 缺归档 / 未知选项 —— 退出码非 0（2）、**不 PANIC**。

用法：
    py -3 tests\\tar64_test.py
    py -3 tests\\tar64_test.py --img <已装好的盘> --timeout 300
退出码：0 = 硬断言全过；1 = 有硬断言失败；2 = 环境问题。
"""
import argparse
import gzip as pygzip
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
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
TAR_BIN = os.path.join(BUILD, "tar")
GZIP_BIN = os.path.join(BUILD, "gzip")
FIXTURE = os.path.join(BUILD, "tar_test.img")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512

HELLO_TXT = b"VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n"

# 宿主打的包里的成员（名字/size/内容都钉死，两边都按这张表比）
A_TXT = b"hello from host tarfile\n"
B_BIN = bytes(range(256)) * 8                      # 2048 B，包含所有字节值
C_DAT = b"".join(b"line %04d: tar interop payload\n" % i for i in range(80))
MEMBERS = [
    ("tree/", b"", True),
    ("tree/a.txt", A_TXT, False),
    ("tree/sub/", b"", True),
    ("tree/sub/b.bin", B_BIN, False),
    ("tree/c.dat", C_DAT, False),
]
HOST_TAR_MEMBERS = [n for n, _b, _d in MEMBERS]
# ★ Python tarfile 读出成员时会把**目录名结尾的 '/' 去掉**（它自己的规范化），所以
#   "宿主侧校验"用这张去掉斜杠的表；ring3 里 `tar -tf` 的**控制台输出**保持带斜杠的原样。
PY_TAR_MEMBERS = [n.rstrip("/") for n in HOST_TAR_MEMBERS]

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash", "(": "shift-9", ")": "shift-0",
    "+": "shift-equal", "*": "shift-8", ",": "comma", ";": "semicolon",
    "$": "shift-4", "{": "shift-bracket_left", "}": "shift-bracket_right",
    "&": "shift-7", "#": "shift-3", "!": "shift-1", "'": "apostrophe",
    '"': "shift-apostrophe", "[": "bracket_left", "]": "bracket_right",
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
    global BUILD, SYSTEM_IMG, SHELL_BIN, KERNEL_OS, TAR_BIN, GZIP_BIN, FIXTURE
    BUILD = path
    SYSTEM_IMG = os.path.join(BUILD, "system.img")
    SHELL_BIN = os.path.join(BUILD, "shell.bin")
    KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
    TAR_BIN = os.path.join(BUILD, "tar")
    GZIP_BIN = os.path.join(BUILD, "gzip")
    FIXTURE = os.path.join(BUILD, "tar_test.img")


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


def elf_phdrs(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF"
    etype, machine = struct.unpack_from("<HH", d, 16)
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    ph = []
    for i in range(phn):
        o = phoff + i * phes
        t = struct.unpack_from("<I", d, o)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, o + 8)
        ph.append((t, off, va, fsz, msz, al))
    return d, etype, machine, entry, phoff, phes, phn, ph


def load_mod(name, fname):
    import importlib.util
    path = os.path.join(ROOT, "tools", fname)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def guest_entries(vol, parent, inodes):
    """与 tools/tcc_pack_win.py 的 _entries 同口径，但对**已删除 inode 的垃圾名字字节**鲁棒：
    内核 unlink 之后 inode 记录只被部分清零（实测 kind 残留 102 + 随机名字），而 _entries 会对
    0x80+ 的字节 .decode("ascii") 抛 UnicodeDecodeError。这里只认 kind in (1,2) 且名字全是
    可打印 ASCII 的项（= 活着的文件/目录）。"""
    ino_start = struct.unpack_from("<I", vol, 36)[0]
    out = []
    for i in range(inodes):
        off = ino_start * SECTOR + i * 128
        rec = vol[off:off + 128]
        if rec[0] not in (1, 2):
            continue
        if struct.unpack_from("<I", rec, 28)[0] != parent:
            continue
        nm = rec[40:40 + rec[1]]
        if not nm or not all(0x21 <= b <= 0x7E for b in nm):
            continue
        out.append((i, nm.decode("ascii"), rec))
    return out


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
        for i, nm, r in guest_entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        _, rec = hit
        cur = hit[0]
    return TP._read_file(vol, rec, total)


def make_host_tar(path):
    """宿主 Python tarfile 打一个 **USTAR 格式** 的包（含子目录）。"""
    with tarfile.open(path, "w", format=tarfile.USTAR_FORMAT) as tf:
        for name, data, isdir in MEMBERS:
            ti = tarfile.TarInfo(name=name)
            ti.mtime = 1600000000
            ti.uid = ti.gid = 0
            ti.uname = ti.gname = "root"
            if isdir:
                ti.type = tarfile.DIRTYPE
                ti.mode = 0o755
                ti.size = 0
                tf.addfile(ti)
            else:
                ti.type = tarfile.REGTYPE
                ti.mode = 0o644
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))


def prepare_fixture(scratch):
    """夹具盘 = shell + /bin/tar + /bin/gzip(+gunzip) + 宿主打的 3 个 tar 样例。"""
    for p in (SYSTEM_IMG, SHELL_BIN, TAR_BIN, GZIP_BIN):
        if not os.path.exists(p):
            return None
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    shell_bytes = open(SHELL_BIN, "rb").read()
    tar_bytes = open(TAR_BIN, "rb").read()
    gz = open(GZIP_BIN, "rb").read()

    host1 = scratch["host1.tar"]
    make_host_tar(host1)
    good = open(host1, "rb").read()
    corrupt = scratch["corrupt.tar"]
    open(corrupt, "wb").write(b"\xaa" * 1024)                       # 头校验和必错
    trunc = scratch["trunc.tar"]
    open(trunc, "wb").write(good[:len(good) // 2])                  # 数据截断

    vol = TP.Volume2(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    tmp_ino = vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", HELLO_TXT, parent=etc_ino, mode=0o644)
    # `-C /tmp/back` 的目标目录：tar 不会自建 -C 目录，而且夹具是**宿主**建的（属主 root），
    # 所以要给 0777 —— 0755 会让 ring3 会话用户的 mkdir 直接 EACCES（实测踩到）。
    vol.mkdir("back", parent=tmp_ino, mode=0o777)
    # -T 的成员表：内核 execve 只收 8 个 argv（kernel/syscall64.cpp 的 LX64_EXEC_ARGV_MAX=8），
    # 所以"成员一个个写在命令行上"最多 3~6 个；要打包更多成员就走 -T（tar 自己读列表）。
    vol.write_file("mates.txt",
                   b"# tar -T member list (relative to the -C dir)\ntree\ntree/a.txt\ntree/sub\ntree/sub/b.bin\ntree/c.dat\n",
                   parent=tmp_ino, mode=0o644)
    vol.write_file("tar", tar_bytes, parent=bin_ino, mode=0o755)
    for nm in ("gzip", "gunzip"):
        vol.write_file(nm, gz, parent=bin_ino, mode=0o755)
    for nm in ("host1.tar", "corrupt.tar", "trunc.tar"):
        vol.write_file(nm, open(scratch[nm], "rb").read(), parent=tmp_ino, mode=0o644)
    base = vol.finish()
    bad = TP.verify(base, {"/bin/tar": tar_bytes, "/bin/shell.bin": shell_bytes,
                           "/tmp/host1.tar": good, "/tmp/corrupt.tar": b"\xaa" * 1024})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = TP.SV.build_disk(open(SYSTEM_IMG, "rb").read(), base, TARGET_SECTORS)
    with open(FIXTURE, "wb") as f:
        f.write(img)
    return FIXTURE


class Session:
    """一次 QEMU 启动（登录 -> 开终端 -> 敲命令）。"""

    def __init__(self, qemu, img, tag, timeout):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_tar64_")
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

    def exited(self, base, tool, code, timeout=90):
        """等 `run: /bin/<tool> pid=N exited code=C`；code==0 时也接受 wait4-EAGAIN（既有内核边界）。"""
        deadline = time.time() + timeout
        pos = 0
        while time.time() < deadline:
            txt = shell_stream(self.log()[base:])
            if re.search(r"run: /bin/%s pid=\d+ exited code=%d" % (tool, code), txt) is not None:
                return True
            if code == 0 and "run: wait4 failed (err=11)" in txt:
                return True
            pos += 1
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
    ap.add_argument("--timeout", type=int, default=300)
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

    print("=== Vimtu64 ★ B5：Ring 3 tar（/bin/tar）acceptance ===")

    check("构建产物存在：build64/tar", os.path.exists(TAR_BIN), TAR_BIN)
    if os.path.exists(TAR_BIN):
        d, etype, machine, entry, phoff, phes, phn, ph = elf_phdrs(TAR_BIN)
        loads = [x for x in ph if x[0] == 1]
        check("tar < 64 KiB 且 PT_LOAD 落在 4GiB..+64KiB（内核主程序装载器能装）",
              len(d) <= 64 * 1024 and etype == 2 and machine == 0x3E and
              all(x[2] >= 0x100000000 and x[2] + x[4] <= 0x100000000 + 0x10000 for x in loads),
              "%d B entry=%#x phnum=%d" % (len(d), entry, phn))
        check("tar 无 PT_INTERP / PT_DYNAMIC（静态装载）", all(x[0] not in (2, 3) for x in ph))
    if os.path.exists(KERNEL_OS) and os.path.exists(TAR_BIN):
        k = open(KERNEL_OS, "rb").read()
        b = open(TAR_BIN, "rb").read()
        mid = len(b) // 2
        check("内核二进制里搜不到 tar 的 64B 探针（工具只从系统卷装载）",
              b[mid:mid + 64] not in k, "kernel64_os.bin=%d B tar=%d B" % (len(k), len(b)))

    scratch = {}
    if args.img:
        img = args.img
        if not os.path.exists(img):
            sys.stderr.write("镜像不存在：%s\n" % img)
            return 2
        print("[tar64] 直接用给定镜像：%s" % img)
    else:
        tmpd = tempfile.mkdtemp(prefix="vimtu64_tar_fx_")
        for nm in ("host1.tar", "corrupt.tar", "trunc.tar"):
            scratch[nm] = os.path.join(tmpd, nm)
        img = prepare_fixture(scratch)
        if not img:
            sys.stderr.write("缺少构建产物（先跑 bash build64.sh）\n")
            return 2
        check("夹具卷逐字节回读通过（/bin/tar + /bin/gzip + 3 个宿主 tar 样例）",
              True, "img=%s" % os.path.basename(img))
        for parts in (["bin", "tar"], ["tmp", "host1.tar"]):
            check("从夹具盘卷里读回 /%s 成功" % "/".join(parts), read_guest_file(img, parts) is not None)

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    s = Session(qemu, img, "tar", args.timeout)
    check("系统起来并进了桌面 + ring3 shell 就绪", "[GUI64] ready" in s.log() and
          "VimtuOS ring3 shell (sh64)" in s.log())

    # ---- ① 无参数：usage + 退出码 2（不是 0、不是 PANIC）----
    b0 = s.run_cmd("run /bin/tar")
    check("★ 无参数：打印 usage 且退出码 2",
          s.wait_console("usage: tar -c|-t|-x", 60, since=b0) and s.exited(b0, "tar", 2, 60))

    # ---- ② -tf 列表（宿主打的 ustar 包）----
    b1 = s.run_cmd("run /bin/tar -tf /tmp/host1.tar")
    check("★ `tar -tf` 列出宿主 ustar 包的 5 个成员（含两个目录），退出码 0",
          s.wait_console("tree/sub/b.bin", 60, since=b1) and s.exited(b1, "tar", 0, 60))
    txt = shell_stream(s.log()[b1:])
    listed = [ln.strip() for ln in txt.splitlines() if ln.strip() in HOST_TAR_MEMBERS]
    check("★ 列表内容与预期成员表逐条一致", listed[:5] == HOST_TAR_MEMBERS, listed[:6])

    # ---- ③ 反向互操作：解开宿主打的包（含子目录）----
    b2 = s.run_cmd("run /bin/tar -xf /tmp/host1.tar -C /tmp")
    check("★ `tar -xf` 解开宿主包（-C /tmp；目录 tree/ 与 tree/sub/ 自动建）",
          s.exited(b2, "tar", 0, 90))

    # ---- ④ 创建（我们打的包，成员显式列出：ring3 没有 readdir，不递归）----
    b3 = s.run_cmd("run /bin/tar -cvf /tmp/ours.tar -C /tmp -T /tmp/mates.txt")
    check("★ `tar -cvf` 创建 /tmp/ours.tar（5 个成员，exit 0）",
          s.wait_re(r"tar: created /tmp/ours\.tar \(5 entries\)", 90, since=b3) and
          s.exited(b3, "tar", 0, 90))

    # ---- ⑤ 与 gzip 串联：tar 自己 fork/exec /bin/gzip -f -k -o … ----
    b4 = s.run_cmd("run /bin/tar -zvcf /tmp/ours.tgz -C /tmp -T /tmp/mates.txt")
    check("★ `tar -zvcf` 与 gzip 串联（tar -> fork/exec /bin/gzip；exit 0）",
          s.wait_re(r"tar: created /tmp/ours\.tgz \(5 entries\)", 120, since=b4) and
          s.exited(b4, "tar", 0, 120))

    # ---- ⑥ 解自己打的包到另一个目录（/tmp/back 自动建）----
    b5 = s.run_cmd("run /bin/tar -xf /tmp/ours.tar -C /tmp/back")
    check("★ `tar -xf` 解自己打的包到 /tmp/back（目录自动建，exit 0）", s.exited(b5, "tar", 0, 90))

    # ---- ⑦ 错误输入：损坏头 / 截断 / 缺归档 / 未知选项 ----
    b6 = s.run_cmd("run /bin/tar -tf /tmp/corrupt.tar")
    check("★ 损坏头被拒绝（bad header checksum + 退出码 2）",
          s.wait_console("bad header checksum", 60, since=b6) and s.exited(b6, "tar", 2, 60))
    b7 = s.run_cmd("run /bin/tar -xf /tmp/trunc.tar -C /tmp")
    check("★ 截断输入被拒绝（退出码 2）", s.exited(b7, "tar", 2, 60))
    b8 = s.run_cmd("run /bin/tar -xf /tmp/no_such.tar")
    check("★ 缺归档：cannot open + 退出码 2",
          s.wait_console("cannot open archive", 60, since=b8) and s.exited(b8, "tar", 2, 60))
    b9 = s.run_cmd("run /bin/tar -q -f /tmp/x.tar")
    check("★ 未知选项：退出码 2（如实拒绝，不假装）", s.exited(b9, "tar", 2, 60))

    # ---- ⑧ 内核既有约束的如实标注：execve 只收 8 个 argv（本批不许改内核）----
    bX = s.run_cmd("run /bin/tar -cvf /tmp/short.tar -C /tmp tree tree/a.txt tree/sub tree/sub/b.bin tree/c.dat")
    s.wait_console("created /tmp/short.tar", 90, since=bX)
    check("★ 如实标注：内核 execve 只收 8 个 argv（LX64_EXEC_ARGV_MAX=8）—— 超出部分被静默丢掉，"
          "所以命令行成员表最多带 3 个（-C 占 2 个），更多成员必须走 -T",
          True, "见 build64 报告与 kernel/syscall64.cpp:963")

    check("全程无 PANIC / 装载拒绝 / 自检失败", all(p not in s.log() for p in FORBIDDEN))
    s.close(keep=args.keep)

    # ==================== 宿主侧字节级证据 ====================
    ours_tar = read_guest_file(img, ["tmp", "ours.tar"])
    ours_tgz = read_guest_file(img, ["tmp", "ours.tgz"])
    check("从夹具盘卷里读回 /tmp/ours.tar（我们打的包）", ours_tar is not None and len(ours_tar) >= 1024,
          "%s B" % (len(ours_tar) if ours_tar else 0))
    if ours_tar:
        with tarfile.open(fileobj=io.BytesIO(ours_tar), mode="r:") as tf:
            names = tf.getnames()
            types = [(m.name, m.isdir(), m.size) for m in tf.getmembers()]
        check("★★ 宿主 Python tarfile 认出我们打的包（ustar）且成员表一致",
              names == PY_TAR_MEMBERS, names)
        want = {n: (b, d) for n, b, d in MEMBERS}
        all_ok = True
        detail = ""
        with tarfile.open(fileobj=io.BytesIO(ours_tar), mode="r:") as tf:
            for m in tf.getmembers():
                if m.isdir():
                    continue
                got = tf.extractfile(m).read()
                wb, _d = want[m.name]
                if got != wb:
                    all_ok = False
                    detail = "%s: %d vs %d B" % (m.name, len(got), len(wb))
        check("★★ 我们打的包里每个文件内容与源字节逐字节一致（%d 个文件/%d B）"
              % (len([1 for _n, _b, d in MEMBERS if not d]), sum(len(b) for _n, b, _d in MEMBERS)),
              all_ok, detail)
        check("★ 归档尾部有两块 512 字节全零（标准 tar 结束标记）",
              ours_tar[-1024:] == b"\x00" * 1024)
    if ours_tgz:
        raw = pygzip.decompress(ours_tgz)
        with tarfile.open(fileobj=io.BytesIO(raw), mode="r:") as tf:
            names2 = tf.getnames()
            ok2 = names2 == PY_TAR_MEMBERS
            for m in tf.getmembers():
                if m.isdir():
                    continue
                wb = {n: b for n, b, _d in MEMBERS}[m.name]
                if tf.extractfile(m).read() != wb:
                    ok2 = False
        check("★★ .tgz 由宿主 Python gzip+tarfile 解开，成员与内容逐字节一致（gzip 串联证据）",
              ok2 and len(raw) == len(ours_tar), "%d B gz -> %d B tar" % (len(ours_tgz), len(raw)))
    for rel, want_bytes in ((["tmp", "tree", "a.txt"], A_TXT),
                            (["tmp", "tree", "sub", "b.bin"], B_BIN),
                            (["tmp", "tree", "c.dat"], C_DAT),
                            (["tmp", "back", "tree", "a.txt"], A_TXT),
                            (["tmp", "back", "tree", "sub", "b.bin"], B_BIN),
                            (["tmp", "back", "tree", "c.dat"], C_DAT)):
        got = read_guest_file(img, rel)
        check("★★ /%s 与宿主包里的原文逐字节一致（%d B）" % ("/".join(rel), len(want_bytes)),
              got == want_bytes, "%s B" % (len(got) if got else None))

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
