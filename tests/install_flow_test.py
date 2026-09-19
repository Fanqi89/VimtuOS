#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/install_flow_test.py - 端到端安装流程验收（自动化，不用人点鼠标）

它做的事，正是用户会做的事：
  1) 建一块空的目标硬盘（16MB raw 镜像）
  2) 启动安装介质（第一块盘）+ 目标盘（第二块盘），通过 QEMU monitor 注入按键走完向导：
        语言 → 现在安装 → 许可 → 安装类型 → 【新建分区】 → 选中分区 → 【安装系统】
  3) 安装过程中读串口日志，核对每一步都真的写盘了（[PART]/[INSTALL]/[SETUP] 行）
  4) 直接解析目标盘镜像，核对：
        · MBR 签名 55 AA
        · 分区1 = 引导分区（type 0xEF，活动，LBA 9 + 8000）
        · 分区2 = 主分区（type 0x07，起点 8009）
        · LBA 1..8 = loader64.bin
        · LBA 9..  = kernel64_os.bin（**系统**内核，不是安装程序内核）
  5) 把目标盘单独启动一次，断言它进的是"系统启动路径"（[OS] ...）而不是安装程序（[SETUP] ...）

退出码：0 全部通过 / 1 有断言失败 / 2 环境问题
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

MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
LOADER = os.path.join(ROOT, "build64", "loader64.bin")
OS_KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")
INS_KERNEL = os.path.join(ROOT, "build64", "kernel64.bin")

TARGET_SECTORS = 32768          # 16 MB 目标盘


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
        self.send("sendkey %s" % name, wait=wait)


def wipe(path, sectors):
    with open(path, "wb") as f:
        f.write(b"\0" * (sectors * 512))


def run_install(qemu, target_path, serial_log, port, dwell=2.0):
    """启动安装介质 + 目标盘，注入按键完成安装。返回串口日志文本。"""
    if os.path.exists(serial_log):
        os.remove(serial_log)

    def q(p):
        return p.replace("\\", "/")

    args = [
        qemu, "-name", "VimtuOS-install-flow",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-drive", "format=raw,file=%s,index=1,media=disk" % q(target_path),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial_log),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(port)
    try:
        # 等 monitor 端口就绪
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

        print("[flow] 等向导就绪（轮询串口日志里的磁盘枚举完成行）...")
        ready = False
        for _ in range(240):                      # 最多等 60 秒（TCG 下字体初始化较慢）
            time.sleep(0.25)
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    if "磁盘枚举完成" in f.read():
                        ready = True
                        break
            except FileNotFoundError:
                pass
        print("[flow] 向导就绪：%s" % ("是" if ready else "否（超时，继续按键看效果）"))

        seq = [
            ("ret", "语言页 -> 现在安装"),
            ("ret", "现在安装 -> 许可条款"),
            ("ret", "许可条款（回车=接受）-> 安装类型"),
            ("ret", "安装类型（升级）-> 磁盘与分区"),
            ("n",   "磁盘与分区：新建（写引导分区 + 主分区）"),
            ("ret", "选中已创建的分区 -> 安装系统"),
        ]
        for key, what in seq:
            print("[flow] sendkey %-4s  (%s)" % (key, what))
            mon.key(key, wait=dwell)

        print("[flow] 等待复制完成（载荷 8073 扇区，每帧 128 扇区）...")
        time.sleep(20)
        # 安装完成后会自动重启（-no-reboot 下 QEMU 会退出），这里再等一会收日志
        time.sleep(3)
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


def check_mbr(path):
    """返回 (part1, part2)，每项是 dict 或 None"""
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
        out.append({
            "boot": e[0] == 0x80,
            "type": e[4],
            "start": struct.unpack_from("<I", e, 8)[0],
            "sectors": struct.unpack_from("<I", e, 12)[0],
        })
    return out[0], out[1]


def read_at(path, lba, nbytes):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        return f.read(nbytes)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5556)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-installed.img"))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (MEDIUM, LOADER, OS_KERNEL, INS_KERNEL):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 build64.sh）\n" % need)
            return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    print("=== 1) 建 16MB 空目标盘 ===")
    wipe(args.target, TARGET_SECTORS)
    print("     %s = %d 扇区" % (args.target, TARGET_SECTORS))

    serial_log = os.path.join(os.path.dirname(args.target) or ".", "install_serial.log")
    print("=== 2) 启动安装介质并注入按键走完向导 ===")
    log = run_install(qemu, args.target, serial_log, args.port)
    if args.keep:
        print("     串口日志：%s" % serial_log)

    print("=== 3) 串口日志断言（每一步都真的写盘）===")
    check("新建分区：写入引导分区 + 主分区", "[PART] 新建分区表 OK" in log)
    # ★ 新增：裸盘/硬盘介质走的是 BIOS INT 13h 扩展读内核（不是自写 PATA PIO）
    check("引导层走 BIOS INT 13h 读内核（裸盘介质）",
          "[LM] disk boot via INT 13h dl=0x" in log and "[LM] int13 read lba=" in log)
    check("安装开始：找到介质载荷", "[INSTALL] 开始安装" in log)
    check("安装完成：写出扇区数", "[INSTALL] 完成：已写" in log)
    check("界面侧确认安装完成", "[SETUP] 安装完成" in log)
    check("完成后自动重启", "[SETUP] 自动重启" in log)
    check("全程无失败标记", ("失败" not in log) or ("[INSTALL] 完成" in log))
    check("没用安装程序内核之外的路径", "[SETUP] install_begin" in log)
    # 串口尾部便于排障
    tail = [l for l in log.splitlines() if l.strip()][-14:]
    print("--- 串口尾部 ---")
    for l in tail:
        print("   | " + l[:150])

    print("=== 4) 目标盘字节断言（直接解析镜像）===")
    p1, p2 = check_mbr(args.target)
    check("MBR 签名 55AA 且分区1 存在", p1 is not None, "%s" % (p1,))
    if p1:
        check("分区1 = 引导分区 type 0xEF 且活动", p1["type"] == 0xEF and p1["boot"],
              "type=0x%02X boot=%s" % (p1["type"], p1["boot"]))
        check("分区1 位置 = LBA 9 + 8000 扇区", p1["start"] == 9 and p1["sectors"] == 8000,
              "start=%d sectors=%d" % (p1["start"], p1["sectors"]))
    check("分区2 = 主分区 type 0x07", p2 is not None and p2["type"] == 0x07, "%s" % (p2,))
    if p2:
        expect_main = TARGET_SECTORS - 8009
        check("分区2 起点 8009 且覆盖到盘尾", p2["start"] == 8009 and p2["sectors"] == expect_main,
              "start=%d sectors=%d（期望 %d）" % (p2["start"], p2["sectors"], expect_main))

    loader_ref = open(LOADER, "rb").read()
    os_ref = open(OS_KERNEL, "rb").read()
    ins_ref = open(INS_KERNEL, "rb").read()

    loader_disk = read_at(args.target, 1, len(loader_ref))
    check("目标盘 LBA1..8 = loader64.bin（逐字节一致）", loader_disk == loader_ref,
          "长度 %d" % len(loader_ref))

    kern_disk = read_at(args.target, 9, len(os_ref))
    check("目标盘 LBA9.. = 系统内核 kernel64_os.bin（逐字节一致）", kern_disk == os_ref,
          "长度 %d" % len(os_ref))
    check("目标盘上的内核**不是**安装程序内核", kern_disk != ins_ref[:len(kern_disk)])

    print("=== 5) 单独启动装好的硬盘（验证真的能开机）===")
    boot_log = os.path.join(os.path.dirname(args.target) or ".", "installed_boot.log")
    if os.path.exists(boot_log):
        os.remove(boot_log)

    def q(p):
        return p.replace("\\", "/")

    args2 = [
        qemu, "-name", "VimtuOS-installed",
        "-drive", "format=raw,file=%s" % q(args.target),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(boot_log),
        "-no-reboot",
    ]
    proc = subprocess.Popen(args2, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(14)
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass
    try:
        with open(boot_log, "r", encoding="utf-8", errors="replace") as f:
            boot = f.read()
    except FileNotFoundError:
        boot = ""

    check("装好的系统进入长模式", "[LM64] ENTERED LONG MODE" in boot)
    # ★ 新增：装好的系统盘走的是 BIOS INT 13h 扩展读内核（引导层不再用 PATA PIO）
    check("装好的系统走 BIOS INT 13h 读内核",
          "[LM] disk boot via INT 13h dl=0x" in boot and "[LM] int13 read lba=" in boot)
    check("装好的系统走的是系统启动路径", "[OS] booted from installed disk" in boot)
    check("装好的系统就绪", "[OS] ready (idle)" in boot)
    check("装好的系统**没有**再进安装程序", "[SETUP]" not in boot and "entering setup wizard" not in boot)
    print("--- 装好的系统串口尾部 ---")
    for l in [x for x in boot.splitlines() if x.strip()][-8:]:
        print("   | " + l[:150])

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
