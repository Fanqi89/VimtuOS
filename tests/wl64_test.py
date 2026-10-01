#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/wl64_test.py - ★ A5 验收：Wayland 基础骨架（surface / buffer / commit / seat + 最小合成器）

要证明的事（与任务书的八条断言一一对应）：
  ① surface 创建/attach/commit 成功（[WL64] surface create / attach / commit / composite 打点齐全）；
  ② **屏幕像素随提交变化**：连续帧的截图差异区域 == 内核打出的 damage 矩形（局部 damage 帧）；
  ③ **未提交的帧不上屏**：
       3a) 往共享缓冲里画 + damage，但**没有 commit** -> 屏幕（面板内）逐像素不变；
       3b) commit 了但**没 dispatch**（异步提交模型）-> 屏幕也不变；
  ④ **双表面 z 序**：两块 surface 故意重叠，重叠区的像素 = **上层**（后创建、id 大）的颜色；
  ⑤ **damage 局部提交只改局部**：局部 damage 帧之间，damage 框**外**的像素差 ≈ 0；
  ⑥ **seat 事件投递**：monitor 注入鼠标/按键 -> 客户端收到，且 surf/inside/sx/sy 与注入的绝对坐标对得上
     （指针移出面板 = inside=0；移进 B = surf=B；再移进 A = surf=A；键盘按最近命中的 surface 路由）；
  ⑦ **surface 销毁后资源回收**：`[WL64] surface destroy .. refs_after=1` -> 客户端退出后
     `[SHM64] release .. refs=0 freed_pages=..` -> `[WL64] demo done .. pool_delta=+0 surfs=0`（页池回基线）；
  ⑧ 无 PANIC / 无 selftest FAIL / 自有 ABI 15..21 不许出现 [SYSCALL] enosys。

用法（必须 Windows 原生 Python）：
    py -3 tests\\wl64_test.py                 # QEMU：monitor 注入（默认，像素断言全在这条路上）
    py -3 tests\\wl64_test.py --keep          # 保留临时目录（截图/串口）
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
CLIENT_ELF = os.path.join(ROOT, "build64", "wlclient.elf")
FIXTURE_IMG = os.path.join(ROOT, "build64", "wl64_test.img")
PART_MAIN_LBA = 8009                    # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 32768                  # 16 MB（与 ipc64/sh64 同一夹具口径）

MOUSE_X0, MOUSE_Y0 = 512, 384           # kernel/input.cpp 的 mouse_init 默认位置
COL_TOL = 8                             # PPM 抓帧的颜色容差
PIX_TOL = 8                             # 逐像素"变/不变"的容差
TOL_EDGE = 2                            # 帧差包围盒与 damage 矩形的允许偏差（像素）


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
            qemu, "-name", "vimtu-wl64",
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

    def wait(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.15)
        return False

    def wait_re(self, rx, timeout):
        """等到正则匹配（返回 match 对象或 None）"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log())
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.15)
        return None

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
    assert raw.startswith(b"P6"), "不是 P6 PPM：%s" % path
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


def diff_bbox(shot_a, shot_b, rect):
    """两帧在 rect=(x,y,w,h) 内的差异包围盒；返回 (x,y,w,h) 或 None（无差异）"""
    wa, ha, pa = shot_a
    wb, hb, pb = shot_b
    if (wa, ha) != (wb, hb):
        return None
    x0, y0, rw, rh = rect
    bx0 = by0 = 1 << 30
    bx1 = by1 = -(1 << 30)
    n = 0
    for y in range(y0, y0 + rh):
        base = y * wa * 3
        for x in range(x0, x0 + rw):
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
        return None
    return (bx0, by0, bx1 - bx0, by1 - by0, n)


def bbox_close(got, want, tol=TOL_EDGE):
    if got is None or want is None:
        return False
    for i in range(4):
        if abs(got[i] - want[i]) > tol:
            return False
    return True


def bbox_inside(got, want, tol=TOL_EDGE):
    """got 的差异必须落在 want 矩形内（允许 tol 的边缘误差）"""
    if got is None:
        return True
    gx, gy, gw, gh = got[0], got[1], got[2], got[3]
    wx, wy, ww, wh = want
    return (gx >= wx - tol and gy >= wy - tol and
            gx + gw <= wx + ww + tol and gy + gh <= wy + wh + tol)


# --------------------------------------------------------------------------- 注入计划
def mouse_step(delta):
    """driver 每包最多走 24 像素；|in|<=14 时 accum 一次走完、不留残值 -> 步长精确可预测"""
    return (delta * 17) // 10 if delta >= 0 else -((-delta * 17) // 10)


def _pick_step(rem, max_in=14, y_axis=False):
    """挑一个注入量，使这一包走的像素尽量接近 rem（y 轴与内核一样带一次翻转）"""
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
    """把 (x0,y0) 移到尽量接近 (tx,ty)；返回 (steps, final_pos) —— steps = [(dx_in, dy_in), ...]

    每包的模拟与 kernel/input.cpp 的算法**逐行同一套整数运算**（灵敏度 ×17/10 截断、每包每轴最多
    24 像素、y 轴翻转），所以 final_pos 就是内核光标会停在的绝对坐标（|注入| <= 14 时不留残值）。"""
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


def prepare_fixture_img(verbose=True):
    """夹具盘 = system.img + VimtuFS2 主分区；卷里写 /bin/shell.bin、/etc/wl64_probe、/wlclient.elf。

    ★ /wlclient.elf **不在内核里**（见 build64.sh 的交付纪律）：它是卷里的文件，由这里写进去。"""
    import make_shellvol as msv
    if not os.path.exists(SHELL_BIN):
        raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % SHELL_BIN)
    if not os.path.exists(CLIENT_ELF):
        raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % CLIENT_ELF)
    shell_bytes = open(SHELL_BIN, "rb").read()
    client_bytes = open(CLIENT_ELF, "rb").read()
    hello = b"wl64 fixture\n"
    probe = b"/* wl64 probe: exists = full demo mode (kernel/wl64.h) */\n"
    vol = msv.Volume(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", hello, parent=etc_ino, mode=0o644)
    vol.write_file("wl64_probe", probe, parent=etc_ino, mode=0o644)
    vol.write_file("wlclient.elf", client_bytes, parent=0, mode=0o755)
    vb = vol.finish()
    bad = msv.verify(vb, {"/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": hello,
                          "/etc/wl64_probe": probe, "/wlclient.elf": client_bytes})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    if verbose:
        print("   夹具卷 OK：/bin/shell.bin=%d B /etc/wl64_probe=%d B /wlclient.elf=%d B（逐字节回读一致）"
              % (len(shell_bytes), len(probe), len(client_bytes)))
    system_bytes = open(SYSTEM_IMG, "rb").read()
    disk = msv.build_disk(system_bytes, vb, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(disk)
    if verbose:
        print("   夹具盘：%s（%d B）" % (FIXTURE_IMG, len(disk)))
    return FIXTURE_IMG


# --------------------------------------------------------------------------- 断言壳
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
    ap.add_argument("--img", default=None, help="夹具盘（缺省自动生成 build64/wl64_test.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5667)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=420)
    args = ap.parse_args()
    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2

    print("=== 0) 准备夹具盘（system.img + VimtuFS2 卷：shell + /wlclient.elf + /etc/wl64_probe）===")
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
    tmp = tempfile.mkdtemp(prefix="vimtu64_wl64_")
    serial = os.path.join(tmp, "serial.log")
    ch = Checks()
    shots = {}
    log = ""
    vm = mon = None
    try:
        vm = QemuVm(qemu, img, args.port, serial)
        mon = Monitor(args.port)
        print("=== 1) 启动：等 [WL64] init（面板几何）+ 客户端起进程 ===")
        m = vm.wait_re(r"\[WL64\] init fb=(\d+)x(\d+) panel=(-?\d+),(-?\d+),(\d+),(\d+) slots=(\d+) "
                       r"surf_max=(\d+) out=(\d+)", args.timeout)
        ch("内核打印 [WL64] init（fb 几何 + 可见区域 + out=1）", m is not None,
           (("fb=%sx%s panel=%s,%s,%s,%s out=%s" %
             (m.group(1), m.group(2), m.group(3), m.group(4), m.group(5), m.group(6), m.group(9)))
            if m else "没等到"))
        if m is None:
            raise RuntimeError("没等到 [WL64] init")
        fbw, fbh = int(m.group(1)), int(m.group(2))
        panel = (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6)))
        ch("面板完整落在屏幕内（不是全屏独占：面板比屏幕小）",
           panel[0] >= 0 and panel[1] >= 0 and panel[2] <= fbw and panel[3] <= fbh and
           panel[2] < fbw and panel[3] < fbh,
           "panel=%s fb=%dx%d" % (panel, fbw, fbh))

        ch("演示进程起来了（[WL64] demo start mode=full）",
           vm.wait_re(r"\[WL64\] demo start pid=(\d+) mode=full", args.timeout) is not None)
        m = vm.wait_re(r"\[WLCLIENT\] mode full=(\d) probe=(\d)", 60)
        ch("客户端也判到完整模式（同一个探针文件：mode full=1 probe=1）",
           m is not None and m.group(1) == "1" and m.group(2) == "1", m.group(0) if m else "")
        ch("客户端拿到 seat（[WLCLIENT] seat=1 + [WL64] seat get id=1）",
           vm.wait_re(r"\[WLCLIENT\] seat=1", 30) is not None and
           re.search(r"\[WL64\] seat get id=1 pid=\d+ flags=3", vm.log()) is not None)

        print("=== 2) ①  surface 创建 / attach / commit（打点齐全）===")
        ms = re.findall(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+) shm=(\d+) pid=(\d+) px=(\d+) "
                        r"fmt=(\d+) slot=(\d+) pos=(-?\d+),(-?\d+)", vm.log())
        ch("两块 surface 都创建成功（id 不同、shm=0 表示还没 attach）", len(ms) >= 2,
           "n=%d %s" % (len(ms), ms[:2]))
        surf = {}
        if len(ms) >= 2:
            for r in ms:
                surf[int(r[0])] = dict(w=int(r[1]), h=int(r[2]), shm=int(r[3]), pid=int(r[4]),
                                       px=int(r[5]), fmt=int(r[6]), slot=int(r[7]),
                                       pos=(int(r[8]), int(r[9])))
            sA = min(surf.keys())
            sB = max(surf.keys())
            ch("A = 160x96 / B = 128x64（单面像素都在 16384 上限内）",
               surf[sA]["w"] == 160 and surf[sA]["h"] == 96 and
               surf[sB]["w"] == 128 and surf[sB]["h"] == 64 and
               surf[sA]["px"] <= 16384 and surf[sB]["px"] <= 16384,
               "A=%dx%d px=%d / B=%dx%d px=%d" % (surf[sA]["w"], surf[sA]["h"], surf[sA]["px"],
                                                  surf[sB]["w"], surf[sB]["h"], surf[sB]["px"]))
            ch("两块 surface 的槽位不同、矩形**故意重叠**（z 序要有可观测的遮挡区）",
               surf[sA]["slot"] != surf[sB]["slot"] and
               surf[sB]["pos"][0] < surf[sA]["pos"][0] + surf[sA]["w"] and
               surf[sB]["pos"][1] < surf[sA]["pos"][1] + surf[sA]["h"],
               "A.pos=%s B.pos=%s" % (surf[sA]["pos"], surf[sB]["pos"]))
            ch("两块 surface 都落在面板矩形内",
               all(surf[k]["pos"][0] >= panel[0] and surf[k]["pos"][1] >= panel[1] and
                   surf[k]["pos"][0] + surf[k]["w"] <= panel[0] + panel[2] and
                   surf[k]["pos"][1] + surf[k]["h"] <= panel[1] + panel[3] for k in (sA, sB)))
        print("=== 3) ② 帧序列：抓帧（每帧一张，带\"抓帧期间没有跳帧\"的守卫）===")
        # 逐帧等日志出现 -> 立刻抓帧（守卫：抓帧期间若又出了新帧，这一张丢弃）
        for want in range(0, 8):
            # 等第 want 帧的日志出现（严格顺序：frame f=0,1,2,...）
            ok = vm.wait_re(r"\[WLCLIENT\] frame f=%d " % want, 120)
            if not ok:
                break
            before = len(re.findall(r"\[WLCLIENT\] frame f=", vm.log()))
            ppm = os.path.join(tmp, "f%d.ppm" % want)
            got = mon.shot(ppm)
            after = len(re.findall(r"\[WLCLIENT\] frame f=", vm.log()))
            if got and after == before:                 # 抓帧期间没有新帧 -> 这张图确定是第 want 帧
                shots[want] = read_ppm(ppm)
            time.sleep(0.1)
        ch("抓到 >= 5 张确定帧号的截图（phase 1 共 8 帧）", len([k for k in shots if k < 8]) >= 5,
           "shots=%s" % sorted(k for k in shots if k < 8))

        # attach 明细：等到 phase 1 跑完（这时 A/B 的每次 attach 都已在日志里：A 两块对象 + B 两块 offset）
        att = re.findall(r"\[WL64\] attach id=(\d+) shm=(\d+) off=(\d+) bytes=(\d+) refs=(\d+) pages=(\d+) prev=(\d+)",
                         vm.log())
        ch("attach 至少 3 次（A 两块缓冲 + B 一块对象两块缓冲）", len(att) >= 3, "n=%d" % len(att))
        ch("每次 attach 内核都持有一个引用（refs=2 = 客户端句柄 + 组合器）",
           len(att) >= 3 and all(int(r[4]) >= 2 for r in att), "refs=%s" % [r[4] for r in att[:6]])
        ch("B 的两块缓冲在**同一个对象**里用不同 offset（32768）",
           any(int(r[2]) == 32768 for r in att), "offs=%s" % [r[2] for r in att[:8]])
        if surf:
            a_id = str(min(surf.keys()))
            ch("A 的两块缓冲是**两个不同的对象**（真双缓冲）",
               len({r[1] for r in att if r[0] == a_id}) >= 2,
               "A shm ids=%s" % sorted({r[1] for r in att if r[0] == a_id}))

        frames = {}
        mf = re.findall(r"\[WLCLIENT\] frame f=(\d+) full=(\d) abuf=(\d) bbuf=(\d) acol=0x([0-9a-fA-F]+) "
                        r"bcol=0x([0-9a-fA-F]+) dmg=(-?\d+),(-?\d+),(-?\d+),(-?\d+) sq=(-?\d+),(-?\d+) "
                        r"cross=(-?\d+),(-?\d+),(-?\d+)", vm.log())
        ch("客户端每帧都打了帧日志（full/abuf/bbuf/颜色/damage/方块/十字）", len(mf) >= 8, "n=%d" % len(mf))
        for r in mf:
            frames[int(r[0])] = dict(full=int(r[1]), abuf=int(r[2]), bbuf=int(r[3]),
                                     acol=int(r[4], 16), bcol=int(r[5], 16),
                                     dmg=(int(r[6]), int(r[7]), int(r[8]), int(r[9])),
                                     sq=(int(r[10]), int(r[11])),
                                     cross=(int(r[12]), int(r[13]), int(r[14])))
        if frames:
            fs = sorted(frames)
            ch("双缓冲真的在 ping-pong（相邻帧 abuf 交替 0/1）",
               all(frames[fs[i]]["abuf"] != frames[fs[i + 1]]["abuf"] for i in range(len(fs) - 1)),
               "abufs=%s" % [frames[k]["abuf"] for k in fs[:8]])
            ch("damage 语义两种都出现了：full=1（整面）与 full=0（局部）",
               any(frames[k]["full"] == 1 for k in fs) and any(frames[k]["full"] == 0 for k in fs),
               "fulls=%s" % [frames[k]["full"] for k in fs[:8]])

        comp = re.findall(r"\[WL64\] composite n=(\d+) bytes=(\d+) us=(\d+)", vm.log())
        ch("内核合成打点（[WL64] composite n=.. bytes=.. us=..）", len(comp) >= 3, "n=%d" % len(comp))
        ch("每轮合成的字节数 > 0（真的拷了像素）",
           len(comp) >= 3 and all(int(r[1]) > 0 for r in comp), "bytes=%s" % [r[1] for r in comp[:4]])

        print("=== 4) ④⑤ 像素证据：帧差区域 == damage 矩形、z 序遮挡、色带变化 ===")
        if surf and len(shots) >= 5:
            A = surf[min(surf.keys())]
            B = surf[max(surf.keys())]
            ax, ay = A["pos"]
            bx, by = B["pos"]
            panel_rect = panel
            # ---- z 序：重叠区必须是**上层**（B）的颜色 ----
            ov_x = bx + 8
            ov_y = by + 20
            if 0 in shots and 0 in frames:
                cA = frames[0]["acol"]
                cB = frames[0]["bcol"]
                got = px_at(shots[0], ax + 130, ay + 40)
                ch("A 独占区像素 = 客户端上报的 A 色带（frame 0 acol）", near(got, cA),
                   "got=0x%06x want=0x%06x" % (got or 0, cA))
                gotA = px_at(shots[0], ov_x, ov_y)
                ch("重叠区（A∩B）像素 = **上层 B** 的颜色（z 序：后创建的在上面）", near(gotA, cB),
                   "got=0x%06x B=0x%06x A=0x%06x" % (gotA or 0, cB, cA))
                gotB = px_at(shots[0], bx + 100, by + 40)
                ch("B 独占区像素 = B 色带", near(gotB, cB), "got=0x%06x want=0x%06x" % (gotB or 0, cB))
                # 面板底色（不是 surface 覆盖区）应当是面板 chrome —— 用来证明"画在可见区域里"
                pc = px_at(shots[0], panel[0] + 6, panel[1] + panel[3] - 6)
                ch("面板内、surface 之外是面板底色（说明 surface 是窗内容、不是全屏独占）",
                   pc is not None and not near(pc, cA) and not near(pc, cB),
                   "panel_bg=0x%06x" % (pc or 0))
            # ---- 局部 damage 帧：帧差包围盒 == 内核 blit 的 src 矩形（换算到绝对坐标） ----
            pairs = []
            for k in sorted(shots):
                if k + 1 in shots and k < 7 and k + 1 in frames:
                    pairs.append((k, k + 1))
            checked_local = 0
            checked_full = 0
            for (k, k1) in pairs:
                d = diff_bbox(shots[k], shots[k1], panel_rect)
                f1 = frames[k1]
                # 内核这一帧的 blit 明细（src = 局部 damage 矩形，dst = 屏幕位置）
                blits = [rb for rb in re.findall(
                    r"\[WL64\] blit surf=(\d+) shm=(\d+) off=(\d+) src=(-?\d+),(-?\d+),(\d+),(\d+) "
                    r"dst=(-?\d+),(-?\d+) bytes=(\d+) clip=(\w+)", vm.log()) if int(rb[0]) == min(surf.keys())]
                if f1["full"] == 0:
                    want = (ax + f1["dmg"][0], ay + f1["dmg"][1], f1["dmg"][2], f1["dmg"][3])
                    if ch("局部 damage 帧 %d->%d：帧差包围盒 == damage 矩形（%s）" % (k, k1, want),
                          bbox_close(d, want), "diff=%s want=%s" % (d, want)):
                        checked_local += 1
                    # 内核的 blit 明细里必须有一条的 src 矩形与客户端上报的 damage 完全一致
                    ch("局部 damage 帧 %d：内核 blit 的 src 矩形 == 客户端上报的 damage（%s）" % (k1, want),
                       any(int(rb[3]) == f1["dmg"][0] and int(rb[4]) == f1["dmg"][1] and
                           int(rb[5]) == f1["dmg"][2] and int(rb[6]) == f1["dmg"][3] and
                           int(rb[7]) == ax + f1["dmg"][0] and int(rb[8]) == ay + f1["dmg"][1]
                           for rb in blits),
                       "blits=%s" % [rb[3:9] for rb in blits[-3:]])
                    ch("局部 damage 帧 %d->%d：damage 框**外**的像素差 ≈ 0（框外不变）" % (k, k1),
                       bbox_inside(d, want))
                else:
                    awant = (ax, ay, A["w"], A["h"])
                    if ch("整面 damage 帧 %d->%d：帧差包围盒 == A 的整面矩形（%s）" % (k, k1, awant),
                          bbox_close(d, awant), "diff=%s want=%s" % (d, awant)):
                        checked_full += 1
                    ch("整面 damage 帧 %d->%d：色带真的换了（acol 变了）" % (k, k1),
                       frames[k]["acol"] != frames[k1]["acol"],
                       "0x%06x -> 0x%06x" % (frames[k]["acol"], frames[k1]["acol"]))
            ch("至少验到 2 组局部 damage 帧（帧差 == damage 矩形）", checked_local >= 2,
               "local_pairs=%d" % checked_local)
            ch("至少验到 1 组整面 damage 帧（色带 + 整面刷新）", checked_full >= 1,
               "full_pairs=%d" % checked_full)

        print("=== 5) ②③ 未提交 / 未 dispatch 都不上屏 ===")
        ch("客户端进入阶段 2a（写缓冲 + damage，但**没有 commit**）",
           vm.wait_re(r"\[WLCLIENT\] nocommit f=\d+ why=no-commit", 60) is not None)
        if 7 in shots:
            ppm = os.path.join(tmp, "nc1.ppm")
            ok1 = mon.shot(ppm) and read_ppm(ppm)
            shots["nc1"] = ok1
            ch("3a) 未提交的帧不上屏（面板内逐像素与最后一帧一致）",
               ok1 and diff_bbox(shots[7], shots["nc1"], panel_rect) is None,
               "diff=%s" % (diff_bbox(shots[7], shots["nc1"], panel_rect) if ok1 else None))
        ch("客户端进入阶段 2b（commit 了但**没 dispatch**）",
           vm.wait_re(r"\[WLCLIENT\] nocommit f=\d+ why=no-dispatch", 60) is not None)
        if "nc1" in shots:
            ppm = os.path.join(tmp, "nc2.ppm")
            ok2 = mon.shot(ppm) and read_ppm(ppm)
            shots["nc2"] = ok2
            ch("3b) commit 而未 dispatch 也不上屏（异步提交模型：合成发生在 dispatch 里）",
               ok2 and diff_bbox(shots["nc1"], shots["nc2"], panel_rect) is None,
               "diff=%s" % (diff_bbox(shots["nc1"], shots["nc2"], panel_rect) if ok2 else None))

        print("=== 6) ⑥ seat：注入键鼠（monitor），断言路由与坐标语义 ===")
        ch("客户端进入 seat 监听（[WLCLIENT] listen start）",
           vm.wait_re(r"\[WLCLIENT\] listen start", 60) is not None)
        if surf:
            A = surf[min(surf.keys())]
            B = surf[max(surf.keys())]
            ax, ay = A["pos"]
            bx, by = B["pos"]
            # 目标：① 面板外（不带 surface）② B 中心（重叠区 -> 上层 B）③ A 左上（A 独占区）
            out_tx, out_ty = max(8, panel[0] - 80), max(8, panel[1] - 60)
            b_tx, b_ty = bx + B["w"] // 2, by + B["h"] // 2
            a_tx, a_ty = ax + 40, ay + 30
            sim_x, sim_y = MOUSE_X0, MOUSE_Y0
            plan = []
            for (tx, ty, tag) in ((out_tx, out_ty, "out"), (b_tx, b_ty, "B"), (a_tx, a_ty, "A")):
                steps, (sim_x, sim_y) = plan_move(sim_x, sim_y, tx, ty)
                plan.append((tag, steps, (sim_x, sim_y)))
            print("   注入计划：起点 (%d,%d) -> %s" %
                  (MOUSE_X0, MOUSE_Y0, " -> ".join("%s(%d,%d) via %d steps" % (t, p[0], p[1], len(s))
                                                   for t, s, p in plan)))
            for tag, steps, (ex, ey) in plan:
                for dx, dy in steps:
                    mon.mouse_move(dx, dy)
            time.sleep(0.4)
            print("   注入按键 a + 左键按下/抬起")
            mon.key("a", wait=0.25)
            mon.mouse_button(1)
            time.sleep(0.35)
            mon.mouse_button(0)
            plan_pos = {tag: pos for tag, _steps, pos in plan}

        ch("客户端跑完并退出（[WLCLIENT] done）", vm.wait("[WLCLIENT] done", 120))
        ch("内核收尾（[WL64] demo done）", vm.wait("[WL64] demo done", 60))
        time.sleep(0.8)
        log = vm.log()

        # 抓最后一张（客户端退出后屏幕保留最后一帧：十字/色带都还在）
        ppm = os.path.join(tmp, "seat.ppm")
        if mon.shot(ppm):
            shots["seat"] = read_ppm(ppm)

        print("=== 7) ⑥ seat 事件原文（内核 + 客户端两侧对账）===")
        sev = re.findall(r"\[WL64\] seat event surf=(\d+) type=(\d+) x=(-?\d+) y=(-?\d+) inside=(\d) "
                         r"sx=(-?\d+) sy=(-?\d+) code=(\d+)", log)
        nos = re.findall(r"\[WL64\] seat no-surface pid=(\d+) type=(\d+) x=(-?\d+) y=(-?\d+)", log)
        cli = re.findall(r"\[WLCLIENT\] seat n=(\d+) surf=(\d+) type=(\d+) code=0x([0-9a-fA-F]+) x=(-?\d+) y=(-?\d+) "
                         r"sx=(-?\d+) sy=(-?\d+) inside=(\d)", log)
        ch("内核打了 seat 路由行（[WL64] seat event）", len(sev) >= 3, "n=%d" % len(sev))
        ch("客户端也收到了同一批事件（[WLCLIENT] seat n=..）", len(cli) >= 3, "n=%d" % len(cli))
        ch("指针移出面板时没有 surface（[WL64] seat no-surface pid=..）", len(nos) >= 1,
           "n=%d %s" % (len(nos), nos[:2]))
        if surf and len(sev) >= 3:
            A = surf[min(surf.keys())]
            B = surf[max(surf.keys())]
            by_surf = {}
            for r in sev:
                by_surf.setdefault(int(r[0]), []).append(r)
            # 移到 B 中心 -> surf = B（重叠区取上层）
            okB = any(int(r[0]) == max(surf.keys()) and int(r[1]) == 3 and int(r[4]) == 1
                      for r in sev)
            ch("指针移到重叠区 -> 路由到**上层 surface**（surf=B=%d、inside=1）" % max(surf.keys()), okB,
               "surf 分布=%s" % sorted(by_surf.keys()))
            # 取**最后一次**命中 B 的移动事件（= 注入计划的终点；中间过程也会落在 B 上）
            hitsB = [x for x in sev if int(x[0]) == max(surf.keys()) and int(x[1]) == 3]
            if okB and hitsB:
                r = hitsB[-1]
                ex, ey = int(r[2]), int(r[3])
                sx, sy = int(r[5]), int(r[6])
                ch("B 上事件的坐标语义正确（sx = x - B.pos.x = %d - %d）" % (sx, ex),
                   sx == ex - B["pos"][0] and sy == ey - B["pos"][1],
                   "x=%d y=%d sx=%d sy=%d B.pos=%s" % (ex, ey, sx, sy, B["pos"]))
                ch("B 上事件的坐标落在 B 矩形内（真的移到重叠区了）",
                   B["pos"][0] <= ex < B["pos"][0] + B["w"] and B["pos"][1] <= ey < B["pos"][1] + B["h"],
                   "got=(%d,%d) B.rect=%s" % (ex, ey, (B["pos"], B["w"], B["h"])))
                # 与注入计划的终点对账（monitor 的 PS/2 包可能被合并 -> 落点允许 ~48px 偏差；如实打印）
                if plan_pos:
                    px_, py_ = plan_pos.get("B")
                    ch("B 上事件的落点与注入计划的终点同向且在容差内（plan=%s）" % ((px_, py_),),
                       abs(ex - px_) <= 48 and abs(ey - py_) <= 48,
                       "got=(%d,%d) plan=(%d,%d)" % (ex, ey, px_, py_))
            hitsA = [x for x in sev if int(x[0]) == min(surf.keys()) and int(x[1]) == 3]
            okA = any(int(r[4]) == 1 for r in hitsA)
            ch("指针再移到 A 独占区 -> surf=A=%d" % min(surf.keys()), okA)
            if hitsA:
                exA, eyA = int(hitsA[-1][2]), int(hitsA[-1][3])
                ch("A 上事件的坐标在 A 矩形内、且不在 B 的矩形内（真的到了 A 独占区）",
                   ax <= exA < ax + A["w"] and ay <= eyA < ay + A["h"] and
                   not (bx <= exA < bx + B["w"] and by <= eyA < by + B["h"]),
                   "got=(%d,%d) A.rect=%s B.pos=%s" % (exA, eyA, (A["pos"], A["w"], A["h"]), B["pos"]))
            keys = [r for r in sev if int(r[1]) in (1, 2)]
            ch("键盘事件也投递了（[WL64] seat event type=1/2）", len(keys) >= 1, "n=%d" % len(keys))
            if keys:
                ch("键盘按**最近指针命中的 surface** 路由（keydown 的 surf = A）",
                   any(int(r[0]) == min(surf.keys()) and int(r[1]) == 1 for r in keys),
                   "keys=%s" % [(r[0], r[1]) for r in keys[:4]])
                ch("键码 = 'a' = 0x61", any(int(r[7]) == 0x61 for r in keys) or
                   any(int(c[3], 16) == 0x61 for c in cli), "codes=%s" % [r[7] for r in keys[:4]])
                ch("键盘事件的 inside=1 且 sx=sy=-1（键盘没有坐标）",
                   all(int(r[4]) == 1 and int(r[5]) == -1 and int(r[6]) == -1 for r in keys),
                   "keys=%s" % [(r[4], r[5], r[6]) for r in keys[:3]])
            ch("鼠标左键按下/抬起都投递了（type=4/5）",
               any(int(r[1]) == 4 for r in sev) and any(int(r[1]) == 5 for r in sev),
               "types=%s" % sorted({int(r[1]) for r in sev}))
            mk = re.search(r"\[WLCLIENT\] key code=0x([0-9a-fA-F]+) surf=(\d+) band=(\d) acol=0x([0-9a-fA-F]+) "
                           r"bcol=0x([0-9a-fA-F]+)", log)
            ch("客户端收到按键并换了色带（[WLCLIENT] key ... band=..）", mk is not None,
               mk.group(0) if mk else "")
            crs = re.findall(r"\[WLCLIENT\] cross surf=(\d+) sx=(-?\d+) sy=(-?\d+)", log)
            ch("客户端按事件路由到的 surface 画十字（[WLCLIENT] cross surf=..）", len(crs) >= 1,
               "n=%d last=%s" % (len(crs), crs[-1] if crs else ""))
            # 屏幕上的**最后一帧**是权威：用那一帧自己上报的 cross=(surf,sx,sy) 去采像素
            lastf = [r for r in mf if int(r[0]) >= 8]
            if lastf and "seat" in shots:
                lf = lastf[-1]
                acol = int(lf[4], 16)
                bcol = int(lf[5], 16)
                base = surf[min(surf.keys())]["pos"]
                bpos = surf[max(surf.keys())]["pos"]
                csf, csx, csy = int(lf[12]), int(lf[13]), int(lf[14])
                if csf in surf and csx >= 0 and csy >= 0:
                    cbase = surf[csf]["pos"]
                    got = px_at(shots["seat"], cbase[0] + csx, cbase[1] + csy)
                    ch("屏上那个位置真的是**十字的绿色**（像素证据：客户端画的十字在最后一帧上屏了）",
                       near(got, 0x00FF40), "got=0x%06x want=0x00ff40 at (%d,%d) cross=(%d,%d,%d)"
                       % (got or 0, cbase[0] + csx, cbase[1] + csy, csf, csx, csy))
                else:
                    ch("最后一帧带十字（cross surf=%d）" % csf, csf in surf and csx >= 0,
                       "cross=(%d,%d,%d)" % (csf, csx, csy))
                # A 的采样点取多个候选（避开白方块区 x<=80、十字可能落在的位置），任一个对上就算过
                cand = [(base[0] + 150, base[1] + 40), (base[0] + 150, base[1] + 70),
                        (base[0] + 20, base[1] + 80)]
                hits = [px_at(shots["seat"], x, y) for (x, y) in cand]
                ch("按键后 A 的色带变成了客户端上报的新颜色（屏幕 = 客户端画的内容）",
                   any(near(v, acol) for v in hits),
                   "got=%s acol=0x%06x" % (["0x%06x" % (v or 0) for v in hits], acol))
                ch("B 仍然显示自己的色带（按键只改它路由到的 surface 的色带）",
                   near(px_at(shots["seat"], bpos[0] + 100, bpos[1] + 40), bcol),
                   "got=0x%06x want=0x%06x" % (px_at(shots["seat"], bpos[0] + 100, bpos[1] + 40) or 0, bcol))

        print("=== 8) ⑦ 销毁与回收（引用计数 / 页池基线）===")
        de = re.findall(r"\[WL64\] surface destroy id=(\d+) pid=(\d+) shm=(\d+) refs_after=(\d+) held=(\d+) why=(\w+)",
                        log)
        ch("两块 surface 都被客户端销毁（[WL64] surface destroy why=client）",
           len([r for r in de if r[5] == "client"]) >= 2, "n=%d" % len(de))
        ch("销毁后组合器持有的那份 shm 引用被还回去（refs_after=1 = 只剩客户端句柄）",
           len(de) >= 2 and all(int(r[3]) == 1 for r in de if r[5] == "client"),
           "refs_after=%s" % [r[3] for r in de])
        ch("组合器的 release 打点可见（[SHM64] release pid=0 refs=1）",
           re.search(r"\[SHM64\] release pid=0 id=\d+ refs=1 freed_pages=0", log) is not None)
        rel0 = re.findall(r"\[SHM64\] release pid=(\d+) id=(\d+) refs=0 freed_pages=(\d+)", log)
        ch("3 块缓冲退出后都归零并还页（refs=0）", len(rel0) >= 3, "n=%d %s" % (len(rel0), rel0[:4]))
        ch("还回的页数与对象尺寸一致（A=15 页 x2、B=16 页）",
           sorted(int(r[2]) for r in rel0[:3]) == [15, 15, 16] or
           sorted(int(r[2]) for r in rel0)[-3:] == [15, 15, 16],
           "freed=%s" % [r[2] for r in rel0[:4]])
        md = re.search(r"\[WL64\] demo done pid=(\d+) exited=(\d+) ticks=(\d+) pool_free=(\d+) "
                       r"pool_delta=([+-]\d+) surfs=(\d+) seat_users=(\d+) seat_logged=(\d+) seat_supp=(\d+)", log)
        ch("内核收尾打点完整（exited/ticks/pool_delta/surfs/seat_*）", md is not None)
        if md:
            ch("演示进程正常退出（exited=1）", md.group(2) == "1")
            ch("页池回到基线（pool_delta=+0 -> 没有泄漏）", md.group(5) == "+0",
               "pool_free=%s delta=%s" % (md.group(4), md.group(5)))
            ch("没有残留 surface（surfs=0）", md.group(6) == "0")
            ch("seat 事件都被客户端取走了（seat_supp=0 = 没有被节流丢掉的证据行）",
               md.group(9) == "0", "logged=%s supp=%s" % (md.group(8), md.group(9)))

        print("=== 9) ⑧ 无 PANIC / 无 selftest FAIL / 无 enosys ===")
        ch("内核自检通过（[WL64] selftest PASS）", "[WL64] selftest PASS" in log)
        ch("客户端在卷里被内核找到（[WL64] client on volume path=/wlclient.elf）",
           re.search(r"\[WL64\] client on volume path=/wlclient\.elf bytes=\d+", log) is not None)
        for needle in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=",
                       "[WL64] demo skipped", "[WL64] demo TIMEOUT", "[WLCLIENT] FAILED",
                       "[WL64] blit surf=1 shm=0", "[SHM64] create FAILED"):
            ch("不得出现 %s" % needle, needle not in log)
        ens = [n for n in range(15, 22) if ("[SYSCALL] enosys nr=%d" % n) in log]
        ch("自有 ABI 15..21 没有 enosys", not ens, "enosys=%s" % ens)
        nline = len(re.findall(r"\[WL64\] seat event ", log))
        ch("seat 路由行数量合理（>= 3 且 <= 200 的日志预算）", 3 <= nline <= 200, "n=%d" % nline)

    finally:
        if vm:
            vm.close()

    if args.keep:
        print("[wl64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ch.ok else "FAIL", ch.n, ch.passed))
    return 0 if ch.ok else 1


if __name__ == "__main__":
    sys.exit(main())
