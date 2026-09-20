#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/fat_check.py - 按 FAT 规范体检一个 FAT12 / FAT16 / **FAT32** 卷

为什么需要：自己写的 FAT 写入器 + 自己写的读取器"逐字节一致"**不能**证明卷是对的
——两者可能犯同一个错。固件（EDK2）的 FAT 驱动更严格：它按**簇数**判定 FAT 类型
（< 4085 → FAT12，4085..65524 → FAT16，>= 65525 → FAT32），并用对应的位宽读 FAT 表。
链里出现非法值就直接返回 EFI_VOLUME_CORRUPTED。

本工具自动识别类型（BPB_FATSz16 != 0 且 RootEntCnt != 0 → FAT12/16；否则 FATSz32 → FAT32），
并按该类型的规范逐项核对：
    * 通用：BPB 字段自洽性、TotalSectors 与实际容量、数据区/簇数推导、两份 FAT 一致
    * FAT12/16：12/16 位 FAT 项、固定根目录区
    * FAT32：BPB_FATSz32、RootClus 根目录**簇链**、FSInfo（三个签名 + free/next-free 合理）、
      备份引导扇区（BkBootSec）逐字节等于 0 号扇区、32 位 FAT 项与 28 位掩码，
      以及 **簇数 >= 65525 的硬断言**（少于这个数就该按 FAT16 解析 -> 卷会被读坏）
    * 目录：每个目录的 "." / ".."、每个文件的簇链（正常结尾 / 长度足够 / 不越界）

输出里明确打印一行：文件系统=FAT32 簇数=…（供验收 grep）

用法：python tools/fat_check.py [esp.img]
退出码：0 = 没发现明显问题；1 = 有 ★ 级问题
"""
import struct
import sys

FAT_TYPE_NAMES = ("FAT12", "FAT16", "FAT32")
CLUSTER_LIMIT_12 = 4085         # 簇数 < 4085 -> FAT12
CLUSTER_LIMIT_16 = 65525        # 簇数 < 65525 -> FAT16，>= 65525 -> FAT32
EOF_MIN = {12: 0xFF8, 16: 0xFFF8, 32: 0x0FFFFFF8}   # 各自的"链尾"下界
EOF_VAL = {12: 0xFFF, 16: 0xFFFF, 32: 0x0FFFFFFF}

import struct
import sys

# ★ 输出统一按 UTF-8 写：本工具被 tests/esp_install_test.py 之类的脚本用
#   subprocess 捕获后 `.decode("utf-8")`，而 Windows 上默认 stdout 编码是 GBK
#   （cp936）—— 不强制 UTF-8 的话中文结论行在 UTF-8 解码后变成乱码，验收 grep
#   "文件系统=FAT32 簇数=…"/"没发现明显问题" 就会假失败。
if hasattr(sys.stdout, "reconfigure"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


class FatVol:
    def __init__(self, b):
        self.b = b
        self.bps = u16(b, 11)
        self.spc = b[13]
        self.rsvd = u16(b, 14)
        self.nfats = b[16]
        self.root_ent = u16(b, 17)
        self.tot16 = u16(b, 19)
        self.media = b[21]
        self.fatsz16 = u16(b, 22)
        # ---- FAT32 BPB 字段 ----
        self.fatsz32 = u32(b, 36)
        self.ext_flags = u16(b, 40)
        self.fs_ver = u16(b, 42)
        self.root_clus = u32(b, 44)
        self.fsinfo = u16(b, 48)
        self.bkboot = u16(b, 50)
        # ---- 类型判定（与规范/FAT 驱动同一口径）----
        if self.fatsz16 != 0 or self.root_ent != 0:
            self.is_fat32 = False
            self.fatsz = self.fatsz16
            self.label = b[43:54].decode("latin-1").rstrip("\x00 ")
            self.fstype_field = b[54:62].decode("latin-1").rstrip("\x00 ")
        else:
            self.is_fat32 = True
            self.fatsz = self.fatsz32
            self.label = b[71:82].decode("latin-1").rstrip("\x00 ")
            self.fstype_field = b[82:90].decode("latin-1").rstrip("\x00 ")
        self.tot = self.tot16 or u32(b, 32)
        self.cluster_bytes = self.bps * self.spc
        self.root_sec = (self.root_ent * 32 + self.bps - 1) // self.bps if not self.is_fat32 else 0
        self.data_start = self.rsvd + self.nfats * self.fatsz + self.root_sec
        self.nclusters = (self.tot - self.data_start) // self.spc if self.tot > self.data_start else 0
        self.fat_off = self.rsvd * self.bps
        if self.nclusters < CLUSTER_LIMIT_12:
            self.type_bits = 12
        elif self.nclusters < CLUSTER_LIMIT_16:
            self.type_bits = 16
        else:
            self.type_bits = 32
        self.type_name = {12: "FAT12", 16: "FAT16", 32: "FAT32"}[self.type_bits]

    # ---- FAT 项（12/16/32 位）----
    def fat_entry(self, cl):
        if self.type_bits == 32:
            return u32(self.b, self.fat_off + cl * 4) & 0x0FFFFFFF
        if self.type_bits == 16:
            return u16(self.b, self.fat_off + cl * 2)
        off = self.fat_off + (cl * 3) // 2                     # 12 位：1.5 字节一项
        v = u16(self.b, off)
        return (v >> 4) if (cl & 1) else (v & 0xFFF)

    def eof_min(self):
        return EOF_MIN[self.type_bits]

    def chain(self, cl, guard_max=2000000):
        """沿簇链走，返回 (簇列表, 是否以 EOF 正常结尾, 备注)。"""
        out, guard = [], 0
        seen = set()
        while cl >= 2 and guard < guard_max:
            if cl in seen:
                return out, False, "簇链成环（簇 %d 重复）" % cl
            seen.add(cl)
            out.append(cl)
            nxt = self.fat_entry(cl)
            if nxt >= self.eof_min():
                return out, True, ""
            if nxt == 0:
                return out, False, "簇 %d 的 FAT 项为 0（空闲 = 链断）" % cl
            if nxt == 1 or nxt < 2:
                return out, False, "簇 %d 的 FAT 项非法（%d）" % (cl, nxt)
            cl = nxt
            guard += 1
        return out, False, "簇链过长/未终止"

    def cluster_off(self, cl):
        return (self.data_start + (cl - 2) * self.spc) * self.bps

    def list_dir(self, cl, walk_chain=True):
        """列一个目录：cl = 0 表示固定根目录区（FAT12/16），否则是簇号。"""
        out = []
        if cl == 0 and not self.is_fat32:
            buf = self.b[self.fat_off + self.nfats * self.fatsz * self.bps:
                         self.fat_off + self.nfats * self.fatsz * self.bps + self.root_ent * 32]
        else:
            ch, term, why = self.chain(cl) if walk_chain else ([cl], True, "")
            buf = bytearray()
            for c in ch:
                o = self.cluster_off(c)
                buf += self.b[o:o + self.cluster_bytes]
        off = 0
        while off + 32 <= len(buf):
            e = buf[off:off + 32]
            if e[0] == 0x00:
                break
            off += 32
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            base = e[0:8].decode("latin-1").rstrip()
            ext = e[8:11].decode("latin-1").rstrip()
            name = base + ("." + ext if ext else "")
            out.append({"name": name, "attr": e[11],
                        "cluster": (u32(e, 20) & 0xFFFF0000) | u16(e, 26),
                        "size": u32(e, 28),
                        "is_dir": bool(e[11] & 0x10), "raw": e})
        return out


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "build64/esp.img"
    with open(path, "rb") as f:
        b = f.read()
    print("=" * 74)
    print("FAT 卷体检：%s (%d 字节 = %d 个 512B 扇区)" % (path, len(b), len(b) // 512))
    print("=" * 74)
    if len(b) < 512:
        print("  ★ 文件太小，连引导扇区都不够")
        return 1

    v = FatVol(b)
    problems = []

    print("BPB：")
    print("  BytsPerSec   = %d" % v.bps)
    print("  SecPerClus   = %d  -> 簇大小 %d 字节" % (v.spc, v.cluster_bytes))
    print("  RsvdSecCnt   = %d" % v.rsvd)
    print("  NumFATs      = %d" % v.nfats)
    print("  RootEntCnt   = %d  -> 固定根目录 %d 扇区（FAT32 应为 0）" % (v.root_ent, v.root_sec))
    print("  TotSec16/32  = %d / %d（实际容量 %d 扇区）" % (v.tot16, u32(b, 32), len(b) // 512))
    print("  Media        = 0x%02X   FATSz16 = %d   FATSz32 = %d" % (v.media, v.fatsz16, v.fatsz32))
    if v.is_fat32:
        print("  FAT32 专有：ExtFlags=0x%04X FSVer=0x%04X RootClus=%d FSInfo=%d BkBootSec=%d"
              % (v.ext_flags, v.fs_ver, v.root_clus, v.fsinfo, v.bkboot))
    print("  卷标/类型     = %r / %r" % (v.label, v.fstype_field))
    print("  --- 推导 ---")
    print("  数据区起始扇区 = %d   数据区簇数 = %d   卷总扇区 = %d"
          % (v.data_start, v.nclusters, v.tot))
    print("  ★ 文件系统=%s 簇数=%d（按簇数推导=%s，BPB 自称=%r）"
          % (v.type_name, v.nclusters, FAT_TYPE_NAMES[0 if v.type_bits == 12 else
                                                     (1 if v.type_bits == 16 else 2)],
             v.fstype_field))

    # ---- 通用检查 ----
    if v.bps not in (512, 1024, 2048, 4096):
        problems.append("★ BytsPerSec=%d 非法" % v.bps)
    if v.spc == 0 or (v.spc & (v.spc - 1)) != 0:
        problems.append("★ SecPerClus=%d 不是 2 的幂" % v.spc)
    if v.nfats < 1:
        problems.append("★ NumFATs=0")
    if v.tot != len(b) // 512:
        problems.append("★ TotSec(%d) 与实际容量(%d 扇区) 不一致" % (v.tot, len(b) // 512))
    if v.nclusters == 0:
        problems.append("★ 数据区簇数为 0")

    # ---- 类型一致性 / 硬断言 ----
    if v.is_fat32:
        if v.nclusters < CLUSTER_LIMIT_16:
            problems.append("★ 致命：BPB 是 FAT32（FATSz16=0/FATSz32=%d），但簇数 %d < %d 属 FAT16 区间"
                            " —— 遵循规范的驱动会按 16 位读 32 位 FAT 表，簇链变成垃圾 -> "
                            "多簇文件读取报 EFI_VOLUME_CORRUPTED" % (v.fatsz32, v.nclusters, CLUSTER_LIMIT_16))
        if v.fatsz32 == 0:
            problems.append("★ BPB_FATSz32 = 0")
        if v.root_clus < 2:
            problems.append("★ BPB_RootClus=%d 非法（FAT32 根目录必须是簇链，通常为 2）" % v.root_clus)
        if v.root_ent != 0:
            problems.append("★ FAT32 的 RootEntCnt 必须为 0（实测 %d）" % v.root_ent)
        # FSInfo
        if v.fsinfo == 0 or v.fsinfo >= v.rsvd:
            problems.append("★ BPB_FSInfo=%d 非法（应落在保留扇区内，通常为 1）" % v.fsinfo)
        else:
            fi = b[v.fsinfo * v.bps:(v.fsinfo + 1) * v.bps]
            lead, struc, trail = u32(fi, 0), u32(fi, 484), u32(fi, 508)
            free, nxt = u32(fi, 488), u32(fi, 492)
            print("FSInfo（扇区 %d）：lead=0x%08X struct=0x%08X trail=0x%08X free=%d next=%d"
                  % (v.fsinfo, lead, struc, trail, free, nxt))
            if lead != 0x41615252:
                problems.append("★ FSInfo 起始签名不是 0x41615252（实测 0x%08X）" % lead)
            if struc != 0x61417272:
                problems.append("★ FSInfo 结构签名不是 0x61417272（实测 0x%08X）" % struc)
            if trail != 0xAA550000:
                problems.append("★ FSInfo 尾签名不是 0xAA550000（实测 0x%08X）" % trail)
            if fi[510] != 0x55 or fi[511] != 0xAA:
                problems.append("★ FSInfo 扇区末尾缺 0xAA55")
            if free != 0xFFFFFFFF and free > v.nclusters:
                problems.append("★ FSInfo 空闲簇数 %d 超过总簇数 %d" % (free, v.nclusters))
            if nxt != 0xFFFFFFFF and not (2 <= nxt <= v.nclusters + 1):
                problems.append("★ FSInfo next-free=%d 越界（合法 2..%d）" % (nxt, v.nclusters + 1))
        # 备份引导扇区
        if not (1 <= v.bkboot < v.rsvd):
            problems.append("★ BPB_BkBootSec=%d 非法（应在保留扇区内）" % v.bkboot)
        else:
            bk_off = v.bkboot * v.bps
            same = b[bk_off:bk_off + v.bps] == b[0:v.bps]
            print("  备份引导扇区（扇区 %d）与 0 号扇区一致：%s" % (v.bkboot, "是" if same else "★ 否"))
            if not same:
                problems.append("★ 备份引导扇区与 0 号扇区不一致")
    else:
        if v.type_bits == 12 and v.fstype_field.upper().startswith("FAT16"):
            problems.append("★ 致命：BPB 自称 FAT16，但簇数 %d < 4085 属 FAT12 区间 —— 遵循规范的驱动"
                            "（EDK2）会按 12 位读 16 位 FAT 表 -> 多簇文件读取报 EFI_VOLUME_CORRUPTED"
                            % v.nclusters)
        if v.type_bits == 16 and v.nclusters > 0xFFEF:
            problems.append("★ 簇数 %d 超出 FAT16 上限 0xFFEF" % v.nclusters)

    # ---- FAT[0] / FAT[1] / 两份 FAT 一致 ----
    fat0, fat1 = v.fat_entry(0), v.fat_entry(1)
    print("FAT[0]=0x%0*X (低位应为 0x%02X)  FAT[1]=0x%0*X (应为 0x%0*X)"
          % (v.type_bits // 4, fat0, v.media, v.type_bits // 4, fat1,
             v.type_bits // 4, EOF_VAL[v.type_bits]))
    if (fat0 & 0xFF) != v.media:
        problems.append("★ FAT[0] 低位不是 media 0x%02X（实测 0x%02X）" % (v.media, fat0 & 0xFF))
    if fat1 != EOF_VAL[v.type_bits]:
        problems.append("★ FAT[1] 不是 0x%0*X" % (v.type_bits // 4, EOF_VAL[v.type_bits]))
    if v.nfats >= 2:
        fat_a = b[v.fat_off:v.fat_off + v.fatsz * v.bps]
        fat_b = b[v.fat_off + v.fatsz * v.bps:v.fat_off + 2 * v.fatsz * v.bps]
        same = fat_a == fat_b
        print("  两个 FAT 副本逐字节一致：%s" % ("是" if same else "★ 否"))
        if not same:
            problems.append("★ 两个 FAT 副本不一致（只认 FAT1 的驱动/修复工具会看到不同的链）")

    # ---- 已用/空闲簇统计（同时核对 FSInfo 的 free）----
    used = 0
    for c in range(2, v.nclusters + 2):
        if v.fat_entry(c) != 0:
            used += 1
    print("  已分配簇 = %d，空闲簇 = %d（FSInfo 声称 free=%s）"
          % (used, v.nclusters - used,
             ("%d" % u32(b, v.fsinfo * v.bps + 488)) if v.is_fat32 and v.fsinfo else "n/a"))
    if v.is_fat32 and v.fsinfo:
        free = u32(b, v.fsinfo * v.bps + 488)
        if free != 0xFFFFFFFF and free != v.nclusters - used:
            problems.append("注意：FSInfo 的 free=%d 与实际空闲 %d 不一致（部分驱动按它判断剩余空间）"
                            % (free, v.nclusters - used))

    # ---- 目录树 ----
    def check_dir(cl, path, depth):
        nonlocal problems
        ents = v.list_dir(cl)
        names = [e["name"] for e in ents]
        is_root = (depth == 0)
        print("%s%s（%d 个条目）%s"
              % ("  " * depth, path or "/", len(ents), " ".join(names[:8])))
        if not is_root:
            if "." not in names:
                problems.append("★ %s 缺 \".\" 条目" % path)
            if ".." not in names:
                problems.append("★ %s 缺 \"..\" 条目" % path)
        for e in ents:
            if e["name"] in (".", ".."):
                # "." 必须指向自己；".." 指向父（根目录按惯例写 0 或根簇）
                continue
            full = (path + "/" if path else "") + e["name"]
            if e["is_dir"]:
                if e["cluster"] < 2:
                    problems.append("★ 目录 %s 的起始簇 %d 非法" % (full, e["cluster"]))
                elif e["cluster"] > v.nclusters + 1:
                    problems.append("★ 目录 %s 的起始簇 %d 越界（数据区 %d 簇）" % (full, e["cluster"], v.nclusters))
                elif depth < 4:
                    print("%s[目录] %s 起始簇=%d" % ("  " * (depth + 1), full, e["cluster"]))
                    check_dir(e["cluster"], full, depth + 1)
            else:
                if e["size"] == 0:
                    print("%s[文件] %s 起始簇=%d 大小=0" % ("  " * (depth + 1), full, e["cluster"]))
                    continue
                ch, term, why = v.chain(e["cluster"])
                need = (e["size"] + v.cluster_bytes - 1) // v.cluster_bytes
                print("%s[文件] %s 起始簇=%d 大小=%d 簇链=%d 簇 -> %d 字节；%s"
                      % ("  " * (depth + 1), full, e["cluster"], e["size"], len(ch),
                         len(ch) * v.cluster_bytes,
                         "以 0x%0*X 正常结尾" % (v.type_bits // 4, EOF_VAL[v.type_bits])
                         if term else "★ 链未正常结尾（%s）" % why))
                if not term:
                    problems.append("★ %s 的簇链没有正常结尾（固件读文件会判 EFI_VOLUME_CORRUPTED）：%s"
                                    % (full, why))
                if len(ch) < need:
                    problems.append("★ %s 簇链长度不足：需要 %d 簇，实际 %d" % (full, need, len(ch)))
                if e["cluster"] > v.nclusters + 1 or (ch and ch[-1] > v.nclusters + 1):
                    problems.append("★ %s 的簇号越界（数据区只有 %d 簇）" % (full, v.nclusters))
                last = ch[-1] if ch else e["cluster"]
                if v.cluster_off(last) + v.cluster_bytes > len(b):
                    problems.append("★ %s 末簇 %d 的结束偏移超出卷大小 %d" % (full, last, len(b)))

    print("目录树：")
    check_dir(v.root_clus if v.is_fat32 else 0, "", 0)
    if v.is_fat32:
        ch, term, why = v.chain(v.root_clus)
        print("  根目录簇链：%d 个簇 -> %d 字节；%s"
              % (len(ch), len(ch) * v.cluster_bytes,
                 "以 0x0FFFFFFF 正常结尾" if term else "★ 未正常结尾（%s）" % why))
        if not term:
            problems.append("★ 根目录簇链没有 0x0FFFFFFF 结尾：%s" % why)
        if not (v.root_clus <= v.nclusters + 1):
            problems.append("★ 根簇 %d 越界" % v.root_clus)

    print("  --- 结论 ---")
    fatal = [m for m in problems if m.startswith("★")]
    if fatal:
        for m in fatal:
            print("    %s" % m)
    for m in [m for m in problems if not m.startswith("★")]:
        print("    %s" % m)
    if not problems:
        print("    没发现明显问题")
    elif not fatal:
        print("    没发现明显问题（只有上面的提示项）")
        return 0
    return 1 if fatal else 0


if __name__ == "__main__":
    sys.exit(main())
