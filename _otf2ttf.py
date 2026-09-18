# -*- coding: utf-8 -*-
"""OTF(CFF) -> TrueType glyf 转换（ChaparralPro）。
Fonts/ChaparralPro-Regular.ttf 已是 TrueType 版，直接拷贝即可。
输出 build/font_chaparral_raw.ttf，供 _subset_fonts.py 子集化。
"""
import os, shutil

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "Fonts", "ChaparralPro-Regular.ttf")
DST = os.path.join(BASE, "build", "font_chaparral_raw.ttf")

if not os.path.exists(SRC):
    raise SystemExit("ERROR: %s not found" % SRC)
shutil.copyfile(SRC, DST)
print("OK: ChaparralPro TTF copied ->", DST, os.path.getsize(DST))
