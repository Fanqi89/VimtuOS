#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_asset_images.py - 生成 gui_rs/assets/images/ 下的**自绘**壁纸与头像（CC0，无第三方素材）

为什么自绘：壁纸/头像目录是用户要求的资源结构的一部分，而"可商用、无侵权风险"的最短路径就是
自己画（本仓库里壁纸的真源仍是内核程序化壁纸 gfx64_wall_build_default64；这里的 PNG 只作为
assets 目录里的样例/素材，供将来换成文件壁纸时使用）。

产物（覆盖写入，可重复运行；字节稳定 = 同样输入同样输出）：
  gui_rs/assets/images/wallpapers/vimtu-gradient-320x200.png
  gui_rs/assets/images/avatars/vimtu-user-64.png

用法：py -3 tools/make_asset_images.py
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMG = os.path.join(ROOT, "gui_rs", "assets", "images")


def gradient_wallpaper(w=320, h=200):
    from PIL import Image
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            t = (x + y) / float(w + h)                 # 对角渐变
            r = int(18 + 84 * t)
            g = int(40 + 118 * (1.0 - t))
            b = int(120 + 96 * t)
            px[x, y] = (r, g, b)
    # 三团柔光（确定性位置，不引入随机数：同一版本产物字节一致）
    for (cx, cy, rad, tint) in ((int(w * 0.22), int(h * 0.30), int(h * 0.42), (255, 255, 255)),
                                (int(w * 0.74), int(h * 0.22), int(h * 0.30), (140, 220, 255)),
                                (int(w * 0.62), int(h * 0.78), int(h * 0.36), (150, 255, 220))):
        for y in range(max(0, cy - rad), min(h, cy + rad)):
            for x in range(max(0, cx - rad), min(w, cx + rad)):
                dx, dy = x - cx, y - cy
                d2 = dx * dx + dy * dy
                if d2 >= rad * rad:
                    continue
                k = 1.0 - (d2 / float(rad * rad))      # 径向衰减
                a = 0.28 * k * k
                cr, cg, cb = px[x, y]
                px[x, y] = (int(cr + (tint[0] - cr) * a),
                            int(cg + (tint[1] - cg) * a),
                            int(cb + (tint[2] - cb) * a))
    return im


def avatar(size=64):
    from PIL import Image, ImageDraw
    im = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.ellipse([0, 0, size - 1, size - 1], fill=(58, 122, 214, 255))
    d.ellipse([3, 3, size - 4, size - 4], fill=(96, 165, 250, 255))
    d.ellipse([int(size * 0.32), int(size * 0.20), int(size * 0.68), int(size * 0.56)],
              fill=(245, 248, 255, 255))                                  # 头
    d.ellipse([int(size * 0.18), int(size * 0.58), int(size * 0.82), int(size * 1.25)],
              fill=(245, 248, 255, 255))                                  # 肩
    return im


def main():
    wp = os.path.join(IMG, "wallpapers", "vimtu-gradient-320x200.png")
    av = os.path.join(IMG, "avatars", "vimtu-user-64.png")
    os.makedirs(os.path.dirname(wp), exist_ok=True)
    os.makedirs(os.path.dirname(av), exist_ok=True)
    gradient_wallpaper().save(wp, optimize=True)
    avatar().save(av, optimize=True)
    print("  %s (%d B)" % (os.path.relpath(wp, ROOT).replace("\\", "/"), os.path.getsize(wp)))
    print("  %s (%d B)" % (os.path.relpath(av, ROOT).replace("\\", "/"), os.path.getsize(av)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
