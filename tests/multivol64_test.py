#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/multivol64_test.py - ★ 多卷挂载 / 按盘符切换（v0.2.0-beta.4 的验收脚本）

覆盖（每一项都要打点/像素/输入/字节证据，不能只看"没崩"）：
  A) 安装两阶段（照 fs_tree_test）：安装介质 + 128MB 目标盘 -> 向导装系统 -> 再进向导把主分区
     格式化成 v3 卷（安装后的系统盘就是 C:）。
  B) 数据盘：**宿主侧 Python 造第二个 VimtuFS2 卷**（32MB 盘，@8192，v3），预置
     /docs（目录）、/docs/notes.txt（文件）、/readme.txt（文件）——宿主侧写盘，见 Vfs3Builder。
  C) 多卷端到端（装好的系统盘 + 数据盘）：
     * [DRV64] letter=C:（系统卷 slot=0）/ letter=D:（盘尾 ESP，FAT32 ro=1）/ 数据盘盘符
       **动态定位**（批次 K 起 ESP 也占盘符：数据盘实际是 E:，见下面的 dl）；
       [VFS64] slots ... used=2
     * 终端 `vol <数据盘盘符>` -> [DRV64] activate letter=…: slot=1 ... ok；`ls` 列出
       /docs + /readme.txt；`cat /readme.txt` 读回预置内容；在数据盘上 mkdir + write 成功
       （[FD64] open/write 真路径）
     * 切回 `vol C:` -> C: 的预置文件（/hello.vap）还在、哨兵文件内容正确；数据盘的哨兵/目录在
       C: 上不存在（**双卷独立**）；宿主侧解析两张镜像逐字节确认
     * Explorer（鼠标注入）：双击数据盘卡片 -> [UI] explorer enter letter=…: slot=1 ok items=2 +
       两个条目的打点 + 内容区像素；经"此电脑"面包屑回退 -> 双击 C: 卡片 -> enter letter=C: slot=0 ok
     * ★ 写卷安全：当前卷 = 数据盘时改配置（`store set` + `set startup.health`）触发 config64 的 3 秒
       自动落盘 -> 等 8 秒 -> 断言 [CONF64] autosave ok + [STORE64] flush via=vfs；**宿主侧**：
       C: 的 /store.a 能解析且含新键（内容正确）；数据盘镜像在整个窗口内**逐字节相同**
     * 坏情况：vol Z:/vol A: -> FAILED（不崩）；vol 1 -> usage
  D) 卷表满：装好的盘 + 4 块 AHCI 数据盘（4 个 VimtuFS2 槽 + 1 个 FAT32 ESP = 5 个可浏览卷，
     超过 VFS64_SLOT_MAX=4）-> 第 4 块盘 [DRV64] skip ... reason=voltable-full、没有 H:、
     [DRV64] selftest PASS、`vol H:` 被拒
  E) 全部阶段禁止 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL

用法：py -3 tests\\multivol64_test.py [--qemu 路径] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  find_qemu / q
import fs_tree_test as fst         # noqa: E402  安装/启动夹具 + Python 侧 v3 格式化
import explorer64_test as exp      # noqa: E402  鼠标闭环（aim_click / 打点探针 / 像素工具）

MEDIUM = fst.MEDIUM
SYSTEM_IMG = fst.SYSTEM_IMG
SECTOR = 512
PART_MAIN_LBA = fst.PART_MAIN_LBA
TARGET_SECTORS = fst.TARGET_SECTORS
TARGET_MAIN_SECTORS = fst.TARGET_MAIN_SECTORS

DATA_SECTORS = 65536           # 32MB 数据盘
DATA_PART_LBA = 8192
DATA_PART_SECTORS = DATA_SECTORS - DATA_PART_LBA
DATA_MAIN_BLOCKS = DATA_PART_SECTORS

# 预置内容（宿主侧写盘；验收时按这几段逐字节比对）
README_TEXT = b"VimtuFS2 data volume D: hello from outside\n"
NOTES_TEXT = b"nested file inside /docs\n"
SENTINEL_C = b"c-side-ok"      # 注入文本必须小写：QEMU sendkey 发不出大写字母（见文件尾注）
SENTINEL_D = b"d-side-ok"

EXP_NAV_TOP = 30               # explorer64.cpp 的 EXP_NAV_TOP
EXP_NAV_ROW_H = 22

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "[VFS64] selftest FAIL",
    "[DRV64] selftest FAIL",
    "[EXPL] selftest FAIL",
    "[STORE64] selftest FAIL",
]

# ==================== VimtuFS2 v3：宿主侧读写（验收的真值来源）====================
# 为什么自己写：#1 造数据盘时要预置 /docs 与 readme.txt（系统自己 mkdir/write 也可以，但那样
# "D: 的预置内容"就变成了被测代码写的，不能当独立证据）；#2 断言"C: 的 /store.a 内容正确 /
# D: 逐字节未变"必须绕过被测内核，直接从镜像里读。
def _u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


def vfs3_vol(buf, start_lba):
    sb = buf[start_lba * SECTOR:(start_lba + 1) * SECTOR]
    if bytes(sb[0:8]) != b"VIMTUFS2":
        return None
    if sb[510] != 0x55 or sb[511] != 0xAA:
        return None
    return dict(start=start_lba, ver=_u32(sb, 8), total=_u32(sb, 20), bm=_u32(sb, 28),
                bmn=_u32(sb, 32), ino=_u32(sb, 36), inon=_u32(sb, 40), inosz=_u32(sb, 44),
                data=_u32(sb, 48), crc=_u32(sb, 60))


def vfs3_inode_raw(buf, vol, idx):
    ib = vol["inosz"]
    off = (vol["start"] + vol["ino"]) * SECTOR + idx * ib
    return bytes(buf[off:off + ib])


def vfs3_inode(buf, vol, idx):
    raw = vfs3_inode_raw(buf, vol, idx)
    t = raw[0]
    nl = raw[1]
    noff = 40 if vol["ver"] in (3, 4) else 32          # ★ P4：v4 的 inode 名字偏移与 v3 相同（40）
    return dict(idx=idx, type=t, namelen=nl,
                name=bytes(raw[noff:noff + nl]).decode("ascii", "replace"),
                size=_u32(raw, 4), d0=_u32(raw, 8), d1=_u32(raw, 12), d2=_u32(raw, 16),
                d3=_u32(raw, 20), ind=_u32(raw, 24), parent=_u32(raw, 28))


def vfs3_inode_dind(buf, vol, idx):
    """★ 批次 M：v3 inode 偏移 71 的二级间接块指针（0 = 没有；v2 卷没有这个字段）。"""
    if vol["ver"] not in (3, 4):                       # ★ P4：v4 也把 dind 放在偏移 71
        return 0
    raw = vfs3_inode_raw(buf, vol, idx)
    return _u32(raw, 71) if len(raw) >= 75 else 0


def vfs3_read(buf, vol, idx):
    ino = vfs3_inode(buf, vol, idx)
    size = ino["size"]
    if ino["type"] != 1 or size == 0:
        return b""
    ptrs = [ino["d0"], ino["d1"], ino["d2"], ino["d3"]]
    if ino["ind"]:
        p = (vol["start"] + ino["ind"]) * SECTOR
        ptrs += [_u32(buf, p + 4 * k) for k in range(128)]
    dind = vfs3_inode_dind(buf, vol, idx)          # ★ 二级间接块：128 个子块 x 128 个块号
    if dind:
        p = (vol["start"] + dind) * SECTOR
        for c in range(128):
            cb = _u32(buf, p + 4 * c)
            if cb == 0:
                ptrs += [0] * 128
                continue
            q = (vol["start"] + cb) * SECTOR
            ptrs += [_u32(buf, q + 4 * k) for k in range(128)]
    out = bytearray()
    for k in range((size + SECTOR - 1) // SECTOR):
        blk = ptrs[k]
        out += bytes(buf[(vol["start"] + blk) * SECTOR:(vol["start"] + blk + 1) * SECTOR])
    return bytes(out[:size])


def vfs3_list(buf, vol, parent=0):
    out = []
    for i in range(1, vol["inon"]):
        ino = vfs3_inode(buf, vol, i)
        if ino["type"] == 0 or ino["parent"] != parent or ino["namelen"] == 0:
            continue
        out.append(ino)
    return out


def vfs3_find(buf, vol, name, parent=0):
    for ino in vfs3_list(buf, vol, parent):
        if ino["name"] == name:
            return ino
    return None


class Vfs3Builder:
    """在宿主侧把文件/目录写进一个刚格式化的 v3 卷（与 kernel/vfs64.cpp 的布局逐字段对齐）。"""

    def __init__(self, buf, start_lba, total_sectors):
        info = fst.vimtufs3_format(buf, start_lba, total_sectors)
        self.buf = buf
        self.start = start_lba
        self.bitmap_blocks = info["bitmap_blocks"]
        self.inode_start = 1 + info["bitmap_blocks"]
        self.next_inode = 1
        self.next_block = info["data_start"]
        self.info = info

    def _bm_set(self, blk, used=True):
        m = blk // 4096
        k = blk % 4096
        off = (self.start + 1 + m) * SECTOR + (k >> 3)
        if used:
            self.buf[off] |= (1 << (k & 7))
        else:
            self.buf[off] &= 0xFF ^ (1 << (k & 7))

    def add(self, name, kind, parent=0, data=None):
        idx = self.next_inode
        self.next_inode += 1
        nb = name.encode("ascii")
        ino = bytearray(128)
        ino[0] = 2 if kind == "dir" else 1
        ino[1] = len(nb)
        struct.pack_into("<I", ino, 28, parent)              # parent
        struct.pack_into("<I", ino, 32, fst._now_packed())   # mtime
        struct.pack_into("<H", ino, 36, 1)                   # nlink
        ino[38] = 2 if kind == "dir" else (5 if name.endswith(".txt") else 1)   # kind
        ino[40:40 + len(nb)] = nb
        if data:
            blk = self.next_block
            self.next_block += 1
            self._bm_set(blk, True)
            struct.pack_into("<I", ino, 4, len(data))        # size
            struct.pack_into("<I", ino, 8, blk)              # d0
            p = (self.start + blk) * SECTOR
            self.buf[p:p + SECTOR] = data + b"\0" * (SECTOR - len(data))
        struct.pack_into("<I", ino, 124, zlib.crc32(bytes(ino[:124])) & 0xFFFFFFFF)
        off = (self.start + self.inode_start) * SECTOR + idx * 128
        self.buf[off:off + 128] = ino
        return idx


def make_data_vol_disk(path, sectors=DATA_SECTORS, part_lba=DATA_PART_LBA, with_content=True):
    """/docs + /readme.txt + /docs/notes.txt 的 v3 数据盘（drive 1 / AHCI 盘都用它）。"""
    total = sectors - part_lba
    buf = bytearray(sectors * SECTOR)
    buf[446:462] = fst._mbr_entry(False, 0x07, part_lba, total)
    buf[510], buf[511] = 0x55, 0xAA
    b = Vfs3Builder(buf, part_lba, total)
    if with_content:
        docs = b.add("docs", "dir")
        b.add("readme.txt", "file", 0, README_TEXT)
        b.add("notes.txt", "file", docs, NOTES_TEXT)
    with open(path, "wb") as f:
        f.write(buf)
    return dict(vol=b, info=b.info)


def parse_store_slot(data):
    """宿主侧解析 store64 的 16KB 槽（magic VSTORE64 + 头部/载荷 CRC + KV 记录）。"""
    if data is None or len(data) != 16384 or bytes(data[0:8]) != b"VSTORE64":
        return None
    plen = _u32(data, 24)
    pcrc = _u32(data, 28)
    hcrc = _u32(data, 32)
    count = _u32(data, 36)
    if hcrc != (zlib.crc32(bytes(data[0:32])) & 0xFFFFFFFF):
        return None
    if plen > 16384 - 64 or pcrc != (zlib.crc32(bytes(data[64:64 + plen])) & 0xFFFFFFFF):
        return None
    keys = {}
    off = 64
    while off < 64 + plen:
        kl = data[off]
        vl = data[off + 1]
        off += 2
        if kl == 0 or off + kl + vl > 64 + plen:
            return None
        k = bytes(data[off:off + kl]).decode("ascii", "replace")
        off += kl
        v = bytes(data[off:off + vl]).decode("utf-8", "replace")
        off += vl
        keys[k] = v
    if len(keys) != count:
        return None
    return dict(gen=_u64(data, 16), keys=keys)


# ==================== QEMU ====================
class Vm:
    def __init__(self, qemu, disks, port, serial, name):
        self.serial = serial
        self.port = port
        args = fst.qemu_args(qemu, disks, serial, port, name)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        return fst.slog(self.serial)

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def quit(self):
        try:
            exp.Monitor(self.port).send("quit", wait=1.5)
        except Exception:
            pass
        for _ in range(20):
            if self.proc.poll() is not None:
                return
            time.sleep(0.5)
        fst.kill(self.proc)

    def close(self):
        fst.kill(self.proc)


def qemu_args_ahci(qemu, system_disk, data_disks, serial, port, name):
    """系统盘 index=0（PATA/drive 0）+ N 块数据盘挂在 ich9-ahci 端口（drive 8..）。"""
    args = [qemu, "-name", name,
            "-drive", "format=raw,file=%s,index=0,media=disk" % p64.q(system_disk),
            "-device", "ich9-ahci,id=ahci"]
    for i, d in enumerate(data_disks):
        args += ["-drive", "file=%s,if=none,id=dat%d,format=raw" % (p64.q(d), i),
                 "-device", "ide-hd,drive=dat%d,bus=ahci.%d" % (i, i)]
    args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
             "-serial", "file:%s" % p64.q(serial), "-no-reboot"]
    if port:
        args += ["-monitor", "telnet:127.0.0.1:%d,server,nowait" % port]
    return args


class VmAhci(Vm):
    def __init__(self, qemu, system_disk, data_disks, port, serial, name):
        self.serial = serial
        self.port = port
        args = qemu_args_ahci(qemu, system_disk, data_disks, serial, port, name)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def click_nav_thispc(vm, mon):
    """点导航窗格第一项（此电脑）—— 单卷内回退，不走历史栈。"""
    exp.aim_single_click(vm, mon, exp.sx(20), exp.sy(exp.CONTENT_Y + EXP_NAV_TOP + EXP_NAV_ROW_H // 2), "nav:0")


def wait_count(vm, pattern, want, timeout):
    """等某条串口打点出现 >= want 次（QEMU 写 -serial file: 有 1~3 秒落盘延迟）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        if len(re.findall(pattern, vm.log())) >= want:
            return True
        time.sleep(0.4)
    return False


def explorer_dclick_card(vm, mon, letter, needle, tries=3):
    """双击盘符卡片 -> 等 enter 打点。鼠标包偶尔会丢（并发跑测试时更明显），所以重试几次，
    并在每次失败后用开始菜单把资源管理器重新置顶。"""
    for _ in range(tries):
        idx = card_idx_of(vm.log(), letter)
        if idx < 0:
            time.sleep(1.0)
            continue
        before = vm.log().count(needle)
        exp.aim_click(vm, mon, exp.card_center(idx)[0], exp.card_center(idx)[1], "card:%d" % idx)
        for _ in range(24):
            if vm.log().count(needle) > before:
                return True
            time.sleep(0.5)
        try:
            exp.ensure_window_on_top(vm, mon)
        except Exception:
            pass
    return False


def back_to_thispc(vm, mon, tries=3):
    """回"此电脑"：先点导航窗格第一项，不行再点面包屑第 0 段（此电脑）。"""
    for _ in range(tries):
        before = vm.log().count("[UI] explorer thispc drives=")
        click_nav_thispc(vm, mon)
        for _ in range(16):
            if vm.log().count("[UI] explorer thispc drives=") > before:
                return True
            time.sleep(0.5)
        # 面包屑第 0 段（此电脑）在地址栏 x=96.. 起，y = 工具栏+地址栏上半
        exp.aim_single_click(vm, mon, exp.sx(110), exp.sy(exp.TOOLBAR_H + exp.ADDR_H // 2), "crumb:0")
        for _ in range(16):
            if vm.log().count("[UI] explorer thispc drives=") > before:
                return True
            time.sleep(0.5)
    return False


def card_idx_of(log, letter):
    """从 [UI] explorer card idx=<i> letter=<L>: 里取该盘符卡片的格子号（不硬编码排序）。"""
    m = None
    for mm in re.finditer(r"\[UI\] explorer card idx=(\d+) letter=(.)", log):
        if mm.group(2) == letter:
            m = mm
    return int(m.group(1)) if m else -1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (MEDIUM, SYSTEM_IMG):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_multivol_")
    target = os.path.join(tmp, "target.img")
    data1 = os.path.join(tmp, "data1.img")
    data_extra = [os.path.join(tmp, "datax%d.img" % i) for i in range(4)]
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbid(tag, log):
        for bad in FORBIDDEN:
            check("%s：不得出现 %s" % (tag, bad), bad not in log)

    # ==================== 阶段 1：安装到 128MB 目标盘 ====================
    print("=== 阶段 1：安装介质 + 128MB 目标盘（向导：回车×4 -> n 新建 -> 回车安装）===")
    fst.wipe(target, TARGET_SECTORS)
    s1 = os.path.join(tmp, "boot1_install.log")
    p1 = fst.free_port()
    proc, mon = fst.boot_media(qemu, target, s1, p1, "Vimtu64-multivol-install")
    try:
        check("向导就绪（磁盘枚举完成）", fst.wait_wizard_ready(proc, s1))
        for _ in range(4):
            mon.key("ret", wait=1.6)
        mon.key("n", wait=2.2)
        mon.key("ret", wait=2.2)
        fst.wait_for(s1, "[INSTALL] 完成：已写", 90, proc)
        time.sleep(6)
        log1 = fst.slog(s1)
    finally:
        fst.kill(proc)
    check("安装完成（[INSTALL] 完成：已写）", "[INSTALL] 完成：已写" in log1)
    forbid("阶段1", log1)

    # ==================== 阶段 2：把主分区格式化成 v3（装好的系统才有 C: 卷）====================
    print("=== 阶段 2：同盘再进向导，光标下移到 P2 按 f 格式化（产出 v3）===")
    s2 = os.path.join(tmp, "boot2_format.log")
    p2 = fst.free_port()
    proc, mon = fst.boot_media(qemu, target, s2, p2, "Vimtu64-multivol-format")
    try:
        fst.wait_wizard_ready(proc, s2)
        for _ in range(4):
            mon.key("ret", wait=1.6)
        mon.key("down", wait=1.6)
        mon.key("f", wait=2.8)
        fst.wait_for(s2, "[VFS64] format ok", 40, proc)
        time.sleep(1.5)
        log2 = fst.slog(s2)
    finally:
        fst.kill(proc)
    check("格式化主分区 P2（[PART] 格式化 OK drive=1 index=2 start=8009）",
          "[PART] 格式化 OK drive=1 index=2 start=8009" in log2)
    mf = fst.last_match(r"\[VFS64\] format ok blocks=(\d+) version=(\d+) inode=(\d+) root=(\d+)", log2)
    check("格式化产出 v4（version=4 inode=128；★ P4 起新格式化 = v4）",
          bool(mf) and mf.group(2) == "4" and mf.group(3) == "128",
          mf.group(0) if mf else "（缺 [VFS64] format ok 行）")
    check("格式化块数 = 主分区扇区数（%d）" % TARGET_MAIN_SECTORS,
          bool(mf) and mf.group(1) == str(TARGET_MAIN_SECTORS),
          "blocks=%s" % (mf.group(1) if mf else "?"))
    check("自检全过（[VFS64] selftest PASS）", "[VFS64] selftest PASS" in log2)
    forbid("阶段2", log2)

    # ==================== 阶段 3：装好的系统盘 + 数据盘（多卷主战场）====================
    print("=== 阶段 3：装好的盘（drive 0）+ 32MB 数据盘（drive 1，预置 /docs 与 readme.txt）===")
    d1 = make_data_vol_disk(data1)
    # 预置内容的宿主侧真值（验收时再解析一次镜像比对）
    dbuf0 = read_file(data1)
    dvol0 = vfs3_vol(dbuf0, DATA_PART_LBA)
    check("数据盘是合法 v3 卷（宿主侧解析）", dvol0 is not None and dvol0["ver"] == 3)
    r0 = vfs3_find(dbuf0, dvol0, "readme.txt")
    docs0 = vfs3_find(dbuf0, dvol0, "docs")
    check("预置 /docs 目录 + /readme.txt（宿主侧）",
          docs0 is not None and docs0["type"] == 2 and r0 is not None and r0["type"] == 1)
    if r0 is not None:
        check("预置 readme.txt 内容 = 常量（宿主侧）", vfs3_read(dbuf0, dvol0, r0["idx"]) == README_TEXT)
    if docs0 is not None:
        n0 = vfs3_find(dbuf0, dvol0, "notes.txt", docs0["idx"])
        check("预置 /docs/notes.txt 内容 = 常量（宿主侧）",
              n0 is not None and vfs3_read(dbuf0, dvol0, n0["idx"]) == NOTES_TEXT)

    s3 = os.path.join(tmp, "boot3_multivol.log")
    p3 = fst.free_port()
    vm = Vm(qemu, [target, data1], p3, s3, "Vimtu64-multivol-os")
    mon = exp.Monitor(p3)          # 该 Monitor 有 shot()（screendump -> PPM）
    try:
        check("装好的系统进入桌面（[GUI64] ready）", fst.wait_desktop(vm.proc, s3, 120))
        log3 = vm.log()
        # ---- 启动期：卷槽表 + 盘符 ----
        check("系统卷挂进 0 号槽（[VFS64] mount_system slot=0）", "[VFS64] mount_system slot=0" in log3)
        check("卷槽表：used=2（系统卷 + 数据卷）",
              re.search(r"\[VFS64\] slots n=4 used=2 system=0 current=0", log3) is not None,
              (re.search(r"\[VFS64\] slots[^\r\n]*", log3).group(0) if re.search(r"\[VFS64\] slots", log3) else "缺"))
        check("C: 有盘符且 slot=0",
              re.search(r"\[DRV64\] letter=C: disk=0 part=\d+ fs=VimtuFS2 total_kb=\d+ free_kb=\d+ slot=0", log3) is not None)
        # ★ 批次 K 后的事实：盘尾 ESP（FAT32）也占盘符，所以数据盘不再是固定的 D:。
        #   按 fs_tree_test.py 的做法**动态定位**：抓 disk=1 part=1 slot=1 的实际盘符。
        dm = re.search(r"\[DRV64\] letter=(\w): disk=1 part=1 fs=VimtuFS2 total_kb=(\d+) free_kb=(\d+) slot=1", log3)
        dl = dm.group(1) if dm else "D"        # 数据盘实际盘符（C=系统卷、D=ESP、数据盘顺延）
        check("%s: 有盘符且 slot=1（数据盘真被挂成第二个卷）" % dl, dm is not None,
              dm.group(0) if dm else "缺 [DRV64] letter=? disk=1 ... slot=1")
        if dm:
            check("%s: 容量 = 数据卷总块/2（%d KB）" % (dl, DATA_MAIN_BLOCKS // 2),
                  int(dm.group(2)) == DATA_MAIN_BLOCKS // 2, "total_kb=%s" % dm.group(2))
            exp_free = (dvol0["total"] - dvol0["data"]) - 2 if dvol0 else -1
            check("%s: 可用 = 数据区块数 - 预置占用的 2 块（%d KB）" % (dl, exp_free // 2 if exp_free >= 0 else -1),
                  exp_free >= 0 and int(dm.group(3)) == exp_free // 2, "free_kb=%s" % dm.group(3))
        check("[DRV64] selftest PASS（多卷槽一致性 bit6 也过了）", "[DRV64] selftest PASS" in log3)
        check("[VFS64] multivol selftest ok（卷槽/切换/on64 守卫/表满拒绝）",
              "[VFS64] multivol selftest ok" in log3)
        check("自检全过（[VFS64] selftest PASS）", "[VFS64] selftest PASS" in log3)

        # ---- 终端：vol 列表 / 切换 / 列目录 / 读 / 写 ----
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, s3, vm.proc))
        mon.type_line("vol")
        check("vol 列表：三个可浏览卷（系统卷 + ESP + 数据卷）+ 当前是 C:",
              vm.wait_log("[VOL] list n=3 current=C:", 20, since=0))
        mon.type_line("vol %s" % dl.lower())
        check("vol %s 切卷成功（[DRV64] activate letter=%s: slot=1 ... ok）" % (dl.lower(), dl),
              vm.wait_log("[DRV64] activate letter=%s: slot=1 disk=1 lba=8192 ok" % dl, 20) and
              vm.wait_log("[VOL] switch letter=%s: slot=1" % dl, 20))
        check("切到 %s: 后 vfs64 当前卷 = slot 1（[VFS64] activate slot=1 ...）" % dl,
              re.search(r"\[VFS64\] activate slot=1 drive=1 start=\d+ blocks=\d+ version=3", vm.log()) is not None)
        mon.type_line("ls")
        check("ls 列出 %s: 的 /docs 与 readme.txt（[TERM] cmd ls entries=2）" % dl,
              vm.wait_log("[TERM] cmd ls entries=2", 25))
        mon.type_line("cat /readme.txt")
        check("cat /readme.txt 从 %s: 读回（[TERM] cmd cat bytes=%d）" % (dl, len(README_TEXT)),
              vm.wait_log("[TERM] cmd cat bytes=%d" % len(README_TEXT), 25))
        mon.type_line("cat /docs/notes.txt")
        check("cat /docs/notes.txt 多级路径也在 %s: 上（bytes=%d）" % (dl, len(NOTES_TEXT)),
              vm.wait_log("[TERM] cmd cat bytes=%d" % len(NOTES_TEXT), 25))
        mon.type_line("df")
        check("df 列出所有卷（[TERM] cmd df volumes=3 current=%s:）" % dl,
              vm.wait_log("[TERM] cmd df volumes=3 current=%s:" % dl, 25))

        # ---- 双卷独立：切回 C: 写哨兵 -> 再切数据卷看它不在 ----
        mon.type_line("vol c")
        check("切回 C: 成功", vm.wait_log("[VOL] switch letter=C: slot=0", 20))
        # ★ P4：C: 是新格式化的 **v4** 卷（/ 属于 root、0755）-> 普通用户不能在 / 下写；切到 root 再写
        n_su = vm.log().count("[USER64] su ok")
        mon.type_line("su - root")
        check("su - root（P4：C: 根目录属于 root，写哨兵要 root 会话）",
              vm.wait_log("[USER64] su ok", 20) is not None and vm.log().count("[USER64] su ok") > n_su)
        before_w9 = len(re.findall(r"\[FD64\] write fd=\d+ n=9 total=9", vm.log()))
        mon.type_line("write /csentinel.txt c-side-ok")
        check("在 C: 上写哨兵文件（FD 层 n=9）",
              wait_count(vm, r"\[FD64\] write fd=\d+ n=9 total=9", before_w9 + 1, 25))
        mon.type_line("cat /csentinel.txt")
        check("C: 哨兵读回 9 字节", vm.wait_log("[TERM] cmd cat bytes=9", 25))
        mon.type_line("vol %s" % dl.lower())
        check("再切到 %s:（current=%s:）" % (dl, dl), vm.wait_log("[VOL] switch letter=%s: slot=1" % dl, 20))
        before_f = len(re.findall(r"\[FD64\] open FAILED path=/csentinel\.txt", vm.log()))
        mon.type_line("cat /csentinel.txt")
        check("%s: 上看不到 C: 的哨兵（[FD64] open FAILED path=/csentinel.txt rc=2）" % dl,
              wait_count(vm, r"\[FD64\] open FAILED path=/csentinel\.txt rc=2", before_f + 1, 25))
        mon.type_line("vol c")
        check("再切回 C:（双向切换）", vm.wait_log("[VOL] switch letter=C: slot=0", 20))
        before_f = len(re.findall(r"\[FD64\] open FAILED path=/readme\.txt", vm.log()))
        mon.type_line("cat /readme.txt")
        check("C: 上没有数据卷的 readme.txt（双卷独立：[FD64] open FAILED path=/readme.txt）",
              wait_count(vm, r"\[FD64\] open FAILED path=/readme\.txt rc=2", before_f + 1, 25))
        mon.type_line("vol")

        # ---- Explorer 端到端（鼠标注入）----
        exp.SAFE_POINT = (exp.sx(exp.CONTENT_X + exp.CONTENT_W - 40),
                          exp.sy(exp.CONTENT_Y + exp.CONTENT_H - 40))
        # 先切回 C:，证明"Explorer 双击数据卷卡片"自己就能切卷（而不是靠终端）
        mon.type_line("vol c")
        time.sleep(1.0)
        mon.key("meta_l", wait=0.9)
        mon.key("2", wait=2.5)                      # 开始菜单第 2 项 = 文件资源管理器
        check("文件管理器打开（[APP] mypc opened）", vm.wait_log("[APP] mypc opened", 25))
        logx = vm.log()
        di = card_idx_of(logx, dl)
        check("此电脑页有 %s: 卡片（[UI] explorer card ... letter=%s:）" % (dl, dl), di >= 0, "card idx=%d" % di)
        if di >= 0:
            entered = explorer_dclick_card(vm, mon, dl, "[UI] explorer enter letter=%s:" % dl)
            check("双击 %s: 卡片 -> 进入数据卷（enter letter=%s: slot=1 ok items=2）" % (dl, dl),
                  entered and vm.wait_log("[UI] explorer enter letter=%s: slot=1 ok items=2" % dl, 25))
            logx2 = vm.log()
            check("内容区列出 /docs（[UI] explorer item idx=0 name=docs type=dir）",
                  re.search(r"\[UI\] explorer item idx=0 name=docs type=dir", logx2) is not None)
            check("内容区列出 readme.txt（idx=1 type=file size=%d）" % len(README_TEXT),
                  re.search(r"\[UI\] explorer item idx=1 name=readme.txt type=file size=%d" % len(README_TEXT),
                            logx2) is not None)
            check("导航打点 [UI] explorer nav path=/ items=2", " items=2" in logx2 and
                  re.search(r"\[UI\] explorer nav path=/ items=2 view=(icons|details)", logx2) is not None)
            # 像素：数据卷根目录的前两个图标单元格里必须有非白像素（图标 + 名字文字）
            shot = os.path.join(tmp, "explorer_ddrive64.ppm")
            if mon.shot(shot):
                wpx, hpx, px = exp.read_ppm(shot)
                n0 = exp.rect_dark(px, wpx, exp.sx(exp.CONTENT_X + 4), exp.sy(exp.CONTENT_Y + 4),
                                   exp.sx(exp.CONTENT_X + exp.ICON_CELL_W - 4),
                                   exp.sy(exp.CONTENT_Y + exp.ICON_CELL_H - 4), 200)
                n1 = exp.rect_dark(px, wpx, exp.sx(exp.CONTENT_X + exp.ICON_CELL_W + 4),
                                   exp.sy(exp.CONTENT_Y + 4),
                                   exp.sx(exp.CONTENT_X + 2 * exp.ICON_CELL_W - 4),
                                   exp.sy(exp.CONTENT_Y + exp.ICON_CELL_H - 4), 200)
                check("像素：%s: 根目录前两个图标格（同一行两列）有内容（深色像素 > 80）" % dl, n0 > 80 and n1 > 80,
                      "cell0=%d cell1=%d" % (n0, n1))
            else:
                check("像素：截屏成功（screendump）", False, "screendump 超时")
            # 回退到"此电脑"（面包屑第 0 段）再双击 C: 卡片
            if back_to_thispc(vm, mon):
                ci = card_idx_of(vm.log(), "C")
                check("此电脑页有 C: 卡片", ci >= 0, "card idx=%d" % ci)
                if ci >= 0:
                    explorer_dclick_card(vm, mon, "C", "[UI] explorer enter letter=C:")
                check("双击 C: 卡片 -> 回到系统卷（enter letter=C: slot=0 ok）",
                      vm.wait_log("[UI] explorer enter letter=C: slot=0 ok", 25))
                check("C: 根目录能看到 /hello.vap（[UI] explorer item ... name=hello.vap）",
                      re.search(r"\[UI\] explorer item idx=\d+ name=hello\.vap type=file", vm.log()) is not None)
            else:
                check("回退到「此电脑」（导航窗格第一项）", False)

        # ---- 坏情况：不存在的盘符 ----
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=2.0)
        mon.type_line("vol z")
        check("vol z 如实失败（[DRV64] activate letter=Z: FAILED reason=no-letter）",
              vm.wait_log("[DRV64] activate letter=Z: FAILED reason=no-letter", 20) and
              vm.wait_log("[VOL] switch FAILED letter=Z:", 20))
        mon.type_line("vol a")
        check("vol a 如实失败（A:/B: 从不分配）",
              vm.wait_log("[DRV64] activate letter=A: FAILED reason=no-letter", 20))
        mon.type_line("vol 1")
        check("vol 1 参数非法 -> usage（[VOL] usage bad-arg，不崩）",
              vm.wait_log("[VOL] usage bad-arg", 20) and vm.wait_log("[TERM] cmd vol fail", 20))

        # ---- 在数据卷上真写文件（放在 explorer 之后：这样 explorer 看到的是预置的 2 条目）----
        mon.type_line("vol %s" % dl.lower())
        check("切到 %s: 准备写盘" % dl, vm.wait_log("[VOL] switch letter=%s: slot=1" % dl, 20))
        mon.type_line("mkdir /dtest")
        check("在 %s: 上 mkdir 成功（[TERM] cmd mkdir ok）" % dl, vm.wait_log("[TERM] cmd mkdir ok", 20))
        before_w9 = len(re.findall(r"\[FD64\] write fd=\d+ n=9 total=9", vm.log()))
        mon.type_line("write /dtest/a.txt d-side-ok")
        check("在 %s: 上 write 成功（FD 层真路径：[FD64] write n=9 total=9 + [TERM] cmd write ok）" % dl,
              wait_count(vm, r"\[FD64\] write fd=\d+ n=9 total=9", before_w9 + 1, 25) and
              vm.wait_log("[TERM] cmd write ok", 20))
        mon.type_line("cat /dtest/a.txt")
        check("%s: 上写入的文件能读回 9 字节" % dl, vm.wait_log("[TERM] cmd cat bytes=9", 25))
        before_ls3 = len(re.findall(r"\[TERM\] cmd ls entries=3", vm.log()))
        mon.type_line("ls")
        check("ls 现在列出 3 个条目（docs + readme.txt + dtest：guest 写盘真的进了这卷的目录）",
              wait_count(vm, r"\[TERM\] cmd ls entries=3", before_ls3 + 1, 25))

        # ---- ★ 写卷安全：当前卷 = 数据卷时自动落盘，只能写 C: ----
        mon.type_line("vol %s" % dl.lower())
        check("写卷安全测试前：当前卷 = %s:" % dl, vm.wait_log("[VOL] switch letter=%s: slot=1" % dl, 20))
        mon.type_line("store set mvprobe d-browse")
        mon.type_line("set startup.health 1")
        check("已改配置（触发 3 秒自动落盘）：[CONF64] set key=startup.health",
              vm.wait_log("[CONF64] set key=startup.health", 20))
        # 基线：等数据卷镜像在宿主侧稳定（两次读一致），再等 8 秒看它有没有被动过
        base = None
        for _ in range(6):
            a = read_file(data1)
            time.sleep(1.0)
            b = read_file(data1)
            if a == b:
                base = a
                break
        check("%s: 镜像基线可稳定读取（两读一致）" % dl, base is not None)
        time.sleep(8.0)                              # >= 5 秒：config64 的 3 秒自动落盘一定跑过
        after = read_file(data1)
        autosave = vm.wait_log("[CONF64] autosave ok keys=", 20)
        check("浏览 %s: 期间 config64 自动落盘真的跑了（[CONF64] autosave ok keys=）" % dl, autosave)
        # autosave 出现的位置必须在"切到数据卷"之后，且中间没有切回 C: 的激活行
        l3 = vm.log()
        off_d = l3.rfind("[VOL] switch letter=%s: slot=1" % dl)
        off_auto = l3.rfind("[CONF64] autosave ok keys=")
        between = l3[off_d:off_auto] if (off_d >= 0 and off_auto > off_d) else ""
        check("★ 自动落盘发生时当前卷仍是 %s:（期间没有 [VFS64] activate slot=0）" % dl,
              off_d >= 0 and off_auto > off_d and "[VFS64] activate slot=0" not in between)
        check("落盘走 VFS 载体（[STORE64] flush via=vfs -> slot=）",
              re.search(r"\[STORE64\] flush via=vfs -> slot=[AB]", vm.log()) is not None)
        check("★ 数据卷在自动落盘窗口内**逐字节未变**（size + bytes）",
              base is not None and len(after) == len(base) and after == base,
              "size %s -> %s" % (len(base) if base is not None else "?", len(after)))
        # C: 的 /store.a 内容正确（宿主侧解析槽 + 新键）
        cbuf = read_file(target)
        cvol = vfs3_vol(cbuf, PART_MAIN_LBA)
        check("C: 是合法 v3/v4 卷（宿主侧；★ P4 起安装向导格式化产出 v4）", cvol is not None and cvol["ver"] in (3, 4))
        if cvol:
            fa = vfs3_find(cbuf, cvol, "store.a")
            fb = vfs3_find(cbuf, cvol, "store.b")
            sa = parse_store_slot(vfs3_read(cbuf, cvol, fa["idx"])) if fa else None
            sb = parse_store_slot(vfs3_read(cbuf, cvol, fb["idx"])) if fb else None
            check("C: 的 /store.a 是有效槽（magic+CRC+记录数）或 /store.b 是（活动槽可能已切换）",
                  (sa is not None) or (sb is not None),
                  "a=%s b=%s" % ("ok" if sa else "bad", "ok" if sb else "bad"))
            keys = {}
            if sa:
                keys.update(sa["keys"])
            if sb:
                keys.update(sb["keys"])
            check("★ C: 的 store 里有本次自动落盘写下的键（cfg.startup.health=1）",
                  keys.get("cfg.startup.health") == "1", "keys=%d" % len(keys))
            check("★ store set 的 mvprobe=d-browse 也落在 C: 上（不是数据卷）",
                  keys.get("mvprobe") == "d-browse", "mvprobe=%r" % keys.get("mvprobe"))
        # 数据卷上不能出现 store 文件（写错卷的直接证据）
        d1buf = read_file(data1)
        d1vol = vfs3_vol(d1buf, DATA_PART_LBA)
        dnames = sorted(x["name"] for x in vfs3_list(d1buf, d1vol)) if d1vol else []
        check("★ 数据卷上没有 /store.a、/store.b（落盘没写错卷）",
              "store.a" not in dnames and "store.b" not in dnames, "names=%s" % dnames)
        # 数据卷上我们写的 /dtest/a.txt 内容正确（宿主侧）
        dt = vfs3_find(d1buf, d1vol, "dtest")
        if dt is not None:
            at = vfs3_find(d1buf, d1vol, "a.txt", dt["idx"])
            check("宿主侧：数据卷的 /dtest/a.txt 内容 = D-SIDE-OK（guest 写盘真的落到数据盘）",
                  at is not None and vfs3_read(d1buf, d1vol, at["idx"]) == SENTINEL_D)
        else:
            check("宿主侧：数据卷有 /dtest 目录", False, "names=%s" % dnames)
        # C: 的哨兵内容正确（宿主侧）
        sc = vfs3_find(cbuf, cvol, "csentinel.txt") if cvol else None
        check("宿主侧：C: 的 /csentinel.txt 内容 = C-SIDE-OK",
              sc is not None and vfs3_read(cbuf, cvol, sc["idx"]) == SENTINEL_C)
        check("宿主侧：C: 上没有 readme.txt（双卷独立）",
              cvol is not None and vfs3_find(cbuf, cvol, "readme.txt") is None)
        # 收尾前把完整日志留档
        log3 = vm.log()
        forbid("阶段3", log3)
    finally:
        vm.quit()

    # ==================== 阶段 4：卷表满（系统盘 + 4 块 AHCI 数据盘）====================
    print("=== 阶段 4：装好的盘 + 4 块 AHCI 数据盘（5 个可浏览卷 > VFS64_SLOT_MAX=4）===")
    for i, pth in enumerate(data_extra):
        make_data_vol_disk(pth, with_content=(i == 0))
    s4 = os.path.join(tmp, "boot4_volfull.log")
    p4 = fst.free_port()
    vm = VmAhci(qemu, target, data_extra, p4, s4, "Vimtu64-multivol-volfull")
    mon = exp.Monitor(p4)
    try:
        check("装好的系统进入桌面（卷表满会话）", fst.wait_desktop(vm.proc, s4, 120))
        log4 = vm.log()
        check("卷槽表 4 个槽全满（used=4 system=0 current=0）",
              re.search(r"\[VFS64\] slots n=4 used=4 system=0 current=0", log4) is not None)
        # ★ 批次 K 后的事实：盘尾 ESP（FAT32）也占一个盘符，所以三个数据卷是 E:/F:/G:。
        check("C:/D:/E:/F:/G: 五个盘符都在（系统卷 + ESP + 三个数据卷）",
              all(re.search(r"\[DRV64\] letter=%s:" % L, log4) is not None for L in "CDEFG"),
              "letters=" + ",".join(L for L in "CDEFGH" if re.search(r"\[DRV64\] letter=%s:" % L, log4)))
        check("H: 没有分到盘符（卷表满如实拒绝）", re.search(r"\[DRV64\] letter=H:", log4) is None)
        check("第 4 块数据盘被拒并打点（skip ... type=VimtuFS2 reason=voltable-full）",
              re.search(r"\[DRV64\] skip lba=\d+ type=VimtuFS2 reason=voltable-full", log4) is not None,
              (re.search(r"\[DRV64\] skip[^\r\n]*voltable-full[^\r\n]*", log4).group(0)
               if re.search(r"voltable-full", log4) else "缺"))
        check("[DRV64] selftest PASS（槽一致 + 表满也不算失败）", "[DRV64] selftest PASS" in log4)
        check("E:/F:/G: 三个数据卷各占一个槽（slot=1/2/3，盘符由启动扫描分配）",
              all(re.search(r"\[DRV64\] letter=%s: disk=\d+ part=\d+ fs=VimtuFS2 total_kb=\d+ free_kb=\d+ slot=%d" % (L, i), log4) is not None
                  for i, L in enumerate("EFG", 1)))
        check("打开终端", fst.open_terminal(mon, s4, vm.proc))
        mon.type_line("vol")
        check("vol 列表：5 个可浏览卷（[VOL] list n=5）", vm.wait_log("[VOL] list n=5", 25))
        mon.type_line("vol h")
        check("vol h 被拒（no-letter：第 4 块数据盘没进卷表）",
              vm.wait_log("[DRV64] activate letter=H: FAILED reason=no-letter", 20))
        mon.type_line("vol g")
        check("vol g 可以切（第 3 个数据卷）", vm.wait_log("[VOL] switch letter=G: slot=3", 20))
        mon.type_line("ls")
        check("G: 上 ls 正常（不给盘符的空卷不在表里，但已挂载的卷可用）",
              vm.wait_log("[TERM] cmd ls entries=", 25))
        log4 = vm.log()
        forbid("阶段4", log4)
    finally:
        vm.quit()

    if args.keep:
        print("[multivol] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


# 备注（踩过的坑）：QEMU monitor 的 `sendkey` 只认小写字母键名（"a".."z"），`sendkey D` 不是
# 合法键名 —— 它一个键都不发（大写要用 "shift-a"）。本脚本注入的文本因此全部小写：
# `vol e` / `store set mvprobe d-browse` / 文件内容 "d-side-ok"；盘符的大小写由内核 cmd_vol 负责。
if __name__ == "__main__":
    sys.exit(main())
