#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/usb_boot_both_fw_test.py - "U 盘形态" 安装介质在 **UEFI** 与 **BIOS** 双固件下的启动验证

被验的组合：`vimtu64-64.iso` 当成一块**磁盘/U 盘**（不是光驱）被引导 —— 也就是
"用 dd/Rufus 把 ISO 写进 U 盘"之后真机走的那条路。三种挂法都测：

  A) UEFI 侧：QEMU + **OVMF**，ISO 挂 **usb-storage（UHCI，bootindex=1）** —— 真 U 盘的总线形态
     （另外跑一档 "ISO 当 IDE 盘"，覆盖不支持 USB 启动的固件/机器）
  B) BIOS 侧：SeaBIOS，ISO 挂 **usb-storage（UHCI，bootindex=1）** —— SeaBIOS 的 USB MSC 驱动
  C) BIOS 侧对照：ISO 直接当 **IDE 盘**（`iso64_usb_test.py --mode ata` 已覆盖，这里再确认一遍）

验收（两个方向都必须**进到安装向导第一屏**，不只是到引导器）：
  * UEFI：自研 BOOTX64.EFI 桩（S12345J）-> 平铺长模式引导器（U: 打点）-> 从 ESP 读
    KERNEL64.BIN/SYSTEM.IMG -> [LM64] ENTERED LONG MODE -> [SETUP] 介质描述符 kind=2
    -> [SETUP] 磁盘枚举完成；无 PANIC/三重故障
  * BIOS：hybrid MBR -> CDISO 桩"硬盘分支"（I:hdd mode）-> 搬内核/载荷进高内存
    -> loader64（L:media=ram）-> [LM64] ENTERED LONG MODE -> 安装向导第一屏

用法：python tests/usb_boot_both_fw_test.py [--qemu ...] [--mode all|uefi-usb|uefi-disk|bios-usb|bios-disk]
退出码：0 全部通过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import os
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import proc64_test as p64          # noqa: E402  （qemu/OVMF 查找 + 变量卷复用）

ISO = os.path.join(ROOT, "vimtu64-64.iso")
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "三重故障"]

WIZARD_READY = "磁盘枚举完成"          # 安装向导第一屏（磁盘列表画完）


def sha256(path):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def q(p):
    return p.replace("\\", "/")


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def run_case(qemu, tag, extra, serial, ovmf=None, timeout=300):
    """启动一次，轮询串口到"安装向导第一屏"或超时。返回 (日志, qemu 提前退出?)。"""
    if os.path.exists(serial):
        os.remove(serial)
    args = [qemu, "-name", "VimtuOS-" + tag, "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(serial), "-no-reboot"] + extra
    if ovmf:
        args += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % q(ovmf),
                 "-drive", "if=pflash,format=raw,unit=1,file=%s" % q(p64._ovmf_vars())]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    early = False
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = slog(serial)
            if WIZARD_READY in s:
                time.sleep(1.0)
                break
            if proc.poll() is not None:
                early = True
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return slog(serial), early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--case", choices=("all", "uefi-usb", "uefi-disk", "bios-usb", "bios-disk"),
                    default="all")
    ap.add_argument("--port", type=int, default=5564)
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(ISO):
        sys.stderr.write("缺少安装介质 %s（先跑 build64.sh）\n" % ISO)
        return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    # 目标盘：让向导有"非介质盘"可枚举（AHCI 0 号口，与本次验收无关，只为磁盘列表非空）
    target = os.path.join(ROOT, "target-usb-bothfw.img")
    with open(target, "wb") as f:
        f.write(b"\0" * (32768 * 512))
    # ★ 介质指纹：真 U 盘/光驱都是只读的，安装介质在整轮引导里**一个字节都不该变**。
    #   （这条断言的价值：曾经 FAT16 自检误把 drive 0 的介质盘格式化掉，串口侧看不出来。）
    iso_sha_before = sha256(ISO)
    # ISO 挂 USB 存储：UHCI（SeaBIOS/OVMF 支持最完整）+ bootindex=1（否则固件不会先去试它）
    def usb_extra():
        return ["-drive", "format=raw,file=%s,if=none,id=med" % q(ISO),
                "-device", "piix3-usb-uhci,id=uhci",
                "-device", "usb-storage,drive=med,bus=uhci.0,bootindex=1",
                "-device", "ich9-ahci,id=ahci",
                "-drive", "file=%s,if=none,id=d0,format=raw" % q(target),
                "-device", "ide-hd,drive=d0,bus=ahci.0",
                "-boot", "order=c"]

    def disk_extra():
        return ["-drive", "format=raw,file=%s,index=0,media=disk" % q(ISO),
                "-drive", "format=raw,file=%s,index=1,media=disk" % q(target),
                "-boot", "order=c"]

    ovmf = p64.find_ovmf()

    def uefi_checks(tag, log):
        print("   --- %s：UEFI 引导链打点 ---" % tag)
        chain = "S12345J" in log
        check("[%s] 自研 PE 桩按步走完（S12345J 打点）" % tag, chain)
        check("[%s] UEFI 引导器启动（U:==== 前缀）" % tag, "U:==== Vimtu64 UEFI" in log)
        check("[%s] 从 ESP 读出 KERNEL64.BIN / SYSTEM.IMG" % tag,
              "U:loaded KERNEL64.BIN" in log and "U:loaded SYSTEM.IMG" in log)
        check("[%s] GOP 线性帧缓冲 + ExitBootServices + 高半区映射" % tag,
              "U:gop " in log and "U:ExitBootServices ok" in log and "U:high half check ok" in log)
        check("[%s] 进长模式内核（[LM64] ENTERED LONG MODE）" % tag,
              "[LM64] ENTERED LONG MODE" in log)
        check("[%s] 安装向导读到介质描述符 kind=2（RAM 形态）" % tag,
              "[SETUP] 介质描述符 OK kind=2" in log)
        check("[%s] ★ 到达安装向导第一屏（磁盘枚举完成）" % tag, WIZARD_READY in log)
        check("[%s] 没走 BIOS 桩（无 I:boot dl=）" % tag, "I:boot dl=" not in log)
        check("[%s] 无 PANIC/三重故障" % tag, not any(x in log for x in FORBIDDEN))
        tail = [l for l in log.splitlines() if l.strip()][-8:]
        for l in tail:
            print("      | " + l[:140])

    def bios_checks(tag, log, via_usb):
        print("   --- %s：BIOS 引导链打点 ---" % tag)
        check("[%s] hybrid MBR 载入引导桩（I:boot dl=）" % tag, "I:boot dl=" in log)
        check("[%s] 桩进硬盘/U 盘分支（I:hdd mode (USB/disk)）" % tag,
              "I:hdd mode (USB/disk)" in log)
        check("[%s] 桩读 loader64 并跳转（I:jump loader64）" % tag, "I:jump loader64" in log)
        check("[%s] loader64 认出 RAM 介质（L:media=ram）" % tag, "L:media=ram" in log)
        check("[%s] 进长模式（[LM64] ENTERED LONG MODE）" % tag,
              "[LM64] ENTERED LONG MODE" in log)
        check("[%s] 安装向导读到介质描述符 kind=2" % tag, "[SETUP] 介质描述符 OK kind=2" in log)
        check("[%s] ★ 到达安装向导第一屏（磁盘枚举完成）" % tag, WIZARD_READY in log)
        check("[%s] 无 PANIC/三重故障" % tag, not any(x in log for x in FORBIDDEN))
        tail = [l for l in log.splitlines() if l.strip()][-8:]
        for l in tail:
            print("      | " + l[:140])

    if args.case in ("all", "uefi-usb"):
        print("=== A) UEFI（OVMF）从 **USB 存储** 启动 ISO ===")
        if not ovmf:
            print("  [SKIP] 找不到 OVMF（%s）" % ", ".join(p64.OVMF_CANDIDATES))
        else:
            log, early = run_case(qemu, "usb-bothfw-uefi-usb", usb_extra(),
                                  os.path.join(ROOT, "usb_bothfw_uefi_usb.log"), ovmf=ovmf)
            uefi_checks("uefi-usb", log)

    if args.case in ("all", "uefi-disk"):
        print("=== B) UEFI（OVMF）从 **IDE 盘形态** 的 ISO 启动（对照档：不支持 USB 启动的机器）===")
        if not ovmf:
            print("  [SKIP] 找不到 OVMF（%s）" % ", ".join(p64.OVMF_CANDIDATES))
        else:
            log, early = run_case(qemu, "usb-bothfw-uefi-disk", disk_extra(),
                                  os.path.join(ROOT, "usb_bothfw_uefi_disk.log"), ovmf=ovmf)
            uefi_checks("uefi-disk", log)

    if args.case in ("all", "bios-usb"):
        print("=== C) BIOS（SeaBIOS）从 **USB 存储** 启动 ISO ===")
        log, early = run_case(qemu, "usb-bothfw-bios-usb", usb_extra(),
                              os.path.join(ROOT, "usb_bothfw_bios_usb.log"))
        bios_checks("bios-usb", log, via_usb=True)

    if args.case in ("all", "bios-disk"):
        print("=== D) BIOS（SeaBIOS）从 **IDE 盘形态** 的 ISO 启动（hybrid MBR 等价路径）===")
        log, early = run_case(qemu, "usb-bothfw-bios-disk", disk_extra(),
                              os.path.join(ROOT, "usb_bothfw_bios_disk.log"))
        bios_checks("bios-disk", log, via_usb=False)

    print("=== 介质零改动断言（★ 关键：真 U 盘/光驱是只读的）===")
    iso_sha_after = sha256(ISO)
    check("安装介质 vimtu64-64.iso 字节未被改动",
          iso_sha_before == iso_sha_after,
          "%s -> %s" % (iso_sha_before[:12], iso_sha_after[:12]))

    print("=== USB × 双固件 RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
