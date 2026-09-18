# -*- coding: utf-8 -*-
"""子集化开源字体（界面/正文 + 衬线/标题）。

源字体（SIL OFL 1.1，见 docs/字体许可说明.md 与 Fonts-open/OFL-*.txt）：
  Fonts-open/NotoSans-Regular.ttf   -> build/font_bahnschrift.ttf  （face 0：界面正文/无衬线）
  build/font_chaparral_raw.ttf      -> build/font_chaparral.ttf    （face 1：衬线/标题，由 _otf2ttf.py 从
                                                                     Fonts-open/NotoSerif-Regular.ttf 拷来）

★ 输出文件名必须保持不变：内核用 objcopy 生成的符号名由文件名决定
  （_binary_font_bahnschrift_ttf_start / _binary_font_chaparral_ttf_start），
  kernel/font.cpp 里就是按这两个名字引用的。
"""
import os
from fontTools import subset

BASE = os.path.dirname(os.path.abspath(__file__))

# 必需字符：ASCII 可打印 + 界面常用扩展（× ÷ ± √ … ▾ ◄ ▲ ▼ · ° ℃ 等）
CHARS = "".join(chr(c) for c in range(32, 127))
CHARS += "×÷±√…▾▪◄▲▼·°℃①②③④⑤⑥⑦⑧⑨⑩¥£€™→←↑↓⟧"

# 垂直度量归一化：内核渲染器把 em 固定成 16px、行高 20px、光栅缓冲 20 行
#（kernel/font.cpp 的 FONT_SIZE_PX / FONT_LINE_HEIGHT / FONT_PX，均未改动）。
# 内核只读 hhea 的 ascender（基线 = 行顶 + ascender），所以：
#   ascender 1.00em + descender 0.20em = 1.20em = 19.2px < 20px
# 与旧字体 bahnschrift 的 1.20em 一致 —— 布局常数不用改，且三套字体的基线一致，
# CJK/拉丁混排与跨 face（face0/face1/face2）对齐不会错位，字形也不会被 20 行缓冲裁掉。
ASC_EM = 1.00
DESC_EM = 0.20


def normalize_vmetrics(font, asc_em=ASC_EM, desc_em=DESC_EM):
    upem = font["head"].unitsPerEm
    asc = int(round(upem * asc_em))
    desc = -int(round(upem * desc_em))
    font["hhea"].ascent = asc
    font["hhea"].descent = desc
    font["hhea"].lineGap = 0
    if "OS/2" in font:
        os2 = font["OS/2"]
        os2.sTypoAscender = asc
        os2.sTypoDescender = desc
        os2.sTypoLineGap = 0
        os2.usWinAscent = asc
        os2.usWinDescent = -desc


def decompose_composites(font):
    """把复合字形展开成简单 glyf 轮廓（返回展开的字形个数）。

    内核 kernel/font.cpp 的 rasterize_glyph() 遇到 contours<0 直接 return false
    （"复合字形暂不支持"），所以子集里只要残留复合字形，那个字符就是空白。
    Noto Sans / Noto Serif 里 "·"（U+00B7）、"…"（U+2026）、'"'（U+0022）、
    "℃"（U+2103）都是复合字形，展开后点数仍远小于内核的 MAX_GLYPH_PTS=512。
    """
    from fontTools.pens.recordingPen import DecomposingRecordingPen
    from fontTools.pens.roundingPen import RoundingPen
    from fontTools.pens.ttGlyphPen import TTGlyphPen
    glyf = font["glyf"]
    glyphSet = font.getGlyphSet()
    n = 0
    for name in font.getGlyphOrder():
        g = glyf[name]
        if not g.isComposite():
            continue
        # 复合字形的分量要用带 draw(pen) 签名的 glyphSet 解析，再用 DecomposingRecordingPen
        # 按偏移/变换递归展开成普通轮廓，RoundPen 把变换出来的浮点坐标取整，
        # 最后回放到 TTGlyphPen 变成简单字形。
        rp = DecomposingRecordingPen(glyphSet)
        glyphSet[name].draw(rp)
        pen = TTGlyphPen(glyphSet)
        rp.replay(RoundingPen(pen))
        glyf[name] = pen.glyph()
        glyf[name].recalcBounds(glyf)
        n += 1
    if n:
        font["maxp"].recalc(font)
    return n


def subset_font(src, dst, chars):
    opts = subset.Options()
    opts.flavor = None        # 保持 TTF
    opts.layout_features = [] # 去掉复杂特性（减小体积；内核只解析 cmap/hmtx/glyf/loca）
    opts.name_IDs = ["*"]     # 保留名称表
    opts.drop_tables = []     # 保留全部表（内核解析需要）
    opts.hinting = False      # 内核不用 hinting：丢掉 cvt/fpgm/prep 与字形指令，省体积
    font = subset.load_font(src, opts)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    n_comp = decompose_composites(font)
    normalize_vmetrics(font)
    subset.save_font(font, dst, opts)
    print("OK:", os.path.basename(dst), os.path.getsize(dst), "bytes, 展开复合字形", n_comp, "个")


subset_font(os.path.join(BASE, "Fonts-open", "NotoSans-Regular.ttf"),
            os.path.join(BASE, "build", "font_bahnschrift.ttf"), CHARS)
subset_font(os.path.join(BASE, "build", "font_chaparral_raw.ttf"),
            os.path.join(BASE, "build", "font_chaparral.ttf"), CHARS)
