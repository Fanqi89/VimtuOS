#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/demo_pack_win.py - ★ 本批：把内核里内嵌的演示程序 blob 搬到系统卷。

交付方式与 /bin/shell.bin、/bin/tcc、/bin/lua、/wlclient.elf、/bin/drvdemo 完全相同的一条纪律：
**内核二进制里搜不到这些字节**（构建期 64B 探针断言，见 build64.sh）。本工具一次做两件事，
可以分开调用（构建脚本里就是分两次调用：blob 一编完就出 raw/header，卷链末尾再装卷）：

  1) --raw / --header：把 18 份演示程序拼成"原始 blob 区" build64/demo64_raw.bin，并生成
     内核侧偏移表 build64/demo64_blobtab.h（**只有路径/偏移/长度，没有字节**）。
     原始区由 build64.sh 用 dd 写进 system.img 的 LBA 7497 起 —— loader 会把整个内核区
     （LBA 9..8008 = 8000 扇区 = 4MB）平铺加载到物理 0x100000（BIOS 的 int13/atapi 路径与
     两条 UEFI 路径都是这样），所以内核按**物理直映**就能读到它，不需要任何 ATA 代码。
     ★ 这是"夹具兜底"（任务书 option (b)）：只兜"卷里没有且是空夹具"这一情形 ——
       内核二进制里这些 blob 的字节是 0；正常交付路径是下面的系统卷文件。

  2) --vol-in / --vol-out：把这些文件写进 VimtuFS2 系统卷（**逐字节回读自检**，二级间接由
     tcc_pack_win.VolumeEdit 提供），给了 --system/--disk 时再拼一次完整演示盘 build64/sysdisk.img。
     这一步就是"搬到系统卷"：每个 blob 的卷内路径见下表（与 kernel 侧代码里的路径一致）。

用法：
    # ① blob 编完后（链接内核之前）：出原始区 + 内核偏移表
    py -3 tools/demo_pack_win.py --build build64 \\
          --raw build64/demo64_raw.bin --header build64/demo64_blobtab.h
    # ② 卷链最后一步：装进系统卷并重拼演示盘
    py -3 tools/demo_pack_win.py --build build64 \\
          --vol-in build64/drvsvcvol.img --vol-out build64/demovol.img \\
          --system build64/system.img --disk build64/sysdisk.img

退出码：0 = 成功（含自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SECTOR = 512

# ★ 原始区在内核区里的起始 LBA（内核区 = LBA 9..8008，见 memlayout64.h / loader64.asm）。
#   7497 是图标包搬走前的老位置；留 7488 个扇区（3,833,856 B）给内核二进制 —— 构建期有断言。
DEMO64_RAW_LBA = 7497
# 原始区在卷里的字节上限（LBA 7497..8008 = 512 扇区 = 262,144 B）。
DEMO64_RAW_MAX_BYTES = 512 * SECTOR

# (卷内路径, build64/ 下的文件名, vfs mode)
# 前 13 个是"内核启动期装进卷再从盘上跑"的那批；后 5 个是直接跑的 blob —— 它们的卷内路径是
# 交付留档（内核按"卷里没有就跳过"处理；见 kernel/kernel64.cpp 的说明）。
BLOBS = [
    ("/hello.elf",          "hello.elf",          0o755),
    ("/hello.vap",          "hello.vap",          0o755),
    ("/proc64.elf",         "proc64.elf",         0o755),
    ("/pipe64.elf",         "pipe64.elf",         0o755),
    ("/spin.elf",           "spin64.elf",         0o755),
    ("/filedemo.elf",       "filedemo64.elf",     0o755),
    ("/evshm.elf",          "evshm.elf",          0o755),
    ("/sig64.elf",          "sig64.elf",          0o755),
    ("/musl_hello.elf",     "musl_hello.elf",     0o755),
    ("/lib/ldvimtu.so",     "ldvimtu.so",         0o755),
    ("/lib/libfoo.so",      "libfoo.so",          0o755),
    ("/dynhello.elf",       "dynhello.elf",       0o755),
    ("/xmmsse.elf",         "xmmsse.elf",         0o755),
    ("/bin/demo64.bin",     "user_demo64.bin",    0o655),
    ("/bin/hello_c.bin",    "user_hello.bin",     0o655),
    ("/bin/libctest_c.bin", "user_libctest.bin",  0o655),
    ("/bin/fbdemo_c.bin",   "user_fbdemo.bin",    0o655),
    ("/bin/fbdemo64.bin",   "user_fbdemo64.bin",  0o655),
]


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def read_blobs(build_dir, want_paths=None):
    """读全部（或指定的）blob 文件；缺文件/空文件 -> 抛 SystemExit(2)。"""
    out = []
    for path, name, mode in BLOBS:
        if want_paths is not None and path not in want_paths:
            continue
        fp = os.path.join(build_dir, name)
        if not os.path.exists(fp):
            sys.stderr.write("缺少演示程序 blob：%s（先编出 %s）\n" % (fp, name))
            raise SystemExit(2)
        with open(fp, "rb") as f:
            d = f.read()
        if len(d) == 0:
            sys.stderr.write("演示程序 blob 是空文件：%s\n" % fp)
            raise SystemExit(2)
        out.append((path, name, mode, d))
    return out


def do_raw(blobs, raw_path, header_path):
    """拼原始区 + 生成内核偏移表；返回原始区长度（含对齐）。"""
    off = 0
    entries = []
    for path, _name, _mode, d in blobs:
        off = (off + 15) & ~15                      # 16 字节对齐（与内核侧的读法无关，只是整齐）
        entries.append((path, off, len(d)))
        off += len(d)
    if off > DEMO64_RAW_MAX_BYTES:
        sys.stderr.write("原始 blob 区 %d B 超过内核区里的预留上限 %d B\n"
                         % (off, DEMO64_RAW_MAX_BYTES))
        raise SystemExit(1)
    raw = bytearray(off)
    for (path, o, n), (_p, _name, _mode, d) in zip(entries, blobs):
        raw[o:o + n] = d
    with open(raw_path, "wb") as f:
        f.write(raw)

    lines = []
    lines.append("// demo64_blobtab.h - 构建期生成（tools/demo_pack_win.py），**请勿手改**")
    lines.append("//")
    lines.append("// 这是演示程序 blob 在\"原始区\"（system.img 的 LBA %d 起，loader 平铺加载）里的" % DEMO64_RAW_LBA)
    lines.append("// 路径 / 偏移 / 长度表 —— 只有元数据，**没有任何一个字节的 blob 本体**。")
    lines.append("// 系统卷里同名文件是正常交付路径；原始区只兜\"卷里没有且是空夹具\"（见 kernel/demo64.cpp）。")
    lines.append("#pragma once")
    lines.append("")
    lines.append("#define DEMO64_RAW_LBA   %d" % DEMO64_RAW_LBA)
    lines.append("#define DEMO64_BLOB_COUNT %d" % len(entries))
    lines.append("static const Demo64BlobEntry64 g_demo64_blobtab64[DEMO64_BLOB_COUNT] = {")
    for path, o, n in entries:
        lines.append("    { \"%s\", %du, %du }," % (path, o, n))
    lines.append("};")
    lines.append("")
    with open(header_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))

    print("    原始 blob 区：%d 份 / %d B -> %s（内核区 LBA %d 起；上限 %d B）"
          % (len(entries), len(raw), raw_path, DEMO64_RAW_LBA, DEMO64_RAW_MAX_BYTES))
    for path, o, n in entries:
        print("      %-24s -> %-22s off=%-7d size=%d" % (path, "", o, n))
    print("    内核偏移表：%s（只有路径/偏移/长度）" % header_path)
    return len(raw)


def do_vol(blobs, vol_in, vol_out, system, disk, target_sectors):
    if not os.path.exists(vol_in):
        sys.stderr.write("找不到输入卷：%s\n" % vol_in)
        raise SystemExit(2)
    img = open(vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % vol_in)
        raise SystemExit(2)
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = {}
    for path, _name, mode, d in blobs:
        vol.put(path, d, mode=mode)
        expect[path] = d
    out = vol.finish()
    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        raise SystemExit(1)
    with open(vol_out, "wb") as f:
        f.write(out)
    print("    演示程序装卷自检 OK：%d 份 -> %s（逐字节回读一致；卷 %d 扇区）"
          % (len(blobs), vol_out, total))
    for path, _name, _mode, d in blobs:
        print("      %-24s %8d B" % (path, len(d)))

    if disk:
        if not system:
            sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
            raise SystemExit(2)
        system_bytes = open(system, "rb").read()
        if len(system_bytes) > target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            raise SystemExit(2)
        disk_bytes = TP.SV.build_disk(system_bytes, out, target_sectors)
        with open(disk, "wb") as f:
            f.write(disk_bytes)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (disk, len(disk_bytes), target_sectors, TP.SV.PART_MAIN_LBA))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build64", help="blob 所在目录（默认 build64）")
    ap.add_argument("--raw", default=None, help="输出原始 blob 区（build64/demo64_raw.bin）")
    ap.add_argument("--header", default=None, help="输出内核偏移表（build64/demo64_blobtab.h）")
    ap.add_argument("--vol-in", default=None, help="输入卷（卷链上一步的产物）")
    ap.add_argument("--vol-out", default=None, help="输出卷（含这些演示程序）")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（build64/sysdisk.img）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not args.raw and not args.header and not args.vol_out:
        sys.stderr.write("至少要给 --raw/--header 或 --vol-out 之一\n")
        return 2
    if (args.raw or args.header) and not (args.raw and args.header):
        sys.stderr.write("--raw 与 --header 必须成对给（内核偏移表与原始区必须同一次生成）\n")
        return 2

    if args.vol_out:
        blobs = read_blobs(args.build)
        do_vol(blobs, args.vol_in, args.vol_out, args.system, args.disk, args.target_sectors)
    if args.raw:
        blobs = read_blobs(args.build)
        do_raw(blobs, args.raw, args.header)
    return 0


if __name__ == "__main__":
    sys.exit(main())
