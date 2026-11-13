# -*- coding: utf-8 -*-
"""tests/wmfull64_test.py —— v0.9.47（W3 第二批）：**全屏 Ring 3 合成器 + 窗口拖动 + 真鼠标点击**

这一份钉的是 W3 第二批的三件事（用户原话）：
  ① /lib/wm.elf 从"320x200 内核面板区"升级成**全屏**多窗口合成器，
     窗口出现在屏幕任意位置（这次跑的就是"Windows 的 MessageBoxA 弹窗挪到屏幕正中"）；
  ② 修复"点不到 OK"的**夹具**链：QEMU monitor 的 mouse_move 是相对位移、内核 ×1.7 + 96px 累加器，
     `-serial file:` 还有 1~3 s 落盘延迟 —— 本测试改用 **`-serial tcp:`（零延迟串口）** +
     按客户机自报的 `[UI] perf … cur=x,y` 做**闭环**，把光标精确送到控件中心；
  ③ 窗口拖动：按住窗口顶部拖动条移动，松手后窗口矩形整体位移。
     判据 = **日志**（`[WL64] move … pos=…` + `[WM] drag move/end`）+ **两张 screendump 的像素**
     （新位置有对话框、旧位置被背景快照还原）。

## 两条来之不易的纪律（都实测踩过，别退回去）

1. **坐标一律来自日志，像素只做核验。** 早的版本用"整屏扫标题栏颜色"定位窗口 —— 1280x800 的
   Python 逐像素扫描要几秒并一直握着 GIL，把串口 TCP 的读线程饿死 → QEMU 的 chardev 反压 →
   客户机串口写阻塞 → 整机掉到 1 fps、日志在 sec=45 处**断掉**（之后的判据全在用旧样本）。
   现在：所有坐标从 `[WM] place` / `[WL64] move` / `[PELOAD] msgboxA btn` 读，
   **像素分析全部挪到 `quit` 之后**。
2. **串口日志要增量缓存**：`text()` 只在有新数据时重新拼接，绝不每次 join 整段。

链路（每一步都用串口 + 像素双证）：
  起桌面 -> Dock 开终端
    -> `elfrun bg /lib/wm.elf`        [WM] composer seat=1 gpu=0 region=0,0,1280,800 / [WM] policy … bg=1
    -> `run /bin/peload /pexcorpus/gui.exe`
         [PELOAD] msgboxA show …  ->  [WL64] move … by=composer  ->  [WM] place id=.. pos=490,325 mode=center
         ->  screendump #1：对话框**在屏幕正中**
    -> 拖动：光标到标题栏 -> 按下 -> 分步移动 -> 松开
         [WM] drag begin / drag move × N / drag end  ->  [WL64] move … pos=<新>  ->  screendump #2
    -> 真点击：光标到 OK 按钮中心 -> 按下/松开
         [PELOAD] msgboxA close reason=clicked  ->  [PROC64] exit code=0  ->  screendump #3：对话框消失

判据三态：PASS / FAIL / SKIP(env)。只有环境不足才 SKIP；产品行为不对一律 FAIL。

## 现状（v0.9.47 交付时）：A–D 绿，E/F 红，原因不是本测试的假设

* A–D（起桌面 / 起合成器 / 对话框被**摆到屏幕正中** / 居中像素核验）**全绿**。
* E/F（拖动、真点 OK）**红**，卡在两处**产品侧**缺陷上，都还没修：
  1. **客户机在"合成器 + 客户端 surface"共存后 ~10–20 s 整机冻住**：`[TASK64] kheart` 与
     `[UI] perf` 同时停在 `ticks_now≈65100`，之后 QEMU monitor 的 `mouse_move` 完全无效
     （`mv=0,0`、`ev=0/s`）。对照实验：不起合成器时鼠标一路正常、客户机活到 tick 232975。
     怀疑"合成器 `input_poll(12)` 与客户端 `wl_display_dispatch(21)` 并发轮询 ev64 队列"这条
     路径缺锁，**未证实**。
  2. 对话框只能活 8 s（`user/win/peload_crt.c` 的 `MB_TIMEOUT_MS`）：实测超过 ~16 s 进程会被
     内核侧某处 `[PROC64] kill pid=… sig=9` 干掉（exit=137、无 close 行）。把超时抬到 45 s
     会让 `tests/peload64_test.py` 的 ⑩ 段从绿变红，所以**保持 8 s**。拖动 + 闭环定位光标
     要十来秒，8 s 装不下 —— 等冻死与那条 SIGKILL 修好之后再把这个验收做硬。
* 因此本文件的 E/F 段现在是**已知红**，不是回归信号。修好上面两条后重跑即可。
"""
import argparse
import collections
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import qemuhelp as qh                     # noqa: E402
import startmenu64_test as SM             # noqa: E402

DISK = os.path.join(ROOT, "build64", "sysdisk.img")
CORPUS = os.path.join(ROOT, "build64", "pexcorpus")
GUI_EXE_VOL = "/pexcorpus/gui.exe"
WM_ELF_VOL = "/lib/wm.elf"
DOCS = os.path.join(ROOT, "docs")

MSG_SHOW_RE = r"\[PELOAD\] msgboxA show surf=(\d+) shm=(\d+) w=(\d+) h=(\d+) type=(-?\d+)"
MSG_BTN_RE = r"\[PELOAD\] msgboxA btn x=(\d+) y=(\d+) w=(\d+) h=(\d+)"
MSG_CLOSE_RE = r"\[PELOAD\] msgboxA close reason=(clicked|timeout)"
EXIT_RE = r"\[PROC64\] exit pid=\d+ code=(-?\d+)"
WM_COMPOSER_RE = r"\[WM\] composer seat=(\d+) gpu=(\d+) region=(-?\d+),(-?\d+),(\d+),(\d+)"
WM_POLICY_RE = r"\[WM\] policy region=full drag_bar=(\d+) place=center\+cascade repair_ms=(\d+) bg=(\d)"
WM_PLACE_RE = r"\[WM\] place id=(\d+) pos=(-?\d+),(-?\d+) mode=(\w+)"
WL_MOVE_RE = (r"\[WL64\] move id=(\d+) pid=(-?\d+) pos=(-?\d+),(-?\d+) "
              r"from=(-?\d+),(-?\d+) by=(\w+)")
WM_DRAG_BEGIN_RE = r"\[WM\] drag begin id=(\d+) grab=(-?\d+),(-?\d+) from=(-?\d+),(-?\d+)"
WM_DRAG_END_RE = r"\[WM\] drag end id=(\d+) pos=(-?\d+),(-?\d+) moved=(\d+)"
PERF_CUR_RE = r"\[UI\] perf[^\n]*cur=(\d+),(\d+)"

# MessageBoxA 自绘的配色（与 user/win/peload_crt.c 一一对应；改那边要同步改这里）
C_FRAME = (0x20, 0x20, 0x20)
C_TITLE = (0x34, 0x65, 0xA4)
C_BODY = (0xF2, 0xF2, 0xF2)
C_TEXT = (0x10, 0x10, 0x10)
PIX_TOL = 6


# ==================== 结果分类（三态） ====================
class Res(object):
    def __init__(self):
        self.pass_n = 0
        self.fail = []
        self.skip = []

    def __call__(self, name, cond, detail="", skip_env=False):
        if skip_env:
            self.skip.append((name, detail))
            print("  SKIP(env) %s%s" % (name, ("  <- " + detail) if detail else ""))
            return
        if cond:
            self.pass_n += 1
            print("  PASS %s%s" % (name, ("  <- " + detail) if detail else ""))
        else:
            self.fail.append((name, detail))
            print("  FAIL %s%s" % (name, ("  <- " + detail) if detail else ""))

    def summary(self):
        print()
        print("=" * 78)
        print("结果：PASS %d / FAIL %d / SKIP(env) %d" % (self.pass_n, len(self.fail), len(self.skip)))
        for n, d in self.fail:
            print("  FAIL %s%s" % (n, ("  <- " + d) if d else ""))
        for n, d in self.skip:
            print("  SKIP(env) %s%s" % (n, ("  <- " + d) if d else ""))
        print("=" * 78)
        return 0 if not self.fail else 1


check = Res()


# ==================== 零延迟串口（-serial tcp:）+ 增量缓存 ====================
class TcpSerial(object):
    """把 QEMU 的 COM1 接成 TCP，后台线程持续读；`text()` **只在有新数据时**重新拼接。

    为什么必须换掉 `-serial file:`：后者有 1~3 s 落盘延迟，"读日志 -> 算修正量"的闭环永远在用
    旧样本。为什么必须增量缓存：早期版本每次 `text()` 都 `b"".join(所有块)` + 整段 decode，
    日志长到几百 KB 后这个 O(n) 操作把读线程饿死 -> QEMU chardev 反压 -> 客户机串口写阻塞 ->
    整机 1 fps、日志直接断在半路（实测 sec=45 断掉）。
    """

    def __init__(self, port, timeout=15.0):
        self.lock = threading.Lock()
        self.parts = []
        self.cache = ""
        self.dirty = True
        self.stop = False
        self.sock = None
        deadline = time.time() + timeout
        last = None
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=3)
                try:
                    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
                except OSError:
                    pass
                self.sock = s
                break
            except OSError as e:                                   # noqa: PERF203
                last = e
                time.sleep(0.2)
        if self.sock is None:
            raise RuntimeError("连不上串口 tcp 端口 %d：%s" % (port, last))
        self.sock.settimeout(0.3)
        self.t = threading.Thread(target=self._run, daemon=True)
        self.t.start()

    def _run(self):
        while not self.stop:
            try:
                d = self.sock.recv(262144)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            with self.lock:
                self.parts.append(d)
                self.dirty = True

    def text(self):
        with self.lock:
            if self.dirty:
                self.cache = b"".join(self.parts).decode("utf-8", errors="replace")
                self.dirty = False
            return self.cache

    def close(self):
        self.stop = True
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def wait_re(log_fn, rx, timeout, proc=None, step=0.2):
    rx = re.compile(rx)
    t0 = time.time()
    while time.time() - t0 < timeout:
        m = rx.search(log_fn())
        if m:
            return m
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(step)
    return rx.search(log_fn())


def last_of(log, rx):
    ms = list(re.finditer(rx, log))
    return ms[-1] if ms else None


# ==================== 光标闭环（零延迟反馈） ====================
def cur_pos(log):
    ms = list(re.finditer(PERF_CUR_RE, log))
    if not ms:
        return None
    return (int(ms[-1].group(1)), int(ms[-1].group(2)))


def n_perf(log):
    return log.count("[UI] perf")


def wait_perf_after(log_fn, n0, timeout=6.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        lg = log_fn()
        if n_perf(lg) > n0:
            return cur_pos(lg), n_perf(lg)
        time.sleep(0.1)
    return cur_pos(log_fn()), n_perf(log_fn())


def plan_moves(dx, dy):
    """按内核的算法（每包 `dx*17/10`，每包最多走 24 px）把一次位移拆成若干 mouse_move。

    kernel/input.cpp 里 `mouse_accum += dx*17/10` 之后有 MOUSE_ACC_MAX=96 的**截断** ——
    一次请求 400 px 会被夹成 96，剩下 304 px 直接丢掉。每步 <=14（1.7x 后 <=23 px）就不会触发上限。
    """
    cmds = []
    gx = gy = 0
    for _ in range(400):
        rx, ry = dx - gx, dy - gy
        if rx == 0 and ry == 0:
            break
        cx = int(rx * 10 / 17.0)
        cy = int(ry * 10 / 17.0)
        if cx == 0 and rx != 0:
            cx = 1 if rx > 0 else -1
        if cy == 0 and ry != 0:
            cy = 1 if ry > 0 else -1
        cx = max(-14, min(14, cx))
        cy = max(-14, min(14, cy))
        cmds.append("mouse_move %d %d" % (cx, cy))
        gx += int(cx * 17 / 10)
        gy += int(cy * 17 / 10)
    return cmds


def goto_exact(mon, log_fn, tx, ty, tol=3, passes=7, verbose=True):
    """把光标送到 (tx,ty) 并**确认到位**（判据 = 客户机自报的 cur=x,y）。"""
    cur = None
    for p in range(passes):
        n0 = n_perf(log_fn())
        cur, n1 = wait_perf_after(log_fn, n0, timeout=5.0)
        if cur is None:
            if verbose:
                print("    [goto] 没有 [UI] perf 行（拿不到光标位置）")
            return None
        dx, dy = tx - cur[0], ty - cur[1]
        if abs(dx) <= tol and abs(dy) <= tol:
            if verbose:
                print("    [goto] pass %d 到位 cur=%d,%d (目标 %d,%d)" % (p + 1, cur[0], cur[1], tx, ty))
            return cur
        if n1 <= n0:
            if verbose:
                print("    [goto] pass %d 没有新的 [UI] perf（客户机很慢），停在 cur=%d,%d"
                      % (p + 1, cur[0], cur[1]))
            return cur
        cmds = plan_moves(dx, dy)
        if verbose:
            print("    [goto] pass %d cur=%d,%d -> 目标 %d,%d（%d 步）"
                  % (p + 1, cur[0], cur[1], tx, ty, len(cmds)))
        mon.raw(cmds, wait_between=0.03, wait_end=0.12)
    n0 = n_perf(log_fn())
    cur, _ = wait_perf_after(log_fn, n0, timeout=5.0)
    return cur


# ==================== PPM（screendump）：只存不解析，解析放到 quit 之后 ====================
class Shot(object):
    """P6 PPM 的只读视图：**不预先把 100 万像素摊成 list**（那要几秒并一直握着 GIL）。"""

    def __init__(self, path):
        with open(path, "rb") as f:
            self.d = f.read()
        d = self.d
        i = 2
        vals = []
        while len(vals) < 3:
            while d[i:i + 1].isspace():
                i += 1
            j = i
            while not d[j:j + 1].isspace():
                j += 1
            vals.append(int(d[i:j]))
            i = j
        i += 1
        self.w, self.h, self.off = vals[0], vals[1], i

    def at(self, x, y):
        x = max(0, min(self.w - 1, int(x)))
        y = max(0, min(self.h - 1, int(y)))
        o = self.off + 3 * (y * self.w + x)
        return (self.d[o], self.d[o + 1], self.d[o + 2])

    def count_near(self, color, x0, y0, x1, y1, step=1, tol=PIX_TOL):
        n = 0
        cr, cg, cb = color
        d = self.d
        for y in range(max(0, y0), min(self.h, y1), step):
            row = self.off + 3 * y * self.w
            for x in range(max(0, x0), min(self.w, x1), step):
                o = row + 3 * x
                if (abs(d[o] - cr) <= tol and abs(d[o + 1] - cg) <= tol
                        and abs(d[o + 2] - cb) <= tol):
                    n += 1
        return n

    def top_colors(self, x0, y0, x1, y1, k=4):
        c = collections.Counter()
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                c[self.at(x, y)] += 1
        return c.most_common(k)


def save_png(ppm_path, png_path):
    try:
        from PIL import Image
    except Exception:
        return None
    Image.open(ppm_path).save(png_path)
    return png_path


def q(p):
    return '"%s"' % p if " " in p else p


def start_qemu(qemu, img, sport, mport):
    cmd = [qemu, "-name", "wmfull64",
           "-drive", "format=raw,file=%s" % q(img),
           "-boot", "order=c", "-m", "512", "-vga", "std",
           "-display", "none",
           "-serial", "tcp:127.0.0.1:%d,server=on,wait=off" % sport,
           "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
           "-no-reboot"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(240):
        if proc.poll() is not None:
            break
        try:
            socket.create_connection(("127.0.0.1", mport), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return proc


def find_qemu(explicit=None):
    cands = [explicit] if explicit else []
    cands += [r"C:\Program Files\qemu\qemu-system-x86_64.exe",
              r"C:\Program Files\qemu\qemu-system-x86_64w.exe"]
    for c in cands:
        if c and os.path.exists(c):
            return c
    return None


# ==================== 主体 ====================
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=420)
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    print("=== Vimtu64 W3 第二批：全屏 Ring 3 合成器 + 拖动 + 真鼠标点击 ===")
    if not qemu:
        check("环境：找到 qemu-system-x86_64", False, "没找到", skip_env=True)
        return check.summary()
    check("环境：找到 qemu-system-x86_64", True, qemu)
    if not os.path.exists(DISK):
        check("环境：build64/sysdisk.img 存在", False, DISK, skip_env=True)
        return check.summary()
    check("环境：build64/sysdisk.img 存在", True, "%.1f MiB" % (os.path.getsize(DISK) / 1048576.0))

    gui_exe = os.path.join(CORPUS, "gui.exe")
    wm_elf = os.path.join(ROOT, "build64", "wm.elf")
    miss = [p for p in (gui_exe, wm_elf) if not os.path.exists(p)]
    if miss:
        check("环境：PE 语料 + wm.elf 都在", False,
              "缺 %s（先跑 py -3 tools/pex_corpus_build.py）" % miss, skip_env=True)
        return check.summary()
    check("环境：PE 语料 + wm.elf 都在", True,
          "gui.exe=%d B wm.elf=%d B" % (os.path.getsize(gui_exe), os.path.getsize(wm_elf)))

    work = tempfile.mkdtemp(prefix="vimtu64_wmfull_")
    img = os.path.join(work, "disk.img")
    shutil.copyfile(DISK, img)
    sport, mport = free_port(), free_port()
    print("    盘：%s" % img)
    print("    串口 tcp:%d（零延迟）/ monitor telnet:%d" % (sport, mport))
    print()

    proc, ser = None, None
    shots = {}
    rect = None
    rect2 = None
    btn = None
    w = h = 0
    try:
        proc = start_qemu(qemu, img, sport, mport)
        ser = TcpSerial(sport)
        mon = SM.Monitor(mport)
        log_fn = ser.text

        # ---------- A. 桌面 ----------
        if not qh.wait_desktop(mon, log_fn, proc, timeout=400, login_timeout=460):
            check("A. 起桌面（qemuhelp.wait_desktop 含登录手势）", False, "超时")
            print("    串口尾部：\n%s" % log_fn()[-2000:])
            return check.summary()
        check("A. 起桌面（qemuhelp.wait_desktop 含登录手势）", True)
        time.sleep(1.0)
        c0 = cur_pos(log_fn())
        check("A. 拿到客户机自报的光标位置（[UI] perf … cur=x,y，零延迟串口）",
              c0 is not None, "cur=%s" % (c0,))

        # ---------- B. 开终端 ----------
        try:
            from peload64_test import open_terminal_dock
            from install_persist64_test import type_line
        except Exception as e:                                     # pragma: no cover
            check("B. 复用既有终端夹具（open_terminal_dock / type_line）", False, repr(e), skip_env=True)
            return check.summary()
        if not open_terminal_dock(log_fn, mon):
            check("B. Dock 打开终端（[APP] term opened）", False, "Dock 入口没成功")
            return check.summary()
        check("B. Dock 打开终端（[APP] term opened）", True)
        time.sleep(1.0)

        # ---------- C. 后台起合成器 ----------
        print()
        print("=== C. elfrun bg /lib/wm.elf（把 Ring 3 合成器起成**后台常驻**进程）===")
        print("    为什么必须 bg：`elfrun` 的等待模式有 120 s 上限，长驻进程会被 SIGKILL（实测 119 s）。")
        type_line(mon, "elfrun bg %s" % WM_ELF_VOL, per=0.10)
        mc = wait_re(log_fn, WM_COMPOSER_RE, 60, proc)
        check("C. 合成器注册（[WM] composer seat=.. gpu=.. region=..）", mc is not None,
              mc.group(0) if mc else "没等到（wm 没起来？）")
        if not mc:
            print("    串口尾部：\n%s" % log_fn()[-2500:])
            return check.summary()
        g_region = (int(mc.group(3)), int(mc.group(4)), int(mc.group(5)), int(mc.group(6)))
        check("C. ① 合成器地盘 = **整屏**（不再是 320x200 面板区）",
              g_region[0] == 0 and g_region[1] == 0 and g_region[2] >= 1024 and g_region[3] >= 700,
              "region=%s" % (g_region,))
        mp = wait_re(log_fn, WM_POLICY_RE, 20, proc)
        check("C. 摆放/拖动策略打点（[WM] policy region=full drag_bar=.. place=center+cascade）",
              mp is not None, mp.group(0) if mp else "缺")
        if mp:
            check("C. ④ 桌面背景快照拿到了（bg=1，拖动才不留拖影）", mp.group(3) == "1",
                  "bg=%s（0 = 快照没拿到，拖动会留影）" % mp.group(3))

        # ---------- D. Windows GUI 程序：对话框必须落在屏幕正中 ----------
        print()
        print("=== D. run /bin/peload /pexcorpus/gui.exe（MessageBoxA 弹窗）===")
        type_line(mon, "run /bin/peload %s" % GUI_EXE_VOL, per=0.08)
        mshow = wait_re(log_fn, MSG_SHOW_RE, args.timeout, proc)
        check("D. 对话框建起来了（[PELOAD] msgboxA show surf=.. shm=.. w=300 h=150）",
              mshow is not None, mshow.group(0) if mshow else "没等到")
        if not mshow:
            print("    串口尾部：\n%s" % log_fn()[-3000:])
            return check.summary()
        w, h = int(mshow.group(3)), int(mshow.group(4))
        mbtn = wait_re(log_fn, MSG_BTN_RE, 10, proc)
        if mbtn:
            btn = (int(mbtn.group(1)), int(mbtn.group(2)), int(mbtn.group(3)), int(mbtn.group(4)))
        check("D. OK 按钮矩形存在（[PELOAD] msgboxA btn x=.. y=.. w=.. h=..）", btn is not None,
              mbtn.group(0) if mbtn else "缺")

        mplace = wait_re(log_fn, WM_PLACE_RE, 30, proc)
        check("D. ② 合成器把窗口摆到屏幕正中（[WM] place .. mode=center）",
              mplace is not None and mplace.group(4) == "center",
              mplace.group(0) if mplace else "缺")
        mwl = last_of(log_fn(), WL_MOVE_RE)
        check("D. ② 位置是**内核**改的（[WL64] move .. by=composer，不是 wm 自己的影子坐标）",
              mwl is not None and mwl.group(7) == "composer", mwl.group(0) if mwl else "缺")
        check("D. ③ 合成器注册后内核不再自己合成（[WL64] dispatch … composite=external）",
              "composite=external" in log_fn(), "")

        fbw, fbh = 1280, 800
        mfb = re.search(r"\[WM\] map pid=\d+ va=\S+ w=(\d+) h=(\d+) ", log_fn())
        if mfb:
            fbw, fbh = int(mfb.group(1)), int(mfb.group(2))
        if mplace:
            rect = (int(mplace.group(2)), int(mplace.group(3)), w, h)
            check("D. ② 居中坐标 == (fb-w)/2,(fb-h)/2",
                  abs(rect[0] - (fbw - w) // 2) <= 2 and abs(rect[1] - (fbh - h) // 2) <= 2,
                  "pos=%d,%d want=%d,%d" % (rect[0], rect[1], (fbw - w) // 2, (fbh - h) // 2))
        else:
            check("D. 拿到对话框屏幕矩形（[WM] place）", False, "缺")
            return check.summary()

        time.sleep(0.6)
        shots["s1"] = os.path.join(work, "s1.ppm")
        mon.shot(shots["s1"], wait=1.0)
        check("D. screendump #1 取到了", os.path.exists(shots["s1"]),
              "%d B" % (os.path.getsize(shots["s1"]) if os.path.exists(shots["s1"]) else 0))

        # ---------- E. 拖动 ----------
        print()
        print("=== E. 窗口拖动（按住标题栏 -> 移动 -> 松开）===")
        dx_want, dy_want = -240, -140
        grab_x, grab_y = rect[0] + 140, rect[1] + 10      # 顶部 22 px = 拖动条
        cur = goto_exact(mon, log_fn, grab_x, grab_y)
        check("E. 光标送到标题栏抓取点 (%d,%d)" % (grab_x, grab_y),
              cur is not None and abs(cur[0] - grab_x) <= 3 and abs(cur[1] - grab_y) <= 3,
              "cur=%s" % (cur,))

        mon.raw(["mouse_button 1"], wait_between=0.05, wait_end=0.30)
        mb = wait_re(log_fn, WM_DRAG_BEGIN_RE, 12, proc)
        check("E. 合成器认出「按住标题栏 = 开始拖动」（[WM] drag begin）", mb is not None,
              mb.group(0) if mb else "缺")

        cur = goto_exact(mon, log_fn, grab_x + dx_want, grab_y + dy_want)
        check("E. 光标移动到 (%d,%d)" % (grab_x + dx_want, grab_y + dy_want),
              cur is not None and abs(cur[0] - (grab_x + dx_want)) <= 3
              and abs(cur[1] - (grab_y + dy_want)) <= 3, "cur=%s" % (cur,))
        mon.raw(["mouse_button 0"], wait_between=0.05, wait_end=0.45)
        me = wait_re(log_fn, WM_DRAG_END_RE, 12, proc)
        check("E. 松手收尾（[WM] drag end .. moved=N>0）",
              me is not None and int(me.group(4)) > 0, me.group(0) if me else "缺")
        n_move = len(re.findall(r"\[WM\] drag move id=\d+", log_fn()))
        check("E. 拖动过程中真的挪了很多步（[WM] drag move × N，N>=3）", n_move >= 3, "N=%d" % n_move)

        # 拖动后的矩形：**从日志读**（内核 surface 表是唯一事实来源）
        mpos = last_of(log_fn(), WL_MOVE_RE)
        if mpos:
            rect2 = (int(mpos.group(3)), int(mpos.group(4)), w, h)
            check("E. ② 内核 surface 表里的 pos 跟着变了（[WL64] move .. pos=.. last）",
                  rect2[0] != rect[0] or rect2[1] != rect[1],
                  "from=%d,%d to=%d,%d" % (rect[0], rect[1], rect2[0], rect2[1]))
            check("E. ② 日志里的位移 == 拖动位移 (%d,%d)（实测 %d,%d，容差 12）"
                  % (dx_want, dy_want, rect2[0] - rect[0], rect2[1] - rect[1]),
                  abs((rect2[0] - rect[0]) - dx_want) <= 12
                  and abs((rect2[1] - rect[1]) - dy_want) <= 12,
                  "got=(%d,%d)" % (rect2[0] - rect[0], rect2[1] - rect[1]))
        else:
            check("E. ② 拖动后拿到新的 [WL64] move pos", False, "缺")

        time.sleep(0.6)
        shots["s2"] = os.path.join(work, "s2.ppm")
        mon.shot(shots["s2"], wait=1.0)
        check("E. screendump #2 取到了", os.path.exists(shots["s2"]), "")

        # ---------- F. 真鼠标点掉 OK ----------
        print()
        print("=== F. 真鼠标点击 OK 按钮（闭环定位到控件中心）===")
        if not btn or not rect2:
            check("F. OK 按钮矩形 + 拖动后窗口矩形都已知", False, "缺 btn/rect2", skip_env=True)
        else:
            bx = rect2[0] + btn[0] + btn[2] // 2
            by = rect2[1] + btn[1] + btn[3] // 2
            print("    OK 按钮中心（屏幕坐标）= (%d,%d)（窗口在 %d,%d）" % (bx, by, rect2[0], rect2[1]))
            cur = goto_exact(mon, log_fn, bx, by, tol=2)
            check("F. 光标精确落在 OK 按钮中心 (%d,%d)" % (bx, by),
                  cur is not None and abs(cur[0] - bx) <= 2 and abs(cur[1] - by) <= 2,
                  "cur=%s" % (cur,))
            check("F. 落点在按钮矩形内（rect2 + btn）",
                  cur is not None
                  and (rect2[0] + btn[0] <= cur[0] < rect2[0] + btn[0] + btn[2])
                  and (rect2[1] + btn[1] <= cur[1] < rect2[1] + btn[1] + btn[3]),
                  "cur=%s rect2=%s btn=%s" % (cur, rect2, btn))

            mon.raw(["mouse_button 1"], wait_between=0.05, wait_end=0.25)
            mon.raw(["mouse_button 0"], wait_between=0.05, wait_end=0.40)
            mc2 = wait_re(log_fn, MSG_CLOSE_RE, 30, proc)
            check("F. ③ **真鼠标点击**让对话框以 reason=clicked 关闭（不是超时）",
                  mc2 is not None and mc2.group(1) == "clicked",
                  mc2.group(0) if mc2 else "缺（timeout 或没关闭）")
            mroute = last_of(log_fn(), r"\[WM\] route surf=(\d+) pid=(\d+) type=(\d) "
                                        r"x=(\d+) y=(\d+) sx=(-?\d+) sy=(-?\d+)")
            check("F. 事件确实经合成器路由到窗口（[WM] route … sx/sy 是窗口局部坐标）",
                  mroute is not None, mroute.group(0) if mroute else "缺")
            wait_re(log_fn, EXIT_RE, 40, proc)
            mexit = last_of(log_fn(), EXIT_RE)          # 取**最后一条**（第一条不是 peload 的）
            check("F. GUI 进程正常退出（最后一条 [PROC64] exit code=0）",
                  mexit is not None and mexit.group(1) == "0", mexit.group(0) if mexit else "缺")

            time.sleep(0.8)
            shots["s3"] = os.path.join(work, "s3.ppm")
            mon.shot(shots["s3"], wait=1.0)
            check("F. screendump #3 取到了", os.path.exists(shots["s3"]), "")

        check("G. 全程没有 PANIC / TRIPLE FAULT / selftest FAIL / composer 被 kill",
              not re.search(r"PANIC|TRIPLE FAULT|selftest FAIL|FAILED mask=", log_fn())
              and "composer release" not in log_fn().split("[WM] policy")[-1].split("[WM] exit")[0],
              "")

        # 串口落盘（事后可复盘；VM 关掉后再做像素分析，避免拖死 TCP 读线程）
        seg = log_fn()
        dst = os.path.join(os.environ.get("PI_SCRATCH_DIR", work), "wmfull_serial.log")
        try:
            with open(dst, "w", encoding="utf-8", errors="replace") as f:
                f.write(seg)
            print("    串口日志 -> %s（%d 字节）" % (dst, len(seg)))
        except OSError:
            pass
        try:
            mon.send("quit")
        except OSError:
            pass
    finally:
        if ser:
            ser.close()
        try:
            if proc and proc.poll() is None:
                proc.terminate()
                time.sleep(1.0)
                if proc.poll() is None:
                    proc.kill()
        except Exception:
            pass

    # ==================== H. 像素核验（VM 已关，随便算） ====================
    print()
    print("=== H. 像素核验（screendump，VM 已关）===")
    if os.path.exists(shots.get("s1", "")) and rect:
        S1 = Shot(shots["s1"])
        x0, y0 = rect[0], rect[1]
        n_title = S1.count_near(C_TITLE, x0 + 4, y0 + 4, x0 + w - 4, y0 + 20)
        check("H. ① 对话框在屏幕正中：标题栏 #3465A4 实心长条（>=2000 px）", n_title >= 2000,
              "n=%d rect=%s" % (n_title, rect))
        n_body = S1.count_near(C_BODY, x0 + 8, y0 + 34, x0 + w - 8, y0 + h - 8)
        check("H. ① 正文底 #F2F2F2（>=15000 px）", n_body >= 15000, "n=%d" % n_body)
        n_text = S1.count_near(C_TEXT, x0 + 10, y0 + 28, x0 + w - 10, y0 + h - 34)
        check("H. ① 正文有黑色字形（>=300 px）", n_text >= 300, "n=%d" % n_text)
        n_left = S1.count_near(C_TITLE, 0, 0, max(0, x0 - 60), S1.h, step=2)
        check("H. ① 对话框左边 60px 之外没有标题栏色（不是整屏染色）", n_left == 0, "n=%d" % n_left)
        # 旧的面板槽位（kernel/wl64.cpp 的默认 pos 964,532）不该再有对话框
        n_panel = S1.count_near(C_TITLE, 960, 528, 1270, 552, step=2)
        check("H. ① 旧的面板槽位（964,532）上已经没有对话框（= 真的搬走了）", n_panel == 0,
              "n=%d" % n_panel)
    else:
        check("H. ① screendump #1 可用于核验", False, "缺图")

    if os.path.exists(shots.get("s2", "")) and rect2 and rect:
        S2 = Shot(shots["s2"])
        x2, y2 = rect2[0], rect2[1]
        n_new = S2.count_near(C_TITLE, x2 + 4, y2 + 4, x2 + w - 4, y2 + 20)
        check("H. ② 拖动后：新位置的标题栏是 #3465A4（>=2000 px）", n_new >= 2000,
              "n=%d rect2=%s" % (n_new, rect2))
        n_old = S2.count_near(C_TITLE, rect[0] + 4, rect[1] + 4, rect[0] + w - 4, rect[1] + 20)
        check("H. ② 旧位置已被背景快照还原（不留拖影：旧标题栏色 = 0）", n_old == 0,
              "n=%d（>0 = 拖影）" % n_old)
        n_body2 = S2.count_near(C_BODY, x2 + 8, y2 + 34, x2 + w - 8, y2 + h - 8)
        check("H. ② 新位置的正文底也是 #F2F2F2（>=15000 px）", n_body2 >= 15000, "n=%d" % n_body2)
    else:
        check("H. ② screendump #2 可用于核验", False, "缺图或坐标")

    if os.path.exists(shots.get("s3", "")):
        S3 = Shot(shots["s3"])
        n_left3 = S3.count_near(C_TITLE, 0, 0, S3.w, S3.h, step=2)
        check("H. ③ 点击后屏幕上完全没有对话框了（标题栏色 = 0）", n_left3 == 0, "n=%d" % n_left3)
    else:
        check("H. ③ screendump #3 可用于核验", False, "缺图")

    # 交付物
    if os.path.isdir(DOCS):
        for key, name in (("s1", "w3b_msgbox_center"), ("s2", "w3b_dragged"), ("s3", "w3b_after_click")):
            if os.path.exists(shots.get(key, "")):
                dst = os.path.join(DOCS, name + ".ppm")
                shutil.copyfile(shots[key], dst)
                if save_png(dst, os.path.join(DOCS, name + ".png")):
                    print("    截图 -> docs/%s.png" % name)
                else:
                    print("    截图 -> docs/%s.ppm（没有 PIL，未转 PNG）" % name)
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
