# -*- coding: utf-8 -*-
"""logo/kaisi.png -> build/icon_start.bin（64x64 RGBA8888，开始按钮图标）"""
import os
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "logo", "kaisi.png")
DST = os.path.join(BASE, "build", "icon_start.bin")

img = Image.open(SRC).convert("RGBA")
img = img.resize((64, 64), Image.LANCZOS)
raw = img.tobytes()
with open(DST, "wb") as f:
    f.write(raw)
print("OK: icon_start.bin", len(raw), "bytes (64x64)")
