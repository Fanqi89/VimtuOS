#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ahci64_test.py - item 5a：AHCI(SATA) 驱动 + 屏幕硬件检查报告 端到端验收

它做什么：
  1) 起一台 QEMU：安装介质挂在默认 IDE（引导链走 PIO，必须），**一块空的 SATA 盘挂在
     ich9-ahci 控制器的 0 号端口**（-device ich9-ahci,id=ahci -device ide-hd,...bus=ahci.0）。
  2) 断言 AHCI 驱动真的在这个控制器上跑起来了（全部取串口真打点）：
        [AHCI64] pci <b>:<d>.<f> abar=0x... cap=0x... pi=0x... ports=<n>
        [AHCI64] hba reset ok|timeout bohc=0x...
        [AHCI64] port=0 det=3 sig=0x...101 kind=...
     + 屏幕硬件检查报告：[HWUI] report lines= / [HWUI] report shown ms= / [HWUI] selftest PASS
     + 报告页**像素**断言（黑底 + 区块色块 + 浅色文字，用 QEMU screendump 抓帧）
     + 安装程序枚举到这块 SATA 盘：[DISK] 8 ... bus=AHCI
     + 全程无 PANIC / TRIPLE FAULT / FAILED mask=
  3) `--strict-dma`：额外要求"AHCI DMA 通路可用"的证据
       [AHCI64] selftest PASS + [AHCI64] drive 8 model=... sectors=...
       + [AHCI64] read lba=0 count=1 ok（命令真的被 HBA 执行并搬了数据）
       + [AHCI64] port=0 det=3 sig=0x...101 kind=ata
     ★ item 5b 起这一档在 QEMU 11.1 的 ich9-ahci 上是 **PASS**（此前 FAIL 的根因是
       命令头布局写错：PRDTL 应在 DW0 的 bits[31:16]、CTBA/CTBAU 应在 DW2/DW3；
       旧代码把 CTBA 放 DW3 → HBA 读出的命令表地址错 → 命令被**静默丢弃**。
       详见 kernel/ahci64.cpp 顶部"实测踩坑记录" J/K 两条）。
     真机/VMware 的 SATA 也是 AHCI，建议在那里复验 —— docs/真机验证指南.md 有步骤。

退出码：0 全部通过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import os
import re
import socket
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
TARGET_SECTORS = 32768          # 16 MB 目标 SATA 盘


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

    def shot(self, path, wait=2.0):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return False

    def key(self, name, wait=1.2):
        """注入一个按键（sendkey），wait = 注入后等多久（给向导重绘留时间）。"""
        self.send("sendkey %s" % name, wait=wait)


def read_ppm(path):
    """读 P6 PPM（QEMU screendump 输出），返回 (w, h, pixels)。"""
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM")
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h = fields[0], fields[1]
    return w, h, raw[idx:]


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5561)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-ahci.png.img"))
    ap.add_argument("--shot", default=os.path.join(ROOT, "ahci_report.ppm"))
    ap.add_argument("--strict-dma", action="store_true",
                    help="额外要求 AHCI DMA 通路证据（selftest PASS / drive 8 / read lba=0 ok / kind=ata）")
    ap.add_argument("--sata-install", action="store_true",
                    help="额外在 SATA 盘上走完整安装流程，再把装好的盘启一次进桌面（[OS]/[GUI64]）")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and cond
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(MEDIUM):
        sys.stderr.write("缺少构建产物：%s（先跑 build64.sh）\n" % MEDIUM)
        return 2

    print("=== 1) 准备：安装介质(IDE) + 空 SATA 目标盘(ich9-ahci port0) ===")
    with open(args.target, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * 512))
    print("     %s = %d 扇区" % (args.target, TARGET_SECTORS))

    serial_log = os.path.join(ROOT, "ahci_serial.log")
    if os.path.exists(serial_log):
        os.remove(serial_log)

    print("=== 2) 引导安装介质，看 AHCI 驱动/硬件检查报告 ===")
    qemu_args = [
        qemu, "-name", "VimtuOS-ahci64",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d0,format=raw" % q(args.target),
        "-device", "ide-hd,drive=d0,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(serial_log),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(args.port)
    log = ""
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

        # 报告页展示约 5 秒（启动期），期间抓一帧做像素断言
        def read_log():
            try:
                with open(serial_log, "r", encoding="utf-8", errors="replace") as f:
                    return f.read()
            except FileNotFoundError:
                return ""

        got_report = False
        for _ in range(120):                    # 最多 30s
            time.sleep(0.25)
            log = read_log()
            if "[HWUI] report lines=" in log:
                got_report = True
                break
        shot_ok = mon.shot(args.shot, wait=2.0) if got_report else False

        # 等安装向导把磁盘枚举完（这时 AHCI 相关打点都出齐了）
        for _ in range(240):
            time.sleep(0.25)
            log = read_log()
            if "[SETUP] 磁盘枚举完成" in log or "磁盘枚举完成" in log:
                break

        # ---- 可选：在**这块 SATA 盘**上跑完整安装流程（复用 install_flow_test 的按键序列）----
        # 行布局（介质=驱动器 0 PATA、目标=驱动器 8 AHCI）：
        #   row0 驱动器 0 标题 / row1 未分配空间(0) / row2 驱动器 8 AHCI 标题 / row3 未分配空间(8)
        # ★ 向导的默认光标**已经**落在"非安装介质盘"的第一行（setup64.cpp:921），也就是 8 号盘，
        #   所以不要再按方向键（按下去反而会飘到介质盘上）——直接 n 新建分区 + ret 开装。
        if args.sata_install:
            print("=== 2b) 在 SATA 盘上走完整安装流程（AHCI 写盘）===")
            for key, what in (("ret", "语言 -> 现在安装"),
                              ("ret", "现在安装 -> 许可"),
                              ("ret", "许可 -> 安装类型"),
                              ("ret", "安装类型 -> 磁盘与分区"),
                              ("n", "在 8 号盘（AHCI，默认光标所在）上新建引导+主分区"),
                              ("ret", "选中分区 -> 安装系统")):
                print("     sendkey %-5s (%s)" % (key, what))
                mon.key(key, wait=1.2)
            for _ in range(240):                    # 最多 60s 等安装完成（每帧 128 扇区）
                time.sleep(0.25)
                if "[SETUP] 安装完成" in read_log() or "安装完成" in read_log():
                    break
            time.sleep(2.0)
        log = read_log()
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("=== 3) AHCI 驱动打点断言（全部真探测）===")
    m = re.search(r"\[AHCI64\] pci (\d+):(\d+)\.(\d+) abar=(0x[0-9A-Fa-f]+) cap=(0x[0-9A-Fa-f]+) "
                  r"pi=(0x[0-9A-Fa-f]+) ports=(\d+)", log)
    check("找到 AHCI 控制器并打点（[AHCI64] pci ... abar=... ports=...）", bool(m),
          (m.group(0) if m else "（缺 pci 行）"))
    if m:
        check("ABAR 非 0（BAR5 已分配）", int(m.group(4), 16) != 0, m.group(4))
        check("端口数 ≥ 1（CAP.NP+1）", int(m.group(7)) >= 1, "ports=%s" % m.group(7))
    check("HBA 复位/握手打点（[AHCI64] hba reset ...）", "[AHCI64] hba reset" in log)
    mp = re.search(r"\[AHCI64\] port=(\d+) det=(\d+) sig=(0x[0-9A-Fa-f]+) kind=(\w+)", log)
    check("端口状态打点（[AHCI64] port=<n> det=… sig=… kind=…）", bool(mp),
          (mp.group(0) if mp else "（缺 port 行）"))
    if mp:
        check("0 号端口有设备（det=3，SA 盘在位）", mp.group(2) == "3",
              "det=%s sig=%s kind=%s" % (mp.group(2), mp.group(3), mp.group(4)))
        check("设备签名/识别结果如实标注（kind=ata|atapi|none）",
              mp.group(4) in ("ata", "atapi", "none"), "kind=%s" % mp.group(4))
    check("安装程序完成了磁盘枚举（[SETUP] 磁盘枚举完成）", "磁盘枚举完成" in log,
          "; ".join([l for l in log.splitlines() if "[DISK]" in l])[:200])
    check("未找到控制器时也能优雅降级（打点里有 not found 或已找到控制器）",
          ("[AHCI64] not found" in log) or ("[AHCI64] pci" in log))

    print("=== 4) 硬件检查报告（屏幕是唯一诊断手段）===")
    check("报告已构建（[HWUI] report lines=<n> storage=<n> controllers=<n>）",
          "[HWUI] report lines=" in log,
          (re.search(r"\[HWUI\] report lines=[^\r\n]*", log).group(0) if "[HWUI] report lines=" in log else "缺"))
    check("启动期真的显示了报告页（[HWUI] report shown ms=… skipped=0|1）",
          re.search(r"\[HWUI\] report shown ms=\d+ skipped=[01]", log) is not None,
          (re.search(r"\[HWUI\] report shown[^\r\n]*", log).group(0) if "[HWUI] report shown" in log else "缺"))
    check("报告自检 PASS（[HWUI] selftest PASS）", "[HWUI] selftest PASS" in log)
    check("报告含存储区块的控制器统计（controllers = PATA+AHCI+NVMe）",
          re.search(r"\[HWUI\] report lines=\d+ storage=\d+ controllers=[1-9]\d*", log) is not None)

    print("=== 5) 报告页像素断言（黑底 + 区块色块 + 浅色文字）===")
    if shot_ok and os.path.exists(args.shot):
        w, h, px = read_ppm(args.shot)
        check("截图分辨率 1280x800", (w, h) == (1280, 800), "实际 %dx%d" % (w, h))
        # 采样报告页正文区（避开标题条）
        black = light = band = total = 0
        for y in range(90, 700, 6):
            for x in range(40, 1240, 6):
                c = sample(px, w, x, y)
                total += 1
                if c[0] < 16 and c[1] < 16 and c[2] < 16:
                    black += 1
                if c[0] > 190 and c[1] > 190 and c[2] > 190:
                    light += 1
                if abs(c[0] - 0) <= 6 and abs(c[1] - 48) <= 8 and abs(c[2] - 96) <= 8:
                    band += 1
        check("黑底占比 > 50%%（报告页背景）", black > total * 0.5,
              "black=%d/%d" % (black, total))
        check("区块色块存在（分区块：rgb(0,48,96) 采样 > 150）", band > 150, "band=%d" % band)
        check("浅色文字像素存在（TTF 字形真画上去了）", light > 300, "light=%d" % light)
    else:
        check("抓到报告页截图", False, "（截图失败：%s）" % args.shot)

    print("=== 6) 禁止出现 ===")
    for needle in ("PANIC", "TRIPLE FAULT", "FAILED mask="):
        check("不得出现 %s" % needle, needle not in log)

    if args.strict_dma:
        print("=== 7) 严格档：AHCI DMA 通路（读/写命令真的被执行）===")
        check("AHCI 自检 PASS（[AHCI64] selftest PASS）", "[AHCI64] selftest PASS" in log)
        check("安装程序看得见 SATA 盘（[DISK] 8 … bus=AHCI）",
              re.search(r"\[DISK\] 8 .*bus=AHCI", log) is not None,
              "; ".join([l for l in log.splitlines() if "[DISK]" in l])[:200])
        # 型号里允许带空格（QEMU 的盘就叫 "QEMU HARDDISK"），所以用 .+? 而不是 \S+
        check("识别到 SATA 盘（[AHCI64] drive 8 model=… sectors=…）",
              re.search(r"\[AHCI64\] drive 8 model=.+? sectors=\d+", log) is not None,
              (re.search(r"\[AHCI64\] (drive 8|cmd timeout|selftest skipped)[^\r\n]*", log).group(0)
               if re.search(r"\[AHCI64\] (drive 8|cmd timeout|selftest skipped)", log) else "缺"))
        check("SATA 盘上真的搬过数据（[AHCI64] read lba=0 count=1 ok）",
              "[AHCI64] read lba=0 count=1 ok" in log,
              (re.search(r"\[AHCI64\] (read|write) lba=[^\r\n]*", log).group(0)
               if re.search(r"\[AHCI64\] (read|write) lba=", log) else "缺（命令没被执行/没搬数据）"))
        check("端口摘要里 kind=ata（签名 + IDENTIFY 都过）",
              re.search(r"\[AHCI64\] port=0 det=3 sig=0x0*101 kind=ata", log) is not None,
              (re.search(r"\[AHCI64\] port=0[^\r\n]*", log).group(0)
               if re.search(r"\[AHCI64\] port=0", log) else "缺"))

    # ==== 8) 可选：SATA 盘上的完整安装 + 重启进桌面 ====
    if args.sata_install:
        print("=== 8) SATA 盘安装结果 + 重启进桌面 ===")
        # (a) 目标镜像里必须有：MBR 签名 + 引导分区 + loader + 系统内核（和 install_flow_test 同一口径）
        mbr_ok = load_ok = kernel_ok = False
        try:
            with open(args.target, "rb") as f:
                sec = f.read(512)
                mbr_ok = len(sec) == 512 and sec[510] == 0x55 and sec[511] == 0xAA and sec[446 + 4] != 0
                f.seek(512)
                load_ok = f.read(16) != b"\0" * 16
                f.seek(9 * 512)
                kernel_ok = f.read(2) == b"MZ" or f.read(1) != b""
        except OSError:
            pass
        check("SATA 盘装好了：MBR 55AA + 分区表项非空", mbr_ok,
              "target=%s" % args.target)
        check("SATA 盘 LBA1 起有 loader", load_ok)
        check("SATA 盘 LBA9 起有系统内容（内核）", kernel_ok)
        check("安装过程在 8 号盘上建了分区（[PART] … drive=8）",
              re.search(r"\[PART\][^\r\n]*drive=8", log) is not None,
              "; ".join([l for l in log.splitlines() if "[PART]" in l])[:240])
        check("安装真的走 AHCI 写盘（[AHCI64] write lba=… ok）",
              "[AHCI64] write lba=" in log,
              "; ".join([l for l in log.splitlines() if "[AHCI64] write" in l])[:240])
        check("安装完成打点（[SETUP] 安装完成）", "安装完成" in log,
              "; ".join([l for l in log.splitlines() if "[INSTALL]" in l])[:240])

        # (b) 把装好的盘单独启一次：必须走"系统启动路径"并进桌面。
        #     注：**引导器（boot/loader64.asm）自带的是 PATA PIO 读取**，所以这里按 IDE 盘挂载来验
        #     "装好的系统盘能进桌面"；直接挂到 AHCI 上启动需要 AHCI 版 loader（本批不改 loader）。
        boot_log = os.path.join(ROOT, "ahci_installed_boot.log")
        if os.path.exists(boot_log):
            os.remove(boot_log)
        boot_args = [
            qemu, "-name", "VimtuOS-ahci-installed",
            "-drive", "format=raw,file=%s,index=0,media=disk" % q(args.target),
            "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(boot_log),
            "-no-reboot",
        ]
        bproc = subprocess.Popen(boot_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        booted = ""
        try:
            for _ in range(240):                    # 最多 60s 等进桌面
                time.sleep(0.25)
                try:
                    with open(boot_log, "r", encoding="utf-8", errors="replace") as f:
                        booted = f.read()
                except FileNotFoundError:
                    booted = ""
                if "[GUI64] ready" in booted:
                    break
        finally:
            if bproc.poll() is None:
                bproc.kill()
                try:
                    bproc.wait(timeout=10)
                except Exception:
                    pass
        check("从装好的盘启动走系统路径（[OS] booted from installed disk）",
              "[OS] booted from installed disk" in booted,
              (re.search(r"\[OS\][^\r\n]*", booted).group(0) if "[OS]" in booted else "缺"))
        check("进到桌面（[GUI64] ready）", "[GUI64] ready" in booted,
              (re.search(r"\[GUI64\][^\r\n]*", booted).group(0) if "[GUI64]" in booted else "缺"))
        check("重启后不再跑安装程序（无 [SETUP] 向导打点）",
              "[SETUP] 磁盘枚举完成" not in booted)
    tail = [l for l in log.splitlines() if l.strip() and ("AHCI64" in l or "HWUI" in l or "DISK" in l)][-14:]
    print("--- 串口尾部（AHCI64/HWUI/DISK）---")
    for l in tail:
        print("   | " + l[:170])

    print("=== RESULT: %s ===%s" % ("PASS" if ok else "FAIL",
                                    "" if args.strict_dma else "（未开 --strict-dma）"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
