#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/drivers_pack_win.py - ★ v0.9.33：把 ring3 驱动框架的用户态两件套装进系统卷

装三样东西（缺一不可，理由都在下面）：
  /usr/bin/ext4svc    ext4 **只读驱动服务**（ring3，通过零拷贝通道从 AHCI 拿数据）
  /usr/bin/drvwatch   看门狗**观察进程**（周期性读 /proc/vimtu/drivers 取证）
  /etc/r3drv.conf     驱动框架配置（autostart / service / watchdog / proto / disk / beat_ticks
                      + 给服务自己读的 bench / crash）

为什么 /etc/r3drv.conf 必须在**构建期**就落到卷里，而不是只靠内核启动时 seed：
  * 内核确实会在文件缺失时种一份（kernel/r3drv64.cpp 的 r3_cfg_load64，和 fontcfg64 同一套），
    但验收要能在**不改内核**的前提下切换"有没有驱动 / 崩不崩 / 跑不跑基准"，
    而测试脚本只会对磁盘镜像做 **等长字节替换**（例如 b"autostart=1" -> b"autostart=0"）。
    文件不存在就无可替换 —— 所以它必须是镜像里真实存在的一串 ASCII。

用法（与 tools/vmtool_pack_win.py 同一条链，接在它后面）：
  py -3 tools/drivers_pack_win.py --vol-in build64/vmtoolvol.img --vol-out build64/drvvol.img \
        --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import tcc_pack_win as TP  # noqa: E402

PART_MAIN_LBA = 32584          # 打印用（真正写盘位置由 SV.build_disk 决定）

# 这份内容必须与 kernel/r3drv64.cpp 的 kR3drvDefaultConf 逐行一致（字段一样、值一样）。
# 最后一行 wtest=0 只在测试里被替换成 wtest=1（等长），所以别改长度。
CONF_TEXT = (
    "# /etc/r3drv.conf - VimtuOS ring3 driver framework (v0.9.33)\n"
    "autostart=1\n"
    "service=/usr/bin/ext4svc\n"
    "watchdog=/usr/bin/drvwatch\n"
    "proto=ext4\n"
    "disk=auto\n"
    "beat_ticks=250\n"
    "bench=0\n"
    "crash=0\n"
    "wtest=0\n"
    "rpc=0\n"
).encode("ascii")


def image_top(exe):
    """PT_LOAD 的最高地址（页对齐）—— 用来判"能不能进 execve 的 64 KiB 主程序窗口"。"""
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
        end = (p_vaddr + max(p_memsz, 1) + 0xFFF) & ~0xFFF
        if end > top:
            top = end
    return top


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(ROOT, "build64"))
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = {}
    for name, dst, mode in (("ext4svc", "/usr/bin/ext4svc", 0o755),
                            ("drvwatch", "/usr/bin/drvwatch", 0o755)):
        p = os.path.join(args.dir, name)
        if not os.path.exists(p):
            sys.stderr.write("缺 %s（先跑 bash tools/drivers_build_win.sh build64）\n" % p)
            return 2
        exe = open(p, "rb").read()
        if exe[:4] != b"\x7fELF":
            sys.stderr.write("%s 不是 ELF\n" % p)
            return 2
        top = image_top(exe)
        limit = 0x100010000                     # 4 GiB + 64 KiB = 用户栈底（USER64_STACK_VA64）
        print("    %-9s %6d B  映射最高 = 0x%X（余量 %d B）" % (name, len(exe), top, limit - top))
        if top > limit:
            sys.stderr.write("ERROR: %s 映射越过 64 KiB 主程序窗口（execve 会拒）\n" % name)
            return 1
        files[dst] = (exe, mode)
    files["/etc/r3drv.conf"] = (CONF_TEXT, 0o644)

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
        sys.stderr.write("输入卷只有 %d 块 < 需要的 %d 块\n" % (total, vol_sectors))
        return 2

    for path in sorted(files):
        vol.put(path, files[path][0], mode=files[path][1])
    out = vol.finish()
    bad = TP.verify(out, {p: files[p][0] for p in files})
    if bad:
        sys.stderr.write("回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    ring3 驱动三件套回读自检 OK（逐字节一致）")

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    驱动卷 %s（%d B）" % (args.vol_out, len(out)))
    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    演示盘 %s（%d B = %d 扇区）" % (args.disk, len(disk), args.target_sectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
