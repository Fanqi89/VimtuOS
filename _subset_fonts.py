# -*- coding: utf-8 -*-
"""子集化 bahnschrift + chaparral（ASCII 32-126 + 界面扩展字符）。
输出 build/font_bahnschrift.ttf / build/font_chaparral.ttf。
"""
import os
from fontTools import subset

BASE = os.path.dirname(os.path.abspath(__file__))

# 必需字符：ASCII 可打印 + 界面常用扩展（× ÷ ± √ … ▾ ◄ ▲ ▼ · ° ℃ 等）
CHARS = "".join(chr(c) for c in range(32, 127))
CHARS += "×÷±√…▾▪◄▲▼·°℃①②③④⑤⑥⑦⑧⑨⑩¥£€™→←↑↓⟧"

def subset_font(src, dst, chars):
    opts = subset.Options()
    opts.flavor = None        # 保持 TTF
    opts.layout_features = [] # 去掉复杂特性（减小体积）
    opts.name_IDs = ["*"]     # 保留名称表
    opts.drop_tables = []     # 保留全部表（内核解析需要）
    font = subset.load_font(src, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    subset.save_font(font, dst, opts)
    print("OK:", os.path.basename(dst), os.path.getsize(dst), "bytes")

subset_font(os.path.join(BASE, "Fonts", "bahnschrift.ttf"),
            os.path.join(BASE, "build", "font_bahnschrift.ttf"), CHARS)
subset_font(os.path.join(BASE, "build", "font_chaparral_raw.ttf"),
            os.path.join(BASE, "build", "font_chaparral.ttf"), CHARS)
