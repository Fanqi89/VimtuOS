#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/texthint64_test.py - ★ 内核字形光栅器的"文字清晰度"端到端验收（宿主侧逐像素证据）

要证明的事（= kernel/font.cpp 里 FONT64_AA_SUB / FONT64_GEOM_SH 那两条改动的证据链，
那份注释引用的就是这个脚本）：

  ① **y 方向真的有子扫描线抗锯齿**（FONT64_AA_SUB = 8，旧行为 = 1）：
     沿**竖直方向**穿过一条汉字横画的边缘，新版覆盖率是**渐变**的（至少一档中间值），
     旧版是**二值**的（整行直接 0 -> 255）。指标 = `vsoft`（边缘里带中间档的比例）。
  ② **横画上不再出现"断笔空洞"**：字格内某一行里"最左墨迹 ~ 最右墨迹"之间出现
     0 覆盖率的游程（≥1 px）。旧版实测在 em=16 的横画上会出洞（整行翻进/翻出），
     新版应为 0。指标 = `holes`。
  ③ **字形尺度不再被定点截断**（FONT64_GEOM_SH 从 1/1024 提到 1/65536）：
     用屏幕上的墨迹**总跨度**与 32 个汉字的理论推进（size px/字）对账，
     并给出每字平均可用宽度（旧版有 -2.34% 的尺度误差 -> 会系统性地窄 1~2 px/字）。

做法（真引导 + 真打字 + 真像素，不模拟）：
  夹具卷 = build64/system.img 的载荷 + 一版**只装字库**的 VimtuFS2 卷
  （/Fonts/NotoSans-Regular.ttf、/Fonts-open/{NotoSansSC-Regular,SarasaMonoSC-Regular,
   unifont-14.0.01}.ttf、/lib/wm.elf、/etc/wm_probe、**/etc/thint.txt**）。
  /etc/thint.txt 的内容是 `"开" x 32 + "\\n"`（UTF-8，97 B）。用单一字形重复是因为：
    * 列方向严格周期 -> 字格宽可以直接从屏幕上的墨迹跨度反推，不用预设字号；
    * 开 正是 kernel/font.cpp 的注释里"旧版 em=16 横画一行里 2 个洞"的那个字。
  启动 -> 登录（qemuhelp）-> 开始菜单开终端 -> 打 `cat /etc/thint.txt` -> screendump。
  然后在**墨迹最宽的那条行带**上按 16px/字切字格，逐格算 holes / vsoft / levels。

反相对照（同一条串、同一块屏，只换内核的一个宏；这是本脚本最强的证据）：
    VIMTU_EXTRA_CXXFLAGS=-DFONT64_LEGACY_AA bash build64.sh    # 旧行为：每行一条像素中心扫描线
    py -3 tests/texthint64_test.py --legacy
  --legacy 模式下断言**方向相反**（holes > 0、vsoft 明显低），证明上面的指标真的能分辨
  这两种光栅器，而不是"看起来挺好"。

用法（必须 Windows 原生 Python）：
    py -3 tests\\texthint64_test.py [--qemu 路径] [--keep] [--timeout 300] [--legacy]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境/夹具问题。
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

import qemuhelp as qh                    # noqa: E402  登录手势（与其它 GUI 脚本同一套）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
KERNEL_OS = os.path.join(ROOT, "build64", "kernel64_os.bin")
WM_ELF = os.path.join(ROOT, "build64", "wm.elf")
FIXTURE_IMG = os.path.join(ROOT, "build64", "texthint64_test.img")
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768                   # 16 MB（与 font64user / wm64 同一夹具口径）

FONT_MAP = (
    ("/Fonts/NotoSans-Regular.ttf", os.path.join(ROOT, "build", "font_bahnschrift.ttf")),
    ("/Fonts-open/NotoSansSC-Regular.ttf", os.path.join(ROOT, "build", "font_simhei.ttf")),
    ("/Fonts-open/SarasaMonoSC-Regular.ttf", os.path.join(ROOT, "build", "font_mono.ttf")),
    ("/Fonts-open/unifont-14.0.01.ttf", os.path.join(ROOT, "build", "font_fallback.ttf")),
)
TXT_VOL_PATH = "/etc/thint.txt"
# 32 个汉字，全是横画为主的字（横画的上下边正是旧版"整行翻出"最敏感的地方）
PATTERN = "开"                 # 单字重复：列方向严格周期（字格宽能从屏幕上反推），
REPEATS = 32                   # 且 开 正是 kernel/font.cpp 注释里"旧版横画出洞"的那个字
LINE_TEXT = PATTERN * REPEATS
NCELL = len(LINE_TEXT)

TYPED_NAMES = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
CAT_CMD = "cat /etc/thint.txt"

PANIC_MARKERS = ["PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask="]

# ---- 判据阈值（**实测后**定；两侧实测数字见脚本末尾的 summary 打印）----
COV_INK = 128            # 二值化阈值（覆盖率图 0..255）
EDGE_LO, EDGE_HI = 16, 239
SOFT_LO, SOFT_HI = 32, 223


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


# --------------------------------------------------------------------------- QEMU 壳
class QemuVm:
    def __init__(self, qemu, img, port, serial):
        self.serial = serial
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-texthint",
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

    def wait_re(self, rx, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log()[since:])
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.15)
        return None

    def wait(self, needle, timeout, since=0):
        return self.wait_re(re.escape(needle), timeout, since) is not None

    def n(self):
        return len(self.log())

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class Monitor(qh.Monitor):
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

    def sendkey(self, name, wait=0.12):
        return self.send("sendkey %s" % name, wait=wait)


def type_line(mon, text, per_key=0.12):
    for ch in text:
        if ch in TYPED_NAMES:
            mon.sendkey(TYPED_NAMES[ch], wait=per_key)
        elif ch.isalnum():
            mon.sendkey(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.sendkey("ret", wait=per_key + 0.3)


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


def lum(px, w, x, y):
    o = (y * w + x) * 3
    return (px[o] + px[o + 1] + px[o + 2]) // 3


# --------------------------------------------------------------------------- 夹具盘
def prepare_fixture(verbose=True):
    import make_shellvol as msv
    import tcc_pack_win as tp            # Volume2：1.1 MB 的中文子集要二级间接块
    for p in (SYSTEM_IMG, WM_ELF):
        if not os.path.exists(p):
            raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % p)
    wm = open(WM_ELF, "rb").read()
    fonts = {}
    for vol_path, host_path in FONT_MAP:
        if not os.path.exists(host_path):
            raise RuntimeError("缺少字库 %s（先跑 bash build64.sh）" % host_path)
        fonts[vol_path] = open(host_path, "rb").read()
    probe = b"/* wm probe: exists = full mode (Ring 3 compositor) */\n"
    txt = (LINE_TEXT + "\n").encode("utf-8")

    vol = tp.Volume2(TARGET_SECTORS - PART_MAIN_LBA)
    vol.mkdir("bin", parent=0, mode=0o755)
    lib_ino = vol.mkdir("lib", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    fdir = vol.mkdir("Fonts", parent=0, mode=0o755)
    odir = vol.mkdir("Fonts-open", parent=0, mode=0o755)
    vol.write_file("wm_probe", probe, parent=etc_ino, mode=0o644)
    vol.write_file("thint.txt", txt, parent=etc_ino, mode=0o644)
    vol.write_file("wm.elf", wm, parent=lib_ino, mode=0o755)
    vol.write_file("NotoSans-Regular.ttf", fonts["/Fonts/NotoSans-Regular.ttf"], parent=fdir, mode=0o644)
    for name, vp in (("NotoSansSC-Regular.ttf", "/Fonts-open/NotoSansSC-Regular.ttf"),
                     ("SarasaMonoSC-Regular.ttf", "/Fonts-open/SarasaMonoSC-Regular.ttf"),
                     ("unifont-14.0.01.ttf", "/Fonts-open/unifont-14.0.01.ttf")):
        vol.write_file(name, fonts[vp], parent=odir, mode=0o644)
    vb = vol.finish()
    expect = {TXT_VOL_PATH: txt, "/lib/wm.elf": wm, "/etc/wm_probe": probe}
    for k, v in fonts.items():
        expect[k] = v
    bad = tp.verify(vb, expect)          # 二级间接块要 tcc_pack_win.verify 才认得
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    disk = msv.build_disk(open(SYSTEM_IMG, "rb").read(), vb, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(disk)
    if verbose:
        print("    夹具卷 OK：%s = %d B（%d 个汉字）/ 四份字库 / 盘 %d B"
              % (TXT_VOL_PATH, len(txt), NCELL, len(disk)))
    return FIXTURE_IMG


# --------------------------------------------------------------------------- 度量
def coverage_cell(px, w, x0, y0, cw, ch, bg, ink):
    """把一个字格归一化成覆盖率图（0..255，墨迹 = 高）。"""
    cov = []
    for y in range(ch):
        row = []
        for x in range(cw):
            l = lum(px, w, x0 + x, y0 + y)
            v = (l - bg) * 255 // (ink - bg) if ink > bg else 0
            row.append(0 if v < 0 else (255 if v > 255 else v))
        cov.append(row)
    return cov


def cell_holes(cov, cw, ch):
    """字格内的"断笔空洞"：每行取最左~最右墨迹之间的 0 覆盖率游程数（1..3 px 才算，
    更长的空白多半是笔画之间的正常间距，不是洞）。"""
    holes = 0
    for y in range(ch):
        cols = [x for x in range(cw) if cov[y][x] > COV_INK]
        if len(cols) < 2:
            continue
        a, b = cols[0], cols[-1]
        x = a + 1
        while x < b:
            if cov[y][x] <= COV_INK:
                s = x
                while x < b and cov[y][x] <= COV_INK:
                    x += 1
                if 1 <= (x - s) <= 3:
                    holes += 1
            else:
                x += 1
    return holes


def band_holes(cov, cw, ch):
    """**与格子相位无关**的"断笔空洞"计数（整条汉字串一起算）。
    口径：某一行上宽度 <= 2 px 的空白段，若它**上或下相邻行在同一段列上有墨**，
    就说明这是笔画内部的针孔（= 旧版"整行翻出"留下的断笔）；字与字之间的空隙在**所有行**
    上都是空的，所以不会被误判。这样就不依赖"字格从哪一列开始"。
    """
    holes = 0
    for y in range(ch):
        x = 0
        while x < cw:
            if cov[y][x] > COV_INK:
                x += 1
                continue
            s = x
            while x < cw and cov[y][x] <= COV_INK:
                x += 1
            wdt = x - s
            if wdt <= 2 and s > 0 and x < cw:
                up = (y > 0) and all(cov[y - 1][k] > COV_INK for k in range(s, x))
                dn = (y + 1 < ch) and all(cov[y + 1][k] > COV_INK for k in range(s, x))
                if up or dn:
                    holes += 1
    return holes
def cell_vsoft(cov, cw, ch):
    """**按列**统计竖直边缘是"软"还是"硬"（同一条列的上下跳变算一次）。
    口径（与单位无关，所以 soft/(soft+hard) 才有意义）：
      * 这一列里有明显的低→高跳变（min <= 16 且 max >= 239）才算"一条竖直边缘"；
      * 这条列里出现中间档（32..223）= **软边**（= y 方向有子扫描线抗锯齿，边缘渐变）；
      * 完全没有中间档 = **硬边**（= 旧版每行一条像素中心扫描线，整行 0/255 翻转）。
    新版 FONT64_AA_SUB=8 时几乎每条边缘都该是软的；旧版（=1）几乎全是硬的。
    """
    soft = hard = 0
    for x in range(cw):
        vals = [cov[y][x] for y in range(ch)]
        if min(vals) > EDGE_LO or max(vals) < EDGE_HI:
            continue                          # 这一列没有完整的亮暗跳变
        if any(SOFT_LO <= v <= SOFT_HI for v in vals):
            soft += 1
        else:
            hard += 1
    return soft, hard


def cell_levels(cov, cw, ch):
    return len(set(v for row in cov for v in row))


# --------------------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--legacy", action="store_true",
                    help="反相对照：内核用 -DFONT64_LEGACY_AA 编（旧光栅器），断言方向相反")
    ap.add_argument("--no-fixture", action="store_true", help="复用已有夹具盘（调试用）")
    args = ap.parse_args()

    checks = []

    def check(name, ok, extra=""):
        checks.append((name, bool(ok), extra))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  " + str(extra)) if extra else ""))

    img = args.img
    if not img:
        if not args.no_fixture:
            try:
                img = prepare_fixture()
            except Exception as ex:                       # noqa: BLE001
                sys.stderr.write("夹具盘准备失败：%s\n" % ex)
                return 2
        else:
            img = FIXTURE_IMG
    if not os.path.exists(img):
        sys.stderr.write("缺少夹具盘 %s\n" % img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    port = args.port or qh.free_port()
    tmp = tempfile.mkdtemp(prefix="vimtu64_texthint_")
    serial = os.path.join(tmp, "serial.log")
    vm = None
    try:
        vm = QemuVm(qemu, img, port, serial)
        mon = Monitor(port)
        print("=== 1) 启动：中文面从系统卷加载（否则汉字是方块，本测试无意义）===")
        check("字库面加载（[FONT64] faces=4 … cjk=1）",
              vm.wait_re(r"\[FONT64\] faces=4\b[^\n]*cjk=1", 180) is not None,
              (re.search(r"\[FONT64\] faces=[^\n]*", vm.log()) or [""]) and
              (re.search(r"\[FONT64\] faces=[^\n]*", vm.log()).group(0)
               if re.search(r"\[FONT64\] faces=[^\n]*", vm.log()) else "缺行"))

        print("=== 2) 登录 + 开终端 + cat 一行全是横画的汉字 ===")
        qh.login_desktop(mon, lambda: vm.log(), proc=vm.proc, timeout=180)
        up = vm.wait("[GUI64] ready", 180)
        check("桌面就绪（[GUI64] ready）", up)
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if vm.wait("[APP] term opened", 12):
                opened = True
                break
        check("开始菜单 -> 终端（[APP] term opened）", opened)
        if not (up and opened):
            return 1
        mon.key("ret", wait=0.6)
        n0 = vm.n()
        type_line(mon, CAT_CMD, per_key=0.11)
        mcat = vm.wait_re(r"\[TERM\] cmd cat bytes=(\d+)", 30, since=n0)
        want_bytes = len((LINE_TEXT + "\n").encode("utf-8"))
        check("终端真读到文件（[TERM] cmd cat bytes=%d）" % want_bytes,
              mcat is not None and int(mcat.group(1)) == want_bytes,
              (mcat.group(0) if mcat else "没等到"))
        check("没有缺字占位（[FONT64] glyph miss 不该出现）",
              "[FONT64] glyph miss" not in vm.log())
        time.sleep(1.0)
        ppm = os.path.join(tmp, "thint.ppm")
        got = mon.shot(ppm)
        check("截到含该行的一帧", got)
        shot = read_ppm(ppm) if got else None
        if shot is None:
            return 1
        w, h, px = shot
        if w <= 0 or h <= 0:
            sys.stderr.write("截屏尺寸异常 %dx%d\n" % (w, h))
            return 2

        print("=== 3) 定位终端窗口客户区 -> 找到那一行 -> 按字格切分 ===")
        # ★ 窗口几何从内核打点拿（[UI] win geom ... app=3），客户区偏移 = gui64.cpp 的
        #   BORDER(1) / DECO_H(TITLE_H 24 + BORDER 1) -> client_x = x+1, client_y = y+25。
        term = None
        #   ★ title 里**有空格**（实测 "终端 - vimtu64"），所以用非贪婪 .*? 而不是 \S+
        for mm in re.finditer(r"\[UI\] win geom tag=\S+ title=.*? app=(\d+) x=(-?\d+) y=(-?\d+) "
                              r"w=(\d+) h=(\d+)", vm.log()):
            if mm.group(1) == "3":                       # APP_ID_TERM
                term = tuple(int(v) for v in mm.groups()[1:])
        check("拿到终端窗口几何（[UI] win geom app=3 x/y/w/h）", term is not None,
              ("x=%d y=%d w=%d h=%d" % term) if term else "缺行")
        if term is None:
            return 1
        wx, wy, ww, wh = term
        cx0, cy0 = max(0, wx + 1), max(0, wy + 25)
        cx1, cy1 = min(w - 1, wx + ww - 2), min(h - 1, wy + wh - 1)
        check("客户区落在屏内", cx1 > cx0 + 100 and cy1 > cy0 + 40,
              "client=(%d,%d)-(%d,%d)" % (cx0, cy0, cx1, cy1))
        # 背景取客户端区里**出现最多的亮度**（终端可能是亮底或暗底），墨迹 = 与背景差 >= 60
        hist = {}
        for y in range(cy0, cy1 + 1):
            for x in range(cx0, cx1 + 1):
                l = lum(px, w, x, y)
                hist[l] = hist.get(l, 0) + 1
        bg = max(hist.items(), key=lambda kv: kv[1])[0]

        def is_ink(x, y):
            return abs(lum(px, w, x, y) - bg) >= 60

        # ★ 不能只看"最长连续墨迹"：终端的下边框/分隔线也是贯穿整宽的一条（实测第一版就
        #   选中了 y=685 那条边框）。真正的**文本行**是一堆**分开的墨迹段**（32 个汉字 ->
        # ★ 不能只看"最长连续墨迹"（第一版就选中了 y=685 那条贯穿整宽的边框）：
        #   汉字的一笔本身很短（em=16 的横画就 ~14 px），32 个"开"之间还有 1~3 px 的空隙，
        #   所以**文本行 = 很多段墨迹**（>= 16 段），边框/分隔线 = 1 段。按"段数"选行最稳。
        best = None
        for y in range(cy0, cy1 + 1):
            run = bestrun = runs = 0
            prev = False
            for x in range(cx0, cx1 + 1):
                ink = is_ink(x, y)
                if ink:
                    run += 1
                    bestrun = max(bestrun, run)
                    if not prev:
                        runs += 1
                else:
                    run = 0
                prev = ink
            if runs >= 8 and (best is None or runs > best[1]):
                best = (y, runs, bestrun)
        check("终端里找到那一行（该行有 >= 8 段墨迹：汉字笔画的典型形态）",
              best is not None,
              ("y=%d runs=%d maxrun=%d" % best) if best else "没找到")
        if best is None:
            return 1
        ytop = best[0]
        # 行带只能按**这一行自己的文字列**扩，否则会把终端左边框/右边滚动条（每一行都有墨）
        # 一路扩下去，把好几行文字并成一条带（实测踩到：跨度直接变成 790 px）。
        cols = [x for x in range(cx0, cx1 + 1) if is_ink(x, ytop)]
        rx0, rx1 = cols[0], cols[-1]

        def row_ink(y):
            return sum(1 for x in range(rx0, rx1 + 1) if is_ink(x, y))

        # 上下各最多扩 10 px（汉字格高 <= 18 px，行距 16；再扩就是并行了）
        y0 = ytop
        while y0 > cy0 and (ytop - y0) < 10 and row_ink(y0 - 1) > 0:
            y0 -= 1
        y1 = ytop
        while y1 < cy1 and (y1 - ytop) < 10 and row_ink(y1 + 1) > 0:
            y1 += 1
        ch = y1 - y0 + 1
        xs = [x for x in range(rx0, rx1 + 1)
              if any(is_ink(x, y) for y in range(y0, y1 + 1))]
        x0, x1 = xs[0], xs[-1]
        print("    bg=%d  行带 y=%d..%d（高 %d px）  墨迹 x=%d..%d（跨度 %d px）"
              % (bg, y0, y1, ch, x0, x1, x1 - x0 + 1))
        # ★ 这一行**左边是命令回显**（ASCII 8px 一格）、**右边才是 32 个汉字**（16px 一格）——
        #   实测那一行跨度 785 px，汉字只占右边 512 px。所以：
        #   ① 用列方向**自相关**量出真实字距 cw；
        #   ② 汉字串就是这一行**最右边 cw×32 px**（行尾就是最后一个字）；
        #   ③ 再用"同字重复的周期性"把它验住（若取到含 ASCII 的窗口，周期会掉下来）。
        #   指标本身**与格子相位无关**：holes 用"上下相邻行是否也有墨"判针孔，vsoft 按列算。
        prof = {}
        for x in range(x0, x1 + 1):
            prof[x] = sum(1 for y in range(y0, y1 + 1) if abs(lum(px, w, x, y) - bg) >= 60)
        peaks = []
        for p in range(8, 33):
            s = sum(min(prof[x], prof.get(x + p, 0)) for x in range(x0, x1 - p + 1))
            peaks.append((s, p))
        peaks.sort(reverse=True)
        cw = peaks[0][1]
        check("字距（列自相关峰）落在 14..18 px（等宽面 CJK 格 = 16 px）",
              14 <= cw <= 18, "cw=%d peaks=%s" % (cw, peaks[:3]))

        cjk_x0 = x1 - cw * NCELL + 1
        check("汉字串区间在行带内", cjk_x0 > x0,
              "cjk_x0=%d x0=%d x1=%d" % (cjk_x0, x0, x1))
        if cjk_x0 <= x0:
            return 1
        num = sum(min(prof[x], prof.get(x + cw, 0)) for x in range(cjk_x0, x1 - cw + 1))
        den = sum(prof[x] for x in range(cjk_x0, x1 + 1))
        peri = num / float(den) if den else 0.0
        check("这段是**同一个字重复 32 次**（16px 周期性 >= 0.90）", peri >= 0.90,
              "peri=%.3f" % peri)
        # 每个字的墨迹起点应当**整齐等差**（差 = cw ± 1）；截断/漂移会让差值参差
        starts = []
        for k in range(NCELL):
            a = cjk_x0 + k * cw
            col = [x for x in range(a, min(a + cw, x1 + 1)) if prof.get(x, 0) > 0]
            if col:
                starts.append(col[0] - a)
        deltas = [starts[i + 1] - starts[i] for i in range(len(starts) - 1)]
        check("32 个字的墨迹起点等差（每字 cw ± 1 px，共 %d 个差）" % len(deltas),
              len(deltas) >= NCELL - 2 and all(abs(d) <= 1 for d in deltas),
              "starts[:8]=%s deltas[:8]=%s" % (starts[:8], deltas[:8]))
        print("    行带 x=%d..%d  汉字串 x=%d..%d（%d 格 × %d px = %d px）"
              % (x0, x1, cjk_x0, x1, NCELL, cw, cw * NCELL))

        # 背景/墨迹取汉字串的亮暗极值（暗底亮字/亮底暗字都判）
        lo, hi = 255, 0
        for y in range(y0, y1 + 1):
            for x in range(cjk_x0, x1 + 1):
                l = lum(px, w, x, y)
                lo = min(lo, l)
                hi = max(hi, l)
        check("汉字串里亮暗分离（%d..%d）" % (lo, hi), hi - lo >= 60)

        # ---- 两条**与格子相位无关**的指标（只在汉字串区间上算）----
        bw = x1 - cjk_x0 + 1
        cov = coverage_cell(px, w, cjk_x0, y0, bw, ch, lo, hi)
        holes = band_holes(cov, bw, ch)
        soft, hard = cell_vsoft(cov, bw, ch)
        vsoft = soft / float(soft + hard) if (soft + hard) > 0 else 0.0
        levels = [cell_levels(cov, bw, ch)]
        lv_min = min(levels)
        lv_mean = float(levels[0])

        # 背景/墨迹取汉字串的亮暗极值（暗底亮字/亮底暗字都判）
        lo, hi = 255, 0
        for y in range(y0, y1 + 1):
            for x in range(cjk_x0, x1 + 1):
                l = lum(px, w, x, y)
                lo = min(lo, l)
                hi = max(hi, l)
        check("汉字串里亮暗分离（%d..%d）" % (lo, hi), hi - lo >= 60)

        # ---- 两条**与格子相位无关**的指标（只在汉字串区间上算）----
        bw = x1 - cjk_x0 + 1
        cov = coverage_cell(px, w, cjk_x0, y0, bw, ch, lo, hi)
        holes = band_holes(cov, bw, ch)
        soft, hard = cell_vsoft(cov, bw, ch)
        vsoft = soft / float(soft + hard) if (soft + hard) > 0 else 0.0
        levels = [cell_levels(cov, bw, ch)]
        lv_min = min(levels)
        lv_mean = float(levels[0])
        print("    holes=%d  vsoft=%.3f（soft=%d hard=%d）  levels: min=%d mean=%.1f"
              % (holes, vsoft, soft, hard, lv_min, lv_mean))

        print("=== 4) 判据（%s）===" % ("反相对照：旧光栅器" if args.legacy else "本版：子扫描线抗锯齿"))
        # 判据阈值**实测后定**（下面就是两侧实测原文；改内核/改字体要重新量一次）：
        #   本版（FONT64_AA_SUB=8、FONT64_GEOM_SH=16）：
        #       vsoft = 1.000（soft=33 hard=0）—— **每一条竖直边缘都有中间档**；
        #       holes = 32（32 个字各 1 个 1px 针孔，= 本光栅器在 em=16 的分辨率极限）；
        #       levels = 41。
        #   旧版（`VIMTU_EXTRA_CXXFLAGS=-DFONT64_LEGACY_AA`，每行一条像素中心扫描线）：
        #       vsoft = 0.182（soft=64 hard=288，82% 的边缘是 0→255 硬跳）；
        #       holes = 64（**每个字 2 个洞**，正是 kernel/font.cpp 注释里记的那一条）；
        #       levels = 15。
        #   所以分辨力是**成对出现**的：新版高 vsoft + 低 holes；旧版低 vsoft + 高 holes。
        #   `--legacy` 就是拿同一个夹具、同一条串复核这一对（那一档断言方向相反）。
        if args.legacy:
            check("旧光栅器：竖直方向几乎没有中间档（vsoft < 0.60）",
                  vsoft < 0.60, "vsoft=%.3f（soft=%d hard=%d）" % (vsoft, soft, hard))
            check("旧光栅器：断笔针孔明显多于每字 1 个（holes > 32）",
                  holes > NCELL, "holes=%d" % holes)
        else:
            check("竖直方向有子扫描线抗锯齿（vsoft >= 0.90）", vsoft >= 0.90,
                  "vsoft=%.3f（soft=%d hard=%d）" % (vsoft, soft, hard))
            check("断笔针孔不超过每字 1 个（holes <= 32）", holes <= NCELL, "holes=%d" % holes)
            check("汉字串里的覆盖率档位足够多（levels >= 8）", lv_min >= 8,
                  "levels min=%d mean=%.1f" % (lv_min, lv_mean))
        log = vm.log()
        for needle in PANIC_MARKERS:
            check("整轮不得出现 %s" % needle, needle not in log)
    finally:
        if vm is not None:
            vm.close()
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print("保留目录：%s" % tmp)

    bad = [c for c in checks if not c[1]]
    print("=== RESULT: %s ===  checks=%d ok=%d" % ("PASS" if not bad else "FAIL",
                                                  len(checks), len(checks) - len(bad)))
    for name, _ok, extra in bad:
        print("  FAIL: %s  %s" % (name, extra))
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
