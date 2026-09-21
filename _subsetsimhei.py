# -*- coding: utf-8 -*-
"""face 1（中文面）子集化：Fonts-open/NotoSansSC-Regular.ttf -> build/font_simhei.ttf。

★ 文件名保持 `_subsetsimhei.py`、产物保持 build/font_simhei.ttf：
  objcopy 符号 `_binary_font_simhei_ttf_start/_end` 由文件名决定，kernel/font.cpp 按它引用；
  改名字就得同时改内核（本轮不动）。

本文件是**薄壳**：需求表（ASCII + GB2312 一级汉字 + 中文标点）与全部处理（glyf/loca 校验、
丢 hinting、复合字形展开、垂直度量归一化 1.00em/−0.20em、覆盖率自检）都在 _subset_fonts.py，
避免两份实现漂移。源字体是**静态 Regular**（NotoSansSC-Regular.ttf，由 _otf2ttf.py 从官方可变
字体 pin wght=400 派生）；万一给的是可变字体，_subset_fonts.load_static() 会如实报告并 pin。
"""
import os
import sys

BASE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, BASE)

import _subset_fonts as sf  # noqa: E402


def main():
    print("==> 字体子集化（face 1 中文面）")
    sf.subset_face(os.path.join(BASE, "Fonts-open/NotoSansSC-Regular.ttf"),
                   os.path.join(BASE, "build/font_simhei.ttf"),
                   sf.cjk_chars(), "中文面",
                   required=sf.CHARS_ASCII + sf.gb2312_level1(), face=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
