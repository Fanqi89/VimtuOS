#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/panels64_test.py - P2 验收：四个二级弹窗（通知/声音/网络/日历）+ 通知列表 + 设备插拔 toast

覆盖（每条都要 串口打点 + 像素/输入 证据）：
  1) 通知面板   开始菜单右侧、竖直长方形；空状态提示；逐条清除 + 全部清除；未读 -> 消息图标**数字角标**。
  2) 设备 toast  插入/拔出（数据源 = **实时** e1000 STATUS.LU；QEMU `set_link vnet0 off/on` 触发真事件）：
                 从桌面右侧**滑入**（帧 x 递减）-> 停留几秒 -> 自动滑出（timeout）；可手动关闭（点 ×）；
                 **多个堆叠不互相覆盖**（stack n=2 + rects 无重叠）；同时进通知列表（notif add）。
  3) 声音面板   开始菜单右侧、四边圆角横长方形；左声音图标 + 右滑轨（左小右大）+ 最右百分比；拖动改数值；
                 输出源音箱/耳机；**没有声卡驱动 -> 不假装成功**（audio driver not implemented yet 打点）。
  4) 网络面板   开始菜单左侧、竖向长方形；WiFi 区**如实空态**（无无线硬件/未检测到无线网卡，列表恒空）；
                 以太网区**固定最下方不随滚动**（真实 e1000 链路：已连接/未连接/未插网线）。
  5) 日历面板   点开始菜单左上角时间 -> **在该区域上方**弹出；360–420 × 420–480；6 行 × 7 列；
                 今天主题强调色圆形高亮（像素）+ 选中日期；滚轮/左右拖动/左右方向键切月、PageUp/PageDown 切年；
                 「今天」回本月；点年份文字展开年份面板；ESC 关闭。
  6) 通用       四个弹窗都：亚克力 + 圆角 + 双层浅阴影 + 1px 高光边缘（打点）、**不压 Dock、不超屏**、
                 锚定在触发按钮附近且**位置跟随开始菜单**（由开始菜单几何推导）。
用法：py -3 tests\\panels64_test.py [--img build64/system.img] [--qemu 路径] [--port 5663] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import calendar
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
PORT = 5663


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
    def __init__(self, qemu, img, port, name="vimtu-panels64", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_panels_")
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
            time.sleep(0.25)
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

    tmp = tempfile.mkdtemp(prefix="vimtu64_panels_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    vm = Vm(qemu, args.img, args.port, "vimtu-panels64", tmp)
    mon = vm.wait_monitor()
    cur = Cursor()
    try:
        print("=== 0) 桌面就绪 + panels64 自检 ===")
        # ★ 缺陷 4（登录必须显式输入）：默认不再自动登录 —— 先等锁屏可交互，再回车两次进桌面。
        vm.wait_log("[LOCK64] bg blur ready", 150)
        mon.key("ret", wait=1.0)          # 锁屏 -> 登录界面
        mon.key("ret", wait=1.5)          # 登录按钮（无密码用户）
        up = vm.wait_log("[GUI64] ready", 120)
        check("桌面就绪（[GUI64] ready）", up)
        log = vm.log()
        ini = re.search(r"\[PANEL64\] init anchors=menu screen=(\d+)x(\d+) dock_top=(\d+) blur=(\d+) edge=1px shadow=2", log)
        check("[PANEL64] init 打点（锚点=开始菜单 / 模糊 / 1px 高光边 / 双层阴影）", ini is not None,
              ini.group(0) if ini else "（无）")
        check("panels64 纯逻辑自检 PASS（锚点/不越屏/不压 Dock/日期换算）",
              "[PANEL64] selftest PASS mask=0" in log,
              (re.search(r"\[PANEL64\] selftest[^\r\n]*", log) or [""])[0])
        # 注意：[TOAST64] init/usb poll 是**主循环第一帧**（panels64_tick64）才打的，
        # 桌面 ready 的快照里可能还没有 -> 用 wait_log 等（and 会短路，必须两边都 wait）
        check("设备事件源打点（实时 e1000 STATUS.LU + usb64 计数差分；USB 无热插拔如实说明）",
              vm.wait_log("[TOAST64] init src=pci-e1000-STATUS.LU(live)+usb64 counts poll=500ms", 8) and
              vm.wait_log("hotplug=unsupported", 8),
              (re.search(r"\[TOAST64\] usb poll[^\r\n]*", vm.log()) or [""])[0])
        if not up:
            raise SystemExit(1)
        scrw, scrh, dock_top = int(ini.group(1)), int(ini.group(2)), int(ini.group(3))
        check("屏幕 1280x800 / Dock 顶边 = 屏高-76", (scrw, scrh, dock_top) == (1280, 800, 724),
              "%dx%d dock_top=%d" % (scrw, scrh, dock_top))

        it0 = re.search(r"\[DOCK64\] item idx=0 app=\d+ name=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log)
        check("Dock 开始按钮几何打点", it0 is not None, it0.group(0) if it0 else "（无）")
        start_cx, start_cy = (int(it0.group(5)), int(it0.group(6))) if it0 else (412, 754)

        def click_until(x, y, needle, tries=6, tag=""):
            """在小范围内点几次直到日志出现 needle（慢速客人下光标准确度有漂移）。"""
            offs = ((0, 0), (0, -10), (0, 10), (-16, 0), (16, 0),
                    (-16, -10), (16, -10), (-16, 10), (16, 10), (0, -20), (0, 20))
            for i in range(max(tries, 9)):
                n0 = len(vm.log())
                dx, dy = offs[i % len(offs)]
                cur.goto(mon, x + dx, y + dy)
                time.sleep(0.2)
                mon.click()
                if vm.wait_log(needle, 3, since=n0):
                    return True
            return False

        def ensure_menu_open():
            # 点开始按钮直到 [START64] open 出现（丢点击时重试）
            for _ in range(3):
                n0 = len(vm.log())
                cur.goto(mon, start_cx, start_cy)
                time.sleep(0.35)
                mon.click()
                if vm.wait_log("[START64] open why=start-button", 5, since=n0):
                    return True
            return False

        def open_panel(icon_name, panel):
            """点状态区图标打开弹窗；若菜单恰好被关了就重开菜单再点（返回打开的日志起点）。"""
            for _ in range(4):
                n0 = len(vm.log())
                ix, iy = icons[icon_name]
                cur.goto(mon, ix + 11, iy + 11)
                time.sleep(0.3)
                mon.click()
                if vm.wait_log("open panel=%s" % panel, 4, since=n0):
                    return n0
                ensure_menu_open()
            return -1

        def menu_geom():
            g = re.search(r"\[START64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) bottom=(\d+) dock_top=(\d+) gap=(\d+) "
                          r"center=1 screen=(\d+)x(\d+) r_top=(\d+) r_bottom=(\d+)", vm.log())
            return tuple(int(g.group(i)) for i in range(1, 12)) if g else None

        # ==================== 1) 通知面板 ====================
        print("=== 1) 通知面板（开始菜单右侧 / 竖直长方形 / 空状态）===")
        before = len(vm.log())
        check("打开开始菜单", ensure_menu_open())
        rects = re.findall(r"\[START64\] status icon (\w+) x=(\d+) y=(\d+)", vm.log())
        icons = {n: (int(x), int(y)) for n, x, y in rects}
        check("状态区图标几何（net/sound/lang/notif）", all(k in icons for k in ("net", "sound", "lang", "notif")),
              str(icons))
        mg = menu_geom()
        check("[START64] geom 打点（供锚点断言）", mg is not None)
        mx, my, mw, mh = mg[0], mg[1], mg[2], mg[3]
        if "notif" in icons:
            before = open_panel("notif", "notif")
            o = re.search(r"\[PANEL64\] open panel=notif anchor=right-of-menu x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) "
                          r"shadow=2 edge=1px blur=(\d+) dock_top=(\d+) screen=(\d+)x(\d+) menu=(\d+),(\d+),(\d+)x(\d+)",
                          vm.log()[before:])
            check("通知面板打开（panel=notif anchor=right-of-menu）", o is not None, o.group(0) if o else "（无）")
            if o:
                nx, ny = int(o.group(1)), int(o.group(2))
                nw, nh = int(o.group(3)), int(o.group(4))
                check("通知面板在**开始菜单右侧**（x >= 菜单右边界，跟随菜单）", nx >= mx + mw,
                      "panel x=%d menu right=%d" % (nx, mx + mw))
                check("锚点=菜单右边 + 8px（位置跟随开始菜单位置）", nx == mx + mw + 8, "x=%d 期望 %d" % (nx, mx + mw + 8))
                check("竖直长方形（h > w）", nh > nw, "%dx%d" % (nw, nh))
                check("弹窗不超屏 / 不压 Dock（y+h <= dock_top-4）", nx >= 0 and ny >= 0 and
                      nx + nw <= 1280 and ny + nh <= dock_top - 4, "(%d,%d) %dx%d dock_top=%d" % (nx, ny, nw, nh, dock_top))
                check("圆角 12–16 + 模糊 20–30（Token 打点）", 12 <= int(o.group(5)) <= 16 and 20 <= int(o.group(6)) <= 30,
                      "r=%s blur=%s" % (o.group(5), o.group(6)))
                check("空状态提示（[PANEL64] notif empty）", vm.wait_log("[PANEL64] notif empty", 4, since=before),
                      (re.search(r"\[PANEL64\] notif empty[^\r\n]*", vm.log()[before:]) or [""])[0])
                time.sleep(0.6)
                mon.shot(os.path.join(tmp, "notif.ppm"))
                w, h, px = read_ppm(os.path.join(tmp, "notif.ppm"))
                pbody = sample(px, w, nx + nw // 2, ny + 40)
                f1 = sample(px, w, nx + 40, ny + 60)
                f2 = sample(px, w, nx + nw - 40, ny + nh - 80)
                flat = max(abs(pbody[i] - f1[i]) + abs(pbody[i] - f2[i]) for i in range(3))
                check("通知面板像素存在（面板内部是平整的亚克力面：多点同色）", flat <= 12,
                      "flat=%d body=%s f1=%s f2=%s" % (flat, pbody, f1, f2))
                dark = 0
                for yy in range(ny + 10, ny + 30):
                    for xx in range(nx + 12, nx + 130):
                        c = sample(px, w, xx, yy)
                        if max(c) < 130:
                            dark += 1
                check("面板上真的画了东西（标题文字深色像素计数）", dark > 10, "深色像素=%d" % dark)
                edge = sample(px, w, min(1279, nx + nw + 2), ny + nh // 2)
                check("面板右边外 2px 是阴影（比面板暗）-> 面板边界真实存在",
                      sum(edge) < sum(pbody) - 15, "edge=%s panel=%s" % (edge, pbody))
                if ny + nh + 6 < dock_top:
                    below = sample(px, w, nx + nw // 2, ny + nh + 6)
                    check("面板底边之下只有阴影、不是面板 -> **不压 Dock**（像素）",
                          dist(below, pbody) > 15 and sum(below) < sum(pbody), "below=%s panel=%s" % (below, pbody))
                right = sample(px, w, min(1279, nx + nw + 6), ny + nh // 2)
                check("面板右边之外没有面板像素 -> **不超屏**（像素）",
                      dist(right, pbody) > 6, "right=%s panel=%s" % (right, pbody))

        # ==================== 2) 设备插拔 toast ====================
        print("=== 2) 设备插拔 toast：拔出网线（set_link vnet0 off）-> 滑入 / 停留 / 自动滑出 ===")
        before = len(vm.log())
        mon.send("set_link vnet0 off", wait=0.6)
        got = vm.wait_log("[TOAST64] device ", 15, since=before)
        dev = re.search(r"kind=ethernet state=unplugged text=设备已断开连接", vm.log()[before:])
        check("真实设备事件（[TOAST64] device ... state=unplugged text=设备已断开连接）",
              got and dev is not None, dev.group(0) if dev else "（无）")
        check("toast 从桌面**右侧滑入**（state=in from_x=1280 -> x=942）",
              vm.wait_log("[TOAST64] toast idx=0 kind=", 5, since=before) and
              re.search(r"\[TOAST64\] toast idx=0 kind=\d+ title=设备已断开连接[^\r\n]*state=in from_x=(\d+) -> x=(\d+) w=(\d+) h=(\d+)",
                        vm.log()[before:]) is not None,
              (re.search(r"\[TOAST64\] toast idx=0[^\r\n]*", vm.log()[before:]) or [""])[0])
        t0 = re.search(r"state=in from_x=(\d+) -> x=(\d+) w=(\d+) h=(\d+)", vm.log()[before:])
        tx, ty, tw, th = (int(t0.group(2)), 18, int(t0.group(3)), int(t0.group(4))) if t0 else (942, 18, 320, 64)
        check("滑入过程有连续帧（x 递减到目标 1280-320-18=942）",
              vm.wait_log("[TOAST64] toast idx=0 state=hold", 6, since=before),
              (re.search(r"\[TOAST64\] toast idx=0 state=hold[^\r\n]*", vm.log()[before:]) or [""])[0])
        check("toast 同时进通知列表（[PANEL64] notif add n=1 title=设备已断开连接）",
              re.search(r"\[PANEL64\] notif add n=1 unread=1 title=设备已断开连接", vm.log()[before:]) is not None,
              (re.search(r"\[PANEL64\] notif add[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.shot(os.path.join(tmp, "toast.ppm"))
        w, h, px = read_ppm(os.path.join(tmp, "toast.ppm"))
        body = sample(px, w, tx + tw // 2, ty + th // 2)
        wall = sample(px, w, tx + tw // 2, ty + th + 30)
        check("toast 像素在右上方（面板色与壁纸不同）", dist(body, wall) > 6, "%s vs %s" % (body, wall))
        # 角标像素：消息图标右上角有红点（#E81123）
        if "notif" in icons:
            bx = icons["notif"][0] + 22 - 7 - 2          # 角标方块左上角（与内核 badge_pos 一致）
            by = icons["notif"][1] - 3
            redn = 0
            for yy in range(by, by + 15):
                for xx in range(bx, bx + 15):
                    c = sample(px, w, xx, yy)
                    if c[0] > 150 and c[1] < 120 and c[2] < 120:
                        redn += 1
            check("未读角标是**红底**（15x15 方块里红色像素计数）", redn > 40, "红色像素=%d" % redn)
        check("有未读 -> 消息图标数字角标（status icon notif unread=1 badge=digit）",
              re.search(r"\[START64\] status icon notif x=\d+ y=\d+ unread=1 badge=digit badge_color=#E81123", vm.log()[before:]) is not None,
              (re.search(r"\[START64\] status icon notif[^\r\n]*", vm.log()[before:]) or [""])[0])
        # 自动滑出（停留 5s 后 by=timeout）
        check("停留几秒后**自动滑出**（toast close by=timeout after hold=5000ms）",
              vm.wait_log("by=timeout", 25, since=before),
              (re.search(r"\[TOAST64\] toast close idx=\d+ by=timeout[^\r\n]*", vm.log()[before:]) or [""])[0])
        # --- 事件 B：手动关闭（先把光标停在 toast 的 × 位置，再触发事件 -> 点到时 toast 还在） ---
        cur.goto(mon, tx + tw - 19, ty + th - 19)
        time.sleep(0.3)
        before3 = len(vm.log())
        mon.send("set_link vnet0 on", wait=0.4)
        vm.wait_log("state=hold", 20, since=before3)
        mon.click()
        check("点 toast 的 × 手动关闭（[TOAST64] toast close idx=.. by=click）",
              vm.wait_log("by=click", 8, since=before3),
              (re.search(r"\[TOAST64\] toast close[^\r\n]*", vm.log()[before3:]) or [""])[0])

        # ==================== 3) toast 堆叠 + 自动滑出 ====================
        print("=== 3) 多个 toast 堆叠不互相覆盖 + 停留几秒自动滑出 ===")
        before = len(vm.log())
        mon.send("set_link vnet0 off", wait=0.4)         # 拔出 -> 第 1 条
        time.sleep(1.2)
        mon.send("set_link vnet0 on", wait=0.4)          # 再插入 -> 第 2 条（两条同时在屏上）
        ok_stack = vm.wait_log("[TOAST64] toast stack n=2 overlap=0", 20, since=before)
        st = re.search(r"\[TOAST64\] toast stack n=2 overlap=0 rects=(\d+),(\d+),(\d+)x(\d+);(\d+),(\d+),(\d+)x(\d+)",
                       vm.log()[before:])
        check("同时两个 toast（[TOAST64] toast stack n=2 overlap=0）", ok_stack and st is not None,
              st.group(0) if st else "（无）")
        if st:
            x1, y1, w1, h1, x2, y2, w2, h2 = (int(st.group(i)) for i in range(1, 9))
            overlap = not (y1 + h1 <= y2 or y2 + h2 <= y1)
            check("两条 toast 的矩形**不重叠**（堆叠）", not overlap,
                  "r1=(%d,%d,%d,%d) r2=(%d,%d,%d,%d)" % (x1, y1, w1, h1, x2, y2, w2, h2))
        check("两条 toast 各自到期自动滑出（by=timeout 计数增加）",
              vm.wait_log("by=timeout", 25, since=before),
              "by=timeout=%d" % vm.log()[before:].count("by=timeout"))
        # 恢复链路（后面网络面板要看到"已连接"）
        before = len(vm.log())
        mon.send("set_link vnet0 on", wait=0.5)
        vm.wait_log("kind=ethernet state=plugged", 15, since=before)

        # ==================== 4) 声音面板 ====================
        print("=== 4) 声音面板：横长方形 / 滑轨拖动 / 百分比 / 输出源（无声卡驱动，如实打点）===")
        before = open_panel("sound", "sound") if "sound" in icons else len(vm.log())
        sg = re.search(r"\[PANEL64\] open panel=sound anchor=right-of-menu x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+)", vm.log()[before:])
        sl = re.search(r"\[PANEL64\] sound panel slider x=(\d+) y=(\d+) w=(\d+) h=(\d+) pct=(\d+) src=(\w+)", vm.log()[before:])
        check("声音面板打开且在开始菜单右侧（panel=sound anchor=right-of-menu）", sg is not None, sg.group(0) if sg else "（无）")
        check("滑轨几何 + 百分比 + 输出源打点（pct/src/icon/pct_right）",
              sl is not None and "icon=left-center pct_right=1" in vm.log()[before:], sl.group(0) if sl else "（无）")
        if sg and sl:
            sx0, sy0 = int(sg.group(1)), int(sg.group(2))
            sw0, sh0 = int(sg.group(3)), int(sg.group(4))
            check("横长方形（w > h）且在菜单右侧", sw0 > sh0 and sx0 >= mx + mw, "%dx%d x=%d" % (sw0, sh0, sx0))
            check("不超屏 / 不压 Dock", sx0 + sw0 <= 1280 and sy0 + sh0 <= dock_top - 4,
                  "x=%d y=%d %dx%d" % (sx0, sy0, sw0, sh0))
            check("锚点跟随菜单（x = 菜单右 + 8）", sx0 == mx + mw + 8, "x=%d 期望 %d" % (sx0, mx + mw + 8))
            slx, sly, slw, slh = (int(sl.group(i)) for i in range(1, 5))
            pct0 = int(sl.group(5))
            # 拖动滑轨：按住左侧 10% 处 -> 拖到 80% 处 -> 松开
            check("没有声卡驱动 -> 如实打点（audio driver not implemented yet）",
                  "audio driver not implemented yet" in vm.log()[before:],
                  (re.search(r"\[PANEL64\] audio driver[^\r\n]*", vm.log()[before:]) or [""])[0])
            cur.goto(mon, slx + slw // 10, sly + 6)
            mon.raw(["mouse_button 1"], wait_end=0.3)
            cur.goto(mon, slx + slw * 4 // 5, sly + 6)
            mon.raw(["mouse_button 0"], wait_end=0.6)
            vols = [int(v) for v in re.findall(r"\[PANEL64\] sound volume=(\d+)", vm.log()[before:])]
            check("拖动滑轨改音量（值域 0..100、新值 > 旧值、applied=0 如实）",
                  len(vols) >= 2 and vols[-1] > pct0 and 0 <= vols[-1] <= 100 and "applied=0" in vm.log()[before:],
                  "pct %d -> %s" % (pct0, vols))
            # 输出源切换
            cur.goto(mon, sx0 + 66 + 62 + 28, sy0 + 19)
            time.sleep(0.3)
            mon.click()
            check("切换输出源到耳机（[PANEL64] sound output src=headphones applied=0）",
                  vm.wait_log("[PANEL64] sound output src=headphones", 5, since=before),
                  (re.search(r"\[PANEL64\] sound output[^\r\n]*", vm.log()[before:]) or [""])[0])

        # ==================== 5) 网络面板 ====================
        print("=== 5) 网络面板：开始菜单左侧 / WiFi 如实空态 / 以太网区固定最下方 ===")
        before = len(vm.log())
        # 先关掉声音面板（ESC），再开网络面板
        mon.key("esc", wait=0.7)
        if "net" in icons:
            before = open_panel("net", "net")
        np = re.search(r"\[PANEL64\] open panel=net anchor=left-of-menu x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+)", vm.log()[before:])
        npl = re.search(r"\[PANEL64\] net panel x=(\d+) y=(\d+) w=(\d+) h=(\d+) wifi_area=(\d+) eth_row=(\d+) eth_h=(\d+)", vm.log()[before:])
        wifi = re.search(r"\[PANEL64\] net wifi count=(\d+) state=(\S+) text=(\S+)", vm.log()[before:])
        eth = re.search(r"\[PANEL64\] net ethernet y=(\d+) h=(\d+) state=(\S+) text=(\S+) link=(\d) fixed=1", vm.log()[before:])
        check("网络面板打开且在开始菜单左侧（panel=net anchor=left-of-menu）", np is not None, np.group(0) if np else "（无）")
        px0 = py0 = pw0 = ph0 = 0
        if np:
            px0, py0, pw0, ph0 = (int(np.group(i)) for i in range(1, 5))
            check("面板在菜单左边（x+w <= 菜单左边界）+ 锚点跟随菜单（右边界 = 菜单左 - 8）",
                  px0 + pw0 <= mx and px0 + pw0 == mx - 8, "x=%d w=%d menu_x=%d" % (px0, pw0, mx))
            check("竖向长方形（h > w）、不超屏、不压 Dock",
                  ph0 > pw0 and px0 >= 0 and py0 >= 0 and px0 + pw0 <= 1280 and py0 + ph0 <= dock_top - 4,
                  "(%d,%d) %dx%d" % (px0, py0, pw0, ph0))
        check("WiFi 区**如实空态**（count=0 无无线硬件 / 未检测到无线网卡，绝不编造列表）",
              wifi is not None and wifi.group(1) == "0" and wifi.group(2) == "no-hardware",
              wifi.group(0) if wifi else "（无）")
        check("以太网区真实链路状态（e1000 link=1 -> 已连接）",
              eth is not None and eth.group(5) == "1" and eth.group(3) == "connected" and eth.group(4) == "已连接",
              eth.group(0) if eth else "（无）")
        if npl and eth:
            ex, ey, ew, eh = int(npl.group(1)), int(npl.group(6)), int(npl.group(3)), int(npl.group(7))
            check("以太网区**固定在最下方**（eth_row + eth_h 贴着面板底边）",
                  ey + eh <= int(npl.group(2)) + int(npl.group(4)) and ey > int(npl.group(2)) + int(npl.group(4)) // 2,
                  "eth_row=%d eth_h=%d panel_y=%s panel_h=%s" % (ey, eh, npl.group(2), npl.group(4)))
            check("以太网区高度 = Token（62）", eh == 62, "eth_h=%d" % eh)
        # 滚轮：以太网区不动（所有 net ethernet 行的 y 一致），WiFi 列表不被伪造
        ys_before = [m.group(1) for m in re.finditer(r"\[PANEL64\] net ethernet y=(\d+)", vm.log())]
        for dz in (1, -1, 1):
            mon.send("mouse_move 0 0 %d" % dz, wait=0.35)
        time.sleep(0.8)
        ys_after = [m.group(1) for m in re.finditer(r"\[PANEL64\] net ethernet y=(\d+)", vm.log())]
        wifis = re.findall(r"\[PANEL64\] net wifi count=(\d+)", vm.log())
        check("滚轮不移动以太网区（y 恒定 = fixed=1）", len(set(ys_before)) <= 1 and ys_before == ys_after,
              "y=%s -> %s" % (ys_before, ys_after))
        check("滚轮也不会冒出 WiFi 列表（count 恒 0）", all(v == "0" for v in wifis), str(wifis))
        time.sleep(0.6)
        mon.shot(os.path.join(tmp, "net.ppm"))
        w, h, px = read_ppm(os.path.join(tmp, "net.ppm"))
        pbody = sample(px, w, px0 + pw0 // 2, py0 + 40)
        f1 = sample(px, w, px0 + 40, py0 + 60)
        f2 = sample(px, w, px0 + pw0 - 40, py0 + ph0 - 90)
        check("网络面板像素存在（平整亚克力面：多点同色）",
              max(abs(pbody[i] - f1[i]) + abs(pbody[i] - f2[i]) for i in range(3)) <= 12,
              "body=%s f1=%s f2=%s" % (pbody, f1, f2))
        dark = 0
        for yy in range(py0 + 10, py0 + 32):
            for xx in range(px0 + 40, px0 + 160):
                c = sample(px, w, xx, yy)
                if max(c) < 130:
                    dark += 1
        check("面板上真的画了东西（Wi-Fi 标题 + 图标深色像素计数）", dark > 10, "深色像素=%d" % dark)
        edge = sample(px, w, min(1279, px0 + pw0 + 2), py0 + 60)
        check("面板右边外 2px 是阴影（比面板暗）-> 面板边界真实存在",
              sum(edge) < sum(pbody) - 15, "edge=%s panel=%s" % (edge, pbody))
        if py0 + ph0 + 6 < dock_top:
            below = sample(px, w, px0 + pw0 // 2, py0 + ph0 + 6)
            check("网络面板底边之下只有阴影 -> 不压 Dock（像素）",
                  dist(below, pbody) > 15 and sum(below) < sum(pbody), "below=%s panel=%s" % (below, pbody))
        left = sample(px, w, max(0, px0 - 6), py0 + ph0 // 2)
        check("网络面板左边之外没有面板像素 -> 不超屏（像素）",
              dist(left, pbody) > 6, "left=%s panel=%s" % (left, pbody))

        # ==================== 6) 日历面板 ====================
        print("=== 6) 日历面板：点开始菜单左上角时间 -> 上方弹出 / 7x6 网格 / 今天高亮 / 切月切年 ===")
        before = len(vm.log())
        mon.key("esc", wait=0.7)                        # 关网络面板
        check("ESC 关掉网络面板（[PANEL64] esc close=net，开始菜单还在）",
              "esc close=net" in vm.log()[before:] and vm.log()[before:].count("[START64] close") == 0,
              (re.search(r"\[PANEL64\] esc close[^\r\n]*", vm.log()[before:]) or [""])[0])
        # 时间区域 = 菜单左上角（x = menu_x + 16, y = menu_y + 16）
        before = len(vm.log())
        cur.goto(mon, mx + 16 + 40, my + 20)
        time.sleep(0.3)
        mon.click()
        cp = re.search(r"\[PANEL64\] open panel=cal anchor=above-time-area x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+)", vm.log()[before:])
        cl = re.search(r"\[PANEL64\] cal panel x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) grid=7x6 cell=(\d+) today=(\d+)-(\d+)-(\d+) sel=(\d+)-(\d+)-(\d+)", vm.log()[before:])
        check("点时间打开日历（panel=cal anchor=above-time-area）", cp is not None, cp.group(0) if cp else "（无）")
        check("日历几何打点（grid=7x6 + today/sel 日期）", cl is not None, cl.group(0) if cl else "（无）")
        cx0 = cy0 = cw0 = ch0 = cr0 = 0
        if cp:
            time.sleep(0.8)          # 打开动画（220ms）跑完再解析动画行
            cx0, cy0, cw0, ch0, cr0 = (int(cp.group(i)) for i in range(1, 6))
            check("宽 360–420 × 高 420–480", 360 <= cw0 <= 420 and 420 <= ch0 <= 480, "%dx%d" % (cw0, ch0))
            check("圆角 12–16", 12 <= cr0 <= 16, "r=%d" % cr0)
            check("在**该区域（左上角时间）上方**弹出（cal_y < menu_y）", cy0 < my, "cal_y=%d menu_y=%d" % (cy0, my))
            check("不超屏 / 不压 Dock", cx0 >= 0 and cy0 >= 0 and cx0 + cw0 <= 1280 and cy0 + ch0 <= dock_top - 4,
                  "(%d,%d) %dx%d" % (cx0, cy0, cw0, ch0))
            check("锚定触发区域（x = 时间区 x - 10，跟随菜单）", cx0 == mx + 16 - 10, "x=%d 期望 %d" % (cx0, mx + 6))
            # 打开动画：fade + scale 0.98->1 + translateY 6->0（200–250ms）
            anims = re.findall(r"\[PANEL64\] cal anim dir=in t=(\d+)ms s=(\d+)/1000 dy=(\d+) alpha=(\d+)", vm.log()[before:])
            check("打开动画打点（200–250ms 内 s 从 <=1000 收敛到 1000、dy 6->0、alpha 递增到 255）",
                  len(anims) >= 2 and int(anims[0][1]) <= 1000 and int(anims[0][2]) > 0 and
                  int(anims[0][3]) < 255 and int(anims[-1][1]) == 1000 and int(anims[-1][2]) == 0 and
                  200 <= int(anims[-1][0]) <= 250 and int(anims[-1][3]) == 255,
                  str(anims[:3]) + " .. " + str(anims[-1:]))
            time.sleep(0.8)
            mon.shot(os.path.join(tmp, "cal.ppm"))
            w, h, px = read_ppm(os.path.join(tmp, "cal.ppm"))
            body = sample(px, w, cx0 + cw0 // 2, cy0 + ch0 // 2)
            f1 = sample(px, w, cx0 + cw0 - 30, cy0 + ch0 - 30)
            check("日历面板像素存在（平整亚克力面：多点同色，底部是留白不是壁纸）",
                  dist(body, f1) <= 14, "body=%s f1=%s" % (body, f1))
            if cy0 + ch0 + 6 < dock_top:
                below = sample(px, w, cx0 + cw0 // 2, cy0 + ch0 + 6)
                check("日历底边之下只有阴影 -> 不压 Dock（像素）",
                      dist(below, body) > 15 and sum(below) < sum(body), "below=%s panel=%s" % (below, body))
            # 今天高亮：用宿主日历算出今天的格子 -> 该格子中心必须是主题强调色（白主题 = #0078D7）
            ty_, tm_, td_ = int(cl.group(7)), int(cl.group(8)), int(cl.group(9))   # today=YYYY-MM-DD
            try:
                lead = calendar.weekday(ty_, tm_, 1)               # kernel: (wd_sun0+6)%7 == Python Monday=0 列号
            except Exception:
                lead = 0
            cell_w = (cw0 - 28) // 7
            cell_h = (ch0 - 108 - 16) // 6
            idx = lead + td_ - 1
            print("      今天格子：lead=%d cell=%dx%d idx=%d 面板=(%d,%d) %dx%d" % (lead, cell_w, cell_h, idx, cx0, cy0, cw0, ch0))
            tcx = cx0 + 14 + (idx % 7) * cell_w + cell_w // 2
            tcy = cy0 + 108 + (idx // 7) * cell_h + cell_h // 2
            if 0 <= tcx < w and 0 <= tcy < h:
                tc = sample(px, w, tcx, tcy)
                check("今天用**主题强调色圆形高亮**（#0078D7 附近像素，落在今天的格子里）",
                      abs(tc[0] - 0) < 70 and abs(tc[1] - 120) < 70 and abs(tc[2] - 215) < 70,
                      "cell=(%d,%d) color=%s today=%s-%s-%s" % (tcx, tcy, tc, cl.group(7), cl.group(8), cl.group(9)))
            else:
                check("今天高亮格子坐标在屏内", False,
                      "tcx=%d tcy=%d panel=(%d,%d) %dx%d grid=%dx%d" % (tcx, tcy, cx0, cy0, cw0, ch0, cell_w, cell_h))
        # 滚轮切月
        before = len(vm.log())
        mon.send("mouse_move 0 0 2", wait=0.5)
        vm.wait_log("[PANEL64] cal wheel dz=", 6, since=before)
        wh = re.search(r"\[PANEL64\] cal wheel dz=(-?\d+) -> month (\d+)-(\d+)", vm.log()[before:])
        check("滚轮切月（[PANEL64] cal wheel dz=.. -> month YYYY-MM）", wh is not None, wh.group(0) if wh else "（无）")
        # 左右方向键切月
        before = len(vm.log())
        mon.key("left", wait=0.8)
        m_left = re.search(r"\[PANEL64\] cal month ym=(\d+)-(\d+) via=arrow", vm.log()[before:])
        check("左方向键切月（via=arrow）", m_left is not None, m_left.group(0) if m_left else "（无）")
        # PageUp 切年（-12 月）
        before = len(vm.log())
        mon.key("pgup", wait=0.9)
        pg = re.search(r"\[PANEL64\] cal month ym=(\d+)-(\d+) via=pageup", vm.log()[before:])
        if pg is None:
            mon.key("prior", wait=0.9)
            pg = re.search(r"\[PANEL64\] cal month ym=(\d+)-(\d+) via=pageup", vm.log()[before:])
        check("PageUp 切年（via=pageup，年月 -1 年）", pg is not None, pg.group(0) if pg else "（无）")
        # 点某个日期（第 1 行第 3 列）
        before = len(vm.log())
        if cl:
            dayx = cx0 + 14 + 2 * ((cw0 - 28) // 7) + ((cw0 - 28) // 7) // 2
            dayy = cy0 + 108 + ((ch0 - 108 - 16) // 6) // 2
            cur.goto(mon, dayx, dayy)
            time.sleep(0.3)
            mon.click()
            sel = re.search(r"\[PANEL64\] cal select date=(\d+)-(\d+)-(\d+) ym=(\d+)-(\d+)", vm.log()[before:])
            check("点日期选中并同步顶部年月（[PANEL64] cal select date=.. ym=..）", sel is not None,
                  sel.group(0) if sel else "（无）")
        # 年份面板
        before = len(vm.log())
        if cl:
            cur.goto(mon, cx0 + 48 + 40, cy0 + 46 + 13)
            time.sleep(0.3)
            mon.click()
            yp = re.search(r"\[PANEL64\] cal year panel open=1 years=(\d+)\.\.(\d+)", vm.log()[before:])
            check("点年份文字展开年份选择面板（open=1 years=2022..2033 及未来）", yp is not None,
                  yp.group(0) if yp else "（无）")
            if yp:
                cur_y = int(re.search(r"\[PANEL64\] cal month ym=(\d+)-", vm.log()).group(1)) if re.search(r"\[PANEL64\] cal month ym=(\d+)-", vm.log()) else None
                check("年份面板窗口 12 年且包含当前显示年份（可跳到 2022…2026 及未来）",
                      int(yp.group(2)) == int(yp.group(1)) + 11 and
                      (cur_y is None or int(yp.group(1)) <= cur_y <= int(yp.group(2))),
                      yp.group(0))
                base0 = int(yp.group(1))
                # 先用键盘选年（方向键 + 回车；慢速客人下鼠标有漂移，键盘是确定性的）
                before_k = len(vm.log())
                mon.key("right", wait=0.6)
                mon.key("ret", wait=0.9)
                got = re.search(r"\[PANEL64\] cal year select y=(\d+) ym=(\d+)-(\d+)", vm.log()[before_k:])
                # 兜底：鼠标点（点年份文字是 toggle，用日志里的 open= 判状态再补开）
                want = base0 + 1
                col = want % 3
                row = (want - base0) // 3
                yx = cx0 + 28 + col * ((cw0 - 28 - 28) // 3)
                yy = cy0 + 84 + 16 + row * 40 + 12
                for attempt in range(0 if got else 3):
                    last_open = re.findall(r"\[PANEL64\] cal year panel open=(\d)", vm.log())
                    if not last_open or last_open[-1] != "1":
                        cur.goto(mon, cx0 + 48 + 40, cy0 + 46 + 13)
                        time.sleep(0.2)
                        mon.click()
                        vm.wait_log("cal year panel open=1", 3)
                    before2 = len(vm.log())
                    click_until(yx, yy, "cal year select", tries=3)
                    ys = re.search(r"\[PANEL64\] cal year select y=(\d+) ym=(\d+)-(\d+)", vm.log()[before2:])
                    if ys:
                        got = ys
                        break
                check("年份面板点选可跳年份（[PANEL64] cal year select y=..，且 ym 年份 = y）",
                      got is not None and got.group(2) == got.group(1),
                      got.group(0) if got else "（无）")
        # 左右拖动切月
        before = len(vm.log())
        cur.goto(mon, cx0 + cw0 // 2, cy0 + 108 + ((ch0 - 108 - 16) // 6) * 5 + 10)
        mon.raw(["mouse_button 1"], wait_end=0.2)
        cur.move_by(mon, 60, 0, wait=0.12)
        cur.move_by(mon, 60, 0, wait=0.12)
        mon.raw(["mouse_button 0"], wait_end=0.5)
        dr = re.search(r"\[PANEL64\] cal month ym=(\d+)-(\d+) via=drag", vm.log()[before:])
        check("左右拖动切月（via=drag）", dr is not None, dr.group(0) if dr else "（无）")
        # 「今天」按钮（放在最后点：即使光标有漂移误点别的格子，也不会污染前面的断言）
        if cl:
            before = len(vm.log())
            today_m = cl.group(11) + "-" + cl.group(12)
            tx0 = cx0 + cw0 - 14 - 58
            ok_key = False
            n0 = len(vm.log())
            mon.key("t", wait=0.9)
            ok_key = vm.wait_log("via=today", 4, since=n0)
            ok_today = ok_key or click_until(tx0 + 29, cy0 + 22, "via=today", tries=5)
            back = re.search(r"\[PANEL64\] cal month ym=(\d+)-(\d+) via=today", vm.log()[before:])
            # cl: 7=today_y 8=today_m 9=today_d 10=sel_y 11=sel_m 12=sel_d
            check("「今天」回本月（via=today，年月 = 今天 %s；鼠标点或键盘 T）" % today_m,
                  ok_today and back is not None and
                  back.group(1) == cl.group(10) and back.group(2) == cl.group(11),
                  back.group(0) if back else "（无）")
        # ESC：先关日历，再关开始菜单（两级退出）
        before = len(vm.log())
        mon.key("esc", wait=0.8)
        check("ESC 一级：关日历（[PANEL64] esc close=cal + cal anim dir=out 150ms）",
              "esc close=cal" in vm.log()[before:] and "cal anim dir=out t=150ms" in vm.log()[before:],
              (re.search(r"\[PANEL64\] cal anim dir=out[^\r\n]*", vm.log()[before:]) or [""])[0])
        mon.key("esc", wait=0.8)
        check("ESC 二级：关开始菜单（[START64] close why=esc）", "[START64] close why=esc" in vm.log()[before:],
              (re.search(r"\[START64\] close why=esc[^\r\n]*", vm.log()[before:]) or [""])[0])

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


if __name__ == "__main__":
    sys.exit(main())
