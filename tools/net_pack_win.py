#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/net_pack_win.py - ★ P9：把用户态网络栈 /bin/netd 装进系统卷（VimtuFS2 v4）

交付物（卷内路径）：
  /bin/netd         Ring 3 网络栈 + CLI（build64/netd.elf，静态 ELF64，<= 64 KiB 主程序装载窗口）
  /etc/netd.conf    （可选）端口/域名/超时的覆盖配置 —— 只由**验收夹具**写（构建期不写，
                    缺省值就在 user/net/netd.c 的 cfg_defaults() 里：QEMU 用户网络的固定值）

与 /bin/sounder、busybox 完全相同的一条体积纪律：**内核里一个字节都不加** ——
netd 是"系统卷里的文件"；构建脚本末尾用 64B 高熵探针在内核二进制里搜一遍（搜到就构建失败）。

两种用法：
  ① 构建期的卷链一步（读上一步的卷 -> 写下一步的卷；写完**逐字节回读自检**）：
     py -3 tools/net_pack_win.py --vol-in build64/fontvol.img --vol-out build64/netvol.img \\
           --netd build64/netd.elf --system build64/system.img --disk build64/sysdisk.img
  ② 验收夹具盘（从**已有整卷** + system.img 造一块盘；可顺带写 /etc/netd.conf）：
     py -3 tools/net_pack_win.py --fixture-img build64/netuser64_test.img \\
           --from-vol build64/netvol.img --conf <(printf 'tcpport=5555\\n') --system build64/system.img \\
           --netd build64/netd.elf

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
NETD_PATH = "/bin/netd"
CONF_PATH = "/etc/netd.conf"


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")     # VolumeEdit / Volume2 / verify / build_disk


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


def collect(args):
    files = {}
    if not os.path.exists(args.netd):
        sys.stderr.write("找不到 build64/netd.elf：%s\n" % args.netd)
        return None
    nb = open(args.netd, "rb").read()
    if nb[:4] != b"\x7fELF" or nb[4] != 2 or nb[5] != 1:
        sys.stderr.write("/bin/netd 不是 ELF64 小端：%d B\n" % len(nb))
        return None
    if len(nb) > 96 * 1024:
        sys.stderr.write("/bin/netd 超过内核读盘缓冲上限 96 KiB：%d B\n" % len(nb))
        return None
    files[NETD_PATH] = nb
    print("    %s = %d B（静态 ELF64；sha256=%s）" % (NETD_PATH, len(nb), sha(nb)[:16]))
    if args.conf:
        if not os.path.exists(args.conf):
            sys.stderr.write("找不到 --conf 文件：%s\n" % args.conf)
            return None
        cb = open(args.conf, "rb").read()
        if len(cb) > 1024:
            sys.stderr.write("--conf 超过 1024 B（netd 的配置读取上限）\n")
            return None
        files[CONF_PATH] = cb
        print("    %s = %d B（验收夹具；sha256=%s）" % (CONF_PATH, len(cb), sha(cb)[:16]))
    return files


def verify_and_report(vol_bytes, files):
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
        print("    回读 %-22s %6d B  宿主 sha256=%s  卷内 sha256=%s"
              % (path, len(want), sha(want)[:12], sha(got)[:12]))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷镜像")
    ap.add_argument("--fixture-img", default=None,
                    help="夹具盘输出（整盘：system.img + 卷）；需要 --from-vol 或 --vol-in")
    ap.add_argument("--from-vol", default=None, help="夹具盘的输入卷（整卷字节，例如 build64/netvol.img）")
    ap.add_argument("--netd", required=True, help="build64/netd.elf")
    ap.add_argument("--conf", default=None, help="（夹具）写进 /etc/netd.conf 的文件")
    ap.add_argument("--system", default=None, help="build64/system.img（拼盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整系统盘（构建期用）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    vol_sectors = args.target_sectors - PART_MAIN_LBA
    if args.fixture_img:
        if not args.from_vol and not args.vol_in:
            sys.stderr.write("--fixture-img 需要 --from-vol（整卷）或 --vol-in\n")
            return 2
        if not args.system:
            sys.stderr.write("--fixture-img 必须同时给 --system（system.img）\n")
            return 2
        src = args.from_vol or args.vol_in
        img = open(src, "rb").read()
        if img[0:8] != b"VIMTUFS2":
            sys.stderr.write("输入不是 VimtuFS2 卷（整卷字节）：%s\n" % src)
            return 2
        total = int.from_bytes(img[20:24], "little")
        vol = TP.VolumeEdit(total)
        vol.load(img)
    elif args.vol_in:
        if not args.vol_out:
            sys.stderr.write("给了 --vol-in 就必须给 --vol-out\n")
            return 2
        img = open(args.vol_in, "rb").read()
        if img[0:8] != b"VIMTUFS2":
            sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
            return 2
        total = int.from_bytes(img[20:24], "little")
        vol = TP.VolumeEdit(total)
        vol.load(img)
    else:
        sys.stderr.write("要 --vol-in/--vol-out（构建期）或 --fixture-img（夹具）\n")
        return 2

    if total < vol_sectors:
        sys.stderr.write("输入卷只有 %d 扇区 < 需要的 %d 扇区\n" % (total, vol_sectors))
        return 2

    files = collect(args)
    if files is None:
        return 2

    for path in sorted(files):
        vol.put(path, files[path], mode=(0o755 if path.startswith("/bin/") else 0o644), kind=None)
    out = vol.finish()

    if not verify_and_report(out, files):
        return 1

    if args.fixture_img:
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B = %d 扇区；主分区 LBA %d 起 = VimtuFS2 v4 卷，%d 个文件）"
              % (args.fixture_img, len(disk), args.target_sectors, PART_MAIN_LBA, len(files)))
        return 0

    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("    网络卷自检 OK：%d 个文件逐字节回读一致 -> %s" % (len(files), args.vol_out))

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
        print("    系统盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(disk), args.target_sectors, PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
