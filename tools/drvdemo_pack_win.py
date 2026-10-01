#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/drvdemo_pack_win.py - ★ 本批：把 Ring 3 驱动服务骨架装进系统卷（/bin/drvdemo）。

交付方式与 /bin/tar、/bin/tcc、/bin/lua、/wlclient.elf 完全相同的一条纪律：**内核里一个字节都不加**。
本步读上一步的卷（默认 build64/tarvol.img），写 build64/drvsvcvol.img（写完**逐字节回读自检**），
给了 --system/--disk 时再拼一次完整演示盘 build64/sysdisk.img。

★ 与另一条线（Ring 3 合成器）的合流：他们的 /bin/wm.elf 由 tools/wm_pack_win.py 写。
  两个"卷链最后一步"必须串成一条链（谁的步骤在后面，谁就读前一步的 --vol-in），否则后跑的
  那一步会用自己的卷覆盖 sysdisk.img、把前一步写进去的文件丢掉 —— 合并时改 --vol-in 即可。

用法：
    py -3 tools/drvdemo_pack_win.py --vol-in build64/tarvol.img --vol-out build64/drvsvcvol.img \
          --drvdemo build64/drvdemo.elf [--system build64/system.img --disk build64/sysdisk.img]
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
    ap.add_argument("--vol-in", required=True, help="输入卷（tar_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--drvdemo", required=True, help="build64/drvdemo.elf -> /bin/drvdemo")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（build64/sysdisk.img）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.drvdemo):
        sys.stderr.write("找不到 build64/drvdemo.elf：%s\n" % args.drvdemo)
        return 2
    d = open(args.drvdemo, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        sys.stderr.write("/bin/drvdemo 不是 ELF64：%d B\n" % len(d))
        return 2
    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = {"/bin/drvdemo": d}
    vol.put("/bin/drvdemo", d, mode=0o755)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    drvdemo 卷自检 OK：/bin/drvdemo = %d B 逐字节回读一致 -> %s" % (len(d), args.vol_out))

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
