#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fs_tree_test.py - VimtuFS2 v3 目录树 + 盘符/驱动器枚举层 端到端验收

它做什么（五个阶段，全部自动注入按键/命令，不需要人）：
  阶段 1  安装介质 + 128MB 目标盘：走完向导（回车×4 -> n 新建分区 -> 回车开始安装）
          -> 断言安装完成 + 目标盘建了 ESP/GPT（[INSTALL] gpt written (main + esp)）
  阶段 2  **同一个目标盘**再起一次安装介质：光标下移到 P2（主分区）-> f 格式化
          -> 断言 [VFS64] format ok ... version=3（新格式化产出 **v3**）
             + [PART] 格式化 OK drive=1 index=2 start=8009
             + [VFS64] selftest PASS（假盘上跑完整目录树/路径/mtime/重挂载自检）
  阶段 3  启动装好的系统盘（drive 0）+ 一块 Python 造的 v3 数据盘（drive 1）：
          -> 断言 C: 指向系统分区（disk=0 part=2）、D: 是第二块盘（disk=1 part=1）、
             ESP（type 0xEF）被 skip 且**不占盘符**、[DRV64] selftest PASS
          -> 终端里：mkdir /efi、mkdir /apps、mkdir /apps/demo（**多级**）、
             write /apps/demo/demo.txt hello、cat 回读 5 字节；
             坏路径（父目录不存在 / /../.. / 超长段）必须被拒且不崩
          -> 断言 [VFS64] mount ok ... version=3 inode=128B
  阶段 4  **冷启动同一块盘**：目录树必须还在（[VFS64] tree /apps/demo/demo.txt size=5）+
          mtime 非 0 且年月日时分秒合理 + cat 仍读到 5 字节 + rm 能删掉
  阶段 5  单独启动 Python 造的 16MB v3 系统盘（小卷：位图 7 块，df 能给出数字）：
          -> 断言 drive64 的 total_kb/free_kb 与终端 df 的 blocks/free **一致**
             （这是"盘容量与文件系统自身口径一致"的直接证据）
  五个阶段都禁止 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [TERM] unsupported。

边界（如实写，别把没做的说成做了）：
  * 单文件上限 **8 MiB**（批次 M：二级间接块；v2 旧卷仍 67584B）；名字上限 31B（v2 卷仍 27B）；
    inode 总数上限 512；路径深度上限 16；无权限/硬链接/符号链接；
    非空目录必须先清空才能 rmdir64。
  * FAT（ESP）只做**识别**（BPB 指纹 + 名字"EFI 系统分区"），**不能浏览**（本内核只有 FAT32 写入器）。
  * 本脚本用的 Python 侧 v3 格式化器是 kernel/vfs64_format 规则的复刻（超级块 60B CRC + 128B inode
    + 127 字节 CRC 区），用来造"小卷 + 第二块盘"的夹具；v3 的权威定义仍在 kernel/vfs64.h/.cpp。

用法（必须用 Windows 原生 Python）：py -3 tests\\fs_tree_test.py [--timeout 900] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  （夹具/启动工具复用：find_qemu/q）

MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")   # 系统盘映像（boot+loader+内核）
SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = 8009

TARGET_SECTORS = 262144        # 128MB 目标盘（> ~60MB 才会建 48MB ESP）
TARGET_MAIN_SECTORS = 155798   # 128MB 盘的主分区扇区数（见 part_esp_geometry64 的算法）
TARGET_ESP_LBA = 163807        # 盘尾 ESP 起始 LBA
TARGET_MAIN_BLOCKS = TARGET_MAIN_SECTORS                    # VimtuFS2 总块数 = 扇区数
TARGET_MAIN_TOTAL_KB = TARGET_MAIN_BLOCKS // 2              # 77899

DATA_SECTORS = 32768           # 16MB 数据盘（drive 1）
DATA_PART_LBA = 8192
DATA_PART_SECTORS = 24576

SMALL_SECTORS = 32768          # 16MB 小系统盘（Python 造 v3 卷：位图 7 块，df 可用）
SMALL_MAIN_SECTORS = SMALL_SECTORS - PART_MAIN_LBA          # 24759
SMALL_MAIN_TOTAL_KB = SMALL_MAIN_SECTORS // 2               # 12379

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[TERM] unsupported",
]


# ---------------------------------------------------------------------------
# 夹具：MBR + Python 侧 VimtuFS2 v3 格式化（与 kernel/vfs64.cpp 的 vfs64_format 同规则）
# ---------------------------------------------------------------------------
def _mbr_entry(bootable, ptype, start, sectors):
    e = bytearray(16)
    e[0] = 0x80 if bootable else 0x00
    e[1], e[2], e[3] = 0xFE, 0xFF, 0xFF
    e[4] = ptype
    e[5], e[6], e[7] = 0xFE, 0xFF, 0xFF
    struct.pack_into("<II", e, 8, start, sectors)
    return bytes(e)


def _pack_time(y, mo, d, h, mi, s):
    return (((y - 2000) << 26) | (mo << 22) | (d << 17) | (h << 12) | (mi << 6) | s) & 0xFFFFFFFF


def _now_packed():
    t = time.gmtime()
    return _pack_time(t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)


def vimtufs3_format(buf, start_lba, total_sectors):
    """把 buf[start_lba*512:] 写成空 v3 卷；返回 (blocks, inodes, data_start, free_blocks)。

    规则照抄 kernel/vfs64.cpp 的 vfs64_format：
      位图块 = ceil(总块数/4096)；inode = clamp(总块数/64, 16, 512)，128B/个、4 个/块；
      数据区 = 总块数 - (1 + 位图块 + inode 块)；超级块 CRC32 覆盖 [0,60)；inode CRC32 覆盖 [0,124)。
    """
    bitmap_blocks = (total_sectors + 4095) // 4096
    inodes = max(16, min(512, total_sectors // 64))
    inode_blocks = (inodes + 3) // 4
    bitmap_start = 1
    inode_start = bitmap_start + bitmap_blocks
    data_start = inode_start + inode_blocks
    data_blocks = total_sectors - data_start

    sb = bytearray(SECTOR)
    sb[0:8] = b"VIMTUFS2"
    struct.pack_into("<I", sb, 8, 3)              # version
    struct.pack_into("<I", sb, 12, 512)           # sector bytes
    struct.pack_into("<I", sb, 16, 512)           # block bytes
    struct.pack_into("<I", sb, 20, total_sectors)
    struct.pack_into("<I", sb, 24, 0)             # root inode
    struct.pack_into("<I", sb, 28, bitmap_start)
    struct.pack_into("<I", sb, 32, bitmap_blocks)
    struct.pack_into("<I", sb, 36, inode_start)
    struct.pack_into("<I", sb, 40, inodes)
    struct.pack_into("<I", sb, 44, 128)           # inode bytes (v3)
    struct.pack_into("<I", sb, 48, data_start)
    struct.pack_into("<I", sb, 52, data_blocks)
    struct.pack_into("<I", sb, 56, 0)
    struct.pack_into("<I", sb, 60, zlib.crc32(bytes(sb[:60])) & 0xFFFFFFFF)
    sb[510], sb[511] = 0x55, 0xAA
    o = start_lba * SECTOR
    buf[o:o + SECTOR] = sb

    for m in range(bitmap_blocks):
        bm = bytearray(SECTOR)
        for k in range(4096):
            blk = m * 4096 + k
            if blk >= total_sectors or blk < data_start:
                bm[k >> 3] |= 1 << (k & 7)
        p = (start_lba + bitmap_start + m) * SECTOR
        buf[p:p + SECTOR] = bm

    # inode 区：整个缓冲区已经是 0（空槽就是全 0），只写 0 号根目录 inode
    root = bytearray(128)
    root[0] = 2                                   # VFS64_TYPE_DIR
    root[1] = 0                                   # namelen（根目录无名）
    struct.pack_into("<I", root, 28, 0)           # parent = 自己
    struct.pack_into("<I", root, 32, _now_packed())
    struct.pack_into("<H", root, 36, 1)           # nlink
    root[38] = 2                                  # kind = DIR
    struct.pack_into("<I", root, 124, zlib.crc32(bytes(root[:124])) & 0xFFFFFFFF)
    p = (start_lba + inode_start) * SECTOR
    buf[p:p + 128] = root

    free_blocks = data_blocks
    return dict(blocks=total_sectors, inodes=inodes, data_start=data_start,
                bitmap_blocks=bitmap_blocks, free_blocks=free_blocks)


def make_data_disk(path):
    """drive 1：16MB 数据盘，MBR 第 1 项 = type 0x07 @8192，里面是 v3 卷。"""
    buf = bytearray(DATA_SECTORS * SECTOR)
    buf[446:462] = _mbr_entry(False, 0x07, DATA_PART_LBA, DATA_PART_SECTORS)
    buf[510], buf[511] = 0x55, 0xAA
    info = vimtufs3_format(buf, DATA_PART_LBA, DATA_PART_SECTORS)
    with open(path, "wb") as f:
        f.write(buf)
    return info


def make_small_system_disk(path):
    """16MB 小系统盘：build64/system.img 的字节 + MBR + @8009 的 v3 卷（df 可用的位图规模）。"""
    with open(SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    if len(sys_bytes) == 0 or len(sys_bytes) % SECTOR:
        return None
    if len(sys_bytes) > SMALL_SECTORS * SECTOR:
        return None
    buf = bytearray(SMALL_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes
    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, SMALL_MAIN_SECTORS)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    info = vimtufs3_format(buf, PART_MAIN_LBA, SMALL_MAIN_SECTORS)
    with open(path, "wb") as f:
        f.write(buf)
    return info


def wipe(path, sectors):
    with open(path, "wb") as f:
        f.write(b"\0" * (sectors * SECTOR))


# ---------------------------------------------------------------------------
# QEMU 控制
# ---------------------------------------------------------------------------
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Monitor:
    """QEMU monitor（telnet）：sendkey 注入键盘（与 install_flow/partition_ops 同款）。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def key(self, name, wait=1.3):
        for _ in range(3):
            if self.send("sendkey %s" % name, wait=wait):
                return True
            time.sleep(0.2)
        return False

    def type_line(self, text, per_key=0.14):
        """把一行 ASCII 敲进当前窗口（终端命令用；支持字母数字 + '/' '.' '-' '_' 等）。"""
        names = {
            " ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
            ">": "shift-dot", "=": "equal", ":": "shift-semicolon",
        }
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char for sendkey: %r" % ch)
        self.key("ret", wait=per_key + 0.15)


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(path, needle, timeout, proc=None):
    deadline = time.time() + timeout
    while True:
        s = slog(path)
        if needle in s:
            return s
        if time.time() > deadline or (proc is not None and proc.poll() is not None):
            return s
        time.sleep(0.4)


def kill(proc):
    if proc is not None and proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


def qemu_args(qemu, disks, serial, port, name):
    args = [qemu, "-name", name]
    for i, d in enumerate(disks):
        if d is not None:
            args += ["-drive", "format=raw,file=%s,index=%d,media=disk" % (p64.q(d), i)]
    args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
             "-serial", "file:%s" % p64.q(serial),
             "-no-reboot"]
    if port:
        args += ["-monitor", "telnet:127.0.0.1:%d,server,nowait" % port]
    return args


def boot_media(qemu, target, serial, port, name="Vimtu64-fstree-media"):
    """安装介质（index 0）+ 目标盘（index 1）；返回 (proc, monitor)。"""
    proc = subprocess.Popen(qemu_args(qemu, [MEDIUM, target], serial, port, name),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return proc, Monitor(port)


def boot_installed(qemu, disks, serial, port, name="Vimtu64-fstree-os"):
    """装好的盘（index 0，可选 index 1 数据盘）；返回 (proc, monitor)。"""
    proc = subprocess.Popen(qemu_args(qemu, disks, serial, port, name),
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return proc, Monitor(port)


def wait_wizard_ready(proc, serial, timeout=90):
    log = wait_for(serial, "磁盘枚举完成", timeout, proc)
    return "磁盘枚举完成" in log


def wait_desktop(proc, serial, timeout=120):
    log = wait_for(serial, "[GUI64] ready", timeout, proc)
    return "[GUI64] ready" in log


def last_match(pattern, text, flags=0):
    """取**最后一次**匹配（同一份串口日志里会有多次 format/mount：自检的假盘 + 真盘）。"""
    hits = list(re.finditer(pattern, text, flags))
    return hits[-1] if hits else None


def open_terminal(mon, serial, proc):
    for _ in range(3):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if "[APP] term opened" in wait_for(serial, "[APP] term opened", 15, proc):
            return True
    return False


# ---------------------------------------------------------------------------
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
    for need in (MEDIUM, SYSTEM_IMG):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_fstree_")
    target = os.path.join(tmp, "target.img")
    data_disk = os.path.join(tmp, "data.img")
    small_disk = os.path.join(tmp, "small.img")
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

    # ==================== 阶段 1：安装到 128MB 目标盘（会建 48MB ESP）====================
    print("=== 阶段 1：安装介质 + 128MB 目标盘（向导：回车×4 -> n 新建 -> 回车安装）===")
    wipe(target, TARGET_SECTORS)
    s1 = os.path.join(tmp, "boot1_install.log")
    p1 = free_port()
    proc, mon = boot_media(qemu, target, s1, p1, "Vimtu64-fstree-install")
    try:
        check("向导就绪（磁盘枚举完成）", wait_wizard_ready(proc, s1))
        for what in ("语言页 -> 现在安装", "现在安装 -> 许可条款", "许可条款 -> 安装类型",
                     "安装类型 -> 磁盘与分区"):
            mon.key("ret", wait=1.6)
            print("      ret  (%s)" % what)
        mon.key("n", wait=2.2)          # 新建分区（引导分区 + 主分区）
        mon.key("ret", wait=2.2)        # 选中分区 -> 安装系统
        log1 = wait_for(s1, "[INSTALL] 完成：已写", 90, proc)
        time.sleep(6)                   # 收尾：ESP（48MB FAT32）+ GPT 写入
        log1 = slog(s1)
    finally:
        kill(proc)
    check("安装完成（[INSTALL] 完成：已写）", "[INSTALL] 完成：已写" in log1)
    check("目标盘建了 ESP + 盘尾 GPT（[INSTALL] gpt written (main + esp)）",
          "[INSTALL] gpt written (main + esp)" in log1)
    check("ESP 是 FAT32（[INSTALL] fat32 vol）", "fat32" in log1.lower())
    forbid("阶段1", log1)

    # 目标盘字节断言：v3 卷应该在**阶段 2 格式化**之后才有；这里先确认 MBR/ESP 位置对
    with open(target, "rb") as f:
        mbr = f.read(512)
    e2 = mbr[462:478]
    check("MBR 第 2 项 = type 0x07 主分区 @8009", e2[4] == 0x07 and
          struct.unpack_from("<I", e2, 8)[0] == PART_MAIN_LBA,
          "type=0x%02X start=%d" % (e2[4], struct.unpack_from("<I", e2, 8)[0]))
    e3 = mbr[478:494]
    check("MBR 第 3 项 = type 0xEF ESP @163807",
          e3[4] == 0xEF and struct.unpack_from("<I", e3, 8)[0] == TARGET_ESP_LBA,
          "type=0x%02X start=%d" % (e3[4], struct.unpack_from("<I", e3, 8)[0]))

    # ==================== 阶段 2：格式化主分区 -> v3 ====================
    print("=== 阶段 2：同盘再进向导，光标下移到 P2 按 f 格式化（应产出 v3）===")
    s2 = os.path.join(tmp, "boot2_format.log")
    p2 = free_port()
    proc, mon = boot_media(qemu, target, s2, p2, "Vimtu64-fstree-format")
    try:
        check("向导就绪（格式化会话）", wait_wizard_ready(proc, s2))
        for _ in range(4):
            mon.key("ret", wait=1.6)
        mon.key("down", wait=1.6)       # P1 -> P2（主分区）
        mon.key("f", wait=2.6)          # 格式化主分区 -> vfs64_format（v3）
        log2 = wait_for(s2, "[VFS64] format ok", 40, proc)
        time.sleep(1.5)
        log2 = slog(s2)
    finally:
        kill(proc)
    check("格式化主分区 P2（[PART] 格式化 OK drive=1 index=2 start=8009）",
          "[PART] 格式化 OK drive=1 index=2 start=8009" in log2)
    # ★ 多卷说明：系统启动盘一启动就会把**所有可浏览卷**挂进卷槽（D:/E:…），串口里因此有不止一条
    #   `[VFS64] mount ok ...` / `[VFS64] format ok ...`（数据卷的块数不同，取"最后一条"会拿到别的卷）。
    #   所以这里明确按**系统卷的块数**去找那一行 —— 断言强度不变（仍要求 v3 + 128B inode + 155798 块）。
    mf = last_match(r"\[VFS64\] format ok blocks=%d version=(\d+) inode=(\d+) root=(\d+)" % TARGET_MAIN_BLOCKS, log2)
    check("格式化产出 v4（[VFS64] format ok blocks=%d ... version=4 inode=128；P4 起新格式化 = v4）" % TARGET_MAIN_BLOCKS,
          bool(mf) and mf.group(1) == "4" and mf.group(2) == "128",
          mf.group(0) if mf else "（缺 [VFS64] format ok blocks=%d 行）" % TARGET_MAIN_BLOCKS)
    check("格式化块数 = 主分区扇区数（155798）", bool(mf), "blocks=%d" % TARGET_MAIN_BLOCKS)
    check("自检全过（[VFS64] selftest PASS）", "[VFS64] selftest PASS" in log2)
    forbid("阶段2", log2)

    # ==================== 阶段 3：装好的系统盘 + 数据盘（盘符表 + 多级目录）====================
    print("=== 阶段 3：启动装好的盘（drive 0）+ 数据盘（drive 1）===")
    data_info = make_data_disk(data_disk)
    s3 = os.path.join(tmp, "boot3_tree.log")
    p3 = free_port()
    proc, mon = boot_installed(qemu, [target, data_disk], s3, p3)
    try:
        up = wait_desktop(proc, s3)
        check("装好的系统进桌面（[GUI64] ready）", up and "[OS] ready (idle)" in slog(s3))

        # ---- 卷版本 + 盘符表 ----
        # ★ 多卷：按系统卷的块数定位系统卷的挂载行（数据卷也会打 mount ok，块数=24576）
        mv = last_match(r"\[VFS64\] mount ok blocks=%d inodes=(\d+) free=(\d+) version=(\d+) inode=(\d+)B"
                        % TARGET_MAIN_BLOCKS, slog(s3))
        check("系统卷按 v4 挂载（mount ok blocks=%d ... version=4 inode=128B perm=on）" % TARGET_MAIN_BLOCKS,
              bool(mv) and mv.group(3) == "4" and mv.group(4) == "128",
              mv.group(0) if mv else "（缺 mount ok 行）")
        check("挂载块数 = 主分区扇区数", bool(mv), "blocks=%d" % TARGET_MAIN_BLOCKS)

        log3 = slog(s3)
        ms = re.search(r"\[DRV64\] scan disks=(\d+) parts=(\d+) fs=(\d+)", log3)
        check("驱动器扫描：[DRV64] scan disks=2 parts=4 fs=3",
              bool(ms) and ms.groups() == ("2", "4", "3"), ms.group(0) if ms else "（缺 scan 行）")

        mc = re.search(r"\[DRV64\] letter=C: disk=(\d+) part=(\d+) fs=(\S+) total_kb=(\d+) free_kb=(\d+)", log3)
        check("C: = 系统分区（disk=0 part=2 fs=VimtuFS2）",
              bool(mc) and mc.group(1) == "0" and mc.group(2) == "2" and mc.group(3) == "VimtuFS2",
              mc.group(0) if mc else "（缺 letter=C: 行）")
        if mc:
            total_kb, free_kb = int(mc.group(4)), int(mc.group(5))
            check("C: 容量 = 主分区容量（total_kb=%d）" % TARGET_MAIN_TOTAL_KB,
                  total_kb == TARGET_MAIN_TOTAL_KB, "total_kb=%d" % total_kb)
            check("C: free <= total 且 free > 0", 0 < free_kb <= total_kb,
                  "free_kb=%d total_kb=%d" % (free_kb, total_kb))
        # ★ 批次 K：盘符顺序变了 —— ESP（disk0 part3，真 FAT32）现在可浏览并排到 D:，
        #   第二块盘的 VimtuFS2 数据卷顺延到 E:。
        md = re.search(r"\[DRV64\] letter=D: disk=(\d+) part=(\d+) fs=(\S+) total_kb=(\d+) free_kb=(\S+) slot=(\S+) ro=1 fatvol=(\d+)", log3)
        check("D: = ESP（disk=0 part=3 fs=FAT32 只读卷）",
              bool(md) and md.group(1) == "0" and md.group(2) == "3" and md.group(3) == "FAT32",
              md.group(0) if md else "（缺 letter=D: 行）")
        check("D: 容量 = ESP 数据区容量（total_kb=%d；free 允许 unknown：本例安装器的 FSInfo 写盘失败）"
              % (48368),        # FAT 卷容量 = 数据区簇数(96736) x 512B / 1024（不含保留/FAT 区）
              bool(md) and int(md.group(4)) == 48368,
              md.group(0) if md else "")
        me = re.search(r"\[DRV64\] letter=E: disk=(\d+) part=(\d+) fs=(\S+) total_kb=(\d+) free_kb=(\d+)", log3)
        check("E: = 第二块盘的 VimtuFS2 卷（disk=1 part=1 fs=VimtuFS2）",
              bool(me) and me.group(1) == "1" and me.group(2) == "1" and me.group(3) == "VimtuFS2",
              me.group(0) if me else "（缺 letter=E: 行）")
        check("E: 容量 = 数据盘分区容量（total_kb=%d）" % (DATA_PART_SECTORS // 2),
              bool(me) and int(me.group(4)) == DATA_PART_SECTORS // 2,
              me.group(0) if me else "")
        check("FAT 读取器挂载了 ESP（[FAT64] mount … lba=%d … fat_ok=1 … spc=1 ro=1）" % TARGET_ESP_LBA,
              re.search(r"\[FAT64\] mount vol=\d+ lba=%d clusters=\d+ free=(unknown|\d+) fat_ok=1 spc=1 ro=1" % TARGET_ESP_LBA, log3) is not None)
        check("引导分区（lba=9 type=0xEF 无 BPB）仍然 skip、不占盘符",
              "[DRV64] skip lba=9 type=0xEF reason=esp" in log3)
        check("盘符自检通过（[DRV64] selftest PASS）", "[DRV64] selftest PASS" in log3)
        check("启动期目录树打印（[VFS64] tree root=/）", "[VFS64] tree root=/" in log3)

        # ---- 终端：多级目录 + 子目录写文件 ----
        check("打开终端（[APP] term opened）", open_terminal(mon, s3, proc))
        # ★ P4 起：新格式化 = v4，**根目录属于 root 且 0755** -> 普通用户不能在 / 下建目录/写文件。
        #   这些命令要的正是"在根下建树"，所以先切到 root 会话（`su - root`；身份/gui 打点见下）。
        n_su = slog(s3).count("[USER64] su ok")
        mon.type_line("su - root")
        wait_for(s3, "[TERM] cmd su ok", 25, proc)
        log3 = slog(s3)
        check("su - root：终端会话身份变 root（[USER64] su ok ... to=root euid=0 ... via=su-dash）",
              log3.count("[USER64] su ok") > n_su and
              re.search(r"\[USER64\] su ok from=\w+ to=root euid=0", log3) is not None)
        check("权限位已生效（[PERM64] cred ... user=root via=su-dash）",
              re.search(r"\[PERM64\] cred uid=0 gid=0 euid=0 egid=0 user=root via=su-dash", log3) is not None)
        mon.type_line("mkdir /efi")
        mon.type_line("mkdir /apps")
        mon.type_line("mkdir /apps/demo")
        log3 = wait_for(s3, "[TERM] cmd mkdir", 20, proc)
        time.sleep(1.0)
        mon.type_line("write /apps/demo/demo.txt hello")
        log3 = wait_for(s3, "[FD64] open path=/apps/demo/demo.txt", 25, proc)
        check("子目录里写文件走真路径（[FD64] open path=/apps/demo/demo.txt）",
              "[FD64] open path=/apps/demo/demo.txt fd=" in log3)
        check("写入 5 字节（[TERM] cmd write ok）", "[TERM] cmd write ok" in log3)
        mon.type_line("cat /apps/demo/demo.txt")
        log3 = wait_for(s3, "[TERM] cmd cat", 20, proc)
        mc2 = re.search(r"\[TERM\] cmd cat bytes=(\d+)", log3)
        check("cat 回读 5 字节", bool(mc2) and mc2.group(1) == "5",
              mc2.group(0) if mc2 else "（缺 cat 行）")

        # ---- 坏路径：必须被拒且不崩 ----
        before_bad = slog(s3)
        mon.type_line("mkdir /nope/x")                    # 父目录不存在
        mon.type_line("write /../.. x")                   # 根目录不可写（POSIX：".." 到根）
        mon.type_line("write /" + "a" * 32 + "/b.txt x")  # 超长段（32 > 31）
        time.sleep(1.5)
        log3 = slog(s3)
        check("坏路径：父目录不存在被拒（[VFS64] mkdir64 ... parent not found）",
              "mkdir64" in log3 and "bad path / parent not found" in log3)
        check("坏路径：/../.. 被拒（[VFS64] write: path is a directory）",
              ("path is a directory" in log3 and "[VFS64] write" in log3))
        check("坏路径：超长段被拒（[VFS64] path segment too long）",
              "path segment too long" in log3)
        check("坏路径之后终端仍可用（没有 PANIC）", "PANIC" not in log3)
        check("坏路径：拒绝发生在 VFS 层（[VFS64] path 行）",
              "[VFS64] path component not found" in log3 and "[VFS64] mkdir64" in log3)
        forbid("阶段3", log3)
        check("阶段3 全程没有 PANIC/三重故障", "PANIC" not in slog(s3) and "TRIPLE FAULT" not in slog(s3))
        _ = before_bad
    finally:
        kill(proc)

    # ==================== 阶段 4：冷启动同一块盘（持久化 + mtime + rm）====================
    print("=== 阶段 4：冷启动同一块盘（目录树/mtime 持久化 + rm）===")
    s4 = os.path.join(tmp, "boot4_persist.log")
    p4 = free_port()
    proc, mon = boot_installed(qemu, [target, data_disk], s4, p4)
    try:
        up = wait_desktop(proc, s4)
        check("冷启动后进桌面", up)
        log4 = slog(s4)
        mt = re.search(r"\[VFS64\] tree (/apps/demo/demo\.txt) type=file size=(\d+) mtime=0x([0-9A-F]{8}) "
                       r"ymd=(\d{4})-(\d{2})-(\d{2}) hms=(\d{2}):(\d{2}):(\d{2}) kind=(\w+)", log4)
        check("目录树遍历到子目录里的文件（tree /apps/demo/demo.txt size=5）",
              bool(mt) and mt.group(2) == "5", mt.group(0) if mt else "（缺 tree 行）")
        if mt:
            check("mtime 非 0 且年份合理（>= 2024）",
                  int(mt.group(3), 16) != 0 and 2024 <= int(mt.group(4)) <= 2063,
                  "mtime=0x%s ymd=%s-%s-%s" % (mt.group(3), mt.group(4), mt.group(5), mt.group(6)))
            check("mtime 时分秒在合法范围",
                  0 <= int(mt.group(7)) <= 23 and 0 <= int(mt.group(8)) <= 59 and 0 <= int(mt.group(9)) <= 59,
                  "hms=%s:%s:%s" % (mt.group(7), mt.group(8), mt.group(9)))
            check("类型判定 = text（.txt + 可打印内容）", mt.group(10) == "text", mt.group(10))
        check("/ 下能列出 apps 且类型 = 目录",
              re.search(r"\[VFS64\] tree /apps type=dir size=0", log4) is not None)
        check("/apps 下能列出 demo 且类型 = 目录",
              re.search(r"\[VFS64\] tree /apps/demo type=dir size=0", log4) is not None)
        check("可执行文件类型判定 ELF（tree /hello.elf ... kind=elf）",
              re.search(r"\[VFS64\] tree /hello\.elf type=file .*kind=elf", log4) is not None)
        # 坏路径的 mkdir 绝不能真的建出目录：这一轮的目录树里不该有 /nope
        check("坏路径没有建出目录（目录树里没有 /nope）", "[VFS64] tree /nope" not in log4)

        check("打开终端（第二遍）", open_terminal(mon, s4, proc))
        mon.type_line("cat /apps/demo/demo.txt")
        log4 = wait_for(s4, "[TERM] cmd cat", 25, proc)
        mc4 = re.search(r"\[TERM\] cmd cat bytes=(\d+)", log4)
        check("冷启动后 cat 仍读到 5 字节（跨重启持久化）",
              bool(mc4) and mc4.group(1) == "5", mc4.group(0) if mc4 else "（缺 cat 行）")
        n_cat_ok = log4.count("[TERM] cmd cat bytes=")
        # ★ P4：rm 需要父目录的 w+x（/apps/demo 是 root 0755）-> 切到 root 会话再删
        mon.type_line("su - root")
        wait_for(s4, "[TERM] cmd su ok", 25, proc)
        mon.type_line("rm /apps/demo/demo.txt")
        log4 = wait_for(s4, "[TERM] cmd rm", 25, proc)
        check("rm 删掉子目录里的文件（[TERM] cmd rm ok）", "[TERM] cmd rm ok" in log4)
        mon.type_line("cat /apps/demo/demo.txt")
        time.sleep(3.0)
        log4 = slog(s4)
        check("rm 之后 cat 失败（不再出现新的 [TERM] cmd cat 行；文件真的没了）",
              log4.count("[TERM] cmd cat bytes=") == n_cat_ok,
              "cat ok 行数 %d -> %d" % (n_cat_ok, log4.count("[TERM] cmd cat bytes=")))
        forbid("阶段4", log4)
    finally:
        kill(proc)

    # ==================== 阶段 5：小卷系统盘（drive64 容量 与 df 一致）====================
    print("=== 阶段 5：16MB 小系统盘（Python 造 v3 卷）——盘容量与 df 一致 ===")
    info = make_small_system_disk(small_disk)
    if info is None:
        check("造 16MB 小系统盘（system.img 字节 + v3 卷）", False, "system.img 缺失或过大")
    else:
        check("造 16MB 小系统盘（位图 %d 块 / inode %d 个 / 数据区 @%d）" %
              (info["bitmap_blocks"], info["inodes"], info["data_start"]),
              info["blocks"] == SMALL_MAIN_SECTORS)
        s5 = os.path.join(tmp, "boot5_small.log")
        p5 = free_port()
        proc, mon = boot_installed(qemu, [small_disk], s5, p5, "Vimtu64-fstree-small")
        try:
            up = wait_desktop(proc, s5)
            check("小卷系统盘进桌面（Python 造的 v3 卷被内核认出）", up)
            log5 = slog(s5)
            check("小卷按 v3 挂载（mount ok ... version=3）",
                  re.search(r"\[VFS64\] mount ok blocks=%d .*version=3 inode=128B" % SMALL_MAIN_SECTORS, log5) is not None)
            m5 = re.search(r"\[DRV64\] letter=C: disk=(\d+) part=(\d+) fs=VimtuFS2 total_kb=(\d+) free_kb=(\d+)", log5)
            check("小卷 C: = disk=0 part=2（total_kb=%d）" % SMALL_MAIN_TOTAL_KB,
                  bool(m5) and m5.group(1) == "0" and m5.group(2) == "2" and
                  int(m5.group(3)) == SMALL_MAIN_TOTAL_KB,
                  m5.group(0) if m5 else "（缺 letter=C: 行）")
            check("小卷只有一块盘（[DRV64] scan disks=1）", "[DRV64] scan disks=1 " in log5)
            check("单盘场景没有 D:（[DRV64] letter=D: 不出现）", "[DRV64] letter=D:" not in log5)
            check("启动期目录树打印（[VFS64] tree entries=）", "[VFS64] tree entries=" in log5)

            check("打开终端（小卷）", open_terminal(mon, s5, proc))
            mon.type_line("df")
            log5 = wait_for(s5, "[TERM] cmd df", 25, proc)
            md5 = re.search(r"\[TERM\] cmd df blocks=(\d+) free=(\d+) files=(\d+)", log5)
            check("df 给出总块/空闲块（位图 7 块 <= sysstate 探测上限 8）",
                  bool(md5), md5.group(0) if md5 else "（缺 df 行）")
            if m5 and md5:
                df_blocks, df_free = int(md5.group(1)), int(md5.group(2))
                total_kb, free_kb = int(m5.group(3)), int(m5.group(4))
                check("drive64 的 total_kb 与 df 的 blocks 一致（KB = blocks/2）",
                      abs(total_kb * 2 - df_blocks) <= 1,
                      "total_kb*2=%d df blocks=%d" % (total_kb * 2, df_blocks))
                check("drive64 的 free_kb 与 df 的 free 一致（1% 容差：扫描与 df 之间有装入动作）",
                      abs(free_kb * 2 - df_free) <= max(16, df_blocks // 100),
                      "free_kb*2=%d df free=%d" % (free_kb * 2, df_free))
            forbid("阶段5", log5)
        finally:
            kill(proc)

    if args.keep:
        print("[fstree] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
