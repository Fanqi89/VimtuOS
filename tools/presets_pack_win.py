# -*- coding: utf-8 -*-
"""tools/presets_pack_win.py —— 把**预设壁纸 / 预设头像**写进系统卷。

交付纪律（与 /bin/office、/bin/store 一致）：
  * 内核二进制里**一个字节都不加**；交付 = 系统卷里的两个目录
      `/Wallpapers/`（预设壁纸，含用户投放 res/wallpapers/*）
      `/Avatars/`（预设头像，代码画 6 张）
  * 写进卷后**逐字节回读自检**（复用 tcc_pack_win 的 verify），再拼出完整演示盘。

为什么单独一步（而不是塞进 tools/make_shellvol.py）：卷链上第一个 pack 步骤是**从原始区重建卷**的，
make_shellvol 产出的 /Wallpapers、/Avatars 在链上不会传下去（实测：shellvol.img 里有，
所有后续 *vol.img 都没有）。所以预设必须在**链尾**单独追加一次。

=============================================================================
★ v0.10.9【动态智能镜像 + 大图可用】—— 本文件的契约变更（旧契约错在哪，写清楚）
=============================================================================
旧契约（v0.10.8 及以前）：
  * 卷容量固定 24,759 扇区（12.09 MiB），装不下就**跳过**该文件并 WARN；
  * 边长 > 2048 的图**直接跳过**（因为内核 JPEG 解码上限 JPG64_MAXDIM = 2048）。
  → 后果：用户放一张 6.8 MB / 9686x5449 的壁纸，构建"爆"了（装不下）或被静默跳过，
    而且用户不知道为什么自己的图不见了。

新契约（v0.10.9）：
  1) 卷容量**没有上限**：make_shellvol.Volume 装不下就自动扩容（_grow），
     再由 make_shellvol.build_disk() 按真实内容收口（trim）—— "打进去多少文件，镜像就多大"。
     所以这里不再需要 free_bytes() 闸门。
  2) 超限原图**照样入库**（图片查看器能打开），同时**自动派生**一张 `-hd.jpg`
     （最长边 HD_MAXDIM = 1920，用 Pillow 的 draft 模式先降采样解码，避免 9686x5449
     直接解码吃掉 200+ MB 内存）。派生副本才是能设为桌面壁纸的那份。
     命名：`3.jpg` -> `3-hd.jpg`（用户自己已经有同名文件时不派生，只 WARN）。

用法（构建期；vol-in = 卷链上一步的产物）：
  py -3 tools/presets_pack_win.py --vol-in build64/storevol.img --vol-out build64/presetsvol.img \\
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

# 内核 kernel/img64.cpp 的 JPEG 解码上限（任一边超了就返回 -2，静默回落内置壁纸）
JPG_MAXDIM = 2048
# 派生副本的最长边：<= JPG_MAXDIM，且是 16:9 / 16:10 屏都合适的通用尺寸
HD_MAXDIM = 1920


def img_dims(b):
    """返回 (w, h)；PNG 读 IHDR，JPEG 扫 SOF0/SOF1/SOF2，BMP 读 BITMAPINFOHEADER，其它 None。"""
    import struct as _st
    if b[:8] == b"\x89PNG\r\n\x1a\n" and b[12:16] == b"IHDR":
        return _st.unpack_from(">II", b, 16)
    if b[:2] == b"BM" and len(b) >= 26:
        w, h = _st.unpack_from("<ii", b, 18)
        return (abs(w), abs(h))
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


def make_hd(data):
    """把一张大图降采样成最长边 HD_MAXDIM 的 JPEG 字节。

    ★ 内存友好：先 im.draft() 让 JPEG 解码器按 1/2·1/4·1/8 的 DCT 尺度解码。
      9686x5449 直接解码是 211 MB；draft 到 1/4 后只有 13 MB 量级。
    """
    import io as _io
    from PIL import Image
    im = Image.open(_io.BytesIO(data))
    w, h = im.size
    sc = float(HD_MAXDIM) / float(max(w, h))
    tw = max(1, int(round(w * sc)))
    th = max(1, int(round(h * sc)))
    # draft 的目标给大一点，保证 draft 出来的像素 >= 最终目标（否则会放大变糊）
    im.draft("RGB", (tw * 2, th * 2))
    im = im.convert("RGB").resize((tw, th), Image.LANCZOS)
    out = _io.BytesIO()
    im.save(out, "JPEG", quality=88, optimize=True)
    return out.getvalue(), (tw, th)


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
        print("    %-34s = %8d B" % (p, len(files[p][0])))

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
    print("    输入卷：%d 扇区（%.2f MiB），数据区空闲 %d 块"
          % (vol.total, vol.total * SECTOR / 1048576.0,
             (vol.total - vol.data_start) - len(vol._used_data_blocks())))

    # ==============================================================
    # ① 尺寸分类：<= JPG_MAXDIM 的原图直接可用；超限的原图照样入库，
    #    但额外派生一张 -hd.jpg 作为"能真正设为桌面壁纸"的那份。
    # ==============================================================
    derived = {}
    notes = []
    for path in sorted(files):
        if not path.startswith("/Wallpapers/"):
            continue
        data = files[path][0]
        dims = img_dims(data)
        name = path[len("/Wallpapers/"):]
        if not dims:
            notes.append(("noparse", name, len(data), None, None))
            continue
        if max(dims) <= JPG_MAXDIM:
            continue                                   # 原图就能当壁纸，不需要派生
        stem = os.path.splitext(name)[0]
        hd_name = stem + "-hd.jpg"
        hd_path = "/Wallpapers/" + hd_name
        if hd_path in files or hd_path in derived:
            notes.append(("conflict", name, len(data), dims, None))
            continue
        try:
            hd_bytes, hd_dims = make_hd(data)
        except Exception as e:                          # noqa: BLE001
            notes.append(("derive-fail", name, len(data), dims, str(e)))
            continue
        derived[hd_path] = (hd_bytes, 0o644)
        notes.append(("derived", name, len(data), dims, (hd_name, len(hd_bytes), hd_dims)))

    for kind, name, size, dims, extra in notes:
        if kind == "derived":
            hd_name, hd_size, hd_dims = extra
            print("    [壁纸] %-14s %dx%d %8d B -> 派生 %s %dx%d %d B（可设为桌面壁纸）"
                  % (name, dims[0], dims[1], size, hd_name, hd_dims[0], hd_dims[1], hd_size))
        elif kind == "noparse":
            sys.stderr.write("    [WARN] %s 不是可解析的 PNG/JPEG（%d B）：仍写进卷，"
                             "但内核解不出尺寸，不能设为壁纸\n" % (name, size))
        elif kind == "conflict":
            sys.stderr.write("    [WARN] %s 已有同名派生文件 %s-hd.jpg，跳过派生\n"
                             % (name, os.path.splitext(name)[0]))
        else:
            sys.stderr.write("    [WARN] %s 派生 -hd.jpg 失败（%s）：原图仍写进卷\n"
                             % (name, extra))
    files.update(derived)

    # ==============================================================
    # ② 全部写进卷（v0.10.9：没有容量闸门，卷会自动长大）
    # ==============================================================
    written = {}
    for path in sorted(files):
        vol.put(path, files[path][0], mode=files[path][1])
        written[path] = files[path][0]
    out = vol.finish()
    print("    写入完成后卷 %d 扇区（%.2f MiB），%d 个文件"
          % (vol.total, vol.total * SECTOR / 1048576.0, len(written)))

    bad = TP.verify(out, written)
    if bad:
        sys.stderr.write("卷回读自检失败：%s\n" % ", ".join(str(x) for x in bad[:5]))
        return 1
    print("    预设卷自检 OK：%d 个文件逐字节回读一致（/Wallpapers %d 张，其中派生 %d 张 / "
          "/Avatars %d 张）"
          % (len(written), len([p for p in written if p.startswith("/Wallpapers/")]),
             len(derived), len([p for p in written if p.startswith("/Avatars/")])))

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
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起，卷 %d 扇区）"
              % (args.disk, len(disk), len(disk) // SECTOR, PART_MAIN_LBA,
                 TP.SV.build_disk.last.get("vol", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
