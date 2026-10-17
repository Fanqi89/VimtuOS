# -*- coding: utf-8 -*-
"""tools/browser_pack_win.py - 把浏览器装进系统卷（v0.5.0）

交付纪律（与 /bin/sounder、/bin/store、/bin/fontdemo 完全相同）：
  * 内核二进制里**一个字节都不加**；交付 = 系统卷里的两个文件
        /bin/browser        浏览器本体（静态 ELF64，0755）
        /www/index.html     发行版起始页（0644，UTF-8）
  * 写进卷后**逐字节回读自检**（复用 tcc_pack_win 的 verify），再拼出完整演示盘。

用法（构建期；storevol.img 是卷链上一步的产物）：
  py -3 tools/browser_pack_win.py --vol-in build64/storevol.img --vol-out build64/browservol.img \
      --browser build64/browser.elf --htdocs user/browser/www \
      --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PART_MAIN_LBA = 8009
SECTOR = 512


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")


def collect(args):
    files = {}
    if not os.path.exists(args.browser):
        sys.stderr.write("缺少浏览器本体：%s（先跑 user/browser/build_browser.sh）\n" % args.browser)
        return None
    with open(args.browser, "rb") as f:
        d = f.read()
    if len(d) < 64 or d[:4] != b"\x7fELF":
        sys.stderr.write("浏览器本体不是 ELF：%s\n" % args.browser)
        return None
    files["/bin/browser"] = d
    index = os.path.join(args.htdocs, "index.html")
    if not os.path.exists(index):
        sys.stderr.write("缺少起始页：%s\n" % index)
        return None
    with open(index, "rb") as f:
        files["/www/index.html"] = f.read()
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷镜像")
    ap.add_argument("--fixture-img", default=None, help="夹具盘输出（测试用）")
    ap.add_argument("--browser", required=True, help="build64/browser.elf")
    ap.add_argument("--htdocs", default=os.path.join("user", "browser", "www"))
    ap.add_argument("--system", default=None, help="build64/system.img（拼盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（构建期用）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = collect(args)
    if files is None:
        return 2
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
        mode = 0o755 if path.startswith("/bin/") else 0o644
        vol.put(path, files[path], mode=mode)
    out = vol.finish()

    bad = TP.verify(out, files)
    if bad:
        sys.stderr.write("卷回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    浏览器卷自检 OK：%d 个文件逐字节回读一致" % len(files))

    if args.fixture_img:
        if not args.system:
            sys.stderr.write("--fixture-img 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B；主分区 LBA %d 起 = VimtuFS2 v4 卷）"
              % (args.fixture_img, len(disk), PART_MAIN_LBA))
        return 0

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    浏览器卷：%s（%d B）" % (args.vol_out, len(out)))

    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    演示盘：%s（%d B = %d 扇区）" % (args.disk, len(disk), args.target_sectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
