#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/fontdemo_pack_win.py - ★ Ring 3 文字栈：把 /bin/fontdemo + 字库 + 合成器装进系统卷

交付物（卷内路径；**内核里一个字节都不加**，与 /bin/sounder、/bin/busybox 同一条纪律）：
  /bin/fontdemo.elf                      用户态文字演示（build64/fontdemo.elf，静态 ELF64，<= 64 KiB）
  /lib/wm.elf                            Ring 3 合成器本体（build64/wm.elf）—— 演示程序自己 fork+execve 它
  /Fonts/NotoSans-Regular.ttf            ← build/font_bahnschrift.ttf（西文面，_subset_fonts.py 子集）
  /Fonts-open/NotoSansSC-Regular.ttf     ← build/font_simhei.ttf     （中文面）
  /Fonts-open/SarasaMonoSC-Regular.ttf   ← build/font_mono.ttf       （终端等宽面）
  /Fonts-open/unifont-14.0.01.ttf        ← build/font_fallback.ttf   （缺字兜底面）

为什么卷里放的是**构建期子集**（build/font_*.ttf）而不是仓库根 Fonts/、Fonts-open/ 的全量字库：
  ① 内核用的就是这四份子集（同一个 _subset_fonts.py / _subsetsimhei.py 产物）—— 用户态与内核
     用同一批字形，像素对照才有意义；
  ② 用户态 mmap 区只有 4GiB+576KiB..4GiB+16MiB（~15.4 MiB），而 Fonts-open/NotoSansSC-Regular.ttf
     是 10.6 MB、卷总量只有 12.67 MB —— 全量字库放不下也读不动。这一点在报告里如实写明。

★ 为什么合成器放 /lib/wm.elf 而不是 /bin/wm.elf：内核 wl64_wm64("/bin/wm.elf") 是在
  gui64_run 之前**同步等待**的（kernel/kernel64.cpp:1173），那段时间桌面/终端还没起来；
  卷里不放 /bin/wm.elf（内核会打一行 [WL64] wm skipped），wm 由演示程序在桌面期自己 fork+execve。

用法（构建期卷链一步；写完**逐字节回读自检**）：
  py -3 tools/fontdemo_pack_win.py --vol-in build64/soundvol.img --vol-out build64/fontvol.img \\
        --fontdemo build64/fontdemo.elf --wm build64/wm.elf --fonts-dir build \\
        --system build64/system.img --disk build64/sysdisk.img
退出码：0 = 成功；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import hashlib
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
PART_MAIN_LBA = 8009                      # = kernel/part64.h / make_shellvol.py
DEFAULT_TARGET_SECTORS = 32768            # 16 MB（与其它卷链步骤同一口径）

# 卷内路径 -> 构建产物文件名（四个面与 kernel/font.cpp 的 face 0..3 一一对应）
FONT_MAP = (
    ("/Fonts/NotoSans-Regular.ttf",            "font_bahnschrift.ttf"),
    ("/Fonts-open/NotoSansSC-Regular.ttf",     "font_simhei.ttf"),
    ("/Fonts-open/SarasaMonoSC-Regular.ttf",   "font_mono.ttf"),
    ("/Fonts-open/unifont-14.0.01.ttf",        "font_fallback.ttf"),
)


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")


def sha(b):
    return hashlib.sha256(b).hexdigest()


def vol_read(vol_bytes, path):
    """**独立**按卷格式把文件读回来（直接块 -> ind -> dind 三级都走）。"""
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in TP._entries(vol_bytes, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return TP._read_file(vol_bytes, rec, total)
        cur = ino
    return None


def sfnt_tables(data):
    """宿主侧独立解析 sfnt 表目录（证明卷里那几份确实是 glyf 字库，且 unitsPerEm 可读）。"""
    if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"true", b"ttcf"):
        return None
    nt = int.from_bytes(data[4:6], "big")
    tabs = {}
    for i in range(nt):
        rec = data[12 + 16 * i:12 + 16 * i + 16]
        if len(rec) < 16:
            return None
        tabs[rec[:4].decode("latin-1")] = int.from_bytes(rec[8:12], "big")
    return tabs


def collect(args):
    files = {}
    for path, arg in (("/bin/fontdemo.elf", args.fontdemo), ("/lib/wm.elf", args.wm)):
        if not os.path.exists(arg):
            sys.stderr.write("找不到 %s（先跑 bash build64.sh）\n" % arg)
            return None
        d = open(arg, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            sys.stderr.write("%s 不是 ELF64 小端\n" % arg)
            return None
        if len(d) > 65536:
            sys.stderr.write("%s 超过 64 KiB（用户窗口装载区上限）：%d B\n" % (arg, len(d)))
            return None
        files[path] = d
        print("    %-22s = %6d B（静态 ELF64；sha256=%s）" % (path, len(d), sha(d)[:16]))

    for path, fname in FONT_MAP:
        src = os.path.join(args.fonts_dir, fname)
        if not os.path.exists(src):
            sys.stderr.write("找不到字库 %s（先跑 bash build64.sh 的 _subset_fonts.py 那几步）\n" % src)
            return None
        d = open(src, "rb").read()
        tabs = sfnt_tables(d)
        if not tabs or "glyf" not in tabs or "head" not in tabs or "hhea" not in tabs:
            sys.stderr.write("%s 不是 glyf TrueType 字库（表=%s）\n" % (src, sorted(tabs or ())))
            return None
        upem = int.from_bytes(d[tabs["head"] + 18:tabs["head"] + 20], "big")
        nglyph = int.from_bytes(d[tabs["maxp"] + 4:tabs["maxp"] + 6], "big") if "maxp" in tabs else -1
        files[path] = d
        print("    %-38s = %7d B  upem=%d nglyph=%d  glyf=1  sha256=%s"
              % (path, len(d), upem, nglyph, sha(d)[:16]))
    total = sum(len(v) for v in files.values())
    print("    交付合计 = %d B（%d 个文件；卷 %d 扇区 = %d B）"
          % (total, len(files), args.target_sectors - PART_MAIN_LBA,
             (args.target_sectors - PART_MAIN_LBA) * SECTOR))
    return files


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--fontdemo", default=os.path.join("build64", "fontdemo.elf"))
    ap.add_argument("--wm", default=os.path.join("build64", "wm.elf"))
    ap.add_argument("--fonts-dir", default="build")
    ap.add_argument("--system", default=None, help="build64/system.img（拼盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（构建期用）")
    ap.add_argument("--target-sectors", type=int, default=DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = collect(args)
    if files is None:
        return 2

    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol_sectors = args.target_sectors - PART_MAIN_LBA
    if total < vol_sectors:
        sys.stderr.write("输入卷只有 %d 扇区 < 需要的 %d 扇区\n" % (total, vol_sectors))
        return 2
    vol = TP.VolumeEdit(total)
    vol.load(img)
    try:
        for path in sorted(files):
            vol.put(path, files[path], mode=(0o755 if path.startswith("/bin/") else 0o644))
        out = vol.finish()
    except ValueError as e:
        sys.stderr.write("写卷失败（多半是卷满/inode 用完）：%s\n" % e)
        return 2

    bad = TP.verify(out, files)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    for path in sorted(files):
        got = vol_read(out, path)
        want = files[path]
        if got is None or got != want:
            sys.stderr.write("卷内读回不一致：%s（%s vs %s）\n"
                             % (path, len(got) if got is not None else -1, len(want)))
            return 1
        print("    回读 %-38s %7d B  宿主 sha256=%s  卷内 sha256=%s"
              % (path, len(want), sha(want)[:12], sha(got)[:12]))
    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("    字体/演示卷自检 OK：%d 个文件逐字节回读一致 -> %s" % (len(files), args.vol_out))

    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    演示盘：%s（%d B = %d 扇区）" % (args.disk, len(disk), args.target_sectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
