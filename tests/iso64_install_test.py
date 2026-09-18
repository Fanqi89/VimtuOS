#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/iso64_install_test.py - 从 **ISO 安装介质**（光盘引导）走完安装并做字节级验收

和 tests/install_flow_test.py 的区别：那份测的是"裸盘镜像当安装介质"（硬盘引导），
这份测的是 ISO（BIOS 光盘引导）—— 后端完全不同：
  BIOS → El Torito 引导镜像(boot/cdiso.asm) → loader64
       → loader64 用 **ATAPI(PACKET)** 把内核从光盘读进 0x100000（不碰 BIOS）
       → 安装程序用 ATAPI 把载荷(8073 扇区)读到内存再写进目标盘

验收：
  1) 串口：I:cdboot → I:jump loader64 → L:media=cd → ... → K
  2) 向导：介质描述符 OK kind=1 → 新建分区 → 安装完成 100% → 自动重启
  3) 目标盘字节：MBR 55AA + P1(0xEF,活动,9+8000) + P2(0x07,8009..尾)
     LBA1..8 = loader64.bin、LBA9.. = kernel64_os.bin 逐字节一致
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

ISO = os.path.join(ROOT, "vimtu64-64.iso")
LOADER = os.path.join(ROOT, "build64", "loader64.bin")
OS_KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")
INS_KERNEL = os.path.join(ROOT, "build64", "kernel64.bin")
TARGET_SECTORS = 32768


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            f = shutil.which(c)
            if f:
                return f
    return None


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=6)
        except OSError:
            return b""
        data = b""
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
            s.settimeout(0.6)
            while True:
                try:
                    ch = s.recv(4096)
                    if not ch:
                        break
                    data += ch
                except socket.timeout:
                    break
        finally:
            s.close()
        return data

    def key(self, name, wait=1.2):
        self.send("sendkey %s" % name, wait=wait)


def run_install(qemu, target, serial_log, port, dwell=1.3):
    if os.path.exists(serial_log):
        os.remove(serial_log)

    def q(p):
        return p.replace("\\", "/")

    args = [
        qemu, "-name", "VimtuOS-iso64",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(target),
        "-cdrom", q(ISO),
        "-boot", "order=d", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial_log),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(port)
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        print("[iso] 等向导就绪（README/串口出现磁盘枚举完成）...")
        ready = False
        for _ in range(240):
            time.sleep(0.25)
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    if "磁盘枚举完成" in f.read():
                        ready = True
                        break
            except FileNotFoundError:
                pass
        print("[iso] 向导就绪：%s" % ready)

        for _ in range(4):
            mon.key("ret", wait=dwell)
        print("[iso] n  新建（写引导分区 + 主分区）")
        mon.key("n", wait=dwell + 0.7)
        print("[iso] ret 安装系统")
        mon.key("ret", wait=dwell)

        print("[iso] 等安装完成（从光盘读 8073 扇区）...")
        for _ in range(180):
            time.sleep(0.5)
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    if "[SETUP] 自动重启" in f.read():
                        break
            except FileNotFoundError:
                pass
        time.sleep(2)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    try:
        with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def read_at(path, lba, n):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        return f.read(n)


def check_mbr(path):
    with open(path, "rb") as f:
        sec = f.read(512)
    if len(sec) < 512 or sec[510] != 0x55 or sec[511] != 0xAA:
        return None, None
    out = []
    for i in range(2):
        e = sec[446 + i * 16: 446 + i * 16 + 16]
        if e[4] == 0:
            out.append(None)
            continue
        out.append({"boot": e[0] == 0x80, "type": e[4],
                    "start": struct.unpack_from("<I", e, 8)[0],
                    "sectors": struct.unpack_from("<I", e, 12)[0]})
    return out[0], out[1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5558)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-iso-installed.img"))
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (ISO, LOADER, OS_KERNEL, INS_KERNEL):
        if not os.path.exists(need):
            sys.stderr.write("缺少 %s（先跑 build64.sh）\n" % need)
            return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    print("=== 1) 造 16MB 空目标盘 ===")
    with open(args.target, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * 512))
    print("     %s" % args.target)

    serial_log = os.path.join(os.path.dirname(args.target) or ".", "iso64_serial.log")
    print("=== 2) 光盘引导 + 注入按键走完安装 ===")
    log = run_install(qemu, args.target, serial_log, args.port)

    print("=== 3) 引导链与介质通路断言 ===")
    check("El Torito 引导桩启动", "I:boot dl=" in log and "I:cd mode" in log)
    check("引导桩读 loader64 并跳转", "I:jump loader64" in log)
    check("loader64 识别出光盘介质", "L:media=cd" in log)
    check("ATAPI 读内核成功（出现 K 标记）", "[LM64] ENTERED LONG MODE" in log)
    check("安装程序读到介质描述符（loader 已转成 RAM 源）",
          "[SETUP] 介质描述符 OK kind=2" in log)
    check("载荷由 loader 读进内存（RAM 源）", "[INSTALL] 介质源 kind=2" in log)
    check("光驱自检通过（ISO9660 主卷描述符）", "[CD] atapi" in log and " ok" in log)
    print("=== 4) 安装动作断言 ===")
    check("新建分区表 OK", "[PART] 新建分区表 OK" in log)
    check("安装开始（按介质描述符取载荷）", "[INSTALL] 介质源 kind=2" in log)
    check("安装完成（写出扇区数）", "[INSTALL] 完成：已写" in log)
    check("界面 100%", "[SETUP] 安装完成 100%" in log)
    check("装完自动重启", "[SETUP] 自动重启" in log)
    tail = [l for l in log.splitlines() if l.strip()][-12:]
    print("--- 串口尾部 ---")
    for l in tail:
        print("   | " + l[:150])

    print("=== 5) 目标盘字节断言 ===")
    p1, p2 = check_mbr(args.target)
    check("MBR 55AA 且分区1 存在", p1 is not None, "%s" % (p1,))
    if p1:
        check("P1 = 引导分区 0xEF 活动，9 + 8000", p1["type"] == 0xEF and p1["boot"]
              and p1["start"] == 9 and p1["sectors"] == 8000, "%s" % (p1,))
    check("P2 = 主分区 0x07，8009..盘尾", p2 is not None and p2["type"] == 0x07
          and p2["start"] == 8009 and p2["sectors"] == TARGET_SECTORS - 8009, "%s" % (p2,))
    loader_ref = open(LOADER, "rb").read()
    os_ref = open(OS_KERNEL, "rb").read()
    ins_ref = open(INS_KERNEL, "rb").read()
    check("LBA1..8 = loader64.bin 逐字节一致",
          read_at(args.target, 1, len(loader_ref)) == loader_ref, "%d 字节" % len(loader_ref))
    kern = read_at(args.target, 9, len(os_ref))
    check("LBA9.. = kernel64_os.bin 逐字节一致", kern == os_ref, "%d 字节" % len(os_ref))
    check("目标盘内核不是安装程序内核", kern != ins_ref[:len(kern)])

    print("=== ISO RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
