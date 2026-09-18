#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/screen64_probe.py - 自动视觉验收：抓 QEMU 屏幕并检查像素

为什么需要它：安装程序是图形界面，"串口日志说画完了"不等于"屏幕上真的画对了"。
本脚本用 QEMU monitor 的 screendump 抓一帧 PPM，然后按像素做断言，
这样 M2（Win10 同款安装界面）每一步都能自动验收，而不是靠人眼看截图。

用法：
    python tests/screen64_probe.py                       # 默认抓 vimtu64-64.img
    python tests/screen64_probe.py --expect-bg 0,84,158  # 断言背景主色
    python tests/screen64_probe.py --out shot.ppm --keep # 保留截图供人工查看

退出码：0 = 断言通过；1 = 有断言失败；2 = 环境问题
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


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            found = shutil.which(c)
            if found:
                return found
    return None


def monitor_cmd(port, command, wait=1.5):
    """向 QEMU monitor 发一条命令（走 telnet 端口）。"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=6)
    except OSError as e:
        return b"", str(e)
    data = b""
    try:
        s.sendall(command.encode() + b"\n")
        time.sleep(wait)
        s.settimeout(1.0)
        while True:
            try:
                chunk = s.recv(4096)
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                break
    finally:
        s.close()
    return data, None


def read_ppm(path):
    """读 P6 PPM（QEMU screendump 输出），返回 (w, h, pixels bytearray)。"""
    with open(path, "rb") as f:
        raw = f.read()
    # 头：P6 <ws> w <ws> h <ws> maxval <single ws>
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":          # 注释行
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1                                     # 头结束的一个空白
    w, h, maxval = fields
    px = raw[idx:]
    if len(px) < w * h * 3:
        raise ValueError("像素数据不足：期望 %d 字节，实际 %d" % (w * h * 3, len(px)))
    return w, h, px


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "vimtu64-64.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--wait", type=float, default=9.0, help="等界面画完的秒数")
    ap.add_argument("--out", default=None, help="截图保存路径（默认临时文件）")
    ap.add_argument("--expect-bg", default="0,84,158", help="期望的背景主色 R,G,B")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    out = args.out or os.path.join(tempfile.mkdtemp(prefix="vimtu64_shot_"), "screen.ppm")
    if os.path.exists(out):
        os.remove(out)

    def q(p):
        return p.replace("\\", "/")

    qemu_args = [
        qemu, "-name", "Vimtu64-screen",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "null",
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    try:
        # 等 monitor 端口就绪
        for _ in range(40):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        time.sleep(args.wait)
        resp, err = monitor_cmd(args.port, "screendump %s" % q(out), wait=2.5)
        if err:
            print("[FAIL] monitor 连接失败：%s" % err)
            return 2
        sys.stderr.write("[probe] monitor resp: %r\n" % resp[:80])
        for _ in range(20):
            if os.path.exists(out) and os.path.getsize(out) > 1024:
                break
            time.sleep(0.3)
        if not os.path.exists(out):
            print("[FAIL] 没有拿到 screendump（%s）" % out)
            return 2

        w, h, px = read_ppm(out)
        print("=== 屏幕视觉验收（%dx%d）===" % (w, h))

        def check(name, cond, detail=""):
            nonlocal ok
            ok = ok and cond
            print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

        # 断言 1：分辨率与预期一致（VBE 1280x800）
        check("分辨率 1280x800", (w, h) == (1280, 800), "实际 %dx%d" % (w, h))

        # 断言 2：背景主色 = Win10 安装程序的深蓝（采样屏幕中下部区域）
        exp = tuple(int(v) for v in args.expect_bg.split(","))
        cnt = 0
        total = 0
        for y in range(400, 700, 20):
            for x in range(400, 1200, 20):
                c = sample(px, w, x, y)
                total += 1
                if all(abs(c[i] - exp[i]) <= 6 for i in range(3)):
                    cnt += 1
        ratio = cnt / max(1, total)
        check("背景主色 %s 占比 >70%%" % (exp,), ratio > 0.70, "实际 %.1f%%" % (ratio * 100))

        # 断言 3：顶部标题条颜色（Win10 安装程序的深色标题条）
        bar = sample(px, w, 640, 20)
        check("顶部标题条颜色", all(abs(bar[i] - v) <= 10 for i, v in enumerate((0, 62, 120))),
              "实际 %s" % (bar,))

        # 断言 4：标题区有白色文字像素（说明 TTF 字形真的画上去了）
        white = 0
        for y in range(30, 100, 2):
            for x in range(40, 900, 2):
                c = sample(px, w, x, y)
                if c[0] > 200 and c[1] > 200 and c[2] > 200:
                    white += 1
        check("标题区有白色文字", white > 100, "白色像素采样数=%d" % white)

        # 断言 5：右下角主按钮（Win10 的"下一步"按钮：蓝底 0,120,215）
        bx, by = w - 160, h - 64
        btn = sample(px, w, bx, by)
        check("右下角主按钮为 Win10 蓝", all(abs(btn[i] - v) <= 14 for i, v in enumerate((0, 120, 215))),
              "坐标(%d,%d) 实际 %s" % (bx, by, btn))

        print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
        print("截图：%s" % out)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
        if not args.keep and args.out is None and not ok:
            pass  # 失败时保留截图，便于排查
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
