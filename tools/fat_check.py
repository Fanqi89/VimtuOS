#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/fat_check.py - 按 FAT 规范体检一个 FAT16 卷（默认 build64/esp.img）

为什么需要：自己写的 FAT 写入器 + 自己写的读取器"逐字节一致"**不能**证明卷是对的
——两者可能犯同一个错。固件（EDK2）的 FAT 驱动更严格：它读文件时会走簇链，
链里出现非法值就直接返回 EFI_VOLUME_CORRUPTED。本工具按规范逐项核对：
BPB 字段自洽性、FAT[0]/FAT[1] 保留项、每个文件的簇链（是否以 0xFFFF 结尾、
簇数×簇大小是否 ≥ 目录项里声明的字节数）、以及数据区边界。

用法：python tools/fat_check.py [esp.img]
"""
import struct
import sys

EOF_MIN = 0xFFF8           # FAT16：>= 0xFFF8 视为链尾


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build64/esp.img"
    with open(path, "rb") as f:
        b = f.read()
    print("=" * 74)
    print("FAT 卷体检：%s (%d 字节 = %d 扇区)" % (path, len(b), len(b) // 512))
    print("=" * 74)

    # ---- BPB ----
    bytes_per_sec = u16(b, 11)
    sec_per_clus = b[13]
    rsvd = u16(b, 14)
    nfats = b[16]
    root_ent = u16(b, 17)
    tot16 = u16(b, 19)
    media = b[21]
    fatsz16 = u16(b, 22)
    tot32 = u32(b, 32)
    label = b[54:62].decode("latin-1").rstrip("\x00 ")
    fstype = b[82:90].decode("latin-1").rstrip("\x00 ") if bytes_per_sec else "?"

    print("BPB:")
    print("  BytsPerSec   = %d" % bytes_per_sec)
    print("  SecPerClus   = %d  -> 簇大小 %d 字节" % (sec_per_clus, bytes_per_sec * sec_per_clus))
    print("  RsvdSecCnt   = %d" % rsvd)
    print("  NumFATs      = %d" % nfats)
    print("  RootEntCnt    = %d  -> 根目录 %d 字节 = %d 扇区"
          % (root_ent, root_ent * 32, (root_ent * 32 + bytes_per_sec - 1) // bytes_per_sec))
    print("  TotSec16     = %d" % tot16)
    print("  TotSec32     = %d" % tot32)
    print("  Media        = 0x%02X   FATSz16 = %d" % (media, fatsz16))
    print("  卷标/类型     = %r / %r" % (label, fstype))

    tot = tot16 or tot32
    cluster_bytes = bytes_per_sec * sec_per_clus
    root_bytes = root_ent * 32
    root_sec = (root_bytes + bytes_per_sec - 1) // bytes_per_sec
    data_start_sec = rsvd + nfats * fatsz16 + root_sec
    nclusters = (tot - data_start_sec) // sec_per_clus
    print("  --- 推导 ---")
    print("  数据区起始扇区 = %d   数据区簇数 = %d   卷总扇区 = %d" % (data_start_sec, nclusters, tot))
    ok = []
    if tot != len(b) // 512:
        ok.append("★ TotSec(%d) 与实际容量(%d 扇区) 不一致" % (tot, len(b) // 512))
    if nclusters > 0xFFEF:
        ok.append("★ 簇数 %d 超出 FAT16 上限 0xFFEF" % nclusters)
    if nclusters < 4085:
        ok.append("注意：簇数 %d < 4085，按规范这应是 FAT12 而不是 FAT16（部分驱动会挑）" % nclusters)

    # ---- FAT ----
    fat_off = rsvd * bytes_per_sec
    fat0, fat1 = u16(b, fat_off), u16(b, fat_off + 2)
    print("FAT[0] = 0x%04X (期望 0xFF00|media = 0xFF%02X 或 0xFFF8)" % (fat0, media))
    fat_type_derived = "FAT12" if nclusters < 4085 else ("FAT16" if nclusters < 65525 else "FAT32")
    print("  按簇数推导的类型 = %s（BPB 自称 %r）" % (fat_type_derived, fstype))
    if "FAT16" in fstype.upper() and nclusters < 4085:
        ok.append("★ 致命：BPB 自称 FAT16，但簇数 %d < 4085 属 FAT12 区间 —— 遵循规范的驱动"
                  "（EDK2）会按 12 位读 FAT 表，多簇文件读取报 EFI_VOLUME_CORRUPTED" % nclusters)
    elif nclusters < 4085:
        ok.append("注意：簇数 %d < 4085，按规范应视为 FAT12" % nclusters)
    print("FAT[1] = 0x%04X (期望 0xFFFF)" % fat1)
    if (fat0 & 0xFF00) != 0xFF00:
        ok.append("★ FAT[0] 高位不对（应为 0xFF%02X）" % media)
    if fat1 != 0xFFFF:
        ok.append("★ FAT[1] 不是 0xFFFF")
    if nfats >= 2:
        fat2_off = fat_off + fatsz16 * bytes_per_sec
        same = b[fat_off:fat_off + fatsz16 * bytes_per_sec] == b[fat2_off:fat2_off + fatsz16 * bytes_per_sec]
        print("  两个 FAT 副本一致：%s" % ("是" if same else "★ 否"))

    def fat_entry(cl):
        return u16(b, fat_off + cl * 2)

    def chain(cl):
        out, guard = [], 0
        while cl >= 2 and guard < 100000:
            out.append(cl)
            nxt = fat_entry(cl)
            if nxt >= EOF_MIN:
                return out, True
            if nxt == 0:
                return out, False          # 空闲 = 链断
            if nxt == 1 or nxt < 2:
                return out, False
            cl = nxt
            guard += 1
        return out, False

    # ---- 根目录 ----
    root_off = (rsvd + nfats * fatsz16) * bytes_per_sec
    print("根目录（%d 项）：" % root_ent)
    nfiles = 0
    for i in range(root_ent):
        o = root_off + i * 32
        if b[o] == 0x00:
            break
        if b[o] == 0xE5:
            continue
        attr = b[o + 11]
        if attr == 0x0F:
            continue
        name = b[o:o + 11].decode("latin-1").rstrip()
        cl = u16(b, o + 26)
        size = u32(b, o + 28)
        is_dir = bool(attr & 0x10)
        kind = "目录" if is_dir else "文件"
        nfiles += 1
        print("  [%2d] %-13s %s attr=0x%02X 起始簇=%d 声明大小=%d" % (i, name, kind, attr, cl, size))
        if size == 0 and not is_dir:
            continue
        ch, terminated = chain(cl)
        chain_bytes = len(ch) * cluster_bytes
        need_clus = (size + cluster_bytes - 1) // cluster_bytes if size else 0
        print("        簇链：%d 个簇 -> %d 字节；%s" % (
            len(ch), chain_bytes, "以 0xFFFF 正常结尾" if terminated else "★ 链未正常结尾（非 0xFFFF）"))
        print("        需要 %d 个簇，链长 %s" % (need_clus, "够" if len(ch) >= need_clus else "★ 不够"))
        if not terminated:
            ok.append("★ %s 的簇链没有 0xFFFF 结尾（固件读文件会判 EFI_VOLUME_CORRUPTED）" % name)
        if len(ch) < need_clus:
            ok.append("★ %s 簇链长度不足：需要 %d 簇，实际 %d" % (name, need_clus, len(ch)))
        if len(ch) > need_clus + 1:
            ok.append("注意：%s 簇链 %d 簇远多于所需 %d" % (name, len(ch), need_clus))
        if cl + len(ch) - 1 > nclusters + 1:
            ok.append("★ %s 的簇号越界（数据区只有 %d 簇）" % (name, nclusters))
        # 抽查首尾簇是否落在卷内
        last = ch[-1]
        end_off = (data_start_sec + (last - 2) * sec_per_clus) * bytes_per_sec + cluster_bytes
        if end_off > len(b):
            ok.append("★ %s 末簇 %d 的结束偏移 %d 超出卷大小 %d" % (name, last, end_off, len(b)))

    print("  --- 结论 ---")
    if ok:
        for m in ok:
            print("    %s" % m)
    else:
        print("    没发现明显问题")


if __name__ == "__main__":
    main()
