#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/textstress64_test.py - P7a ④「文字渲染重叠错乱」的**加压复现 + 压力断言**（不许削弱）。

根因（本次修的就是它，见 kernel/font.cpp rasterize_glyph 的 ★ 注释）：
  `rasterize_glyph()` 是**累加**语义（同一字形轮廓自重叠要叠加覆盖率），但两个调用方
  （ASCII 固定缓存 cur->cache[idx] / CJK LRU 槽 fc->lru_bm[victim]）**复用位图却从不清零**
  （font.cpp 里一个 memset 都没有）：
    * font_set_size64() 把 cacheOk[] 全置 false 但没有清位图 -> 重光栅化时叠上**旧字号**同一字形；
    * CJK LRU 槽换出后装的是**别的字形** -> 叠上"上一个字的残影"（偶发，取决于换出顺序）。
  => 同一个字符串在**缓存被搅动之后**再画，像素会不一样（变粗/带残影）= 用户看到的偶发重叠错乱。

断言（本脚本的核心，**逐帧/逐循环按像素校验，不放宽**）：
  1) 基准帧：桌面稳定后，分别抓
       * 开始菜单区域（Win 键打开；几何来自 [START64] geom 打点）—— 里面是 CJK 应用名（字形 LRU 的主用户）
       * 任务管理器窗口的**标题栏文字带**（Ctrl+Shift+Esc 打开；几何来自 [UI] win geom 打点）
  2) 每个循环先"搅动字形缓存"（Win 菜单开/关 ×2 = 画一遍中文应用名网格；Ctrl+Shift+T 切主题两下
     = 整屏重建、所有文字重画；任务管理器开关一遍），再抓**同一区域**与基准逐像素比对。
  3) 判定：max|Δ| == 0（逐像素完全一致）。同一串文字、同一字号、同一主题下重画必须**逐像素确定**；
     任何非零差异都说明光栅缓冲被复用污染（= 缺陷）。
  运行顺序建议：先跑未修的镜像（期望 FAIL，即复现），再跑修好的镜像（期望 PASS）。

用法：py -3 tests/textstress64_test.py [--img build64/system.img] [--cycles 6] [--port 5661]
退出码：0 = 全部循环逐像素一致（无重叠错乱）；1 = 有循环出现像素差异（复现）。
"""
import argparse
import os
import socket
import subprocess
import sys
import tempfile
import time

import numpy as np

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
LOCK_INTERACTIVE = "[LOCK64] bg blur ready"
DESKTOP_READY = "[GUI64] ready"


def q(p):
    return p.replace("\\", "/")


def find_qemu(explicit=None):
    for c in ([explicit] if explicit else []) + QEMU_CANDIDATES:
        if not c:
            continue
        if os.path.isabs(c) and os.path.exists(c):
            return c
        if not os.path.isabs(c):
            from shutil import which
            w = which(c)
            if w:
                return w
    return None


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
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

    def key(self, name, wait=0.9):
        return self.send("sendkey %s" % name, wait=wait)

    def shot(self, path, wait=1.6):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                try:
                    return read_ppm(path)
                except OSError:
                    return None
            time.sleep(0.3)
        return None


def read_ppm(path):
    data = open(path, "rb").read()
    pos, vals = 2, []
    while len(vals) < 3:
        while data[pos:pos + 1].isspace():
            pos += 1
        s = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(data[s:pos]))
    pos += 1
    w, h = vals[0], vals[1]
    return w, h, data[pos:pos + w * h * 3]


def frame_arr(ppm):
    w, h, data = ppm
    a = np.frombuffer(data, dtype=np.uint8)
    return a.reshape(h, w, 3)


def max_diff(a, b, box):
    x, y, w, h = box
    ra = a[y:y + h, x:x + w].astype(np.int16)
    rb = b[y:y + h, x:x + w].astype(np.int16)
    n = min(ra.shape[0], rb.shape[0]), min(ra.shape[1], rb.shape[1])
    ra, rb = ra[:n[0], :n[1]], rb[:n[0], :n[1]]
    if ra.size == 0:
        return 999, 0.0
    d = abs(ra - rb)
    return int(d.max()), float((d.max(axis=2) > 0).mean())


class Vm:
    def __init__(self, qemu, img, port, tag):
        self.tmp = os.path.join(tempfile.gettempdir(), "vimtu_textstress_%d" % port)
        os.makedirs(self.tmp, exist_ok=True)
        self.serial = os.path.join(self.tmp, tag + "_serial.log")
        for p in (self.serial,):
            if os.path.exists(p):
                os.remove(p)
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-textstress",
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port

    def monitor(self):
        for _ in range(120):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return Monitor(self.port)
            except OSError:
                time.sleep(0.25)
        return None

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
        if self.proc.poll() is None:
            self.proc.kill()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass


import re                                                            # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5661)
    ap.add_argument("--cycles", type=int, default=6)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", args.img and os.path.dirname(args.img)),
                          "textstress")
    os.makedirs(outdir, exist_ok=True)
    logf = open(os.path.join(outdir, "textstress64_test.log"), "a", encoding="utf-8")

    def L(*a):
        line = " ".join(str(x) for x in a)
        print(line, flush=True)
        logf.write(line + "\n")
        logf.flush()

    qemu = find_qemu(args.qemu)
    if not qemu:
        L("跳过：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        L("跳过：镜像不存在 %s（先跑 build64.sh）" % args.img)
        return 2
    L("=== textstress64_test：逐循环逐像素校验文字重绘确定性（④）img=%s ===" % args.img)
    vm = Vm(qemu, args.img, args.port, "run")
    ok = True
    try:
        mon = vm.monitor()
        if not mon:
            L("FAIL 连不上 QEMU monitor")
            return 1
        if not vm.wait_log(LOCK_INTERACTIVE, 240):
            L("FAIL 等不到锁屏；串口尾=%r" % vm.log()[-300:])
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(DESKTOP_READY, 180):
            L("FAIL 登录后等不到桌面")
            return 1
        time.sleep(3.0)

        def shot(name):
            return mon.shot(os.path.join(outdir, name))

        def sm_box():
            m = re.search(r"\[START64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log())
            if not m:
                return None
            k = [int(v) for v in m.groups()]
            # 只取"应用名网格"那一片（避开可能含时间的部件）：菜单下方 60% 高度、左侧 90% 宽
            return (k[0] + 8, k[1] + int(k[3] * 0.35), k[2] - 16, int(k[3] * 0.60))

        def tm_title_box():
            m = None
            for mm in re.finditer(r"\[UI\] win geom tag=\S+ title=(\S+) app=(\d+) x=(-?\d+) y=(-?\d+) "
                                  r"w=(\d+) h=(\d+)", vm.log()):
                if mm.group(2) == "4":                      # APP_ID_TMGR
                    m = mm
            if not m:
                return None
            x, y, w = int(m.group(3)), int(m.group(4)), int(m.group(5))
            return (x + 6, y + 4, min(w - 100, 320), 18)     # 标题文字那一条（避开右侧三按钮）

        # ---- 基准帧 ----
        mon.key("meta_l", 1.2)                               # Win：打开开始菜单
        time.sleep(0.8)
        base_sm = shot("sm_base.ppm")
        mon.key("esc", 0.8)
        mon.key("ctrl-shift-esc", 1.6)                       # 任务管理器（中文标题 + 大量中文标签）
        time.sleep(1.2)
        base_tm = shot("tm_base.ppm")
        box_sm, box_tm = sm_box(), tm_title_box()
        L("[base] 开始菜单区域=%s  任务管理器标题带=%s" % (box_sm, box_tm))
        if base_sm is None or base_tm is None or not box_sm or not box_tm:
            L("FAIL 基准帧/几何缺失（sm=%s tm=%s）" % (box_sm, box_tm))
            return 1
        A_sm, A_tm = frame_arr(base_sm), frame_arr(base_tm)
        mon.key("esc", 1.0)                                  # 关任务管理器

        for cyc in range(1, args.cycles + 1):
            # ---- 搅动 CJK 字形缓存（**只有这一条路会在运行期换出 LRU 槽**）----
            #   为什么不用 Ctrl+Shift+T 切主题：切换是"时长/颜色"全局变化，只要有一次按键丢，
            #   后面所有帧都会和基准不同（第一版就吃了这个假阳性）；只用"开/关窗口 + 菜单网格"，
            #   每个窗口/菜单画的都是**同一批中文标签**，静止后重画必须逐像素一致。
            for _ in range(3):
                mon.key("meta_l", 1.0)                        # 开始菜单：中文应用名网格（~30 字形）
                mon.key("esc", 0.7)
                mon.key("ctrl-shift-esc", 1.4)                # 任务管理器：大量中文标签（~100 字形）
                time.sleep(0.8)
                mon.key("esc", 0.7)
            # ---- 采集（与基准同样的姿态与顺序）----
            mon.key("ctrl-shift-esc", 1.6)
            time.sleep(1.0)
            cur_tm = shot("tm_%d.ppm" % cyc)
            mon.key("esc", 1.0)
            sm_mark = len(vm.log())
            mon.key("meta_l", 1.2)
            time.sleep(0.8)
            cur_sm = shot("sm_%d.ppm" % cyc)
            menu_open = "[START64] open why=" in vm.log()[sm_mark:]
            mon.key("esc", 0.8)
            if cur_sm is None or cur_tm is None:
                L("[cycle %d] FAIL 抓帧失败" % cyc)
                ok = False
                continue
            if not menu_open:
                L("[cycle %d] 开始菜单没能打开（按键丢了）—— 本轮不计入断言" % cyc)
                continue
            d_sm, f_sm = max_diff(A_sm, frame_arr(cur_sm), box_sm)
            d_tm, f_tm = max_diff(A_tm, frame_arr(cur_tm), box_tm)
            L("[cycle %d] 开始菜单区域 max|d|=%3d 差异像素=%.4f | 任务管理器标题带 max|d|=%3d 差异像素=%.4f"
              % (cyc, d_sm, f_sm, d_tm, f_tm))
            if d_sm != 0 or d_tm != 0:
                ok = False
        L("PASS/FAIL: %s（逐像素确定 = %s）" % ("PASS" if ok else "FAIL", ok))
        L("证据帧目录：%s" % outdir)
    finally:
        if not args.keep:
            vm.close()
            time.sleep(1)
    logf.close()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
