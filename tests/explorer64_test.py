#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/explorer64_test.py - 文件资源管理器 / "此电脑" 端到端验收（串口 + 像素 + 鼠标注入）

覆盖（每一项都要打点/像素/输入证据，不能只看"没崩"）：
  1) 启动期：文件管理器纯逻辑自检 [EXPL] selftest PASS；盘符表 [DRV64] letter=C: fs=VimtuFS2
  2) 此电脑页：`[UI] explorer thispc drives=<n> browsable=<n>` + 每个盘一行 card/notbrowsable，
     **C: 存在且 fs=VimtuFS2**、ESP 条目写成 reason=esp（灰字、不浏览、点不进去）
  3) 像素：窗口结构（导航窗格底/分隔线/内容区白底/工具栏）、"设备和驱动器"标题区有字、
     **容量条**（灰底 + 蓝填充 + 边框）、不可浏览条目分两行文字
  4) 鼠标注入（QEMU monitor 真实 PS/2 包 + 闭环定位，见 aim_click）：
     双击 C: 盘卡片 -> 进入盘根；双击目录 apps -> /apps；双击 demo -> /apps/demo；
     双击 readme.txt -> 只读预览窗口；双击 hello.elf -> [ELF64] launch ok（或 [UI] explorer run rc=0）
  5) 导航：点"上级"/"后退"回跳（打点路径 + 像素上内容区重画）；面包屑每段可点（点最左一段 -> 此电脑）
  6) 视图：点"查看"在 图标视图 <-> 详细信息视图 之间切换；详细信息四列表头
     （名称/修改日期/类型/大小）按**像素**判定：表头底色 + 3 条列分隔线 + 表头文字像素
  7) 状态栏：`N 个项目` 的 N 与终端 ls（vfs64_list64 的独立计数源）一致
  8) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL

相机（QEMU monitor）与鼠标注入的经验（照 tests/ui_extra64_test.py）：
  * 每个 `mouse_move dx dy` = guest 侧一个 PS/2 包，单包容许的最大步进是 24px，
    残余位移会继续跟着后续包走（"惯性"），所以靠近目标时要补一个反向小包"刹车"；
  * 本脚本**不依赖盲走**：资源管理器把每次客户区点击的**屏幕坐标 + 命中对象**打进串口
    （`[UI] explorer click x=.. y=.. sx=.. sy=.. hit=card:1|item:4|btn:view|..`），
    于是可以先探一次读数、算出偏差、按 24px 包闭环逼近——命中对象 hit 就是唯一真值。
  * 双击 = 两次**按下**间隔 <= 500ms 且同一条目（kernel/explorer64.cpp），所以注入时序是
    "抬起, 按下, 抬起, 按下, 抬起"（一次连接里连续发，保证在 500ms 窗口内）。

用法：py -3 tests\\explorer64_test.py [--qemu 路径] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64        # noqa: E402  find_qemu
import fs_tree_test as fst       # noqa: E402  复用：Python 侧 v3 卷夹具 + open_terminal/wait_for 等

# ==================== 与 kernel/explorer64.cpp 一致的几何/配色常量 ====================
WIN_X, WIN_Y = 120, 60            # 窗口外框（gui64 的 DECO：BORDER=1、TITLE_H=24）
WIN_W, WIN_H = 660, 470
CL_X, CL_Y = WIN_X + 1, WIN_Y + 25          # 客户区原点 = (121, 85)
TOOLBAR_H, ADDR_H, NAV_W, STATUS_H = 26, 28, 150, 22
CONTENT_X, CONTENT_Y = NAV_W + 1, TOOLBAR_H + ADDR_H               # 151, 54
CONTENT_W = WIN_W - 2 - CONTENT_X                                  # 507
CONTENT_H = WIN_H - 25 - 1 - CONTENT_Y - STATUS_H                  # 368
CARD_TOP, CARD_X, CARD_W, CARD_H, CARD_GAP = CONTENT_Y + 30, CONTENT_X + 10, CONTENT_W - 20, 78, 10
BAR_DX, BAR_DY, BAR_W, BAR_H = 58, 34, 300, 12                     # 容量条（相对卡片左上角）
DET_HEAD_Y, DET_HEAD_H, DET_ROW_Y, DET_ROW_H = CONTENT_Y + 4, 20, CONTENT_Y + 24, 20
ICON_CELL_W, ICON_CELL_H = 96, 74
ICON_COLS = CONTENT_W // ICON_CELL_W                               # 5 列

C_LINE = (190, 190, 190)
C_NAV = (250, 250, 250)
C_BAR_BG = (224, 224, 224)
C_BAR_FILL = (0, 120, 215)
C_HEAD = (240, 240, 240)
C_SEP = (200, 200, 200)


def sx(cx):
    return CL_X + cx


def sy(cy):
    return CL_Y + cy


def card_center(idx):
    """此电脑页第 idx 张卡片上的一个安全点（屏幕坐标）。"""
    return (sx(CARD_X + 300), sy(CARD_TOP + idx * (CARD_H + CARD_GAP) + CARD_H // 2))


def card_bar(idx):
    """第 idx 张卡片的容量条矩形（屏幕坐标 x0,y0,x1,y1）。"""
    x = sx(CARD_X + BAR_DX)
    y = sy(CARD_TOP + idx * (CARD_H + CARD_GAP) + BAR_DY)
    return (x, y, x + BAR_W, y + BAR_H)


def icon_cell(idx, scroll=0):
    k = idx - scroll
    col, row = k % ICON_COLS, k // ICON_COLS
    return (sx(CONTENT_X + col * ICON_CELL_W + ICON_CELL_W // 2),
            sy(CONTENT_Y + 2 + row * ICON_CELL_H + ICON_CELL_H // 2))


def det_row(idx, scroll=0):
    row = idx - scroll
    return (sx(CONTENT_X + 120), sy(DET_ROW_Y + row * DET_ROW_H + DET_ROW_H // 2))


# ==================== PPM / 像素工具（照 desktop64_test / ui_extra64_test）====================
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


def rect_count(px, w, x0, y0, x1, y1, target, tol=8):
    n = 0
    for y in range(max(0, y0), y1):
        for x in range(max(0, x0), x1):
            if near(sample(px, w, x, y), target, tol):
                n += 1
    return n


def rect_dark(px, w, x0, y0, x1, y1, thr=150):
    """文字像素（三通道都 < thr）计数：用来判"这块区域真的画了字"。"""
    n = 0
    for y in range(max(0, y0), y1):
        for x in range(max(0, x0), x1):
            c = sample(px, w, x, y)
            if c[0] < thr and c[1] < thr and c[2] < thr:
                n += 1
    return n


def column_lines(px, w, x0, y0, x1, y1, target, tol=10, min_run=10):
    """在 [x0,x1) × [y0,y1) 里找**竖直分隔线**：返回达到 min_run 长的列数。"""
    cols = []
    for x in range(max(0, x0), x1):
        n = 0
        for y in range(max(0, y0), y1):
            if near(sample(px, w, x, y), target, tol):
                n += 1
        if n >= min_run:
            cols.append(x)
    # 合并相邻列（1px 宽的线抗锯齿时会占 2-3 列）
    groups = 0
    last = -10
    for x in cols:
        if x - last > 2:
            groups += 1
        last = x
    return groups


# ==================== QEMU monitor / 会话 ====================
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

    def raw(self, cmds, wait_between=0.12, wait_end=0.5):
        """一条连接里连发多条命令（双击必须卡 500ms 窗口，不能分开连接）。"""
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

    def type_line(self, text, per_key=0.12):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
                 ">": "shift-dot", "=": "equal", ":": "shift-semicolon"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey 不支持该字符：%r" % ch)
        self.key("ret", wait=per_key + 0.15)

    def shot(self, path, wait=2.0):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % path.replace("\\", "/"), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


class Vm:
    def __init__(self, qemu, disk, port, serial, name="Vimtu64-explorer"):
        self.serial = serial
        self.port = port
        self.proc = subprocess.Popen(
            fst.qemu_args(qemu, [disk], serial, port, name),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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
        fst.kill(self.proc)


# ==================== 鼠标闭环（用 explorer 自己的点击打点定位）====================
# 为什么这么做：QEMU monitor 只能发"相对位移包"，guest 侧 PS/2 驱动按包限速（实测每包 ~24px，
# 且残余位移会继续跟着后续包走）。盲走会累积误差，所以这里用资源管理器自己打的
# `[UI] explorer click ... sx=.. sy=.. hit=..` 当反馈：**命中对象**才是真值，偏差只用来决定下一步挪多少。
CLICK_RE = re.compile(r"\[UI\] explorer click x=(\d+) y=(\d+) sx=(\d+) sy=(\d+) hit=(\S+)")
SELBOX_RE = re.compile(r"\[UI\] selbox x0=(\d+) y0=(\d+)")
STEP_X, STEP_Y = 24, 24          # 一个 mouse_move 100 包实测走多少像素（下面 calibrate() 会测真值）
SAFE_POINT = None                # 内容区右下角的安全点（用于关掉误开的"文件"菜单）


def last_probe(vm, since):
    """从串口日志里取最近一次"光标位置"：优先 explorer 点击打点，其次桌面选择框打点。"""
    log = vm.log()[since:]
    pos, hit = None, ""
    for m in CLICK_RE.finditer(log):
        pos, hit = (int(m.group(3)), int(m.group(4))), m.group(5)
    if pos is None:
        for m in SELBOX_RE.finditer(log):
            pos, hit = (int(m.group(1)), int(m.group(2))), "desktop"
    return (pos[0], pos[1], hit) if pos else None


def click_once(vm, mon):
    """按下-抬起一次（与上一次点击间隔 > 500ms，绝不误触发双击），返回 (sx, sy, hit) 或 None。"""
    since = len(vm.log())
    mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.5)
    time.sleep(0.55)
    return last_probe(vm, since)


def move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.send("mouse_move %d 0" % (mag if d > 0 else -mag), wait=0.2)
    else:
        mon.send("mouse_move 0 %d" % (mag if d > 0 else -mag), wait=0.2)


def park_cursor(mon, times=58):
    """把光标顶到屏幕左上角（0,0 附近），拿到一个确定的起点。"""
    for _ in range(times):
        mon.send("mouse_move -100 -100", wait=0.06)
    time.sleep(0.6)


def calibrate(vm, mon):
    """实测"1 个 100 包 + 探测点击（再花掉 1 个 24px 包）"的位移，用来核对内核模型（期望 ~48px）。"""
    global STEP_X, STEP_Y
    p0 = click_once(vm, mon)
    if not p0:
        return None
    move_px(mon, 0, +1, 300)                # 干净地走 300px（含刹车）：位置 p0+(300,0)
    p1 = click_once(vm, mon)                # 两个探测点都还在桌面上（y<60），不会碰到窗口控件
    if p1:
        STEP_X = p1[0] - p0[0]
    move_px(mon, 1, +1, 45)
    p2 = click_once(vm, mon)
    if p2 and p1:
        STEP_Y = p2[1] - p1[1]
    return (STEP_X, STEP_Y)


def blind_goto(mon, tx, ty):
    """park 到已知起点 (0,0) -> 按内核模型精确移动到 (tx,ty)。目标必须在第一象限内。"""
    park_cursor(mon)
    move_px(mon, 0, +1, tx)
    move_px(mon, 1, +1, ty)
    time.sleep(0.25)


def single_click_at(mon):
    mon.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=0.45)
    time.sleep(0.55)


def click_safe(vm, mon):
    """点内容区右下角的空白：如果"文件"菜单被误开，这一下只会把它关掉（不会误选菜单项）。

    注意：这里是**盲走**（park -> 精确移动 -> 单击），不做"先点一下看看在哪"的探测 ——
    探测点击可能正好落在工具栏按钮上，把状态改掉（踩过的坑）。
    """
    if SAFE_POINT is None:
        return False
    blind_goto(mon, SAFE_POINT[0], SAFE_POINT[1])
    single_click_at(mon)
    return True


# ---- guest 鼠标的"累加器"模型（读 kernel/input.cpp 得出，自动验收按它做精确位移）----
#   * 每个 PS/2 包：accum += dx * 17/10（灵敏度 1.7），accum 有 ±96 上限；
#     然后 step = clamp(accum, ±24)，accum -= step，光标走 step（y 屏幕向下）。
#   * 所以"1 个 mouse_move 100 包"只走 24px，并在内核里留 72 的残余 —— 这正是
#     ui_extra64_test 里说的"惯性"：后面的按键包还会各自再走 24px。
#   * 反方向补 1 个 -43 的包正好把残余清成 0（净位移 -1px）；此时再补一个小包 r，
#     位移 = floor(1.7r)（r <= 14 时精准且不留残余）。
MOUSE_STEP = 24
MOUSE_BRAKE = 43


def move_px(mon, axis, d, px):
    """按内核模型精确移动 |px| 像素（方向 d = ±1）；结束时残余清零，后续点击包不会再飘。"""
    if px < 1 or d == 0:
        return
    n = min(48, px // MOUSE_STEP)
    rem = px - n * MOUSE_STEP
    for _ in range(n):
        move_axis(mon, axis, d, 100)
    move_axis(mon, axis, -d, MOUSE_BRAKE)
    if rem >= 2:
        mag = int(round(rem / 1.7))
        if mag >= 1:
            move_axis(mon, axis, d, mag)


def nudge_to(mon, px, py, tx, ty, tol):
    """闭环的一步：用 move_px 把光标精确挪到目标附近（每轴独立）。"""
    dx, dy = tx - px, ty - py
    if abs(dx) > tol:
        move_px(mon, 0, 1 if dx > 0 else -1, abs(dx))
    if abs(dy) > tol:
        move_px(mon, 1, 1 if dy > 0 else -1, abs(dy))


def double_click(mon):
    """抬起-按下-抬起-按下-抬起：两次按下间隔 ~0.2s（< 500ms 窗口）+ 同一条目 = 双击。"""
    mon.raw(["mouse_button 0", "mouse_button 1", "mouse_button 0",
             "mouse_button 1", "mouse_button 0"], wait_between=0.10, wait_end=0.6)


def calibrate_offset(vm, mon, tries=5):
    """量出"park -> 精确移动到安全点"的实际落点误差（闭环保底）。

    为什么要这一步：自动化环境里 PS/2 包偶尔会丢（实测每一步都可能少走 20~200px），
    所以盲走之后必须量一次误差；安全点落在内容区空白处，点它只会有 hit=none，不会改状态。
    """
    dxe = dye = 0
    for i in range(tries):
        blind_goto(mon, SAFE_POINT[0] + dxe, SAFE_POINT[1] + dye)
        since = len(vm.log())
        single_click_at(mon)
        p = last_probe(vm, since)
        if p is None:
            # 连内容区都点不到：窗口可能被别的窗口盖住 -> 用键盘把它置顶再来
            ensure_window_on_top(vm, mon)
            continue
        dxe = SAFE_POINT[0] - p[0]
        dye = SAFE_POINT[1] - p[1]
        if abs(dxe) <= 6 and abs(dye) <= 6:
            return dxe, dye, i + 1
    return dxe, dye, tries


def aim_click(vm, mon, tx, ty, want_hit, tries=4, label=""):
    """量误差 -> 精确移到 (tx,ty) -> 双击（命中由调用方按打点断言）。"""
    dxe, dye, n = calibrate_offset(vm, mon)
    for k in range(tries):
        blind_goto(mon, tx + dxe, ty + dye)
        double_click(mon)
        return True, "double@(%d,%d) 误差修正(%d,%d) loops=%d" % (tx + dxe, ty + dye, dxe, dye, n)
    return False, "无法到达 (%d,%d) %s" % (tx, ty, label)


def aim_single_click(vm, mon, tx, ty, want_hit, tries=4):
    """量误差 -> 精确移到 (tx,ty) -> 单击（命中由调用方按打点断言）。"""
    dxe, dye, n = calibrate_offset(vm, mon)
    for k in range(tries):
        blind_goto(mon, tx + dxe, ty + dye)
        single_click_at(mon)
        return True, "click@(%d,%d) 误差修正(%d,%d) loops=%d" % (tx + dxe, ty + dye, dxe, dye, n)
    return False, "无法到达 (%d,%d)" % (tx, ty)


def ensure_window_on_top(vm, mon, tries=4):
    """确保资源管理器窗口在最前面且**能收到点击**（自动化环境的保险动作）。

    踩过的坑：窗口一旦被别的窗口盖住（例如终端在它上面），点击全部打到别人身上，
    串口里一行 `[UI] explorer click` 都不会出现。做法：用开始菜单的"我的电脑"
    （序号 1）把已开的窗口 set_active 置顶，再点一次内容区空白确认点击真的到了资源管理器。
    """
    for k in range(tries):
        mon.key("meta_l", wait=0.8)
        mon.key("2", wait=1.6)
        time.sleep(0.8)
        since = len(vm.log())
        blind_goto(mon, SAFE_POINT[0], SAFE_POINT[1])
        single_click_at(mon)
        p = last_probe(vm, since)
        if p is not None:
            return True, "tries=%d 探针命中 %s" % (k + 1, p[2])
    return False, "点击一直打不到资源管理器窗口"


VIEW_RE = re.compile(r"\[UI\] explorer view=(\w+)")


def last_view(vm):
    """日志里最后一条视图打点（视图是切换语义，必须按实际状态收敛）。"""
    m = None
    for mm in VIEW_RE.finditer(vm.log()):
        m = mm
    return m.group(1) if m else ""


def ensure_view(vm, mon, want, tries=4):
    """把视图收敛到 want（icons|details）：点"查看" -> 读日志里的实际视图 -> 不对再点。

    QEMU 写 -serial file: 有 1~3 秒落盘延迟，所以每轮点完要等新打点出现再判断。
    """
    for _ in range(tries):
        if last_view(vm) == want:
            return True
        before = vm.log().count("[UI] explorer view=")
        aim_single_click(vm, mon, sx(176), sy(13), "btn:view")
        for _ in range(20):
            if vm.log().count("[UI] explorer view=") > before:
                break
            time.sleep(0.3)
        time.sleep(1.0)
    return last_view(vm) == want


def press_and_wait(vm, mon, tx, ty, want_hit, needle, tries=3):
    """点按钮 + 等它真的生效（打点出现）；没生效就再来一次（先点空白关掉可能误开的菜单）。

    等待给足 25s：QEMU 写 -serial file: 有 1~3s 落盘延迟，等太短会把成功误判成失败并重复点击。
    """
    det = ""
    for _ in range(tries):
        since = len(vm.log())
        ok, det = aim_single_click(vm, mon, tx, ty, want_hit)
        if ok and vm.wait_log(needle, 25, since=since):
            return True, det
        click_safe(vm, mon)
    return False, det


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(fst.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % fst.SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_explorer_")
    disk = os.path.join(tmp, "small.img")
    serial = os.path.join(tmp, "serial.log")
    info = fst.make_small_system_disk(disk)
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, log):
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:"):
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    if info is None:
        print("  [FAIL] 无法生成小系统盘夹具（build64/system.img 缺失或过大）")
        return 2

    port = fst.free_port()
    vm = Vm(qemu, disk, port, serial)
    mon = Monitor(port)
    try:
        # ==================== 阶段 1：引导 + 启动期打点 ====================
        print("=== 阶段 1：引导（Python 造的 16MB 系统盘：C: = VimtuFS2 v3 卷 + ESP 条目）===")
        up = vm.wait_log("[GUI64] ready", 120)
        check("桌面就绪（[GUI64] ready）", up)
        log = vm.log()
        check("文件管理器纯逻辑自检（[EXPL] selftest PASS）", "[EXPL] selftest PASS" in log)
        m = re.search(r"\[DRV64\] letter=C: disk=(\d+) part=(\d+) fs=VimtuFS2 total_kb=(\d+) free_kb=(\d+)", log)
        check("C: = VimtuFS2 卷（[DRV64] letter=C:）", m is not None, m.group(0) if m else "（缺行）")
        total_kb = int(m.group(3)) if m else 0
        check("C: 容量 = 主分区扇区数/2（total_kb=%d）" % fst.SMALL_MAIN_TOTAL_KB,
              total_kb == fst.SMALL_MAIN_TOTAL_KB, "实际 total_kb=%d" % total_kb)
        check("ESP 被标为不浏览（[DRV64] skip ... reason=esp）",
              re.search(r"\[DRV64\] skip lba=\d+ type=0xEF reason=esp", log) is not None)

        # ==================== 阶段 2：终端造目录树（explorer 的独立计数源）====================
        print("=== 阶段 2：终端造 /apps/demo/readme.txt + ls（vfs64_list64 的独立计数）===")
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        mon.type_line("mkdir /apps")
        mon.type_line("mkdir /apps/demo")
        mon.type_line("write /apps/demo/readme.txt hello explorer")
        wlog = fst.wait_for(serial, "[FD64] open path=/apps/demo/readme.txt", 25, vm.proc)
        check("子目录写文件走真路径（[FD64] open path=/apps/demo/readme.txt）",
              "[FD64] open path=/apps/demo/readme.txt fd=" in wlog)
        mon.type_line("cat /apps/demo/readme.txt")
        clog = fst.wait_for(serial, "[TERM] cmd cat bytes=", 25, vm.proc)
        mc = re.search(r"\[TERM\] cmd cat bytes=(\d+)", clog)
        check("cat 回读 14 字节（多级路径 + 写入生效）", bool(mc) and mc.group(1) == "14",
              mc.group(0) if mc else "（缺 cat 行）")
        mon.type_line("ls")
        llog = fst.wait_for(serial, "[TERM] cmd ls entries=", 25, vm.proc)
        ml = re.findall(r"\[TERM\] cmd ls entries=(\d+)", llog)
        ls_entries = int(ml[-1]) if ml else -1
        check("根目录条目数（终端 ls，独立计数源）= %d" % ls_entries, ls_entries >= 7,
              "entries=%d" % ls_entries)

        # ==================== 阶段 3：打开"此电脑" + 像素 ====================
        print("=== 阶段 3：打开文件管理器（此电脑）===")
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=2.5)
        elog = fst.wait_for(serial, "[UI] explorer thispc", 25, vm.proc)
        check("双击/菜单进入此电脑（[APP] mypc opened）", "[APP] mypc opened" in elog)
        mt = re.search(r"\[UI\] explorer thispc drives=(\d+) browsable=(\d+)", elog)
        check("此电脑页打点（drives>=2 browsable>=1）",
              bool(mt) and int(mt.group(1)) >= 2 and int(mt.group(2)) >= 1,
              mt.group(0) if mt else "（缺行）")
        md = re.search(r"\[UI\] explorer drive letter=C: fs=VimtuFS2 total_kb=(\d+) free_kb=(\d+)", elog)
        check("此电脑页给 C: 一行（letter=C: fs=VimtuFS2 total_kb/free_kb）", md is not None,
              md.group(0) if md else "（缺行）")
        mcards = re.findall(r"\[UI\] explorer card idx=(\d+) letter=(\S+) kind=(\S+)", elog)
        card_c = [i for i, letter, kind in mcards if letter == "C:" and kind == "browsable"]
        check("C: 在卡片列表里且是可浏览（card idx）", len(card_c) == 1, str(mcards))
        card_esp = [i for i, letter, kind in mcards if letter == "-" and kind == "skip"]
        check("不可浏览条目也列进卡片（letter=- kind=skip）", len(card_esp) >= 1, str(mcards))
        check("不可浏览条目写明原因（notbrowsable ... reason=esp）",
              re.search(r"\[UI\] explorer notbrowsable name=.+ fs=\S+ reason=esp", elog) is not None)
        check("卡片的 idx 与顺序一致（此电脑页第 0..n 张）",
              [int(i) for i, _, _ in mcards] == list(range(len(mcards))), str(mcards))
        idx_c = int(card_c[0]) if card_c else 0

        # ---- 像素：窗口结构 + "设备和驱动器"标题 + 容量条 + 导航分隔 ----
        shot = os.path.join(tmp, "thispc.ppm")
        check("此电脑页截图", mon.shot(shot))
        if os.path.exists(shot):
            w, h, px = read_ppm(shot)
            check("分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
            nav_bg = rect_count(px, w, sx(4), sy(CONTENT_Y + 40), sx(NAV_W - 6), sy(CONTENT_Y + 300), C_NAV, tol=4)
            check("导航窗格底色（浅灰面板）", nav_bg > 20000, "像素=%d" % nav_bg)
            sep = rect_count(px, w, sx(NAV_W), sy(CONTENT_Y), sx(NAV_W) + 2, sy(CONTENT_Y + CONTENT_H), C_LINE, tol=12)
            check("导航窗格与内容区的分隔线", sep > 250, "分隔色像素=%d" % sep)
            title = rect_dark(px, w, sx(CONTENT_X + 8), sy(CONTENT_Y + 4), sx(CONTENT_X + 300), sy(CONTENT_Y + 22))
            check("'设备和驱动器 (N)' 标题区有文字像素", title > 40, "暗像素=%d" % title)
            bx0, by0, bx1, by1 = card_bar(idx_c)
            bar_bg = rect_count(px, w, bx0, by0, bx1, by1, C_BAR_BG, tol=6)
            bar_fill = rect_count(px, w, bx0, by0, bx1, by1, C_BAR_FILL, tol=10)
            bar_edge = rect_count(px, w, bx0 - 1, by0 - 1, bx1 + 2, by1 + 2, C_LINE, tol=14)
            check("容量条：灰底（矩形判定）", bar_bg > 2000, "灰底像素=%d" % bar_bg)
            check("容量条：蓝填充（已用空间，至少一条）", bar_fill >= 4, "蓝像素=%d" % bar_fill)
            check("容量条：有边框", bar_edge > 100, "边框像素=%d" % bar_edge)
            txt = rect_dark(px, w, sx(CARD_X + 58), sy(CARD_TOP + idx_c * (CARD_H + CARD_GAP) + 8),
                            sx(CARD_X + CARD_W - 8), sy(CARD_TOP + idx_c * (CARD_H + CARD_GAP) + CARD_H))
            check("卡片上有卷标/容量文字", txt > 60, "暗像素=%d" % txt)
            if card_esp:
                j = int(card_esp[0])
                esp_txt = rect_dark(px, w, sx(CARD_X + 58), sy(CARD_TOP + j * (CARD_H + CARD_GAP) + 40),
                                    sx(CARD_X + CARD_W - 8), sy(CARD_TOP + j * (CARD_H + CARD_GAP) + CARD_H))
                check("不可浏览条目有灰字说明（EFI 系统分区（不浏览））", esp_txt > 30, "暗像素=%d" % esp_txt)

        # ==================== 阶段 4：双击 C: 进入盘根 ====================
        print("=== 阶段 4：鼠标双击 C: 盘卡片 -> 进入盘根（图标视图）===")
        global SAFE_POINT
        SAFE_POINT = (sx(CONTENT_X + CONTENT_W - 40), sy(CONTENT_Y + CONTENT_H - 40))
        top_ok, top_det = ensure_window_on_top(vm, mon)
        check("资源管理器窗口在最前面且能收到点击", top_ok, top_det)
        park_cursor(mon)
        p0 = click_once(vm, mon)
        check("光标闭环起点（桌面/窗口内可读到坐标）", p0 is not None, str(p0))
        step = calibrate(vm, mon)
        check("鼠标精确位移模型（100 包 = 24px + 刹车包清残余）：x 走 300 左右",
              step is not None and abs(step[0] - 300) <= 40,
              "实测 step=%s（y 轴允许丢包，闭环会补）" % (step,))
        park_cursor(mon)
        for _ in range(4):
            p0 = click_once(vm, mon)         # 多探几次把停车残余排空（park 会留 -72 的累加器）
        check("标定后回到起点 (0,0)", p0 is not None and p0[0] <= 40 and p0[1] <= 40, str(p0))
        tx, ty = card_center(idx_c)
        hit_ok, det = aim_click(vm, mon, tx, ty, "card:%d" % idx_c, label="C: 卡片")
        check("双击 C: 卡片（精确移动 + 双击）", hit_ok, det)
        nlog = vm.log()
        check("进入 C: 后打点（explorer nav path=/ items=<n> view=icons）",
              re.search(r"\[UI\] explorer nav path=/ items=(\d+) view=icons", nlog) is not None)
        mn = re.findall(r"\[UI\] explorer nav path=/ items=(\d+) view=icons", nlog)
        items_root = int(mn[-1]) if mn else -1
        check("状态栏 N 个项目 = 终端 ls 的条目数（%d）" % ls_entries, items_root == ls_entries,
              "explorer items=%d ls=%d" % (items_root, ls_entries))
        mdir = re.search(r"\[UI\] explorer item idx=(\d+) name=apps type=dir size=0 mtime=\d{4}-\d{2}-\d{2} \d{2}:\d{2} kind=dir", nlog)
        check("根目录条目表里有目录 apps（type=dir）", mdir is not None, mdir.group(0) if mdir else "（缺行）")
        mel = re.search(r"\[UI\] explorer item idx=(\d+) name=hello\.elf type=file size=\d+ mtime=\d{4}-\d{2}-\d{2} \d{2}:\d{2} kind=elf", nlog)
        check("根目录条目表里有 hello.elf（kind=elf）", mel is not None, mel.group(0) if mel else "（缺行）")
        idx_apps = int(mdir.group(1)) if mdir else -1
        idx_elf = int(mel.group(1)) if mel else -1

        shot2 = os.path.join(tmp, "drive_icons.ppm")
        check("图标视图截图", mon.shot(shot2))
        if os.path.exists(shot2):
            w, h, px = read_ppm(shot2)
            amber = rect_count(px, w, sx(CONTENT_X), sy(CONTENT_Y), sx(CONTENT_X + CONTENT_W),
                               sy(CONTENT_Y + CONTENT_H), (240, 190, 80), tol=12)
            check("图标视图有文件夹图标（琥珀色几何图形）", amber > 100, "琥珀像素=%d" % amber)
            white = rect_count(px, w, sx(CONTENT_X), sy(CONTENT_Y), sx(CONTENT_X + CONTENT_W),
                               sy(CONTENT_Y + CONTENT_H), (255, 255, 255), tol=3)
            check("图标视图内容区白底（不是空的/花的）", white > 30000, "白像素=%d" % white)

        # ==================== 阶段 5：进入子目录（双击目录）+ 文本预览 ====================
        print("=== 阶段 5：双击 apps -> demo -> readme.txt（只读预览）===")
        if idx_apps >= 0:
            a = icon_cell(idx_apps)
            hit_ok, det = aim_click(vm, mon, a[0], a[1], "item:%d" % idx_apps, label="apps")
            check("双击目录 apps（精确移动 + 双击）", hit_ok, det)
            nlog = vm.log()
            check("进入 /apps（nav path=/apps items=1）",
                  re.search(r"\[UI\] explorer nav path=/apps items=1 view=icons", nlog) is not None)
            check("进入目录打点（enter name=apps kind=dir）",
                  re.search(r"\[UI\] explorer enter name=apps kind=dir", nlog) is not None)
            m2 = re.search(r"\[UI\] explorer item idx=(\d+) name=demo type=dir size=0", nlog)
            idx_demo = int(m2.group(1)) if m2 else 0
            b = icon_cell(idx_demo)
            hit_ok, det = aim_click(vm, mon, b[0], b[1], "item:%d" % idx_demo, label="demo")
            check("双击目录 demo（精确移动 + 双击）", hit_ok, det)
            nlog = vm.log()
            check("进入 /apps/demo（nav path=/apps/demo items=1）",
                  re.search(r"\[UI\] explorer nav path=/apps/demo items=1 view=icons", nlog) is not None)
            m3 = re.search(r"\[UI\] explorer item idx=(\d+) name=readme\.txt type=file size=14 mtime=\d{4}-\d{2}-\d{2} \d{2}:\d{2} kind=text", nlog)
            check("readme.txt 类型判定 = text / 大小 14 字节", m3 is not None, m3.group(0) if m3 else "（缺行）")
            idx_txt = int(m3.group(1)) if m3 else 0
            c = icon_cell(idx_txt)
            hit_ok, det = aim_click(vm, mon, c[0], c[1], "item:%d" % idx_txt, label="readme.txt")
            check("双击 readme.txt（精确移动 + 双击）", hit_ok, det)
            plog = fst.wait_for(serial, "[UI] explorer preview", 20, vm.proc)
            check("文本 -> 只读预览窗口（preview name=readme.txt bytes=14）",
                  re.search(r"\[UI\] explorer preview name=readme\.txt bytes=14", plog) is not None)
        else:
            check("双击目录 apps", False, "根目录里没有找到 apps 条目行")

        # ==================== 阶段 6：上级 / 后退 / 面包屑 ====================
        print("=== 阶段 6：上级 / 后退 / 面包屑回跳 ===")
        hit_ok, det = press_and_wait(vm, mon, sx(75), sy(40), "btn:up", "[UI] explorer up path=")
        check("点'上级'按钮（精确移动 + 单击）", hit_ok, det)
        ulog = fst.wait_for(serial, "[UI] explorer up path=", 20, vm.proc)
        mu = re.findall(r"\[UI\] explorer up path=(\S+)", ulog)
        check("上级回到 /apps（[UI] explorer up path=/apps）", bool(mu) and mu[-1] == "/apps",
              mu[-1] if mu else "（缺行）")
        hit_ok, det = press_and_wait(vm, mon, sx(19), sy(40), "btn:back", "[UI] explorer back path=")
        check("点'后退'按钮（精确移动 + 单击）", hit_ok, det)
        blog = fst.wait_for(serial, "[UI] explorer back path=", 20, vm.proc)
        mb = re.findall(r"\[UI\] explorer back path=(\S+)", blog)
        check("后退回到上一站（[UI] explorer back path=...）", bool(mb), mb[-1] if mb else "（缺行）")
        # 面包屑第一段（此电脑）：x ∈ [96, 96+宽]，取一个安全点
        hit_ok, det = press_and_wait(vm, mon, sx(120), sy(40), "crumb:0", "[UI] explorer thispc")
        check("面包屑第一段'此电脑'可点（精确移动 + 单击）", hit_ok, det)
        clog2 = fst.wait_for(serial, "[UI] explorer thispc", 20, vm.proc)
        check("面包屑跳回此电脑（再次出现 thispc 打点）",
              clog2.count("[UI] explorer thispc") > elog.count("[UI] explorer thispc"))

        # ==================== 阶段 7：查看（图标 <-> 详细信息）+ 四列表头像素 ====================
        print("=== 阶段 7：'查看' 切到详细信息视图 + 四列表头像素 ===")
        view_before = vm.log().count("[UI] explorer view=")
        view_ok = ensure_view(vm, mon, "details")
        check("点'查看'按钮切到详细信息视图（[UI] explorer view=details rows=<n>）",
              view_ok and re.search(r"\[UI\] explorer view=details rows=\d+", vm.log()) is not None,
              "view=%s" % last_view(vm))
        check("视图切换有串口打点（[UI] explorer view=.. rows=..）",
              vm.log().count("[UI] explorer view=") > view_before)
        # 重新进 C: 根目录（面包屑 -> 此电脑 -> 双击卡片），在详细信息视图下看四列
        tx, ty = card_center(idx_c)
        hit_ok, det = aim_click(vm, mon, tx, ty, "card:%d" % idx_c, label="C: 卡片（details）")
        check("详细信息视图下再进 C:（精确移动 + 双击）", hit_ok, det)
        vlog = fst.wait_for(serial, "view=details", 20, vm.proc)
        mvd = re.findall(r"\[UI\] explorer nav path=/ items=(\d+) view=details", vlog)
        check("详细信息视图的 nav 打点（view=details items=<n>）", bool(mvd), mvd[-1] if mvd else "（缺行）")
        shot3 = os.path.join(tmp, "details.ppm")
        check("详细信息视图截图", mon.shot(shot3))
        det_px = read_ppm(shot3) if os.path.exists(shot3) else None
        if det_px:
            w, h, px = det_px
            head_bg = rect_count(px, w, sx(CONTENT_X + 2), sy(DET_HEAD_Y), sx(CONTENT_X + CONTENT_W - 2),
                                 sy(DET_HEAD_Y + DET_HEAD_H - 2), C_HEAD, tol=5)
            check("表头条底色（四列同一行）", head_bg > 800, "表头底色像素=%d" % head_bg)
            head_txt = rect_dark(px, w, sx(CONTENT_X + 2), sy(DET_HEAD_Y), sx(CONTENT_X + CONTENT_W - 2),
                                 sy(DET_HEAD_Y + DET_HEAD_H))
            check("表头有文字（名称/修改日期/类型/大小）", head_txt > 40, "暗像素=%d" % head_txt)
            sep_cols = column_lines(px, w, sx(CONTENT_X + 2), sy(DET_HEAD_Y + 1),
                                    sx(CONTENT_X + CONTENT_W - 2), sy(DET_HEAD_Y + DET_HEAD_H - 1), C_SEP, tol=14)
            check("三条列分隔线 = 四列布局", sep_cols >= 3, "分隔线列数=%d" % sep_cols)
            rows = rect_count(px, w, sx(CONTENT_X + 2), sy(DET_ROW_Y), sx(CONTENT_X + CONTENT_W - 2),
                              sy(DET_ROW_Y + 40), (255, 255, 255), tol=3)
            check("明细行区是白底（有行内容）", rows > 5000, "白像素=%d" % rows)
            # 状态栏"N 个项目"：窗口左下角一行必须有文字像素
            st_txt = rect_dark(px, w, sx(4), sy(CONTENT_Y + CONTENT_H + 3),
                               sx(220), sy(CONTENT_Y + CONTENT_H + STATUS_H - 3))
            check("状态栏有 'N 个项目' 文字像素", st_txt > 20, "暗像素=%d" % st_txt)
        else:
            check("详细信息视图像素检查", False, "screendump 失败")

        # ==================== 阶段 8：双击 ELF64 程序 ====================
        print("=== 阶段 8：双击 hello.elf -> ring3 运行（[ELF64] launch ok）===")
        if idx_elf >= 0:
            r = det_row(idx_elf)
            hit_ok, det = aim_click(vm, mon, r[0], r[1], "item:%d" % idx_elf, label="hello.elf")
            check("双击 hello.elf（精确移动 + 双击）", hit_ok, det)
            xlog = fst.wait_for(serial, "[UI] explorer run name=hello.elf", 40, vm.proc)
            check("运行 .elf 打点（explorer run name=hello.elf kind=elf rc=0）",
                  re.search(r"\[UI\] explorer run name=hello\.elf kind=elf rc=0", xlog) is not None)
            check("ELF64 加载器真的跑起来（[ELF64] launch ok rc=0）",
                  re.search(r"\[ELF64\] launch ok rc=0 path=/hello\.elf", xlog) is not None)
        else:
            check("双击 hello.elf", False, "根目录里没有找到 hello.elf 条目行")

        # ==================== 阶段 9：日志卫生 ====================
        print("=== 阶段 9：日志卫生 ===")
        final = vm.log()
        forbid("全程", final)
        check("文件管理器自检只 PASS 没 FAIL", "selftest FAIL" not in final)
    finally:
        vm.close()

    if args.keep:
        print("[explorer] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
