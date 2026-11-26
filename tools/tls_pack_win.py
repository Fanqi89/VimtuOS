#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/tls_pack_win.py - ★ v0.12.4：把 HTTPS 探针装进系统卷（VimtuFS2 v4）

交付物（卷内路径）：
  /bin/httpsprobe             ring3 HTTPS 探针（build64/httpsprobe.elf，mbedTLS + musl 静态 ELF64）
  /etc/ssl/certs/ca.pem       CA 包（PEM，可多份拼接；--ca 给了才写）—— 证书链校验的信任根
  /etc/tlsprobe.conf          （夹具）探针配置：正面目标 / 负向对照目标
  /etc/tls_probe.on           （夹具）**触发开关**：有它内核才在启动期跑 /bin/httpsprobe

两种用法（与 tools/net_pack_win.py 完全同构）：
  ① 构建期的卷链一步（读上一步的卷 -> 写下一步的卷；写完逐字节回读自检）：
     py -3 tools/tls_pack_win.py --vol-in build64/viewervol.img --vol-out build64/tlsvol.img \\
           --probe build64/httpsprobe.elf --ca build64/ca.pem \\
           --system build64/system.img --disk build64/sysdisk.img
  ② 验收夹具盘（从**已有整卷** + system.img 造一块盘；顺带写 /etc/tls_probe.on 与 conf）：
     py -3 tools/tls_pack_win.py --fixture-img build64/tlsuser64_test.img \\
           --from-vol build64/tlsvol.img --probe build64/httpsprobe.elf \\
           --conf <配置路径> --on --system build64/system.img

为什么开关是"卷上的文件"：QEMU 的 sendkey 在 1 fps 的 GUI 下会丢字/吞长命令（v0.12.2 的血账），
所以验收一律走"夹具卷 + 读串口"，一个键盘事件都不发。

退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import hashlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512
PART_MAIN_LBA = 32584                      # = kernel/part64.h / make_shellvol.py
PROBE_PATH = "/bin/httpsprobe"
CA_PATH = "/etc/ssl/certs/ca.pem"
CONF_PATH = "/etc/tlsprobe.conf"
ON_PATH = "/etc/tls_probe.on"


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
    path = TP.alias_vol_path(path)      # ★ 卷内路径映射（调用方写的还是旧路径）
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
    if not os.path.exists(args.probe):
        sys.stderr.write("找不到探针 ELF：%s\n" % args.probe)
        return None
    pb = open(args.probe, "rb").read()
    if pb[:4] != b"\x7fELF" or pb[4] != 2 or pb[5] != 1:
        sys.stderr.write("/bin/httpsprobe 不是 ELF64 小端：%d B\n" % len(pb))
        return None
    if len(pb) > 8 * 1024 * 1024:
        sys.stderr.write("/bin/httpsprobe 超过 8 MiB（VFS64_MAX_FILE_BYTES）\n")
        return None
    files[PROBE_PATH] = pb
    print("    %s = %d B（静态 ELF64；sha256=%s）" % (PROBE_PATH, len(pb), sha(pb)[:16]))

    if args.ca:
        if not os.path.exists(args.ca):
            sys.stderr.write("找不到 --ca 文件：%s\n" % args.ca)
            return None
        cb = open(args.ca, "rb").read()
        n = cb.count(b"-----BEGIN CERTIFICATE-----")
        if n < 1:
            sys.stderr.write("--ca 里一份 PEM 证书都没有：%s（%d B）\n" % (args.ca, len(cb)))
            return None
        files[CA_PATH] = cb
        print("    %s = %d B（%d 份证书；sha256=%s）" % (CA_PATH, len(cb), n, sha(cb)[:16]))

    if args.conf:
        if not os.path.exists(args.conf):
            sys.stderr.write("找不到 --conf 文件：%s\n" % args.conf)
            return None
        fb = open(args.conf, "rb").read()
        if len(fb) > 4096:
            sys.stderr.write("--conf 超过 4096 B（探针的配置读取上限）\n")
            return None
        files[CONF_PATH] = fb
        print("    %s = %d B（夹具；sha256=%s）" % (CONF_PATH, len(fb), sha(fb)[:16]))

    if args.on:
        # 内容故意是人读得懂的一行：卷里 ls 出来就知道"这盘会跑探针"
        files[ON_PATH] = b"1\n"
        print("    %s = 2 B（**触发开关**：内核启动期跑 /bin/httpsprobe）" % ON_PATH)
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
        print("    回读 %-24s %8d B  宿主 sha256=%s  卷内 sha256=%s"
              % (path, len(want), sha(want)[:12], sha(got)[:12]))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷镜像")
    ap.add_argument("--fixture-img", default=None,
                    help="夹具盘输出（整盘：system.img + 卷）；需要 --from-vol 或 --vol-in")
    ap.add_argument("--from-vol", default=None, help="夹具盘的输入卷（整卷字节，例如 build64/tlsvol.img）")
    ap.add_argument("--probe", required=True, help="build64/httpsprobe.elf")
    ap.add_argument("--ca", default=None, help="CA 包（PEM）-> /etc/ssl/certs/ca.pem")
    ap.add_argument("--conf", default=None, help="（夹具）探针配置 -> /etc/tlsprobe.conf")
    ap.add_argument("--on", action="store_true", help="（夹具）写 /etc/tls_probe.on，让内核启动期跑探针")
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
        mode = 0o755 if path.startswith("/bin/") else 0o644
        vol.put(path, files[path], mode=mode, kind=None)
    out = vol.finish()

    if not verify_and_report(out, files):
        return 1

    if args.fixture_img:
        system_bytes = open(args.system, "rb").read()
        # ★ v0.12.4：v0.5.11 之后 system.img 的**末尾还并入了安装载荷**（整块 sysdisk 的副本），
        #   于是它比"引导区 + 卷"大得多。夹具盘只要引导区那一段（LBA 0..PART_MAIN_LBA-1），
        #   载荷不进夹具 —— 否则这里会误报 "system.img 比整块盘还大"。
        boot_bytes = PART_MAIN_LBA * SECTOR
        if len(system_bytes) > boot_bytes:
            print("    system.img=%d B > 引导区 %d B -> 只取引导区（安装载荷不进夹具）"
                  % (len(system_bytes), boot_bytes))
            system_bytes = system_bytes[:boot_bytes]
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 引导区比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B = %d 扇区；主分区 LBA %d 起 = VimtuFS2 v4 卷）"
              % (args.fixture_img, len(disk), args.target_sectors, PART_MAIN_LBA))
        return 0

    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("    TLS 卷自检 OK：%d 个文件逐字节回读一致 -> %s" % (len(files), args.vol_out))

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
