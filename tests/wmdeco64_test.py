"""tests/wmdeco64_test.py —— ★ v0.11.2：Ring 3 合成器（/bin/wm）接管窗口装饰 + 三按钮真实语义

证明两件事：
  A. **/bin/wm 也画装饰**：PE 对话框（`run /bin/peload /pexcorpus/gui.exe`）在 wm 合成路下
     标题带 / 三按钮 / 圆角都在 —— 判据 = 串口（`[WM] title` / `[WM] surf … flags`）+ screendump 像素。
  B. **三按钮不是只画不接**：点最小化 -> `[WL64] ctl … action=collapse collapsed=1` 且窗高变 25；
     再点 -> collapsed=0；点最大化 -> frame=0,0,<屏宽>,<屏高>；再点还原；
     点关闭 -> `[WL64] ctl … action=close` + surface destroy + 客户端收到 CLOSE 事件（`close by=composer`）。

坐标全部从**串口**读（`[WM] place` 给框原点、`[PELOAD] msgboxA show` 给宽高），按钮布局与
`user/wm/wm.c` 的 wm_deco_frame 同一把尺子（槽 30 / 块 24x16 / 右边距 1）。

用法：py -3 tests/wmdeco64_test.py [--qemu PATH] [--keep] [--timeout SEC]
退出码：0 = 全过（含 SKIP）；1 = 有失败；2 = 环境问题
"""
import argparse
import os
import re
import shutil
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
import deco64_test as D                                          # noqa: E402（起 QEMU / 开终端 / 读日志）
import wmfull64_test as F                                        # noqa: E402（闭环光标 goto_exact / TcpSerial）

DISK = os.path.join(ROOT, "build64", "sysdisk.img")
CORPUS_GUI = os.path.join(ROOT, "build64", "pexcorpus", "gui.exe")
WM_ELF = os.path.join(ROOT, "build64", "wm.elf")
IDLE_MS = 60000            # 给 peload 的对话框一个宽裕的空闲超时（默认 8 s 不够点五下）

PLACE_RE = r"\[WM\] place id=(\d+) pos=(-?\d+),(-?\d+) mode=\w+"
SURF_RE = r"\[WM\] surf id=(\d+) pid=-?\d+ rect=-?\d+,-?\d+,\d+,\d+ pix=\d+ flags=0x([0-9a-fA-F]+)"
TITLE_RE = r'\[WM\] title id=(\d+) text="([^"]*)"'
BTN_RE = r"\[WM\] btn id=(\d+) which=(\w+) action=(\w+) rc=(-?\d+)"
CTL_RE = r"\[WL64\] ctl id=(\d+) pid=-?\d+ action=(\w+)(?: by=(\w+))?(.*)"
DESTROY_RE = r"\[WL64\] surface destroy id=(\d+) pid=-?\d+ .*why=(\w+)"
SHOW_RE = r"\[PELOAD\] msgboxA show surf=(\d+) shm=\d+ w=(\d+) h=(\d+)"
KCLOSE_RE = r"\[PELOAD\] msgboxA close by=composer surf=(\d+)"
CLOSE_RE = r"\[PELOAD\] msgboxA close reason=(\w+)"
EXIT_RE = r"\[PROC64\] exit pid=\d+ code=(\d+)"

ok = True
checks = []


def check(name, cond, detail="", skip_env=False):
    global ok
    if skip_env:
        print("  [SKIP] %s %s" % (name, detail), flush=True)
        return
    ok = ok and bool(cond)
    checks.append((name, bool(cond)))
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def m1(rx, seg, grp=1):
    m = re.search(rx, seg)
    return m.group(grp) if m else None


def shot(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.send("screendump %s" % path.replace("\\", "/"), wait=1.2)
    t0 = time.time()
    while time.time() - t0 < 12:
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            time.sleep(0.25)
            return D.read_ppm(path)
        time.sleep(0.2)
    return None


def click_at(mon, log_fn, x, y):
    """闭环把光标送到 (x,y) 再点击（复用 wmfull64_test 的 goto_exact：靠客户机自报光标位置收敛）"""
    F.goto_exact(mon, log_fn, x, y, tol=3, passes=6, verbose=False)
    mon.click()
    time.sleep(0.4)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=420)
    args = ap.parse_args()

    qemu = D.find_qemu(args.qemu)
    miss = [p for p in (DISK, CORPUS_GUI, WM_ELF) if not os.path.exists(p)]
    if not qemu or miss:
        sys.stderr.write("环境问题：qemu=%r 缺 %s\n" % (qemu, miss))
        return 2

    work = tempfile.mkdtemp(prefix="vimtu64_wmdeco_")
    img = os.path.join(work, "disk.img")
    shutil.copyfile(DISK, img)
    print("=== VimtuOS v0.11.2：Ring 3 合成器接管装饰 + 三按钮真实语义 ===")
    print("    盘：%s" % img)

    proc, ser, mon = None, None, None
    try:
        sport, mport = F.free_port(), F.free_port()
        proc = F.start_qemu(qemu, img, sport, mport)
        ser = F.TcpSerial(sport)
        mon = SM.Monitor(mport)
        log_fn = ser.text
        if not qh.wait_desktop(mon, log_fn, proc, timeout=400, login_timeout=460):
            check("A0. 起桌面", False, "超时")
            raise SystemExit(1)
        check("A0. 起桌面（含登录手势）", True)

        if not D.open_terminal_dock(log_fn, mon):
            check("A1. 开终端（[APP] term opened）", False, "Dock 入口没成功")
            raise SystemExit(1)
        check("A1. 开终端（[APP] term opened）", True)
        time.sleep(1.2)

        # ---------- 把 Ring 3 合成器起成**后台常驻**进程（与 wmfull64_test 同一条已验证路径）----------
        # 为什么要手动起：内核启动期那次 `wl64_wm64` 是**有界演示**（没有 /etc/wm_probe 时 short 模式
        # 20 s 就被收掉，串口会打 `[WL64] wm done … composer=0`）。验收要的是一个跑得够久的合成器。
        wm_before = len(log_fn())
        P.type_line(mon, "elfrun bg /lib/wm.elf")
        t0 = time.time()
        composer = None
        while time.time() - t0 < 120:
            composer = re.search(r"\[WM\] composer seat=\d+ gpu=\d+ region=", log_fn()[wm_before:])
            if composer:
                break
            time.sleep(0.4)
        check("A1b. 合成器常驻起来（elfrun bg /lib/wm.elf -> [WM] composer）", composer is not None,
              "" if composer is None else composer.group(0))
        if composer is None:
            raise SystemExit(1)
        time.sleep(1.0)

        # ---------- 跑 PE 对话框（带 60 s 空闲超时）----------
        before = len(log_fn())
        P.type_line(mon, "run /bin/peload /pexcorpus/gui.exe %d" % IDLE_MS)
        seg = ""
        t0 = time.time()
        while time.time() - t0 < 180:
            seg = log_fn()[before:]
            if re.search(SHOW_RE, seg) and re.search(r"\[WM\] place id=", seg):
                break
            if "[PANIC64] stop=" in seg:
                break
            time.sleep(0.4)
        time.sleep(1.2)
        seg = log_fn()[before:]

        show = re.search(SHOW_RE, seg)
        place = re.search(PLACE_RE, seg)
        title = re.search(TITLE_RE, seg)
        surfs = {int(m.group(1)): int(m.group(2), 16) for m in re.finditer(SURF_RE, seg)}

        check("A2. wm 注册为合成器（[WM] composer … region=整屏）",
              re.search(r"\[WM\] composer seat=\d+ gpu=\d+ region=0,0,\d+,\d+", log_fn()) is not None, "")
        check("A3. wm 读到装饰窗口的标题（[WM] title … text=PE 的 caption）",
              title is not None and show is not None and title.group(2) != "",
              "" if title is None else "title=%r" % title.group(2))
        # 注：`[WM] surf` 是在 wm **第一次看到**这条 surface 时打的，而客户端"建立面 -> 调 28 号设装饰"
        # 之间有一个窗口期（实测那一行是 flags=0x0）；装饰位在后来的导出里就是 0x8。所以判据取
        # "任意一次 surf 行带 0x8 **或** wm 成功读到标题（29 号只在有装饰时才被调用）"。
        check("A4. 23 号导出带装饰位（[WM] surf … flags & 0x8，或已读到标题）",
              (bool(surfs) and any(f & 0x8 for f in surfs.values())) or title is not None,
              "flags=%s" % {k: hex(v) for k, v in surfs.items()})
        if not (show and place):
            check("A5. 拿到对话框几何（[PELOAD] show + [WM] place）", False, "缺行")
            print("    串口尾部：\n%s" % log_fn()[-2500:])
            raise SystemExit(1)
        dw, dh = int(show.group(2)), int(show.group(3))
        fx, fy = int(place.group(2)), int(place.group(3))
        check("A5. 拿到对话框几何（[PELOAD] show + [WM] place）", True,
              "rect=%d,%d,%dx%d" % (fx, fy, dw, dh))

        # ---------- B. 装饰像素（wm 画的，不是客户端自绘）----------
        im = shot(mon, os.path.join(work, "deco.ppm"))
        if im is None:
            check("B0. screendump 可抓", False, "截图未生成")
        else:
            check("B0. screendump 可抓", True, "")
            tc = D.px_at(im, fx + dw // 2, fy + 12)
            bx0, bx1, by0, by1 = fx + dw // 4, fx + dw * 3 // 4, fy + 2, fy + 22
            tot = (bx1 - bx0) * (by1 - by0)
            same = D.count_region(im, bx0, by0, bx1, by1, lambda c: D.near(c, tc, 12))
            check("B1. 标题带是 wm 画的实心带（中段同色 >= 80%%，色=%d,%d,%d）" % tc,
                  tot > 0 and same * 100 >= tot * 80, "%d/%d" % (same, tot))
            blue = D.count_region(im, fx, fy, fx + dw, fy + 25, lambda c: D.near(c, (0x34, 0x65, 0xA4), 6))
            check("B2. 客户端旧标题栏色 0x3465A4 在标题带里 0 次", blue == 0, "命中 %d px" % blue)
            red = D.count_region(im, fx + dw - 33, fy + 4, fx + dw - 3, fy + 22,
                                 lambda c: c[0] > 180 and c[1] < 90 and c[2] < 90)
            check("B3. 右上角 close 按钮红底可见（>= 20 px）", red >= 20, "%d px" % red)
            rad = 14
            corners = [(fx, fy), (fx + dw - rad, fy), (fx, fy + dh - rad), (fx + dw - rad, fy + dh - rad)]
            cor_same = sum(D.count_region(im, cx, cy, cx + rad, cy + rad, lambda c: D.near(c, tc, 2))
                           for cx, cy in corners)
            check("B4. 四角被圆角裁切（角区严格同色 < 75%%）", cor_same * 100 < rad * rad * 4 * 75,
                  "%d/%d" % (cor_same, rad * rad * 4))

        # ---------- C. 三按钮：真实语义 ----------
        def btn_xy(i, fr=None):
            x0, y0, w0 = (fr if fr else (fx, fy, dw))
            bx = x0 + w0 - 1 - 30 * 3
            return bx + i * 30 + 15, y0 + 12

        def wait_ctl(action, timeout=40):
            t = time.time()
            while time.time() - t < timeout:
                m = None
                for mm in re.finditer(CTL_RE, log_fn()):
                    if mm.group(2) == action:
                        m = mm
                if m:
                    return m
                time.sleep(0.4)
            return None

        # ① 最小化 = **收容到 Dock**（★ v0.11.3 起；v0.11.2 是"折叠成 25px 标题带"——
        #    那是窗口还在占屏幕一行、Dock 上什么都看不见，不是任务栏语义）
        x, y = btn_xy(0)
        click_at(mon, log_fn, x, y)
        c = wait_ctl("minimize")
        check("C1. 点最小化 -> [WL64] ctl action=minimize（minimized=1，窗口从屏幕摘掉）",
              c is not None and "minimized=1" in c.group(4),
              "" if c is None else c.group(0))
        md = re.search(r"\[DOCK64\] ext-state surf=\d+ minimized=1", log_fn())
        check("C1b. Dock 运行项收到 minimized=1（任务栏知道它最小化了）", md is not None,
              md.group(0) if md else "")
        im = shot(mon, os.path.join(work, "min.ppm"))
        if im is not None:
            # 整个窗口矩形（含标题带）都不再是窗口：标题带底色一个像素都不剩
            band = D.count_region(im, fx, fy, fx + dw, fy + 25,
                                  lambda c: D.near(c, (0xF2, 0xF4, 0xF8), 2))
            check("C2. 最小化后窗口矩形里标题带像素 = 0（整窗摘掉，不是只收成一行）",
                  band == 0, "band=%d" % band)

        # ② 点 Dock 项 = 恢复（30 号 ctl action=restore；几何不动，所以恢复一定逐像素回原位）
        mi = re.search(r"\[DOCK64\] item idx=\d+[^\n]*cx=(\d+) cy=(\d+)[^\n]*win=10001", log_fn())
        check("C3a. Dock 列表里有这条外部窗口运行项（win=10001）", mi is not None,
              mi.group(0) if mi else "")
        if mi:
            click_at(mon, log_fn, int(mi.group(1)), int(mi.group(2)))
            c = wait_ctl("restore")
            check("C3. 点 Dock 项 -> [WL64] ctl action=restore（minimized=0）",
                  c is not None and "minimized=0" in c.group(4), "" if c is None else c.group(0))
            im2 = shot(mon, os.path.join(work, "restored.ppm"))
            if im is not None and im2 is not None:
                band2 = D.count_region(im2, fx, fy, fx + dw, fy + 25,
                                       lambda c: D.near(c, (0xF2, 0xF4, 0xF8), 2))
                check("C3b. 恢复后标题带回来了（像素 > 0）", band2 > 200, "band=%d" % band2)

        # ③ 最大化 = 装饰框铺满整屏
        x, y = btn_xy(1)
        click_at(mon, log_fn, x, y)
        c = wait_ctl("maximize")
        check("C4. 点最大化 -> [WL64] ctl action=maximize maxed=1 frame=0,0,<屏宽>,<屏高>",
              c is not None and "maxed=1" in c.group(4) and "frame=0,0," in c.group(4),
              "" if c is None else c.group(0))

        # ④ 再点一次 = 还原（★ 最大化后按钮位置跟着整屏走了：必须按新框算坐标）
        cm = re.search(r"\[WM\] composer seat=\d+ gpu=\d+ region=(-?\d+),(-?\d+),(\d+),(\d+)", log_fn())
        sw = int(cm.group(3)) if cm else 1280
        x, y = btn_xy(1, (0, 0, sw))
        click_at(mon, log_fn, x, y)
        c = wait_ctl("maximize")
        check("C5. 再点最大化 -> maxed=0（还原到原位置）",
              c is not None and "maxed=0" in c.group(4), "" if c is None else c.group(0))

        # ⑤ 关闭 = 销毁 surface + 给客户端投 CLOSE 事件
        x, y = btn_xy(2)
        click_at(mon, log_fn, x, y)
        c = wait_ctl("close")
        check("C6. 点关闭 -> [WL64] ctl action=close destroyed=1", c is not None,
              "" if c is None else c.group(0))
        t = time.time()
        kc = None
        while time.time() - t < 40:
            kc = re.search(KCLOSE_RE, log_fn())
            if kc:
                break
            time.sleep(0.4)
        check("C7. 客户端收到 CLOSE 事件（[PELOAD] msgboxA close by=composer）", kc is not None,
              "" if kc is None else kc.group(0))
        check("C8. surface 真的被销毁（[WL64] surface destroy … why=client）",
              re.search(DESTROY_RE, log_fn()) is not None, "")
        t = time.time()
        code = None
        while time.time() - t < 40:
            code = m1(EXIT_RE, log_fn())
            if code is not None:
                break
            time.sleep(0.4)
        check("C9. 进程退出 0（无 PANIC）", code == "0" and "[PANIC64] stop=" not in log_fn(),
              "exit=%s" % code)
    finally:
        try:
            if ser is not None:
                if args.keep:
                    try:
                        open(os.path.join(work, "serial.txt"), "w", encoding="utf-8").write(ser.text())
                    except Exception:
                        pass
                ser.close()
        except Exception:
            pass
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except Exception:
                proc.kill()
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)
        else:
            print("保留：%s" % work)

    npass = sum(1 for _, c in checks if c)
    print("\n=== RESULT: %s === checks=%d ok=%d fail=%d"
          % ("PASS" if ok else "FAIL", len(checks), npass, len(checks) - npass))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
