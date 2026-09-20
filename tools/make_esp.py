#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_esp.py - 生成 **FAT32** 的 EFI 系统分区（ESP）镜像

为什么自己写：这台机器上没有 mkfs.fat / mtools（MSYS2 里没装），而本项目本来就在
tools/make_iso64.py 里自己解析 ISO9660 —— 那就顺手把 FAT32 也自己写掉，不引入新依赖。

ESP 里放这些文件（都是 8.3 短名，不需要 LFN）：
    EFI/BOOT/BOOTX64.EFI   自研 UEFI 引导程序（PE32+）
    KERNEL64.BIN           安装程序内核（UEFI 引导程序会读进 0x100000）
    SYSTEM.IMG             系统镜像载荷（读进 0x04000000）
    UEFI64.BIN             平铺长模式引导器（UEFI 引导程序读进 0x800000）

布局（标准 FAT32，512B 扇区 / 1 扇区每簇）：
    扇区 0            ：引导扇区（FAT32 BPB + 0x55AA；EFI 不执行它的代码，但字段必须自洽）
    扇区 1            ：FSInfo（0x41615252 / 0x61417272 / 0xAA550000 签名 + free/next-free）
    扇区 6            ：备份引导扇区（BPB_BkBootSec = 6，逐字节等于扇区 0）
    扇区 7            ：FSInfo 的备份（规范建议 6..8 是备份区）
    扇区 32..32+FATsz*2-1 ：FAT1 + FAT2（32 位项；FAT[0]=0x0FFFFFF8 / FAT[1]=0x0FFFFFFF）
    然后是数据区：**根目录也是一个簇链，起始簇 = 2**，子目录同样从数据区分配

★★ 关键规范约束（这才是"真 FAT32"，不是把类型字符串改掉就算）：
    Microsoft FAT 规范按**簇数**判定类型：
        < 4085         -> FAT12
        4085..65524    -> FAT16
        >= 65525       -> FAT32
    而 EDK2（OVMF / VMware EFI）的 FAT 驱动**按簇数**决定用 12/16/32 位读 FAT 表：
    簇数不到 65525 却自称 FAT32 的卷，会被按 FAT16 解析 -> 32 位 FAT 项被砍成 16 位，
    簇链立刻变成垃圾 -> 读文件返回 EFI_VOLUME_CORRUPTED。
    这跟当初"名为 FAT16、簇数却落在 FAT12 区间"的坑是镜像关系，所以这里同样加**硬断言**：
        cluster_count >= 65525
    违反就报错退出（构建期），内核侧 fat64_selftest64 / fat64_format64 同样拒绝。

    512B 扇区 + SPC=1 时，65525 个簇就是 65525 个数据扇区（= 32.0MB）；再加上
    32 个保留扇区 + 2 份 FAT（每簇 4 字节 -> 约 0.75MB）+ 根目录簇，
    合法 FAT32 卷的下限约 33.5MB。本脚本直接取 **48MB**（98304 扇区）留余量：
    解出 96736 簇（>= 65525，余量约 48%），而 ESP 里的全部载荷（内核 4MB + 载荷 4.1MB
    + 引导器）只要 ~16 万个 512B 扇区中的 ~16300 个簇。

用法：
    python tools/make_esp.py <输出.img> <BOOTX64.EFI> <KERNEL64.BIN> <SYSTEM.IMG> [UEFI64.BIN]
"""
import struct
import sys

SECTOR = 512
SPC = 1                     # 每簇 1 扇区 = 512B
RESERVED = 32               # ★ FAT32 规范要求保留扇区 >= 32（0=BS, 1=FSInfo, 6=备份 BS）
NUM_FATS = 2
ROOT_CLUSTER = 2            # ★ FAT32 的根目录是**簇链**（FAT12/16 才是固定区），从簇 2 开始
FSINFO_SECTOR = 1
BACKUP_BOOT_SECTOR = 6
MEDIA = 0xF8
VOL_LABEL = b"VIMTU64ESP "  # 11 字节卷标
OEM = b"VIMTU64 "
FS_TYPE = b"FAT32   "
CLUSTER_MIN = 65525         # ★ FAT32 合法簇数下界（< 65525 就是 FAT16）
ESP_MIN_SECTORS = 98304     # 48MB：>= 33.5MB 的 FAT32 下限，留余量
FSINFO_LEAD = 0x41615252    # 'RRaA'
FSINFO_STRUC = 0x61417272   # 'rrAa'
FSINFO_TRAIL = 0xAA550000   # 尾签名（最后 2 字节再写 0x55AA）


def _fat32_geometry(total_sectors):
    """解出 FAT32 的 FAT 大小（簇数依赖 FAT 大小，迭代一次即可收敛）。

    与 kernel/fat64.cpp 的 geometry() 同一算法，两个实现必须给出同样的数字，
    否则"构建期写的卷"和"安装时写的卷"会不一样（这是本项目踩过的坑）。
    """
    fatsz = 1
    while True:
        data_sectors = total_sectors - RESERVED - NUM_FATS * fatsz     # FAT32 没有固定根目录区
        clusters = data_sectors // SPC
        need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR             # 每簇一个 u32
        if need <= fatsz:
            return fatsz, clusters, data_sectors
        fatsz = need
        if fatsz > 4096:
            raise SystemExit("make_esp.py 错误：FAT 表 > 4096 扇区（卷大得不像 FAT32 了）")


class EspImage:
    def __init__(self, files):
        # files: [(8.3 名, 属性, 数据, [父目录路径])]；目录本身也作为条目加入
        # 需要的簇数 = **根目录 1 簇** + 2 个目录（EFI、EFI/BOOT，各 1 簇）
        #              + 每个文件 ceil(size / 簇字节)（空文件也占 1 簇）
        need_clusters = 1 + 2 + sum(max(1, (len(d) + SPC * SECTOR - 1) // (SPC * SECTOR))
                                    for (_, _, d, _) in files)
        need_sectors = need_clusters * SPC
        total = ESP_MIN_SECTORS
        # 容量必须用**解出来的几何**核对（FAT 表自身也占扇区，且随簇数增长）：
        #   载荷再大就按 1MB 步长长大，直到数据区装得下全部文件+目录。
        while True:
            fatsz, clusters, data_sectors = _fat32_geometry(total)
            if data_sectors >= need_sectors:
                break
            total += 2048
        self.total_sectors = total
        self.fatsz, self.clusters, self.data_sectors = fatsz, clusters, data_sectors
        self.need_clusters = need_clusters
        # ★ 硬断言（见文件顶部说明）：簇数 >= 65525 才是真 FAT32。
        #   没有这条断言，"名叫 FAT32、实际该按 FAT16 读"这类错就会静默复活，
        #   症状是"目录能列、单簇文件能读、多簇文件报 EFI_VOLUME_CORRUPTED"。
        if self.clusters < CLUSTER_MIN:
            raise SystemExit(
                "make_esp.py 错误：簇数 %d < %d，这不是真 FAT32（会被固件按 FAT16 解析）。\n"
                "  SPC=1/512B 扇区时，数据区至少需要 65525 个扇区（32MB），\n"
                "  加上保留扇区与两份 32 位 FAT 表，合法下限约 33.5MB；本脚本取 %d 扇区（%.1fMB）。\n"
                "  修法：调大 ESP_MIN_SECTORS（别调小）。"
                % (self.clusters, CLUSTER_MIN, ESP_MIN_SECTORS, ESP_MIN_SECTORS * SECTOR / 1048576.0))
        # 数据区起点：FAT32 没有固定根目录区，目录/文件全在数据区（根目录 = 簇 2）
        self.data_start = RESERVED + NUM_FATS * self.fatsz
        self.image = bytearray(self.total_sectors * SECTOR)
        self.fat = [0] * (self.clusters + 2)
        self.fat[0] = 0x0FFFFFF8          # 0x0FFFFFFF | media（FAT32 只用低 28 位）
        self.fat[1] = 0x0FFFFFFF
        self.next_cluster = ROOT_CLUSTER
        root = self.alloc_chain(1)        # 根目录占簇 2（单簇 512B = 16 个目录项，够用）
        assert root == ROOT_CLUSTER, root
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
                "  修法：给更大载荷时 __init__ 会自动长大 total_sectors；若仍不足请看 SPC。"
                % (self.next_cluster - 2 + n_clusters, n_clusters,
                   self.next_cluster - 2, self.clusters,
                   self.total_sectors, self.fatsz, self.data_sectors))
        start = self.next_cluster
        for i in range(n_clusters):
            c = self.next_cluster
            self.next_cluster += 1
            self.fat[c] = (c + 1) if i + 1 < n_clusters else 0x0FFFFFFF
        return start

    def write_data(self, start_cluster, data):
        off = self.data_start * SECTOR + (start_cluster - ROOT_CLUSTER) * SPC * SECTOR
        self.image[off:off + len(data)] = data

    @staticmethod
    def _entry(name, attr, cluster, size):
        """一个 32 字节目录项。★ FAT32 的起始簇是 **32 位**：
        低 16 位在偏移 26（DIR_FstClusLO），高 16 位在偏移 20（DIR_FstClusHI）。"""
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
        struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)   # 起始簇高 16 位
        struct.pack_into("<H", e, 26, cluster & 0xFFFF)           # 起始簇低 16 位
        struct.pack_into("<I", e, 28, size)
        return bytes(e)

    def build(self):
        # 1) 目录：EFI（根下）、EFI/BOOT（EFI 下）—— 都是数据区的簇，走簇链
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
            """一个目录的数据块：前两项固定是 "." 和 ".."，然后是普通条目。

            ★ 头两项是 FAT 规范的硬要求：固件/Shell 靠它们解析路径。缺了会出现
            "目录能列、但按路径打不开文件"（实测症状：根目录文件能 load，
            EFI\\BOOT\\BOOTX64.EFI 一律 "Not Found"）。
            ★ ".." 的簇号：父目录是根目录时写 0（FAT 惯例："0 = 根"；FAT12/16 的根
            没有簇号，FAT32 这样写同样被 EDK2 接受），否则写父目录的真实簇号。
            """
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

        # 3) 根目录（FAT32：**数据区的簇链**，起始簇 = 2）
        root_off = self.data_start * SECTOR + (ROOT_CLUSTER - ROOT_CLUSTER) * SPC * SECTOR
        idx = 0
        self.image[root_off:root_off + 32] = self._entry("EFI", 0x10, efi_cluster, 0)
        idx = 1
        for (name, attr, c, size, parent) in self.written:
            if parent != "":
                continue
            self.image[root_off + idx * 32: root_off + idx * 32 + 32] = self._entry(name, attr, c, size)
            idx += 1

        # 4) EFI/ 目录：BOOT 子目录（+ "." / ".."）
        efi_off = self.data_start * SECTOR + (efi_cluster - ROOT_CLUSTER) * SPC * SECTOR
        efi_entries = [self._entry("BOOT", 0x10, boot_cluster, 0)]
        for (name, attr, c, size, parent) in self.written:
            if parent == "EFI":
                efi_entries.append(self._entry(name, attr, c, size))
        self.image[efi_off:efi_off + SPC * SECTOR] = dir_block(efi_cluster, 0, efi_entries)

        # 5) EFI/BOOT 目录：BOOTX64.EFI（+ "." / ".."）
        bt_off = self.data_start * SECTOR + (boot_cluster - ROOT_CLUSTER) * SPC * SECTOR
        boot_entries = [self._entry(name, attr, c, size)
                        for (name, attr, c, size, parent) in self.written if parent == "EFI/BOOT"]
        self.image[bt_off:bt_off + SPC * SECTOR] = dir_block(boot_cluster, efi_cluster, boot_entries)

        # 6) FAT 表（两份；32 位项）
        fat_bytes = bytearray(self.fatsz * SECTOR)
        for i, v in enumerate(self.fat):
            struct.pack_into("<I", fat_bytes, i * 4, v & 0x0FFFFFFF)
        for k in range(NUM_FATS):
            off = (RESERVED + k * self.fatsz) * SECTOR
            self.image[off:off + len(fat_bytes)] = fat_bytes

        # 7) FSInfo 扇区（+ 备份）：free / next-free 都是真实值
        allocated = self.next_cluster - ROOT_CLUSTER
        free = self.clusters - allocated
        next_free = self.next_cluster if self.next_cluster <= self.clusters + 1 else 0xFFFFFFFF
        fi = bytearray(SECTOR)
        struct.pack_into("<I", fi, 0, FSINFO_LEAD)
        struct.pack_into("<I", fi, 484, FSINFO_STRUC)
        struct.pack_into("<I", fi, 488, free)
        struct.pack_into("<I", fi, 492, next_free)
        struct.pack_into("<I", fi, 508, FSINFO_TRAIL)
        fi[510] = 0x55
        fi[511] = 0xAA
        self.image[FSINFO_SECTOR * SECTOR:(FSINFO_SECTOR + 1) * SECTOR] = fi
        self.image[(FSINFO_SECTOR + BACKUP_BOOT_SECTOR) * SECTOR
                   :(FSINFO_SECTOR + BACKUP_BOOT_SECTOR + 1) * SECTOR] = fi

        # 8) 引导扇区（FAT32 BPB；EFI 不看它的代码段，但字段必须自洽）
        bs = bytearray(SECTOR)
        bs[0:3] = b"\xEB\x58\x90"
        bs[3:11] = OEM
        struct.pack_into("<H", bs, 11, SECTOR)                 # 每扇区字节
        bs[13] = SPC                                           # 每簇扇区
        struct.pack_into("<H", bs, 14, RESERVED)               # 保留扇区（FAT32 必须 >= 32）
        bs[16] = NUM_FATS
        struct.pack_into("<H", bs, 17, 0)                      # ★ FAT32：根目录项数必须为 0
        struct.pack_into("<H", bs, 19, 0)                      # ★ FAT32：TotSec16 必须为 0
        bs[21] = MEDIA
        struct.pack_into("<H", bs, 22, 0)                      # ★ FAT32：FATSz16 必须为 0
        struct.pack_into("<H", bs, 24, 32)                     # 每道扇区
        struct.pack_into("<H", bs, 26, 64)                     # 磁头
        struct.pack_into("<I", bs, 28, 0)                      # 隐藏扇区
        struct.pack_into("<I", bs, 32, self.total_sectors)     # 总扇区（32 位）
        struct.pack_into("<I", bs, 36, self.fatsz)             # ★ BPB_FATSz32
        struct.pack_into("<H", bs, 40, 0)                      # ExtFlags：0 = 两份 FAT 互为镜像
        struct.pack_into("<H", bs, 42, 0)                      # FSVer 0.0
        struct.pack_into("<I", bs, 44, ROOT_CLUSTER)           # ★ BPB_RootClus = 2
        struct.pack_into("<H", bs, 48, FSINFO_SECTOR)          # ★ BPB_FSInfo = 1
        struct.pack_into("<H", bs, 50, BACKUP_BOOT_SECTOR)     # ★ BPB_BkBootSec = 6
        bs[64] = 0x80                                          # 驱动器号
        bs[66] = 0x29                                          # 扩展引导签名
        struct.pack_into("<I", bs, 67, 0x56494D54)             # 卷序号 'VIMT'
        bs[71:82] = VOL_LABEL
        bs[82:90] = FS_TYPE
        bs[510] = 0x55
        bs[511] = 0xAA
        self.image[0:SECTOR] = bs
        # ★ 备份引导扇区（6 号扇区必须逐字节等于 0 号扇区：固件/修复工具会拿它兜底）
        self.image[BACKUP_BOOT_SECTOR * SECTOR:(BACKUP_BOOT_SECTOR + 1) * SECTOR] = bytes(bs)
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
    print("ESP 镜像: %s = %d 字节（FAT32, %d 簇 >= %d, 每簇 %d 字节, FATsz=%d, 根簇=%d；"
          "文件+目录需要 %d 簇）"
          % (out, len(img), esp.clusters, CLUSTER_MIN, SPC * SECTOR, esp.fatsz,
             ROOT_CLUSTER, esp.need_clusters))
    print("  内含: EFI/BOOT/BOOTX64.EFI=%d B  KERNEL64.BIN=%d B  SYSTEM.IMG=%d B"
          % (len(data_efi), len(data_k), len(data_p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
