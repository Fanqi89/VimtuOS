#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/partition_ops_test.py - 「新建 / 格式化 / 删除」的真实写盘验收（自动化）

为什么单独有这个测试：install_flow_test.py 只覆盖「新建 + 安装」。按需求，
安装程序还必须能**格式化**和**删除**分区，这两步也得证明真的写到盘上了。

做法（全程真按键、真写盘、真读盘校验）：
  1) 造一块 16MB 目标盘，并**预先在将被分区覆盖的区域写入垃圾数据**（0xA5/0x5A），
     这样"格式化只是假装"会被当场拆穿：格式化后那些扇区必须变成全 0。
  2) 启动安装介质 + 目标盘，通过 QEMU monitor 注入按键：
       回车×4（语言→现在安装→许可→安装类型→磁盘页）→ n（新建）
      → f（格式化 引导分区 P1）→ ↓ → f（格式化 主分区 P2，会写 VimtuFS2 真超级块）
       → d（删除 P2）→ ↓↓↓ → d（删除 P1）
  3) 串口断言每一步的日志（含 drive/index/start/sectors 等具体数值）
  4) 直接解析目标盘字节断言：
       * P1 首 64 扇区被清零（垃圾 0xA5 没了）
       * P2 首扇区 = VimtuFS2 真超级块（magic "VIMTUFS2" + 版本/几何 + CRC32 + 55AA）
       * 删除后 MBR 里两项的 16 字节全 0，签名仍是 55AA

退出码：0 全过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
TARGET_SECTORS = 32768
PART_BOOT_LBA = 9
PART_MAIN_LBA = 8009


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


def make_target(path):
    """16MB 目标盘：在 P1/P2 的数据区预填垃圾，用于验证格式化真的清零。"""
    with open(path, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * 512))
    with open(path, "r+b") as f:
        f.seek(PART_BOOT_LBA * 512)
        f.write(b"\xA5" * (64 * 512))                 # P1 数据区垃圾
        f.seek((PART_BOOT_LBA + 70) * 512)
        f.write(b"\xA5" * (4 * 512))                  # P1 更深处（格式化只清首 64 扇区）
        f.seek(PART_MAIN_LBA * 512)
        f.write(b"\x5A" * (64 * 512))                 # P2 数据区垃圾


def run_wizard(qemu, target, serial_log, port, dwell=1.3):
    if os.path.exists(serial_log):
        os.remove(serial_log)

    def q(p):
        return p.replace("\\", "/")

    args = [
        qemu, "-name", "VimtuOS-partops",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-drive", "format=raw,file=%s,index=1,media=disk" % q(target),
        "-boot", "order=c", "-m", "512", "-vga", "std",
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
        print("[partops] 向导就绪：%s" % ready)

        # 回车×4：语言 -> 现在安装 -> 许可 -> 安装类型 -> 磁盘与分区
        for _ in range(4):
            mon.key("ret", wait=dwell)
        print("[partops] n   新建分区（写引导分区 + 主分区）")
        mon.key("n", wait=dwell + 0.7)
        print("[partops] f   格式化引导分区 P1（应把预填的 0xA5 清成 0）")
        mon.key("f", wait=dwell + 0.7)
        print("[partops] down 选中 P2，f 格式化主分区（应写 VimtuFS2 真超级块）")
        mon.key("down", wait=dwell)
        mon.key("f", wait=dwell + 0.7)
        print("[partops] d   删除 P2（应清掉 MBR 第 2 项）")
        mon.key("d", wait=dwell + 0.7)
        # 删除后向导会把光标停在**同一块盘**的第一个分区上（见 setup64.cpp 的删除分支），
        # 所以删完 P2 直接按 d 就是在删 P1，不需要再按方向键。
        print("[partops] d   删除 P1（光标已在 P1 上；应清掉 MBR 第 1 项）")
        mon.key("d", wait=dwell + 0.7)
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


def read_at(path, lba, nbytes):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        return f.read(nbytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5557)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-partops.img"))
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(MEDIUM):
        sys.stderr.write("缺少安装介质 %s（先跑 build64.sh）\n" % MEDIUM)
        return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    print("=== 1) 造 16MB 目标盘（预填垃圾数据）===")
    make_target(args.target)
    print("     %s" % args.target)

    serial_log = os.path.join(os.path.dirname(args.target) or ".", "partops_serial.log")
    print("=== 2) 注入按键：新建 / 格式化 / 删除 ===")
    log = run_wizard(qemu, args.target, serial_log, args.port)

    print("=== 3) 串口断言 ===")
    check("新建分区表 OK", "[PART] 新建分区表 OK drive=1 boot=9+8000" in log)
    check("格式化引导分区 P1（index=1 start=9）",
          "[PART] 格式化 OK drive=1 index=1 start=9" in log)
    check("格式化主分区 P2（index=2 start=8009）",
          "[PART] 格式化 OK drive=1 index=2 start=8009" in log)
    check("删除分区项 index=2（P2）", "[PART] 删除分区 OK drive=1 index=2" in log)
    check("删除分区项 index=1（P1）", "[PART] 删除分区 OK drive=1 index=1" in log)
    # 现在注入的按键序列：ret×4 + n + f + down + f + d + d = 10
    # （删除后光标停回同一块盘的第一个分区，所以删 P1 不需要再按方向键）
    check("按键全部到达向导", log.count("[SETUP] key=") >= 10,
          "key 行数=%d" % log.count("[SETUP] key="))
    print("--- 串口里的 PART 行 ---")
    for l in log.splitlines():
        if "[PART]" in l:
            print("   | " + l.strip()[:140])

    print("=== 4) 目标盘字节断言 ===")
    p1_head = read_at(args.target, PART_BOOT_LBA, 512)
    check("P1 首扇区被格式化清零（0xA5 垃圾已消失）", p1_head == b"\0" * 512,
          "前 16 字节=%s" % p1_head[:16].hex())
    p1_far = read_at(args.target, PART_BOOT_LBA + 10, 512)
    check("P1 第 10 扇区也在被清的 64 扇区内（全 0）", p1_far == b"\0" * 512)
    p1_deep = read_at(args.target, PART_BOOT_LBA + 70, 512)
    check("P1 更深处的垃圾保留（只清首 64 扇区，不整盘擦）",
          p1_deep == b"\xA5" * 512, "首字节=0x%02X" % p1_deep[0])

    p2 = read_at(args.target, PART_MAIN_LBA, 512)
    # 注意：格式化写的是**真文件系统 VimtuFS2** 的超级块（不再是占位串 "VIMTUFS1"）。
    # 字段偏移见 kernel/vfs64.cpp 的 VFS_O_*：8=版本、12=扇区大小、16=块大小、20=总块数、
    # 24=根 inode、28/32=位图起点/块数、36/40/44=inode 起点/个数/大小、48/52=数据区起点/块数、
    # 60=CRC32([0,60))、510=0x55AA。下面期望的几何数字按 vfs64_format 的规则算：
    # ★ v3（目录树版）：inode 128B/个（每块 4 个）——
    #   24759 块 -> 位图 ceil(24759/4096)=7 块；inode min(24759/64,512)=386 个=ceil(386/4)=97 块；
    #   数据区 = 24759 - (1+7+97) = 24654 块。
    check("P2 首扇区 = VimtuFS2 真超级块 magic VIMTUFS2", p2[:8] == b"VIMTUFS2", "%r" % p2[:8])
    gf = struct.unpack_from("<13I", p2, 8)
    check("VimtuFS2 超级块版本 = 3（v3 目录树）且扇区/块 = 512B、inode = 128B",
          gf[0] == 3 and gf[1] == 512 and gf[2] == 512 and gf[9] == 128,
          "version=%d sector=%d block=%d inode=%d" % (gf[0], gf[1], gf[2], gf[9]))
    check("VimtuFS2 超级块记录总块数 = 24759",
          gf[3] == TARGET_SECTORS - PART_MAIN_LBA, "total=%d" % gf[3])
    check("VimtuFS2 超级块几何自洽（根 inode 0 / 位图 7 块 / inode 386 个 97 块 / 数据区 24654 块）",
          gf[4] == 0 and gf[5] == 1 and gf[6] == 7 and gf[7] == 8 and gf[8] == 386 and
          gf[10] == 105 and gf[11] == 24654,
          "root=%d bitmap=%d@%d inodes=%d@%d data=%d@%d" %
          (gf[4], gf[6], gf[5], gf[8], gf[7], gf[11], gf[10]))
    check("VimtuFS2 超级块 CRC32([0,60)) 正确",
          struct.unpack_from("<I", p2, 60)[0] == zlib.crc32(p2[:60]) & 0xFFFFFFFF,
          "crc=%08X" % struct.unpack_from("<I", p2, 60)[0])
    check("VimtuFS2 超级块尾部签名 55AA", p2[510] == 0x55 and p2[511] == 0xAA)

    mbr = read_at(args.target, 0, 512)
    check("MBR 签名仍为 55AA", mbr[510] == 0x55 and mbr[511] == 0xAA)
    e1 = mbr[446:462]
    e2 = mbr[462:478]
    check("MBR 第 1 项已被删除（16 字节全 0）", e1 == b"\0" * 16, e1.hex())
    check("MBR 第 2 项已被删除（16 字节全 0）", e2 == b"\0" * 16, e2.hex())

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
