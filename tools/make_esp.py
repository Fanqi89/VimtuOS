#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_esp.py - 生成 FAT16 的 EFI 系统分区（ESP）镜像

为什么自己写：这台机器上没有 mkfs.fat / mtools（MSYS2 里没装），而本项目本来就在
tools/make_iso64.py 里自己解析 ISO9660 —— 那就顺手把 FAT16 也自己写掉，不引入新依赖。

ESP 里放三个文件（都是 8.3 短名，不需要 LFN）：
    EFI/BOOT/BOOTX64.EFI   自研 UEFI 引导程序（PE32+）
    KERNEL64.BIN           安装程序内核（UEFI 引导程序会读进 0x100000）
    SYSTEM.IMG             系统镜像载荷（读进 0x04000000）

布局（标准 FAT16，512B 扇区 / 2KB 簇）：
    扇区 0          ：引导扇区（BPB + 0x55AA；EFI 不需要它可引导，但要合法）
    扇区 1..FATsz   ：FAT1
    然后 FAT2
    然后根目录（512 项 = 32 扇区）
    然后数据区

用法：
    python tools/make_esp.py <输出.img> <BOOTX64.EFI> <KERNEL64.BIN> <SYSTEM.IMG>
"""
import os
import struct
import sys

SECTOR = 512
# ★ 每簇扇区数必须是 1：卷要"真的是 FAT16"。
#   规范（Microsoft FAT）用**簇数**判定类型：< 4085 应为 FAT12，4085..65524 才是 FAT16。
#   6MB 的 ESP 用 2KB 簇只有 3057 簇 —— 名字叫 FAT16、簇数却落在 FAT12 区间，
#   而 EDK2 的 FAT 驱动**按簇数**决定用 12 位还是 16 位读 FAT 表：
#   它把我们的 16 位 FAT 当 12 位解析，簇链立刻变成垃圾 → 读文件返回 EFI_VOLUME_CORRUPTED。
#   症状极具迷惑性（我们踩过）：
#     · 目录能列、文件能打开、**单簇文件能读**（2048 字节的 BOOTX64.EFI 恰好 1 簇，
#       读完不需要查 FAT 项 → 手动 load 一直"成功"）
#     · **多簇文件必挂**（UEFI64.BIN 28KB 要查 FAT 链 → 固件报损坏 → 桩 return 3）
#   改成 SPC=1 后同样 6MB 卷有 12157 簇，落进 FAT16 合法区间，且 ESP 体积不变。
SPC = 1                     # 每簇 1 扇区 = 512B（簇数 12157，真 FAT16）
RESERVED = 1
NUM_FATS = 2
ROOT_ENTRIES = 512
ROOT_SECTORS = (ROOT_ENTRIES * 32 + SECTOR - 1) // SECTOR      # 32 扇区
MEDIA = 0xF8
VOL_LABEL = b"VIMTU64ESP "  # 11 字节卷标
OEM = b"VIMTU64 "


def _fat16_geometry(total_sectors):
    """解出 FAT16 的 FAT 大小（簇数依赖 FAT 大小，迭代一次即可收敛）。"""
    fatsz = 1
    while True:
        data_sectors = total_sectors - RESERVED - NUM_FATS * fatsz - ROOT_SECTORS
        clusters = data_sectors // SPC
        need = ((clusters + 2) * 2 + SECTOR - 1) // SECTOR          # 每簇一个 u16
        if need <= fatsz:
            return fatsz, clusters, data_sectors
        fatsz = need


class EspImage:
    def __init__(self, files):
        # files: [(8.3 名, 属性, 数据, [父目录路径])]；目录本身也作为条目加入
        # 需要的簇数 = 2 个目录（EFI、EFI/BOOT，各 1 簇）+ 每个文件 ceil(size / 簇字节)
        # （空文件也占 1 簇，与 build() 里的 max(1, ...) 一致）
        need_clusters = 2 + sum(max(1, (len(d) + SPC * SECTOR - 1) // (SPC * SECTOR))
                                for (_, _, d, _) in files)
        need_sectors = need_clusters * SPC
        # 先按"一次就够"的估算取一个 1MB 步长的漂亮大小
        total = RESERVED + ROOT_SECTORS + need_sectors + 64
        total = ((total + 2047) // 2048) * 2048
        # ★ 容量必须用**解出来的几何**核对（真根因之修，见下面 build/alloc_chain 的注释）：
        #   FAT 表自身占 NUM_FATS * FATsz 个扇区，而 FATsz 随簇数增长（每簇 2 字节）。
        #   旧版本只按"固定 64 扇区余量"估算，2.06MB 内核那种载荷会算出"够用"其实不够：
        #   total = 12288 扇区时 FATsz = 49 -> 数据区只剩 12157 簇，而文件+目录要 12178 簇，
        #   alloc_chain 在 self.fat[c] 上直接 IndexError。
        #   这里改成"按解出的 data_sectors 核对，不够就按 1MB 步长长大"，收敛很快（≤ 1 次）。
        while True:
            fatsz, clusters, data_sectors = _fat16_geometry(total)
            if data_sectors >= need_sectors:
                break
            total += 2048
        self.total_sectors = total
        self.fatsz, self.clusters, self.data_sectors = fatsz, clusters, data_sectors
        self.need_clusters = need_clusters
        # 硬断言（见文件顶部 SPC 的说明）：卷必须落在 FAT16 的合法簇数区间，
        # 否则固件会按 FAT12 解析本卷，多簇文件读取必然 EFI_VOLUME_CORRUPTED。
        # 有这条断言，"名字叫 FAT16、实际该按 FAT12 读"这类错就不可能静默复活。
        if not (4085 <= self.clusters < 65525):
            raise SystemExit(
                "make_esp.py 错误：簇数 %d 不在 FAT16 合法区间 [4085, 65525)。\n"
                "  固件（EDK2）会按 FAT12 解析本卷 -> 多簇文件读取报 EFI_VOLUME_CORRUPTED。\n"
                "  修法：调小 SPC（每簇扇区数）或调大 total_sectors。" % self.clusters)
        self.data_start = RESERVED + NUM_FATS * self.fatsz + ROOT_SECTORS
        self.image = bytearray(self.total_sectors * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0xFFF8
        self.fat[1] = 0xFFFF
        self.next_cluster = 2
        self.files = files
        self.written = []          # (名字, attr, 起始簇, 大小, 父目录)
        self.dirs = {}             # 目录路径 -> 起始簇

    # ---- 簇分配 ----
    def alloc_chain(self, n_clusters):
        # ★ 容量硬断言（在分配**之前**）：合法簇号是 2..clusters+1（self.fat 有 clusters+2 项）。
        #   少了这条，容量算错时会以 `self.fat[c] = ...` 的 IndexError 暴露 —— 报错点离
        #   根因（几何估算）很远，极难定位。这里直接把"需要多少 / 卷有多少"说清楚。
        if self.next_cluster + n_clusters > self.clusters + 2:
            raise SystemExit(
                "make_esp.py 错误：数据区容量不足。\n"
                "  需要 %d 簇（本次还要 %d 簇，已用 %d），卷只有 %d 簇"
                "（总 %d 扇区 / FATsz %d / 数据区 %d 扇区）。\n"
                "  修法：给更大载荷时 __init__ 会自动长大 total_sectors；若仍不足请看 SPC/ROOT_ENTRIES。"
                % (self.next_cluster - 2 + n_clusters, n_clusters,
                   self.next_cluster - 2, self.clusters,
                   self.total_sectors, self.fatsz, self.data_sectors))
        start = self.next_cluster
        for i in range(n_clusters):
            c = self.next_cluster
            self.next_cluster += 1
            self.fat[c] = (c + 1) if i + 1 < n_clusters else 0xFFFF
        return start

    def write_data(self, start_cluster, data):
        off = self.data_start * SECTOR + (start_cluster - 2) * SPC * SECTOR
        self.image[off:off + len(data)] = data

    @staticmethod
    def _entry(name, attr, cluster, size):
        n = name.encode("ascii") if isinstance(name, str) else name
        e = bytearray(32)
        # 8.3：主名 8 + 扩展名 3（空格填充）
        if b"." in n:
            base, ext = n.split(b".", 1)
        else:
            base, ext = n, b""
        e[0:8] = base[:8].ljust(8, b" ")
        e[8:11] = ext[:3].ljust(3, b" ")
        e[11] = attr
        e[12] = 0
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)          # 起始簇低 16 位
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def build(self):
        # 1) 目录：EFI（根下）、EFI/BOOT（EFI 下）
        efi_cluster = self.alloc_chain(1)
        boot_cluster = self.alloc_chain(1)
        self.dirs["EFI"] = efi_cluster
        self.dirs["EFI/BOOT"] = boot_cluster

        # 2) 文件（数据区，簇链连续分配）
        for (name, attr, data, parent) in self.files:
            ncls = max(1, (len(data) + SPC * SECTOR - 1) // (SPC * SECTOR))
            c = self.alloc_chain(ncls)
            self.write_data(c, data)
            self.written.append((name, attr, c, len(data), parent))

        def dir_block(self_cluster, parent_cluster, entries):
            """一个目录的数据块：前两项固定是 "." 和 ".."，然后是普通条目。"""
            buf = bytearray(SPC * SECTOR)
            dot = bytearray(self._entry(".", 0x10, self_cluster, 0))
            dot[0:11] = b".          "
            dotdot = bytearray(self._entry("..", 0x10, parent_cluster, 0))
            dotdot[0:11] = b"..         "
            buf[0:32] = dot
            buf[32:64] = dotdot
            off = 64
            for e in entries:
                buf[off:off + 32] = e
                off += 32
            return bytes(buf)

        # 3) 根目录（FAT16 固定区）
        root_off = (RESERVED + NUM_FATS * self.fatsz) * SECTOR
        idx = 0
        self.image[root_off:root_off + 32] = self._entry("EFI", 0x10, efi_cluster, 0)
        idx = 1
        for (name, attr, c, size, parent) in self.written:
            if parent != "":
                continue
            self.image[root_off + idx * 32: root_off + idx * 32 + 32] = self._entry(name, attr, c, size)
            idx += 1

        # 4) EFI/ 目录：BOOT 子目录（+ "." / ".."）
        efi_off = self.data_start * SECTOR + (efi_cluster - 2) * SPC * SECTOR
        efi_entries = [self._entry("BOOT", 0x10, boot_cluster, 0)]
        for (name, attr, c, size, parent) in self.written:
            if parent == "EFI":
                efi_entries.append(self._entry(name, attr, c, size))
        self.image[efi_off:efi_off + SPC * SECTOR] = dir_block(efi_cluster, 0, efi_entries)

        # 5) EFI/BOOT 目录：BOOTX64.EFI（+ "." / ".."）
        # ★ 头两项 "." / ".." 是 FAT 规范的硬要求：固件/Shell 靠它们解析路径。
        #   缺了它们会出现"目录能列、但按路径打不开文件"——实测症状就是根目录的文件能 load，
        #   而 EFI\BOOT\BOOTX64.EFI 一律 "Not Found"，连 CD 的 UEFI 引导都起不来。
        bt_off = self.data_start * SECTOR + (boot_cluster - 2) * SPC * SECTOR
        boot_entries = [self._entry(name, attr, c, size)
                        for (name, attr, c, size, parent) in self.written if parent == "EFI/BOOT"]
        self.image[bt_off:bt_off + SPC * SECTOR] = dir_block(boot_cluster, efi_cluster, boot_entries)

        # 6) FAT 表（两份）
        fat_bytes = bytearray(self.fatsz * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<H", fat_bytes, i * 2, v)
        for k in range(NUM_FATS):
            off = (RESERVED + k * self.fatsz) * SECTOR
            self.image[off:off + len(fat_bytes)] = fat_bytes

        # 7) 引导扇区（BPB；EFI 不看它的代码段，但字段必须自洽）
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x3C\x90"
        bs[3:11] = OEM
        struct.pack_into("<H", bs, 11, SECTOR)                 # 每扇区字节
        bs[13] = SPC                                           # 每簇扇区
        struct.pack_into("<H", bs, 14, RESERVED)               # 保留扇区
        bs[16] = NUM_FATS
        struct.pack_into("<H", bs, 17, ROOT_ENTRIES)
        struct.pack_into("<H", bs, 19, self.total_sectors if self.total_sectors < 0x10000 else 0)
        bs[21] = MEDIA
        struct.pack_into("<H", bs, 22, self.fatsz)
        struct.pack_into("<H", bs, 24, 32)                     # 每道扇区
        struct.pack_into("<H", bs, 26, 64)                     # 磁头
        struct.pack_into("<I", bs, 28, 0)                      # 隐藏扇区
        struct.pack_into("<I", bs, 32, self.total_sectors if self.total_sectors >= 0x10000 else 0)
        bs[36] = 0x80                                          # 驱动器号
        bs[38] = 0x29                                          # 扩展引导签名
        struct.pack_into("<I", bs, 39, 0x56494D54)             # 卷序号 'VIMT'
        bs[43:54] = VOL_LABEL
        bs[54:62] = b"FAT16   "
        bs[510] = 0x55
        bs[511] = 0xAA
        self.image[0:SECTOR] = bs
        return bytes(self.image)


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 2
    out, efi, kernel, payload = sys.argv[1:5]
    data_efi = open(efi, "rb").read()
    data_k = open(kernel, "rb").read()
    data_p = open(payload, "rb").read()
    files = [
        ("BOOTX64.EFI", 0x20, data_efi, "EFI/BOOT"),
        ("KERNEL64.BIN", 0x20, data_k, ""),
        ("SYSTEM.IMG", 0x20, data_p, ""),
    ]
    # 第 5 个可选参数：UEFI 平铺长模式引导器（UEFI64.BIN）放根目录，
    # 由 EFI/BOOT/BOOTX64.EFI 这个极小 PE 桩读进 0x800000 后跳过去执行。
    if len(sys.argv) >= 6:
        data_u = open(sys.argv[5], "rb").read()
        files.append(("UEFI64.BIN", 0x20, data_u, ""))
        print("  另含 UEFI64.BIN=%d B（平铺长模式引导器）" % len(data_u))
    esp = EspImage(files)
    img = esp.build()
    with open(out, "wb") as f:
        f.write(img)
    print("ESP 镜像: %s = %d 字节（FAT16, %d 簇, 每簇 %d 字节；文件+目录需要 %d 簇）"
          % (out, len(img), esp.clusters, SPC * SECTOR, esp.need_clusters))
    print("  内含: EFI/BOOT/BOOTX64.EFI=%d B  KERNEL64.BIN=%d B  SYSTEM.IMG=%d B"
          % (len(data_efi), len(data_k), len(data_p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
