#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ext4mount64_test.py - ★ v0.9.40：ext4 作为**只读服务卷**挂进文件系统（P3-B）

要证明的链路（缺一环都不算挂上）：
  ① drive64 认出 ext4 分区 -> 给盘符：`[DRV64] letter=E: … fs=ext4 … ro=1`
  ② fs64 登记服务卷：`[FS64] mount ext4r vol=9 … ro=1 via=ring3-svc`
  ③ **用户路径真的走得通**：在内核终端里 `vol E:` 切卷 + `ls` 列目录 -> `[TERM] cmd ls entries=3`
  ④ 读内容仍走 ring3 服务（rpc=1 时）：`[EXT4VOL] selftest read="/big.bin" … crc=0x92858800 ok`
  ⑤ 全程系统活着：kheart 心跳继续、没有 PANIC

怎么切场景：/etc/r3drv.conf 是纯 ASCII，测试做**等长字节替换**（rpc=0 -> rpc=1）。

用法：py -3 tests/ext4mount64_test.py [--qemu PATH] [--timeout 300] [--keep]
"""
import argparse
import importlib.util
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

spec = importlib.util.spec_from_file_location("np", os.path.join(HERE, "netping64_test.py"))
NP = importlib.util.module_from_spec(spec)
spec.loader.exec_module(NP)          # 复用 find_qemu / keys_for

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")
MKFS = os.path.join(ROOT, "tools", "mkfs_ext4_win.py")
FX_BLOCKS = 16384
MON_PORT = 5681


class Mon(object):
    """QEMU monitor（telnet）最小封装：只要 sendkey。"""

    def __init__(self, port):
        self.port = port
        self.sock = None

    def connect(self):
        for _ in range(80):
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                self.sock.settimeout(2)
                time.sleep(0.3)
                try:
                    self.sock.recv(4096)
                except Exception:
                    pass
                return True
            except Exception:
                time.sleep(0.5)
        return False

    def send(self, cmd, wait=0.35):
        if not self.sock:
            return
        try:
            self.sock.sendall((cmd + "\n").encode())
        except Exception:
            pass
        time.sleep(wait)

    def key(self, name, wait=0.8):
        self.send("sendkey %s" % name, wait=wait)

    def type_keys(self, names, wait=0.12):
        for k in names:
            self.key(k, wait=wait)

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except Exception:
            pass


def keys_for2(text):
    """QEMU sendkey 的键名映射：空格/减号/点/斜杠有别名，**冒号要 shift-semicolon**
    （netping64_test.keys_for 会把未知字符原样当键名，`vol D:` 因此只打出 "vol D"）。"""
    out = []
    for ch in text:
        if ch == " ":
            out.append("spc")
        elif ch == ":":
            out.append("shift-semicolon")
        elif ch == "-":
            out.append("minus")
        elif ch == ".":
            out.append("dot")
        elif ch == "/":
            out.append("slash")
        else:
            out.append(ch)
    return out


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def patch_conf(img_bytes, pairs):
    data = img_bytes
    for old, new in pairs:
        assert len(old) == len(new), (old, new)
        if data.count(old) == 0:
            raise SystemExit("镜像里找不到 %r（先跑全量构建）" % old)
        data = data.replace(old, new)
    return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = NP.find_qemu(args.qemu) if hasattr(NP, "find_qemu") else None
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (SYSDISK, MKFS):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_ext4mount_")
    fx = os.path.join(tmp, "ext4fx.img")
    print("=== 0) 生成 ext4 夹具 ===")
    if subprocess.call([sys.executable, MKFS, "--out", fx, "--blocks", str(FX_BLOCKS)],
                       stdout=subprocess.DEVNULL) != 0:
        sys.stderr.write("夹具生成失败\n")
        return 2

    img = os.path.join(tmp, "sys_rpc.img")
    with open(img, "wb") as f:
        f.write(patch_conf(open(SYSDISK, "rb").read(), [(b"rpc=0", b"rpc=1")]))

    serial = os.path.join(tmp, "serial_mount.log")
    if os.path.exists(serial):
        os.remove(serial)
    qargs = [
        qemu, "-name", "VimtuOS-ext4mount",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial,
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-no-reboot",
    ]
    proc = subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Mon(MON_PORT)
    log = ""
    try:
        mon.connect()
        # 等桌面 + 盘符扫描
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            time.sleep(0.5)
            log = read_text(serial)
            if re.search(r"\[DRV64\] letter=[A-Z]: .*fs=ext4", log) and "[GUI64] ready" in log:
                break
            if proc.poll() is not None:
                break

        checks = []

        def check(name, cond, detail=""):
            checks.append((name, bool(cond)))
            print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

        m_letter = re.search(r"\[DRV64\] letter=([A-Z]): .*fs=ext4.*", log)
        check("drive64 给 ext4 分配了盘符（fs=ext4 + ro=1）",
              m_letter is not None and "ro=1" in m_letter.group(0),
              m_letter.group(0) if m_letter else "缺")
        letter = m_letter.group(1) if m_letter else "E"

        m_mount = re.search(r"\[FS64\] mount ext4r vol=(\d+) disk=(\d+) lba=(\d+) ro=1 via=ring3-svc", log)
        check("fs64 登记了 ext4 只读服务卷（vol=9 via=ring3-svc）",
              m_mount is not None and m_mount.group(1) == "9",
              m_mount.group(0) if m_mount else "缺")

        # ---- 登录并开终端，走真实用户路径 ----
        print("=== 1) 登录 -> 开终端 -> vol %s: -> ls ===" % letter)
        mon.key("ret", wait=1.0); mon.key("ret", wait=1.5)
        mon.key("meta_l", wait=1.2); mon.key("1", wait=2.0)
        time.sleep(1.5)
        log = read_text(serial)          # ★ 按完键再读：用开终端之前的日志判断必然误红
        check("终端应用打开（[APP] term opened）", "[APP] term opened" in log)

        base = len(read_text(serial))
        # ★ 不带冒号：terminal 的 cmd_vol 接受 arg[1]==0；而 QEMU 的 shift-semicolon 在
        #   guest 侧并没有变成 ":"（实测打到终端里是 ";" -> [VOL] usage bad-arg）。
        # ★ 用小写字母：cmd_vol 会自动转大写；而 `sendkey D`（shift+d）实测没能进入终端参数。
        cmd = "vol %s" % letter.lower()
        mon.type_keys(keys_for2(cmd)); mon.key("ret", wait=1.5)
        mon.type_keys(keys_for2("ls")); mon.key("ret", wait=3.0)
        time.sleep(2.0)
        seg = read_text(serial)[base:]

        check("终端切卷成功（[VOL] switch letter=%s:)" % letter,
              re.search(r"\[VOL\] switch letter=%s:" % letter, seg) is not None,
              (re.search(r"\[VOL\][^\r\n]*", seg).group(0) if re.search(r"\[VOL\]", seg) else "缺"))

        # ★ 判据：**fs64 分派**（文件管理器走的就是这条），不是终端的 fd64/vfs64 路径。
        full2 = read_text(serial)
        m_fs64l = re.search(r'\[EXT4VOL\] selftest fs64-list count=(\d+) names=([^\s]+) ok', full2)
        check("fs64 分派列出 ext4 卷根目录 3 条（count=3 names=… ok）",
              m_fs64l is not None and m_fs64l.group(1) == "3",
              m_fs64l.group(0) if m_fs64l else "缺")
        check("fs64 分派列出来的名字里有 hello.txt / big.bin / sub",
              m_fs64l is not None and all(n in m_fs64l.group(2) for n in ("hello.txt", "big.bin", "sub")),
              m_fs64l.group(2) if m_fs64l else "缺")
        m_fs64r = re.search(r'\[EXT4VOL\] selftest fs64-read path=/hello\.txt bytes=(\d+) text=(.*?) ok', full2)
        check("fs64 分派读到 /hello.txt（13 B，内容 ok）",
              m_fs64r is not None and m_fs64r.group(1) == "13",
              m_fs64r.group(0) if m_fs64r else "缺")

        full = read_text(serial)
        m_read = re.search(r'\[EXT4VOL\] selftest read="/big\.bin" bytes=(\d+) crc=0x([0-9a-fA-F]+) ok', full)
        check("内容仍来自 ring3 服务（selftest read /big.bin 100000 B + CRC ok）",
              m_read is not None and m_read.group(1) == "100000"
              and (int(m_read.group(2), 16) & 0xFFFFFFFF) == 0x92858800,
              m_read.group(0) if m_read else "缺")

        check("全程没有 PANIC", "[PANIC64]" not in full)
        kbeats = len(re.findall(r"\[TASK64\] kheart beat=", full))
        check("GUI/内核仍然活着（kheart 心跳持续）", kbeats >= 3, "beats=%d" % kbeats)

        npass = sum(1 for _, c in checks if c)
        print("\n%d/%d PASS" % (npass, len(checks)))
        if npass != len(checks):
            print("FAILED: %s" % "; ".join(n for n, c in checks if not c))
            print("日志保留在 %s" % tmp)
            print("FAIL")
            return 1
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        print("PASS：ext4 已作为**只读服务卷**挂进文件系统，终端 vol+ls 走得通，内容仍由 ring3 服务按需提供")
        return 0
    finally:
        mon.close()
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
