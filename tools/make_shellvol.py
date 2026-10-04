#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_shellvol.py - ★ A4-1：把 ring3 shell 装进 **VimtuFS2 系统卷**

为什么需要这个工具（本批次的核心交付方式）：
  shell **不内嵌内核**（内核镜像只剩 6 个扇区的余量，且要求"内核只负责装载 shell.bin"）。
  于是 shell 的交付 = 系统卷里的一个文件 /bin/shell.bin —— 卷是 VimtuFS2（v4），由本脚本
  在构建期"造好一块带文件的卷镜像"，再放进 16 MB 的测试/演示盘的主分区（LBA 8009）。

  卷格式的唯一定义点是 kernel/vfs64.h / kernel/vfs64.cpp（本脚本只是那份格式的一个**离线
  写入器**，逐字段照抄；任何一处改动都要同步三边）：
    块 = 512 B；块 0 = 超级块（magic "VIMTUFS2" + 几何 + [0,60) 的 CRC32 + 末尾 0xAA55）
    块 1..1+bmn-1 = 空闲块位图（1 = 已用）；块 bp..bp+ibn-1 = inode 表（v4 = 128 B/个，4 个/块）
    块 dp.. = 数据区
    v4 inode：type(0/1/2) namelen rsvd(0) size d0 d1 d2 d3 ind parent mtime nlink kind rsvd2
              name[31] dind uid gid mode rsvd[81..124) crc32([0,124))
    目录 = "parent 字段等于它的那批 inode"（目录没有数据块）

用法：
  # 只造卷镜像（tools/make_shellvol.py 的产物之一）
  py -3 tools/make_shellvol.py --shell build64/shell.bin --vol build64/shellvol.img
  # 造一块能直接启动的演示盘（system.img 字节 + 标准 MBR + 主分区 = 带 /bin/shell.bin 的卷）
  py -3 tools/make_shellvol.py --shell build64/shell.bin \
        --system build64/system.img --disk build64/sysdisk.img

退出码：0 = 成功（含自检通过）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import os
import struct
import sys
import zlib

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS          # 8009（= kernel/part64.h）
DEFAULT_TARGET_SECTORS = 32768                           # 16 MB 演示/测试盘

VFS_VERSION = 4
VFS_INODE_BYTES = 128
VFS_INODES_PER_BLK = 4
VFS_BITMAP_BLK_BITS = 4096
VFS_MAX_INODES = 512
VFS_DIRECT_BLOCKS = 4
VFS_INDIRECT_PTRS = 128
VFS_MAX_FILE_BLOCKS = 16384                              # 8 MiB 上限（v4）
VFS_MAX_FILE_BLOCKS_L1 = VFS_DIRECT_BLOCKS + VFS_INDIRECT_PTRS   # 132：再往上需要二级间接块

S_IFDIR = 0o040000
S_IFREG = 0o100000
KIND_DIR = 2
KIND_ELF = 4
KIND_TEXT = 5

NAME_MAX = 31


def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


class Volume:
    """一块 VimtuFS2 v4 卷的内存镜像 + 极小的离线写入器（只做本批次要用的：建目录 + 建文件）。"""

    def __init__(self, total_sectors):
        self.total = total_sectors
        self.bm_bits = VFS_BITMAP_BLK_BITS
        self.bmn = (total_sectors + self.bm_bits - 1) // self.bm_bits
        # ★ 本批容量收口（先有实测证据才改）：inode 密度 总扇区/64 -> 总扇区/32（仍夹在
        #   [16, VFS_MAX_INODES=512]）。为什么必须改：本批实测系统卷 24759 扇区 -> **386 个 inode**
        #   在装完全部交付物后用掉 373（只剩 13 个空闲），而**开机自己**就要新建 11 个
        #   （/etc/users.db、/home{,/vimtu{,/Desktop}}、/root{,/Desktop}、/store.a、/logo{,/kaisi.png}、
        #   /kaisi.png、/etc/iconpack.bin）—— 开完机只剩 **2 个**空闲 inode，于是任何"在卷里新建文件"
        #   的验收（busybox 的 tar/gzip/live 用例、fs_term / fd64 / fileops ...）都会撞
        #   `[VFS64] write_stream64: no free inode` -> -ENOSPC（实测 busybox64_test 连第 4 个新文件都
        #   建不出来）。密度翻倍后系统卷 = 上限 512（空闲 512-373 = 139，扣掉开机的 11 还有 128），
        #   代价只是少 31 个数据块（约 16 KiB / 24654 块的 0.1%）。
        #   口径说明：这只是**离线造卷工具**的容量参数，内核自己的 vfs64_format 仍按 /64；两者都只
        #   在超级块里记真实值，内核挂载时只读超级块（<= VFS_MAX_INODES 即可），互不依赖。
        inodes = total_sectors // 32
        inodes = max(16, min(VFS_MAX_INODES, inodes))
        self.inodes = inodes
        self.ino_blocks = (inodes + VFS_INODES_PER_BLK - 1) // VFS_INODES_PER_BLK
        self.bitmap_start = 1
        self.inode_start = self.bitmap_start + self.bmn
        self.data_start = self.inode_start + self.ino_blocks
        self.data_blocks = total_sectors - self.data_start
        if self.data_blocks <= 0:
            raise ValueError("卷太小：total=%d" % total_sectors)
        self.buf = bytearray(total_sectors * SECTOR)
        self.used = set()                                 # 已用块号（位图的真源）
        for blk in range(self.data_start):
            self.used.add(blk)
        self.next_ino = 1                                 # 0 = 根目录
        self._mk_inode(0, 2, 0, {}, 0, 0, 0, S_IFDIR | 0o755, KIND_DIR)

    # ---------------- 块 / inode 原语 ----------------
    def _blk_off(self, blk):
        return blk * SECTOR

    def _alloc_blk(self):
        for blk in range(self.data_start, self.total):
            if blk not in self.used:
                self.used.add(blk)
                return blk
        raise ValueError("卷空间不足（数据块用尽）")

    def _rd32(self, blk, off):
        return struct.unpack_from("<I", self.buf, self._blk_off(blk) + off)[0]

    def _wr32(self, blk, off, v):
        struct.pack_into("<I", self.buf, self._blk_off(blk) + off, v & 0xFFFFFFFF)

    def _mk_inode(self, ino, typ, size, blocks, parent, uid, gid, mode, kind, name=""):
        """blocks：直接块列表（最多 4）+ ind 块号 + dind 块号（后两个由 _write_file 决定）"""
        if ino >= self.inodes:
            raise ValueError("inode 用完（%d/%d）" % (ino, self.inodes))
        nb = name.encode("ascii")
        if len(nb) > NAME_MAX:
            raise ValueError("名字太长：%r" % name)
        rec = bytearray(VFS_INODE_BYTES)
        rec[0] = typ
        rec[1] = len(nb)
        struct.pack_into("<I", rec, 4, size)
        d = list(blocks.get("direct", [])) + [0] * VFS_DIRECT_BLOCKS
        for i in range(VFS_DIRECT_BLOCKS):
            struct.pack_into("<I", rec, 8 + 4 * i, d[i] if i < len(d) else 0)
        struct.pack_into("<I", rec, 24, blocks.get("ind", 0))
        struct.pack_into("<I", rec, 28, parent)
        struct.pack_into("<I", rec, 32, 0)                       # mtime = 0（未知；0 是合法值）
        struct.pack_into("<H", rec, 36, 1)                       # nlink（目录的计数由调用方给）
        rec[38] = kind
        rec[40:40 + len(nb)] = nb
        struct.pack_into("<I", rec, 71, blocks.get("dind", 0))
        struct.pack_into("<H", rec, 75, uid)
        struct.pack_into("<H", rec, 77, gid)
        struct.pack_into("<H", rec, 79, mode)
        struct.pack_into("<I", rec, 124, _crc32(bytes(rec[:124])))
        off = self.inode_start * SECTOR + ino * VFS_INODE_BYTES
        self.buf[off:off + VFS_INODE_BYTES] = rec
        return rec

    # ---------------- 对外：建目录 / 建文件 ----------------
    def mkdir(self, name, parent=0, uid=0, gid=0, mode=0o755):
        ino = self.next_ino
        self.next_ino += 1
        self._mk_inode(ino, 2, 0, {}, parent, uid, gid, S_IFDIR | mode, KIND_DIR, name)
        return ino

    def write_file(self, name, data, parent=0, uid=0, gid=0, mode=0o644, kind=None):
        nblk = (len(data) + SECTOR - 1) // SECTOR
        if nblk > VFS_MAX_FILE_BLOCKS:
            raise ValueError("文件超过 VimtuFS2 上限（%d > %d 块）" % (nblk, VFS_MAX_FILE_BLOCKS))
        if nblk > VFS_MAX_FILE_BLOCKS_L1:
            raise ValueError("本离线写入器只做 直接块+一级间接（<= %d 块 = %d B）；本文件 %d 块"
                             % (VFS_MAX_FILE_BLOCKS_L1, VFS_MAX_FILE_BLOCKS_L1 * SECTOR, nblk))
        blocks = []
        for i in range(nblk):
            blk = self._alloc_blk()
            blocks.append(blk)
            chunk = data[i * SECTOR:(i + 1) * SECTOR]
            off = self._blk_off(blk)
            self.buf[off:off + len(chunk)] = chunk
        ref = {"direct": blocks[:VFS_DIRECT_BLOCKS]}
        rest = blocks[VFS_DIRECT_BLOCKS:]
        if rest:
            if len(rest) > VFS_INDIRECT_PTRS:
                raise ValueError("间接块装不下（%d 个）" % len(rest))
            ind = self._alloc_blk()
            for i in range(VFS_INDIRECT_PTRS):
                self._wr32(ind, 4 * i, rest[i] if i < len(rest) else 0)
            ref["ind"] = ind
        if kind is None:
            kind = KIND_ELF if data[:4] == b"\x7fELF" else KIND_TEXT
        ino = self.next_ino
        self.next_ino += 1
        self._mk_inode(ino, 1, len(data), ref, parent, uid, gid, S_IFREG | mode, kind, name)
        return ino

    # ---------------- 收尾：位图 / 超级块 ----------------
    def finish(self):
        for m in range(self.bmn):
            bm = bytearray(SECTOR)
            for k in range(self.bm_bits):
                blk = m * self.bm_bits + k
                if blk >= self.total or blk in self.used:
                    bm[k >> 3] |= 1 << (k & 7)
            off = (self.bitmap_start + m) * SECTOR
            self.buf[off:off + SECTOR] = bm
        sb = bytearray(SECTOR)
        sb[0:8] = b"VIMTUFS2"
        struct.pack_into("<I", sb, 8, VFS_VERSION)
        struct.pack_into("<I", sb, 12, SECTOR)
        struct.pack_into("<I", sb, 16, SECTOR)
        struct.pack_into("<I", sb, 20, self.total)
        struct.pack_into("<I", sb, 24, 0)                  # 根目录 inode = 0
        struct.pack_into("<I", sb, 28, self.bitmap_start)
        struct.pack_into("<I", sb, 32, self.bmn)
        struct.pack_into("<I", sb, 36, self.inode_start)
        struct.pack_into("<I", sb, 40, self.inodes)
        struct.pack_into("<I", sb, 44, VFS_INODE_BYTES)
        struct.pack_into("<I", sb, 48, self.data_start)
        struct.pack_into("<I", sb, 52, self.data_blocks)
        struct.pack_into("<I", sb, 56, 0)
        struct.pack_into("<I", sb, 60, _crc32(bytes(sb[:60])))
        sb[510], sb[511] = 0x55, 0xAA
        self.buf[0:SECTOR] = sb
        return bytes(self.buf)


# ---------------------------------------------------------------------------
# 自检：把卷**重新解析**一遍（独立走查，不看写入时的中间状态），核对：
#   超级块 CRC/签名 + 根目录里能看到条目 + /bin/shell.bin 按块链读回来的字节逐字节一致
# ---------------------------------------------------------------------------
def verify(vol_bytes, expect_files):
    if vol_bytes[0:8] != b"VIMTUFS2":
        return "超级块 magic 不对"
    if vol_bytes[510] != 0x55 or vol_bytes[511] != 0xAA:
        return "超级块结尾签名不对"
    if struct.unpack_from("<I", vol_bytes, 60)[0] != _crc32(vol_bytes[:60]):
        return "超级块 CRC32 不对"
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    ino_start = struct.unpack_from("<I", vol_bytes, 36)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]

    def inode(i):
        off = ino_start * SECTOR + i * VFS_INODE_BYTES
        rec = vol_bytes[off:off + VFS_INODE_BYTES]
        if rec[0] != 0 and struct.unpack_from("<I", rec, 124)[0] != _crc32(rec[:124]):
            raise ValueError("inode %d 的 CRC32 不对" % i)
        return rec

    def entries(parent):
        """返回 [(inode 号, 名字, inode 记录)]：目录 = parent 字段等于给定 inode 的那批 inode。"""
        out = []
        for i in range(inodes):
            rec = inode(i)
            if rec[0] == 0:
                continue
            if struct.unpack_from("<I", rec, 28)[0] != parent:
                continue
            nm = rec[40:40 + rec[1]].decode("ascii")
            out.append((i, nm, rec))
        return out

    def find(parent, name):
        for i, nm, rec in entries(parent):
            if nm == name:
                return i, rec
        return None

    def read_file(rec):
        size = struct.unpack_from("<I", rec, 4)[0]
        blocks = [struct.unpack_from("<I", rec, 8 + 4 * d)[0] for d in range(VFS_DIRECT_BLOCKS)]
        ind = struct.unpack_from("<I", rec, 24)[0]
        if ind:
            for k in range(VFS_INDIRECT_PTRS):
                blocks.append(struct.unpack_from("<I", vol_bytes, ind * SECTOR + 4 * k)[0])
        data = bytearray()
        for b in blocks:
            if len(data) >= size:
                break
            if b == 0 or b >= total:
                raise ValueError("块号越界：%d" % b)
            data += vol_bytes[b * SECTOR:(b + 1) * SECTOR]
        return bytes(data[:size])

    root = entries(0)
    # 逐文件核对（按名字逐级走下去，最后把文件字节按块链读回来比对）
    for path, want in expect_files.items():
        parts = [p for p in path.split("/") if p]
        cur_ino = 0
        found = True
        for k, part in enumerate(parts):
            hit = find(cur_ino, part)
            if not hit:
                return "卷里找不到 /" + "/".join(parts[:k + 1])
            ino, rec = hit
            if k == len(parts) - 1:
                got = read_file(rec)
                if got != want:
                    return "%s 读回的字节不一致（%d vs %d B）" % (path, len(got), len(want))
            else:
                if rec[0] != 2:
                    return "%s 不是目录" % ("/".join(parts[:k + 1]))
                cur_ino = ino
        if not found:
            return "解析失败：%s" % path
    return None


def _mbr_entry(bootable, ptype, start, sectors):
    e = bytearray(16)
    e[0] = 0x80 if bootable else 0x00
    e[1:4] = b"\xFE\xFF\xFF"
    e[4] = ptype
    e[5:8] = b"\xFE\xFF\xFF"
    struct.pack_into("<I", e, 8, start)
    struct.pack_into("<I", e, 12, sectors)
    return e


def build_volume(shell_bytes, hello_text, total_sectors):
    vol = Volume(total_sectors)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)               # 世界可写：任何会话身份都能在里面 mkdir/写文件
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", hello_text, parent=etc_ino, mode=0o644)
    return vol.finish()


def build_disk(system_bytes, vol_bytes, target_sectors):
    buf = bytearray(target_sectors * SECTOR)
    buf[0:len(system_bytes)] = system_bytes
    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)      # 引导区（内核）
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, target_sectors - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    base = PART_MAIN_LBA * SECTOR
    buf[base:base + len(vol_bytes)] = vol_bytes
    return bytes(buf)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", required=True, help="user/shell/build_shell.sh 产出的 shell.bin（ELF64）")
    ap.add_argument("--vol", default=None, help="输出卷镜像（VimtuFS2 v4，含 /bin/shell.bin）")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时用）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--vol-sectors", type=int, default=DEFAULT_TARGET_SECTORS - PART_MAIN_LBA)
    ap.add_argument("--target-sectors", type=int, default=DEFAULT_TARGET_SECTORS)
    ap.add_argument("--hello", default="VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\\n")
    args = ap.parse_args()

    if not os.path.exists(args.shell):
        sys.stderr.write("找不到 shell 载荷：%s\n" % args.shell)
        return 2
    shell_bytes = open(args.shell, "rb").read()
    if shell_bytes[:4] != b"\x7fELF":
        sys.stderr.write("shell 载荷不是 ELF：%s\n" % args.shell)
        return 2
    hello_text = args.hello.replace("\\n", "\n").encode("utf-8")

    if args.disk and not args.system:
        sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
        return 2

    vol_sectors = args.vol_sectors if not args.disk else (args.target_sectors - PART_MAIN_LBA)
    vol_bytes = build_volume(shell_bytes, hello_text, vol_sectors)
    expect = {"/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": hello_text}
    bad = verify(vol_bytes, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    print("    卷自检 OK：/bin/shell.bin = %d B（逐字节回读一致）/ /etc/sh64hello.txt = %d B / /tmp(0777) / 卷 %d 扇区"
          % (len(shell_bytes), len(hello_text), vol_sectors))

    if args.vol:
        open(args.vol, "wb").write(vol_bytes)
        print("    卷镜像：%s（%d B）" % (args.vol, len(vol_bytes)))
    if args.disk:
        system_bytes = open(args.system, "rb").read()
        # system.img = 8073 扇区：尾部的 8009..8072 是 store64 的"裸盘降级槽"（与数据分区起点重叠，
        # 见 kernel/memlayout64.h 的 ★★ 段）。上限放宽到**整块盘**即可 —— 卷写在同一位置会原样
        # 盖掉那 64 个扇区（正常运行时用的是卷里的 /store.a|b，降级槽本来就不该有内容）。
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大（%d B > %d B）\n" % (len(system_bytes), args.target_sectors * SECTOR))
            return 2
        img = build_disk(system_bytes, vol_bytes, args.target_sectors)
        open(args.disk, "wb").write(img)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(img), args.target_sectors, PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
