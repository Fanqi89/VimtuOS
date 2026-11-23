#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/viewer64_test.py - ★ v0.10.5：图片查看器 + 全量文件关联路由 端到端验收

两条完全独立的证据链，都对"真文件、真窗口、真像素"负责：

  A. **图片查看器**（终端驱动，不碰鼠标、不看运气）
     `run /bin/viewer <图>` 依次打开四个真文件，逐个验：
       * 读盘：`[VIEWER] read ok bytes=<真字节数> magic=<png|jpeg|bmp>`
       * 解码：`[VIEWER] decode ok w=128 h=96 comp=3`（stb_image 真解出来的尺寸）
       * 几何：128x96 -> `win w=130 h=117 … mode=1:1`；1600x1200 -> `mode=fit` 且
         窗口 = 762x541（按比例塞进 760x520 的图像区）
       * 上屏：内核 `[WL64] surface create … pid=<viewer 的 pid> … pos=x,y`（surface 真进了表）
       * **像素**：screendump 在窗口矩形里数"左半纯红 / 右半纯蓝"——**解码错、通道 BGR/RGB
         互换、把图当文本开**，这三种常见错都会让这条立刻红
       * 收尾：ESC -> `[VIEWER] done frames=<n> reason=esc`

  B. **文件关联路由**（文件管理器里真实点击 + 回车，逐条对应用户的 6 条规则）
       .png/.jpg/.bmp -> `route=image` + `route spawn app=/bin/viewer`
       .txt           -> `route=text`  + `[UI] explorer preview name=…`（文本编辑器）
       .bin/.so/.o    -> `route=binary`，**不打开**（无 preview、无 spawn）
       .mp3           -> `route=unknown`，**不打开**（且保留 `[UI] explorer noassoc …` 老打点）
       .exe           -> `route=pe`    + `route spawn app=/bin/peload`
       .deb           -> `route=deb`   + `route spawn app=/bin/store`（包管理器）

鼠标口径（为什么能不用校准就点得准）：
  先用 70 次 `mouse_move(-100,-100)` 把光标**顶到 (0,0)**（kernel/input.cpp 会钳下界），
  于是位置是**已知**的；之后按 `dx*17/10` 的模型走一程，再单击一次读回 explorer 自己打的
  `[UI] explorer click x=.. y=.. sx=.. sy=.. hit=..` —— 那是**真值**，用它纠偏，最多 3 轮。
  选中条目**靠这次点击本身**（不靠按几次下键），所以零键盘计数、零状态耦合。

用法：py -3 tests\\viewer64_test.py [--qemu 路径] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题。
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
import qemuhelp as qh                                            # noqa: E402（公共登录手势）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
DISK_SRC = os.path.join(ROOT, "build64", "sysdisk.img")
VIEWER_ELF = os.path.join(ROOT, "build64", "viewer.elf")

# ---- 与 kernel/explorer64.cpp + gui64 的窗口装饰一致的几何常量（同 tests/explorer64_test.py）----
WIN_X, WIN_Y = 120, 60
CL_X, CL_Y = WIN_X + 1, WIN_Y + 25          # 客户区原点
TOOLBAR_H, ADDR_H, NAV_W, STATUS_H = 26, 28, 150, 22
CONTENT_X, CONTENT_Y = NAV_W + 1, TOOLBAR_H + ADDR_H
CONTENT_W = 660 - 2 - CONTENT_X
CARD_TOP, CARD_X, CARD_W, CARD_H, CARD_GAP = CONTENT_Y + 30, CONTENT_X + 10, CONTENT_W - 20, 78, 10
ICON_CELL_W, ICON_CELL_H = 96, 74
ICON_COLS = CONTENT_W // ICON_CELL_W

# 查看器配色（user/viewer/viewer.c 的 CO_FRAME / CO_BG / 1px 边框）
CO_FRAME = (0x3A, 0x46, 0x58)     # ★ 必须与 kernel 面板底色 0x20242A 区分（见 viewer.c）
CO_BG = (0x14, 0x18, 0x1F)

KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus",
          "_": "shift-minus", ">": "shift-dot", "<": "shift-comma",
          "=": "equal", ";": "semicolon", chr(39): "apostrophe"}


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
            f = shutil.which(c)
            if f:
                return f
    return None


# ---------------------------------------------------------------------------
class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.30):
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

    def key(self, name, wait=0.7):
        self.send("sendkey %s" % name, wait=wait)

    def move(self, dx, dy, wait=0.045):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def click(self, wait=0.45):
        self.send("mouse_button 1", wait=0.09)
        self.send("mouse_button 0", wait=wait)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n = os.path.getsize(ppm)
                time.sleep(0.12)
                if os.path.getsize(ppm) == n:
                    return True
            time.sleep(0.05)
        return False


class Vm:
    def __init__(self, qemu, img, port, serial, name, timeout=240):
        self.serial = serial
        self.timeout = timeout
        if os.path.exists(serial):
            os.remove(serial)
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(200):
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

    def n(self):
        return len(self.log())

    def wait_re(self, rx, timeout=None, since=0):
        timeout = timeout or self.timeout
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log()[since:])
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.2)
        return None

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:                                  # noqa: BLE001
                pass


def type_line(mon, text, per_key=0.09):
    """逐键打字。★ 大写字母必须走 `shift-<小写>`：QEMU 的 sendkey 只认小写键名，
    直接发 `sendkey P` 会被**静默丢弃**（实测 `/Pictures/x.png` 变成 `/ictures/x.png`，
    查看器于是 reason=open 打不开 —— 这条坑花了一整轮才定位）。"""
    for ch in text:
        if ch in KEYMAP:
            mon.key(KEYMAP[ch], wait=per_key)
        elif ch.isupper():
            mon.key("shift-" + ch.lower(), wait=per_key)
        elif ch.islower() or ch.isdigit():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.3)


# ---------------------------------------------------------------------------
def read_ppm(path):
    d = open(path, "rb").read()
    parts, i = [], 0
    while len(parts) < 4:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while i < len(d) and d[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    return w, h, d[i + 1:i + 1 + w * h * 3]


def count(px, w, h, rect, fn):
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(max(0, y0), min(y1, h)):
        base = (y * w) * 3
        for x in range(max(0, x0), min(x1, w)):
            o = base + x * 3
            if fn(px[o], px[o + 1], px[o + 2]):
                n += 1
    return n


def is_red(r, g, b):
    return r >= 180 and g <= 80 and b <= 80


def is_blue(r, g, b):
    return b >= 180 and r <= 80 and g <= 80


def is_frame(r, g, b, tol=10):
    return abs(r - CO_FRAME[0]) <= tol and abs(g - CO_FRAME[1]) <= tol and abs(b - CO_FRAME[2]) <= tol


# ---------------------------------------------------------------------------
# 鼠标：先顶到 (0,0)，再按 dx*17/10 的模型走；单击读回真值纠偏
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 鼠标：内核累加器模型的精确位移 + "真值打点"闭环纠偏
#   kernel/input.cpp：accum += dx*17/10（上限 ±96）；每包最多走 MOUSE_STEP=24，剩余留到
#   下一包。于是"1 个 mouse_move 100 包"只走 24px 并留 72 残余 —— 必须分段，再补一个
#   -43 的包把残余清掉；小包（|r|<=14）位移 = floor(1.7r) 且不留残余。
#   （直接拿 ±14 小包按"1.7 倍"算，在 2 fps 下误差能到 ±200px：实测点到了桌面空白，
#    串口里出现 [UI] selbox 而不是 [UI] explorer click。）
# ---------------------------------------------------------------------------
MOUSE_STEP = 24
MOUSE_BRAKE = 43
CLICK_RE = re.compile(r"\[UI\] explorer click x=(\d+) y=(\d+) sx=(-?\d+) sy=(-?\d+) hit=(\S+)")
SELBOX_RE = re.compile(r"\[UI\] selbox x0=(\d+) y0=(\d+)")


def last_probe(vm, since):
    """最近一次**真实光标位置**：优先 explorer 点击打点（真值），其次桌面选择框打点。

    ★ 坐标对口径：[UI] explorer click x=<客户区> y=<客户区> sx=<屏幕> sy=<屏幕>
      （实测 702-581=121、381-296=85 = 客户区原点）。所以屏幕坐标是 sx/sy。
    """
    log = vm.log()[since:]
    pos, hit = None, ""
    for m in CLICK_RE.finditer(log):
        pos, hit = (int(m.group(3)), int(m.group(4))), m.group(5)
    if pos is None:
        for m in SELBOX_RE.finditer(log):
            pos, hit = (int(m.group(1)), int(m.group(2))), "desktop"
    return (pos[0], pos[1], hit) if pos else None


def move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.send("mouse_move %d 0" % (mag if d > 0 else -mag), wait=0.18)
    else:
        mon.send("mouse_move 0 %d" % (mag if d > 0 else -mag), wait=0.18)


def move_px(mon, axis, d, px):
    """按内核模型精确移动 |px| 像素（方向 d=±1），结束时残余清零。"""
    if px < 1 or d == 0:
        return
    n = min(48, px // MOUSE_STEP)
    rem = px - n * MOUSE_STEP
    for _ in range(n):
        move_axis(mon, axis, d, 100)
    move_axis(mon, axis, -d, MOUSE_BRAKE)          # 刹车：清掉 72 的残余
    if rem >= 2:
        mag = int(round(rem / 1.7))
        if mag >= 1:
            move_axis(mon, axis, d, mag)


def park_cursor(mon, times=58):
    """把光标顶到 (0,0)（内核钳下界）—— 拿到一个确定的起点。"""
    for _ in range(times):
        mon.send("mouse_move -100 -100", wait=0.06)
    time.sleep(0.6)


def park(mon):
    park_cursor(mon)


def single_click(mon):
    """按下-抬起一次。与上一次点击至少隔开 park（3.5s），绝不误触发双击。"""
    mon.send("mouse_button 1", wait=0.12)
    mon.send("mouse_button 0", wait=0.5)
    time.sleep(0.5)


class Pad:
    """把光标走到屏幕绝对坐标 (tx,ty) 并单击；用真实打点闭环纠偏（5 轮内收敛到 ±4px）。

    want_hit 不为 None 时还要求这一次点击**真的命中了它**（'card:' / 'item:' 前缀）——
    只靠"坐标差不多"不够（可能落在别的窗口上，那连打点都不会有）。
    """

    def __init__(self, vm, mon):
        self.vm = vm
        self.mon = mon
        self.dxe = 0
        self.dye = 0

    def to(self, tx, ty, rounds=5, tol=4, want_hit=None):
        for _ in range(rounds):
            park_cursor(self.mon)
            move_px(self.mon, 0, +1, max(0, tx + self.dxe))
            move_px(self.mon, 1, +1, max(0, ty + self.dye))
            time.sleep(0.35)
            since = self.vm.n()
            single_click(self.mon)
            p = last_probe(self.vm, since)
            if not p:
                continue
            gx, gy, hit = p
            self.dxe += tx - gx
            self.dye += ty - gy
            near = abs(gx - tx) <= tol and abs(gy - ty) <= tol
            if want_hit and not hit.startswith(want_hit):
                near = False
            if near:
                return True
        return False


def point_over(mon, tx, ty):
    """把指针挪到屏幕 (tx,ty) 并点一下（不需要探针：查看器/预览窗口不打 click）。

    点一下是必要的：内核只在"指针命中 surface"时把键盘焦点给它，ESC 才送得到。
    """
    park_cursor(mon)
    move_px(mon, 0, +1, max(0, tx))
    move_px(mon, 1, +1, max(0, ty))
    time.sleep(0.3)
    single_click(mon)


def esc_close_viewer(vm, mon, since, tries=2):
    """按 ESC 关查看器，**一次只按一下**，等不到 reason=esc 才补一次。

    ★ 一次只按一下（等不到 reason=esc 才补一次），理由：
      一次 ESC 会被**两条通道**同时投递 —— EV64 捕获者（/lib/wm.elf）转发给 wl 客户端，
      gui64 的 kbd 环又投给最前窗口的 on_key（terminal64 的 term_key）。v0.10.5 之前
      term_key 把 ESC 一律当"关闭本窗口" -> term_release_state() -> sh64_stop64() 把终端
      连同 ring3 外壳杀掉（实测 [SH64] exit pid=26 + [APP] term closed，之后敲什么都没人执行）。
      内核已改成"shell 模式下 ESC 归 shell"（真终端语义）；测试这边仍然只按一次 ——
      按两次没有任何好处，而且一旦内核行为回退就会把终端关掉。
    """
    for _ in range(tries):
        mon.key("esc", wait=1.0)
        if vm.wait_re(r"\[VIEWER\] done frames=\d+ reason=esc", 8, since=since) is not None:
            return True
    return False


def new_terminal(mon, vm):
    """点 dock 的终端图标开一个**新**终端窗口，并等它起来。

    为什么每张图都要新开：查看器（以及它拉起的 /lib/wm.elf 合成器）会把键盘焦点带到 wl 那边；
    它退出后内核打 `[EV64] focus pid=0 … reason=capture-release`，焦点**不会**回到原终端窗口，
    后续按键就全丢了。新窗口自带焦点 —— 这也是真机上用户的做法（再点一次终端图标）。
    坐标取自 `[DOCK64] item idx=3 app=3 name=终端 cx=583 cy=754`。"""
    n0 = vm.n()
    point_over(mon, 583, 754)
    return vm.wait_re(r"\[TERM\] opened inst=(\d+)", 15, since=n0) is not None


def _shell(mon, vm):
    """在当前焦点窗口（内核终端）里输入 shell，等 ring3 外壳起来；返回那个匹配对象。"""
    n0 = vm.n()
    type_line(mon, "shell", per_key=0.12)
    return vm.wait_re(r"\[SH64\] launch path=/bin/shell\.bin size=(\d+) pid=(\d+)", 40, since=n0)




def sx(cx):
    return CL_X + cx


def sy(cy):
    return CL_Y + cy


def card_center(idx):
    return (sx(CARD_X + 300), sy(CARD_TOP + idx * (CARD_H + CARD_GAP) + CARD_H // 2))


def icon_cell(idx, scroll=0):
    k = idx - scroll
    col, row = k % ICON_COLS, k // ICON_COLS
    return (sx(CONTENT_X + col * ICON_CELL_W + ICON_CELL_W // 2),
            sy(CONTENT_Y + 2 + row * ICON_CELL_H + ICON_CELL_H // 2))


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5621)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except Exception:                                  # noqa: BLE001
            pass

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for p in (DISK_SRC, VIEWER_ELF):
        if not os.path.exists(p):
            sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % p)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_viewer_")
    # ★ 绝不直接启动构建产物（它会被写回）：先复制一份再启动
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(DISK_SRC, disk)
    print("[viewer64] 夹具盘：%s（从 build64/sysdisk.img 复制，%d B）" % (disk, os.path.getsize(disk)))

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    vm = None
    try:
        vm = Vm(qemu, disk, args.port, os.path.join(tmp, "serial.log"), "vimtu-viewer", args.timeout)
        mon = Monitor(args.port)
        qh.login_desktop(mon, vm.log, vm.proc, timeout=min(args.timeout, 200))
        check("桌面就绪 [GUI64] ready", vm.wait_re(r"\[GUI64\] ready", args.timeout) is not None)

        # ================= 阶段 0：终端 + ring3 外壳 =================
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if vm.wait_re(r"\[APP\] term opened", 12):
                opened = True
                break
        check("终端打开（[APP] term opened）", opened)
        sh = _shell(mon, vm)
        check("ring3 外壳起来了（[SH64] launch）", sh is not None,
              sh.group(0) if sh else "")

        # ================= 阶段 1：图片查看器（三个格式 + 缩放）=================
        # ★ 驱动模型的取向（上一轮踩出来的，别再改回去）：
        #   逐张来：先点一下终端窗口把它置顶（gui64 按 z 序把按键投给最前那个窗口的 on_key，
        #   点窗口会走 gui64_set_active -> z_push_front），再**整行打完才按回车**。
        #   为什么不能"一次 burst 敲 4 条"：竞态 —— 第一条回车一到（约 0.7 s）查看器就把顶层
        #   窗口占住，后 3 行按键于是进了 wl 客户端（它只认 ESC），实测 loads=1、后三张"没发生"。
        #   整行打完再回车时查看器还没启动，按键不可能被抢走。
        # (路径, 魔数, 解码宽, 解码高, mode, **图像显示尺寸**)
        # ★ fit 是保持宽高比的：1600x1200 塞进 760x520 的框，高先顶到 -> 520，
        #   宽 = 1600*520/1200 = 693（截断）。窗口尺寸由它推导（+2 边框 / +21 标题栏），
        #   不写死两个数字（那样"拉伸铺满"和"按比例"两种实现都能过 = 假绿）。
        IW_MAX, IH_MAX = 760, 520            # user/viewer/viewer.c 的显示框

        def fit_sz(w, h):
            """与 user/viewer/viewer.c 的 fit_sz 同一套整数口径（测试自己算，再与产品对账）。"""
            if w <= IW_MAX and h <= IH_MAX:
                return w, h
            a = IW_MAX * h
            b = IH_MAX * w
            if a <= b:
                return IW_MAX, a // w
            return b // h, IH_MAX

        IMAGES = (
            ("/Pictures/redblue.png", "png", 128, 96),
            ("/Pictures/redblue.jpg", "jpeg", 128, 96),
            ("/Pictures/redblue.bmp", "bmp", 128, 96),
            ("/Pictures/big.png", "png", 1600, 1200),
        )
        IMAGES = [(p, f, w, h, "1:1" if fit_sz(w, h) == (w, h) else "fit", fit_sz(w, h))
                  for (p, f, w, h) in IMAGES]
        n_stage1 = vm.n()
        ppm = os.path.join(tmp, "v1.ppm")
        results = {}

        def open_in_viewer(path, since, fmt, want_w, want_h, want_mode, want_img):
            mv = vm.wait_re(r"\[VIEWER\] ver=1 pid=(\d+) file=%s" % re.escape(path),
                            90, since=since)
            # ★ 自证：这一张**真的被启动了**（以防按键被别的窗口吃掉、根本没执行）
            loads = len(re.findall(r"\[ELF64\] load path=/bin/viewer\b", vm.log()[n_stage1:]))
            check("① %s：真的启动了查看器（[ELF64] load path=/bin/viewer 次数 >= %d）"
                  % (path, len(results) + 1), loads >= len(results) + 1, "loads=%d" % loads)
            if not mv:
                return None
            pid = int(mv.group(1))
            info = {"pid": pid, "n0": since, "magic": None, "bytes": 0}
            check("① %s：查看器起来了（[VIEWER] ver=1 pid=%d file=%s）" % (path, pid, path),
                  mv.group(0).endswith("ver=1 pid=%d file=%s" % (pid, path)), mv.group(0))
            mr = vm.wait_re(r"\[VIEWER\] read ok bytes=(\d+) magic=(\S+)", 25, since=since)
            if mr:
                info["magic"] = mr.group(2)
                info["bytes"] = int(mr.group(1))
            check("① %s：读盘 %d 字节且魔数=%s" % (path, info["bytes"], fmt),
                  mr is not None and mr.group(2) == fmt, mr.group(0) if mr else "")
            md = vm.wait_re(r"\[VIEWER\] decode ok w=(\d+) h=(\d+) comp=(\d+)", 30, since=since)
            check("① %s：stb_image 解码出 %dx%d" % (path, want_w, want_h),
                  md is not None and int(md.group(1)) == want_w and int(md.group(2)) == want_h,
                  md.group(0) if md else "")
            mw = vm.wait_re(r"\[VIEWER\] win w=(\d+) h=(\d+) img=(\d+)x(\d+) mode=(\S+)", 15,
                            since=since)
            # ★ `img=` 是**解码尺寸**；窗口尺寸 = fit_sz 出来的**显示尺寸** + (2*PAD, TITLE_H+PAD)。
            #   fit_sz 是整数口径（user/viewer/viewer.c）：a = IW_MAX*h, b = IH_MAX*w；
            #   a<=b -> (IW_MAX, a/w)，否则 -> (b/h, IH_MAX)。测试按同一式子复算，不写死数字。
            #   ★ v0.11.3：surface 尺寸 = **客户区**（28 号装饰的 25px 标题带画在 surface 之外），
            #   所以窗口尺寸就是 fit 尺寸本身（v0.11.2 是 +2/+21 的自绘标题栏与边框）。
            want_win = (want_img[0], want_img[1])   # ★ v0.11.3：画布 = 客户区（标题栏由合成器画，不在 surface 里）
            check("① %s：窗口几何 w=%d h=%d mode=%s（显示 %dx%d + 边框/标题栏；解码 %dx%d）"
                  % (path, want_win[0], want_win[1], want_mode,
                     want_img[0], want_img[1], want_w, want_h),
                  mw is not None
                  and (int(mw.group(1)), int(mw.group(2))) == want_win
                  and (int(mw.group(3)), int(mw.group(4))) == (want_w, want_h)
                  and mw.group(5) == want_mode, mw.group(0) if mw else "")
            ms = vm.wait_re(r"\[VIEWER\] surf id=(\d+) w=(\d+) h=(\d+) shm=(\d+) map=(\d+)", 15,
                            since=since)
            check("① %s：画布 + surface 就绪（map=0）" % path,
                  ms is not None and ms.group(5) == "0", ms.group(0) if ms else "")
            ma = vm.wait_re(r"\[VIEWER\] attach rc=(-?\d+)", 15, since=since)
            check("① %s：缓冲真挂上了 surface（[VIEWER] attach rc=0）" % path,
                  ma is not None and ma.group(1) == "0", ma.group(0) if ma else "")
            mse = vm.wait_re(r"\[VIEWER\] seat=(-?\d+) warm=(-?\d+)", 15, since=since)
            check("① %s：seat 绑上了（没有 seat 就收不到 ESC）" % path,
                  mse is not None and int(mse.group(1)) > 0, mse.group(0) if mse else "")
            # 内核 surface 表：必须是这个 pid 的、尺寸一致、带屏上坐标
            mk = vm.wait_re(r"\[WL64\] surface create id=\d+ w=(\d+) h=(\d+) shm=\d+ pid=%d "
                            r"px=\d+ fmt=\d+ slot=\d+ pos=(-?\d+),(-?\d+)" % pid, 20, since=since)
            check("① %s：窗口进了内核 surface 表（pid=%d）" % (path, pid), mk is not None,
                  mk.group(0) if mk else "")
            # ---- 屏上原点：**优先**取合成器自报的真实矩形 ----
            # 为什么要这样：/lib/wm.elf 活着时它是外部合成器，会把新窗口挪到屏幕正中再合成
            # （实测同一份代码两次运行结果不同：wm 超时那次 pos=964,532 是对的，wm 活着那次
            # 窗口在正中，按 964,532 量到的是桌面）。v0.10.4 立过这条规矩。
            mwa = vm.wait_re(r"\[WM\] surf id=\d+ pid=%d rect=(-?\d+),(-?\d+),(\d+),(\d+)" % pid,
                             6, since=since)
            if mwa:
                info["pos"] = (int(mwa.group(1)), int(mwa.group(2)))
                info["size"] = (int(mwa.group(3)), int(mwa.group(4)))
                info["origin"] = "[WM] surf rect (合成器真实矩形)"
            else:
                info["pos"] = (int(mk.group(3)), int(mk.group(4))) if mk else None
                info["size"] = (int(mk.group(1)), int(mk.group(2))) if mk else None
                info["origin"] = "[WL64] surface create pos (内核默认槽位)"
            check("① %s：拿到屏上原点（来源=%s）" % (path, info["origin"]),
                  info["pos"] is not None, "pos=%s size=%s" % (info["pos"], info["size"]))
            return info

        # 路由样本文件（notes.txt / blob.bin / song.mp3 / app.exe / setup.deb）由
        # tools/viewer_pack_win.py 在**构建期**写进 /Pictures（见那里 SAMPLES 的说明）。
        for path, fmt, want_w, want_h, want_mode, want_img in IMAGES:
            if results:
                # 查看器退出后它不是最前的窗口 —— 点一下终端把它置顶，按键才到得了外壳
                mon.click(wait=0.6)
            n0 = vm.n()
            type_line(mon, "run /bin/viewer " + path, per_key=0.05)
            info = open_in_viewer(path, n0, fmt, want_w, want_h, want_mode, want_img)
            results[path] = info
            if not info or not info.get("pos"):
                continue
            vx, vy = info["pos"]
            vw, vh = info["size"]
            print("[viewer64] %s 屏上矩形 = (%d,%d) %dx%d  来源=%s"
                  % (path, vx, vy, vw, vh, info["origin"]))
            point_over(mon, vx + vw // 2, vy + vh // 2)      # 指针进窗口（ESC 才送得到）
            # ★ 必须等**合成器真的出一帧**再截图：big.png 的 surface 是 695x541 = 1.5 MB / 368 页，
            #   合成器要一帧才画得出来。只 sleep 会截到"已擦除、还没画上"的黑块
            #   （实测 frame=0 / red=0 / blue=0，而窗口明明在 (292,129)）。
            # since 必须是**这张图开始处**（画面静止后合成器就空闲、不再打帧，取 now 永远等不到）；
            # px>0 才是"真的画上了"（实测紧接着 attach 的那帧是 px=0，只擦不画）。
            mf = vm.wait_re(r"\[WM\] frame n=\d+ surfs=\d+[^\r\n]*px=([1-9]\d*)", 12,
                            since=info["n0"])
            check("① %s：合成器真出了一帧（[WM] frame … px=…）" % path, mf is not None,
                  mf.group(0)[:110] if mf else "")
            time.sleep(0.35)
            got = mon.shot(ppm)
            if got and mf is None:
                time.sleep(0.8)                              # 没等到帧：给一次机会再截
                got = mon.shot(ppm)
            check("① %s：截到画面（screendump）" % path, got)
            if got:
                ww, hh, px = read_ppm(ppm)
                check("① %s：窗口矩形在屏内" % path,
                      vx >= 0 and vy >= 0 and vx + vw <= ww and vy + vh <= hh,
                      "pos=%d,%d %dx%d screen=%dx%d" % (vx, vy, vw, vh, ww, hh))
                # ★ v0.11.3：标题栏改由**合成器**画（28 号装饰）——判据从"查看器自绘的
                #   0x3A4658"换成"合成器标题带 #F2F4F8 + 三按钮"，并且旧自绘色必须 0 px
                #   （强度不降：老判据是"窗口真上屏"，新判据同时钉住"画家换了、旧画家退场"）。
                fr = count(px, ww, hh, (vx, vy, vx + vw, vy + 25),
                           lambda r, g, b: abs(r - 0xF2) <= 2 and abs(g - 0xF4) <= 2 and abs(b - 0xF8) <= 2)
                oldfr = count(px, ww, hh, (vx, vy, vx + vw, vy + vh), is_frame)
                check("① %s：合成器标题带 #F2F4F8 在窗口顶部 25 行（>= 300 px）" % path, fr >= 300,
                      "band=%d" % fr)
                check("① %s：旧自绘标题栏 0x3A4658 = 0 px（画家已换成合成器）" % path, oldfr == 0,
                      "old=%d" % oldfr)
                cres = count(px, ww, hh, (vx + vw - 100, vy + 1, vx + vw, vy + 25),
                             lambda r, g, b: r >= 200 and g <= 60 and b <= 60)
                check("① %s：关闭按钮红底在标题带右侧（>= 200 px）" % path, cres >= 200,
                      "red=%d" % cres)
                red = count(px, ww, hh, (vx + 1, vy + 26, vx + vw - 1, vy + vh - 1), is_red)
                blue = count(px, ww, hh, (vx + 1, vy + 26, vx + vw - 1, vy + vh - 1), is_blue)
                # 128x96 图半幅 = 64*96 = 6144 px；big.png 缩放后 760x520 半幅更大。
                # 判据留足余量（JPEG 有损），但"通道搞反 / 解码错 / 当文本开"都会立刻红。
                check("① %s：左半是纯红（>= 3000 px，解码没把通道搞反）" % path, red >= 3000,
                      "red=%d" % red)
                check("① %s：右半是纯蓝（>= 3000 px）" % path, blue >= 3000, "blue=%d" % blue)
            check("① %s：ESC 收尾（[VIEWER] done … reason=esc）" % path,
                  esc_close_viewer(vm, mon, info["n0"]))

        # 三个格式都过 = 三种解码器都真跑过（不是同一份 GIF/BMP 兜底）
        magics = set(v.get("magic") for v in results.values() if v and v.get("magic"))
        check("① 三种格式各走各的解码器（magic 集合 = {png, jpeg, bmp}）",
              magics == {"png", "jpeg", "bmp"}, str(sorted(magics)))

        # ================= 阶段 2：文件管理器 + 全量路由 =================
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=2.5)
        check("② 文件管理器打开（[APP] mypc opened）",
              vm.wait_re(r"\[APP\] mypc opened", 25) is not None)
        mp = vm.wait_re(r"\[UI\] explorer card idx=(\d+) letter=C: kind=browsable", 20)
        check("② 此电脑页列出 C:（card idx 可定位）", mp is not None, mp.group(0) if mp else "")

        pad = Pad(vm, mon)
        park(mon)
        pad.x, pad.y = 0, 0
        if mp:
            tgt = card_center(int(mp.group(1)))
            r = pad.to(tgt[0], tgt[1], want_hit="card:")
            check("② 光标走到 C: 卡片（点击读回坐标纠偏）", r, "target=%s" % (tgt,))
        # ★ 偏移必须在**回车之前**取：exp_refresh 先逐条打 `item`、最后才打 `nav`，
        #   等看到 nav 行再取偏移就把整张清单跳过了（上一轮 25 条级联红就是这个）。
        n_pre_root = vm.n()
        if mp:
            mon.key("ret", wait=2.2)
        nav_root = vm.wait_re(r"\[UI\] explorer nav path=/ items=(\d+)", 15, since=n_pre_root)
        check("② 进入盘根（[UI] explorer nav path=/ items=N）", nav_root is not None,
              nav_root.group(0) if nav_root else "")
        mpic = None
        if nav_root:
            mpic = re.search(r"\[UI\] explorer item idx=(\d+) name=Pictures type=dir",
                             vm.log()[n_pre_root:])
        check("② 盘根里找到 Pictures 目录（[UI] explorer item）", mpic is not None,
              mpic.group(0) if mpic else "")

        items = {}
        if mpic:
            tgt = icon_cell(int(mpic.group(1)))
            r = pad.to(tgt[0], tgt[1], want_hit="item:")
            check("② 光标走到 Pictures 图标格", r, "target=%s" % (tgt,))
            n_pre_pics = vm.n()
            mon.key("ret", wait=2.2)
            nav_pics = vm.wait_re(r"\[UI\] explorer nav path=/Pictures items=(\d+)", 15,
                                  since=n_pre_pics)
            check("② 进入 /Pictures（[UI] explorer nav path=/Pictures）", nav_pics is not None,
                  nav_pics.group(0) if nav_pics else "")
            if nav_pics:
                for m in re.finditer(r"\[UI\] explorer item idx=(\d+) name=(\S+) type=(\S+)",
                                     vm.log()[n_pre_pics:]):
                    items[m.group(2)] = (int(m.group(1)), m.group(2))
        check("② /Pictures 里列出全部样本（4 图 + 5 样本）",
              all(k in items for k in ("redblue.png", "redblue.jpg", "redblue.bmp", "big.png",
                                       "notes.txt", "blob.bin", "song.mp3", "app.exe",
                                       "setup.deb")),
              str(sorted(items.keys())))

        def open_item(name, wait=2.5):
            """点中该条目（点击同时选中 + 把资源管理器置顶）+ 回车打开；
            返回 (这一段的日志, 匹配到的 route 行)。"""
            if name not in items:
                return None, None
            idx = items[name][0]
            tgt = icon_cell(idx)
            if not pad.to(tgt[0], tgt[1], want_hit="item:"):
                return None, None
            n1 = vm.n()
            mon.key("ret", wait=wait)
            m = vm.wait_re(r"\[UI\] explorer route name=%s ext=(\S+) route=(\S+)"
                           % re.escape(name), 15, since=n1)
            return (vm.log()[n1:], m)

        # ---- ⑤ 二进制拦截：.bin 不许文本打开 ----
        seg, m = open_item("blob.bin")
        check("⑤ .bin -> route=binary（禁止文本打开）",
              m is not None and m.group(2) == "binary", m.group(0) if m else "")
        check("⑤ .bin 真的没被当文本打开（无 preview 打点）",
              seg is not None and "preview name=blob.bin" not in seg)

        # ---- ⑥ 未知后缀：.mp3 不许文本打开，且保留 noassoc 老打点 ----
        seg, m = open_item("song.mp3")
        check("⑥ .mp3 -> route=unknown（禁止文本打开）",
              m is not None and m.group(2) == "unknown", m.group(0) if m else "")
        check("⑥ .mp3 保留 noassoc 打点（tests/fatread64_test 依赖同一行格式）",
              seg is not None and re.search(r"\[UI\] explorer noassoc name=song\.mp3 kind=\w+", seg)
              is not None)
        check("⑥ .mp3 没被当文本打开（无 preview 打点）",
              seg is not None and "preview name=song.mp3" not in seg)

        # ---- ④ 文本：.txt 允许文本编辑器 ----
        n_txt = vm.n()
        seg, m = open_item("notes.txt")
        check("④ .txt -> route=text（允许文本编辑器）",
              m is not None and m.group(2) == "text", m.group(0) if m else "")
        check("④ .txt 真打开成只读文本预览（[UI] explorer preview name=notes.txt）",
              seg is not None and "preview name=notes.txt" in seg)
        # ★ 文本预览是**独立窗口**且被 gui64_set_active 置顶，会吃掉后面的回车 ——
        #   必须真的把它关掉（点它自己的关闭按钮），否则后面每一条路由都是假红。
        #   关闭按钮命中带（kernel/gui64.cpp 的标题栏按钮）：bx = x+w-BORDER-BTN_W*3，
        #   b=(mx-bx)/BTN_W，b==2 是关闭；y 命中 [y+BORDER+2, y+TITLE_H)。BORDER=1/TITLE_H=24。
        mpv = None
        for mpv in re.finditer(r"\[UI\] win geom tag=new title=\S+ app=\d+ x=(-?\d+) y=(-?\d+) "
                               r"w=(\d+) h=(\d+) client=\d+x\d+ grip=\d+", vm.log()[n_txt:]):
            pass
        check("④ 文本预览拿到了自己的窗口矩形（[UI] win geom）", mpv is not None,
              mpv.group(0) if mpv else "")
        if mpv:
            pvx, pvy, pvw = int(mpv.group(1)), int(mpv.group(2)), int(mpv.group(3))
            point_over(mon, pvx + pvw - 16, pvy + 12)        # 关闭按钮
            time.sleep(0.6)

        # ---- ③ 图片：三种扩展名都拉起 /bin/viewer ----
        for name in ("redblue.png", "redblue.jpg", "redblue.bmp"):
            n_before = vm.n()
            seg, m = open_item(name, wait=3.0)
            check("③ %s -> route=image" % name, m is not None and m.group(2) == "image",
                  m.group(0) if m else "")
            sp = vm.wait_re(r"\[UI\] explorer route spawn app=/bin/viewer pid=(\d+) "
                            r"arg=/Pictures/%s" % re.escape(name), 20, since=n_before)
            check("③ %s -> 真拉起 /bin/viewer（pid + 路径都对）" % name, sp is not None,
                  sp.group(0) if sp else "")
            if sp:
                vpid = int(sp.group(1))
                mv2 = vm.wait_re(r"\[VIEWER\] ver=1 pid=%d file=/Pictures/%s"
                                 % (vpid, re.escape(name)), 25, since=n_before)
                check("③ %s：被拉起的查看器真的打开了这张图（pid=%d 的 ver=1）" % (name, vpid),
                      mv2 is not None, mv2.group(0) if mv2 else "")
                md2 = vm.wait_re(r"\[VIEWER\] decode ok w=(\d+) h=(\d+) comp=(\d+)", 25,
                                 since=n_before)
                check("③ %s：被拉起的查看器真的解码了 128x96" % name,
                      md2 is not None and int(md2.group(1)) == 128 and int(md2.group(2)) == 96,
                      md2.group(0) if md2 else "")
                # 指针必须进窗口再按 ESC（查看器自己不打 click 探针 -> 用合成器自报的矩形）
                mrw = vm.wait_re(r"\[WM\] surf id=\d+ pid=%d rect=(-?\d+),(-?\d+),(\d+),(\d+)"
                                 % vpid, 10, since=n_before)
                check("③ %s：合成器给出界面矩形（ESC 才送得到）" % name, mrw is not None,
                      mrw.group(0) if mrw else "")
                if mrw:
                    vx = int(mrw.group(1))
                    vy = int(mrw.group(2))
                    vw = int(mrw.group(3))
                    vh = int(mrw.group(4))
                    point_over(mon, vx + vw // 2, vy + vh // 2)
            check("③ %s：ESC 关掉查看器窗口" % name,
                  esc_close_viewer(vm, mon, n_before))

        # ---- ① 运行：.exe 拉起 PE 装载器 ----
        n_before = vm.n()
        seg, m = open_item("app.exe", wait=3.0)
        check("① app.exe -> route=pe", m is not None and m.group(2) == "pe",
              m.group(0) if m else "")
        sp = vm.wait_re(r"\[UI\] explorer route spawn app=/bin/peload pid=(\d+) "
                        r"arg=/Pictures/app\.exe", 20, since=n_before)
        check("① .exe 真拉起 /bin/peload（pid + 路径）", sp is not None, sp.group(0) if sp else "")

        # ---- ② 安装包：.deb 拉起包管理器（放最后：商店窗口最重） ----
        n_before = vm.n()
        seg, m = open_item("setup.deb", wait=3.0)
        check("② setup.deb -> route=deb", m is not None and m.group(2) == "deb",
              m.group(0) if m else "")
        sp = vm.wait_re(r"\[UI\] explorer route spawn app=/bin/store pid=(\d+)", 25,
                        since=n_before)
        check("② .deb 真拉起包管理器 /bin/store（pid）", sp is not None, sp.group(0) if sp else "")

        log = vm.log()
        for needle in ("PANIC", "TRIPLE FAULT", "FAILED mask="):
            check("全程不出现 %s" % needle, needle not in log)
    finally:
        if vm is not None:
            vm.stop()

    if args.keep:
        print("[viewer64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _n, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
