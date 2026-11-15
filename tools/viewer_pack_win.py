#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/viewer_pack_win.py - ★ v0.10.5：把图片查看器与验收图册装进系统卷

落点：
    /bin/viewer                build64/viewer.elf（静态 ELF64；`run /bin/viewer <图>` 直接跑）
    /Pictures/redblue.png      128x96  1:1 打开（PNG 解码路径）
    /Pictures/redblue.jpg      128x96  JPEG 解码路径
    /Pictures/redblue.bmp      128x96  BMP 解码路径
    /Pictures/big.png          1600x1200  缩小到窗口内（mode=fit）

为什么这些图必须**进卷**（而不是测试时临时拷进去）：
    tests/viewer64_test.py 走的是"在文件管理器里双击 -> 路由表 -> /bin/viewer"这条真路径，
    双击的对象必须是卷上真实存在的文件。图册同时是**交付内容**：装上系统就能点开看图。

为什么插在卷链的这个位置：
    卷链是"卷进卷出"（... drvvol.img -> peinfovol.img -> payload_embed -> system.img）。
    本步用 peinfovol.img 当输入、再重拼一次 sysdisk.img，于是**后面所有步骤都不用改**
    （payload_embed_win.py 读的就是 sysdisk.img）。这是这条链上最不容易出错的插入点
    —— 与 tools/peinfo_pack_win.py 的做法完全一致。

用法：
    py -3 tools/viewer_pack_win.py --vol-in build64/peinfovol.img --vol-out build64/viewervol.img \\
          --viewer build64/viewer.elf --pics build64/pics \\
          [--system build64/system.img --disk build64/sysdisk.img]
退出码：0 = 成功（含逐字节回读自检）；1 = 自检失败；2 = 参数/输入问题。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512

VIEWER_VOL = "/bin/viewer"
PICS_VOL_DIR = "/Pictures"
# 图册清单（名字固定：测试按这些路径双击；缺一个就是真缺陷，不是 WARN）
PICS = ("redblue.png", "redblue.jpg", "redblue.bmp", "big.png")

# v0.10.5：**文件关联路由**的 5 个样本（扩展名各代表一条路由规则）。
# 为什么构建期就写进卷、而不是让测试现场造：ring3 外壳是 uid=1000，
# /Pictures 属主 root:root mode 0755 -> `echo > /Pictures/x` 会被
# `[PERM64] deny op=create … need=wx` 拒掉（实测）。夹具本来就该是卷内容。
SAMPLES = {
    "notes.txt": b"hello viewer route\n",     # ④ 文本 -> 只读预览
    "blob.bin":  b"\x00\x01\x02binary",      # ⑤ 二进制 -> 拒绝
    "song.mp3":  b"not a real mp3\n",          # ⑥ 未知 -> 拒绝
    "app.exe":   b"MZ not a real pe\n",        # ① 运行 -> peload
    "setup.deb": b"!<arch>\nnot real\n",      # ② 安装包 -> store
}


def _load_tcc_pack():
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（peinfo_pack_win.py 的 peinfovol.img）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--viewer", required=True, help="build64/viewer.elf -> /bin/viewer")
    ap.add_argument("--pics", default=os.path.join(ROOT, "build64", "pics"),
                    help="图册目录（tools/mk_pics_win.ps1 的产物）")
    ap.add_argument("--system", default=None, help="build64/system.img（重拼演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.viewer):
        sys.stderr.write("找不到 %s（先跑 bash user/viewer/build_viewer.sh）\n" % args.viewer)
        return 2
    vw = open(args.viewer, "rb").read()
    if vw[:4] != b"\x7fELF":
        sys.stderr.write("/bin/viewer 不是 ELF：%d B\n" % len(vw))
        return 2

    if not os.path.exists(args.vol_in):
        sys.stderr.write("找不到输入卷 %s\n" % args.vol_in)
        return 2
    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)

    expect = {}
    vol.put(VIEWER_VOL, vw, mode=0o755)
    expect[VIEWER_VOL] = vw

    # ---- 图册：四个都在才算过（不是"有多少装多少"）----
    missing = []
    for name in PICS:
        src = os.path.join(args.pics, name)
        if not os.path.exists(src):
            missing.append(name)
            continue
        data = open(src, "rb").read()
        vpath = PICS_VOL_DIR + "/" + name
        vol.put(vpath, data, mode=0o644)
        expect[vpath] = data
    if missing:
        sys.stderr.write("图册缺文件：%s（先跑 powershell -File tools/mk_pics_win.ps1）\n"
                         % ", ".join(missing))
        return 2

    # ---- 路由样本（5 个扩展名各代表一条规则，见上面 SAMPLES）----
    for name, blob in sorted(SAMPLES.items()):
        vpath = PICS_VOL_DIR + "/" + name
        vol.put(vpath, blob, mode=0o644)
        expect[vpath] = blob

    out = vol.finish()
    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    viewer 卷自检 OK：%d 个文件、%d B 逐字节回读一致"
          "（/bin/viewer = %d B；图册 %d 个 + 路由样本 %d 个）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()), len(vw), len(PICS),
             len(SAMPLES), args.vol_out))

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
