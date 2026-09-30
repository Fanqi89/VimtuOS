#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/gzip_pack_win.py - ★ A4-4c：把 **gzip/gunzip** 与 1 MiB 试验文件装进系统卷，并可
顺手把整块演示盘拼出来。

三步走里的最后一步（见 tools/lua_pack_win.py 的说明）：
  基础卷（shell + tcc） -> lua_pack 加 Lua -> **本脚本**加：
     /bin/gzip   用户态 gzip（fixed-Huffman deflate + 完整 inflate；静态 ELF64，< 64 KiB）
     /bin/gunzip 同一份字节（程序按 argv[0] 含 "gunzip" 决定默认方向，busybox 风格）
     /tcc/demo/big1m.txt   **正好 1 MiB** 的可压缩文本（gzip 往返/互操作测试用；内容由
                           本脚本的 make_big_text() 确定性生成，测试可以重新生成同一份逐字节比对）
  给了 --system/--disk 时，再用 make_shellvol.build_disk 拼出完整演示盘（与 tcc 那一步同口径）。

用法：
    py -3 tools/gzip_pack_win.py --vol-in build64/luavol.img --vol-out build64/sysvol.img \
          --gzip build64/gzip [--system build64/system.img --disk build64/sysdisk.img]
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512
BIG_BYTES = 1 << 20            # 正好 1 MiB


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def make_big_text(n=BIG_BYTES):
    """确定性生成 n 字节的可压缩文本（LF 行尾）。

    为什么要"确定性"：tests/gzip64_test.py 会用**同一个函数**重新生成一份，把 ring3 里
    gzip/gunzip 的结果与它逐字节比对 —— 内容不能有任何随机源。
    """
    out = bytearray()
    i = 0
    while len(out) < n:
        line = ("line %06d: vimtuos gzip roundtrip test ABCDEFGHIJKLMNOPQRSTUVWXYZ "
                "0123456789 the quick brown fox jumps over the lazy dog\n") % i
        out += line.encode("ascii")
        i += 1
    return bytes(out[:n])


def pack_into(vol, gz):
    """把 gzip 三件套装进一个 VolumeEdit；返回 {卷内路径: 字节}。"""
    expect = {}
    for name in ("gzip", "gunzip"):
        vol.put("/bin/" + name, gz, mode=0o755)
        expect["/bin/" + name] = gz
    big = make_big_text()
    vol.put("/tcc/demo/big1m.txt", big, mode=0o644)
    expect["/tcc/demo/big1m.txt"] = big
    return expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（VimtuFS2 v4，lua_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--gzip", required=True, help="build64/gzip -> /bin/gzip 与 /bin/gunzip")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.gzip):
        sys.stderr.write("找不到 build64/gzip：%s\n" % args.gzip)
        return 2
    gz = open(args.gzip, "rb").read()
    if gz[:4] != b"\x7fELF":
        sys.stderr.write("/bin/gzip 不是 ELF：%d B\n" % len(gz))
        return 2
    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = pack_into(vol, gz)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    gzip 卷自检 OK：%d 个文件、%d B 逐字节回读一致（/bin/gzip = /bin/gunzip = %d B、"
          "/tcc/demo/big1m.txt = %d B）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()), len(gz), BIG_BYTES, args.vol_out))

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
