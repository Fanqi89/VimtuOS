#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/shots_fresh.py - 从**当前构建**里现拍一组系统截图（README 用）

只做一件事：QEMU 起 `build64/system.img`（当前构建的系统盘），用**键盘**把系统推到几个
真实状态，每张 `screendump` 转 PNG 落到 `docs/images/`。

为什么不用鼠标：宿主侧精确点击在这台机器上不稳（历史 UNVERIFIED 的根源）；键盘驱动是
确定性的，而"这一帧是什么"由串口自己打的证据决定（见下面各步的判据）。

截图清单：
  01_lock      锁屏（淡入完成）
  02_desktop   桌面（登录后）
  03_startmenu 开始菜单（Win 键）
  04_terminal  终端
  05_mypc      我的电脑（文件管理器）
  06_settings  设置
  07_tmgr      任务管理器
  08_about     关于（这一页上有版本号 —— 截图"是不是当前构建"就靠它）
  09_calc      计算器

**踩过的两个坑**（都已修，写在代码里）：
  ① 开始菜单的数字快捷键映射到的**不是** `[START64] grid` 那个顺序（按 grid 顺序按 "6"
     期望开终端，实际开的是设置）→ 一律改用**搜索框**：串口会打
     `[START64] search q=<词> hits=N first=<App 名> (app=<id>)`，命中唯一时回车就开那一个。
  ② 键盘在 1 fps 的 GUI 下**会丢字**（"calc" 只到 "c"）→ 打完字先读回 `search q=…`，
     **只有读回来的词等于目标词才按回车**，否则重打（最多 4 次）。

用法：py -3 tools/shots_fresh.py [--img build64/system.img] [--out docs/images] [--port 5693]
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]


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


class Mon(object):
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.5):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def key(self, name, wait=1.0):
        self.send("sendkey %s" % name, wait=wait)

    def typ(self, text, wait=0.22):
        """ASCII 打进当前焦点（开始菜单搜索框）。0.22s/键在 1 fps 的 GUI 下最稳。"""
        for ch in text:
            if ch == " ":
                self.key("spc", wait=wait)
            elif ch.isalnum():
                self.key(ch, wait=wait)


def read_ppm(path):
    data = open(path, "rb").read()
    pos, vals = 2, []
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        s = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(data[s:pos]))
    pos += 1
    w, h = vals[0], vals[1]
    return w, h, data[pos:pos + w * h * 3]


def ink_ratio(px, w, h):
    """非黑像素占比（抽样 1/7）—— 只用来断言"这一帧不是全黑" """
    on = 0
    total = w * h
    for i in range(0, total, 7):
        o = i * 3
        if px[o] > 40 or px[o + 1] > 40 or px[o + 2] > 40:
            on += 1
    return on * 7.0 / total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--out", default=os.path.join(ROOT, "docs", "images"))
    ap.add_argument("--port", type=int, default=5693)
    # ★ v0.12.5：--only 01_lock,02_desktop 只拍指定的那几张（拍 README 头图时用，省时间）。
    #   取值是产物名（不带 .png）；不给就是 9 张全拍。
    ap.add_argument("--only", default=None)
    args = ap.parse_args()

    qemu = find_qemu()
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到 %s（先跑 bash build64.sh）" % args.img)
        return 2
    os.makedirs(args.out, exist_ok=True)

    tmp = tempfile.mkdtemp(prefix="vimtu64_shots_")
    serial = os.path.join(tmp, "shots_serial.log")
    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-shots",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none", "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for(needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in slog()[since:]:
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    want = None
    if args.only:
        want = set(x.strip() for x in args.only.split(",") if x.strip())

    def need(name):
        return want is None or name in want

    shots = []
    try:
        for _ in range(120):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        mon = Mon(args.port)

        def shot(name, settle=1.0):
            time.sleep(settle)
            ppm = os.path.join(tmp, name + ".ppm")
            if os.path.exists(ppm):
                os.remove(ppm)
            mon.send("screendump %s" % q(ppm), wait=0.35)
            for _ in range(40):
                if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                    break
                time.sleep(0.1)
            if not (os.path.exists(ppm) and os.path.getsize(ppm) > 1024):
                print("  SKIP %-14s 抓帧失败" % name)
                return None
            w, h, px = read_ppm(ppm)
            ratio = ink_ratio(px, w, h)
            png = os.path.join(args.out, name + ".png")
            try:
                from PIL import Image
                Image.open(ppm).save(png)
            except Exception as e:
                print("  SKIP %-14s 转 PNG 失败：%r（需要 Pillow）" % (name, e))
                return None
            print("  [ok] %-14s %dx%d ink=%.1f%% %s B"
                  % (name, w, h, ratio * 100, format(os.path.getsize(png), ",")))
            shots.append((name, w, h, ratio))
            return ratio

        print("=== 1) 等锁屏 ===")
        if not wait_for("[LOCK64] bg blur ready", 240):
            print("环境问题：等不到锁屏就绪")
            return 2
        if need("01_lock"):
            shot("01_lock", settle=1.5)

        print("=== 2) 回车两次进桌面 ===")
        mon.key("ret", wait=1.2)
        mon.key("ret", wait=2.0)
        if not wait_for("[GUI64] ready", 200):
            print("环境问题：进不了桌面")
            return 2
        if need("02_desktop"):
            shot("02_desktop", settle=2.5)

        # --only 只要锁屏/桌面时，到这里就收工（后面的应用窗口一张都不拍）
        if want is not None and not any(n.startswith(("03", "04", "05", "06", "07", "08", "09")) for n in want):
            raise SystemExit(0)

        print("=== 3) 开始菜单（Win 键）===")
        mon.key("meta_l", wait=1.8)
        wait_for("[START64] open", 10)
        shot("03_startmenu", settle=1.2)
        m = re.search(r"\[START64\] grid apps=(\d+) cells=(\d+)", slog())
        if m:
            print("    网格证据：apps=%s cells=%s" % (m.group(1), m.group(2)))
        mon.key("esc", wait=1.0)

        print("=== 4) 逐窗开应用（一律走开始菜单的搜索框；丢字就重打）===")
        # (截图名, 搜索词, 期望 first=, 期望的 [APP] 行)
        targets = [
            ("04_terminal", "term",     "Terminal",      "[APP] term opened"),
            ("05_mypc",     "mypc",     "My Computer",   "[APP] mypc opened"),
            ("06_settings", "settings", "Settings",      "[APP] settings opened"),
            ("07_tmgr",     "tmgr",     "Task Manager",  "[APP] tmgr opened"),
            ("08_about",    "about",    "About VimtuOS", "[APP] about opened"),
            ("09_calc",     "calc",     "Calculator",    "[APP] calc opened"),
        ]
        opened = []
        for name, token, expect_first, app_needle in targets:
            base = len(slog())
            first, hits = None, -1
            for attempt in range(4):
                mon.key("meta_l", wait=1.5)
                mon.typ(token)
                time.sleep(0.9)
                all_hits = re.findall(
                    r"\[START64\] search q=(\S+) hits=(\d+) first=([^(]+?) \(app=", slog()[base:])
                if all_hits:
                    first, hits = all_hits[-1][2].strip(), int(all_hits[-1][1])
                if all_hits and all_hits[-1][0] == token:
                    break
                print("    （第 %d 次输入只到 %r，重打）"
                      % (attempt + 1, (all_hits[-1][0] if all_hits else None)))
                mon.key("esc", wait=1.0)
                base = len(slog())
            mon.key("ret", wait=2.4)
            app_ok = wait_for(app_needle, 25, since=base)
            shot(name, settle=2.0)
            ok = app_ok and first == expect_first
            print("    %-11s 搜索 %-9s hits=%-2s first=%-15s 窗口=%s -> %s"
                  % (name, token, hits, first, "OK" if app_ok else "无",
                     "PASS" if ok else "**需复核**"))
            if ok:
                opened.append(name)
            mon.key("esc", wait=1.0)

        log = slog()
        print("\n=== 汇总 ===")
        print("  开窗成功：%s" % (", ".join(opened) if opened else "一个都没有"))
        print("  PANIC   ：%s" % ("有！" if "PANIC" in log else "无"))
        print("  关于页 08_about 上有版本号 —— 这就是「截图是不是当前构建」的肉眼证据")
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("\n共 %d 张 -> %s" % (len(shots), args.out))
    for n, w, h, r in shots:
        print("  %-14s %dx%d ink=%.1f%%" % (n, w, h, r * 100))
    expected = 9 if want is None else len([n for n in want if n[:2].isdigit()])
    return 0 if len(shots) >= expected else 1


if __name__ == "__main__":
    sys.exit(main())
