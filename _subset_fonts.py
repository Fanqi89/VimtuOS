# -*- coding: utf-8 -*-
"""子集化四个字体面里的三个（西文 / 终端等宽 / 缺字兜底），中文面在 _subsetsimhei.py。

面表（与 kernel/font.{h,cpp} 的 FONT_FACE_* 一一对应）：
  face 0  西文 UI     Fonts/NotoSans-Regular.ttf            -> build/font_bahnschrift.ttf
  face 1  中文        Fonts-open/NotoSansSC-Regular.ttf     -> build/font_simhei.ttf   （_subsetsimhei.py，先跑）
  face 2  终端等宽    Fonts-open/sarasa-mono-sc-regular.ttf -> build/font_mono.ttf
  face 3  缺字兜底    Fonts-open/unifont-14.0.01.ttf        -> build/font_fallback.ttf

★ 输出文件名必须保持不变：内核用 objcopy 生成的符号名由文件名决定
  （_binary_font_bahnschrift_ttf_start / _binary_font_simhei_ttf_start / _binary_font_mono_ttf_start /
   _binary_font_fallback_ttf_start），kernel/font.cpp 按这四个名字引用。

码点需求表（每个面一张）：
  face 0  = ASCII 32..126（硬要求）+ 界面常用符号（×÷±√…▾▪◄▲▼·°℃①..⑩¥£€™→←↑↓⟧ —— 源字体有就收，
            没有的交给 face 1/2/3 的查询链，见末端的链路自检）
  face 1  = ASCII（硬要求）+ GB2312 一级汉字（硬要求，界面用到的汉字是它的子集）+ 中文标点
  face 2  = ASCII（硬要求）+ 制表符/框线 U+2500..257F + 块元素 U+2580..259F + 几何图形 U+25A0..25FF +
            方向箭头（硬要求：ASCII 推进宽度 *2 == CJK 推进宽度，中英严格 1:2）
  face 3  = **扫描 kernel/*.cpp、kernel/*.h、user/*.asm 的字符串字面量**，取"前三个面的产物都没有、
            但界面/终端可能用到"的码点（Unifont 是 12MB / 5.7 万字的库，不全量嵌入）
"""
import os
import re
import sys
import glob

from fontTools import subset
from fontTools.ttLib import TTFont

BASE = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(BASE, "build")

# 内核渲染器把 em 固定成 16px、行高 20px、光栅缓冲 20 行（kernel/font.cpp 的 FONT_SIZE_PX /
# FONT_LINE_HEIGHT / FONT_PX 都没改）。四套字体统一归一化 1.00em ascender + 0.20em descender
# = 1.20em = 19.2px < 20px：基线一致，跨面（0/1/2/3）对齐不错位，字形也不会被 20 行缓冲裁掉。
ASC_EM = 1.00
DESC_EM = 0.20
EM_PX = 16                      # = kernel/font.cpp 的 FONT_SIZE_PX（报告推进宽度用）

CHARS_ASCII = "".join(chr(c) for c in range(32, 127))
# face 0：界面常用扩展符号（源字体有就收；缺口由查询链兜住）
UI_SYMBOLS = "×÷±√…▾▪◄▲▼·°℃①②③④⑤⑥⑦⑧⑨⑩¥£€™→←↑↓–—“”‘’（）"
CHARS_UI = CHARS_ASCII + UI_SYMBOLS
CJK_PUNCT = "×÷±√…▾▪·°℃①②③④⑤⑥⑦⑧⑨⑩“”‘’《》【】！？，。：；、（）"
# face 2：终端等宽 —— ASCII + 制表符/框线 + 块元素 + 几何图形 + 方向箭头 + 终端常用标点
CHARS_MONO = (CHARS_ASCII
              + "".join(chr(c) for c in range(0x2500, 0x25A0))
              + "".join(chr(c) for c in range(0x25A0, 0x2600))
              + "".join(chr(c) for c in range(0x2190, 0x2194))
              + "·×÷°…•±≈≤≥→←↑↓")


def gb2312_level1():
    """GB2312 一级汉字（区位 16-55，94 位/区），用 Python 内置 codec 解码。"""
    out = []
    for zone in range(16, 56):
        for pos in range(1, 95):
            try:
                out.append(bytes([zone + 0xA0, pos + 0xA0]).decode("gb2312"))
            except UnicodeDecodeError:
                pass
    return "".join(out)


def cjk_chars():
    """face 1 需求表：ASCII + GB2312 一级汉字（3755 字）+ 中文标点。

    界面用到的汉字（preload64 预热的 193 个字）是它的子集；生僻字/扩展区由 face 3 兜底。
    """
    return CHARS_ASCII + gb2312_level1() + CJK_PUNCT


# ==================== 源码字面量扫描（face 3 需求表的来源）====================
_STR_LIT = re.compile(r'"(?:[^"\\\n]|\\.)*"')


def scan_source_codepoints():
    """扫描 kernel/*.cpp、kernel/*.h、user/*.asm 的**字符串字面量** -> {码点: {文件名}}。

    ★ 只统计字符串字面量（真正可能被渲染出来的文字），不统计注释：注释里的汉字不必占内核体积。
    """
    hits = {}
    files = []
    for pat in ("kernel/*.cpp", "kernel/*.h", "user/*.asm"):
        files += sorted(glob.glob(os.path.join(BASE, pat)))
    for p in files:
        try:
            text = open(p, encoding="utf-8").read()
        except (UnicodeDecodeError, OSError):
            continue
        for m in _STR_LIT.finditer(text):
            lit = m.group(0)[1:-1]
            lit = (lit.replace("\\\\", "\x00").replace('\\"', '"').replace("\\n", "\n")
                      .replace("\\t", "\t").replace("\\r", "\r").replace("\x00", "\\"))
            for ch in lit:
                cp = ord(ch)
                if 0x80 <= cp <= 0xFFFF:
                    hits.setdefault(cp, set()).add(os.path.basename(p))
    return hits


# ==================== 公共处理 ====================
def cmap_of(path):
    """字体实际覆盖的码点集合（BMP）。"""
    f = TTFont(path, lazy=True)
    cmap = f.getBestCmap()
    out = set(cp for cp in cmap.keys() if cp <= 0xFFFF)
    f.close()
    return out


def decompose_composites(font):
    """把复合字形展开成简单 glyf 轮廓（返回展开的字形个数）。

    内核 kernel/font.cpp 的 rasterize_glyph() 遇到 contours<0 直接 return false
    （"复合字形暂不支持"），子集里只要残留复合字形，那个字符就是空白。
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


def load_static(path, label):
    """加载源字体；**静态字重**时代不需要 varLib.instancer，源文件要是可变字体就如实报告并 pin。"""
    font = TTFont(path)
    if "fvar" not in font:
        return font
    from fontTools.varLib import instancer
    axes = [(a.axisTag, a.defaultValue) for a in font["fvar"].axes]
    print("    ★ %s：源文件是**可变字体** %s —— 先 pin 成静态实例（wght=400）再子集化" % (label, axes))
    loc = {a.axisTag: (400 if a.axisTag == "wght" else a.defaultValue) for a in font["fvar"].axes}
    inst = instancer.instantiateVariableFont(font, loc, inplace=False, updateFontNames=True)
    font.close()
    return inst


def px_advance(path, cp, cmap=None):
    f = TTFont(path, lazy=True)
    cmap = cmap if cmap is not None else f.getBestCmap()
    g = cmap.get(cp)
    px = None if not g else int(round(f["hmtx"][g][0] * EM_PX / f["head"].unitsPerEm))
    f.close()
    return px


def subset_face(src, dst, chars, label, required="", face=None):
    """子集化一个面：glyf/loca 校验 + 丢 hinting + 展开复合字形 + 垂直度量归一化 + 覆盖率自检。"""
    if not os.path.exists(src):
        raise SystemExit("ERROR: 缺少源字体：%s（先跑 _otf2ttf.py 校验/补齐）" % src)
    if os.path.getsize(src) < 1024:
        raise SystemExit("ERROR: 源字体异常（只有 %d 字节，像是下载中断）：%s"
                         % (os.path.getsize(src), src))
    opts = subset.Options()
    opts.flavor = None         # 保持 TTF
    opts.layout_features = []  # 去掉复杂特性（内核只解析 cmap/hmtx/glyf/loca）
    opts.name_IDs = ["*"]      # 保留名称表
    opts.drop_tables = []      # 保留全部表（内核解析需要）
    opts.hinting = False       # 内核不用 hinting：丢掉 cvt/fpgm/prep 与字形指令，省体积
    font = load_static(src, label)
    tables = set(font.keys())
    if "glyf" not in tables or "loca" not in tables:
        raise SystemExit("ERROR: %s 没有 glyf/loca 表（CFF/OTTO 轮廓）——内核 TrueType 渲染器不支持" % src)
    if "CFF " in tables or "CFF2" in tables:
        raise SystemExit("ERROR: %s 含 CFF/CFF2 表——本管线不做 CFF→glyf 转换" % src)

    subsetter = subset.Subsetter(options=opts)
    subsetter.populate(text=chars)
    subsetter.subset(font)
    n_comp = decompose_composites(font)
    normalize_vmetrics(font)
    os.makedirs(BUILD, exist_ok=True)
    subset.save_font(font, dst, opts)
    font.close()

    # 覆盖率自检：硬要求缺一个就报错（否则就是"构建成功但缺字"），可选符号如实列出
    have = cmap_of(dst)
    want = list(dict.fromkeys(chars))
    missing = [ord(c) for c in want if ord(c) not in have]
    need_missing = [ord(c) for c in dict.fromkeys(required) if ord(c) not in have]
    if need_missing:
        raise SystemExit("ERROR: %s 子集缺**硬要求**字形：%s"
                         % (os.path.basename(dst), " ".join("U+%04X" % c for c in need_missing[:20])))
    if missing:
        print("    ★ %s：源字体没有这 %d 个可选码点（交给查询链兜底）：%s"
              % (os.path.basename(dst), len(missing), " ".join("U+%04X" % c for c in missing[:24])))
    f = TTFont(dst, lazy=True)
    n_glyph = f["maxp"].numGlyphs
    f.close()
    line = "OK: %-22s %8d B  glyphs=%-5d 需求=%-5d 未收=%d 展开复合字形=%d" % (
        os.path.basename(dst), os.path.getsize(dst), n_glyph, len(want), len(missing), n_comp)
    info = dict(dst=dst, glyphs=n_glyph, size=os.path.getsize(dst), missing=missing)
    if face == 2:
        # 中英 1:2 的判据：等宽面的 ASCII 推进宽度 *2 == 中文面（face 1）的汉字推进宽度。
        # 终端就是这样混排的：ASCII 从 face 2 取（8px），汉字由查询链落到 face 1（16px）。
        a = px_advance(dst, 0x41)
        cjk_art = os.path.join(BUILD, "font_simhei.ttf")
        c = px_advance(cjk_art, 0x4E00) if os.path.exists(cjk_art) else None
        if a and c and a * 2 == c:
            line += "  等宽 1:2 ok (mono ASCII=%dpx, CJK=%dpx)" % (a, c)
            info["mono"] = (a, c)
        else:
            raise SystemExit("ERROR: 等宽面不是 1:2：mono ASCII=%s, CJK=%s（face 2 的硬性要求）" % (a, c))
    print(line)
    return info


def fallback_chars(faces_covered):
    """face 3 需求表：源码字面量里出现、但 face 0/1/2 的**产物**都没有的码点。"""
    hits = scan_source_codepoints()
    need = sorted(cp for cp in hits if cp not in faces_covered)
    print("    兜底面需求表：kernel/*.cpp + kernel/*.h + user/*.asm 的字符串字面量里 %d 个 BMP 非 ASCII 码点，"
          "其中前三个面都没有的 %d 个" % (len(hits), len(need)))
    for cp in need:
        print("      U+%04X %s   <- %s" % (cp, chr(cp) if cp < 0x2E80 else "汉",
                                          ",".join(sorted(hits[cp]))[:64]))
    if not need:
        raise SystemExit("ERROR: 兜底面需求表是空的 —— 前三个面已覆盖源码里所有码点。\n"
                         "  自检需要至少 1 个只能由兜底面命中的码点（kernel/font.cpp 的 font_selftest()）")
    return "".join(chr(cp) for cp in need), need


def audit_chain(all_required, artifacts):
    """链路自检：需求表里的每个码点，四个面里至少有一个能画（否则就是缺字占位）。"""
    covered = {}
    for idx, a in enumerate(artifacts):
        covered[idx] = cmap_of(a["dst"])
    uncovered = []
    per_face = [0, 0, 0, 0]
    for cp in sorted(set(map(ord, all_required))):
        hit = None
        for idx in range(4):
            if cp in covered[idx]:
                hit = idx
                break
        if hit is None:
            uncovered.append(cp)
        else:
            per_face[hit] += 1
    print("    链路自检：需求表共 %d 个码点；面 0/1/2/3 各命中 %d/%d/%d/%d，无人可画 %d 个"
          % (len(set(map(ord, all_required))), per_face[0], per_face[1], per_face[2], per_face[3],
             len(uncovered)))
    if uncovered:
        raise SystemExit("ERROR: 这些码点四个面都画不出来（会变缺字占位）：%s"
                         % " ".join("U+%04X" % c for c in uncovered[:30]))
    return per_face


def main():
    print("==> 字体子集化（face 0 西文 / face 2 终端等宽 / face 3 缺字兜底；face 1 中文见 _subsetsimhei.py）")
    cjk_art = os.path.join(BUILD, "font_simhei.ttf")
    if not os.path.exists(cjk_art):
        raise SystemExit("ERROR: 缺 %s —— 先跑 _subsetsimhei.py（face 1 要在兜底面之前建好）" % cjk_art)
    f0 = subset_face(os.path.join(BASE, "Fonts/NotoSans-Regular.ttf"),
                     os.path.join(BUILD, "font_bahnschrift.ttf"), CHARS_UI, "西文面",
                     required=CHARS_ASCII, face=0)
    f2 = subset_face(os.path.join(BASE, "Fonts-open/sarasa-mono-sc-regular.ttf"),
                     os.path.join(BUILD, "font_mono.ttf"), CHARS_MONO, "终端等宽面",
                     required=CHARS_ASCII, face=2)
    covered = cmap_of(f0["dst"]) | cmap_of(cjk_art) | cmap_of(f2["dst"])
    fb_chars, fb_cps = fallback_chars(covered)
    f3 = subset_face(os.path.join(BASE, "Fonts-open/unifont-14.0.01.ttf"),
                     os.path.join(BUILD, "font_fallback.ttf"), fb_chars, "缺字兜底面",
                     required="".join(chr(c) for c in fb_cps), face=3)
    audit_chain(CHARS_UI + cjk_chars() + CHARS_MONO + fb_chars,
                [f0, dict(dst=cjk_art), f2, f3])
    print("    面 0/2/3 产物：%s" % ", ".join(os.path.basename(a["dst"]) for a in (f0, f2, f3)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
