#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/browser_pack_win.py - ★ v0.12.6：把浏览器装进系统卷（VimtuFS2 v4）

交付物（卷内路径）：
  /bin/browser                我们的浏览器（build64/browser.elf，musl+mbedTLS+NetSurf 静态 ELF64）
  /etc/browser.conf           （夹具/构建期可选）首页与开关：一行 `home=<url>`
  /etc/browser.on             （夹具）**触发开关**：有它内核才在启动期跑 /bin/browser
  /etc/ssl/certs/ca.pem       TLS 信任根（v0.12.4 已交付；本脚本不动它，只保证浏览器不会因为缺 CA 而假绿）

两种用法（与 tools/tls_pack_win.py 同构）：
  ① 构建期的卷链一步（读上一步的卷 -> 写下一步的卷）：
     py -3 tools/browser_pack_win.py --vol-in build64/tlsvol.img --vol-out build64/browservol.img \
           --browser build64/browser.elf --system build64/system.img --disk build64/sysdisk.img
  ② 验收夹具盘（从**已有整卷** + system.img 造一块盘；顺带写 /etc/browser.on）：
     py -3 tools/browser_pack_win.py --fixture-img build64/browser64_test.img \
           --from-vol build64/browservol.img --browser build64/browser.elf --on --system build64/system.img

为什么开关是"卷上的文件"：QEMU sendkey 在 1 fps GUI 下会丢字/吞长命令（v0.12.2 的血账），
所以验收一律走"夹具盘 + 读串口"，一个键盘事件都不发。

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
BROWSER_PATH = "/bin/browser"
CONF_PATH = "/etc/browser.conf"
ON_PATH = "/etc/browser.on"
MAX_ELF = 8 * 1024 * 1024                  # VFS64_MAX_FILE_BYTES


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
    path = TP.alias_vol_path(path)
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
    if not os.path.exists(args.browser):
        sys.stderr.write("找不到浏览器 ELF：%s\n" % args.browser)
        return None
    pb = open(args.browser, "rb").read()
    if pb[:4] != b"\x7fELF" or pb[4] != 2 or pb[5] != 1:
        sys.stderr.write("/bin/browser 不是 ELF64 小端：%d B\n" % len(pb))
        return None
    if len(pb) > MAX_ELF:
        sys.stderr.write("/bin/browser 超过 %d B（VFS64_MAX_FILE_BYTES）\n" % MAX_ELF)
        return None
    files[BROWSER_PATH] = pb
    print("    %s = %d B（静态 ELF64；sha256=%s）" % (BROWSER_PATH, len(pb), sha(pb)[:16]))

    if args.conf:
        if not os.path.exists(args.conf):
            sys.stderr.write("找不到 --conf 文件：%s\n" % args.conf)
            return None
        fb = open(args.conf, "rb").read()
        if len(fb) > 4096:
            sys.stderr.write("--conf 超过 4096 B\n")
            return None
        files[CONF_PATH] = fb
        print("    %s = %d B（sha256=%s）" % (CONF_PATH, len(fb), sha(fb)[:16]))

    if args.on:
        files[ON_PATH] = b"1\n"
        print("    %s = 2 B（**触发开关**：内核启动期跑 /bin/browser）" % ON_PATH)
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
        print("    回读 %-22s %8d B  宿主 sha256=%s  卷内 sha256=%s"
              % (path, len(want), sha(want)[:12], sha(got)[:12]))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷镜像")
    ap.add_argument("--fixture-img", default=None, help="夹具盘输出（整盘：system.img + 卷）")
    ap.add_argument("--from-vol", default=None, help="夹具盘的输入卷（整卷字节）")
    ap.add_argument("--browser", required=True, help="build64/browser.elf")
    ap.add_argument("--conf", default=None, help="（夹具）-> /etc/browser.conf")
    ap.add_argument("--on", action="store_true", help="（夹具）写 /etc/browser.on 触发启动期运行")
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
        # system.img 末尾并着安装载荷（整块 sysdisk 的副本）；夹具只取引导区那一段
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
    print("    浏览器卷自检 OK：%d 个文件逐字节回读一致 -> %s" % (len(files), args.vol_out))

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
