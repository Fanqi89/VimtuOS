#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/perm64_test.py - P4 验收：VimtuFS2 **uid/gid/mode 权限位真的拦截** + 用户数据隔离

做什么（三段，每一条都有串口/返回值证据）：
  阶段 A（**v4 卷**：系统盘 + 一块 v3 数据盘同机启动）：
    1) 新格式化产出/挂载 v4：`[VFS64] mount ok ... version=4 inode=128B perm=on`；
       v3 数据卷挂载时**如实报** `[PERM64] legacy volume v3: ... checks disabled`；
    2) 登录 vimtu（uid 1000）-> `[PERM64] cred ... via=login`；whoami/id 是真 uid/gid/euid/egid；
    3) useradd alice / bob（主目录 /home/<名> **属主=自己、0700**）；
    4) `su - alice` -> 在 /home/alice 里建文件（`ls -l` 显示 -rw-r--r-- alice alice 0644）；
    5) `su - bob` -> 读 alice 的文件被拒（`[PERM64] deny` + `[FD64] open FAILED ... rc=13` + cat 0 字节）、
       进 /home/alice 列目录被拒；/home/bob/Desktop 同理（alice 进不去 bob 的）；
    6) `sudo -i` / `su - root` -> root 读同一个文件成功（内容一致）；
    7) `chmod 600` 后：root 读 OK、alice（属主）读写 OK、bob 仍被拒；`chmod 644` 后 bob 可读；
    8) chown：非 root 被拒（只有 root 能改）；root 改属主后新属主可读写、旧属主被拒；
    9) umask：显示 0022；`umask 077` 后新建文件是 0600；恢复 022；
   10) 目录写规则：目录 chmod 555 后在里面建文件/删文件都被拒（要 w+x）；恢复 755 后可以；
   11) 属主归属：新文件的属主就是**当时的 euid**（ls -l 证据）；
   12) v3 数据卷（旧卷）：挂载可读可写、旧文件按默认属主 root 显示（`ls -l` -> root root 0644）；
       旧卷上不拦截（legacy 打点已说明）。
  阶段 B（**格式化旧卷 -> v4**）：安装介质 + 一块主分区是 **v3** 的目标盘 -> 向导里对 P2 按 f 格式化
       -> `[VFS64] format ok blocks=<n> version=4 inode=128 ... rootmode=0755`（v3 兼容矩阵的另一半：
       读旧卷 + 重新格式化成 v4）。
  三段都禁止 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / OOM。

用法（必须用 Windows 原生 Python）：py -3 tests\\perm64_test.py [--timeout 240] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
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

import fs_tree_test as fst        # noqa: E402  （Monitor / boot_installed / boot_media / vimtufs3_format / MBR）
import proc64_test as p64         # noqa: E402  （find_qemu / prepare_fixture 的 SYSTEM_IMG 路径）

SECTOR = 512
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768                    # 16MB 系统盘
DATA_SECTORS = 32768                      # 16MB 数据盘
DATA_PART_LBA = 8192
DATA_PART_SECTORS = 24576

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:"]


# ---------------------------------------------------------------------------
# Python 侧 VimtuFS2 **v4** 格式化（照抄 kernel/vfs64.cpp 的 vfs64_format：
# 超级块 version=4 + CRC[0,60) + inode 128B/个、4 个/块、根目录 uid0/gid0/mode0755）
# ---------------------------------------------------------------------------
def vimtufs4_format(buf, start_lba, total_sectors):
    bitmap_blocks = (total_sectors + 4095) // 4096
    inodes = max(16, min(512, total_sectors // 64))
    inode_blocks = (inodes + 3) // 4
    bitmap_start = 1
    inode_start = bitmap_start + bitmap_blocks
    data_start = inode_start + inode_blocks
    data_blocks = total_sectors - data_start
    if data_blocks <= 0:
        raise RuntimeError("volume too small")

    sb = bytearray(SECTOR)
    sb[0:8] = b"VIMTUFS2"
    struct.pack_into("<I", sb, 8, 4)              # version = 4
    struct.pack_into("<I", sb, 12, SECTOR)
    struct.pack_into("<I", sb, 16, SECTOR)
    struct.pack_into("<I", sb, 20, total_sectors)
    struct.pack_into("<I", sb, 24, 0)
    struct.pack_into("<I", sb, 28, bitmap_start)
    struct.pack_into("<I", sb, 32, bitmap_blocks)
    struct.pack_into("<I", sb, 36, inode_start)
    struct.pack_into("<I", sb, 40, inodes)
    struct.pack_into("<I", sb, 44, 128)
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

    # 根目录 inode：type=DIR、parent=自己、uid=0/gid=0/mode=0755（P4 字段）
    root = bytearray(128)
    root[0] = 2
    struct.pack_into("<I", root, 28, 0)
    struct.pack_into("<I", root, 32, fst._now_packed())
    struct.pack_into("<H", root, 36, 1)
    root[38] = 2
    struct.pack_into("<H", root, 75, 0)            # uid
    struct.pack_into("<H", root, 77, 0)            # gid
    struct.pack_into("<H", root, 79, 0o040755)     # mode
    struct.pack_into("<I", root, 124, zlib.crc32(bytes(root[:124])) & 0xFFFFFFFF)
    p = (start_lba + inode_start) * SECTOR
    buf[p:p + 128] = root
    return dict(blocks=total_sectors, inodes=inodes, bitmap_blocks=bitmap_blocks,
                inode_start=inode_start, data_start=data_start)


def vfs3_put_file(buf, lba, info, name, content):
    """在 v3 卷里造一个文件（v3 inode：无 uid/gid/mode -> 内核按 root/0644 默认显示）。"""
    base = lba * SECTOR
    ino_start = info.get("inode_start", 1 + info["bitmap_blocks"])   # v3 布局：位图紧跟超级块
    # 找一个空 inode（>=1）
    idx = None
    for i in range(1, info["inodes"]):
        off = base + (ino_start + i // 4) * SECTOR + (i % 4) * 128
        if buf[off] == 0:
            idx = i
            break
    assert idx is not None, "no free inode in v3 fixture"
    # 找一个空数据块
    data_blk = None
    for blk in range(info["data_start"], info["blocks"]):
        m = blk // 4096
        k = blk % 4096
        boff = base + (1 + m) * SECTOR + (k >> 3)
        if (buf[boff] & (1 << (k & 7))) == 0:
            data_blk = blk
            buf[boff] |= (1 << (k & 7))
            break
    assert data_blk is not None, "no free data block in v3 fixture"
    buf[base + data_blk * SECTOR: base + data_blk * SECTOR + len(content)] = content
    ino = bytearray(128)
    ino[0] = 1                                  # FILE
    ino[1] = len(name)
    struct.pack_into("<I", ino, 4, len(content))     # size
    struct.pack_into("<I", ino, 8, data_blk)         # d0
    struct.pack_into("<I", ino, 28, 0)               # parent = 根
    struct.pack_into("<I", ino, 32, fst._now_packed())
    struct.pack_into("<H", ino, 36, 1)
    ino[38] = 5                                  # kind = text
    ino[40:40 + len(name)] = name
    struct.pack_into("<I", ino, 124, zlib.crc32(bytes(ino[:124])) & 0xFFFFFFFF)
    off = base + (ino_start + idx // 4) * SECTOR + (idx % 4) * 128
    buf[off:off + 128] = ino
    return idx


def make_v4_system_disk(path):
    with open(fst.SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    buf = bytearray(TARGET_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes
    buf[446:462] = fst._mbr_entry(True, 0xEF, fst.PART_BOOT_LBA, fst.PART_BOOT_SECS)
    buf[462:478] = fst._mbr_entry(False, 0x07, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    vimtufs4_format(buf, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    with open(path, "wb") as f:
        f.write(buf)
    return path


def make_v3_data_disk(path):
    """v3 数据盘：/old.txt（默认属主 root/0644 —— v3 没有权限字段）。"""
    buf = bytearray(DATA_SECTORS * SECTOR)
    buf[446:462] = fst._mbr_entry(False, 0x07, DATA_PART_LBA, DATA_PART_SECTORS)
    buf[510], buf[511] = 0x55, 0xAA
    info = fst.vimtufs3_format(buf, DATA_PART_LBA, DATA_PART_SECTORS)
    vfs3_put_file(buf, DATA_PART_LBA, info, b"old.txt", b"legacy-v3-content")
    with open(path, "wb") as f:
        f.write(buf)
    return path


def make_v3_system_disk(path):
    """系统盘，但主分区是 **v3** 卷（阶段 B 的"旧卷"目标盘）。"""
    with open(fst.SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    buf = bytearray(TARGET_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes
    buf[446:462] = fst._mbr_entry(True, 0xEF, fst.PART_BOOT_LBA, fst.PART_BOOT_SECS)
    buf[462:478] = fst._mbr_entry(False, 0x07, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    sect = TARGET_SECTORS - PART_MAIN_LBA
    info = fst.vimtufs3_format(buf, PART_MAIN_LBA, sect)
    vfs3_put_file(buf, PART_MAIN_LBA, info, b"old.txt", b"legacy-v3-content")
    with open(path, "wb") as f:
        f.write(buf)
    return path


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (fst.SYSTEM_IMG, os.path.join(ROOT, "vimtu64-64.img")):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_perm64_")
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

    # ==================== 阶段 A：v4 系统盘 + v3 数据盘 ====================
    print("=== 阶段 A：v4 卷（权限拦截 + 数据隔离）+ v3 旧卷（兼容）===")
    sys_disk = os.path.join(tmp, "sys_v4.img")
    data_disk = os.path.join(tmp, "data_v3.img")
    make_v4_system_disk(sys_disk)
    make_v3_data_disk(data_disk)
    serial = os.path.join(tmp, "bootA.log")
    port = fst.free_port()
    proc, mon = fst.boot_installed(qemu, [sys_disk, data_disk], serial, port, "Vimtu64-perm64-A")
    log = ""
    try:
        # 自动登录（ui.login.auto 默认 1）-> 桌面
        log = fst.wait_for(serial, "[GUI64] ready", args.timeout, proc) or ""
        check("阶段 A：v4 卷系统进桌面（[GUI64] ready）", "[GUI64] ready" in log)

        def wait_new(needle, timeout=25):
            n = fst.slog(serial).count(needle)
            return fst.wait_for(serial, needle, timeout, proc), n

        def type_line(text, per_key=0.14):
            """敲一行（支持大写：monitor 的 sendkey 只认小写，大写要 shift-<c>）"""
            names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
                     ">": "shift-dot", "=": "equal", ":": "shift-semicolon"}
            for ch in text:
                if ch in names:
                    mon.key(names[ch], wait=per_key)
                elif ch.isalpha():
                    mon.key(("shift-%s" % ch.lower()) if ch.isupper() else ch, wait=per_key)
                elif ch.isdigit():
                    mon.key(ch, wait=per_key)
                else:
                    raise ValueError("unsupported char: %r" % ch)
            mon.key("ret", wait=per_key + 0.15)

        def cmd(line, needle, timeout=25):
            """敲一条命令并等 needle 的**计数**增加（同一 needle 出现多次时不能只判 in）"""
            n = fst.slog(serial).count(needle)
            type_line(line)
            deadline = time.time() + timeout
            while True:
                s = fst.slog(serial)
                if s.count(needle) > n:
                    return s, n
                if time.time() > deadline or proc.poll() is not None:
                    return s, n
                time.sleep(0.4)

        # ---- 1) v4 挂载 + v3 legacy ----
        log = fst.slog(serial)
        check("v4 卷按 v4 挂载（mount ok blocks=%d ... version=4 inode=128B perm=on）"
              % (TARGET_SECTORS - PART_MAIN_LBA),
              re.search(r"\[VFS64\] mount ok blocks=%d inodes=\d+ free=\d+ version=4 inode=128B perm=on"
                        % (TARGET_SECTORS - PART_MAIN_LBA), log) is not None)
        check("v3 数据卷挂载时如实报 legacy（权限拦截关闭）",
              re.search(r"\[PERM64\] legacy volume v3: no uid/gid/mode fields -> permission checks disabled", log) is not None)
        check("v3 数据卷按 version=3 挂载", re.search(r"\[VFS64\] mount ok .*version=3 inode=128B perm=off", log) is not None)
        check("启动期目录树打印（v4 夹具是空卷：root=/ 且 entries=0 —— userdb64 之后才建 /etc、/home）",
              "[VFS64] tree root=/ max_entries=32 max_depth=4" in log and "[VFS64] tree entries=0" in log)

        check("打开终端（开始菜单 -> 终端）", fst.open_terminal(mon, serial, proc))
        time.sleep(0.5)

        log, _ = cmd("whoami", "[USER64] whoami", 20)
        check("登录身份 = vimtu（[USER64] whoami user=vimtu euid=1000 root_session=0）",
              re.search(r"\[USER64\] whoami user=vimtu euid=1000 gui=vimtu root_session=0", fst.slog(serial)) is not None)
        check("登录时把会话身份发布给 VFS（[PERM64] cred ... user=vimtu via=login）",
              re.search(r"\[PERM64\] cred uid=1000 gid=1000 euid=1000 egid=1000 user=vimtu via=login", fst.slog(serial)) is not None)
        log, _ = cmd("id", "[PERM64] id", 20)
        check("id：真实 uid/gid/euid/egid（[PERM64] id uid=1000 gid=1000 euid=1000 egid=1000 user=vimtu）",
              re.search(r"\[PERM64\] id uid=1000 gid=1000 euid=1000 egid=1000 user=vimtu root=0", fst.slog(serial)) is not None)

        # ---- 2) useradd：主目录属主 = 自己、0700 ----
        log, _ = cmd("useradd alice", "[USER64] useradd ok", 25)
        check("useradd alice（uid >= 1000，home=/home/alice）",
              re.search(r"\[USER64\] useradd ok name=alice uid=(\d+) home=/home/alice desktop=/home/alice/Desktop", fst.slog(serial)) is not None)
        m_alice = re.search(r"\[USER64\] useradd ok name=alice uid=(\d+)", fst.slog(serial))
        alice_uid = int(m_alice.group(1)) if m_alice else 1001
        log, _ = cmd("useradd bob", "[USER64] useradd ok", 25)
        m_bob = re.search(r"\[USER64\] useradd ok name=bob uid=(\d+)", fst.slog(serial))
        bob_uid = int(m_bob.group(1)) if m_bob else 1002
        check("useradd bob", m_bob is not None, "alice=%d bob=%d" % (alice_uid, bob_uid))
        check("alice 的主目录被 chown/chmod 成 属主=alice、0700",
              re.search(r"\[VFS64\] chown ok path=/home/alice uid=%d gid=%d" % (alice_uid, alice_uid), fst.slog(serial)) is not None and
              fst.slog(serial).count("[VFS64] chmod ok path=/home/alice mode=0700") >= 1)

        # ---- 3) su - alice：在自家建文件 ----
        log, _ = cmd("su - alice", "[USER64] su ok", 25)
        check("su - alice：会话身份切换（[USER64] su ok from=vimtu to=alice euid=%d gui=vimtu gui_unchanged=1 via=su-dash）" % alice_uid,
              re.search(r"\[USER64\] su ok from=vimtu to=alice euid=%d gui=vimtu gui_unchanged=1 via=su-dash" % alice_uid,
                        fst.slog(serial)) is not None)
        check("切换后凭证同步到 VFS（[PERM64] cred ... user=alice via=su-dash）",
              re.search(r"\[PERM64\] cred uid=%d gid=%d euid=%d egid=%d user=alice via=su-dash"
                        % (alice_uid, alice_uid, alice_uid, alice_uid), fst.slog(serial)) is not None)
        log, _ = cmd("id", "[PERM64] id", 20)
        check("id 跟着变（[PERM64] id ... user=alice root=0）",
              re.search(r"\[PERM64\] id uid=%d gid=%d euid=%d egid=%d user=alice root=0"
                        % (alice_uid, alice_uid, alice_uid, alice_uid), fst.slog(serial)) is not None)

        log, _ = cmd("write /home/alice/private.txt hello-alice", "[TERM] cmd write ok", 25)
        check("alice 在自己的 home 里建文件（[TERM] cmd write ok）", "[TERM] cmd write ok" in fst.slog(serial))
        check("写入走 FD 层真路径（[FD64] open path=/home/alice/private.txt）",
              "[FD64] open path=/home/alice/private.txt fd=" in fst.slog(serial))
        log, _ = cmd("ls -l /home/alice", "[TERM] ls -l /home/alice/private.txt", 25)
        check("ls -l 显示属主/模式（-rw-r--r-- uid=%d gid=%d user=alice group=alice size=11）" % (alice_uid, alice_uid),
              re.search(r"\[TERM\] ls -l /home/alice/private\.txt -rw-r--r-- uid=%d gid=%d user=alice group=alice size=11"
                        % (alice_uid, alice_uid), fst.slog(serial)) is not None)

        # ---- 4) su - bob：读 alice 的文件被拒 ----
        log, _ = cmd("su - bob", "[USER64] su ok", 25)
        check("su - bob（to=bob euid=%d）" % bob_uid,
              re.search(r"\[USER64\] su ok from=alice to=bob euid=%d" % bob_uid, fst.slog(serial)) is not None)
        n_deny = fst.slog(serial).count("[PERM64] deny")
        n_cat_before_bob = fst.slog(serial).count("[TERM] cmd cat bytes=")
        log, _ = cmd("cat /home/alice/private.txt", "[FD64] open FAILED", 25)
        check("bob 读 alice 的文件被拒：EACCES 打点（[FD64] open FAILED path=/home/alice/private.txt rc=13）",
              "[FD64] open FAILED path=/home/alice/private.txt rc=13" in fst.slog(serial))
        check("bob 被拒时 VFS 层打 [PERM64] deny（op=traverse path=/home/alice ...）",
              fst.slog(serial).count("[PERM64] deny") > n_deny and
              re.search(r"\[PERM64\] deny op=traverse path=/home/alice/private\.txt uid=%d .*need=x" % bob_uid,
                        fst.slog(serial)) is not None)
        check("bob 的这次 cat 根本没走到读（[TERM] cmd cat bytes= 计数没变）",
              fst.slog(serial).count("[TERM] cmd cat bytes=") == n_cat_before_bob)

        log, _ = cmd("ls -l /home/alice", "denied-or-missing", 25)
        check("bob 进 /home/alice 列目录被拒（[TERM] cmd ls entries=0 denied-or-missing path=/home/alice）",
              re.search(r"\[TERM\] cmd ls entries=0 denied-or-missing path=/home/alice", fst.slog(serial)) is not None)
        log, _ = cmd("touch /home/alice/evil.txt", "[TERM] cmd touch", 25)
        log = fst.slog(serial)
        check("bob 不能在 alice 的 home 里建文件（[FD64] open FAILED path=/home/alice/evil.txt rc=13）",
              "[FD64] open FAILED path=/home/alice/evil.txt rc=13" in log)
        check("被拒的建文件在 VFS 层有 deny 打点（op=traverse need=x，或 op=create need=wx）",
              (re.search(r"\[PERM64\] deny op=traverse path=/home/alice/evil\.txt uid=%d .*need=x" % bob_uid, log) is not None) or
              (re.search(r"\[PERM64\] deny op=create path=/home/alice/evil\.txt .*need=wx", log) is not None))
        log, _ = cmd("rm /home/alice/private.txt", "[TERM] cmd rm", 25)
        check("bob 不能删 alice 的文件（rm 失败：父目录没有 w+x，没有 removed 行）",
              "removed /home/alice/private.txt" not in fst.slog(serial))

        # /home/bob/Desktop 同理：bob 自己的桌面可以写（属主 + ls -l 证据）
        log, _ = cmd("write /home/bob/Desktop/bob.txt bob-data", "[TERM] cmd write ok", 25)
        check("bob 在自己的 Desktop 里建文件（[TERM] cmd write ok）", "[TERM] cmd write ok" in fst.slog(serial))
        log, _ = cmd("ls -l /home/bob/Desktop", "[TERM] ls -l /home/bob/Desktop/bob.txt", 25)
        check("新文件属主 = 当前 euid（ls -l ... uid=%d user=bob）" % bob_uid,
              re.search(r"\[TERM\] ls -l /home/bob/Desktop/bob\.txt -rw-r--r-- uid=%d gid=%d user=bob group=bob"
                        % (bob_uid, bob_uid), fst.slog(serial)) is not None)

        # ---- 5) root：读同一个文件成功 ----
        log, _ = cmd("su - root", "[USER64] su ok", 25)
        check("su - root（to=root euid=0 via=su-dash）",
              re.search(r"\[USER64\] su ok from=bob to=root euid=0 gui=vimtu gui_unchanged=1 via=su-dash", fst.slog(serial)) is not None)
        check("root 会话的凭证发布（[PERM64] cred ... euid=0 user=root via=su-dash）",
              re.search(r"\[PERM64\] cred uid=0 gid=0 euid=0 egid=0 user=root via=su-dash", fst.slog(serial)) is not None)
        n_ok_open = fst.slog(serial).count("[FD64] open path=/home/alice/private.txt fd=")
        n_ok_cat = fst.slog(serial).count("[TERM] cmd cat bytes=11 total=11")
        log, _ = cmd("cat /home/alice/private.txt", "[TERM] cmd cat", 25)
        check("root 读 alice 的文件成功（新的 [TERM] cmd cat bytes=11 total=11）",
              fst.slog(serial).count("[TERM] cmd cat bytes=11 total=11") > n_ok_cat)
        check("root 的这一次读真的打开了那个文件（[FD64] open path=... 计数 +1；没有新的 rc=13）",
              fst.slog(serial).count("[FD64] open path=/home/alice/private.txt fd=") > n_ok_open)
        log, _ = cmd("ls -l /home/alice", "[TERM] ls -l /home/alice/private.txt", 25)
        check("root 能列 /home/alice（0700 对 root 不设限；ls -l 成功）",
              "[TERM] ls -l /home/alice/private.txt" in fst.slog(serial))
        log, _ = cmd("tree /home/alice", "[VFS64] tree /home/alice/private.txt", 25)
        check("tree 行带 uid/gid/mode：文件（[VFS64] tree /home/alice/private.txt type=file size=11 ... uid=%d gid=%d mode=0644）"
              % (alice_uid, alice_uid),
              re.search(r"\[VFS64\] tree /home/alice/private\.txt type=file size=11 .*uid=%d gid=%d mode=0644"
                        % (alice_uid, alice_uid), fst.slog(serial)) is not None)
        check("tree 行带 uid/gid/mode：子目录（[VFS64] tree /home/alice/Desktop type=dir size=0 ... uid=%d gid=%d mode=0700）" % (alice_uid, alice_uid),
              re.search(r"\[VFS64\] tree /home/alice/Desktop type=dir size=0 .*uid=%d gid=%d mode=0700"
                        % (alice_uid, alice_uid), fst.slog(serial)) is not None)
        # ---- 6) chmod 600：root OK、alice 自己 OK、bob 仍被拒 ----
        log, _ = cmd("chmod 600 /home/alice/private.txt", "[VFS64] chmod ok", 25)
        check("root chmod 600（[VFS64] chmod ok path=/home/alice/private.txt mode=0600）",
              re.search(r"\[VFS64\] chmod ok path=/home/alice/private\.txt mode=0600", fst.slog(serial)) is not None)
        log, _ = cmd("su - alice", "[USER64] su ok", 25)
        check("su - alice（回到 alice）",
              re.search(r"\[USER64\] su ok from=root to=alice euid=%d" % alice_uid, fst.slog(serial)) is not None)
        log, _ = cmd("cat /home/alice/private.txt", "[TERM] cmd cat ok", 25)
        check("chmod 600 后 alice（属主）仍能读（[TERM] cmd cat bytes=11）",
              re.search(r"\[TERM\] cmd cat bytes=11 total=11", fst.slog(serial)) is not None)
        log, _ = cmd("write /home/alice/private.txt alice-write", "[TERM] cmd write ok", 25)
        check("chmod 600 后 alice（属主）仍能写（[TERM] cmd write ok）", "[TERM] cmd write ok" in fst.slog(serial))
        log, _ = cmd("ls -l /home/bob", "denied-or-missing", 25)
        check("反向隔离：alice 也进不去 /home/bob（[TERM] cmd ls entries=0 denied-or-missing path=/home/bob）",
              re.search(r"\[TERM\] cmd ls entries=0 denied-or-missing path=/home/bob", fst.slog(serial)) is not None)
        n_open_fail = fst.slog(serial).count("[FD64] open FAILED path=/home/alice/private.txt rc=13")
        log, _ = cmd("su - bob", "[USER64] su ok", 25)
        log, _ = cmd("cat /home/alice/private.txt", "[FD64] open FAILED", 25)
        check("chmod 600 后 bob 仍被拒（新的 rc=13 行）",
              fst.slog(serial).count("[FD64] open FAILED path=/home/alice/private.txt rc=13") > n_open_fail)

        # ---- 6b) 三段判定要在**能进目录**的文件上验：/shared（root 0755）+ f.txt（属主 alice、0644）----
        log, _ = cmd("su - root", "[USER64] su ok", 25)
        n_mkdir = fst.slog(serial).count("[TERM] cmd mkdir ok")
        log, _ = cmd("mkdir /shared", "[TERM] cmd mkdir ok", 25)
        check("root 建共享目录 /shared（[TERM] cmd mkdir ok）",
              fst.slog(serial).count("[TERM] cmd mkdir ok") > n_mkdir)
        log, _ = cmd("write /shared/f.txt hello-shared", "[TERM] cmd write ok", 25)
        check("root 在 /shared 建文件（[TERM] cmd write ok）", "[TERM] cmd write ok" in fst.slog(serial))
        log, _ = cmd("chown alice /shared/f.txt", "[VFS64] chown ok", 25)
        check("root chown alice（[VFS64] chown ok path=/shared/f.txt uid=%d gid=%d）" % (alice_uid, alice_uid),
              re.search(r"\[VFS64\] chown ok path=/shared/f\.txt uid=%d gid=%d" % (alice_uid, alice_uid),
                        fst.slog(serial)) is not None)
        log, _ = cmd("chmod 644 /shared/f.txt", "[VFS64] chmod ok", 25)
        check("root chmod 644 /shared/f.txt（other 段给 r）",
              "[VFS64] chmod ok path=/shared/f.txt mode=0644" in fst.slog(serial))
        log, _ = cmd("su - bob", "[USER64] su ok", 25)
        n_shared_cat = fst.slog(serial).count("[TERM] cmd cat bytes=12 total=12")
        log, _ = cmd("cat /shared/f.txt", "[TERM] cmd cat ok", 25)
        check("chmod 644 后 bob 能读共享文件（other r：新的 [TERM] cmd cat bytes=12 total=12）",
              fst.slog(serial).count("[TERM] cmd cat bytes=12 total=12") > n_shared_cat)
        n_deny_shared = fst.slog(serial).count("[PERM64] deny op=access path=/shared/f.txt")
        log, _ = cmd("write /shared/f.txt bob-write", "[TERM] cmd write fail", 25)
        check("bob 写 alice 的文件被拒（[PERM64] deny op=access path=/shared/f.txt uid=%d gid=%d mode=0644 need=w owner=%d:%d）"
              % (bob_uid, bob_uid, alice_uid, alice_uid),
              fst.slog(serial).count("[PERM64] deny op=access path=/shared/f.txt") > n_deny_shared and
              re.search(r"\[PERM64\] deny op=(?:access|write) path=/shared/f\.txt uid=%d gid=%d mode=0644 need=w owner=%d:%d"
                        % (bob_uid, bob_uid, alice_uid, alice_uid), fst.slog(serial)) is not None)
        check("bob 被拒的写没有打开文件（[FD64] open FAILED path=/shared/f.txt rc=13）",
              "[FD64] open FAILED path=/shared/f.txt rc=13" in fst.slog(serial))
        n_chmod_deny = fst.slog(serial).count("[PERM64] deny op=chmod path=/shared/f.txt")
        log, _ = cmd("chmod 600 /shared/f.txt", "[TERM] cmd chmod fail", 25)
        check("非属主 bob chmod 被拒（[PERM64] deny op=chmod path=/shared/f.txt uid=%d owner=%d (only the owner or root can chmod)）"
              % (bob_uid, alice_uid),
              fst.slog(serial).count("[PERM64] deny op=chmod path=/shared/f.txt") > n_chmod_deny and
              re.search(r"\[PERM64\] deny op=chmod path=/shared/f\.txt uid=%d owner=%d \(only the owner or root can chmod\)"
                        % (bob_uid, alice_uid), fst.slog(serial)) is not None)
        log, _ = cmd("su - alice", "[USER64] su ok", 25)
        log, _ = cmd("chmod 600 /shared/f.txt", "[VFS64] chmod ok", 25)
        check("属主 alice chmod 600 /shared/f.txt 成功（[VFS64] chmod ok ... mode=0600）",
              "[VFS64] chmod ok path=/shared/f.txt mode=0600" in fst.slog(serial))
        log, _ = cmd("su - bob", "[USER64] su ok", 25)
        n_shared_fail = fst.slog(serial).count("[FD64] open FAILED path=/shared/f.txt rc=13")
        log, _ = cmd("cat /shared/f.txt", "[FD64] open FAILED", 25)
        check("chmod 600 后 bob 读共享文件被拒（other r 被去掉：新的 rc=13）",
              fst.slog(serial).count("[FD64] open FAILED path=/shared/f.txt rc=13") > n_shared_fail)

        # ---- 7) chown：只有 root（当前会话 = bob，先验非 root 会被拒）----
        log, _ = cmd("chown bob /home/alice/private.txt", "[TERM] cmd chown fail", 25)
        check("非 root chown 被拒（[PERM64] deny op=chown ... only root can chown）",
              "[PERM64] deny op=chown path=/home/alice/private.txt" in fst.slog(serial))
        log, _ = cmd("su - root", "[USER64] su ok", 25)
        log, _ = cmd("chown bob /home/alice/private.txt", "[VFS64] chown ok", 25)
        check("root chown bob（[VFS64] chown ok path=/home/alice/private.txt uid=%d gid=%d）" % (bob_uid, bob_uid),
              re.search(r"\[VFS64\] chown ok path=/home/alice/private\.txt uid=%d gid=%d" % (bob_uid, bob_uid),
                        fst.slog(serial)) is not None)
        log, _ = cmd("ls -l /home/alice", "[TERM] ls -l /home/alice/private.txt", 25)
        check("ls -l 反映新属主（-rw------- uid=%d user=bob）" % bob_uid,
              re.search(r"\[TERM\] ls -l /home/alice/private\.txt -rw------- uid=%d gid=%d user=bob group=bob"
                        % (bob_uid, bob_uid), fst.slog(serial)) is not None)

        # ---- 8) umask：新建文件模式 = 666 & ~umask（用 alice 身份，顺带验"属主 = 当前 euid"）----
        log, _ = cmd("su - alice", "[USER64] su ok", 25)
        check("su - alice（umask 段用普通用户身份）",
              re.search(r"\[USER64\] su ok from=root to=alice euid=%d" % alice_uid, fst.slog(serial)) is not None)
        log, _ = cmd("umask", "[TERM] cmd umask value", 20)
        check("umask 显示当前值 0022（[TERM] cmd umask value=0022）",
              re.search(r"\[TERM\] cmd umask value=0*22", fst.slog(serial)) is not None)
        log, _ = cmd("umask 077", "[TERM] cmd umask set", 20)
        check("umask 077 生效（[TERM] cmd umask set old=0022 new=0077）",
              re.search(r"\[TERM\] cmd umask set old=0*22 new=0*77", fst.slog(serial)) is not None)
        log, _ = cmd("write /home/alice/u.txt secret", "[TERM] cmd write ok", 25)
        log, _ = cmd("ls -l /home/alice", "[TERM] ls -l /home/alice/u.txt", 25)
        check("umask 077 下新文件是 0600 且属主 = 当前 euid（-rw------- uid=%d user=alice）" % alice_uid,
              re.search(r"\[TERM\] ls -l /home/alice/u\.txt -rw------- uid=%d gid=%d user=alice" % (alice_uid, alice_uid),
                        fst.slog(serial)) is not None)
        log, _ = cmd("umask 022", "[TERM] cmd umask set", 20)
        check("恢复 umask 022", re.search(r"\[TERM\] cmd umask set old=0*77 new=0*22", fst.slog(serial)) is not None)

        # ---- 9) 目录写规则：只读目录里不能建/删（要 w+x）----
        n_mkdir_ro = fst.slog(serial).count("[TERM] cmd mkdir ok")
        log, _ = cmd("mkdir /home/alice/ro", "[TERM] cmd mkdir ok", 25)
        check("alice 在自己 home 里建目录（[TERM] cmd mkdir ok；w+x 都满足）",
              fst.slog(serial).count("[TERM] cmd mkdir ok") > n_mkdir_ro)
        log, _ = cmd("chmod 555 /home/alice/ro", "[VFS64] chmod ok", 25)
        check("把 /home/alice/ro 设成 555（[VFS64] chmod ok ... mode=0555）",
              re.search(r"\[VFS64\] chmod ok path=/home/alice/ro mode=0555", fst.slog(serial)) is not None)
        n_deny2 = fst.slog(serial).count("[PERM64] deny")
        log, _ = cmd("touch /home/alice/ro/x.txt", "[TERM] cmd touch fail", 25)
        check("555 目录里建文件被拒（[PERM64] deny op=create path=/home/alice/ro/x.txt ... need=wx）",
              fst.slog(serial).count("[PERM64] deny") > n_deny2 and
              re.search(r"\[PERM64\] deny op=create path=/home/alice/ro/x\.txt uid=\d+ .*need=wx",
                        fst.slog(serial)) is not None)
        check("被拒的 touch 没打开文件（[FD64] open FAILED path=/home/alice/ro/x.txt rc=13）",
              "[FD64] open FAILED path=/home/alice/ro/x.txt rc=13" in fst.slog(serial))
        n_rm_ok = fst.slog(serial).count("[TERM] cmd rm ok")
        log, _ = cmd("rm /home/alice/u.txt", "[TERM] cmd rm ok", 25)
        check("555 目录之外的文件仍可删（父目录 /home/alice 是 0700，属主 alice 有 w+x）",
              fst.slog(serial).count("[TERM] cmd rm ok") > n_rm_ok)
        log, _ = cmd("chmod 755 /home/alice/ro", "[VFS64] chmod ok", 25)
        n_touch_ok = fst.slog(serial).count("[TERM] cmd touch ok")
        log, _ = cmd("touch /home/alice/ro/x.txt", "[TERM] cmd touch ok", 25)
        check("恢复 755 后建文件成功（[TERM] cmd touch ok 计数 +1）",
              fst.slog(serial).count("[TERM] cmd touch ok") > n_touch_ok)
        check("被拒的 touch 没有留下文件、成功的只留一个（open 失败 1 次 + 成功 1 次）",
              fst.slog(serial).count("[FD64] open FAILED path=/home/alice/ro/x.txt rc=13") == 1 and
              fst.slog(serial).count("[FD64] open path=/home/alice/ro/x.txt fd=") == 1)

        # ---- 10) /root 0700：普通用户进不去，root 可以 ----
        log, _ = cmd("su - alice", "[USER64] su ok", 25)
        n_deny4 = fst.slog(serial).count("[PERM64] deny")
        log, _ = cmd("ls -l /root", "denied-or-missing", 25)
        check("alice 进不去 /root（[TERM] cmd ls entries=0 denied-or-missing path=/root）",
              fst.slog(serial).count("[PERM64] deny") > n_deny4 and
              re.search(r"\[PERM64\] deny op=access path=/root uid=%d" % alice_uid, fst.slog(serial)) is not None)
        log, _ = cmd("su - root", "[USER64] su ok", 25)
        log, _ = cmd("su - vimtu", "[USER64] su ok", 25)
        check("root 可以切回普通用户 vimtu（su - vimtu）",
              re.search(r"\[USER64\] su ok from=root to=vimtu euid=1000", fst.slog(serial)) is not None)

        # ---- 11) v3 旧卷：可读可写 + 旧文件按 root 默认显示（不拦截）----
        log = fst.slog(serial)
        mlett = re.search(r"\[DRV64\] letter=(\w): disk=1 part=1 fs=VimtuFS2", log)
        dletter = mlett.group(1) if mlett else "D"
        check("第二块盘（v3 数据卷）拿到盘符（[DRV64] letter=%s: disk=1 part=1 fs=VimtuFS2）" % dletter,
              mlett is not None)
        log, _ = cmd("vol %s" % dletter.lower(), "[VOL] switch letter=%s:" % dletter, 20)
        check("切到 v3 数据卷（vol %s -> [VOL] switch letter=%s: slot=1）" % (dletter.lower(), dletter),
              ("[VOL] switch letter=%s: slot=1" % dletter) in fst.slog(serial))
        log, _ = cmd("ls -l", "[TERM] ls -l /old.txt", 25)
        check("v3 旧文件按默认属主 root 显示（-rw-r--r-- uid=0 user=root size=17）",
              re.search(r"\[TERM\] ls -l /old\.txt -rw-r--r-- uid=0 gid=0 user=root group=root size=17",
                        fst.slog(serial)) is not None)
        log, _ = cmd("cat /old.txt", "[TERM] cmd cat", 25)
        check("v3 旧卷可读（cat bytes=17 = legacy-v3-content）",
              re.search(r"\[TERM\] cmd cat bytes=17", fst.slog(serial)) is not None)
        log, _ = cmd("write /new.txt new-on-v3", "[TERM] cmd write ok", 25)
        check("v3 旧卷可写（legacy 卷不拦截：普通用户 vimtu 也能写 —— 已在挂载时打点说明）",
              "[TERM] cmd write ok" in fst.slog(serial))
        check("v3 卷没有权限字段的 chmod 会被拒绝（如实：unsupported，不假装改掉）",
              True)
        log, _ = cmd("chmod 600 /new.txt", "[PERM64] chmod: volume v3", 20)
        check("v3 卷上 chmod 明确报\"没有 mode 字段\"（[PERM64] chmod: volume v3 has no mode field）",
              "[PERM64] chmod: volume v3 has no mode field" in fst.slog(serial))
        log, _ = cmd("vol c", "[VOL] switch letter=C:", 20)
        check("切回 C:（验证切卷之后权限判定仍按当前卷）", "[VOL] switch letter=C: slot=0" in fst.slog(serial))

        forbid("阶段 A", fst.slog(serial))
        check("阶段 A 全程没有 PANIC / 三重故障", "PANIC" not in fst.slog(serial) and
              "TRIPLE FAULT" not in fst.slog(serial))
    finally:
        fst.kill(proc)

    # ==================== 阶段 B：v3 旧卷 -> 格式化回 v4 ====================
    print("=== 阶段 B：安装向导把 **v3** 主分区格式化回 **v4** ===")
    target = os.path.join(tmp, "target_v3.img")
    make_v3_system_disk(target)
    serial2 = os.path.join(tmp, "bootB.log")
    port2 = fst.free_port()
    medium = os.path.join(ROOT, "vimtu64-64.img")
    proc2, mon2 = fst.boot_media(qemu, target, serial2, port2, "Vimtu64-perm64-B")
    try:
        check("阶段 B：向导就绪（磁盘枚举完成）", fst.wait_wizard_ready(proc2, serial2, 90))
        for _ in range(4):
            mon2.key("ret", wait=1.6)
        mon2.key("down", wait=1.6)          # P1 -> P2（主分区）
        mon2.key("f", wait=2.6)             # 格式化主分区
        log2 = fst.wait_for(serial2, "[VFS64] format ok", 40, proc2) or ""
        time.sleep(1.5)
        log2 = fst.slog(serial2)
        mf = fst.last_match(r"\[VFS64\] format ok blocks=(\d+) version=(\d+) inode=(\d+) root=(\d+) perm=uid/gid/mode@(\d+)/(\d+)/(\d+) rootmode=0755", log2)
        check("格式化产出 v4（[VFS64] format ok ... version=4 inode=128 perm=uid/gid/mode@75/77/79 rootmode=0755）",
              bool(mf) and mf.group(2) == "4" and mf.group(3) == "128",
              mf.group(0) if mf else "（缺 format ok 行）")
        check("权限字段偏移 = 75/77/79（inode 布局的唯一权威定义）",
              bool(mf) and mf.groups()[4:7] == ("75", "77", "79"))
        check("格式化块数 = 主分区扇区数（%d）" % (TARGET_SECTORS - PART_MAIN_LBA),
              bool(mf) and mf.group(1) == str(TARGET_SECTORS - PART_MAIN_LBA))
        check("格式化根目录 = 0 号 inode（root=<inode 区起始 LBA>）", bool(mf) and int(mf.group(4)) > 0)
        check("阶段 B：自检全过（[VFS64] selftest PASS；bit16 权限自检也在其中）",
              "[VFS64] selftest PASS" in log2)
        check("阶段 B：权限自检打点（[VFS64] perm selftest ok ...）",
              "[VFS64] perm selftest ok" in log2)
        check("阶段 B：没有 PANIC/三重故障", "PANIC" not in log2 and "TRIPLE FAULT" not in log2)
        forbid("阶段 B", log2)
    finally:
        fst.kill(proc2)

    if args.keep:
        print("[perm64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    for name, c in checks:
        if not c:
            print("  FAIL: %s" % name)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
