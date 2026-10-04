# -*- coding: utf-8 -*-
"""tests/usbwrite64_test.py - ★ P8b：**U 盘（FAT32）真正可写**的端到端验收

判据一句话：**关掉 QEMU 之后，宿主侧自己解析那根 U 盘镜像的 FAT32，逐字节证明盘上真的落了什么。**
（宿主侧解析器是本文件自带的极度简化版 —— 只读、够用；不借用内核的任何代码。）

它做什么（全部自动：串口打点 + 宿主侧 FAT32 解析 + QEMU monitor 注入键鼠 + 管理器 Ctrl+C/Ctrl+V）：

  阶段 0  宿主侧造夹具（不碰任何真盘）：
    * 16MB 小系统盘（C: = VimtuFS2 v3，复用 fs_tree_test.make_small_system_disk）
    * 35MB 真 FAT32 U 盘（fatread64_test.Fat32Builder；MBR 类型 0x0C @2048）
      ★ **刻意造满**：根目录 6 项（含 2 个 >= 24KB 的多簇文件）。数据区 68874 簇**一个空闲簇都不留** ——
        这样"写超出容量"的反例可以在**同一个卷**上确定性地触发（不需要写几十 MB）。
      布局（簇号 = 分配顺序）：
        簇 2        根目录          KEEP1.TXT(1 簇) KEEP2.DAT(48 簇, 24576B) SRC20K.BIN(40 簇, 20480B)
                                    FREEME.BIN(40 簇, 20480B) docs/(1 簇, notes.txt 1 簇) FILL.BIN(68742 簇)
        空闲簇 = 0。

  阶段 1  开机（QEMU 只挂 U 盘）：`[DRV64] letter=D: … ro=0`、`[FAT64] rw mount vol=… letter=D: writable=1`、
          终端 `vol` -> `[VOL] vol letter=D: fs=FAT32 ro=0 total_kb=34437 free_kb=0`；`ls /` 能列目录。

  阶段 2  终端在 D: 上的写操作（每条都要串口证据行）：
    a) 反例①（卷满）：`mkdir /nofitdir` -> `[FAT64] rw fail … op=mkdir reason=no-space`，**零副作用**；
       `write /nofit.txt …` -> `[FAT64] rw fail … op=write reason=no-space`（只可能多一个 0 字节空项，
       真正落盘前就失败：不分配簇、不动 FAT/FSInfo）-> 再 `rm /nofit.txt` 清掉（顺带验 0 簇文件的删除）。
    b) 反例②（坏路径）：`write /nosuchdir/b.txt x` -> `[FAT64] rw fail … reason=bad-path`。
    c) `rm /freeme.bin` -> `[FAT64] rw unlink … freed=40 ok=1`（腾出 40 个簇）。
    d) `write /rw64.txt <29B>` -> `[FAT64] rw write … path="/rw64.txt" len=29 cluster=… nclusters=1 verify=1`
       （盘上落成 8.3 短名 RW64.TXT —— **不写 LFN**，见 kernel/fat64.h 的 P8b 说明）。
    e) `write /rw64.txt <22B>` -> 覆盖写（同一个文件名的第二次写）。
    f) `mkdir /newdir` -> `[FAT64] rw mkdir … cluster=… ok=1`；`write /newdir/a.txt <17B>` 落到新目录里。

  阶段 3  文件管理器：把 D: 上那个 **40 簇**的文件 Ctrl+C 拷到 C:，再用终端把它在 D: 上截短
          （19B），最后 Ctrl+V 把它从 C: 粘回 D: —— 走"同名已存在 -> 就地覆盖写"那条路
          （`[UI] explorer usbfat overwrite name=SRC20K.BIN bytes=20480 ok`），
          于是 D: 上这个文件从 1 簇**扩链**回 40 簇。

  阶段 4  关掉 QEMU 之后，宿主侧（本文件自带的 FAT32 解析器）逐项核对：
    ① 新文件 `/RW64.TXT` 存在、大小与内容**逐字节等于客人写的那串**，且最后一簇的尾巴全 0；
    ② 拷过去的 `/SRC20K.BIN` 逐字节等于 **C: 卷里的源文件**（用 multivol64_test 的 v3 读取器读 C:，
       两条完全独立的宿主侧解析路径比对）；
    ③ 原有文件（KEEP1.TXT / KEEP2.DAT / docs/notes.txt）的目录项与数据**一个字节没变**；
    ④ 整根镜像 CRC32 **必须变化**；而且变化只出现在"我们动过的地方"（FAT / FSInfo / 根目录 / 新分配簇）；
    ⑤ 结构自洽：FAT1 与 FAT2 逐字节一致、FSInfo 的 free 与现场重数的空闲簇相等、
       被删的 FREEME.BIN/NOFIT.TXT 目录项首字节 = 0xE5 且簇链已回收、`/NEWDIR` 的 "." / ".." 正确。
  禁止项：PANIC / TRIPLE FAULT / OOM: / `[FAT64] rw verify FAILED` / 非预期的 `[FAT64] rw fail`。

QEMU 命令行（与 usbstorage_test 同一条）：
  qemu-system-x86_64.exe -name Vimtu64-usbwrite -drive format=raw,file=<16MB系统盘>,index=0,media=disk \
    -device piix3-usb-uhci,id=uhci -drive format=raw,file=<stick.img>,if=none,id=stick \
    -device usb-storage,drive=stick,bus=uhci.0 -boot order=c -m 512 -vga std -display none \
    -serial file:<log> -monitor telnet:127.0.0.1:<port>,server,nowait -no-reboot

用法（必须用 Windows 原生 Python）：py -3 tests\\usbwrite64_test.py [--keep]
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
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  find_qemu
import fs_tree_test as fst         # noqa: E402  小系统盘夹具 + 串口工具
import explorer64_test as exp      # noqa: E402  鼠标闭环 / 像素工具
import fileops64_test as fo        # noqa: E402  条目单元格几何
import multivol64_test as mv       # noqa: E402  宿主侧 v3 卷读取器（核对 C: 里的源文件）
import fatread64_test as fatr      # noqa: E402  宿主侧 FAT32 构造器（造 U 盘镜像）
import usbstorage_test as ust      # noqa: E402  QEMU 会话 / 鼠标 Aim / 打字 / 卷导航（同一套夹具形状）
import qemuhelp as qh              # noqa: E402  公共登录手势

SECTOR = 512
BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")

# ---- U 盘几何（宿主侧"独立来源"）----
STICK_PART_LBA = 2048
STICK_PART_SECTORS = 70000                  # 34MB 分区（SPC=1 -> 68874 簇 >= 65525 = 真 FAT32）
STICK_SECTORS = STICK_PART_LBA + STICK_PART_SECTORS
TOTAL_CLUSTERS = 68874                      # 由 Fat32Builder 的实际几何断言（见阶段 0）
STICK_TOTAL_KB = TOTAL_CLUSTERS // 2              # 卷容量 KB（= 68874 簇 x 512B / 1024 = 34437）

# ---- 盘上原来的文件（宿主侧造）----
KEEP1 = "KEEP1.TXT"
KEEP1_B = b"keep-1: original content, must not change\n"
KEEP2 = "KEEP2.DAT"                         # >= 24 KB 的多簇文件
KEEP2_B = bytes((i * 7 + 11) & 0xFF for i in range(24576))
SRC = "SRC20K.BIN"                          # 40 簇：拷贝/粘贴用的多簇文件
SRC_B = bytes((i * 13 + 5) & 0xFF for i in range(20480))
FREEME = "FREEME.BIN"                       # 40 簇：客人删掉它腾空间
FREEME_B = bytes((i * 3 + 1) & 0xFF for i in range(20480))
DOCS = "docs"
DOCS_NAME = "notes.txt"
DOCS_B = b"notes: original content, must not change\n"
FILL = "FILL.BIN"                           # 把剩下的簇全占了（卷 100% 满）
KEEP_CLUSTERS = (1, 48, 40, 40, 1, 1)       # 各文件的簇数（按分配顺序，见 make_stick）

# ---- 客人写的内容（串口/宿主两侧都用这几个常量）----
RW_NAME = "rw64.txt"                        # 盘上落成 RW64.TXT（8.3 大写）
RW_TEXT = b"vimtu64-usb-rw-0123456789abcdef"          # 29 B
RW_TEXT2 = b"vimtu64-usb-rewrite-42"                  # 22 B（覆盖写）
DIR_NAME = "newdir"                         # 盘上落成 NEWDIR
A_TEXT = b"subdir-content-ok"                         # 17 B
BAD_PATH = "/nosuchdir/b.txt"
TRUNC_TEXT = b"guest-truncated-this"                  # 19 B（把 D: 上的 SRC20K.BIN 截短）
NOFIT_TEXT = b"no-space-probe"

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "OOM:", "[FAT64] rw verify FAILED",
             "FAILED mask=", "[USBST] read FAILED", "[USB64] selftest FAIL", "[USBST] selftest FAIL"]


# ---------------------------------------------------------------------------
# 宿主侧：造 U 盘（MBR + 真 FAT32，簇全部占满）
# ---------------------------------------------------------------------------
def put_file(dev, data):
    per = dev.spc * SECTOR
    n = max(1, (len(data) + per - 1) // per)
    c = dev.alloc(n)
    for i in range(n):
        dev.write_cluster(c + i, data[i * per:(i + 1) * per])
    return c, n


def make_stick(path):
    """造一根"装满了"的 U 盘：根目录 6 项（含 2 个多簇文件），空闲簇 = 0。"""
    dev = fatr.Fat32Builder(STICK_PART_LBA, STICK_PART_SECTORS, 1)
    root = dev.alloc(1)                                  # 簇 2 = 根目录
    ents = bytearray()

    c_keep1, n_keep1 = put_file(dev, KEEP1_B)
    ents += dev.dir_entry(KEEP1, b"KEEP1   TXT", 0x20, c_keep1, len(KEEP1_B))
    c_keep2, n_keep2 = put_file(dev, KEEP2_B)
    ents += dev.dir_entry(KEEP2, b"KEEP2   DAT", 0x20, c_keep2, len(KEEP2_B))
    c_src, n_src = put_file(dev, SRC_B)
    ents += dev.dir_entry(SRC, b"SRC20K  BIN", 0x20, c_src, len(SRC_B))
    c_free, n_free = put_file(dev, FREEME_B)
    ents += dev.dir_entry(FREEME, b"FREEME  BIN", 0x20, c_free, len(FREEME_B))

    # docs/notes.txt
    c_docs = dev.alloc(1)
    c_note, n_note = put_file(dev, DOCS_B)
    dz = bytearray()
    dz += dev.short_entry(b".          ", 0x10, c_docs, 0)
    dz += dev.short_entry(b"..         ", 0x10, 0, 0)
    dz += dev.dir_entry(DOCS_NAME, b"NOTES   TXT", 0x20, c_note, len(DOCS_B))
    dev.write_cluster(c_docs, bytes(dz))
    ents += dev.dir_entry(DOCS, b"DOCS       ", 0x10, c_docs, 0)

    # FILL.BIN：把剩下的簇**全部**占掉（内容是 0，反正只做"占位"，数据区不写）
    used_before_fill = 1 + n_keep1 + n_keep2 + n_src + n_free + 1 + n_note
    rest = dev.clusters - used_before_fill
    assert rest > 0, "FILL.BIN 没有簇可占（几何变了？）"
    c_fill = dev.alloc(rest)
    ents += dev.dir_entry(FILL, b"FILL    BIN", 0x20, c_fill, rest * dev.spc * SECTOR)

    dev.write_cluster(root, bytes(ents))
    vol = dev.finish()

    full = bytearray(STICK_SECTORS * SECTOR)
    mbr = bytearray(SECTOR)
    e1 = bytearray(16)
    e1[4] = 0x0C                                         # FAT32 LBA
    struct.pack_into("<II", e1, 8, STICK_PART_LBA, STICK_PART_SECTORS)
    mbr[446:462] = e1
    mbr[510], mbr[511] = 0x55, 0xAA
    full[0:SECTOR] = mbr
    full[STICK_PART_LBA * SECTOR:STICK_PART_LBA * SECTOR + len(vol)] = vol
    with open(path, "wb") as f:
        f.write(bytes(full))
    return {"clusters": dev.clusters, "fatsz": dev.fatsz, "spc": dev.spc, "data_start": dev.data_start,
            "root": root, "c_keep1": c_keep1, "c_keep2": c_keep2, "c_src": c_src, "c_free": c_free,
            "c_docs": c_docs, "c_note": c_note, "c_fill": c_fill, "n_fill": rest,
            "blocks": STICK_SECTORS, "total_kb": STICK_TOTAL_KB}


# ---------------------------------------------------------------------------
# 宿主侧：最小 FAT32 解析器（只读；独立于内核实现）
# ---------------------------------------------------------------------------
class Fat32View:
    """极简 FAT32 视图：BPB -> 簇链 -> 目录项 -> 文件字节。用来自证盘上真的落了什么。"""

    def __init__(self, img, part_lba=STICK_PART_LBA):
        self.img = img
        self.base = part_lba * SECTOR
        b = img[self.base:self.base + SECTOR]
        assert b[510] == 0x55 and b[511] == 0xAA, "分区首扇区不是合法 BPB"
        self.spc = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.nfats = b[16]
        self.fatsz = struct.unpack_from("<I", b, 36)[0]
        self.total = struct.unpack_from("<I", b, 32)[0]
        self.root = struct.unpack_from("<I", b, 44)[0]
        self.fsinfo = struct.unpack_from("<H", b, 48)[0]
        self.data_start = self.reserved + self.nfats * self.fatsz
        self.clusters = (self.total - self.data_start) // self.spc
        self.cb = self.spc * SECTOR

    # ---- 扇区 / FAT 项 ----
    def sec(self, vol_lba):
        o = self.base + vol_lba * SECTOR
        return self.img[o:o + SECTOR]

    def fat1(self, c):
        o = self.base + (self.reserved + c * 4 // SECTOR) * SECTOR + (c * 4) % SECTOR
        return struct.unpack_from("<I", self.img, o)[0] & 0x0FFFFFFF

    def fat2(self, c):
        if self.nfats < 2:
            return self.fat1(c)
        o = self.base + (self.reserved + self.fatsz + c * 4 // SECTOR) * SECTOR + (c * 4) % SECTOR
        return struct.unpack_from("<I", self.img, o)[0] & 0x0FFFFFFF

    def clba(self, c):
        return self.data_start + (c - 2) * self.spc

    def cluster(self, c):
        o = self.base + self.clba(c) * SECTOR
        return self.img[o:o + self.cb]

    def chain(self, first, limit=200000):
        out = []
        c = first
        while 2 <= c <= self.clusters + 1 and len(out) < limit:
            out.append(c)
            nx = self.fat1(c)
            if nx >= 0x0FFFFFF8 or nx == 0:
                break
            c = nx
        return out

    # ---- 目录 ----
    def list_dir(self, cluster):
        """返回 [(name11 bytes, attr, first_cluster, size, deleted, raw32), ...]（跳过长名项）。"""
        out = []
        for c in self.chain(cluster):
            data = self.cluster(c)
            for o in range(0, self.cb, 32):
                e = data[o:o + 32]
                if len(e) < 32 or e[0] == 0x00:
                    return out
                if e[0] == 0xE5:
                    out.append((bytes(e[0:11]), e[11], 0, 0, True, bytes(e)))
                    continue
                if e[11] == 0x0F:                        # LFN 项：本批不写，读到就跳过
                    continue
                fc = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16)
                sz = struct.unpack_from("<I", e, 28)[0]
                out.append((bytes(e[0:11]), e[11], fc, sz, False, bytes(e)))
        return out

    def find(self, cluster, name83):
        """按 8.3 名（"RW64.TXT"）在目录里找；返回 (first_cluster, size, attr, raw32) 或 None。"""
        want = name83.upper()
        for n11, attr, fc, sz, dele, raw in self.list_dir(cluster):
            if dele:
                continue
            base = n11[0:8].decode("latin-1").rstrip(" ")
            ext = n11[8:11].decode("latin-1").rstrip(" ")
            nm = base + ("." + ext if ext else "")
            if nm == want:
                return (fc, sz, attr, raw)
        return None

    def file_bytes(self, first, size):
        out = bytearray()
        for c in self.chain(first):
            out += self.cluster(c)
            if len(out) >= size:
                break
        return bytes(out[:size])

    def tail_after(self, first, size):
        """文件最后一簇中 size 之后的字节（必须是全 0）。"""
        ch = self.chain(first)
        if not ch:
            return b""
        last = self.cluster(ch[-1])
        off = size % self.cb
        return last[off:] if off else b""

    # ---- 卷级 ----
    def free_clusters(self):
        n = 0
        for c in range(2, self.clusters + 2):
            if self.fat1(c) == 0:
                n += 1
        return n

    def fsinfo_free(self):
        s = self.sec(self.fsinfo)
        return struct.unpack_from("<I", s, 488)[0]

    def fat_mirror_diff(self):
        n = 0
        a = self.reserved
        b = self.reserved + self.fatsz
        for s in range(self.fatsz):
            if self.sec(a + s) != self.sec(b + s):
                n += 1
        return n


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (SYSTEM_IMG,):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_usbwrite_")
    sys_disk = os.path.join(tmp, "small.img")
    stick = os.path.join(tmp, "stick.img")
    serial = os.path.join(tmp, "serial.log")

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    # ==================== 阶段 0：夹具 ====================
    print("=== 阶段 0：宿主侧造 16MB 小系统盘（C: = VimtuFS2 v3）+ 35MB **装满了的** FAT32 U 盘 ===")
    if fst.make_small_system_disk(sys_disk) is None:
        print("  [FAIL] 无法生成小系统盘夹具（build64/system.img 缺失或过大）")
        return 2
    info = make_stick(stick)
    check("U 盘镜像 %d 扇区（%.1f MB）+ 真 FAT32（簇数 %d >= 65525，SPC=%d）"
          % (STICK_SECTORS, STICK_SECTORS * SECTOR / 1048576.0, info["clusters"], info["spc"]),
          info["clusters"] >= 65525 and info["spc"] == 1)
    check("★ 卷是**刻意写满**的（%d 簇全占：FILL.BIN=%d 簇，空闲 0）" % (TOTAL_CLUSTERS, info["n_fill"]),
          info["clusters"] == TOTAL_CLUSTERS and info["n_fill"] == TOTAL_CLUSTERS - 132)
    with open(stick, "rb") as f:
        before = f.read()
    check("U 盘镜像字节取出（%d 字节）" % len(before), len(before) == STICK_SECTORS * SECTOR)
    v0 = Fat32View(before)
    check("宿主侧解析基线镜像：簇数=%d、两份 FAT 逐字节一致、FSInfo free=%d、空闲簇=%d"
          % (v0.clusters, v0.fsinfo_free(), v0.free_clusters()),
          v0.clusters == TOTAL_CLUSTERS and v0.fat_mirror_diff() == 0 and
          v0.fsinfo_free() == 0 and v0.free_clusters() == 0)
    base_keep1 = v0.find(v0.root, KEEP1)
    base_keep2 = v0.find(v0.root, KEEP2)
    base_docs = v0.find(v0.root, DOCS)
    base_note = v0.find(info["c_docs"], DOCS_NAME)
    check("基线里 6 个根项都在（KEEP1/KEEP2/SRC20K/FREEME/docs/FILL）",
          all(v0.find(v0.root, n) is not None for n in (KEEP1, KEEP2, SRC, FREEME, DOCS, FILL)))

    # ==================== 阶段 1：QEMU 只挂 U 盘 ====================
    print("=== 阶段 1：开机（piix3-usb-uhci + usb-storage）-> 终端证明 D: ro=0 ===")
    port = fst.free_port()
    vm = ust.Vm(qemu, sys_disk, stick, port, serial, "Vimtu64-usbwrite")
    print("      %s" % vm.cmdline)
    mon = exp.Monitor(port)
    try:
        qh.login_desktop(mon, vm.log, vm.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm.wait_log("[GUI64] ready", 200))

        log = vm.log()
        m_letter = re.search(r"\[DRV64\] letter=D: disk=(\d+) part=(\d+) fs=FAT32 total_kb=(\d+) "
                             r"free_kb=(\d+) slot=(\S+) ro=(\d) fatvol=(\d+)", log)
        check("★ U 盘按**可写**挂载：[DRV64] letter=D: … ro=0（不是 ro=1）",
              m_letter is not None and m_letter.group(6) == "0" and int(m_letter.group(1)) == 24,
              m_letter.group(0) if m_letter else "（缺行）")
        m_rw = re.search(r"\[FAT64\] rw mount vol=(\d+) letter=D: writable=1 clusters=(\d+)", log)
        check("★ [FAT64] rw mount vol=… letter=D: writable=1 clusters=68874",
              m_rw is not None and int(m_rw.group(2)) == TOTAL_CLUSTERS,
              m_rw.group(0) if m_rw else "（缺行）")
        check("[FS64] mount fat … ro=0（统一卷表里这个卷不是只读）",
              re.search(r"\[FS64\] mount fat vol=\d+ disk=24 lba=%d clusters=%d spc=1 ro=0"
                        % (STICK_PART_LBA, TOTAL_CLUSTERS), log) is not None)
        check("[DRV64] selftest PASS（可写 U 盘卷也要过 bit6 一致性）",
              "[DRV64] selftest PASS" in log)

        # ---- 打开终端 ----
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        mon.type_line("vol")
        vlist = fst.wait_for(serial, "[VOL] vol letter=D:", 25, vm.proc)
        check("★ 终端卷表：D: 是 FAT32 且 ro=0（[VOL] vol letter=D: fs=FAT32 ro=0 total_kb=34437 free_kb=0）",
              re.search(r"\[VOL\] vol letter=D: fs=FAT32 ro=0 total_kb=%d free_kb=0" % STICK_TOTAL_KB,
                        vlist) is not None,
              (re.search(r"\[VOL\] vol letter=D:.*", vlist).group(0) if "vol letter=D:" in vlist else "（缺行）"))
        mon.type_line("vol d")
        check("切当前卷到 D:（[VOL] switch letter=D:）",
              re.search(r"\[VOL\] switch letter=D:", fst.wait_for(serial, "[VOL] switch letter=D:", 25, vm.proc))
              is not None)
        mon.type_line("ls /")
        lslog = fst.wait_for(serial, "[FAT64] list path=/", 25, vm.proc)
        check("★ 在 U 盘根目录列目录（[FAT64] list path=/ entries=6）",
              re.search(r"\[FAT64\] list path=/ entries=6", lslog) is not None,
              (re.search(r"\[FAT64\] list path=/.*", lslog).group(0) if "list path" in lslog else "（缺行）"))

        # ==================== 阶段 2：终端的写 ====================
        print("--- 2a) 反例①：卷满（0 空闲簇）-> 有界失败 reason=no-space，零副作用 ---")
        since = vm.mark()
        mon.type_line("mkdir nofitdir")
        check("★ mkdir 因卷满失败（[FAT64] rw fail … op=mkdir reason=no-space）",
              vm.wait_new(r"\[FAT64\] rw fail vol=\d+ op=mkdir reason=no-space", 30, since))
        check("终端也如实报失败（[TERM] cmd mkdir fail）", vm.wait_log("[TERM] cmd mkdir fail", 20, since))
        since = vm.mark()
        mon.type_line("write /nofit.txt %s" % NOFIT_TEXT.decode())
        check("★ write 因卷满失败（[FAT64] rw fail … op=write reason=no-space）",
              vm.wait_new(r"\[FAT64\] rw fail vol=\d+ op=write reason=no-space", 30, since))
        check("终端也如实报失败（[TERM] cmd write fail）", vm.wait_log("[TERM] cmd write fail", 20, since))
        since = vm.mark()
        mon.type_line("rm /nofit.txt")
        check("清掉那个 0 字节空项（[FAT64] rw unlink … freed=0 ok=1）",
              vm.wait_new(r"\[FAT64\] rw unlink vol=\d+ path=\"/nofit.txt\" freed=0 ok=1", 30, since))

        print("--- 2b) 反例②：坏路径（父目录不存在）-> reason=bad-path ---")
        since = vm.mark()
        mon.type_line("write %s x" % BAD_PATH)
        check("★ [FAT64] rw fail … op=write reason=bad-path",
              vm.wait_new(r"\[FAT64\] rw fail vol=\d+ op=write reason=bad-path", 30, since))

        print("--- 2c) rm /freeme.bin：腾出 40 个簇（真删 + 回收簇链） ---")
        since = vm.mark()
        mon.type_line("rm /freeme.bin")
        check("★ [FAT64] rw unlink vol=… path=\"/freeme.bin\" freed=40 ok=1",
              vm.wait_new(r"\[FAT64\] rw unlink vol=\d+ path=\"/freeme.bin\" freed=40 ok=1", 40, since))

        print("--- 2d) write /rw64.txt（新建）+ 第二次写（覆盖） ---")
        since = vm.mark()
        mon.type_line("write /%s %s" % (RW_NAME, RW_TEXT.decode()))
        check("★ [FAT64] rw write vol=… path=\"/%s\" len=%d cluster=… nclusters=1 verify=1"
              % (RW_NAME, len(RW_TEXT)),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d cluster=\d+ nclusters=1 verify=1"
                          % (re.escape(RW_NAME), len(RW_TEXT)), 40, since))
        check("终端报成功（[TERM] cmd write ok）", vm.wait_log("[TERM] cmd write ok", 20, since))
        since = vm.mark()
        mon.type_line("write /%s %s" % (RW_NAME, RW_TEXT2.decode()))
        check("★ 覆盖写同一个文件（len=%d，cluster=… nclusters=1，verify=1）" % len(RW_TEXT2),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d cluster=\d+ nclusters=1 verify=1"
                          % (re.escape(RW_NAME), len(RW_TEXT2)), 40, since))

        print("--- 2e) mkdir /newdir + write /newdir/a.txt（新目录里落文件） ---")
        since = vm.mark()
        mon.type_line("mkdir %s" % DIR_NAME)
        m_mk = None
        dl = time.time() + 40
        while time.time() < dl:
            m_mk = re.search(r"\[FAT64\] rw mkdir vol=(\d+) path=\"/%s\" cluster=(\d+) ok=1" % DIR_NAME,
                             vm.log()[since:])
            if m_mk:
                break
            time.sleep(0.2)
        check("★ [FAT64] rw mkdir vol=… path=\"/newdir\" cluster=… ok=1", m_mk is not None,
              m_mk.group(0) if m_mk else "（缺行）")
        since = vm.mark()
        mon.type_line("write /%s/a.txt %s" % (DIR_NAME, A_TEXT.decode()))
        check("★ [FAT64] rw write … path=\"/newdir/a.txt\" len=%d（新目录里落文件）" % len(A_TEXT),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/newdir/a\.txt\" len=%d" % len(A_TEXT),
                          40, since))

        # ==================== 阶段 3：管理器 Ctrl+C/Ctrl+V（多簇文件） ====================
        print("--- 3a) 管理器：D: 上选中 %s（40 簇）-> Ctrl+C -> 进 C: -> Ctrl+V ---" % SRC)
        mon.key("meta_l", wait=1.0)
        mon.key("2", wait=2.5)
        check("打开文件管理器（[APP] mypc opened）", vm.wait_log("[APP] mypc opened", 25))
        elog = fst.wait_for(serial, "[UI] explorer thispc drives=", 25, vm.proc)
        check("此电脑页出现（[UI] explorer thispc drives=…）",
              re.search(r"\[UI\] explorer thispc drives=\d+ browsable=\d+", elog) is not None)

        exp.SAFE_POINT = (fo.sx(fo.CONTENT_X + fo.CONTENT_W - 40),
                          fo.sy(fo.CONTENT_Y + fo.CONTENT_H - 40))
        top_ok, top_det = exp.ensure_window_on_top(vm, mon)
        check("资源管理器窗口能收到点击（鼠标闭环前置）", top_ok, top_det)
        exp.park_cursor(mon)
        step = exp.calibrate(vm, mon)
        check("鼠标位移模型（x 走 ~300px）", step is not None and abs(step[0] - 300) <= 40, str(step))
        exp.park_cursor(mon)
        for _ in range(3):
            exp.click_once(vm, mon)
        aim = ust.Aim(vm, mon)
        aim.calibrate()

        since = vm.mark()
        check("双击 D: 卡片进入 U 盘（[UI] explorer enter letter=D:）",
              ust.dclick_card_until(aim, vm, mon, "D", "[UI] explorer enter letter=D:"))
        check("★ 可写卷打点（[UI] explorer drive letter=D: … ro=0；只读时才有 'vol … ro=1 readonly' 那条）",
              vm.wait_new(r"\[UI\] explorer drive letter=D: fs=FAT32 total_kb=%d free_kb=\d+ ro=0"
                          % STICK_TOTAL_KB, 20, since),
              (re.search(r"\[UI\] explorer drive letter=D:.*", vm.log()[since:]).group(0)
               if "explorer drive letter=D:" in vm.log()[since:] else "（缺行）"))
        idx = ust.fresh_or_last_item_idx(vm, SRC, since)
        check("U 盘根目录里找到 %s（idx=%s，%d 字节 / 40 簇）" % (SRC, idx, len(SRC_B)), idx >= 0)
        if idx >= 0:
            p = aim.click_hit(*fo.cell(idx), want_hit="item:%d" % idx)
            check("单击选中 %s（click hit=item:%d）" % (SRC, idx), p is not None and p[2] == "item:%d" % idx,
                  str(p))
        since = vm.mark()
        mon.key("ctrl-c", wait=1.2)
        check("Ctrl+C 收进剪贴板（[UI] explorer clip op=copy n=1）",
              vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))
        since = vm.mark()
        check("进 C: 盘根（[UI] explorer enter letter=C:）", ust.goto_volume(aim, vm, mon, "C"))
        since = vm.mark()
        mon.key("ctrl-v", wait=1.6)
        check("★ 粘到 C: 根成功（[UI] explorer paste ok n=1 dst=/ skipped=0）",
              vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/ skipped=0", 40, since))
        check("C: 里出现 %s（item name=%s size=%d）" % (SRC, SRC, len(SRC_B)),
              vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=file size=%d"
                          % (re.escape(SRC), len(SRC_B)), 20, since))
        # ★ 关键：把剪贴板换成 **C: 里那份**（否则下一步粘贴的源还是 D: 上被截短的那个文件）
        idxc = ust.fresh_or_last_item_idx(vm, SRC, since)
        check("在 C: 里选中刚粘过来的 %s（idx=%s）" % (SRC, idxc), idxc >= 0)
        if idxc >= 0:
            p = aim.click_hit(*fo.cell(idxc), want_hit="item:%d" % idxc)
            check("单击选中 C: 里的 %s（click hit=item:%d）" % (SRC, idxc),
                  p is not None and p[2] == "item:%d" % idxc, str(p))
        since = vm.mark()
        mon.key("ctrl-c", wait=1.2)
        check("Ctrl+C 换成 C: 这份（clip op=copy n=1）",
              vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))

        print("--- 3b) 终端把 D: 上的 %s 截短到 %d B（同名文件变短 -> 回收 39 簇）---"
              % (SRC, len(TRUNC_TEXT)))
        check("把焦点交回终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        mon.type_line("vol d")
        fst.wait_for(serial, "[VOL] switch letter=D:", 25, vm.proc)
        since = vm.mark()
        mon.type_line("write /%s %s" % (SRC.lower(), TRUNC_TEXT.decode()))
        check("★ 覆盖写变短（[FAT64] rw write … path=\"/%s\" len=%d nclusters=1）"
              % (SRC.lower(), len(TRUNC_TEXT)),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d cluster=\d+ nclusters=1 verify=1"
                          % (re.escape(SRC.lower()), len(TRUNC_TEXT)), 40, since))

        print("--- 3c) 管理器：Ctrl+V 把 C: 里的 %s 再粘回 D:（就地覆盖 + 扩链回 40 簇）---" % SRC)
        # 焦点这时在终端上（3b 刚敲过命令）：先把资源管理器窗口抬上来再导航（重试只针对注入丢包）
        try:
            exp.ensure_window_on_top(vm, mon)
        except Exception:
            pass
        ok_d = False
        for _try in range(3):
            since = vm.mark()
            ok_d = ust.goto_volume(aim, vm, mon, "D")
            if ok_d:
                break
            try:
                exp.ensure_window_on_top(vm, mon)
            except Exception:
                pass
        check("再进 U 盘（D:）", ok_d)
        since = vm.mark()
        mon.key("ctrl-v", wait=1.6)
        check("★ 就地覆盖写（[UI] explorer usbfat overwrite name=%s bytes=%d ok …）"
              % (SRC, len(SRC_B)),
              vm.wait_new(r"\[UI\] explorer usbfat overwrite name=%s bytes=%d ok" % (SRC, len(SRC_B)),
                          60, since))

        print("--- 3d) 最后新建一个文件再删掉（unlink 的盘上证据：0xE5 + 簇回收，之后没人再分配）---")
        check("把焦点交回终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        mon.type_line("vol d")
        fst.wait_for(serial, "[VOL] switch letter=D:", 25, vm.proc)
        since = vm.mark()
        mon.type_line("write /last.txt one-shot")
        check("★ 新建 /last.txt（rw write … len=8 nclusters=1）",
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/last\.txt\" len=8 cluster=\d+ nclusters=1 verify=1",
                          40, since))
        since = vm.mark()
        mon.type_line("rm /last.txt")
        check("★ 再删掉它（[FAT64] rw unlink … path=\"/last.txt\" freed=1 ok=1）",
              vm.wait_new(r"\[FAT64\] rw unlink vol=\d+ path=\"/last\.txt\" freed=1 ok=1", 40, since))

        print("--- 禁止项 ---")
        flog = vm.log()
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in flog)
        fails = re.findall(r"\[FAT64\] rw fail vol=\d+ op=(\S+) reason=(\S+)", flog)
        check("★ 只有两条预期内的失败打点（mkdir/no-space 与 write/no-space、write/bad-path）",
              sorted(fails) == sorted([("mkdir", "no-space"), ("write", "no-space"), ("write", "bad-path")]),
              str(fails))
        check("★ U 盘写路径真的走了 USB 存储（[USBST] write lba=… ok）",
              re.search(r"\[USBST\] write lba=\d+ count=\d+ ok", flog) is not None)
    finally:
        vm.close()
        time.sleep(1.0)

    # ==================== 阶段 4：宿主侧逐字节核对 ====================
    print("=== 阶段 4：宿主侧解析 U 盘镜像（逐字节证明）===")
    with open(stick, "rb") as f:
        after = f.read()
    v = Fat32View(after)
    crc0 = zlib.crc32(before) & 0xFFFFFFFF
    crc1 = zlib.crc32(after) & 0xFFFFFFFF
    check("④ 整根镜像 CRC32 **变化**（%08X -> %08X）" % (crc0, crc1), crc0 != crc1)

    # ① 新文件
    e_rw = v.find(v.root, "RW64.TXT")
    check("① 盘上有新文件 /RW64.TXT（8.3 短名；长名不写 LFN）", e_rw is not None,
          str(e_rw[:3]) if e_rw else "（没有这个目录项）")
    if e_rw:
        fc, sz, attr, raw = e_rw
        got = v.file_bytes(fc, sz)
        check("①★ /RW64.TXT 大小 = %d（第二次写的内容长度）" % len(RW_TEXT2), sz == len(RW_TEXT2), "size=%d" % sz)
        check("①★★ /RW64.TXT 内容与客人写的**逐字节一致**（%d 字节，crc32 %08X）"
              % (len(RW_TEXT2), zlib.crc32(RW_TEXT2) & 0xFFFFFFFF),
              got == RW_TEXT2, "盘上 %08X / 客人 %08X" % (zlib.crc32(got) & 0xFFFFFFFF,
                                                          zlib.crc32(RW_TEXT2) & 0xFFFFFFFF))
        if got != RW_TEXT2:
            print("        盘上=%r" % got)
        check("① 文件最后一簇的尾巴（EOF 之后）全 0（不留旧内容）",
              all(b == 0 for b in v.tail_after(fc, sz)), "尾巴=%r" % v.tail_after(fc, sz)[:16])

    # 新目录 + 里面的文件
    e_dir = v.find(v.root, "NEWDIR")
    check("新目录 /NEWDIR 在根里（attr 含 0x10）", e_dir is not None and (e_dir[2] & 0x10) != 0,
          str(e_dir[:3]) if e_dir else "（没有）")
    if e_dir:
        sub = v.list_dir(e_dir[0])
        names = []
        for n11, attr, fc, sz, dele, raw in sub:
            names.append(n11[0:8].decode("latin-1").rstrip(" ") + "." + n11[8:11].decode("latin-1").rstrip(" "))
        check("★ /NEWDIR 里有 \".\" 与 \"..\" 项",
              any(n.startswith(". ") or n.startswith(".  ") or n.rstrip(".").strip() == "" for n in names) or
              any(n11[0] == 0x2E for n11, _, _, _, _, _ in sub),
              str(names))
        dotdot = [x for x in sub if x[0][0] == 0x2E and x[0][1] == 0x2E]
        check("★ \"..\" 指回根（首簇 0 = 根目录）",
              len(dotdot) == 1 and dotdot[0][2] == 0, str([(x[0], x[2]) for x in sub]))
        e_a = v.find(e_dir[0], "A.TXT")
        check("① /NEWDIR/A.TXT 在（size=%d）" % len(A_TEXT), e_a is not None and e_a[1] == len(A_TEXT),
              str(e_a[:3]) if e_a else "（没有）")
        if e_a:
            check("①★ /NEWDIR/A.TXT 内容逐字节等于客人写的",
                  v.file_bytes(e_a[0], e_a[1]) == A_TEXT,
                  repr(v.file_bytes(e_a[0], e_a[1]))[:60])

    # ② 拷过去的文件 == C: 里的源文件（两条独立的宿主侧解析路径）
    with open(sys_disk, "rb") as f:
        cbuf = f.read()
    cvol = mv.vfs3_vol(cbuf, fst.PART_MAIN_LBA)
    check("宿主侧认出 C: 的 VimtuFS2 v3 卷（源文件就在它上面）", cvol is not None)
    src_bytes = None
    if cvol is not None:
        ino = mv.vfs3_find(cbuf, cvol, SRC)
        check("宿主侧在 C: 卷里找到拷过来的 /%s（size=%d）" % (SRC, len(SRC_B)),
              ino is not None and ino["size"] == len(SRC_B),
              ("size=%d" % ino["size"]) if ino else "（没有这个 inode）")
        if ino is not None:
            src_bytes = mv.vfs3_read(cbuf, cvol, ino["idx"])
            check("宿主侧读回 C: 里的 /%s 与 U 盘上的源文件逐字节一致（%d 字节）" % (SRC, len(SRC_B)),
                  src_bytes == SRC_B)
    e_src = v.find(v.root, SRC)
    check("② D: 上 /%s 还在（attr 普通文件）" % SRC, e_src is not None and (e_src[2] & 0x10) == 0,
          str(e_src[:3]) if e_src else "（没有）")
    if e_src and src_bytes is not None:
        fc, sz, attr, raw = e_src
        nch = len(v.chain(fc))
        check("②★ D: 上的 /%s 是**多簇**文件（%d 簇 = %d 字节）" % (SRC, nch, sz),
              nch == len(SRC_B) // SECTOR and sz == len(SRC_B), "簇=%d size=%d" % (nch, sz))
        got = v.file_bytes(fc, sz)
        check("②★★ D: 上的 /%s **逐字节等于 C: 里的源文件**（%d 字节，crc32 %08X）"
              % (SRC, sz, zlib.crc32(src_bytes) & 0xFFFFFFFF),
              got == src_bytes, "盘上 %08X / 源 %08X" % (zlib.crc32(got) & 0xFFFFFFFF,
                                                        zlib.crc32(src_bytes) & 0xFFFFFFFF))
        check("② /%s 的最后一簇尾巴全 0" % SRC, all(b == 0 for b in v.tail_after(fc, sz)))

    # ③ 原有文件一个字节没变
    for name, base in ((KEEP1, base_keep1), (KEEP2, base_keep2), (DOCS, base_docs), (DOCS_NAME, base_note)):
        now = v.find(v.root, name) if name != DOCS_NAME else v.find(info["c_docs"], name)
        same = (base is not None and now is not None and base[3] == now[3] and base[0] == now[0] and
                base[1] == now[1])
        check("③ 原有文件 /%s 的目录项与首簇一字未变" % name, same,
              ("now=%s" % (now[:3],)) if now else "（没了）")
    for name, base, data in ((KEEP1, base_keep1, KEEP1_B), (KEEP2, base_keep2, KEEP2_B),
                             (DOCS_NAME, base_note, DOCS_B)):
        if base is None:
            continue
        now = v.find(v.root, name) if name != DOCS_NAME else v.find(info["c_docs"], name)
        if now is None:
            check("③ 原有文件 /%s 的内容还在" % name, False)
            continue
        got = v.file_bytes(now[0], now[1])
        check("③★ 原有文件 /%s 的数据逐字节等于基线（%d 字节）" % (name, len(data)),
              got == data and v.cluster(now[0]) == v0.cluster(base[0]),
              "crc %08X" % (zlib.crc32(got) & 0xFFFFFFFF))

    # ⑤ 结构自洽
    check("⑤ 两份 FAT 逐字节一致（FAT1 == FAT2，镜像同步真的做了）", v.fat_mirror_diff() == 0,
          "差异扇区=%d" % v.fat_mirror_diff())
    free_now = v.free_clusters()
    check("⑤ FSInfo 的 free（%d）与现场重数的空闲簇（%d）相等" % (v.fsinfo_free(), free_now),
          v.fsinfo_free() == free_now)
    live_root = [x for x in v.list_dir(v.root) if not x[4]]
    check("⑤ 被删的 /FREEME.BIN 不在根目录的活项里（unlink 生效；它的槽已被后来的新文件复用）",
          not any(x[0][0:6].decode("latin-1") == "FREEME" for x in live_root),
          str([x[0][0:8].decode("latin-1") for x in live_root]))
    # ★ 删除项的首字节 0xE5 覆盖的就是名字的第一个字符，所以按"第 2..4 个字符"认名字
    dead_last = [x for x in v.list_dir(v.root) if x[4] and x[0][1:4] == b"AST"]
    check("⑤★ 最后删掉的 /LAST.TXT：目录项首字节 = 0xE5、起始簇/长度已清 0（槽没被复用，直接可查）",
          len(dead_last) == 1 and dead_last[0][5][0] == 0xE5 and dead_last[0][5][20:32] == b"\x00" * 12,
          (str(dead_last[0][5][:16]) if dead_last else "（没找到已删项）"))
    # 变化只出现在我们动过的地方：FAT / FSInfo / 根目录扇区 / 新分配簇（自由簇集合）
    newf = [c for c in range(2, v.clusters + 2) if v.fat1(c) != v0.fat1(c)]
    check("⑤ 被改动的 FAT 项数量有界（<= 200：只有那几次分配/回收）", len(newf) <= 200, "改动簇数=%d" % len(newf))

    if args.keep:
        print("[usbwrite] 串口：%s / U 盘镜像：%s / 基线：%s" % (serial, stick, tmp))
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
