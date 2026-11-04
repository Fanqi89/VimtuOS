#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/gpu64_vmware_probe.py - v0.9.4 通用 GPU 层的 **VMware 真机取证**（一次性取证脚本，不是 CI 门禁）

为什么单独有这一份：
  `tests/gpu64_test.py` 跑的是 QEMU 的四种图形后端，厂商分别是 1234(Bochs) / 1af4(virtio) / 1b36(QXL)，
  都不是 Intel/AMD/NVIDIA。VMware 的 SVGA II 是**又一个真实厂商**（0x15AD），而且它的 BAR0 与 QEMU 三种后端
  都不同 —— 它是 **I/O BAR**（`mmio=io`），正好能证明「不是 MMIO 就如实写 bar0=0x0 size=0、一个字节都不解引用」
  这条降级路径在**真实虚拟硬件**上也是对的。

做法（与 tests/vmware_make_vm.py 同一套机制，不重复实现 VM 逻辑）：
  1) 把**装好的系统盘** build64/sysdisk.img 复制到临时目录（不写仓库里的任何产物）；
  2) 生成 monolithicFlat 的 .vmdk 描述符（直接指向那份副本）+ 一份 .vmx：
     IDE0:0 = 该盘、serial0 -> 文件（抓客人串口）、RemoteDisplay.vnc 打开（送登录手势）；
  3) `vmrun -T ws start <vmx> nogui`，轮询串口文件等 `[GPU] report`，打印所有 `[GPU]` 行；
  4) 用 tests/qemuhelp.py 的 vnc_login_desktop()（RFB KeyEvent）敲两次回车 -> 等 `[GUI64] ready`；
  5) `vmrun stop <vmx> hard` 收尾。

★ 注意：安装介质 vimtu64-64.img 里装的是**安装程序内核**（只链 gpu64 的弱兜底），跑它看不到任何 `[GPU]` 行。
  必须用装好的系统盘（build64/sysdisk.img 或你自己安装出来的盘）。

实测（v0.9.4 发布时真跑，VMware Workstation + BIOS）：
  [GPU] scan buses=5 devices=6 gpus=1 known=0 unknown=1 fb=1024x768 backend=vbe
  [GPU] dev idx=0 bdf=00:0f.0 vendor=15ad device=0405 name=unknown class=030000
        bar0=0x0 size=0 mmio=io cmd=0x0007 mem=1 bm=1
  [GPU] probe vendor=unknown idx=0 skipped reason=no-safe-probe
  [GPU] report gpus=1 known=0 unknown=1 probed=0 skipped=1 mmio_ok=0 cmd_on=1 extra=0 accel=none-not-implemented
  [GUI64] ready（VNC 注入两次回车后）

用法：py -3 tests\\gpu64_vmware_probe.py [--img build64\\sysdisk.img] [--vnc-port 5905] [--keep]
退出码：0 = [GPU] 行齐全且桌面 ready；1 = 有断言不满足；2 = 环境问题（没有 vmrun / VM 起不来）
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import qemuhelp as qh  # noqa: E402

VMRUN_CANDIDATES = (
    r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe",
    r"C:\Program Files\VMware\VMware Workstation\vmrun.exe",
)
SECTOR = 512


def vmdk_descriptor(path, flat_name, sectors, cid=0x09400940):
    txt = ("# Disk DescriptorFile\nversion=1\nencoding=\"UTF-8\"\nCID=%08x\nparentCID=ffffffff\n"
           "createType=\"monolithicFlat\"\n\n# Extent description\nRW %d FLAT \"%s\" 0\n\n"
           "# The Disk Data Base\n#DDB\n\nddb.virtualHWVersion = \"7\"\n"
           "ddb.geometry.cylinders = \"%d\"\nddb.geometry.heads = \"16\"\n"
           "ddb.geometry.sectors = \"63\"\nddb.adapterType = \"ide\"\n"
           % (cid, sectors, flat_name, max(1, sectors // (16 * 63))))
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt)


def vmx_text(name, disk, serial_log, vnc_port):
    return ("#!/usr/bin/vmware\n.encoding = \"UTF-8\"\nconfig.version = \"8\"\n"
            "virtualHW.version = \"7\"\ndisplayName = \"%s\"\nguestOS = \"other-64\"\n"
            "msg.autoAnswer = \"TRUE\"\nnumvcpus = \"1\"\nmemsize = \"512\"\n"
            "svga.vramSize = \"134217728\"\n"
            "ide0:0.present = \"TRUE\"\nide0:0.deviceType = \"disk\"\n"
            "ide0:0.fileName = \"%s\"\nide0:0.startConnected = \"TRUE\"\n"
            "ide0:1.present = \"FALSE\"\nide1:0.present = \"FALSE\"\n"
            "floppy0.present = \"FALSE\"\nethernet0.present = \"FALSE\"\n"
            "usb.present = \"FALSE\"\nsound.present = \"FALSE\"\n"
            "serial0.present = \"TRUE\"\nserial0.fileType = \"file\"\n"
            "serial0.fileName = \"%s\"\nserial0.tryNoRxLoss = \"FALSE\"\n"
            "serial0.autodetect = \"FALSE\"\n"
            "RemoteDisplay.vnc.enabled = \"TRUE\"\nRemoteDisplay.vnc.port = \"%d\"\n"
            "RemoteDisplay.vnc.key = \"\"\nbios.bootDelay = \"0\"\n"
            % (name, disk, serial_log.replace("\\", "\\\\"), vnc_port))


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"))
    ap.add_argument("--vnc-port", type=int, default=5905)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    vmrun = next((p for p in VMRUN_CANDIDATES if os.path.exists(p)), None)
    if not vmrun:
        print("环境问题：没装 VMware Workstation（找不到 vmrun.exe）")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统盘 %s（先 bash build64.sh；注意安装介质里是安装内核，没有 [GPU] 行）" % args.img)
        return 2

    def run(*a, timeout=240):
        r = subprocess.run([vmrun, "-T", "ws"] + list(a), capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")

    tmp = tempfile.mkdtemp(prefix="gpu64_vmw_")
    flat = os.path.join(tmp, "disk-flat.img")
    shutil.copyfile(args.img, flat)                      # ★ 只写副本，不碰仓库里的产物
    sectors = os.path.getsize(flat) // SECTOR
    vmdk_descriptor(os.path.join(tmp, "disk.vmdk"), "disk-flat.img", sectors)
    serial = os.path.join(tmp, "serial.log")
    vmx = os.path.join(tmp, "gpu094.vmx")
    with open(vmx, "w", encoding="utf-8", newline="\n") as f:
        f.write(vmx_text("vimtu64-gpu094", "disk.vmdk", serial, args.vnc_port))
    print("VMX = %s" % vmx)

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-46s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    code, out = run("start", vmx, "nogui")
    if code != 0:
        print("环境问题：vmrun start 失败 rc=%d %s" % (code, out.strip()[:300]))
        return 2
    try:
        t0 = time.time()
        while time.time() - t0 < 240 and "[GPU] report" not in read(serial):
            time.sleep(1.0)
        log = read(serial)
        gpu = [ln.strip() for ln in log.splitlines() if "[GPU]" in ln]
        print("---- [GPU] 行（VMware SVGA）----")
        for ln in gpu:
            print("  " + ln)
        check("VMware 下打出 [GPU] scan / dev / probe / report 四类行", len(gpu) >= 4,
              "%d 行" % len(gpu))
        check("[GPU] scan 里 fb 与 [G64] fb render 的 phys 一致",
              any("backend=vbe" in x for x in gpu) and "[G64] fb render=" in log, gpu[0] if gpu else "缺")
        check("VMware SVGA 的厂商如实为 unknown（15ad 不在三家里，不许编造）",
              any("vendor=15ad" in x and "name=unknown" in x for x in gpu), "")
        check("BAR0 是 I/O BAR 时如实写 bar0=0x0 size=0 mmio=io（不解引用、不编窗口）",
              any("mmio=io" in x and "bar0=0x0 size=0" in x for x in gpu), "")
        check("三家的只读探测如实 skipped（probed=0）",
              any("probed=0" in x and "accel=none-not-implemented" in x for x in gpu), "")
        check("报告里 mmio_ok=0（这张卡本来就没有 MMIO 窗口）",
              any("mmio_ok=0" in x for x in gpu), "")

        got = qh.vnc_login_desktop(lambda: read(serial), args.vnc_port, None, timeout=180, count=2)
        check("VNC 登录手势注入成功（两次回车）", got, "VNC %d" % args.vnc_port)
        t0 = time.time()
        while time.time() - t0 < 240 and "[GUI64] ready" not in read(serial):
            time.sleep(1.0)
        check("VMware 下桌面照常起来（[GUI64] ready）", "[GUI64] ready" in read(serial), "")
        bad = [p for p in ("PANIC", "TRIPLE FAULT", "OOM:") if p in read(serial)]
        check("全程没有 PANIC / TRIPLE FAULT / OOM", not bad, ",".join(bad))
    finally:
        run("stop", vmx, "hard")
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)

    npass = sum(1 for c in checks if c[1])
    print("\n==== gpu64_vmware_probe：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, len(checks) - npass))
    return 0 if npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
