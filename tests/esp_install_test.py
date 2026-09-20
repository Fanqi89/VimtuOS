#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/esp_install_test.py - 安装时建 ESP + GPT，并证明**装好的盘 UEFI/BIOS 双启动**

被验的需求：安装程序以前只写"MBR + 固定 LBA 引导区"，UEFI 固件在装好的盘上
看不到任何 FAT 卷 -> 装完的盘在 UEFI 机器上起不来。本测试验的是修好之后的行为：

  1) 造 64MB 空目标盘（挂在 **AHCI** 上，驱动器号 8）+ 安装介质（vimtu64-64.iso 当 IDE 盘）
  2) QEMU monitor 注入按键走完整安装（语言→现在安装→许可→类型→新建→安装）
  3) 目标盘字节断言：
       * MBR：P1 引导分区 0xEF 活动 9+8000 / P2 主分区 0x07 / P3 ESP 0xEF（盘尾）
       * 盘尾**备份 GPT**："EFI PART" + 头 CRC 正确 + 项数组 CRC 正确 +
         项1 主分区（基本数据 GUID）+ 项2 ESP（C12A7328-… GUID）
         （LBA 1 是 loader64.bin，主 GPT 头按规范应在那里 —— 冲突，所以只有备份头）
       * ESP 里的 FAT32 卷：BPB 自洽（FATSz16=0 / FATSz32 / RootClus=2 / FSInfo=1 /
         BkBootSec=6、簇数 >= 65525、两份 FAT 逐字节一致、FSInfo 三个签名与 free/next-free）、
         根目录簇链（从簇 2 开始）、目录结构 EFI/BOOT/、"." / ".." 齐备
         （复用 tools/fat_check.py 做规范体检）
       * ESP 里三个文件与本地构建产物**逐字节一致**：
         EFI/BOOT/BOOTX64.EFI == build64/BOOTX64.EFI
         UEFI64.BIN           == build64/UEFI64.BIN
         KERNEL64.BIN         == build64/kernel64_os.bin（补零到 4MB 内核区）
  4) **UEFI（OVMF）从这块装好的盘启动** -> "[OS] booted from installed disk" + "[GUI64] ready"
  5) **BIOS（SeaBIOS）从同一块盘启动** -> 同样进系统（这就是"新老设备都能启动"的直接证据）

用法：python tests/esp_install_test.py [--qemu ...] [--target ...] [--keep]
退出码：0 全部通过 / 1 有断言失败 / 2 环境问题
"""
import argparse
import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import proc64_test as p64          # noqa: E402  （OVMF 路径/变量卷/qemu 参数复用）

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build64")
MEDIUM = os.path.join(ROOT, "vimtu64-64.iso")
OS_KERNEL = os.path.join(BUILD, "kernel64_os.bin")
STUB = os.path.join(BUILD, "BOOTX64.EFI")
UEFI64 = os.path.join(BUILD, "UEFI64.BIN")
LOADER = os.path.join(BUILD, "loader64.bin")

SECTOR = 512
TARGET_SECTORS = 131072          # 64MB（要放得下 4MB 引导区 + 主分区 8MB 起 + 48MB FAT32 ESP
                                #  + 盘尾备份 GPT；真 FAT32 的簇数硬下限让最小目标盘 ~60MB）
PART_BOOT_LBA, PART_BOOT_SECS = 9, 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS        # 8009
ESP_SECTORS_EXPECT = 98304                            # kernel/part64.h 的 PART_ESP_SECTORS（48MB）
GPT_BACKUP = 33
GPT_TYPE_ESP = bytes.fromhex("28732ac1" "1ff8" "d211" "ba4b" "00a0c93ec93b")
GPT_TYPE_BASIC = bytes.fromhex("a2a0d0eb" "e5b9" "3344" "87c0" "68b6b72699c7")

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "三重故障"]


def q(p):
    return p.replace("\\", "/")


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_log(path, needle, seconds):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if needle in slog(path):
            return True
        time.sleep(0.5)
    return False


class Monitor:
    def __init__(self, port):
        self.port = port

    def key(self, name, wait=1.2):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=6)
        except OSError:
            return
        try:
            s.sendall(("sendkey %s\n" % name).encode())
            time.sleep(wait)
        finally:
            s.close()


def run_install(qemu, target, serial, port, dwell=1.2):
    """ISO 当 IDE 盘 + 目标盘挂 AHCI：注入按键走完安装。返回 (日志, 是否自动重启)。"""
    if os.path.exists(serial):
        os.remove(serial)
    args = [
        qemu, "-name", "VimtuOS-esp-install",
        "-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d0,format=raw" % q(target),
        "-device", "ide-hd,drive=d0,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = Monitor(port)
    rebooted = False
    try:
        for _ in range(120):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        if not wait_log(serial, "磁盘枚举完成", 300):
            print("   [!!] 等不到向导就绪（磁盘枚举完成）")
            return slog(serial), False
        # 向导默认光标已经落在"不是安装介质的盘"（8 号 AHCI 盘）上 —— 与 ahci64_test 同口径
        for name, what in (("ret", "语言->现在安装"), ("ret", "现在安装->许可"),
                           ("ret", "许可->安装类型"), ("ret", "类型->磁盘与分区"),
                           ("n", "新建（写 MBR 引导区+主分区+ESP 项）"), ("ret", "安装系统")):
            print("     sendkey %-4s (%s)" % (name, what))
            mon.key(name, wait=dwell)
        rebooted = wait_log(serial, "[SETUP] 自动重启", 420)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return slog(serial), rebooted


def boot_disk(qemu, img, serial, tag, ovmf=None, timeout=300):
    """从一块盘启动：BIOS 或 UEFI（ovmf=code fd 路径）。返回串口日志。"""
    if os.path.exists(serial):
        os.remove(serial)
    args = [qemu, "-name", "VimtuOS-" + tag, "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(serial), "-no-reboot",
            "-drive", "format=raw,file=%s" % q(img), "-boot", "order=c"]
    if ovmf:
        args += ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % q(ovmf),
                 "-drive", "if=pflash,format=raw,unit=1,file=%s" % q(p64._ovmf_vars())]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + timeout
        while time.time() < deadline:
            s = slog(serial)
            if "[GUI64] ready" in s:
                time.sleep(2.0)
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return slog(serial)


# ---------------------------------------------------------------------------
# 宿主侧解析：MBR / 盘尾备份 GPT / FAT32 卷（独立于内核实现，用于交叉验证）
# ---------------------------------------------------------------------------
def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def mbr_entries(b):
    out = []
    for i in range(4):
        e = b[446 + i * 16: 446 + i * 16 + 16]
        out.append({"boot": e[0] == 0x80, "type": e[4],
                    "start": u32(e, 8), "sectors": u32(e, 12)})
    return out


def parse_gpt_backup(disk):
    """解析盘尾备份 GPT（头 + 项数组），校验两个 CRC。返回 (info, err)。"""
    total = len(disk) // SECTOR
    h = disk[(total - 1) * SECTOR: total * SECTOR]
    if h[0:8] != b"EFI PART":
        return None, "盘尾没有 EFI PART"
    hdr_size = u32(h, 12)
    hdr_crc = u32(h, 16)
    # ★ GPT 规范：头的 CRC32 是"把 HeaderCRC32 字段本身当 0"之后算出来的
    #   （UEFI 规范 "with the CRC32 field set to zero"；EDK2 与 make_iso64.py 都这么做）
    h_zero = bytearray(h)
    h_zero[16:20] = b"\0\0\0\0"
    if zlib.crc32(bytes(h_zero[:hdr_size])) & 0xFFFFFFFF != hdr_crc:
        return None, "备份 GPT 头 CRC 不符"
    my_lba, alt_lba = u64(h, 24), u64(h, 32)
    first_usable, last_usable = u64(h, 40), u64(h, 48)
    ent_lba = u64(h, 72)
    n_ent, ent_size = u32(h, 80), u32(h, 84)
    ent_crc = u32(h, 88)
    ents = disk[ent_lba * SECTOR: ent_lba * SECTOR + n_ent * ent_size]
    if zlib.crc32(ents) & 0xFFFFFFFF != ent_crc:
        return None, "备份 GPT 项数组 CRC 不符"
    parts = []
    for i in range(n_ent):
        e = ents[i * ent_size: (i + 1) * ent_size]
        if e[0:16] == b"\0" * 16:
            continue
        parts.append({"type": e[0:16], "first": u64(e, 32), "last": u64(e, 40),
                      "name": e[56:56 + 72].decode("utf-16-le").rstrip("\0")})
    return {"my_lba": my_lba, "alt_lba": alt_lba, "first_usable": first_usable,
            "last_usable": last_usable, "ent_lba": ent_lba, "n_ent": n_ent,
            "ent_size": ent_size, "parts": parts, "total": total}, ""


class Fat32:
    """宿主侧 FAT32 只读解析器（独立于内核实现，用于交叉验证）：
    根目录是**簇链**（BPB_RootClus，通常 2）、FAT 项 32 位（只用低 28 位）、
    目录项的起始簇是 32 位（低 16 位 @26 + 高 16 位 @20）、FSInfo 在保留扇区里。
    """

    def __init__(self, b):
        self.b = b
        self.bytes_per_sec = u16(b, 11)
        self.spc = b[13]
        self.rsvd = u16(b, 14)
        self.nfats = b[16]
        self.root_ent = u16(b, 17)
        self.fatsz16 = u16(b, 22)
        self.tot = u16(b, 19) or u32(b, 32)
        self.fatsz = u32(b, 36)          # BPB_FATSz32
        self.ext_flags = u16(b, 40)
        self.fs_ver = u16(b, 42)
        self.root_clus = u32(b, 44)      # BPB_RootClus
        self.fsinfo_sec = u16(b, 48)     # BPB_FSInfo
        self.bkboot = u16(b, 50)         # BPB_BkBootSec
        self.drvnum = b[64]
        self.bootsig = b[66]
        self.volid = u32(b, 67)
        self.label = b[71:82].decode("latin-1").rstrip("\x00 ")
        self.fstype = b[82:90].decode("latin-1").rstrip("\x00 ")
        self.data_start = self.rsvd + self.nfats * self.fatsz      # FAT32 没有固定根目录区
        self.cluster_bytes = self.bytes_per_sec * self.spc
        self.clusters = (self.tot - self.data_start) // self.spc
        self.fat_off = self.rsvd * self.bytes_per_sec

    def fat_entry(self, c):
        return u32(self.b, self.fat_off + c * 4) & 0x0FFFFFFF

    def fsinfo(self):
        off = self.fsinfo_sec * self.bytes_per_sec
        s = self.b[off:off + self.bytes_per_sec]
        return {"lead": u32(s, 0), "struc": u32(s, 484), "trail": u32(s, 508),
                "free": u32(s, 488), "next": u32(s, 492),
                "sig55": s[510] == 0x55 and s[511] == 0xAA, "raw": s}

    def entry_at(self, off):
        e = self.b[off:off + 32]
        if len(e) < 32 or e[0] == 0 or e[0] == 0xE5 or e[11] == 0x0F:
            return None
        base = e[0:8].decode("latin-1").rstrip()
        ext = e[8:11].decode("latin-1").rstrip()
        name = base + ("." + ext if ext else "")
        cluster = (u32(e, 20) & 0xFFFF0000) | u16(e, 26)          # ★ 32 位簇号
        return {"name": name, "attr": e[11], "cluster": cluster, "size": u32(e, 28),
                "clus_hi": u16(e, 20), "clus_lo": u16(e, 26)}

    def chain(self, c, guard=300000):
        out = []
        while 2 <= c <= self.clusters + 1 and guard > 0:
            out.append(c)
            c = self.fat_entry(c)
            guard -= 1
        return out

    def dir_bytes(self, cluster):
        data = bytearray()
        for c in self.chain(cluster):
            off = (self.data_start + (c - 2) * self.spc) * self.bytes_per_sec
            data += self.b[off:off + self.cluster_bytes]
        return bytes(data)

    def list_dir(self, cluster):
        out = []
        data = self.dir_bytes(cluster)
        for i in range(len(data) // 32):
            if data[i * 32] == 0:
                break

            raw = data[i * 32:i * 32 + 32]
            if raw[0] == 0xE5 or raw[11] == 0x0F:
                continue
            base = raw[0:8].decode("latin-1").rstrip()
            ext = raw[8:11].decode("latin-1").rstrip()
            name = base + ("." + ext if ext else "")
            out.append({"name": name, "attr": raw[11],
                        "cluster": (u32(raw, 20) & 0xFFFF0000) | u16(raw, 26),
                        "size": u32(raw, 28), "clus_hi": u16(raw, 20), "clus_lo": u16(raw, 26)})

        return out

    def find(self, path_parts):
        cur = self.root_clus
        found = None
        for want in path_parts:
            found = None
            for e in self.list_dir(cur):
                if e["name"].upper() == want.upper():
                    found = e
                    break
            if found is None:
                return None
            cur = found["cluster"]
        return found

    def read_file(self, entry):
        data = bytearray()
        for c in self.chain(entry["cluster"]):
            off = (self.data_start + (c - 2) * self.spc) * self.bytes_per_sec
            data += self.b[off:off + self.cluster_bytes]
        return bytes(data[:entry["size"]]), entry["size"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--target", default=os.path.join(ROOT, "target-esp-installed.img"))
    ap.add_argument("--port", type=int, default=5562)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (MEDIUM, OS_KERNEL, STUB, UEFI64, LOADER):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 build64.sh）\n" % need)
            return 2
    ovmf = p64.find_ovmf()
    if not ovmf:
        sys.stderr.write("找不到 OVMF（%s）\n" % ", ".join(p64.OVMF_CANDIDATES))
        return 2

    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    loader_ref = open(LOADER, "rb").read()
    serial = os.path.join(ROOT, "esp_install_serial.log")
    print("=== 1) 造 %d 扇区（%dMB）空目标盘（AHCI 8 号盘）==="
          % (TARGET_SECTORS, TARGET_SECTORS // 2048))
    with open(args.target, "wb") as f:
        f.write(b"\0" * (TARGET_SECTORS * SECTOR))

    print("=== 2) 走完整安装（新建分区 + 安装系统）===")
    log, rebooted = run_install(qemu, args.target, serial, args.port)
    if args.keep:
        print("     串口日志：%s" % serial)

    print("=== 3) 安装动作 + 打点断言 ===")
    check("FAT32 写入器自检 PASS", "[FAT64] selftest PASS" in log)
    check("新建分区表 OK（含 ESP 项）", "[PART] 新建分区表 OK" in log and "esp=" in log,
          [l for l in log.splitlines() if "[PART] 新建分区表" in l][:1])
    check("ESP 格式化（[INSTALL] esp: … fs=FAT32 … fat_ok=1）",
          "[INSTALL] esp: lba=" in log and "fs=FAT32" in log and "fat_ok=1" in log,
          [l for l in log.splitlines() if "[INSTALL] esp:" in l][:1])
    check("ESP 三文件写入打点（BOOTX64.EFI/UEFI64.BIN/KERNEL64.BIN 字节数）",
          "[INSTALL] esp files: BOOTX64.EFI=" in log and "UEFI64.BIN=" in log
          and "KERNEL64.BIN=" in log,
          [l for l in log.splitlines() if "[INSTALL] esp files:" in l][:1])
    check("GPT + 混合 MBR 打点（[INSTALL] gpt written (main + esp), pmbr ok）",
          "[INSTALL] gpt written (main + esp), pmbr ok" in log,
          [l for l in log.splitlines() if "gpt written" in l][:1])
    check("安装完成并自动重启", "[INSTALL] 完成：已写" in log and rebooted)
    check("无 PANIC / 三重故障", not any(x in log for x in FORBIDDEN))

    print("=== 4) 目标盘字节断言：MBR / 盘尾备份 GPT / ESP ===")
    with open(args.target, "rb") as f:
        disk = f.read()
    check("目标盘大小 = %d 扇区" % TARGET_SECTORS, len(disk) == TARGET_SECTORS * SECTOR)

    mbr = mbr_entries(disk[0:SECTOR])
    check("MBR 签名 55AA", disk[510] == 0x55 and disk[511] == 0xAA)
    check("P1 = 引导分区 0xEF 活动 9+8000",
          mbr[0]["type"] == 0xEF and mbr[0]["boot"] and mbr[0]["start"] == PART_BOOT_LBA
          and mbr[0]["sectors"] == PART_BOOT_SECS, "%s" % (mbr[0],))
    check("P2 = 主分区 0x07，起点 8009",
          mbr[1]["type"] == 0x07 and mbr[1]["start"] == PART_MAIN_LBA, "%s" % (mbr[1],))
    check("P3 = ESP 0xEF（盘尾、GPT 备份表之前，%d 扇区）" % ESP_SECTORS_EXPECT,
          mbr[2]["type"] == 0xEF and mbr[2]["sectors"] == ESP_SECTORS_EXPECT
          and mbr[2]["start"] + mbr[2]["sectors"] == TARGET_SECTORS - GPT_BACKUP,
          "%s" % (mbr[2],))
    check("P2 主分区与 ESP 不重叠",
          mbr[1]["start"] + mbr[1]["sectors"] <= mbr[2]["start"],
          "main_end=%d esp_start=%d" % (mbr[1]["start"] + mbr[1]["sectors"], mbr[2]["start"]))
    check("LBA 1..8 仍是 loader64.bin（主 GPT 头的规范位置与 loader 冲突 -> 只写备份头）",
          disk[SECTOR:SECTOR + len(loader_ref)] == loader_ref)

    gpt, err = parse_gpt_backup(disk)
    check("盘尾有备份 GPT（EFI PART + 头/项数组两个 CRC 正确）", gpt is not None, err)
    if gpt:
        check("备份头 MyLBA = 最后一块盘、AlternateLBA = 1",
              gpt["my_lba"] == gpt["total"] - 1 and gpt["alt_lba"] == 1,
              "my=%d alt=%d total=%d" % (gpt["my_lba"], gpt["alt_lba"], gpt["total"]))
        types = [p["type"] for p in gpt["parts"]]
        check("GPT 项1 = 主分区（基本数据 GUID，8009 起）",
              len(types) >= 1 and types[0] == GPT_TYPE_BASIC
              and gpt["parts"][0]["first"] == PART_MAIN_LBA, "%s" % (gpt["parts"][:1],))
        check("GPT 项2 = ESP（C12A7328-… GUID，盘尾 %d 扇区）" % ESP_SECTORS_EXPECT,
              len(types) >= 2 and types[1] == GPT_TYPE_ESP
              and gpt["parts"][1]["last"] - gpt["parts"][1]["first"] + 1 == ESP_SECTORS_EXPECT,
              "%s" % (gpt["parts"][1:2],))
        check("GPT 里的 ESP 与 MBR P3 位置一致",
              len(types) >= 2 and gpt["parts"][1]["first"] == mbr[2]["start"],
              "%s vs %s" % (gpt["parts"][1:2], mbr[2]))

    esp_start, esp_sectors = mbr[2]["start"], mbr[2]["sectors"]
    esp = disk[esp_start * SECTOR: (esp_start + esp_sectors) * SECTOR]
    fat = Fat32(esp)
    check("ESP BPB：512B 扇区 / SPC=1 / 2 份 FAT / FATSz16=0 / RootEntCnt=0",
          fat.bytes_per_sec == 512 and fat.spc == 1 and fat.nfats == 2
          and fat.fatsz16 == 0 and fat.root_ent == 0 and fat.fatsz != 0,
          "bps=%d spc=%d nfats=%d root=%d fatsz16=%d fatsz32=%d"
          % (fat.bytes_per_sec, fat.spc, fat.nfats, fat.root_ent, fat.fatsz16, fat.fatsz))
    check("ESP BPB：类型串 %r / 卷标 %r / 卷序号 0x%08X / 扩展引导签名 0x%02X"
          % (fat.fstype, fat.label, fat.volid, fat.bootsig),
          fat.fstype == "FAT32" and fat.label == "VIMTU64ESP"
          and fat.volid == 0x56494D54 and fat.bootsig == 0x29 and fat.drvnum == 0x80)
    check("★ ESP 是真 FAT32（簇数 %d >= 65525；少于这个数固件会按 FAT16 读）" % fat.clusters,
          fat.clusters >= 65525)
    check("ESP 根目录是簇链：RootClus=%d / FSInfo=%d / BkBootSec=%d"
          % (fat.root_clus, fat.fsinfo_sec, fat.bkboot),
          fat.root_clus == 2 and fat.fsinfo_sec == 1 and fat.bkboot == 6
          and fat.fat_entry(fat.root_clus) >= 0x0FFFFFF8)
    check("FAT[0]=0x0FFFFFF8 / FAT[1]=0x0FFFFFFF / 两份 FAT 逐字节一致",
          fat.fat_entry(0) == 0x0FFFFFF8 and fat.fat_entry(1) == 0x0FFFFFFF
          and esp[fat.fat_off:fat.fat_off + fat.fatsz * 512]
          == esp[fat.fat_off + fat.fatsz * 512:fat.fat_off + 2 * fat.fatsz * 512])
    fi = fat.fsinfo()
    check("FSInfo（扇区 %d）：0x41615252/0x61417272/0xAA550000 + free=%d next=%d"
          % (fat.fsinfo_sec, fi["free"], fi["next"]),
          fi["lead"] == 0x41615252 and fi["struc"] == 0x61417272
          and fi["trail"] == 0xAA550000 and fi["sig55"]
          and 0 < fi["free"] <= fat.clusters and 2 <= fi["next"] <= fat.clusters + 1)
    bkb = esp[fat.bkboot * 512:(fat.bkboot + 1) * 512]
    check("备份引导扇区（扇区 %d）逐字节等于 0 号扇区" % fat.bkboot, bkb == esp[0:512])
    root_ents = [e["name"] for e in fat.list_dir(fat.root_clus)]
    check("根目录簇（簇 %d）条目：%s" % (fat.root_clus, root_ents),
          "EFI" in root_ents and "UEFI64.BIN" in root_ents and "KERNEL64.BIN" in root_ents)
    efi_dir_ents = [e["name"] for e in fat.list_dir(fat.find(["EFI"])["cluster"])]
    check("EFI/ 目录含 \".\" / \"..\" / BOOT（FAT 规范硬要求）",
          "." in efi_dir_ents and ".." in efi_dir_ents and "BOOT" in efi_dir_ents,
          "%s" % efi_dir_ents)
    boot_dir_ents = [e["name"] for e in fat.list_dir(fat.find(["EFI", "BOOT"])["cluster"])]
    check("EFI/BOOT/ 目录含 \".\" / \"..\" / BOOTX64.EFI",
          "." in boot_dir_ents and ".." in boot_dir_ents and "BOOTX64.EFI" in boot_dir_ents,
          "%s" % boot_dir_ents)
    efi = fat.find(["EFI", "BOOT", "BOOTX64.EFI"])
    check("目录结构 EFI/BOOT/BOOTX64.EFI 存在（普通文件属性 0x20；32 位簇号 高=%d 低=%d）"
          % (efi["clus_hi"], efi["clus_lo"]) if efi else "EFI/BOOT/BOOTX64.EFI 缺失",
          efi is not None and (efi["attr"] & 0x20) and not (efi["attr"] & 0x10)
          and efi["cluster"] == ((efi["clus_hi"] << 16) | efi["clus_lo"]),
          "%s" % (efi,))

    print("--- tools/fat_check.py 规范体检（把 ESP 分区字节单独导出）---")
    with tempfile.TemporaryDirectory(prefix="vimtu_esp_") as td:
        esp_img = os.path.join(td, "esp.img")
        with open(esp_img, "wb") as f:
            f.write(esp)
        r = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "fat_check.py"), esp_img],
                           capture_output=True, timeout=120)
        # fat_check.py 现在强制按 UTF-8 写 stdout；稳妥起见两种编码都试一遍
        out = r.stdout.decode("utf-8", "replace")
        if "\u6ca1\u53d1\u73b0\u660e\u663e\u95ee\u9898" not in out:
            try:
                out = r.stdout.decode("gbk", "replace")
            except Exception:
                pass
        for line in out.splitlines():
            if any(k in line for k in ("文件系统", "簇数", "数据区", "根目录", "结论", "FAT[0]", "FAT[1]",
                                       "两个 FAT", "FSInfo", "备份引导", "没发现", "★", "[文件]", "[目录]")):
                print("   | " + line.strip()[:150])
        check("fat_check.py：识别为 FAT32 且列出三个文件",
              r.returncode == 0 and "文件系统=FAT32 簇数=" in out
              and "BOOTX64.EFI" in out and "KERNEL64.BIN" in out and "UEFI64.BIN" in out)
        check("fat_check.py：没发现明显问题", r.returncode == 0 and "没发现明显问题" in out)

    print("--- ESP 三文件与本地构建产物逐字节比对 ---")
    stub_ref = open(STUB, "rb").read()
    uefi_ref = open(UEFI64, "rb").read()
    kern_ref = open(OS_KERNEL, "rb").read()
    kregion = kern_ref + b"\0" * (8000 * 512 - len(kern_ref))    # 目标盘 LBA 9..8008 的整块
    if efi is not None:
        data, size = fat.read_file(efi)
        check("EFI/BOOT/BOOTX64.EFI = build64/BOOTX64.EFI（%d 字节）" % len(stub_ref),
              data == stub_ref, "盘上 %d 字节" % size)
    u = fat.find(["UEFI64.BIN"])
    check("UEFI64.BIN 存在", u is not None)
    if u:
        data, size = fat.read_file(u)
        check("UEFI64.BIN = build64/UEFI64.BIN（%d 字节）" % len(uefi_ref),
              data == uefi_ref, "盘上 %d 字节" % size)
    k = fat.find(["KERNEL64.BIN"])
    check("KERNEL64.BIN 存在", k is not None)
    if k:
        data, size = fat.read_file(k)
        check("KERNEL64.BIN = 系统内核整块（kernel64_os.bin + 补零 = %d 字节）" % len(kregion),
              data == kregion, "盘上 %d 字节" % size)
        check("KERNEL64.BIN 与目标盘 LBA 9.. 的内核区逐字节一致",
              data == disk[PART_BOOT_LBA * SECTOR: (PART_BOOT_LBA + 8000) * SECTOR])

    print("=== 5) ★ UEFI（OVMF）从这块**装好的盘**启动 ===")
    uefi_log = boot_disk(qemu, args.target, os.path.join(ROOT, "esp_installed_uefi.log"),
                         "esp-uefi", ovmf=ovmf)
    check("走了 UEFI 引导链（自研 BOOTX64.EFI + 平铺长模式引导器）",
          "U:==== Vimtu64 UEFI" in uefi_log and "U:loaded KERNEL64.BIN" in uefi_log)
    check("装好的盘上没有安装载荷（SYSTEM.IMG 缺失 -> 跳过 + kind=0）",
          "U:missing SYSTEM.IMG (optional, skipped)" in uefi_log
          and "U:medium desc kind=0" in uefi_log)
    check("★ [OS] booted from installed disk（UEFI）",
          "[OS] booted from installed disk" in uefi_log)
    check("★ [GUI64] ready（UEFI 进桌面）", "[GUI64] ready" in uefi_log)
    check("UEFI 启动无 PANIC/三重故障", not any(x in uefi_log for x in FORBIDDEN))
    with open(args.target, "rb") as f:
        after = f.read()
    check("UEFI 启动后 LBA 1..8 的 loader 字节没被固件改写（没有 GPT 修复式回写）",
          after[SECTOR:SECTOR + len(loader_ref)] == loader_ref)

    print("=== 6) ★ BIOS 从**同一块**装好的盘启动 ===")
    bios_log = boot_disk(qemu, args.target, os.path.join(ROOT, "esp_installed_bios.log"), "esp-bios")
    check("BIOS 走 MBR -> loader64 -> INT 13h 读内核",
          "[LM] disk boot via INT 13h" in bios_log)
    check("★ [OS] booted from installed disk（BIOS）",
          "[OS] booted from installed disk" in bios_log)
    check("★ [GUI64] ready（BIOS 进桌面）", "[GUI64] ready" in bios_log)
    check("BIOS 启动无 PANIC/三重故障", not any(x in bios_log for x in FORBIDDEN))

    print("=== ESP/双固件 RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    if not ok:
        print("--- 安装串口尾部 ---")
        for l in [x for x in log.splitlines() if x.strip()][-14:]:
            print("   | " + l[:150])
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
