#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/uefi_install64_test.py —— **UEFI 端到端安装验收**（v0.10.8 新增）。

用户报的真 Bug：用 ISO 装好系统后，UEFI 模式起不来硬盘。
本脚本就是那条链路的**可复现证据链**：

  1) VMware 建夹具：ide0:0 = 大目标盘（默认 256 MiB），ide1:0 = vimtu64-64.iso
     固件 = **efi**（这一步同时验证"ISO 的 UEFI 引导链：El Torito 0xEF → ESP → BOOTX64.EFI"）
  2) 走完安装向导（COM2 网络串口送按键，与 vmware_install_test.py 同一套通道）
  3) 停机后**逐字节体检目标盘**：
        MBR 三项（0xEF 引导区@65 / 0x07 主分区 / 0xEF ESP@盘尾）
        ESP 起点必须是合法 FAT32 BPB（EB.. + "FAT32   " + 0x55AA）
        盘尾必须有备份 GPT（"EFI PART"）
  4) **重启**：同一块目标盘、固件 = efi，必须自己起来进系统
        证据 = 串口 `[OS] booted from installed disk` / `[GUI64] ready`
  5) 全程无 PANIC / TRIPLE FAULT

为什么目标盘必须 >= ~60 MiB：
  kernel/part64.cpp 的 part_esp_geometry64() 要求
      32584 + 16384(主分区下限) + 98304(48MB ESP) + 33(备份 GPT) = 122,730 扇区
  小于这个数就**不建 ESP**（老 MBR-only 布局），UEFI 自然认不出硬盘 ——
  旧夹具用的 28 MiB 目标盘正好落在这一侧，所以以前的安装测试从来没暴露这个缺陷。

用法：
  py -3 tests/uefi_install64_test.py [--target-mb 256] [--keep] [--skip-install]
退出码：0 全通过 / 1 有失败 / 2 环境不可用
"""
import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SCRATCH = os.environ.get("PI_SCRATCH_DIR", os.path.join(ROOT, "build64"))
WORK = os.path.join(SCRATCH, "uefi-e2e")
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
ISO = os.path.join(ROOT, "vimtu64-64.iso")
KEY_TCP_PORT = 5907
VNC_PORT = 5908

TARGET = os.path.join(WORK, "target.img")
SER_INSTALL = os.path.join(WORK, "serial-install.log")
SER_BOOT = os.path.join(WORK, "serial-boot.log")
VMX_INSTALL = os.path.join(WORK, "install.vmx")
VMX_BOOT = os.path.join(WORK, "boot.vmx")

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass


# ---------------------------------------------------------------- VMware
def vmrun(*args, timeout=180):
    try:
        r = subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -1, "timeout"


def stop_all():
    for p in (VMX_INSTALL, VMX_BOOT):
        if os.path.exists(p):
            vmrun("stop", p, "hard", timeout=60)
    time.sleep(1)
    _rc, out = vmrun("list")
    for line in out.splitlines():
        if ".vmx" in line and "uefi-e2e" in line.lower():
            vmrun("stop", line.strip(), "hard", timeout=60)
    subprocess.run(["taskkill", "/F", "/IM", "vmware-vmx.exe"], capture_output=True)
    time.sleep(2)


def vmdk_descriptor(path, flat, sectors, cid):
    txt = ('# Disk DescriptorFile\nversion=1\nCID=%08x\nparentCID=ffffffff\n'
           'createType="monolithicFlat"\n\n# Extent description\n'
           'RW %d FLAT "%s" 0\n\n# The Disk Data Base\n#DDB\n'
           'ddb.adapterType = "ide"\nddb.geometry.cylinders = "%d"\n'
           'ddb.geometry.heads = "16"\nddb.geometry.sectors = "63"\n'
           'ddb.virtualHWVersion = "7"\n' % (cid, sectors, flat, sectors // (16 * 63)))
    open(path, "w", encoding="ascii", newline="\n").write(txt)


def vmx_text(title, disk, serial, cdrom=None, key_pipe=False, vnc_port=None):
    t = ['#!/usr/bin/vmware', '.encoding = "UTF-8"', 'config.version = "8"',
         'virtualHW.version = "7"', 'displayName = "%s"' % title,
         'guestOS = "other-64"', 'msg.autoAnswer = "TRUE"', 'numvcpus = "1"',
         'memsize = "1024"', 'svga.vramSize = "134217728"',
         'ide0:0.present = "TRUE"', 'ide0:0.deviceType = "disk"',
         'ide0:0.fileName = "%s"' % disk, 'ide0:0.startConnected = "TRUE"',
         'ide0:1.present = "FALSE"']
    if cdrom:
        t += ['ide1:0.present = "TRUE"', 'ide1:0.deviceType = "cdrom-image"',
              'ide1:0.fileName = "%s"' % cdrom.replace("\\", "\\\\"),
              'ide1:0.startConnected = "TRUE"', 'bios.bootOrder = "cdrom,hdd"']
    else:
        t += ['ide1:0.present = "FALSE"']
    t += ['floppy0.present = "FALSE"', 'ethernet0.present = "FALSE"',
          'usb.present = "FALSE"', 'sound.present = "FALSE"', 'vmci0.present = "FALSE"',
          'tools.syncTime = "FALSE"', 'tools.install.state = "none"',
          'tools.upgrade.policy = "manual"', 'mks.enable3d = "FALSE"',
          'isolation.tools.hgfs.disable = "TRUE"',
          'firmware = "efi"', 'uefi.secureBoot = "FALSE"', 'bios.bootDelay = "2000"',
          'serial0.present = "TRUE"', 'serial0.fileType = "file"',
          'serial0.fileName = "%s"' % serial.replace("\\", "\\\\"),
          'serial0.tryNoRxLoss = "FALSE"', 'serial0.startConnected = "TRUE"']
    if key_pipe:
        t += ['serial1.present = "TRUE"', 'serial1.fileType = "network"',
              'serial1.fileName = "tcp://127.0.0.1:%d"' % KEY_TCP_PORT,
              'serial1.mode = "server"', 'serial1.startConnected = "TRUE"']
    if vnc_port:
        t += ['RemoteDisplay.vnc.enabled = "TRUE"',
              'RemoteDisplay.vnc.port = "%d"' % vnc_port]
    return "\n".join(t) + "\n"


def prepare(target_mb):
    os.makedirs(WORK, exist_ok=True)
    stop_all()
    for n in os.listdir(WORK):
        if n.endswith((".lck", ".vmem")) or n in ("nvram",):
            p = os.path.join(WORK, n)
            shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else os.remove(p)
    sectors = target_mb * 2048
    with open(TARGET, "wb") as f:            # 全零目标盘
        f.truncate(sectors * 512)
    vmdk_descriptor(os.path.join(WORK, "target.vmdk"), "target.img", sectors, 0x44444444)
    for p in (SER_INSTALL, SER_BOOT):
        open(p, "wb").close()
    open(VMX_INSTALL, "w", encoding="utf-8", newline="\n").write(
        vmx_text("VimtuOS UEFI install", "target.vmdk", SER_INSTALL,
                 cdrom=ISO, key_pipe=True, vnc_port=VNC_PORT))
    open(VMX_BOOT, "w", encoding="utf-8", newline="\n").write(
        vmx_text("VimtuOS UEFI boot", "target.vmdk", SER_BOOT))
    print("[fixture] 目标盘 %d MiB（%d 扇区）firmware=efi  ISO=%s" %
          (target_mb, sectors, os.path.basename(ISO)))
    return sectors


def start_vm(vmx):
    subprocess.Popen([VMRUN, "-T", "ws", "start", vmx, "gui"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_text(path):
    for _ in range(5):
        try:
            with open(path, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            time.sleep(0.3)
    return ""


def wait_mark(path, needle, seconds, what):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if needle in read_text(path):
            print("   [ok] %s（%.1fs）" % (what, time.time() - t0))
            return True
        time.sleep(1.0)
    print("   [!!] 等不到 %s（%r，%.0fs）" % (what, needle, seconds))
    tail = [l for l in read_text(path).splitlines() if l.strip()][-12:]
    for l in tail:
        print("      | " + l[:150])
    return False


def key_socket(seconds=120):
    t0 = time.time()
    while time.time() - t0 < seconds:
        try:
            return socket.create_connection(("127.0.0.1", KEY_TCP_PORT), timeout=5)
        except OSError:
            time.sleep(0.5)
    return None


def send_keys(sock, keys, dwell=1.4):
    for k in keys:
        try:
            sock.sendall(bytes([k]))
        except OSError as e:
            print("   [!!] 送键失败 0x%02X：%s" % (k, e))
            return False
        time.sleep(dwell)
    return True


# ---------------------------------------------------------------- 体检
def check_mbr(path):
    with open(path, "rb") as f:
        sec = f.read(512)
    if sec[510:512] != b"\x55\xaa":
        return None
    parts = []
    for i in range(4):
        e = sec[446 + 16 * i:446 + 16 * (i + 1)]
        if e[4] == 0 and e[8:12] == b"\0\0\0\0":
            continue
        parts.append({"idx": i, "boot": e[0] == 0x80, "type": e[4],
                      "start": struct.unpack("<I", e[8:12])[0],
                      "sectors": struct.unpack("<I", e[12:16])[0]})
    return parts


def fat32_at(path, lba):
    with open(path, "rb") as f:
        f.seek(lba * 512)
        bs = f.read(512)
    if len(bs) < 512:
        return None
    ok = (bs[510:512] == b"\x55\xaa" and bs[0] in (0xEB, 0xE9)
          and struct.unpack("<H", bs[11:13])[0] == 512 and 1 <= bs[13] <= 128
          and struct.unpack("<H", bs[14:16])[0] >= 32 and bs[16] in (1, 2)
          and struct.unpack("<I", bs[36:40])[0] > 0 and bs[82:90] == b"FAT32   ")
    return {"ok": ok, "oem": bs[3:11], "fstype": bs[82:90], "sig": bs[510:512].hex()}


def gpt_backup(path):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(size - 512)
        h = f.read(512)
    return {"sig": h[0:8], "entries_lba": struct.unpack("<Q", h[72:80])[0]
            if h[0:8] == b"EFI PART" else None}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target-mb", type=int, default=256)
    ap.add_argument("--keep", action="store_true", help="跑完不停 VM（调试）")
    ap.add_argument("--skip-install", action="store_true", help="复用已装好的目标盘，只验 UEFI 引导")
    a = ap.parse_args()

    results = []

    def chk(name, cond, detail=""):
        results.append((bool(cond), name, detail))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    if not os.path.exists(ISO):
        print("[env] 缺 ISO：%s" % ISO)
        return 2

    print("=== 0) 夹具（firmware=efi，目标盘 %d MiB）===" % a.target_mb)
    sectors = prepare(a.target_mb) if not a.skip_install else a.target_mb * 2048

    if not a.skip_install:
        print("=== 1) UEFI 起 ISO 安装介质 ===")
        start_vm(VMX_INSTALL)
        if not wait_mark(SER_INSTALL, "磁盘枚举完成", 300, "安装向导就绪(ISO/UEFI 引导成功)"):
            chk("ISO 在 UEFI 下引导进安装程序", False, "等不到向导")
            stop_all()
            return 1
        chk("ISO 在 UEFI 下引导进安装程序", True)

        print("=== 2) COM2 送键走完向导 ===")
        sock = key_socket()
        if sock is None:
            chk("COM2 按键通道", False)
            stop_all()
            return 1
        try:
            send_keys(sock, [0x0D, 0x0D, 0x0D, 0x0D], dwell=1.3)   # 语言/现在安装/许可/类型
            send_keys(sock, [0x0D], dwell=1.3)                      # 用户与时区（默认值）
            send_keys(sock, [ord("n")], dwell=2.0)                  # 新建分区表
            send_keys(sock, [0x0D], dwell=1.2)                      # 开始安装
        finally:
            sock.close()

        print("=== 3) 等安装完成 ===")
        done = wait_mark(SER_INSTALL, "[INSTALL] 完成：已写", 300, "安装完成")
        chk("安装写出扇区", done)
        # ★ 顺序：user.conf 在"[SETUP] 安装完成 100%"之后才写（挂卷槽 + mkdir + write），
        #   所以在"[INSTALL] 完成：已写"这个时点立刻读日志会**太早**（踩过：UEFI 路径假红）。
        #   这里改成等事件，再断言写入行本身带 write=NNN 且没有 SKIP/BUILD FAILED。
        uc_ok = wait_mark(SER_INSTALL, "[SETUP] user.conf write", 120, "写入 /etc/user.conf")
        uc_log = read_text(SER_INSTALL)
        chk("安装收尾写入 /etc/user.conf（新增页收集的用户名/时区）", uc_ok and ("path=/etc/user.conf" in uc_log),
            "write 行见串口")
        chk("user.conf 写入无失败（无 SKIP / 无 BUILD FAILED）",
            ("[SETUP] user.conf SKIP" not in uc_log) and ("user.conf BUILD FAILED" not in uc_log))
        wait_mark(SER_INSTALL, "[SETUP] 自动重启", 90, "完成后自动重启")
        if not a.keep:
            stop_all()
        log = read_text(SER_INSTALL)
        chk("无 PANIC", "PANIC" not in log and "TRIPLE FAULT" not in log.upper())
        chk("安装器写了 ESP（[INSTALL] esp 打点）",
            ("esp=" in log) or ("[INSTALL] esp" in log) or ("ESP" in log.upper()),
            "见串口 esp 相关行")
        for l in log.splitlines():
            if "esp" in l.lower() or "ESP" in l:
                print("      | " + l[:150])

    print("=== 4) 目标盘体检 ===")
    parts = check_mbr(TARGET)
    chk("MBR 55AA 且至少 2 个分区项", bool(parts) and len(parts) >= 2,
        "%s" % [(p["idx"], hex(p["type"]), p["start"], p["sectors"]) for p in (parts or [])])
    esp = None
    for p in (parts or []):
        if p["type"] == 0xEF and p["start"] > 36000:      # 盘尾那个才是 FAT32 ESP
            esp = p
    chk("MBR 里有盘尾 0xEF 分区项（ESP）", esp is not None,
        "esp=%s 盘扇区=%d" % (esp, sectors))
    if esp:
        info = fat32_at(TARGET, esp["start"])
        chk("ESP 起点是合法 FAT32 引导扇区（BPB+55AA）", info and info["ok"],
            "%s" % (info,))
    g = gpt_backup(TARGET)
    chk("盘尾有备份 GPT（EFI PART）", g["sig"] == b"EFI PART", "%s" % (g,))

    if a.keep:
        print("[keep] 不重启，目标盘留在 %s" % TARGET)
        return 0 if all(r[0] for r in results) else 1

    print("=== 5) ★ 从目标盘以 UEFI 重启（本 Bug 的关键一步）===")
    stop_all()
    open(SER_BOOT, "wb").close()
    start_vm(VMX_BOOT)
    t0 = time.time()
    booted = False
    while time.time() - t0 < 300:
        txt = read_text(SER_BOOT)
        if "[OS] booted from installed disk" in txt or "[GUI64] ready" in txt:
            booted = True
            break
        if "PANIC" in txt or "TRIPLE FAULT" in txt.upper():
            break
        time.sleep(2)
    txt = read_text(SER_BOOT)
    chk("UEFI 从装好的目标盘引导成功（[OS] booted / [GUI64] ready）", booted,
        "%.0fs 串口 %d 字节" % (time.time() - t0, len(txt)))
    for k in ("U:", "S", "[OS] booted", "[GUI64] ready", "[VFS64] mount", "PANIC"):
        print("      串口命中 %-26r x%d" % (k, txt.count(k)))
    if not booted:
        vm_log = os.path.join(WORK, "vmware.log")
        if os.path.exists(vm_log):
            lines = [l for l in read_text(vm_log).splitlines()
                     if "EFI boot" in l or "No operating system" in l or "OsNotFound" in l]
            print("      --- VMware EFI 引导尝试 ---")
            for l in lines[-8:]:
                print("      | " + l[:160])
    stop_all()

    good = sum(1 for r in results if r[0])
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if good == len(results) else "FAIL", len(results), good))
    for ok, name, detail in results:
        if not ok:
            print("   FAIL: %s  %s" % (name, detail))
    return 0 if good == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
