#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ui_extra64_test.py - 64 位桌面 UI 补充验收（本轮 6 项修复的端到端证据）

覆盖（每一项都要串口打点 + 像素/输入证据）：
  1) 开机 logo 淡入      [UI] boot logo show frames=N fade=ok
  2) 关机/重启画面        [UI] shutdown anim start / [UI] reboot anim start + 动画帧数 + 画面像素
  3) 开始按钮图标        [UI] start icon blit size=24 + 任务栏开始按钮处有彩色图标像素
  4) 桌面图标拖动        QEMU monitor 注入左键拖动 >5px -> [UI] icon drag idx=..
                        再对（移动后的）图标双击 -> [UI] desktop icon open kind=..（双击没被弄坏）
  5) 标题栏三按钮        像素可区分：最小化/最大化不再与标题栏同色（64,64,64），关闭是红底、
                        最小化是横线（白像素纵向跨度小）、最大化是方框（跨度大）
  6) 时钟年月日          [UI] clock text=YYYY-MM-DD HH:MM:SS + 任务栏右侧白色文字像素
  另：桌面选择框（空白处拉框）[UI] selbox x0=.. y0=.. x1=.. y1=.. sel=..
      + 拖动中的截图里有边框色(64,160,255)与半透明填充色(0,90,173)

鼠标注入的实测规律（本脚本据此做闭环定位，见 settle()/aim_axis()/press_on_icon()）：
  * QEMU monitor 的 `mouse_move dx dy` 每个事件 = guest 侧**一个 PS/2 包**；
  * kernel/input.cpp 每包最多走 24px，且累加器有余量 -> 后面每个包（含按键包）还会沿原方向
    继续走最多 3 个 24px（"惯性"）；
  * 所以先连按几次"按下-松开"把惯性排空（settle，零尺寸选择框会把坐标打进串口）再闭环定位。
用法：python tests/ui_extra64_test.py [--img build64/system.img] [--keep]
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

# 与 kernel/gui64.cpp 一致的常量（判像素用）
TASKBAR_H = 32
BTN_W = 30
C_TITLE_ACT = (32, 32, 32)
C_BTN_BG = (64, 64, 64)
C_DESKTOP = (0, 84, 158)
C_SEL_EDGE = (64, 160, 255)
C_SEL_FILL = (0, 90, 173)      # 桌面色 (0,84,158) 与 (0,128,255)@alpha40 的混合结果
C_ICON_SEL = (0, 120, 215)

# 桌面图标 0 的命中框（kernel/gui64.cpp：x±4 / y-4..y+ICON_W+18，ICON_W=48）
ICON0_HIT = (20, 76, 20, 90)


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


def near(c, target, tol=8):
    return all(abs(c[i] - target[i]) <= tol for i in range(3))


class Monitor:
    """QEMU monitor（telnet）客户端；raw() 在**一条连接**里连发多条命令（双击要卡 500ms 窗口）。"""

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
    """一次 QEMU 引导（装好的系统盘，直接进桌面）。"""

    def __init__(self, qemu, img, port, name="vimtu-ui-extra", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_uiextra_")
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

    def wait_ready(self, timeout=60):
        return self.wait_log("[GUI64] ready", timeout)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def rect_count_color(px, w, x0, y0, x1, y1, target, tol=8):
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            if near(sample(px, w, x, y), target, tol):
                n += 1
    return n


def rect_white_spread(px, w, x0, y0, x1, y1):
    """白色（>=200）像素数量 + 纵向跨度。"""
    n = 0
    ys = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = sample(px, w, x, y)
            if c[0] > 200 and c[1] > 200 and c[2] > 200:
                n += 1
                ys.append(y)
    return n, (max(ys) - min(ys) if ys else -1)


def rect_colorful(px, w, x0, y0, x1, y1, thr=40):
    """彩色像素数（max-min 通道差 > thr）——开始按钮应是有图案的图标，不是同色块。"""
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            c = sample(px, w, x, y)
            if max(c) - min(c) > thr:
                n += 1
    return n


# ==================== 鼠标闭环定位 ====================

def probe_pos(vm, mon, wait_open=0.45):
    """按下-松开探测光标位置：朝桌面空白处按会拉起零尺寸选择框，松手时把 x0/y0 打进串口。
    返回 (x, y)；若按在了图标上（只打 select，没有 selbox 行）返回 None。"""
    before = len(vm.log())
    mon.button(1, wait=wait_open)
    mon.button(0, wait=0.55)
    t0 = time.time()
    while time.time() - t0 < 2.5:
        log = vm.log()[before:]
        m = re.search(r"\[UI\] selbox x0=(\d+) y0=(\d+)", log)
        if m:
            return (int(m.group(1)), int(m.group(2)))
        if "[UI] desktop icon select kind=" in log:
            return None
        time.sleep(0.25)
    return None


def settle(vm, mon, times=3):
    """连按几次把鼠标惯性（每包 24px 的残余）排空，返回稳定坐标；按在图标上时返回 None。"""
    pos = None
    for _ in range(times):
        pos = probe_pos(vm, mon)
        if pos is None:
            return None
    return pos


def _move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.move(mag if d > 0 else -mag, 0)
    else:
        mon.move(0, mag if d > 0 else -mag)


def aim_axis(mon, axis, d):
    """朝 d 的方向挪一步。粗步：每包 24px，末尾补一个反向小包"刹车"把残余惯性压到 <24；
    细步：14px 的包在累加器清零时只走 23px，且不留惯性。"""
    if abs(d) < 20:
        return
    if abs(d) < 48:
        _move_axis(mon, axis, d, 14)
        return
    for _ in range((abs(d) + 23) // 24 - 1):
        _move_axis(mon, axis, d, 100)
    _move_axis(mon, axis, d, -20)


def press_on_icon(vm, mon, tx, ty, tries=8):
    """把光标闭环挪到 (tx,ty) 附近（每包 24px）并按下；True = 已按下且命中桌面图标。"""
    for _ in range(tries):
        pos = settle(vm, mon)
        if pos is not None:
            cx, cy = pos
            if abs(cx - tx) > 20 or abs(cy - ty) > 24:
                aim_axis(mon, 0, tx - cx)
                aim_axis(mon, 1, ty - cy)
                continue
        # 已经在目标附近（或探测不到 = 压在图标上）：按下试试
        before = len(vm.log())
        mon.button(1, wait=0.5)
        if vm.wait_log("[UI] desktop icon select kind=", 2.5, since=before):
            return True
        mon.button(0, wait=0.6)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5612)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_uiextra_")
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    # ==================== 场景 1：桌面 + 输入 E2E ====================
    vm = Vm(qemu, args.img, args.port, "vimtu-ui-main", tmp)
    try:
        mon = vm.wait_monitor()
        print("=== 1) 引导 + 本轮新增打点 ===")
        check("桌面就绪 [GUI64] ready", vm.wait_ready(60))
        log0 = vm.log()

        m = re.search(r"\[UI\] boot logo show frames=(\d+) fade=ok", log0)
        check("开机 logo 淡入打点", m is not None, "frames=%s" % (m.group(1) if m else "?"))
        check("开机 logo 帧数 >= 8", (m is not None) and int(m.group(1)) >= 8)

        m = re.search(r"\[UI\] start icon blit size=(\d+)", log0)
        check("开始按钮图标打点", m is not None, "size=%s" % (m.group(1) if m else "?"))
        check("开始按钮图标 16..32px", (m is not None) and 16 <= int(m.group(1)) <= 32)

        m = re.search(r"\[UI\] clock text=(\d{4})-(\d{2})-(\d{2}) (\d{2}):(\d{2}):(\d{2})", log0)
        check("时钟文本含年月日", m is not None)
        if m:
            y, mo, d = int(m.group(1)), int(m.group(2)), int(m.group(3))
            check("年月日取值合理", y >= 2020 and 1 <= mo <= 12 and 1 <= d <= 31,
                  "%04d-%02d-%02d" % (y, mo, d))

        print("=== 2) 桌面选择框（真实鼠标拖动，空白处拉框）===")
        pos = settle(vm, mon)
        check("光标位置探测（零尺寸选择框打点）", pos is not None, "pos=%s" % (pos,))
        before = len(vm.log())
        mon.button(1, wait=0.5)                     # 从当前空白处按下
        for _ in range(22):
            mon.move(-100, -100, wait=0.3)          # 每包 24px -> 往左上拉出大框
        shot_sel = os.path.join(tmp, "selbox.ppm")
        mon.shot(shot_sel)
        if os.path.exists(shot_sel):
            w, h, px = read_ppm(shot_sel)
            edge = rect_count_color(px, w, 0, 0, 520, 390, C_SEL_EDGE, tol=10)
            fill = rect_count_color(px, w, 8, 8, 500, 380, C_SEL_FILL, tol=4)
            check("选择框边框像素", edge > 50, "边框色像素=%d" % edge)
            check("选择框半透明填充像素", fill > 500, "填充色像素=%d" % fill)
        mon.button(0, wait=0.6)
        time.sleep(0.8)
        m = re.search(r"\[UI\] selbox x0=(\d+) y0=(\d+) x1=(\d+) y1=(\d+) sel=(\d+)",
                      vm.log()[before:])
        check("选择框串口打点", m is not None, m.group(0) if m else "（无打点）")
        if m:
            x0, y0, x1, y1, sel = (int(m.group(i)) for i in range(1, 6))
            check("选择框几何合理（拖动距离 >100px）",
                  x1 > x0 + 100 and y1 > y0 + 100, "(%d,%d)-(%d,%d)" % (x0, y0, x1, y1))
            check("选择框内图标被选中（sel>=1）", sel >= 1, "sel=%d" % sel)

        print("=== 3) 桌面图标拖动（真实鼠标拖动 >5px）+ 双击仍可打开 ===")
        tx = (ICON0_HIT[0] + ICON0_HIT[1]) // 2
        ty = (ICON0_HIT[2] + ICON0_HIT[3]) // 2
        pressed = press_on_icon(vm, mon, tx, ty)
        check("光标落到桌面图标上（按下有 select 打点）", pressed)
        dragged = False
        opened_by_dbl = False
        fx = fy = None
        if pressed:
            for _ in range(3):
                mon.move(100, 0, wait=0.3)          # 向右拖 3 包 = 72px（>5px 阈值）
            mid = len(vm.log())
            mon.button(0, wait=0.6)                 # 松开 -> 拖动结束打点
            dragged = vm.wait_log("[UI] icon drag idx=", 3, since=mid)
            m = re.search(r"\[UI\] icon drag idx=(\d+) x=(\d+) y=(\d+)", vm.log()[mid:])
            if m:
                fx, fy = int(m.group(2)), int(m.group(3))
                check("拖动结束后图标 x 右移（初始 24）", fx > 24, "final x=%d y=%d" % (fx, fy))
            time.sleep(0.8)
            shot_drag = os.path.join(tmp, "drag.ppm")
            mon.shot(shot_drag)
            if os.path.exists(shot_drag) and fx is not None:
                w, h, px = read_ppm(shot_drag)
                sel_new = rect_count_color(px, w, max(0, fx - 8), 8, min(w, fx + 60), 104,
                                           C_ICON_SEL, tol=10)
                check("拖动后图标出现在新位置（选中底色像素）", sel_new > 100,
                      "x=%d 附近像素=%d" % (fx, sel_new))
            # 双击：单击 1 的松开 + 500ms 内的按-放 = 单击 2 -> 打开
            if fx is not None:
                moved = press_on_icon(vm, mon, fx + 24, fy + 24)
                check("双击前光标回到移动后的图标上", moved)
                if moved:
                    before_dbl = len(vm.log())
                    mon.raw(["mouse_button 0", "mouse_button 1", "mouse_button 0"],
                            wait_between=0.1, wait_end=0.8)
                    opened_by_dbl = vm.wait_log("[UI] desktop icon open kind=", 4,
                                                since=before_dbl)
        check("桌面图标拖动打点 [UI] icon drag", dragged)
        check("双击仍能打开图标（[UI] desktop icon open）", opened_by_dbl)
        if opened_by_dbl:
            check("双击真的开了应用", "[APP] mypc opened" in vm.log() or
                  "[APP] recycle opened" in vm.log() or "[APP] term opened" in vm.log())

        print("=== 4) 标题栏三按钮像素可区分 ===")
        mon.key("meta_l", wait=1.0)
        mon.key("4", wait=2.2)                      # 菜单序号 3 = 计算器
        check("计算器已打开", "[APP] calc opened" in vm.log())
        check("标题栏三按钮打点 [UI] title btn min/max/close draw ok",
              "[UI] title btn min/max/close draw ok" in vm.log())
        mon.move(300, 300, wait=0.6)                # 光标离开标题栏（不触发 hover）
        time.sleep(0.8)
        shot_win = os.path.join(tmp, "win.ppm")
        mon.shot(shot_win)
        if os.path.exists(shot_win):
            w, h, px = read_ppm(shot_win)
            rows = []
            for y in range(0, h // 2):
                c = 0
                for x in range(0, w, 2):
                    if near(sample(px, w, x, y), C_TITLE_ACT, 6):
                        c += 1
                if c > 30:
                    rows.append(y)
            if rows:
                ty2 = rows[len(rows) // 2]
                xs = [x for x in range(0, w, 1) if near(sample(px, w, x, ty2), C_TITLE_ACT, 6)]
                bx = max(xs) - BTN_W * 3            # 三按钮区左边界（= 窗口右缘 - 90）
                y0, y1 = ty2 - 4, ty2 + 8           # 落在按钮矩形（高 16，居中）内部
                r_min = (bx + 3, bx + 3 + BTN_W - 6)
                r_max = (bx + BTN_W + 3, bx + BTN_W + 3 + BTN_W - 6)
                r_cls = (bx + 2 * BTN_W + 3, bx + 2 * BTN_W + 3 + BTN_W - 6)
                min_bg = rect_count_color(px, w, r_min[0], y0, r_min[1], y1, C_BTN_BG, tol=10)
                max_bg = rect_count_color(px, w, r_max[0], y0, r_max[1], y1, C_BTN_BG, tol=10)
                title_in = min(rect_count_color(px, w, r[0], y0, r[1], y1, C_TITLE_ACT, 4)
                               for r in (r_min, r_max, r_cls))
                min_white, min_spread = rect_white_spread(px, w, r_min[0], y0, r_min[1], y1)
                max_white, max_spread = rect_white_spread(px, w, r_max[0], y0, r_max[1], y1)
                close_white, _ = rect_white_spread(px, w, r_cls[0], y0, r_cls[1], y1)
                close_red = 0
                for y in range(y0, y1):
                    for x in range(r_cls[0], r_cls[1]):
                        c = sample(px, w, x, y)
                        if c[0] > 130 and c[0] - c[1] > 60 and c[0] - c[2] > 60:
                            close_red += 1
                check("三按钮不再与标题栏同色", title_in < 20, "按钮内标题栏色像素=%d" % title_in)
                check("最小化按钮背景可见", min_bg > 100, "(64,64,64) 像素=%d" % min_bg)
                check("最大化按钮背景可见", max_bg > 100, "(64,64,64) 像素=%d" % max_bg)
                check("最小化按钮有白色横线", min_white > 5, "白色像素=%d" % min_white)
                check("最大化按钮有白色方框", max_white > 5, "白色像素=%d" % max_white)
                check("最小化=横线 / 最大化=方框（纵向跨度可区分）",
                      0 <= min_spread < max_spread, "min=%d max=%d" % (min_spread, max_spread))
                check("关闭按钮是红底", close_red > 100, "红系像素=%d" % close_red)
                check("关闭按钮有白色 X 图案", close_white > 3, "白色像素=%d" % close_white)
            else:
                check("找到窗口标题栏（按钮位置）", False, "截图里没有 (32,32,32) 标题栏行")
        else:
            check("窗口截图", False, "screendump 失败")

        print("=== 5) 开始按钮图标 + 时钟像素 ===")
        shot_tb = os.path.join(tmp, "taskbar.ppm")
        mon.shot(shot_tb)
        if os.path.exists(shot_tb):
            w, h, px = read_ppm(shot_tb)
            yb = h - TASKBAR_H
            colorful = rect_colorful(px, w, 4, yb + 2, 34, yb + 30, thr=40)
            check("开始按钮处有彩色图标像素（非手画色块）", colorful > 60, "彩色像素=%d" % colorful)
            white = 0
            for y in range(yb + 2, yb + TASKBAR_H - 2):
                for x in range(w - 300, w - 4):
                    c = sample(px, w, x, y)
                    if c[0] > 200 and c[1] > 200 and c[2] > 200:
                        white += 1
            check("任务栏右侧有白色时钟文字", white > 25, "白色像素=%d" % white)

        print("=== 6) 不能出现的日志 ===")
        log = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:",
                    "kfree: bad"):
            check("不应出现 %s" % bad, bad not in log)
    finally:
        vm.close()

    # ==================== 场景 2：关机画面 ====================
    print("=== 7) 关机画面（[UI] shutdown anim start + 画面像素）===")
    vm2 = Vm(qemu, args.img, args.port + 1, "vimtu-ui-shutdown", tmp)
    try:
        mon2 = vm2.wait_monitor()
        check("关机场景：桌面就绪", vm2.wait_ready(60))
        mon2.key("meta_l", wait=1.0)
        mon2.send("sendkey 0", wait=0.35)           # 菜单序号 9 = 关机
        check("关机打点 [UI] shutdown anim start",
              vm2.wait_log("[UI] shutdown anim start", 8))
        shot_off = os.path.join(tmp, "shutdown.ppm")
        mon2.shot(shot_off, wait=1.0)
        if os.path.exists(shot_off):
            w, h, px = read_ppm(shot_off)
            n = total = blue = 0
            for y in range(0, h, 8):
                for x in range(0, w, 8):
                    c = sample(px, w, x, y)
                    total += 1
                    if c[0] < 16 and c[1] < 16 and c[2] < 16:
                        n += 1
                    if c[0] < 32 and 80 < c[1] < 160 and 180 < c[2] < 255:
                        blue += 1
            check("关机画面以黑底为主", n > total * 0.6, "黑像素占比 %.0f%%" % (100.0 * n / total))
            check("关机画面有蓝色进度条", blue > 20, "进度条像素=%d" % blue)
        check("关机动画帧数 >= 30（约 1 秒）",
              vm2.wait_log("[UI] power anim frames=", 6) and
              int(re.search(r"\[UI\] power anim frames=(\d+)", vm2.log()).group(1)) >= 30)
    finally:
        vm2.close()

    # ==================== 场景 3：重启画面 ====================
    print("=== 8) 重启画面（[UI] reboot anim start）===")
    vm3 = Vm(qemu, args.img, args.port + 2, "vimtu-ui-reboot", tmp)
    try:
        mon3 = vm3.wait_monitor()
        check("重启场景：桌面就绪", vm3.wait_ready(60))
        mon3.key("meta_l", wait=1.0)
        mon3.send("sendkey 9", wait=0.35)           # 菜单序号 8 = 重启
        check("重启打点 [UI] reboot anim start",
              vm3.wait_log("[UI] reboot anim start", 8))
        check("重启也走同一套动画（帧数 >= 30）",
              vm3.wait_log("[UI] power anim frames=", 6) and
              int(re.search(r"\[UI\] power anim frames=(\d+)", vm3.log()).group(1)) >= 30)
    finally:
        vm3.close()

    if args.keep:
        print("截图目录：%s" % tmp)
    n_pass = sum(1 for _, c in checks if c)
    print("断言：%d/%d PASS" % (n_pass, len(checks)))
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
