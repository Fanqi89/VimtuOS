#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/vmware_mouse_probe.py - 在 VMware 里抓 PS/2 鼠标原始字节

背景：QEMU 下鼠标正常，VMware 下光标只往窗口边缘跑。为定位到底是
  (a) 字节流错位（包头缺 bit3 同步位 / 包大小不一致 -> 位移变成垃圾），还是
  (b) 位移本身就巨大（溢出位/符号位解释错），
这份脚本让安装程序内核把每个 PS/2 字节打到 COM1（build64.sh 里的 -DVIMTU_PS2_TRACE=1），
然后**用宿主鼠标真的去动 VMware 窗口**，再把串口日志抓回来分析。

用法：python tests/vmware_mouse_probe.py [--seconds 12] [--no-build-vm]
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
BASE = r"C:\Users\fanqi\Desktop\新建文件夹"
TESTDIR = os.path.join(BASE, "v64-install-test")
VMX = os.path.join(TESTDIR, "vimtu64-install-test.vmx")
SERIAL = os.path.join(TESTDIR, "serial-install.log")
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
ISO = os.path.join(ROOT, "vimtu64-64.iso")

user32 = ctypes.windll.user32


def vmrun(*args, timeout=180):
    return subprocess.run([VMRUN, "-T", "ws"] + list(args),
                          capture_output=True, timeout=timeout)

def stop_all():
    r = vmrun("list")
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        if line.strip().endswith(".vmx"):
            vmrun("stop", line.strip(), "hard")
    time.sleep(1)


def find_window(title_part):
    res = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, lparam):
        n = ctypes.create_unicode_buffer(512)
        user32.GetWindowTextW(hwnd, n, 512)
        if title_part.lower() in n.value.lower() and user32.IsWindowVisible(hwnd):
            res.append((hwnd, n.value))
        return True

    user32.EnumWindows(cb, 0)
    return res[0] if res else (None, None)


def move_mouse_pattern(seconds):
    """在窗口里画圈 + 来回扫，制造连续的中等位移。"""
    hwnd, title = find_window("VimtuOS")
    print("[probe] 窗口: %s (hwnd=%s)" % (title, hwnd))
    if hwnd:
        user32.SetForegroundWindow(hwnd)
        time.sleep(0.5)
        # 点一下，让 VMware 抓住鼠标（PS/2 相对模式）
        rect = ctypes.wintypes.RECT() if hasattr(ctypes, "wintypes") else None
        import ctypes.wintypes as wt
        rect = wt.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(rect))
        cx = (rect.left + rect.right) // 2
        cy = (rect.top + rect.bottom) // 2
        user32.SetCursorPos(cx, cy)
        time.sleep(0.3)
        user32.mouse_event(0x0002, 0, 0, 0, 0)   # 左键按下
        time.sleep(0.1)
        user32.mouse_event(0x0004, 0, 0, 0, 0)   # 左键抬起
        time.sleep(0.5)

    import math
    t0 = time.time()
    i = 0
    while time.time() - t0 < seconds:
        ang = i * 0.15
        if hwnd:
            import ctypes.wintypes as wt
            rect = wt.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(rect))
            cx = (rect.left + rect.right) // 2
            cy = (rect.top + rect.bottom) // 2
            r = 60 + 40 * math.sin(i * 0.05)
            user32.SetCursorPos(int(cx + r * math.cos(ang)), int(cy + r * math.sin(ang)))
        i += 1
        time.sleep(0.02)
    print("[probe] 鼠标动作完成（%d 步）" % i)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=12.0)
    ap.add_argument("--no-build-vm", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(ISO):
        print("缺少 %s（先跑 build64.sh）" % ISO)
        return 2

    if not args.no_build_vm:
        print("[probe] 重建测试 VM（ISO 当安装介质）...")
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "vmware_make_vm.py"), "--iso"],
                           cwd=ROOT, capture_output=True, timeout=300)
        if r.returncode != 0:
            print(r.stdout.decode("utf-8", "replace")[-800:])
            print(r.stderr.decode("utf-8", "replace")[-800:])
            return 2
        print("   " + r.stdout.decode("utf-8", "replace").strip().splitlines()[-1])

    stop_all()
    if os.path.exists(SERIAL):
        os.remove(SERIAL)

    print("[probe] 启动 VM（GUI 模式，需要宿主鼠标注入）...")
    code, out = vmrun("start", VMX, "gui", timeout=180)
    print("   vmrun start rc=%s %s" % (code, out.decode("utf-8", "replace").strip()[:120]))

    print("[probe] 等 VMware 起来 + 客人引导 + 安装界面就绪 ...")
    time.sleep(22)

    move_mouse_pattern(args.seconds)
    time.sleep(2)

    print("[probe] 关机并读串口 ...")
    vmrun("stop", VMX, "hard", timeout=120)
    time.sleep(1)

    if not os.path.exists(SERIAL):
        print("没有串口日志 %s" % SERIAL)
        return 1

    text = open(SERIAL, "r", encoding="utf-8", errors="replace").read()
    trace = "".join(re.findall(r"[MK][0-9A-F]{2}", text))
    pkts = re.findall(r"\|pB([0-9A-F]{2})X([0-9A-F]{2})Y([0-9A-F]{2})", text)
    bad = re.findall(r"!([0-9A-F]{2})", text)
    print("串口总长度 %d 字节；原始字节对 %d 个；完整包 %d 个；同步位异常(丢包) %d 次"
          % (len(text), len(trace) // 3, len(pkts), len(bad)))

    print("--- 原始字节对（前 120 个）---")
    print(" ".join(trace[i:i + 3] for i in range(0, min(len(trace), 360), 3)))

    print("--- 解析出的包（b0, dx字节, dy字节）→ 位移 ---")
    for (b0, x, y) in pkts[:60]:
        v0, vx, vy = int(b0, 16), int(x, 16), int(y, 16)
        dx = vx - 256 if vx >= 128 else vx
        dy = vy - 256 if vy >= 128 else vy
        print("   b0=%02X dx=%4d dy=%4d  %s%s%s%s"
              % (v0, dx, dy,
                 "Xovf" if v0 & 0x40 else "", "Yovf" if v0 & 0x80 else "",
                 "X-" if v0 & 0x10 else "X+", "Y-" if v0 & 0x20 else "Y+"))

    if bad:
        print("--- 同步位异常样本（前 30 个字节值）---")
        print(" ".join(bad[:30]))

    # 结论提示
    big = sum(1 for (b0, x, y) in pkts
              if abs((int(x, 16) - 256 if int(x, 16) >= 128 else int(x, 16))) > 40
              or abs((int(y, 16) - 256 if int(y, 16) >= 128 else int(y, 16))) > 40)
    print("--- 小结 ---")
    print("  位移 >40px 的包: %d / %d" % (big, len(pkts)))
    print("  丢包(同步位异常): %d" % len(bad))
    return 0


if __name__ == "__main__":
    sys.exit(main())
