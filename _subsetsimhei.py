# -*- coding: utf-8 -*-
"""子集化 simhei：ASCII 32-126 + GB2312 一级汉字（3755 字，区位 16-55 区）。
输出 build/font_simhei.ttf。
"""
import os
from fontTools import subset

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "Fonts", "simhei.ttf")
DST = os.path.join(BASE, "build", "font_simhei.ttf")

def gb2312_level1():
    """GB2312 一级汉字（区位 16-55，94 位/区），用 Python 内置 codec 解码。"""
    out = []
    for zone in range(16, 56):
        for pos in range(1, 95):
            b = bytes([zone + 0xA0, pos + 0xA0])
            try:
                out.append(b.decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)

def subset_simhei():
    chars_ascii = "".join(chr(c) for c in range(32, 127))
    chars = chars_ascii + gb2312_level1() + "×÷±√…▾▪·°℃①②③④⑤⑥⑦⑧⑨⑩“”‘’《》【】！？，。：；、"
    opts = subset.Options()
    opts.flavor = None
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.drop_tables = []
    font = subset.load_font(SRC, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    subset.save_font(font, DST, opts)
    print("OK: font_simhei.ttf", os.path.getsize(DST), "bytes, chars:", len(set(chars)))

if __name__ == "__main__":
    subset_simhei()
