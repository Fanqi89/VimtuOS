# -*- coding: utf-8 -*-
"""logo/logo.png -> build/logo_rgba.bin（240x150 RGBA8888，开机动画嵌入）"""
import os
from PIL import Image

BASE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(BASE, "logo", "logo.png")
DST = os.path.join(BASE, "build", "logo_rgba.bin")

img = Image.open(SRC).convert("RGBA")
img = img.resize((240, 150), Image.LANCZOS)
raw = img.tobytes()   # RGBA 每像素 4 字节
with open(DST, "wb") as f:
    f.write(raw)
print("OK: logo_rgba.bin", len(raw), "bytes (240x150)")
