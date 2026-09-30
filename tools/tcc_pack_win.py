#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/tcc_pack_win.py - ★ A4-2b：把 TinyCC 与它的头/库装进 **VimtuFS2 系统卷**

本脚本是 tools/make_shellvol.py 的"扩展版离线造卷器"，只做两件事：
  1) 在 A4-1 那份"带 /bin/shell.bin 的系统卷"基础上，**再加一棵 /tcc 树**（tcc 本体、
     系统头、libtcc1.a、极小 libc/crt）与 /bin/tcc（装载驱动）、/hello（演示产物）；
  2) 写完之后**独立回读校验**：逐文件按 inode 的直接块 / 一级间接 / **二级间接**解析回来，
     与写进去的字节逐字节比对。

为什么要单开一个脚本（不改 make_shellvol.py）：
  * make_shellvol.py 的离线写入器只实现了"直接块 + 一级间接"（**单文件 ≤ 132 块 = 67584 B**），
    而 tcc.bin 是 568 块（282 KB）—— 卷格式（kernel/vfs64.h/.cpp）本来就有二级间接（dind），
    v4 inode 的 dind 在**偏移 71**（参见 kernel/vfs64.cpp:70 的注释），本脚本把这一层补上；
  * 卷的其它内容（/bin/shell.bin、/etc/sh64hello.txt、/tmp）与 A4-1 完全一致，且**复用**
    make_shellvol.py 的 Volume 类（直接 import 它的源码文件，避免格式定义散成两份）。

用法（与 make_shellvol.py 同口径，多两个 --tcc 相关参数）：
    py -3 tools/tcc_pack_win.py --shell build64/shell.bin --bin-tcc build64/tcc \\
          --tcc build64/tcc.bin --stage build64/tcc_stage --hello build64/tcc_demo_hello \\
          --demo-dir user/apps/tcc --vol build64/tccvol.img
    py -3 tools/tcc_pack_win.py ... --system build64/system.img --disk build64/sysdisk.img

退出码：0 = 成功（含回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SECTOR = 512
VFS_DIRECT_BLOCKS = 4
VFS_INDIRECT_PTRS = 128
VFS_L2_FIRST_BLOCK = VFS_DIRECT_BLOCKS + VFS_INDIRECT_PTRS          # 132
VFS_DIND_CHILDREN = 128
VFS_INODE_BYTES = 128
VFS_I_DIND = 71                                                    # v4：二级间接块指针
VFS_I_PARENT = 28
VFS_I_SIZE = 4
S_IFDIR = 0o040000
KIND_DIR = 2
KIND_ELF = 4
S_IFREG = 0o100000
KIND_TEXT = 5


def _load_shellvol():
    """把 tools/make_shellvol.py 当模块加载（复用它的 Volume 类与 build_disk）。"""
    path = os.path.join(HERE, "make_shellvol.py")
    spec = importlib.util.spec_from_file_location("make_shellvol", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


SV = _load_shellvol()


class Volume2(SV.Volume):
    """在 make_shellvol.Volume 上加一层：单文件支持二级间接（dind），上限 = 内核的 8 MiB。"""

    def write_file(self, name, data, parent=0, uid=0, gid=0, mode=0o644, kind=None):
        nblk = (len(data) + SECTOR - 1) // SECTOR
        if nblk <= VFS_L2_FIRST_BLOCK:
            return SV.Volume.write_file(self, name, data, parent, uid, gid, mode, kind)
        if nblk > VFS_L2_FIRST_BLOCK + VFS_DIND_CHILDREN * VFS_INDIRECT_PTRS:
            raise ValueError("文件超过 VimtuFS2 单文件上限（%d 块）" % nblk)

        blocks = []
        for i in range(nblk):
            blk = self._alloc_blk()
            blocks.append(blk)
            chunk = data[i * SECTOR:(i + 1) * SECTOR]
            off = self._blk_off(blk)
            self.buf[off:off + len(chunk)] = chunk

        direct = blocks[:VFS_DIRECT_BLOCKS]
        rest = blocks[VFS_DIRECT_BLOCKS:]
        ind = 0
        if rest:
            ind = self._alloc_blk()
            head = rest[:VFS_INDIRECT_PTRS]
            for i in range(VFS_INDIRECT_PTRS):
                self._wr32(ind, 4 * i, head[i] if i < len(head) else 0)
            rest = rest[VFS_INDIRECT_PTRS:]

        dind = 0
        if rest:
            dind = self._alloc_blk()
            for c in range(VFS_DIND_CHILDREN):
                part = rest[c * VFS_INDIRECT_PTRS:(c + 1) * VFS_INDIRECT_PTRS]
                if not part:
                    self._wr32(dind, 4 * c, 0)
                    continue
                child = self._alloc_blk()
                self._wr32(dind, 4 * c, child)
                for i in range(VFS_INDIRECT_PTRS):
                    self._wr32(child, 4 * i, part[i] if i < len(part) else 0)

        if kind is None:
            kind = KIND_ELF if data[:4] == b"\x7fELF" else KIND_TEXT
        ino = self.next_ino
        self.next_ino += 1
        self._mk_inode(ino, 1, len(data),
                       {"direct": direct, "ind": ind, "dind": dind},
                       parent, uid, gid, S_IFREG | mode, kind, name)
        return ino


# ---------------------------------------------------------------------------
# ★ A4-4：VolumeEdit —— "在一块**已存在**的 VimtuFS2 v4 卷镜像上继续写文件" 的离线编辑器
#
# 为什么需要它：本批的交付是"多个工具各自把自己的文件装进**同一块**系统卷"（tcc 的脚本先造出
# 基础卷 -> lua 的脚本加上 /bin/lua + /lib/lua.bin + /tcc/demo/*.lua -> gzip 的脚本再加上
# /bin/gzip、/bin/gunzip 与 1 MiB 试验文件）—— 谁都不该重写别人那棵树。
# v4 卷的几何（位图/inode 表/数据区起点）由**总扇区数**唯一决定（与 make_shellvol.Volume 完全
# 同一套公式，本类继承 Volume2 就是为了共用），所以这里把装进来的字节覆盖回去、从位图重建
# "已用块"集合、再按空闲 inode 槽继续分配 —— 不需要动格式里的任何一个字段定义。
# ---------------------------------------------------------------------------
class VolumeEdit(Volume2):
    def load(self, img):
        """把一块已存在的卷镜像装进来（几何/超级块原样保留；只重建分配器状态）。"""
        want = self.total * SECTOR
        self.buf[:] = b"\0" * want
        self.buf[:min(len(img), want)] = img[:want]
        self.used = set()
        for m in range(self.bmn):
            off = (self.bitmap_start + m) * SECTOR
            blk = self.buf[off:off + SECTOR]
            for k in range(SV.VFS_BITMAP_BLK_BITS):
                if (blk[k >> 3] >> (k & 7)) & 1:
                    self.used.add(m * SV.VFS_BITMAP_BLK_BITS + k)

    def inode_rec(self, i):
        off = self.inode_start * SECTOR + i * VFS_INODE_BYTES
        return self.buf[off:off + VFS_INODE_BYTES]

    def _free_inode(self):
        for i in range(1, self.inodes):          # inode 0 = 根目录，永不分配
            if self.inode_rec(i)[0] == 0:
                return i
        raise ValueError("inode 用完（%d 个）" % self.inodes)

    def find_dir(self, path):
        """按路径找目录 inode（\"/\" 也认）；找不到返回 None。"""
        cur = 0
        for part in [p for p in path.split("/") if p]:
            hit = None
            for i in range(self.inodes):
                rec = self.inode_rec(i)
                if rec[0] != 2:
                    continue
                if struct.unpack_from("<I", rec, VFS_I_PARENT)[0] != cur:
                    continue
                if rec[40:40 + rec[1]].decode("ascii") == part:
                    hit = i
                    break
            if hit is None:
                return None
            cur = hit
        return cur

    def mkdirs(self, path, mode=0o755):
        """逐级建目录（已存在就复用）；返回最后一级目录的 inode。"""
        cur = self.find_dir("/")
        if cur is None:
            raise ValueError("卷里没有根目录（img 不是 VimtuFS2 卷？）")
        for part in [p for p in path.split("/") if p]:
            nxt = None
            for i in range(self.inodes):
                rec = self.inode_rec(i)
                if rec[0] != 2:
                    continue
                if struct.unpack_from("<I", rec, VFS_I_PARENT)[0] != cur:
                    continue
                if rec[40:40 + rec[1]].decode("ascii") == part:
                    nxt = i
                    break
            if nxt is None:
                ino = self._free_inode()
                self.next_ino = ino
                self.mkdir(part, parent=cur, mode=mode)
                nxt = ino
            cur = nxt
        return cur

    def put(self, path, data, mode=0o644, kind=None):
        """把 data 写到卷里 path（父目录自动建）；返回写入的 inode 号。"""
        parts = [p for p in path.split("/") if p]
        if not parts:
            raise ValueError("put 的路径不能是根：%r" % path)
        d = self.mkdirs("/".join(parts[:-1]))
        # 同名文件先删掉（卷不覆盖重名项）——直接清 inode 记录（块不回收：离线卷一次成型，够用）
        for i in range(self.inodes):
            rec = self.inode_rec(i)
            if rec[0] == 1 and struct.unpack_from("<I", rec, VFS_I_PARENT)[0] == d and \
               rec[40:40 + rec[1]].decode("ascii") == parts[-1]:
                self.buf[self.inode_start * SECTOR + i * VFS_INODE_BYTES:
                         self.inode_start * SECTOR + (i + 1) * VFS_INODE_BYTES] = b"\0" * VFS_INODE_BYTES
        ino = self._free_inode()
        self.next_ino = ino
        self.write_file(parts[-1], data, parent=d, mode=mode, kind=kind)
        return ino


# ---------------------------------------------------------------------------
# 回读自检：**独立**按卷格式把文件读回来（直接块 -> ind -> dind 三级都走一遍）
# ---------------------------------------------------------------------------
def _inode_rec(vol, ino):
    ino_start = struct.unpack_from("<I", vol, 36)[0]
    off = ino_start * SECTOR + ino * VFS_INODE_BYTES
    return vol[off:off + VFS_INODE_BYTES]


def _entries(vol, parent, inodes):
    out = []
    for i in range(inodes):
        rec = _inode_rec(vol, i)
        if rec[0] == 0:
            continue
        if struct.unpack_from("<I", rec, VFS_I_PARENT)[0] != parent:
            continue
        nm = rec[40:40 + rec[1]].decode("ascii")
        out.append((i, nm, rec))
    return out


def _read_file(vol, rec, total):
    size = struct.unpack_from("<I", rec, VFS_I_SIZE)[0]
    blocks = [struct.unpack_from("<I", rec, 8 + 4 * d)[0] for d in range(VFS_DIRECT_BLOCKS)]
    ind = struct.unpack_from("<I", rec, 24)[0]
    if ind:
        for k in range(VFS_INDIRECT_PTRS):
            blocks.append(struct.unpack_from("<I", vol, ind * SECTOR + 4 * k)[0])
    dind = struct.unpack_from("<I", rec, VFS_I_DIND)[0]
    if dind:
        for c in range(VFS_DIND_CHILDREN):
            child = struct.unpack_from("<I", vol, dind * SECTOR + 4 * c)[0]
            if not child:
                continue
            for k in range(VFS_INDIRECT_PTRS):
                blocks.append(struct.unpack_from("<I", vol, child * SECTOR + 4 * k)[0])
    data = bytearray()
    for b in blocks:
        if len(data) >= size:
            break
        if b == 0:
            continue
        if b >= total:
            raise ValueError("块号越界：%d" % b)
        data += vol[b * SECTOR:(b + 1) * SECTOR]
    return bytes(data[:size])


def verify(vol, expect_files):
    """expect_files: {路径: 字节}；逐级走名字，最后逐字节比对。"""
    if vol[0:8] != b"VIMTUFS2":
        return "超级块 magic 不对"
    total = struct.unpack_from("<I", vol, 20)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
    for path, want in expect_files.items():
        parts = [p for p in path.split("/") if p]
        cur = 0
        for k, part in enumerate(parts):
            hit = None
            for i, nm, rec in _entries(vol, cur, inodes):
                if nm == part:
                    hit = (i, rec)
                    break
            if not hit:
                return "卷里找不到 /" + "/".join(parts[:k + 1])
            ino, rec = hit
            if k == len(parts) - 1:
                got = _read_file(vol, rec, total)
                if got != want:
                    return "%s 读回的字节不一致（%d vs %d B）" % (path, len(got), len(want))
            else:
                if rec[0] != 2:
                    return "/%s 不是目录" % "/".join(parts[:k + 1])
                cur = ino
    return None


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--shell", required=True, help="build64/shell.bin（A4-1 的 ring3 shell）")
    ap.add_argument("--bin-tcc", required=True, help="build64/tcc（装载驱动）-> 卷里 /bin/tcc")
    ap.add_argument("--tcc", required=True, help="build64/tcc.bin（tcc 本体）-> 卷里 /lib/tcc.bin")
    ap.add_argument("--stage", required=True, help="build64/tcc_stage（/tcc 那棵树的离线镜像）")
    ap.add_argument("--hello", default=None, help="build64/tcc_demo_hello -> 卷里 /hello（可选）")
    ap.add_argument("--demo-dir", default=os.path.join(ROOT, "user", "apps", "tcc"),
                    help="演示源码目录 -> 卷里 /tcc/demo/（默认 user/apps/tcc）")
    ap.add_argument("--vol", default=None, help="输出卷镜像")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时用）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--vol-sectors", type=int,
                    default=SV.DEFAULT_TARGET_SECTORS - SV.PART_MAIN_LBA)
    ap.add_argument("--target-sectors", type=int, default=SV.DEFAULT_TARGET_SECTORS)
    ap.add_argument("--hello-text", default="VimtuOS A4-1 ring3 shell: /etc/sh64hello.txt byte test\n")
    args = ap.parse_args()

    for p, what in ((args.shell, "shell.bin"), (args.bin_tcc, "/bin/tcc 驱动"),
                    (args.tcc, "tcc.bin"), (args.stage, "tcc_stage")):
        if not os.path.exists(p):
            sys.stderr.write("找不到 %s：%s\n" % (what, p))
            return 2
    shell_bytes = open(args.shell, "rb").read()
    bin_tcc = open(args.bin_tcc, "rb").read()
    tcc_bytes = open(args.tcc, "rb").read()
    for nm, b in (("/bin/shell.bin", shell_bytes), ("/bin/tcc", bin_tcc), ("/lib/tcc.bin", tcc_bytes)):
        if b[:4] != b"\x7fELF":
            sys.stderr.write("%s 不是 ELF：%d B\n" % (nm, len(b)))
            return 2
    if args.disk and not args.system:
        sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
        return 2

    vol_sectors = args.vol_sectors if not args.disk else (args.target_sectors - SV.PART_MAIN_LBA)
    vol = Volume2(vol_sectors)

    # ---- A4-1 原有的那棵树（与 make_shellvol.py 的 build_volume 逐条一致）----
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    etc_ino = vol.mkdir("etc", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    hello_text = args.hello_text.replace("\\n", "\n").encode("utf-8")
    expect = {}
    vol.write_file("shell.bin", shell_bytes, parent=bin_ino, mode=0o755)
    expect["/bin/shell.bin"] = shell_bytes
    vol.write_file("sh64hello.txt", hello_text, parent=etc_ino, mode=0o644)
    expect["/etc/sh64hello.txt"] = hello_text

    # ---- A4-2b：/bin/tcc（装载驱动）+ /lib/tcc.bin（tcc 本体）----
    vol.write_file("tcc", bin_tcc, parent=bin_ino, mode=0o755)
    expect["/bin/tcc"] = bin_tcc
    lib_ino = vol.mkdir("lib", parent=0, mode=0o755)
    vol.write_file("tcc.bin", tcc_bytes, parent=lib_ino, mode=0o755)
    expect["/lib/tcc.bin"] = tcc_bytes

    # ---- A4-2b：/tcc（libtcc1.a + 系统头 + crt/libc）----
    tcc_ino = vol.mkdir("tcc", parent=0, mode=0o755)
    tcc_lib_ino = vol.mkdir("lib", parent=tcc_ino, mode=0o755)
    tcc_inc_ino = vol.mkdir("include", parent=tcc_ino, mode=0o755)
    tcc_demo_ino = vol.mkdir("demo", parent=tcc_ino, mode=0o755)

    for rel in ("libtcc1.a",):
        p = os.path.join(args.stage, rel)
        b = open(p, "rb").read()
        vol.write_file(rel, b, parent=tcc_ino, mode=0o644)
        expect["/tcc/" + rel] = b

    for rel in ("libc.a", "crt1.o", "crti.o", "crtn.o"):
        p = os.path.join(args.stage, "lib", rel)
        if not os.path.exists(p):
            sys.stderr.write("缺 %s\n" % p)
            return 2
        b = open(p, "rb").read()
        vol.write_file(rel, b, parent=tcc_lib_ino, mode=0o644)
        expect["/tcc/lib/" + rel] = b

    inc_root = os.path.join(args.stage, "include")
    inc_map = {}                                         # 目录相对路径 -> 卷内 inode（walk 时填）
    n_inc = 0
    for dirpath, dirnames, filenames in os.walk(inc_root):
        rel = os.path.relpath(dirpath, inc_root)
        cur_ino = tcc_inc_ino if rel == "." else inc_map[rel]
        for d in sorted(dirnames):
            child_rel = d if rel == "." else os.path.join(rel, d)
            inc_map[child_rel] = vol.mkdir(d, parent=cur_ino, mode=0o755)
        for f in sorted(filenames):
            b = open(os.path.join(dirpath, f), "rb").read()
            vol.write_file(f, b, parent=cur_ino, mode=0o644)
            vpath = "/tcc/include/" + (f if rel == "." else rel.replace("\\", "/") + "/" + f)
            expect[vpath] = b
            n_inc += 1

    # ---- A4-2b：/tcc/demo（演示源码）+ /hello（宿主版 tcc 产出的可执行文件，可选）----
    demo_files = ("demo_tiny.c", "hello.c", "demo_headers.c")
    for f in demo_files:
        p = os.path.join(args.demo_dir, f)
        if not os.path.exists(p):
            continue
        b = open(p, "rb").read()
        vol.write_file(f, b, parent=tcc_demo_ino, mode=0o644)
        expect["/tcc/demo/" + f] = b
    if args.hello and os.path.exists(args.hello):
        b = open(args.hello, "rb").read()
        vol.write_file("hello", b, parent=0, mode=0o755)
        expect["/hello"] = b

    vol_bytes = vol.finish()
    bad = verify(vol_bytes, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    tot = sum(len(v) for v in expect.values())
    print("    卷自检 OK（%d 个文件、%d B 逐字节回读一致；含 tcc.bin 的二级间接解析）"
          % (len(expect), tot))
    print("    /bin/shell.bin=%d B  /bin/tcc=%d B  /lib/tcc.bin=%d B  /tcc/include=%d 个头  /tcc/libtcc1.a=%d B"
          % (len(shell_bytes), len(bin_tcc), len(tcc_bytes), n_inc,
             os.path.getsize(os.path.join(args.stage, "libtcc1.a"))))

    if args.vol:
        open(args.vol, "wb").write(vol_bytes)
        print("    卷镜像：%s（%d B）" % (args.vol, len(vol_bytes)))
    if args.disk:
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        img = SV.build_disk(system_bytes, vol_bytes, args.target_sectors)
        open(args.disk, "wb").write(img)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(img), args.target_sectors, SV.PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
