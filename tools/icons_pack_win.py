#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/icons_pack_win.py - ★ v0.6.1：把图标**逐张 PNG** 装进系统卷的 /System/icons/

背景（用户要求"图标文件应该看得见"）：
  以前图标只以 `iconpack.bin`（一个打包 blob，8.8 KB）+ 4 个 `icon_*.bin` 存在 /etc 里，
  文件管理器里看不到任何"图标文件"。原因是 inode 只有 512 个、已用 404 —— 拆不开。
  v0.6.0 把 inode 上限放到 2048、目录重排到 /System，现在可以逐张放了。

落点：`/System/icons/<system|apps>/<name>@<size>.png`
  内核 icons64.cpp 的第一优先级就是 `/icons/<sub>/<name>@<size>.png`
  （`/icons/` 经内核映射层指到 `/System/icons/`），命中就打 `[ICON64] load … src=vfs`；
  没命中才回退 iconpack.bin。所以 iconpack.bin 保留作兜底，不删。

输入：`build/icons/<sub>/*.png`（tools/make_iconpack.py 已经逐尺寸预渲染好的）
用法：
  py -3 tools/icons_pack_win.py --vol-in build64/sysvol.img --vol-out build64/iconvol.img
  py -3 tools/icons_pack_win.py --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import tcc_pack_win as TP  # noqa: E402

PART_MAIN_LBA = TP.SV.PART_MAIN_LBA
ICONS_DIR = os.path.join(ROOT, "build", "icons")


def collect(src_dir):
    """{卷内路径: (字节, mode)}；只收 <sub>/*.png（sub = system | apps）。"""
    out = {}
    if not os.path.isdir(src_dir):
        return out
    for sub in sorted(os.listdir(src_dir)):
        d = os.path.join(src_dir, sub)
        if not os.path.isdir(d):
            continue
        for f in sorted(os.listdir(d)):
            if not f.lower().endswith(".png"):
                continue
            p = os.path.join(d, f)
            if not os.path.isfile(p):
                continue
            out["/icons/%s/%s" % (sub, f)] = (open(p, "rb").read(), 0o644)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--icons", default=ICONS_DIR)
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = collect(args.icons)
    if not files:
        sys.stderr.write("没有图标 PNG（先跑 tools/make_iconpack.py，目录 %s）\n" % args.icons)
        return 2
    n_sys = len([p for p in files if p.startswith("/icons/system/")])
    n_app = len([p for p in files if p.startswith("/icons/apps/")])
    print("    图标源：%s 共 %d 张（system %d / apps %d，合计 %d B）"
          % (args.icons, len(files), n_sys, n_app, sum(len(v[0]) for v in files.values())))

    vol_sectors = args.target_sectors - PART_MAIN_LBA
    if args.vol_in:
        img = open(args.vol_in, "rb").read()
        if img[0:8] != b"VIMTUFS2":
            sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
            return 2
        total = int.from_bytes(img[20:24], "little")
        vol = TP.VolumeEdit(total)
        vol.load(img)
    else:
        vol = TP.VolumeEdit(vol_sectors)
        vol.load(TP.Volume2(vol_sectors).finish())
        total = vol_sectors
    if total < vol_sectors:
        sys.stderr.write("输入卷只有 %d 扇区 < 需要的 %d 扇区\n" % (total, vol_sectors))
        return 2

    for path in sorted(files):
        vol.put(path, files[path][0], mode=files[path][1])
    out = vol.finish()
    bad = TP.verify(out, {p: files[p][0] for p in files})
    if bad:
        sys.stderr.write("卷回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    图标卷自检 OK：%d 张逐字节回读一致（落点 /System/icons/{system,apps}/<name>@<size>.png）"
          % len(files))

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    图标卷：%s（%d B）" % (args.vol_out, len(out)))
    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(disk), args.target_sectors, PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
