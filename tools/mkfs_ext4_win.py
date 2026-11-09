#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/mkfs_ext4_win.py - 生成一个**真实合法**的 ext4 只读夹具镜像（供 VimtuOS Ring 3 ext4 驱动用）

纯 Python 3 标准库，Windows 上 `py -3` 直接可跑。
注意：所有 print 只用 ASCII —— Windows 控制台默认是 GBK(cp936)，
打印中文/非 ASCII 会 UnicodeEncodeError。注释可以中文，文件本身是 UTF-8。

镜像布局（块大小永远 4096，只有 1 个块组）：

    block 0            boot block（全 0）；超级块在**字节偏移 1024**，长 1024
    block 1            组描述符表 GDT（只用了 1 个 32 字节描述符，其余 0）
    block 2            块位图 block bitmap
    block 3            inode 位图 inode bitmap
    block 4..11        inode 表（128 个 inode * 256 B = 8 个块）
    block 12           根目录 "/" 的数据块
    block 13           "/sub" 目录数据块
    block 14           "/hello.txt" 数据（1 块）
    block 15..39       "/big.bin" 数据（25 块，**单个 extent**）
    block 40..41       "/sub/data.bin" 数据（2 块，**单个 extent**）
    其余               全 0

内置固定文件（不解析 --file，直接写死；只有 --blocks 能改镜像大小）：

    /hello.txt       13 B      b"hello vimtu!\\n"
    /big.bin         100000 B  第 i 字节 = (i*31 + 7) & 0xFF
    /sub/data.bin    5000 B    第 i 字节 = (i*17 + 3) & 0xFF

inode 号：2 = "/"（根目录），12 = "/sub"，13 = "/hello.txt"，14 = "/big.bin"，
15 = "/sub/data.bin"。inode 11（/lost+found）不使用、根目录里也不引用它。

写完后**默认**用一个独立的读取函数把镜像重新解析一遍（超级块 -> GDT -> 位图 ->
目录项 -> inode -> extent -> 数据 -> sha256/crc32），任何不一致就
`SELFCHECK FAIL <原因>` + exit 1；全对则 `SELFCHECK OK files=n blocks=n`。

用法：
    py -3 tools/mkfs_ext4_win.py --out build64/ext4fx.img --blocks 16384
    py -3 tools/mkfs_ext4_win.py --out <scratch>/ext4fx.img --blocks 16384 --dump
    py -3 tools/mkfs_ext4_win.py --out <scratch>/ext4fx.img --verify-only   # 只校验已有镜像
退出码：0 = 成功且自校验通过；1 = 参数错误 / 自校验失败
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

# ---------------------------------------------------------------------------
# 常量（ext4 契约）
# ---------------------------------------------------------------------------
BLOCK_SIZE = 4096
LOG_BLOCK_SIZE = 2              # s_log_block_size，4096 = 1 << (10 + 2)
INODE_SIZE = 256                # s_inode_size
INODES_PER_GROUP = 128          # s_inodes_per_group（1 组足够装下全部 inode）
BLOCKS_PER_GROUP = 8 * BLOCK_SIZE   # 32768 = 1 块位图能表示的块数，单组上限
DESC_SIZE = 32                  # 不置 64BIT 时组描述符就是 32 B
EXT4_MAGIC = 0xEF53
EXT4_EXT_MAGIC = 0xF30A
FIRST_DATA_BLOCK = 0            # 4 KiB 块：第一个数据块就是块 0
FIXED_TIME = 1700000000         # 固定时间戳（可重现镜像）

FEATURE_COMPAT = 0x0
INCOMPAT_FILETYPE = 0x0002      # 目录项 file_type 字段有效
INCOMPAT_EXTENTS = 0x0040       # inode 用 extent 树
FEATURE_INCOMPAT = INCOMPAT_FILETYPE | INCOMPAT_EXTENTS     # = 0x0042
FEATURE_RO_COMPAT = 0x0000
UUID_BYTES = bytes(range(0x10, 0x20))
VOLUME_NAME = b"vimtu-ext4fx"

# 目录项 file_type
FT_REG_FILE = 1
FT_DIR = 2

# 布局（写死，方便对方 C 解析器断言）
BOOT_BLOCK = 0
GDT_BLOCK = FIRST_DATA_BLOCK + 1        # = 1
BLOCK_BITMAP_BLOCK = 2
INODE_BITMAP_BLOCK = 3
INODE_TABLE_BLOCK = 4
INODE_TABLE_BLOCKS = INODES_PER_GROUP * INODE_SIZE // BLOCK_SIZE    # = 8
ROOT_DIR_BLOCK = 12
SUB_DIR_BLOCK = 13
HELLO_BLOCK = 14
BIG_BLOCK = 15
BIG_BLOCK_COUNT = 25            # ceil(100000 / 4096) = 25
DATA_BLOCK = BIG_BLOCK + BIG_BLOCK_COUNT        # = 40
DATA_BLOCK_COUNT = 2            # ceil(5000 / 4096) = 2
FIRST_UNUSED_BLOCK = DATA_BLOCK + DATA_BLOCK_COUNT      # = 42
USED_BLOCK_COUNT = FIRST_UNUSED_BLOCK                   # 块 0..41 共 42 个

# inode 号
INO_ROOT = 2
INO_SUB = 12
INO_HELLO = 13
INO_BIG = 14
INO_DATA = 15
FIRST_INO = 11                  # rev 1 的 s_first_ino；11 起是普通可用 inode
USED_INODES = (INO_ROOT, INO_SUB, INO_HELLO, INO_BIG, INO_DATA)     # 5 个
USED_DIRS = (INO_ROOT, INO_SUB)                                     # 2 个

MODE_DIR = 0o040755
MODE_FILE = 0o100644

HELLO_TEXT = b"hello vimtu!\n"      # 13 B

# 固定文件清单：路径 -> (inode, size, 生成器参数)；mul = 0 表示用字面量
FILE_SPECS = (
    ("/hello.txt", INO_HELLO, HELLO_TEXT, 0, 0, 1),
    ("/big.bin", INO_BIG, None, 31, 7, BIG_BLOCK_COUNT),
    ("/sub/data.bin", INO_DATA, None, 17, 3, DATA_BLOCK_COUNT),
)


def pattern_bytes(size, mul, add):
    """确定性内容：第 i 字节 = (i*mul + add) & 0xFF。"""
    return bytes(((i * mul + add) & 0xFF) for i in range(size))


def file_content(spec):
    """按 FILE_SPECS 的一条生成期望内容（写镜像和自校验都用同一份规范）。"""
    path, _ino, literal, mul, add, _nblk = spec
    if literal is not None:
        return literal
    return pattern_bytes(SIZES[path], mul, add)


SIZES = {
    "/hello.txt": len(HELLO_TEXT),
    "/big.bin": 100000,
    "/sub/data.bin": 5000,
}


def ascii_bytes(b):
    """把字节串渲染成纯 ASCII 转义串，用于打印。"""
    out = []
    for ch in b:
        if 0x20 <= ch < 0x7F and ch not in (0x22, 0x5C):
            out.append(chr(ch))
        else:
            out.append("\\x%02x" % ch)
    return "".join(out)


# ---------------------------------------------------------------------------
# 构造（写入侧）
# ---------------------------------------------------------------------------
def build_superblock(blocks, free_blocks, free_inodes):
    sb = bytearray(1024)

    def p16(off, val):
        struct.pack_into("<H", sb, off, val & 0xFFFF)

    def p32(off, val):
        struct.pack_into("<I", sb, off, val & 0xFFFFFFFF)

    p32(0x00, INODES_PER_GROUP)          # s_inodes_count
    p32(0x04, blocks)                    # s_blocks_count_lo
    p32(0x08, 0)                         # s_r_blocks_count_lo
    p32(0x0C, free_blocks)               # s_free_blocks_count_lo
    p32(0x10, free_inodes)               # s_free_inodes_count_lo
    p32(0x14, FIRST_DATA_BLOCK)          # s_first_data_block = 0
    p32(0x18, LOG_BLOCK_SIZE)            # s_log_block_size = 2
    p32(0x1C, LOG_BLOCK_SIZE)            # s_log_cluster_size = 2
    p32(0x20, BLOCKS_PER_GROUP)          # s_blocks_per_group（覆盖全部 --blocks）
    p32(0x24, BLOCKS_PER_GROUP)          # s_clusters_per_group
    p32(0x28, INODES_PER_GROUP)          # s_inodes_per_group
    p32(0x2C, FIXED_TIME)                # s_mtime
    p32(0x30, FIXED_TIME)                # s_wtime
    p16(0x34, 0)                         # s_mnt_count
    p16(0x36, 0xFFFF)                    # s_max_mnt_count
    p16(0x38, EXT4_MAGIC)                # s_magic = 0xEF53
    p16(0x3A, 1)                         # s_state = cleanly unmounted
    p16(0x3C, 1)                         # s_errors = continue
    p16(0x3E, 0)                         # s_minor_rev_level
    p32(0x40, FIXED_TIME)                # s_lastcheck
    p32(0x44, 0)                         # s_checkinterval
    p32(0x48, 0)                         # s_creator_os = linux
    p32(0x4C, 1)                         # s_rev_level = DYNAMIC (1)
    p16(0x50, 0)                         # s_def_resuid
    p16(0x52, 0)                         # s_def_resgid
    p32(0x54, FIRST_INO)                 # s_first_ino
    p16(0x58, INODE_SIZE)                # s_inode_size = 256
    p16(0x5A, 0)                         # s_block_group_nr = 0
    p32(0x5C, FEATURE_COMPAT)            # s_feature_compat
    p32(0x60, FEATURE_INCOMPAT)          # s_feature_incompat = FILETYPE|EXTENTS
    p32(0x64, FEATURE_RO_COMPAT)         # s_feature_ro_compat = 0
    sb[0x68:0x78] = UUID_BYTES           # s_uuid
    sb[0x78:0x78 + len(VOLUME_NAME)] = VOLUME_NAME     # s_volume_name
    p32(0xE0, 0)                         # s_journal_inum
    p32(0xE4, 0)                         # s_journal_dev
    p32(0xE8, 0)                         # s_last_orphan
    p16(0xFE, DESC_SIZE)                 # s_desc_size = 32
    p32(0x100, 0)                        # s_default_mount_opts
    p32(0x104, 0)                        # s_first_meta_bg
    return bytes(sb)


def build_group_desc(free_blocks, free_inodes, used_dirs):
    gd = bytearray(DESC_SIZE)
    struct.pack_into("<I", gd, 0x00, BLOCK_BITMAP_BLOCK)    # bg_block_bitmap_lo
    struct.pack_into("<I", gd, 0x04, INODE_BITMAP_BLOCK)    # bg_inode_bitmap_lo
    struct.pack_into("<I", gd, 0x08, INODE_TABLE_BLOCK)     # bg_inode_table_lo
    struct.pack_into("<H", gd, 0x0C, free_blocks)           # bg_free_blocks_count_lo
    struct.pack_into("<H", gd, 0x0E, free_inodes)           # bg_free_inodes_count_lo
    struct.pack_into("<H", gd, 0x10, used_dirs)             # bg_used_dirs_count_lo
    struct.pack_into("<H", gd, 0x12, 0)                     # bg_flags
    struct.pack_into("<H", gd, 0x14, 0)                     # bg_exclude_bitmap_lo
    struct.pack_into("<H", gd, 0x16, 0)                     # bg_block_bitmap_csum_lo
    struct.pack_into("<H", gd, 0x18, 0)                     # bg_inode_bitmap_csum_lo
    struct.pack_into("<H", gd, 0x1A, 0)                     # bg_itable_unused_lo
    struct.pack_into("<H", gd, 0x1C, 0)                     # bg_checksum
    return bytes(gd)


def build_extent_iblock(extents):
    """extents = [(logical_block, nblocks, phys_start), ...]，最多 4 个，depth 0。

    头部 12 B：eh_magic(u16) eh_entries(u16) eh_max(u16) eh_depth(u16) eh_generation(u32)
    每项 12 B：ee_block(u32) ee_len(u16) ee_start_hi(u16) ee_start_lo(u32)
    剩下 36 字节补 0。
    """
    if not extents:
        raise ValueError("empty extent list")
    if len(extents) > 4:
        raise ValueError("too many extents (max 4, depth-0 tree)")
    buf = bytearray(60)
    struct.pack_into("<HHHHI", buf, 0, EXT4_EXT_MAGIC, len(extents), 4, 0, 0)
    off = 12
    for logical, nblocks, phys in extents:
        if not (1 <= nblocks <= 32768):
            raise ValueError("extent length out of range")
        struct.pack_into("<IHHI", buf, off, logical, nblocks,
                         (phys >> 32) & 0xFFFF, phys & 0xFFFFFFFF)
        off += 12
    return bytes(buf)


def build_inode(mode, size, links, iblock, blocks_512, is_dir=False):
    ino = bytearray(INODE_SIZE)
    struct.pack_into("<H", ino, 0x00, mode)
    struct.pack_into("<H", ino, 0x02, 0)                 # i_uid
    struct.pack_into("<I", ino, 0x04, size & 0xFFFFFFFF)  # i_size_lo
    struct.pack_into("<I", ino, 0x08, FIXED_TIME)        # i_atime
    struct.pack_into("<I", ino, 0x0C, FIXED_TIME)        # i_ctime
    struct.pack_into("<I", ino, 0x10, FIXED_TIME)        # i_mtime
    struct.pack_into("<I", ino, 0x14, 0)                 # i_dtime
    struct.pack_into("<H", ino, 0x18, 0)                 # i_gid
    struct.pack_into("<H", ino, 0x1A, links)             # i_links_count
    struct.pack_into("<I", ino, 0x1C, blocks_512)        # i_blocks_lo (512 B 扇区数)
    struct.pack_into("<I", ino, 0x20, 0)                 # i_flags（不用 htree / extents flag bit）
    struct.pack_into("<I", ino, 0x24, 0)                 # i_osd1 / l_i_version
    ino[0x28:0x28 + 60] = iblock                         # i_block[60]
    struct.pack_into("<I", ino, 0x64, 0)                 # i_generation
    struct.pack_into("<I", ino, 0x68, 0)                 # i_file_acl_lo
    struct.pack_into("<I", ino, 0x6C, (size >> 32) & 0xFFFFFFFF)   # i_size_high
    return bytes(ino)


def build_dir_block(entries):
    """entries = [(name, inode, file_type)]，最后一个目录项的 rec_len 延伸到块尾。"""
    buf = bytearray(BLOCK_SIZE)
    off = 0
    last = len(entries) - 1
    for idx, (name, inode, ftype) in enumerate(entries):
        raw = name.encode("ascii")
        if idx == last:
            rec_len = BLOCK_SIZE - off
        else:
            rec_len = (8 + len(raw) + 3) & ~3
        if rec_len < 8 + len(raw) or rec_len % 4:
            raise ValueError("bad rec_len for %r" % name)
        struct.pack_into("<IHBB", buf, off, inode, rec_len, len(raw), ftype)
        buf[off + 8:off + 8 + len(raw)] = raw
        off += rec_len
    if off != BLOCK_SIZE:
        raise ValueError("directory block not filled exactly")
    return bytes(buf)


def bitmap_with_bits(total_bits, used_bits):
    bm = bytearray(total_bits // 8)
    for b in used_bits:
        bm[b >> 3] |= 1 << (b & 7)
    return bytes(bm)


def inode_bitmap_used():
    """inode 号 -> 位图位序号（位 0 = inode 1）。"""
    return [n - 1 for n in USED_INODES]


def build_image(path, blocks):
    """生成镜像，返回 {blk: bytes} 的写入计划 + 统计信息。"""
    if blocks < FIRST_UNUSED_BLOCK + 1:
        raise SystemExit("mkfs_ext4_win: ERROR --blocks must be >= %d" % (FIRST_UNUSED_BLOCK + 1))
    if blocks > BLOCKS_PER_GROUP:
        raise SystemExit("mkfs_ext4_win: ERROR --blocks must be <= %d (single block group)"
                         % BLOCKS_PER_GROUP)

    free_blocks = blocks - USED_BLOCK_COUNT
    free_inodes = INODES_PER_GROUP - len(USED_INODES)

    # 内容
    hello = HELLO_TEXT
    big = pattern_bytes(SIZES["/big.bin"], 31, 7)
    data = pattern_bytes(SIZES["/sub/data.bin"], 17, 3)

    # inode
    inodes = {}
    inodes[INO_ROOT] = build_inode(MODE_DIR, BLOCK_SIZE, 3,
                                   build_extent_iblock([(0, 1, ROOT_DIR_BLOCK)]),
                                   1 * BLOCK_SIZE // 512, is_dir=True)
    inodes[INO_SUB] = build_inode(MODE_DIR, BLOCK_SIZE, 2,
                                  build_extent_iblock([(0, 1, SUB_DIR_BLOCK)]),
                                  1 * BLOCK_SIZE // 512, is_dir=True)
    inodes[INO_HELLO] = build_inode(MODE_FILE, len(hello), 1,
                                    build_extent_iblock([(0, 1, HELLO_BLOCK)]),
                                    1 * BLOCK_SIZE // 512)
    inodes[INO_BIG] = build_inode(MODE_FILE, len(big), 1,
                                  build_extent_iblock([(0, BIG_BLOCK_COUNT, BIG_BLOCK)]),
                                  BIG_BLOCK_COUNT * BLOCK_SIZE // 512)
    inodes[INO_DATA] = build_inode(MODE_FILE, len(data), 1,
                                   build_extent_iblock([(0, DATA_BLOCK_COUNT, DATA_BLOCK)]),
                                   DATA_BLOCK_COUNT * BLOCK_SIZE // 512)

    root_dir = build_dir_block([
        (".", INO_ROOT, FT_DIR),
        ("..", INO_ROOT, FT_DIR),
        ("hello.txt", INO_HELLO, FT_REG_FILE),
        ("big.bin", INO_BIG, FT_REG_FILE),
        ("sub", INO_SUB, FT_DIR),
    ])
    sub_dir = build_dir_block([
        (".", INO_SUB, FT_DIR),
        ("..", INO_ROOT, FT_DIR),
        ("data.bin", INO_DATA, FT_REG_FILE),
    ])

    # inode 表：8 个块，按 inode 号从 1 开始紧密排列
    itable = bytearray(INODE_TABLE_BLOCKS * BLOCK_SIZE)
    for ino_num, blob in inodes.items():
        off = (ino_num - 1) * INODE_SIZE
        itable[off:off + INODE_SIZE] = blob
    itable_blocks = [bytes(itable[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE])
                     for i in range(INODE_TABLE_BLOCKS)]

    # 位图
    blk_bm = bitmap_with_bits(blocks, range(USED_BLOCK_COUNT))
    ino_bm = bitmap_with_bits(INODES_PER_GROUP, inode_bitmap_used())

    # 写入计划
    plan = {
        BOOT_BLOCK: build_superblock(blocks, free_blocks, free_inodes),   # 块 0 只写前 1024 B
        GDT_BLOCK: build_group_desc(free_blocks, free_inodes, len(USED_DIRS)),
        BLOCK_BITMAP_BLOCK: blk_bm,
        INODE_BITMAP_BLOCK: ino_bm,
        ROOT_DIR_BLOCK: root_dir,
        SUB_DIR_BLOCK: sub_dir,
        HELLO_BLOCK: hello.ljust(BLOCK_SIZE, b"\x00"),
    }
    for i, blk in enumerate(itable_blocks):
        plan[INODE_TABLE_BLOCK + i] = blk
    for i in range(BIG_BLOCK_COUNT):
        chunk = big[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
        plan[BIG_BLOCK + i] = chunk.ljust(BLOCK_SIZE, b"\x00")
    for i in range(DATA_BLOCK_COUNT):
        chunk = data[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
        plan[DATA_BLOCK + i] = chunk.ljust(BLOCK_SIZE, b"\x00")

    # 落盘：先 truncate 到最终尺寸，再逐块 seek+write（未使用的块保持 0）
    dad = os.path.dirname(os.path.abspath(path))
    if dad and not os.path.isdir(dad):
        os.makedirs(dad)
    with open(path, "wb") as f:
        f.truncate(blocks * BLOCK_SIZE)
        for blk in sorted(plan):
            payload = plan[blk]
            if blk == BOOT_BLOCK:
                # 超级块在字节偏移 1024，块 0 前 1024 B 保持 0
                f.seek(1024)
                f.write(payload)
            else:
                f.seek(blk * BLOCK_SIZE)
                f.write(payload)
    return {
        "blocks": blocks,
        "free_blocks": free_blocks,
        "free_inodes": free_inodes,
        "used_dirs": len(USED_DIRS),
        "plan_blocks": sorted(plan),
    }


# ---------------------------------------------------------------------------
# 独立读取侧（自校验）—— 不复用上面的构造中间变量，全部从镜像字节重新解析
# ---------------------------------------------------------------------------
class Image(object):
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.size = self.f.tell()

    def close(self):
        self.f.close()

    def read_at(self, off, n):
        self.f.seek(off)
        blob = self.f.read(n)
        if len(blob) != n:
            raise CheckError("short read at %d (want %d got %d)" % (off, n, len(blob)))
        return blob

    def read_block(self, blk, nblk=1):
        return self.read_at(blk * BLOCK_SIZE, nblk * BLOCK_SIZE)


class CheckError(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise CheckError(msg)


def parse_superblock(img):
    sb = img.read_at(1024, 1024)
    g16 = lambda o: struct.unpack_from("<H", sb, o)[0]
    g32 = lambda o: struct.unpack_from("<I", sb, o)[0]
    out = {
        "magic": g16(0x38),
        "inodes_count": g32(0x00),
        "blocks_count": g32(0x04),
        "free_blocks": g32(0x0C),
        "free_inodes": g32(0x10),
        "first_data_block": g32(0x14),
        "log_block_size": g32(0x18),
        "blocks_per_group": g32(0x20),
        "inodes_per_group": g32(0x28),
        "rev_level": g32(0x4C),
        "first_ino": g32(0x54),
        "inode_size": g16(0x58),
        "block_group_nr": g16(0x5A),
        "feature_compat": g32(0x5C),
        "feature_incompat": g32(0x60),
        "feature_ro_compat": g32(0x64),
        "desc_size": g16(0xFE),
    }
    check(out["magic"] == EXT4_MAGIC, "superblock magic 0x%04X != 0xEF53" % out["magic"])
    check(out["log_block_size"] == 2, "s_log_block_size %d != 2" % out["log_block_size"])
    bs = 1024 << out["log_block_size"]
    check(bs == BLOCK_SIZE, "block size %d != %d" % (bs, BLOCK_SIZE))
    check(out["first_data_block"] == 0, "s_first_data_block %d != 0" % out["first_data_block"])
    check(out["inode_size"] == INODE_SIZE, "s_inode_size %d != 256" % out["inode_size"])
    check(out["block_group_nr"] == 0, "s_block_group_nr %d != 0" % out["block_group_nr"])
    check(out["desc_size"] == DESC_SIZE, "s_desc_size %d != 32" % out["desc_size"])
    check(out["rev_level"] == 1, "s_rev_level %d != 1" % out["rev_level"])
    check(out["blocks_per_group"] > 0, "s_blocks_per_group == 0")
    check(out["inodes_per_group"] > 0, "s_inodes_per_group == 0")
    check((out["feature_incompat"] & INCOMPAT_EXTENTS) != 0, "EXTENTS feature missing")
    check((out["feature_incompat"] & INCOMPAT_FILETYPE) != 0, "FILETYPE feature missing")
    for bit, name in ((0x80, "64BIT"), (0x200, "FLEX_BG"), (0x400, "META_CSUM")):
        check((out["feature_incompat"] & bit) == 0, "feature_incompat bit %s set" % name)
    for bit, name in ((0x400, "METADATA_CSUM"), (0x200, "GDT_CSUM")):
        check((out["feature_ro_compat"] & bit) == 0, "feature_ro_compat bit %s set" % name)
    check(out["inodes_count"] > 0, "s_inodes_count == 0")
    check(out["blocks_count"] > 0, "s_blocks_count == 0")
    return out


def parse_group_desc(img, sb):
    gdt_off = (sb["first_data_block"] + 1) * BLOCK_SIZE
    gd = img.read_at(gdt_off, DESC_SIZE)
    g32 = lambda o: struct.unpack_from("<I", gd, o)[0]
    g16 = lambda o: struct.unpack_from("<H", gd, o)[0]
    out = {
        "block_bitmap": g32(0x00),
        "inode_bitmap": g32(0x04),
        "inode_table": g32(0x08),
        "free_blocks": g16(0x0C),
        "free_inodes": g16(0x0E),
        "used_dirs": g16(0x10),
    }
    n = sb["blocks_count"]
    check(out["block_bitmap"] < n, "bg_block_bitmap %d out of range" % out["block_bitmap"])
    check(out["inode_bitmap"] < n, "bg_inode_bitmap %d out of range" % out["inode_bitmap"])
    check(out["inode_table"] < n, "bg_inode_table %d out of range" % out["inode_table"])
    check(out["free_blocks"] <= n, "bg_free_blocks_count %d > blocks %d" % (out["free_blocks"], n))
    return out


def inode_bytes(img, sb, gd, ino):
    check(1 <= ino <= sb["inodes_count"], "inode %d out of range" % ino)
    per_group = sb["inodes_per_group"]
    grp = (ino - 1) // per_group
    check(grp == 0, "multi-group inode %d not supported by fixture" % ino)
    idx = (ino - 1) % per_group
    off = gd["inode_table"] * BLOCK_SIZE + idx * sb["inode_size"]
    return img.read_at(off, sb["inode_size"])


def parse_inode(img, sb, gd, ino):
    raw = inode_bytes(img, sb, gd, ino)
    g16 = lambda o: struct.unpack_from("<H", raw, o)[0]
    g32 = lambda o: struct.unpack_from("<I", raw, o)[0]
    out = {
        "ino": ino,
        "mode": g16(0x00),
        "size_lo": g32(0x04),
        "links": g16(0x1A),
        "blocks_lo": g32(0x1C),
        "flags": g32(0x20),
        "iblock": raw[0x28:0x28 + 60],
        "size_high": g32(0x6C),
    }
    out["size"] = out["size_lo"] | (out["size_high"] << 32)
    check(out["size"] == out["size_lo"], "inode %d: i_size_high used" % ino)
    return out


def parse_extents(iblock):
    magic, entries, emax, depth, _gen = struct.unpack_from("<HHHHI", iblock, 0)
    check(magic == EXT4_EXT_MAGIC, "extent magic 0x%04X != 0xF30A" % magic)
    check(emax == 4, "extent eh_max %d != 4" % emax)
    check(depth == 0, "extent depth %d != 0" % depth)
    check(1 <= entries <= 4, "extent entries %d out of range" % entries)
    exts = []
    for i in range(entries):
        off = 12 + i * 12
        lblk, nlen, hi, lo = struct.unpack_from("<IHHI", iblock, off)
        check(nlen != 0, "extent %d has ee_len 0" % i)
        check(nlen <= 32768, "extent %d is uninitialized/long (ee_len %d)" % (i, nlen))
        exts.append((lblk, nlen, (hi << 32) | lo))
    # 逻辑块必须从 0 开始且连续
    expect = 0
    for lblk, nlen, _start in exts:
        check(lblk == expect, "extent logical block %d != %d" % (lblk, expect))
        expect += nlen
    return exts


def read_file_data(img, sb, gd, ino):
    inode = parse_inode(img, sb, gd, ino)
    exts = parse_extents(inode["iblock"])
    total = sum(e[1] for e in exts) * BLOCK_SIZE
    check(inode["size"] <= total, "inode %d: size %d > extent capacity %d"
          % (ino, inode["size"], total))
    buf = bytearray()
    for _lblk, nlen, start in exts:
        check(start + nlen <= sb["blocks_count"],
              "inode %d: extent [%d,+%d) beyond blocks_count %d"
              % (ino, start, nlen, sb["blocks_count"]))
        buf += img.read_block(start, nlen)
    return inode, bytes(buf[:inode["size"]])


def parse_dir_block(blob, block_no):
    off = 0
    ents = []
    total = len(blob)
    while off < total:
        check(off + 8 <= total, "dir block %d: truncated entry header at %d" % (block_no, off))
        ino, rec_len, name_len, ftype = struct.unpack_from("<IHBB", blob, off)
        check(rec_len >= 8, "dir block %d: rec_len %d < 8 at %d" % (block_no, rec_len, off))
        check(rec_len % 4 == 0, "dir block %d: rec_len %d not 4-aligned" % (block_no, rec_len))
        check(off + rec_len <= total, "dir block %d: rec_len %d overruns block at %d"
              % (block_no, rec_len, off))
        check(name_len <= rec_len - 8, "dir block %d: name_len %d > rec_len-8 at %d"
              % (block_no, name_len, off))
        name = blob[off + 8:off + 8 + name_len]
        ents.append({"inode": ino, "rec_len": rec_len, "name_len": name_len,
                     "file_type": ftype, "name": name, "offset": off,
                     "is_last": off + rec_len == total})
        off += rec_len
    check(off == total, "dir block %d: entries do not end exactly at block end" % block_no)
    return ents


def read_dir(img, sb, gd, ino):
    inode, blob = read_file_data(img, sb, gd, ino)
    check((inode["mode"] & 0xF000) == 0x4000, "inode %d is not a directory (mode 0o%o)"
          % (ino, inode["mode"]))
    check(inode["size"] % BLOCK_SIZE == 0, "inode %d: dir size %d not block aligned"
          % (ino, inode["size"]))
    ents = []
    for b in range(inode["size"] // BLOCK_SIZE):
        chunk = blob[b * BLOCK_SIZE:(b + 1) * BLOCK_SIZE]
        ents.extend(parse_dir_block(chunk, b))
    last = ents[-1]
    check(last["is_last"], "inode %d: last dirent does not reach block end" % ino)
    return inode, ents


def resolve(img, sb, gd, path):
    """从根目录逐段解析 path，返回 inode 号。"""
    ino = INO_ROOT
    if path == "/":
        return ino
    for part in path.strip("/").split("/"):
        _d, ents = read_dir(img, sb, gd, ino)
        hit = [e for e in ents if e["name"].decode("ascii", "replace") == part]
        check(len(hit) == 1, "path %s: component %r not found in dir inode %d"
              % (path, part, ino))
        ino = hit[0]["inode"]
    return ino


def verify_image(path, blocks_expected):
    """从镜像字节独立重建一遍，和规范比对。返回统计 dict。"""
    img = Image(path)
    try:
        check(img.size == blocks_expected * BLOCK_SIZE,
              "image size %d != blocks*4096 %d" % (img.size, blocks_expected * BLOCK_SIZE))
        sb = parse_superblock(img)
        check(sb["blocks_count"] == blocks_expected,
              "s_blocks_count %d != %d" % (sb["blocks_count"], blocks_expected))
        gd = parse_group_desc(img, sb)

        # --- 位图一致性 ---
        blk_bm = img.read_block(gd["block_bitmap"])
        ino_bm = img.read_block(gd["inode_bitmap"])
        used_blocks = set()
        for b in range(sb["blocks_count"]):
            if blk_bm[b >> 3] & (1 << (b & 7)):
                used_blocks.add(b)
        check(len(used_blocks) == sb["blocks_count"] - sb["free_blocks"],
              "block bitmap has %d used bits but s_free_blocks_count says %d used"
              % (len(used_blocks), sb["blocks_count"] - sb["free_blocks"]))
        check(gd["free_blocks"] == sb["free_blocks"],
              "bg_free_blocks_count %d != s_free_blocks_count %d"
              % (gd["free_blocks"], sb["free_blocks"]))
        free_inodes_bitmap = sum(1 for b in range(sb["inodes_count"])
                                 if not (ino_bm[b >> 3] & (1 << (b & 7))))
        check(free_inodes_bitmap == sb["free_inodes"],
              "inode bitmap free %d != s_free_inodes_count %d"
              % (free_inodes_bitmap, sb["free_inodes"]))
        check(gd["free_inodes"] == sb["free_inodes"],
              "bg_free_inodes_count %d != s_free_inodes_count %d"
              % (gd["free_inodes"], sb["free_inodes"]))

        # 元数据块必须置位
        for blk in ([BOOT_BLOCK, GDT_BLOCK, gd["block_bitmap"], gd["inode_bitmap"]]
                    + list(range(gd["inode_table"], gd["inode_table"] + INODE_TABLE_BLOCKS))):
            check(blk in used_blocks, "block %d should be marked used in bitmap" % blk)
        # used_dirs
        check(gd["used_dirs"] == len(USED_DIRS),
              "bg_used_dirs_count %d != %d" % (gd["used_dirs"], len(USED_DIRS)))
        # 5 个 inode 的位必须置位，11 号必须空闲
        for ino in USED_INODES:
            bit = ino - 1
            check(ino_bm[bit >> 3] & (1 << (bit & 7)), "inode bitmap bit for inode %d clear" % ino)
        bit11 = FIRST_INO - 1
        check(not (ino_bm[bit11 >> 3] & (1 << (bit11 & 7))),
              "inode 11 (lost+found) should be free")

        # --- 目录树 ---
        root_inode, root_ents = read_dir(img, sb, gd, INO_ROOT)
        check(root_inode["size"] == BLOCK_SIZE,
              "root dir size %d != %d" % (root_inode["size"], BLOCK_SIZE))
        names = [e["name"].decode("ascii", "replace") for e in root_ents]
        check(names[:2] == [".", ".."], "root dir first entries %r" % (names[:2],))
        for e in root_ents:
            if e["name"] == b".":
                check(e["inode"] == INO_ROOT, "root '.' inode %d" % e["inode"])
                check(e["file_type"] == FT_DIR, "root '.' file_type %d" % e["file_type"])
            if e["name"] == b"..":
                check(e["inode"] == INO_ROOT, "root '..' inode %d" % e["inode"])
            if e["name"] in (b".", b".."):
                continue
            check(e["file_type"] in (FT_REG_FILE, FT_DIR),
                  "root entry %r has file_type %d" % (e["name"], e["file_type"]))
        check(b"lost+found" not in [e["name"] for e in root_ents],
              "root dir references lost+found but inode 11 is unused")

        _sub_inode, sub_ents = read_dir(img, sb, gd, INO_SUB)
        sub_names = [e["name"].decode("ascii", "replace") for e in sub_ents]
        check(sub_names[:2] == [".", ".."], "/sub first entries %r" % (sub_names[:2],))

        # --- 期望值 ---
        expected = {}
        for spec in FILE_SPECS:
            p = spec[0]
            expected[p] = {
                "ino": spec[1],
                "data": file_content(spec),
            }

        found_paths = sorted(expected.keys())

        # 目录里不许有多余文件
        root_files = sorted(e["name"].decode("ascii") for e in root_ents
                            if e["name"] not in (b".", b"..")
                            and e["file_type"] == FT_REG_FILE)
        check(root_files == ["big.bin", "hello.txt"], "root regular files %r" % (root_files,))
        sub_files = sorted(e["name"].decode("ascii") for e in sub_ents
                           if e["name"] not in (b".", b"..")
                           and e["file_type"] == FT_REG_FILE)
        check(sub_files == ["data.bin"], "/sub regular files %r" % (sub_files,))

        results = []
        for p in found_paths:
            exp = expected[p]
            ino = resolve(img, sb, gd, p)
            check(ino == exp["ino"], "%s: resolved inode %d != %d" % (p, ino, exp["ino"]))
            inode, blob = read_file_data(img, sb, gd, ino)
            check((inode["mode"] & 0xF000) == 0x8000,
                  "%s: mode 0o%o is not a regular file" % (p, inode["mode"]))
            check(inode["size"] == len(exp["data"]),
                  "%s: size %d != %d" % (p, inode["size"], len(exp["data"])))
            check(blob == exp["data"], "%s: content mismatch vs spec pattern" % p)
            # 每个数据块都要在位图里置位
            for _lblk, nlen, start in parse_extents(inode["iblock"]):
                for b in range(start, start + nlen):
                    check(b in used_blocks,
                          "%s: data block %d not marked used in bitmap" % (p, b))
            results.append({
                "path": p,
                "ino": ino,
                "size": inode["size"],
                "nblocks": sum(e[1] for e in parse_extents(inode["iblock"])),
                "sha256": hashlib.sha256(blob).hexdigest(),
                "crc32": zlib.crc32(blob) & 0xFFFFFFFF,
                "data": blob,
                "iblock": inode["iblock"],
            })

        # --- 未被使用的块必须是 0 ---
        if img.size <= 256 * 1024 * 1024:
            check_zero_outside(img, used_blocks)

        # --- 备份：块 0 的 1024 之前必须为 0 ---
        check(img.read_at(0, 1024) == b"\x00" * 1024, "boot block before superblock is not zero")

        return {
            "superblock": sb,
            "group_desc": gd,
            "root_entries": root_ents,
            "sub_entries": sub_ents,
            "files": results,
            "used_blocks": sorted(used_blocks),
        }
    finally:
        img.close()


def check_zero_outside(img, used_blocks):
    """扫描镜像：不在 used_blocks 里的块必须全 0（要求 9 的硬断言）。"""
    total = img.size // BLOCK_SIZE
    chunk_blocks = 512
    blk = 0
    while blk < total:
        n = min(chunk_blocks, total - blk)
        chunk = img.read_block(blk, n)
        for i in range(n):
            b = blk + i
            if b in used_blocks:
                continue
            piece = chunk[i * BLOCK_SIZE:(i + 1) * BLOCK_SIZE]
            if piece != b"\x00" * BLOCK_SIZE:
                off = piece.find(next(c for c in piece if c != 0))
                raise CheckError("unused block %d is not zero (first nonzero byte at +%d)"
                                 % (b, off))
        blk += n


# ---------------------------------------------------------------------------
# 打印（全部 ASCII）
# ---------------------------------------------------------------------------
def print_report(stats):
    sb = stats["superblock"]
    gd = stats["group_desc"]
    path = stats["path"]
    blocks = stats["blocks"]
    print("IMAGE path=%s blocks=%d block_size=%d size=%d"
          % (path, blocks, BLOCK_SIZE, blocks * BLOCK_SIZE))
    print("SUPERBLOCK offset=1024 magic=0x%04X inodes_count=%d blocks_count=%d "
          "first_data_block=%d log_block_size=%d blocks_per_group=%d inodes_per_group=%d "
          "inode_size=%d desc_size=%d rev_level=%d first_ino=%d"
          % (sb["magic"], sb["inodes_count"], sb["blocks_count"], sb["first_data_block"],
             sb["log_block_size"], sb["blocks_per_group"], sb["inodes_per_group"],
             sb["inode_size"], sb["desc_size"], sb["rev_level"], sb["first_ino"]))
    print("FEATURES compat=0x%08X incompat=0x%08X ro_compat=0x%08X"
          % (sb["feature_compat"], sb["feature_incompat"], sb["feature_ro_compat"]))
    print("GROUP0 gdt_block=%d block_bitmap=%d inode_bitmap=%d inode_table=%d "
          "free_blocks=%d free_inodes=%d used_dirs=%d"
          % (GDT_BLOCK, gd["block_bitmap"], gd["inode_bitmap"], gd["inode_table"],
             gd["free_blocks"], gd["free_inodes"], gd["used_dirs"]))
    for f in stats["files"]:
        print("FILE path=%s ino=%d size=%d blocks=%d sha256_16=%s crc32=0x%08X"
              % (f["path"], f["ino"], f["size"], f["nblocks"],
                 f["sha256"][:16], f["crc32"]))
    for f in stats["files"]:
        if f["size"] <= 64:
            print("CONTENT path=%s bytes=%s" % (f["path"], ascii_bytes(f["data"])))


def print_dump(stats):
    print("--- dump ---")
    for label, ents in (("ROOT", stats["root_entries"]), ("SUB", stats["sub_entries"])):
        for e in ents:
            print("DIRENT dir=%s off=%d ino=%d rec_len=%d name_len=%d file_type=%d name=%s%s"
                  % (label, e["offset"], e["inode"], e["rec_len"], e["name_len"],
                     e["file_type"], e["name"].decode("ascii", "replace"),
                     " (extends_to_block_end)" if e["is_last"] else ""))
    for f in stats["files"]:
        print("INODE ino=%d path=%s i_block60=%s"
              % (f["ino"], f["path"], f["iblock"].hex()))
    # 目录 inode 的 i_block 也打印出来
    print("USED_BLOCKS %s" % compact_ranges(stats["used_blocks"]))


def compact_ranges(blocks):
    if not blocks:
        return "(none)"
    out = []
    start = prev = blocks[0]
    for b in blocks[1:]:
        if b == prev + 1:
            prev = b
            continue
        out.append((start, prev))
        start = prev = b
    out.append((start, prev))
    return ",".join("%d" % a if a == b else "%d-%d" % (a, b) for a, b in out)


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Generate a small, real, read-only ext4 fixture image (pure stdlib).")
    ap.add_argument("--out", required=True, help="output image path")
    ap.add_argument("--blocks", type=int, default=16384,
                    help="total 4 KiB blocks (default 16384 = 64 MiB)")
    ap.add_argument("--verify", dest="verify", action="store_true", default=True,
                    help="self-check the generated image (default: on)")
    ap.add_argument("--no-verify", dest="verify", action="store_false",
                    help="skip self-check")
    ap.add_argument("--verify-only", action="store_true", default=False,
                    help="do not write, only self-check an existing image")
    ap.add_argument("--dump", action="store_true", default=False,
                    help="print dirents and inode i_block hex (ASCII)")
    ap.add_argument("--file", action="append", default=None,
                    help="accepted for command-line compatibility but ignored: "
                         "the fixture always uses the built-in file table "
                         "(/hello.txt, /big.bin, /sub/data.bin)")
    args = ap.parse_args(argv)

    if args.file:
        print("NOTE --file ignored, built-in file table is used")

    if not args.verify_only:
        info = build_image(args.out, args.blocks)
        print("WROTE path=%s blocks=%d free_blocks=%d free_inodes=%d"
              % (args.out, info["blocks"], info["free_blocks"], info["free_inodes"]))

    stats = None
    if args.verify or args.verify_only:
        try:
            stats = verify_image(args.out, args.blocks)
        except CheckError as exc:
            print("SELFCHECK FAIL %s" % exc)
            return 1
        stats["path"] = args.out
        stats["blocks"] = args.blocks
        print_report(stats)
        if args.dump:
            print_dump(stats)
        print("SELFCHECK OK files=%d blocks=%d" % (len(stats["files"]), args.blocks))
    return 0


if __name__ == "__main__":
    sys.exit(main())
