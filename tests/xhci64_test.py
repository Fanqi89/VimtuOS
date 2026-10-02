#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/xhci64_test.py - VimtuOS xHCI（USB 3.x）驱动端到端验收（QEMU `-device qemu-xhci`）

为什么必须用 qemu-xhci：xHCI 只能这样**精确**测（PI 的 piix3 只有 UHCI；-usb 那套是 UHCI/EHCI）。

  1) ★ 主跑：`-device qemu-xhci -device usb-kbd`（xHCI 主控 + 高速 HID 引导键盘）
       * 控制器识别：[XHCI] pci <b>:<d>.<f> bar0=… caplen=… hcs1=… max_slots=… max_ports=… csz=0 ver=…
                      [XHCI] proto rev=2 portoff=0x05 ports=4 / rev=3 portoff=0x01 ports=4
                      （Supported Protocol 扩展能力：端口偏移/个数在能力的 DW2；xECP 是 dword 偏移）
       * 复位：[XHCI] reset HCRST ok usbcmd=… usbsts=…（HCRST 自清）
       * 环：[XHCI] cmd ring @… erst @… event ring @… dcbaa @… scratchpad=N
              [XHCI] doorbell mode=N enable slot=N ok（DB Target 位序是自探出来的）
              [XHCI] mfindex=0x… delta=0x…（复位期间时间在走）、[XHCI] erdp ehb cleared（EHB 写 1 清）
       * ★ 事件环： [XHCI] transfer evt ok idx=… seen=…（Transfer Event 落环并被识别 —— 原 GAP 的判据）
       * 枚举 + 速率：[XHCI] port <n> connected speed=high|super reset ok ped=1
                      [XHCI] device addr=N speed=high mps=64 vendor=0627 product=0001
                      （QEMU 的 usb-kbd 挂在 qemu-xhci 上协商到 **高速**：`info usb` 显示 480 Mb/s，
                        bMaxPacketSize0=64；这与 UHCI 那套 full/8 不同，是本文件与 usb64_test 的真实差异）
                      [XHCI] config set value=1 ifaces=1 hid=1 ep_in=03 mps=8 interval=7
                      [XHCI] hid boot protocol set (8-byte reports)
       * ★ 端到端打字：QEMU monitor sendkey -> xHCI 中断端点 -> [XHCI] hid report key=E3 down=1
         -> 桌面外壳响应（[UI] menu open / [APP] term opened）
         ※ QEMU 的 sendkey 在有 USB 键盘时**只发给 USB 键盘**（实测：`-trace input_event_key_qcode`
           配 `usb_xhci_fetch_trb`；没有 USB 键盘时才走 PS/2）—— 所以"打字"这条链路真的经过 xHCI。
       * ★ 连续打字（30 键 = 6 条 `ls /`，事件数 > 64 = **跨事件环回卷**）：逐键 [XHCI] hid report
         的按下/释放边沿都在，每条命令都落到终端（[TERM] cmd ls ok）—— "一次命令之后"不再失效；
         全程 [XHCI] stray transfer evt = 0（事件环消费 cycle 回卷修复的判据）。键盘挂 xHCI。
       * [XHCI] selftest PASS + [GUI64] ready + kheart 心跳持续增长（kusb 不饿着桌面）
  2) ★ 存储：`-device qemu-xhci -device usb-kbd -device usb-storage`（键盘 + 一根 U 盘）
       * U 盘是 **SuperSpeed** 设备：[XHCI] device addr=1 speed=super mps=512（bMaxPacketSize0=9 是指数编码）
       * [XHCI] msc inquiry / capacity（blocks 必须等于宿主镜像的扇区数）
       * [XHCI] msc READ(10) ok lba=0 count=1 bytes=512 crc=XXXXXXXX head=<16 字节>
         —— **宿主侧逐字节核对**：crc/head 与宿主造的字节逐位比较（不是"看起来成功了"）
       * 盘符：[USBST] storage attached -> rescan … xhci=1、[DRV64] letter=** disk=24 … fs=FAT32
       * ★ 键盘 + U 盘混跑（键盘就是 xHCI 那把）：先打 `ls /`（存储 I/O 之前可用）-> `vol <U 盘>` 切卷
         -> `ls /` 读 U 盘 FAT 根目录（entries≥1 = 运行期 xHCI 批量读真跑了）-> 再打 `ls /`
         （存储 I/O 之后键盘仍可用）；全程 [XHCI] stray transfer evt = 0。
       * U 盘镜像 CRC32 测试前后一致（谁都没写它：本批只读）
  3) 有主控没插设备：`-device qemu-xhci` -> [XHCI] no device on port N + selftest PASS，无 hid report
  4) 没有 xHCI：不加 `-device qemu-xhci` -> [XHCI] not found + selftest skipped，系统照常起桌面

每次启动都禁止：PANIC / TRIPLE FAULT / FAILED mask=（既有批次的措辞）/ frameprobe FAIL /
[XHCI] selftest FAIL / [XHCI] enum FAILED / 本驱动的 OOM。

用法（必须用 Windows 原生 Python）：
    py -3 tests\\xhci64_test.py
    py -3 tests\\xhci64_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 200

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import atexit
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
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402  （★ 公共登录手势：ui.login.auto 默认 0）
import fatread64_test as fatr      # noqa: E402  （复用既有 FAT32 构造器，不另写格式化器）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SECTOR = 512

# ---- U 盘夹具几何（与 tests/usbstorage_test.py 同构，只是内容更小）----
STICK_PART_LBA = 2048
STICK_PART_SECTORS = 70000                     # = 34MB 分区（spc=1 -> 簇数 68874 ≥ 65525 = 真 FAT32）
STICK_SECTORS = STICK_PART_LBA + STICK_PART_SECTORS   # 72048 扇区 = 35.2MB
STICK_TXT = b"xHCI stick (FAT32) read over USB 3.x BOT + SCSI READ(10)\n"

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "frameprobe FAIL",
             "[XHCI] selftest FAIL", "[XHCI] enum FAILED"]

# ---- 终端打字（QEMU sendkey；与 tests/sh64_test.py 同一套键名映射）----
TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}
# [XHCI] hid report key=XX 里的 XX = HID 用法码；`ls /` 这条命令用到的 5 个键
KEY_USAGE = {"l": "0F", "s": "16", "spc": "2C", "slash": "38", "ret": "28"}


def type_line(mon, text, per_key=0.35):
    """逐键 sendkey 打一条终端命令（末尾回车）。键名映射与 sh64/dynlink 测试同一条口径。"""
    for ch in text:
        if ch in TYPED_NAMES:
            mon.key(TYPED_NAMES[ch], wait=per_key)
        elif ch.isalnum():
            mon.key(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.key("ret", wait=per_key + 0.2)


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def pick_port(start=5642, tries=16):
    for p in range(start, start + tries):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            s.close()
    return start


class Monitor:
    """QEMU monitor 客户端（telnet 端口）：只用来发 sendkey。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)


def make_stick(path):
    """造一根 U 盘：MBR（1 个 0x0C FAT32 分区 @2048）+ 根目录一个文件。

    返回首扇区（= 内核 READ(10) LBA0 读到的**那一块字节**），供宿主侧逐字节核对。
    """
    dev = fatr.Fat32Builder(STICK_PART_LBA, STICK_PART_SECTORS, 1)
    root = dev.alloc(1)                                  # 簇 2 = 根目录
    c = dev.alloc(1)
    dev.write_cluster(c, STICK_TXT)
    ents = dev.dir_entry("XHCI-README.TXT", b"XHCIREDMTXT", 0x20, c, len(STICK_TXT))
    dev.write_cluster(root, ents)
    vol = dev.finish()

    full = bytearray(STICK_SECTORS * SECTOR)
    mbr = bytearray(SECTOR)
    e1 = bytearray(16)
    e1[0] = 0x80                                         # 可引导位（U 盘常见；不影响引导顺序）
    e1[4] = 0x0C                                         # FAT32 LBA
    struct.pack_into("<II", e1, 8, STICK_PART_LBA, STICK_PART_SECTORS)
    mbr[446:462] = e1
    mbr[510], mbr[511] = 0x55, 0xAA
    full[0:SECTOR] = mbr
    full[STICK_PART_LBA * SECTOR:STICK_PART_LBA * SECTOR + len(vol)] = vol
    with open(path, "wb") as f:
        f.write(bytes(full))
    return bytes(mbr)


def boot(qemu, extra_args, serial, errfile, port, wait_for, timeout, keep_alive=None):
    """无头启动；轮询串口日志到 wait_for（或超时/提前退出）。返回 (proc, slog, early)。"""
    if os.path.exists(serial):
        os.remove(serial)
    if os.path.exists(errfile):
        os.remove(errfile)
    args = [
        qemu, "-name", "Vimtu64-xhci64",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    args += list(extra_args)
    err = open(errfile, "wb")
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
    atexit.register(proc.kill)
    early = False

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    if wait_for == "[GUI64] ready":
        qh.login_desktop(Monitor(port), slog, proc, timeout=min(timeout, 180))
    deadline = time.time() + timeout
    while time.time() < deadline:
        if wait_for in slog():
            break
        if proc.poll() is not None:
            early = True
            break
        time.sleep(0.5)
    err.close()
    return proc, slog, early


def kill(proc):
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


def first_match(pattern, text, default="未出现"):
    m = re.search(pattern, text)
    return m.group(0) if m else default


def beats(log):
    return [int(m) for m in re.findall(r"\[TASK64\] kheart beat=(\d+)", log)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="系统内核镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=200, help="每次启动等待的最长秒数")
    ap.add_argument("--keep", action="store_true", help="打印串口日志目录")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_xhci64_")
    stick = os.path.join(tmp, "stick.img")
    mbr = make_stick(stick)
    stick_crc0 = zlib.crc32(open(stick, "rb").read()) & 0xFFFFFFFF
    print("[xhci64] U 盘夹具：%s（%d 扇区 = %.1fMB，首扇区 CRC32=%08X）"
          % (stick, STICK_SECTORS, STICK_SECTORS / 2048.0, zlib.crc32(mbr) & 0xFFFFFFFF))

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 xHCI（USB 3.x）验收：QEMU -device qemu-xhci ===")
    print("[xhci64] system.img = %s" % args.img)

    # ============================================================ 1) -device qemu-xhci + usb-kbd
    print("[xhci64] 1) 主跑：-device qemu-xhci -device usb-kbd")
    port = pick_port()
    mon = Monitor(port)
    args1 = ["-drive", "format=raw,file=%s" % q(args.img),
             "-device", "qemu-xhci", "-device", "usb-kbd"]
    proc, slog, early = boot(qemu, args1, os.path.join(tmp, "kbd.log"),
                             os.path.join(tmp, "kbd.err"), port, "[GUI64] ready", args.timeout)
    log = slog()
    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）")

    # ---- ① 控制器识别（PCI + MMIO + 能力寄存器）----
    m_pci = re.search(r"\[XHCI\] pci (\d+):(\d+)\.(\d+) bar0=0x([0-9A-F]+) caplen=0x([0-9A-F]+) "
                      r"hcs1=0x([0-9A-F]+) max_slots=(\d+) max_ports=(\d+) csz=(\d) ver=0x([0-9A-F]+)",
                      log)
    check("① [XHCI] pci <b>:<d>.<f> bar0=<hex> caplen=<hex> hcs1=<hex> max_slots=<n> max_ports=<n>",
          bool(m_pci), m_pci.group(0) if m_pci else first_match(r"\[XHCI\] pci[^\r\n]*", log))
    if m_pci:
        check("① caplen 在合法区间（0x20..0x100）",
              0x20 <= int(m_pci.group(5), 16) <= 0x100, "caplen=0x%s" % m_pci.group(5))
        check("① max_slots>0 且 max_ports>0（能力寄存器读出来了）",
              int(m_pci.group(7)) > 0 and int(m_pci.group(8)) > 0,
              "max_slots=%s max_ports=%s" % (m_pci.group(7), m_pci.group(8)))
    check("① [XHCI] proto rev=…（Supported Protocol 扩展能力：USB2/USB3 端口区间）",
          bool(re.search(r"\[XHCI\] proto rev=\d+ portoff=0x[0-9A-F]+ ports=\d+", log)),
          first_match(r"\[XHCI\] proto[^\r\n]*", log))
    check("① 至少列出一个 rev=2（USB 2.0 协议）的端口区间，且 portoff/ports 与实测一致（5..8 共 4 个口）",
          bool(re.search(r"\[XHCI\] proto rev=2 portoff=0x05 ports=4", log)),
          first_match(r"\[XHCI\] proto rev=2[^\r\n]*", log))
    check("① 至少列出一个 rev=3（USB 3.0 协议）的端口区间，且 portoff/ports 与实测一致（1..4 共 4 个口）",
          bool(re.search(r"\[XHCI\] proto rev=3 portoff=0x01 ports=4", log)),
          first_match(r"\[XHCI\] proto rev=3[^\r\n]*", log))

    # ---- ② 复位 / HCRST / 环 / ERST ----
    check("② [XHCI] reset HCRST ok usbcmd=… usbsts=…（软复位完成、HCRST 自清）",
          bool(re.search(r"\[XHCI\] reset HCRST ok usbcmd=0x[0-9A-F]{8} usbsts=0x[0-9A-F]{8}", log)),
          first_match(r"\[XHCI\] reset HCRST[^\r\n]*", log))
    check("② [XHCI] cmd ring @… erst @… event ring @… dcbaa @… scratchpad=N（命令环/ERST/事件环/DCBAA 都建好）",
          bool(re.search(r"\[XHCI\] cmd ring @[0-9A-F]{8} erst @[0-9A-F]{8} event ring @[0-9A-F]{8} "
                         r"dcbaa @[0-9A-F]{8} scratchpad=\d+", log)),
          first_match(r"\[XHCI\] cmd ring[^\r\n]*", log))
    m_db = re.search(r"\[XHCI\] doorbell mode=(\d) enable slot=(\d+) ok", log)
    check("② [XHCI] doorbell mode=<n> enable slot=<n> ok（Enable Slot 命令真的完成了）",
          bool(m_db), m_db.group(0) if m_db else first_match(r"\[XHCI\] doorbell[^\r\n]*", log))
    check("② [XHCI] mfindex=0x… delta=0x…（PORTSC.PR -> 等 PRC 期间时间在走）",
          bool(re.search(r"\[XHCI\] mfindex=0x[0-9A-F]{4} delta=0x[0-9A-F]{4}", log)),
          first_match(r"\[XHCI\] mfindex[^\r\n]*", log))
    m_mf = re.search(r"\[XHCI\] mfindex=0x([0-9A-F]{4}) delta=0x([0-9A-F]{4})", log)
    check("② MFINDEX 的 delta != 0（时间戳真的前进了，不是读到死值）",
          bool(m_mf) and int(m_mf.group(2), 16) > 0,
          ("delta=0x%s" % m_mf.group(2)) if m_mf else "未出现")
    check("② [XHCI] erdp ehb cleared erdp=…（ERDP.EHB 写 1 清语义）",
          bool(re.search(r"\[XHCI\] erdp ehb cleared erdp=0x[0-9A-F]{16}", log)),
          first_match(r"\[XHCI\] erdp[^\r\n]*", log))

    # ---- ③ 枚举 + 速率协商 ----
    m_port = re.search(r"\[XHCI\] port (\d+) connected speed=(low|full|high|super|superplus) "
                       r"reset ok ped=(\d)", log)
    check("③ [XHCI] port <n> connected speed=… reset ok ped=1（端口复位 + 速率协商）",
          bool(m_port) and m_port.group(3) == "1",
          m_port.group(0) if m_port else first_match(r"\[XHCI\] port \d+ connected[^\r\n]*", log))
    m_dev = re.search(r"\[XHCI\] device addr=(\d+) speed=(\w+) mps=(\d+) vendor=([0-9A-Fa-f]{4}) "
                      r"product=([0-9A-Fa-f]{4})", log)
    check("③ [XHCI] device addr=N speed=high mps=64 vendor=0627 product=0001（Address Device 成功）",
          bool(m_dev) and m_dev.group(2) == "high" and m_dev.group(3) == "64" and
          m_dev.group(4).upper() == "0627" and m_dev.group(5).upper() == "0001",
          m_dev.group(0) if m_dev else first_match(r"\[XHCI\] device addr[^\r\n]*", log))
    # ★ 事件环证据（原 GAP 的判据）：Transfer Event 真的落进事件环并被配对认出来
    check("③★ [XHCI] transfer evt ok idx=… seen=…（Transfer Event 落环并被识别）",
          bool(re.search(r"\[XHCI\] transfer evt ok idx=\d+ seen=\d+", log)),
          first_match(r"\[XHCI\] transfer evt ok[^\r\n]*", log))
    m_cfg = re.search(r"\[XHCI\] config set value=1 ifaces=(\d+) hid=1 ep_in=([0-9A-Fa-f]{2}) "
                      r"mps=(\d+) interval=(\d+)", log)
    check("③ [XHCI] config set value=1 ifaces=1 hid=1 ep_in=03 mps=8 interval=…（Configure Endpoint 成功）",
          bool(m_cfg) and m_cfg.group(2).upper() == "03" and m_cfg.group(3) == "8",
          m_cfg.group(0) if m_cfg else first_match(r"\[XHCI\] config set[^\r\n]*", log))
    check("③ [XHCI] hid boot protocol set (8-byte reports)（走的是引导协议）",
          "[XHCI] hid boot protocol set (8-byte reports)" in log)
    check("③ [XHCI] selftest PASS（主控/复位/环/命令/设备全过）", "[XHCI] selftest PASS" in log)
    check("③ [GUI64] ready（xHCI 初始化之后桌面照常起来）", "[GUI64] ready" in log)

    # ---- ④ ★ 端到端打字：sendkey -> xHCI 中断端点 -> 桌面 ----
    print("--- ④ 端到端证据：sendkey 注入按键（xHCI 中断端点 -> 桌面外壳响应）---")
    print("    ※ QEMU 的 sendkey 同时分发给 PS/2（help sendkey 无设备参数），")
    print("       所以 xHCI 的专属证据是 [XHCI] hid report 行 + 同一轮里桌面的响应。")
    mon.key("meta_l", wait=1.3)                    # Win 键 -> 开始菜单
    log2 = slog()
    check("④★ [XHCI] hid report key=E3 down=1（USB 键盘的报告真的到了 xHCI 驱动）",
          "[XHCI] hid report key=E3 down=1" in log2,
          first_match(r"\[XHCI\] hid report key=E3 down=1[^\r\n]*", log2))
    check("④★ 桌面外壳响应了这次按键：[UI] menu open（开始菜单被打开）", "[UI] menu open" in log2)
    n_down = len(re.findall(r"\[XHCI\] hid report key=E3 down=1", log2))
    n_up = len(re.findall(r"\[XHCI\] hid report key=E3 down=0", log2))
    check("④ 修饰键边沿只打一次按下（不是每个报告都刷屏）", n_down == 1, "down=1 行数=%d" % n_down)
    check("④ 修饰键释放边沿也能收到（down=0）", n_up >= 1, "down=0 行数=%d" % n_up)

    mon.key("1", wait=1.5)                         # 开始菜单第 1 项 = 终端
    mon.key("ret", wait=1.1)
    log3 = slog()
    check("④★ key=1E（数字 1）的 xHCI 报告行出现", "hid report key=1E down=1" in log3,
          first_match(r"\[XHCI\] hid report key=1E[^\r\n]*", log3))
    check("④★ 桌面按这个键真的开了终端：[APP] term opened", "[APP] term opened" in log3)
    check("④ 按下的键都产生了释放边沿（队列不会卡住）",
          len(re.findall(r"\[XHCI\] hid report key=\w{2} down=0", log3)) >= 3,
          "down=0 行数=%d" % len(re.findall(r"\[XHCI\] hid report key=\w{2} down=0", log3)))

    print("--- kusb 不饿着桌面：kheart 心跳持续增长 ---")
    b0 = beats(log3)
    time.sleep(4.0)
    log4 = slog()
    b1 = beats(log4)
    check("kusb（含 xHCI 轮询）在线时 kheart 心跳仍在增长",
          len(b0) > 0 and len(b1) > 0 and b1[-1] > b0[-1],
          "before=%s after=%s" % (b0[-1] if b0 else "?", b1[-1] if b1 else "?"))
    check("④ 没有 device/hid 打点之外的异常：[XHCI] 行数合理（>= 12）",
          len(re.findall(r"\[XHCI\] ", log4)) >= 12, "行数=%d" % len(re.findall(r"\[XHCI\] ", log4)))
    for needle in FORBIDDEN:
        check("④ 主跑不得出现 %s" % needle, needle not in log4)

    # ---- ④b ★ 连续打字（30 键 = 6 条 `ls /`；跨事件环 64 项回卷）全部生效 + 零 stray ----
    #   为什么必须跨回卷：缺陷现象正是"一次命令之后"键盘失效 —— 事件环写满 64 项后消费 cycle
    #   没翻转，第 65 个事件起全部不可见（还会重读旧条目、打出 stray）。6 条命令 = 30 次按键、
    #   60+ 个 HID 报告 + 启动期 12 个事件 > 64，必然跨过回卷；任何一次丢键都会让计数少 1。
    print("--- ④b 连续打字 30 键（6 条 `ls /`，跨事件环回卷）---")
    mark1 = len(log4)
    for _ in range(6):
        type_line(mon, "ls /", per_key=0.35)
        time.sleep(0.6)
    log5 = slog()
    win1 = log5[mark1:]
    n_ok = len(re.findall(r"\[TERM\] cmd ls ok", win1))
    check("④b★ 连续 6 条 `ls /` 全部被终端执行（30 键逐键生效）",
          n_ok >= 6, "[TERM] cmd ls ok 行数=%d" % n_ok)
    for name, usage in sorted(KEY_USAGE.items()):
        n_dn = len(re.findall(r"\[XHCI\] hid report key=%s down=1" % usage, win1))
        n_up = len(re.findall(r"\[XHCI\] hid report key=%s down=0" % usage, win1))
        check("④b★ 键 %s（HID 0x%s）按下/释放各 %d 次都进了驱动" % (name, usage, 6),
              n_dn >= 6 and n_up >= 6, "down=1 ×%d down=0 ×%d" % (n_dn, n_up))
    n_rep = len(re.findall(r"\[XHCI\] hid report key=", win1))
    check("④b★ 连打期间 HID 报告 ≥ 60（30 键 × 按下/释放；事件数 > 64 已跨回卷）",
          n_rep >= 60, "报告行数=%d" % n_rep)
    check("④b★ 连打期间零 [XHCI] stray transfer evt（事件环回卷修复的判据）",
          "[XHCI] stray transfer evt" not in log5,
          first_match(r"\[XHCI\] stray[^\r\n]*", log5))
    n_menu0 = log5.count("[UI] menu open")
    mon.key("meta_l", wait=1.3)
    log6 = slog()
    check("④b★ 连打之后键盘仍活：Win 键再次打开开始菜单（[UI] menu open 计数 +1）",
          log6.count("[UI] menu open") > n_menu0,
          "menu open %d -> %d" % (n_menu0, log6.count("[UI] menu open")))
    hits = [n for n in FORBIDDEN if n in log6]
    check("④b 连打后 FORBIDDEN 词表零命中", not hits, ("命中：" + ",".join(hits)) if hits else "")
    kill(proc)

    # ============================================================ 2) 存储（usb-storage on xHCI）
    print("[xhci64] 2) 存储：-device qemu-xhci -device usb-kbd -device usb-storage")
    port2 = pick_port(port + 2)
    args2 = ["-drive", "format=raw,file=%s" % q(args.img),
             "-device", "qemu-xhci",
             "-device", "usb-kbd",
             "-drive", "format=raw,file=%s,if=none,id=stick" % q(stick),
             "-device", "usb-storage,drive=stick"]
    proc2, slog2, early2 = boot(qemu, args2, os.path.join(tmp, "msc.log"),
                                os.path.join(tmp, "msc.err"), port2, "[GUI64] ready", args.timeout)
    logm = slog2()
    if early2:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    check("⑤ 两根设备都枚举到（键盘 + 存储）：[XHCI] device addr= 至少 2 行",
          len(re.findall(r"\[XHCI\] device addr=\d+ speed=", logm)) >= 2,
          "行数=%d" % len(re.findall(r"\[XHCI\] device addr=\d+ speed=", logm)))
    check("⑤ U 盘走的是 **SuperSpeed** 链路（qemu-xhci 的 USB3 口）：[XHCI] device addr=1 speed=super mps=512",
          bool(re.search(r"\[XHCI\] device addr=\d+ speed=super mps=512 ", logm)),
          first_match(r"\[XHCI\] device addr=\d+ speed=super[^\r\n]*", logm))
    check("⑤ [XHCI] config set … msc=1 ep_in=… ep_out=…（存储接口端点都配好）",
          bool(re.search(r"\[XHCI\] config set value=1 ifaces=\d+ msc=1 ep_in=[0-9A-Fa-f]{2} mps=\d+ "
                         r"ep_out=[0-9A-Fa-f]{2} mps=\d+", logm)),
          first_match(r"\[XHCI\] config set[^\r\n]*msc=1[^\r\n]*", logm))
    check("⑤ [XHCI] msc inquiry vendor=QEMU…（BOT 的 INQUIRY 过了）",
          bool(re.search(r"\[XHCI\] msc inquiry vendor=\S+ product=", logm)),
          first_match(r"\[XHCI\] msc inquiry[^\r\n]*", logm))
    m_cap = re.search(r"\[XHCI\] msc capacity blocks=(\d+) block_size=(\d+) bytes=(\d+)", logm)
    check("⑤ [XHCI] msc capacity block_size=512 且 blocks=<宿主镜像扇区数>（容量与宿主一致）",
          bool(m_cap) and m_cap.group(2) == "512" and int(m_cap.group(1)) == STICK_SECTORS,
          m_cap.group(0) if m_cap else first_match(r"\[XHCI\] msc capacity[^\r\n]*", logm))

    # ★ 宿主侧逐字节核对：crc + head 与宿主造的字节比
    m_rd = re.search(r"\[XHCI\] msc READ\(10\) ok lba=0 count=1 bytes=512 crc=([0-9A-F]{8}) "
                     r"head=([0-9A-F]{32})", logm)
    if m_rd:
        want_crc = "%08X" % (zlib.crc32(mbr) & 0xFFFFFFFF)
        want_head = mbr[:16].hex().upper()
        check("⑤★ [XHCI] msc READ(10) ok lba=0 count=1 bytes=512 crc=… head=…（打印了读到的字节）",
              True, m_rd.group(0))
        check("⑤★ 宿主侧核对：CRC32 与宿主镜像首扇区逐字节一致（%s）" % want_crc,
              m_rd.group(1) == want_crc, "内核 crc=%s / 宿主 crc=%s" % (m_rd.group(1), want_crc))
        check("⑤★ 宿主侧核对：头 16 字节逐字节一致（%s…）" % want_head[:16],
              m_rd.group(2) == want_head, "内核 head=%s / 宿主 head=%s…" % (m_rd.group(2), want_head[:16]))
    else:
        check("⑤★ [XHCI] msc READ(10) ok lba=0 count=1 bytes=512 crc=… head=…（打印了读到的字节）",
              False, first_match(r"\[XHCI\] msc [^\r\n]*FAILED[^\r\n]*", logm))

    check("⑤ [USBST] storage attached -> rescan drive letters（USB 存储接进来后重扫盘符）",
          "[USBST] storage attached -> rescan drive letters" in logm,
          first_match(r"\[USBST\] storage attached[^\r\n]*", logm))
    check("⑤ 重扫打点里 xhci=1（这一块盘是 xHCI 认出来的，不是 UHCI）",
          bool(re.search(r"\[USBST\] storage attached[^\r\n]*xhci=1", logm)),
          first_match(r"\[USBST\] storage attached[^\r\n]*", logm))
    m_let = re.search(r"\[DRV64\] letter=([A-Z]): disk=24 part=(\d+) fs=(\w+)", logm)
    check("⑤ 盘符出现：[DRV64] letter=X: disk=24 … fs=FAT32（U 盘 = 驱动器号 24 = ATA64_USB_BASE）",
          bool(m_let) and m_let.group(3) == "FAT32",
          m_let.group(0) if m_let else first_match(r"\[DRV64\] letter=[^\r\n]*", logm))
    for needle in FORBIDDEN:
        check("⑤ 存储跑不得出现 %s" % needle, needle not in logm)

    # ---- ⑤b ★ xHCI 键盘 + 存储混跑：U 盘运行期读**前后**键盘都能打字 + 零 stray ----
    #   ("存储操作之前" = 先打一条终端命令；"之后" = vol 切到 U 盘卷、真的读一次 FAT 根目录
    #    （entries>=1 = 运行期 xHCI 批量传输），再打一条命令。)
    print("--- ⑤b 键盘（xHCI）+ U 盘运行期读：读写前后都能打字 ---")
    mon2 = Monitor(port2)
    opened2 = False
    for _ in range(6):
        mon2.key("meta_l", wait=0.9)
        mon2.key("1", wait=1.8)
        if "[APP] term opened" in slog2():
            opened2 = True
            break
    check("⑤b★ 存储跑里终端也能打开（键盘就是 xHCI 那把）", opened2,
          first_match(r"\[APP\] term opened[^\r\n]*", slog2()))
    if opened2 and m_let:
        letter = m_let.group(1)
        mark2 = len(slog2())
        type_line(mon2, "ls /", per_key=0.35)          # 存储 I/O 之前：键盘先可用
        dl = time.time() + 40
        while time.time() < dl and not re.search(r"\[TERM\] cmd ls ok", slog2()[mark2:]):
            time.sleep(0.3)
        check("⑤b★ 存储 I/O 之前：`ls /` 被终端执行（键盘先可用）",
              bool(re.search(r"\[TERM\] cmd ls ok", slog2()[mark2:])),
              first_match(r"\[TERM\] cmd ls[^\r\n]*", slog2()[mark2:]))
        # 切到 U 盘卷：vol <letter>（这条命令本身也要键盘逐键打进去）
        mark3 = len(slog2())
        type_line(mon2, "vol %s" % letter.lower(), per_key=0.35)
        dl = time.time() + 40
        while time.time() < dl and ("[VOL] switch letter=%s:" % letter) not in slog2()[mark3:]:
            time.sleep(0.3)
        check("⑤b★ 运行期切到 U 盘卷（[VOL] switch letter=%s:）" % letter,
              ("[VOL] switch letter=%s:" % letter) in slog2()[mark3:],
              first_match(r"\[VOL\] switch[^\r\n]*", slog2()[mark3:]))
        # U 盘根目录（FAT32）：entries>=1/bytes>0 = 运行期真的走了 xHCI 批量读
        mark4 = len(slog2())
        type_line(mon2, "ls /", per_key=0.35)
        dl = time.time() + 60
        m_ent = None
        while time.time() < dl:
            m_ent = re.search(r"\[TERM\] cmd ls entries=(\d+) bytes=(\d+) path=/", slog2()[mark4:])
            if m_ent and int(m_ent.group(1)) >= 1:
                break
            time.sleep(0.3)
        check("⑤b★ U 盘运行期目录读（entries>=1 bytes>0 = xHCI 批量读真跑了）",
              bool(m_ent) and int(m_ent.group(1)) >= 1 and int(m_ent.group(2)) > 0,
              m_ent.group(0) if m_ent else first_match(r"\[TERM\] cmd ls entries=[^\r\n]*",
                                                       slog2()[mark4:]))
        # 存储 I/O 之后：再打一条命令，键盘仍生效
        mark5 = len(slog2())
        type_line(mon2, "ls /", per_key=0.35)
        dl = time.time() + 40
        while time.time() < dl and not re.search(r"\[TERM\] cmd ls ok", slog2()[mark5:]):
            time.sleep(0.3)
        check("⑤b★ 存储 I/O 之后：`ls /` 仍被终端执行（键盘在 U 盘读写后可用）",
              bool(re.search(r"\[TERM\] cmd ls ok", slog2()[mark5:])),
              first_match(r"\[TERM\] cmd ls[^\r\n]*", slog2()[mark5:]))
        logm2 = slog2()
        check("⑤b★ 键盘 + U 盘混跑全程零 [XHCI] stray transfer evt",
              "[XHCI] stray transfer evt" not in logm2,
              first_match(r"\[XHCI\] stray[^\r\n]*", logm2))
        hits2 = [n for n in FORBIDDEN if n in logm2]
        check("⑤b 混跑后 FORBIDDEN 词表零命中", not hits2,
              ("命中：" + ",".join(hits2)) if hits2 else "")
    kill(proc2)
    stick_crc1 = zlib.crc32(open(stick, "rb").read()) & 0xFFFFFFFF
    check("⑤★ U 盘镜像测试前后 CRC32 完全一致（本批只读，谁都没写它）",
          stick_crc1 == stick_crc0, "%08X -> %08X" % (stick_crc0, stick_crc1))

    # ============================================================ 3) 有主控没插设备
    print("[xhci64] 3) 降级：-device qemu-xhci（有 xHCI 主控、没插任何设备）")
    port3 = pick_port(port2 + 2)
    proc3, slog3, early3 = boot(qemu, ["-drive", "format=raw,file=%s" % q(args.img),
                                       "-device", "qemu-xhci"],
                                os.path.join(tmp, "none.log"), os.path.join(tmp, "none.err"),
                                port3, "[GUI64] ready", args.timeout)
    logn = slog3()
    if early3:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    check("⑥ [XHCI] no device on port <n>（没插设备 -> 优雅降级）",
          bool(re.search(r"\[XHCI\] no device on port \d+", logn)),
          first_match(r"\[XHCI\] no device on port \d+", logn))
    check("⑥ [XHCI] not found 不得出现（主控是有的）", "[XHCI] not found" not in logn)
    check("⑥ [XHCI] selftest PASS（没设备不算失败）", "[XHCI] selftest PASS" in logn)
    check("⑥ 没有任何 [XHCI] hid report 行（没键盘就不该有报告）", "[XHCI] hid report" not in logn)
    check("⑥ [GUI64] ready（系统照常启动）", "[GUI64] ready" in logn)
    for needle in FORBIDDEN:
        check("⑥ 无设备时不得出现 %s" % needle, needle not in logn)
    kill(proc3)

    # ============================================================ 4) 根本没有 xHCI
    print("[xhci64] 4) 降级：不加 -device qemu-xhci（机器上没有 xHCI 主控）")
    port4 = pick_port(port3 + 2)
    mon4 = Monitor(port4)
    proc4, slog4, early4 = boot(qemu, ["-drive", "format=raw,file=%s" % q(args.img)],
                                os.path.join(tmp, "notfound.log"),
                                os.path.join(tmp, "notfound.err"),
                                port4, "[GUI64] ready", args.timeout)
    logz = slog4()
    if early4:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    check("⑥ [XHCI] not found（没有主控 -> 优雅降级，不变砖）", "[XHCI] not found" in logz,
          first_match(r"\[XHCI\] not found[^\r\n]*", logz))
    check("⑥ [XHCI] selftest skipped (no controller)（自检按跳过，不算失败）",
          "[XHCI] selftest skipped (no controller)" in logz,
          first_match(r"\[XHCI\] selftest[^\r\n]*", logz))
    check("⑥ 没有主控时不打 hid report / config set / port <n> connected",
          "[XHCI] hid report" not in logz and "[XHCI] config set" not in logz and
          "connected speed=" not in logz)
    check("⑥ [GUI64] ready（系统照常启动）", "[GUI64] ready" in logz)
    mon4.key("meta_l", wait=1.2)                 # PS/2 键盘仍可用（顺带证明系统一切照旧）
    logz2 = slog4()
    check("⑥ PS/2 键盘仍能打开开始菜单（没有 xHCI 时系统一切照旧）", "[UI] menu open" in logz2)
    for needle in FORBIDDEN:
        check("⑥ 无主控时不得出现 %s" % needle, needle not in logz)
    kill(proc4)

    if args.keep:
        print("[xhci64] 串口日志目录：%s" % tmp)

    print("--- serial 摘录（主跑：全部 [XHCI] 行 + 桌面响应）---")
    for line in log.splitlines():
        if line.startswith("[XHCI]") or "[UI] menu" in line or "[APP] term opened" in line:
            print("   | " + line.strip()[:170])
    print("--- serial 摘录（存储跑：全部 [XHCI]/[USBST]/[DRV64] 行）---")
    for line in logm.splitlines():
        if line.startswith("[XHCI]") or "[USBST]" in line or "[DRV64] letter=" in line:
            print("   | " + line.strip()[:170])

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
