#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/vmware_svga64_test.py - ★ v0.9.18：**VMware 虚拟机上的 GPU 取证**（用户指定用 VMware 测）

为什么另开这份：QEMU 的 3D 走 virgl（host `vrend` 拒绝了我们的 `BIND_OBJECT`，见
docs/2D与3D加速.md §2.3）。VMware 的虚拟 GPU 是 **SVGA II（15AD:0405）**，3D 走完全不同的
SVGA3D 协议 —— 所以"3D 加速"这件事必须**换一个真实厂商设备取证**。

本脚本证明（每条都有串口原文）：

  ① 内核在 VMware 上**能读 SVGA 寄存器**（BAR0 是 I/O 端口 BAR，走 base+0/base+1 两个口）：
     `[VMWARE] svga id=0x90000002 …`（版本协商写 ID_2 再回读成功）
  ② **能力位**：`3d=<0|1>` 直接回答"这台 VMware 到底给不给 3D"；同时报 rect_fill / alpha /
     gmr / screen2 / fifo_ext —— 这些是**只读**得来的，不 enable 设备、不改显示。
  ③ 几何/显存/FIFO 自洽：`max=WxH` 不小于 1024x768、`bpp=32`、`vram`/`memlen` 合理。
  ④ **DRM 2D 路径在 VMware 上也成立**：`[DRM] selftest dum=ok … ok=1`（同一个内核自检）。
  ⑤ 桌面照旧 `[GUI64] ready`；全程无 PANIC / TRIPLE FAULT。
  ⑥ 如实说明：SVGA3D **命令协议**尚未实现 —— 所以即使 `3d=1`，也**不**声称"3D 出图已验证"。

用法：py -3 tests\\vmware_svga64_test.py [--img build64\\sysdisk.img] [--vnc-port 5907] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（没有 vmrun / VM 起不来）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402  （VNC 登录手势：虚拟机里要按两次回车才进桌面）
SECTOR = 512
VMRUN_CANDIDATES = (
    r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe",
    r"C:\Program Files\VMware\VMware Workstation\vmrun.exe",
)


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
            "svga.autodetect = \"FALSE\"\n"
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
    ap.add_argument("--vnc-port", type=int, default=5907)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    vmrun = next((p for p in VMRUN_CANDIDATES if os.path.exists(p)), None)
    if not vmrun:
        print("环境问题：没装 VMware Workstation（找不到 vmrun.exe）")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：缺少 %s（先跑 bash build64.sh）" % args.img)
        return 2

    def run(*a, timeout=300):
        r = subprocess.run([vmrun, "-T", "ws"] + list(a), capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")

    tmp = tempfile.mkdtemp(prefix="vmware_svga_")
    flat = os.path.join(tmp, "disk-flat.img")
    shutil.copyfile(args.img, flat)
    sectors = os.path.getsize(flat) // SECTOR
    vmdk_descriptor(os.path.join(tmp, "disk.vmdk"), "disk-flat.img", sectors)
    serial = os.path.join(tmp, "serial.log")
    vmx = os.path.join(tmp, "svga0918.vmx")
    with open(vmx, "w", encoding="utf-8", newline="\n") as f:
        f.write(vmx_text("vimtu64-svga0918", "disk.vmdk", serial, args.vnc_port))
    print("VMX = %s" % vmx)

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-58s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    print("=== VimtuOS VMware SVGA acceptance（VMware 虚拟机上的 GPU 取证）===")
    code, out = run("start", vmx, "nogui")
    if code != 0:
        print("环境问题：vmrun start 失败 rc=%d %s" % (code, out.strip()[:300]))
        return 2
    try:
        t0 = time.time()
        # 等到"SVGA 探测行"出现（再看桌面）
        while time.time() - t0 < 240:
            log = read(serial)
            if "[VMWARE] svga" in log:
                break
            time.sleep(1.0)
        # 登录手势：VMware 的客人同样停在登录界面，要按两次回车才进桌面
        try:
            got_login = qh.vnc_login_desktop(lambda: read(serial), args.vnc_port, None,
                                            timeout=180, count=2)
        except Exception as e:                                   # noqa: BLE001
            print("  [info] VNC 登录手势异常：%r" % (e,))
            got_login = False
        print("  [info] VNC 登录手势（两次回车）：%s" % ("ok" if got_login else "失败"))
        # 桌面起来（最多再等 180 s）
        t1 = time.time()
        while time.time() - t1 < 180:
            if "[GUI64] ready" in read(serial):
                break
            time.sleep(1.0)
        log = read(serial)

        svga = [ln.strip() for ln in log.splitlines() if "[VMWARE]" in ln]
        print("---- [VMWARE] 行 ----")
        for ln in svga:
            print("  " + ln)

        check("① VMware 上读到了 SVGA 寄存器（[VMWARE] svga 行存在）", len(svga) >= 1,
              svga[0] if svga else "缺 [VMWARE] 行")
        m = re.search(r"\[VMWARE\] svga id=(0x[0-9a-f]+) caps=(0x[0-9a-f]+)", log)
        check("① 版本协商成功（id=0x90000002 或 0x90000001：写 ID 后回读一致）",
              m is not None and m.group(1).lower() in ("0x90000002", "0x90000001"),
              (m.group(0) if m else "缺 id/caps"))
        m3 = re.search(r"\[VMWARE\] svga .* 3d=([01])", log)
        check("② **能力位读出来了**（3d=<0|1> 明确回答这台 VMware 给不给 3D）",
              m3 is not None, ("3d=%s" % m3.group(1)) if m3 else "缺 3d 字段")
        mg = re.search(r"geom=(\d+)x(\d+) max=(\d+)x(\d+) bpp=(\d+)", log)
        ok_geom = False
        if mg:
            ok_geom = (int(mg.group(3)) >= 1024 and int(mg.group(4)) >= 768 and int(mg.group(5)) == 32)
        check("③ 几何/位深自洽（max>=1024x768、bpp=32）", ok_geom,
              (mg.group(0) if mg else "缺 geom 字段"))
        mm = re.search(r"vram=(\d+) mem=0x[0-9a-f]+ memlen=(\d+)", log)
        check("③ 显存与 FIFO 大小合理（vram>=16 MiB、memlen>=256 KiB）",
              mm is not None and int(mm.group(1)) >= 16 * 1024 * 1024 and int(mm.group(2)) >= 256 * 1024,
              (mm.group(0) if mm else "缺 vram/memlen"))
        drm_self = re.search(r"\[DRM\] selftest dumb=ok flip=ok vblank=\d+ mode=(hw|soft) ok=1", log)
        check("④ DRM 2D 路径在 VMware 上也成立（[DRM] selftest … ok=1）", drm_self is not None,
              (drm_self.group(0) if drm_self else "缺 [DRM] selftest ok=1"))
        check("⑤ 桌面照旧起来（[GUI64] ready）", "[GUI64] ready" in log)
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("⑤ 不得出现 %s" % bad, bad not in log)

        # ⑥ 如实结论
        three_d = (m3.group(1) == "1") if m3 else False
        print("")
        if three_d:
            print("  ⑥ 结论：VMware SVGA3D **能力位 = 1**（宿主提供 3D）。")
            print("     但 SVGA3D **命令协议尚未实现** —— 所以本脚本**不**声称\"3D 出图已验证\"；")
            print("     这是下一步（SVGA3D 的 FIFO 命令 + OT 表 + 着色器）的工作。")
        else:
            print("  ⑥ 结论：这台 VMware 的 SVGA 能力位里 **没有 3D**（3d=0）—— 如实记录。")
    finally:
        run("stop", vmx, "hard", timeout=120)
        if args.keep:
            print("[vmware] 串口日志：%s" % serial)

    npass = sum(1 for _n, c, _d in checks if c)
    nfail = len(checks) - npass
    print("\n==== vmware_svga64_test：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, nfail))
    if nfail:
        print("失败项：")
        for n, c, d in checks:
            if not c:
                print("  [FAIL] %s   %s" % (n, d))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
