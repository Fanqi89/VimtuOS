#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/pipe64_test.py - ★ 本批：**pipe 默认阻塞**（POSIX 语义）+ fcntl(F_SETFL) 切 O_NONBLOCK 验收

要证明的事（每条都有串口/宿主侧字节证据）：
  ① busybox 组合链（真 pipe + fork + dup2，busybox ash 驱动；输出**落盘**再宿主侧逐字节核对，
     避免串口按 64B 分片 + 内核日志交错导致的误判）：
       echo hi | cat > out_echo.txt              -> "hi\\n"
       ls -l /tcc/demo | grep hello | wc -l      -> "2\\n"（★ 真值：名字含 hello 的有 hello.c 与 hello.lua）
       cat lines.txt | sed s/beta/BETA/ > b      -> BETA 已替换、不残留 beta（5 行）
       cat big64.txt | wc -c                     -> "300\\n"（300B 过 64B 环：分次写完不丢字节 + EOF）
  ② 内核侧 fdtest（与 syscall 72 同一实现 fd64_fcntl64）：
       F_SETFL O_NONBLOCK 生效（setfl=1 / getfl_nonblock=1 / fdcloexec=1）；
       非阻塞短写=64/100、空读=-EAGAIN（旧语义保留）；F_SETFD/F_GETFD 的 FD_CLOEXEC 往返。
  ③ 阻塞等待打点 [FD64] pipe wait ...（pid 真值、why=empty|full、ticks、writers/readers），
     且**没有**撞 30s 有界上限；正常管线里不出现 busybox 的 EAGAIN/EPIPE 报错文本。
  ④ 结构性前提：pipe 池按**空闲槽**分配（原实现无条件复用 0 号槽 —— 实测第二条管道会把第一条
     清零并串数据，见 kernel/fd64.cpp 的 fd64_pipe_alloc64 注释）；本测试的多条管道同时存活
     就是这条修正的回归。

夹具：build64/sysdisk.img 副本 + /tmp/bb/pipe2.sh + /tmp/bb/big64.txt；并把卷根改成 0777
（登录会话是 uid=1000，`fdtest` 要在根目录建 /fdtest.a —— 只影响这份测试副本）。

用法（必须 Windows 原生 Python）：py -3 tests\\pipe64_test.py [--timeout 180] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import importlib.util
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402

BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")          # busybox 完整演示盘（夹具底座）
SYSTEM_IMG = os.path.join(BUILD, "system.img")     # 引导 + 内核（拼盘用）
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash", "(": "shift-9", ")": "shift-0",
}

BIG64 = (b"0123456789" * 30)                      # 300 B（> 64 B 环容量：必须分次写完）
PIPE2 = b"""# /tmp/bb/pipe2.sh - tests/pipe64_test.py fixture (blocking pipe chains)
echo '@@P_ECHO'
echo hi | cat > /tmp/bb/out_echo.txt
echo '@@P_THREE'
ls -l /tcc/demo | grep hello | wc -l > /tmp/bb/out_three.txt
echo '@@P_SED'
cat /tmp/bb/lines.txt | sed s/beta/BETA/ > /tmp/bb/pipe2out.txt
echo '@@P_FULL'
cat /tmp/bb/big64.txt | wc -c > /tmp/bb/out_full.txt
echo '@@P_DONE'
"""

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[FD64] demo FAIL",
    "[FD64] demo skipped",
]


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def q(p):
    return p.replace("\\", "/")


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def build_fixture(tmp):
    """sysdisk.img 副本 + /tmp/bb/pipe2.sh + /tmp/bb/big64.txt；卷根改 0777（fdtest 要在根建文件）。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(DISK, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:(PART_MAIN_LBA + TARGET_SECTORS) * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = int.from_bytes(vol[20:24], "little")
    ino_start = int.from_bytes(vol[36:40], "little")
    ed = TP.VolumeEdit(total)
    ed.load(vol)
    # 卷根（inode 0）-> 0777：登录会话 = uid 1000（否则 [PERM64] deny op=create path=/fdtest.a），
    # fdtest 的演示文件建在根目录。只改这份测试副本；v4 inode 的 CRC32 同步重算（否则挂载会拒绝）。
    off = ino_start * SECTOR + 79
    struct.pack_into("<H", ed.buf, off, 0o40777)
    struct.pack_into("<I", ed.buf, ino_start * SECTOR + 124,
                     zlib.crc32(bytes(ed.buf[ino_start * SECTOR:ino_start * SECTOR + 124])) & 0xFFFFFFFF)
    ed.put("/tmp/bb/pipe2.sh", PIPE2, mode=0o666)
    ed.put("/tmp/bb/big64.txt", BIG64, mode=0o666)
    out = ed.finish()
    system = open(SYSTEM_IMG, "rb").read()
    disk = TP.SV.build_disk(system, out, TARGET_SECTORS)
    path = os.path.join(tmp, "pipe64.img")
    open(path, "wb").write(disk)
    return path


def guest_file(img, parts):
    """宿主侧独立解析夹具卷里的文件（QEMU 停掉之后读，验证落盘产物）。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:(PART_MAIN_LBA + TARGET_SECTORS) * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = int.from_bytes(vol[20:24], "little")
    inodes = int.from_bytes(vol[40:44], "little")
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
        cur, rec = hit
    return TP._read_file(vol, rec, total)


def wait_guest_file(img, parts, want, timeout=240, step=2.0):
    """轮询宿主侧解析（QEMU 仍在跑；内核写文件是同步落盘的）：内容变成 want 或超时。

    为什么必须轮询：ash 的 waitforjob 走内核 wait4，而内核 wait4 是**有界 5 秒**（既有边界，
    见 kernel/proc64.cpp），本内核装载一个 busybox applet 要数秒 —— ash 会提前返回下一条命令，
    旧管线的子进程还在跑。所以 @@P_DONE 只代表"ash 把命令都发出去了"，产物要等子进程收尾。"""
    deadline = time.time() + timeout
    got = None
    while time.time() < deadline:
        got = guest_file(img, parts)
        if got == want:
            return got
        time.sleep(step)
    return got

class Session:
    def __init__(self, qemu, img, tag, timeout):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_pipe64_")
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
        self.wait("[GUI64] ready", timeout)
        self.open_terminal()

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(self, needle, timeout=30, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            time.sleep(0.3)
        return False

    def type_line(self, text, per_key=0.08):
        for ch in text:
            if ch in TYPED_NAMES:
                self.mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch.isalnum():
                self.mon.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey 不支持这个字符：%r" % ch)
        self.mon.key("ret", wait=per_key + 0.2)

    def open_terminal(self):
        for _ in range(6):
            if "[APP] term opened" in self.log():
                break
            self.mon.key("ret", wait=1.6)
            self.mon.key("ret", wait=1.6)
            time.sleep(1.0)
            self.mon.key("meta_l", wait=1.8)
            self.mon.key("1", wait=2.4)
        self.wait("[APP] term opened", 20)

    def close(self, keep=False):
        self.proc.kill()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass
        if keep:
            print("[pipe64] 串口日志：%s" % self.serial)


def marked(text, name):
    k = text.find("@@%s" % name)
    if k < 0:
        return None
    k = text.find("\n", k)
    n = text.find("@@", k + 1)
    return text[k + 1:n if n > 0 else len(text)]


def payload_lines(section):
    out = []
    for ln in (section or "").splitlines():
        s = ln.rstrip("\r")
        if s.strip() and not s.lstrip().startswith("["):
            out.append(s)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(DISK) or not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s / %s（先跑 bash build64.sh）\n" % (DISK, SYSTEM_IMG))
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_pipe64_fix_")
    img = build_fixture(tmp)
    if not img:
        sys.stderr.write("造夹具盘失败（sysdisk.img 里没有 VimtuFS2 卷）\n")
        return 2

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 pipe64 acceptance（默认阻塞 + F_SETFL O_NONBLOCK）===")
    sess = Session(qemu, img, "pipe64", args.timeout)
    final = ""
    try:
        # ---- 内核侧 fdtest：与 syscall 72 同一个 fd64_fcntl64 ----
        base = len(sess.log())
        sess.type_line("fdtest")
        sess.wait("[TERM] cmd fdtest", 90)
        time.sleep(1.0)
        t = sess.log()[base:]
        check("fdtest 跑完（[TERM] cmd fdtest ok）", "[TERM] cmd fdtest ok" in t,
              "（fail 时见 [PERM64]/[FD64] open FAILED 行）")
        check("F_SETFL O_NONBLOCK 生效：setfl=1 / getfl_nonblock=1 / fdcloexec=1",
              re.search(r"\[FD64\] demo pipe setfl=1 getfl_nonblock=1 fdcloexec=1", t) is not None, "")
        check("非阻塞短写/空读（旧语义保留）：shortwrite=64/100 + empty=1011",
              "[FD64] demo pipe shortwrite=64/100 (ring=64)" in t and
              "[FD64] demo pipe empty=1011 (1000+EAGAIN) eof=0" in t, "")
        check("fcntl 打点：F_SETFL O_NONBLOCK ok=1 / 未知 flag ok=0 / F_SETFD FD_CLOEXEC ok=1",
              re.search(r"\[FD64\] fcntl fd=\d+ setfl=0x0*800 ok=1", t) is not None and
              re.search(r"\[FD64\] fcntl fd=\d+ setfl=0x0*100000 ok=0", t) is not None and
              re.search(r"\[FD64\] fcntl fd=\d+ setfl=0x0*1 ok=1", t) is not None, "")
        check("fdtest 全过：[FD64] demo PASS ok=1", "[FD64] demo PASS ok=1" in t)

        # ---- ring3：busybox ash 组合链 ----
        sess.type_line("shell")
        sess.wait("[SH64] launch path=/bin/shell.bin", 90)
        time.sleep(0.8)
        base = len(sess.log())
        sess.type_line("run /bin/busybox sh /tmp/bb/pipe2.sh", per_key=0.06)
        ran = sess.wait("@@P_DONE", timeout=args.timeout, since=base)
        time.sleep(1.5)
        t = sess.log()[base:]
        final = sess.log()
        check("busybox 组合链脚本跑到 @@P_DONE", ran, "")

        pl = payload_lines(marked(t, "P_THREE"))
        pl2 = payload_lines(marked(t, "P_ECHO"))
        print("  [info] 串口有序片段（会被内核日志按 64B 分片打断，仅作参考）：P_THREE=%r P_ECHO=%r"
              % (pl[:3], pl2[:3]))

        # ---- 宿主侧逐字节（落盘产物；轮询到子进程收尾 —— ash 的 wait4 有界 5 秒会提前返回）----
        check_t = 240
        echo_out = wait_guest_file(img, ["tmp", "bb", "out_echo.txt"], b"hi\n", check_t)
        check("宿主侧 /tmp/bb/out_echo.txt == b'hi\\n'（echo hi | cat）",
              echo_out == b"hi\n", repr(echo_out))
        three_out = wait_guest_file(img, ["tmp", "bb", "out_three.txt"], b"2\n", check_t)
        check("宿主侧 /tmp/bb/out_three.txt == b'2\\n'（三方管道 ls -l | grep hello | wc -l；真值 2）",
              three_out == b"2\n", repr(three_out))
        sed_want = b"alpha\nBETA\ngamma\nhello world\ndelta\n"
        sed_out = wait_guest_file(img, ["tmp", "bb", "pipe2out.txt"], sed_want, check_t)
        check("宿主侧 /tmp/bb/pipe2out.txt（cat | sed s/beta/BETA/ > b）逐字节 == 期望 5 行",
              sed_out == sed_want, repr(sed_out))
        full_out = wait_guest_file(img, ["tmp", "bb", "out_full.txt"], b"300\n", check_t)
        check("宿主侧 /tmp/bb/out_full.txt == b'300\\n'（300B 过 64B 环：分次写完不丢字节 + EOF）",
              full_out == b"300\n", repr(full_out))

        # ---- 阻塞等待证据（含 pid/两端计数）----
        waits = re.findall(r"\[FD64\] pipe wait pid=(\d+) fd=(\d+) why=(\w+) ticks=(\d+) "
                           r"writers=(\d+) readers=(\d+)", final)
        check("有阻塞等待打点（[FD64] pipe wait ...）：pid/why/ticks/writers/readers 字段齐全",
              len(waits) >= 2, "waits=%d" % len(waits))
        check("等待打点的 pid 是真进程号（>0，不是 0/缺失）",
              any(int(w[0]) > 0 for w in waits), "pids=%s" % sorted(set(w[0] for w in waits))[:6])
        check("写 300B 过 64B 环必然出现 why=full（满 -> 阻塞等待 -> 继续写）",
              any(w[2] == "full" for w in waits), "whys=%s" % [w[2] for w in waits])
        max_ticks = max((int(w[3]) for w in waits), default=0)
        check("等待**没有**撞 30s 有界上限（ticks 远小于 7500 = 30s@250Hz）",
              max_ticks < 7500, "max_ticks=%d (~%.1f s)" % (max_ticks, max_ticks / 250.0))
        final = sess.log()                     # 轮询产物期间可能还有迟到的子进程日志：取最新
        check("正常管线里不出现 busybox 的 EAGAIN/EPIPE 报错文本（含迟到的）",
              "Resource temporarily unavailable" not in final and "Broken pipe" not in final, "")
        # FD_CLOEXEC 的 execve 落地：只有"标记过 cloexec 的进程 execve"才有 cloexec close 行；
        # busybox 自己带了用户态 fcntl 垫片（user/busybox/vimtu_fcntl.c，本批不允许改 user/**），
        # 所以这里只打印为如实说明，不算失败（内核侧 F_SETFD/F_GETFD 往返证据在 fdtest 的 fdcloexec=1）。
        n_cloexec = len(re.findall(r"\[FD64\] cloexec close n=\d+ path=", final))
        print("  [info] [FD64] cloexec close 行 = %d（0 = 本批没有任何进程对 fd 打过 FD_CLOEXEC）"
              % n_cloexec)

        print("--- 禁止项 ---")
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in final)
    finally:
        sess.close(keep=args.keep)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
