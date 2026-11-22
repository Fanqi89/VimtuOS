"""tests/deco64_test.py —— ★ v0.11.1 第二步：wl_surface → Window（装饰归合成器）

这一刀要证明的事（一句话）：**PE 窗口的标题栏/圆角/边框/三按钮不再由客户端自绘，
而是合成器画的**，并且与内核窗口用同一份装饰实现（kernel/gui64.cpp 的
gui64_deco_frame64 / gui64_deco_edge64）。

判据分两段，缺一不算过：

  A. 串口（协议与实现的自证）
     * `[WL64] selftest PASS (… move 27 + decor 28 …)`     启动期 ABI 自检通过
     * `[PELOAD] msgboxA decor rc=0`                        客户端 28 号调用成功
     * `[WL64] decor id=<n> pid=<p> flags=1 mode=window hdr=25 title="<caption>"`
     * `[WL64] deco paint surf=<n> rect=x,y,w,h title="…" btns=3 corner=<r> hdr=25 parts=<k>`
     * `[WL64] blit surf=<n> … bytes=<n> clip=ok` 且 bytes == 4*w*(h-25)   —— 顶部 25 行确实让给了装饰

  B. 像素（QEMU screendump，对话框在屏时抓帧）—— 这是"真的上屏了"的唯一硬证据
     * 标题栏：rect 顶部 25 行里，与"标题栏中段色"相近的像素占多数（>= 80%）
     * **客户端旧标题栏色 0x3465A4 在标题带里出现 0 次** —— 证明客户端不再自绘（被接管）
     * 三个按钮：右上角 3×30 px 区域里有 close 按钮的红底像素（>= 20 px）
     * 四角圆角裁切：四个角 (r×r) 里"标题栏色"合计 < 75%，而紧邻的内部行 > 90%
     * 标题文字：标题带左侧有深色笔画像素（>= 20 px，客户端已经不画任何文字了）

用法：py -3 tests/deco64_test.py [--qemu PATH] [--keep]
退出码：0 = 全过（含 SKIP）；1 = 有断言失败；2 = 环境问题（缺 qemu / 缺盘 / 缺语料）
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh                                            # noqa: E402
import startmenu64_test as SM                                    # noqa: E402
import install_persist64_test as P                               # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files\qemu\qemu-system-x86_64w.exe",
]
DISK = os.path.join(ROOT, "build64", "sysdisk.img")
CORPUS_GUI = os.path.join(ROOT, "build64", "pexcorpus", "gui.exe")

ok = True
checks = []
skipped = []


def check(name, cond, detail="", skip_env=False):
    global ok
    if skip_env:
        skipped.append(name)
        print("  [SKIP] %s %s" % (name, detail), flush=True)
        return
    ok = ok and bool(cond)
    checks.append((name, bool(cond)))
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def q(p):
    return p.replace("\\", "/")


def find_qemu(cli):
    for c in ([cli] if cli else []) + QEMU_CANDIDATES:
        if c and os.path.exists(c):
            return c
    return None


def log_of(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def start(qemu, img, serial, port):
    if os.path.exists(serial):
        os.remove(serial)
    cmd = [qemu, "-name", "deco64",
           "-drive", "format=raw,file=%s" % q(img),
           "-boot", "order=c", "-m", "512", "-vga", "std",
           "-display", "none",
           "-serial", "file:%s" % q(serial),
           "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
           "-no-reboot"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(240):
        if proc.poll() is not None:
            break
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return proc, SM.Monitor(port)


def open_terminal_dock(log_fn, mon, timeout=120):
    """从 Dock 的终端图标开终端（与 peload64_test 同一条已验证路径）。"""
    deadline = time.time() + timeout
    hit = None
    while time.time() < deadline and not hit:
        lg = log_fn()
        if "[PANIC64] stop=" in lg:
            return False
        for mm in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(-?\d+) name=\S+ x=(\d+) y=(\d+) "
                              r"w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", lg):
            if mm.group(2) == "3":                                   # app=3 = 终端
                hit = mm
                break
        if not hit:
            time.sleep(1.0)
    if not hit:
        return False
    cur = SM.Cursor()
    cur.goto(mon, int(hit.group(7)), int(hit.group(8)))
    cur.settle(mon)
    time.sleep(0.3)
    mon.click()
    t0 = time.time()
    while time.time() - t0 < 90:
        lg = log_fn()
        if "[APP] term opened" in lg:
            return True
        if "[PANIC64] stop=" in lg:
            return False
        time.sleep(0.5)
    return False


# ==================== PPM（QEMU screendump）====================
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
    i = 2
    vals = []
    while len(vals) < 3:
        while i < len(raw) and raw[i:i + 1].isspace():
            i += 1
        if raw[i:i + 1] == b"#":
            while i < len(raw) and raw[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(raw) and not raw[j:j + 1].isspace():
            j += 1
        vals.append(int(raw[i:j]))
        i = j
    while i < len(raw) and raw[i:i + 1].isspace():
        i += 1
    w, h, _maxv = vals
    need = w * h * 3
    body = raw[i:i + need]
    if len(body) < need:
        raise ValueError("PPM 像素不足：%d < %d" % (len(body), need))
    return w, h, body


def px_at(img, x, y):
    w, h, body = img
    if x < 0 or y < 0 or x >= w or y >= h:
        return (0, 0, 0)
    o = (y * w + x) * 3
    return (body[o], body[o + 1], body[o + 2])


def near(c1, c2, tol):
    return abs(c1[0] - c2[0]) <= tol and abs(c1[1] - c2[1]) <= tol and abs(c1[2] - c2[2]) <= tol


def count_region(img, x0, y0, x1, y1, pred):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if pred(px_at(img, x, y)):
                n += 1
    return n


def screendump(mon, path, wait=1.2):
    if os.path.exists(path):
        os.remove(path)
    mon.send("screendump %s" % q(path), wait=wait)
    t0 = time.time()
    while time.time() - t0 < 12:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.2)                 # 让 QEMU 写完
            return True
        time.sleep(0.2)
    return False


# ==================== 串口切片 ====================
DECOR_RE = (r'\[WL64\] decor id=(\d+) pid=(-?\d+) flags=(\d+) mode=(\w+) hdr=(\d+) title="([^"]*)"')
PAINT_RE = (r"\[WL64\] deco paint surf=(\d+) rect=(-?\d+),(-?\d+),(\d+),(\d+) "
            r'title="([^"]*)" btns=(\d+) corner=(\d+) hdr=(\d+) parts=(\d+)')
BLIT_RE = (r"\[WL64\] blit surf=(\d+) shm=\d+ off=\d+ src=-?\d+,-?\d+,\d+,\d+ "
           r"dst=(-?\d+),(-?\d+) bytes=(\d+) clip=ok")
SHOW_RE = (r'\[PELOAD\] msgboxA show surf=(\d+) shm=(\d+) w=(\d+) h=(\d+) type=(\d+) '
           r'caption="([^"]*)" text="([^"]*)"')
CLOSE_RE = r"\[PELOAD\] msgboxA close reason=(\w+)"
EXIT_RE = r"\[PROC64\] exit pid=\d+ code=(\d+)"


def m1(rx, seg, grp=1):
    m = re.search(rx, seg)
    return m.group(grp) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu or not os.path.exists(DISK):
        sys.stderr.write("环境问题：qemu=%r disk=%r\n" % (qemu, DISK))
        return 2
    if not os.path.exists(CORPUS_GUI):
        sys.stderr.write("环境问题：缺 PE 语料 %s\n" % CORPUS_GUI)
        return 2

    work = tempfile.mkdtemp(prefix="vimtu64_deco_")
    img = os.path.join(work, "disk.img")
    serial = os.path.join(work, "run.log")
    shot = os.path.join(work, "deco.ppm")
    shutil.copyfile(DISK, img)          # ★ 绝不写共享产物（build64/sysdisk.img）
    print("=== VimtuOS v0.11.1 第二步：wl_surface → Window（装饰归合成器）===")
    print("    盘：%s" % img)
    print()

    log_fn = lambda: log_of(serial)                                  # noqa: E731
    proc = None
    try:
        port = qh.free_port()
        proc, mon = start(qemu, img, serial, port)
        if not qh.wait_desktop(mon, log_fn, proc, timeout=400, login_timeout=460):
            check("桌面就绪（qh.wait_desktop 含登录手势）", False, "超时")
            raise SystemExit(1)
        check("桌面就绪（qh.wait_desktop 含登录手势）", True)

        # ---------- A0：启动期 ABI 自检（含 decor 28 + 装饰高与 gui64 同一把尺子）----------
        lg = log_fn()
        check("A0. [WL64] selftest PASS 且含 move 27 + decor 28",
              "[WL64] selftest PASS" in lg and "decor 28" in lg,
              "" if "[WL64] selftest PASS" in lg else "缺 PASS 行")

        if not open_terminal_dock(log_fn, mon):
            check("终端打开（[APP] term opened）", False, "Dock 入口没成功")
            raise SystemExit(1)
        check("终端打开（[APP] term opened）", True)
        time.sleep(1.5)

        # ---------- 跑 gui.exe：观察 decor -> commit -> deco paint -> 上屏 ----------
        before = len(log_fn())
        P.type_line(mon, "run /bin/peload /pexcorpus/gui.exe")
        seg = ""
        deadline = time.time() + 240
        shot_ok = False
        while time.time() < deadline:
            seg = log_fn()[before:]
            if not shot_ok and re.search(BLIT_RE, seg) and re.search(PAINT_RE, seg):
                time.sleep(1.0)                     # 让这一帧画完
                shot_ok = screendump(mon, shot, wait=1.0)
            if re.search(CLOSE_RE, seg) and re.search(EXIT_RE, seg):
                break
            if "[PANIC64] stop=" in seg:
                break
            time.sleep(0.3)
        time.sleep(0.5)
        seg = log_fn()[before:]

        dec = re.search(DECOR_RE, seg)
        pnt = re.search(PAINT_RE, seg)
        show = re.search(SHOW_RE, seg)
        blits = re.findall(BLIT_RE, seg)

        check("A1. 客户端调 28 号成功（[PELOAD] msgboxA decor rc=0）",
              "[PELOAD] msgboxA decor rc=0" in seg,
              m1(r"\[PELOAD\] msgboxA decor rc=(-?\d+)", seg, 1) or "缺行")
        check("A2. 内核认了装饰（[WL64] decor … mode=window hdr=25 + 标题=PE 的 caption）",
              dec is not None and dec.group(4) == "window" and dec.group(5) == "25"
              and show is not None and dec.group(6) == show.group(6),
              "" if dec is None else "id=%s title=%r hdr=%s" % (dec.group(1), dec.group(6), dec.group(5)))
        check("A3. 合成器真的画了装饰（[WL64] deco paint … btns=3 corner>=8 hdr=25 parts>=1）",
              pnt is not None and pnt.group(7) == "3" and int(pnt.group(8)) >= 8
              and pnt.group(9) == "25" and int(pnt.group(10)) >= 1,
              "" if pnt is None else "rect=%s,%s,%s,%s parts=%s" % pnt.group(2, 3, 4, 5, 10))
        want_bytes = (int(show.group(3)) * (int(show.group(4)) - 25) * 4) if show else 0
        check("A4. 内容上屏字节数 == 4*w*(h-25)（= 顶部 25 行确实让给了装饰）",
              bool(show) and any(int(b[3]) == want_bytes for b in blits),
              "want=%d got=%s" % (want_bytes, [b[3] for b in blits]))
        check("A5. 对话框走完生命周期并退出 0（reason=clicked|timeout）",
              re.search(CLOSE_RE, seg) is not None and m1(EXIT_RE, seg) == "0",
              "reason=%s exit=%s" % (m1(CLOSE_RE, seg), m1(EXIT_RE, seg)))

        # ---------- B：像素判据（screendump）----------
        if not shot_ok or not os.path.exists(shot):
            check("B0. 对话框在屏时抓到了 screendump", False, "截图未生成")
        else:
            check("B0. 对话框在屏时抓到了 screendump", True, shot)
            im = read_ppm(shot)
            if pnt is None:
                check("B1. 有 rect 才能做像素判据", False, "缺 deco paint 行")
            else:
                wx, wy, ww, wh = (int(pnt.group(2)), int(pnt.group(3)),
                                  int(pnt.group(4)), int(pnt.group(5)))
                rad = int(pnt.group(8))
                tc = px_at(im, wx + ww // 2, wy + 12)          # 标题栏中段（避开文字/按钮）
                band_x0, band_x1 = wx + ww // 4, wx + ww * 3 // 4
                band_y0, band_y1 = wy + 2, wy + 22
                tot = (band_x1 - band_x0) * (band_y1 - band_y0)
                same = count_region(im, band_x0, band_y0, band_x1, band_y1,
                                    lambda c: near(c, tc, 12))
                check("B1. 标题栏是合成器画的实心带（中段同色占比 >= 80%%，色=%d,%d,%d）" % tc,
                      tot > 0 and same * 100 >= tot * 80, "%d/%d" % (same, tot))

                blue = count_region(im, wx, wy, wx + ww, wy + 25,
                                    lambda c: near(c, (0x34, 0x65, 0xA4), 6))
                check("B2. **客户端旧标题栏色 0x3465A4 在标题带里 0 次**（= 客户端不再自绘）",
                      blue == 0, "命中 %d px" % blue)

                red = count_region(im, wx + ww - 30 - 3, wy + 4, wx + ww - 3, wy + 22,
                                   lambda c: c[0] > 140 and c[1] < 110 and c[2] < 110)
                check("B3. 三个按钮里的 close 红底可见（右上角 >= 20 px 红）", red >= 20,
                      "%d px" % red)

                # 圆角裁切：四角 (r x r) 里"标题栏色"必须少（没裁的话角上会是 100% 标题色，
                # 因为标题带横贯整个窗口宽度）；紧邻的内部行必须多。
                # 这里用**严格色**（tol=2）而不是 tol=12：本机壁纸在被测位置恰好是接近白的浅色，
                # 松容差会把"壁纸"和"标题带"算成同色（实测 tol=12 时 85%，tol=2 时 27%）。
                corners = [(wx, wy), (wx + ww - rad, wy), (wx, wy + wh - rad), (wx + ww - rad, wy + wh - rad)]
                cor_tot = rad * rad * 4
                cor_same = 0
                for (cx, cy) in corners:
                    cor_same += count_region(im, cx, cy, cx + rad, cy + rad,
                                             lambda c: near(c, tc, 2))
                inner_tot = (ww - 2 * rad) * 4
                inner_same = count_region(im, wx + rad, wy + 2, wx + ww - rad, wy + 6,
                                          lambda c: near(c, tc, 2))
                check("B4. 四角做了圆角裁切（角区严格同色 < 75%，内部严格同色 > 90%）",
                      cor_tot > 0 and inner_tot > 0 and cor_same * 100 < cor_tot * 75
                      and inner_same * 100 >= inner_tot * 90,
                      "角 %d/%d，内部 %d/%d" % (cor_same, cor_tot, inner_same, inner_tot))

                # 标题文字（TTF，合成器画的）：标题带左侧必须有深色笔画像素。
                # 客户端已经**不再画任何文字**（mb_draw 只画正文 + OK），所以这些深色像素
                # 只能来自合成器 —— 这是"标题栏被接管"的第二个像素证据。
                dark = count_region(im, wx + 4, wy + 3, wx + ww - 100, wy + 22,
                                    lambda c: (c[0] + c[1] + c[2]) < 360)
                check("B5. 标题带里有合成器画的标题文字（深色笔画像素 >= 20）", dark >= 20,
                      "%d px" % dark)
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("\n保留：%s" % work)

    npass = sum(1 for _, c in checks if c)
    print("\n=== RESULT: %s === checks=%d ok=%d fail=%d skip=%d"
          % ("PASS" if ok else "FAIL", len(checks), npass, len(checks) - npass, len(skipped)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
