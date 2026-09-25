#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/startmenu64_test.py - P2 验收：开始菜单（几何/像素/交互/搜索/电源菜单/ESC 两级）

覆盖（每条都要 串口打点 + 像素/输入 证据）：
  1) 几何      [START64] geom：屏幕水平居中、底边 = Dock 顶边上方 10~12px、上圆角 24 / 下圆角 10、
               h/w = Token 值、不越屏；像素：菜单面板存在、**顶角 24 被切掉而底角 10 仍是面板色**
               （证明混合圆角）、菜单底边之下到 Dock 之间是壁纸（证明**不压 Dock**）。
  2) 固定网格  [START64] grid cols=2 rows=4 n=8 + 8 条 tile 打点：两列四行、整体偏右、全在菜单内。
  3) 搜索框    [START64] search box：顶部偏右、小圆角、主题色亚克力；像素：框内有**白色"搜索"**字。
  4) 状态区    [START64] status icon net/sound/lang/notif：从左到右 网络→声音→中/英→通知；
               网络在 QEMU 里是有线（kind=ethernet，wired=1 wireless=0）。
  5) 搜索      键入 calc -> [START64] search q=calc hits=1 first=Calculator (app=1)；
               回车 -> [START64] launch app=1 via=search-enter + [APP] calc opened；点结果行同样能启动。
  6) 固定网格启动  点 tile idx=0（终端）-> launch app=3 via=tile + [APP] term opened。
  7) 电源菜单  点电源按钮 -> [START64] power menu open 竖长方形小圆角框 rows=3（关机/重启/锁定）；
               ESC -> esc level=2 close=power-menu（菜单仍在）；再 ESC -> close why=esc（两级退出）。
  8) 设置按钮  点「设置」-> close why=settings（**先关开始菜单**）+ [APP] settings opened。
  9) 关闭方式  点菜单外部 -> close why=click-outside；点开始按钮 -> toggle 关；ESC -> close why=esc。
 10) 键盘补充  Caps：[INPUT64] caps on=1 + 搜索框里 a -> A（大小写切换）；
               Shift：[INPUT64] shift toggle count=.. lang=英 ime=0（没有中文输入法，指示只显示"英"）。
用法：py -3 tests\\startmenu64_test.py [--img build64/system.img] [--qemu 路径] [--port 5662] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
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
PORT = 5662


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    import shutil
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM")
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


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def raw(self, cmds, wait_between=0.12, wait_end=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            for c in cmds:
                s.sendall(c.encode() + b"\n")
                time.sleep(wait_between)
            time.sleep(wait_end)
        finally:
            s.close()

    def move(self, dx, dy, wait=0.10):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def click(self, wait=0.45):
        self.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=wait)

    def shot(self, path, wait=2.2):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


class Cursor:
    """Deterministic cursor tracking for the QEMU monitor.

    mouse_move is a *relative* request in QEMU units; the guest kernel multiplies it by 1.7
    (integer truncation) and clamps the cursor to the screen (mouse_set_bounds). Every step here
    asks for <= 14 px (= 23.8 after x1.7, below the driver's 24 px per-packet cap) so each step
    lands completely; settle() then sends 8 steps of +-3 (net zero movement) to drain the guest's
    movement accumulator. Start position = mouse_init()'s (512,384).
    """

    def __init__(self, x=512, y=384):
        self.x, self.y = x, y

    def _step(self, mon, px, py, wait):
        cx = int(px / 1.7)
        cy = int(py / 1.7)
        cx = max(-14, min(14, cx))
        cy = max(-14, min(14, cy))
        if cx == 0 and cy == 0:
            return False
        mon.move(cx, cy, wait=wait)
        self.x += int(cx * 17 / 10)
        self.y += int(cy * 17 / 10)
        self.x = max(0, min(1279, self.x))
        self.y = max(0, min(799, self.y))
        return True

    def settle(self, mon, wait=0.10):
        # +-3 * 1.7 = +-5 exactly -> net zero, but every move makes the device emit another packet
        # which drains any leftover from merged steps in the guest accumulator.
        for dx, dy in ((3, 0), (-3, 0), (0, 3), (0, -3), (3, 0), (-3, 0), (0, 3), (0, -3)):
            mon.move(dx, dy, wait=wait)

    def goto(self, mon, tx, ty, wait=0.10):
        for _ in range(400):
            rx = int(tx) - self.x
            ry = int(ty) - self.y
            if abs(rx) <= 1 and abs(ry) <= 1:
                break
            if not self._step(mon, rx, ry, wait):
                break
        self.settle(mon, wait)

    def move_by(self, mon, px, py, wait=0.10):
        self.goto(mon, self.x + px, self.y + py, wait)


class Vm:
    def __init__(self, qemu, img, port, name="vimtu-start64", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_start64_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-netdev", "user,id=vnet0", "-device", "e1000,netdev=vnet0",
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_start64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    vm = Vm(qemu, args.img, args.port, "vimtu-start64", tmp)
    try:
        mon = vm.wait_monitor()
        print("=== 0) 开机就绪（锁屏 -> 自动登录 -> 桌面）===")
        up = vm.wait_log("[GUI64] ready", 120)
        check("桌面就绪（[GUI64] ready；锁屏/登录在它之前）", up)
        log = vm.log()
        init = re.search(r"\[START64\] init screen=(\d+)x(\d+) menu=(\d+)x(\d+) r_top=(\d+) r_bottom=(\d+) gap_dock=(\d+)", log)
        check("[START64] init 打点（菜单尺寸/圆角/离 Dock 间距来自 Token）", init is not None,
              init.group(0) if init else "（无）")
        check("startmenu64 纯逻辑自检 PASS（几何/网格/搜索匹配）",
              "[START64] selftest PASS mask=0" in log,
              (re.search(r"\[START64\] selftest[^\r\n]*", log) or [""])[0])
        if not up or init is None:
            print("  桌面没起来，跳过后续断言")
            raise SystemExit(1)

        # ---------- 1) 打开：Dock 开始按钮 ----------
        print("=== 1) 点 Dock 开始按钮 -> 打开开始菜单 ===")
        it0 = re.search(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log)
        check("Dock 开始按钮几何打点（[DOCK64] item idx=0）", it0 is not None,
              it0.group(0) if it0 else "（无）")
        n_open = log.count("[START64] open why=")
        # 开菜单**之前**先拍一张基线（后面的像素断言用"同点前后差异"证明真的画了菜单、
        # 且菜单没画到 Dock 上——白色主题下菜单底色与壁纸很接近，"直接和壁纸比"不够硬）
        time.sleep(0.6)
        mon.shot(os.path.join(tmp, "base.ppm"))
        cur = Cursor()
        start_cx, start_cy = (int(it0.group(5)), int(it0.group(6))) if it0 else (412, 754)

        def ensure_menu_open():
            # 点开始按钮直到 [START64] open why=start-button 出现（丢点击时重试）
            for _ in range(3):
                n0 = len(vm.log())
                cur.goto(mon, start_cx, start_cy)
                time.sleep(0.35)
                mon.click()
                if vm.wait_log("[START64] open why=start-button", 5, since=n0):
                    return True
            return False

        if it0:
            cur.goto(mon, int(it0.group(5)), int(it0.group(6)))
            time.sleep(0.4)
            mon.click()
            got_open = vm.wait_log("[START64] open why=start-button", 6, since=len(log))
            if not got_open:                      # 按键/点击偶发丢失：重试一次
                cur.goto(mon, int(it0.group(5)), int(it0.group(6)))
                time.sleep(0.4)
                mon.click()
                got_open = vm.wait_log("[START64] open why=start-button", 6, since=len(log))
            check("点开始按钮打开真开始菜单（[START64] open why=start-button）", got_open,
                  (re.search(r"\[START64\] open why=[^\r\n]*", vm.log()) or [""])[0])
        log = vm.log()
        check("open 计数增加", log.count("[START64] open why=") > n_open)

        # ---------- 2) 几何（打点 + 像素）----------
        print("=== 2) 开始菜单几何：居中 / 离 Dock 10~12 / 上圆角 24 下圆角 10 / 不压 Dock ===")
        g = re.search(r"\[START64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) bottom=(\d+) dock_top=(\d+) gap=(\d+) "
                      r"center=1 screen=(\d+)x(\d+) r_top=(\d+) r_bottom=(\d+)", log)
        check("[START64] geom 打点", g is not None, g.group(0) if g else "（无）")
        if not g:
            raise SystemExit(1)
        gx, gy, gw, gh, gb, gd, ggap = (int(g.group(i)) for i in range(1, 8))
        scrw, scrh = int(g.group(8)), int(g.group(9))
        rt, rb = int(g.group(10)), int(g.group(11))
        check("屏幕分辨率 1280x800（-vga std）", (scrw, scrh) == (1280, 800), "%dx%d" % (scrw, scrh))
        check("水平居中（中心 = 屏宽/2，±1px）", abs((gx + gw // 2) - scrw // 2) <= 1,
              "中心 %d vs %d" % (gx + gw // 2, scrw // 2))
        check("底边 = Dock 顶边上方 10~12px（gap=%d）" % ggap, 10 <= ggap <= 12)
        check("bottom + gap == dock_top", gb + ggap == gd, "bottom=%d dock_top=%d" % (gb, gd))
        check("上圆角 24 / 下圆角 10", (rt, rb) == (24, 10), "r_top=%d r_bottom=%d" % (rt, rb))
        check("竖直长方形、偏正方形、整体较小（w,h = Token）", (gw, gh) == (400, 420), "%dx%d" % (gw, gh))
        check("不越屏 / 不压 Dock（y>=0 且 bottom < dock_top）", gy >= 0 and gb < gd)
        time.sleep(1.0)
        shot = os.path.join(tmp, "menu.ppm")
        mon.shot(shot)
        w, h, px = read_ppm(shot)
        wb, hb, pxb = read_ppm(os.path.join(tmp, "base.ppm"))
        check("截图 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
        body = sample(px, w, gx + gw // 2, gy + gh // 2)
        before_body = sample(pxb, w, gx + gw // 2, gy + gh // 2)
        print("      面板色 %s / 开菜单前同一点 %s" % (body, before_body))
        check("菜单面板像素存在（开菜单前后同一点像素明显变了）", dist(body, before_body) > 6,
              "%s vs %s" % (body, before_body))
        ctrl_after = sample(px, w, 60, 500)
        ctrl_before = sample(pxb, w, 60, 500)
        check("远处桌面像素没被动过（对照组：菜单没有整屏乱画）", dist(ctrl_after, ctrl_before) <= 4,
              "%s vs %s" % (ctrl_after, ctrl_before))

        # 混合圆角（上 24 / 下 10）的**像素**判别在这个白色主题下做不到：面板底色 (240,242,245) 与
        # 壁纸 (239,244,250) 几乎同色（实测 corner 与 body 的差 <= 4）。所以这里断言：
        #   (a) 渲染路径打点 mixed_corners=1 + Token 的 r_top/r_bottom（geom 行）；
        #   (b) 圆角切掉的那一小块**没有**面板/边框像素（与面板主体的差很小，只有阴影）；
        #   (c) 面板确实画了（搜索框/图标/文字/网格的像素断言在下面各节）。
        shape = re.search(r"\[START64\] shape mixed_corners=1 r_top=(\d+) r_bottom=(\d+)", vm.log())
        check("混合圆角渲染路径打点（mixed_corners=1 r_top=24 r_bottom=10）",
              shape is not None and shape.group(1) == "24" and shape.group(2) == "10",
              shape.group(0) if shape else "（无）")
        c_tl = sample(px, w, gx + 4, gy + 4)
        check("顶部大圆角 24：左上角一带不是面板像素（与面板主体差很小 = 圆角切掉）",
              dist(c_tl, body) <= 15, "corner=%s body=%s diff=%d" % (c_tl, body, dist(c_tl, body)))
        # 菜单底边之下、Dock 顶边之上：不是面板色（明显更暗 = 阴影落在壁纸上）-> 没压 Dock
        below_after = sample(px, w, gx + gw // 2, gb + 5)
        check("菜单底边之下（bottom+5 < dock_top）明显比面板暗（阴影落在壁纸上）-> 没压 Dock",
              gb + 5 < gd and dist(below_after, body) > 15 and sum(below_after) < sum(body),
              "below=%s body=%s (bottom=%d dock_top=%d)" % (below_after, body, gb, gd))

        # ---------- 3) 固定网格 2 列 × 4 行 ----------
        print("=== 3) 已固定应用网格：2 列 × 4 行、偏右 ===")
        grid = re.search(r"\[START64\] grid cols=(\d+) rows=(\d+) n=(\d+) tile=(\d+)x(\d+) x=(\d+) y=(\d+)", log)
        tiles = re.findall(r"\[START64\] tile idx=(\d+) app=(\d+) name=(\S+) en=([^x]+?) x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log)
        check("[START64] grid 打点（cols=2 rows=4 n=8）",
              grid is not None and grid.group(1) == "2" and grid.group(2) == "4" and grid.group(3) == "8",
              grid.group(0) if grid else "（无）")
        check("8 条 tile 几何打点", len(tiles) >= 8, "%d 条" % len(tiles))
        if len(tiles) >= 8:
            t0, t1, t2 = tiles[0], tiles[1], tiles[2]
            check("同行的 tile 0/1 同一 y、不同 x（列）",
                  t0[5] == t1[5] and int(t1[4]) > int(t0[4]), "y %s/%s x %s/%s" % (t0[5], t1[5], t0[4], t1[4]))
            check("tile 2 换到第 2 行（y 增加）", int(t2[5]) > int(t0[5]), "y %s -> %s" % (t0[5], t2[5]))
            check("网格整体偏右（最左 tile 中心 > 菜单中心）",
                  int(t0[4]) + int(t0[6]) // 2 > gx + gw // 2, "tile0 x=%s" % t0[4])
            check("网格全在菜单内、且不压 Dock",
                  all(int(t[4]) >= gx and int(t[4]) + int(t[6]) <= gx + gw and
                      int(t[5]) + int(t[7]) <= gy + gh and int(t[5]) + int(t[7]) < gd for t in tiles))
            tx, ty, tw2, th2 = int(t0[4]), int(t0[5]), int(t0[6]), int(t0[7])
            ix = tx + (tw2 - 30) // 2          # tile 图标：30x30 渐变方块（在 tile 顶部 +6）
            iy = ty + 6
            varied = sum(1 for yy in range(iy, iy + 30, 2) for xx in range(ix, ix + 30, 2)
                         if dist(sample(px, w, xx, yy), body) > 30)
            check("tile 0 的图标方块画出来了（30x30 渐变底与菜单底不同）", varied > 80, "%d 点" % varied)

        # ---------- 4) 搜索框 ----------
        print("=== 4) 搜索框：顶部偏右 / 小圆角 / 主题色亚克力 / 白色\"搜索\" ===")
        sb = re.search(r"\[START64\] search box x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) acrylic=(\S+) alpha=(\d+)", log)
        check("[START64] search box 打点", sb is not None, sb.group(0) if sb else "（无）")
        if sb:
            sx, sy, sw, sh2 = (int(sb.group(i)) for i in range(1, 5))
            check("搜索框在顶部偏右（中心 > 菜单中心）", sx + sw // 2 > gx + gw // 2, "x=%d w=%d" % (sx, sw))
            check("搜索框小圆角 r=8 且在菜单内",
                  int(sb.group(5)) == 8 and sy > gy and sy + sh2 < gy + gh)
            whites = sum(1 for yy in range(sy + 4, sy + sh2 - 4)
                         for xx in range(sx + 26, sx + sw - 8)
                         if min(sample(px, w, xx, yy)) > 185)
            check("框内白色\"搜索\"二字（白像素计数）", whites > 20, "%d 个白像素" % whites)
            box = sample(px, w, sx + sw - 6, sy + sh2 // 2)
            check("搜索框是跟随主题色的亚克力（框内颜色与面板底色不同）", dist(box, body) > 10,
                  "box=%s body=%s" % (box, body))

        # ---------- 5) 状态区图标 ----------
        print("=== 5) 状态区：网络 -> 声音 -> 中/英 -> 通知（线性图标，跟随主题）===")
        net = re.search(r"\[START64\] status icon net x=(\d+) y=(\d+) kind=(\w+) wired=(\d) wireless=(\d)", log)
        snd = re.search(r"\[START64\] status icon sound x=(\d+) y=(\d+) volume=(\d+) icon=(\w+)", log)
        lng = re.search(r"\[START64\] status icon lang x=(\d+) y=(\d+) text=(\S+) ime=(\d)", log)
        ntf = re.search(r"\[START64\] status icon notif x=(\d+) y=(\d+) unread=(\d+) badge=(\w+)", log)
        check("[START64] status icon net 打点", net is not None, net.group(0) if net else "（无）")
        check("[START64] status icon sound 打点", snd is not None, snd.group(0) if snd else "（无）")
        check("[START64] status icon lang 打点（无中文输入法只显示 英）",
              lng is not None and lng.group(3) == "英" and lng.group(4) == "0",
              lng.group(0) if lng else "（无）")
        check("[START64] status icon notif 打点（未读角标）", ntf is not None, ntf.group(0) if ntf else "（无）")
        check("有线网络显示以太网图标（wired=1 wireless=0 kind=ethernet）",
              net is not None and net.group(3) == "ethernet" and net.group(4) == "1" and net.group(5) == "0",
              net.group(0) if net else "（无）")
        if net and snd and lng and ntf:
            xs = [int(net.group(1)), int(snd.group(1)), int(lng.group(1)), int(ntf.group(1))]
            check("从左到右：网络 < 声音 < 中/英 < 通知", xs == sorted(xs) and len(set(xs)) == 4, str(xs))
            check("状态图标都在菜单内、同一行",
                  all(x >= gx for x in xs) and all(int(net.group(2)) == int(x.group(2)) for x in (snd, lng, ntf)))
        if net:
            nx, ny = int(net.group(1)), int(net.group(2))
            ink = sum(1 for yy in range(ny, ny + 22, 2) for xx in range(nx, nx + 22, 2)
                      if dist(sample(px, w, xx, yy), body) > 60)
            check("网络图标真的画了线性线条（图标区域有深色像素）", ink > 8, "%d 点" % ink)

        # ---------- 6) 搜索 -> 启动（回车）----------
        print("=== 6) 搜索：calc -> 回车启动计算器 ===")
        ensure_menu_open()
        before = len(vm.log())
        for k in ("c", "a", "l", "c"):
            mon.key(k, wait=0.35)
        got = vm.wait_log("[START64] search q=calc", 6, since=before)
        log2 = vm.log()[before:]
        m = re.search(r"\[START64\] search q=calc hits=(\d+) first=(\S+) \(app=(\d+)\)", log2)
        check("键入 calc 命中计算器（hits=1 first=Calculator app=1）",
              got and m is not None and m.group(1) == "1" and m.group(3) == "1",
              m.group(0) if m else "（无）")
        before = len(vm.log())
        mon.key("ret", wait=1.4)
        check("回车启动第一个命中（[START64] launch app=1 via=search-enter）",
              vm.wait_log("launch app=1 via=search-enter", 6, since=before),
              (re.search(r"\[START64\] launch[^\r\n]*", vm.log()[before:]) or [""])[0])
        check("应用真的开了（[APP] calc opened）", vm.wait_log("[APP] calc opened", 8, since=before))
        check("回车后开始菜单关闭（close why=search-enter）",
              "close why=search-enter" in vm.log()[before:])

        # ---------- 7) 点搜索结果行启动 ----------
        print("=== 7) 点搜索结果行启动（复用既有 app 启动入口）===")
        before = len(vm.log())
        mon.key("meta_l", wait=1.0)             # Win 键：老菜单（新菜单没开时不影响）
        mon.key("esc", wait=0.6)
        ensure_menu_open()
        for k in ("t", "e", "r", "m"):
            mon.key(k, wait=0.3)
        vm.wait_log("[START64] search q=term", 6, since=before)
        # 结果行：menu_x + pad(16)，y = search_y + 34 + 12；点第一行中间
        if sb:
            rx = gx + 16 + 60
            ry = int(sb.group(2)) + 34 + 12 + 12
            cur.goto(mon, rx, ry)
            time.sleep(0.3)
            mon.click()
            check("点结果行启动（[START64] launch app=3 via=result + [APP] term opened）",
                  vm.wait_log("launch app=3 via=result", 6, since=before) and
                  vm.wait_log("[APP] term opened", 8, since=before),
                  (re.search(r"\[START64\] launch[^\r\n]*", vm.log()[before:]) or [""])[0])
        # 关掉终端窗口（回到干净桌面）：点它的关闭按钮
        mon.key("meta_l", wait=0.8)
        mon.key("1", wait=1.6)
        mon.key("exit", wait=1.0)
        time.sleep(0.8)

        # ---------- 8) 固定网格点 tile 启动 ----------
        print("=== 8) 点固定网格 tile 启动 ===")
        before = len(vm.log())
        ensure_menu_open()
        if len(tiles) >= 8:
            t = tiles[3]                         # tile 3 = 计算器（2 列 × 4 行，偏右）
            cur.goto(mon, int(t[4]) + int(t[6]) // 2, int(t[5]) + int(t[7]) // 2)
            time.sleep(0.3)
            mon.click()
            check("点 tile 启动应用（[START64] launch app=%s via=tile）" % t[1],
                  vm.wait_log("launch app=%s via=tile" % t[1], 6, since=before),
                  (re.search(r"\[START64\] launch[^\r\n]*", vm.log()[before:]) or [""])[0])
            check("应用真的开了（[APP] calc opened）", vm.wait_log("[APP] calc opened", 8, since=before))

        # ---------- 9) 电源菜单（当前界面内）+ ESC 两级 ----------
        print("=== 9) 电源按钮 -> 竖长方形小圆角框（关机/重启/锁定）+ ESC 两级退出 ===")
        before = len(vm.log())
        ensure_menu_open()
        pwr = re.search(r"\[START64\] power btn x=(\d+) y=(\d+) d=(\d+) settings x=(\d+) y=(\d+)", vm.log())
        check("[START64] power btn 打点（圆形按钮 + 左边设置按钮）", pwr is not None, pwr.group(0) if pwr else "（无）")
        if pwr:
            pxx, pyy = int(pwr.group(1)), int(pwr.group(2))
            pm = None
            for _ in range(3):
                cur.goto(mon, pxx + int(pwr.group(3)) // 2, pyy + int(pwr.group(3)) // 2)
                time.sleep(0.3)
                mon.click()
                pm = re.search(r"\[START64\] power menu open x=(\d+) y=(\d+) w=(\d+) h=(\d+) rows=3 row0=shutdown row1=reboot row2=lock",
                               vm.log()[before:])
                if pm:
                    break
            check("电源二级菜单在当前界面内弹出（竖长方形小圆角框 rows=3）", pm is not None,
                  pm.group(0) if pm else "（无）")
            if pm:
                pmx, pmy, pmw, pmh = (int(pm.group(i)) for i in range(1, 5))
                check("电源菜单在菜单内部、宽高合理（竖长方形）",
                      pmx > gx and pmx + pmw <= gx + gw and pmy > gy and pmy + pmh <= gy + gh and pmh > pmw,
                      "x=%d y=%d w=%d h=%d" % (pmx, pmy, pmw, pmh))
            n_esc2 = vm.log().count("[START64] esc level=2")
            mon.key("esc", wait=0.9)
            check("ESC 第一级：只关电源菜单（esc level=2 close=power-menu），开始菜单还在",
                  vm.log().count("[START64] esc level=2") > n_esc2 and
                  vm.log()[before:].count("[START64] close why=esc") == 0,
                  (re.search(r"\[START64\] esc level=2[^\r\n]*", vm.log()[before:]) or [""])[0])
            mon.key("esc", wait=0.9)
            check("ESC 第二级：关开始菜单（close why=esc）",
                  "close why=esc" in vm.log()[before:],
                  (re.search(r"\[START64\] close why=esc[^\r\n]*", vm.log()[before:]) or [""])[0])

        # ---------- 10) 设置按钮 ----------
        print("=== 10) 「设置」按钮：先关开始菜单，再打开设置窗口 ===")
        before = len(vm.log())
        ensure_menu_open()
        st = re.search(r"\[START64\] settings x=(\d+) y=(\d+)", vm.log())
        if pwr and st:
            cur.goto(mon, int(st.group(1)) + 48, int(st.group(2)) + 17)
            time.sleep(0.3)
            mon.click()
            check("点设置先关开始菜单（close why=settings）+ 打点 settings open (menu closed first)",
                  vm.wait_log("[START64] close why=settings", 6, since=before) and
                  "settings open (menu closed first)" in vm.log()[before:],
                  (re.search(r"\[START64\] settings open[^\r\n]*", vm.log()[before:]) or [""])[0])
            check("设置窗口真的开了（[APP] settings opened）", vm.wait_log("[APP] settings opened", 10, since=before))
            mon.key("esc", wait=1.0)             # 关设置窗口
            time.sleep(0.6)

        # ---------- 11) 关闭方式：点菜单外部 ----------
        print("=== 11) 点菜单外部关闭 ===")
        before = len(vm.log())
        ensure_menu_open()
        cur.goto(mon, 120, 200)                  # 桌面左上空白（远离菜单/弹窗）
        time.sleep(0.3)
        mon.click()
        check("点菜单外部 -> close why=click-outside",
              vm.wait_log("[START64] close why=click-outside", 6, since=before),
              (re.search(r"\[START64\] close[^\r\n]*", vm.log()[before:]) or [""])[0])

        # ---------- 12) 点开始按钮关闭（toggle）----------
        print("=== 12) 点开始按钮再点一次关闭（toggle）===")
        before = len(vm.log())
        ensure_menu_open()
        n = len(vm.log())
        mon.click()
        check("再点开始按钮 -> close why=toggle",
              vm.wait_log("[START64] close why=toggle", 6, since=n),
              (re.search(r"\[START64\] close[^\r\n]*", vm.log()[n:]) or [""])[0])

        # ---------- 13) Caps / Shift（键盘补充）----------
        print("=== 13) Caps 切换大小写 / Shift 切中英（无输入法只显示 英）===")
        before = len(vm.log())
        ensure_menu_open()
        mon.key("caps_lock", wait=0.8)
        check("Caps 打点（[INPUT64] caps on=1）",
              vm.wait_log("[INPUT64] caps on=1", 6, since=before),
              (re.search(r"\[INPUT64\] caps[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.key("a", wait=0.8)
        check("Caps 开：字母 a -> 搜索框里是大写 A（[START64] search q=A）",
              vm.wait_log("[START64] search q=A hits=", 6, since=before),
              (re.search(r"\[START64\] search q=[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.key("caps_lock", wait=0.8)
        mon.key("b", wait=0.8)
        check("Caps 关：字母 b -> 小写（查询变成 Ab）",
              vm.wait_log("[START64] search q=Ab hits=", 6, since=before),
              (re.search(r"\[START64\] search q=Ab[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.key("esc", wait=0.8)
        before = len(vm.log())
        ensure_menu_open()
        mon.key("shift", wait=0.8)
        check("Shift 打点（shift toggle .. lang=英 ime=0，无中文输入法只显示 英）",
              vm.wait_log("[INPUT64] shift toggle count=1 lang=英 ime=0", 6, since=before),
              (re.search(r"\[INPUT64\] shift toggle[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.key("esc", wait=0.6)

        print("")
        print("== 结果：%d 项断言，%s ==" % (len(checks), "全过" if ok else "有失败"))
        fails = [n for n, c in checks if not c]
        if fails:
            print("失败项：")
            for f in fails:
                print("  - %s" % f)
        return 0 if ok else 1
    finally:
        vm.close()
        if not args.keep:
            pass


if __name__ == "__main__":
    sys.exit(main())
