#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/statfs64_test.py - ★ 本批：statfs(137) / fstatfs(138) 端到端验收（让 busybox `df` 能用）

要证明的事：
  ① 夹具 = build64/sysdisk.img 副本 + /proc/mounts（Linux 上由内核提供；本内核**没有 mount 表 ABI**，
     所以夹具放一份标准 mtab 供 busybox df 枚举 —— df 的容量数字全部来自 statfs(137) 的 statvfs）。
  ② 内核终端 `df` 在同一时刻给出 blocks/free（与 [DRV64] letter= 同源：drive64->fs64->vfs64 现数位图）。
  ③ ring3 `run /bin/df` -> statvfs -> statfs(137)：[FS64] statfs 行的 blocks/bfree 必须与 ② 逐值相等，
     且 == [DRV64] total_kb×2 / free_kb×2（同一来源）；busybox 表格的 1K 块数 = blocks/2、
     Available = bfree/2，挂载点 "/"。
  ④ f_files/f_ffree 与宿主侧独立解析夹具卷的 inode 表逐值一致（总槽数 / 空槽数）。
  ⑤ `df -h` 输出合理（人类可读表头 + 挂载点）。

用法（必须 Windows 原生 Python）：py -3 tests\\statfs64_test.py [--timeout 180] [--keep]
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402

BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512
INODE_BYTES = 128

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}

MTAB = b"/dev/sda2 / vimtufs2 rw 0 0\n"

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
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


def vol_bytes_of(path):
    d = open(path, "rb").read()
    return d[PART_MAIN_LBA * SECTOR:(PART_MAIN_LBA + TARGET_SECTORS) * SECTOR]


def build_fixture(tmp):
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    vol = vol_bytes_of(DISK)
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = int.from_bytes(vol[20:24], "little")
    ed = TP.VolumeEdit(total)
    ed.load(vol)
    ed.put("/proc/mounts", MTAB, mode=0o644)     # 夹具 mtab（内核没有 mount 表 ABI，见文件头）
    out = ed.finish()
    system = open(SYSTEM_IMG, "rb").read()
    disk = TP.SV.build_disk(system, out, TARGET_SECTORS)
    path = os.path.join(tmp, "statfs64.img")
    open(path, "wb").write(disk)
    return path


def host_inode_stats(img):
    """宿主侧**独立**解析夹具卷的 inode 表：返回 (总槽数, 已用数, 空槽数)。
    口径与内核 vfs64_inode_stats64 相同：type != 0 都算已用（含 0 号根目录）。"""
    vol = vol_bytes_of(img)
    ino_start = struct.unpack_from("<I", vol, 36)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
    used = 0
    for i in range(inodes):
        off = ino_start * SECTOR + i * INODE_BYTES
        if vol[off] != 0:
            used += 1
    return inodes, used, inodes - used


class Session:
    def __init__(self, qemu, img, tag, timeout):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_statfs64_")
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
            print("[statfs64] 串口日志：%s" % self.serial)


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_statfs64_fix_")
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

    print("=== Vimtu64 statfs/fstatfs acceptance（busybox df）===")
    sess = Session(qemu, img, "statfs64", args.timeout)
    try:
        # ① 内核终端 df：同一时刻的 blocks/free（[TERM] 行 + 重打的实时 [DRV64] 行）
        sess.type_line("df")
        sess.wait("[TERM] cmd df ok", 60)
        time.sleep(0.6)
        t0 = sess.log()
        mdf = re.findall(r"\[TERM\] cmd df blocks=(\d+) free=(\d+) files=(\d+)", t0)
        mdrv = re.findall(r"\[DRV64\] letter=C: disk=\d+ part=\d+ fs=VimtuFS2 total_kb=(\d+) free_kb=(\d+)", t0)
        check("内核终端 df 与实时 [DRV64] letter=C: 行都在（同刻来源）",
              bool(mdf) and bool(mdrv), "df=%s drv=%s" % (mdf[-1] if mdf else None, mdrv[-1] if mdrv else None))

        # ② ring3：run /bin/df -> statvfs -> statfs(137)
        sess.type_line("shell")
        sess.wait("[SH64] launch path=/bin/shell.bin", 120)
        time.sleep(0.8)
        base = len(sess.log())
        sess.type_line("run /bin/df", per_key=0.08)
        sess.wait("[FS64] statfs path=/", timeout=args.timeout, since=base)
        sess.wait("run: /bin/df pid=", timeout=args.timeout, since=base)
        time.sleep(0.8)
        t1 = sess.log()[base:]
        msf = re.findall(r"\[FS64\] statfs path=/ bsize=(\d+) blocks=(\d+) bfree=(\d+) files=(\d+) ffree=(\d+)", t1)
        check("statfs(137) 被真调用：bsize=512 + blocks/bfree/files/ffree 齐全",
              bool(msf) and msf[-1][0] == "512", "line=%s" % (msf[-1] if msf else None,))

        if mdf and mdrv and msf:
            T, F = int(mdf[-1][0]), int(mdf[-1][1])
            tk, fk = int(mdrv[-1][0]), int(mdrv[-1][1])
            sb, sf, sfiles, sffree = (int(msf[-1][1]), int(msf[-1][2]), int(msf[-1][3]), int(msf[-1][4]))
            check("statfs 的 blocks/bfree == 内核 df 同刻的 total/free",
                  sb == T and sf == F, "statfs=%d/%d df=%d/%d" % (sb, sf, T, F))
            check("statfs 的 blocks/bfree == [DRV64] total_kb×2 / free_kb×2（同一来源）",
                  sb == tk * 2 and sf == fk * 2, "statfs=%d/%d drv_kb=%d/%d" % (sb, sf, tk, fk))
            ninodes, nused, nfree = host_inode_stats(img)
            check("f_files/f_ffree == 宿主侧独立解析 inode 表（总槽数/空槽数）",
                  sfiles == ninodes and sffree == nfree,
                  "statfs=%d/%d host=%d/%d used=%d" % (sfiles, sffree, ninodes, nfree, nused))
        else:
            check("statfs 数值对照（缺证据行 -> FAIL）", False, "mdf=%s mdrv=%s msf=%s" % (mdf, mdrv, msf))

        # ③ busybox df 表格（同一段串口里找 /dev/sda2 行：1K-blocks/Used/Available/Use% + 挂载点 /）
        mt = re.search(r"/dev/sda2\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)%\s+/", t1)
        check("busybox df 表格：/dev/sda2 一行 + 挂载点 / + 1K-blocks 表头",
              mt is not None and "1K-blocks" in t1, "row=%s" % (mt.group(0) if mt else None))
        if mt and msf:
            blk = int(msf[-1][1])
            avail = int(msf[-1][2])
            check("busybox df 的 1K 块数/Available == statfs blocks/2、bfree/2",
                  int(mt.group(1)) == blk // 2 and int(mt.group(3)) == avail // 2,
                  "df=%s,%s statfs/2=%d,%d" % (mt.group(1), mt.group(3), blk // 2, avail // 2))

        # ④ df -h（人类可读）
        base = len(sess.log())
        sess.type_line("run /bin/df -h", per_key=0.1)
        sess.wait("Mounted on", timeout=120, since=base)
        time.sleep(0.8)
        t2 = sess.log()[base:]
        check("busybox df -h 输出合理（Size/Used/Available 表头 + /dev/sda2 + 挂载点 /）",
              "Size" in t2 and "Mounted on" in t2 and "/dev/sda2" in t2 and re.search(r"\d+(\.\d+)?[KM]\s+\d+(\.\d+)?[KM]\s+\d+(\.\d+)?[KM]", t2) is not None, "")

        print("--- 禁止项 ---")
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in sess.log())
    finally:
        sess.close(keep=args.keep)

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
