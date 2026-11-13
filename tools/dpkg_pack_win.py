#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/dpkg_pack_win.py - ★ v0.9.42（L1）：把 **dpkg/apt 前端** 装进系统卷 + 造一个自包含测试 .deb

★ L1 变了什么：本脚本原来装的是 build64/dpkg（user/dpkg/dpkg.c 编出来的**第二套** .deb 引擎）。
  v0.9.42 起那套已废弃、不再编译 —— 现在装的是 **build64/store/vpkg.elf 这同一份字节**，
  由 user/store/vpkg.c 按 argv[0] 的 basename 决定前端（busybox 模式）：
    basename == "vpkg" -> vpkg 前端（原行为，一个字节都没变）
    basename == "dpkg" -> dpkg 前端（-i / -l / -r / -s / --version，文案与旧 mini-dpkg 逐字节一致）
    basename == "apt"  -> apt  前端（install ./X.deb / update / list / remove）
  于是全系统只有**一份** ar/tar/gzip/xz/依赖/文件清单实现（user/store/vs_*.c）。

落点：
  /usr/bin/dpkg        build64/store/vpkg.elf 的字节（v0.7.5 起 /bin 由内核映射到 /usr/bin）
  /usr/bin/apt         同一份字节（argv[0] 的 basename 是 "apt" 时走 apt 前端）
  /usr/share/dpkg/test-vimtu-1.0.deb
                       构建期现场生成的**真 .deb**（ar + control.tar.gz + data.tar.gz），
                       内容是 /usr/share/vimtu-dpkg-test/hello.txt —— 用来验收 `dpkg -i` 真能装。
用法：
  py -3 tools/dpkg_pack_win.py --vol-in build64/iconvol.img --vol-out build64/dpkgvol.img \
        --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import gzip
import io
import os
import sys
import tarfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import tcc_pack_win as TP  # noqa: E402

PART_MAIN_LBA = TP.SV.PART_MAIN_LBA
CTRL_PATH = "/usr/share/vimtu-dpkg-test/hello.txt"


def ar_member(name, data):
    """ar 成员头（60 B）+ 数据 + 2 字节对齐。"""
    h = ("%-16s%-12s%-6s%-6s%-8s%-10s`\n" % (name, int(time.time()), 0, 0, 100644, len(data))).encode("ascii")
    assert len(h) == 60, len(h)
    out = h + data
    if len(data) & 1:
        out += b"\n"
    return out


def image_top(exe):
    """ELF64：所有 PT_LOAD 的 (p_vaddr + p_memsz) 按页对齐后取最大值。

    为什么看 memsz 而不是文件大小：.bss 不进文件，但它占目标地址空间
    —— v0.7.5 就是 .bss 76 KiB 越过了 execve 的 64 KiB 主程序窗口，
    装载器报 `overlaps-user-region`，dpkg/apt 根本起不来。
    """
    if len(exe) < 64:
        return 0
    phoff = int.from_bytes(exe[0x20:0x28], "little")
    phentsize = int.from_bytes(exe[0x36:0x38], "little")
    phnum = int.from_bytes(exe[0x38:0x3A], "little")
    top = 0
    for i in range(phnum):
        o = phoff + i * phentsize
        if o + 56 > len(exe):
            break
        if int.from_bytes(exe[o:o + 4], "little") != 1:      # PT_LOAD
            continue
        vaddr = int.from_bytes(exe[o + 16:o + 24], "little")
        memsz = int.from_bytes(exe[o + 40:o + 48], "little")
        if memsz == 0:
            continue
        top = max(top, (vaddr + memsz + 0xFFF) & ~0xFFF)
    return top


def make_deb():
    """造一个能被 mini-dpkg 安装的真 .deb（control.tar.gz + data.tar.gz）。"""
    control = (
        "Package: vimtu-dpkg-test\n"
        "Version: 1.0\n"
        "Architecture: all\n"
        "Maintainer: VimtuOS <dev@vimtu.local>\n"
        "Section: misc\n"
        "Priority: optional\n"
        "Description: dpkg install self-test package\n"
    ).encode()
    body = b"hello from a real .deb installed by vimtu-dpkg\n"

    def tgz(entries):
        raw = io.BytesIO()
        with tarfile.open(fileobj=raw, mode="w") as t:
            for name, data, mode in entries:
                ti = tarfile.TarInfo(name)
                ti.size = len(data)
                ti.mode = mode
                ti.mtime = 1700000000
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = "root"
                t.addfile(ti, io.BytesIO(data))
        gz = io.BytesIO()
        with gzip.GzipFile(fileobj=gz, mode="wb", mtime=0) as g:
            g.write(raw.getvalue())
        return gz.getvalue()

    ctl = tgz([("./control", control, 0o644)])
    data = tgz([("./" + CTRL_PATH.lstrip("/"), body, 0o644)])

    out = b"!<arch>\n"
    out += ar_member("debian-binary", b"2.0\n")
    out += ar_member("control.tar.gz", ctl)
    out += ar_member("data.tar.gz", data)
    return out, body


def main():
    ap = argparse.ArgumentParser()
    # ★ v0.9.42（L1）：默认源从 build64/dpkg（已废弃的独立引擎）改成 build64/store/vpkg.elf。
    #   参数名保持 --dpkg（构建脚本与文档里都这么叫），但装进去的**是 vpkg 的字节**。
    ap.add_argument("--dpkg", default=os.path.join(ROOT, "build64", "store", "vpkg.elf"))
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.dpkg):
        sys.stderr.write("缺 %s（先跑 user/store/build_store.sh 编出 /bin/vpkg）\n" % args.dpkg)
        return 2
    exe = open(args.dpkg, "rb").read()
    if exe[:4] != b"\x7fELF":
        sys.stderr.write("%s 不是 ELF\n" % args.dpkg)
        return 2
    # ★ v0.9.42（P0.1 之后）：主程序映像的上界不再是"64 KiB 窗口"。
    #   旧门禁写的是 0x100010000（= 4 GiB + 64 KiB，v0.4 时代的窗口顶），而
    #   kernel/usermode64.h 的 USER64_LOADER_BIG_TOP_VA64 现在是
    #   **4 GiB + 998 MiB**（= 窗口 1008 MiB − 8 MiB SHM − 2 MiB DEV）。用它才是真约束：
    #   映像越过它，内核会 `[ELF64] exec reject reason=outside-user-window`。
    #   为什么必须放宽：/bin/vpkg 现在同时要当 dpkg/apt 前端，映像从 4 GiB+60 KiB 涨到
    #   64 KiB 上下 —— 旧的 64 KiB 门禁会误报，而**产品侧本来是允许的**。
    top = image_top(exe)
    limit = 0x100000000 + 998 * 1024 * 1024
    print("    映像最高地址 = 0x%X（PT_LOAD vaddr+memsz 页对齐）；窗口上界 = 0x%X，余量 %d B"
          % (top, limit, limit - top))
    if top > limit:
        sys.stderr.write("ERROR: dpkg/apt 前端映像越过主程序装载区顶（0x%X > 0x%X），"
                         "execve 会拒载（outside-user-window）\n" % (top, limit))
        return 1
    deb, body = make_deb()
    files = {
        "/usr/bin/dpkg": (exe, 0o755),
        "/usr/bin/apt": (exe, 0o755),                     # 同一份字节：argv[0] 决定前端
        # ★ v0.7.6：名字仅用 `-` / `.`。QEMU sendkey 没有下划线，实测 `test-vimtu_1.0.deb`
    #   在终端里会变成 `test-vimtu1.0.deb`（下划线被吞）-> dpkg -i 找不到文件。
    "/usr/share/dpkg/test-vimtu-1.0.deb": (deb, 0o644),
    }
    print("    vpkg.elf=%d B（同一份字节装成 /usr/bin/dpkg 与 /usr/bin/apt，argv[0] 决定前端）；"
          "测试 .deb=%d B（内容 %d B 落到 %s）"
          % (len(exe), len(deb), len(body), CTRL_PATH))

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
    print("    dpkg 卷自检 OK：%d 个文件逐字节回读一致（/usr/bin/dpkg、/usr/bin/apt、测试 .deb）" % len(files))

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    dpkg 卷：%s（%d B）" % (args.vol_out, len(out)))
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
