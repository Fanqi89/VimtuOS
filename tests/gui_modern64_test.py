#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/gui_modern64_test.py - Windows 11 风格桌面（Dock + 主题 + 壁纸适应模式）验收

覆盖（每条都要串口 + 像素/输入证据）：
  1) Dock 几何        [DOCK64] geom/item：y = 屏高-76、高 60、水平居中、圆角 24、图标 44–48、间距 10–12；
                      像素：面板存在（与壁纸不同）、最左开始按钮是**真图标**（彩色像素）。
  2) 悬停放大/让位     鼠标闭环挪到某个图标上 -> [DOCK64] hover idx=.. scale=120 + 像素：
                      被悬停图标的顶边升高（放大 1.20），紧邻图标的左/右边界外移（轻微让位）。
  3) 点击回弹          点击 -> [DOCK64] bounce idx=.. frame=.. dy=.. 连续帧位移（向上再向下回弹）+
                      bounce done frames>=6（弹簧 260ms）。
  4) 最小化小横杠      Dock 图标 -> 最小化 -> [DOCK64] press action=minimize + [DOCK64] minbar ...
                      （宽 = 图标 40%、跟随强调色）+ 像素：横杠处有强调色像素；
                      并且**在横杠上点击不产生任何 Dock 动作**（纯视觉不可点击）。
  5) 主题切换（>=4）    Ctrl+Shift+T 循环：白 -> 暗 -> 蓝白 -> 粉白 …；每条 [THEME64] apply 打点 +
                      整屏像素差 > 阈值（暗色主题下 Dock 变深灰、主色变化）。
  6) 壁纸 6 种适应模式  Ctrl+Shift+N 循环 fill/fit/stretch/tile/center/span：
                      [GFX64] wall 几何（dst/scale/crop/tiles）+ 4 个定位标记的屏幕位置/裁剪 +
                      留白（无图区域方差≈0）/ 平铺接缝（x=1200 列 == x=0 列）/ 拉伸比例失真。
  7) 减少动画开关      Ctrl+Shift+R -> [THEME64] motion reduce=1 dock=0ms，点击后 bounce frames=1 motion=reduced。
用法：python tests/gui_modern64_test.py [--img build64/system.img] [--keep]
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

DOCK_H = 60
DOCK_MARGIN = 16
DOCK_R = 24
MARK_COLORS = {"r": (192, 48, 48), "g": (32, 160, 64), "b": (48, 64, 192), "y": (224, 192, 48)}

# ---- 各主题：Dock 面板/主色的期望（判"切到暗色后 Dock 变深灰"用）----
THEME_EXPECT = {
    0: ("white", 0),
    1: ("dark", 1),
    2: ("bluegrad", 0),
    3: ("pinkgrad", 0),
    4: ("pinkgreen", 0),
    5: ("pinkpurple", 0),
}


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


def near(c, t, tol=20):
    return all(abs(c[i] - t[i]) <= tol for i in range(3))


def diff_count(a, b):
    n = 0
    for i in range(0, min(len(a), len(b)), 3):
        if a[i] != b[i] or a[i + 1] != b[i + 1] or a[i + 2] != b[i + 2]:
            n += 1
    return n


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def raw(self, cmds, wait_between=0.12, wait_end=0.5):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            for c in cmds:
                s.sendall(c.encode() + b"\n")
                time.sleep(wait_between)
            time.sleep(wait_end)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def move(self, dx, dy, wait=0.3):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def button(self, val, wait=0.4):
        self.send("mouse_button %d" % val, wait=wait)

    def shot(self, path, wait=2.5):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


class Vm:
    def __init__(self, qemu, img, port, name="vimtu-modern", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_modern_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
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

    def wait_ready(self, timeout=90):
        return self.wait_log("[GUI64] ready", timeout)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


# ==================== 鼠标闭环定位（移植自 tests/ui_extra64_test.py 的实测规律）====================
def probe_pos(vm, mon, wait_open=0.45):
    before = len(vm.log())
    mon.button(1, wait=wait_open)
    mon.button(0, wait=0.55)
    t0 = time.time()
    while time.time() - t0 < 2.5:
        log = vm.log()[before:]
        m = re.search(r"\[UI\] selbox x0=(\d+) y0=(\d+)", log)
        if m:
            return (int(m.group(1)), int(m.group(2)))
        if "[UI] desktop icon select kind=" in log or "[DOCK64] press" in log:
            return None
        time.sleep(0.25)
    return None


def _move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.move(mag if d > 0 else -mag, 0)
    else:
        mon.move(0, mag if d > 0 else -mag)


def aim_axis(mon, axis, d):
    if abs(d) < 20:
        return
    if abs(d) < 48:
        _move_axis(mon, axis, d, 14)
        return
    for _ in range((abs(d) + 23) // 24 - 1):
        _move_axis(mon, axis, d, 100)
    _move_axis(mon, axis, d, -20)


def goto(vm, mon, tx, ty, tries=10):
    """把光标闭环挪到 (tx,ty) 附近；返回最终探测到的位置（或 None，表示已压在图标上）。"""
    for _ in range(tries):
        pos = probe_pos(vm, mon)
        if pos is None:
            return None
        cx, cy = pos
        if abs(cx - tx) <= 20 and abs(cy - ty) <= 24:
            return pos
        aim_axis(mon, 0, tx - cx)
        aim_axis(mon, 1, ty - cy)
    return pos


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5652)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_modern_")
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    vm = Vm(qemu, args.img, args.port, "vimtu-modern", tmp)
    try:
        mon = vm.wait_monitor()
        print("=== 1) Dock 几何（串口 + 像素）===")
        up = vm.wait_ready(90)
        check("桌面就绪", up)
        log = vm.log()
        g = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) gap=(\d+) items=(\d+) margin=(\d+) center=1 screen=(\d+)x(\d+)", log)
        check("[DOCK64] geom 打点", g is not None, g.group(0) if g else "（无）")
        if not up or g is None:
            print("  桌面没起来，跳过全部像素断言")
            raise SystemExit(1)
        dx, dy, dw, dh, dr = (int(g.group(i)) for i in range(1, 6))
        icon, gap, items = int(g.group(6)), int(g.group(7)), int(g.group(8))
        W, H = int(g.group(10)), int(g.group(11))
        check("Dock 高 60", dh == DOCK_H, "h=%d" % dh)
        check("Dock y = 屏高 - 76（离底 16px）", dy == H - DOCK_H - DOCK_MARGIN, "y=%d" % dy)
        check("Dock 水平居中（中心 = 屏宽/2）", abs((dx + dw // 2) - W // 2) <= 1,
              "中心 %d vs %d" % (dx + dw // 2, W // 2))
        check("Dock 圆角 24", dr == DOCK_R, "r=%d" % dr)
        check("图标 44-48px", 44 <= icon <= 48, "icon=%d" % icon)
        check("图标间距 10-12px", 10 <= gap <= 12, "gap=%d" % gap)
        clk = re.search(r"\[DOCK64\] clock chip x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log)
        dclk = (int(clk.group(1)), int(clk.group(3))) if clk else (W - DOCK_MARGIN - 210, 210)
        check("右下角时钟玻璃片几何打点", clk is not None, clk.group(0) if clk else "（无）")
        items_log = re.findall(r"\[DOCK64\] item idx=(\d+) app=(\d+) name=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log)
        check("每项都有几何打点（>= %d 项）" % items, len(items_log) >= items, "items=%d" % len(items_log))
        if items_log:
            i0 = items_log[0]
            check("最左是开始按钮（idx=0 且挨着面板左内边）",
                  int(i0[0]) == 0 and abs(int(i0[3]) - (dx + 9)) <= 2, "start x=%s dock_x=%d" % (i0[3], dx))
            i1 = items_log[1] if len(items_log) > 1 else None
            if i1:
                step = int(i1[3]) - int(i0[3])
                check("相邻图标步长 = 图标 + 间距", step == icon + gap, "step=%d" % step)

        shot0 = os.path.join(tmp, "base.ppm")
        mon.shot(shot0)
        w, h, px0 = read_ppm(shot0)
        check("分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
        gap_x = dx + 9 + icon + 5          # 图标 1 与图标 2 之间的间隙中点（纯面板）
        panel = sample(px0, w, gap_x, dy + dh // 2)
        above = sample(px0, w, gap_x, dy - 40)
        print("      面板色 %s / 面板上方壁纸 %s" % (panel, above))
        check("Dock 面板像素存在（面板与壁纸明显不同）",
              sum(abs(panel[i] - above[i]) for i in range(3)) > 6,
              "面板 %s 壁纸 %s" % (panel, above))
        # 开始按钮：真图标（彩色像素）
        scx = (int(i0[3]) + icon // 2, int(i0[4]) + icon // 2)
        colorful = 0
        for y in range(scx[1] - icon // 2, scx[1] + icon // 2):
            for x in range(scx[0] - icon // 2, scx[0] + icon // 2):
                c = sample(px0, w, x, y)
                if max(c) - min(c) > 40:
                    colorful += 1
        check("开始按钮用的是彩色真图标（logo/kaisi.png 像素）", colorful > 80, "彩色像素=%d" % colorful)

        print("=== 2) 悬停放大 1.20 + 邻位让位（真实鼠标）===")
        # 先把光标挪到桌面空白处（顶边），再闭环挪到 idx=3（终端）图标中心
        mon.move(0, -1200, wait=0.5)
        HOVER_IDX = 4                       # 计算器：渐变方块图标（整块都是图标像素，放大/让位看得清）
        tgt = items_log[HOVER_IDX]
        tcx, tcy = int(tgt[7]), int(tgt[8])
        nb = items_log[HOVER_IDX - 1]       # 左邻
        ncx = int(nb[7])
        before_hover = len(vm.log())
        pos = goto(vm, mon, tcx, tcy)
        print("      图标中心 (%d,%d)，光标探测 %s" % (tcx, tcy, pos))
        hovered = vm.wait_log("[DOCK64] hover idx=%d" % HOVER_IDX, 6, since=before_hover)
        check("悬停打点（[DOCK64] hover idx=%d scale=120）" % HOVER_IDX, hovered)
        ms = re.search(r"\[DOCK64\] hover idx=%d scale=(\d+) neighbor_shift=(\d+)" % HOVER_IDX,
                       vm.log()[before_hover:])
        check("悬停放大 1.20（120%）且邻位让位 104%",
              ms is not None and ms.group(1) == "120" and ms.group(2) == "104",
              ms.group(0) if ms else "（无）")
        time.sleep(1.0)
        shot_h = os.path.join(tmp, "hover.ppm")
        mon.shot(shot_h)
        w2, h2, pxh = read_ppm(shot_h)

        def top_edge(px, cx):
            """从面板顶部往下扫，第一个与面板色明显不同的 y = 该列上图标顶边。
            （悬停放大后图标更高 → 顶边更靠上 = y 更小）"""
            for y in range(dy + 2, dy + dh - 2):
                c = sample(px, w, cx, y)
                if sum(abs(c[i] - panel[i]) for i in range(3)) > 40:
                    return y
            return dy + dh - 2

        base_top = top_edge(px0, tcx)
        hov_top = top_edge(pxh, tcx)
        print("      被悬停图标顶边：基线 %d -> 悬停 %d" % (base_top, hov_top))
        check("悬停图标包围盒变大（顶边升高 >= 3px）", base_top - hov_top >= 3,
              "base=%d hover=%d" % (base_top, hov_top))

        def left_edge(px, cx, span=30):
            for x in range(cx - span, cx + span):
                c = sample(px, w, x, dy + dh // 2)
                if sum(abs(c[i] - panel[i]) for i in range(3)) > 40:
                    return x
            return cx

        base_neigh = left_edge(px0, ncx)
        hov_neigh = left_edge(pxh, ncx)
        print("      左邻图标左边：基线 %d -> 悬停 %d" % (base_neigh, hov_neigh))
        check("邻位图标轻微让位（左边向外移 >= 2px）", base_neigh - hov_neigh >= 2,
              "base=%d hover=%d（外移 %d）" % (base_neigh, hov_neigh, base_neigh - hov_neigh))

        print("=== 3) 点击回弹（连续帧位移 + 弹簧 260ms）===")
        before_click = len(vm.log())
        mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.4)
        got = vm.wait_log("[DOCK64] bounce idx=%d" % HOVER_IDX, 6, since=before_click)
        check("点击产生回弹动画打点", got)
        time.sleep(1.6)
        seg = vm.log()[before_click:]
        frames = re.findall(r"\[DOCK64\] bounce idx=%d frame=(\d+) t=(\d+) dy=(\d+) dir=(\w+)" % HOVER_IDX, seg)
        done = re.search(r"\[DOCK64\] bounce idx=%d done frames=(\d+) motion=spring" % HOVER_IDX, seg)
        dys = [int(f[2]) for f in frames]
        dirs = [f[3] for f in frames]
        print("      回弹帧：%d 帧，dy 序列 %s，方向 %s" % (len(frames), dys[:10], dirs[:10]))
        check("回弹有连续帧（>= 4 帧）", len(frames) >= 4, "%d 帧" % len(frames))
        check("相邻帧位移在变（上下回弹，不是一步到位）",
              len(set(dys)) >= 3, "dy=%s" % dys[:10])
        check("弹簧曲线出现过冲（先向上、后向下回落）",
              ("up" in dirs) and ("down" in dirs or "rest" in dirs), "dirs=%s" % dirs[:10])
        check("回弹走完（done frames >= 6，约 260ms）",
              done is not None and int(done.group(1)) >= 6,
              done.group(0) if done else "（无 done 行）")

        print("=== 4) 最小化小横杠（40% 宽、强调色、不可点击）===")
        # 先开计算器（开始菜单数字快捷键 4）
        mon.key("meta_l", wait=1.0)
        mon.key("4", wait=2.2)
        check("计算器已打开（菜单快捷键 4）", "[APP] calc opened" in vm.log())
        # 再点 Dock 上的计算器图标 -> 最小化
        calc = None
        for it in items_log:
            if it[1] == "1":       # APP_ID_CALC == 1
                calc = it
        check("Dock 里有计算器项", calc is not None)
        if calc:
            ccx, ccy = int(calc[7]), int(calc[8])
            # 光标此刻就停在计算器图标上（第 2/3 节闭环挪过来的）；Dock 区里 selbox 探测不到位置，
            # 所以这里不再 goto，直接点即可。
            before_min = len(vm.log())
            mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.6)
            minimized = vm.wait_log("[DOCK64] press idx=%s app=1 action=minimize" % calc[0], 6, since=before_min)
            check("点 Dock 图标把该应用最小化（action=minimize）", minimized,
                  vm.log()[before_min:before_min + 400].splitlines()[-1] if not minimized else "")
            mb = vm.wait_log("[DOCK64] minbar idx=", 6, since=before_min)
            check("最小化后出现小横杠打点", mb)
            m = re.search(r"\[DOCK64\] minbar idx=(\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) color=#([0-9A-Fa-f]{6}) clickable=0",
                          vm.log()[before_min:])
            check("小横杠打点含几何/颜色/clickable=0", m is not None, m.group(0) if m else "（无）")
            if m:
                bx, by, bw2 = int(m.group(2)), int(m.group(3)), int(m.group(4))
                col = tuple(int(m.group(6)[i:i + 2], 16) for i in (0, 2, 4))
                check("小横杠宽 = 图标宽 40%%（%d 的 40%% = %d）" % (icon, icon * 40 // 100),
                      bw2 == icon * 40 // 100, "w=%d" % bw2)
                check("小横杠高 3px 且位置在图标下方（Dock 内）",
                      int(m.group(5)) == 3 and dy < by < dy + dh, "y=%d" % by)
                time.sleep(0.8)
                shot_mb = os.path.join(tmp, "minbar.ppm")
                mon.shot(shot_mb)
                w3, h3, pxm = read_ppm(shot_mb)
                hit = 0
                for x in range(bx - 2, bx + bw2 + 2):
                    for y in range(by - 1, by + 4):
                        if near(sample(pxm, w3, x, y), col, 24):
                            hit += 1
                check("小横杠像素跟随强调色（横杠处强调色像素 >= 8）", hit >= 8,
                      "匹配像素=%d 期望色=%s" % (hit, col))
                # 纯视觉不可点击：内核自己对横杠中心做命中测试并打点（hit=-1 = 不是任何 Dock 项）
                ht = re.search(r"\[DOCK64\] minbar hit_test idx=(\d+) x=(\d+) y=(\d+) -> hit=none \(visual only, not clickable\)",
                               vm.log()[before_min:])
                check("横杠不可点击（内核命中测试 hit=none）",
                      ht is not None, ht.group(0) if ht else "（无）")

        print("=== 5) 主题切换（>= 4 种，实时生效 + 像素差）===")
        shots = {}
        base_log = len(vm.log())
        cur = re.search(r"\[THEME64\] init themes=\d+ theme=(\d+)", vm.log())
        theme_id = int(cur.group(1)) if cur else 0
        shots[theme_id] = px0
        # 依次 Ctrl+Shift+T 切换 5 次（覆盖 6 个主题；单次切完断言打点与像素差）
        for step in range(5):
            want = (theme_id + 1) % 6
            before = len(vm.log())
            mon.key("ctrl-shift-t", wait=1.0)
            got = vm.wait_log("[THEME64] apply theme=%d" % want, 30)
            if not got:     # 按键偶发丢失：再按一次（QEMU sendkey 在多核/忙时可能丢）
                mon.key("ctrl-shift-t", wait=1.0)
                got = vm.wait_log("[THEME64] apply theme=%d" % want, 30)
            name, dark = THEME_EXPECT[want]
            ml = re.search(r"\[THEME64\] apply theme=%d name=%s dark=\d accent=#\w{6}" % (want, name), vm.log())
            check("切到主题 %d（%s）打点（含 name/dark/accent）" % (want, name), got and ml is not None,
                  ml.group(0) if ml else "（无）")
            time.sleep(2.0)     # 壁纸重建（模糊只算一次，但整屏重建要 1-2 秒）
            sp = os.path.join(tmp, "theme%d.ppm" % want)
            mon.shot(sp)
            wa, ha, pxa = read_ppm(sp)
            shots[want] = pxa
            d = diff_count(px0, pxa)
            check("主题 %d 的整屏像素与白色主题不同（变化 > 20 万像素）" % want,
                  d > 200000, "变化像素=%d" % d)
            # Dock 面板亮度（暗色主题必须变深灰）
            pc = sample(pxa, wa, gap_x, dy + dh // 2)
            lum = (pc[0] + pc[1] + pc[2]) / 3.0
            print("      主题 %d 面板色 %s 亮度 %.0f" % (want, pc, lum))
            if dark:
                check("暗色主题下 Dock 是深灰（亮度 < 110）", lum < 110, "%.0f" % lum)
            else:
                check("非暗色主题下 Dock 用浅色固定默认色（亮度 > 150）", lum > 150, "%.0f" % lum)
            theme_id = want
        ns = len({id(v): v for v in shots.values()}) if False else len(shots)
        check("至少 4 种主题有像素证据", ns >= 4, "主题数=%d" % ns)
        # 主色（强调色）随主题变化：暗色/蓝白 的 accent 打点不同
        accents = re.findall(r"\[THEME64\] apply theme=(\d+) name=\S+ dark=\d accent=#(\w{6})", vm.log())
        check("不同主题的强调色不同（主色变化）",
              len({a[1] for a in accents}) >= 3, "accent 集合=%s" % sorted({a[1] for a in accents}))
        # 切回白（把系统留在干净状态，避免影响复跑）
        if theme_id != 0:
            mon.key("ctrl-shift-t", wait=1.0)
            vm.wait_log("[THEME64] apply theme=0", 25)
            time.sleep(1.5)

        print("=== 6) 壁纸 6 种适应模式（几何 + 定位标记像素）===")
        mk = re.search(r"\[GFX64\] wall markers r=(\d+),(\d+) g=(\d+),(\d+) b=(\d+),(\d+) y=(\d+),(\d+) size=(\d+)", vm.log())
        check("[GFX64] wall markers 打点（4 个定位标记）", mk is not None, mk.group(0) if mk else "（无）")
        marks = {}
        if mk:
            marks["r"] = (int(mk.group(1)), int(mk.group(2)))
            marks["g"] = (int(mk.group(3)), int(mk.group(4)))
            marks["b"] = (int(mk.group(5)), int(mk.group(6)))
            marks["y"] = (int(mk.group(7)), int(mk.group(8)))
            msize = int(mk.group(9))
        else:
            msize = 16
        for want_mode, want_name in ((0, "fill"), (1, "fit"), (2, "stretch"), (3, "tile"), (4, "center"), (5, "span")):
            before = len(vm.log())
            if want_mode != 0:
                mon.key("ctrl-shift-n", wait=1.0)
            elif "[GFX64] wall mode=0 name=fill" not in vm.log():
                mon.key("ctrl-shift-n", wait=1.0)
            wl = vm.wait_log("[GFX64] wall mode=%d name=%s" % (want_mode, want_name), 30)
            check("模式 %d（%s）生效并打了几何行" % (want_mode, want_name), wl)
            time.sleep(2.5)
            spawn = os.path.join(tmp, "wall%d.ppm" % want_mode)
            mon.shot(spawn, wait=3.0)
            ww, wh, pxw = read_ppm(spawn)
            gl = re.search(r"\[GFX64\] wall mode=%d name=%s src=(\d+)x(\d+) screen=(\d+)x(\d+) dst=(-?\d+),(-?\d+),(\d+)x(\d+) scale=(\d+)/(\d+) crop=l(\d+),t(\d+),r(\d+),b(\d+) tiles=(\d+)x(\d+)" % (want_mode, want_name),
                           vm.log())
            check("模式 %d 几何行可解析（含 dst/scale/crop/tiles）" % want_mode, gl is not None,
                  gl.group(0) if gl else "（无）")
            if gl is None:
                continue
            sw, sh = int(gl.group(1)), int(gl.group(2))
            sW, sH = int(gl.group(3)), int(gl.group(4))
            mdx, mdy, mdw, mdh = (int(gl.group(i)) for i in range(5, 9))
            tiles_x = int(gl.group(15))
            # 模式定义自检（几何必须在屏幕上成立）
            if want_mode in (0, 5):     # 填充/跨屏：铺满 + 等比 + 有裁剪
                check("模式 %d 铺满屏幕（dst 覆盖全屏）" % want_mode,
                      mdx <= 0 and mdy <= 0 and mdx + mdw >= sW and mdy + mdh >= sH,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
                ratio = (mdw / float(sw)) / (mdh / float(sh))
                check("模式 %d 保持等比（不拉伸变形）" % want_mode, abs(ratio - 1.0) < 0.02, "比例=%.3f" % ratio)
            elif want_mode == 1:        # 适应：整张可见
                check("适应模式：dst 不超出屏幕", mdx >= 0 and mdy >= 0 and mdx + mdw <= sW and mdy + mdh <= sH,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            elif want_mode == 2:        # 拉伸：铺满（允许变形）
                check("拉伸模式：dst == 全屏", (mdx, mdy, mdw, mdh) == (0, 0, sW, sH),
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            elif want_mode == 3:        # 平铺：1:1 且不止一块
                check("平铺模式：1:1 尺寸（dst == 源尺寸）", mdw == sw and mdh == sh,
                      "dst=%dx%d src=%dx%d" % (mdw, mdh, sw, sh))
            elif want_mode == 4:        # 居中：1:1 居中
                check("居中模式：1:1 且屏幕居中",
                      mdw == sw and mdh == sh and abs(mdx - (sW - sw) // 2) <= 1 and abs(mdy - (sH - sh) // 2) <= 1,
                      "dst=%d,%d,%dx%d" % (mdx, mdy, mdw, mdh))
            # 定位标记：源坐标 -> 屏幕坐标
            def mark_screen(sx, sy):
                if want_mode == 3:
                    return (mdx + sx, mdy + sy)
                return (mdx + int(round(sx * mdw / float(sw))), mdy + int(round(sy * mdh / float(sh))))

            for key, (sx, sy) in marks.items():
                ex, ey = mark_screen(sx, sy)
                cx2, cy2 = ex + max(1, int(msize * mdw / float(sw) / 2)), ey + max(1, int(msize * mdh / float(sh) / 2))
                if not (2 <= cx2 < sW - 2 and 2 <= cy2 < sH - 2):
                    continue            # 该标记被裁掉（预期：填充模式下四角标记都在屏内，这里只跳过越界）
                # 被 Dock / 时钟片挡住的标记跳过（Dock 是顶层，标记被盖住看不到）
                if dy - 2 <= cy2 <= dy + dh + 2 and (dx - 2 <= cx2 <= dx + dw + 2 or
                                                     dclk[0] - 2 <= cx2 <= dclk[0] + dclk[1] + 2):
                    continue
                c = sample(pxw, ww, cx2, cy2)
                if abs(c[0] - MARK_COLORS[key][0]) > 30 or abs(c[1] - MARK_COLORS[key][1]) > 30 or \
                        abs(c[2] - MARK_COLORS[key][2]) > 30:
                    # 找一找 ±6px 邻域里有没有（缩放取整误差）
                    found = False
                    for oy in range(-6, 7):
                        for ox in range(-6, 7):
                            c2 = sample(pxw, ww, min(sW - 1, max(0, cx2 + ox)), min(sH - 1, max(0, cy2 + oy)))
                            if abs(c2[0] - MARK_COLORS[key][0]) <= 30 and abs(c2[1] - MARK_COLORS[key][1]) <= 30 and \
                                    abs(c2[2] - MARK_COLORS[key][2]) <= 30:
                                found = True
                                break
                        if found:
                            break
                else:
                    found = True
                check("模式 %d：%s 标记在预期位置（源 %d,%d -> 屏 %d,%d）" % (want_mode, key, sx, sy, cx2, cy2),
                      found, "实测色 %s 期望 %s" % (c, MARK_COLORS[key]))
            # 模式专属结构证据
            if want_mode == 1:      # 适应：左右留白（无图区域接近纯色）
                strip_var = 0
                vals = [sample(pxw, ww, x, 400)[1] for x in range(4, mdx - 6, 4)]
                if len(vals) > 4:
                    mean = sum(vals) / len(vals)
                    strip_var = sum((v - mean) ** 2 for v in vals) / len(vals)
                in_vals = [sample(pxw, ww, x, 400)[1] for x in range(mdx + 8, mdx + 120, 4)]
                in_mean = sum(in_vals) / len(in_vals)
                in_var = sum((v - in_mean) ** 2 for v in in_vals) / len(in_vals)
                check("适应模式：留白区是纯色底（方差远低于图内）",
                      strip_var * 3 < max(1.0, in_var), "留白方差 %.2f 图内方差 %.2f" % (strip_var, in_var))
            if want_mode == 3:      # 平铺：第二块瓷砖的接缝（x = 源宽 处复制源 x=0 列）
                ok_seam = True
                for y in range(60, 700, 37):
                    if ww > 1200 and sw == 1200:
                        c0 = sample(pxw, ww, y % 2, y)
                        c1 = sample(pxw, ww, 1200 + (y % 2), y)
                        if sum(abs(c0[i] - c1[i]) for i in range(3)) > 6:
                            ok_seam = False
                            break
                check("平铺模式：x=1200 列与 x=0 列相同（真的重复铺）", ok_seam and tiles_x >= 1)
            if want_mode == 2:      # 拉伸：红标记被拉扁（宽高比 != 1）
                def bbox_of(tcol, tol=40):
                    xs, ys = [], []
                    for yy in range(0, sH, 2):
                        for xx in range(0, sW, 2):
                            cc = sample(pxw, ww, xx, yy)
                            if abs(cc[0] - tcol[0]) <= tol and abs(cc[1] - tcol[1]) <= tol and abs(cc[2] - tcol[2]) <= tol:
                                xs.append(xx)
                                ys.append(yy)
                    if not xs:
                        return None
                    return (max(xs) - min(xs) + 1, max(ys) - min(ys) + 1)
                bb = bbox_of(MARK_COLORS["r"])
                check("拉伸模式：标记宽高比 != 1（图被拉变形）", bb is not None and abs(bb[0] / float(bb[1]) - 1.0) > 0.08,
                      "bbox=%s" % (bb,))

        print("=== 7) 减少动画开关 ===")
        before = len(vm.log())
        mon.key("ctrl-shift-r", wait=1.0)
        got = vm.wait_log("[THEME64] motion reduce=1", 8, since=before)
        check("Ctrl+Shift+R 打开减少动画（[THEME64] motion reduce=1）", got,
              (re.search(r"\[THEME64\] motion reduce=1[^\r\n]*", vm.log()[before:]) or [""])[0] if got else "")
        mrm = re.search(r"\[THEME64\] motion reduce=1 fast=\d+ normal=\d+ large=\d+ dock=(\d+)ms", vm.log())
        check("减少动画时动效时长退化为 0ms", mrm is not None and mrm.group(1) == "0",
              mrm.group(0) if mrm else "（无）")
        # 点一个 Dock 图标 -> 回弹 1 帧到位
        goto(vm, mon, tcx, tcy)
        before_rm = len(vm.log())
        mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.6)
        rm = vm.wait_log("[DOCK64] bounce idx=%d frames=1 motion=reduced" % HOVER_IDX, 6)
        check("减少动画：点击回弹 1 帧到位（bounce frames=1 motion=reduced）", rm)
        before = len(vm.log())
        mon.key("ctrl-shift-r", wait=1.0)
        back = vm.wait_log("[THEME64] motion reduce=0", 8)
        check("再按一次恢复动画（reduce=0）", back)

        print("=== 8) 不能出现的日志 ===")
        log = slog = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:", "kfree: bad"):
            check("不应出现 %s" % bad, bad not in log)
        if args.keep:
            print("截图/串口：%s" % tmp)
    finally:
        vm.close()

    n_pass = sum(1 for _, c in checks if c)
    print("断言：%d/%d PASS" % (n_pass, len(checks)))
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
