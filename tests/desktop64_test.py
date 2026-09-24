#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/desktop64_test.py - 64 位桌面栈自动验收（串口 + 像素 + 输入）

验收目标（每一层都要有证据，不能只看"没崩"）：
  1) 内存层      [MEM64] selftest PASS（堆/页池/归属记账/坏指针防护）
  2) 外壳自检    [GUI64] selftest PASS（窗口建销/z 序/几何/close_app/语言/脏矩形）
  3) 桌面起来    [GUI64] desktop init WxH + [GUI64] ready + [OS] ready (idle)
  4) 壁纸桌面    默认白色主题：桌面区是浅色壁纸（不是纯色块，有渐变/纹理），且不是旧的 Win10 蓝
  5) Dock 栏     底部 76px 保留区里是 Windows 11 风格 Dock（面板色与壁纸不同 + 右侧时钟玻璃片有文字）
  6) 应用能开    用 Win 键 + 数字快捷键逐个打开 8 个应用，断言各自的串口开场行
  7) 窗口真的画  开窗后 (32,32,32) 标题栏像素数从 ~0 跳到几千（前后对比）
  8) 脏矩形      鼠标移动后只有小块像素变化（>20 且 <3% 屏幕），证明不是整屏重绘
  9) 无异常      串口不得出现 PANIC / OOM / bad free / selftest FAIL

为什么必须做像素断言：串口说"画完了"不等于屏幕上真的画对了（这条经验来自安装界面
阶段：当时日志全绿但屏幕是花的）。本脚本用 QEMU monitor 的 screendump 抓 PPM 判像素。

用法：
    python tests/desktop64_test.py            # 默认引导 build64/system.img（=装好的系统）
    python tests/desktop64_test.py --img X    # 指定其它"已安装系统"镜像
    python tests/desktop64_test.py --keep     # 保留截图
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
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

# 开始菜单里的数字快捷键：数字键 -> 菜单序号（0='1' ... 9='0'）
# 菜单顺序（与 shell 一致）：0终端 1我的电脑 2系统监视器 3计算器 4扫雷 5设置 6任务管理器 7关于
DIGIT_APPS = [
    ("1", "app_term_open64", "[APP] term opened"),
    ("2", "app_mypc_open64", "[APP] mypc opened"),
    ("3", "app_monitor_open64", "[APP] monitor opened"),
    ("4", "app_calc_open64", "[APP] calc opened"),
    ("5", "app_mines_open64", "[APP] mines opened"),
    ("6", "app_settings_open64", "[APP] settings opened"),
    ("7", "app_tmgr_open64", "[APP] tmgr opened"),
    ("8", "app_about_open64", "[APP] about opened"),
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
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


class Monitor:
    """QEMU monitor 客户端（telnet 端口）。"""

    def __init__(self, port):
        self.port = port
        self.buf = b""

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
            s.settimeout(1.0)
            data = b""
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
        self.buf += data
        return data

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def mouse_move(self, dx, dy):
        self.send("mouse_move %d %d" % (dx, dy), wait=0.6)

    def shot(self, path, wait=2.5):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
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
    w, h, _ = fields
    return w, h, raw[idx:]


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def count_color(px, w, h, target, tol=10, step=1):
    """统计接近 target 的像素数（step>1 时抽样）。"""
    n = 0
    for y in range(0, h, step):
        for x in range(0, w, step):
            c = sample(px, w, x, y)
            if abs(c[0] - target[0]) <= tol and abs(c[1] - target[1]) <= tol and abs(c[2] - target[2]) <= tol:
                n += 1
    return n


def diff_count(px1, px2):
    """两张同尺寸 PPM 的字节级差异像素数。"""
    n = 0
    for i in range(0, min(len(px1), len(px2)), 3):
        if px1[i] != px2[i] or px1[i + 1] != px2[i + 1] or px1[i + 2] != px2[i + 2]:
            n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"),
                    help="已安装系统镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5588)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s\n（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_desktop_")
    serial = os.path.join(tmp, "serial.log")
    shot1 = os.path.join(tmp, "s1.ppm")
    shot2 = os.path.join(tmp, "s2.ppm")
    shot3 = os.path.join(tmp, "s3.ppm")

    qemu_args = [
        qemu, "-name", "Vimtu64-desktop",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ]
    print("[desktop] 引导 %s" % args.img)
    proc = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond), detail))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for(needle, timeout, what):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in slog():
                print("  [ok] %s（%.1fs）" % (what, time.time() - t0))
                return True
            if proc.poll() is not None:
                print("  [!] QEMU 提前退出（复位/三重故障？）")
                return False
            time.sleep(0.5)
        print("  [!] 等 %r 超时 %.0fs" % (needle, timeout))
        return False

    try:
        # monitor 端口就绪
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        mon = Monitor(args.port)

        print("=== 1) 等桌面起来 ===")
        up = wait_for("[GUI64] ready", 60, "桌面就绪 [GUI64] ready")
        log = slog()
        check("等待桌面就绪", up)
        check("内存自检通过", "[MEM64] selftest PASS" in log)
        check("外壳自检通过", "[GUI64] selftest PASS" in log)
        check("已安装系统启动路径", "[OS] booted from installed disk" in log)
        check("兼容断言 [OS] ready (idle)", "[OS] ready (idle)" in log)

        print("=== 2) 首帧像素 ===")
        if not up:
            print("  桌面没起来，跳过像素断言")
        else:
            mon.shot(shot1)
            w, h, px = read_ppm(shot1)
            check("分辨率 1280x800", (w, h) == (1280, 800), "实际 %dx%d" % (w, h))
            if (w, h) == (1280, 800):
                # 本批（Windows 11 现代外观）：默认白色主题，桌面是"壁纸 + 适应模式"，
                # 不再是纯 Win10 蓝。这里锁三件事：不是旧蓝、是浅色、有纹理（不是纯色块）。
                nblue = count_color(px, w, h, (0, 84, 158), tol=8, step=8)
                total = ((w + 7) // 8) * ((h + 7) // 8)
                ratio = nblue / max(1, total)
                check("不再是旧的 Win10 纯蓝桌面（占比 <5%）", ratio < 0.05, "旧蓝占比 %.2f%%" % (ratio * 100))
                # 桌面区（避开 Dock 与桌面图标）均值 + 方差
                tot = 0
                npx = 0
                vals = []
                for y in range(120, 420, 4):
                    for x in range(300, 900, 4):
                        c = sample(px, w, x, y)
                        tot += c[0] + c[1] + c[2]
                        npx += 1
                        vals.append(c[1])
                lum = tot / (3.0 * npx)
                mean = sum(vals) / len(vals)
                var = sum((v - mean) ** 2 for v in vals) / len(vals)
                check("白色主题：桌面是浅色（平均亮度 > 200）", lum > 200, "亮度 %.1f" % lum)
                check("桌面是壁纸（有渐变/纹理，方差 > 0.5）", var > 0.5, "绿通道方差 %.2f" % var)
                # Dock：面板色（固定浅灰）出现在保留区；上方壁纸不同
                bar = sample(px, w, 640, 750)
                above = sample(px, w, 640, 680)
                check("底部保留区里是 Dock 面板（与壁纸明显不同）",
                      sum(abs(bar[i] - above[i]) for i in range(3)) > 6, "面板 %s 壁纸 %s" % (bar, above))
                # 右侧时钟玻璃片：内有文字像素（与面板底色不同）
                text = 0
                for y in range(736, 772, 1):
                    for x in range(1058, 1260, 1):
                        c = sample(px, w, x, y)
                        if sum(abs(c[i] - bar[i]) for i in range(3)) > 45:
                            text += 1
                check("时钟玻璃片里有文字像素", text > 25, "文字像素=%d" % text)
                # 开窗之前：屏幕中心是壁纸（没有窗口）
                ctr = sample(px, w, 640, 400)
                check("开窗之前屏幕中心是壁纸（不是窗口内容）", ctr != (240, 240, 240),
                      "center=%s" % (ctr,))

        print("=== 3) 鼠标移动 -> 脏矩形（只该有一小块变化）===")
        if up:
            mon.mouse_move(300, 200)
            time.sleep(0.8)
            mon.shot(shot2)
            w2, h2, px2 = read_ppm(shot2)
            if (w, h) == (w2, h2):
                d = diff_count(px, px2)
                check("鼠标移动后有像素变化", d > 20, "变化像素=%d" % d)
                check("变化是局部脏矩形（<3% 屏幕）", d < int(w * h * 0.03),
                      "占比 %.3f%%" % (100.0 * d / (w * h)))
            else:
                check("两次截图尺寸一致", False, "%dx%d vs %dx%d" % (w, h, w2, h2))

        print("=== 4) 逐个打开 8 个应用（Win 键 + 数字快捷键）===")
        for digit, fn, need in DIGIT_APPS:
            before = slog()
            mon.key("meta_l", wait=1.0)          # 打开开始菜单
            if "[UI] menu open" not in slog():
                mon.key("meta_l", wait=1.0)      # 再试一次（按键丢失时）
            mon.key(digit, wait=2.2)             # 数字 = 菜单序号
            after = slog()
            opened = need in after and need not in before
            check("打开 %s" % need, opened or (need in after),
                  "" if opened else "（本次日志未见新行）")

        print("=== 5) 窗口绘制像素 + 菜单 ===")
        if up:
            mon.shot(shot3)
            w3, h3, px3 = read_ppm(shot3)
            if (w3, h3) == (1280, 800):
                # 窗口真的画上去了：与"开窗前"的同区域对比（屏幕中上部大面积变化）
                d_win = 0
                for y in range(60, 500, 2):
                    for x in range(200, 1100, 2):
                        if sample(px, w, x, y) != sample(px3, w3, x, y):
                            d_win += 1
                check("开窗后屏幕中上部大面积变化（窗口真的画出来了）", d_win > 5000, "变化点=%d" % d_win)
                log = slog()
                check("扫雷布局日志出现", "[UI] mines layout client=" in log,
                      "" if "[UI] mines layout client=" in log else "（未出现）")

        print("=== 6) 不能出现的日志（异常/泄漏/失败）===")
        log = slog()
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:",
                    "kfree: bad", "bad/double free", "FAILED mask="):
            check("不应出现 %s" % bad, bad not in log)

        if args.keep:
            print("截图：%s" % tmp)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    if not args.keep:
        print("（截图目录：%s）" % tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
