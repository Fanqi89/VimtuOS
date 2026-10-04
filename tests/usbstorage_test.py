#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/usbstorage_test.py - 批次 O：**USB 存储（U 盘）**端到端验收（只读 + 从 U 盘拷应用）

它做什么（全部自动：串口打点 + QEMU monitor 注入键鼠 + 宿主侧字节校验）：

  阶段 1  宿主侧造一根"U 盘"（make_usbstick）：
    * 主机侧 Python 造的 **真 FAT32 卷**（35MB，SPC=1 -> 簇数 68874 >= 65525，MBR 类型 0x0C）
    * 根目录 3 个文件 + 1 个子目录：
        根  USB-Readme-2026.txt  （VFAT 长名 -> 证明 LFN 解析也走 USB 通路）
        根  STICKAPP.VAP         （= build64/hello.vap 的字节，240 B）
        根  STICKELF.ELF         （= build64/hello.elf 的字节，9664 B -> 多簇/多条 READ(10)）
        根  docs/notes.txt       （子目录 -> 证明能在管理器里进去看）
    * 盘长 = 2048 + 70000 扇区（= 72048 扇区 = 35.2MB），也是"容量断言"的独立来源。

  阶段 2  QEMU 只挂 **U 盘**（piix3-usb-uhci + usb-storage，**不含键盘设备**）：
    a) 枚举/传输层串口证据：[USB64] port 1 connected / [USBST] iface found class=08 sub=06
       proto=50 ep_in=.. ep_out=.. / inquiry vendor=../product=.. / capacity blocks=<n>
       block_size=512 / read lba=0 count=1 ok / selftest PASS mask=0 / [USB64] selftest PASS；
       容量与宿主侧镜像逐项对得上（blocks == 72048、cap_mb == 35）。
    b) 磁盘抽象接入：[DRV64] letter=D: disk=24 part=1 fs=FAT32 … ro=1 fatvol=…（驱动器号 24 =
       ATA64_USB_BASE）；[FAT64] mount … lba=2048 clusters=68874 … fat_ok=1 spc=1 ro=1；
       [USBST] storage attached -> rescan drive letters。
    c) 文件管理器：此电脑页有 D: 卡片 -> 双击进入（enter letter=D: fatvol=… ok items=4）+
       只读打点（vol letter=D: fs=FAT32 ro=1）+ 目录项（含长名 USB-Readme-2026.txt / 两个应用 /
       docs 子目录）+ 像素（图标单元格有暗像素）+ 截图存 docs/screenshots/explorer_usbstick64.png。
    d) ★ 从 U 盘拷文件到 C:（VimtuFS2）：进入 D: -> 选中 STICKELF.ELF -> Ctrl+C -> 回此电脑 ->
       进 C: 根 -> Ctrl+V（paste ok n=1 dst=/ skipped=0）-> 列表里出现 STICKELF.ELF；
       再用终端 `cat /STICKELF.ELF` 读回同样字节数；**关掉 QEMU 后**宿主侧解析 C: 卷
       （multivol64_test 的 v3 读取器）与 build64/hello.elf **逐字节比对**（两个文件都比）。
    e) 写 U 盘被拒（三类证据）：
       * 终端：`vol` 把 D: 标成 ro=1；`write` / `mkdir` 失败（终端在**入口**就按只读卷拦下 ——
         所以这里**不会**出现 `[FS64] reject … op=write`，那是"绕过入口"才有的行，如实说明不假装）；
       * 管理器：在 D: 上 Delete / Ctrl+V 被拒（`[UI] explorer roact op=delete|paste letter=D: fs=FAT32 ro=1`）；
       * 整根 U 盘镜像（35.2MB）**测试前后 CRC32 完全一致**（谁都没写它）。
    f) 禁止项：PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [USBST] read FAILED …

  阶段 3  **键盘 + U 盘同时插**（回归：批次 O 改成"最多两台设备"后 HID 键盘不能坏）：
    断言 [USB64] config set value=1 ifaces=1 hid=1 ep_in=81 mps=8 + hid boot protocol set
    + [USBST] capacity/inquiry/read 都在（两台设备各拿一个地址 1/2，互不影响）。

QEMU 命令行（实测能枚举到的就是这一条；-device piix3-usb-uhci + usb-storage,bus=uhci.0）：
  qemu-system-x86_64.exe -name Vimtu64-usbstorage \\
    -drive format=raw,file=<16MB小系统盘>,index=0,media=disk \\
    -device piix3-usb-uhci,id=uhci \\
    -drive format=raw,file=<stick.img>,if=none,id=stick \\
    -device usb-storage,drive=stick,bus=uhci.0 \\
    -boot order=c -m 512 -vga std -display none -serial file:<log> \\
    -monitor telnet:127.0.0.1:<port>,server,nowait -no-reboot

用法（必须用 Windows 原生 Python）：py -3 tests\\usbstorage_test.py [--keep]
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
import fs_tree_test as fst         # noqa: E402  小系统盘夹具 + 串口工具
import explorer64_test as exp      # noqa: E402  鼠标闭环 / 像素工具
import fileops64_test as fo        # noqa: E402  条目单元格几何 / 剪贴板流程
import multivol64_test as mv       # noqa: E402  宿主侧 v3 卷解析（C: 内容逐字节核对）
import fatread64_test as fatr      # noqa: E402  宿主侧 FAT32 构造器（造 U 盘镜像）
import qemuhelp as qh              # noqa: E402  （★ 公共登录手势：ui.login.auto 默认 0）

SECTOR = 512
BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
HELLO_VAP = os.path.join(BUILD, "hello.vap")
HELLO_ELF = os.path.join(BUILD, "hello.elf")
SHOT_PNG = os.path.join(ROOT, "docs", "screenshots", "explorer_usbstick64.png")

# ---- U 盘镜像几何（阶段 1 的宿主侧"独立来源"）----
STICK_PART_LBA = 2048
STICK_PART_SECTORS = 70000          # 34MB 分区（SPC=1 -> 簇数 68874 >= 65525 = 真 FAT32）
STICK_SECTORS = STICK_PART_LBA + STICK_PART_SECTORS      # 72048 扇区 = 35.2MB
STICK_TOTAL_KB = STICK_SECTORS // 2

# ---- U 盘根目录内容 ----
TXT_NAME = "USB-Readme-2026.txt"                 # VFAT 长名
TXT_TEXT = b"USB stick (FAT32) browsed over UHCI BOT + SCSI READ(10)\n"
VAP_NAME = "STICKAPP.VAP"                        # 8.3 短名（= build64/hello.vap 的字节，240 B）
ELF_NAME = "STICKELF.ELF"                        # 8.3 短名（= build64/hello.elf 的字节，9664 B -> 多簇）
DOCS_DIR = "docs"
DOCS_NAME = "notes.txt"
DOCS_TEXT = b"notes inside the USB stick\n"
# ★ P8b：终端往 U 盘上写的那个文件（关掉 QEMU 后宿主侧逐字节核对）
WR_NAME = "usbwr.txt"
WR_TEXT = b"vimtu64-usbstorage-now-writable"
ROOT_ITEMS = 4                                   # 3 个文件 + 1 个目录

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:",
             "[USBST] read FAILED", "[USB64] selftest FAIL", "[USBST] selftest FAIL",
             "[USB64] enum FAILED"]

# ---------------------------------------------------------------------------
# 宿主侧：造 U 盘（MBR + 真 FAT32）
# ---------------------------------------------------------------------------
def put_file(dev, data):
    """按簇分配 + 写入（返回 (首簇, 簇数)）。簇大小按 dev.spc 算（这里 spc=1）。"""
    per = dev.spc * SECTOR
    n = max(1, (len(data) + per - 1) // per)
    c = dev.alloc(n)
    for i in range(n):
        dev.write_cluster(c + i, data[i * per:(i + 1) * per])
    return c, n


def make_usbstick(path):
    """造一根 U 盘：MBR（1 个 0x0C FAT32 LBA 分区 @2048）+ 根目录 3 文件 + docs/。"""
    dev = fatr.Fat32Builder(STICK_PART_LBA, STICK_PART_SECTORS, 1)
    root = dev.alloc(1)                                # 簇 2 = 根目录
    ents = bytearray()

    vap = open(HELLO_VAP, "rb").read()
    elf = open(HELLO_ELF, "rb").read()

    c_txt, n_txt = put_file(dev, TXT_TEXT)
    ents += dev.dir_entry(TXT_NAME, b"USBREA~1TXT", 0x20, c_txt, len(TXT_TEXT))
    c_vap, n_vap = put_file(dev, vap)
    ents += dev.dir_entry(VAP_NAME, b"STICKAPPVAP", 0x20, c_vap, len(vap))
    c_elf, n_elf = put_file(dev, elf)
    ents += dev.dir_entry(ELF_NAME, b"STICKELFELF", 0x20, c_elf, len(elf))

    # 子目录 docs/notes.txt
    c_docs = dev.alloc(1)
    c_note, n_note = put_file(dev, DOCS_TEXT)
    dz = bytearray()
    dz += dev.short_entry(b".          ", 0x10, c_docs, 0)
    dz += dev.short_entry(b"..         ", 0x10, 0, 0)
    dz += dev.dir_entry(DOCS_NAME, b"NOTES   TXT", 0x20, c_note, len(DOCS_TEXT))
    dev.write_cluster(c_docs, bytes(dz))
    ents += dev.dir_entry(DOCS_DIR, b"DOCS       ", 0x10, c_docs, 0)

    dev.write_cluster(root, bytes(ents))
    vol = dev.finish()

    full = bytearray(STICK_SECTORS * SECTOR)
    mbr = bytearray(SECTOR)
    e1 = bytearray(16)
    e1[0] = 0x80                                     # 可引导位（U 盘常见；这里不影响引导顺序）
    e1[4] = 0x0C                                     # FAT32 LBA
    struct.pack_into("<II", e1, 8, STICK_PART_LBA, STICK_PART_SECTORS)
    mbr[446:462] = e1
    mbr[510], mbr[511] = 0x55, 0xAA
    full[0:SECTOR] = mbr
    full[STICK_PART_LBA * SECTOR:STICK_PART_LBA * SECTOR + len(vol)] = vol
    with open(path, "wb") as f:
        f.write(bytes(full))
    return {"clusters": dev.clusters, "fatsz": dev.fatsz, "spc": dev.spc,
            "blocks": STICK_SECTORS, "total_kb": STICK_TOTAL_KB, "data_start": dev.data_start,
            "map": {"root": (root, 1), "txt": (c_txt, n_txt), "vap": (c_vap, n_vap),
                    "elf": (c_elf, n_elf), "docs": (c_docs, 1), "note": (c_note, n_note)},
            "vap": vap, "elf": elf, "txt": TXT_TEXT, "docs": DOCS_TEXT}


# ---------------------------------------------------------------------------
# QEMU
# ---------------------------------------------------------------------------
def usb_args(qemu, system_disk, stick, serial, port, name, with_kbd=False):
    """小系统盘（IDE index 0）+ U 盘（usb-storage on piix3-usb-uhci）；可选再插一个 usb-kbd。

    ★ 设备顺序/端口有讲究（实测）：
      * `usb-kbd` 排在 `usb-storage` **前面**（先插键盘占 port 1）；
      * 同时插时给存储**显式 `port=2`** —— 不指定的话 QEMU 会在 port 2 上自动建一个 8 口 hub、
        把存储挂到 hub 后面（qtree: `addr 0.0, port 2, name QEMU USB Hub` -> `port 2.1`），
        而本驱动不做 hub（见 kernel/usb64.h 的限制），那样就验不到"两台设备同时在线"。
    """
    args = [qemu, "-name", name,
            "-drive", "format=raw,file=%s,index=0,media=disk" % p64.q(system_disk),
            "-device", "piix3-usb-uhci,id=uhci"]
    if with_kbd:
        args += ["-device", "usb-kbd,bus=uhci.0"]
    args += ["-drive", "format=raw,file=%s,if=none,id=stick" % p64.q(stick),
             "-device", "usb-storage,drive=stick,bus=uhci.0" + (",port=2" if with_kbd else "")]
    args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
             "-serial", "file:%s" % p64.q(serial),
             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
             "-no-reboot"]
    return args


class Vm:
    """小系统盘 + U 盘的 QEMU 会话（复用 fileops64_test 的 vm 接口形状）。"""

    def __init__(self, qemu, system_disk, stick, port, serial, name, with_kbd=False):
        self.serial = serial
        self.port = port
        args = usb_args(qemu, system_disk, stick, serial, port, name, with_kbd)
        self.cmdline = " ".join(args)
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        return fst.slog(self.serial)

    def mark(self):
        return len(self.log())

    def wait_log(self, needle, timeout, since=0):
        # 轮询 0.15s（原来 0.3s）：只改等待粒度，不改任何判据 —— 只影响"多久发现打点"
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

    def wait_nav(self, path, items, timeout, since):
        return self.wait_new(r"\[UI\] explorer nav path=%s items=%d view=\w+" % (re.escape(path), items),
                             timeout, since)

    def close(self):
        fst.kill(self.proc)


def save_png(ppm, png):
    """PPM -> PNG（Pillow）；没装 Pillow 就保留 PPM 并如实说明。"""
    try:
        from PIL import Image
        Image.open(ppm).save(png)
        return True
    except Exception as e:                                    # pragma: no cover
        print("      （没有 Pillow：%s，已保留 PPM %s）" % (e, ppm))
        return False

def at_thispc(vm):
    """日志里最后一条导航打点是不是"此电脑"页（用来决定要不要先按"返回此电脑"）。"""
    log = vm.log()
    last_thispc = log.rfind("[UI] explorer thispc drives=")
    last_inside = max(log.rfind("[UI] explorer enter letter="), log.rfind("[UI] explorer up path="),
                      log.rfind("[UI] explorer nav path="))
    return last_thispc > last_inside







def type_line_ex(mon, text, per_key=0.12):
    """sendkey 打字：比 explorer64_test 的 type_line 多支持**大写字母**（shift-<小写>）。

    为什么需要：U 盘上的 8.3 短名按规范是大写（STICKAPP.VAP），而 QEMU HMP 的 sendkey 键名是
    小写（"s"），直接发 "S" 会被 QEMU 拒掉 —— 命令就打不全（实测：cat 命令少了大写字符）。
    """
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

def fresh_item_idx(vm, name, since):
    """只看 since 之后的 `[UI] explorer item` 打点（避免拿到别的卷/旧视图里的同名 idx）。"""
    m = None
    for mm in re.finditer(r"\[UI\] explorer item idx=(\d+) name=(\S+) type=", vm.log()[since:]):
        if mm.group(2) == name:
            m = mm
    return int(m.group(1)) if m else -1


def fresh_or_last_item_idx(vm, name, since):
    """先在 since 之后的窗口里找；找不到就退回"全日志最后一次"（同一卷已经列过时的兜底）。"""
    idx = fresh_item_idx(vm, name, since)
    return idx if idx >= 0 else fo.last_item_idx(vm, name)

# ---------------------------------------------------------------------------
# ★ 跑法提速（缺陷 2：预算内跑完；判据一字不改）：
#   原来每一次 aim_click / aim_single_click / fo.Mouse.click 都重新做一次
#   calibrate_offset（一次 park + ~500px 盲走 ≈ 10s+），一轮十几处就是 150s+；
#   鼠标包节奏 0.06/0.2 也偏保守。这里：
#     1) 误差标定**只做一次**并缓存，命中/导航打点没出现才重标（对丢包仍是保险）；
#     2) park / 盲走用同一模型、同样的包数，只把监控器等待从 0.06/0.2 压到 0.04/0.12
#        （PS/2 设备队列远不会因为 25 包/秒 而溢出）。
#   断言、等待对象、超时长度都不变。
# ---------------------------------------------------------------------------
def fast_park(mon, times=58, wait=0.04):
    for _ in range(times):
        mon.send("mouse_move -100 -100", wait=wait)
    time.sleep(0.3)


def fast_move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.send("mouse_move %d 0" % (mag if d > 0 else -mag), wait=0.12)
    else:
        mon.send("mouse_move 0 %d" % (mag if d > 0 else -mag), wait=0.12)


def fast_move_px(mon, axis, d, px):
    """与 explorer64_test.move_px 同一内核模型（100 包 = 24px + 刹车包清残余），只改等待粒度。"""
    if px < 1 or d == 0:
        return
    n = min(48, px // exp.MOUSE_STEP)
    rem = px - n * exp.MOUSE_STEP
    for _ in range(n):
        fast_move_axis(mon, axis, d, 100)
    fast_move_axis(mon, axis, -d, exp.MOUSE_BRAKE)
    if rem >= 2:
        mag = int(round(rem / 1.7))
        if mag >= 1:
            fast_move_axis(mon, axis, d, mag)


def fast_goto(mon, tx, ty):
    fast_park(mon)
    fast_move_px(mon, 0, +1, tx)
    fast_move_px(mon, 1, +1, ty)
    time.sleep(0.2)


class Aim:
    """一次标定、反复复用的鼠标闭环（只提速，不改判据）。"""

    def __init__(self, vm, mon):
        self.vm, self.mon = vm, mon
        self.dxe = self.dye = 0

    def calibrate(self):
        self.dxe, self.dye, _ = exp.calibrate_offset(self.vm, self.mon)
        return self.dxe, self.dye

    def click_hit(self, tx, ty, want_hit, tries=3):
        """单击 + 读 `[UI] explorer click … hit=…` 打点验证命中（同 fo.Mouse.click 的判据）。"""
        for _ in range(tries):
            since = len(self.vm.log())
            fast_goto(self.mon, tx + self.dxe, ty + self.dye)
            exp.single_click_at(self.mon)
            p = exp.last_probe(self.vm, since)
            if p is not None and p[2] == want_hit:
                return p
            self.calibrate()
        return None

    def click_once_at(self, tx, ty):
        fast_goto(self.mon, tx + self.dxe, ty + self.dye)
        exp.single_click_at(self.mon)

    def click_until(self, tx, ty, needle, tries=3, timeout=20):
        since = len(self.vm.log())
        for _ in range(tries):
            fast_goto(self.mon, tx + self.dxe, ty + self.dye)
            exp.single_click_at(self.mon)
            if self.vm.wait_log(needle, timeout, since=since):
                return True
            since = len(self.vm.log())
            self.calibrate()
        return False

    def dclick_until(self, tx, ty, needle, tries=3, timeout=20):
        since = len(self.vm.log())
        for _ in range(tries):
            fast_goto(self.mon, tx + self.dxe, ty + self.dye)
            exp.double_click(self.mon)            # 抬-按-抬-按-抬：两次按下 ~0.2s < 500ms 窗口
            if self.vm.wait_log(needle, timeout, since=since):
                return True
            since = len(self.vm.log())
            try:
                exp.ensure_window_on_top(self.vm, self.mon)
            except Exception:
                pass
            self.calibrate()
        return False


def back_to_thispc(aim, vm, mon, tries=3):
    """回"此电脑"：点导航窗格第一项；不行再点面包屑第 0 段（与 multivol64_test 同判据）。"""
    needle = "[UI] explorer thispc drives="
    for _ in range(tries):
        n0 = vm.log().count(needle)
        aim.click_once_at(exp.sx(20), exp.sy(exp.CONTENT_Y + 30 + 22 // 2))     # 导航窗格第 1 项
        if vm.wait_log(needle, 6, since=0) and vm.log().count(needle) > n0:
            return True
        n0 = vm.log().count(needle)
        aim.click_once_at(exp.sx(110), exp.sy(exp.TOOLBAR_H + exp.ADDR_H // 2))  # 面包屑第 0 段
        if vm.wait_log(needle, 8, since=0) and vm.log().count(needle) > n0:
            return True
    return False


def dclick_card_until(aim, vm, mon, letter, needle, tries=3):
    """双击 <letter>: 盘卡片 -> 等 enter 打点（重试只针对注入丢包，判据不变）。"""
    idx = fo.card_idx(vm, letter)
    if idx is None:
        return False
    cx, cy = exp.card_center(idx)
    return aim.dclick_until(cx, cy, needle, tries=tries)


def goto_volume(aim, vm, mon, letter):
    """进入某个卷：不在"此电脑"页就先回去，再双击 <letter>: 卡片（等 enter 打点）。"""
    if not at_thispc(vm):
        if not back_to_thispc(aim, vm, mon):
            return False
    return dclick_card_until(aim, vm, mon, letter, "[UI] explorer enter letter=%s:" % letter)




class SkipPhase(Exception):
    """--phase 调试开关用：跳过某个阶段（不改变默认全量行为）。"""


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--phase", default="all",
                    help="只跑指定阶段（调试用）：1 / 2 / 3 / 1,2；默认 all")
    args = ap.parse_args()
    phases = {"1", "2", "3"} if args.phase == "all" else set(args.phase.split(","))
    ph1 = "1" in phases
    ph2 = "2" in phases
    ph3 = "3" in phases

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (SYSTEM_IMG, HELLO_VAP, HELLO_ELF):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_usbstorage_")
    sys_disk = os.path.join(tmp, "small.img")
    stick = os.path.join(tmp, "stick.img")
    serial = os.path.join(tmp, "serial.log")
    serial2 = os.path.join(tmp, "serial_kbd.log")
    shot_ppm = os.path.join(tmp, "usbstick.ppm")

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

    # ==================== 阶段 0：夹具 ====================
    print("=== 阶段 0：宿主侧造 16MB 小系统盘（C: = VimtuFS2 v3）+ 35MB FAT32 U 盘 ===")
    if fst.make_small_system_disk(sys_disk) is None:
        print("  [FAIL] 无法生成小系统盘夹具（build64/system.img 缺失或过大）")
        return 2
    info = make_usbstick(stick)
    check("U 盘镜像 %d 扇区（%.1f MB）+ 真 FAT32（簇数 %d >= 65525，SPC=%d）"
          % (STICK_SECTORS, STICK_SECTORS * SECTOR / 1048576.0, info["clusters"], info["spc"]),
          info["clusters"] >= 65525 and info["spc"] == 1)
    with open(stick, "rb") as f:
        stick_before = f.read()
    check("U 盘镜像字节取出（%d 字节）" % len(stick_before), len(stick_before) == STICK_SECTORS * SECTOR)
    check("U 盘上三个文件的源字节（hello.vap=%d B / hello.elf=%d B / 长名文本=%d B）"
          % (len(info["vap"]), len(info["elf"]), len(info["txt"])),
          len(info["vap"]) > 0 and len(info["elf"]) > len(info["vap"]))

    # ==================== 阶段 1：只挂 U 盘 -> 枚举/容量/浏览/拷贝/只读 ====================
    print("=== 阶段 1：QEMU 只挂 U 盘（piix3-usb-uhci + usb-storage）===")
    port = fst.free_port()
    vm = Vm(qemu, sys_disk, stick, port, serial, "Vimtu64-usbstorage")
    print("      %s" % vm.cmdline)
    mon = exp.Monitor(port)
    try:
        if not ph1:                                  # --phase：调试时跳过整段（夹具照建，QEMU 照起）
            raise SkipPhase()
        # ★ 登录手势：等锁屏可交互（[LOCK64] bg blur ready）-> 回车两次（锁屏 -> 登录 -> 桌面）
        qh.login_desktop(mon, vm.log, vm.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm.wait_log("[GUI64] ready", 200))

        # ---- 1a) USB 主机 + 存储枚举的串口证据 ----
        print("--- 1a) 串口：U 盘枚举 / INQUIRY / READ CAPACITY / READ(10) ---")
        log = vm.log()
        check("[USB64] uhci pci <bus>:<dev>.<fn> io=<hex> ports=2",
              re.search(r"\[USB64\] uhci pci \d+:\d+\.\d+ io=[0-9A-F]{4} ports=2", log) is not None,
              (re.search(r"\[USB64\] uhci pci .*", log).group(0) if "[USB64] uhci" in log else "（缺行）"))
        check("[USB64] port 1 connected speed=full reset ok",
              re.search(r"\[USB64\] port 1 connected speed=full reset ok", log) is not None)
        check("[USB64] set address=1 ok", re.search(r"\[USB64\] set address=1 ok", log) is not None)
        check("[USB64] config set value=1 ifaces=1 msc=1 ep_in=<hex> ep_out=<hex> mps=64",
              re.search(r"\[USB64\] config set value=1 ifaces=1 msc=1 ep_in=[0-9A-F]{2} "
                        r"ep_out=[0-9A-F]{2} mps=64", log) is not None)
        m_if = re.search(r"\[USBST\] iface found class=08 sub=06 proto=50 "
                         r"ep_in=([0-9A-F]{2}) ep_out=([0-9A-F]{2})", log)
        check("★ [USBST] iface found class=08 sub=06 proto=50 ep_in=<hex> ep_out=<hex>",
              m_if is not None, m_if.group(0) if m_if else "（缺行）")
        m_inq = re.search(r"\[USBST\] inquiry vendor=(\S+) product=(.+?)(?: rmb=[01])?$", log, re.M)
        check("★ [USBST] inquiry vendor=<..> product=<..>（QEMU usb-storage）",
              m_inq is not None, m_inq.group(0) if m_inq else "（缺行）")
        m_cap = re.search(r"\[USBST\] capacity blocks=(\d+) block_size=(\d+) bytes=(\d+) cap_mb=(\d+)",
                          log)
        check("★ [USBST] capacity blocks=<n> block_size=<n>（并换算 MB）", m_cap is not None,
              m_cap.group(0) if m_cap else "（缺行）")
        if m_cap:
            blocks, bsize, nbytes, mb = (int(m_cap.group(i)) for i in (1, 2, 3, 4))
            check("容量与宿主侧镜像一致（blocks=%d == 72048 扇区、block_size=512、cap_mb=%d）"
                  % (blocks, mb),
                  blocks == STICK_SECTORS and bsize == 512 and
                  nbytes == STICK_SECTORS * SECTOR and mb == STICK_SECTORS * SECTOR // (1024 * 1024))
        check("★ [USBST] read lba=0 count=1 ok（自检真的从 U 盘读了一块）",
              re.search(r"\[USBST\] read lba=0 count=1 ok", log) is not None)
        check("[USBST] selftest PASS mask=0",
              re.search(r"\[USBST\] selftest PASS mask=0", log) is not None)
        check("[USB64] selftest PASS（UHCI + 存储自检全过）", "[USB64] selftest PASS" in log)
        check("没有任何 [USBST] read FAILED / 写拒绝打点",
              "[USBST] read FAILED" not in log and "[USBST] write refused" not in log)

        # ---- 1b) 磁盘抽象 + 盘符 + FAT 只读挂载 ----
        print("--- 1b) 串口：驱动器号 24 / 盘符 D: / FAT32 只读挂载 ---")
        check("U 盘接入后重扫盘符（[USBST] storage attached -> rescan drive letters）",
              "[USBST] storage attached -> rescan drive letters" in log)
        # 驱动器号 24（= ATA64_USB_BASE）接入的证据：盘符表 dump 里那条 FAT32 条目
        #   （disk=24 part=1 lba=2048+70000 —— 这三项只能来自 USB 后端的 IDENTIFY/分区表）
        m_dump = re.search(r"\[DRV64\] dump\s+\[\d+\] D: .*disk=(\d+) part=(\d+) "
                           r"lba=(\d+)\+(\d+) total_kb", log)
        check("★ U 盘以驱动器号 24 接入（[DRV64] dump … D: … disk=24 part=1 lba=2048+70000）",
              m_dump is not None and int(m_dump.group(1)) == 24 and int(m_dump.group(2)) == 1
              and int(m_dump.group(3)) == STICK_PART_LBA and int(m_dump.group(4)) == STICK_PART_SECTORS,
              m_dump.group(0) if m_dump else "（缺行）")
        m_probe = re.search(r"\[FAT64\] probe lba=%d fs=FAT32 clusters=(\d+)" % STICK_PART_LBA, log)
        check("★ U 盘分区的 FAT32 只读探测（[FAT64] probe lba=2048 fs=FAT32 clusters=%d）"
              % info["clusters"],
              m_probe is not None and int(m_probe.group(1)) == info["clusters"],
              m_probe.group(0) if m_probe else "（缺行）")
        m_mount = re.search(r"\[FAT64\] mount vol=(\d+) lba=%d clusters=(\d+) free=(\S+) "
                            r"fat_ok=1 spc=1 ro=1" % STICK_PART_LBA, log)
        check("★ FAT32 只读挂载（[FAT64] mount … lba=2048 clusters=%d fat_ok=1 spc=1 ro=1）"
              % info["clusters"],
              m_mount is not None and int(m_mount.group(2)) == info["clusters"],
              m_mount.group(0) if m_mount else "（缺行）")
        m_letter = re.search(r"\[DRV64\] letter=D: disk=(\d+) part=(\d+) fs=FAT32 total_kb=(\d+) "
                             r"free_kb=(\d+) slot=(\S+) ro=(\d) fatvol=(\d+)", log)
        # 注意 total_kb 是**卷容量**（按 BPB 的簇数 x 簇大小算）而不是整盘容量：
        #   clusters(68874) x 512B / 1024 = 34437 KB。整盘容量在 [USBST] capacity 那条断言里。
        vol_kb = info["clusters"] // 2
        # ★ P8b：U 盘上的 FAT32 = **可写卷** -> ro=0（固定盘/ESP 上的仍 ro=1，见 fs_tree/fatread 两个测试）
        check("★ U 盘拿到盘符 D: 且是**可写卷**（disk=24 part=1 fs=FAT32 total_kb=%d ro=0 fatvol=…）" % vol_kb,
              m_letter is not None and int(m_letter.group(1)) == 24 and int(m_letter.group(2)) == 1
              and int(m_letter.group(3)) == vol_kb and int(m_letter.group(4)) <= vol_kb
              and m_letter.group(6) == "0",
              m_letter.group(0) if m_letter else "（缺行）")
        check("★ 写开关打开后的最终状态（[FAT64] rw mount vol=… letter=D: writable=1 clusters=%d）"
              % info["clusters"],
              re.search(r"\[FAT64\] rw mount vol=\d+ letter=D: writable=1 clusters=%d" % info["clusters"],
                        log) is not None)
        check("[DRV64] selftest PASS（重扫后盘符表自洽）", "[DRV64] selftest PASS" in log)
        print("--- 1c) 文件管理器：此电脑 -> 双击 D: 卡片 -> U 盘根目录 ---")
        mon.key("meta_l", wait=1.0)

        # ---- 1c) 文件管理器：进入 U 盘 + 像素 + 截图 ----

        mon.key("2", wait=2.5)
        check("打开文件管理器（[APP] mypc opened）", vm.wait_log("[APP] mypc opened", 25))
        elog = fst.wait_for(serial, "[UI] explorer thispc drives=", 25, vm.proc)
        check("此电脑页打点（[UI] explorer thispc drives=… browsable=…）",
              re.search(r"\[UI\] explorer thispc drives=\d+ browsable=\d+", elog) is not None)
        cards = re.findall(r"\[UI\] explorer card idx=(\d+) letter=(\S+) kind=(\S+)", elog)
        check("U 盘在卡片列表里（card idx=… letter=D: kind=browsable）",
              any(l == "D:" and k == "browsable" for _, l, k in cards), str(cards))

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
        # ★ 缺陷 2：误差标定只做一次并缓存（原来每次 aim/click 都重标 —— 单次 ~10s，一轮十几处）
        aim = Aim(vm, mon)
        aim.calibrate()

        since = vm.mark()
        ok_enter = dclick_card_until(aim, vm, mon, "D", "[UI] explorer enter letter=D:")
        check("双击 D: 卡片进入 U 盘（[UI] explorer enter letter=D: fatvol=… ok items=%d）"
              % ROOT_ITEMS, ok_enter)
        elog2 = vm.log()[since:]
        check("★ 可写卷打点（[UI] explorer drive letter=D: fs=FAT32 … ro=0；只读卷才有 'vol … ro=1 readonly'）",
              re.search(r"\[UI\] explorer drive letter=D: fs=FAT32 total_kb=%d free_kb=\d+ ro=0"
                        % vol_kb, elog2) is not None)
        check("U 盘根目录列出 %d 项（[UI] explorer nav path=/ items=%d）" % (ROOT_ITEMS, ROOT_ITEMS),
              vm.wait_nav("/", ROOT_ITEMS, 20, since))
        check("★ 长名文件在列（item name=%s）" % TXT_NAME,
              vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=file size=%d"
                          % (re.escape(TXT_NAME), len(TXT_TEXT)), 10, since))
        check("★ .vap 应用在列（item name=%s size=%d）" % (VAP_NAME, len(info["vap"])),
              vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=file size=%d"
                          % (re.escape(VAP_NAME), len(info["vap"])), 10, since))
        check("★ .elf 应用在列（item name=%s size=%d）" % (ELF_NAME, len(info["elf"])),
              vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=file size=%d"
                          % (re.escape(ELF_NAME), len(info["elf"])), 10, since))
        check("子目录在列（item name=%s type=dir）" % DOCS_DIR,
              vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=dir" % re.escape(DOCS_DIR),
                          10, since))

        shot_ok = mon.shot(shot_ppm)
        check("U 盘目录页截图", shot_ok)
        if shot_ok and os.path.exists(shot_ppm):
            w, h, px = exp.read_ppm(shot_ppm)
            check("截图分辨率 1280x800", (w, h) == (1280, 800), "%dx%d" % (w, h))
            names = exp.rect_dark(px, w, exp.sx(exp.CONTENT_X + 4), exp.sy(exp.CONTENT_Y + 4),
                                  exp.sx(exp.CONTENT_X + exp.ICON_CELL_W - 4),
                                  exp.sy(exp.CONTENT_Y + exp.ICON_CELL_H - 4))
            check("内容区画了 U 盘条目（图标单元格有暗像素）", names > 30, "暗像素=%d" % names)
            saved = save_png(shot_ppm, SHOT_PNG)
            check("截图存 PNG（%s）" % os.path.relpath(SHOT_PNG, ROOT).replace("\\", "/"),
                  saved and os.path.exists(SHOT_PNG) and os.path.getsize(SHOT_PNG) > 10000,
                  "%d 字节" % (os.path.getsize(SHOT_PNG) if os.path.exists(SHOT_PNG) else 0))

        # ---- 1d) 在 U 盘上按 Ctrl+V：只读拒绝 ----
        print("--- 1d) 只读语义：先在 C: 复制一个文件，再到 U 盘上 Ctrl+V ---")
        check("打开终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        # ★ 先切回 C:：上一步在管理器里进了 U 盘（D:），终端与管理器共用"当前卷"，
        #   不切的话 `write /via.txt` 会被只读拦下来（那是另一个证据，见下）。
        mon.type_line("vol c")
        check("终端切回 C:（[VOL] switch letter=C: …）",
              re.search(r"\[VOL\] switch letter=C:",
                        fst.wait_for(serial, "[VOL] switch letter=C:", 25, vm.proc)) is not None)
        mon.type_line("write /via.txt hello-usb")
        check("终端在 C: 写 /via.txt（[TERM] cmd write ok）",
              vm.wait_log("[TERM] cmd write ok", 25))
        mon.type_line("vol d")
        vlog = fst.wait_for(serial, "[VOL] switch letter=D:", 25, vm.proc)
        check("终端切到 U 盘 D:（[VOL] switch letter=D: …）",
              re.search(r"\[VOL\] switch letter=D:", vlog) is not None)
        # 卷只读标记（终端 vol 列表）+ 写类命令被拒。
        # 说明（如实）：在**只读卷**上写，终端/管理器在**入口**就按 fs64_is_readonly64(-1) 拦下来了
        #   （用户界面看到"read-only volume (FAT32)"），所以不会走到 fs64 的写入口、也就**不会**有
        #   `[FS64] reject … op=write` 那一行 —— 这里断言的是真正可达的三类证据：
        #   ① 终端的 vol 列表把 D: 标成 ro=1；② 写/建目录命令确实失败；③ 管理器里粘贴/删除被拒。
        since = vm.mark()
        mon.type_line("vol")
        vlist = fst.wait_for(serial, "[VOL] vol letter=", 25, vm.proc)
        check("★ 终端的卷表把 U 盘标成只读（[VOL] vol letter=D: fs=FAT32 ro=1 total_kb=…）",
              re.search(r"\[VOL\] vol letter=D: fs=FAT32 ro=1 total_kb=\d+ free_kb=\d+", vlist) is not None,
              (re.search(r"\[VOL\] vol letter=D:.*", vlist).group(0) if "vol letter=D:" in vlist else "（缺行）"))
        mon.type_line("write x.txt usb")
        wlog = fst.wait_for(serial, "[TERM] cmd write fail", 25, vm.proc)
        check("★ 往 U 盘写被拒（[TERM] cmd write fail）", "[TERM] cmd write fail" in wlog)
        mon.type_line("mkdir newdir")
        mlog = fst.wait_for(serial, "[TERM] cmd mkdir fail", 25, vm.proc)
        check("★ U 盘上建目录被拒（[TERM] cmd mkdir fail）", "[TERM] cmd mkdir fail" in mlog)

        # 管理器里：先在 D: 选中一个条目再按 Delete（只读卷 -> roact），然后回 C: 复制 via.txt
        exp.SAFE_POINT = (fo.sx(fo.CONTENT_X + fo.CONTENT_W - 40),
                          fo.sy(fo.CONTENT_Y + fo.CONTENT_H - 40))
        since = vm.mark()
        idx_d = fresh_or_last_item_idx(vm, VAP_NAME, since)
        check("U 盘条目可定位（%s idx=%s）" % (VAP_NAME, idx_d), idx_d >= 0)
        if idx_d >= 0:
            p = aim.click_hit(*fo.cell(idx_d), want_hit="item:%d" % idx_d)
            check("单击选中 U 盘条目（click hit=item:%d）" % idx_d,
                  p is not None and p[2] == "item:%d" % idx_d, str(p))
            since = vm.mark()
            mon.key("delete", wait=1.2)
            check("★ 只读卷上 Delete 被拒（[UI] explorer roact op=delete letter=D: fs:FAT32 ro=1）",
                  vm.wait_new(r"\[UI\] explorer roact op=delete letter=D: fs=FAT32 ro=1", 20, since))
            # 注：被拒的删除不会触发列表刷新（内容没变就不重打 item 行）——"文件还在"由下一步的
            #     拷贝阶段证明（它必须能重新列出并选中同一个文件）。

        # C: 里复制 via.txt（为"往只读卷粘贴"准备一个非空剪贴板）
        ok_back = back_to_thispc(aim, vm, mon)
        check("回此电脑（导航窗格/面包屑）", ok_back)
        cidx = fo.card_idx(vm, "C")
        check("C: 卡片可定位（card idx=%s）" % cidx, cidx is not None)
        since = vm.mark()
        ok_c = goto_volume(aim, vm, mon, "C")
        check("双击 C: 进入系统盘根目录", ok_c)
        idx_via = fresh_or_last_item_idx(vm, "via.txt", since)
        check("C: 根目录里能看到 via.txt（idx=%s）" % idx_via, idx_via >= 0)
        if idx_via >= 0:
            p = aim.click_hit(*fo.cell(idx_via), want_hit="item:%d" % idx_via)
            check("单击选中 via.txt（click hit=item:%d）" % idx_via,
                  p is not None and p[2] == "item:%d" % idx_via, str(p))
            since = vm.mark()
            mon.key("ctrl-c", wait=1.2)
            check("Ctrl+C 把 via.txt 收进剪贴板（clip op=copy n=1）",
                  vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since))
            since = vm.mark()
            ok_d = goto_volume(aim, vm, mon, "D")
            check("再进 U 盘（D:）", ok_d)
            since = vm.mark()
            mon.key("ctrl-v", wait=1.5)
            check("★ 往 U 盘粘贴被拒（[UI] explorer roact op=paste letter=D: fs=FAT32 ro=1 + paste ok n=0）",
                  vm.wait_new(r"\[UI\] explorer roact op=paste letter=D: fs=FAT32 ro=1", 25, since) and
                  vm.wait_new(r"\[UI\] explorer paste ok n=0 dst=/ skipped=1", 15, since))

        # ---- 1e) ★ 从 U 盘把两个应用拷进 C:（选中 -> Ctrl+C -> 进 C: -> Ctrl+V）----
        print("--- 1e) ★ 从 U 盘拷贝：STICKELF.ELF + STICKAPP.VAP -> C: 根目录 ---")
        copied = []
        for name, size in ((ELF_NAME, len(info["elf"])), (VAP_NAME, len(info["vap"]))):
            since = vm.mark()
            if not goto_volume(aim, vm, mon, "D"):
                check("拷贝 %s：进 U 盘 D:" % name, False)
                continue
            idx = fresh_or_last_item_idx(vm, name, since)
            check("U 盘根目录里找到 %s（idx=%s）" % (name, idx), idx >= 0)
            if idx < 0:
                continue
            p = aim.click_hit(*fo.cell(idx), want_hit="item:%d" % idx)
            check("单击选中 %s（click hit=item:%d）" % (name, idx),
                  p is not None and p[2] == "item:%d" % idx, str(p))
            since = vm.mark()
            mon.key("ctrl-c", wait=1.2)
            if not vm.wait_new(r"\[UI\] explorer clip op=copy n=1", 20, since):
                check("Ctrl+C 复制 %s" % name, False, "（没看到 clip op=copy）")
                continue
            since = vm.mark()
            if not goto_volume(aim, vm, mon, "C"):
                check("拷贝 %s：回此电脑并进 C:" % name, False)
                continue
            since = vm.mark()
            mon.key("ctrl-v", wait=1.6)
            ok_paste = vm.wait_new(r"\[UI\] explorer paste ok n=1 dst=/ skipped=0", 30, since)
            check("★ 粘贴 %s 到 C: 根（paste ok n=1 dst=/ skipped=0）" % name, ok_paste)
            check("粘贴出来的条目在列（item name=%s size=%d）" % (name, size),
                  vm.wait_new(r"\[UI\] explorer item idx=\d+ name=%s type=file size=%d"
                              % (re.escape(name), size), 20, since))
            copied.append((name, size))

        # 终端读回（内核视角的字节数）：先把焦点交回终端 —— 这时前台是资源管理器，
        # 不重新点一下的话 sendkey 全被管理器吃掉（实测：命令一条都没进终端）。
        # 注意 `cat` 是"看文本"的命令，单次最多显示 4096 B（kernel/terminal64.cpp 的 CAT_MAX_OUT，
        # 超了会明确提示截断）—— 所以这里期望 min(size, 4096)；9KB 的 .elf 的**逐字节一致性**
        # 由阶段 3 的宿主侧卷解析核对（那才是无上限的真比对）。
        if copied:
            check("把焦点交回终端（[APP] term opened）", fst.open_terminal(mon, serial, vm.proc))
        for name, size in copied:
            want = min(size, 4096)
            type_line_ex(mon, "vol c")
            fst.wait_for(serial, "[VOL] switch letter=C:", 20, vm.proc)
            since = vm.mark()
            type_line_ex(mon, "cat /" + name)
            # ★ 缺陷 1：等**这条命令自己的**新打点。旧写法用 `wait_for(serial, "cmd cat bytes=")`
            #   等的是"日志里有没有 cat 行"——上一轮的 `cmd cat bytes=4096` 已经在了，于是立刻
            #   返回、再用 bytes=240 去匹配老日志 -> 240 B 的文件被误报成"输出了 4096 B"
            #   （详情行打印的是上一轮 ELF 的 4096）。现在：只看 since 之后的新行，且
            #   path + bytes 双证据必须与真实文件大小一致（判据不变，cat 上限 4096 也一样）。
            clog = ""
            for _w in range(150):                       # ≤30s（落盘延迟 1~3s + 命令执行）
                seg = vm.log()[since:]
                if "cmd cat path=" in seg or "cmd cat bytes=" in seg:
                    clog = seg
                    break
                time.sleep(0.2)
            mp = re.search(r"\[TERM\] cmd cat path=(\S+) bytes=(\d+)\b", clog)
            ml = re.search(r"\[TERM\] cmd cat bytes=(\d+) total=(\d+) truncated=(\d)\b", clog)
            ok_cat = (mp is not None and mp.group(1) == "/" + name and int(mp.group(2)) == want
                      and ml is not None and int(ml.group(1)) == want)
            check("★ 终端 cat /%s 读回 %d 字节（内核视角；cat 上限 4096 B）" % (name, want), ok_cat,
                  (mp.group(0) if mp else (ml.group(0) if ml else "（缺行）")))
        print("--- 禁止项 ---")
        forbid("阶段 1", vm.log())
        check("U 盘上没有任何字节被写（没有 [USBST] write refused）",
              "[USBST] write refused" not in vm.log())
    except SkipPhase:
        print("      （--phase %s：跳过阶段 1）" % args.phase)
    finally:
        vm.close()
        time.sleep(1.0)

    # ==================== 阶段 2：键盘 + U 盘同时插（回归） ====================
    print("=== 阶段 2：键盘 + U 盘同时插（HID 键盘不能因为多设备而坏）===")
    port2 = fst.free_port()
    vm2 = Vm(qemu, sys_disk, stick, port2, serial2, "Vimtu64-usbstorage-kbd", with_kbd=True)
    print("      %s" % vm2.cmdline)
    try:
        if not ph2:
            raise SkipPhase()
        # ★ 登录手势（阶段 2 同一个盘：同样必须显式回车）
        qh.login_desktop(exp.Monitor(port2), vm2.log, vm2.proc, timeout=180)
        check("桌面就绪（[GUI64] ready）", vm2.wait_log("[GUI64] ready", 200))
        log2 = vm2.log()
        check("★ 键盘仍然枚举成功（[USB64] config set value=1 ifaces=1 hid=1 ep_in=.. mps=8）",
              re.search(r"\[USB64\] config set value=1 ifaces=1 hid=1 ep_in=[0-9A-F]{2} mps=8",
                        log2) is not None)
        check("键盘引导协议设置（hid boot protocol set）", "[USB64] hid boot protocol set (8-byte reports)" in log2)
        check("★ U 盘也枚举成功（[USBST] iface found + capacity）",
              "[USBST] iface found class=08 sub=06 proto=50" in log2 and
              re.search(r"\[USBST\] capacity blocks=%d block_size=512" % STICK_SECTORS, log2) is not None)
        check("两台设备各拿一个地址（set address=1 ok + set address=2 ok）",
              "[USB64] set address=1 ok" in log2 and "[USB64] set address=2 ok" in log2)
        check("U 盘还是 D:（[DRV64] letter=D: disk=24 … fs=FAT32）",
              re.search(r"\[DRV64\] letter=D: disk=24 part=1 fs=FAT32", log2) is not None)
        check("[USBST] selftest PASS mask=0 + [USB64] selftest PASS",
              "[USBST] selftest PASS mask=0" in log2 and "[USB64] selftest PASS" in log2)
        forbid("阶段 2", log2)
    except SkipPhase:
        print("      （--phase %s：跳过阶段 2）" % args.phase)
    finally:
        vm2.close()
        time.sleep(0.5)

    # ==================== 阶段 3：宿主侧逐字节核对 ====================
    if not ph3 or not ph1:
        print("=== 跳过宿主侧核对（--phase %s）===" % args.phase)
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1
    print("=== 阶段 3：宿主侧解析（C: 卷内容 + U 盘未被写）===")
    with open(sys_disk, "rb") as f:
        cbuf = f.read()
    vol = mv.vfs3_vol(cbuf, fst.PART_MAIN_LBA)
    check("宿主侧认出 C: 的 VimtuFS2 v3 卷（'VIMTUFS2' + 55AA）", vol is not None)
    if vol is not None:
        for name, src in ((ELF_NAME, info["elf"]), (VAP_NAME, info["vap"])):
            ino = mv.vfs3_find(cbuf, vol, name)
            check("★ 宿主侧在 C: 卷里找到 /%s（size=%d）" % (name, len(src)),
                  ino is not None and ino["size"] == len(src),
                  ("size=%d" % ino["size"]) if ino else "（没有这个 inode）")
            if ino is not None:
                got = mv.vfs3_read(cbuf, vol, ino["idx"])
                check("★★ 从 U 盘拷进来的 /%s 与源**逐字节一致**（%d 字节，crc32 相同）"
                      % (name, len(src)),
                      got == src, "内核 %08X / 宿主源 %08X" % (zlib.crc32(got) & 0xFFFFFFFF,
                                                              zlib.crc32(src) & 0xFFFFFFFF))
    with open(stick, "rb") as f:
        stick_after = f.read()
    h0 = zlib.crc32(stick_before) & 0xFFFFFFFF
    h1 = zlib.crc32(stick_after) & 0xFFFFFFFF
    # ★ P8b：判据**改了，语义更强** —— 以前是"整根镜像字节不变"（只读），现在是：
    #   ① 镜像**变了**（真的写了盘）② 原有文件的数据簇**逐字节未变** ③ 新文件内容逐字节对得上。
    check("★ U 盘镜像（%d 字节 = %.1fMB）测试前后 CRC32 **已变化**（可写卷：真的写了盘）"
          % (len(stick_before), len(stick_before) / 1048576.0), h0 != h1,
          "before=%08X after=%08X" % (h0, h1))
    base_off = STICK_PART_LBA * SECTOR + info["data_start"] * SECTOR
    changed = []
    for nm, (c0, n) in info["map"].items():
        if nm == "root":
            continue
        for k in range(n):
            o = base_off + ((c0 - 2) + k) * SECTOR
            if stick_before[o:o + SECTOR] != stick_after[o:o + SECTOR]:
                changed.append((nm, k))
    check("★★ 原有文件（长名文本 / .vap / .elf / docs/notes.txt）的数据簇**逐字节未变**",
          not changed, "被改动=%s" % (changed[:6],))
    ents = []
    for i in range(16):
        e = stick_after[base_off + i * 32:base_off + (i + 1) * 32]
        if len(e) < 32 or e[0] == 0x00:
            break
        if e[0] == 0xE5 or e[11] == 0x0F:
            continue
        nm = e[0:8].decode("latin-1").rstrip(" ") + "." + e[8:11].decode("latin-1").rstrip(" ")
        fc = struct.unpack_from("<H", e, 26)[0] | (struct.unpack_from("<H", e, 20)[0] << 16)
        sz = struct.unpack_from("<I", e, 28)[0]
        ents.append((nm, fc, sz))
    got = [x for x in ents if x[0] == WR_NAME.upper()]
    check("★ 宿主侧在 U 盘根目录里找到终端写的新文件 /%s（size=%d）" % (WR_NAME.upper(), len(WR_TEXT)),
          len(got) == 1 and got[0][2] == len(WR_TEXT), str(ents))
    if len(got) == 1:
        off = base_off + (got[0][1] - 2) * SECTOR
        data = stick_after[off:off + got[0][2]]
        check("★★ /%s 的内容与客人写的**逐字节一致**（%d 字节）" % (WR_NAME.upper(), len(WR_TEXT)),
              data == WR_TEXT, "盘上=%r" % data[:48])
    check("★ 原有四个条目仍在根目录里（长名文本 / .vap / .elf / docs）",
          all(any(x[0].startswith(n.split(".")[0][:6].upper()) for x in ents)
              for n in (TXT_NAME, VAP_NAME, ELF_NAME, DOCS_DIR)), str([x[0] for x in ents]))

    if args.keep:
        print("[usbstorage] 串口：%s / %s / 截图：%s" % (serial, serial2, shot_ppm))
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
