# -*- coding: utf-8 -*-
"""生成中文字体子集：开源字体 Noto Sans SC（SIL OFL 1.1）替代商业 simhei。

源：Fonts-open/NotoSansSC[wght].ttf —— Google Fonts 的可变字体（TrueType/glyf 轮廓，
    默认实例是 Thin，所以先 pin 到 wght=400 变成静态 Regular 再子集化）。
字符集（与旧版一致）：ASCII 32-126 + GB2312 一级汉字（3755 字，区位 16-55 区）+ 中文标点。
输出 build/font_simhei.ttf —— ★ 文件名不变（objcopy 符号 _binary_font_simhei_ttf_* 不能改）。
许可全文：Fonts-open/OFL-NotoSansSC.txt，说明见 docs/字体许可说明.md。
"""
import os
from fontTools import subset
from fontTools.ttLib import TTFont
from fontTools.varLib import instancer

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "Fonts-open", "NotoSansSC[wght].ttf")
DST = os.path.join(BASE, "build", "font_simhei.ttf")

# 垂直度量归一化：见 _subset_fonts.py 的说明（三套字体统一 1.00em + 0.20em = 1.20em，
# 等于旧字体 bahnschrift 的行高，布局常数 FONT_LINE_HEIGHT=20 不用改）。
ASC_EM = 1.00
DESC_EM = 0.20


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

    内核 kernel/font.cpp 的 rasterize_glyph() 不支持复合字形（contours<0 直接 return false），
    残留复合字形 = 那个字符画不出来。Noto Sans SC 的子集实测 0 个复合字形，
    这里保留同样的兜底：换别的源字体时不会静默变成空白。
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
        # 按偏移/变换递归展开成普通轮廓，RoundingPen 把变换出来的浮点坐标取整，
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


def load_static_regular(path):
    """可变字体 -> 静态 Regular（pin wght=400；其余轴保持默认）。"""
    font = TTFont(path)
    if "fvar" not in font:
        return font                      # 已经是静态字体
    loc = {}
    for axis in font["fvar"].axes:
        loc[axis.axisTag] = 400 if axis.axisTag == "wght" else axis.defaultValue
    return instancer.instantiateVariableFont(font, loc, inplace=False, updateFontNames=True)


def subset_simhei():
    chars_ascii = "".join(chr(c) for c in range(32, 127))
    chars = chars_ascii + gb2312_level1() + "×÷±√…▾▪·°℃①②③④⑤⑥⑦⑧⑨⑩“”‘’《》【】！？，。：；、"
    opts = subset.Options()
    opts.flavor = None
    opts.layout_features = []
    opts.name_IDs = ["*"]
    opts.drop_tables = []        # 保留全部表（内核解析需要）
    opts.hinting = False         # 内核不用 hinting：丢掉字形指令，省体积
    font = load_static_regular(SRC)
    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    n_comp = decompose_composites(font)
    normalize_vmetrics(font)
    subset.save_font(font, DST, opts)
    print("OK: font_simhei.ttf", os.path.getsize(DST), "bytes, chars:", len(set(chars)),
          ", 展开复合字形", n_comp, "个")


if __name__ == "__main__":
    subset_simhei()
