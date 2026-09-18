#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/store64_test.py - 设置持久化 store 端到端验收（终端命令 -> VimtuFS2 /store.a|b -> 冷启动读回）

被验的缺陷：kernel/memlayout64.h 的裸盘 store 保留区 LBA 8009..8072 与安装程序建的数据分区
（起点 LBA 8009）**完全重叠**。修完之后：store 的槽优先放在文件系统里（/store.a、/store.b，
各 16KB = 一个槽），裸盘槽只作为"没有可用卷"时的降级路径并打醒目 WARN。

做什么（三段，全部无头 QEMU + -serial file: + [PASS]/[FAIL] + === RESULT: ===）：
  1) 造一块"已装好系统 + 主分区已格式化"的 16MB 测试盘（与 tests/app64_test.py 同款夹具：
     build64/system.img 字节 + 标准 MBR + Python 复刻的 VimtuFS2 空卷 @LBA 8009）；
  2) 第一遍启动：QEMU monitor 注入按键（Win 键 -> 开始菜单 -> 数字 1 = 终端）打开终端，
     敲 `store set theme dark` / `store flush` / `store dump`，断言串口出现
     [STORE64] cmd set/flush/dump、[STORE64] flush via=vfs -> slot=... ok、[STORE64] selftest PASS；
     并且**宿主 Python 直接解析这块 raw 镜像的 VimtuFS2**：/store.a|/store.b 存在、16KB、
     VSTORE64 头部 CRC + payload CRC 正确、KV 里有 theme=dark（绕过 GUI 的字节级证据）；
  3) 第二遍**冷启动同一块镜像**：断言 [STORE64] init via=vfs ... 与 dump 里的 theme=dark
     —— 这就证明"设置真的跨重启存下来了"；
  4) 第三遍（只读对照）：启动**没有分区表/没有卷**的 build64/system.img，断言降级路径打印
     [STORE64] WARN raw slot area LBA 8009 overlaps the data partition; use VFS-backed store
     且 init via=raw —— 证明这条警告只在"无卷降级"时出现（正常路径两遍都不许出现）。
禁止出现：PANIC / TRIPLE FAULT / FAILED mask=。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\store64_test.py
    py -3 tests\\store64_test.py --timeout 150 --keep
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "store64_test.img")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS     # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                              # 16MB

STORE_SLOT_BYTES = 16384
STORE_MAGIC = b"VSTORE64"
STORE_TMP = "st64.tmp"

# 正常路径必须出现
MUST1 = [
    ("[VFS64] mount ok",                                   "主分区挂载成 VimtuFS2 卷"),
    ("[STORE64] selftest PASS",                            "store 自检全过（含真卷 16KB 往返）"),
    ("[STORE64] selftest vfs probe ok",                     "有卷时走 vfs64_write+vfs64_read 的 16KB 往返"),
    ("[STORE64] init via=vfs slot=",                        "store 载体 = VFS 文件（/store.a|b）"),
    ("[APP] term opened",                                   "终端窗口打开（随后注入按键）"),
    ("[STORE64] cmd set key=theme value=dark rc=0",         "终端 store set 命中真实现"),
    ("[STORE64] cmd flush rc=0",                            "终端 store flush 命中真实现"),
    ("[STORE64] flush via=vfs -> slot=",                    "flush 走 VFS 载体"),
    ("[STORE64] cmd dump keys=",                            "终端 store dump 命中真实现"),
    ("[GUI64] ready",                                       "桌面照常起来"),
]

# 第二遍冷启动必须出现
MUST2 = [
    ("[STORE64] selftest PASS",                             "store 自检全过"),
    ("[GUI64] ready",                                       "桌面照常起来"),
]

# 第三遍（无卷）必须出现：降级警告
MUST3 = [
    ("[STORE64] WARN raw slot area LBA 8009 overlaps the data partition; use VFS-backed store",
     "无卷时大声报警（裸盘槽区与数据分区重叠）"),
    ("[STORE64] init via=raw slot=none gen=0 keys=0",       "无卷时降级到裸盘槽区（空 store）"),
    ("[GUI64] ready",                                       "桌面照常起来"),
]

WARN_RAW = "[STORE64] WARN raw slot area LBA 8009 overlaps the data partition; use VFS-backed store"

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "[STORE64] flush FAILED"]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# 夹具：一块"已装好系统 + 主分区已格式化"的盘（与 tests/app64_test.py 同一套规则）
# ---------------------------------------------------------------------------
def _vimtufs2_format(buf, start_lba, total_sectors):
    bmn = (total_sectors + 4095) // 4096
    inodes = max(16, min(256, total_sectors // 64))
    ino_blocks = (inodes + 7) // 8
    bm_start = 1
    ino_start = bm_start + bmn
    data_start = ino_start + ino_blocks
    data_blocks = total_sectors - data_start
    if data_blocks <= 0:
        raise RuntimeError("fixture volume too small")

    sb = bytearray(SECTOR)
    sb[0:8] = b"VIMTUFS2"
    struct.pack_into("<I", sb, 8, 2)
    struct.pack_into("<I", sb, 12, SECTOR)
    struct.pack_into("<I", sb, 16, SECTOR)
    struct.pack_into("<I", sb, 20, total_sectors)
    struct.pack_into("<I", sb, 24, 0)
    struct.pack_into("<I", sb, 28, bm_start)
    struct.pack_into("<I", sb, 32, bmn)
    struct.pack_into("<I", sb, 36, ino_start)
    struct.pack_into("<I", sb, 40, inodes)
    struct.pack_into("<I", sb, 44, 64)
    struct.pack_into("<I", sb, 48, data_start)
    struct.pack_into("<I", sb, 52, data_blocks)
    struct.pack_into("<I", sb, 56, 0)
    struct.pack_into("<I", sb, 60, _crc32(sb[:60]))
    sb[510], sb[511] = 0x55, 0xAA

    base = start_lba * SECTOR
    buf[base:base + SECTOR] = sb

    for m in range(bmn):
        bm = bytearray(SECTOR)
        for k in range(4096):
            blk = m * 4096 + k
            if blk >= total_sectors or blk < data_start:
                bm[k >> 3] |= 1 << (k & 7)
        off = base + (bm_start + m) * SECTOR
        buf[off:off + SECTOR] = bm

    for i in range(ino_blocks):
        off = base + (ino_start + i) * SECTOR
        buf[off:off + SECTOR] = b"\0" * SECTOR

    root = bytearray(64)
    root[0] = 2
    struct.pack_into("<I", root, 28, 0)
    struct.pack_into("<I", root, 60, _crc32(root[:60]))
    off = base + ino_start * SECTOR
    buf[off:off + 64] = root


def _mbr_entry(bootable, ptype, start, sectors):
    e = bytearray(16)
    e[0] = 0x80 if bootable else 0x00
    e[1:4] = b"\xFE\xFF\xFF"
    e[4] = ptype
    e[5:8] = b"\xFE\xFF\xFF"
    struct.pack_into("<I", e, 8, start)
    struct.pack_into("<I", e, 12, sectors)
    return e


def prepare_fixture():
    if not os.path.exists(SYSTEM_IMG):
        return None
    with open(SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    if len(sys_bytes) == 0 or len(sys_bytes) % SECTOR:
        return None
    if len(sys_bytes) > TARGET_SECTORS * SECTOR:
        return None
    buf = bytearray(TARGET_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes

    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA

    _vimtufs2_format(buf, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)

    with open(FIXTURE_IMG, "wb") as f:
        f.write(buf)
    return FIXTURE_IMG


# ---------------------------------------------------------------------------
# 宿主侧直接解析 raw 镜像：VimtuFS2 根目录 + VSTORE64 槽（绕过 GUI 的字节级证据）
# ---------------------------------------------------------------------------
def read_vimtufs2_files(path):
    with open(path, "rb") as f:
        img = f.read()
    base = PART_MAIN_LBA * SECTOR
    sb = img[base:base + SECTOR]
    if len(sb) < SECTOR or sb[0:8] != b"VIMTUFS2":
        return None
    ino_start = struct.unpack_from("<I", sb, 36)[0]
    ino_count = struct.unpack_from("<I", sb, 40)[0]
    files = {}
    for i in range(ino_count):
        off = base + (ino_start + i // 8) * SECTOR + (i % 8) * 64
        ino = img[off:off + 64]
        if len(ino) < 64:
            break
        typ = ino[0]
        if typ not in (1, 2):
            continue
        if struct.unpack_from("<I", ino, 60)[0] != _crc32(ino[:60]):
            continue                      # inode CRC 不符 -> 跳过（坏项）
        nl = ino[1]
        name = ino[32:32 + nl].decode("ascii", "replace")
        size = struct.unpack_from("<I", ino, 4)[0]
        blocks = [struct.unpack_from("<I", ino, 8 + 4 * k)[0] for k in range(4)]
        ind = struct.unpack_from("<I", ino, 24)[0]
        if ind:
            indoff = base + ind * SECTOR
            for k in range(128):
                b = struct.unpack_from("<I", img, indoff + 4 * k)[0]
                if b:
                    blocks.append(b)
        data = bytearray()
        for b in blocks:
            if not b:
                break
            data += img[base + b * SECTOR: base + b * SECTOR + SECTOR]
            if len(data) >= size:
                break
        files[name] = {"type": typ, "size": size, "data": bytes(data[:size])}
    return files


def parse_store_slot(b):
    """解析一个 VSTORE64 槽镜像（与小端格式/store64.cpp 的偏移一一对应）。"""
    if len(b) != STORE_SLOT_BYTES:
        return {"ok": False, "why": "size=%d" % len(b)}
    magic = b[0:8]
    ver = struct.unpack_from("<I", b, 8)[0]
    gen = struct.unpack_from("<Q", b, 16)[0]
    plen = struct.unpack_from("<I", b, 24)[0]
    pcrc = struct.unpack_from("<I", b, 28)[0]
    hcrc = struct.unpack_from("<I", b, 32)[0]
    count = struct.unpack_from("<I", b, 36)[0]
    header_ok = (hcrc == _crc32(b[0:32]))
    payload_ok = (plen <= STORE_SLOT_BYTES - 64) and (pcrc == _crc32(b[64:64 + plen]))
    kv = {}
    off, end = 64, 64 + plen
    while off + 2 <= end:
        kl, vl = b[off], b[off + 1]
        off += 2
        k = b[off:off + kl].decode("ascii", "replace")
        off += kl
        v = b[off:off + vl].decode("utf-8", "replace")
        off += vl
        kv[k] = v
    return {"ok": (magic == STORE_MAGIC and ver == 1 and header_ok and payload_ok
                   and count == len(kv)),
            "magic": magic, "ver": ver, "gen": gen, "plen": plen, "count": count,
            "header_ok": header_ok, "payload_ok": payload_ok, "kv": kv}


# ---------------------------------------------------------------------------
# QEMU + monitor
# ---------------------------------------------------------------------------
class Monitor:
    """QEMU monitor 客户端（telnet 端口），与 tests/desktop64_test.py 同款。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
            s.settimeout(0.4)
            data = b""
            while True:
                try:
                    ch = s.recv(4096)
                    if not ch:
                        break
                    data += ch
                except socket.timeout:
                    break
        finally:
            s.close()
        return data

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)


KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal", "_": "shift-minus"}


class Vm:
    def __init__(self, qemu, img, logdir, tag, port, timeout):
        self.qemu = qemu
        self.img = img
        self.serial = os.path.join(logdir, tag + ".log")
        self.tag = tag
        self.port = port
        self.timeout = timeout
        self.proc = None
        if os.path.exists(self.serial):
            os.remove(self.serial)

    def start(self):
        args = [
            self.qemu, "-name", "Vimtu64-" + self.tag,
            "-drive", "format=raw,file=%s" % q(self.img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % self.port,
            "-no-reboot",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for(self, needle, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log():
                return True
            if self.proc is not None and self.proc.poll() is not None:
                return False
            time.sleep(0.4)
        return needle in self.log()

    def monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return Monitor(self.port)
            except OSError:
                time.sleep(0.25)
        return None

    def stop(self):
        if self.proc is None:
            return
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass
        self.proc = None


def type_line(mon, text, per_key=0.22):
    for ch in text:
        mon.key(KEYMAP.get(ch, ch), wait=per_key)
    mon.key("ret", wait=1.0)


def open_terminal(vm, mon, checks):
    """Win 键 -> 开始菜单，数字 1 -> 终端（与 tests/desktop64_test.py 的手法相同）。"""
    for attempt in range(3):
        before = vm.log()
        mon.key("meta_l", wait=1.2)
        if "[UI] menu open" not in vm.log():
            continue
        mon.key("1", wait=2.2)
        if "[APP] term opened" in vm.log() and "[APP] term opened" not in before.split("[UI] menu open")[-1]:
            return True
        if "[APP] term opened" in vm.log():
            return True
        time.sleep(0.6)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5597)
    ap.add_argument("--timeout", type=int, default=150, help="每遍启动的最长等待秒数")
    ap.add_argument("--keep", action="store_true", help="保留临时目录（打印路径）")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2

    img = prepare_fixture()
    if not img:
        sys.stderr.write("造测试盘失败（%s 不合法）\n" % SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_store64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 store64 acceptance（设置持久化：终端命令 -> VimtuFS2 -> 冷启动读回）===")
    print("[store64] 测试盘已生成：%s（%d 扇区，主分区 LBA %d 已格式化）"
          % (img, TARGET_SECTORS, PART_MAIN_LBA))

    # ---------------- 第一遍：注入按键敲 store 命令 ----------------
    print("[store64] 第一遍启动：打开终端 -> store set theme dark / store flush / store dump")
    vm = Vm(qemu, img, tmp, "boot1", args.port, args.timeout)
    vm.start()
    up1 = vm.wait_for("[GUI64] ready", args.timeout)
    mon = vm.monitor() if up1 else None
    typed = False
    if mon:
        typed = open_terminal(vm, mon, checks)
        if not typed:
            print("  [!] 终端没打开（meta_l + 1 注入失败）")
        else:
            for cmd, marker in (("store set theme dark", "[STORE64] cmd set key=theme value=dark"),
                                ("store flush",          "[STORE64] cmd flush rc=0"),
                                ("store get theme",      "[STORE64] cmd get key=theme value=dark"),
                                ("store dump",           "[STORE64] cmd dump keys=")):
                done = False
                for attempt in range(2):
                    type_line(mon, cmd)
                    if marker in vm.log():
                        done = True
                        break
                    time.sleep(1.0)
                if not done:
                    print("  [!] 注入命令没生效：%r" % cmd)
    time.sleep(1.0)
    log1 = vm.log()
    vm.stop()

    print("--- 第一遍：必须出现 ---")
    check("等待桌面就绪 [GUI64] ready", up1)
    for needle, what in MUST1:
        check("%s（%s）" % (needle, what), needle in log1)
    check("注入按键打开终端并敲进命令", typed)
    m = re.search(r"\[STORE64\] flush via=vfs -> slot=([AB]) gen=(\d+) crc=0x([0-9A-F]{8}) ok", log1)
    check("flush 成功行含 slot/gen/crc", bool(m), (m.group(0) if m else "未出现"))
    check("正常路径不得出现裸盘重叠警告", WARN_RAW not in log1)
    for needle in FORBIDDEN:
        check("第一遍不得出现 %s" % needle, needle not in log1)

    print("--- 第一遍：宿主侧直接解析 raw 镜像（绕过 GUI 的字节级证据）---")
    files = read_vimtufs2_files(img)
    check("镜像里能解析出 VimtuFS2 卷（magic + inode CRC 有效）", files is not None)
    files = files or {}
    names = sorted(files.keys())
    print("     卷内文件：%s" % (names if names else "(空)"))
    slot_name, slot = None, None
    for cand in ("store.a", "store.b"):
        if cand in files:
            slot_name = cand
            slot = parse_store_slot(files[cand]["data"])
            break
    check("存在 /store.a 或 /store.b 槽文件", slot is not None,
          ("命中 /%s size=%d" % (slot_name, files[slot_name]["size"])) if slot_name else "")
    if slot_name:
        check("槽文件大小 = 16384（= 一个槽）", files[slot_name]["size"] == STORE_SLOT_BYTES)
    if slot:
        check("VSTORE64 magic + 结构版本 1", slot["magic"] == STORE_MAGIC and slot["ver"] == 1)
        check("槽头部 CRC32 正确（覆盖 [0,32)）", slot["header_ok"])
        check("槽 payload CRC32 正确（覆盖记录区）", slot["payload_ok"])
        check("世代号 >= 1", slot["gen"] >= 1, "gen=%d" % slot["gen"])
        check("KV 里有 theme=dark", slot["kv"].get("theme") == "dark",
              "kv=%s" % (slot["kv"],))
    check("没有残留自检临时文件 /%s" % STORE_TMP, STORE_TMP not in files)

    # ---------------- 第二遍：冷启动同一块镜像 ----------------
    print("[store64] 第二遍启动（冷启动同一块镜像）：从盘上读回设置")
    vm = Vm(qemu, img, tmp, "boot2", args.port + 1, args.timeout)
    vm.start()
    up2 = vm.wait_for("[GUI64] ready", args.timeout)
    log2 = vm.log()
    vm.stop()

    print("--- 第二遍：必须出现 ---")
    check("等待桌面就绪 [GUI64] ready", up2)
    for needle, what in MUST2:
        check("第二遍 %s（%s）" % (needle, what), needle in log2)
    m2 = re.search(r"\[STORE64\] init via=vfs slot=([AB]) gen=(\d+) keys=(\d+)", log2)
    check("第二遍 [STORE64] init via=vfs slot=A|B gen=N keys=1", bool(m2),
          (m2.group(0) if m2 else "未出现"))
    if m2:
        check("冷启动读到 1 个键（= 第一遍存的那条）", m2.group(3) == "1")
        check("世代号沿用盘上的（>=1）", int(m2.group(2)) >= 1)
    check("第二遍 dump 里有 theme=dark（真正的跨重启持久化）",
          "[STORE64] dump theme=dark" in log2)
    check("第二遍正常路径不得出现裸盘重叠警告", WARN_RAW not in log2)
    for needle in FORBIDDEN:
        check("第二遍不得出现 %s" % needle, needle not in log2)

    # ---------------- 第三遍：无卷降级（对照）----------------
    print("[store64] 第三遍启动（build64/system.img，没有分区表/没有卷）：验证降级警告")
    vm = Vm(qemu, SYSTEM_IMG, tmp, "boot3", args.port + 2, args.timeout)
    vm.start()
    up3 = vm.wait_for("[GUI64] ready", args.timeout)
    log3 = vm.log()
    vm.stop()

    print("--- 第三遍（无卷降级）：必须出现 ---")
    check("等待桌面就绪 [GUI64] ready", up3)
    for needle, what in MUST3:
        check("%s（%s）" % (needle, what), needle in log3)
    for needle in ("PANIC", "TRIPLE FAULT", "FAILED mask="):
        check("第三遍不得出现 %s" % needle, needle not in log3)

    if args.keep:
        print("[store64] 串口日志：%s / %s / %s" % (os.path.join(tmp, "boot1.log"),
                                                    os.path.join(tmp, "boot2.log"),
                                                    os.path.join(tmp, "boot3.log")))

    # 串口原文（[STORE64] 相关行）——验收要贴的证据
    for tag, log in (("boot1", log1), ("boot2", log2), ("boot3", log3)):
        print("--- %s 的 [STORE64] 串口原文 ---" % tag)
        for line in [x.strip() for x in log.splitlines() if "[STORE64]" in x][:24]:
            print("   | " + line[:200])

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
