#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fatread64_test.py - 批次 K：**FAT32 只读浏览（含 VFAT 长文件名）**端到端验收

它做什么（两阶段，全部自动注入按键/命令）：
  阶段 1  造盘：
    a) 用安装介质（vimtu64-64.iso 当 IDE 盘）+ 64MB 空目标盘（AHCI）**走完真实安装**，
       得到"装好的盘"—— 目标盘盘尾有安装器用 kernel/fat64.cpp 写入的 48MB **真 FAT32 ESP**
       （EFI/BOOT/BOOTX64.EFI、UEFI64.BIN、KERNEL64.BIN），与 tests/esp_install_test.py 同款夹具。
    b) 宿主侧 Python 另造一块 64MB 数据盘：MBR + 48MB FAT32 分区，里面放
       **VFAT 长名**（LongName-Document-2026.txt / 这个是一个很长的文件名.txt）、8.3 短名
       （SHORT.TXT）、子目录 Docs/inner.txt、卷标项、0xE5 删除项 —— 专门喂读取器的 LFN 解析。
  阶段 2  启动装好的盘（index 0）+ 长名数据盘（index 1），用终端与文件管理器验收：
    1) [FAT64] probe/mount（fs=FAT32 / fat_ok=1 / ro=1）、[FS64] mount fat、[DRV64] letter=… fs=FAT32 … ro=1
    2) 终端：vol 列出 FAT32 卷并标 ro；切到 ESP 后 ls 列出 3 个文件；`fatcheck` 把 ESP 上三个文件
       按块读一遍算 CRC32 —— **与宿主侧对 build64/ 构建产物算的 zlib.crc32 逐项相等**（逐字节一致）。
    3) 只读语义：write / mkdir / rm 在 ESP 上一律被拒（[FS64] reject … ro=1、[TERM] cmd … fail），
       并且**启动前后 ESP 分区字节哈希完全不变**（谁都没写它）。
    4) LFN：explorer 进入长名盘，串口 item 打点里出现完整长名（不是 LONGNA~1.TXT）；`fatcheck`
       用长名当路径也能读出正确的 CRC（证明路径解析走的是 LFN）。
    5) Explorer 端到端（鼠标注入 + 像素）：双击 ESP 卡片 -> [UI] explorer enter letter=… fatvol=…
       + [UI] explorer vol … ro=1；双击 EFI -> BOOT -> 看到 BOOTX64.EFI；双击二进制 ->
       "没有关联的应用打开此文件"（noassoc）；长名 .txt 双击 -> 文本预览（preview）。
       只读卷上按 F2/Delete/点"粘贴" -> [UI] explorer roact op=… fs=FAT32 ro=1（置灰 + 明确提示）。
    6) 禁止项：PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

边界（如实写，细节见 kernel/fat64.h / kernel/fs64.h）：
  * FAT **只读**：写/删/改名/建目录/粘贴全部被拒（-FS64_EROFS）；FAT12/16 只识别不浏览；
  * 可用空间取挂载时的 FSInfo 快照（不实时刷新）；扇区固定 512B，簇大小按 BPB（1..128 扇区/簇）；
  * LFN 上限 256B 缓冲内的 UTF-16（实际规范上限 255 码元）；8.3 与 LFN 冲突只按"LFN 是否自洽"处理。

用法（必须用 Windows 原生 Python）：py -3 tests\\fatread64_test.py [--keep] [--timeout 900]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import struct
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402
import fs_tree_test as fst         # noqa: E402  （Monitor/wait_for/kill/qemu_args/open_terminal）
import esp_install_test as esp     # noqa: E402  （run_install：真实安装夹具）
import explorer64_test as ex       # noqa: E402  （鼠标闭环 + 像素工具）

SECTOR = 512
BUILD = os.path.join(ROOT, "build64")
MEDIUM = os.path.join(ROOT, "vimtu64-64.iso")

DISK_SECTORS = 131072            # 64MB 数据盘（> 60MB 才装得下 48MB 真 FAT32）
PART_LBA = 2048
PART_SECTORS = 98304             # 48MB 分区（簇数 >= 65525，真 FAT32）
RESERVED = 32
NUM_FATS = 2
SPC = 1                          # P1：1 扇区/簇（48MB 才能凑够 65525 簇）
SPC2 = 8                         # P2：8 扇区/簇 = 4KB（U 盘常见；证明簇大小按 BPB 算）
FSINFO_SEC = 1
BKBOOT_SEC = 6
ROOT_CLUSTER = 2

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL",
             "[FAT64] selftest FAIL", "[FS64] selftest FAIL", "[DRV64] selftest FAIL"]

LONGNAME = "LongName-Document-2026.txt"
LONGNAME_CN = "这个是一个很长的文件名.txt"
LONG_TEXT = b"LFN-OK: LongName-Document-2026\n"
CN_TEXT = b"CN-LONGNAME-OK\n"
SHORT_TEXT = b"short-name-ok\n"
INNER_TEXT = b"inner-file-ok\n"
BIG_TEXT = b"big-cluster-ok\n"

DISK_SECTORS = 660000            # 320MB 数据盘（两块真 FAT32：48MB@SPC=1 + 270MB@SPC=8）
PART_LBA = 2048
PART_SECTORS = 98304             # 48MB 分区（SPC=1 -> 簇数 >= 65525，真 FAT32）
PART2_LBA = 100352
PART2_SECTORS = 553000           # ~270MB（SPC=8 = 4KB/簇 -> 簇数 ~69100 >= 65525）

# ---------------------------------------------------------------------------
# 宿主侧 FAT32 + VFAT 长名构造器（独立实现，用于喂内核读取器）
# ---------------------------------------------------------------------------
def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def p16(b, o, v):
    struct.pack_into("<H", b, o, v)


def p32(b, o, v):
    struct.pack_into("<I", b, o, v)


def lfn_checksum(short11):
    s = 0
    for c in short11:
        s = (((s & 1) << 7) + (s >> 1) + c) & 0xFF
    return s


def lfn_entries(name, short11):
    """返回 [0x0F 目录项...]（磁盘顺序：最高序号在前，带 0x40 标志）+ 短名项。"""
    units = [ord(c) for c in name] + [0]
    n = (len(units) + 12) // 13
    csum = lfn_checksum(short11)
    offs = [1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30]
    out = []
    for seq in range(n, 0, -1):
        e = bytearray(32)
        e[0] = seq | (0x40 if seq == n else 0)
        e[11] = 0x0F
        e[12] = csum
        e[13] = 0
        e[26] = 0
        e[27] = 0
        for k in range(13):
            idx = (seq - 1) * 13 + k
            ch = units[idx] if idx < len(units) else 0
            struct.pack_into("<H", e, offs[k], ch)
        out.append(bytes(e))
    return out


class Fat32Builder:
    """极简 FAT32 构造器（512B 扇区；SPC 由调用方给；只做"能读"的最小实现）。"""

    def __init__(self, start_lba, total_sectors, spc):
        self.start = start_lba
        self.total = total_sectors
        self.spc = spc
        fsz = 1
        while True:
            data = total_sectors - RESERVED - NUM_FATS * fsz
            clusters = data // spc
            need = ((clusters + 2) * 4 + SECTOR - 1) // SECTOR
            if need <= fsz:
                self.fatsz = fsz
                self.clusters = clusters
                self.data_start = RESERVED + NUM_FATS * fsz
                break
            fsz = need
        self.img = bytearray(total_sectors * SECTOR)
        self.fat = bytearray(self.fatsz * SECTOR)
        p32(self.fat, 0, 0x0FFFFFF8)
        p32(self.fat, 4, 0x0FFFFFFF)
        self.next_free = ROOT_CLUSTER

    def alloc(self, n):
        c = self.next_free
        self.next_free += n
        assert self.next_free <= self.clusters + 2, "volume full"
        for i in range(n):
            p32(self.fat, (c + i) * 4, 0x0FFFFFFF if i == n - 1 else (c + i + 1))
        return c

    def write_cluster(self, c, data):
        off = (self.data_start + (c - 2) * self.spc) * SECTOR
        assert len(data) <= self.spc * SECTOR
        self.img[off:off + len(data)] = data

    def read_cluster(self, c):
        off = (self.data_start + (c - 2) * self.spc) * SECTOR
        return self.img[off:off + self.spc * SECTOR]

    # ---- 目录项 ----
    def short_entry(self, name11, attr, cluster, size, ntflags=0):
        e = bytearray(32)
        e[0:11] = name11
        e[11] = attr
        e[12] = ntflags
        p16(e, 20, (cluster >> 16) & 0xFFFF)
        p16(e, 26, cluster & 0xFFFF)
        p32(e, 28, size)
        # 2026-09-20 12:34:10
        p16(e, 22, (12 << 11) | (34 << 5) | (10 // 2))
        p16(e, 24, (26 << 9) | (9 << 5) | 20)
        return bytes(e)

    def dir_entry(self, name, short11, attr, cluster, size):
        """长名（> 8.3 能表示）时自动带 LFN 组。"""
        base, _, ext = name.rpartition(".")
        is_83 = (0 < len(base) <= 8 and len(ext) <= 3 and name.isascii()
                 and all(0x21 <= ord(ch) <= 0x7E for ch in name.replace(".", "")))
        if is_83:
            return self.short_entry(short11, attr, cluster, size)
        return b"".join(lfn_entries(name, short11)) + self.short_entry(short11, attr, cluster, size)

    def finish(self):
        # BPB
        b = bytearray(SECTOR)
        b[0:3] = b"\xEB\x58\x90"
        b[3:11] = b"MSDOS5.0"
        p16(b, 11, SECTOR)
        b[13] = self.spc
        p16(b, 14, RESERVED)
        b[16] = NUM_FATS
        p16(b, 17, 0)
        p16(b, 19, 0)
        b[21] = 0xF8
        p16(b, 22, 0)
        p16(b, 24, 32)
        p16(b, 26, 64)
        p32(b, 28, 0)
        p32(b, 32, self.total)
        p32(b, 36, self.fatsz)
        p16(b, 40, 0)
        p16(b, 42, 0)
        p32(b, 44, ROOT_CLUSTER)
        p16(b, 48, FSINFO_SEC)
        p16(b, 50, BKBOOT_SEC)
        b[64] = 0x80
        b[66] = 0x29
        p32(b, 67, 0x4C464E31)          # 'LFN1'
        b[71:82] = b"VIMTULFN   "
        b[82:90] = b"FAT32   "
        b[510], b[511] = 0x55, 0xAA
        self.img[0:SECTOR] = b
        self.img[BKBOOT_SEC * SECTOR:(BKBOOT_SEC + 1) * SECTOR] = b
        # FAT x2
        for k in range(NUM_FATS):
            o = (RESERVED + k * self.fatsz) * SECTOR
            self.img[o:o + len(self.fat)] = self.fat
        # FSInfo
        fi = bytearray(SECTOR)
        p32(fi, 0, 0x41615252)
        p32(fi, 484, 0x61417272)
        free = self.clusters + 2 - self.next_free
        p32(fi, 488, free)
        p32(fi, 492, self.next_free)
        p32(fi, 508, 0xAA550000)
        fi[510], fi[511] = 0x55, 0xAA
        self.img[FSINFO_SEC * SECTOR:(FSINFO_SEC + 1) * SECTOR] = fi
        self.img[(FSINFO_SEC + BKBOOT_SEC) * SECTOR:(FSINFO_SEC + BKBOOT_SEC + 1) * SECTOR] = fi
        return self.img


def make_lfn_disk(path):
    """造 320MB 数据盘：MBR + 两块真 FAT32
       P1（48MB, SPC=1）: 长名/中文长名/短名/子目录/卷标/删除项 —— LFN 解析的靶子
       P2（270MB, SPC=8=4KB/簇）: BIGCLU.TXT —— 证明簇大小按 BPB 算、不写死 512B/簇
    """
    dev = Fat32Builder(PART_LBA, PART_SECTORS, SPC)
    root = dev.alloc(1)                     # 簇 2 = 根目录
    ents = bytearray()
    # 1) 卷标项（attr 0x08，必须被跳过）
    vol = bytearray(32)
    vol[0:11] = b"LFNVOL     "
    vol[11] = 0x08
    ents += vol
    # 2) 0xE5 删除项（必须被跳过）
    dele = bytearray(32)
    dele[0] = 0xE5
    dele[1:11] = b"DELETED TXT"[:10]
    delt = bytearray(32)
    delt[0] = 0xE5
    delt[11] = 0x0F
    ents += dele + delt
    # 3) 长名 1（ASCII）
    c1 = dev.alloc((len(LONG_TEXT) + SPC * SECTOR - 1) // (SPC * SECTOR))
    dev.write_cluster(c1, LONG_TEXT)
    ents += dev.dir_entry(LONGNAME, b"LONGNA~1TXT", 0x20, c1, len(LONG_TEXT))
    # 4) 长名 2（中文；LFN 的 UTF-16 路径）
    c2 = dev.alloc(1)
    dev.write_cluster(c2, CN_TEXT)
    ents += dev.dir_entry(LONGNAME_CN, b"ZHONGW~1TXT", 0x20, c2, len(CN_TEXT))
    # 5) 8.3 短名
    c3 = dev.alloc(1)
    dev.write_cluster(c3, SHORT_TEXT)
    ents += dev.dir_entry("SHORT.TXT", b"SHORT   TXT", 0x20, c3, len(SHORT_TEXT))
    # 6) 子目录 Docs（"." / ".." / inner.txt）
    docs_c = dev.alloc(1)
    inner_c = dev.alloc(1)
    dev.write_cluster(inner_c, INNER_TEXT)
    dz = bytearray()
    dz += dev.short_entry(b".          ", 0x10, docs_c, 0)
    dz += dev.short_entry(b"..         ", 0x10, 0, 0)
    dz += dev.dir_entry("inner.txt", b"INNER   TXT", 0x20, inner_c, len(INNER_TEXT))
    dev.write_cluster(docs_c, bytes(dz))
    ents += dev.dir_entry("Docs", b"DOCS       ", 0x10, docs_c, 0)
    dev.write_cluster(root, bytes(ents))
    img1 = dev.finish()

    # P2：4KB/簇
    big = Fat32Builder(PART2_LBA, PART2_SECTORS, SPC2)
    broot = big.alloc(1)
    bc = big.alloc(1)
    big.write_cluster(bc, BIG_TEXT)
    bents = big.dir_entry("BIGCLU.TXT", b"BIGCLU  TXT", 0x20, bc, len(BIG_TEXT))
    big.write_cluster(broot, bents)
    img2 = big.finish()

    # MBR：P1/P2 = 0x0C FAT32 LBA（P1 活动）
    full = bytearray(DISK_SECTORS * SECTOR)
    mbr = bytearray(SECTOR)
    e1 = bytearray(16)
    e1[0] = 0x80
    e1[4] = 0x0C
    struct.pack_into("<II", e1, 8, PART_LBA, PART_SECTORS)
    e2 = bytearray(16)
    e2[4] = 0x0C
    struct.pack_into("<II", e2, 8, PART2_LBA, PART2_SECTORS)
    mbr[446:462] = e1
    mbr[462:478] = e2
    mbr[510], mbr[511] = 0x55, 0xAA
    full[0:SECTOR] = mbr
    full[PART_LBA * SECTOR:PART_LBA * SECTOR + len(img1)] = img1
    full[PART2_LBA * SECTOR:PART2_LBA * SECTOR + len(img2)] = img2
    with open(path, "wb") as f:
        f.write(bytes(full))
    return {"clusters": dev.clusters, "fatsz": dev.fatsz, "spc": dev.spc,
            "clusters2": big.clusters, "spc2": big.spc}


# ---------------------------------------------------------------------------
def double_click(mon):
    mon.raw(["mouse_button 1", "mouse_button 0", "mouse_button 1", "mouse_button 0"],
            wait_between=0.10, wait_end=0.6)


def crc32_line(log, path):
    m = re.search(r"\[FAT64\] crc path=%s size=(\d+) crc32=([0-9A-Fa-f]+)" % re.escape(path), log)
    return (int(m.group(1)), int(m.group(2), 16)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (MEDIUM, os.path.join(BUILD, "BOOTX64.EFI"), os.path.join(BUILD, "UEFI64.BIN"),
                 os.path.join(BUILD, "kernel64_os.bin"), os.path.join(BUILD, "system.img")):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    tmp = tempfile.mkdtemp(prefix="vimtu64_fatread_")
    target = os.path.join(tmp, "installed.img")
    lfndisk = os.path.join(tmp, "lfn.img")
    inst_serial = os.path.join(tmp, "install.log")
    serial = os.path.join(tmp, "boot.log")
    shot = os.path.join(tmp, "esp_boot.ppm")

    print("=== 1) 造盘：真实安装（ESP）+ 宿主侧 VFAT 长名盘 ===")
    if not os.path.exists(target):
        open(target, "wb").write(b"\0" * (esp.TARGET_SECTORS * SECTOR))
    ilog, rebooted = esp.run_install(qemu, target, inst_serial, fst.free_port())
    check("真实安装完成（[INSTALL] 完成 + 自动重启）", "[INSTALL] 完成：已写" in ilog and rebooted)
    check("安装器写 ESP 的打点齐全（fs=FAT32 fat_ok=1 + 三文件）",
          "[INSTALL] esp: lba=" in ilog and "fs=FAT32" in ilog and "fat_ok=1" in ilog and
          "[INSTALL] esp files:" in ilog)
    info = make_lfn_disk(lfndisk)
    check("长名盘构造：48MB FAT32（簇数 %d >= 65525，SPC=%d）" % (info["clusters"], info["spc"]),
          info["clusters"] >= 65525 and info["spc"] == SPC)

    with open(target, "rb") as f:
        disk_before = f.read()
    m = re.search(r"\[INSTALL\] esp: lba=(\d+) sectors=(\d+)", ilog)
    check("安装日志里有 ESP 的 LBA/扇区数", m is not None)
    if not m:
        return 1
    esp_lba, esp_secs = int(m.group(1)), int(m.group(2))
    esp_before = disk_before[esp_lba * SECTOR:(esp_lba + esp_secs) * SECTOR]
    check("ESP 分区字节取出（lba=%d sectors=%d）" % (esp_lba, esp_secs), len(esp_before) == esp_secs * SECTOR)

    print("=== 2) 启动装好的盘 + 长名盘（两块 IDE 盘）===")
    port = fst.free_port()
    proc = __import__("subprocess").Popen(
        fst.qemu_args(qemu, [target, lfndisk], serial, port, "Vimtu64-fatread"),
        stdout=__import__("subprocess").DEVNULL, stderr=__import__("subprocess").DEVNULL)
    mon = ex.Monitor(port)          # explorer64_test 的 Monitor 有 raw（双击）与 shot（screendump）
    try:
        log = fst.wait_for(serial, "[GUI64] ready", 300, proc)
        check("桌面就绪（[GUI64] ready）", "[GUI64] ready" in log)

        # ---- 启动期 FAT 读取器证据 ----
        check("[FAT64] selftest PASS（写入器 + 新的只读读取器往返 + LFN 纯函数）",
              "[FAT64] selftest PASS" in log)
        check("[FS64] selftest PASS（统一卷表 + FAT 只读语义）", "[FS64] selftest PASS" in log)
        probes = re.findall(r"\[FAT64\] probe lba=(\d+) fs=(\w+) clusters=(\d+)", log)
        check("真实分区只读探测写明实际类型（[FAT64] probe … fs=FAT32）",
              any(fs == "FAT32" and int(cl) >= 65525 for _, fs, cl in probes), str(probes[:4]))
        check("[FAT64] mount … fat_ok=1 ro=1（两份 FAT 校验 + FSInfo 快照）",
              re.search(r"\[FAT64\] mount vol=\d+ lba=\d+ clusters=\d+ free=\d+ fat_ok=1", log) is not None)
        check("簇大小按 BPB 算：4KB/簇（SPC=8）的 FAT32 也挂上了（mount … spc=8 ro=1）",
              re.search(r"\[FAT64\] mount vol=\d+ lba=\d+ clusters=\d+ free=\d+ fat_ok=1 spc=8 ro=1", log) is not None)
        check("[FS64] mount fat … ro=1", "[FS64] mount fat vol=" in log and " ro=1" in log)
        fps = re.findall(r"\[DRV64\] letter=(\w): disk=(\d+) part=(\d+) fs=(\w+) total_kb=(\d+) free_kb=(\d+) slot=(\S+) ro=1 fatvol=(\d+)", log)
        check("FAT32 卷分到盘符且标只读（[DRV64] letter=… fs=FAT32 … slot=- ro=1 fatvol=…）", len(fps) >= 3, str(fps))
        esp_letter = lfn_letter = big_letter = None
        for l, diskid, part, fsname, _, _, _, _ in fps:
            if fsname != "FAT32":
                continue
            if diskid == "0":
                esp_letter = l
            elif part == "1":
                lfn_letter = l
            elif part == "2":
                big_letter = l
        check("ESP（disk=0）+ 长名盘（disk=1 part=1）+ 4KB/簇盘（disk=1 part=2）都拿到 FAT32 盘符",
              bool(esp_letter) and bool(lfn_letter) and bool(big_letter),
              "esp=%s lfn=%s big=%s" % (esp_letter, lfn_letter, big_letter))
        if not (esp_letter and lfn_letter and big_letter):
            return 1
        check("[DRV64] selftest PASS（FAT 条目也纳入 bit6 卷一致性）", "[DRV64] selftest PASS" in log)

        # ---- 终端 ----
        print("=== 3) 终端：vol / ls / fatcheck（CRC 逐字节）/ 只读拒绝 ===")
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial, proc))
        mon.type_line("vol")
        vlog = fst.wait_for(serial, "[VOL] vol letter=", 25, proc)
        check("vol 列出 FAT32 卷并标 ro（[VOL] vol letter=… fs=FAT32 ro=1 …）",
              re.search(r"\[VOL\] vol letter=\w: fs=FAT32 ro=1 total_kb=\d+ free_kb=\d+", vlog) is not None)
        check("vol 列表打点（[VOL] list n=… 含 FAT 卷）", re.search(r"\[VOL\] list n=\d+", vlog) is not None)

        mon.type_line("vol " + esp_letter.lower())
        slog = fst.wait_for(serial, "[VOL] switch letter=%s:" % esp_letter, 25, proc)
        check("切到 ESP（[VOL] switch letter=%s: slot=- …）" % esp_letter,
              re.search(r"\[VOL\] switch letter=%s: slot=-" % esp_letter, slog) is not None)
        mon.type_line("ls")
        llog = fst.wait_for(serial, "[TERM] cmd ls entries=", 30, proc)
        ml = re.findall(r"\[TERM\] cmd ls entries=(\d+)", llog)
        check("ls 列出 ESP 根目录 3 项（EFI/UEFI64.BIN/KERNEL64.BIN）", bool(ml) and ml[-1] == "3",
              "entries=%s" % (ml[-1] if ml else "?"))
        check("FAT 读取器列目录打点（[FAT64] list path=/ entries=3）",
              "[FAT64] list path=/ entries=3" in llog)

        mon.type_line("fatcheck")
        clog = fst.wait_for(serial, "[FAT64] crc path=/KERNEL64.BIN", 120, proc)
        stub_ref = open(os.path.join(BUILD, "BOOTX64.EFI"), "rb").read()
        uefi_ref = open(os.path.join(BUILD, "UEFI64.BIN"), "rb").read()
        kern_ref = open(os.path.join(BUILD, "kernel64_os.bin"), "rb").read()
        kern_region = kern_ref + b"\0" * (8000 * 512 - len(kern_ref))
        for p, ref in (("/EFI/BOOT/BOOTX64.EFI", stub_ref), ("/UEFI64.BIN", uefi_ref),
                       ("/KERNEL64.BIN", kern_region)):
            got = crc32_line(clog, p)
            want_crc = zlib.crc32(ref) & 0xFFFFFFFF
            check("★ ESP %s：size=%d crc32 与 build64/ 构建产物一致" % (p, len(ref)),
                  got == (len(ref), want_crc),
                  "内核 %s / 宿主 (%d, %08X)" % (got, len(ref), want_crc))

        before = len(fst.slog(serial)) if hasattr(fst, "slog") else 0
        mon.type_line("write x.txt hello")
        wlog = fst.wait_for(serial, "[TERM] cmd write fail", 25, proc)
        check("write 到 ESP 被拒（[TERM] cmd write fail + [FS64] reject … ro=1 op=write）",
              "[TERM] cmd write fail" in wlog and re.search(r"\[FS64\] reject vol=\d+ ro=1 op=write", wlog) is not None)
        check("被拒的写**没有**真的打开/创建文件（没有 [FD64] open path=/X.TXT）",
              "[FD64] open path=/x.txt" not in wlog)
        wlen = len(fst.slog(serial))
        mon.type_line("mkdir ZZ")
        mlog = fst.wait_for(serial, "[TERM] cmd mkdir fail", 25, proc)
        check("mkdir 到 ESP 被拒（[FS64] reject … op=mkdir）",
              "[TERM] cmd mkdir fail" in mlog and "op=mkdir" in mlog)
        mon.type_line("rm efi")
        rlog = fst.wait_for(serial, "[TERM] cmd rm fail", 25, proc)
        check("rm 在 ESP 上被拒（[FS64] reject … op=unlink，只读卷）",
              "[TERM] cmd rm fail" in rlog and "ro=1" in rlog)
        _ = before, wlen

        # ---- LFN：长名盘的 list/cat/长名路径 ----
        print("=== 4) LFN：长名盘（VFAT 长文件名 / 中文长名 / 短名 / 子目录）===")
        mon.type_line("vol " + lfn_letter.lower())
        fst.wait_for(serial, "[VOL] switch letter=%s:" % lfn_letter, 25, proc)
        mon.type_line("ls")
        lfn_ls = fst.wait_for(serial, "[FAT64] list path=/ entries=", 30, proc)
        check("长名盘根目录列出 4 项（2 长名 + 1 短名 + Docs；卷标/删除项被跳过）",
              "[FAT64] list path=/ entries=4" in lfn_ls)
        mon.type_line("fatcheck " + LONGNAME.lower())
        lc = fst.wait_for(serial, "[FAT64] crc path=/" + LONGNAME.lower(), 40, proc)
        got = crc32_line(lc, "/" + LONGNAME.lower())
        check("★ 长名路径能解析并读到正确字节（%s size=%d crc 一致）" % (LONGNAME, len(LONG_TEXT)),
              got == (len(LONG_TEXT), zlib.crc32(LONG_TEXT) & 0xFFFFFFFF), "%s" % (got,))
        # 中文长名：QEMU sendkey 打不进中文，所以由**内核自检的 UTF-16 -> UTF-8 用例**
        # （[FAT64] selftest PASS 里 bit24 那条）+ 下面 explorer 的 item 打点（完整中文长名）验证。
        check("★ 中文长名由内核 LFN 自检 + explorer item 打点验证（sendkey 无法输入中文）",
              "[FAT64] selftest PASS" in clog)

        # ---- 4KB/簇的 FAT32（P2）：证明簇大小按 BPB 算 ----
        mon.type_line("vol " + big_letter.lower())
        fst.wait_for(serial, "[VOL] switch letter=%s:" % big_letter, 25, proc)
        mon.type_line("fatcheck bigclu.txt")
        bc_log = fst.wait_for(serial, "[FAT64] crc path=/bigclu.txt", 40, proc)
        gotb = crc32_line(bc_log, "/bigclu.txt")
        check("★ 4KB/簇（SPC=8）卷上的文件也能按 BPB 算出的簇大小读对（BIGCLU.TXT crc 一致）",
              gotb == (len(BIG_TEXT), zlib.crc32(BIG_TEXT) & 0xFFFFFFFF), "%s" % (gotb,))
        check("长名盘分区大小自洽（P1 clusters=%d >= 65525 / P2 clusters=%d >= 65525）"
              % (info["clusters"], info["clusters2"]),
              info["clusters"] >= 65525 and info["clusters2"] >= 65525)

        # ---- Explorer 端到端（鼠标注入 + 像素）----
        print("=== 5) Explorer：进入 ESP（EFI/BOOT）、二进制 noassoc、长名预览、只读置灰 ===")
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=1.0)
            mon.key("2", wait=2.5)
            if "[APP] mypc opened" in fst.wait_for(serial, "[APP] mypc opened", 15, proc):
                opened = True
                break
        check("打开文件管理器（[APP] mypc opened）", opened)
        elog = fst.wait_for(serial, "[UI] explorer thispc", 25, proc)
        cards = re.findall(r"\[UI\] explorer card idx=(\d+) letter=(\S+) kind=(\S+)", elog)
        esp_idx = None
        for i, l, kind in cards:
            if l == esp_letter + ":" and kind == "browsable":
                esp_idx = int(i)
        if esp_idx is None:
            esp_idx = 0
        check("此电脑页有 ESP 卡片（card idx=… letter=%s: kind=browsable）" % esp_letter, esp_idx is not None,
              str(cards))
        lfn_idx = None
        for i, l, kind in cards:
            if l == lfn_letter + ":" and kind == "browsable":
                lfn_idx = int(i)

        ex.blind_goto(mon, *ex.card_center(esp_idx))
        double_click(mon)
        elog = fst.wait_for(serial, "[UI] explorer enter letter=%s:" % esp_letter, 30, proc)
        check("双击 ESP 卡片进入（[UI] explorer enter letter=%s: fatvol=… ok items=3）" % esp_letter,
              re.search(r"\[UI\] explorer enter letter=%s: fatvol=\d+ ok items=3" % esp_letter, elog) is not None)
        check("★ 只读卷打点（[UI] explorer vol letter=%s: fs=FAT32 ro=1 …）" % esp_letter,
              re.search(r"\[UI\] explorer vol letter=%s: fs=FAT32 ro=1" % esp_letter, elog) is not None)
        check("ESP 根目录列出 EFI（[UI] explorer item … name=EFI type=dir）",
              re.search(r"\[UI\] explorer item idx=\d+ name=EFI type=dir", elog) is not None)
        check("导航打点（[UI] explorer nav path=/ items=3）",
              re.search(r"\[UI] explorer nav path=/ items=3 view=\w+".replace("[UI]", "[UI\\]"), elog) is not None)

        # EFI -> BOOT：进目录用**键盘回车**（选定当前条目并打开），避开盲走鼠标的漂移
        mon.key("ret", wait=1.6)                       # EFI
        elog = fst.wait_for(serial, "[UI] explorer nav path=/EFI", 30, proc)
        check("进入 EFI 目录（nav path=/EFI）",
              re.search(r"\[UI\] explorer nav path=/EFI items=\d+ view=\w+", elog) is not None)
        mon.key("ret", wait=1.6)                       # BOOT
        elog = fst.wait_for(serial, "[UI] explorer nav path=/EFI/BOOT", 30, proc)
        check("进入 BOOT 目录（nav path=/EFI/BOOT）",
              re.search(r"\[UI\] explorer nav path=/EFI/BOOT items=\d+ view=\w+", elog) is not None)
        check("★ BOOTX64.EFI 在列（[UI] explorer item … name=BOOTX64.EFI）",
              re.search(r"\[UI\] explorer item idx=\d+ name=BOOTX64\.EFI type=file size=\d+", elog) is not None)
        check("EFI/BOOT 列出 1 个文件（list path=/EFI/BOOT entries=1）",
              "[FAT64] list path=/EFI/BOOT entries=1" in elog)
        check("目录页截图（ESP 的 EFI/BOOT）", mon.shot(shot, wait=2.5))
        if os.path.exists(shot):
            w, h, px = ex.read_ppm(shot)
            check("截图分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
            names = ex.rect_dark(px, w, ex.sx(ex.CONTENT_X + 4), ex.sy(ex.CONTENT_Y + 4),
                                 ex.sx(ex.CONTENT_X + ex.ICON_CELL_W - 4),
                                 ex.sy(ex.CONTENT_Y + ex.ICON_CELL_H - 4))
            check("内容区画了条目（图标单元格有暗像素）", names > 30, "暗像素=%d" % names)

        # 只会前进不会开条目：先"下"选中 BOOTX64.EFI，再 F2/Delete（只读卷 -> roact），最后回车 -> noassoc
        mon.key("down", wait=1.0)
        mon.key("f2", wait=1.0)
        mon.key("delete", wait=1.0)
        rlog = fst.wait_for(serial, "[UI] explorer roact op=delete", 20, proc)
        check("只读卷上 F2/Delete 被拒并有打点（[UI] explorer roact op=rename/mkdir / op=delete … ro=1）",
              "[UI] explorer roact op=rename/mkdir" in rlog and "[UI] explorer roact op=delete" in rlog and
              re.search(r"\[UI\] explorer roact op=delete letter=%s: fs=FAT32 ro=1" % esp_letter, rlog) is not None)
        mon.key("ret", wait=1.6)                       # 打开 BOOTX64.EFI -> 没有关联应用
        nlog = fst.wait_for(serial, "[UI] explorer noassoc name=BOOTX64.EFI", 25, proc)
        check("双击二进制 -> 没有关联的应用打开此文件（noassoc）",
              re.search(r"\[UI\] explorer noassoc name=BOOTX64\.EFI kind=\w+", nlog) is not None)

        # 回此电脑（工具栏"计算机"按钮，最多试 3 次）-> 进长名盘 -> 双击长名 .txt -> 文本预览
        back_ok = False
        for _ in range(3):
            before_tp = fst.slog(serial).count("[UI] explorer thispc")
            ex.blind_goto(mon, ex.sx(108), ex.sy(13))     # 工具栏"计算机"按钮（76..140）
            ex.single_click_at(mon)
            if fst.wait_for(serial, "[UI] explorer thispc", 12, proc) and \
               fst.slog(serial).count("[UI] explorer thispc") > before_tp:
                back_ok = True
                break
        check("回到此电脑（可重试）", back_ok)
        if lfn_idx is not None:
            ex.blind_goto(mon, *ex.card_center(lfn_idx))
            double_click(mon)
            plog = fst.wait_for(serial, "[UI] explorer enter letter=%s:" % lfn_letter, 30, proc)
            check("双击长名盘进入（enter letter=%s: fatvol=… items=4）" % lfn_letter,
                  re.search(r"\[UI\] explorer enter letter=%s: fatvol=\d+ ok items=4" % lfn_letter, plog) is not None)
            check("★ 串口打点里是**完整长名**（不是 LONGNA~1.TXT）：%s" % LONGNAME,
                  ("name=" + LONGNAME) in plog)
            check("★ 中文长名也在列：%s" % LONGNAME_CN, ("name=" + LONGNAME_CN) in plog)
            if re.search(r"\[UI\] explorer item idx=\d+ name=%s type=file" % re.escape(LONGNAME), plog):
                mon.key("ret", wait=1.6)          # 打开索引 0 = LongName-Document-2026.txt -> 文本预览
                pv = fst.wait_for(serial, "[UI] explorer preview name=" + LONGNAME, 30, proc)
                check("打开长名文本 -> 只读文本预览（preview name=%s bytes=%d）" % (LONGNAME, len(LONG_TEXT)),
                      re.search(r"\[UI\] explorer preview name=%s bytes=%d" % (re.escape(LONGNAME), len(LONG_TEXT)), pv) is not None)
            else:
                check("长名条目的 idx 可定位", False, "（缺 item 行）")
        else:
            check("长名盘卡片定位", False, "（缺 card 行）")

        print("--- 禁止项 ---")
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in fst.slog(serial))
    finally:
        fst.kill(proc)

    print("=== 6) 只读的最终证据：启动前后 ESP 分区字节哈希不变 ===")
    with open(target, "rb") as f:
        disk_after = f.read()
    esp_after = disk_after[esp_lba * SECTOR:(esp_lba + esp_secs) * SECTOR]
    h0 = zlib.crc32(esp_before) & 0xFFFFFFFF
    h1 = zlib.crc32(esp_after) & 0xFFFFFFFF
    check("ESP 分区（%d 字节）启动前后 CRC32 相同（没有任何写发生）" % len(esp_before), h0 == h1,
          "before=%08X after=%08X" % (h0, h1))

    if args.keep:
        print("[fatread] 串口日志：%s / 安装日志：%s / 截图：%s" % (serial, inst_serial, shot))
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
