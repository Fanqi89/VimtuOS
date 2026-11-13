#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/peinfo_pack_win.py - ★ v0.9.43（W0）：把 PE 体检器与真 exe 语料装进系统卷

落点：
    /bin/peinfo            build64/peinfo（静态 ELF64，`run /bin/peinfo <file.exe>` 直接跑）
    /pexcorpus/tiny.exe    ① 极小 PE，**无导入**（验"导入表为空"分支）
    /pexcorpus/cui.exe     ② 控制台 exe（subsystem=3）+ CRT
    /pexcorpus/gui.exe     ③ 图形 exe（subsystem=2）+ MessageBoxA

为什么不把 exe 内嵌进内核：与 /bin/gzip、/bin/wm.elf 同一条纪律 —— 用户态程序只从**系统卷**
装载。tests/peinfo64_test.py 会在 VimtuOS 里跑 `/bin/peinfo /pexcorpus/gui.exe`，
再与宿主 tools/pe_crosscheck.py 的解析**逐字段比对**。

为什么语料是"可选"的：exe 由 mingw-w64 现场编（tools/pex_corpus_build.py）。没装 mingw 的
机器上构建**不该失败** —— 那就只装 /bin/peinfo，语料缺失只打 WARN（测试会把这几条记 SKIP）。

用法：
    py -3 tools/peinfo_pack_win.py --vol-in build64/drvvol.img --vol-out build64/peinfovol.img \\
          --peinfo build64/peinfo [--corpus build64/pexcorpus] \\
          [--system build64/system.img --disk build64/sysdisk.img]
退出码：0 = 成功（含逐字节回读自检）；1 = 自检失败；2 = 参数/输入问题。
"""
import argparse
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（drivers_pack_win.py 的 drvvol.img）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--peinfo", required=True, help="build64/peinfo -> /bin/peinfo")
    ap.add_argument("--corpus", default=None, help="build64/pexcorpus（缺省则按 --peinfo 推）")
    ap.add_argument("--system", default=None, help="build64/system.img（重拼演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.peinfo):
        sys.stderr.write("找不到 %s（先跑 tools/peinfo_build_win.sh）\n" % args.peinfo)
        return 2
    pi = open(args.peinfo, "rb").read()
    if pi[:4] != b"\x7fELF":
        sys.stderr.write("/bin/peinfo 不是 ELF：%d B\n" % len(pi))
        return 2

    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)

    expect = {}
    vol.put("/bin/peinfo", pi, mode=0o755)
    expect["/bin/peinfo"] = pi

    # ---- 真 exe 语料（可选）----
    corpus = args.corpus or os.path.join(os.path.dirname(args.peinfo), "pexcorpus")
    man_path = os.path.join(corpus, "PEX.json")
    n_exe = 0
    if os.path.exists(man_path):
        with open(man_path, encoding="utf-8") as f:
            manifest = json.load(f)
        for rec in manifest:
            src = rec["src"]
            if not os.path.exists(src):
                sys.stderr.write("WARN: PEX.json 里列了 %s 但文件不在，跳过\n" % src)
                continue
            data = open(src, "rb").read()
            if data[:2] != b"MZ":
                sys.stderr.write("WARN: %s 不是 PE（缺 MZ），跳过\n" % src)
                continue
            vol.put(rec["vol"], data, mode=0o644)
            expect[rec["vol"]] = data
            n_exe += 1
    else:
        sys.stderr.write("WARN: 没有 %s —— 只装 /bin/peinfo，不带 exe 语料"
                         "（没装 mingw-w64？跑 py -3 tools/pex_corpus_build.py）\n" % man_path)

    out = vol.finish()
    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    peinfo 卷自检 OK：%d 个文件、%d B 逐字节回读一致（/bin/peinfo = %d B；exe 语料 %d 个）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()), len(pi), n_exe, args.vol_out))

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
