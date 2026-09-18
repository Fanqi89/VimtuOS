# -*- coding: utf-8 -*-
"""源字体准备（衬线 face）：把 Fonts-open/NotoSerif-Regular.ttf 拷成 build/font_chaparral_raw.ttf，
供 _subset_fonts.py 子集化。

★ 关于 CFF（CFF/OTTO，比如 Source Han Sans OTF、Noto Sans CJK OTF）：本脚本**不做** CFF→glyf 转换，
  只是"源文件就是 TrueType(glyf) 轮廓"时的搬运 + 校验。原因：内核渲染器 kernel/font.cpp 的 face_init()
  要求字体同时具备 head/hhea/hmtx/maxp/**loca/glyf**/cmap 表，缺一个就直接 font_ok=false（连字都画不出来），
  而且 rasterize_glyph() 明确不支持复合字形（contours<0 直接返回）。所以源字体必须是 TrueType 轮廓，
  用 CFF 的 OTF 会在校验这一步直接报错退出（不会静默产出一个渲染不出来的内核）。
"""
import os, shutil
from fontTools.ttLib import TTFont

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "Fonts-open", "NotoSerif-Regular.ttf")
DST = os.path.join(BASE, "build", "font_chaparral_raw.ttf")

if not os.path.exists(SRC):
    raise SystemExit("ERROR: %s not found（下载方式见 README 第五节 / docs/字体许可说明.md）" % SRC)

font = TTFont(SRC, lazy=True)
tables = set(font.keys())
if "glyf" not in tables or "loca" not in tables:
    raise SystemExit("ERROR: %s 没有 glyf/loca 表（CFF/OTTO 轮廓）——内核 TrueType 渲染器不支持，"
                     "请换 TrueType 轮廓的源字体" % SRC)
if "CFF " in tables or "CFF2" in tables:
    raise SystemExit("ERROR: %s 含 CFF 表——本管线不做 CFF→glyf 转换" % SRC)
upem, n_glyphs = font["head"].unitsPerEm, font["maxp"].numGlyphs
font.close()

shutil.copyfile(SRC, DST)
print("OK: NotoSerif-Regular.ttf (TrueType/glyf, upem=%d, glyphs=%d) copied ->" % (upem, n_glyphs),
      DST, os.path.getsize(DST))
