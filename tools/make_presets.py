# -*- coding: utf-8 -*-
"""tools/make_presets.py —— v0.5.8：生成"系统预设头像"与"内置示例壁纸"。

产出（build64/presets/）：
  avatar-01.png .. avatar-06.png     128x128 预设头像（**代码画**：渐变圆底 + 白色几何标记）
  wall-aurora.png / wall-grid.png / wall-sunrise.png   1024x768 PNG 内置示例壁纸

为什么要它们：
  * 用户点名的需求是"根目录里放**预设头像**和**预设壁纸**两个目录，两者分开"。
  * 头像由代码画（不需要外部素材）；壁纸**由用户提供** —— 投放目录是 `res/wallpapers/`，
    构建期把里面的 *.png/*.jpg/*.jpeg/*.bmp 原样打进卷内 `/Wallpapers/`（本工具只负责
    "目录不为空"的内置示例，用户丢图进来就会一起进卷）。

为什么壁纸尺寸取 1024x768：与本仓库 QEMU 默认分辨率一致（`gfx64` 的壁纸按填充/适应模式缩放），
渐变 + 几何图案在这一档上体积很小（每张几十 KB），不挤占卷空间。

用法：py -3 tools/make_presets.py [--out build64/presets]
"""
import argparse
import math
import os
import sys

try:
    from PIL import Image, ImageDraw
except ImportError:
    sys.stderr.write("需要 Pillow：pip install pillow\n")
    raise SystemExit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "build64", "presets")
AV = 128          # 头像边长
SS = 4            # 超采样倍数（画完缩小 = 抗锯齿）
WW, WH = 1024, 768  # 壁纸尺寸

# (输出名, 底色渐变两色, 前景几何种类)
AVATARS = [
    ("avatar-01.png", ((64, 132, 240), (24, 84, 190)), "V"),
    ("avatar-02.png", ((250, 150, 60), (214, 92, 32)), "A"),
    ("avatar-03.png", ((58, 196, 128), (24, 132, 96)), "ring"),
    ("avatar-04.png", ((150, 110, 240), (96, 60, 190)), "star"),
    ("avatar-05.png", ((48, 176, 210), (20, 110, 160)), "bars"),
    ("avatar-06.png", ((240, 110, 160), (190, 50, 110)), "wave"),
]


def lerp(a, b, t):
    return int(round(a + (b - a) * t))


def diag_gradient(size, c0, c1):
    """对角线性渐变（左上 c0 -> 右下 c1）。"""
    img = Image.new("RGB", (size, size))
    px = img.load()
    for y in range(size):
        for x in range(size):
            t = (x + y) / float(2 * (size - 1)) if size > 1 else 0.0
            px[x, y] = (lerp(c0[0], c1[0], t), lerp(c0[1], c1[1], t), lerp(c0[2], c1[2], t))
    return img


def draw_mark(d, kind, size, wcol=(255, 255, 255)):
    """在 size x size 的画布上画白色几何标记（坐标按比例给，便于超采样）。"""
    s = size
    lw = max(2, int(s * 0.075))
    cx, cy = s // 2, s // 2
    if kind == "V":
        d.line([(int(s * 0.32), int(s * 0.34)), (cx, int(s * 0.70)), (int(s * 0.68), int(s * 0.34))],
               fill=wcol, width=lw, joint="curve")
    elif kind == "A":
        d.line([(int(s * 0.30), int(s * 0.70)), (cx, int(s * 0.30)), (int(s * 0.70), int(s * 0.70))],
               fill=wcol, width=lw, joint="curve")
        d.line([(int(s * 0.40), int(s * 0.56)), (int(s * 0.60), int(s * 0.56))], fill=wcol, width=lw)
    elif kind == "ring":
        r = int(s * 0.22)
        d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=wcol, width=lw)
    elif kind == "star":
        # 四点星
        r1, r2 = int(s * 0.26), int(s * 0.09)
        pts = []
        for i in range(8):
            ang = math.pi / 4 * i - math.pi / 2
            r = r1 if i % 2 == 0 else r2
            pts.append((cx + int(r * math.cos(ang)), cy + int(r * math.sin(ang))))
        d.polygon(pts, fill=wcol)
    elif kind == "bars":
        for k in range(3):
            y = cy - int(s * 0.14) + k * int(s * 0.14)
            x1 = int(s * (0.32 - 0.04 * k))
            x2 = int(s * (0.68 + 0.04 * k))
            x2 = min(x2, s - lw)
            d.line([(x1, y), (x2, y)], fill=wcol, width=lw)
    elif kind == "wave":
        pts = []
        for i in range(0, s + 1, max(1, s // 48)):
            t = i / float(s)
            pts.append((i, cy + int(math.sin(t * math.pi * 2) * s * 0.12)))
        d.line(pts, fill=wcol, width=lw, joint="curve")


def make_avatar(name, c0, c1, kind, out_dir):
    big = AV * SS
    bg = diag_gradient(big, c0, c1).convert("RGBA")
    d = ImageDraw.Draw(bg)
    draw_mark(d, kind, big)
    img = bg.resize((AV, AV), Image.LANCZOS)
    # 圆形蒙版（头像都是圆形）
    mask = Image.new("L", (AV * SS, AV * SS), 0)
    ImageDraw.Draw(mask).ellipse([0, 0, AV * SS - 1, AV * SS - 1], fill=255)
    mask = mask.resize((AV, AV), Image.LANCZOS)
    out = Image.new("RGBA", (AV, AV), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    p = os.path.join(out_dir, name)
    out.save(p, "PNG", optimize=True)
    return p, os.path.getsize(p)


def wall_aurora():
    """对角三段渐变（深蓝 -> 紫 -> 暖粉）+ 一层柔和暗角。"""
    img = Image.new("RGB", (WW, WH))
    px = img.load()
    stops = [(18, 26, 60), (92, 62, 168), (236, 138, 172)]
    for y in range(WH):
        for x in range(WW):
            t = (x / float(WW - 1) * 0.6 + y / float(WH - 1) * 0.4)
            seg = min(int(t * 2), 1)
            lt = t * 2 - seg
            a, b = stops[seg], stops[seg + 1]
            r = lerp(a[0], b[0], lt)
            g = lerp(a[1], b[1], lt)
            bl = lerp(a[2], b[2], lt)
            # 暗角：越靠边越暗
            dx = abs(x - WW / 2.0) / (WW / 2.0)
            dy = abs(y - WH / 2.0) / (WH / 2.0)
            v = 1.0 - 0.35 * (dx * dx + dy * dy)
            px[x, y] = (int(r * v), int(g * v), int(bl * v))
    return img


def wall_grid():
    """深灰底 + 细网格线 + 右上角柔光（低对比，适合当工作背景）。"""
    img = Image.new("RGB", (WW, WH), (22, 26, 34))
    d = ImageDraw.Draw(img)
    step = 64
    for x in range(0, WW, step):
        d.line([(x, 0), (x, WH)], fill=(34, 40, 52))
    for y in range(0, WH, step):
        d.line([(0, y), (WW, y)], fill=(34, 40, 52))
    # 柔光
    gl = Image.new("L", (WW, WH), 0)
    gd = ImageDraw.Draw(gl)
    r = 420
    for i in range(60):
        rr = int(r * (1.0 - i / 60.0))
        gd.ellipse([WW - r + (r - rr) // 2, -rr // 2, WW - r + (r + rr) // 2, rr + rr // 2],
                   fill=int(10 + i * 1.2))
    img = Image.composite(Image.new("RGB", (WW, WH), (70, 110, 160)), img, gl)
    return img


def wall_sunrise():
    """暖色天空渐变 + 一轮太阳 + 深色地平线。"""
    img = Image.new("RGB", (WW, WH))
    px = img.load()
    horizon = int(WH * 0.62)
    sky_top, sky_bot = (36, 54, 110), (255, 176, 96)
    for y in range(WH):
        if y < horizon:
            t = y / float(horizon)
            c = (lerp(sky_top[0], sky_bot[0], t), lerp(sky_top[1], sky_bot[1], t),
                 lerp(sky_top[2], sky_bot[2], t))
        else:
            t = (y - horizon) / float(WH - horizon)
            c = (lerp(38, 12, t), lerp(30, 10, t), lerp(40, 14, t))
        for x in range(WW):
            px[x, y] = c
    d = ImageDraw.Draw(img)
    # 太阳（在地平线上方一点），带柔和外圈
    sx, sy, sr = int(WW * 0.68), int(horizon * 0.78), 86
    for k in range(14, 0, -1):
        rr = sr + k * 6
        d.ellipse([sx - rr, sy - rr, sx + rr, sy + rr],
                  fill=(lerp(255, 255, 1) if False else 250 - k * 4, 200 - k * 6, 120 - k * 4))
    d.ellipse([sx - sr, sy - sr, sx + sr, sy + sr], fill=(255, 238, 190))
    # 地平线
    d.line([(0, horizon), (WW, horizon)], fill=(20, 18, 28), width=3)
    return img


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)

    print("==> 预设头像（代码画，%dx%d）" % (AV, AV))
    total = 0
    for name, cols, kind in AVATARS:
        c0, c1 = cols
        p, sz = make_avatar(name, c0, c1, kind, a.out)
        total += sz
        print("    %-18s %6d B  %s" % (name, sz, os.path.basename(p)))
    print("    头像合计 %d B（%d 张）" % (total, len(AVATARS)))

    print("==> 内置示例壁纸（%dx%d PNG）" % (WW, WH))
    walls = [("wall-aurora.png", wall_aurora()),
             ("wall-grid.png", wall_grid()),
             ("wall-sunrise.png", wall_sunrise())]
    wtotal = 0
    for name, img in walls:
        p = os.path.join(a.out, name)
        img.save(p, "PNG", optimize=True)
        sz = os.path.getsize(p)
        wtotal += sz
        print("    %-18s %6d B  %dx%d" % (name, sz, img.width, img.height))
    print("    壁纸合计 %d B（%d 张）" % (wtotal, len(walls)))

    # 用户投放目录：存在就报一下会打进卷的图（构建期由 make_shellvol 真正拷贝）
    user_dir = os.path.join(ROOT, "res", "wallpapers")
    if os.path.isdir(user_dir):
        exts = (".png", ".jpg", ".jpeg", ".bmp")
        u = sorted(f for f in os.listdir(user_dir) if f.lower().endswith(exts))
        print("==> 用户投放目录 res/wallpapers/：%d 张 -> 会一起打进 /Wallpapers/" % len(u))
        for f in u:
            print("    %-28s %7d B" % (f, os.path.getsize(os.path.join(user_dir, f))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
