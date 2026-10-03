#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/sounder_pack_win.py - ★ 系统音效：把素材 + 用户态播放器装进系统卷（VimtuFS2 v4）

交付物（卷内路径）：
  /bin/sounder                  用户态播放器（build64/sounder.elf，静态 ELF64，<= 64 KiB）
  /usr/share/sounds/startup.wav 开机/登录完成（0.700 s）
  /usr/share/sounds/notify.wav  通知（0.150 s）
  /usr/share/sounds/click.wav   点击（0.030 s）
  /usr/share/sounds/error.wav   错误（0.250 s）

与 /bin/drvdemo、busybox 完全相同的一条体积纪律：**内核里一个字节都不加** —— 素材与播放器都是
"系统卷里的文件"，构建脚本用 64B 高熵探针在内核二进制里搜一遍（搜到就失败）。

两种用法：
  ① 构建期的卷链一步（读上一步的卷 -> 写下一步的卷；写完**逐字节回读自检**）：
     py -3 tools/sounder_pack_win.py --vol-in build64/busyboxvol.img --vol-out build64/soundvol.img \\
           --sounder build64/sounder.elf --sounds-dir build64/sounds \\
           --system build64/system.img --disk build64/sysdisk.img
  ② 验收夹具盘（从零造一块卷，测试用；同样逐字节回读自检）：
     py -3 tools/sounder_pack_win.py --fixture-img build64/sounds64_test.img \\
           --sounder build64/sounder.elf --sounds-dir build64/sounds \\
           --with-shell build64/shell.bin --system build64/system.img

退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
PART_MAIN_LBA = 8009                      # = kernel/part64.h / make_shellvol.py
SOUNDS = ("startup", "notify", "click", "error")
SOUND_DIR = "/usr/share/sounds"


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")     # VolumeEdit / Volume2 / verify / build_disk
SG = load_mod("sounds_gen", "sounds_gen.py")         # 素材统计（宿主侧独立重算）


def sha(b):
    return hashlib.sha256(b).hexdigest()


def vol_read(vol_bytes, path):
    """**独立**按卷格式把文件读回来（直接块 -> ind -> dind 三级都走）。"""
    import struct
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


def collect_inputs(args):
    """返回 {卷内路径: 字节} + 打印宿主侧素材统计（这就是报告里的 ① 的对照值）。"""
    files = {}
    if not os.path.exists(args.sounder):
        sys.stderr.write("找不到 build64/sounder.elf：%s\n" % args.sounder)
        return None
    sb = open(args.sounder, "rb").read()
    if sb[:4] != b"\x7fELF" or sb[4] != 2:
        sys.stderr.write("/bin/sounder 不是 ELF64：%d B\n" % len(sb))
        return None
    if len(sb) > 65536:
        sys.stderr.write("/bin/sounder 超过 64 KiB（用户窗口装载区上限）：%d B\n" % len(sb))
        return None
    files["/bin/sounder"] = sb
    print("    /bin/sounder = %d B（静态 ELF64；sha256=%s）" % (len(sb), sha(sb)[:16]))

    for n in SOUNDS:
        p = os.path.join(args.sounds_dir, n + ".wav")
        if not os.path.exists(p):
            sys.stderr.write("缺素材：%s（先跑 py -3 tools/sounds_gen.py）\n" % p)
            return None
        d = open(p, "rb").read()
        st = SG.stats(p)
        files["%s/%s.wav" % (SOUND_DIR, n)] = d
        print("    %s/%s.wav = %d B | %d Hz/%dch/%dbit frames=%d dur=%.1f ms peak=%d "
              "sha256=%s" % (SOUND_DIR, n, len(d), st["rate"], st["ch"], st["bits"],
                             st["frames"], st["dur_ms"], st["peak"], st["sha256"][:16]))
    if args.with_shell:
        sh = open(args.with_shell, "rb").read()
        files["/bin/shell.bin"] = sh
        print("    /bin/shell.bin = %d B（夹具用：让测试能通过用户态 shell 的 `run` 传 argv）" % len(sh))
    return files


def verify_and_report(vol_bytes, files):
    """逐字节回读自检 + 打印"宿主 sha256 vs 卷内读回 sha256"（这就是 ① 的证据）。"""
    bad = TP.verify(vol_bytes, files)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return False
    for path in sorted(files):
        got = vol_read(vol_bytes, path)
        want = files[path]
        if got is None or got != want:
            sys.stderr.write("卷内读回不一致：%s（%s vs %s）\n"
                             % (path, len(got) if got is not None else -1, len(want)))
            return False
        print("    回读 %-34s %6d B  宿主 sha256=%s  卷内 sha256=%s"
              % (path, len(want), sha(want)[:12], sha(got)[:12]))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷镜像")
    ap.add_argument("--fixture-img", default=None, help="夹具盘输出（与 --vol-in/--vol-out 二选一）")
    ap.add_argument("--sounder", required=True, help="build64/sounder.elf")
    ap.add_argument("--sounds-dir", default=os.path.join("build64", "sounds"),
                    help="build64/sounds（tools/sounds_gen.py 的产物）")
    ap.add_argument("--with-shell", default=None, help="（夹具）build64/shell.bin -> 卷内 /bin/shell.bin")
    ap.add_argument("--system", default=None, help="build64/system.img（拼盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（构建期用）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if bool(args.vol_in) == bool(args.fixture_img):
        sys.stderr.write("--vol-out/--vol-in（装进已有卷）与 --fixture-img（从零造卷）必须二选一\n")
        return 2
    if args.vol_in and not args.vol_out:
        sys.stderr.write("给了 --vol-in 就必须给 --vol-out\n")
        return 2

    files = collect_inputs(args)
    if files is None:
        return 2

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
        vol.load(TP.Volume2(vol_sectors).finish())        # 空卷（几何/超级块合法）上继续写
        total = vol_sectors

    if total < vol_sectors:
        sys.stderr.write("输入卷只有 %d 扇区 < 需要的 %d 扇区\n" % (total, vol_sectors))
        return 2

    for path in sorted(files):
        kind = None
        vol.put(path, files[path], mode=(0o755 if path.startswith("/bin/") else 0o644), kind=kind)
    out = vol.finish()

    if not verify_and_report(out, files):
        return 1

    if args.fixture_img:
        if not args.system:
            sys.stderr.write("--fixture-img 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B = %d 扇区；主分区 LBA %d 起 = VimtuFS2 v4 卷）"
              % (args.fixture_img, len(disk), args.target_sectors, PART_MAIN_LBA))
        return 0

    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("    音效卷自检 OK：%d 个文件逐字节回读一致 -> %s" % (len(files), args.vol_out))

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
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(disk), args.target_sectors, PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
