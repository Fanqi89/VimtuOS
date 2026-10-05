#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ehci64_test.py - ★ EHCI（USB 2.0）上的 U 盘端到端验收：枚举 -> 盘符 -> 读 -> 写 -> 热插拔

判据一句话：**关掉 QEMU 之后，宿主侧自己解析那根 U 盘镜像的 FAT32，逐字节证明盘上真的落了什么**
（读：宿主侧对同一份夹具字节算 CRC32 与客人 `fatcheck` 的结果逐位比对；写：宿主侧解析器核对新文件内容
+ 原有文件一个字节没变 + 整盘 CRC32 已变化 + 变化只出现在我们动过的扇区里）。

三个场景（各自一次真实 QEMU）：

  场景 ① **只挂 EHCI**（`-device usb-ehci,id=ehci` + `usb-storage,bus=ehci.0`，机器上没有 UHCI）：
    * 枚举：`[EHCI] pci` / `reset ok` / `async qh` / `port N reset ok speed=high ped=1` /
      `device addr=1 mps=..` / `set address=1 ok` / `config set … msc=1 ep_in=.. ep_out=..`；
    * 容量：`[USBST] inquiry` / `[USBST] capacity blocks=70000 block_size=512` / `read lba=0 count=1 ok`；
    * 盘符：`[USBST] storage attached -> rescan drive letters (usb drives=1, uhci=0 ehci=1 xhci=0)`、
      `[DRV64] letter=D: disk=24 … fs=FAT32 … ro=0`、`[FAT64] rw mount … writable=1`、`[VOL] … ro=0`；
    * 读文件（宿主侧逐字节）：终端 `fatcheck /KEEP1.TXT`（单簇 42B）与 `/KEEP2.DAT`（48 簇 24576B）
      的 CRC32 **等于宿主侧对同一份夹具字节算的 zlib CRC32**；
    * 写文件：终端 `write /ehcirw.txt …` -> `[FAT64] rw write … verify=1` + `[USBST] write verify … ok`，
      再 `fatcheck` 读回同一 CRC；**关 QEMU 后**宿主侧解析镜像核对（内容/尾部零/原有文件不变/CRC32 变化）；
    * 热插拔：monitor `device_del` -> `[EHCI] port N detached` + `[USBST] storage detached on EHCI ->
      rescan drive letters` + D: 从盘符表里消失；`device_add` -> `[EHCI] port N attached speed=high reset ok`
      + 重新枚举/探测 + `[DRV64] letter=D: … ro=0` 回来；**插回来之后再写一次**证明热插拔后的盘真能用。

  场景 ② **companion**（`-device ich9-usb-ehci1,id=ehci` + 伴随 `ich9-usb-uhci1,masterbus=ehci.0,firstport=0`）：
    * U 盘（High-speed）落在 **EHCI 的 port 1**（QEMU 里 HS 设备由 EHCI 持有 port；FS/LS 才交给伴随控制器）
      —— 落点写在 QEMU 命令行注释里，测试也断言 `[EHCI] port 1 reset ok speed=high ped=1`；
    * 伴随端口不被本驱动接管（PortOwner=1 的端口只打点、不抢；本驱动只做高速）；
    * 无 spurious `[EHCI] enum FAILED` / `[USB64] enum FAILED`、无 PANIC；盘符/读/写照常（fatcheck + write）。

  场景 ③ **反例（有界失败、不崩、不碰介质）**：只挂 EHCI，但把 U 盘镜像**只读挂给 QEMU**（`readonly=on`）：
    * 越界 LBA（读 + 写）都被**在发任何 SCSI 命令之前**拒绝：`[USBST] read-bounds probe … rejected=1 …`
      + `[USBST] write-bounds probe … rejected=1 …`；而 `[USBST] selftest PASS mask=0 (EHCI)` 不把"正确拒绝"
      当失败（P8b 那条教训：探针被拒是**预期**，不是失败）；
    * 探针之后设备照常可用（`fatcheck` 仍然对得上）= 有界失败没把设备打坏；
    * 设备侧拒绝写（协议层"坏状态/坏帧"的等价形式：CSW 报错）：`write …` ->
      `[USBST] write FAILED … reason=csw status`，终端如实报失败；**不出现** `[USBST] write verify FAILED`
      （不是"静默写坏"）；
    * 关 QEMU 后：整盘镜像**一个字节都没变**（CRC32 与启动前相等）—— 被拒绝的写真的没碰介质。

如实说明（这一版没做到 / 造不出来的，别误会）：
  * EHCI 上的 **HID 键盘没做**（中断传输）：键盘由 UHCI（kernel/usb64.cpp）/ xHCI（kernel/xhci64.cpp）覆盖；
  * 只接管 **High-speed** 端口：FS/LS 设备在 EHCI 端口上会被写 PortOwner=1 交还伴随控制器；
  * QEMU 的 EHCI **没有**"注入坏 CRC/坏帧"的开关，所以场景 ③ 的"坏帧被拒"用的是**等价的可注入形式**：
    设备侧拒写（CSW 报错）+ 越界探针 + 热插拔期间的有界失败。真正的 USB 事务 CRC 错误只有真实硬件能造，
    这一版**没有**在真机上验过。

QEMU 命令行（场景 ①；②③ 只换 -device 那一行）：
  qemu-system-x86_64.exe -name Vimtu64-ehci \\
    -drive format=raw,file=<16MB系统盘>,index=0,media=disk \\
    -device usb-ehci,id=ehci \\
    -blockdev node-name=stick,driver=file,filename=<stick.img> \\
    -device usb-storage,id=stickdev,drive=stick,bus=ehci.0 \\
    -boot order=c -m 512 -vga std -display none -serial file:<log> \\
    -monitor telnet:127.0.0.1:<port>,server,nowait -no-reboot

用法（必须用 Windows 原生 Python）：py -3 tests\\ehci64_test.py [--keep]
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

import proc64_test as p64          # noqa: E402  find_qemu / q
import fs_tree_test as fst         # noqa: E402  小系统盘夹具 + 串口工具 + 打开终端
import explorer64_test as exp      # noqa: E402  QEMU monitor（sendkey / 任意 HMP 命令）
import fatread64_test as fatr      # noqa: E402  宿主侧 FAT32 构造器（造 U 盘镜像）
import qemuhelp as qh              # noqa: E402  公共登录手势

SECTOR = 512
BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")

# ---- U 盘几何（宿主侧"独立来源"）----
STICK_PART_LBA = 2048
STICK_PART_LBA = 2048
STICK_PART_SECTORS = 70000                  # 34MB 分区（SPC=1 -> 68874 簇 >= 65525 = 真 FAT32）
STICK_SECTORS = STICK_PART_LBA + STICK_PART_SECTORS
# ★ 注意：`usb-storage` 报的是**整根镜像**的容量（含 MBR 那 2048 扇区）= 72048 扇区，
#   不是分区大小 —— READ CAPACITY(10) 的结果就是它，测试按这个数核对（别混）。
STICK_DISK_BLOCKS = STICK_SECTORS
TOTAL_CLUSTERS = 68874
STICK_TOTAL_KB = TOTAL_CLUSTERS // 2        # 卷容量 KB（68874 簇 × 512B / 1024 = 34437）

# ---- 盘上原来的文件（宿主侧造；写入的只是**新增**文件，这些一个字节都不许动）----
KEEP1 = "KEEP1.TXT"
KEEP1_B = b"keep-1: original content, must not change\n"          # 43 B，1 簇
KEEP2 = "KEEP2.DAT"
KEEP2_B = bytes((i * 7 + 11) & 0xFF for i in range(24576))         # 24576 B = 48 簇
DOCS = "docs"
DOCS_NAME = "notes.txt"
DOCS_B = b"notes: original content, must not change\n"            # 1 簇
KEEP_CLUSTERS = (1, 48, 1, 1)               # 根目录 1 簇 + KEEP1/KEEP2/docs/notes 的簇数（见 make_stick）

# ---- 客人写的内容（串口/宿主两侧都用这几个常量）----
RW_NAME = "ehcirw.txt"                      # 盘上落成 EHCIRW.TXT（8.3 大写）
RW_TEXT = b"vimtu64-ehci-rw-0123456789abcdef"        # 31 B
HOT_NAME = "ehcihot.txt"                    # 盘上落成 EHC I HOT.TXT
HOT_TEXT = b"vimtu64-ehci-after-hotplug-42"           # 29 B
WP_NAME = "ehciwp.txt"                      # 场景 ③：设备拒写（只读挂给 QEMU）
WP_TEXT = b"vimtu64-ehci-writeprotect-probe"          # 33 B

# ① / ② 的禁止项（启动到热插拔之前那一整段）：坏帧/坏状态、伪枚举失败、自检失败、崩溃
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "OOM:",
             "[EHCI] enum FAILED", "[USB64] enum FAILED",
             "[USB64] selftest FAIL", "[EHCI] selftest FAIL", "[USBST] selftest FAIL",
             "[USBST] write verify FAILED", "[FAT64] rw verify FAILED"]
# ② 的禁止项：只加"companion 上不该出现的东西"
FORBIDDEN_COMPANION = FORBIDDEN + ["[EHCI] hotplug"]


# ---------------------------------------------------------------------------
# 宿主侧：造 U 盘（MBR + 真 FAT32；**不写满** —— 要留空间给客人真写）
# ---------------------------------------------------------------------------
def put_file(dev, data):
    per = dev.spc * SECTOR
    n = max(1, (len(data) + per - 1) // per)
    c = dev.alloc(n)
    for i in range(n):
        dev.write_cluster(c + i, data[i * per:(i + 1) * per])
    return c, n


def make_stick(path):
    """造一根 35MB 的真 FAT32 U 盘：根目录 4 项（KEEP1/KEEP2/docs/FILL 之一），其余簇全空闲。"""
    dev = fatr.Fat32Builder(STICK_PART_LBA, STICK_PART_SECTORS, 1)
    root = dev.alloc(1)                                  # 簇 2 = 根目录
    ents = bytearray()

    c_keep1, _ = put_file(dev, KEEP1_B)
    ents += dev.dir_entry(KEEP1, b"KEEP1   TXT", 0x20, c_keep1, len(KEEP1_B))
    c_keep2, _ = put_file(dev, KEEP2_B)
    ents += dev.dir_entry(KEEP2, b"KEEP2   DAT", 0x20, c_keep2, len(KEEP2_B))

    c_docs = dev.alloc(1)
    c_note, _ = put_file(dev, DOCS_B)
    dz = bytearray()
    dz += dev.short_entry(b".          ", 0x10, c_docs, 0)
    dz += dev.short_entry(b"..         ", 0x10, 0, 0)
    dz += dev.dir_entry(DOCS_NAME, b"NOTES   TXT", 0x20, c_note, len(DOCS_B))
    dev.write_cluster(c_docs, bytes(dz))
    ents += dev.dir_entry(DOCS, b"DOCS       ", 0x10, c_docs, 0)

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
    return {"clusters": dev.clusters, "spc": dev.spc, "data_start": dev.data_start, "reserved": fatr.RESERVED,
            "nfats": fatr.NUM_FATS, "fatsz": dev.fatsz, "root": root, "c_keep1": c_keep1,
            "c_keep2": c_keep2, "c_docs": c_docs, "c_note": c_note, "blocks": STICK_SECTORS}


# ---------------------------------------------------------------------------
# 宿主侧：最小 FAT32 解析器（只读；独立于内核实现 —— 与 tests/usbwrite64_test.py 同款口径）
# ---------------------------------------------------------------------------
class Fat32View:
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

    def sec(self, vol_lba):
        o = self.base + vol_lba * SECTOR
        return self.img[o:o + SECTOR]

    def fat1(self, c):
        o = self.base + (self.reserved + c * 4 // SECTOR) * SECTOR + (c * 4) % SECTOR
        return struct.unpack_from("<I", self.img, o)[0] & 0x0FFFFFFF

    def fat2(self, c):
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

    def list_dir(self, cluster):
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
                if e[11] == 0x0F:
                    continue
                fc = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16)
                sz = struct.unpack_from("<I", e, 28)[0]
                out.append((bytes(e[0:11]), e[11], fc, sz, False, bytes(e)))
        return out

    def find(self, cluster, name83):
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
        ch = self.chain(first)
        if not ch:
            return b""
        last = self.cluster(ch[-1])
        off = size % self.cb
        return last[off:] if off else b""

    def free_clusters(self):
        return sum(1 for c in range(2, self.clusters + 2) if self.fat1(c) == 0)

    def fsinfo_free(self):
        return struct.unpack_from("<I", self.sec(self.fsinfo), 488)[0]

    def fat_mirror_diff(self):
        n = 0
        a = self.reserved
        b = self.reserved + self.fatsz
        for s in range(self.fatsz):
            if self.sec(a + s) != self.sec(b + s):
                n += 1
        return n

    def cluster_lbas(self, c):
        """簇 c 占的**卷内** LBA 列表（用来判"变化只出现在动过的地方"）。"""
        return [self.clba(c) + k for k in range(self.spc)]


# ---------------------------------------------------------------------------
# QEMU 会话
# ---------------------------------------------------------------------------
def vm_args(qemu, mode, sys_disk, stick, serial, port, name):
    """① ehci / ② companion / ③ ro（只读挂给 QEMU）。

    ★ 场景 ② 落点说明：`-device ich9-usb-ehci1,id=ehci` 配一个伴随 `ich9-usb-uhci1,masterbus=ehci.0,
      firstport=0`（伴随 UHCI 覆盖 EHCI 的 port 1-2）。U 盘是 **High-speed** 设备，QEMU 把它交给
      **EHCI 的 port 1**（FS/LS 设备才会落到伴随 UHCI 上）；所以盘符来自 EHCI，测试断言的就是这一条。

    ★ U 盘用 `-blockdev node-name=stick` 而不是 `-drive ...,if=none,id=stick`：后者在后端被
      `device_del` 之后**连块设备一起销毁**，再 `device_add ... ,drive=stick` 会报
      `Property 'usb-storage.drive' can't find value 'stick'`（实测），热插拔就插不回来；
      `-blockdev` 的节点是独立对象，删设备后仍在，`device_add` 能重新引用它（实测可插回）。
    """
    args = [qemu, "-name", name,
            "-drive", "format=raw,file=%s,index=0,media=disk" % p64.q(sys_disk)]
    if mode == "companion":
        args += ["-device", "ich9-usb-ehci1,id=ehci",
                 "-device", "ich9-usb-uhci1,masterbus=ehci.0,firstport=0,id=uhci1"]
    else:
        args += ["-device", "usb-ehci,id=ehci"]
    args += ["-blockdev", "node-name=stick,driver=file,filename=%s%s"
             % (p64.q(stick), ",read-only=on" if mode == "ro" else ""),
             "-device", "usb-storage,id=stickdev,drive=stick,bus=ehci.0"]
    args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
             "-serial", "file:%s" % p64.q(serial),
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-no-reboot"]
    return args


class Vm:
    def __init__(self, qemu, mode, sys_disk, stick, port, serial, name):
        self.serial = serial
        self.port = port
        args = vm_args(qemu, mode, sys_disk, stick, serial, port, name)
        self.cmdline = " ".join(args)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        return fst.slog(self.serial)

    def mark(self):
        return len(self.log())

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.15)
        return False

    def wait_new(self, pattern, timeout, since):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if re.search(pattern, self.log()[since:]):
                return True
            time.sleep(0.15)
        return False

    def close(self):
        fst.kill(self.proc)


def first_match(pattern, text, flags=0):
    m = re.search(pattern, text, flags)
    return m.group(0) if m else "（缺行）"


def crc32_line(log, path):
    m = re.search(r"\[FAT64\] crc path=%s size=(\d+) crc32=([0-9A-Fa-f]+)" % re.escape(path), log)
    return (int(m.group(1)), int(m.group(2), 16)) if m else None


def tcmd(vm, mon, cmd, needle, timeout=30, since=None):
    """打一条终端命令并等串口出现 needle（返回 needle 之后的日志；超时返回全部日志）。

    ★ 用 type_line_ex（与 tests/usbstorage_test.py 同款）而不是 Monitor.type_line：QEMU HMP 的
    sendkey 键名是小写，直接发大写字母会被拒 —— 命令会打不全（8.3 短名是大写，这条路径必须支持大写）。
    """
    if since is None:
        since = vm.mark()
    type_line_ex(mon, cmd)
    t0 = time.time()
    while time.time() - t0 < timeout:
        log = vm.log()
        if needle in log[since:]:
            return log
        time.sleep(0.2)
    return vm.log()


def type_line_ex(mon, text, per_key=0.12):
    """sendkey 打字（支持大写：shift-<小写>）—— 与 tests/usbstorage_test.py 的 type_line_ex 同款。"""
    names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus",
             ">": "shift-dot", "=": "equal", ":": "shift-semicolon", "(": "shift-9", ")": "shift-0"}
    for ch in text:
        if ch in names:
            mon.key(names[ch], wait=per_key)
        elif ch.isdigit() or ('a' <= ch <= 'z'):
            mon.key(ch, wait=per_key)
        elif 'A' <= ch <= 'Z':
            mon.key("shift-" + ch.lower(), wait=per_key)
        else:
            raise ValueError("sendkey 不支持该字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.15)


def qemu_quit(vm, mon):
    mon.send("quit", wait=1.5)
    for _ in range(60):
        if vm.proc.poll() is not None:
            return True
        time.sleep(0.5)
    vm.close()
    return False


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_ehci_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def forbidden(tag, log, needles=FORBIDDEN):
        for needle in needles:
            check("%s 不得出现 %s" % (tag, needle), needle not in log,
                  first_match(re.escape(needle) + r"[^\r\n]*", log))

    # ==================== 场景 ①：只挂 EHCI ====================
    print("=== 场景 ①：只挂 EHCI（-device usb-ehci,id=ehci）+ 宿主侧造的真 FAT32 U 盘 ===")
    sys_disk1 = os.path.join(tmp, "small1.img")
    stick1 = os.path.join(tmp, "stick1.img")
    serial1 = os.path.join(tmp, "serial1.log")
    if fst.make_small_system_disk(sys_disk1) is None:
        print("  [FAIL] 无法生成小系统盘夹具（build64/system.img 缺失或过大）")
        return 2
    info1 = make_stick(stick1)
    check("U 盘夹具：%d 扇区（%.1f MB）真 FAT32（簇数 %d >= 65525，SPC=%d，根项 4）"
          % (STICK_SECTORS, STICK_SECTORS * SECTOR / 1048576.0, info1["clusters"], info1["spc"]),
          info1["clusters"] == TOTAL_CLUSTERS and info1["spc"] == 1)
    with open(stick1, "rb") as f:
        img1_before = f.read()
    crc1_before = zlib.crc32(img1_before) & 0xFFFFFFFF
    v1 = Fat32View(img1_before)
    check("宿主侧基线解析：簇数=%d、两份 FAT 一致、结构自洽（根目录活项 %d 个）"
          % (v1.clusters, len([x for x in v1.list_dir(v1.root) if not x[4]])),
          v1.clusters == TOTAL_CLUSTERS and v1.fat_mirror_diff() == 0 and
          len([x for x in v1.list_dir(v1.root) if not x[4]]) == 3)
    base1 = {n: v1.find(v1.root if n != DOCS_NAME else info1["c_docs"], n)
             for n in (KEEP1, KEEP2, DOCS, DOCS_NAME)}

    port = fst.free_port()
    vm = Vm(qemu, "ehci", sys_disk1, stick1, port, serial1, "Vimtu64-ehci")
    print("      %s" % vm.cmdline)
    mon = exp.Monitor(port)
    try:
        qh.login_desktop(mon, vm.log, vm.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm.wait_log("[GUI64] ready", 200))
        boot = vm.log()

        # ---- 1) 主控 + 端口 + 设备枚举 ----
        m_pci = re.search(r"\[EHCI\] pci ([0-9A-Fa-f]+):([0-9A-Fa-f]+)\.(\d+) mmio=([0-9A-Fa-f]+) "
                          r"ports=(\d+) caplen=(\d+)", boot)
        check("★ [EHCI] pci <b>:<d>.<f> mmio=… ports=… caplen=…（EHCI 主控找到了）", m_pci is not None,
              m_pci.group(0) if m_pci else first_match(r"\[EHCI\][^\r\n]*", boot))
        check("[EHCI] reset ok hcs=… hcc=… dboff=…（软复位 + 能力寄存器快照）",
              re.search(r"\[EHCI\] reset ok hcs=[0-9a-f]+ hcc=[0-9a-f]+ dboff=\d+", boot) is not None,
              first_match(r"\[EHCI\] reset ok[^\r\n]*", boot))
        check("[EHCI] async qh=… qtd=… qtds=…（异步调度建起来了）",
              re.search(r"\[EHCI\] async qh=[0-9A-Fa-f]+ qtd=[0-9A-Fa-f]+ qtds=\d+", boot) is not None,
              first_match(r"\[EHCI\] async qh[^\r\n]*", boot))
        check("★ 机器上只有 EHCI（没有 UHCI 主控 -> UHCI 侧如实 not found，不冒充）",
              "[USB64] not found" in boot, first_match(r"\[USB64\] not found[^\r\n]*", boot))
        m_res = re.search(r"\[EHCI\] port (\d+) reset ok speed=high ped=1", boot)
        port_n = int(m_res.group(1)) if m_res else -1
        check("★ [EHCI] port N reset ok speed=high ped=1（高速端口复位成功）", m_res is not None,
              m_res.group(0) if m_res else first_match(r"\[EHCI\] port \d+[^\r\n]*", boot))
        check("★ [EHCI] device addr=1 mps=… vendor=… product=…（枚举到设备）",
              re.search(r"\[EHCI\] device addr=1 mps=\d+ vendor=[0-9A-Fa-f]+ product=[0-9A-Fa-f]+",
                        boot) is not None,
              first_match(r"\[EHCI\] device addr[^\r\n]*", boot))
        check("[EHCI] set address=1 ok", re.search(r"\[EHCI\] set address=1 ok", boot) is not None)
        m_cfg = re.search(r"\[EHCI\] config set value=1 ifaces=\d+ msc=1 ep_in=([0-9a-f]+) ep_out=([0-9a-f]+) mps=(\d+)", boot)
        check("★ [EHCI] config set value=1 … msc=1 ep_in=… ep_out=…（BOT 存储接口配好了）", m_cfg is not None,
              m_cfg.group(0) if m_cfg else first_match(r"\[EHCI\] config set[^\r\n]*", boot))
        check("★ [USBST] iface found class=08 sub=06 proto=50（走的是与 UHCI 同一套存储族打点）",
              re.search(r"\[USBST\] iface found class=08 sub=06 proto=50", boot) is not None,
              first_match(r"\[USBST\] iface found[^\r\n]*", boot))

        # ---- 2) 探测 / 容量 / 读 ----
        check("★ [USBST] inquiry vendor=… product=…（INQUIRY 成功）",
              re.search(r"\[USBST\] inquiry vendor=\S+ product=", boot) is not None,
              first_match(r"\[USBST\] inquiry[^\r\n]*", boot))
        m_cap = re.search(r"\[USBST\] capacity blocks=(\d+) block_size=512", boot)
        check("★ [USBST] capacity blocks=%d block_size=512（容量按 READ CAPACITY(10) 报；整根镜像）"
              % STICK_DISK_BLOCKS,
              m_cap is not None and int(m_cap.group(1)) == STICK_DISK_BLOCKS,
              m_cap.group(0) if m_cap else first_match(r"\[USBST\] capacity[^\r\n]*", boot))
        check("★ [USBST] read lba=0 count=1 ok（真的从盘上读了一块）",
              re.search(r"\[USBST\] read lba=0 count=1 ok", boot) is not None,
              first_match(r"\[USBST\] read lba[^\r\n]*", boot))
        check("★ [USBST] selftest PASS mask=0 (EHCI)（探测全过，才暴露成块设备）",
              "[USBST] selftest PASS mask=0 (EHCI)" in boot)
        check("★ [EHCI] selftest PASS mask=0", "[EHCI] selftest PASS mask=0" in boot)
        check("越界读探针被拒（[USBST] read-bounds probe … rejected=1 … rejected=1）",
              re.search(r"\[USBST\] read-bounds probe blocks=\d+ lba=blocks rejected=1 "
                        r"lba=blocks-1 count=2 rejected=1", boot) is not None,
              first_match(r"\[USBST\] read-bounds[^\r\n]*", boot))
        check("越界写探针被拒（[USBST] write-bounds probe … rejected=1 … rejected=1）",
              re.search(r"\[USBST\] write-bounds probe blocks=\d+ lba=blocks rejected=1 "
                        r"lba=blocks-1 count=2 rejected=1", boot) is not None,
              first_match(r"\[USBST\] write-bounds[^\r\n]*", boot))

        # ---- 3) 盘符（门面把 EHCI 的盘也接进来了）----
        check("★ 重扫打点把 EHCI 算进来了（[USBST] storage attached -> rescan drive letters "
              "(usb drives=1, uhci=0 ehci=1 xhci=0)）",
              re.search(r"\[USBST\] storage attached -> rescan drive letters \(usb drives=1, "
                        r"uhci=0 ehci=1 xhci=0\)", boot) is not None,
              first_match(r"\[USBST\] storage attached[^\r\n]*", boot))
        m_letter = re.search(r"\[DRV64\] letter=(\w): disk=(\d+) part=(\d+) fs=FAT32 total_kb=(\d+) "
                             r"free_kb=(\d+) slot=(\S+) ro=(\d) fatvol=(\d+)", boot)
        letter = m_letter.group(1) if m_letter else "D"
        check("★ EHCI 上的 FAT32 U 盘拿到盘符且**可写**（[DRV64] letter=%s: disk=24 … ro=0）" % letter,
              m_letter is not None and m_letter.group(2) == "24" and m_letter.group(7) == "0",
              m_letter.group(0) if m_letter else first_match(r"\[DRV64\] letter[^\r\n]*", boot))
        check("★ [FAT64] rw mount vol=… letter=%s: writable=1 clusters=%d" % (letter, TOTAL_CLUSTERS),
              re.search(r"\[FAT64\] rw mount vol=\d+ letter=%s: writable=1 clusters=%d"
                        % (letter, TOTAL_CLUSTERS), boot) is not None,
              first_match(r"\[FAT64\] rw mount[^\r\n]*", boot))
        check("[DRV64] selftest PASS（可写 U 盘卷也要过一致性自检）", "[DRV64] selftest PASS" in boot)
        forbidden("①", boot)

        # ---- 4) 终端：读文件（宿主侧逐字节）----
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial1, vm.proc))
        llog = tcmd(vm, mon, "vol", "[VOL] vol letter=")
        check("终端卷表：%s: 是 FAT32 且 ro=0（[VOL] vol letter=%s: fs=FAT32 ro=0 total_kb=%d）"
              % (letter, letter, STICK_TOTAL_KB),
              re.search(r"\[VOL\] vol letter=%s: fs=FAT32 ro=0 total_kb=%d" % (letter, STICK_TOTAL_KB), llog)
              is not None,
              first_match(r"\[VOL\] vol letter[^\r\n]*", llog))
        tcmd(vm, mon, "vol %s" % letter.lower(), "[VOL] switch letter=")
        llog = tcmd(vm, mon, "ls /", "[FAT64] list path=/")
        check("★ 在 U 盘根目录列目录（[FAT64] list path=/ entries=3）",
              re.search(r"\[FAT64\] list path=/ entries=3", llog) is not None,
              first_match(r"\[FAT64\] list path[^\r\n]*", llog))
        for name83, data in ((KEEP1, KEEP1_B), (KEEP2, KEEP2_B)):
            cmd = "fatcheck /%s" % name83
            rlog = tcmd(vm, mon, cmd, "[FAT64] crc path=/%s" % name83, timeout=60)
            got = crc32_line(rlog, "/%s" % name83)
            want = zlib.crc32(data) & 0xFFFFFFFF
            check("★ 读文件宿主侧逐字节：fatcheck /%s -> size=%d crc32=%08X（宿主侧 zlib 对同一份夹具字节）"
                  % (name83, len(data), want),
                  got is not None and got[0] == len(data) and got[1] == want,
                  "客人 size=%s crc=%s" % (got[0] if got else "-", "%08X" % got[1] if got else "-"))

        # ---- 5) 终端：写文件 + 读回 ----
        since = vm.mark()
        tcmd(vm, mon, "write /%s %s" % (RW_NAME, RW_TEXT.decode()), "[FAT64] rw write", timeout=40, since=since)
        check("★ 写文件（[FAT64] rw write path=\"/%s\" len=%d … nclusters=1 verify=1）" % (RW_NAME, len(RW_TEXT)),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d cluster=\d+ nclusters=1 verify=1"
                          % (re.escape(RW_NAME), len(RW_TEXT)), 20, since),
              first_match(r"\[FAT64\] rw write[^\r\n]*", vm.log()[since:]))
        check("★ 驱动侧写后读回校验（[USBST] write verify lba=… count=1 ok (read back, byte-for-byte)）",
              vm.wait_new(r"\[USBST\] write verify lba=\d+ count=\d+ ok \(read back, byte-for-byte\)", 20, since))
        check("★ 上层写路径真的走到了 EHCI（[USBST] write lba=… count=… ok (EHCI)）",
              vm.wait_new(r"\[USBST\] write lba=\d+ count=\d+ ok \(EHCI\)", 20, since),
              first_match(r"\[USBST\] write lba[^\r\n]*", vm.log()[since:]))
        rlog = tcmd(vm, mon, "fatcheck /%s" % RW_NAME.upper(), "[FAT64] crc path=/%s" % RW_NAME.upper(),
                    timeout=60)
        got = crc32_line(rlog, "/%s" % RW_NAME.upper())
        want = zlib.crc32(RW_TEXT) & 0xFFFFFFFF
        check("★ 写后读回校验（宿主侧口径）：fatcheck /%s -> size=%d crc32=%08X"
              % (RW_NAME.upper(), len(RW_TEXT), want),
              got is not None and got[0] == len(RW_TEXT) and got[1] == want,
              "客人 size=%s crc=%s" % (got[0] if got else "-", "%08X" % got[1] if got else "-"))

        # ---- 6) 热插拔：拔一次 + 插一次 ----
        print("--- ①-热插拔：device_del -> 盘符消失 -> device_add -> 盘符与读写回来 ---")
        since = vm.mark()
        mon.send("device_del stickdev", wait=1.0)
        check("★ 拔出：monitor device_del -> [EHCI] port %d detached（%d = 场景①的落点）" % (port_n, port_n),
              vm.wait_log("[EHCI] port %d detached" % port_n, 30, since),
              first_match(r"\[EHCI\] port \d+ detached[^\r\n]*", vm.log()[since:]))
        check("★ 拔出后重扫盘符（[USBST] storage detached on EHCI -> rescan drive letters (usb drives=0)）",
              vm.wait_new(r"\[USBST\] storage detached on EHCI -> rescan drive letters \(usb drives=0\)",
                          30, since),
              first_match(r"\[USBST\] storage detached[^\r\n]*", vm.log()[since:]))
        tcmd(vm, mon, "vol", "[VOL] list n=", timeout=30)
        check("★ 拔出后盘符表里没有 %s: 了（[DRV64] scan 之后不再有这条 FAT32 条目）" % letter,
              re.search(r"\[DRV64\] letter=%s: disk=24" % letter, vm.log()[since:]) is None)
        tcmd(vm, mon, "vol c", "[VOL] switch letter=", timeout=30)      # 回到 C:（D: 已经拔了）

        since = vm.mark()
        mon.send("device_add usb-storage,id=stickdev,drive=stick,bus=ehci.0", wait=1.5)
        # ★ 落点：QEMU 把插回来的设备放到**下一个空闲端口**（实测 boot 在 port 1、插回在 port 2），
        #   所以这里按"任意端口 attached speed=high reset ok"判定，并把实际端口打进 detail。
        m_back = None
        dl = time.time() + 40
        while time.time() < dl:
            m_back = re.search(r"\[EHCI\] port (\d+) attached speed=high reset ok", vm.log()[since:])
            if m_back:
                break
            time.sleep(0.3)
        check("★ 插回：monitor device_add -> [EHCI] port N attached speed=high reset ok（boot 落点 %d）"
              % port_n, m_back is not None,
              m_back.group(0) if m_back else first_match(r"\[EHCI\] port \d+ attached[^\r\n]*",
                                                         vm.log()[since:]))
        check("★ 插回后重新枚举 + 探测（[EHCI] config set … msc=1 + [USBST] capacity blocks=%d）"
              % STICK_DISK_BLOCKS,
              vm.wait_new(r"\[USBST\] capacity blocks=%d block_size=512" % STICK_DISK_BLOCKS, 60, since))
        check("★ 插回后重扫盘符（[USBST] storage attached on EHCI -> rescan drive letters (usb drives=1)）",
              vm.wait_new(r"\[USBST\] storage attached on EHCI -> rescan drive letters \(usb drives=1\)",
                          60, since),
              first_match(r"\[USBST\] storage attached on EHCI[^\r\n]*", vm.log()[since:]))
        check("★ 盘符回来了（[DRV64] letter=%s: disk=24 … fs=FAT32 … ro=0）" % letter,
              vm.wait_new(r"\[DRV64\] letter=%s: disk=24 part=\d+ fs=FAT32 total_kb=%d [^\r\n]*ro=0"
                          % (letter, STICK_TOTAL_KB), 60, since),
              first_match(r"\[DRV64\] letter[^\r\n]*", vm.log()[since:]))
        check("插入过程不许冒充整机枚举失败（无 [EHCI] enum FAILED / [EHCI] hotplug … attach failed）",
              "[EHCI] enum FAILED" not in vm.log()[since:] and "[EHCI] hotplug" not in vm.log()[since:])
        forbidden("①-热插拔", vm.log()[since:],
                  ["PANIC", "TRIPLE FAULT", "[EHCI] enum FAILED", "[USB64] enum FAILED",
                   "[EHCI] hotplug", "[EHCI] selftest FAIL"])

        # 插回来以后**真的能写**（不只是有个盘符）
        since = vm.mark()
        tcmd(vm, mon, "vol %s" % letter.lower(), "[VOL] switch letter=", timeout=30)
        since = vm.mark()
        tcmd(vm, mon, "write /%s %s" % (HOT_NAME, HOT_TEXT.decode()), "[FAT64] rw write", timeout=40, since=since)
        check("★ 热插拔之后再写一次（[FAT64] rw write path=\"/%s\" len=%d … verify=1）" % (HOT_NAME, len(HOT_TEXT)),
              vm.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d cluster=\d+ nclusters=1 verify=1"
                          % (re.escape(HOT_NAME), len(HOT_TEXT)), 20, since),
              first_match(r"\[FAT64\] rw write[^\r\n]*", vm.log()[since:]))
        check("★ 热插拔后的写也过驱动侧读回校验（[USBST] write verify … ok）",
              vm.wait_new(r"\[USBST\] write verify lba=\d+ count=\d+ ok \(read back, byte-for-byte\)", 20, since))
        rlog = tcmd(vm, mon, "fatcheck /%s" % HOT_NAME.upper(), "[FAT64] crc path=/%s" % HOT_NAME.upper(),
                    timeout=60)
        got = crc32_line(rlog, "/%s" % HOT_NAME.upper())
        want = zlib.crc32(HOT_TEXT) & 0xFFFFFFFF
        check("★ 热插拔后的文件读回（host 口径）：fatcheck /%s -> size=%d crc32=%08X"
              % (HOT_NAME.upper(), len(HOT_TEXT), want),
              got is not None and got[0] == len(HOT_TEXT) and got[1] == want,
              "客人 size=%s crc=%s" % (got[0] if got else "-", "%08X" % got[1] if got else "-"))
        check("★ 关 QEMU（monitor quit）", qemu_quit(vm, mon))
    finally:
        vm.close()

    # ---- 7) 关掉 QEMU 之后：宿主侧解析镜像，逐字节核对 ----
    print("--- ①-宿主侧：关掉 QEMU 之后自己解析镜像（逐字节）---")
    with open(stick1, "rb") as f:
        img1_after = f.read()
    crc1_after = zlib.crc32(img1_after) & 0xFFFFFFFF
    v1b = Fat32View(img1_after)
    check("★ 整盘 CRC32 **已变化**（真的写进去了）：%08X -> %08X" % (crc1_before, crc1_after),
          crc1_after != crc1_before)
    for name83, data in ((RW_NAME.upper(), RW_TEXT), (HOT_NAME.upper(), HOT_TEXT)):
        e = v1b.find(v1b.root, name83)
        got = v1b.file_bytes(e[0], e[1]) if e else b""
        check("★ ②宿主侧逐字节：/%s 在盘上、size=%d、内容逐字节等于客人写的那串" % (name83, len(data)),
              e is not None and e[1] == len(data) and got == data,
              "crc %08X（宿主重算）" % (zlib.crc32(got) & 0xFFFFFFFF))
        if e:
            check("★ /%s 最后一簇里 size 之后的尾巴全 0（没有垃圾外泄）" % name83,
                  all(b == 0 for b in v1b.tail_after(e[0], e[1])))
    for name83, data in ((KEEP1, KEEP1_B), (KEEP2, KEEP2_B), (DOCS_NAME, DOCS_B)):
        b = base1[name83]
        if name83 == DOCS_NAME:
            cur = v1b.find(info1["c_docs"], name83)
        else:
            cur = v1b.find(v1b.root, name83)
        same = (b is not None and cur is not None and cur[3] == b[3] and
                v1b.file_bytes(cur[0], cur[1]) == data and v1b.cluster(cur[0]) == v1.cluster(b[0]))
        check("★ 原有文件 /%s 的数据与目录项一个字节没变（%d 字节）" % (name83, len(data)), same,
              "crc %08X" % (zlib.crc32(data) & 0xFFFFFFFF))
    d = v1b.find(v1b.root, DOCS)
    check("★ 原有子目录 docs/ 的起始簇没变", d is not None and d[0] == base1[DOCS][0])
    check("★ 两份 FAT 逐字节一致（FAT1 == FAT2）", v1b.fat_mirror_diff() == 0)
    check("★ FSInfo 的 free（%d）与现场重数的空闲簇（%d）相等" % (v1b.fsinfo_free(), v1b.free_clusters()),
          v1b.fsinfo_free() == v1b.free_clusters())

    allowed = set()
    for lba in range(0, v1b.data_start):                       # BPB / FSInfo / BKBOOT / FAT1 / FAT2
        allowed.add(STICK_PART_LBA + lba)
    for c in v1b.chain(v1b.root):
        allowed.update(STICK_PART_LBA + x for x in v1b.cluster_lbas(c))
    for name83 in (RW_NAME.upper(), HOT_NAME.upper()):
        e = v1b.find(v1b.root, name83)
        if e:
            for c in v1b.chain(e[0]):
                allowed.update(STICK_PART_LBA + x for x in v1b.cluster_lbas(c))
    changed = [s for s in range(0, len(img1_before) // SECTOR)
               if img1_before[s * SECTOR:(s + 1) * SECTOR] != img1_after[s * SECTOR:(s + 1) * SECTOR]]
    check("★ 变化只出现在动过的地方（%d 个扇区；FAT/FSInfo/根目录/新分配簇）" % len(changed),
          0 < len(changed) and all(s in allowed for s in changed),
          "越界的改动扇区 = %s" % [s for s in changed if s not in allowed][:8])

    # ==================== 场景 ②：companion（ich9-usb-ehci1 + 伴随 UHCI）====================
    print("=== 场景 ②：companion（-device ich9-usb-ehci1 + 伴随 ich9-usb-uhci1）===")
    sys_disk2 = os.path.join(tmp, "small2.img")
    stick2 = os.path.join(tmp, "stick2.img")
    serial2 = os.path.join(tmp, "serial2.log")
    if fst.make_small_system_disk(sys_disk2) is None:
        print("  [FAIL] 无法生成小系统盘夹具")
        return 2
    make_stick(stick2)
    with open(stick2, "rb") as f:
        img2_before = f.read()
    port2 = fst.free_port()
    vm2 = Vm(qemu, "companion", sys_disk2, stick2, port2, serial2, "Vimtu64-ehci-companion")
    print("      %s" % vm2.cmdline)
    mon2 = exp.Monitor(port2)
    try:
        qh.login_desktop(mon2, vm2.log, vm2.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm2.wait_log("[GUI64] ready", 200))
        boot2 = vm2.log()
        check("[EHCI] pci …（ich9-usb-ehci1 认出来了）",
              re.search(r"\[EHCI\] pci [0-9A-Fa-f]+:[0-9A-Fa-f]+\.\d+ mmio=[0-9A-Fa-f]+ ports=\d+",
                        boot2) is not None,
              first_match(r"\[EHCI\] pci[^\r\n]*", boot2))
        check("★ U 盘落点确定：EHCI 的 **port 1** 复位成高速"
              "（[EHCI] port 1 reset ok speed=high ped=1；HS 设备归 EHCI，FS/LS 才归伴随控制器）",
              re.search(r"\[EHCI\] port 1 reset ok speed=high ped=1", boot2) is not None,
              first_match(r"\[EHCI\] port 1[^\r\n]*", boot2))
        ports2 = re.findall(r"\[EHCI\] port (\d+) owner=(\d) speed=\w+ ccs=(\d)", boot2)
        owned = [p for p, o, _ in ports2 if o == "1"]
        check("★ 伴随控制器持有的端口（owner=1）不被本驱动接管"
              "（有 owner=1 行也不许出现对应端口的 reset ok）",
              all(re.search(r"\[EHCI\] port %s reset ok" % p, boot2) is None for p in owned),
              "owner=1 的端口 = %s" % (owned or "（本 QEMU 拓扑里没有）"))
        check("★ [EHCI] config set value=1 … msc=1（伴随拓扑下存储接口照样配起来）",
              re.search(r"\[EHCI\] config set value=1 ifaces=\d+ msc=1 ep_in=[0-9a-f]+ ep_out=[0-9a-f]+", boot2)
              is not None)
        check("★ [USBST] capacity blocks=%d + [USBST] selftest PASS mask=0 (EHCI)" % STICK_DISK_BLOCKS,
              re.search(r"\[USBST\] capacity blocks=%d block_size=512" % STICK_DISK_BLOCKS, boot2) is not None and
              "[USBST] selftest PASS mask=0 (EHCI)" in boot2)
        m_let2 = re.search(r"\[DRV64\] letter=(\w): disk=24 part=\d+ fs=FAT32 total_kb=\d+ "
                           r"free_kb=\d+ slot=\S+ ro=0", boot2)
        letter2 = m_let2.group(1) if m_let2 else "D"
        check("★ companion 拓扑下盘符照常出现且可写（[DRV64] letter=%s: disk=24 … ro=0）" % letter2,
              m_let2 is not None, m_let2.group(0) if m_let2 else first_match(r"\[DRV64\] letter[^\r\n]*", boot2))
        check("★ 门面计数：storage attached -> rescan drive letters (usb drives=1, uhci=0 ehci=1 xhci=0)",
              re.search(r"\[USBST\] storage attached -> rescan drive letters \(usb drives=1, "
                        r"uhci=0 ehci=1 xhci=0\)", boot2) is not None,
              first_match(r"\[USBST\] storage attached[^\r\n]*", boot2))
        forbidden("②", boot2, FORBIDDEN_COMPANION)

        check("打开终端（[APP] term opened）", fst.open_terminal(mon2, serial2, vm2.proc))
        tcmd(vm2, mon2, "vol %s" % letter2.lower(), "[VOL] switch letter=")
        rlog2 = tcmd(vm2, mon2, "fatcheck /%s" % KEEP1, "[FAT64] crc path=/%s" % KEEP1, timeout=60)
        got2 = crc32_line(rlog2, "/%s" % KEEP1)
        want2 = zlib.crc32(KEEP1_B) & 0xFFFFFFFF
        check("★ companion 场景读文件（宿主侧逐字节）：fatcheck /%s -> size=%d crc32=%08X"
              % (KEEP1, len(KEEP1_B), want2),
              got2 is not None and got2[0] == len(KEEP1_B) and got2[1] == want2,
              "客人 size=%s crc=%s" % (got2[0] if got2 else "-", "%08X" % got2[1] if got2 else "-"))
        since2 = vm2.mark()
        tcmd(vm2, mon2, "write /%s %s" % (RW_NAME, RW_TEXT.decode()), "[FAT64] rw write", timeout=40, since=since2)
        check("★ companion 场景写文件（[FAT64] rw write len=%d verify=1 + [USBST] write lba=… ok (EHCI)）"
              % len(RW_TEXT),
              vm2.wait_new(r"\[FAT64\] rw write vol=\d+ path=\"/%s\" len=%d [^\r\n]*verify=1"
                           % (re.escape(RW_NAME), len(RW_TEXT)), 20, since2) and
              vm2.wait_new(r"\[USBST\] write lba=\d+ count=\d+ ok \(EHCI\)", 20, since2),
              first_match(r"\[FAT64\] rw write[^\r\n]*", vm2.log()[since2:]))
        check("★ 关 QEMU（monitor quit）", qemu_quit(vm2, mon2))
    finally:
        vm2.close()
    with open(stick2, "rb") as f:
        img2_after = f.read()
    v2 = Fat32View(img2_after)
    e2 = v2.find(v2.root, RW_NAME.upper())
    got2b = v2.file_bytes(e2[0], e2[1]) if e2 else b""
    check("★ companion：关 QEMU 后宿主侧核对 /%s 内容逐字节（%d 字节）" % (RW_NAME.upper(), len(RW_TEXT)),
          e2 is not None and e2[1] == len(RW_TEXT) and got2b == RW_TEXT,
          "crc %08X" % (zlib.crc32(got2b) & 0xFFFFFFFF))
    check("★ companion：整盘 CRC32 已变化（写真的落到盘上）",
          (zlib.crc32(img2_after) & 0xFFFFFFFF) != (zlib.crc32(img2_before) & 0xFFFFFFFF))

    # ==================== 场景 ③：反例（有界失败、不崩、不碰介质）====================
    print("=== 场景 ③：反例（U 盘只读挂给 QEMU：越界被拒 / 设备拒写 -> 有界失败、不崩、盘没被写坏）===")
    sys_disk3 = os.path.join(tmp, "small3.img")
    stick3 = os.path.join(tmp, "stick3.img")
    serial3 = os.path.join(tmp, "serial3.log")
    if fst.make_small_system_disk(sys_disk3) is None:
        print("  [FAIL] 无法生成小系统盘夹具")
        return 2
    make_stick(stick3)
    with open(stick3, "rb") as f:
        img3_before = f.read()
    crc3_before = zlib.crc32(img3_before) & 0xFFFFFFFF
    port3 = fst.free_port()
    vm3 = Vm(qemu, "ro", sys_disk3, stick3, port3, serial3, "Vimtu64-ehci-ro")
    print("      %s" % vm3.cmdline)
    mon3 = exp.Monitor(port3)
    try:
        qh.login_desktop(mon3, vm3.log, vm3.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm3.wait_log("[GUI64] ready", 200))
        boot3 = vm3.log()
        check("③ 越界**读**被拒（[USBST] read-bounds probe … rejected=1 … rejected=1）",
              re.search(r"\[USBST\] read-bounds probe blocks=\d+ lba=blocks rejected=1 "
                        r"lba=blocks-1 count=2 rejected=1", boot3) is not None,
              first_match(r"\[USBST\] read-bounds[^\r\n]*", boot3))
        check("③ 越界**写**被拒（[USBST] write-bounds probe … rejected=1 … rejected=1）",
              re.search(r"\[USBST\] write-bounds probe blocks=\d+ lba=blocks rejected=1 "
                        r"lba=blocks-1 count=2 rejected=1", boot3) is not None,
              first_match(r"\[USBST\] write-bounds[^\r\n]*", boot3))
        check("③ 越界被拒**不算失败**（[USBST] selftest PASS mask=0 (EHCI) + [EHCI] selftest PASS mask=0）",
              "[USBST] selftest PASS mask=0 (EHCI)" in boot3 and "[EHCI] selftest PASS mask=0" in boot3,
              first_match(r"\[(EHCI|USBST)\] selftest[^\r\n]*", boot3))
        m_let3 = re.search(r"\[DRV64\] letter=(\w): disk=24 part=\d+ fs=FAT32", boot3)
        letter3 = m_let3.group(1) if m_let3 else "D"
        check("③ 盘照样拿到盘符（越界探针没有把设备打坏）：[DRV64] letter=%s: disk=24 … fs=FAT32" % letter3,
              m_let3 is not None, m_let3.group(0) if m_let3 else first_match(r"\[DRV64\] letter[^\r\n]*", boot3))
        check("打开终端（[APP] term opened）", fst.open_terminal(mon3, serial3, vm3.proc))
        tcmd(vm3, mon3, "vol %s" % letter3.lower(), "[VOL] switch letter=")
        rlog3 = tcmd(vm3, mon3, "fatcheck /%s" % KEEP2, "[FAT64] crc path=/%s" % KEEP2, timeout=60)
        got3 = crc32_line(rlog3, "/%s" % KEEP2)
        want3 = zlib.crc32(KEEP2_B) & 0xFFFFFFFF
        check("③ 越界探针之后设备照常可用（读 48 簇多簇文件 CRC32 仍然对得上）",
              got3 is not None and got3[0] == len(KEEP2_B) and got3[1] == want3,
              "客人 size=%s crc=%s" % (got3[0] if got3 else "-", "%08X" % got3[1] if got3 else "-"))

        # 设备侧拒写（协议层"坏状态/坏帧"的等价形式）：QEMU 侧 readonly=on -> CSW 报错
        print("--- ③-设备拒写（CSW 报错）：write -> [USBST] write FAILED reason=csw status ---")
        since3 = vm3.mark()
        tcmd(vm3, mon3, "write /%s %s" % (WP_NAME, WP_TEXT.decode()), "[TERM] cmd write",
             timeout=60, since=since3)
        after3 = vm3.log()[since3:]
        check("③★ 设备拒写 -> 驱动如实报失败（[USBST] write FAILED lba=… count=… reason=csw status）",
              re.search(r"\[USBST\] write FAILED lba=\d+ count=\d+ reason=csw status", after3) is not None,
              first_match(r"\[USBST\] write FAILED[^\r\n]*", after3))
        check("③★ 终端也如实报失败（[TERM] cmd write fail）", "[TERM] cmd write fail" in after3,
              first_match(r"\[TERM\] cmd write[^\r\n]*", after3))
        check("③★ 不是静默写坏：**不出现** [USBST] write verify FAILED / [FAT64] rw verify FAILED",
              "[USBST] write verify FAILED" not in after3 and "[FAT64] rw verify FAILED" not in after3)
        check("③★ 有界失败、不崩：写失败之后设备照常可读（fatcheck /%s 仍然对得上）" % KEEP1,
              (lambda g: g is not None and g[0] == len(KEEP1_B) and g[1] == (zlib.crc32(KEEP1_B) & 0xFFFFFFFF))(
                  crc32_line(tcmd(vm3, mon3, "fatcheck /%s" % KEEP1, "[FAT64] crc path=/%s" % KEEP1,
                                  timeout=60), "/%s" % KEEP1)))
        check("③ 关 QEMU（monitor quit）", qemu_quit(vm3, mon3))
    finally:
        vm3.close()
    with open(stick3, "rb") as f:
        img3_after = f.read()
    crc3_after = zlib.crc32(img3_after) & 0xFFFFFFFF
    check("③★ 关 QEMU 后：整盘镜像**一个字节都没变**（%08X -> %08X）—— 被拒绝的写真的没碰介质"
          % (crc3_before, crc3_after), crc3_after == crc3_before)
    v3 = Fat32View(img3_after)
    check("③★ 盘上没有被拒写的文件（/%s 不存在）" % WP_NAME.upper(),
          v3.find(v3.root, WP_NAME.upper()) is None)
    forbidden("③", vm3.log(), ["PANIC", "TRIPLE FAULT", "[EHCI] enum FAILED", "[USB64] enum FAILED",
                               "[EHCI] hotplug"])

    if args.keep:
        print("[ehci64] 临时目录（镜像/串口日志都在里面）：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
