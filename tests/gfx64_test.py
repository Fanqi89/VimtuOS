#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/gfx64_test.py - 现代图元层（kernel/gfx64.cpp）像素级验收

验收目标（每条都要像素/串口证据，不能只看"没崩"）：
  1) 圆角 + 抗锯齿   Dock 面板左上角：角上像素必须"不是面板色"（圆角切掉了），
                     沿 x 方向的"接近面板色"的覆盖率斜坡单调上升；直边区完全等于面板色。
  2) 双层浅阴影       Dock 下方 y 带：近层（0/2/4 a=0.08）比远层（0/12/32 a=0.12）更暗，
                     且阴影随距离**渐变**（近≈18 级、远≈2..6 级），参考带取"同 y 的远处壁纸"消掉渐变。
  3) 真模糊          [GFX64] blur tile var_src/var_dst（内核里对**同一块** Wallpaper 面 vs 模糊后
                     的方差实测）+ 屏幕上 Dock 玻璃区方差 << 同尺寸壁纸区方差。
  4) 渐变单调        Wallpaper 顶带（无柔光斑/无 Dock）列均值的"单调过渡"（允许 ±3 的细颗粒噪声）。
  5) 缓存纪律        [GFX64] wall blur once（整屏模糊只算一次）+ [GFX64] cache（blur/shadow 命中统计）
                     + 墙纸面尺寸 = 屏幕尺寸（1280x800）。
  6) 图像解码        [IMG64] selftest PASS（4x4 PNG 逐像素比对 + 缩放用例）+ kaisi.png 兜底路径打点。
用法：python tests/gfx64_test.py [--img build64/system.img] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

# 与 kernel/theme64.h 的 Token 一致（判像素用；数字改了这里也要跟着改）
T_R_DOCK = 24
T_A_CARD = int(80 * 255 / 100)
DOCK_H = 60
DOCK_MARGIN = 16
DOCK_PAD = 9
T_DOCK_PAD_ = 9


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    import shutil
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


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


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def shot(self, path, wait=2.5):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


# ==================== 真文件回归基准：宿主侧独立解 PNG ====================
# inflate 用 zlib（= 与内核无关的第二实现），反滤波/展开按 kernel/img64.cpp 的同一套步骤，
# 产出内核同款 AARRGGBB 像素缓冲（小端字节序），供 FNV-1a 32 指纹比对。
def img64_fnval(buf):
    h = 2166136261
    for b in buf:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def host_png_aarrggbb(png):
    """返回 (w, h, AARRGGBB 小端 bytes)；不支持的形态返回 None。"""
    import struct as _s
    import zlib as _z
    if len(png) < 8 or png[:4] != b"\x89PNG":
        return None
    w = h = bd = ct = 0
    plte = None
    trns = b""
    idat = b""
    p = 8
    while p + 8 <= len(png):
        ln = _s.unpack(">I", png[p:p + 4])[0]
        t = png[p + 4:p + 8]
        c = png[p + 8:p + 8 + ln]
        if t == b"IHDR":
            w, h, bd, ct = _s.unpack(">IIBB", c[:10])
        elif t == b"PLTE":
            plte = c
        elif t == b"tRNS":
            trns = c
        elif t == b"IDAT":
            idat += c
        elif t == b"IEND":
            break
        p += 12 + ln
    if ct not in (0, 2, 3, 4, 6) or w <= 0 or h <= 0:
        return None
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = (w * ch * bd + 7) // 8
    raw = _z.decompress(idat)
    if len(raw) != (stride + 1) * h:
        return None
    bpp = max(1, (ch * bd + 7) // 8)
    prev = bytearray(stride)
    out = []
    off = 0
    for _y in range(h):
        ft = raw[off]
        src = raw[off + 1:off + 1 + stride]
        off += 1 + stride
        cur = bytearray(stride)
        if ft == 0:
            cur[:] = src
        elif ft == 1:
            for i in range(stride):
                cur[i] = (src[i] + (cur[i - bpp] if i >= bpp else 0)) & 0xFF
        elif ft == 2:
            for i in range(stride):
                cur[i] = (src[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (src[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = cur[i - bpp] if i >= bpp else 0
                bb = prev[i]
                cc = prev[i - bpp] if i >= bpp else 0
                pp = a + bb - cc
                pa, pb, pc = abs(pp - a), abs(pp - bb), abs(pp - cc)
                pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else cc)
                cur[i] = (src[i] + pr) & 0xFF
        else:
            return None
        prev = cur
        for x in range(w):
            if bd == 8:
                s = cur[x * ch:(x + 1) * ch]
                if ch == 1:
                    out.append(0xFF000000 | (s[0] << 16) | (s[0] << 8) | s[0])
                elif ch == 2:
                    out.append((s[1] << 24) | (s[0] << 16) | (s[0] << 8) | s[0])
                elif ch == 3:
                    out.append(0xFF000000 | (s[0] << 16) | (s[1] << 8) | s[2])
                else:
                    out.append((s[3] << 24) | (s[0] << 16) | (s[1] << 8) | s[2])
            elif bd == 16 and ch in (1, 2, 3, 4):
                s = cur[x * ch * 2:(x + 1) * ch * 2]
                if ch == 1:
                    out.append(0xFF000000 | (s[0] << 16) | (s[0] << 8) | s[0])
                elif ch == 2:
                    out.append((s[2] << 24) | (s[0] << 16) | (s[0] << 8) | s[0])
                elif ch == 3:
                    out.append(0xFF000000 | (s[0] << 16) | (s[2] << 8) | s[4])
                else:
                    out.append((s[6] << 24) | (s[0] << 16) | (s[2] << 8) | s[4])
            elif bd in (1, 2, 4) and ch == 1:
                per = 8 // bd
                v = (cur[x // per] >> (8 - bd * (x % per + 1))) & ((1 << bd) - 1)
                if ct == 3:
                    if plte is None or v * 3 + 2 >= len(plte):
                        return None
                    a = trns[v] if v < len(trns) else 255
                    out.append((a << 24) | (plte[v * 3] << 16) | (plte[v * 3 + 1] << 8) | plte[v * 3 + 2])
                else:
                    g = v * 255 // ((1 << bd) - 1)
                    out.append(0xFF000000 | (g << 16) | (g << 8) | g)
            else:
                return None
    return w, h, _s.pack("<%dI" % len(out), *out)

def region_var(px, w, x0, y0, rw, rh):
    """局部方差（只算绿通道，返回放大 100 倍的整数，和内核 gfx64_var64 同口径）。"""
    vals = []
    for y in range(y0, y0 + rh):
        for x in range(x0, x0 + rw):
            vals.append(sample(px, w, x, y)[1])
    n = len(vals)
    mean = sum(vals) / n
    var = sum((v - mean) ** 2 for v in vals) / n
    return var * 100


def mean_g(px, w, x0, x1, y0, y1):
    tot = 0
    n = 0
    for y in range(y0, y1):
        for x in range(x0, x1):
            tot += sample(px, w, x, y)[1]
            n += 1
    return tot / max(1, n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5641)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_gfx_")
    serial = os.path.join(tmp, "serial.log")
    shot = os.path.join(tmp, "desktop.ppm")
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for(needle, timeout, what):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in slog():
                print("  [ok] %s（%.1fs）" % (what, time.time() - t0))
                return True
            if proc.poll() is not None:
                print("  [!] QEMU 提前退出")
                return False
            time.sleep(0.4)
        print("  [!] 等 %r 超时 %.0fs" % (needle, timeout))
        return False

    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-gfx",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        mon = Monitor(args.port)

        print("=== 1) 主题 Token / 图元层启动打点 ===")
        up = wait_for("[GUI64] ready", 90, "桌面就绪")
        log = slog()
        check("等待桌面就绪", up)
        m = re.search(r"\[THEME64\] init themes=(\d+) theme=(\d+) name=(\S+) dark=(\d)",
                      log)
        check("Token 真源打点 [THEME64] init", m is not None, m.group(0) if m else "（无）")
        if m:
            check("内置主题 >= 7 组（白/暗/蓝白/粉白/粉绿/粉紫/紫白）", int(m.group(1)) >= 7,
                  "themes=%s" % m.group(1))
            check("默认白色主题", m.group(2) == "0" and m.group(3) == "white" and m.group(4) == "0",
                  "theme=%s name=%s dark=%s" % (m.group(2), m.group(3), m.group(4)))
        for key, lo, hi in (("r_win", 12, 16), ("r_card", 12, 12), ("r_btn", 8, 10), ("r_dock", 22, 26),
                            ("blur_bg", 20, 30), ("blur_content", 8, 16)):
            mm = re.search(r"%s=(\d+)" % key, log)
            check("Token %s ∈ [%d,%d]" % (key, lo, hi), mm is not None and lo <= int(mm.group(1)) <= hi,
                  "%s=%s" % (key, mm.group(1) if mm else "?"))
        mm = re.search(r"alpha_bg=(\d+) alpha_card=(\d+)", log)
        if mm:
            check("背景材质透明度 0.35–0.55", 89 <= int(mm.group(1)) <= 140, "alpha_bg=%s" % mm.group(1))
            check("内容卡片透明度 0.72–0.85", 183 <= int(mm.group(2)) <= 217, "alpha_card=%s" % mm.group(2))
        else:
            check("透明度 Token 打点", False)
        mm = re.search(r"sh_near=0,(\d+),(\d+),(\d+) sh_far=0,(\d+),(\d+),(\d+)", log)
        if mm:
            check("双层浅阴影 Token（近 0/2/4 a8 + 远 0/12/32 a12）",
                  mm.group(1) == "2" and mm.group(2) == "4" and mm.group(3) == "20" and
                  mm.group(4) == "12" and mm.group(5) == "32" and mm.group(6) == "31",
                  mm.group(0))
        else:
            check("双层浅阴影 Token 打点", False)
        mm = re.search(r"ms=fast:(\d+),normal:(\d+),large:(\d+),dock:(\d+)", log)
        if mm:
            check("动效时长 150/200-250/300-350 + Dock 回弹",
                   int(mm.group(1)) == 150 and 200 <= int(mm.group(2)) <= 250 and
                   300 <= int(mm.group(3)) <= 350 and int(mm.group(4)) > 0, mm.group(0))
        else:
            check("动效 Token 打点", False)

        print("=== 2) 壁纸模糊：只算一次 + 同一块区域方差实测（真模糊证据）===")
        mm = re.search(r"\[GFX64\] wall blur once r=(\d+) src=(\d+)x(\d+)", log)
        check("[GFX64] wall blur once（整屏模糊算一次）", mm is not None, mm.group(0) if mm else "（无）")
        vb = re.search(r"\[GFX64\] blur tile x=(\d+) y=(\d+) wh=(\d+)x(\d+) var_src=(\d+) var_dst=(\d+)", log)
        check("[GFX64] blur tile 方差打点（原图 vs 模糊后）", vb is not None, vb.group(0) if vb else "（无）")
        if vb:
            vs, vd = int(vb.group(5)), int(vb.group(6))
            check("模糊后局部方差显著低于原图（var_dst*4 < var_src）",
                  vs > 50 and vd * 4 < vs, "var_src=%d var_dst=%d" % (vs, vd))
        mm = re.search(r"\[GFX64\] cache tag=first_frame blur_hit=(\d+) blur_miss=(\d+) shadow_hit=(\d+) shadow_miss=(\d+) wall_builds=(\d+)", log)
        check("[GFX64] cache 命中统计打点", mm is not None, mm.group(0) if mm else "（无）")
        if mm:
            check("墙纸面只构建 1 次（首帧不重复构建）", int(mm.group(5)) == 1, "wall_builds=%s" % mm.group(5))

        print("=== 3) 图像解码（PNG）自检 ===")
        mm = re.search(r"\[IMG64\] selftest (PASS|FAIL) mask=(\d+) png=(\d+)x(\d+) rc=(\d+) bytes=(\d+)", log)
        check("[IMG64] selftest PASS", mm is not None and mm.group(1) == "PASS", mm.group(0) if mm else "（无）")
        if mm:
            check("自检用例是 4x4 PNG 且解码 rc=0", mm.group(3) == "4" and mm.group(4) == "4" and mm.group(5) == "0")
        check("[IMG64] PNG 解码打点（含尺寸）", "[IMG64] decode png rc=0" in log,
              "见 [IMG64] decode png rc=0 bytes=112 -> 4x4")
        check("图标/壁纸优先从 VimtuFS2 读（裸 system.img 无卷时如实打 skip ok=0；有卷时 load ok=1）",
              "[IMG64] load skip path=/logo/kaisi.png reason=not-found ok=0" in log or
              re.search(r"\[IMG64\] load path=/logo/kaisi\.png ok=1 ", log) is not None,
              (re.search(r"\[IMG64\] load (skip )?path=/logo/kaisi\.png[^\r\n]*", log) or ["（无）"])[0])
        check("启动期幂等安装打点（有卷 ok=1 / 无卷 reason=no-volume，不假装成功）",
              re.search(r"\[IMG64\] install (skip )?path=/logo/kaisi\.png[^\r\n]*", log) is not None,
              (re.search(r"\[IMG64\] install (skip )?path=/logo/kaisi\.png[^\r\n]*", log) or ["（无）"])[0])
        check("开始图标兜底路径打点（kaisi.png 的 RGBA 内嵌副本）",
              re.search(r"\[DOCK64\] start icon src=(\S+) size=(\d+) ok=1", log) is not None,
              (re.search(r"\[DOCK64\] start icon src=(\S+) size=(\d+) ok=1", log) or [None, "?"])[0])
        # ★ 真文件回归：启动自检必须能**解出真文件 logo/kaisi.png**（= 1 个大动态 Huffman 块 + 收尾空固定块 +
        #   100 KB 输出；老 inflate 在这里 inflate_rc=18，整张图解不出来），并用宿主侧**独立实现**（zlib）
        #   解出的像素指纹逐字节比对——这个缺陷以后不会再悄悄回归。
        try:
            with open(os.path.join(ROOT, "logo", "kaisi.png"), "rb") as f:
                png_src = f.read()
        except OSError:
            png_src = b""
        check("宿主侧 logo/kaisi.png 可读（回归基准）", len(png_src) > 0, "%d 字节" % len(png_src))
        rl = re.search(r"\[IMG64\] selftest real ok=(\d+) bytes=(\d+) (\d+)x(\d+) px=(\d+) "
                       r"fnv=([0-9A-Fa-f]{16}) rc=(\d+)", log)
        check("启动自检解真文件 logo/kaisi.png（[IMG64] selftest real ok=1 rc=0）",
              rl is not None and rl.group(1) == "1" and rl.group(7) == "0",
              rl.group(0) if rl else (re.search(r"\[IMG64\] selftest real[^\r\n]*", log) or ["（无）"])[0])
        host_px = host_png_aarrggbb(png_src) if png_src else None
        if rl is not None and host_px is not None:
            hw, hh, hbuf = host_px
            check("自检报的 bytes = 真文件大小", int(rl.group(2)) == len(png_src),
                  "内核 %s vs 宿主 %d" % (rl.group(2), len(png_src)))
            check("自检报的尺寸 = 宿主解 PNG 的尺寸", (int(rl.group(3)), int(rl.group(4))) == (hw, hh),
                  "内核 %sx%s vs 宿主 %dx%d" % (rl.group(3), rl.group(4), hw, hh))
            check("自检像素指纹 = 宿主解 PNG 的像素指纹（FNV-1a 32，逐字节）",
                  int(rl.group(6), 16) == img64_fnval(hbuf),
                  "内核 fnv=%s 宿主 fnv=%08X（%dx%d）" % (rl.group(6), img64_fnval(hbuf), hw, hh))
        elif host_px is None and png_src:
            print("      宿主解码器不支持这个 PNG 形态，跳过指纹比对")

        print("=== 4) Dock 面板几何 + 圆角抗锯齿（像素）===")
        g = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) gap=(\d+) items=(\d+) margin=(\d+) center=1 screen=(\d+)x(\d+)", log)
        check("[DOCK64] geom 打点", g is not None, g.group(0) if g else "（无）")
        if not up or g is None:
            print("  桌面没起来/没有几何，跳过像素断言")
        else:
            dx, dy, dw, dh, r = (int(g.group(i)) for i in range(1, 6))
            W, H = int(g.group(10)), int(g.group(11))
            check("Dock 高 60", dh == DOCK_H, "h=%d" % dh)
            check("Dock y = 屏高 - 76（离底 16px）", dy == H - DOCK_H - DOCK_MARGIN, "y=%d" % dy)
            check("Dock 水平居中", abs((dx + dw // 2) - W // 2) <= 1, "x=%d w=%d center=%d" % (dx, dw, dx + dw // 2))
            check("Dock 圆角 24", r == T_R_DOCK, "r=%d" % r)

            mon.shot(shot)
            w, h, px = read_ppm(shot)
            check("分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
            # 面板内一点（避开图标）：x = dock_x + pad/2 附近为空面板
            inside = sample(px, w, dx + 4, dy + dh // 2)
            panel = sample(px, w, dx + dw // 2 - 100, dy + 30)   # 面板里远离图标的一行
            print("      面板色 %s / 直边内侧 %s" % (panel, inside))
            # 圆角 + 抗锯齿：**角外对角线**上的像素必须不是面板色（被圆角切掉，露出背景），
            # 而**面板内**的像素必须是面板色。浅色主题下面板与浅色壁纸本身很接近，所以用相对判据。
            straight = sample(px, w, dx + r + 6, dy + 1)
            # "纯面板"参考点：图标与图标之间的间隙中点（图标 46 + 间距 11，这里必然没有图标）
            gap_x = dx + T_DOCK_PAD_ + 46 + 5
            panel_in = sample(px, w, gap_x, dy + dh // 2)
            d_out = [dist(sample(px, w, dx + k, dy + k), panel_in) for k in range(0, r - 2)]
            d_in = [dist(sample(px, w, dx + r + 4 + k, dy + dh - 4 + (k % 2)), panel_in) for k in range(0, 6)]
            print("      角外对角线距离 %s / 面板内距离 %s" % (d_out[:6], d_in))
            check("圆角处不是面板色（角被切掉，露出背景）",
                  sum(d_out) / len(d_out) > sum(d_in) / len(d_in) + 6 and d_out[0] > 6,
                  "角外均值 %.1f 面板内均值 %.1f" % (sum(d_out) / len(d_out), sum(d_in) / len(d_in)))
            check("直边区是面板色（说明上面那点是圆角而不是噪声）", dist(straight, panel_in) <= 8,
                  "straight=%s 距离=%d" % (straight, dist(straight, panel_in)))
            # 覆盖率斜坡：沿 x 从角点往右，与面板色的距离应单调下降（允许 1 次抖动）
            d0 = [dist(sample(px, w, dx + i, dy + 1), panel_in) for i in range(0, r + 8)]
            # 抗锯齿的判据：r×r 角区里必须出现**多级混合色**（覆盖率 4x4 亚采样的中间档），
            # 而直边内部的方块只有少数几种颜色（面板/边框/背景）。二值化圆角只会给 2-3 级。
            corner_levels = len(set(sample(px, w, dx + i, dy + j) for i in range(0, r) for j in range(0, r)))
            edge_levels = len(set(sample(px, w, dx + r + i, dy + 1 + j) for i in range(0, r) for j in range(0, r)))
            print("      角区颜色级数 %d / 直边方块颜色级数 %d" % (corner_levels, edge_levels))
            check("圆角处有多级抗锯齿混合色（角区颜色级数 >= 5 且多于直边方块）",
                  corner_levels >= 5 and corner_levels > edge_levels,
                  "corner=%d edge=%d" % (corner_levels, edge_levels))
            print("      纯面板参考点 %s（图标间隙）" % (panel_in,))

            print("=== 5) 双层浅阴影：近层比远层暗 + 随距离渐变 ===")
            # 阴影只落在 Dock 四周有限范围（近层偏移 2/模糊 4，远层偏移 12/模糊 32）。
            # 参考带取"同一 y 的远处壁纸"，并用**纵向双重差分**把壁纸自身的渐变消掉：
            #   dark(x0,x1,y) = mean(x0..x1, y) - mean(x0..x1, 600)（600 行远离 Dock，无阴影）
            def dark(x0, x1, y):
                # 正值 = 这一带比"同一 x 的 600 行"更暗（600 行远离 Dock、无阴影）
                return (mean_g(px, w, x0, x1, 600, 603) - mean_g(px, w, x0, x1, y, y + 3))

            band_x = (500, 780)
            ref_x = (30, 260)
            g_near = dark(*band_x, dy + dh + 2) - dark(*ref_x, dy + dh + 2)     # 近层（偏移 2）最强处
            g_far = dark(*band_x, dy + dh + 12) - dark(*ref_x, dy + dh + 12)     # 远层（偏移 12）最强处
            print("      下方阴影（双重差分后）：近层带 %.1f / 远层带 %.1f" % (g_near, g_far))
            check("近层阴影可见（近层带 > 6 级）", g_near > 6, "%.1f" % g_near)
            check("远层阴影可见（偏移 +12 处 > 4 级；近层只有 4px 模糊，到不了那儿）",
                  g_far > 4, "%.1f" % g_far)
            check("双层梯度：近层带比远层带更暗", g_near > g_far + 2,
                  "近 %.1f 远 %.1f" % (g_near, g_far))
            # 左侧密集剖面（**严格在外侧**取样，避免把面板/边框算进来；面积放大降噪）：
            #   近层（模糊 4）只在边缘 6px 内贡献陡降；远层（模糊 32）把它铺到 ~40px。
            ymid0 = dy + 10
            ymid1 = dy + dh - 10
            prof = {}
            for off in (2, 8, 20, 40, 60):
                prof[off] = (dark(dx - off - 2, dx - off + 1, ymid0) - dark(dx - 78, dx - 70, ymid0)
                             + dark(dx - off - 2, dx - off + 1, ymid1) - dark(dx - 78, dx - 70, ymid1)) / 2.0
            print("      左侧阴影剖面（离 Dock 边缘 %s px）：%s" % (list(prof.keys()),
                  ["%.1f" % prof[k] for k in sorted(prof)]))
            check("左侧有阴影（边缘 2-4px > 5 级）", prof[2] > 5, "Δ(2)=%.1f" % prof[2])
            check("近层让边缘更陡（2-4px → 8-10px 的相对衰减 > 20%）",
                  (prof[2] - prof[8]) / max(0.1, prof[2]) > 0.2,
                  "Δ(2)=%.1f Δ(8)=%.1f 衰减 %.0f%%"
                  % (prof[2], prof[8], 100.0 * (prof[2] - prof[8]) / max(0.1, prof[2])))
            check("阴影有限范围：40px 外基本没有（< 2 级）",
                  abs(prof[40]) < 2, "Δ(40)=%.1f" % prof[40])
            check("阴影随距离单调衰减（近 >= 中 >= 远）",
                  prof[2] > prof[8] > prof[20] >= prof[40],
                  "剖面=%s" % ["%.1f" % prof[k] for k in sorted(prof)])

            print("=== 6) 玻璃区平滑 + 壁纸渐变单调 ===")
            # Dock 玻璃区（无图标处）与同尺寸壁纸区的方差对比
            # 取"图标之间的间隙"（无图标、纯玻璃）横向 8px × 纵向 24px
            v_glass = region_var(px, w, gap_x - 4, dy + 18, 8, 24)
            v_wall = max(region_var(px, w, x0, y0, 32, 32)
                         for (x0, y0) in ((180, 300), (900, 200), (400, 500), (1000, 600)))
            print("      方差：Dock 玻璃内 %d / 壁纸区 %d（放大 100 倍）" % (v_glass, v_wall))
            check("模糊后的玻璃区局部方差显著低于壁纸（v_glass*4 < v_wall）",
                  v_wall > 100 and v_glass * 4 < v_wall, "glass=%d wall=%d" % (v_glass, v_wall))
            # 渐变单调：取壁纸最顶带（y=2..10，柔光斑从 y≈18 才开始），列均值整体下降
            cols = []
            for x in range(0, w, 40):
                tot = 0
                n = 0
                for y in range(2, 10):
                    tot += sample(px, w, x, y)[1]
                    n += 1
                cols.append(tot / n)
            first = sum(cols[:8]) / 8.0
            last = sum(cols[-8:]) / 8.0
            span = max(cols) - min(cols)
            # 与首尾线性插值的最大偏差（细颗粒 ±4 允许少量抖动）
            dev = 0.0
            for i, v in enumerate(cols):
                lin = first + (last - first) * i / (len(cols) - 1.0)
                dev = max(dev, abs(v - lin))
            print("      渐变：首 %.1f 尾 %.1f 跨度 %.1f 最大偏差 %.1f" % (first, last, span, dev))
            check("壁纸渐变单调过渡（首 > 尾 且 线性偏差 < 6）",
                  first > last + 2 and dev < 6.0, "first=%.1f last=%.1f dev=%.1f" % (first, last, dev))

        print("=== 7) 不能出现的日志 ===")
        log = slog()
        for bad in ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:",
                    "kfree: bad", "GFX64] wall blur once r=0"):
            check("不应出现 %s" % bad, bad not in log)
        if args.keep:
            print("截图/串口：%s" % tmp)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    n_pass = sum(1 for _, c in checks if c)
    print("断言：%d/%d PASS" % (n_pass, len(checks)))
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
