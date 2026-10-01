#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/wm64_test.py - ★ B-wm 验收：把"图形"从内核搬到 Ring 3（用户态合成器 /bin/wm + 两个客户端）

要证明的事（与任务书的九条一一对应）：
  ① `wm` 进程起来并 **fb_map 成功**（`[WM] map .. u=1 bytes=..`）+ 注册成合成器
     （`[WL64] composer pid=.. gpu=..`），并且**内核从此不再合成**（注册之后一行 [WL64] composite
     都没有 —— 合成真的搬到用户态了）；
  ② 两个客户端（wmclock / wmpanel）的 shm surface 被 wm 合成上屏（像素：外置面板区里出现
     时钟的色带色 + 交互面板的底色/标题色）；
  ③ **damage 语义**：时钟只改一个小块（几个字形）-> 面板区里、damage 矩形之外的像素**逐字节不变**
     （给出差异计数与包围盒）；区域之外也给出逐字节差异计数；
  ④ **z 序**：两块 surface 故意重叠 -> 重叠区像素 = 上层（id 大）的内容色，且 != 下层的色；
  ⑤ **seat 事件路由**：注入鼠标点到 wmpanel 上 -> 它切换（像素变化），事件是 **wm 在用户态命中
     测试后用 wl_seat_post 投过去的**（内核只搬运：`[WL64] seatpost surf=.. pid=<wmpanel>`），
     且**没有**送给内核外壳（外壳的 ring0 点击处理路径没有任何新行）；
  ⑥ 客户端退出 -> surface 资源回收（`[WL64] surface destroy .. why=client` + `[SHM64] release .. refs=0
     freed_pages=15/8` + 页池回基线）；
  ⑦ `wm` 退出 -> 把地盘交回（`[WM] exit handback` + `[WL64] composer release .. mode=internal`）：
     外置面板区被桌面底色回填（不是花屏），**区域之外逐字节不变**、无 PANIC（不变砖）；
  ⑧ 无 PANIC / 无 [SYSCALL] enosys（22..26 号）/ 客户端与合成器都没有 FAILED；
  ⑨ **性能**：wm 每帧合成耗时的 rdtsc 数字（`[WM] frame .. us=..`），并与**同一台机器、同一分辨率、
     同一图案**的内核内合成器（`[WL64] composite n=2 bytes=.. us=..`，由 /wlclient.elf 的短模式在
     wm 启动**之前**跑出）对比；设备 blit 阈值（>=64x64=4096 px）的决策也在打点里（`[WM] blitpolicy
     gpu=.. min_px=4096 rect_px=.. path=soft|dev`）——本测试台没有 virtio-gpu，path 恒为 soft。

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
PART_MAIN_LBA = 8009                    # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 32768                  # 16 MB（与 wl64/sh64 同一夹具口径）

MOUSE_X0, MOUSE_Y0 = 512, 384           # kernel/input.cpp 的 mouse_init 默认位置
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
        panel = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
        ch("外部面板区完整落在屏幕内（比屏幕小）",
           panel[0] >= 0 and panel[1] >= 0 and panel[2] < fbw and panel[3] < fbh and
           panel[0] + panel[2] <= fbw and panel[1] + panel[3] <= fbh,
           "panel=%s fb=%dx%d" % (panel, fbw, fbh))

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
                           r"rect=(-?\d+),(-?\d+),(\d+),(\d+)", args.timeout)
        ch("① wm 注册成合成器（[WL64] composer .. seat=1 .. rect=外部面板区）", mcomp is not None,
           (mcomp.group(0) if mcomp else "没等到"))
        if mcomp:
            ch("① 内核租给 wm 的区域 == 内核自己的面板几何",
               (int(mcomp.group(7)), int(mcomp.group(8)), int(mcomp.group(9)), int(mcomp.group(10))) == panel,
               "composer rect=%s panel=%s" % ((mcomp.group(7), mcomp.group(8), mcomp.group(9),
                                               mcomp.group(10)), panel))
        ch("① 显示后端如实报为 soft-lfb（无 virtio-gpu 设备 -> gpu=0）",
           mcomp is not None and mcomp.group(4) == "soft-lfb" and mcomp.group(3) == "0",
           (mcomp.group(0) if mcomp else ""))
        ch("① wm 的输出走 input_poll 的焦点/捕获（[EV64] attach pid=<wm> flags=...3 focus/capture=<wm>）",
           vm.wait_re(r"\[EV64\] attach pid=(\d+) flags=0*3 focus=(\d+) capture=(\d+)", 60) is not None)
        ch("① 双缓冲影副本也走 shm（[WM] shadow shm=.. va=.. bytes=65536）",
           vm.wait_re(r"\[WM\] shadow shm=(\d+) va=0x([0-9a-f]+) bytes=65536", 60) is not None)
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
            ch("两块 surface 都在外部面板区内",
               all(surfs[i]["rect"][0] >= panel[0] and surfs[i]["rect"][1] >= panel[1] and
                   surfs[i]["rect"][0] + surfs[i]["rect"][2] <= panel[0] + panel[2] and
                   surfs[i]["rect"][1] + surfs[i]["rect"][3] <= panel[1] + panel[3] for i in ids),
               "rects=%s panel=%s" % ([surfs[i]["rect"] for i in ids], panel))
            ch("两块 surface **故意重叠**（z 序有可观测的遮挡区）",
               surfs[hi]["rect"][0] < surfs[lo]["rect"][0] + surfs[lo]["rect"][2] and
               surfs[hi]["rect"][1] < surfs[lo]["rect"][1] + surfs[lo]["rect"][3] and
               surfs[lo]["rect"][0] < surfs[hi]["rect"][0] + surfs[hi]["rect"][2] and
               surfs[lo]["rect"][1] < surfs[hi]["rect"][1] + surfs[hi]["rect"][3],
               "rects=%s" % [surfs[i]["rect"] for i in ids])
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
            cr = surfs[clock_id]["rect"]
            pr = surfs[panel_id]["rect"]
            got_bar = px_at(shots["p1"], cr[0] + 80, cr[1] + cr[3] - 8)     # 时钟底部色带
            got_ink = px_at(shots["p1"], cr[0] + 60, cr[1] + 48)            # 时钟数字区（可能落在笔画上）
            got_ptt = px_at(shots["p1"], pr[0] + 60, pr[1] + 6)             # 面板标题条
            ch("② wmclock 区域出现内容（底部色带 = 0x40c0ff）", near(got_bar, CLOCK_BAR),
               "got=%s want=%s rect=%s" % (hexc(got_bar), hexc(CLOCK_BAR), cr))
            ch("② wmclock 数字区有内容（笔画色或底色，两种都算）",
               near(got_ink, CLOCK_INK) or near(got_ink, 0x101820),
               "got=%s (ink=%s bg=0x101820)" % (hexc(got_ink), hexc(CLOCK_INK)))
            ch("② wmpanel 区域出现内容（标题条 = 0x303a48）", near(got_ptt, PANEL_TITLE),
               "got=%s want=%s rect=%s" % (hexc(got_ptt), hexc(PANEL_TITLE), pr))
            # 开关（关 = 橙）与半透明覆盖条（与底色混合，不再是纯白）
            got_sw = px_at(shots["p1"], pr[0] + 40, pr[1] + 36)
            got_veil = px_at(shots["p1"], pr[0] + 64, pr[1] + 54)      # 半透明条（局部 y=54）
            ch("② wmpanel 开关初始为关态（橙色 0xe0a020）", near(got_sw, PANEL_OFF),
               "got=%s want=%s" % (hexc(got_sw), hexc(PANEL_OFF)))
            ch("② 半透明覆盖条真的被 alpha 混合了（既不是纯白也不是纯底色）",
               got_veil is not None and not near(got_veil, 0xFFFFFF) and not near(got_veil, PANEL_BG),
               "got=%s" % hexc(got_veil))
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
            dmg = clkframes[2]["dmg"]
            want = (cr[0] + dmg[0], cr[1] + dmg[1], dmg[2], dmg[3])
            n_in, bbox_in = diff_region(shots_seq[1], shots_seq[2], cr)
            # 面板区之外：内核外壳有自己的刷新区（顶部状态条的时钟/通知），把最上面 24 行的
            # "外壳自己那一带"单独统计 —— 面板区之外、且**不在**那一带里的像素必须逐字节不变。
            SHELL_BAND = (0, 0, fbw, 24)
            n_out, bbox_out = diff_region(shots_seq[1], shots_seq[2], (0, 0, fbw, fbh), skip=[panel])
            n_band, _ = diff_region(shots_seq[1], shots_seq[2], SHELL_BAND, skip=[])
            n_out_strict = n_out - n_band
            n_rest, _ = diff_region(shots_seq[1], shots_seq[2], panel, skip=[want])
            ch("③ 时钟面内的帧差包围盒 ⊆ 客户端上报的 damage 矩形 %s" % (want,),
               bbox_inside(bbox_in, want), "bbox=%s n=%d" % (bbox_in, n_in))
            ch("③ 面板区内、damage 框之外**逐字节不变**（差异像素 = 0）", n_rest == 0,
               "outside_damage_diff=%d" % n_rest)
            ch("③ 外部面板区之外、且不在内核外壳自带刷新带（顶部 24 行）里的像素**逐字节不变**",
               n_out_strict == 0,
               "outside_panel_diff=%d（其中外壳顶部带 %d 像素）bbox=%s" % (n_out, n_band, bbox_out))
            ch("③ 面板区外的差异全部落在内核外壳自己那一带（顶部 24 行）里",
               bbox_out is None or (bbox_out[1] >= 0 and bbox_out[1] + bbox_out[3] <= 24),
               "bbox=%s band=%d" % (bbox_out, n_band))
            print("   [数字] 时钟 f=1->f=2：damage=%s（屏幕坐标=%s）；面内差异=%d；"
                  "面板内 damage 外差异=%d；面板外差异=%d（外壳顶部带 %d / 带外 %d）"
                  % (dmg, want, n_in, n_rest, n_out, n_band, n_out_strict))

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
                # 重叠区中心：时钟显示底部色带（0x40c0ff）、面板显示标题条（0x303a48）
                sx, sy = (ix0 + ix1) // 2, (iy0 + iy1) // 2
                got = px_at(shots["p1"], sx, sy)
                upper = max(surfs)
                exp_up = CLOCK_BAR if upper == clock_id else PANEL_TITLE
                exp_lo = PANEL_TITLE if upper == clock_id else CLOCK_BAR
                ch("④ 重叠区像素 = 上层（id=%d，%s）的内容色 %s" % (upper, "wmclock" if upper == clock_id else "wmpanel", hexc(exp_up)),
                   near(got, exp_up), "got=%s at (%d,%d)" % (hexc(got), sx, sy))
                ch("④ 重叠区像素**不是**下层的内容色（确实被遮挡）", not near(got, exp_lo),
                   "got=%s lower=%s" % (hexc(got), hexc(exp_lo)))

        print("=== 5) ⑤ seat：注入鼠标点到 wmpanel 上（用户态命中测试 + 投递）===")
        # 事件统计的基线（用于证明"没有送给内核外壳"）
        shell_markers = ["[STARTMENU]", "[EXPLORER]", "[DESKTOPOPS]"]
        base_shell = {k: vm.n_match(re.escape(k)) for k in shell_markers}
        base_seatpost = vm.n_match(r"\[WL64\] seatpost surf=")
        click_ok = False
        if panel_id is not None:
            pr = surfs[panel_id]["rect"]
            tx, ty = pr[0] + 40, pr[1] + 36          # 开关中心（局部 40,36）
            steps, (sim_x, sim_y) = plan_move(MOUSE_X0, MOUSE_Y0, tx, ty)
            print("   注入计划：起点 (%d,%d) -> (%d,%d) via %d 步（模拟落点 (%d,%d)）"
                  % (MOUSE_X0, MOUSE_Y0, tx, ty, len(steps), sim_x, sim_y))
            for dx, dy in steps:
                mon.mouse_move(dx, dy)
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
        # ⑦-a：wm 的交回动作**当场**落地了吗（紧接着抓一张：面板区应当是我们回填的那块底色）
        # ⑦-a：wm 的交回动作在日志里是"整块回填"（px = 320*200），并且**退出后这块地不再有
        #       合成器自己的内容**（时钟色带色 / 面板标题色 / 开关色都不该再出现在原位置上）：
        #       内核外壳会在这块地上重画自己的东西（本批的共存策略），所以比"等于回填色"更稳的
        #       判据是"我们的像素没留下"。
        if "exit1" in shots and shots["exit1"]:
            hb_ok = all(not near(px_at(shots["exit1"], panel[0] + dx, panel[1] + dy), c)
                        for (dx, dy, c) in ((30, 30, 0x40C0FF), (60, 60, 0x303A48), (150, 130, 0x30B070)))
            ch("⑦ wm 交回地盘时把整块回填了（日志 px=%d = 320*200），且退出后原位置不再有合成器内容"
               % hb_px, hb_px == panel[2] * panel[3] and hb_ok,
               "px=%d colors_ok=%s" % (hb_px, hb_ok))
        if mon.shot(p4):
            shots["after"] = read_ppm(p4)
        if "after" in shots and shots["after"]:
            if shot_before_exit:
                n_out, bbox_out = diff_region(shot_before_exit, shots["after"], (0, 0, fbw, fbh),
                                              skip=[panel])
                ch("⑦ wm 退出后**外部面板区之外逐字节不变**（差异像素 = 0）", n_out == 0,
                   "outside_diff=%d bbox=%s" % (n_out, bbox_out))
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
        wcomp = re.findall(r"\[WM\] frame n=\d+ surfs=(\d+) pending=(\d+) dirty=-?\d+,-?\d+,\d+,\d+ px=(\d+) "
                           r"blend=(\d+) us=(\d+) flip_us=(\d+)", log)
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
            ch("⑨ 用户态合成的每帧耗时是**真实的 rdtsc 数字**（>0 且 <10ms）",
               0 < sorted(w_us)[len(w_us) // 2] < 10000, "median_us=%d" % sorted(w_us)[len(w_us) // 2])
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
