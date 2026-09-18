# -*- coding: utf-8 -*-
"""生成 128x128 RGBA 桌面图标（Windows 风格）：
   icon_mycomputer.bin / icon_recyclebin.bin / icon_terminal.bin
"""
import os
from PIL import Image, ImageDraw

BASE = os.path.dirname(os.path.abspath(__file__))
S = 128

def save(img, name):
    p = os.path.join(BASE, "build", name)
    with open(p, "wb") as f:
        f.write(img.tobytes())
    print("OK:", name, len(img.tobytes()), "bytes")

# ---- My Computer：显示器 + 主机 ----
def mycomputer():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 显示器外框（深灰）
    d.rounded_rectangle([14, 16, 96, 84], radius=6, fill=(90, 96, 104, 255))
    # 屏幕（蓝渐变简化）
    d.rectangle([21, 23, 89, 77], fill=(64, 128, 255, 255))
    d.rectangle([24, 26, 86, 74], fill=(80, 150, 255, 255))
    # 屏幕亮点
    d.rectangle([40, 34, 66, 52], fill=(130, 185, 255, 255))
    # 底座
    d.rectangle([38, 84, 72, 92], fill=(90, 96, 104, 255))
    d.rectangle([30, 92, 80, 98], fill=(70, 76, 84, 255))
    # 主机箱
    d.rounded_rectangle([84, 40, 122, 104], radius=4, fill=(110, 116, 124, 255))
    d.rounded_rectangle([88, 46, 118, 100], radius=3, fill=(140, 146, 154, 255))
    d.rounded_rectangle([96, 62, 110, 82], radius=3, fill=(60, 66, 74, 255))
    d.rectangle([102, 66, 104, 78], fill=(120, 190, 255, 255))
    d.rectangle([96, 84, 110, 88], fill=(60, 66, 74, 255))
    d.rectangle([96, 90, 110, 94], fill=(60, 66, 74, 255))
    # 电源灯
    d.ellipse([92, 50, 96, 54], fill=(90, 230, 120, 255))
    return im

# ---- Recycle Bin：垃圾桶 ----
def recyclebin():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 桶身（蓝灰渐变）
    d.polygon([(36, 34), (92, 34), (98, 108), (30, 108)], fill=(130, 170, 200, 255))
    d.polygon([(42, 38), (86, 38), (91, 104), (37, 104)], fill=(165, 200, 225, 255))
    # 桶盖
    d.rounded_rectangle([28, 18, 100, 34], radius=4, fill=(105, 140, 170, 255))
    d.rounded_rectangle([34, 12, 94, 22], radius=3, fill=(90, 125, 155, 255))
    # 竖纹
    d.rectangle([52, 40, 56, 102], fill=(120, 160, 190, 255))
    d.rectangle([64, 40, 68, 102], fill=(120, 160, 190, 255))
    d.rectangle([76, 40, 80, 102], fill=(120, 160, 190, 255))
    # 回收标志（简化箭头）
    d.arc([50, 52, 78, 80], start=40, end=320, fill=(60, 95, 125, 255), width=4)
    d.polygon([(72, 54), (82, 52), (78, 62)], fill=(60, 95, 125, 255))
    d.polygon([(78, 74), (68, 78), (78, 82)], fill=(60, 95, 125, 255))
    return im

# ---- Terminal：黑底窗口 + 提示符 ----
def terminal():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    # 窗口
    d.rounded_rectangle([16, 14, 112, 110], radius=5, fill=(40, 44, 52, 255))
    d.rounded_rectangle([20, 18, 108, 106], radius=3, fill=(16, 20, 26, 255))
    # 标题栏
    d.rectangle([20, 18, 108, 30], fill=(60, 66, 76, 255))
    d.ellipse([26, 22, 30, 26], fill=(255, 90, 90, 255))
    d.ellipse([34, 22, 38, 26], fill=(255, 200, 60, 255))
    d.ellipse([42, 22, 46, 26], fill=(90, 220, 90, 255))
    # 文字（白色提示符）
    d.text((24, 40), ">_", fill=(220, 240, 220, 255))
    d.rectangle([54, 56, 96, 58], fill=(120, 255, 120, 255))   # 光标
    d.text((24, 66), "vimtu", fill=(150, 200, 255, 255))
    return im

save(mycomputer(), "icon_mycomputer.bin")
save(recyclebin(), "icon_recyclebin.bin")
save(terminal(), "icon_terminal.bin")
