# -*- coding: utf-8 -*-
"""tools/presets_pack_win.py —— v0.5.8：把**预设壁纸 / 预设头像**写进系统卷。

交付纪律（与 /bin/office、/bin/store 一致）：
  * 内核二进制里**一个字节都不加**；交付 = 系统卷里的两个目录
      `/Wallpapers/`（预设壁纸，含用户投放 res/wallpapers/*）
      `/Avatars/`（预设头像，代码画 6 张）
  * 写进卷后**逐字节回读自检**（复用 tcc_pack_win 的 verify），再拼出完整演示盘。

为什么单独一步（而不是塞进 tools/make_shellvol.py）：卷链上第一个 pack 步骤是**从原始区重建卷**的，
make_shellvol 产出的 /Wallpapers、/Avatars 在链上不会传下去（实测：shellvol.img 里有，
所有后续 *vol.img 都没有）。所以预设必须在**链尾**单独追加一次。

用法（构建期；vol-in = 卷链上一步的产物）：
  py -3 tools/presets_pack_win.py --vol-in build64/officevol.img --vol-out build64/presetsvol.img \
      --system build64/system.img --disk build64/sysdisk.img
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PART_MAIN_LBA = 32584
SECTOR = 512


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
MK = load_mod("make_shellvol", "make_shellvol.py")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", default=None)
    ap.add_argument("--vol-out", default=None)
    ap.add_argument("--system", default=None)
    ap.add_argument("--disk", default=None)
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    files = {}
    for n, d in MK.preset_wallpapers():
        files["/Wallpapers/" + n] = (d, 0o644)
    for n, d in MK.preset_avatars():
        files["/Avatars/" + n] = (d, 0o644)
    if not files:
        sys.stderr.write("没有任何预设（先跑 tools/make_presets.py）\n")
        return 2
    walls = [p for p in files if p.startswith("/Wallpapers/")]
    avas = [p for p in files if p.startswith("/Avatars/")]
    if len(walls) < 1 or len(avas) < 1:
        sys.stderr.write("预设不完整：壁纸 %d 张 / 头像 %d 张\n" % (len(walls), len(avas)))
        return 2
    for p in sorted(files):
        print("    %-30s = %7d B" % (p, len(files[p][0])))

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

    # ★ 先算空间：装不下的单个文件跳过并【大声告警】，绝不让一个 6.8 MB 的壁纸
    #   把整条构建链打断（res/wallpapers 是用户投放目录，内容不受版本控制）。
    #   注意：必须在 put **之前**判断 —— put 撞墙时已经把块占掉一半，后面小文件会被饿死。
    def free_bytes():
        n = 0
        for blk in range(vol.data_start, vol.total):
            if blk not in vol.used:
                n += 1
        return n * SECTOR

    # ★ 尺寸守卫：kernel/img64.cpp 的 JPEG 解码上限是 JPG64_MAXDIM = 2048（任一边超了就返回 -2），
    #   壁纸超限时内核只会**静默回落内置壁纸** —— 用户选了没反应，是最难查的一类"假正常"。
    #   所以装卷阶段就挡掉，并在构建日志里大声说清楚。
    JPG_MAXDIM = 2048

    def img_dims(b):
        """返回 (w, h)；PNG 读 IHDR，JPEG 扫 SOF0/SOF1/SOF2，其它返回 None。"""
        import struct as _st
        if b[:8] == b"\x89PNG\r\n\x1a\n" and b[12:16] == b"IHDR":
            return _st.unpack_from(">II", b, 16)
        if b[:2] == b"\xff\xd8":
            i = 2
            while i < len(b) - 3:
                if b[i] != 0xFF:
                    i += 1
                    continue
                m = b[i + 1]
                if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
                    i += 2
                    continue
                ln = _st.unpack_from(">H", b, i + 2)[0]
                if m in (0xC0, 0xC1, 0xC2, 0xC3):
                    h, w = _st.unpack_from(">HH", b, i + 5)
                    return (w, h)
                i += 2 + ln
        return None

    written = {}
    skipped = []
    for path in sorted(files):
        data = files[path][0]
        if path.startswith("/Wallpapers/"):
            dims = img_dims(data)
            if not dims:
                skipped.append((path, len(data), -1))
                continue
            if dims[0] > JPG_MAXDIM or dims[1] > JPG_MAXDIM:
                skipped.append((path, len(data), -2))
                continue
        # 余量：间接块（每 2048 B 一个）+ 目录项/元数据，统一留 64 KiB
        need = len(data) + 65536
        if free_bytes() < need:
            skipped.append((path, len(data), free_bytes()))
            continue
        vol.put(path, data, mode=files[path][1])
        written[path] = data
    out = vol.finish()
    if skipped:
        sys.stderr.write("    [WARN] 系统卷放不下这 %d 个预设文件，已跳过（用户投放目录太大）：\n"
                         % len(skipped))
        for p, sz, fr in skipped:
            if fr == -1:
                sys.stderr.write("    [WARN]   跳过 %-30s = %8d B（不是 PNG/JPEG，内核解不出来）\n"
                                 % (p, sz))
            elif fr == -2:
                sys.stderr.write("    [WARN]   跳过 %-30s = %8d B（尺寸超过内核 JPEG 解码上限 %d）\n"
                                 % (p, sz, JPG_MAXDIM))
            else:
                sys.stderr.write("    [WARN]   跳过 %-30s = %8d B（卷空间不足，跳过时剩余 %d B）\n"
                                 % (p, sz, fr))
        sys.stderr.write("    [WARN] 想全装进去：把 res/wallpapers/ 里的大图删掉/换小图，"
                         "或加大卷容量（make_shellvol.DEFAULT_TARGET_SECTORS）\n")
    bad = TP.verify(out, written)
    if bad:
        sys.stderr.write("卷回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    预设卷自检 OK：%d 个文件逐字节回读一致（/Wallpapers %d 张 / /Avatars %d 张，跳过 %d）"
          % (len(written), len([p for p in written if p.startswith("/Wallpapers/")]),
             len([p for p in written if p.startswith("/Avatars/")]), len(skipped)))

    if args.vol_out:
        with open(args.vol_out, "wb") as f:
            f.write(out)
        print("    预设卷：%s（%d B）" % (args.vol_out, len(out)))
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
