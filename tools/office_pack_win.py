# -*- coding: utf-8 -*-
"""tools/office_pack_win.py - 把办公套件装进系统卷（v0.5.3）

交付纪律（与 /bin/browser、/bin/sounder、/bin/store 完全相同）：
  * 内核二进制里**一个字节都不加**；交付 = 系统卷里的 `/bin/office`（静态 ELF64，0755）；
  * 写进卷后**逐字节回读自检**（复用 tcc_pack_win 的 verify），再拼出完整演示盘。

用法（构建期；browservol.img 是卷链上一步的产物）：
  py -3 tools/office_pack_win.py --vol-in build64/browservol.img --vol-out build64/officevol.img \
      --office build64/office.elf --system build64/system.img --disk build64/sysdisk.img
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--fixture-img", default=None)
    ap.add_argument("--office", required=True, help="build64/office.elf")
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.office):
        sys.stderr.write("缺少办公套件本体：%s（先跑 user/office/build_office.sh）\n" % args.office)
        return 2
    with open(args.office, "rb") as f:
        data = f.read()
    if len(data) < 64 or data[:4] != b"\x7fELF":
        sys.stderr.write("不是 ELF：%s\n" % args.office)
        return 2
    files = {"/bin/office": data}
    for p in sorted(files):
        print("    %-22s = %d B" % (p, len(files[p])))

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
        vol.put(path, files[path], mode=0o755)
    out = vol.finish()
    bad = TP.verify(out, files)
    if bad:
        sys.stderr.write("卷回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    办公套件卷自检 OK：%d 个文件逐字节回读一致" % len(files))

    if args.fixture_img:
        if not args.system:
            sys.stderr.write("--fixture-img 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B；主分区 LBA %d 起）" % (args.fixture_img, len(disk), PART_MAIN_LBA))
        return 0

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    办公套件卷：%s（%d B）" % (args.vol_out, len(out)))
    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    演示盘：%s（%d B = %d 扇区）" % (args.disk, len(disk), args.target_sectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
