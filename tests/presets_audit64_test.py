#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/presets_audit64_test.py - ★ v0.5.16：预设资源「源目录 -> 卷内路径」逐文件核对（**秒级**，不跑 QEMU）

用户点名的两个源目录：
  build64/presets/    内置预设（构建期由 tools/make_presets.py 生成）
                      · avatar-01..06.png  第 1..6 个预设头像（代码画的渐变圆底 + 白色几何标记）
                      · wall-*.png         内置示例壁纸
  res/wallpapers/     **用户自己放的壁纸**（png/jpg/jpeg/bmp；README.md 之类非图片按扩展名忽略）

判据（每条都能独立证伪）：
  ① 卷里 `/Avatars/` 正好等于 `build64/presets/avatar-*.png`：文件数、字节、尺寸一致；
  ② 卷里 `/Wallpapers/` 正好等于 `build64/presets/wall-*.png` + `res/wallpapers/*`：
     文件数一致、没有多余项；未超限的图**逐字节一致**；
  ③ 任何进卷的图，最长边必须 **<= 2048**（内核 img64 的解码上限；超限的图在**打包期**按上限等比
     缩小，构建日志会打 `[wallpaper fit] …`）。修前 `res/wallpapers/1.jpeg` = 2649x1600，
     被内核如实拒绝（`[SET64] wall path=… ok=0 reason=jpeg too large (>2048)`）；
  ④ 非图片文件（如 res/wallpapers/README.md）**不得**进卷。

用法：py -3 tests/presets_audit64_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（缺产物）
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from volreloc64_test import Vol  # noqa: E402

PRESETS = os.path.join(ROOT, "build64", "presets")
USERWALL = os.path.join(ROOT, "res", "wallpapers")
VOL = os.path.join(ROOT, "build64", "sysdisk.img")
MAIN_LBA = 8009
IMG_EXTS = (".png", ".jpg", ".jpeg", ".bmp")
MAX_PX = 2048           # 与 kernel/img64.cpp 的解码上限一致

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def dims(name, data):
    low = name.lower()
    if low.endswith(".png") and data[:8] == b"\x89PNG\r\n\x1a\n":
        return struct.unpack(">II", data[16:24])
    if low.endswith((".jpg", ".jpeg")) and data[:2] == b"\xff\xd8":
        i = 2
        while i < len(data) - 9:
            if data[i] != 0xFF:
                i += 1
                continue
            m = data[i + 1]
            if m in (0xC0, 0xC1, 0xC2):
                return (struct.unpack(">H", data[i + 7:i + 9])[0],
                        struct.unpack(">H", data[i + 5:i + 7])[0])
            if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
                i += 2
                continue
            i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
    if low.endswith(".bmp"):
        w, h = struct.unpack("<ii", data[18:26])
        return abs(w), abs(h)
    return None


def src_dir(d, prefix=None):
    out = {}
    if not os.path.isdir(d):
        return out
    for f in sorted(os.listdir(d)):
        if not f.lower().endswith(IMG_EXTS):
            continue
        if prefix and not f.startswith(prefix):
            continue
        p = os.path.join(d, f)
        if os.path.isfile(p):
            out[f] = open(p, "rb").read()
    return out


def vol_files(v, dname):
    """卷内某个一级目录下的 {名字: 字节}（用 inode 的 parent 链确认目录，避免同名误判）。"""
    out = {}
    for i in range(v.inon):
        r = v.inode(i)
        if r[0] != 1:
            continue
        nl = r[1]
        if not (0 < nl <= 31):
            continue
        par = struct.unpack_from("<I", r, 28)[0]
        pr = v.inode(par)
        pnl = pr[1]
        pname = pr[40:40 + pnl].decode("latin1") if 0 < pnl <= 31 else ""
        if pname == dname:
            out[r[40:40 + nl].decode("latin1")] = v.read_file(r)
    return out


def main():
    if not os.path.exists(VOL):
        print("跳过：缺 %s（先跑 build64.sh）" % VOL)
        return 2
    if not os.path.isdir(PRESETS):
        print("跳过：缺 %s（先跑 build64.sh）" % PRESETS)
        return 2

    avatars = src_dir(PRESETS, "avatar-")
    walls_builtin = src_dir(PRESETS, "wall-")
    walls_user = src_dir(USERWALL)
    v = Vol(VOL, MAIN_LBA)
    got_av = vol_files(v, "Avatars")
    got_wp = vol_files(v, "Wallpapers")

    print("源：build64/presets 头像 %d、内置壁纸 %d；res/wallpapers 用户壁纸 %d"
          % (len(avatars), len(walls_builtin), len(walls_user)))
    print("卷：/Avatars %d 个、/Wallpapers %d 个（total=%d 块）"
          % (len(got_av), len(got_wp), v.total))

    # ① 头像
    check("卷内 /Avatars 文件集合与 build64/presets/avatar-*.png 一致",
          set(got_av) == set(avatars), "卷=%s" % sorted(got_av))
    bad = [n for n in avatars if n in got_av and got_av[n] != avatars[n]]
    check("头像逐字节一致（%d 张）" % len(avatars), not bad, "不一致：%s" % bad)

    # ② 壁纸（内置 + 用户）
    want_wp = set(walls_builtin) | set(walls_user)
    check("卷内 /Wallpapers 文件集合 = 内置 wall-*.png + res/wallpapers/*",
          set(got_wp) == want_wp,
          "缺=%s 多=%s" % (sorted(want_wp - set(got_wp)), sorted(set(got_wp) - want_wp)))
    same = [n for n in want_wp if n in got_wp and n in walls_builtin and got_wp[n] == walls_builtin[n]]
    check("内置壁纸逐字节一致（%d 张）" % len(walls_builtin), len(same) == len(walls_builtin),
          "一致 %d/%d" % (len(same), len(walls_builtin)))

    # ③ 尺寸上限 + 来源记录
    over = []
    for n, data in sorted(got_wp.items()):
        d = dims(n, data)
        if not d:
            check("卷内壁纸可解析尺寸：%s" % n, False)
            continue
        if max(d) > MAX_PX:
            over.append((n, d))
    check("卷内所有壁纸最长边 <= %d（内核解码上限）" % MAX_PX, not over, "超限：%s" % over)
    for n, data in sorted(walls_user.items()):
        gd = dims(n, got_wp.get(n, b"")) if n in got_wp else None
        sd = dims(n, data)
        print("    · 用户壁纸 %-16s 源 %sx%s -> 卷内 %s（%d B -> %d B）"
              % (n, sd[0] if sd else "?", sd[1] if sd else "?",
                 "%dx%d" % gd if gd else "缺失", len(data), len(got_wp.get(n, b""))))

    # ④ 非图片不得进卷
    junk = [n for n in got_wp if not n.lower().endswith(IMG_EXTS)]
    check("非图片文件没有进卷（例如 res/wallpapers/README.md）", not junk, "多余：%s" % junk)

    v.close()
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
