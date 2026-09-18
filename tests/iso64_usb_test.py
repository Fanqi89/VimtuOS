#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/iso64_usb_test.py - 把 ISO 安装介质当**硬盘**引导（U 盘 / hybrid 路径）走完安装

和 tests/iso64_install_test.py（光盘引导）的区别：这里的引导路径完全不同：
  BIOS 执行 ISO 的**第 0 扇区**（我们自研的 hybrid MBR，boot/hybrid_mbr.asm）
    -> 它把自己搬到 0x0600，用 INT 13h 把 ISO 里的引导桩 CDISO.BIN 读进 0x7C00 并跳过去
    -> 桩进"硬盘分支"：unreal mode 一次往返（段限设 4GB）
       + INT 13h 分块读（BIOS 只能写低内存）+ a32 rep movsb 搬到 0x100000 / 0x04000000
    -> 写描述符 kind=2（RAM 源）-> loader64 -> 内核

这条路径就是"用 Rufus/dd 把 ISO 写进 U 盘"之后真机走的那条，也是虚拟机上
"拿 ISO 当硬盘挂"走的那条。ATAPI 在这条路上完全用不上（U 盘在 USB 控制器后面，
我们的 ATA PIO 够不着），所以全靠 BIOS + unreal mode。

验收：
  1) 串口：I:hdd mode (USB/disk) -> I:jump loader64 -> L:media=ram -> K
  2) 向导：介质描述符 kind=2 -> **认出介质盘并跳过默认行** -> 新建 -> 安装 100% -> 自动重启
  3) 目标盘字节：MBR 55AA + P1(0xEF,活动,9+8000) + P2(0x07,8009..尾)
     LBA1..8 = loader64.bin、LBA9.. = kernel64_os.bin 逐字节一致
  4) ★ 介质盘（ISO 文件）**从头到尾一个字节都没变**（没有把系统装回介质）
"""
import argparse
import hashlib
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


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


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


def qemu_args(qemu, target, serial_log, port, mode):
    """mode=ata：ISO 直接当 IDE 硬盘（最接近\"dd 进 U 盘\"时的 BIOS 视角）
       mode=usb：ISO 挂在 USB 存储上（真 U 盘的总线形态；SeaBIOS 的 USB MSC 驱动）"""
    def q(p):
        return p.replace("\\", "/")

    args = [qemu, "-name", "VimtuOS-usb64", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial_log),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot"]
    if mode == "usb":
        # 介质：USB 存储（真 U 盘的总线形态）。两个要点（都实测踩过）：
        #   * 控制器用 UHCI（USB 1.1）—— SeaBIOS 对它的支持最完整，xHCI 下 BIOS 压根不引导；
        #   * 必须给 USB 盘 **bootindex=1**，否则 SeaBIOS 只按 0x80 那块 IDE 盘试引导，
        #     空白目标盘引导失败后不会再去试 USB 盘（表现为串口一个字都没有）。
        args += ["-drive", "format=raw,file=%s,if=none,id=med" % q(ISO),
                 "-device", "piix3-usb-uhci,id=uhci",
                 "-device", "usb-storage,drive=med,bus=uhci.0,bootindex=1",
                 "-drive", "format=raw,file=%s,index=0,media=disk" % q(target)]
    else:
        # 介质 = IDE index0，目标盘 = IDE index1（BIOS: 0x80 / 0x81）
        args += ["-drive", "format=raw,file=%s,index=0,media=disk" % q(ISO),
                 "-drive", "format=raw,file=%s,index=1,media=disk" % q(target)]
    args += ["-boot", "order=c"]
    return args


def run_install(qemu, target, serial_log, port, mode, dwell=1.3):
    if os.path.exists(serial_log):
        os.remove(serial_log)

    proc = subprocess.Popen(qemu_args(qemu, target, serial_log, port, mode),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(port)
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

        print("[usb] 等 BIOS 引导 + 桩搬 ~8MB 到高内存 + 内核枚举磁盘 ...")
        ready = False
        for _ in range(480):                     # hybrid 路径要搬 ~8MB，比光盘慢，给足时间
            time.sleep(0.25)
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    if "磁盘枚举完成" in f.read():
                        ready = True
                        break
            except FileNotFoundError:
                pass
        print("[usb] 向导就绪：%s" % ready)

        for _ in range(4):
            mon.key("ret", wait=dwell)           # 语言 -> 现在安装 -> 许可 -> 类型
        print("[usb] n  新建（写引导分区 + 主分区）")
        mon.key("n", wait=dwell + 0.7)
        print("[usb] ret 安装系统")
        mon.key("ret", wait=dwell)

        print("[usb] 等安装完成（RAM 源 -> 目标盘）...")
        for _ in range(240):
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
    ap.add_argument("--mode", choices=("ata", "usb"), default="ata",
                    help="ata = ISO 当 IDE 硬盘（默认）；usb = ISO 挂 USB 存储（真 U 盘形态）")
    ap.add_argument("--port", type=int, default=5560)
    ap.add_argument("--target", default=None)
    args = ap.parse_args()

    if args.target is None:
        args.target = os.path.join(ROOT, "target-usb-installed.img")

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

    print("=== 0) hybrid MBR 自检（ISO 第 0 扇区）===")
    sec0 = read_at(ISO, 0, 512)
    check("ISO 第 0 扇区有引导签名 55AA", sec0[510] == 0x55 and sec0[511] == 0xAA)
    e0 = sec0[446:462]
    check("分区表一项：活动 + 类型 0x17 + 起始 LBA 0",
          e0[0] == 0x80 and e0[4] == 0x17 and struct.unpack_from("<I", e0, 8)[0] == 0,
          "type=0x%02X start=%d" % (e0[4], struct.unpack_from("<I", e0, 8)[0]))
    check("MBR 里带回填魔数 VMHY/VMHZ",
          b"VMHY" in sec0 and b"VMHZ" in sec0)

    print("=== 1) 造 16MB 空目标盘，记录介质盘指纹 ===")
    with open(args.target, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * 512))
    print("     %s" % args.target)
    iso_before = sha256(ISO)

    serial_log = os.path.join(ROOT, "usb64_serial.log")
    print("=== 2) 硬盘/hybrid 引导（mode=%s）+ 注入按键走完安装 ===" % args.mode)
    log = run_install(qemu, args.target, serial_log, args.port, args.mode)

    print("=== 3) 引导链断言（hybrid MBR -> 桩 -> unreal mode -> loader64）===")
    check("hybrid MBR 载入引导桩并执行（桩打印 dl）", "I:boot dl=" in log)
    check("走的是硬盘分支（unreal mode）", "I:hdd mode (USB/disk)" in log)
    check("桩读 loader64 并跳转", "I:jump loader64" in log)
    check("loader64 认出 RAM 介质（桩已搬好内核/载荷）", "L:media=ram" in log)
    check("进入长模式（K 标记）", "[LM64] ENTERED LONG MODE" in log)
    check("安装程序读到介质描述符 kind=2", "[SETUP] 介质描述符 OK kind=2" in log)
    check("载荷走 RAM 源（不再读介质）", "[INSTALL] 介质源 kind=2" in log)

    print("=== 4) 介质盘识别与保护断言 ===")
    if args.mode == "ata":
        # 介质盘自己就是一块 ATA 盘（hybrid 当硬盘挂）：必须被认出来并从向导默认行跳过
        check("认出安装介质所在盘（并从向导默认行跳过）",
              "[SETUP] 安装介质所在盘 drive=" in log)
    else:
        # USB 形态：介质在 USB 控制器后面，内核的 ATA PIO 看不到它（所以载荷必须由引导桩
        # 预先搬进内存）。此时介质盘不该出现在磁盘列表里，也就不需要识别/跳过。
        check("USB 形态：介质不在 ATA 线上（无需识别，也不会被误装）",
              "[SETUP] 安装介质所在盘 drive=" not in log)
    check("介质盘没被当成安装目标（目标盘上新建了分区）",
          "[PART] 新建分区表 OK" in log)

    print("=== 5) 安装动作断言 ===")
    check("安装完成（写出扇区数）", "[INSTALL] 完成：已写" in log)
    check("界面 100%", "[SETUP] 安装完成 100%" in log)
    check("装完自动重启", "[SETUP] 自动重启" in log)
    tail = [l for l in log.splitlines() if l.strip()][-12:]
    print("--- 串口尾部 ---")
    for l in tail:
        print("   | " + l[:150])

    print("=== 6) 介质盘零改动断言（★ 关键）===")
    iso_after = sha256(ISO)
    check("ISO 介质文件字节未变", iso_before == iso_after,
          "%s -> %s" % (iso_before[:12], iso_after[:12]))

    print("=== 7) 目标盘字节断言 ===")
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

    print("=== USB/hybrid RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
