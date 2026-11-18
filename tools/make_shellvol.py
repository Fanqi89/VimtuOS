#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_shellvol.py - ★ A4-1：把 ring3 shell 装进 **VimtuFS2 系统卷**

为什么需要这个工具（本批次的核心交付方式）：
  shell **不内嵌内核**（内核镜像只剩 6 个扇区的余量，且要求"内核只负责装载 shell.bin"）。
  于是 shell 的交付 = 系统卷里的一个文件 /bin/shell.bin —— 卷是 VimtuFS2（v4），由本脚本
  在构建期"造好一块带文件的卷镜像"，再放进 16 MB 的测试/演示盘的主分区（LBA 32584）。

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
PART_BOOT_LBA = 65
PART_BOOT_SECS = 32519
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS          # 32584（= kernel/part64.h）
DEFAULT_TARGET_SECTORS = 57343                           # ★ v0.10.9：**不再是上限**，只是"卷的起始容量"。
                                                         #   卷装不下时自动增长（Volume._grow），
                                                         #   写完由 build_disk() 按真实内容收口（trim）。
                                                         #   于是 sysdisk.img / system.img / ISO 的大小 = 32,584 + 真实卷扇区。

# ===========================================================================
# ★ v0.10.9【动态智能镜像机制】几何规划
#   卷布局：块 0 = 超级块；块 1..bmn = 位图；inode 表 inode_blocks 块；其后为数据区。
#   为什么"改 total"必须整体重排，而不能只改超级块里的那一行：
#   kernel/vfs64.cpp 挂载时**强制校验** bmn == ceil(total/4096)（不符即 SB_ERR_LAYOUT），
#   而 total 一变 data_start 就变，于是所有数据块号与 inode 表位置全部失效。
#   这就是 _remap_to() 存在的原因（压实 + 块号重写 + inode CRC 重算）。
# ===========================================================================
VOL_ALIGN_SECTORS = 2048          # 收口对齐粒度 = 1 MiB（"按块对齐即可"）
VOL_TRIM_SLACK = 4096             # ★ v0.10.9：**运行时**预留的空闲数据块 = 2 MiB。
                                  #   为什么必须这么大：收口只留 512 块（256 KiB）时，链尾最后一步
                                  #   把卷填到只剩 129 块，运行时第一次 `cfg save` 就
                                  #   `[CONF64] flush failed rc=1 via=vfs`（ENOSPC，实测刷屏）；
                                  #   第二次启动甚至只剩 8 块。首启还要新建 /etc、/home、日志块重写。
VOL_GROW_STEP = 4096              # 单次增长下限 2 MiB（避免逐块重排）


def vol_layout(total_sectors):
    """返回 (bmn, inode_start, data_start)：与 Volume.__init__ 同一口径。"""
    bmn = (total_sectors + VFS_BITMAP_BLK_BITS - 1) // VFS_BITMAP_BLK_BITS
    inode_start = 1 + bmn
    data_start = inode_start + VFS_INODE_BLOCKS
    return bmn, inode_start, data_start


def plan_total_sectors(need_data_blocks, min_total=0):
    """给定"需要多少个数据块"，解出满足布局的最小 total_sectors（迭代收敛）。

    不变量：total - data_start(total) >= need_data_blocks。
    data_start 随 total 增大而增大（bmn 涨），所以必须迭代，不能一步算完。
    """
    need = max(0, int(need_data_blocks))
    t = max(int(min_total), need + 1024)     # 初猜只留布局余量；"长一截"由 _grow 自己加
    for _ in range(16):
        _, _, ds = vol_layout(t)
        want = ds + need
        if want <= t:
            break
        t = want
    return max(t, int(min_total))


def align_up_sectors(n, align):
    a = max(1, int(align))
    return ((int(n) + a - 1) // a) * a


VFS_VERSION = 5          # ★ v0.9.28：VimtuFS2 v5（= v4 + 超级块特性扩展头 + inode xattr/快照字段）
                         #   本脚本产出的卷 feature_bits 恒 0（扩展头全 0），行为与 v4 逐字节相同；
                         #   与内核 vfs64_format 的产出一致，安装后的系统卷才支持日志/配额/ACL/快照。
VFS_INODE_BYTES = 128
VFS_INODES_PER_BLK = 4
VFS_BITMAP_BLK_BITS = 4096
VFS_MAX_INODES = 2048                                   # ★ v0.6.0：512 -> 2048（见 kernel/vfs64.h 的说明）
VFS_DIRECT_BLOCKS = 4
VFS_INDIRECT_PTRS = 128
VFS_DIND_CHILDREN = 128                                  # 二级间接块里的一级间接块个数（v4；见 kernel/vfs64.h）
VFS_MAX_FILE_BLOCKS = 16384                              # 8 MiB 上限（v4）
VFS_MAX_FILE_BLOCKS_L1 = VFS_DIRECT_BLOCKS + VFS_INDIRECT_PTRS   # 132：再往上需要二级间接块
VFS_INODE_BLOCKS = (VFS_MAX_INODES + VFS_INODES_PER_BLK - 1) // VFS_INODES_PER_BLK  # 512

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
        # ★ v0.6.0：系统卷**直接给满**（2048），不再按 total/32 夹。原因是接下来要把图标包
        #   （/System/icons）拆成单个 PNG、并把根目录重排成 /System + /usr/bin + /var 的布局，
        #   512 个 inode 根本不够（实测装完只剩 108 个空闲）。
        #   代价：inode 表 128 -> 512 扇区（+192 KB），数据区少 384 块（-192 KB），可接受。
        inodes = VFS_MAX_INODES
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
        # ★ v0.10.9：数据区用尽时**自动扩容**（不再有固定体积上限）。
        #   这条路径被卷链上所有工具共享（它们都继承本类），所以"加一张 6.8 MB 壁纸
        #   就把整条构建链打断"再也不会发生 —— 卷按内容长大，末了统一收口。
        for blk in range(self.data_start, self.total):
            if blk not in self.used:
                self.used.add(blk)
                return blk
        self._grow()
        for blk in range(self.data_start, self.total):
            if blk not in self.used:
                self.used.add(blk)
                return blk
        raise ValueError("卷空间不足（扩容后仍无空闲块，total=%d）" % self.total)

    # ================= v0.10.9 动态容量原语 =================
    def load(self, img):
        """把一块已存在的卷装进来（几何沿用**本对象**的 total；只重建分配器状态）。"""
        want = self.total * SECTOR
        self.buf = bytearray(want)
        n = min(len(img), want)
        self.buf[0:n] = img[0:n]
        self.used = set()
        for m in range(self.bmn):
            off = (self.bitmap_start + m) * SECTOR
            blk = self.buf[off:off + SECTOR]
            for k in range(VFS_BITMAP_BLK_BITS):
                if (blk[k >> 3] >> (k & 7)) & 1:
                    b = m * VFS_BITMAP_BLK_BITS + k
                    # ★ v0.10.9：finish() 会把 blk >= total 的位也标 1（"分区外，永不发放"），
                    #   从位图重建时**必须夹掉**，否则越界块号会污染分配器与重排（真踩过：
                    #   24,576 扇区的卷被拷出短切片压成 21,330 扇区）。
                    if b < self.total:
                        self.used.add(b)

    def _used_data_blocks(self):
        return sorted(b for b in self.used if self.data_start <= b < self.total)

    def _remap_to(self, new_total):
        """把整块卷重排到 new_total 扇区：数据块**压实**后重新分配块号，并重写所有块指针。

        为什么必须整体重排：内核挂载校验 bmn == ceil(total/4096)；total 一变，
        inode 表起点与数据区起点都移动，旧块号全部失效。
        """
        new_total = int(new_total)
        if new_total == self.total:
            return self.total
        kept = self._used_data_blocks()
        bmn2, is2, ds2 = vol_layout(new_total)
        if len(kept) + ds2 > new_total:
            raise ValueError("卷收得过小：需要 %d 个数据块，total=%d 只有 %d 块"
                             % (len(kept), new_total, new_total - ds2))
        nmap = {}
        nxt = ds2
        for b in kept:
            nmap[b] = nxt
            nxt += 1
        new = bytearray(new_total * SECTOR)
        inode_bytes = self.inodes * VFS_INODE_BYTES
        new[is2 * SECTOR:is2 * SECTOR + inode_bytes] = \
            self.buf[self.inode_start * SECTOR:self.inode_start * SECTOR + inode_bytes]
        for b in kept:
            new[nmap[b] * SECTOR:(nmap[b] + 1) * SECTOR] = self.buf[b * SECTOR:(b + 1) * SECTOR]
        # ① inode 里的块指针（direct[0..3] / ind / dind）+ CRC 重算
        for i in range(self.inodes):
            o = is2 * SECTOR + i * VFS_INODE_BYTES
            if new[o] == 0:
                continue
            for d in range(VFS_DIRECT_BLOCKS):
                p = o + 8 + 4 * d
                v = struct.unpack_from("<I", new, p)[0]
                if v:
                    struct.pack_into("<I", new, p, nmap[v])
            for p in (o + 24, o + 71):
                v = struct.unpack_from("<I", new, p)[0]
                if v:
                    struct.pack_into("<I", new, p, nmap[v])
            struct.pack_into("<I", new, o + 124, _crc32(bytes(new[o:o + 124])))
        # ② 间接块容器里的块号（ind / dind / dind 的 child）也要跟着改
        cont = set()
        for i in range(self.inodes):
            o = self.inode_start * SECTOR + i * VFS_INODE_BYTES
            if self.buf[o] == 0:
                continue
            ind = struct.unpack_from("<I", self.buf, o + 24)[0]
            dind = struct.unpack_from("<I", self.buf, o + 71)[0]
            if ind:
                cont.add(ind)
            if dind:
                cont.add(dind)
                for c in range(VFS_DIND_CHILDREN):
                    ch = struct.unpack_from("<I", self.buf, dind * SECTOR + 4 * c)[0]
                    if ch:
                        cont.add(ch)
        for b in cont:
            dst = nmap[b] * SECTOR
            for k in range(VFS_INDIRECT_PTRS):
                v = struct.unpack_from("<I", new, dst + 4 * k)[0]
                if v:
                    struct.pack_into("<I", new, dst + 4 * k, nmap[v])
        self.buf = new
        self.total = new_total
        self.bmn = bmn2
        self.inode_start = is2
        self.data_start = ds2
        self.data_blocks = new_total - ds2
        self.used = set(range(ds2)) | set(nmap.values())
        return new_total

    def _reserve(self, nblocks):
        """确保数据区至少还有 nblocks 个空闲块；不够就**一次性**扩容到这里。

        为什么必须"一次性预留"而不是"用完了再长"：write_file 是先收集块号、
        最后才写 inode 的；中途重排会让已收集的块号失效（真踩过：6.8 MB 壁纸 crc 不对）。
        """
        nblocks = int(nblocks)
        if nblocks <= 0:
            return
        free = (self.total - self.data_start) - len(self._used_data_blocks())
        # ★ v0.10.9：不仅是"够写这个文件"，还要留出运行时余量（否则链尾会把卷填死）
        if free >= nblocks + VOL_TRIM_SLACK:
            return
        need = len(self._used_data_blocks()) + nblocks + VOL_TRIM_SLACK
        self._remap_to(plan_total_sectors(need, min_total=self.total))

    def _reserve_for_file(self, nblk):
        """按"数据块 + 一级间接 + 二级间接 + child"预留一个文件的全部块。"""
        meta = 0
        rest = max(0, int(nblk) - VFS_DIRECT_BLOCKS)
        if rest:
            meta += 1                                            # 一级间接块
        if rest > VFS_INDIRECT_PTRS:
            extra = rest - VFS_INDIRECT_PTRS
            meta += 1                                            # 二级间接块
            meta += (extra + VFS_INDIRECT_PTRS - 1) // VFS_INDIRECT_PTRS
        self._reserve(int(nblk) + meta + 4)

    def _grow(self, extra_blocks=0):
        need = len(self._used_data_blocks()) + max(0, int(extra_blocks)) + VOL_GROW_STEP
        t = plan_total_sectors(need, min_total=self.total)
        if t <= self.total:
            t = self.total + VOL_GROW_STEP
        return self._remap_to(t)

    def trim(self, align=VOL_ALIGN_SECTORS, slack_blocks=VOL_TRIM_SLACK):
        """按真实内容收口（"打进去多少文件，镜像就多大"），按 align 扇区对齐。"""
        need = len(self._used_data_blocks()) + max(0, int(slack_blocks))
        t = align_up_sectors(plan_total_sectors(need), align)
        if t < self.total:
            self._remap_to(t)
        return self.total

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
        self._reserve_for_file(nblk)      # ★ v0.10.9：一次预留，避免写一半被重排打乱块号
        # ★ v0.5.8：**补上二级间接块（dind）**。原来这里只做"直接块 + 一级间接"（<= 132 块
        #   = 67,584 B），于是用户提供的大壁纸 / 大图直接 ValueError 报错 —— 而"预设壁纸由用户
        #   提供"必然是大图（1920x1080 的 jpg 常见 200~800 KB）。v4 卷的 inode 早在 rec[71] 就有
        #   dind 字段（_mk_inode 一直会写），只是这条离线写入路径没构造它。
        #   块序与 kernel/vfs64.cpp 的 read_at_inode 一致：direct[0..3] -> ind[0..127] ->
        #   dind[0][0..127] -> dind[1][0..127] -> …
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
            take = min(len(rest), VFS_INDIRECT_PTRS)
            ind = self._alloc_blk()
            for i in range(VFS_INDIRECT_PTRS):
                self._wr32(ind, 4 * i, rest[i] if i < take else 0)
            ref["ind"] = ind
            rest = rest[take:]
        if rest:
            if len(rest) > VFS_DIND_CHILDREN * VFS_INDIRECT_PTRS:
                raise ValueError("二级间接块装不下（%d 个）" % len(rest))
            dind = self._alloc_blk()
            for c in range(VFS_DIND_CHILDREN):
                seg = rest[c * VFS_INDIRECT_PTRS:(c + 1) * VFS_INDIRECT_PTRS]
                if not seg:
                    self._wr32(dind, 4 * c, 0)
                    continue
                child = self._alloc_blk()
                for i in range(VFS_INDIRECT_PTRS):
                    self._wr32(child, 4 * i, seg[i] if i < len(seg) else 0)
                self._wr32(dind, 4 * c, child)
            ref["dind"] = dind
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
        # ★ v0.5.8：二级间接块（dind 在 inode 偏移 71，见 kernel/vfs64.h 的 v3/v4 布局）。
        #   没有这一段，verify 就读不了 >67,584 B 的文件 —— 而预设壁纸正好是这种。
        dind = struct.unpack_from("<I", rec, 71)[0]
        if dind:
            for c in range(VFS_DIND_CHILDREN):
                child = struct.unpack_from("<I", vol_bytes, dind * SECTOR + 4 * c)[0]
                if not child:
                    continue
                for k in range(VFS_INDIRECT_PTRS):
                    blocks.append(struct.unpack_from("<I", vol_bytes, child * SECTOR + 4 * k)[0])
        data = bytearray()
        for b in blocks:
            if len(data) >= size:
                break
            if b == 0:
                continue                     # 间接块未用满的占位（不该出现在数据区，防御）
            if b >= total:
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


# ==================== ★ v0.5.8：系统预设头像 / 预设壁纸（两个独立目录）====================
# 用户点名需求：根目录里要有**分开**的两个目录放"系统预设头像"和"预设壁纸"。
#   /Wallpapers/  预设背景图 —— 内置示例由 tools/make_presets.py 生成；
#                 用户把自己的图丢进 res/wallpapers/ 就会一起进卷（png/jpg/jpeg/bmp）。
#   /Avatars/     预设头像 —— 代码画的 6 张 PNG（渐变圆底 + 白色几何标记）。
# 为什么分两个目录：以前壁纸和头像**都指 /logo/kaisi.png**（设置里两个"预设"按钮都写死这条
# 路径），语义混在一起；分开放才能各自枚举 / 各自切换。
IMAGE_EXTS = (".png", ".jpg", ".jpeg", ".bmp")
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)


def _read_images(d, want_prefix=None):
    """读目录里的图片（按名字排序）；want_prefix 非空时只收该前缀。"""
    out = []
    if not os.path.isdir(d):
        return out
    for f in sorted(os.listdir(d)):
        if not f.lower().endswith(IMAGE_EXTS):
            continue
        if want_prefix and not f.startswith(want_prefix):
            continue
        p = os.path.join(d, f)
        if not os.path.isfile(p):
            continue
        out.append((f, open(p, "rb").read()))
    return out


# 内核 img64 的解码上限（img64.cpp 的 "jpeg too large (>2048)" / png 同理）：装进卷里的
# 预设壁纸必须是内核能解的尺寸，否则设置里点"预设壁纸"会如实报错而**用不了**（实测踩到）。
# ★ v0.5.17：内核改成**解码期缩放**后，打包期不再缩用户图（保留原图质量）。
#   这里留一个大上限当护栏：只有病态尺寸（>16384px）才会在打包时等比缩小。
WALL_MAX_PX = 16384


def _fit_image(name, data, limit=WALL_MAX_PX):
    """超过 limit 的图按比例缩到 limit 以内（保持原格式与宽高比）。

    返回 (name, data, info)。info=None 表示没动；否则是 (w, h, nw, nh) 便于构建日志核对。
    没有 Pillow 时**原样返回**（脚本不因此失败；那种情况下超大图会在运行时被如实拒绝）。
    """
    try:
        import io as _io
        from PIL import Image
    except Exception:
        return name, data, None
    try:
        im = Image.open(_io.BytesIO(data))
        w, h = im.size
        if max(w, h) <= limit:
            return name, data, None
        sc = limit / float(max(w, h))
        nw, nh = max(1, int(w * sc + 0.5)), max(1, int(h * sc + 0.5))
        im2 = im if im.mode in ("RGB", "L") else im.convert("RGB")
        im2 = im2.resize((nw, nh), Image.LANCZOS)
        out = _io.BytesIO()
        low = name.lower()
        if low.endswith((".jpg", ".jpeg")):
            im2.save(out, "JPEG", quality=88, optimize=True)
        elif low.endswith(".png"):
            im2.save(out, "PNG", optimize=True)
        else:
            im2.save(out, "BMP")
        return name, out.getvalue(), (w, h, nw, nh)
    except Exception:
        return name, data, None


def preset_wallpapers():
    """壁纸 = 内置示例（build64/presets/wall-*.png）+ 用户投放（res/wallpapers/*）。"""
    builtin = _read_images(os.path.join(REPO, "build64", "presets"), "wall-")
    user = _read_images(os.path.join(REPO, "res", "wallpapers"))
    seen = set()
    out = []
    for name, data in builtin + user:
        key = name.lower()
        if key in seen:
            continue
        seen.add(key)
        name, data, info = _fit_image(name, data)
        if info:
            print("    [wallpaper fit] %s %dx%d -> %dx%d（内核解码上限 %d）"
                  % (name, info[0], info[1], info[2], info[3], WALL_MAX_PX))
        out.append((name, data))
    return out


def preset_avatars():
    """头像 = build64/presets/avatar-*.png（代码画）。"""
    return _read_images(os.path.join(REPO, "build64", "presets"), "avatar-")


def build_volume(shell_bytes, hello_text, total_sectors):
    vol = Volume(total_sectors)
    # ★ v0.6.0：/bin -> /usr/bin（用户要求"像 Linux 那样分类"）。
    #   注意：本文件的 Volume 类**不继承** tcc_pack_win.VolumeEdit，所以这里要显式建两级。
    usr_ino = vol.mkdir("usr", parent=0, mode=0o755)
    bin_ino = vol.mkdir("bin", parent=usr_ino, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)               # 世界可写：任何会话身份都能在里面 mkdir/写文件
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    vol.write_file("sh64hello.txt", hello_text, parent=etc_ino, mode=0o644)
    # ★ v0.5.8：/Wallpapers 与 /Avatars **不在这里写**。
    #   卷链上第一个 pack 步骤是从"原始区"重建卷的，本函数写进去的东西传不下去
    #   （实测：shellvol.img 里有 /Wallpapers，所有后续 *vol.img 都没有）。
    #   预设由**链尾**的 tools/presets_pack_win.py 单独追加（它是最后写 sysdisk.img 的步骤）。
    #   本文件只保留 preset_wallpapers()/preset_avatars() 这两个"读源目录"的助手供它复用。
    return vol.finish()


def _vol_file_map(vol_bytes):
    """把整卷解析成 {路径: (size, crc32)} —— 收口自证用（前后必须逐项一致）。"""
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    ino = struct.unpack_from("<I", vol_bytes, 36)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    ib = struct.unpack_from("<I", vol_bytes, 44)[0]

    def rec(i):
        o = ino * SECTOR + i * ib
        return vol_bytes[o:o + ib]

    def name_of(r):
        n = r[1]
        try:
            return r[40:40 + n].decode("ascii")
        except UnicodeDecodeError:
            return None

    def read_file(r):
        size = struct.unpack_from("<I", r, 4)[0]
        blocks = [struct.unpack_from("<I", r, 8 + 4 * d)[0] for d in range(VFS_DIRECT_BLOCKS)]
        ind = struct.unpack_from("<I", r, 24)[0]
        if ind:
            for k in range(VFS_INDIRECT_PTRS):
                blocks.append(struct.unpack_from("<I", vol_bytes, ind * SECTOR + 4 * k)[0])
        dind = struct.unpack_from("<I", r, 71)[0]
        if dind:
            for c in range(VFS_DIND_CHILDREN):
                child = struct.unpack_from("<I", vol_bytes, dind * SECTOR + 4 * c)[0]
                if not child:
                    continue
                for k in range(VFS_INDIRECT_PTRS):
                    blocks.append(struct.unpack_from("<I", vol_bytes, child * SECTOR + 4 * k)[0])
        data = bytearray()
        for b in blocks:
            if len(data) >= size:
                break
            if b == 0 or b >= total:
                continue
            data += vol_bytes[b * SECTOR:(b + 1) * SECTOR]
        return bytes(data[:size])

    out = {}
    for i in range(inodes):
        r = rec(i)
        if r[0] != 1:
            continue
        chain = []
        cur = i
        ok = True
        while True:
            rr = rec(cur)
            nm = name_of(rr)
            if nm is None:
                ok = False
                break
            chain.append(nm)
            cur = struct.unpack_from("<I", rr, 28)[0]
            if cur == 0:
                break
        if not ok:
            continue
        body = read_file(r)
        out["/" + "/".join(reversed(chain))] = (len(body), _crc32(body))
    return out


def compact_volume(vol_bytes, align=VOL_ALIGN_SECTORS, verify=True):
    """把卷收口到"真实内容 + 对齐 slack"（★ 动态智能镜像的核心）。

    返回 (新卷字节, 收口前扇区数, 收口后扇区数)。verify=True 时做**逐文件 CRC 自证**：
    收口前后每个文件的字节必须完全一致，否则抛错（绝不让静默损坏过关）。
    """
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    v = Volume(total)
    v.load(vol_bytes)
    before = _vol_file_map(vol_bytes) if verify else None
    v.trim(align)
    out = v.finish()
    if verify:
        after = _vol_file_map(out)
        if before != after:
            diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
            raise ValueError("卷收口自证失败：%d 个文件的字节不再一致（例：%s）"
                             % (len(diff), diff[:5]))
    return out, total, v.total


def build_disk(system_bytes, vol_bytes, target_sectors, compact=True):
    """拼出完整盘：引导区（system_bytes）+ 主分区卷。

    ★ v0.10.9：盘大小 = PART_MAIN_LBA + **真实卷扇区数**，不再是固定的 target_sectors。
      卷先按内容收口（compact_volume），所以"打进去多少文件，镜像就多大"。
      target_sectors 退化为"起始容量/日志参考"：卷比它大时照样生成（**上限已废除**）。
    """
    note = None
    if compact:
        vol_bytes, vol_before, vol_after = compact_volume(vol_bytes)
        note = (vol_before, vol_after)
    used = (len(vol_bytes) + SECTOR - 1) // SECTOR
    total = PART_MAIN_LBA + used
    need_sys = (len(system_bytes) + SECTOR - 1) // SECTOR
    if total < need_sys:                       # 引导区比"主分区起点"还长（裸槽 8 扇区）
        total = need_sys
        used = total - PART_MAIN_LBA
    buf = bytearray(total * SECTOR)
    buf[0:len(system_bytes)] = system_bytes
    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)      # 引导区（内核）
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, used)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    base = PART_MAIN_LBA * SECTOR
    buf[base:base + len(vol_bytes)] = vol_bytes
    build_disk.last = {"total": total, "vol": used, "compact": note,
                       "target": int(target_sectors)}
    if note and note[0] != note[1]:
        print("    [VOL64] 动态镜像：卷 %d -> %d 扇区（%.2f MiB），盘 %d 扇区（%.2f MiB）"
              % (note[0], note[1], note[1] * SECTOR / 1048576.0,
                 total, total * SECTOR / 1048576.0))
    return bytes(buf)


build_disk.last = {}


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
    expect = {"/usr/bin/shell.bin": shell_bytes, "/etc/sh64hello.txt": hello_text}
    # ★ v0.5.8：预设壁纸/头像**不在这里自检** —— 本卷不再写它们（链上传不下去），
    #   由链尾的 tools/presets_pack_win.py 追加并自带逐字节回读自检（含 dind 大图）。
    bad = verify(vol_bytes, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    print("    卷自检 OK：/usr/bin/shell.bin = %d B（逐字节回读一致）/ /etc/sh64hello.txt = %d B / /tmp(0777) / 卷 %d 扇区"
          % (len(shell_bytes), len(hello_text), vol_sectors))

    if args.vol:
        open(args.vol, "wb").write(vol_bytes)
        print("    卷镜像：%s（%d B）" % (args.vol, len(vol_bytes)))
    if args.disk:
        system_bytes = open(args.system, "rb").read()
        # system.img = 8073 扇区：尾部的 32584..8072 是 store64 的"裸盘降级槽"（与数据分区起点重叠，
        # 见 kernel/memlayout64.h 的 ★★ 段）。上限放宽到**整块盘**即可 —— 卷写在同一位置会原样
        # 盖掉那 64 个扇区（正常运行时用的是卷里的 /store.a|b，降级槽本来就不该有内容）。
        # ★ v0.10.9：不再用 target_sectors 当盘大小（上限已废除，改由真实卷内容决定）。
        img = build_disk(system_bytes, vol_bytes, args.target_sectors)
        open(args.disk, "wb").write(img)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起，卷 %d 扇区）"
              % (args.disk, len(img), len(img) // SECTOR, PART_MAIN_LBA,
                 build_disk.last.get("vol", 0)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
