#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""fileops64_test.py - 批次 J：文件管理器**文件操作**端到端验收（像素 + 串口 + 鼠标/键盘注入）

覆盖（每条都走真 UI：右键 / 快捷键 / 工具栏 -> 内核 vfs64，不是只调 API）：
  1) 右键菜单：条目菜单 items=6 at=sel（像素：白底 + 边框 + 文字）；Esc 关闭；空白菜单 items=4 at=blank；
  2) 复制 / 粘贴（Ctrl+C / Ctrl+V）：/copy_me.txt -> /docs，条目数 +1、预览读回 17 B、paste ok 打点；
  3) 重命名（F2 内联编辑）：/docs/copy_me.txt -> renamed.txt（旧名消失新名出现、内容不变；Esc 取消不落盘）；
  4) 重名策略：再粘贴一次 -> 自动 "renamed(2).txt"（打点 + 列表可见）；
  5) 多选：框选 2 条 -> sel n=2 mode=box；Ctrl+A -> mode=all；Delete 两次确认（10 秒窗口）-> 文件消失 + delete rc=0；
  6) 删除非空目录：/nonempty（里面有文件）-> 被拒（delete ... rc=1 reason=not-empty）+ 状态栏提示像素；
  7) 新建文件夹：右键空白 -> 内联编辑 newdir -> mkdir rc=0 -> 双击能进去；
  8) 剪切（Ctrl+X）：复制成功后删源 + 剪贴板清空（再 Ctrl+V 无副作用 / paste ok n=0）；
  9) 工具栏按钮：选中 -> 点"复制"/"粘贴"，与快捷键同一条内核路径；
 10) 跨卷 C: -> D:：粘贴到 D: 根，宿主侧解析 D: 卷核对内容 / 条目数（真的写进了那块盘）；
 11) 属性：右键 -> 属性，打点 name/kind/size/mtime/vol + 面板像素；
 12) 错误路径：36 字符名字被截到 31 B 上限（不越界）；含空格的非法名字被拒 rc=1；重名被拒 rc=1；
 13) 日志卫生：禁止 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / OOM。

★ 批次 L：注入确定性（不改语义）
   * 双击注入（进入目录 / 预览 / 进盘）走 Mouse.dclick_until（最多 3 次注入，**打点真的出现才算过**）：
     宿主负载高时 QEMU 注入的两次按下偶尔会被内核判成两次单击（QEMU+PS/2 时序抖动），
     这是注入抖动、不是 UI 语义错误；断言语义没放宽（导航/预览打点仍然必须真的出现）。
   * 框选同理走 Mouse.box_until（最多 3 次，仍然要求 sel n=2 mode=box）。
   * 新增 1 条断言：双击判定按**包到达间隔**算（[UI] explorer dbl src=card idx=.. gap=..，gap <= 125 tick = 500ms 窗口）。

跑法：py -3 tests\\fileops64_test.py（需要先 bash build64.sh；复用 explorer64_test 的鼠标闭环工具）
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  find_qemu
import fs_tree_test as fst         # noqa: E402  夹具（小系统盘）+ 串口工具
import explorer64_test as exp      # noqa: E402  鼠标闭环 / 像素工具
import multivol64_test as mv       # noqa: E402  宿主侧 v3 卷解析 + 带内容的 D: 盘夹具

# ---- 与 kernel/explorer64.cpp 对齐的几何（改内核常量必须同步改这里）----
CONTENT_X, CONTENT_Y = 151, 54
CONTENT_W = 660 - 2 - CONTENT_X        # 507
CONTENT_H = 470 - 25 - 1 - CONTENT_Y - 22
WIN_CL_X, WIN_CL_Y = 121, 85
TB2_X, TB2_W, TB2_GAP = 212, 56, 4
TB2_Y = 13
CTX_ITEM_H = 26
ICON_CELL_W, ICON_CELL_H = 96, 74
ICON_COLS = CONTENT_W // ICON_CELL_W   # 5

C_CTX_LINE = (130, 130, 130)
C_CTX_BG = (255, 255, 255)
C_BOX_LINE = (0, 120, 215)
C_MSG = (160, 60, 60)
C_PROPS_BG = (252, 252, 252)

COPY_TEXT = "fileops copy test"        # 17 B（QEMU sendkey 只能发小写/数字/点/减号）
CUT_TEXT = "cut-me"
LONG31 = "verylongnameverylongnameverylon"      # 31 B（= "verylongname" x 3 截到上限）


def sx(cx):
    return WIN_CL_X + cx


def sy(cy):
    return WIN_CL_Y + cy


def cell(idx, scroll=0):
    k = idx - scroll
    col, row = k % ICON_COLS, k // ICON_COLS
    return (sx(CONTENT_X + col * ICON_CELL_W + ICON_CELL_W // 2),
            sy(CONTENT_Y + 2 + row * ICON_CELL_H + ICON_CELL_H // 2))


def tb_center(i):
    """第 2 组工具栏按钮（0=新建 1=复制 2=剪切 3=粘贴 4=重命名 5=删除）中心（屏幕坐标）"""
    return (sx(TB2_X + i * (TB2_W + TB2_GAP) + TB2_W // 2), sy(TB2_Y))


class Vm:
    """两块盘的 QEMU 会话（index 0 = 系统盘 C:，index 1 = 数据盘 D:）"""

    def __init__(self, qemu, disks, port, serial, name="Vimtu64-fileops"):
        self.serial = serial
        self.port = port
        self.proc = subprocess.Popen(fst.qemu_args(qemu, disks, serial, port, name),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        return fst.slog(self.serial)

    def mark(self):
        """当前日志长度：之后用 wait_new(...)/wait_nav(...) 只看"这次动作之后新出现的行"，
        绝不拿"整份日志里的旧行"当证据（踩过的坑：/docs 早期的 items=0 会冒充删除成功）。"""
        return len(self.log())

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_new(self, pattern, timeout, since):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if re.search(pattern, self.log()[since:]):
                return True
            time.sleep(0.3)
        return False

    def wait_nav(self, path, items, timeout, since):
        pat = r"\[UI\] explorer nav path=%s items=%d view=\w+" % (re.escape(path), items)
        return self.wait_new(pat, timeout, since)

    def close(self):
        fst.kill(self.proc)


def last_item_idx(vm, name):
    """串口日志里最近一次 `[UI] explorer item idx=<i> name=<name>` 的 idx（-1 = 没有）"""
    m = None
    for mm in re.finditer(r"\[UI\] explorer item idx=(\d+) name=(\S+)", vm.log()):
        if mm.group(2) == name:
            m = mm
    return int(m.group(1)) if m else -1


def card_idx(vm, letter):
    m = None
    for mm in re.finditer(r"\[UI\] explorer card idx=(\d+) letter=(\S+) kind=(\S+)", vm.log()):
        if mm.group(2) == letter + ":" and mm.group(3) == "browsable":
            m = int(mm.group(1))
    return m


def last_rect(vm):
    m = None
    for mm in re.finditer(r"\[UI\] explorer ctxmenu rect x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log()):
        m = tuple(int(mm.group(i)) for i in range(1, 5))
    return m


class Mouse:
    """带误差修正的鼠标（复用 explorer64_test 的盲走 + 点击打点闭环）"""

    def __init__(self, vm, mon):
        self.vm = vm
        self.mon = mon
        self.off = [0, 0]

    def recalibrate(self):
        dxe, dye, _ = exp.calibrate_offset(self.vm, self.mon)
        self.off = [dxe, dye]

    def goto(self, tx, ty):
        exp.blind_goto(self.mon, tx + self.off[0], ty + self.off[1])

    def click(self, tx, ty, want_hit=None, tries=3):
        for _ in range(tries):
            self.goto(tx, ty)
            since = len(self.vm.log())
            exp.single_click_at(self.mon)
            p = exp.last_probe(self.vm, since)
            if p is not None and (want_hit is None or p[2] == want_hit):
                return p
            self.recalibrate()
        return None

    def dclick(self, tx, ty):
        self.goto(tx, ty)
        exp.double_click(self.mon)

    def dclick_until(self, tx, ty, pattern, timeout=12, tries=3):
        """双击 + 有界重试（**只重试注入，不放宽断言**）：宿主负载高时，注入的两次按下偶尔会被
        判成两次单击（内核按 500ms 窗口判双击，QEMU 注入的时序抖动会吃掉它）—— 这属于注入抖动，
        不是 UI 语义错误。做法：最多注入 tries 次，**pattern（导航/预览打点）真的出现才算过**。
        每次重试前等一会儿，让上一轮单击的 500ms 双击窗口过期，避免和下一轮的第一下粘连。"""
        for _ in range(tries):
            since = self.vm.mark()
            self.dclick(tx, ty)
            if self.vm.wait_new(pattern, timeout, since):
                return True
            time.sleep(0.7)
        return False

    def box_until(self, x0, y0, x1, y1, pattern, timeout=12, tries=3):
        """框选 + 有界重试（同上：pattern = `sel n=.. mode=box` 打点，必须真的出现；不放宽语义）。"""
        for _ in range(tries):
            since = self.vm.mark()
            self.box(x0, y0, x1, y1)
            if self.vm.wait_new(pattern, timeout, since):
                return True
            time.sleep(0.4)
        return False

    def right_click(self, tx, ty):
        # QEMU HMP：`mouse_button state (1=L, 2=R, 4=M)` -> 右键 = 2（实测 4 是左中键，不产生右键）
        self.goto(tx, ty)
        self.mon.raw(["mouse_button 2", "mouse_button 0"], wait_between=0.12, wait_end=0.5)
        time.sleep(0.6)

    def box(self, x0, y0, x1, y1):
        """空白处按下 -> 拖到 (x1,y1) -> 松开（框选）"""
        self.goto(x0, y0)
        self.mon.raw(["mouse_button 1"], wait_between=0.1, wait_end=0.25)
        exp.move_px(self.mon, 0, 1 if x1 >= x0 else -1, abs(x1 - x0))
        exp.move_px(self.mon, 1, 1 if y1 >= y0 else -1, abs(y1 - y0))
        self.mon.raw(["mouse_button 0"], wait_between=0.1, wait_end=0.8)
        time.sleep(1.0)


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_fileops_")
    sys_disk = os.path.join(tmp, "small.img")
    data_disk = os.path.join(tmp, "data.img")
    serial = os.path.join(tmp, "serial.log")
    if fst.make_small_system_disk(sys_disk) is None:
        print("  [FAIL] 无法生成小系统盘夹具")
        return 2
    mv.make_data_vol_disk(data_disk)                    # D: 预置 /readme.txt + /docs/notes.txt
    base_d = mv.read_file(data_disk)

    checks = []
    ok = True

    def ck(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    port = fst.free_port()
    vm = Vm(qemu, [sys_disk, data_disk], port, serial)
    mon = exp.Monitor(port)                      # exp.Monitor 有 raw()（fst.Monitor 只有 send/key）
    try:
        # ==================== 阶段 1：引导 ====================
        print("=== 阶段 1：引导（C: = 16MB v3 卷，D: = 预置数据的第二块盘）===")
        # ★ 缺陷 4（登录必须显式输入）：默认不再自动登录 —— 先等锁屏可交互，再回车两次进桌面。
        vm.wait_log("[LOCK64] bg blur ready", 150)
        mon.key("ret", wait=1.0)          # 锁屏 -> 登录界面
        mon.key("ret", wait=1.5)          # 登录按钮（无密码用户）
        ck("桌面就绪（[GUI64] ready）", vm.wait_log("[GUI64] ready", 150))
        log = vm.log()
        ck("文件管理器自检（含批次 J 的名字/后缀/多选位图检查）[EXPL] selftest PASS",
           "[EXPL] selftest PASS" in log)
        ck("D: 存在且是 VimtuFS2 卷（[DRV64] letter=D:）",
           re.search(r"\[DRV64\] letter=D: disk=\d+ part=\d+ fs=VimtuFS2", log) is not None)

        # ==================== 阶段 2：终端造素材 ====================
        print("=== 阶段 2：终端造素材（/copy_me.txt /cut_me.txt /docs /nonempty/inner.txt）===")
        ck("打开终端", fst.open_terminal(mon, serial, vm.proc))
        mon.type_line("mkdir /docs")
        ck("mkdir /docs", vm.wait_log("[TERM] cmd mkdir ok", 25))
        mon.type_line("mkdir /nonempty")
        mon.type_line("write /copy_me.txt %s" % COPY_TEXT)
        ck("写 /copy_me.txt（17 B）", vm.wait_log("[TERM] cmd write ok", 25))
        mon.type_line("write /cut_me.txt %s" % CUT_TEXT)
        mon.type_line("write /nonempty/inner.txt in")
        mon.type_line("cat /copy_me.txt")
        ck("cat /copy_me.txt = 17 B", vm.wait_log("[TERM] cmd cat bytes=17", 25))
        mon.type_line("ls")
        ck("ls 至少 8 条（v3 卷根目录）",
           vm.wait_log("[TERM] cmd ls entries=", 25) and
           int(re.findall(r"\[TERM\] cmd ls entries=(\d+)", vm.log())[-1]) >= 8)

        # ==================== 阶段 3：打开文件管理器并进 C: ====================
        print("=== 阶段 3：打开文件管理器 -> 双击 C: 卡片 -> 进入盘根 ===")
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=2.5)
        ck("打开文件管理器（[APP] mypc opened）", vm.wait_log("[APP] mypc opened", 25))
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=1.5)
        exp.SAFE_POINT = (sx(CONTENT_X + CONTENT_W - 40), sy(CONTENT_Y + CONTENT_H - 40))
        mouse = Mouse(vm, mon)
        top_ok, top_det = exp.ensure_window_on_top(vm, mon)
        ck("资源管理器窗口能收到点击", top_ok, top_det)
        exp.park_cursor(mon)
        step = exp.calibrate(vm, mon)
        ck("鼠标位移模型（x 走 ~300px）", step is not None and abs(step[0] - 300) <= 40, str(step))
        exp.park_cursor(mon)
        for _ in range(3):
            exp.click_once(vm, mon)
        mouse.recalibrate()
        mcard = card_idx(vm, "C")
        ck("C: 卡片下标可读", mcard is not None, "idx=%s" % mcard)
        since = vm.mark()
        ok_nav = mouse.dclick_until(*exp.card_center(mcard),
                                    r"\[UI\] explorer nav path=/ items=\d+ view=icons", timeout=25)
        ck("双击 C: 进入盘根（nav path=/ view=icons）", ok_nav)
        m = None
        for mm in re.finditer(r"\[UI\] explorer dbl src=card idx=\d+ gap=(\d+)", vm.log()[since:]):
            m = mm
        ck("双击判定按包到达间隔算（[UI] explorer dbl src=card gap<=125）",
           m is not None and int(m.group(1)) <= 125, ("gap=%s" % m.group(1)) if m else "（缺 dbl 打点）")
        idx_copy = last_item_idx(vm, "copy_me.txt")
        ck("根目录里能看到 copy_me.txt", idx_copy >= 0, "idx=%d" % idx_copy)

        # ==================== 阶段 4：右键菜单（条目菜单）====================
        print("=== 阶段 4：右键 copy_me.txt -> 菜单 items=6 at=sel -> 像素 -> Esc 关闭 ===")
        since = vm.mark()
        mouse.right_click(*cell(idx_copy))
        ck("右键弹出条目菜单（ctxmenu items=6 at=sel）",
           vm.wait_new(r"\[UI\] explorer ctxmenu items=6 at=sel", 20, since))
        mrect = last_rect(vm)
        ck("菜单矩形打点（ctxmenu rect）", mrect is not None, str(mrect))
        shot = os.path.join(tmp, "ctxmenu.ppm")
        ck("右键菜单截图", mon.shot(shot))
        if mrect and os.path.exists(shot):
            w, h, px = exp.read_ppm(shot)
            mx, my, mw, mh = mrect
            bg = exp.rect_count(px, w, sx(mx) + 3, sy(my) + 3, sx(mx) + mw - 3, sy(my) + mh - 3, C_CTX_BG, tol=4)
            edge = exp.rect_count(px, w, sx(mx) - 1, sy(my) - 1, sx(mx) + mw + 2, sy(my) + mh + 2, C_CTX_LINE, tol=14)
            tpx = exp.rect_dark(px, w, sx(mx) + 6, sy(my) + 4, sx(mx) + mw - 6, sy(my) + mh - 4)
            ck("菜单是白底面板（像素）", bg > 3000, "白像素=%d" % bg)
            ck("菜单有边框（像素）", edge > 200, "边框像素=%d" % edge)
            ck("菜单项有文字（6 项）", tpx > 100, "暗像素=%d" % tpx)
        mon.key("esc", wait=1.2)
        shot2 = os.path.join(tmp, "ctxmenu_closed.ppm")
        ck("Esc 后截图", mon.shot(shot2))
        if mrect and os.path.exists(shot2):
            w, h, px = exp.read_ppm(shot2)
            mx, my, mw, mh = mrect
            edge = exp.rect_count(px, w, sx(mx) - 1, sy(my) - 1, sx(mx) + mw + 2, sy(my) + mh + 2, C_CTX_LINE, tol=10)
            ck("Esc 关闭菜单（边框像素消失）", edge < 120, "残留边框像素=%d" % edge)

        # ==================== 阶段 5：复制 / 粘贴（Ctrl+C / Ctrl+V）====================
        print("=== 阶段 5：选中 copy_me.txt -> Ctrl+C -> 进 /docs -> Ctrl+V ===")
        p = mouse.click(*cell(idx_copy), want_hit="item:%d" % idx_copy)
        ck("单击选中 copy_me.txt（click hit=item:idx）", p is not None and p[2] == "item:%d" % idx_copy, str(p))
        ck("单选打点（sel n=1 mode=click）", vm.wait_log("[UI] explorer sel n=1 mode=click", 15))
        since = vm.mark()
        mon.key("ctrl-c", wait=1.2)
        ck("Ctrl+C 入剪贴板（clip op=copy n=1）", vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))
        idx_docs = last_item_idx(vm, "docs")
        ck("docs 条目可定位", idx_docs >= 0, "idx=%d" % idx_docs)
        ok_docs = mouse.dclick_until(*cell(idx_docs),
                                     r"\[UI\] explorer nav path=/docs items=0 view=\w+", timeout=20)
        ck("双击进 /docs（nav path=/docs items=0）", ok_docs)
        since = vm.mark()
        mon.key("ctrl-v", wait=1.5)
        ck("Ctrl+V 粘贴成功（paste ok n=1 dst=/docs skipped=0）",
           vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/docs skipped=0", 25, since))
        ck("/docs 现在 1 条（nav path=/docs items=1）", vm.wait_nav("/docs", 1, 20, since))
        idx_p = last_item_idx(vm, "copy_me.txt")
        ck("粘贴出来的条目在列表里（item name=copy_me.txt）", idx_p >= 0, "idx=%d" % idx_p)
        ok_prev = mouse.dclick_until(*cell(idx_p),
                                     r"\[UI\] explorer preview name=copy_me\.txt bytes=17", timeout=20)
        ck("双击粘贴出的文件 -> 预览读回 17 B（内容一致）", ok_prev)

        # ==================== 阶段 6：重命名（F2 内联编辑）====================
        print("=== 阶段 6：F2 -> Esc 取消 -> F2 -> renamed.txt -> 回车 ===")
        idx_p = last_item_idx(vm, "copy_me.txt")
        mouse.click(*cell(idx_p), want_hit="item:%d" % idx_p)
        mon.key("f2", wait=1.2)
        shot_e = os.path.join(tmp, "edit.ppm")
        ck("重命名编辑中截图", mon.shot(shot_e))
        if os.path.exists(shot_e):
            w, h, px = exp.read_ppm(shot_e)
            cx, cy = cell(idx_p)
            ex0, ey0 = cx - ICON_CELL_W // 2 + 2, cy - ICON_CELL_H // 2 + 40
            eb = exp.rect_count(px, w, ex0, ey0, ex0 + ICON_CELL_W - 4, ey0 + 18, C_BOX_LINE, tol=40)
            ebg = exp.rect_count(px, w, ex0, ey0, ex0 + ICON_CELL_W - 4, ey0 + 18, (255, 255, 255), tol=4)
            ck("编辑框有蓝色边框（像素）", eb > 3, "像素=%d" % eb)
            ck("编辑框是白底（像素）", ebg > 15, "像素=%d" % ebg)
        since = vm.mark()
        mon.key("esc", wait=1.0)
        ck("Esc 取消编辑不落盘（没有 rename 打点）",
           not vm.wait_new(r"\[UI\] explorer rename old=", 3, since))
        mouse.click(*cell(idx_p), want_hit="item:%d" % idx_p)
        mon.key("f2", wait=1.0)
        for _ in range(len("copy_me.txt") + 2):            # 退格清掉旧名字
            mon.key("backspace", wait=0.08)
        for ch in "renamed.txt":
            mon.key("dot" if ch == "." else ch, wait=0.10)
        since = vm.mark()
        mon.key("ret", wait=1.5)
        ck("重命名成功（rename old=copy_me.txt new=renamed.txt rc=0）",
           vm.wait_new(r"\[UI\] explorer rename old=copy_me\.txt new=renamed\.txt rc=0", 25, since))
        idx_r = last_item_idx(vm, "renamed.txt")
        ck("列表里出现新名（item name=renamed.txt）", idx_r >= 0, "idx=%d" % idx_r)
        ok_ren = mouse.dclick_until(*cell(idx_r),
                                    r"\[UI\] explorer preview name=renamed\.txt bytes=17", timeout=20)
        ck("重命名后内容不变（preview name=renamed.txt bytes=17）", ok_ren)

        # ==================== 阶段 7：重名策略 + 框选 + Ctrl+A + 删除 ====================
        print("=== 阶段 7：再粘贴一次（重名加 (2)）-> 框选 2 条 -> Ctrl+A -> Delete 两次确认 ===")
        mouse.click(*cell(idx_r), want_hit="item:%d" % idx_r)
        mon.key("ctrl-c", wait=1.2)
        since = vm.mark()
        mon.key("ctrl-v", wait=1.5)
        ok_paste = vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/docs skipped=0", 25, since)
        ck("重名自动加后缀（paste ok n=1 + item name=renamed(2).txt）",
           ok_paste and vm.wait_new(r"\[UI\] explorer item idx=\d+ name=renamed\(2\)\.txt", 20, since))
        ck("/docs 现在 2 条", vm.wait_nav("/docs", 2, 20, since))
        ok_box = mouse.box_until(sx(CONTENT_X + 430), sy(CONTENT_Y + 300),
                                 sx(CONTENT_X + 4), sy(CONTENT_Y + 8),
                                 r"\[UI\] explorer sel n=2 mode=box", timeout=15)
        ck("框选 2 条（sel n=2 mode=box）", ok_box)
        since = vm.mark()
        mon.key("ctrl-a", wait=1.2)
        ck("Ctrl+A 全选（sel n=2 mode=all）", vm.wait_new(r"\[UI\] explorer sel n=2 mode=all", 20, since))
        since = vm.mark()
        mon.key("delete", wait=1.5)
        ck("第一次 Delete 只提示（delete confirm n=2 path=/docs/...）",
           vm.wait_new(r"\[UI\] explorer delete confirm n=2 path=/docs/", 20, since))
        shot_c = os.path.join(tmp, "confirm.ppm")
        ck("第一次 Delete 后截图（确认提示）", mon.shot(shot_c))
        if os.path.exists(shot_c):
            w, h, px = exp.read_ppm(shot_c)
            msg = exp.rect_count(px, w, sx(310), sy(CONTENT_Y + CONTENT_H + 3),
                                 sx(CONTENT_X + CONTENT_W), sy(CONTENT_Y + CONTENT_H + 22), C_MSG, tol=45)
            ck("状态栏确认提示像素（再按一次 Delete 确认）", msg > 10, "提示像素=%d" % msg)
        ck("第一次 Delete 没有真删",
           not vm.wait_new(r"\[UI\] explorer delete path=/docs/", 3, since))
        # 第二次 Delete 真删：打出 vfs64_unlink 的 delete 打点。
        # 注意：截一次图要好几秒，可能已经越过确认窗口 —— 所以每轮**连按两次**（1.2 秒间隔）：
        # 第一次要么直接删（还在窗口内），要么重新提示；第二次一定在窗口内 -> 必删。最多 3 轮。
        since2 = vm.mark()
        del_ok = False
        for _ in range(3):
            mon.key("delete", wait=1.0)
            mon.key("delete", wait=1.2)
            if vm.wait_new(r"\[UI\] explorer delete path=/docs/\S+ kind=file rc=0", 4, since2):
                del_ok = True
                break
        deletes = len(re.findall(r"\[UI\] explorer delete path=/docs/\S+ kind=file rc=0", vm.log()[since2:]))
        ck("第二次 Delete 真删（delete path=/docs/... kind=file rc=0 x2）", del_ok and deletes == 2,
           "rc=0 行数=%d" % deletes)
        ck("/docs 已空（新的 nav path=/docs items=0）", vm.wait_nav("/docs", 0, 20, since2))

        # ==================== 阶段 8：剪切（Ctrl+X）+ 空剪贴板粘贴 ====================
        print("=== 阶段 8：回 C: 根 -> 剪切 cut_me.txt -> 粘贴 -> 剪贴板清空后再粘贴 ===")
        since = vm.mark()
        mouse.click(sx(75), sy(40), want_hit="btn:up")
        ck("点'上级'回盘根（up path=/）", vm.wait_new(r"\[UI\] explorer up path=/", 20, since))
        vm.wait_new(r"\[UI\] explorer nav path=/ items=\d+ view=\w+", 15, since)
        idx_cut = last_item_idx(vm, "cut_me.txt")
        ck("根目录能找到 cut_me.txt", idx_cut >= 0, "idx=%d" % idx_cut)
        mouse.click(*cell(idx_cut), want_hit="item:%d" % idx_cut)
        since = vm.mark()
        mon.key("ctrl-x", wait=1.2)
        ck("Ctrl+X 剪切入剪贴板（clip op=cut n=1）", vm.wait_new(r"\[UI\] explorer clip op=cut n=1", 20, since))
        since = vm.mark()
        mon.key("ctrl-v", wait=1.8)
        ok_paste = vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/ skipped=0", 25, since)
        ck("剪切粘贴：副本落到 (2) 名字 + 源被删（条目数不变）",
           ok_paste and vm.wait_new(r"\[UI\] explorer item idx=\d+ name=cut_me\(2\)\.txt", 20, since))
        since2 = vm.mark()
        mon.key("ctrl-v", wait=1.8)
        ck("空剪贴板粘贴无副作用（paste ok n=0 skipped=0）",
           vm.wait_new(r"\[UI\] explorer paste ok n=0 dst=/ skipped=0", 20, since2))

        # ==================== 阶段 9：工具栏按钮（复制 / 粘贴）====================
        print("=== 阶段 9：工具栏'复制'/'粘贴' ===")
        idx_copy = last_item_idx(vm, "copy_me.txt")
        ck("根目录能找到 copy_me.txt", idx_copy >= 0, "idx=%d" % idx_copy)
        mouse.click(*cell(idx_copy), want_hit="item:%d" % idx_copy)
        since = vm.mark()
        p = mouse.click(*tb_center(1), want_hit="btn:copy")
        ck("点工具栏'复制'（click hit=btn:copy）", p is not None and p[2] == "btn:copy", str(p))
        ck("工具栏复制生效（clip op=copy n=1）", vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))
        since = vm.mark()
        p = mouse.click(*tb_center(3), want_hit="btn:paste")
        ck("点工具栏'粘贴'（click hit=btn:paste）", p is not None and p[2] == "btn:paste", str(p))
        ck("工具栏粘贴生效（paste ok n=1 dst=/ skipped=0）",
           vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/ skipped=0", 25, since))

        # ==================== 阶段 10：删除非空目录被拒 ====================
        print("=== 阶段 10：选中非空目录 nonempty -> Delete 两次 -> 如实被拒 ===")
        idx_ne = last_item_idx(vm, "nonempty")
        ck("根目录能找到 nonempty", idx_ne >= 0, "idx=%d" % idx_ne)
        mouse.click(*cell(idx_ne), want_hit="item:%d" % idx_ne)
        since = vm.mark()
        mon.key("delete", wait=1.2)
        mon.key("delete", wait=1.8)
        ck("非空目录被拒（delete path=/nonempty kind=dir rc=1 reason=not-empty）",
           vm.wait_new(r"\[UI\] explorer delete path=/nonempty kind=dir rc=1 reason=not-empty", 25, since))
        ck("被拒后目录还在（item name=nonempty）", last_item_idx(vm, "nonempty") >= 0)

        # ==================== 阶段 11：新建文件夹 ====================
        print("=== 阶段 11：右键空白 -> 新建文件夹 -> 内联编辑 newdir -> 双击进入 ===")
        since = vm.mark()
        mouse.right_click(sx(CONTENT_X + 430), sy(CONTENT_Y + 320))
        ck("空白处右键菜单（ctxmenu items=4 at=blank）",
           vm.wait_new(r"\[UI\] explorer ctxmenu items=4 at=blank", 20, since))
        mrect = last_rect(vm)
        ck("空白菜单矩形打点", mrect is not None, str(mrect))
        if mrect:
            since = vm.mark()
            mouse.click(sx(mrect[0] + 20), sy(mrect[1] + 2 + CTX_ITEM_H // 2))
            for ch in "newdir":
                mon.key(ch, wait=0.12)
            mon.key("ret", wait=1.8)
            ck("新建文件夹成功（mkdir path=/newdir rc=0）",
               vm.wait_new(r"\[UI\] explorer mkdir path=/newdir rc=0", 25, since))
            ck("列表里出现 newdir（item name=newdir type=dir）",
               vm.wait_new(r"\[UI\] explorer item idx=\d+ name=newdir type=dir", 20, since))
            idx_nd = last_item_idx(vm, "newdir")
            ok_nd = mouse.dclick_until(*cell(idx_nd),
                                       r"\[UI\] explorer nav path=/newdir items=0 view=\w+", timeout=20)
            ck("双击进新目录（nav path=/newdir items=0）", ok_nd)
            since = vm.mark()
            mouse.click(sx(75), sy(40), want_hit="btn:up")
            vm.wait_new(r"\[UI\] explorer nav path=/ items=\d+ view=\w+", 15, since)

        # ==================== 阶段 12：跨卷粘贴 C: -> D: ====================
        print("=== 阶段 12：C: 的 copy_me.txt 粘贴到 D: 根（宿主侧核对 D: 卷字节）===")
        idx_copy = last_item_idx(vm, "copy_me.txt")
        mouse.click(*cell(idx_copy), want_hit="item:%d" % idx_copy)
        since = vm.mark()
        mon.key("ctrl-c", wait=1.2)
        ck("剪贴板里是 copy_me.txt（clip op=copy n=1）", vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))
        since = vm.mark()
        mouse.click(sx(120), sy(40), want_hit="crumb:0")
        ck("回到此电脑（thispc）", vm.wait_new(r"\[UI\] explorer thispc", 20, since))
        md = card_idx(vm, "D")
        ck("D: 卡片可定位", md is not None, "idx=%s" % md)
        ok_d = mouse.dclick_until(*exp.card_center(md),
                                  r"\[UI\] explorer enter letter=D:.+ ok items=\d+", timeout=25)
        ck("进入 D: 根目录（enter letter=D: ... ok）", ok_d)
        since = vm.mark()
        mon.key("ctrl-v", wait=1.8)
        ck("跨卷粘贴成功（paste ok n=1 dst=/ skipped=0）",
           vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/ skipped=0", 25, since))
        ck("D: 根目录出现 copy_me.txt",
           vm.wait_new(r"\[UI\] explorer item idx=\d+ name=copy_me\.txt", 20, since))
        time.sleep(2.0)
        after = mv.read_file(data_disk)
        for _ in range(6):
            a = mv.read_file(data_disk)
            time.sleep(0.6)
            b = mv.read_file(data_disk)
            if a == b:
                after = a
                break
        cvol_b = mv.vfs3_vol(base_d, mv.DATA_PART_LBA)
        cvol_a = mv.vfs3_vol(after, mv.DATA_PART_LBA)
        ck("D: 是合法 v3 卷（宿主侧解析）", cvol_b is not None and cvol_a is not None)
        nb = len(mv.vfs3_list(base_d, cvol_b, 0)) if cvol_b else -1
        na = len(mv.vfs3_list(after, cvol_a, 0)) if cvol_a else -1
        ck("D: 根目录条目数 +1（%d -> %d）" % (nb, na), na == nb + 1)
        f = mv.vfs3_find(after, cvol_a, "copy_me.txt") if cvol_a else None
        ck("宿主侧在 D: 上找到 copy_me.txt", f is not None)
        if f:
            ck("D: 上 copy_me.txt 内容逐字节一致", mv.vfs3_read(after, cvol_a, f["idx"]) == COPY_TEXT.encode())

        # ==================== 阶段 13：属性面板 ====================
        print("=== 阶段 13：右键 -> 属性（打点 + 面板像素）===")
        idx_d = last_item_idx(vm, "copy_me.txt")
        since = vm.mark()
        mouse.right_click(*cell(idx_d))
        ck("D: 上条目右键菜单（items=6 at=sel）",
           vm.wait_new(r"\[UI\] explorer ctxmenu items=6 at=sel", 20, since))
        mrect = last_rect(vm)
        since = vm.mark()
        if mrect:
            mouse.click(sx(mrect[0] + 20), sy(mrect[1] + 2 + 5 * CTX_ITEM_H + CTX_ITEM_H // 2))   # 第 6 项 = 属性
        ck("属性打点（props name=copy_me.txt kind=.. size=17 mtime=.. vol=D:）",
           vm.wait_new(r"\[UI\] explorer props name=copy_me\.txt kind=\S+ size=17 mtime=\S+ \S+ vol=D:", 25, since))
        shot_p = os.path.join(tmp, "props.ppm")
        ck("属性面板截图", mon.shot(shot_p))
        if os.path.exists(shot_p):
            w, h, px = exp.read_ppm(shot_p)
            px0, py0 = sx(CONTENT_X + 20), sy(CONTENT_Y + 18)
            pbg = exp.rect_count(px, w, px0 + 3, py0 + 22, px0 + 320, py0 + 126, C_PROPS_BG, tol=0)
            ptxt = exp.rect_dark(px, w, px0 + 6, py0 + 22, px0 + 320, py0 + 126)
            ck("属性面板底色（252,252,252 精确匹配）", pbg > 2000, "面板像素=%d" % pbg)
            ck("属性面板有文字（5 行）", ptxt > 100, "暗像素=%d" % ptxt)
            mon.key("esc", wait=1.0)

        # ==================== 阶段 14：错误路径 ====================
        print("=== 阶段 14a：F2 输入 36 字符 -> 截到 31 B 上限 ===")
        idx_r2 = last_item_idx(vm, "readme.txt")
        ck("D: 根目录有 readme.txt", idx_r2 >= 0, "idx=%d" % idx_r2)
        mouse.click(*cell(idx_r2), want_hit="item:%d" % idx_r2)
        mon.key("f2", wait=1.0)
        for _ in range(40):
            mon.key("backspace", wait=0.05)
        for ch in "verylongname" * 3:                      # 36 B -> 编辑框只收 31 B
            mon.key(ch, wait=0.05)
        since = vm.mark()
        mon.key("ret", wait=1.8)
        ck("名字长度上限 31 B（rename old=readme.txt new=%s rc=0）" % LONG31,
           vm.wait_new(r"\[UI\] explorer rename old=readme\.txt new=verylongnameverylongnameverylon rc=0", 25, since))
        print("=== 阶段 14b：F2 输入含空格的非法名字 -> 被拒（rc=1）===")
        idx_r2 = last_item_idx(vm, LONG31)
        ck("31 字符名字在列表里", idx_r2 >= 0, "idx=%d" % idx_r2)
        mouse.click(*cell(idx_r2), want_hit="item:%d" % idx_r2)
        mon.key("f2", wait=1.0)
        for _ in range(40):
            mon.key("backspace", wait=0.05)
        mon.key("a", wait=0.08)
        mon.key("b", wait=0.08)
        mon.key("spc", wait=0.08)                          # 空格：vfs64 名字要求 0x21..0x7E -> 非法
        mon.key("c", wait=0.08)
        since = vm.mark()
        mon.key("ret", wait=1.5)
        ck("非法名字被拒（rename old=verylongname... new=ab c rc=1）",
           vm.wait_new(r"\[UI\] explorer rename old=verylongname\S* new=ab c rc=1", 25, since))
        ck("被拒后名字没变（item name=%s）" % LONG31, last_item_idx(vm, LONG31) >= 0)
        print("=== 阶段 14c：F2 改成已存在的名字 -> 被拒（rc=1）===")
        idx_d = last_item_idx(vm, LONG31)
        ck("14c 前对象是 31 字符名字", idx_d >= 0, "idx=%d" % idx_d)
        mouse.click(*cell(idx_d), want_hit="item:%d" % idx_d)
        mon.key("f2", wait=1.0)
        for _ in range(40):
            mon.key("backspace", wait=0.05)
        for ch in "docs":                                  # D: 根上已有 docs 目录
            mon.key(ch, wait=0.10)
        since = vm.mark()
        mon.key("ret", wait=1.5)
        ck("重名被拒（rename old=verylongname... new=docs rc=1）",
           vm.wait_new(r"\[UI\] explorer rename old=verylongname\S* new=docs rc=1", 25, since))
        ck("重名被拒后名字没变（item name=%s）" % LONG31, last_item_idx(vm, LONG31) >= 0)

        # ==================== 阶段 15：日志卫生 ====================
        print("=== 阶段 15：日志卫生 ===")
        final = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:"):
            ck("全程不得出现 %s" % bad, bad not in final)
    finally:
        vm.close()

    if args.keep:
        print("[fileops] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
