#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/desktop64_test.py - 64 位桌面栈自动验收（串口 + 像素 + 输入）

验收目标（每一层都要有证据，不能只看"没崩"）：
  1) 内存层      [MEM64] selftest PASS（堆/页池/归属记账/坏指针防护）
  2) 外壳自检    [GUI64] selftest PASS（窗口建销/z 序/几何/close_app/语言/脏矩形）
  3) 桌面起来    [GUI64] desktop init WxH + [GUI64] ready + [OS] ready (idle)
  4) 纯色桌面    桌面主色（Win10 蓝 0,84,158）占屏幕多数像素
  5) 任务栏      底部 32px 是深色条，且有条内白色文字（时钟）
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
                nblue = count_color(px, w, h, (0, 84, 158), tol=8, step=8)
                total = ((w + 7) // 8) * ((h + 7) // 8)
                ratio = nblue / max(1, total)
                check("桌面主色占比 >40%", ratio > 0.40, "实际 %.1f%%" % (ratio * 100))
                bar = sample(px, w, 640, 785)
                check("任务栏是深色条", all(abs(bar[i] - v) <= 12 for i, v in enumerate((20, 20, 20))),
                      "y=785 实际 %s" % (bar,))
                # 时钟是右对齐的 8 个数字，字形细、抗锯齿，必须 step=1 精确数
                white = 0
                for y in range(772, 798, 1):
                    for x in range(1000, 1279, 1):
                        c = sample(px, w, x, y)
                        if c[0] > 200 and c[1] > 200 and c[2] > 200:
                            white += 1
                check("任务栏有白色时钟文字", white > 25, "白色像素=%d" % white)
                dwin0 = count_color(px, w, h, (32, 32, 32), tol=6, step=2)
                check("开窗之前没有窗口标题栏", dwin0 < 200, "(32,32,32) 像素=%d" % dwin0)

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
                dwin1 = count_color(px3, w3, h3, (32, 32, 32), tol=6, step=2)
                check("开窗后出现标题栏像素（>2000）", dwin1 > 2000, "(32,32,32) 像素=%d" % dwin1)
                # 任务栏窗口按钮：任务栏里应该出现非纯深色的按钮块
                # 任务栏窗口按钮：只在任务栏条内按 step=1 精确统计（不用全屏抽样）
                # 单独扫任务栏区域（避免全屏 step 混入）
                n_tb = 0
                for y in range(772, 796, 2):
                    for x in range(40, 900, 3):
                        c = sample(px3, w3, x, y)
                        if abs(c[0] - 48) <= 14 and abs(c[1] - 48) <= 14 and abs(c[2] - 48) <= 14:
                            n_tb += 1
                check("任务栏出现窗口按钮", n_tb > 20, "按钮色像素=%d" % n_tb)
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
