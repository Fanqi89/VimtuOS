#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/screenshot64.py - 抓一张 64 位桌面的真实截图（PNG），供人工查看/留档

做的事：
  1) 引导 build64/system.img（= 装好的系统，直接进桌面）
  2) 等 [GUI64] ready
  3) 用 Win 键 + 数字快捷键打开指定应用（默认：扫雷、计算器、任务管理器）
  4) QEMU monitor screendump 抓帧 -> Pillow 转 PNG -> 存到 docs/screenshots/

用法：
    python tests/screenshot64.py                       # 默认开扫雷/计算器/任务管理器
    python tests/screenshot64.py --apps 5,4            # 只开扫雷(5)和计算器(4)
    python tests/screenshot64.py --out X.png
    python tests/screenshot64.py --no-apps             # 只抓纯净桌面

菜单序号（与 kernel/gui64.cpp 一致）：1终端 2我的电脑 3系统监视器 4计算器 5扫雷 6设置 7任务管理器 8关于
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

# 数字键 -> 菜单序号（'1'=第0项 ... '0'=第9项）
DIGIT_OF_INDEX = {0: "1", 1: "2", 2: "3", 3: "4", 4: "5", 5: "6", 6: "7", 7: "8", 8: "9", 9: "0"}


def find_qemu():
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
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


def mon_send(port, cmd, wait=0.6):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=8)
    except OSError:
        return
    try:
        s.sendall(cmd.encode() + b"\n")
        time.sleep(wait)
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "screenshots", "desktop64.png"))
    ap.add_argument("--apps", default="4,3,6",
                    help="要打开的**菜单序号**（逗号分隔）：1终端 2我的电脑 3系统监视器 4计算器 5扫雷 6设置 7任务管理器 8关于（别选 0重启/9关机）")
    ap.add_argument("--no-apps", action="store_true")
    ap.add_argument("--port", type=int, default=5599)
    ap.add_argument("--keys", default="",
                    help="打开应用后再注入的 QEMU sendkey 名（逗号分隔），例如 'right,down,down,down' "
                         "把任务管理器切到性能页并把选中项移到\"显卡\"；'1' 把设置页切到\"系统（设备规格）\"")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu()
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_shot_")
    serial = os.path.join(tmp, "serial.log")
    ppm = os.path.join(tmp, "desktop.ppm")

    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-shot",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ok = False
    try:
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

        t0 = time.time()
        while time.time() - t0 < 60:
            try:
                with open(serial, "r", encoding="utf-8", errors="replace") as f:
                    if "[GUI64] ready" in f.read():
                        ok = True
                        break
            except OSError:
                pass
            time.sleep(0.5)
        if not ok:
            sys.stderr.write("桌面没起来（等 [GUI64] ready 超时）\n")
            return 1

        print("[shot] 桌面就绪，%.1fs" % (time.time() - t0))
        if not args.no_apps:
            for s in args.apps.split(","):
                s = s.strip()
                if not s.isdigit():
                    continue
                idx = int(s)
                digit = DIGIT_OF_INDEX.get(idx)
                if not digit:
                    continue
                mon_send(args.port, "sendkey meta_l", wait=1.1)   # 开始菜单
                mon_send(args.port, "sendkey %s" % digit, wait=2.0)
                print("[shot] 已打开菜单序号 %d（按键 %s）" % (idx, digit))
            time.sleep(1.5)

        # 可选的页面导航键（批次 B）：例如 tmgr 性能页 -> 显卡项，或设置页 -> 系统（设备规格）
        if args.keys.strip():
            for k in args.keys.split(","):
                k = k.strip()
                if not k:
                    continue
                mon_send(args.port, "sendkey %s" % k, wait=0.9)
                print("[shot] 注入按键 %s" % k)
            time.sleep(1.2)
        # 把鼠标挪到画面中间偏下，避免光标压在窗口标题上
        mon_send(args.port, "mouse_move 200 120", wait=0.8)
        time.sleep(0.8)
        if os.path.exists(ppm):
            os.remove(ppm)
        mon_send(args.port, "screendump %s" % q(ppm), wait=3.0)
        for _ in range(20):
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                break
            time.sleep(0.3)
        if not os.path.exists(ppm):
            sys.stderr.write("没拿到 screendump\n")
            return 1

        out_dir = os.path.dirname(os.path.abspath(args.out))
        os.makedirs(out_dir, exist_ok=True)
        try:
            from PIL import Image   # noqa
            Image.open(ppm).save(args.out)
            print("[shot] 已保存 PNG：%s（%d 字节）" % (args.out, os.path.getsize(args.out)))
        except Exception as e:
            kept = os.path.join(out_dir, "desktop64.ppm")
            with open(ppm, "rb") as a, open(kept, "wb") as b:
                b.write(a.read())
            print("[shot] 没有 Pillow（%s），已保留 PPM：%s" % (e, kept))
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass


if __name__ == "__main__":
    sys.exit(main())
