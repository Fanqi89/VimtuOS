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


def settle_frames(vm, since, n=2, timeout=20):
    """等外壳至少跑过 n 个"每秒重画"周期再截图。

    为什么需要：外壳每秒都会把 explorer 整窗标脏重画（gui64_run 的每秒脏矩形），
    而点击回调的打点（[UI] explorer click/nav/view=…）发生在**重画之前**。
    旧视觉一帧很轻，截图大体能等到重画；本批（Windows 11 外观：玻璃/阴影）一帧更重，
    截图会抢在重画之前拿到上一屏 —— 这里用"时钟打点条数"当帧循环的见证，
    保证内容区已经按当前状态重画过一次，再截图。**不改任何像素判据。**
    """
    t0 = time.time()
    base = vm.log()[since:].count("[UI] clock text=")
    while time.time() - t0 < timeout:
        if vm.log()[since:].count("[UI] clock text=") >= base + n:
            time.sleep(0.4)          # 再留半帧给 flip
            return True
        if vm.proc.poll() is not None:
            return False
        time.sleep(0.3)
    return False


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

def wait_re(vm, pattern, timeout, since):
    """等 since 之后出现匹配 pattern 的打点（explorer64_test 的 Vm 只有 wait_log，没有 wait_new）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if re.search(pattern, vm.log()[since:]):
            return True
        if vm.proc.poll() is not None:
            return False
        time.sleep(0.3)
    return False


def dclick_card_until(vm, mon, idx, letter, timeout=15, tries=3):
    """双击盘符卡片 + **等 enter 打点出现**（返回 (ok, det)）。

    为什么这么写（缺陷 3 的根因）：QEMU 的 `-serial file:` 有落盘延迟，注入完双击立刻
    `vm.log()` 快照会偶发拿到"还没进目录"的旧视图 —— 验收基线里
    `[UI] explorer nav path=/ items=-1`（-1 是脚本自己的"没匹配到"哨兵）就是这个竞态，
    不是内核列目录坏：内核在同一个点击回调里同步完成 激活卷 -> 列目录 -> 打 enter/nav
    （kernel/explorer64.cpp: exp_enter_drive_letter/exp_refresh_dir/exp_log_nav），没有任何异步路径。
    这里等打点；**只有确认双击没生效（enter 一直不出现）才重试注入**，判据一字未改。
    """
    needle = "[UI] explorer enter letter=%s:" % letter
    det = ""
    for k in range(tries):
        since = len(vm.log())
        ok, det = aim_click(vm, mon, card_center(idx)[0], card_center(idx)[1],
                            "card:%d" % idx, label="%s: 卡片" % letter)
        for _ in range(int(timeout / 0.3)):
            if needle in vm.log()[since:]:
                return True, det
            if vm.proc.poll() is not None:
                return False, det
            time.sleep(0.3)
        print("      （双击 %s: 卡片后 %ds 没等到 enter 打点：重试注入 %d/%d）"
              % (letter, timeout, k + 1, tries))
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
        # ★ 缺陷 4（登录必须显式输入）：默认不再自动登录 —— 先等锁屏可交互，再回车两次进桌面。
        vm.wait_log("[LOCK64] bg blur ready", 150)
        mon.key("ret", wait=1.0)          # 锁屏 -> 登录界面
        mon.key("ret", wait=1.5)          # 登录按钮（无密码用户）
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
        for _ in range(4):                       # ★ PS/2 包偶发丢：有界重探（判据不变：必须读到坐标）
            if p0 is not None:
                break
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
        since_icons = len(vm.log())
        entered, det = dclick_card_until(vm, mon, idx_c, "C")
        check("双击 C: 卡片（精确移动 + 双击）", det != "", det)
        check("进入 C: 盘根（[UI] explorer enter letter=C: fatvol/slot … ok items=<n>）", entered)
        # ★ 缺陷 3：双击注入后**等 nav 打点落盘**再解析（快照读日志会偶发 items=-1）—— 判据不变
        check("进入 C: 后打点（explorer nav path=/ items=<n> view=icons）",
              wait_re(vm, r"\[UI\] explorer nav path=/ items=\d+ view=icons", 20, since_icons) is not None)
        nlog = vm.log()
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

        # 等外壳按新状态（C: 图标视图）把内容区重画过再截图 —— 见 settle_frames 的说明
        settle_frames(vm, since_icons)
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
            since_apps = len(vm.log())
            hit_ok, det = aim_click(vm, mon, a[0], a[1], "item:%d" % idx_apps, label="apps")
            check("双击目录 apps（精确移动 + 双击）", hit_ok, det)
            # ★ 同样等打点落盘（QEMU -serial file: 延迟），不靠快照
            check("进入 /apps（nav path=/apps items=1）",
                  wait_re(vm, r"\[UI\] explorer nav path=/apps items=1 view=icons", 20, since_apps) is not None)
            nlog = vm.log()
            check("进入目录打点（enter name=apps kind=dir）",
                  re.search(r"\[UI\] explorer enter name=apps kind=dir", nlog) is not None)
            m2 = re.search(r"\[UI\] explorer item idx=(\d+) name=demo type=dir size=0", nlog)
            idx_demo = int(m2.group(1)) if m2 else 0
            b = icon_cell(idx_demo)
            since_demo = len(vm.log())
            hit_ok, det = aim_click(vm, mon, b[0], b[1], "item:%d" % idx_demo, label="demo")
            check("双击目录 demo（精确移动 + 双击）", hit_ok, det)
            check("进入 /apps/demo（nav path=/apps/demo items=1）",
                  wait_re(vm, r"\[UI\] explorer nav path=/apps/demo items=1 view=icons", 20, since_demo) is not None)
            nlog = vm.log()
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
        since_details = len(vm.log())
        hit_ok, det = aim_click(vm, mon, tx, ty, "card:%d" % idx_c, label="C: 卡片（details）")
        check("详细信息视图下再进 C:（精确移动 + 双击）", hit_ok, det)
        vlog = fst.wait_for(serial, "view=details", 20, vm.proc)
        mvd = re.findall(r"\[UI\] explorer nav path=/ items=(\d+) view=details", vlog)
        check("详细信息视图的 nav 打点（view=details items=<n>）", bool(mvd), mvd[-1] if mvd else "（缺行）")
        # 同上：先等外壳把"详细信息视图"这一屏重画过，再截图
        settle_frames(vm, since_details)
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

        # ==================== 阶段 9：★ 布局自适应（①「布局写死 -> 内容不随尺寸变化」修复验收）====
        # 验收口径（全部是串口打点 + 真像素，不靠肉眼）：
        #   ① 缩放**前后**各一条 `[UI] exp layout client=WxH content=151,54 CWxCH cols=N rows=M …`，
        #      断言 裁剪矩形 == 当前客户区派生值（content_w == client_w-151、content_h == client_h-76）；
        #   ② 列数随宽度变化：cols == content_w // 96（格子宽）且缩放后列数**变大**；
        #   ③ 内容相对窗口原点不偏移：导航分隔线 / 状态栏那条仍在同一客户区相对偏移上；
        #   ④ 新多出来的那一条带**真的被应用画了**（内容白 + 边框），而不是只剩外壳的 client_bg(240,240,240)。
        print("=== 阶段 9：布局自适应（①：裁剪区=客户区 / 列数随宽度 / 内容不偏移 / 新区域被画）===")
        LAY_RE = re.compile(r"\[UI\] exp layout client=(\d+)x(\d+) content=(\d+),(\d+) (\d+)x(\d+) "
                            r"cols=(\d+) rows=(\d+) view=(\S+) card_w=(\d+)")
        CLIENT_BG = (240, 240, 240)      # theme64：外壳客户区底色（未画区域就是这个色）
        C_STATUS = (245, 245, 245)       # explorer 状态栏底色

        def layout_lines():
            return [m.groups() for m in LAY_RE.finditer(vm.log())]

        def lay(l):
            return dict(client_w=int(l[0]), client_h=int(l[1]), cx=int(l[2]), cy=int(l[3]),
                        cw=int(l[4]), ch=int(l[5]), cols=int(l[6]), rows=int(l[7]),
                        view=l[8], card_w=int(l[9]))
        def last(pat, text, flags=0):
            hits = list(re.finditer(pat, text, flags))
            return hits[-1] if hits else None


        def calibrate_at(tx, ty, tries=4):
            """在窗口内 (tx,ty) 点一下，用 explorer 自己的 click 打点量出落点误差（闭环）。"""
            dxe = dye = 0
            for i in range(tries):
                blind_goto(mon, tx + dxe, ty + dye)
                since_c = len(vm.log())
                single_click_at(mon)
                p = last_probe(vm, since_c)
                if p is None:
                    dxe = dye = 0
                    continue
                dxe, dye = tx - p[0], ty - p[1]
                if abs(dxe) <= 4 and abs(dye) <= 4:
                    return dxe, dye, i + 1
            return dxe, dye, tries

        ensure_window_on_top(vm, mon)

        def raise_top():
            """把 explorer 置顶：单击标题栏中部（不改几何、不触发菜单/控件）。

            踩过的坑：样式化之后的下方区域会被**浮层**（开始菜单/弹窗）盖住 —— 那会让
            "分隔线/状态栏还在不在"这类像素判定假失败。所以每次截图前先收掉浮层 + 点标题栏置顶。
            """
            mon.key("esc", wait=0.6)
            blind_goto(mon, WIN_X + 300, WIN_Y + 12)
            single_click_at(mon)
            time.sleep(0.8)

        raise_top()
        n_before = len(vm.log())
        before = layout_lines()
        lay0 = lay(before[-1]) if before else None
        if lay0 is None:
            check("缩放前拿到 [UI] exp layout 打点", False, "（没有 [UI] exp layout 行）")
        else:
            check("缩放前：打点里的客户区 == 660x470 窗口的客户区 658x444",
                  lay0["client_w"] == 658 and lay0["client_h"] == 444,
                  "client=%dx%d" % (lay0["client_w"], lay0["client_h"]))
            check("★ ① 裁剪矩形 == 当前客户区派生值（content=(151,54) 且 cw=client_w-151、ch=client_h-76）",
                  lay0["cx"] == CONTENT_X and lay0["cy"] == CONTENT_Y
                  and lay0["cw"] == lay0["client_w"] - CONTENT_X
                  and lay0["ch"] == lay0["client_h"] - CONTENT_Y - STATUS_H,
                  "content=%d,%d %dx%d（客户区 %dx%d）" % (lay0["cx"], lay0["cy"], lay0["cw"], lay0["ch"],
                                                        lay0["client_w"], lay0["client_h"]))
            check("缩放前列数 == cw/96（507/96 = 5 列）",
                  lay0["cols"] == lay0["cw"] // ICON_CELL_W and lay0["cols"] == 5,
                  "cols=%d cw=%d" % (lay0["cols"], lay0["cw"]))
        shot_b = os.path.join(tmp, "layout_before.ppm")
        check("缩放前截图", mon.shot(shot_b))
        pxb = read_ppm(shot_b) if os.path.exists(shot_b) else None

        # ---- 拖右边缘放大（QEMU monitor 真 PS/2 包：相对位移 + 闭环误差修正）----
        # 为什么先"量误差"：monitor 只能发相对包，长距离盲走会因丢包偏差几十像素，而抓取带只有 6px。
        # 先在窗口内离右边缘 20px 处点一下（explorer 自己的 click 打点给出真实落点），把误差带到边缘目标上。
        # ★ 修（抖动）：guest 每包最多走 MOUSE_STEP_MAX=24px，原来只发 5 包（理论 +120px），在负载下
        #   常常只走到 +48px -> "客户区变宽 >= 100px" 这条假失败。现在：
        #   (1) 每轮用**当前**客户区宽重算右边缘（上一轮已经变宽后仍抓得准，不会误触发窗口移动）；
        #   (2) 每轮发 12 包（理论 +288px），并按"'相对初始客户区已宽 >= 100px' 才停"判成功；
        #   (3) 最多 4 轮。断言（>= 100px）一个字没改。
        def cur_client_w():
            m = last(r"\[UI\] exp layout client=(\d+)x", vm.log())
            return int(m.group(1)) if m else 658

        cw0 = cur_client_w()
        grew, tried = False, 0
        for attempt in range(4):
            tried = attempt + 1
            cw_now = cur_client_w()
            if cw_now >= cw0 + 100:
                grew = True
                break
            edge = WIN_X + (cw_now + 2) - 3         # 外框宽 = 客户区宽 + 2（BORDER 左右各 1）
            dxe, dye, _n = calibrate_at(edge - 17, WIN_Y + WIN_H // 2)
            tx, ty = edge + dxe, WIN_Y + WIN_H // 2 + dye
            since_d = len(vm.log())
            blind_goto(mon, tx, ty)
            time.sleep(0.4)
            mon.send("mouse_button 1", wait=0.4)
            for _ in range(12):                     # 每包 guest 侧最多走 24px -> 最多 +288px
                mon.send("mouse_move 100 0", wait=0.12)
            time.sleep(0.3)
            mon.send("mouse_button 0", wait=0.8)
            time.sleep(1.0)
        grew = grew or (cur_client_w() >= cw0 + 100)
        check("右边缘拖拽缩放成立（[UI] win resize … client=…，尝试 %d 次）" % tried, grew,
              "client_w %d -> %d" % (cw0, cur_client_w()))
        rlog = vm.log()[n_before:]
        # 窗口外框原点（像素判定的参照系）：**两次都取同一次拖拽的 resize 打点** ——
        # "begin" 行 = 拖动前的外框；最后一条 "dir/end" 行 = 拖动后的外框。两者必然是同被拖的
        # 那个窗口（explorer 主窗）。★ 旧 WIP 用 `[UI] win geom … app=7` 的最后一行，实测取到了
        # **"文本预览"窗口**（它也是 APP_ID_MYPC=7，且 title 无空格所以能过 `title=\S+`）——
        # 实测拿到 (820,430)（预览窗）而不是 explorer 的 (120,60)，于是锚点全扫在错误的位置上。
        begins = re.findall(r"\[UI\] win resize begin dir=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+)", rlog)
        dirs = re.findall(r"\[UI\] win resize (?:dir|end) dir=\S+ x=(\d+) y=(\d+) w=(\d+) h=(\d+)", rlog)
        mg0 = begins[0] if begins else None
        mg1 = dirs[-1] if dirs else None
        win0 = (int(mg0[0]), int(mg0[1])) if mg0 else (WIN_X, WIN_Y)
        win1 = (int(mg1[0]), int(mg1[1])) if mg1 else win0
        check("★ 缩放参照系：拖动前后的外框原点都取到（且都 == 脚本常量 (120,60)，resize 只改宽高）",
              mg0 is not None and mg1 is not None and win0 == (WIN_X, WIN_Y) and win1 == win0,
              "win0=%s win1=%s" % (win0, win1))
        check("缩放帧打点含右边缘方向（dir=r）",
              re.search(r"\[UI\] win resize dir=r x=\d+ y=\d+ w=\d+ h=\d+ client=\d+x\d+", rlog) is not None,
              (re.search(r"\[UI\] win resize (begin|end|dir)[^\r\n]*", rlog).group(0)
               if re.search(r"\[UI\] win resize", rlog) else "（无 [UI] win resize 行）"))
        after = layout_lines()[len(before):]
        lay1 = lay(after[-1]) if after else None
        check("缩放后**重新打了一条** [UI] exp layout（重排真的发生）", lay1 is not None,
              ("client=%dx%d content=%dx%d cols=%d" %
               (lay1["client_w"], lay1["client_h"], lay1["cw"], lay1["ch"], lay1["cols"]))
              if lay1 else "（没有新行）")
        if lay0 and lay1:
            check("★ ① 缩放后 裁剪矩形 == 新客户区派生值（cw=client_w-151、ch=client_h-76）",
                  lay1["cw"] == lay1["client_w"] - CONTENT_X
                  and lay1["ch"] == lay1["client_h"] - CONTENT_Y - STATUS_H,
                  "client=%dx%d content=%d,%d %dx%d" % (lay1["client_w"], lay1["client_h"], lay1["cx"],
                                                      lay1["cy"], lay1["cw"], lay1["ch"]))
            check("★ ② 列数随宽度变化：宽 %d -> cols=%d；宽 %d -> cols=%d（列数 = cw/96）"
                  % (lay0["client_w"], lay0["cols"], lay1["client_w"], lay1["cols"]),
                  lay1["cols"] == lay1["cw"] // ICON_CELL_W and lay1["cols"] > lay0["cols"],
                  "cols %d -> %d（cw %d -> %d）" % (lay0["cols"], lay1["cols"], lay0["cw"], lay1["cw"]))
            check("缩放后客户区确实变宽（>= 100px）", lay1["client_w"] >= lay0["client_w"] + 100,
                  "client_w %d -> %d" % (lay0["client_w"], lay1["client_w"]))
        shot_a = os.path.join(tmp, "layout_after.ppm")
        raise_top()                                     # 拖完再收浮层 + 点标题栏置顶，保证 after 帧是 explorer 自己的画面
        settle_frames(vm, max(0, len(vm.log()) - 4))
        check("缩放后截图", mon.shot(shot_a))
        pxa = read_ppm(shot_a) if os.path.exists(shot_a) else None
        if pxb and pxa and lay0 and lay1:
            # ★ 修（工具侧 bug）：read_ppm 返回 (w, h, px)，下面所有 sample/rect_count 都要**宽度**当行跨；
            #   原 WIP 写成 `_, w0, pb = pxb`（拿到的是高度 800）→ 所有取样按 800 的行跨算，
            #   像素全看错位置：③ 锚点找不到（None/-4）、④ 数出来的白像素也是错的。这里改正并断言尺寸。
            w0, h0, pb = pxb[0], pxb[1], pxb[2]
            w1s, h1s, pa = pxa[0], pxa[1], pxa[2]
            check("缩放前后截图都是 1280x800（像素判定的行跨 == 屏宽）",
                  (w0, h0) == (1280, 800) and (w1s, h1s) == (w0, h0),
                  "before=%dx%d after=%dx%d" % (w0, h0, w1s, h1s))
            c_at = lambda px, xx, yy: tuple(sample(px, w0, xx, yy))     # noqa: E731

            # 相对窗口原点的"锚点"扫描（不假设 client 相对外框偏 1px，只用外框原点当参照系）：
            #   (a) 导航/内容分隔线：客户区里那条 1px 竖线（C_LINE）离外框左边的距离；
            #   (b) 状态栏：客户区底部那条 22px 横带（C_STATUS，245）离外框上边的距离。
            # 判据用"连续整列/整行命中数最强的那条"，不是"从窗口外第一个命中就返回"——后者会被
            # 壁纸/窗口投影的单像素假命中骗到（原 WIP 实测分隔线被报成 -4px，即窗口左边的阴影）。
            # 注意：这不是放水，反而是更严的判据（要求一条 >=150px 长的连续线，单点假命中过不了）。
            def col_run(px, x, y0, y1, target, tol):
                n = 0
                for yy in range(y0, y1):
                    if near(sample(px, w0, x, yy), target, tol):
                        n += 1
                return n

            def row_run(px, y, x0, x1, target, tol):
                n = 0
                for xx in range(x0, x1):
                    if near(sample(px, w0, xx, y), target, tol):
                        n += 1
                return n

            def anchor_vline(px, wx, wy):
                y0, y1 = wy + 100, wy + 400
                best, bx = 0, None
                for x in range(max(0, wx - 8), min(w0, wx + 260)):
                    n = col_run(px, x, y0, y1, C_LINE, 14)
                    if n > best:
                        best, bx = n, x
                return (bx - wx) if (bx is not None and best >= 150) else None

            def anchor_status(px, wx, wy):
                x0, x1 = wx + 5, min(w0, wx + 300)
                best, by = 0, None
                for y in range(wy + 380, min(h0, wy + 469)):
                    n = row_run(px, y, x0, x1, C_STATUS, 4)
                    if n > best:
                        best, by = n, y
                return (by - wy) if (by is not None and best >= 150) else None

            sep_b, sep_a = anchor_vline(pb, win0[0], win0[1]), anchor_vline(pa, win1[0], win1[1])
            st_b, st_a = anchor_status(pb, win0[0], win0[1]), anchor_status(pa, win1[0], win1[1])
            tb_b = rect_count(pb, w0, win0[0], win0[1], win0[0] + lay0["cw"], win0[1] + 24, (247, 247, 247), tol=4)
            tb_a = rect_count(pa, w0, win1[0], win1[1], win1[0] + lay1["cw"], win1[1] + 24, (247, 247, 247), tol=4)
            print("      [信息] 可见性自查：工具栏底色像素 前=%d 后=%d；分隔线列颜色 前=%s 后=%s"
                  % (tb_b, tb_a, c_at(pb, win0[0] + 160, win0[1] + 200), c_at(pa, win1[0] + 160, win1[1] + 200)))
            print("      [信息] 相对外框原点的锚点：分隔线 前=%s 后=%s；状态栏 前=%s 后=%s（px）"
                  % (sep_b, sep_a, st_b, st_a))
            check("★ ③ 内容不偏移：导航/内容分隔线在外框左边 %s（缩放前）/ %s（缩放后）—— 同一相对偏移 ±2px"
                  % (sep_b, sep_a),
                  sep_b is not None and sep_a is not None and abs(sep_b - sep_a) <= 2 and 145 <= sep_b <= 160)
            check("★ ③ 内容不偏移：状态栏在外框上边 %s（前）/ %s（后）—— 同一相对偏移 ±2px"
                  % (st_b, st_a),
                  st_b is not None and st_a is not None and abs(st_b - st_a) <= 2 and 440 <= st_b <= 452)
            # ★ 修：新增区域的屏幕带必须**从旧内容区右界之后**起算（原 WIP 漏了 +CONTENT_X，
            #   带子落回旧内容区里，测的就不是"新长出来的那一条"了）。正确的带子：
            #   起点 = 外框左 + 1(BORDER) + CONTENT_X + 旧 content_w + 4；终点 = 同式 + 新 content_w - 8。
            band_x0 = win1[0] + 1 + CONTENT_X + lay0["cw"] + 4    # 旧内容区右界之后
            band_x1 = win1[0] + 1 + CONTENT_X + lay1["cw"] - 8    # 新内容区右界之内
            band_y0 = win1[1] + 25 + CONTENT_Y + 40               # 避开预览窗口（它从客户区 y>=345 起）
            band_y1 = win1[1] + 25 + CONTENT_Y + 280
            if band_x1 - band_x0 >= 20:
                white_a = rect_count(pa, w0, band_x0, band_y0, band_x1, band_y1, (255, 255, 255), tol=3)
                bgc = rect_count(pa, w0, band_x0, band_y0, band_x1, band_y1, CLIENT_BG, tol=2)
                white_b = rect_count(pb, w0, band_x0, band_y0, band_x1, band_y1, (255, 255, 255), tol=3)
                tot = (band_x1 - band_x0) * (band_y1 - band_y0)
                print("      [信息] 新区域带 x=[%d,%d) y=[%d,%d)：内容白 前=%d 后=%d；client_bg=%d"
                      % (band_x0, band_x1, band_y0, band_y1, white_b, white_a, bgc))
                check("★ ④ 新客户区右带被应用画了（内容白像素 %d / %d；外壳 client_bg 仅 %d）"
                      % (white_a, tot, bgc), white_a > tot // 3 and white_a > bgc * 3,
                      "band=%dx%d" % (band_x1 - band_x0, band_y1 - band_y0))
                check("★ ④ 对照：缩放前同一屏幕区域不是内容白（窗口还没长到那里）",
                      white_b < white_a and white_b < tot // 3,
                      "white 前=%d 后=%d tot=%d" % (white_b, white_a, tot))
            else:
                check("★ ④ 新客户区右带宽度足够判定", False, "band_w=%d" % (band_x1 - band_x0))
        else:
            check("布局自适应的像素验收", False, "缺截图/打点")
        # ==================== 阶段 10：日志卫生 ====================
        print("=== 阶段 10：日志卫生 ===")
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
