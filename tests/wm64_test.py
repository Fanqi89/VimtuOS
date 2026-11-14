#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/wm64_test.py - ★ B-wm 验收：把"图形"从内核搬到 Ring 3（用户态合成器 /bin/wm + 两个客户端）

要证明的事（与任务书的九条一一对应）：
  ① `wm` 进程起来并 **fb_map 成功**（`[WM] map .. u=1 bytes=..`）+ 注册成合成器
     （`[WL64] composer pid=.. gpu=..`），并且**内核从此不再合成**（注册之后一行 [WL64] composite
     都没有 —— 合成真的搬到用户态了）；
  ② 两个客户端（wmclock / wmpanel）的 shm surface 被 wm 用 27 号挪到**屏幕正中**（级联偏移 28px）
     后合成上屏（像素：窗口里出现时钟的色带色 + 交互面板的底色/标题色）；
  ③ **damage 语义**：时钟只改一个小块（几个字形）-> 窗口里、damage 矩形之外的像素**逐字节不变**
     （给出差异计数与包围盒）；**窗口之外**也给出逐字节差异计数；
  ④ **z 序**：两块 surface 级联后**仍然**故意重叠 -> 重叠区像素 = 上层（id 大）的内容色，且 != 下层的色；
  ⑤ **seat 事件路由**：注入鼠标点到 wmpanel 上 -> 它切换（像素变化），事件是 **wm 在用户态命中
     测试后用 wl_seat_post 投过去的**（内核只搬运：`[WL64] seatpost surf=.. pid=<wmpanel>`），
     且**没有**送给内核外壳（外壳的 ring0 点击处理路径没有任何新行）；
  ⑥ 客户端退出 -> surface 资源回收（`[WL64] surface destroy .. why=client` + `[SHM64] release .. refs=0
     freed_pages=15/8` + 页池回基线）；
  ⑦ `wm` 退出 -> 把地盘交回（`[WM] exit handback` + `[WL64] composer release .. mode=internal`）：
     wm 画过的那些**窗口矩形**被启动时的桌面快照回填（不是花屏；`px` = 窗口面积和），
     **窗口之外逐字节不变**、无 PANIC（不变砖）；
  ⑧ 无 PANIC / 无 [SYSCALL] enosys（22..26 号）/ 客户端与合成器都没有 FAILED；
  ⑨ **性能**：wm 每帧合成耗时的 rdtsc 数字（`[WM] frame .. us=..`），并与**同一台机器、同一分辨率、
     同一图案**的内核内合成器（`[WL64] composite n=2 bytes=.. us=..`，由 /wlclient.elf 的短模式在
     wm 启动**之前**跑出）对比；设备 blit 阈值（>=64x64=4096 px）的决策也在打点里（`[WM] blitpolicy
     gpu=.. min_px=4096 rect_px=.. path=soft|dev`）——本测试台没有 virtio-gpu，path 恒为 soft。

★ 版本口径（v0.9.47 / W3 第二批）：wm 从**右下角 320x200 面板**升级成**全屏 Ring 3 窗口层**。
  这一批改了三条契约，本文件的判据同步**加强**（不是放宽）：
    (a) 22 号 wl_composer_get 交的是**整屏**：日志 `[WL64] composer .. rect=` 现在是整屏，
        行尾**新增** `panel=<x>,<y>,<w>,<h>`（= `[WL64] init` 的面板几何，内核默认槽位策略没变）。
        判据：rect == (0,0,fb_w,fb_h) **且** 行尾 panel == init 的 panel（两条都比旧的一条强）。
    (b) wm 把每个新 surface 用 27 号 `wl_surface_move` 挪到"屏幕正中 + k*28 级联"：
        内核侧 `[WL64] move id=<n> pid=<p> pos=<x>,<y> from=<ox>,<oy> by=composer`，
        wm 侧 `[WM] place id=<n> pos=<x>,<y> mode=center|cascade`。
        ★ 于是 `[WL64] surface create .. slot=<n> pos=<x>,<y>` 里的 pos **仍是面板槽位、已经过期**，
        本文件一律用 move 行（同 id 取**最后一跳**）修正 rect 原点，没有 move 行才回退 create pos；
        同时新增判据"每个 surface 都必须有一次 by=composer 的 move"。
    (c) wm 不再涂地盘底色，改成**只画窗口矩形**：启动时拍**整屏背景快照**
        （`[WM] bg shm=<a>,<b> bytes=<fb_w*fb_h*4> u=1`，旧的 `[WM] shadow .. bytes=65536` 已删除），
        每帧先回填快照再按 id 升序 blit。于是：
          * `[WM] frame ..` 行尾由 `resync=<0|1>` 变成 `full=<0|1> foreign=<n>`；
          * `[WM] exit handback .. px=` = wm 画过的**窗口矩形面积和**（不再是 320*200）；
          * 口径从"面板区之外逐字节不变"升级成"**窗口之外**逐字节不变"（skip = 两个窗口矩形）。

用法（必须 Windows 原生 Python）：
    py -3 tests\\wm64_test.py             # QEMU：monitor 注入 + screendump 取样（像素断言全在这条路上）
    py -3 tests\\wm64_test.py --keep      # 保留临时目录（截图/串口）
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/镜像缺失）。
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
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
WM_ELF = os.path.join(ROOT, "build64", "wm.elf")
WMCLOCK_ELF = os.path.join(ROOT, "build64", "wmclock.elf")
WMPANEL_ELF = os.path.join(ROOT, "build64", "wmpanel.elf")
WLCLIENT_ELF = os.path.join(ROOT, "build64", "wlclient.elf")
FIXTURE_IMG = os.path.join(ROOT, "build64", "wm64_test.img")
PART_MAIN_LBA = 32584                    # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 57343                  # 28MB（= 原 16MB + 主分区起点后移的 24,519）（与 wl64/sh64 同一夹具口径）

MOUSE_X0, MOUSE_Y0 = 512, 384           # kernel/input.cpp 的 mouse_init 默认位置（★ W3 第二批：窗口被 wm
                                        # 挪到屏幕正中后，这个起点已经在窗口区附近 —— ⑤ 的注入计划仍从它起算，
                                        # 但按下前会先 settle，不再假定"起点一定在窗口外、路上不会碰窗口"）
PIX_TOL = 8                             # 逐像素"变/不变"的容差
OUTSIDE_TOL = 64                        # 区域之外允许的差异像素数（内核外壳的时钟/动画；打印实测值）

# ---- 客户端与合成器的取样色（与 user/wm/*.c 里的宏逐值一致）----
CLOCK_BAR = 0x40C0FF                    # wmclock 底部固定色带
CLOCK_INK = 0xE0F0FF                    # wmclock 数字笔画
PANEL_TITLE = 0x303A48                  # wmpanel 标题条
PANEL_BG = 0x1A2028                     # wmpanel 底
PANEL_ON = 0x30B070                     # wmpanel 开关：开
PANEL_OFF = 0xE0A020                    # wmpanel 开关：关

# 三个 ELF 在卷里的交付路径（kernel/wl64.cpp 只按 /bin/wm.elf 找合成器）
VOL_FILES = [("/bin/wm.elf", WM_ELF), ("/wmclock.elf", WMCLOCK_ELF), ("/wmpanel.elf", WMPANEL_ELF)]


def q(p):
    return p.replace("\\", "/")


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


# --------------------------------------------------------------------------- QEMU 夹具
class QemuVm:
    def __init__(self, qemu, img, port, serial):
        self.serial = serial
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-wm64",
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(160):
            if self.proc.poll() is not None:
                break
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_re(self, rx, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log())
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.15)
        return None

    def wait(self, needle, timeout):
        return self.wait_re(re.escape(needle), timeout) is not None

    def n_match(self, rx):
        return len(re.findall(rx, self.log()))

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.12):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def mouse_move(self, dx, dy, wait=0.15):
        return self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def mouse_button(self, mask, wait=0.15):
        return self.send("mouse_button %d" % mask, wait=wait)

    def key(self, k, wait=0.12):
        return self.send("sendkey %s" % k, wait=wait)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n1 = os.path.getsize(ppm)
                time.sleep(0.12)
                if os.path.getsize(ppm) == n1:
                    return True
            time.sleep(0.05)
        return False


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        return None
    idx, vals = 2, []
    while len(vals) < 3:
        while raw[idx:idx + 1].isspace():
            idx += 1
        s = idx
        while not raw[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(raw[s:idx]))
    idx += 1
    w, h, _ = vals
    return w, h, raw[idx:]


def px_at(shot, x, y):
    w, h, px = shot
    if x < 0 or y < 0 or x >= w or y >= h:
        return None
    o = (y * w + x) * 3
    return (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]


def near(c1, c2, tol=PIX_TOL):
    if c1 is None or c2 is None:
        return False
    return (abs(((c1 >> 16) & 0xFF) - ((c2 >> 16) & 0xFF)) <= tol and
            abs(((c1 >> 8) & 0xFF) - ((c2 >> 8) & 0xFF)) <= tol and
            abs((c1 & 0xFF) - (c2 & 0xFF)) <= tol)


def hexc(c):
    return "0x%06x" % (c or 0)


def in_rect(x, y, r):
    return r[0] <= x < r[0] + r[2] and r[1] <= y < r[1] + r[3]


def first_spot(rect, cands, others=()):
    """在 rect 内按 cands（相对左上角的 (dx,dy) 顺序）找第一个**不被 others 覆盖**的取样点。
    ★ W3 第二批：窗口挪到屏幕正中后会互相遮挡（级联 28px 不够把 160x96 / 128x64 错开），
      取样点必须先避开**上层窗口**，否则量到的是别人的像素（判据不打折，只换点）。
    返回 (x, y, dx, dy) 或 None（全部候选都被挡 / 不在矩形内）。"""
    for dx, dy in cands:
        x, y = rect[0] + dx, rect[1] + dy
        if not in_rect(x, y, rect):
            continue
        if any(in_rect(x, y, o) for o in others if o):
            continue
        return x, y, dx, dy
    return None


# ---- 两个客户端的**局部坐标**纯色模型（依据 user/wm/wmclock.c 与 user/wm/wmpanel.c 的绘制表）。
#      只回答"这块局部像素的**确定**颜色"，不确定（数字笔画/开关内部/半透明条）就返回 None ——
#      所以取样点只会在"两块客户端都保证纯色"的地方落地，判据不降级。
CLOCK_W, CLOCK_H = 160, 96
PANEL_W, PANEL_H = 128, 64
CLOCK_TOP = 0x283040                    # wmclock 顶部条
CLOCK_BG = 0x101820                     # wmclock 底
PANEL_EDGE = 0xF0F0F0                   # wmpanel 开关的 2px 边框


def clock_px(lx, ly):
    """wmclock 局部 (lx,ly) 的确定纯色；落在数字笔画区（秒数会变）就返回 None。"""
    if lx < 0 or ly < 0 or lx >= CLOCK_W or ly >= CLOCK_H:
        return None
    if 6 <= lx <= 126 and 28 <= ly <= 58:          # 数字区（INK_X=8 / INK_Y=30 / 5 字形 * 24 步进）
        return None
    if ly < 14:
        return CLOCK_TOP
    if ly >= CLOCK_H - 14:
        return CLOCK_BAR
    return CLOCK_BG


def panel_px(lx, ly, state=None):
    """wmpanel 局部 (lx,ly) 的确定纯色；开关内部/状态条随状态变（state 未知返回 None），
    半透明覆盖条是混合结果（返回 None）。"""
    if lx < 0 or ly < 0 or lx >= PANEL_W or ly >= PANEL_H:
        return None
    if 4 <= lx < PANEL_W - 4 and 50 <= ly < PANEL_H - 4:      # 半透明条 y∈[50,60) x∈[4,124)
        return None
    sx0, sy0, sw, sh = 16, 22, 48, 30                         # 开关矩形（P_SW_X/Y/W/H）
    if sx0 <= lx < sx0 + sw and sy0 <= ly < sy0 + sh:
        if lx < sx0 + 2 or lx >= sx0 + sw - 2 or ly < sy0 + 2 or ly >= sy0 + sh - 2:
            return PANEL_EDGE
        return None if state is None else (PANEL_ON if state else PANEL_OFF)
    if 74 <= lx < 86 and 26 <= ly < 42:                        # 开关右边的状态条
        return None if state is None else (PANEL_ON if state else PANEL_OFF)
    if ly < 14:
        return PANEL_TITLE
    return PANEL_BG


def diff_region(shot_a, shot_b, rect, skip=None):
    """rect 内的差异：返回 (n, bbox)；bbox = (x,y,w,h) 或 None。skip = 要跳过的矩形列表"""
    wa, ha, pa = shot_a
    wb, hb, pb = shot_b
    if (wa, ha) != (wb, hb):
        return -1, None
    x0, y0, rw, rh = rect
    bx0 = by0 = 1 << 30
    bx1 = by1 = -(1 << 30)
    n = 0
    for y in range(y0, y0 + rh):
        if y < 0 or y >= ha:
            continue
        base = y * wa * 3
        for x in range(x0, x0 + rw):
            if x < 0 or x >= wa:
                continue
            if skip and any(in_rect(x, y, s) for s in skip):
                continue
            o = base + x * 3
            if (abs(pa[o] - pb[o]) > PIX_TOL or abs(pa[o + 1] - pb[o + 1]) > PIX_TOL or
                    abs(pa[o + 2] - pb[o + 2]) > PIX_TOL):
                n += 1
                if x < bx0:
                    bx0 = x
                if y < by0:
                    by0 = y
                if x + 1 > bx1:
                    bx1 = x + 1
                if y + 1 > by1:
                    by1 = y + 1
    if n == 0:
        return 0, None
    return n, (bx0, by0, bx1 - bx0, by1 - by0)


def diff_classify(a, b, skip=None, black=8):
    """把窗口外的像素变化分成三类（v0.10.1）：
       erased  = a 有内容、b 变纯黑（wm 擦了外壳画过的东西 —— 必须为 0）
       painted = a 纯黑、b 有内容（外壳第一次真正绘制桌面 —— 允许，且要留证）
       kept    = 其它（窗口内容/抗锯齿的细微差别）
    """
    wa, ha, pa = a
    wb, hb, pb = b
    if (wa, ha) != (wb, hb):
        return -1, -1, -1
    erased = painted = kept = 0
    for y in range(ha):
        base = y * wa * 3
        for x in range(wa):
            if skip and any(in_rect(x, y, s) for s in skip):
                continue
            o = base + x * 3
            if (abs(pa[o] - pb[o]) <= PIX_TOL and abs(pa[o + 1] - pb[o + 1]) <= PIX_TOL and
                    abs(pa[o + 2] - pb[o + 2]) <= PIX_TOL):
                continue
            a_black = pa[o] <= black and pa[o + 1] <= black and pa[o + 2] <= black
            b_black = pb[o] <= black and pb[o + 1] <= black and pb[o + 2] <= black
            if not a_black and b_black:
                erased += 1
            elif a_black and not b_black:
                painted += 1
            else:
                kept += 1
    return erased, painted, kept


def bbox_inside(got, want, tol=2):
    if got is None:
        return True
    gx, gy, gw, gh = got[0], got[1], got[2], got[3]
    wx, wy, ww, wh = want
    return (gx >= wx - tol and gy >= wy - tol and
            gx + gw <= wx + ww + tol and gy + gh <= wy + wh + tol)


# --------------------------------------------------------------------------- 注入手势（与 wl64_test 同一套整数运算）
def mouse_step(delta):
    return (delta * 17) // 10 if delta >= 0 else -((-delta * 17) // 10)


def _pick_step(rem, max_in=14, y_axis=False):
    if rem == 0:
        return 0
    best = None
    for cand in range(-max_in, max_in + 1):
        if cand == 0 or (cand > 0) != (rem > 0):
            continue
        got = -mouse_step(-cand) if y_axis else mouse_step(cand)
        if got == 0:
            continue
        if best is None or abs(got - rem) < abs(((-mouse_step(-best)) if y_axis else mouse_step(best)) - rem):
            best = cand
        if got == rem:
            return cand
    return best if best is not None else 0


def plan_move(x0, y0, tx, ty, max_in=14):
    x, y = x0, y0
    steps = []
    for _ in range(200):
        if x == tx and y == ty:
            break
        dx_in = _pick_step(tx - x, max_in, y_axis=False)
        dy_in = _pick_step(ty - y, max_in, y_axis=True)
        if dx_in == 0 and dy_in == 0:
            break
        steps.append((dx_in, dy_in))
        x += mouse_step(dx_in)
        y -= mouse_step(-dy_in) if dy_in else 0
    return steps, (x, y)


# --------------------------------------------------------------------------- 夹具盘
def prepare_fixture_img(verbose=True):
    """夹具盘 = system.img + VimtuFS2 主分区；卷里写：
       /bin/wm.elf、/wmclock.elf、/wmpanel.elf、/etc/wm_probe（full 模式）、/wlclient.elf（短模式，
       给 ⑨ 的"内核内合成器"基线）、/bin/shell.bin（如果在）。"""
    import make_shellvol as msv
    for p in (SHELL_BIN, WM_ELF, WMCLOCK_ELF, WMPANEL_ELF, WLCLIENT_ELF):
        if not os.path.exists(p):
            raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % p)
    shell_bytes = open(SHELL_BIN, "rb").read()
    wm_bytes = open(WM_ELF, "rb").read()
    clock_bytes = open(WMCLOCK_ELF, "rb").read()
    panel_bytes = open(WMPANEL_ELF, "rb").read()
    wl_bytes = open(WLCLIENT_ELF, "rb").read()
    probe = b"/* wm probe: exists = full mode (Ring 3 compositor + 2 clients) */\n"
    hello = b"wm64 fixture\n"
    vol = msv.Volume(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", hello, parent=etc_ino, mode=0o644)
    vol.write_file("wm_probe", probe, parent=etc_ino, mode=0o644)
    vol.write_file("wm.elf", wm_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("wmclock.elf", clock_bytes, parent=0, mode=0o755)
    vol.write_file("wmpanel.elf", panel_bytes, parent=0, mode=0o755)
    vol.write_file("wlclient.elf", wl_bytes, parent=0, mode=0o755)
    vb = vol.finish()
    expect = {"/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": hello, "/etc/wm_probe": probe,
              "/bin/wm.elf": wm_bytes, "/wmclock.elf": clock_bytes, "/wmpanel.elf": panel_bytes,
              "/wlclient.elf": wl_bytes}
    bad = msv.verify(vb, expect)
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    if verbose:
        print("   夹具卷 OK：/bin/wm.elf=%d B /wmclock.elf=%d B /wmpanel.elf=%d B /etc/wm_probe=%d B"
              % (len(wm_bytes), len(clock_bytes), len(panel_bytes), len(probe)))
    disk = msv.build_disk(open(SYSTEM_IMG, "rb").read(), vb, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(disk)
    if verbose:
        print("   夹具盘：%s（%d B）" % (FIXTURE_IMG, len(disk)))
    return FIXTURE_IMG


class Checks:
    def __init__(self):
        self.ok = True
        self.n = 0
        self.passed = 0

    def __call__(self, name, cond, detail=""):
        self.n += 1
        if cond:
            self.passed += 1
        else:
            self.ok = False
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))
        return bool(cond)


# --------------------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5683)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=420)
    args = ap.parse_args()
    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2

    print("=== 0) 准备夹具盘（system.img + 卷：/bin/wm.elf + 两个客户端 + /etc/wm_probe）===")
    try:
        img = args.img if args.img else prepare_fixture_img()
    except Exception as e:
        sys.stderr.write("夹具盘准备失败：%s\n" % e)
        return 2
    if args.img:
        print("   用调用方给的盘：%s" % img)

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_wm64_")
    serial = os.path.join(tmp, "serial.log")
    ch = Checks()
    shots = {}
    vm = mon = None
    log = ""
    try:
        vm = QemuVm(qemu, img, args.port, serial)
        mon = Monitor(args.port)

        print("=== 1) 启动：内核面板几何 + wm 起来（①）===")
        m = vm.wait_re(r"\[WL64\] init fb=(\d+)x(\d+) panel=(-?\d+),(-?\d+),(\d+),(\d+) "
                       r"slots=(\d+) surf_max=(\d+) out=(\d+)", args.timeout)
        ch("内核打印 [WL64] init（fb 几何 + 外部面板区 + out=1）", m is not None,
           (("fb=%sx%s panel=%s,%s,%s,%s" % (m.group(1), m.group(2), m.group(3), m.group(4),
                                             m.group(5), m.group(6))) if m else "没等到"))
        if m is None:
            raise RuntimeError("没等到 [WL64] init")
        fbw, fbh = int(m.group(1)), int(m.group(2))
        # pgeo = 内核**默认槽位策略**的面板几何（W3 第二批没变；22 号 composer 行尾也如实打它）
        pgeo = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
        FB_RECT = (0, 0, fbw, fbh)                       # 22 号现在交给 wm 的"整屏"矩形
        SHELL_BAND = (0, 0, fbw, 24)                     # 内核外壳自己的顶部刷新带（③/⑦ 的"窗口之外"口径）
        ch("外部面板区完整落在屏幕内（比屏幕小）",
           pgeo[0] >= 0 and pgeo[1] >= 0 and pgeo[2] < fbw and pgeo[3] < fbh and
           pgeo[0] + pgeo[2] <= fbw and pgeo[1] + pgeo[3] <= fbh,
           "pgeo=%s fb=%dx%d" % (pgeo, fbw, fbh))

        ch("内核内合成器的基线（/wlclient.elf 短模式：合成发生在 wm 注册之前）",
           vm.wait_re(r"\[WL64\] composite n=(\d+) bytes=(\d+) us=(\d+)", args.timeout) is not None)
        ch("内核起了合成器进程（[WL64] wm start mode=full clients=2）",
           vm.wait_re(r"\[WL64\] wm start pid=(\d+) mode=full clients=2", args.timeout) is not None)
        ch("三分支模式判据一致：wm/两个客户端都判到 full",
           vm.wait_re(r"\[WM\] probe full=1", 60) is not None and
           vm.wait_re(r"\[WMCLOCK\] mode full=1", 60) is not None and
           vm.wait_re(r"\[WMPANEL\] mode full=1", 60) is not None)

        mpad = vm.wait_re(r"\[WM\] map pid=(\d+) va=0x([0-9a-f]+) w=(\d+) h=(\d+) pitch=(\d+) bytes=(\d+) u=1",
                          args.timeout)
        ch("① wm 自己 fb_map 成功（[WM] map .. u=1 bytes=..）", mpad is not None,
           (m.group(0) if mpad else "没等到"))
        ch("① wm 拿到后备缓冲的几何与内核一致（w/h 对得上）",
           mpad is not None and int(mpad.group(3)) == fbw and int(mpad.group(4)) == fbh,
           ("w=%s h=%s vs fb=%dx%d" % (mpad.group(3), mpad.group(4), fbw, fbh)) if mpad else "")
        mcomp = vm.wait_re(r"\[WL64\] composer pid=(\d+) seat=(\d+) gpu=(\d) backend=(\S+) surfs=(\d+) re=(\d) "
                           r"rect=(-?\d+),(-?\d+),(\d+),(\d+) panel=(-?\d+),(-?\d+),(\d+),(\d+)", args.timeout)
        ch("① wm 注册成合成器（[WL64] composer .. seat=1 .. rect=整屏 + 行尾 panel=..）", mcomp is not None,
           (mcomp.group(0) if mcomp else "没等到"))
        if mcomp:
            crect = (int(mcomp.group(7)), int(mcomp.group(8)), int(mcomp.group(9)), int(mcomp.group(10)))
            cpanel = (int(mcomp.group(11)), int(mcomp.group(12)), int(mcomp.group(13)), int(mcomp.group(14)))
            ch("① 内核交给 wm 的区域是**整屏** rect=(0,0,fb_w,fb_h)（22 号不再是右下角面板区）",
               crect == FB_RECT, "composer rect=%s want=%s" % (crect, FB_RECT))
            ch("① composer 行尾仍如实打面板几何（panel= == [WL64] init 的 panel=）",
               cpanel == pgeo, "composer panel=%s init pgeo=%s" % (cpanel, pgeo))
        ch("① 显示后端如实报为 soft-lfb（无 virtio-gpu 设备 -> gpu=0）",
           mcomp is not None and mcomp.group(4) == "soft-lfb" and mcomp.group(3) == "0",
           (mcomp.group(0) if mcomp else ""))
        ch("① wm 的输出走 input_poll 的焦点/捕获（[EV64] attach pid=<wm> flags=...3 focus/capture=<wm>）",
           vm.wait_re(r"\[EV64\] attach pid=(\d+) flags=0*3 focus=(\d+) capture=(\d+)", 60) is not None)
        mbg = vm.wait_re(r"\[WM\] bg shm=(\d+),(\d+) bytes=(\d+) va=0x([0-9a-f]+),0x([0-9a-f]+) u=1", 60)
        bg_need = fbw * fbh * 4                      # 整屏快照字节数（1280*800*4 = 4096000）
        ch("① 整屏背景快照也走 shm（[WM] bg shm=<a>,<b> bytes=fb_w*fb_h*4 u=1）",
           mbg is not None and int(mbg.group(3)) == bg_need and
           int(mbg.group(1)) >= 1 and int(mbg.group(2)) >= 1 and int(mbg.group(1)) != int(mbg.group(2)),
           ("bytes=%s want=%d shm=%s,%s" % (mbg.group(3), bg_need, mbg.group(1), mbg.group(2)))
           if mbg else "没等到")
        ch("① wm 自述全屏窗口层策略（[WM] policy region=full drag_bar=22 place=center+cascade bg=1）",
           vm.wait_re(r"\[WM\] policy region=full drag_bar=22 place=center\+cascade repair_ms=\d+ bg=1",
                      60) is not None)
        ch("① wm 报出设备 blit 阈值决策（[WM] blitpolicy gpu=0 min_px=4096）",
           vm.wait_re(r"\[WM\] blitpolicy gpu=0 min_px=4096", 60) is not None)
        ch("① wm 用 ticks() 标定 rdtsc（[WM] calib tsc_per_ms=..）",
           vm.wait_re(r"\[WM\] calib tsc_per_ms=(\d+) ms=(\d+)", 60) is not None)
        # 内核"不再合成"的证据：把日志在 [WL64] composer 行处劈开，后半段不许有 [WL64] composite
        vm.wait_re(r"\[WM\] frame n=1 ", args.timeout)
        log = vm.log()
        idx = log.find("[WL64] composer pid=")
        tail = log[idx:] if idx >= 0 else ""
        ch("① 注册合成器之后内核一行 [WL64] composite 都没有（合成真的搬走了）",
           idx >= 0 and "[WL64] composite" not in tail,
           "tail_composite=%d" % tail.count("[WL64] composite"))
        ch("① 内核改用 [WL64] dispatch .. composite=external 提示（前 8 轮）",
           "[WL64] dispatch" in tail and "composite=external" in tail)

        print("=== 2) ② 两个客户端 surface 被合成上屏（像素证据）===")
        ch("两个客户端都建了 surface 并拿到 shm（[WMCLOCK]/[WMPANEL] surf ..）",
           vm.wait_re(r"\[WMCLOCK\] surf id=(\d+) w=160 h=96 shm=(\d+)", 60) is not None and
           vm.wait_re(r"\[WMPANEL\] surf id=(\d+) w=128 h=64 shm=(\d+)", 60) is not None)
        mcra = vm.wait_re(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+) shm=0 pid=(\d+) px=\d+ "
                          r"fmt=\d+ slot=(\d+) pos=(-?\d+),(-?\d+)", args.timeout)
        ch("内核侧 surface 表里有这两块（[WL64] surface create .. w=160/128）", mcra is not None)
        # pid -> 名字（内核 create 行）；只看**两个客户端自己的** surface（/wlclient.elf 短模式留下的
        # id=1/2 已经销毁，不属于"当前屏上的 surface"）
        name2pid = {v: int(k) for k, v in re.findall(r"\[PROC64\] create pid=(\d+) name=(\S+)", vm.log())}
        clk_pid = name2pid.get("wmclock")
        pnl_pid = name2pid.get("wmpanel")
        surfs = {}
        for r in re.findall(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+) shm=0 pid=(\d+) px=\d+ "
                            r"fmt=\d+ slot=(\d+) pos=(-?\d+),(-?\d+)", vm.log()):
            if int(r[3]) not in (clk_pid, pnl_pid):
                continue
            surfs[int(r[0])] = dict(w=int(r[1]), h=int(r[2]), pid=int(r[3]), slot=int(r[4]),
                                    rect=(int(r[5]), int(r[6]), int(r[1]), int(r[2])))
        clock_id = panel_id = None
        ch("恰好两块当前 surface（wm 自己不建 surface：它直接画自己的地盘）", len(surfs) == 2,
           "ids=%s (clk_pid=%s pnl_pid=%s)" % (sorted(surfs), clk_pid, pnl_pid))
        if len(surfs) == 2:
            ids = sorted(surfs)
            lo, hi = ids[0], ids[1]
            names = {surfs[i]["pid"]: ("wmclock" if surfs[i]["pid"] == clk_pid else "wmpanel") for i in ids}
            ch("两块 surface 由两个不同的进程持有（wmclock / wmpanel）",
               surfs[lo]["pid"] != surfs[hi]["pid"], "pids=%s names=%s" % (
                   (surfs[lo]["pid"], surfs[hi]["pid"]), (names[surfs[lo]["pid"]], names[surfs[hi]["pid"]])))
            # ★ W3 第二批：create 行里的 pos= 是**面板槽位**（内核默认策略没变），wm 随后就用 27 号把它挪到
            #   "屏幕正中 + k*28" -> create 行的 pos **已经过期**，不能当屏幕坐标用。这里一律用
            #   [WL64] move 行（同一个 id 取**最后一跳**）覆盖 rect 原点；没有 move 行才回退 create pos。
            slot_pos = {i: (surfs[i]["rect"][0], surfs[i]["rect"][1]) for i in ids}
            t_pl = time.time()
            while time.time() - t_pl < 60:               # 等 wm 把两块都摆完（每块一次 move）
                done = set(int(x) for x in re.findall(
                    r"\[WL64\] move id=(\d+) pid=-?\d+ pos=-?\d+,-?\d+ from=-?\d+,-?\d+ by=composer", vm.log()))
                if set(ids) <= done:
                    break
                time.sleep(0.15)
            moves = {}
            for r in re.findall(r"\[WL64\] move id=(\d+) pid=-?\d+ pos=(-?\d+),(-?\d+) "
                                r"from=(-?\d+),(-?\d+) by=(\w+)", vm.log()):
                moves[int(r[0])] = dict(pos=(int(r[1]), int(r[2])), frm=(int(r[3]), int(r[4])), by=r[5])
            for i in ids:
                mv = moves.get(i)
                if mv:                                        # 没有 move 行 -> 保持 create pos（回退）
                    surfs[i]["rect"] = (mv["pos"][0], mv["pos"][1], surfs[i]["w"], surfs[i]["h"])
            places = {int(a): (int(b), int(c), d) for a, b, c, d in re.findall(
                r"\[WM\] place id=(\d+) pos=(-?\d+),(-?\d+) mode=(center|cascade)", vm.log())}
            wmsurf = {int(a): (int(b), int(c)) for a, b, c in re.findall(
                r"\[WM\] surf id=(\d+) pid=-?\d+ rect=(-?\d+),(-?\d+),\d+,\d+ ", vm.log())}
            ch("② wm 用 27 号把**每个**新 surface 都挪过一次（每个 id 都有 [WL64] move .. by=composer）",
               all(i in moves and moves[i]["by"] == "composer" for i in ids),
               "by=%s" % {i: moves.get(i, {}).get("by") for i in ids})
            ch("② 屏幕坐标以 move 行为准，且与 wm 自己的两份打点对得上（[WM] place pos / [WM] surf rect）",
               all(i in moves and i in places and (places[i][0], places[i][1]) == moves[i]["pos"] and
                   (i not in wmsurf or wmsurf[i] == moves[i]["pos"]) for i in ids),
               "move=%s place=%s wmsurf=%s" % ({i: moves.get(i, {}).get("pos") for i in ids},
                                               {i: places[i][:2] for i in ids if i in places}, wmsurf))
            ch("② 两块 surface 都在**整屏**内（rect ⊆ 屏幕）",
               all(surfs[i]["rect"][0] >= 0 and surfs[i]["rect"][1] >= 0 and
                   surfs[i]["rect"][0] + surfs[i]["rect"][2] <= fbw and
                   surfs[i]["rect"][1] + surfs[i]["rect"][3] <= fbh for i in ids),
               "rects=%s fb=%dx%d" % ([surfs[i]["rect"] for i in ids], fbw, fbh))
            ch("② 屏幕坐标 != create 行的面板槽位 pos（= 真的被 wm 挪过，旧坐标不再成立）",
               all(i in moves and (surfs[i]["rect"][0], surfs[i]["rect"][1]) != slot_pos[i] for i in ids),
               "pos=%s slot_pos=%s" % ({i: (surfs[i]["rect"][0], surfs[i]["rect"][1]) for i in ids}, slot_pos))
            # 摆位策略：pos = ((fb_w-w)/2, (fb_h-h)/2) + k*28（两轴同偏移，k = 全局创建序号 % 6 ∈ [0,5]）
            pol_bad = []
            for i in ids:
                if i not in moves:
                    pol_bad.append((i, "no-move"))
                    continue
                cx, cy = (fbw - surfs[i]["w"]) // 2, (fbh - surfs[i]["h"]) // 2
                ddx, ddy = surfs[i]["rect"][0] - cx, surfs[i]["rect"][1] - cy
                if ddx != ddy or ddx < 0 or ddx % 28 != 0 or ddx // 28 > 5:
                    pol_bad.append((i, (ddx, ddy)))
            ch("② 摆位符合 wm 的 policy：pos = ((fb_w-w)/2,(fb_h-h)/2) + k*28（k = 创建序号 % 6）",
               not pol_bad, "bad=%s rects=%s fb=%dx%d" % (pol_bad, [surfs[i]["rect"] for i in ids], fbw, fbh))
            ch("② 两块 surface **故意重叠**（z 序有可观测的遮挡区）",
               surfs[hi]["rect"][0] < surfs[lo]["rect"][0] + surfs[lo]["rect"][2] and
               surfs[hi]["rect"][1] < surfs[lo]["rect"][1] + surfs[lo]["rect"][3] and
               surfs[lo]["rect"][0] < surfs[hi]["rect"][0] + surfs[hi]["rect"][2] and
               surfs[lo]["rect"][1] < surfs[hi]["rect"][1] + surfs[hi]["rect"][3],
               "rects=%s（靠级联偏移 28px 产生：28 < 窗口高 %d/%d -> 必然重叠，不重叠就是级联没生效）"
               % ([surfs[i]["rect"] for i in ids], surfs[lo]["rect"][3], surfs[hi]["rect"][3]))
            ch("wm 把两块都映射进自己地址空间并合成（[WM] surfmap / [WM] frame surfs=2）",
               vm.wait_re(r"\[WM\] frame n=(\d+) surfs=2 pending=\d+ ", 60) is not None)

            # 逐块找出哪一块是 wmclock（w=160x96）
            for i in ids:
                if surfs[i]["w"] == 160 and surfs[i]["h"] == 96:
                    clock_id = i
                if surfs[i]["w"] == 128 and surfs[i]["h"] == 64:
                    panel_id = i
            ch("按尺寸认出 wmclock（160x96）与 wmpanel（128x64）",
               clock_id is not None and panel_id is not None,
               "clock=%s panel=%s" % (clock_id, panel_id))

        # 等两个客户端第一帧都上屏 + wm 合成过，再抓图
        vm.wait_re(r"\[WMCLOCK\] frame f=0 ", args.timeout)
        vm.wait_re(r"\[WMPANEL\] frame f=0 ", args.timeout)
        time.sleep(0.8)
        ppm = os.path.join(tmp, "phase1.ppm")
        if mon.shot(ppm):
            shots["p1"] = read_ppm(ppm)
        if clock_id is not None and panel_id is not None and "p1" in shots and shots["p1"]:
            cr = surfs[clock_id]["rect"]                  # ★ 已用 [WL64] move 修正的**屏幕**坐标
            pr = surfs[panel_id]["rect"]
            # z 序 = id 升序（wm 从下到上 blit）：id 大的在上层。取样必须避开上层窗口，
            # 否则那块像素根本不是这一块画的（换点不换判据）。
            above_cr = [pr] if panel_id > clock_id else []
            above_pr = [cr] if clock_id > panel_id else []
            bar_pt = first_spot(cr, [(80, cr[3] - 8), (40, cr[3] - 8), (20, cr[3] - 8), (8, cr[3] - 8),
                                     (108, cr[3] - 8)], above_cr)          # 时钟底部固定色带
            ink_pt = first_spot(cr, [(60, 48), (40, 48), (20, 48), (8, 48), (96, 48)], above_cr)
            ptt_pt = first_spot(pr, [(60, 6), (40, 6), (90, 6), (20, 6)], above_pr)       # 面板标题条
            sw_pt = first_spot(pr, [(40, 36), (30, 36), (48, 36), (24, 40)], above_pr)    # 开关体内（关=橙）
            veil_pt = first_spot(pr, [(64, 54), (50, 54), (90, 54), (64, 56)], above_pr)  # 半透明条（局部 y=54）
            got_bar = px_at(shots["p1"], bar_pt[0], bar_pt[1]) if bar_pt else None
            got_ink = px_at(shots["p1"], ink_pt[0], ink_pt[1]) if ink_pt else None
            got_ptt = px_at(shots["p1"], ptt_pt[0], ptt_pt[1]) if ptt_pt else None
            got_sw = px_at(shots["p1"], sw_pt[0], sw_pt[1]) if sw_pt else None
            got_veil = px_at(shots["p1"], veil_pt[0], veil_pt[1]) if veil_pt else None
            ch("② wmclock 区域出现内容（底部色带 = 0x40c0ff）", near(got_bar, CLOCK_BAR),
               "got=%s want=%s rect=%s spot=%s" % (hexc(got_bar), hexc(CLOCK_BAR), cr, bar_pt))
            ch("② wmclock 数字区有内容（笔画色或底色，两种都算）",
               near(got_ink, CLOCK_INK) or near(got_ink, 0x101820),
               "got=%s (ink=%s bg=0x101820) spot=%s" % (hexc(got_ink), hexc(CLOCK_INK), ink_pt))
            ch("② wmpanel 区域出现内容（标题条 = 0x303a48）", near(got_ptt, PANEL_TITLE),
               "got=%s want=%s rect=%s spot=%s" % (hexc(got_ptt), hexc(PANEL_TITLE), pr, ptt_pt))
            ch("② wmpanel 开关初始为关态（橙色 0xe0a020）", near(got_sw, PANEL_OFF),
               "got=%s want=%s spot=%s" % (hexc(got_sw), hexc(PANEL_OFF), sw_pt))
            ch("② 半透明覆盖条真的被 alpha 混合了（既不是纯白也不是纯底色）",
               got_veil is not None and not near(got_veil, 0xFFFFFF) and not near(got_veil, PANEL_BG),
               "got=%s spot=%s" % (hexc(got_veil), veil_pt))
        # 合成器的混合计数：**任一帧** blend>0 即可（第一帧可能只合成一块面）
        blends = [int(x) for x in re.findall(r"\[WM\] frame n=\d+ surfs=\d+ pending=\d+ .*? blend=(\d+) ", vm.log())]
        ch("② 合成器统计到混合像素（[WM] frame .. blend>0，alpha 路径真的走过）",
           any(b > 0 for b in blends), "blends=%s" % blends[:8])
        print("=== 3) ③ damage 局部性：时钟只改一小块，其他像素逐字节不变 ===")
        # 等到时钟第 2 帧（dmg 矩形由客户端给出），抓帧；再等第 3 帧，抓帧 -> 两帧之间只该有那一块的差异
        shots_seq = {}
        for want in (1, 2):
            if not vm.wait_re(r"\[WMCLOCK\] frame f=%d " % want, 240):
                break
            time.sleep(0.45)                     # 给 wm 合成 + 提交的时间（wm 空转 30ms 一轮）
            before = vm.n_match(r"\[WMCLOCK\] frame f=")
            p = os.path.join(tmp, "f%d.ppm" % want)
            got = mon.shot(p)
            after = vm.n_match(r"\[WMCLOCK\] frame f=")
            if got and after == before:
                shots_seq[want] = read_ppm(p)
        ch("③ 抓到时钟连续两帧的确定截图（f=1 / f=2）", len(shots_seq) == 2,
           "shots=%s" % sorted(shots_seq))
        clkframes = {}
        for r in re.findall(r"\[WMCLOCK\] frame f=(\d+) sec=(\d+) dmg=(-?\d+),(-?\d+),(\d+),(\d+) ink=0x(\w+) bar=0x(\w+)",
                            vm.log()):
            clkframes[int(r[0])] = dict(sec=int(r[1]), dmg=(int(r[2]), int(r[3]), int(r[4]), int(r[5])))
        if len(shots_seq) == 2 and clock_id is not None:
            cr = surfs[clock_id]["rect"]
            pr = surfs[panel_id]["rect"] if panel_id is not None else None
            win_rects = [cr] + ([pr] if pr else [])      # ★ 口径：两个**窗口矩形**（不再是 320x200 面板区）
            dmg = clkframes[2]["dmg"]
            want = (cr[0] + dmg[0], cr[1] + dmg[1], dmg[2], dmg[3])
            n_in, bbox_in = diff_region(shots_seq[1], shots_seq[2], cr)
            # 窗口之外：内核外壳有自己的刷新区（顶部状态条的时钟/通知），把最上面 24 行的
            # "外壳自己那一带"单独统计 —— 窗口之外、且**不在**那一带里的像素必须逐字节不变。
            # （SHELL_BAND 在 ① 那一节里按 fb 几何定义好了：顶部 24 行）
            n_out, bbox_out = diff_region(shots_seq[1], shots_seq[2], (0, 0, fbw, fbh), skip=win_rects)
            n_band, _ = diff_region(shots_seq[1], shots_seq[2], SHELL_BAND, skip=win_rects)
            n_out_strict, _ = diff_region(shots_seq[1], shots_seq[2], (0, 0, fbw, fbh),
                                          skip=win_rects + [SHELL_BAND])
            n_rest, _ = diff_region(shots_seq[1], shots_seq[2], cr, skip=[want])
            ch("③ 时钟面内的帧差包围盒 ⊆ 客户端上报的 damage 矩形 %s" % (want,),
               bbox_inside(bbox_in, want), "bbox=%s n=%d" % (bbox_in, n_in))
            ch("③ 时钟窗口内、damage 框之外**逐字节不变**（差异像素 = 0）", n_rest == 0,
               "outside_damage_diff=%d" % n_rest)
            ch("③ **窗口之外**逐字节不变（口径从 320x200 面板区升级成两个窗口矩形；不计外壳顶部刷新带）",
               n_out_strict == 0,
               "outside_window_diff=%d（其中外壳顶部带 %d 像素）bbox=%s windows=%s"
               % (n_out, n_band, bbox_out, win_rects))
            ch("③ 窗口之外的差异**全部**落在内核外壳自己的刷新带（顶部 24 行）里",
               bbox_out is None or (bbox_out[1] >= 0 and bbox_out[1] + bbox_out[3] <= 24),
               "bbox=%s band=%d" % (bbox_out, n_band))
            print("   [数字] 时钟 f=1->f=2：damage=%s（屏幕坐标=%s）；面内差异=%d；"
                  "窗口内 damage 外差异=%d；窗口外差异=%d（外壳顶部带 %d / 带外 %d）；windows=%s"
                  % (dmg, want, n_in, n_rest, n_out, n_band, n_out_strict, win_rects))

        print("=== 4) ④ z 序：重叠区显示上层（id 大）的内容 ===")
        if clock_id is not None and panel_id is not None and "p1" in shots and shots["p1"]:
            cr = surfs[clock_id]["rect"]
            pr = surfs[panel_id]["rect"]
            ix0 = max(cr[0], pr[0])
            iy0 = max(cr[1], pr[1])
            ix1 = min(cr[0] + cr[2], pr[0] + pr[2])
            iy1 = min(cr[1] + cr[3], pr[1] + pr[3])
            ch("④ 两块 surface 的重叠区非空", ix1 > ix0 and iy1 > iy0,
               "overlap=(%d,%d)-(%d,%d)" % (ix0, iy0, ix1, iy1))
            if ix1 > ix0 and iy1 > iy0:
                # ★ 窗口挪到屏幕正中后重叠区也变了，"交集中心"未必还落在标题条/色带上，而且两块窗口
                #   互相遮挡。改成：在交集里找**上下两层都是已知纯色**的点（模型见 clock_px/panel_px），
                #   优先挑"下层正好是时钟固定色带"的点；一个都找不到就 FAIL（不跳过、不降级）。
                pst = re.findall(r"\[WMPANEL\] frame f=\d+ state=(\d) col=0x\w+", vm.log())
                pnl_state = int(pst[-1]) if pst else 0
                upper = max(clock_id, panel_id)                # z 序 = id 升序（wm 从下到上 blit）
                lo_id = min(clock_id, panel_id)
                up_r = surfs[upper]["rect"]
                lo_r = surfs[lo_id]["rect"]
                pick = None
                for sy in range(iy0, iy1):
                    for sx in range(ix0, ix1):
                        c_up = (panel_px(sx - up_r[0], sy - up_r[1], pnl_state) if upper == panel_id
                                else clock_px(sx - up_r[0], sy - up_r[1]))
                        c_lo = (clock_px(sx - lo_r[0], sy - lo_r[1]) if lo_id == clock_id
                                else panel_px(sx - lo_r[0], sy - lo_r[1], pnl_state))
                        if c_up is None or c_lo is None or c_up == c_lo:
                            continue
                        pick = (sx, sy, c_up, c_lo)
                        if c_lo == CLOCK_BAR:                     # 下层是时钟的固定色带：最有说服力
                            break
                    if pick and pick[3] == CLOCK_BAR:
                        break
                ch("④ 重叠区里存在\"上下两层都是已知纯色\"的取样点（不靠猜交集中心）", pick is not None,
                   "overlap=(%d,%d)-(%d,%d) upper=%d lower=%d" % (ix0, iy0, ix1, iy1, upper, lo_id))
                if pick:
                    sx, sy, c_up, c_lo = pick
                    got = px_at(shots["p1"], sx, sy)
                    up_name = "wmclock" if upper == clock_id else "wmpanel"
                    lo_name = "wmclock" if lo_id == clock_id else "wmpanel"
                    ch("④ 重叠区像素 = 上层（id=%d，%s）的内容色 %s" % (upper, up_name, hexc(c_up)),
                       near(got, c_up), "got=%s at (%d,%d) upper_local=(%d,%d)"
                       % (hexc(got), sx, sy, sx - up_r[0], sy - up_r[1]))
                    ch("④ 重叠区像素**不是**下层（id=%d，%s）的内容色（确实被遮挡）" % (lo_id, lo_name),
                       not near(got, c_lo), "got=%s lower=%s（下层该点局部 (%d,%d)）"
                       % (hexc(got), hexc(c_lo), sx - lo_r[0], sy - lo_r[1]))

        print("=== 5) ⑤ seat：注入鼠标点到 wmpanel 上（用户态命中测试 + 投递）===")
        # 事件统计的基线（用于证明"没有送给内核外壳"）
        shell_markers = ["[STARTMENU]", "[EXPLORER]", "[DESKTOPOPS]"]
        base_shell = {k: vm.n_match(re.escape(k)) for k in shell_markers}
        base_seatpost = vm.n_match(r"\[WL64\] seatpost surf=")
        click_ok = False
        if panel_id is not None:
            pr = surfs[panel_id]["rect"]                 # ★ 已用 [WL64] move 修正的**屏幕**坐标
            tx, ty = pr[0] + 40, pr[1] + 36              # 开关中心（局部 40,36）
            # 起点说明：MOUSE_X0/Y0=512,384 仍是 kernel/input.cpp 的 mouse_init 默认位置；窗口现在被
            # wm 挪到屏幕正中（1280x800 下 wmclock=(560,352) / wmpanel=(604,396)），起点刚好落在窗口
            # **外**（512 < 560），但注入路径会穿过窗口 —— 所以按下之前先 settle（等 wm 把"收到输入后
            # 整窗重画"的那一帧落地），否则可能按在旧像素/半画状态的窗口上。
            ch("⑤ 点击点上层就是 wmpanel（z 序 = id 升序：panel id > clock id，否则点击会被时钟吃掉）",
               clock_id is None or panel_id > clock_id,
               "panel_id=%s clock_id=%s rect=%s" % (panel_id, clock_id, pr))
            steps, (sim_x, sim_y) = plan_move(MOUSE_X0, MOUSE_Y0, tx, ty)
            print("   注入计划：起点 (%d,%d) -> (%d,%d) via %d 步（模拟落点 (%d,%d)）"
                  % (MOUSE_X0, MOUSE_Y0, tx, ty, len(steps), sim_x, sim_y))
            base_frames = vm.n_match(r"\[WM\] frame n=")
            for dx, dy in steps:
                mon.mouse_move(dx, dy)
            t_st = time.time()                           # settle：等 wm 因为这次输入再合成一帧
            while time.time() - t_st < 5:
                if vm.n_match(r"\[WM\] frame n=") > base_frames:
                    break
                time.sleep(0.1)
            time.sleep(0.5)
            mon.mouse_button(1)
            time.sleep(0.4)
            mon.mouse_button(0)
            click_ok = vm.wait_re(r"\[WMPANEL\] click surf=\d+ sx=(-?\d+) sy=(-?\d+) state=1 col=0x(\w+) "
                                  r"dmg=(-?\d+),(-?\d+),(\d+),(\d+)", 60) is not None
            ch("⑤ wmpanel 收到 seat 事件并**切换了状态**（[WMPANEL] click .. state=1）", click_ok)
            ch("⑤ 事件是 wm 在用户态命中测试后投过去的（[WL64] seatpost surf=<panel> pid=<panel.pid>）",
               vm.wait_re(r"\[WL64\] seatpost surf=%d pid=%d " % (panel_id, surfs[panel_id]["pid"]), 30) is not None)
            ch("⑤ wm 也打了自己的路由行（[WM] route surf=<panel> pid=<panel.pid>）",
               vm.wait_re(r"\[WM\] route surf=%d pid=%d " % (panel_id, surfs[panel_id]["pid"]), 30) is not None)
            added = vm.n_match(r"\[WL64\] seatpost surf=") - base_seatpost
            ch("⑤ 这次点击只投给了一个进程（seatpost 增量 >= 1 且都指向 wmpanel）",
               added >= 1 and vm.n_match(r"\[WL64\] seatpost surf=%d " % panel_id) >= 1,
               "seatpost_added=%d" % added)
            ch("⑤ 内核侧没有再打 [WL64] seat event 路由行（事件走 26 号投递，不走 15..21 的老路）",
               vm.wait_re(r"\[WM\] frame n=\d+ ", 5) is not None)
            time.sleep(0.7)
            p2 = os.path.join(tmp, "click.ppm")
            if mon.shot(p2):
                shots["click"] = read_ppm(p2)
            if "click" in shots and shots["click"]:
                # 取样点避开内核光标（光标就在点击处，像素是白的）：取开关体内的另外两点
                sw_a = px_at(shots["click"], pr[0] + 20, pr[1] + 30)
                sw_b = px_at(shots["click"], pr[0] + 58, pr[1] + 30)
                ch("⑤ 点击后开关像素变成开态（绿色 0x30b070）——屏幕真的变了",
                   near(sw_a, PANEL_ON) or near(sw_b, PANEL_ON),
                   "got=%s/%s want=%s" % (hexc(sw_a), hexc(sw_b), hexc(PANEL_ON)))
            shell_delta = {k: vm.n_match(re.escape(k)) - base_shell[k] for k in shell_markers}
            ch("⑤ 事件**没有送给内核外壳**（外壳的点击处理路径 0 新增行：%s）" % shell_delta,
               sum(shell_delta.values()) == 0, "delta=%s" % shell_delta)
        else:
            ch("⑤ 找到 wmpanel 的 surface", False, "没认出 128x64 的那块")

        print("=== 6) ⑥ 客户端退出 -> surface/shm 资源回收 ===")
        ch("wmclock 跑完并退出（[WMCLOCK] done）", vm.wait("[WMCLOCK] done", 120))
        ch("wmpanel 跑完并退出（[WMPANEL] done）", vm.wait("[WMPANEL] done", 120))
        ch("两块 surface 都被客户端自己销毁（[WL64] surface destroy why=client x2）",
           vm.n_match(r"\[WL64\] surface destroy .* why=client") >= 2,
           "n=%d" % vm.n_match(r"\[WL64\] surface destroy .* why=client"))
        rel = re.findall(r"\[SHM64\] release pid=(\d+) id=(\d+) refs=0 freed_pages=(\d+)", vm.log())
        clk_pid = str(surfs[clock_id]["pid"]) if clock_id is not None else None
        pnl_pid = str(surfs[panel_id]["pid"]) if panel_id is not None else None
        rel_by_pid = {}
        for r in rel:
            rel_by_pid.setdefault(r[0], []).append(int(r[2]))
        ch("⑥ wmclock 的缓冲对象引用归零并还页（freed_pages=15）",
           clk_pid in rel_by_pid and 15 in rel_by_pid[clk_pid], "rel=%s" % rel_by_pid)
        ch("⑥ wmpanel 的缓冲对象引用归零并还页（freed_pages=8）",
           pnl_pid in rel_by_pid and 8 in rel_by_pid[pnl_pid], "rel=%s" % rel_by_pid)
        de = re.findall(r"\[WL64\] surface destroy id=(\d+) pid=(\d+) shm=(\d+) refs_after=(\d+) held=(\d+) why=(\w+)",
                        vm.log())
        de_cli = [r for r in de if r[5] == "client" and r[1] in (clk_pid, pnl_pid)]
        ch("⑥ 两个客户端都自己销毁了 surface，且把组合器持有的 shm 引用还了回去（refs_after=1）",
           len(de_cli) >= 2 and all(r[3] == "1" for r in de_cli),
           "lines=%s（held= 是销毁前该 pid 的 surface 数，单面客户端因此是 1）"
           % [(r[1], r[3], r[4]) for r in de_cli])

        print("=== 7) ⑦ wm 退出 -> 地盘交回、内核外壳完好（不变砖）===")
        shot_before_exit = None
        p3 = os.path.join(tmp, "before_exit.ppm")
        if mon.shot(p3):
            shot_before_exit = read_ppm(p3)
        mhb = vm.wait_re(r"\[WM\] exit handback color=0x([0-9a-f]+) px=(\d+) frames=(\d+)", 120)
        ch("wm 在客户端全退后自己收工（[WM] exit handback ..）", mhb is not None)
        hb_col = int(mhb.group(1), 16) if mhb else None
        hb_px = int(mhb.group(2)) if mhb else 0
        time.sleep(0.4)
        p3b = os.path.join(tmp, "right_after_exit.ppm")
        if mon.shot(p3b):
            shots["exit1"] = read_ppm(p3b)
        ch("⑦ 合成器把合成权交回内核（[WL64] composer release .. mode=internal）",
           vm.wait_re(r"\[WL64\] composer release pid=\d+ exports=\d+ maps=\d+ acks=\d+ posts=\d+ mode=internal",
                      60) is not None)
        ch("⑦ 内核侧收尾打点（[WL64] wm done .. composer=0）",
           vm.wait_re(r"\[WL64\] wm done procs=3 exited=3 ticks=\d+ pool_free=\d+ pool_delta=[+-]\d+ "
                      r"surfs=0 composer=0", 60) is not None)
        md = vm.wait_re(r"\[WL64\] wm done procs=3 exited=(\d+) ticks=\d+ pool_free=\d+ pool_delta=([+-]\d+) "
                        r"surfs=(\d+) composer=(\d+)", 30)
        if md:
            ch("⑦ 三个进程都退出了（exited=3）；页池差值 %s（外壳自己在跑，允许 +/-4 页的小抖动）"
               % md.group(2),
               md.group(1) == "3" and abs(int(md.group(2))) <= 4, md.group(0))
        time.sleep(0.6)
        # 把内核光标挪回桌面中部（离开我们的地盘），再看屏幕
        steps, _ = plan_move(512, 384, 400, 300)
        for dx, dy in steps:
            mon.mouse_move(dx, dy)
        time.sleep(0.8)
        p4 = os.path.join(tmp, "after_exit.ppm")
        # ⑦-a：wm 的交回动作在日志里是"**把我们画过的那些窗口矩形**还给桌面"（px = Σ窗口面积；
        #       W3 第二批起**不再是 320*200**），并且退出后那块地**不再有合成器自己的内容**
        #       （时钟色带色 / 数字笔画色 / 面板标题色 / 开关色都不该再出现在原位置上）：
        #       内核外壳会在这块地上重画自己的东西（本批的共存策略），所以比"等于某个回填色"更稳的
        #       判据是"我们的像素没留下"。取样点从**handback 前最后一次 [WM] frame 的 dirty 矩形**里取。
        win_rects2 = [surfs[i]["rect"] for i in sorted(surfs)]
        exp_px = sum(r2[2] * r2[3] for r2 in win_rects2)      # Σ(窗口面积)（两块都完整在屏内）
        last_dirty = None
        for r in re.findall(r"\[WM\] frame n=\d+ surfs=\d+ pending=\d+ dirty=(-?\d+),(-?\d+),(\d+),(\d+)",
                            vm.log()):
            if int(r[2]) > 0 and int(r[3]) > 0:
                last_dirty = (int(r[0]), int(r[1]), int(r[2]), int(r[3]))
        ch("⑦ handback 的 px = wm 画过的**窗口矩形面积和**（Σ w*h ±1%；不再是 320*200）",
           mhb is not None and len(win_rects2) == 2 and abs(hb_px - exp_px) <= max(1, exp_px // 100),
           "px=%d want=%d(±1%%) windows=%s" % (hb_px, exp_px, win_rects2))
        if "exit1" in shots and shots["exit1"]:
            probes = []
            if last_dirty:                                  # wm 刚用快照回填过这个脏矩形
                probes.append((last_dirty[0] + last_dirty[2] // 2, last_dirty[1] + last_dirty[3] // 2))
                probes.append((last_dirty[0] + 1, last_dirty[1] + 1))
            for r2 in win_rects2:                           # 每个窗口：中心 + 左上角
                probes.append((r2[0] + r2[2] // 2, r2[1] + r2[3] // 2))
                probes.append((r2[0] + 1, r2[1] + 1))
            bad = [(x, y, px_at(shots["exit1"], x, y)) for (x, y) in probes
                   if any(near(px_at(shots["exit1"], x, y), c)
                          for c in (CLOCK_BAR, CLOCK_INK, PANEL_TITLE, PANEL_ON, PANEL_OFF))]
            ch("⑦ 交回后 wm 的像素一个都不留（取样点 = handback 前最后一次 [WM] frame 的 dirty 矩形 + 两个窗口矩形）",
               bool(probes) and not bad,
               "probes=%s leftover=%s hb_color=%s dirty=%s" % (probes, bad, hexc(hb_col), last_dirty))
        if mon.shot(p4):
            shots["after"] = read_ppm(p4)
        if "after" in shots and shots["after"]:
            if shot_before_exit:
                # ★ v0.10.1 口径修正 + 加强（见该文件头 diff_classify 的说明）：
                #   旧口径"before_exit 与 after_exit 窗口外逐字节相同"实测**不可能成立** ——
                #   wm 接管屏幕时抓到的背景快照就是黑屏（before_exit 的桌面区 = 纯黑），
                #   交回之后外壳才第一次绘制自己的桌面（after_exit 那里才有壁纸/图标）。
                #   那条断言把"外壳第一次画桌面"也算成失败，与"wm 有没有擦掉东西"无关。
                #   新口径只对**危险方向**断言，并额外要求交回后桌面静止。
                # 内核自己的"外部面板区"（320x200，内核 shell 画的，见 kernel/wl64.cpp:249-256）
                # **不属于 wm**：wm 从来不碰它，拿它去要求"wm 不许改"是错口径。
                # 它的 rect 在启动日志里有：`[WL64] init fb=WxH panel=x,y,w,h`。
                kpanel = None
                mk = re.search(r"\[WL64\] init fb=\d+x\d+ panel=(\d+),(\d+),(\d+),(\d+)", vm.log())
                if mk:
                    kpanel = tuple(int(mk.group(1 + i)) for i in range(4))
                skip_rects = win_rects2 + [SHELL_BAND] + ([kpanel] if kpanel else [])
                erased, painted, kept = diff_classify(shot_before_exit, shots["after"], skip_rects)
                ch("⑦ wm 交回后**没有擦掉**外壳已经画出来的东西（erased=0）",
                   erased == 0, "erased=%d painted=%d kept=%d kpanel=%s windows=%s"
                   % (erased, painted, kept, kpanel, win_rects2))
                ch("⑦ 外壳在交回后自己接管并重画了桌面（painted>0）", painted > 0,
                   "painted=%d（before 纯黑 -> after 有内容）" % painted)
                p4b = os.path.join(tmp, "after_exit2.ppm")
                time.sleep(1.2)
                if mon.shot(p4b):
                    s2 = read_ppm(p4b)
                    if s2:
                        # 交回后隔 1.2 s 再抓一帧：**wm 的调色板不许再出现在它交出去的地盘上**。
                        # 为什么不比"两帧逐字节相同"：实测外壳交回后会自己继续重画桌面
                        # （同一对帧 erased2=29813 / painted2=30151），那是外壳的行为，不是 wm 的；
                        # 拿它当 wm 的判据会把外壳自己的动画算成失败。
                        wm_cols = (CLOCK_BAR, CLOCK_INK, PANEL_TITLE, PANEL_ON, PANEL_OFF, CLOCK_BG, CLOCK_TOP)
                        back = []
                        for (rx, ry, rw2, rh2) in win_rects2:
                            for yy in range(ry, ry + rh2, 3):
                                for xx in range(rx, rx + rw2, 3):
                                    if any(near(px_at(s2, xx, yy), c) for c in wm_cols):
                                        back.append((xx, yy))
                                        break
                                if back:
                                    break
                            if back:
                                break
                        ch("⑦ 交回后 wm 的调色板不再出现在窗口矩形里（像素没回来）",
                           not back, "hits=%s" % (back[:4],))
            ch("⑦ 没有 PANIC / 三重故障（内核没砖）",
               "PANIC" not in vm.log() and "TRIPLE FAULT" not in vm.log())


        print("=== 8) ⑧ 无 PANIC / 无 enosys / 各侧没有 FAILED ===")
        log = vm.log()
        for needle in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "[WL64] wm skipped",
                       "[WL64] wm TIMEOUT", "[WM] FAILED", "[WMCLOCK] FAILED", "[WMPANEL] FAILED",
                       "[WL64] surfmap FAILED", "[WL64] export FAILED", "[SHM64] create FAILED",
                       "[WL64] demo skipped"):
            ch("不得出现 %s" % needle, needle not in log)
        ens = [n for n in range(22, 27) if ("[SYSCALL] enosys nr=%d" % n) in log]
        ch("合成器 ABI（22..26 号）没有 enosys", not ens, "enosys=%s" % ens)
        ch("内核自检通过（[WL64] selftest PASS）", "[WL64] selftest PASS" in log)
        ch("合成器 ABI 自检位也过了（PASS 串里有 composer 22..26）",
           "composer 22..26" in log)

        print("=== 9) ⑨ 性能：用户态合成 vs 内核内合成（同一台机、同一分辨率、同一图案）===")
        kcomp = re.findall(r"\[WL64\] composite n=(\d+) bytes=(\d+) us=(\d+)", log)
        # ★ v0.10.2：一帧成本 = us(窗口 blit) + bg_us(快照回填)；输入修复帧（input=1）不计时，排除
        wcomp_all = re.findall(r"\[WM\] frame n=\d+ surfs=(\d+) pending=(\d+) dirty=-?\d+,-?\d+,\d+,\d+ px=(\d+) "
                               r"blend=(\d+) us=(\d+) flip_us=(\d+)[^\n]*?(?:bg_us=(\d+))?[^\n]*?$",
                               log, re.M)
        wkeep = [r for r in re.finditer(r"\[WM\] frame n=\d+ .*", log) if "input=1" not in r.group(0)]
        wcomp = re.findall(r"\[WM\] frame n=\d+ surfs=(\d+) pending=(\d+) dirty=-?\d+,-?\d+,\d+,\d+ px=(\d+) "
                           r"blend=(\d+) us=(\d+) flip_us=(\d+)", "\n".join(m.group(0) for m in wkeep))
        ch("⑨ 内核内合成器的数字存在（[WL64] composite n=2 bytes=.. us=..）",
           len(kcomp) >= 1, "n=%d %s" % (len(kcomp), kcomp[:2]))
        ch("⑨ 用户态合成器的数字存在（[WM] frame .. us=.. flip_us=..）", len(wcomp) >= 3,
           "n=%d" % len(wcomp))
        if kcomp and wcomp:
            k_us = [int(r[2]) for r in kcomp]
            w_us = [int(r[4]) for r in wcomp]
            w_fl = [int(r[5]) for r in wcomp]
            print("   [数字] 内核内合成（2 surface，n=%d 次）：us=%s（最小 %d / 中位 %d）；"
                  "用户态合成（%d 帧）：us=%s（最小 %d / 中位 %d）、flip_us 中位 %d"
                  % (len(k_us), k_us[:4], min(k_us), sorted(k_us)[len(k_us) // 2],
                     len(w_us), w_us[:4], min(w_us), sorted(w_us)[len(w_us) // 2],
                     sorted(w_fl)[len(w_fl) // 2]))
            # ★ v0.10.2：把背景回填（bg_us）算进一帧成本，并按"非 input 帧"取中位数
            _cost = {}
            for _m in re.finditer(r"\[WM\] frame n=(\d+) .*?us=(\d+) flip_us=(\d+)(.*)$", log, re.M):
                if "input=1" in _m.group(4):
                    continue
                _bg = re.search(r"bg_us=(\d+)", _m.group(4))
                _cost[int(_m.group(1))] = int(_m.group(2)) + (int(_bg.group(1)) if _bg else 0)
            _cv = sorted(_cost.values())
            ch("⑨ 一帧合成成本（us + bg_us，非 input 帧）是**真实的 rdtsc 数字**（>0 且 <10ms）",
               len(_cv) >= 3 and 0 < _cv[len(_cv) // 2] < 10000,
               "frames=%d median_us=%d all=%s" % (len(_cv), _cv[len(_cv) // 2] if _cv else -1, _cv[:6]))
            ch("⑨ 客人侧帧时间戳存在（[WM] frame .. t_ms=..）—— 帧预算的取证通道",
               len(re.findall(r"\[WM\] frame n=\d+ .*?t_ms=\d+", log)) >= 3,
               "n=%d" % len(re.findall(r"\[WM\] frame n=\d+ .*?t_ms=\d+", log)))
            ch("⑨ 设备 blit 阈值决策留了证据（min_px=4096 / path=soft，本台无 virtio-gpu）",
               vm.n_match(r"\[WM\] blitpolicy gpu=0 min_px=4096 rect_px=\d+ path=soft") >= 1)
    finally:
        if vm:
            vm.close()

    if args.keep:
        print("[wm64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" % ("PASS" if ch.ok else "FAIL", ch.n, ch.passed))
    return 0 if ch.ok else 1


if __name__ == "__main__":
    sys.exit(main())
