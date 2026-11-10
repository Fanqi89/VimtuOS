"""tests/vmsvga_display64_test.py - ★ v0.9.37：VMware 上"**人眼能看到画面**"的自动验收

★★ 本文件的意义之一就是**换观察者**：
  v0.9.18～v0.9.36 的"VMware 黑屏"结论全部来自 VNC 抓帧。实测（同一次开机、同一时刻）：
    * VMware 窗口：纯黑 5.7~6.3%、近白 82~84%、Dock 强调蓝在 —— 桌面正常；
    * VNC 抓帧：纯黑 **99.3%**（v0.9.36 与 v0.9.37 两版完全一致）。
  ⇒ "VMware 黑屏"是**观察伪像**。所以本测试**只认宿主窗口像素**
    （`tests/textvnc.py` 的 `WinView`：PrintWindow(PW_RENDERFULLCONTENT) 优先，
      失败回退屏幕 DC BitBlt），VNC 只用来发**输入**（两次回车登录 / Super 活体校验），
    **任何像素判定都不再经过 VNC**。

断言：
  ① `[VMWARE] present ok=1 path=…` 且 `enable=1`（默认路径，不需要任何 marker）
  ①' `present regs` 行里 `lfbtl=` 是壁纸色（客人显存里就是桌面）
  ② 窗口纯黑 < 20%   ③ 窗口近白 > 50%   ④ 窗口强调蓝 > 0.5%
  ⑤ 串口无 PANIC
  ⑥ 活体校验：按 Super 后窗口像素变化 > 1%（排除"抓到宿主缓存"）

用法：py -3 tests/vmsvga_display64_test.py [--img build64/sysdisk.img] [--vnc-port 5909] [--keep]
（需要 VMware Workstation 的 GUI 窗口 —— 判定对象就是它的画面。）
"""
import argparse
import re
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh               # noqa: E402
import textvnc as tv                # noqa: E402


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


VS = _load("vmware_svga64_test")     # 复用夹具生成器（vmdk 描述符 + vmx 模板 + read()）

SECTOR = 512


def blackpct(a):
    h, w, _ = a.shape
    bm = (a[:, :, 0] == 0) & (a[:, :, 1] == 0) & (a[:, :, 2] == 0)
    return 100.0 * bm.sum() / (h * w)


def near_white_pct(a):
    m = (a[:, :, 0] > 230) & (a[:, :, 1] > 230) & (a[:, :, 2] > 235)
    return 100.0 * m.sum() / (a.shape[0] * a.shape[1])


def bluish_pct(a):
    m = (a[:, :, 2].astype(int) - a[:, :, 0].astype(int)) > 80
    return 100.0 * m.sum() / (a.shape[0] * a.shape[1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"))
    ap.add_argument("--vnc-port", type=int, default=5909)   # 只用于发输入，不做像素判定
    ap.add_argument("--settle", type=float, default=12.0)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    vmrun = next((p for p in VS.VMRUN_CANDIDATES if os.path.exists(p)), None)
    if not vmrun:
        print("环境问题：没装 VMware Workstation（找不到 vmrun.exe）")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：缺少 %s（先跑 bash build64.sh）" % args.img)
        return 2

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-64s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    tmp = tempfile.mkdtemp(prefix="vmsvga_display_")
    flat = os.path.join(tmp, "sys.img")
    shutil.copyfile(args.img, flat)                  # 默认路径：不注任何 marker
    sectors = os.path.getsize(flat) // SECTOR
    VS.vmdk_descriptor(os.path.join(tmp, "sys.vmdk"), "sys.img", sectors)
    serial = os.path.join(tmp, "serial.log")
    vmxp = os.path.join(tmp, "display.vmx")
    name = "vimtu64-display"
    with open(vmxp, "w", encoding="utf-8", newline="\n") as f:
        f.write(VS.vmx_text(name, "sys.vmdk", serial, args.vnc_port))

    def run(*a, timeout=300):
        r = subprocess.run([vmrun, "-T", "ws"] + list(a), capture_output=True, timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")

    print("=== VimtuOS VMware 显示验收（**宿主窗口像素**判据）===")
    print("VMX = %s" % vmxp)
    # ★ 必须带 GUI：判定对象就是"VMware 窗口里看到的画面"。分离启动，避免阻塞。
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
                    "-Arguments @{ CommandLine = '\"%s\" -T ws start \"%s\"' } | Out-Null"
                    % (vmrun, vmxp)], capture_output=True, text=True, timeout=60)

    try:
        t0 = time.time()
        while time.time() - t0 < 360:
            if qh.LOCK_INTERACTIVE in VS.read(serial):
                break
            time.sleep(1.0)
        try:
            qh.vnc_press(args.vnc_port, count=2, gap=1.5)        # 输入：锁屏上按两次回车
        except Exception as e:                                   # noqa: BLE001
            print("  [info] 登录手势异常：%r" % (e,))
        t1 = time.time()
        while time.time() - t1 < 300:
            if "[GUI64] ready" in VS.read(serial):
                break
            time.sleep(1.0)
        time.sleep(args.settle)
        log = VS.read(serial)

        pres = [x.strip() for x in log.splitlines() if x.startswith("[VMWARE] present")]
        print("---- [VMWARE] present 行（末 6 条）----")
        for ln in pres[-6:]:
            print("  " + ln)

        ok_line = next((x for x in pres if "ok=1" in x), "")
        check("① 呈现通路建起来了（[VMWARE] present ok=1，默认路径无需 marker）",
              bool(ok_line), ok_line or "缺 [VMWARE] present ok=1 行")
        check("① SVGA_REG_ENABLE 已由客人驱动置 1", "enable=1" in ok_line, ok_line)
        # ①' 客人显存回读（30 s 一拍）：等到"那一拍真的读到桌面像素"为止（最多约 90 s）
        regs = ""
        for _try in range(5):
            pres = [x.strip() for x in VS.read(serial).splitlines()
                    if x.startswith("[VMWARE] present")]
            regs = next((x for x in reversed(pres) if "present regs" in x), "")
            if "lfbtl=0xf8f9fb" in regs:
                break
            time.sleep(20)
        log = VS.read(serial)
        check("①' 客人显存里就是桌面（present regs 的 lfbtl=壁纸色）",
              "lfbtl=0xf8f9fb" in regs, regs)

        # ---- 宿主窗口像素（**唯一**显示判据；VNC 只发输入，不参与任何像素判定）----
        m = None
        for m in re.finditer(r"\[FB64\] fb source=\S+ addr=\S+ w=(\d+) h=(\d+)", log):
            pass
        guest_wh = (int(m.group(1)), int(m.group(2))) if m else (1024, 768)
        wv = tv.WinView(name, guest_wh=guest_wh)
        try:
            _shot, title = wv.shot()
        except OSError as e:
            check("② 找到了 VMware 窗口", False, "%r（需要在交互式会话里跑）" % (e,))
            raise SystemExit(0)
        f = wv.capture()
        a = f.rgb
        print("  窗口：%r  客人画面 box=%r  取帧 %dx%d（客人 %dx%d）"
              % (title, wv.box, f.w, f.h, guest_wh[0], guest_wh[1]))
        check("② 宿主窗口**不是黑屏**（纯黑 < 20%）", blackpct(a) < 20.0,
              "black=%.2f%%" % blackpct(a))
        check("③ 窗口里近白面积 > 50%（壁纸铺满）", near_white_pct(a) > 50.0,
              "near_white=%.2f%%" % near_white_pct(a))
        check("④ 窗口里强调蓝像素 > 0.5%（Dock/强调色在画）", bluish_pct(a) > 0.5,
              "bluish=%.2f%%" % bluish_pct(a))

        # ---- 活体校验：动一下客人，窗口像素必须跟着变（否则抓的是宿主缓存）----
        try:
            qh.vnc_press(args.vnc_port, keysym=0xFFEB)           # 输入：Super 打开开始菜单
            time.sleep(2.5)
            f2 = wv.capture()
            d = (100.0 * (a != f2.rgb).any(axis=2).mean())
            check("⑥ 活体校验：按 Super 后窗口像素变化 > 1%（抓的是活画面）", d > 1.0,
                  "changed=%.2f%%" % d)
            if d > 1.0:
                a = f2.rgb
        except Exception as e:                                   # noqa: BLE001
            check("⑥ 活体校验：按 Super 后窗口像素变化 > 1%（抓的是活画面）", False, repr(e))

        check("⑤ 全程无 PANIC", "PANIC" not in VS.read(serial),
              "PANIC 行数 %d" % VS.read(serial).count("PANIC"))

        try:
            from PIL import Image
            p = os.path.join(ROOT, "build64", "vmware-display-window.png")
            Image.fromarray(a, "RGB").save(p)
            print("  证据帧：%s" % p)
        except Exception:                                        # noqa: BLE001
            pass
    finally:
        try:
            run("stop", vmxp, "hard", timeout=180)
        except Exception:                                        # noqa: BLE001
            pass
        subprocess.run(["taskkill", "/F", "/IM", "vmware-vmx.exe"], capture_output=True)
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)

    npass = sum(1 for _n, ok, _d in checks if ok)
    nfail = len(checks) - npass
    print("\n==== vmsvga_display64_test：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, nfail))
    for n, ok, d in checks:
        if not ok:
            print("  - FAIL %s  %s" % (n, d))
    return 0 if nfail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
