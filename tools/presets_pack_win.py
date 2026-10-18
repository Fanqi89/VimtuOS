# -*- coding: utf-8 -*-
"""tools/presets_pack_win.py —— v0.5.8：把**预设壁纸 / 预设头像**写进系统卷。

交付纪律（与 /bin/office、/bin/store 一致）：
  * 内核二进制里**一个字节都不加**；交付 = 系统卷里的两个目录
      `/Wallpapers/`（预设壁纸，含用户投放 res/wallpapers/*）
      `/Avatars/`（预设头像，代码画 6 张）
  * 写进卷后**逐字节回读自检**（复用 tcc_pack_win 的 verify），再拼出完整演示盘。

为什么单独一步（而不是塞进 tools/make_shellvol.py）：卷链上第一个 pack 步骤是**从原始区重建卷**的，
make_shellvol 产出的 /Wallpapers、/Avatars 在链上不会传下去（实测：shellvol.img 里有，
所有后续 *vol.img 都没有）。所以预设必须在**链尾**单独追加一次。

用法（构建期；vol-in = 卷链上一步的产物）：
  py -3 tools/presets_pack_win.py --vol-in build64/officevol.img --vol-out build64/presetsvol.img \
      --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PART_MAIN_LBA = 8009
SECTOR = 512


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
MK = load_mod("make_shellvol", "make_shellvol.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = {}
    for n, d in MK.preset_wallpapers():
        files["/Wallpapers/" + n] = (d, 0o644)
    for n, d in MK.preset_avatars():
        files["/Avatars/" + n] = (d, 0o644)
    if not files:
        sys.stderr.write("没有任何预设（先跑 tools/make_presets.py）\n")
        return 2
    walls = [p for p in files if p.startswith("/Wallpapers/")]
    avas = [p for p in files if p.startswith("/Avatars/")]
    if len(walls) < 1 or len(avas) < 1:
        sys.stderr.write("预设不完整：壁纸 %d 张 / 头像 %d 张\n" % (len(walls), len(avas)))
        return 2
    for p in sorted(files):
        print("    %-30s = %7d B" % (p, len(files[p][0])))

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
    print("    预设卷自检 OK：%d 个文件逐字节回读一致（/Wallpapers %d 张 / /Avatars %d 张）"
          % (len(files), len(walls), len(avas)))

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    预设卷：%s（%d B）" % (args.vol_out, len(out)))
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
