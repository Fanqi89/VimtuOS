#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/assets_pack_win.py - ★ 本轮：把内核里的 raw 图标/logo 搬进 **系统卷**（腾内核预算）

背景（为什么要有这一步）：
  内核里曾经内嵌着 5 份 raw RGBA 资源，合计 356,992 B：
    logo_rgba.bin        144,000 B（240x150，开机/关机画面）
    icon_mycomputer.bin   65,536 B（128x128，桌面/ Dock 的"我的电脑"位图）
    icon_recyclebin.bin   65,536 B（128x128，回收站）
    icon_terminal.bin     65,536 B（128x128，终端）
    icon_start.bin        16,384 B（64x64，开始按钮的兜底图 = logo/kaisi.png 的 RGBA）
  它们把系统内核挤到 3,726,496 B（内核区硬上限 4,096,000 B，余量只剩 369,504 B）。
  本脚本照 /bin/shell.bin、/lib/tcc.bin、/etc/iconpack.bin 的既有模式，把这 5 份字节
  **构建期写进系统卷**；内核侧改为运行期从卷里读（kernel/gui64.cpp 的取用点 +
  kernel/img64.cpp 的 img64_load_asset64），读不到就回落内置/程序化绘制并打点。

卷内路径（全部落在**已存在**的 /etc 里 —— 为什么不建根级新目录：见 kernel/icons64.h
的 ★ 段，新建根级目录会让卷根条目数 +1，把 fs_term_test 的"rm 之后 entries 恰好 -1"
与 fd64 的目录缓存上限顶出边界，实测踩过）：
    /etc/logo.bin          144,000 B  <- build/logo_rgba.bin        （逐字节原样搬）
    /etc/icon_mypc.bin      65,536 B  <- build/icon_mycomputer.bin
    /etc/icon_recycle.bin   65,536 B  <- build/icon_recyclebin.bin
    /etc/icon_term.bin      65,536 B  <- build/icon_terminal.bin
    /etc/icon_start.bin     16,384 B  <- build/icon_start.bin

★ 二级间接（VimtuFS2 v4 的 dind）：/etc/logo.bin 是 282 块 > 132 块（4 直接 + 128 一级间接），
  必须走 dind —— tcc_pack_win.py 的 Volume2 已经实现了这一层（v4 inode 偏移 71），本脚本
  直接复用它，并用 tcc_pack_win.verify 把 5 个文件**逐字节**回读比对（直接块/ ind / dind 三级）。

用法（build64.sh 的第 5 步，接在 edit_pack_win.py 之后 —— 前 4 步的产物一个字节不动）：
    py -3 tools/assets_pack_win.py --vol-in build64/editvol.img --vol-out build64/assetsvol.img \\
          --res build --system build64/system.img --disk build64/sysdisk.img
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512
KIND_BIN = 6                      # = kernel/vfs64.h 的 VFS64_KIND_BIN

# (卷内路径, 资源文件名, 期望字节数, 尺寸说明) —— 期望字节数是**钉死的**：资源脚本改了尺寸
# 而这里没同步就必须当场失败（内核侧按同一条长度断言读取，两边不会悄悄错位）。
ASSETS = [
    ("/etc/logo.bin",        "logo_rgba.bin",       144000, "240x150 RGBA（logo/logo.png）"),
    ("/etc/icon_mypc.bin",   "icon_mycomputer.bin",  65536, "128x128 RGBA"),
    ("/etc/icon_recycle.bin", "icon_recyclebin.bin", 65536, "128x128 RGBA"),
    ("/etc/icon_term.bin",   "icon_terminal.bin",    65536, "128x128 RGBA"),
    ("/etc/icon_start.bin",  "icon_start.bin",       16384, "64x64 RGBA（logo/kaisi.png）"),
]


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def read_assets(res_dir):
    """读 build/ 下的 5 份资源；字节数与 ASSETS 钉死的不一致就报错（不静默）。"""
    out = {}
    for vpath, name, want, desc in ASSETS:
        src = os.path.join(res_dir, name)
        if not os.path.exists(src):
            raise IOError("找不到资源 %s（先跑 build64.sh 里的 _make_logo.py / _make_icons.py / "
                          "_make_start_icon.py）" % src)
        data = open(src, "rb").read()
        if len(data) != want:
            raise IOError("资源 %s = %d B，与钉死的 %d B 不符（%s）" % (src, len(data), want, desc))
        out[vpath] = data
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（VimtuFS2 v4，edit_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--res", default=os.path.join(ROOT, "build"), help="资源目录（默认 build/）")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    try:
        expect = read_assets(args.res)
    except IOError as e:
        sys.stderr.write("%s\n" % e)
        return 2

    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)

    for vpath, name, want, desc in ASSETS:
        ino = vol.put(vpath, expect[vpath], mode=0o644, kind=KIND_BIN)
        print("    %-22s <- %-20s %8d B（%s）ino=%d" % (vpath, name, want, desc, ino))

    out = vol.finish()
    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    外置资源卷自检 OK：%d 个文件 / %d B 逐字节回读一致（含 dind：/etc/logo.bin = %d 块 > 132）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()),
             (144000 + SECTOR - 1) // SECTOR, args.vol_out))

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
