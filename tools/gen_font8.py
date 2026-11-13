#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/gen_font8.py - 为 /bin/peload 的 MessageBox 生成 8x8 位图字体

为什么需要它：`peload` 是**freestanding**（不链 libc / 不链 user/lib/font64）——
`font64` 里带 stb_truetype，一进来就 50 KiB，而 peload 现在 54.5 KiB、上限 64 KiB。
但 MessageBoxA 必须**真的把字画出来**（否则"弹了个空框"不算证据）。

所以：从宿主机的等宽字体栅格化出 **95 个可打印 ASCII 的 8x8 位图**（95×8 = 760 B），
生成 `user/win/peload_font8.h` 提交进仓库。运行时只做"查表 + 置位"，零依赖。

用法：py -3 tools/gen_font8.py [--font PATH] [--size N] [--show]
产物：user/win/peload_font8.h（可直接提交；内容确定，同字体同参数下逐字节稳定）
退出码：0 = 生成成功；2 = 找不到可用字体
"""
import argparse
import os
import sys

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "user", "win", "peload_font8.h")

# 候选字体：等宽优先（对话框里对齐好看），其次常用的无衬线
CANDIDATES = [
    r"C:\Windows\Fonts\consola.ttf",
    r"C:\Windows\Fonts\lucon.ttf",
    r"C:\Windows\Fonts\cour.ttf",
    r"C:\Windows\Fonts\arial.ttf",
    r"C:\Windows\Fonts\segoeui.ttf",
    r"C:\msys64\mingw64\share\fonts\liberation\LiberationMono-Regular.ttf",
]

FIRST = 32
LAST = 126
N = LAST - FIRST + 1
CW, CH = 8, 8
ADV = 6          # 实际显示时的字符前进量（字形只占 5 列，6 像素步进更好看）


def render(font_path, size, dy):
    """把 95 个字符各栅格化成 8x8 的 0/1 位图（MSB = 最左像素）。"""
    try:
        font = ImageFont.truetype(font_path, size)
    except OSError:
        return None
    glyphs = []
    for code in range(FIRST, LAST + 1):
        im = Image.new("L", (CW, CH), 0)
        d = ImageDraw.Draw(im)
        d.text((0, dy), chr(code), fill=255, font=font)
        rows = []
        for y in range(CH):
            b = 0
            for x in range(CW):
                if im.getpixel((x, y)) >= 110:
                    b |= 1 << (7 - x)
            rows.append(b)
        glyphs.append(rows)
    return glyphs


def art(glyphs, ch):
    rows = glyphs[ord(ch) - FIRST]
    out = []
    for r in rows:
        out.append("".join("#" if (r >> (7 - x)) & 1 else "." for x in range(CW)))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", default=None)
    ap.add_argument("--size", type=int, default=10)
    ap.add_argument("--dy", type=int, default=0)
    ap.add_argument("--show", action="store_true", help="打印几个字符的 ASCII art 供人工核验")
    args = ap.parse_args()

    fonts = ([args.font] if args.font else []) + CANDIDATES
    used = None
    for f in fonts:
        if f and os.path.exists(f):
            used = f
            break
    if not used:
        sys.stderr.write("找不到可用字体；用 --font 指定一个 .ttf\n")
        return 2

    glyphs = render(used, args.size, args.dy)
    if glyphs is None:
        sys.stderr.write("PIL 打不开字体：%s\n" % used)
        return 2

    # ---- 自检：空格必须全空；'H'/'A'/'o' 必须有墨；每个字形不许整行全黑（8 行都有内容或空白都正常，
    #      但"墨量"要有区分度）。判据宁可吵，也不要生成一张全是垃圾的表。
    if any(glyphs[0]):
        sys.stderr.write("空格字形不是空的 —— 字体/阈值不对\n")
        return 2
    for ch in ("H", "A", "o", "1", "M"):
        ink = sum(bin(r).count("1") for r in glyphs[ord(ch) - FIRST])
        if ink < 3:
            sys.stderr.write("%r 的墨量只有 %d 像素，字体/尺寸不对（试 --size 10）\n" % (ch, ink))
            return 2

    if args.show:
        for ch in "Hello VimtuOS 42!":
            a = art(glyphs, ch)
            print("---- %r ----" % ch)
            for line in a:
                print("   " + line)

    lines = []
    lines.append("/* peload_font8.h - ★ W3：MessageBox 用的 8x8 位图字体（**生成文件，别手改**）")
    lines.append(" *")
    lines.append(" * 生成器：tools/gen_font8.py")
    lines.append(" * 字体源：%s" % os.path.basename(used))
    lines.append(" * 参数  ：size=%d dy=%d，栅格 8x8，阈值 110/255" % (args.size, args.dy))
    lines.append(" *")
    lines.append(" * 覆盖可打印 ASCII 32..126（95 个字），每字 8 字节：一行一个字节，**MSB = 最左像素**。")
    lines.append(" * 为什么不用 user/lib/font64：那是 stb_truetype，50 KiB 起来，而 peload 上限 64 KiB。")
    lines.append(" * 为什么提交生成物而不是运行时栅格化：ring3 没有字体文件可用，且位图是确定性的。")
    lines.append(" */")
    lines.append("#ifndef PELOAD_FONT8_H")
    lines.append("#define PELOAD_FONT8_H")
    lines.append("")
    lines.append("#define PL_FONT8_FIRST  %du" % FIRST)
    lines.append("#define PL_FONT8_LAST   %du" % LAST)
    lines.append("#define PL_FONT8_W      8u")
    lines.append("#define PL_FONT8_H      8u")
    lines.append("#define PL_FONT8_ADV    %du" % ADV)
    lines.append("#define PL_FONT8_N      %du" % N)
    lines.append("")
    lines.append("static const unsigned char g_pl_font8[PL_FONT8_N][8] = {")
    for i, g in enumerate(glyphs):
        code = FIRST + i
        ascii_art = ", ".join("0x%02x" % r for r in g)
        lines.append("    { %s },   /* %3d  %s */" % (ascii_art, code,
                                                       (repr(chr(code))[1:-1] if code != 32 else "space")))
    lines.append("};")
    lines.append("")
    lines.append("#endif /* PELOAD_FONT8_H */")

    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")

    total_ink = sum(bin(r).count("1") for g in glyphs for r in g)
    print("生成 %s" % OUT)
    print("  字体=%s size=%d  字形=%d  字节=%d  字距=%d  总墨量=%d 像素"
          % (os.path.basename(used), args.size, N, N * 8, ADV, total_ink))
    return 0


if __name__ == "__main__":
    sys.exit(main())
