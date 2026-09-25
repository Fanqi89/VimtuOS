#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/locklogin64_test.py - P1c 验收：锁屏 + 登录界面 + 多用户骨架（串口 + 像素 + 键鼠注入）

覆盖（每一条都有串口打点 / 像素证据，不能只看"没崩"）：
  1) 冷启动顺序：开机滚屏 -> 开机动画 -> `[LOCK64] lock screen shown` ->（桌面**还没**出现）
     锁屏像素：时间字形墨高 64–80px、年月日 16–20px、背景**清晰**（局部方差 ≈ 壁纸原图，blur=0）
  2) 回车/点击 -> `[LOGIN64] login screen shown blur=20` -> 背景方差显著下降（真 20px 模糊）
     -> 无密码用户点登录按钮/回车 -> `[LOGIN64] login ok user=... uid=...` -> 桌面出现
  3) 有密码用户：弹 360×48 磨砂密码框 + 右侧 48×48 白底「确认」按钮（几何 + 像素）
     输错 -> `[LOGIN64] password wrong`（不进桌面）；输对 -> 进桌面
  4) ESC：登录 -> 锁屏，动画可测（`[LOGIN64] blur anim dir=out` 的 v 在 250–350ms 窗口内 20 -> 0）
  5) 软重启后用同一块盘冷启动：仍进锁屏；登录界面**只有普通用户、没有 root**
  6) 多用户数据：useradd 建 alice/bob -> 各自 /home/<u>/Desktop 放不同文件 -> 登录 alice 时
     桌面文件列表（[USER64] desktop list）只看到自己的（最小语义；严格权限属 P4）
  7) 终端身份：`su - root` / `sudo -i` / `su -` -> 提示符/whoami 变 root（euid=0），
     **GUI 用户名/头像不变**（打点 gui_unchanged=1）；`exit` 退回普通用户
  8) 持久化：`passwd` 设的口令跨重启仍能登录；用户表 /etc/users.db 跨重启存在
  9) 回归：日志里不得出现 PANIC / TRIPLE FAULT / selftest FAIL / FAILED mask= / OOM

用法：py -3 tests\\locklogin64_test.py [--qemu 路径] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import fs_tree_test as fst        # noqa: E402  （Python 侧 v3 卷夹具 + QEMU 启动工具）
import proc64_test as p64         # noqa: E402  （find_qemu）

FORBIDDEN = ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:")
# 与 kernel/locklogin64.h 的数字一致（验收按**像素**量，不只看打点）
TIME_INK_MIN, TIME_INK_MAX = 64, 80
DATE_INK_MIN, DATE_INK_MAX = 16, 20
PW_W, PW_H, PW_BTN = 360, 48, 48
BLUR_PX = 20
SENS = 1.7                         # kernel/input.cpp：鼠标灵敏度 ×1.7（dx*17/10）
# 与 kernel/locklogin64.cpp 的 geom64() 一致的几何（1280x800）
PANEL = (280, 230, 720, 240)
AVATAR = (640, 190, 104)           # 圆心 + 直径
BTN = (592, 364, 96, 96)           # 登录按钮
PW_BOX = (432, 506, 360, 48)       # 密码框
PW_BTN_BOX = (800, 506, 48, 48)    # 确认按钮
SMALL_Y, SMALL_D, SMALL_GAP, SMALL_X0 = 294, 44, 18, 556
TILE = (340, 724, 32, 32)          # 背景清晰/模糊探针（与 gfx64 的 blur tile 同位置）
TILE_LOCK_VAR_MIN = 20.0           # 内置壁纸有细颗粒：清晰时应远高于 20

KEYS_PLAIN = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "=": "equal", ",": "comma",
              ";": "semicolon", "'": "apostrophe", "\\": "backslash", "[": "bracket_left",
              "]": "bracket_right", "`": "grave_accent"}
KEYS_SHIFT = {"_": "shift-minus", ">": "shift-dot", "%": "shift-5", ":": "shift-semicolon",
              "+": "shift-equal", "*": "shift-8", "<": "shift-comma", "!": "shift-1"}


class Vm:
    """一个 QEMU 实例（装好的盘 + telnet monitor + 串口文件）。"""

    def __init__(self, qemu, disks, serial, port, name):
        self.serial = serial
        self.port = port
        self.proc, self.mon = fst.boot_installed(qemu, disks, serial, port, name)
        # 软件光标初值：kernel/input.cpp mouse_init() 写死 (512,384)
        self.cursor = [512, 384]
    def log(self):
        return fst.slog(self.serial)

    def wait(self, needle, timeout):
        deadline = time.time() + timeout
        while True:
            s = self.log()
            if needle in s:
                return s
            if time.time() > deadline or self.proc.poll() is not None:
                return None
            time.sleep(0.3)

    def wait_new(self, needle, timeout, since_count):
        deadline = time.time() + timeout
        while True:
            s = self.log()
            if s.count(needle) > since_count:
                return s
            if time.time() > deadline or self.proc.poll() is not None:
                return None
            time.sleep(0.3)

    def key(self, name, wait=0.5):
        self.mon.key(name, wait=wait)

    def click(self, wait=0.5):
        """在当前光标位置点一次（按键状态 1=左键按下，0=抬起）。"""
        self.mon.send("mouse_button 1", wait=0.2)
        self.mon.send("mouse_button 0", wait=wait)

    def move_to(self, x, y):
        """把软件光标挪到 (x,y)。HMP 的 mouse_move 是**相对**位移；内核 input.cpp 会按
        灵敏度 ×1.7（dx*17/10）累加、且每个包最多走 24px（MOUSE_STEP_MAX），所以这里
        也按 ×1.7 折算着发小步（实测标定：发 40 -> 走 24；发 10 -> 走 17）。
        初值 (512,384) 见 input.cpp mouse_init()。"""
        tx, ty = x - self.cursor[0], y - self.cursor[1]      # 还差的像素
        guard = 0
        while (abs(tx) > 2 or abs(ty) > 2) and guard < 40:
            guard += 1
            sx = int(max(-12, min(12, round(tx / SENS))))
            sy = int(max(-12, min(12, round(ty / SENS))))
            if sx == 0 and sy == 0:
                break
            self.mon.send("mouse_move %d %d" % (sx, sy), wait=0.12)
            tx -= int(sx * SENS)                             # 按实测倍率扣账
            ty -= int(sy * SENS)
        self.cursor = [x - tx, y - ty]

    def click_login_button(self, tries=3):
        """把光标挪进 96×96 登录按钮矩形（592..688, 364..460）再点；返回是否出现点击打点。
        按钮中心 (640,412)；万一某一击被丢包，后续几次仍在矩形内轻微错开重试。"""
        for i in range(tries):
            n = self.log().count("[LOGIN64] login button key=click")
            self.move_to(640 + i * 12, 412 + (i % 2) * 8)
            self.click(wait=0.4)
            if self.wait_new("[LOGIN64] login button key=click", 3, n):
                return True
        return False

    def type_line(self, text, per_key=0.13):
        for ch in text:
            if ch in KEYS_PLAIN:
                name = KEYS_PLAIN[ch]
            elif ch in KEYS_SHIFT:
                name = KEYS_SHIFT[ch]
            elif ch.isalpha() and ch.isupper():
                name = "shift-%s" % ch.lower()
            elif ch.isalnum():
                name = ch
            else:
                raise ValueError("unsupported char: %r" % ch)
            self.key(name, wait=per_key)
        self.key("ret", wait=per_key + 0.2)

    def shot(self, path):
        if os.path.exists(path):
            os.remove(path)
        self.mon.send("screendump %s" % p64.q(path), wait=0.3)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return path
            time.sleep(0.1)
        return None

    def kill(self):
        fst.kill(self.proc)


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM: %r" % raw[:16])
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


def lum(c):
    return (c[0] * 299 + c[1] * 587 + c[2] * 114) // 1000


def tile_var(px, w, x0, y0, tw=32, th=32, ch=1):
    """局部方差（与 kernel/gfx64.cpp 的 gfx64_var64 同口径：单通道，放大 100 倍）。"""
    vals = []
    for y in range(y0, y0 + th):
        for x in range(x0, x0 + tw):
            vals.append(sample(px, w, x, y)[ch])
    n = len(vals)
    mean = sum(vals) / float(n)
    return (sum((v - mean) ** 2 for v in vals) / float(n)) * 100.0


def ink_rows(px, w, x0, y0, bw, bh, ref_lum=None):
    """数"墨迹行"：一行里至少 2 个像素明显暗于**面板底色**（ref_lum）就算有字。

    为什么不用"该行中位数"当基准：数字底部的横杠能占满整行（>50%），中位数本身就变暗了
    （实测踩过：72px 的字量出来只有 60px）。这里固定用面板空白处的亮度当基准。
    返回 (行数, 首行, 末行)。
    """
    if ref_lum is None:
        band = [lum(sample(px, w, x, y0 - 8)) for x in range(x0, x0 + bw)]
        ref_lum = sorted(band)[len(band) // 2]
    flags = []
    for y in range(y0, y0 + bh):
        row = [lum(sample(px, w, x, y)) for x in range(x0, x0 + bw)]
        flags.append(sum(1 for c in row if ref_lum - c >= 60) >= 2)
    idx = [i for i, v in enumerate(flags) if v]
    if not idx:
        return 0, -1, -1
    return idx[-1] - idx[0] + 1, y0 + idx[0], y0 + idx[-1]


def count_dark(px, w, x0, y0, bw, bh, max_lum=130):
    n = 0
    for y in range(y0, y0 + bh):
        for x in range(x0, x0 + bw):
            if lum(sample(px, w, x, y)) < max_lum:
                n += 1
    return n


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_locklogin_")
    disk = os.path.join(tmp, "small.img")
    info = fst.make_small_system_disk(disk)       # 同一块盘：第一遍改状态、第二遍验持久化
    if info is None:
        sys.stderr.write("无法生成小系统盘夹具（build64/system.img 缺失或过大）\n")
        return 2
    serial1 = os.path.join(tmp, "boot1.log")
    serial2 = os.path.join(tmp, "boot2.log")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond), detail))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def forbid(tag, log):
        for bad in FORBIDDEN:
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    def last(pattern, log, default=None):
        hits = re.findall(pattern, log)
        return hits[-1] if hits else default

    # ==================================================================
    # 第一遍：冷启动 -> 锁屏 -> 登录 -> 桌面 -> 多用户/身份 -> 回锁屏 -> 口令
    # ==================================================================
    print("=== 第一遍：冷启动（锁屏 -> 登录 -> 桌面 -> 多用户 -> su/sudo -> 回锁屏 -> 口令）===")
    vm = Vm(qemu, [disk], serial1, fst.free_port(), "Vimtu64-locklogin1")
    lv = 0.0
    try:
        log = vm.wait("[LOCK64] lock screen shown", 150)
        check("开机后先出现锁屏（[LOCK64] lock screen shown）", log is not None)
        if log is None:
            print("  锁屏没出现：后续断言全部跳过")
        else:
            # ---- 1) 冷启动顺序 + 打点 ----
            check("锁屏出现时桌面还没起来（锁屏那一行之前没有 [GUI64] ready）",
                  "[GUI64] ready" not in log[:log.find("[LOCK64] lock screen shown")],
                  "lock@%d" % log.find("[LOCK64] lock screen shown"))
            check("开机滚屏 -> 开机动画 -> 锁屏（[UI] boot logo show 在锁屏前）",
                  "[UI] boot logo show" in log and
                  log.find("[UI] boot logo show") < log.find("[LOCK64] lock screen shown"))
            # 锁屏首帧画完才有 [LOCK64] lock text 行：等它再取截图/盒子（首帧画在 "lock screen shown" 之后）
            vm.wait("[LOCK64] lock text", 60)
            log = vm.log()
            check("锁屏背景标记 blur_bg=0（清晰，不做模糊）",
                  bool(re.search(r"\[LOCK64\] lock screen shown .*blur_bg=0", log)))
            check("锁屏壁纸模式打点（lock_mode / desktop_mode / applied）",
                  bool(re.search(r"\[LOCK64\] wall src=.+? lock_mode=(\d+) desktop_mode=(\d+) applied=(\d+) screen=\d+x\d+", log)),
                  (re.search(r"\[LOCK64\] wall[^\r\n]*", log) or re.search(r"$^", log)).group(0))
            check("locklogin64 纯逻辑自检 PASS（数字区间/星期换算/用户过滤）",
                  bool(re.search(r"\[LOCK64\] selftest PASS mask=0", log)))
            m = last(r"\[LOCK64\] lock text time_ink=(\d+)px time_box=(\d+),(\d+),(\d+),(\d+) "
                     r"date_ink=(\d+)px date_box=(\d+),(\d+),(\d+),(\d+) panel_box=", log)
            check("锁屏打点给出时间/年月日的墨高与盒子", m is not None, str(m))
            check("锁屏大号时间墨高打点 = 72px（需求 64–80）", m is not None and m[0] == "72", str(m))
            check("锁屏年月日墨高打点 = 18px（需求 16–20）", m is not None and m[5] == "18", str(m))
            check("亚克力面板几何 = 720x240 @ (280,230) 居中偏上",
                  bool(re.search(r"panel_box=280,230,720,240", log)))
            # ---- 锁屏像素 + 点击路径 ----
            # 顺序讲究（实测踩过）：
            #   1) 锁屏循环真正跑起来的标志是 "[LOCK64] bg blur ready"（之前的绘制/模糊阶段几秒内
            #      注入的**鼠标**事件会被丢掉；键盘队列能留，鼠标包留不住）；
            #   2) 先截图（此刻画面就是锁屏），再立刻注入点击，最后才做耗时的 Python 像素分析 ——
            #      因为锁屏空闲 8 秒会自动登录（ui.login.auto），分析放在点击之后就不会被抢。
            vm.wait("[LOCK64] bg blur ready", 90)
            shot = vm.shot(os.path.join(tmp, "lock.ppm"))
            lv = 0.0
            w, h, px = (0, 0, b"")
            n_login = vm.log().count("[LOGIN64] login screen shown")
            vm.click()
            log = vm.wait_new("[LOGIN64] login screen shown", 30, n_login)
            check("鼠标点击锁屏进入登录界面（[LOGIN64] login screen shown blur=20 anim=..ms）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] login screen shown blur=20 anim=\d+ms", log)),
                  (re.search(r"\[LOGIN64\] login screen shown[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            check("登录界面的过渡由点击触发（why=click）", bool(log) and "why=click" in log)
            if not shot:
                check("锁屏截图", False, "screendump 失败")
            else:
                w, h, px = read_ppm(shot)
                check("分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
                if m:
                    ink, r0, r1 = ink_rows(px, w, int(m[1]), int(m[2]), int(m[3]), int(m[4]))
                    check("锁屏时间字形高度按像素量在 64–80px",
                          TIME_INK_MIN <= ink <= TIME_INK_MAX, "实测 %dpx（行 %d..%d）" % (ink, r0, r1))
                    dink, dr0, dr1 = ink_rows(px, w, int(m[6]), int(m[7]), int(m[8]), int(m[9]))
                    check("锁屏年月日字形高度按像素量在 16–20px",
                          DATE_INK_MIN <= dink <= DATE_INK_MAX, "实测 %dpx（行 %d..%d）" % (dink, dr0, dr1))
                lv = tile_var(px, w, TILE[0], TILE[1], TILE[2], TILE[3])
                gv = last(r"\[LOCK64\] bg tile x=340 y=724 wh=32x32 var=(\d+)", log)
                check("锁屏背景保持清晰（像素方差 %.1f > 20，打点 var=%s，blur=0）" % (lv, gv),
                      lv > TILE_LOCK_VAR_MIN and gv is not None and float(gv) > TILE_LOCK_VAR_MIN)
                pv = tile_var(px, w, PANEL[0] + 20, PANEL[1] + 14, 32, 24)
                check("时间后面的白色亚克力面板糊掉了壁纸颗粒（面板内方差 < 1.0）",
                      pv < 1.0, "面板内方差 %.2f（面板外 %.1f）" % (pv, lv))
            # ---- 2) 登录界面：模糊 20px + 点登录按钮 ----
            # 同样先注入（1 秒内），再做像素分析：登录界面空闲 8 秒也会自动登录
            mdl = last(r"\[LOGIN64\] blur tile x=(\d+) y=(\d+) wh=32x32 r=(\d+) var_src=(\d+) var_dst=(\d+) ms=(\d+)",
                       vm.log())
            check("登录背景模糊半径 r=20，方差从 var_src 掉到 var_dst（真模糊）",
                  mdl is not None and mdl[2] == "20" and float(mdl[3]) > 0 and float(mdl[4]) < float(mdl[3]) * 0.5,
                  str(mdl))
            time.sleep(1.2)                       # 等淡入动画走完（300ms 逻辑时长 + 渲染时间）
            n_okv = vm.log().count("[LOGIN64] login ok user=vimtu")
            shot2 = vm.shot(os.path.join(tmp, "login.ppm"))
            check("鼠标点击 96×96 登录按钮（[LOGIN64] login button key=click）",
                  vm.click_login_button())      # 光标初值 (512,384) -> 按钮中心 (640,412)（×1.7 灵敏度已折算）
            log = vm.wait_new("[LOGIN64] login ok user=vimtu uid=1000 via=click", 30, n_okv)
            check("无密码用户：点击 96×96 登录按钮直接进桌面"
                  "（[LOGIN64] login ok user=vimtu uid=1000 via=click）",
                  log is not None,
                  (re.search(r"\[LOGIN64\] login ok[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            check("桌面出现（[GUI64] ready 在 login ok 之后）", vm.wait("[GUI64] ready", 30) is not None)
            check("GUI 身份写入用户表（[USER64] login ok name=vimtu ... gui=vimtu euid=1000）",
                  bool(log) and bool(re.search(r"\[USER64\] login ok name=vimtu uid=1000 home=/home/vimtu "
                                               r"desktop=/home/vimtu/Desktop gui=vimtu euid=1000", log)))
            check("用户表落盘（[USER64] userdb save ok path=/etc/users.db）",
                  bool(log) and "[USER64] userdb save ok path=/etc/users.db" in log)
            # 登录界面的像素证据（这时才做，Python 分析慢，不影响客人）
            if shot2:
                w2, h2, px2 = read_ppm(shot2)
                if (w2, h2) == (1280, 800):
                    lv2 = tile_var(px2, w2, TILE[0], TILE[1], TILE[2], TILE[3])
                    check("登录界面同一 tile 的像素方差显著下降（< 清晰时的一半）",
                          lv2 < max(1.0, lv) * 0.5, "清晰 %.1f -> 模糊 %.1f（打点 var_dst=%s）"
                          % (lv, lv2, mdl[4] if mdl else "?"))
                    av = tile_var(px2, w2, AVATAR[0] - 12, AVATAR[1] - 12, 24, 24)
                    check("登录界面圆形头像区域画了东西（局部方差 > 5）", av > 5.0, "方差 %.2f" % av)
                    bv = tile_var(px2, w2, BTN[0], BTN[1] + 40, 24, 24)
                    check("登录按钮（96×96 渐变）区域画了东西（局部方差 > 5）", bv > 5.0, "方差 %.2f" % bv)

            # ---- 6/7) 多用户 + 会话身份（终端）----
            check("打开终端（开始菜单 -> 终端）", fst.open_terminal(vm.mon, vm.serial, vm.proc))
            time.sleep(0.6)
            # ★ 关掉自动登录：后面几次锁屏（loginctl lock / ESC / 口令流程）都要**只等用户输入**，
            #   否则 8 秒空闲会自动进桌面，跟验收注入的键/鼠标抢（内核每秒重读该配置）。
            #   注意用 `KEY=VALUE` 写法：`cfg set KEY VALUE` 那种写法在既有实现里会把 KEY 也带进值里。
            n = vm.log().count("[CONF64] set key=ui.login.auto")
            vm.type_line("cfg set ui.login.auto=0")
            vm.wait_new("[CONF64] set key=ui.login.auto", 25, n)
            check("终端关闭自动登录（cfg set ui.login.auto=0）——后续锁屏只等用户输入",
                  bool(re.search(r"\[CONF64\] set key=ui\.login\.auto value=0", vm.log())))
            n_add = vm.log().count("[USER64] useradd ok")
            vm.type_line("useradd alice")
            vm.type_line("useradd bob")
            log = vm.wait_new("[USER64] useradd ok", 25, n_add + 1)      # 第 2 条 ok
            check("useradd 建普通用户 alice / bob（uid >= 1000，home=/home/<名>，desktop=<home>/Desktop）",
                  bool(log) and bool(re.search(r"\[USER64\] useradd ok name=alice uid=\d+ home=/home/alice "
                                               r"desktop=/home/alice/Desktop", log)) and
                  bool(re.search(r"\[USER64\] useradd ok name=bob uid=\d+ home=/home/bob "
                                 r"desktop=/home/bob/Desktop", log)))
            n = vm.log().count("[USER64] users ")
            vm.type_line("users")
            log = vm.wait_new("[USER64] users ", 25, n)
            check("users 列表标出 root 与普通用户（[USER64] users count=4 root=1 normal=3）",
                  bool(log) and bool(re.search(r"\[USER64\] users count=4 root=1 normal=3", log)),
                  (re.search(r"\[USER64\] users[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            n = vm.log().count("[USER64] passwd ok user=alice set=1")
            vm.type_line("passwd alice s3cret-pw")
            log = vm.wait_new("[USER64] passwd ok user=alice set=1", 25, n)
            check("passwd 设口令：加盐 SHA-256 + 1000 轮（[USER64] passwd ok user=alice set=1 algo=sha256 iter=1000）",
                  bool(log) and bool(re.search(r"\[USER64\] passwd ok user=alice set=1 algo=sha256 iter=1000", log)))
            check("明文口令绝不进串口（日志里搜不到 s3cret-pw）", "s3cret-pw" not in vm.log())
            n = vm.log().count("[TERM] cmd write ok")
            vm.type_line("write /home/alice/Desktop/aa.txt hello-alice")
            vm.type_line("write /home/bob/Desktop/bb.txt hello-bob")
            log = vm.wait_new("[TERM] cmd write ok", 25, n + 1)
            check("各自桌面上写文件成功（[TERM] cmd write ok x2）",
                  bool(log) and vm.log().count("[TERM] cmd write ok") >= 2,
                  "write ok=%d" % vm.log().count("[TERM] cmd write ok"))
            n = vm.log().count("[TERM] cmd cat ok")
            vm.type_line("cat /home/alice/Desktop/aa.txt")
            log = vm.wait_new("[TERM] cmd cat ok", 25, n)
            check("alice 的桌面文件真的在盘上（[TERM] cmd cat bytes=<n> total=<n> truncated=0）",
                  bool(log) and bool(re.search(r"\[TERM\] cmd cat bytes=(\d+) total=(\d+) truncated=0", log)) and
                  bool(re.search(r"\[TERM\] cmd cat bytes=[1-9]", log)),
                  (re.search(r"\[TERM\] cmd cat[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            # ---- whoami / su / sudo / exit ----
            n = vm.log().count("[USER64] whoami")
            vm.type_line("whoami")
            log = vm.wait_new("[USER64] whoami", 20, n)
            check("whoami = 普通用户（[USER64] whoami user=vimtu euid=1000 root_session=0）",
                  bool(log) and bool(re.search(r"\[USER64\] whoami user=vimtu euid=1000 gui=vimtu root_session=0", log)))
            n = vm.log().count("[USER64] su ok")
            vm.type_line("su - root")
            log = vm.wait_new("[USER64] su ok", 20, n)
            check("su - root：会话 euid 变 0，GUI 身份不变（gui_unchanged=1）",
                  bool(log) and bool(re.search(r"\[USER64\] su ok from=vimtu to=root euid=0 gui=vimtu gui_unchanged=1 via=su-dash", log)))
            check("打点如实写明权限位尚未拦截（P4）", "permission bits not enforced yet; P4" in vm.log())
            n = vm.log().count("[USER64] whoami")
            vm.type_line("whoami")
            log = vm.wait_new("[USER64] whoami", 20, n)
            check("root 会话下 whoami = root（euid=0）",
                  bool(log) and bool(re.search(r"\[USER64\] whoami user=root euid=0 gui=vimtu root_session=1", log)))
            n = vm.log().count("[USER64] exit ok")
            vm.type_line("exit")
            log = vm.wait_new("[USER64] exit ok", 20, n)
            check("exit 退回普通用户（[USER64] exit ok from=root to=vimtu euid=1000）",
                  bool(log) and bool(re.search(r"\[USER64\] exit ok from=root to=vimtu euid=1000", log)))
            n = vm.log().count("[USER64] whoami")
            vm.type_line("whoami")
            log = vm.wait_new("[USER64] whoami", 20, n)
            check("exit 之后 whoami 又是普通用户",
                  bool(log) and bool(re.search(r"\[USER64\] whoami user=vimtu euid=1000", log)))
            n = vm.log().count("[USER64] su ok")
            vm.type_line("sudo -i")
            log = vm.wait_new("[USER64] su ok", 20, n)
            check("sudo -i：会话身份变 root（via=sudo-i）",
                  bool(log) and bool(re.search(r"\[USER64\] su ok from=vimtu to=root euid=0 gui=vimtu gui_unchanged=1 via=sudo-i", log)))
            n = vm.log().count("[USER64] exit ok")
            vm.type_line("exit")
            vm.wait_new("[USER64] exit ok", 20, n)
            n = vm.log().count("[USER64] su ok")
            vm.type_line("su -")
            log = vm.wait_new("[USER64] su ok", 20, n)
            check("su -：会话身份也变 root（via=su-dash 出现第 2 次）",
                  bool(log) and vm.log().count("via=su-dash") >= 2)
            n = vm.log().count("[USER64] exit ok")
            vm.type_line("exit")
            log = vm.wait_new("[USER64] exit ok", 20, n)
            check("三次 exit 都退回普通用户", bool(log) and vm.log().count("[USER64] exit ok from=root to=vimtu") >= 3,
                  "exit ok=%d" % vm.log().count("[USER64] exit ok from=root to=vimtu"))

            # ---- 4) loginctl lock -> 锁屏（又清晰）----
            n_lock = vm.log().count("[LOCK64] lock screen shown")
            vm.type_line("loginctl lock")
            log = vm.wait_new("[LOCK64] lock screen shown", 25, n_lock)
            check("终端 `loginctl lock` 回到锁屏（[LOCK64] lock requested by=terminal + 新的 lock screen shown）",
                  bool(log) and "[LOCK64] lock requested by=terminal" in log,
                  (re.search(r"\[LOCK64\] lock requested[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            shot3 = vm.shot(os.path.join(tmp, "lock2.ppm"))
            if shot3:
                w3, h3, px3 = read_ppm(shot3)
                lv3 = tile_var(px3, w3, TILE[0], TILE[1], TILE[2], TILE[3])
                check("回到锁屏后背景又清晰了（tile 方差 > 20；blur 20 -> 0）", lv3 > TILE_LOCK_VAR_MIN,
                      "方差 %.1f" % lv3)
            # ---- 登录界面：只有普通用户；方向键选 alice；ESC 反向动画 ----
            vm.key("ret", wait=1.0)
            n_login = vm.log().count("[LOGIN64] login screen shown")
            vm.wait_new("[LOGIN64] login screen shown", 25, n_login - 1 if n_login else 0)
            log = vm.wait("[LOGIN64] userlist", 20)
            check("登录界面只列普通用户（[LOGIN64] userlist n=3 names=... root=hidden=1）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] userlist n=3 names=[^ ]* root=hidden=1", log)),
                  (re.search(r"\[LOGIN64\] userlist[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            check("登录界面的用户列表里没有 root",
                  bool(log) and not re.search(r"userlist n=\d+ names=([^\s]*)\broot\b", log))
            # 反向动画：先按 ESC 回锁屏（20 -> 0，250–350ms），再回来点/选用户
            n_anim = vm.log().count("[LOGIN64] blur anim dir=out")
            vm.key("esc", wait=0.8)
            log = vm.wait("[LOGIN64] lock screen restored (ESC)", 20)
            check("ESC 从登录界面回到锁屏（[LOGIN64] lock screen restored (ESC)）", log is not None)
            deadline = time.time() + 10
            while time.time() < deadline:
                if vm.log().count("[LOGIN64] blur anim dir=out") > n_anim:
                    break
                time.sleep(0.3)
            steps = re.findall(r"\[LOGIN64\] blur anim dir=out t=(\d+)ms v=(\d+)/\d+", vm.log())
            check("反向动画有采样（[LOGIN64] blur anim dir=out ...）", len(steps) >= 2, "steps=%d" % len(steps))
            if steps:
                v0, v1, t_end = int(steps[0][1]), int(steps[-1][1]), int(steps[-1][0])
                check("反向动画：模糊值 %d -> %d，末次采样 t=%dms 落在 250–350ms 窗口" % (v0, v1, t_end),
                      v0 == BLUR_PX and v1 == 0 and 250 <= t_end <= 350, "steps=%s" % (steps,))
                vs = [int(s[1]) for s in steps]
                check("反向动画单调不增（不闪切）", all(vs[i] >= vs[i + 1] for i in range(len(vs) - 1)), str(vs))

            # ---- 3) 有口令用户：密码框 + 输错/输对 ----
            vm.key("ret", wait=1.0)                     # 锁屏 -> 登录
            time.sleep(0.8)
            n_sel = vm.log().count("[LOGIN64] user select")
            vm.key("right", wait=0.8)                   # 方向键：vimtu -> alice（确定性切换）
            log = vm.wait_new("[LOGIN64] user select", 15, n_sel)
            check("方向键切换选中用户（[LOGIN64] user select ... via=arrow）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] user select idx=\d+ user=alice[^\r\n]*via=arrow", log)),
                  (re.search(r"\[LOGIN64\] user select[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            # （alice 是否需要口令，看下面"回车弹密码框"那一行打点）
            n_pw = vm.log().count("[LOGIN64] password prompt")
            vm.key("ret", wait=1.0)
            log = vm.wait_new("[LOGIN64] password prompt", 20, n_pw)
            check("回车弹出密码框（[LOGIN64] password prompt user=alice box=360x48 btn=48x48）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] password prompt user=alice box=360x48 btn=48x48", log)))
            shot4 = vm.shot(os.path.join(tmp, "pw.ppm"))
            if shot4:
                w4, h4, px4 = read_ppm(shot4)
                dark = count_dark(px4, w4, PW_BTN_BOX[0], PW_BTN_BOX[1], PW_BTN, PW_BTN, 130)
                check("右侧 48×48 确认按钮里有黑字「确认」（暗像素 > 10）", dark > 10, "暗像素=%d" % dark)
                bright = 0
                for y in range(PW_BTN_BOX[1], PW_BTN_BOX[1] + PW_BTN):
                    for x in range(PW_BTN_BOX[0], PW_BTN_BOX[0] + PW_BTN):
                        if lum(sample(px4, w4, x, y)) > 190:
                            bright += 1
                check("确认按钮是白底（亮像素 > 800/2304）", bright > 800, "亮像素=%d" % bright)
                box_lum = lum(sample(px4, w4, PW_BOX[0] + 30, PW_BOX[1] + PW_H // 2))
                above_lum = lum(sample(px4, w4, PW_BOX[0] + 30, PW_BOX[1] - 14))
                check("密码框 360×48 是磨砂面板（框内/框上亮度可见差异）",
                      abs(box_lum - above_lum) >= 2 or box_lum > 200,
                      "框内 %d / 框上 %d" % (box_lum, above_lum))
            n_wrong = vm.log().count("[LOGIN64] password wrong")
            vm.type_line("wrong-pw")
            log = vm.wait_new("[LOGIN64] password wrong", 25, n_wrong)
            check("输错口令：明确失败打点（[LOGIN64] password wrong）", log is not None,
                  (re.search(r"\[LOGIN64\] password wrong[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            check("输错后仍在登录界面（没有新的 login ok）", vm.log().count("[LOGIN64] login ok") == 1,
                  "login ok=%d" % vm.log().count("[LOGIN64] login ok"))
            n_ok = vm.log().count("[LOGIN64] login ok user=alice")   # 必须与下面的 needle **同一个**计数
            vm.type_line("s3cret-pw")
            log = vm.wait_new("[LOGIN64] login ok user=alice", 25, n_ok)
            check("输对口令后进桌面（[LOGIN64] login ok user=alice uid=1001）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] login ok user=alice uid=\d+", log)))
            check("alice 登录后加载自己的桌面（[USER64] login ok name=alice home=/home/alice "
                  "desktop=/home/alice/Desktop）",
                  bool(log) and bool(re.search(r"\[USER64\] login ok name=alice uid=\d+ home=/home/alice "
                                               r"desktop=/home/alice/Desktop", log)))
            check("桌面文件列表只有 alice 自己的 aa.txt（看不到 bob 的 bb.txt）",
                  bool(log) and bool(re.search(r"\[USER64\] desktop list user=alice dir=/home/alice/Desktop "
                                               r"entries=1 names=aa\.txt", log)) and
                  not re.search(r"desktop list user=alice [^\r\n]*bb\.txt", log),
                  (re.search(r"\[USER64\] desktop list[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
            check("GUI 身份随登录用户变（login ok name=alice ... gui=alice euid=1001）",
                  bool(log) and bool(re.search(r"login ok name=alice [^\r\n]*gui=alice euid=\d+", log)))
            entered_alice = bool(log)

            # ---- 8) 交互式 passwd（隐藏输入）+ 软重启（重启后验持久化）----
            if not entered_alice:
                print("  [!] alice 没登录成功：后面 交互式 passwd / 重启 断言会失真，这里如实记一条 FAIL")
                check("alice 口令流程走通（前置条件）", False, "见上面的 FAIL")
            else:
                check("重新打开/激活终端", fst.open_terminal(vm.mon, vm.serial, vm.proc))
                time.sleep(0.8)
                n_set = vm.log().count("[USER64] passwd ok user=alice set=1")
                n_pterm = vm.log().count("[TERM] cmd passwd ok")
                vm.type_line("passwd alice", per_key=0.2)
                # 等内核把"隐藏输入"装好（装好后立刻打 [TERM] cmd passwd ok），再输口令行；
                # 否则 QEMU 注入的字符可能跟提示抢跑（实测会把口令行当成下一条命令）。
                vm.wait_new("[TERM] cmd passwd ok", 20, n_pterm)
                time.sleep(0.4)
                vm.type_line("s3cret-pw2", per_key=0.3)   # 交互式：这一行是隐藏输入
                ok_pw2 = vm.wait_new("[USER64] passwd ok user=alice set=1", 30, n_set) is not None
                check("交互式 passwd（隐藏输入）设口令成功（[USER64] passwd ok user=alice set=1）", ok_pw2,
                      (re.search(r"\[USER64\] passwd ok user=alice[^\r\n]*", vm.log()) or re.search(r"$^", vm.log())).group(0))
                if not ok_pw2:
                    # QEMU 注入偶尔丢键（只剩回车 -> 交互式会"取消"而不是清口令）：用非交互形式兜底，
                    # 规格允许两种形式；这里如实把两条都记下来。
                    vm.type_line("passwd alice s3cret-pw2")
                    ok_pw2 = vm.wait_new("[USER64] passwd ok user=alice set=1", 30, n_set) is not None
                    check("兜底：非交互形式 `passwd alice s3cret-pw2` 也设上了", ok_pw2)
                check("交互式输入的口令同样不进串口（明文绝不落日志）", "s3cret-pw2" not in vm.log())
            vm.type_line("restart --soft")
            log = vm.wait("[SYS64] soft restart requested", 25)
            check("软重启被接受（[SYS64] soft restart requested）", log is not None)
            t0 = time.time()
            while time.time() - t0 < 45:
                if vm.proc.poll() is not None:
                    break
                time.sleep(0.5)
            check("软重启最终触发真复位（-no-reboot 下 QEMU 退出）", vm.proc.poll() is not None)
            forbid("第一遍", vm.log())
    finally:
        vm.kill()

    # ==================================================================
    # 第二遍：同一块盘冷启动 -> 仍锁屏、只有普通用户、口令/文件持久化
    # ==================================================================
    print("=== 第二遍：重启后（同一块盘）===")
    vm2 = Vm(qemu, [disk], serial2, fst.free_port(), "Vimtu64-locklogin2")
    try:
        log = vm2.wait("[LOCK64] lock screen shown", 150)
        check("重启后仍进锁屏（[LOCK64] lock screen shown）", log is not None)
        if log is not None:
            check("重启后桌面还没出现（[GUI64] ready 不在锁屏之前）",
                  "[GUI64] ready" not in log[:log.find("[LOCK64] lock screen shown")])
            check("用户表跨重启存在（[USER64] userdb init ... users=4 normal=3 root=hidden）",
                  bool(re.search(r"\[USER64\] userdb init[^\r\n]*users=4 normal=3 root=hidden", log)))
            check("用户表是从盘上读回来的（[USER64] userdb load name=alice / name=bob）",
                  bool(re.search(r"userdb load line=\d+ name=alice", log)) and
                  bool(re.search(r"userdb load line=\d+ name=bob", log)))
            check("重启后登录前会话身份没有 euid=0（身份只存在内存里）",
                  "euid=0" not in log.split("[LOGIN64] login ok")[0])
            vm2.key("ret", wait=1.0)
            log = vm2.wait("[LOGIN64] userlist", 25)
            check("重启后登录界面仍然只有普通用户（userlist n=3 names=... root=hidden=1）",
                  bool(log) and bool(re.search(r"\[LOGIN64\] userlist n=3 names=[^ ]* root=hidden=1", log)),
                  (re.search(r"\[LOGIN64\] userlist[^\r\n]*", vm2.log()) or re.search(r"$^", vm2.log())).group(0))
            check("重启后登录界面没有 root 用户",
                  bool(log) and not re.search(r"userlist n=\d+ names=([^\s]*)\broot\b", log))
            sel = last(r"\[LOGIN64\] login screen shown blur=20 anim=\d+ms user=(\w+) password=(\w+)", vm2.log())
            check("重启后默认选中上次登录用户 alice 且要求口令（口令已持久化）",
                  sel == ("alice", "required"), str(sel))
            n_pw = vm2.log().count("[LOGIN64] password prompt")
            vm2.key("ret", wait=1.0)
            log = vm2.wait_new("[LOGIN64] password prompt", 20, n_pw)
            check("重启后仍需口令（弹密码框）", log is not None)
            vm2.type_line("s3cret-pw2")
            log = vm2.wait("[LOGIN64] login ok user=alice", 25)
            check("重启后用新口令能登录（/etc/users.db 跨重启生效）", log is not None)
            check("重启后 alice 的桌面文件列表仍在（aa.txt 持久化）",
                  bool(log) and bool(re.search(r"\[USER64\] desktop list user=alice [^\r\n]*names=aa\.txt", log)))
            forbid("第二遍", vm2.log())
    finally:
        vm2.kill()

    print("=== RESULT: %s（%d/%d 条通过）===" % ("PASS" if ok else "FAIL",
                                                sum(1 for c in checks if c[1]), len(checks)))
    for name, c, _ in checks:
        if not c:
            print("  FAIL: %s" % name)
    print("（临时目录：%s）" % tmp)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
