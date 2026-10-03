#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/jpeg_make_testset.py - 生成 img64 基线 JPEG 解码验收用的测试图集（宿主侧、确定性）

为什么需要它：tests/jpeg64_test.py 要把一批 **覆盖各条解码路径** 的 JPEG 装进 VimtuFS2 系统卷，
让内核（kernel/img64.cpp 的 JPEG 分支）真的去解，再与宿主 Pillow 的结果逐像素对照。图集必须
**确定性**（同一个脚本每次都生成同一批字节），否则"内核解出来是什么"不可复现。

图集（12 个槽位，与 kernel/img64.cpp 的 kJpgSuiteName 一一对应）：
  j01.jpg  纯色块 + 三条色带   64x48  **4:4:4**  SOF0          预期 rc=0
  j02.jpg  RGB 渐变            80x56  **4:2:2**  SOF0          预期 rc=0
  j03.jpg  文字 + 细线         80x56  **4:2:0**  SOF0          预期 rc=0
  j04.jpg  随机噪声            64x48  4:2:0 + **RSTn 重启标记** 预期 rc=0（rst>0）
  j05.jpg  方向性图案 + EXIF  48x32  4:4:4  Orientation=3       预期 rc=0
  j06.jpg  方向性图案 + EXIF  48x32  4:4:4  Orientation=6       预期 rc=0
  j07.jpg  方向性图案 + EXIF  48x32  4:2:0  Orientation=8       预期 rc=0
  j08.jpg  **截断**（j02 的前 50%）                              预期 rc=3（数据损坏）
  j09.jpg  **渐进式 SOF2**（j05 的内容）                         预期 rc=2（如实不支持）
  j10.jpg  **损坏**（j01 的熵编码段被打乱）                      预期 rc∈{2,3}
  j11.jpg  灰度（1 分量）      48x32  4:4:4(gray)                预期 rc=0
  j12.jpg  4:2:0 高质量（同一张方向图，质量 95）                 预期 rc=0（可选槽位）

用法：
  py -3 tools/jpeg_make_testset.py --out build64/jpeg_test          # 写文件（人看/留证）
退出码：0 = 成功；2 = 环境问题（没有 Pillow）
"""
import argparse
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 每个槽位的元信息（测试脚本与内核打点用它做断言；顺序 == kernel/img64.cpp 的 kJpgSuiteName）
SPEC = [
    ("j01.jpg", 64, 48, "444", 1, 0, 0),
    ("j02.jpg", 80, 56, "422", 1, 0, 0),
    ("j03.jpg", 80, 56, "420", 1, 0, 0),
    ("j04.jpg", 64, 48, "420", 1, 0, 1),      # 最后一位 = 是否要重启标记
    ("j05.jpg", 48, 32, "444", 3, 0, 0),
    ("j06.jpg", 48, 32, "444", 6, 0, 0),
    ("j07.jpg", 48, 32, "420", 8, 0, 0),
    ("j08.jpg", 0, 0, "-", 1, 3, 0),          # 截断（rc = -3）
    ("j09.jpg", 48, 32, "444", 1, 2, 0),      # 渐进式（rc = -2）
    ("j10.jpg", 64, 48, "444", 1, 2, 0),      # 损坏（rc ∈ {2,3}）
    ("j11.jpg", 48, 32, "gray", 1, 0, 0),     # 灰度（comps=1）
    ("j12.jpg", 48, 32, "420", 1, 0, 0),
]


def _pillow():
    try:
        from PIL import Image, ImageDraw, ImageFont   # noqa: F401
    except Exception as exc:                          # pragma: no cover
        sys.stderr.write("需要 Pillow 才能生成测试图集：%s\n" % exc)
        return None
    return "ok"


def _save(im, **kw):
    import io
    b = io.BytesIO()
    im.save(b, "JPEG", **kw)
    return b.getvalue()


def _subsampling(name):
    return {"444": 0, "422": 1, "420": 2, "gray": 0}.get(name, 0)


def _solid():
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (64, 48), (32, 120, 200))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, 31, 15], fill=(220, 40, 40))
    d.rectangle([32, 0, 63, 15], fill=(30, 180, 90))
    d.rectangle([0, 32, 63, 47], fill=(250, 240, 120))
    return im


def _gradient(w=80, h=56):
    from PIL import Image
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = (int(x * 255 / (w - 1)), int(y * 255 / (h - 1)),
                        int((x + y) * 255 / (w + h - 2)))
    return im


def _text(w=80, h=56):
    from PIL import Image, ImageDraw, ImageFont
    im = Image.new("RGB", (w, h), (250, 250, 245))
    d = ImageDraw.Draw(im)
    try:
        font = ImageFont.load_default()
    except Exception:
        font = None
    d.text((3, 3), "VimtuOS JPEG 4:2:0", fill=(10, 10, 10), font=font)
    d.text((3, 18), "SOF0 baseline", fill=(170, 20, 20), font=font)
    d.line([0, 40, w - 1, 40], fill=(20, 20, 130), width=2)
    d.text((3, 44), "0123456789", fill=(0, 90, 0), font=font)
    return im


def _noise(seed=20260401, w=64, h=48):
    from PIL import Image
    im = Image.new("RGB", (w, h))
    px = im.load()
    rnd = random.Random(seed)
    for y in range(h):
        for x in range(w):
            px[x, y] = (rnd.randrange(256), rnd.randrange(256), rnd.randrange(256))
    return im


def _pattern(w=48, h=32):
    """方向性图案：四象限异色 + 指向右侧的箭头（旋转/镜像一眼可辨）。"""
    from PIL import Image, ImageDraw
    im = Image.new("RGB", (w, h), (245, 245, 245))
    d = ImageDraw.Draw(im)
    d.rectangle([0, 0, w // 2 - 1, h // 2 - 1], fill=(200, 30, 30))
    d.rectangle([w // 2, 0, w - 1, h // 2 - 1], fill=(30, 160, 60))
    d.rectangle([0, h // 2, w // 2 - 1, h - 1], fill=(30, 60, 200))
    d.rectangle([w // 2, h // 2, w - 1, h - 1], fill=(240, 210, 40))
    d.polygon([(6, h // 2), (w - 8, h // 2), (w - 8, h // 2 - 6), (w - 2, h // 2),
               (w - 8, h // 2 + 6), (w - 8, h // 2)], fill=(10, 10, 10))
    return im


def _with_exif(im, orient):
    from PIL import Image
    ex = Image.Exif()
    ex[0x0112] = orient
    return ex


def build_testset():
    """返回 ({文件名: 字节}, {文件名: 元信息})；确定性（不依赖随机源之外的状态）。"""
    from PIL import Image
    files = {}
    pill = _pillow()
    if not pill:
        return None, None

    files["j01.jpg"] = _save(_solid(), quality=92, subsampling=0)
    files["j02.jpg"] = _save(_gradient(), quality=92, subsampling=1)
    files["j03.jpg"] = _save(_text(), quality=90, subsampling=2)
    files["j04.jpg"] = _save(_noise(), quality=88, subsampling=2, restart_marker_blocks=1)
    files["j05.jpg"] = _save(_pattern(), quality=92, subsampling=0, exif=_with_exif(_pattern(), 3))
    files["j06.jpg"] = _save(_pattern(), quality=92, subsampling=0, exif=_with_exif(_pattern(), 6))
    files["j07.jpg"] = _save(_pattern(), quality=92, subsampling=2, exif=_with_exif(_pattern(), 8))
    # j08 = 截断：j02 的前一半（保证是"解到一半没数据了"而不是"头就烂了"）
    files["j08.jpg"] = files["j02.jpg"][:len(files["j02.jpg"]) // 2]
    # j09 = 渐进式（SOF2）：内容与 j05 相同
    files["j09.jpg"] = _save(_pattern(), quality=90, progressive=True, subsampling=0)
    # j10 = 损坏：j01 的 SOS 段头之后 64 字节打乱（熵编码段被破坏）
    b = bytearray(files["j01.jpg"])
    i = b.find(b"\xff\xda")
    if i < 0:
        raise RuntimeError("j01 里找不到 SOS 标记")
    seg = (b[i + 2] << 8) | b[i + 3]
    start = i + 2 + seg
    for k in range(start, min(start + 64, len(b))):
        b[k] = 0x37 if (k & 1) else 0xC1
    files["j10.jpg"] = bytes(b)
    # j11 = 灰度（1 分量）
    files["j11.jpg"] = _save(_pattern().convert("L"), quality=92, subsampling=0)
    # j12 = 4:2:0 高质量
    files["j12.jpg"] = _save(_pattern(), quality=95, subsampling=2)

    meta = {}
    for name, w, h, samp, orient, rc, rst in SPEC:
        meta[name] = {"w": w, "h": h, "samp": samp, "orient": orient,
                      "expect_rc": rc, "want_rst": rst, "bytes": len(files[name])}
    return files, meta


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build64", "jpeg_test"))
    args = ap.parse_args()
    if not _pillow():
        return 2
    files, meta = build_testset()
    if not files:
        return 2
    os.makedirs(args.out, exist_ok=True)
    for name in sorted(files):
        with open(os.path.join(args.out, name), "wb") as f:
            f.write(files[name])
        m = meta[name]
        print("  %s bytes=%-6d %dx%d samp=%-4s orient=%d expect_rc=%d rst=%s"
              % (name, m["bytes"], m["w"], m["h"], m["samp"], m["orient"],
                 m["expect_rc"], "want" if m["want_rst"] else "-"))
    print("图集已写入 %s（%d 个文件，共 %d B）"
          % (args.out, len(files), sum(len(v) for v in files.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
