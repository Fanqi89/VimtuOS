# -*- coding: utf-8 -*-
"""tools/wm_titlefont_win.py —— 生成 Ring 3 合成器的**TTF 标题字模**（user/wm/wm_ttf.h）

为什么不在 wm 里直接链 stb_truetype：/lib/wm.elf 走的是 evfs/elf64 的 **64 KiB 装载窗口**
（kernel/usermode64.h 的 USER64_STACK_VA64 - USER64_CODE_VA64），font64.o 一个目标文件就 94,800 B。
所以：**构建机上用同一份 TTF、同一 em（16px）预光栅化 ASCII 32..126**，存成 4-bit alpha 表（~5 KB）。
字形与内核窗口标题同源（face 2 = sarasa-mono-sc），不再是 8x8 位图。

对齐口径（"同一把尺子"）：内核 `text_ttf(x+8, ty, …)` 的 y = **字形顶端**（kernel/font.cpp 的
"再往上抬到 0"），所以这里按"最高字形的顶端 = 0"归一化：by = y0 - min(y0)。这样大写字母/数字
顶到 title_y，小写字母按其 x-height 自然下沉 —— 与内核窗口标题逐行同一个高度参照。

用法：py -3 tools/wm_titlefont_win.py
输出：user/wm/wm_ttf.h（覆盖写）
"""
import os
import sys

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.stderr.write("需要 Pillow：py -3 -m pip install pillow\n")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAND = [
    os.path.join(ROOT, "build64", "font_mono.ttf"),                 # 卷里装的那份子集
    os.path.join(ROOT, "Fonts-open", "sarasa-mono-sc-Regular.ttf"),
]
EM = 16            # = kernel/font.cpp 的 FONT_SIZE_PX（默认 em 高度）
OUT = os.path.join(ROOT, "user", "wm", "wm_ttf.h")


def pick_font():
    for p in CAND:
        if os.path.exists(p):
            return p
    raise SystemExit("找不到字库：%r" % (CAND,))


def main():
    path = pick_font()
    f = ImageFont.truetype(path, EM)
    asc, _desc = f.getmetrics()

    # 第一遍：量 bbox / advance，求"字形顶端"的参照（ASCII 可打印里最高的那个）
    boxes = {}
    top_ref = 0
    for code in range(32, 127):
        ch = chr(code)
        adv = int(round(f.getlength(ch)))
        if code == 32:
            boxes[code] = (0, 0, 0, 0, adv)
            continue
        x0, y0, x1, y1 = f.getbbox(ch, anchor="ls")
        w, h = max(0, x1 - x0), max(0, y1 - y0)
        boxes[code] = (w, h, x0, y0, adv)
        if h > 0 and y0 < top_ref:
            top_ref = y0

    # 第二遍：光栅化 + 打包
    glyphs = []
    blob = bytearray()
    for code in range(32, 127):
        ch = chr(code)
        w, h, x0, y0, adv = boxes[code]
        if w <= 0 or h <= 0:
            glyphs.append((0, 0, 0, 0, adv, len(blob)))
            continue
        img = Image.new("L", (w + 4, h + 4), 0)
        ImageDraw.Draw(img).text((2 - x0, 2 + asc), ch, font=f, fill=255, anchor="ls")
        px = img.load()
        for gy in range(h):
            for gx in range(0, w, 2):
                a0 = px[2 + gx, 2 + gy] >> 4
                a1 = (px[2 + gx + 1, 2 + gy] >> 4) if (gx + 1) < w else 0
                blob.append(((a0 & 0xF) << 4) | (a1 & 0xF))
        assert len(blob) < 65536, "字模超 64 KiB（unsigned short off 放不下）"
        glyphs.append((w, h, x0, y0 - top_ref, adv, len(blob) - ((w + 1) // 2) * h))

    assert len(glyphs) == 95, len(glyphs)
    L = []
    L.append("/* user/wm/wm_ttf.h - ★ v0.11.3：Ring 3 合成器的 TTF 标题字模（自动生成，勿手改）")
    L.append(" *")
    L.append(" * 来源：%s" % os.path.relpath(path, ROOT).replace("\\", "/"))
    L.append(" * 生成：py -3 tools/wm_titlefont_win.py   （em=%dpx，ASCII 32..126，4-bit alpha）" % EM)
    L.append(" * 为什么预光栅化而不链 stb_truetype：/lib/wm.elf 的 ELF64 装载窗口只有 64 KiB")
    L.append(" *   （kernel/usermode64.h），font64.o 目标文件 94,800 B —— 塞不进去。本表 alpha %d B。" % len(blob))
    L.append(" * 对齐：by 以**最高字形顶端 = 0** 归一化，与 kernel/gui64.cpp 的 text_ttf(x,y,…)（y = 字形顶）同口径；")
    L.append(" *   屏幕坐标 = (pen_x + bx, title_y + by) + 字模内 (gx,gy)。")
    L.append(" */")
    L.append("#ifndef VIMTU64_WM_TTF_H")
    L.append("#define VIMTU64_WM_TTF_H")
    L.append("")
    L.append("#define WM_TF_EM     %d" % EM)
    L.append("#define WM_TF_ASC    %d" % asc)
    L.append("#define WM_TF_FIRST  32")
    L.append("#define WM_TF_LAST   126")
    L.append("#define WM_TF_COUNT  (WM_TF_LAST - WM_TF_FIRST + 1)")
    L.append("")
    L.append("struct WmTfGlyph { unsigned char w, h; signed char bx, by; unsigned char adv; unsigned short off; };")
    L.append("")
    L.append("static const struct WmTfGlyph g_wm_tf[WM_TF_COUNT] = {")
    for i, (w, h, bx, by, adv, off) in enumerate(glyphs):
        ch = chr(32 + i)
        c = ch if ch not in ('"', "\\") else "\\" + ch
        L.append("    { %2d, %2d, %4d, %3d, %2d, %5d },   /* '%s' */" % (w, h, bx, by, adv, off, c))
    L.append("};")
    L.append("")
    L.append("static const unsigned char g_wm_tf_alpha[%d] = {" % len(blob))
    for i in range(0, len(blob), 16):
        L.append("    " + ",".join("0x%02X" % b for b in blob[i:i + 16]) + ",")
    L.append("};")
    L.append("")
    L.append("#endif /* VIMTU64_WM_TTF_H */")
    open(OUT, "w", encoding="utf-8", newline="\n").write("\n".join(L) + "\n")
    print("字模写好：%s（95 字形，alpha %d B，asc=%d，top_ref=%d）" % (OUT, len(blob), asc, top_ref))


main()
