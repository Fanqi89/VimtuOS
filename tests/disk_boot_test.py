#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/disk_boot_test.py - ★ 真机可用性关键证据：**只挂 AHCI 的盘**也能启动已安装的系统

和 tests/ahci64_test.py --sata-install 的区别（那份测的是"AHCI 写盘 + 以 IDE 兼容方式重启"）：
  这里的第二段**把装好的盘单独挂在 ich9-ahci 上启动，机器上再也没有 IDE 兼容的盘**
  （安装介质盘拔掉）。这正是现代主板"SATA 默认 AHCI、没有 IDE 兼容模式"的形态。

  改造前引导层（boot/loader64.asm）读内核走的是自写 PATA PIO（只认 0x1F0/0x170），
  AHCI 模式下那些端口后面什么都没有 -> 装完系统重启就是黑屏。
  改造后磁盘启动路径走 **BIOS INT 13h 扩展读（AH=0x42 + DAP）**：
  固件认识 SATA/AHCI、固件映射过的 NVMe、USB、老 PATA —— 凡固件能引导的盘都能读内核。

它做什么（三段，全部真跑 QEMU）：
  1) 安装介质挂 IDE(index0)，16MB 空盘挂 ich9-ahci port0；
     注入按键走完向导 -> **系统真的通过 AHCI 写进 SATA 盘**
     （断言 [AHCI64] write lba=… / [INSTALL] 完成 / 目标盘 MBR 字节）
  2) **只挂那块 SATA 盘（只挂在 AHCI 上）** 启动，断言：
       [LM] disk boot via INT 13h dl=0x80       <- 引导层真的走了 BIOS INT 13h
       [LM] int13 read lba=… count=… ok         <- 内核是被 INT 13h 读进 0x100000 的
       [OS] booted from installed disk          <- 系统启动路径（不是安装程序）
       [GUI64] ready                            <- 进桌面
     + 屏上不该再出现 [SETUP]（没有又跑安装程序）+ 无 PANIC/TRIPLE FAULT/int13 FAILED
  3) 失败路径（真造一块"被截断"的盘：MBR + loader 有，内核区没有）：
     必须打 [LM] int13 read FAILED ah=0x… lba=… retry=… 然后停机，**不许静默继续**
     （断言没有 [LM64] ENTERED LONG MODE / [OS] booted）

退出码：0 全部通过 / 1 有断言失败 / 2 环境问题
用法：py -3 tests/disk_boot_test.py
      py -3 tests/disk_boot_test.py --port 5613 --keep
"""
import argparse
import os
import re
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

MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
TARGET_SECTORS = 32768                 # 16MB 目标 SATA 盘（与 ahci64_test 同口径）
LOADER = os.path.join(ROOT, "build64", "loader64.bin")
BOOTBIN = os.path.join(ROOT, "build64", "boot.bin")


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


def q(p):
    return p.replace("\\", "/")


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
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
        """注入一个按键（sendkey），wait = 注入后等多久（给向导留重绘/落盘时间）。"""
        self.send("sendkey %s" % name, wait=wait)


def wait_monitor(port, tries=60):
    for _ in range(tries):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.25)
    return False


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def run_qemu(args):
    return subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def kill(proc):
    if proc is None:
        return
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


def wait_for(log_path, needles, seconds, poll=0.4):
    """等日志里出现任一 needle（返回命中的那个，超时返回 None）。"""
    deadline = time.time() + seconds
    last = ""
    while time.time() < deadline:
        last = read_text(log_path)
        for n in needles:
            if n in last:
                return n
        time.sleep(poll)
    return None


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


def phase1_install(qemu, target, log, port):
    """安装介质挂 IDE(index0) + 目标盘挂 ich9-ahci port0，注入按键走完整安装（走 AHCI 写盘）。"""
    if os.path.exists(log):
        os.remove(log)
    args = [
        qemu, "-name", "VimtuOS-diskboot-install",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d0,format=raw" % q(target),
        "-device", "ide-hd,drive=d0,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(log),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    print("     qemu: 介质=IDE index0，目标盘=ich9-ahci port0")
    proc = run_qemu(args)
    mon = Monitor(port)
    try:
        if not wait_monitor(port):
            print("     [warn] monitor 端口没起来")
        wait_for(log, ["磁盘枚举完成"], 60)
        # 行布局（介质 = 驱动器 0 PATA、目标 = 驱动器 8 AHCI）：
        #   向导默认光标已经落在"非安装介质盘"的第一行（setup64.cpp 的 g_row_sel 逻辑）,
        #   也就是 8 号盘，所以不按方向键，直接 n 新建 + ret 开装（与 ahci64_test 同序列）。
        for key, what in (("ret", "语言 -> 现在安装"), ("ret", "现在安装 -> 许可"),
                          ("ret", "许可 -> 安装类型"), ("ret", "安装类型 -> 磁盘与分区"),
                          ("n", "在 8 号盘（AHCI，默认光标所在）上新建引导+主分区"),
                          ("ret", "选中分区 -> 安装系统")):
            print("     sendkey %-5s (%s)" % (key, what))
            mon.key(key, wait=1.2)
        got = wait_for(log, ["[SETUP] 安装完成", "安装完成"], 120)
        print("     安装完成标记：%s" % (got or "（超时）"))
        time.sleep(2.0)
    finally:
        kill(proc)
    return read_text(log)


def phase2_boot_ahci(qemu, target, log, seconds=180):
    """★ 关键一段：**只把装好的盘挂在 AHCI 上**（没有 IDE 兼容盘、没有安装介质）启动。"""
    if os.path.exists(log):
        os.remove(log)
    args = [
        qemu, "-name", "VimtuOS-diskboot-ahci",
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d0,format=raw" % q(target),
        "-device", "ide-hd,drive=d0,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(log),
        "-no-reboot",
    ]
    print("     qemu: 唯一的盘 = 装好的系统盘，挂在 ich9-ahci port0（无 IDE 兼容盘）")
    proc = run_qemu(args)
    try:
        got = wait_for(log, ["[GUI64] ready", "PANIC", "TRIPLE FAULT"], seconds)
        print("     启动终点标记：%s" % (got or "（超时）"))
    finally:
        kill(proc)
    return read_text(log)


def phase3_truncated(qemu, src_img, img, log, seconds=90):
    """失败路径：造一块"内核区被截断"的盘（MBR + loader 在，内核不在）-> 必须打点停机。"""
    if os.path.exists(log):
        os.remove(log)
    # 取装好的盘的前 1000 扇区：MBR(LBA0) + loader(LBA1..8) 都在，LBA 9..8008 的内核区没了
    with open(src_img, "rb") as f:
        head = f.read(1000 * 512)
    with open(img, "wb") as f:
        f.write(head)
    args = [
        qemu, "-name", "VimtuOS-diskboot-trunc",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(log),
        "-no-reboot",
    ]
    print("     qemu: 截断盘（1000 扇区，内核区 9..8008 不存在）")
    proc = run_qemu(args)
    try:
        got = wait_for(log, ["[LM] int13 read FAILED: kernel not loaded"], seconds)
        print("     失败路径标记：%s" % (got or "（超时）"))
    finally:
        kill(proc)
    return read_text(log)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5613)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-diskboot.img"))
    ap.add_argument("--keep", action="store_true", help="保留镜像与日志")
    args = ap.parse_args()

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (MEDIUM, LOADER, BOOTBIN):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 build64.sh）\n" % need)
            return 2

    print("=== 1) 造 16MB 空 SATA 目标盘，并**通过 AHCI 装进系统** ===")
    with open(args.target, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * 512))
    print("     %s = %d 扇区" % (args.target, TARGET_SECTORS))
    ins_log = os.path.join(ROOT, "disk_boot_install_serial.log")
    log1 = phase1_install(qemu, args.target, ins_log, args.port)

    print("=== 2) SATA 盘安装结果断言（走的是 AHCI 写盘）===")
    check("安装程序完成了 AHCI 自检（[AHCI64] selftest PASS）", "[AHCI64] selftest PASS" in log1)
    check("安装真的走 AHCI 写盘（[AHCI64] write lba=… ok）", "[AHCI64] write lba=" in log1,
          "; ".join([l for l in log1.splitlines() if "[AHCI64] write" in l])[:200])
    check("安装完成打点（[INSTALL] 完成：已写）", "[INSTALL] 完成：已写" in log1,
          "; ".join([l for l in log1.splitlines() if "[INSTALL]" in l])[:240])
    check("安装过程在 8 号盘（AHCI）上建了分区", re.search(r"\[PART\][^\r\n]*drive=8", log1) is not None)
    mbr_ok = load_ok = kernel_ok = False
    try:
        with open(args.target, "rb") as f:
            sec = f.read(512)
            mbr_ok = len(sec) == 512 and sec[510] == 0x55 and sec[511] == 0xAA and sec[446 + 4] != 0
            f.seek(512)
            load_ok = f.read(16) != b"\0" * 16
            f.seek(9 * 512)
            kernel_ok = f.read(1) != b""
    except OSError:
        pass
    check("SATA 盘装好了：MBR 55AA + 分区表项非空", mbr_ok, "target=%s" % args.target)
    check("SATA 盘 LBA1 起有 loader（与 build64/loader64.bin 同长度非空）", load_ok)
    check("SATA 盘 LBA9 起有系统内核", kernel_ok)

    print("=== 3) ★ 只挂 AHCI 盘启动（这就是\"纯 SATA 机器能启动\"的铁证）===")
    boot_log = os.path.join(ROOT, "disk_boot_ahci_serial.log")
    log2 = phase2_boot_ahci(qemu, args.target, boot_log)

    m = re.search(r"\[LM\] disk boot via INT 13h dl=0x([0-9A-Fa-f]{2})", log2)
    check("引导层走了 BIOS INT 13h 磁盘路径（[LM] disk boot via INT 13h dl=0xNN）",
          bool(m), (m.group(0) if m else "（缺 [LM] 行）"))
    reads = re.findall(r"\[LM\] int13 read lba=([0-9A-Fa-f]+) count=([0-9A-Fa-f]+) ok", log2)
    check("内核由 INT 13h 扩展读进内存（[LM] int13 read … ok）", bool(reads),
          "%d 条打点：%s" % (len(reads), reads[:4]))
    check("打点只打首尾（不刷屏：1..4 条）", 1 <= len(reads) <= 4, "条数=%d" % len(reads))
    check("装好的系统走系统启动路径（[OS] booted from installed disk）",
          "[OS] booted from installed disk" in log2,
          (re.search(r"\[OS\][^\r\n]*", log2).group(0) if "[OS]" in log2 else "缺"))
    check("进到桌面（[GUI64] ready）", "[GUI64] ready" in log2,
          (re.search(r"\[GUI64\][^\r\n]*", log2).group(0) if "[GUI64]" in log2 else "缺"))
    check("重启后不再跑安装程序（无 [SETUP] 向导打点）", "[SETUP] 磁盘枚举完成" not in log2)
    check("没有走光盘/RAM 介质分支（无 L:media=cd / L:media=ram）",
          ("L:media=cd" not in log2) and ("L:media=ram" not in log2))
    for needle in ("PANIC", "TRIPLE FAULT", "[LM] int13 read FAILED"):
        check("不得出现 %s" % needle, needle not in log2)

    print("=== 4) 失败路径：内核区被截断的盘必须打点停机（不许静默失败）===")
    trunc = os.path.join(ROOT, "target-diskboot-trunc.img") if args.keep else \
        os.path.join(ROOT, "target-diskboot-trunc.tmp.img")
    trunc_log = os.path.join(ROOT, "disk_boot_trunc_serial.log")
    log3 = phase3_truncated(qemu, args.target, trunc, trunc_log)
    mf = re.search(r"\[LM\] int13 read FAILED ah=0x([0-9A-Fa-f]{2}) lba=([0-9A-Fa-f]+) retry=([0-9A-Fa-f]{2})",
                   log3)
    check("失败时打出 [LM] int13 read FAILED ah=0x… lba=… retry=…", bool(mf),
          (mf.group(0) if mf else "（缺 FAILED 行）"))
    check("重试到 0 次后明确停机（[LM] int13 read FAILED: kernel not loaded, halted）",
          "[LM] int13 read FAILED: kernel not loaded, halted" in log3)
    check("失败时没有偷偷继续（没有 [LM64] ENTERED LONG MODE）",
          "[LM64] ENTERED LONG MODE" not in log3)
    check("失败时没有进到系统（没有 [OS] booted from installed disk）",
          "[OS] booted from installed disk" not in log3)

    print("=== 5) 打点原文（[LM] 行）===")
    for line in [l for l in log2.splitlines() if "[LM]" in l][:6]:
        print("   | " + line.strip()[:160])
    print("--- 截断盘串口尾部 ---")
    for line in [l for l in log3.splitlines() if l.strip()][-6:]:
        print("   | " + line.strip()[:160])

    if not args.keep:
        for p in (trunc,):
            try:
                os.remove(p)
            except OSError:
                pass

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
