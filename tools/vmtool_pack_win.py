#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/vmtool_pack_win.py - ★ v0.9.30：把 ring-3 的 vmtool 装进系统卷（/usr/bin/vmtool）

为什么单独一步：vmtool 是 ACL/配额/日志/快照的**用户态入口**，必须真的在系统卷里
（`/usr/bin/vmtool`），否则用户和验收脚本都调不到。装卷走的是和 dpkg/tcc 完全一样的
VolumeEdit 路径（`--vol-in` 接上游卷 -> 写文件 -> 逐字节回读自检 -> `--disk` 重拼 sysdisk.img）。

用法：
  py -3 tools/vmtool_pack_win.py --vol-in build64/dpkgvol.img --vol-out build64/vmtoolvol.img \
        --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import tcc_pack_win as TP  # noqa: E402

PART_MAIN_LBA = 8009
SZ_SECTOR = 512


def image_top(exe):
    """PT_LOAD 的最高地址（页对齐）——用来判"能不能进 execve 的 64 KiB 主程序窗口"。"""
    import struct
    if exe[:4] != b"\x7fELF":
        raise SystemExit("不是 ELF")
    e_phoff = struct.unpack_from("<Q", exe, 0x20)[0]
    e_phentsize = struct.unpack_from("<H", exe, 0x36)[0]
    e_phnum = struct.unpack_from("<H", exe, 0x38)[0]
    top = 0
    for i in range(e_phnum):
        off = e_phoff + i * e_phentsize
        p_type, _flags, _off, p_vaddr, _paddr, _filesz, p_memsz = struct.unpack_from("<IIQQQQQ", exe, off)
        if p_type != 1:                       # PT_LOAD
            continue
        end = p_vaddr + max(p_memsz, 1)
        end = (end + 0xFFF) & ~0xFFF
        if end > top:
            top = end
    return top


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vmtool", default=os.path.join(ROOT, "build64", "vmtool"))
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.vmtool):
        sys.stderr.write("缺 %s（先跑 bash tools/vmtool_build_win.sh）\n" % args.vmtool)
        return 2
    exe = open(args.vmtool, "rb").read()
    if exe[:4] != b"\x7fELF":
        sys.stderr.write("%s 不是 ELF\n" % args.vmtool)
        return 2
    top = image_top(exe)
    limit = 0x100010000                     # 4 GiB + 64 KiB = 用户栈起点（USER64_STACK_VA64）
    print("    映像最高地址 = 0x%X（窗口上界 = 0x%X，余量 %d B）" % (top, limit, limit - top))
    if top > limit:
        sys.stderr.write("ERROR: vmtool 映像越过 64 KiB 主程序窗口（execve 会拒载）\n")
        return 1

    files = {"/usr/bin/vmtool": (exe, 0o755)}
    print("    vmtool = %d B -> /usr/bin/vmtool" % len(exe))

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
    print("    vmtool 卷自检 OK：/usr/bin/vmtool 逐字节回读一致")

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    vmtool 卷：%s（%d B）" % (args.vol_out, len(out)))
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
