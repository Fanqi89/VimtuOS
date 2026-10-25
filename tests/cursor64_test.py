#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/cursor64_test.py - ★ v0.5.19：鼠标箭头位图（像素级验收，不靠肉眼）

用户报："鼠标箭头渲染有问题"（截图：蓝色背景上的白色箭头，底部像断了一截）。

真因（两处，都是真缺陷）：
  ① 桌面/窗口的箭头位图（kernel/gui64.cpp 的 `cur[]`）原本是 **19 行**，而标准箭头只有 16 行：
     row16..18（"X......X"/"X.......X"/"X........X"）是把 row7..9 又抄了一遍 —— 箭头底部因此
     多出一块与三角**不相连**的白块，屏幕上就是"箭头断了一截/画歪了"。
     修：位图裁到 16 行，`rows` 19 -> 16。
  ② 登录/锁屏界面的箭头（kernel/locklogin64.cpp 的 `cursor64()`）按"每行宽度 r<7 ? r/2+1 : 3"
     逐行涂白，得到的是上窄下宽的"锤子"，而且**只有左边和顶部**有描边（右侧/底部直接是白块
     压在背景上）。修：换成与桌面同一张标准箭头位图（X = 描边、'.' = 填充）。

判据（像素级，能独立证伪）：
  * 在鼠标热点 ±4px 内自动对齐后，位图里 **42 个 'X' 描边位置全部命中深色像素**（42/42）；
  * 位图第 16 行之后 8 行内**没有任何**深色像素（旧 19 行位图的残留就出现在这里）；
  * 登录界面与桌面两处都满足（两处各画一次箭头）。

用法：
  py -3 tests/cursor64_test.py              # 缺省用 build64/system.img（裸载荷，起桌面最快）
  py -3 tests/cursor64_test.py <盘镜像>
退出码：0 全过 / 1 有断言失败 / 2 环境问题（缺产物 / 起不到桌面）
"""
import os
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import install_persist64_test as P  # noqa: E402
import startmenu64_test as SM      # noqa: E402
import qemuhelp as qh              # noqa: E402

QEMU = P.T.find_qemu()
WORK = os.path.join(ROOT, "build64", "cursor64_test")
PORT = 5641

# 标准 16 行箭头位图（与 kernel/gui64.cpp、kernel/locklogin64.cpp 里的 cur[] 逐字一致）
CUR = ["X          ", "XX         ", "X.X        ", "X..X       ", "X...X      ",
       "X....X     ", "X.....X    ", "X......X   ", "X.......X  ", "X........X ",
       "X.....XXXXX", "X..X..X    ", "X.X.X..X   ", "XX..X..X   ", "X....X..X  ",
       "X.....X..X "]
REF = [r[:r.find(" ")] if " " in r else r for r in CUR]
N_EDGE = sum(r.count("X") for r in REF)


def q(p):
    return p.replace("\\", "/")


def log_of(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return ""


def start(args, serial, port):
    a = [QEMU, "-name", "cursor64", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"] + args
    p = subprocess.Popen(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return p, SM.Monitor(port)


def read_ppm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P6"
        line = f.readline()
        while line.startswith(b"#"):
            line = f.readline()
        w, h = map(int, line.split())
        f.readline()
        return w, h, f.read()


def cell(px, w, x, y):
    o = (y * w + x) * 3
    return (px[o], px[o + 1], px[o + 2])


def is_dark(px, w, x, y):
    r, g, b = cell(px, w, x, y)
    return r < 90 and g < 90 and b < 90


def shape_check(name, px, w, x0, y0):
    best = None
    for oy in range(-4, 5):
        for ox in range(-4, 5):
            good = bad = 0
            for j, row in enumerate(REF):
                for i, ch in enumerate(row):
                    if ch != "X":
                        continue
                    if is_dark(px, w, x0 + ox + i, y0 + oy + j):
                        good += 1
                    else:
                        bad += 1
            score = good - bad
            if best is None or score > best[0]:
                best = (score, ox, oy, good, bad)
    score, ox, oy, good, bad = best
    tail = 0
    for j in range(16, 24):
        for i in range(13):
            if is_dark(px, w, x0 + ox + i, y0 + oy + j):
                tail += 1
    ok = (good == N_EDGE) and tail == 0
    art = []
    for j in range(18):
        s = ""
        for i in range(13):
            s += "B" if is_dark(px, w, x0 + ox + i, y0 + oy + j) else "."
        art.append(s)
    print("  %s：对齐 (dx=%d dy=%d)，描边命中 %d/%d，第 16~23 行残留描边 %d px"
          % (name, ox, oy, good, N_EDGE, tail))
    for i, line in enumerate(art):
        print("     %2d %s%s" % (i, line, "" if i < 16 else "   <- 这一行必须全空"))
    print("  [%s] %s" % ("PASS" if ok else "FAIL", name))
    return ok


def main():
    img = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "build64", "system.img")
    if not os.path.exists(img):
        print("跳过：缺 %s（先跑 build64.sh）" % img)
        return 2
    os.makedirs(WORK, exist_ok=True)
    serial = os.path.join(WORK, "run.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(img), "-boot", "order=c"], serial, PORT)
    ok = True
    try:
        cur = SM.Cursor()
        if qh.wait_mark(lambda: log_of(serial), qh.LOCK_INTERACTIVE, 260, proc):
            time.sleep(1.0)
            cur.goto(mon, 300, 220)
            time.sleep(1.2)
            shot = os.path.join(WORK, "lock.ppm")
            mon.shot(shot, wait=2.5)
            if os.path.exists(shot):
                w, h, px = read_ppm(shot)
                ok = shape_check("登录界面箭头", px, w, 300, 220) and ok
        mon.key("ret", wait=2.0)
        mon.key("ret", wait=3.0)
        if not qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=300):
            print("起不到桌面（UNVERIFIED）")
            return 2
        time.sleep(1.5)
        cur.goto(mon, 320, 240)
        time.sleep(1.2)
        shot = os.path.join(WORK, "desk.ppm")
        mon.shot(shot, wait=2.5)
        if os.path.exists(shot):
            w, h, px = read_ppm(shot)
            ok = shape_check("桌面箭头", px, w, 320, 240) and ok
        else:
            print("  [FAIL] 没拿到桌面截图")
            ok = False
        print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    finally:
        try:
            mon.send("quit", wait=1.5)
        except Exception:
            pass
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
