#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/uefi64_install_test.py - 在 **VMware（UEFI 固件）** 里验完整条 UEFI 引导 + 安装

和 tests/vmware_install_test.py（BIOS）的区别：这份跑的是 UEFI 路径 —— 全 64 位：
    VMware EFI 固件（本身就是长模式）
      → 从 ISO 的 El Torito(platform 0xEF) 读 FAT32 的 ESP 附加分区（48MB 真 FAT32）
      → 执行 EFI/BOOT/BOOTX64.EFI（我们自研的 PE32+，不用 gnu-efi）
      → 它用 GOP 拿帧缓冲参数、用 SimpleFileSystem 读 KERNEL64.BIN / SYSTEM.IMG 到内存
      → 写 BootInfo(0x1000) / E820(0x2000) / 介质描述符(0x0F00, kind=2)
      → ExitBootServices → 自己建 0..512GB 恒等映射页表 → 载 GDT → jmp 0x100000
      → 内核 entry64 → kmain64（安装向导）

验收：
  1) 串口有 "U:" 前缀的 UEFI 引导标记（证明走的是 UEFI 而不是 BIOS 桩）
  2) **没有** BIOS 引导桩的 "I:boot dl=" 标记（证明没走 MBR 那条路）
  3) 安装程序读到介质描述符 kind=2，走完向导：新建分区 → 安装 100% → 自动重启
  4) 目标盘字节级：MBR 55AA + P1(0xEF,活动,65+32519) + P2(0x07,32584..尾)
     LBA1..64 = loader64.bin、LBA65.. = kernel64_os.bin 逐字节一致
用法：python tests/uefi64_install_test.py
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import time

# Windows 控制台默认 GBK：本脚本会打印固件转储里的非 ASCII 字符（替换符 U+FFFD），
# 不强制 UTF-8 会以 UnicodeEncodeError 假失败（与测试逻辑无关，纯输出编码问题）。
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BASE = r"C:\Users\fanqi\Desktop\新建文件夹"
TESTDIR = os.path.join(BASE, "v64-uefi-test")
VMX = os.path.join(TESTDIR, "vimtu64-install-test.vmx")
SERIAL = os.path.join(TESTDIR, "serial-install.log")
TARGET = os.path.join(TESTDIR, "target.img")
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
KEY_TCP_PORT = 4557
TARGET_SECTORS = 57343


def vmrun(*args, timeout=240):
    return subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)


def stop_all():
    r = vmrun("list")
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        if line.strip().endswith(".vmx"):
            vmrun("stop", line.strip(), "hard")
    time.sleep(1)


def start_vm(vmx, tries=6):
    for i in range(tries):
        r = vmrun("start", vmx, "nogui")
        if r.returncode == 0:
            return True
        time.sleep(4)
    return False


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except FileNotFoundError:
        return ""


def wait_for(path, needle, seconds, what):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if needle in read_text(path):
            return True
        time.sleep(1.0)
    print("   [!!] 等不到 %s（%s）" % (needle, what))
    return False


def open_key_channel(port=KEY_TCP_PORT, seconds=120):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            return socket.create_connection(("127.0.0.1", port), timeout=5)
        except OSError:
            time.sleep(2)
    return None


def send_keys(sock, keys, dwell=1.6):
    """往 COM2 写字节：回车/普通键由客人侧 setup64_keys.h 翻译。"""
    for k in keys:
        sock.sendall(k.encode())
        time.sleep(dwell)


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
    ap.add_argument("--no-build-vm", action="store_true", help="不重建 VM（沿用上次）")
    args = ap.parse_args()

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    print("=== 1) 准备 UEFI 测试 VM（firmware=efi，ISO 挂光驱）===")
    if not args.no_build_vm:
        r = subprocess.run([sys.executable, os.path.join(HERE, "vmware_make_vm.py"), "--iso", "--uefi"],
                           cwd=ROOT, capture_output=True, timeout=300)
        out = r.stdout.decode("utf-8", "replace")
        print("   " + "\n   ".join(out.strip().splitlines()[-3:]))
        if r.returncode != 0:
            print(r.stderr.decode("utf-8", "replace")[-600:])
            return 2

    print("=== 2) 启动 VM 并等 UEFI 引导进安装界面 ===")
    stop_all()
    if os.path.exists(SERIAL):
        os.remove(SERIAL)
    if not start_vm(VMX):
        print("   [!!] vmrun start 失败")
        return 2
    if not wait_for(SERIAL, "U:====", 240, "UEFI 引导程序启动"):
        print(read_text(SERIAL)[-1200:])
        stop_all()
        return 2
    got_ready = wait_for(SERIAL, "磁盘枚举完成", 300, "安装向导就绪")
    # ★ 引导阶段日志快照（见下面第 3 节说明：装完自动重启会截断串口日志文件）
    boot_snapshot = read_text(SERIAL)

    print("=== 3) UEFI 引导链断言（全 64 位）===")
    # ★ 必须用**快照**：装完会自动重启，而 VMware 在 VM 重启时会**截断串口日志文件**，
    #   于是"引导阶段"的证据会被后一次启动的输出冲掉（我们在这里踩过：安装成功后
    #   反而报"UEFI 引导标记缺失"）。所以引导阶段的日志在 wait_for 通过后立刻存下来。
    log = boot_snapshot if boot_snapshot else read_text(SERIAL)
    check("自研 BOOTX64.EFI 启动（U: 前缀标记）", "U:==== Vimtu64 UEFI" in log)
    check("挂载 ESP（FAT32，48MB 真 FAT32 卷）并读出内核",
          "U:loaded KERNEL64.BIN" in log and "U:high half check ok" in log)
    check("从 ESP 读出内核", "U:loaded KERNEL64.BIN" in log)
    check("从 ESP 读出载荷", "U:loaded SYSTEM.IMG" in log)
    # ★ v0.10.2：按**文件真实大小**读，并把真实扇区数写进介质描述符。
    #   旧代码把 PAYLOAD_BYTES 写死成 8073 扇区（4.13MB），装出来的盘主分区整块是零，
    #   而当时的断言只比 LBA1..64 / LBA65..（都在前 4MB 内）——全是绿的。
    _sysimg = os.path.join(ROOT, "build64", "system.img")
    _sysbytes = os.path.getsize(_sysimg)
    _msize = re.search(r"U:size SYSTEM\.IMG = (\d+)", log)
    check("引导器报出 SYSTEM.IMG 的真实字节数（U:size SYSTEM.IMG = %d）" % _sysbytes,
          _msize is not None and int(_msize.group(1)) == _sysbytes,
          ("%s" % (_msize.group(0) if _msize else "缺")) if _msize else "缺")
    _mpsec = re.search(r"U:payload sectors=(\d+)", log)
    check("介质描述符报出真实扇区数（U:payload sectors=%d = ceil(bytes/512)）"
          % ((_sysbytes + 511) // 512),
          _mpsec is not None and int(_mpsec.group(1)) == (_sysbytes + 511) // 512,
          ("%s" % (_mpsec.group(0) if _mpsec else "缺")) if _mpsec else "缺")
    check("GOP 拿到线性帧缓冲", "U:gop " in log and "lfb=" in log)
    check("ExitBootServices 成功", "U:ExitBootServices ok" in log)
    check("高半区直映（内核搬高半区后固件仍能启动它）",
          ("U:direct map" in log) or ("U:high half map" in log))
    check("进了长模式内核（[LM64] 标记）", "[LM64] ENTERED LONG MODE" in log)
    check("★ 没有走 BIOS 引导桩（无 I:boot dl=）", "I:boot dl=" not in log)
    check("内核读到介质描述符 kind=2", "[SETUP] 介质描述符 OK kind=2" in log)

    print("=== 4) 安装向导（COM2 按键通道）===")
    sock = open_key_channel()
    check("COM2 按键通道已连上", sock is not None)
    if sock:
        try:
            send_keys(sock, ["\r", "\r", "\r", "\r"])       # 语言→现在安装→许可→类型
            send_keys(sock, ["n"], dwell=2.4)                # 新建分区
            send_keys(sock, ["\r"], dwell=2.0)               # 安装系统
        finally:
            sock.close()
        wait_for(SERIAL, "[SETUP] 自动重启", 600, "安装完成并自动重启")
        log = read_text(SERIAL)

    print("=== 5) 安装动作断言 ===")
    check("向导就绪", got_ready or "磁盘枚举完成" in log)
    check("新建分区表 OK", "[PART] 新建分区表 OK" in log)
    check("安装完成（写出扇区数）", "[INSTALL] 完成：已写" in log)
    check("界面 100%", "[SETUP] 安装完成 100%" in log)
    check("装完自动重启", "[SETUP] 自动重启" in log)
    tail = [l for l in log.splitlines() if l.strip()][-14:]
    print("--- 串口尾部 ---")
    for l in tail:
        print("   | " + l[:150])

    stop_all()

    print("=== 6) 目标盘字节断言 ===")
    p1, p2 = check_mbr(TARGET)
    check("MBR 55AA 且分区1 存在", p1 is not None, "%s" % (p1,))
    if p1:
        check("P1 = 引导分区 0xEF 活动，65 + 32519", p1["type"] == 0xEF and p1["boot"]
              and p1["start"] == 65 and p1["sectors"] == 32519, "%s" % (p1,))
    check("目标盘大小 = system.img 大小（%d 字节）" % os.path.getsize(os.path.join(ROOT, "build64", "system.img")),
          os.path.getsize(TARGET) == os.path.getsize(os.path.join(ROOT, "build64", "system.img")),
          "%d vs %d" % (os.path.getsize(TARGET),
                        os.path.getsize(os.path.join(ROOT, "build64", "system.img"))))
    check("P2 = 主分区 0x07，32584..盘尾", p2 is not None and p2["type"] == 0x07
          and p2["start"] == 32584 and p2["sectors"] == TARGET_SECTORS - 32584, "%s" % (p2,))
    loader_ref = open(os.path.join(ROOT, "build64", "loader64.bin"), "rb").read()
    os_ref = open(os.path.join(ROOT, "build64", "kernel64_os.bin"), "rb").read()
    check("LBA1..64 = loader64.bin 逐字节一致",
          read_at(TARGET, 1, len(loader_ref)) == loader_ref, "%d 字节" % len(loader_ref))
    check("LBA65.. = kernel64_os.bin 逐字节一致",
          read_at(TARGET, 65, len(os_ref)) == os_ref, "%d 字节" % len(os_ref))
    # ★ v0.10.2：**载荷真的整块写进去了吗**。上面两条都在前 4.13 MB 内，看不出截断。
    #   这里拿 build64/system.img 当参照，比到主分区起点（LBA 32584，VimtuFS2 超级块）和盘尾。
    sys_ref = open(os.path.join(ROOT, "build64", "system.img"), "rb").read()
    mid_lba = (32584 + TARGET_SECTORS) // 2
    for lba, name in ((32584, "主分区起点"), (mid_lba, "主分区中段"), (TARGET_SECTORS - 2, "盘尾")):
        want = sys_ref[lba * 512:(lba + 1) * 512]
        got = read_at(TARGET, lba, 512)
        check("LBA%d（%s）= system.img 同偏移逐字节一致（载荷没有被截断）" % (lba, name),
              got == want, "got[:8]=%s want[:8]=%s" % (got[:8].hex(), want[:8].hex()))

    print("=== UEFI RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
