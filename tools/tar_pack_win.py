#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/tar_pack_win.py - ★ B5：把 **tar** 装进系统卷（/bin/tar）。

第七步（最后一步）：把 build64/tar 写进**同一块**系统卷的 /bin/tar（写完逐字节回读自检），
给了 --system/--disk 时再拼一次完整演示盘。
gzip 早就在卷里（/bin/gzip、/bin/gunzip，A4-4c），所以 `tar czf x.tgz …`（tar 自己 fork/exec
gzip）与 `tar cf x.tar … && gzip -k -o x.tgz x.tar` 两条串法在 ring3 里都能跑。

用法：
    py -3 tools/tar_pack_win.py --vol-in build64/makevol.img --vol-out build64/tarvol.img \
          --tar build64/tar [--system build64/system.img --disk build64/sysdisk.img]
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷（make_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--tar", required=True, help="build64/tar -> /bin/tar")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.tar):
        sys.stderr.write("找不到 build64/tar：%s\n" % args.tar)
        return 2
    tar_bytes = open(args.tar, "rb").read()
    if tar_bytes[:4] != b"\x7fELF":
        sys.stderr.write("/bin/tar 不是 ELF：%d B\n" % len(tar_bytes))
        return 2
    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = {"/bin/tar": tar_bytes}
    vol.put("/bin/tar", tar_bytes, mode=0o755)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    tar 卷自检 OK：/bin/tar = %d B 逐字节回读一致 -> %s" % (len(tar_bytes), args.vol_out))

    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        open(args.disk, "wb").write(disk)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(disk), args.target_sectors, TP.SV.PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
