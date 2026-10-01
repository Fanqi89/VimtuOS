#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/wm_pack_win.py - ★ B-wm：把 Ring 3 合成器与两个客户端打包成**可引导的系统盘**

做三件事（与 tools/tcc_pack_win.py / gzip_pack_win.py 同一套路，只是这里装的是"程序"而不是"数据"）：
  1) 把 build64/{wm,wmclock,wmpanel}.elf 按交付路径写进 VimtuFS2 卷：
       /bin/wm.elf（合成器）、/wmclock.elf（时钟客户端）、/wmpanel.elf（交互客户端）、
       /etc/wm_probe（探针：卷里存在它 = full 模式，内核与三个程序各自判一次同一个文件）；
  2) 用 system.img 拼出一张**完整可引导盘**（build64/wm_test.img）——
     盘上就有这套文件，开机即可跑 wm（不需要测试脚本现搭夹具盘）；
  3) **逐字节回读核对**：make_shellvol.verify 把卷里每个文件的字节与源文件比一遍，
     不一致就非零退出（绝不用"写进去了"当证据）。

用法（Windows 原生 Python）：
    py -3 tools\\wm_pack_win.py                 # 默认读 build64/，写 build64/wm_test.img
    py -3 tools\\wm_pack_win.py --system <img> --out <img>
退出码：0 = 打包 + 回读全过；2 = 环境问题（缺文件/卷工具）。
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
OUT_IMG = os.path.join(ROOT, "build64", "wm_test.img")
PART_MAIN_LBA = 8009                      # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 32768                    # 16 MB（与其它夹具同一口径）

# 交付路径 -> build64/ 里的产物（合成器必须叫 /bin/wm.elf：kernel/wl64.cpp 就按这个路径找）
FILES = [
    ("/bin/wm.elf",      "wm.elf",     0o755),
    ("/wmclock.elf",     "wmclock.elf", 0o755),
    ("/wmpanel.elf",     "wmpanel.elf", 0o755),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--system", default=SYSTEM_IMG)
    ap.add_argument("--out", default=OUT_IMG)
    ap.add_argument("--build", default=os.path.join(ROOT, "build64"))
    args = ap.parse_args()

    try:
        import make_shellvol as msv
    except Exception as e:                                  # pragma: no cover
        sys.stderr.write("缺少 tools/make_shellvol.py：%s\n" % e)
        return 2
    if not os.path.exists(args.system):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % args.system)
        return 2

    payload = {}
    for vpath, name, _mode in FILES:
        p = os.path.join(args.build, name)
        if not os.path.exists(p):
            sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % p)
            return 2
        payload[vpath] = open(p, "rb").read()
    probe = b"/* wm probe: exists = full mode (Ring 3 compositor + 2 clients) */\n"

    vol = msv.Volume(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    for vpath, name, mode in FILES:
        parent = bin_ino if vpath.startswith("/bin/") else 0
        vol.write_file(os.path.basename(vpath), payload[vpath], parent=parent, mode=mode)
    vol.write_file("wm_probe", probe, parent=etc_ino, mode=0o644)
    vb = vol.finish()

    expect = dict(payload)
    expect["/etc/wm_probe"] = probe
    bad = msv.verify(vb, expect)
    if bad:
        sys.stderr.write("卷自检失败（逐字节回读不一致）：%s\n" % bad)
        return 2

    disk = msv.build_disk(open(args.system, "rb").read(), vb, TARGET_SECTORS)
    with open(args.out, "wb") as f:
        f.write(disk)
    print("=== Ring 3 合成器打包 OK ===")
    for vpath, name, _mode in FILES:
        print("    %-16s <- build64/%-14s %6d B（回读一致）" % (vpath, name, len(payload[vpath])))
    print("    /etc/wm_probe    %6d B（full 模式探针；去掉它 = short 模式）" % len(probe))
    print("    可引导盘：%s（%d B = system.img + 卷）" % (args.out, len(disk)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
