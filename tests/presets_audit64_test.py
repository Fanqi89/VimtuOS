#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/presets_audit64_test.py - 预设资源「源目录 -> 卷内路径」逐文件核对（**秒级**，不跑 QEMU）

用户点名的两个源目录：
  build64/presets/    内置预设（构建期由 tools/make_presets.py 生成）
                      · avatar-01..06.png  第 1..6 个预设头像（代码画的渐变圆底 + 白色几何标记）
                      · wall-*.png         内置示例壁纸
  res/wallpapers/     **用户自己放的壁纸**（png/jpg/jpeg/bmp；README.md 之类非图片按扩展名忽略）

=============================================================================
★ v0.10.9【动态智能镜像】契约变更 —— 旧契约错在哪里，逐条写清楚
=============================================================================
旧契约（v0.10.8 及以前）：
  ② 卷里 `/Wallpapers/` 正好等于"内置 + res 里**边长 <= 2048** 的那些"；
  ③ ★ 负向对照：res 里**超限**的图必须**不在卷里**；卷内所有壁纸最长边 <= 2048。
  → 这两条建立在"内核一律只能解 <= 2048"这个**错误前提**上。实际 kernel/img64.cpp 的
    `img64_decode_fit64()` 会按 max_side 选 DCT 降采样因子 decim（1/2/4/8），
    真正拒绝大图的是 `IMG64_MAX_PIXELS = 32 MP`（源像素数），不是 2048。
    2048 只约束 **decim <= 1（整幅进内存）**的那条路径。
  → 后果：用户放一张 6.8 MB / 9686x5449 的壁纸，构建时被静默跳过（或卷装不下而报错），
    用户既看不到图，也不知道为什么。

新契约（v0.10.9）：
  ② 卷里 `/Wallpapers/` = 内置 wall-*.png ∪ **res/wallpapers/* 里所有可解析图片**
     ∪ 每张"最长边 > 2048"的原图所对应的派生副本 `<stem>-hd.jpg`（最长边 1920）；
  ③ ★ 反转的负向对照：**超限原图必须在卷里**（旧契约要求必须在卷外）；
  ⑥ ★ 新增：每张超限原图都必须有一张同名的 `-hd.jpg` 派生副本（能真正设为桌面壁纸）；
  ⑦ ★ 新增：派生副本必须是合法 baseline/ext JPEG、最长边 <= HD_MAXDIM、宽高比与原图一致；
  ⑧ ★ 新增：卷内每张图都满足内核 `img64_decode_fit64` 的真实闸门
     （max 边 <= IMG64_MAX_SIDE=16384 且 w*h <= 32 MP）；**超 32 MP 的图必须靠派生副本兜底**，
     否则用户选了壁纸内核只会静默回落内置壁纸；
  ⑩ ★ 新增：动态镜像 —— sysdisk.img 的盘大小 == 32,584 + 卷扇区数（MBR 分区项同值），
     卷扇区数按 1 MiB 对齐、空闲数据区 <= ~1.5 MiB（证明它是"按内容收口"的，不是固定 24,759）。

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
MAIN_LBA = 32584
IMG_EXTS = (".png", ".jpg", ".jpeg", ".bmp")
MAX_PX = 2048            # 装卷闸门触发线 = "整幅进内存"路径的上限（decim<=1）
HD_MAXDIM = 1920         # 派生副本最长边（tools/presets_pack_win.py: HD_MAXDIM）
KERNEL_MAX_SIDE = 16384  # kernel/img64.cpp: IMG64_MAX_SIDE
KERNEL_MAX_PIXELS = 32 * 1000 * 1000   # kernel/img64.cpp: IMG64_MAX_PIXELS
VOL_ALIGN = 2048         # make_shellvol.VOL_ALIGN_SECTORS（1 MiB，收口对齐粒度）
OLD_FIXED_SECTORS = 24759  # v0.10.8 及以前**固定**的卷扇区数（本次要废除的那个上限）

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
    if low.endswith(".bmp") and len(data) >= 26:
        w, h = struct.unpack("<ii", data[18:26])
        return abs(w), abs(h)
    return None


def jpeg_sof(name, data):
    """返回 SOF 标记号（0xC0/0xC1/0xC2…）；不是 JPEG 返回 None。"""
    if data[:2] != b"\xff\xd8":
        return None
    i = 2
    while i < len(data) - 9:
        if data[i] != 0xFF:
            i += 1
            continue
        m = data[i + 1]
        if m in (0xD8, 0xD9) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if 0xC0 <= m <= 0xCF and m not in (0xC4, 0xC8, 0xCC):
            return m
        i += 2 + struct.unpack(">H", data[i + 2:i + 4])[0]
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


def mbr_part(img, index):
    """MBR 分区项（16 B）：boot1 + 起始CHS3 + type1 + 结束CHS3 + 起始LBA4 + 扇区数4。"""
    off = 446 + 16 * index
    _boot, _chs1, ptype, _chs2, start, sectors = struct.unpack_from("<B3sB3sII", img, off)
    return ptype, start, sectors


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
    # ★ v0.6.0：目录重排后预设落在 /System/themes/{avatars,wallpapers}
    got_av = vol_files(v, "avatars")
    got_wp = vol_files(v, "wallpapers")

    print("源：build64/presets 头像 %d、内置壁纸 %d；res/wallpapers 用户壁纸 %d"
          % (len(avatars), len(walls_builtin), len(walls_user)))
    print("卷：/System/themes/avatars %d 个、/System/themes/wallpapers %d 个"
          "（total=%d 块 = %.2f MiB）"
          % (len(got_av), len(got_wp), v.total, v.total * 512 / 1048576.0))

    # ---------------- ① 头像 ----------------
    check("卷内 /System/themes/avatars 文件集合与 build64/presets/avatar-*.png 一致",
          set(got_av) == set(avatars), "卷=%s" % sorted(got_av))
    bad = [n for n in avatars if n in got_av and got_av[n] != avatars[n]]
    check("头像逐字节一致（%d 张）" % len(avatars), not bad, "不一致：%s" % bad)

    # ---------------- ② 壁纸集合：内置 + 全部用户图 + 派生副本 ----------------
    user_names = set(walls_user)
    derived_map = {}          # 派生文件名 -> 原图文件名
    over_src = {}             # 超限原图（>MAX_PX）
    for n, data in sorted(walls_user.items()):
        d = dims(n, data)
        if not d:
            continue
        if max(d) > MAX_PX:
            over_src[n] = d
            stem = os.path.splitext(n)[0]
            hd = stem + "-hd.jpg"
            if hd in user_names:
                continue      # 用户自己放的同名文件：当普通用户图处理，不算派生
            derived_map[hd] = n

    print("    分类：用户图 %d 张，其中超限（>%d）%d 张 %s；应派生 %d 张 %s"
          % (len(walls_user), MAX_PX, len(over_src), sorted(over_src),
             len(derived_map), sorted(derived_map)))

    want_wp = set(walls_builtin) | user_names | set(derived_map)
    check("卷内壁纸集合 = 内置 wall-*.png + res/wallpapers/* 全部可解析图片 + 派生 -hd.jpg",
          set(got_wp) == want_wp,
          "缺=%s 多=%s" % (sorted(want_wp - set(got_wp)), sorted(set(got_wp) - want_wp)))
    same = [n for n in walls_builtin if n in got_wp and got_wp[n] == walls_builtin[n]]
    check("内置壁纸逐字节一致（%d 张）" % len(walls_builtin), len(same) == len(walls_builtin),
          "一致 %d/%d" % (len(same), len(walls_builtin)))

    # ---------------- ④ 用户原图不缩图 ----------------
    same_u = [n for n in walls_user if n in got_wp and got_wp[n] == walls_user[n]]
    check("用户原图逐字节一致（打包期不缩图；%d 张）" % len(walls_user),
          len(same_u) == len(walls_user),
          "一致 %d/%d，不一致：%s"
          % (len(same_u), len(walls_user),
             [n for n in walls_user if n not in same_u]))

    # ---------------- ⑤ 反转的负向对照：超限原图必须在卷里 ----------------
    missing_over = [n for n in sorted(over_src) if n not in got_wp]
    check("★ 超限原图**必须在卷里**（v0.10.9 反转旧契约；%d 张%s）"
          % (len(over_src), ("：" + ", ".join(sorted(over_src))) if over_src else ""),
          not missing_over, "不在卷里：%s" % missing_over)

    # ---------------- ⑥ 派生副本必须存在 ----------------
    miss_hd = [hd for hd in sorted(derived_map) if hd not in got_wp]
    check("★ 每张超限原图都有 -hd.jpg 派生副本（%d 张）" % len(derived_map),
          not miss_hd,
          "缺派生：%s" % [(hd, derived_map[hd]) for hd in miss_hd])

    # ---------------- ⑦ 派生副本质量 ----------------
    hd_problems = []
    for hd, src in sorted(derived_map.items()):
        if hd not in got_wp:
            continue
        blob = got_wp[hd]
        d_hd = dims(hd, blob)
        sof = jpeg_sof(hd, blob)
        if not d_hd:
            hd_problems.append("%s(尺寸不可解析)" % hd)
            continue
        if sof not in (0xC0, 0xC1):
            hd_problems.append("%s(SOF=%s，内核只支持 baseline/ext)" % (hd, hex(sof) if sof else None))
        if max(d_hd) > HD_MAXDIM:
            hd_problems.append("%s(%dx%d 超过 %d)" % (hd, d_hd[0], d_hd[1], HD_MAXDIM))
        sd = dims(src, walls_user[src])
        if sd:
            r_src = float(sd[0]) / float(sd[1])
            r_hd = float(d_hd[0]) / float(d_hd[1])
            if abs(r_src - r_hd) / r_src > 0.02:
                hd_problems.append("%s(宽高比 %.4f != 原图 %.4f)" % (hd, r_hd, r_src))
    check("★ 派生副本：baseline/ext JPEG、最长边 <= %d、宽高比与原图一致（±2%%）" % HD_MAXDIM,
          not hd_problems, "问题：%s" % hd_problems)

    # ---------------- ⑧ 内核可解码性（kimg64_decode_fit64 的真实闸门） ----------------
    undecodable = []
    for n, blob in sorted(got_wp.items()):
        d = dims(n, blob)
        if not d:
            undecodable.append("%s(尺寸不可解析)" % n)
            continue
        if max(d) > KERNEL_MAX_SIDE:
            undecodable.append("%s(%dx%d 边长 > %d)" % (n, d[0], d[1], KERNEL_MAX_SIDE))
        elif d[0] * d[1] > KERNEL_MAX_PIXELS:
            # 超过 32 MP：原图内核解不了，必须靠派生副本兜底（否则设壁纸会静默回落）
            stem = os.path.splitext(n)[0]
            if (stem + "-hd.jpg") not in got_wp:
                undecodable.append("%s(%.1f MP > 32 MP 且没有 -hd.jpg 兜底)"
                                   % (n, d[0] * d[1] / 1000000.0))
    check("★ 卷内每张图都能被内核 img64_decode_fit64 解出（超 32 MP 的有 -hd.jpg 兜底）",
          not undecodable, "问题：%s" % undecodable)

    # ---------------- 逐张来源记录 ----------------
    for n, data in sorted(walls_user.items()):
        gd = dims(n, got_wp.get(n, b"")) if n in got_wp else None
        sd = dims(n, data)
        where = "%dx%d" % gd if gd else ("缺失" if n in got_wp else "不在卷里")
        extra = ""
        stem = os.path.splitext(n)[0]
        if (stem + "-hd.jpg") in derived_map:
            hdd = dims(stem + "-hd.jpg", got_wp.get(stem + "-hd.jpg", b""))
            extra = " + 派生 %s %s" % (stem + "-hd.jpg",
                                       ("%dx%d" % hdd) if hdd else "缺失")
        print("    · 用户壁纸 %-16s 源 %sx%s -> 卷内 %s（%d B -> %d B）%s"
              % (n, sd[0] if sd else "?", sd[1] if sd else "?",
                 where, len(data), len(got_wp.get(n, b"")), extra))

    # ---------------- ⑨ 非图片不得进卷 ----------------
    junk = [n for n in got_wp if not n.lower().endswith(IMG_EXTS)]
    check("非图片文件没有进卷（例如 res/wallpapers/README.md）", not junk, "多余：%s" % junk)

    # ---------------- ⑩ 动态镜像：盘大小 = 引导区 + 真实卷 ----------------
    img_bytes = os.path.getsize(VOL)
    img_sectors = img_bytes // 512
    ptype, start, sectors = mbr_part(open(VOL, "rb").read(512), 1)
    check("★ 动态镜像：盘扇区数 == 32,584 + 卷扇区数（%d + %d = %d）"
          % (MAIN_LBA, v.total, MAIN_LBA + v.total),
          img_sectors == MAIN_LBA + v.total,
          "实测 %d 扇区（%.2f MiB）" % (img_sectors, img_bytes / 1048576.0))
    check("★ MBR 主分区项 sectors == 卷扇区数、start == %d、type == 0x07" % MAIN_LBA,
          sectors == v.total and start == MAIN_LBA and ptype == 0x07,
          "type=0x%02x start=%d sectors=%d" % (ptype, start, sectors))
    # 累计卷内**文件字节总量**（只读 inode 的 size 字段，不读块）
    content_bytes = 0
    n_files = 0
    for i in range(v.inon):
        r = v.inode(i)
        if r[0] == 1:
            content_bytes += struct.unpack_from("<I", r, 4)[0]
            n_files += 1
    # 旧固定卷（v0.10.8 及以前）的可用数据区：24,759 - 520 块
    old_data_bytes = (OLD_FIXED_SECTORS - 520) * 512
    widened = content_bytes > old_data_bytes
    check("★ 镜像随内容变化（内容 %d 个文件 / %.2f MiB %s 旧固定卷可用 %.2f MiB -> 卷应%s 24,759 扇区）"
          % (n_files, content_bytes / 1048576.0, ">" if widened else "<=",
             old_data_bytes / 1048576.0, "大于" if widened else "小于"),
          (v.total > OLD_FIXED_SECTORS) if widened else (v.total < OLD_FIXED_SECTORS),
          "实测卷 total=%d 扇区（%.2f MiB）" % (v.total, v.total * 512 / 1048576.0))
    check("★ 卷不再是旧固定值 24,759 扇区（动态智能镜像）",
          v.total != OLD_FIXED_SECTORS, "total=%d" % v.total)

    free_bytes = 0
    for k in range(v.total - v.data_start):
        blk = v.data_start + k
        m, bit = divmod(blk, 4096)
        if m >= v.bmn:
            break
        b = v.blk(1 + m)
        if not ((b[bit >> 3] >> (bit & 7)) & 1):
            free_bytes += 512
    # 收口目标 = "真实内容 + VOL_TRIM_SLACK（2 MiB 运行时余量）"，再按 1 MiB 对齐，
    # 所以空闲上限 = (4096 + 2048) 块 = 3 MiB。旧实现是"固定留 12.09 MiB 卷"。
    slack_cap = (4096 + VOL_ALIGN) * 512
    check("★ 卷按内容收口（空闲数据区 <= %d B = 3 MiB，不是固定留 12.09 MiB）" % slack_cap,
          free_bytes <= slack_cap,
          "空闲 %d B（%.2f MiB）" % (free_bytes, free_bytes / 1048576.0))

    v.close()
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
