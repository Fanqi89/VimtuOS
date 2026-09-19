#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/sysstate64_test.py - 本轮四个子系统的端到端验收（sysstate64 / config64 / session64 / panic64）

被验的四项（都来自 legacy32 的退役工程，本轮移植到纯 64 位内核）：
  1) sysstate64：运行状态机（BOOT -> STARTING -> RUNNING）、模块注册表（>=12 个模块）、
     健康报告（[SYS64] health ok modules=N failed=0）、ring log（终端 syslog 能列出条目）；
  2) config64：类型化 KV（int/str/bool）建在 store64 上（VimtuFS2 /store.a|b，真落盘）——
     **跨重启持久化**：终端 `cfg set demo 1` -> `store flush` -> 冷启动第二遍读回 [CONF64] load demo=1；
     桌面图标位置也持久化（拖到新位置 -> 冷启动 -> [CONF64] apply ... icon0=X,Y 记录的是新位置）；
  3) session64：会话/应用内容策略（VOLATILE 关窗清状态 / PERSIST 保留 + 启动恢复），终端 session 有真输出；
  4) panic64：受控 BSOD（终端 `panic <code>` -> 蓝底白字屏 + 串口 [PANIC64] stop=<code> + 6 秒后停住）
     与看门狗（[WD64] watchdog armed；正常运行 60 秒不许出现 watchdog fire）。

另外断言：原来那批"尚未支持"桩命令（cfg/syslog/state/health/session/disk/hw/lspci/user/panic/restart --soft）
现在都没有"尚未支持"/"not supported yet" 字样，且各自有真输出（[TERM] cmd <name> ok + 具体打点）。

夹具：与 tests/store64_test.py 同一套 —— build64/system.img 的字节 + 标准 MBR + Python 复刻的
VimtuFS2 空卷（@LBA 8009）。只有"有卷"时才可能拿到 carrier=vfs 的 config64（这是正常安装盘的形态）。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\sysstate64_test.py
    py -3 tests\\sysstate64_test.py --timeout 150 --keep
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
FIXTURE_IMG = os.path.join(ROOT, "build64", "sysstate64_test.img")
BSOD_PNG = os.path.join(ROOT, "docs", "screenshots", "bsod64.png")

SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS      # 8009
TARGET_SECTORS = 32768                              # 16MB

STORE_SLOT_BYTES = 16384
STORE_MAGIC = b"VSTORE64"

# 桌面图标 0 的命中框（kernel/gui64.cpp：x±4 / y-4..y+ICON_W+18，ICON_W=48）
ICON0_HIT = (20, 76, 20, 90)

# BSOD 配色（kernel/panic64.cpp 的 BSOD_BG / BSOD_FG）
BSOD_BG = (0, 120, 212)     # 0x000078D4
BSOD_FG = (255, 255, 255)

# 关键打点（第一遍启动就该有）
MUST1 = [
    "[SYS64] state=BOOT",
    "[SYS64] state=STARTING",
    "[SYS64] state=RUNNING",
    "[SYS64] health ok modules=",
    "[SYS64] health module mem64 state=READY",
    "[SESS64] policy=VOLATILE",
    "[WD64] watchdog armed timeout=5000ms",
    "[WD64] watchdog task up id=",
    "[CONF64] apply lang=",
    "[GUI64] ready",
]

# 原来是"尚未支持"桩的命令（进终端敲）与各自的真输出判据
CMDS = [
    ("syslog", "[SYS64] syslog lines="),
    ("state", "[TERM] cmd state ok"),
    ("health", "[SYS64] health ok modules="),
    ("session", "[SESS64] policy="),
    ("hw", "[TERM] hw cpu="),
    ("lspci", "[TERM] lspci scanned="),
    ("disk", "[TERM] disk ata_present="),
    ("user", "[TERM] user ring3 mapped="),
    ("cfg", "[CONF64] cmd dump keys="),
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def near(c, target, tol=10):
    return all(abs(c[i] - target[i]) <= tol for i in range(3))


# ---------------------------------------------------------------------------
# 夹具：已装好系统 + 主分区已格式化的 16MB 测试盘（与 tests/store64_test.py 同款）
# ---------------------------------------------------------------------------
def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


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


def read_vimtufs2_files(path):
    """宿主侧直接解析 raw 镜像的 VimtuFS2 根目录（绕过内核的字节级证据）。"""
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
            continue
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
    if len(b) != STORE_SLOT_BYTES:
        return {"ok": False, "kv": {}}
    plen = struct.unpack_from("<I", b, 24)[0]
    pcrc = struct.unpack_from("<I", b, 28)[0]
    hcrc = struct.unpack_from("<I", b, 32)[0]
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
    return {"ok": (b[0:8] == STORE_MAGIC and header_ok and payload_ok), "kv": kv}


# ---------------------------------------------------------------------------
# QEMU + monitor
# ---------------------------------------------------------------------------
class Monitor:
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

    def move(self, dx, dy, wait=0.3):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def button(self, val, wait=0.4):
        self.send("mouse_button %d" % val, wait=wait)

    def shot(self, path, wait=3.0):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(25):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


# QEMU sendkey 的名字表：字母/数字就是字符本身；注意**不能用大写**（"D" 不是合法键名，
# 会被 QEMU 静默丢掉 —— 曾经因此让 `panic DEMO01` 变成 `panic 01`）。
KEYMAP = {" ": "spc", ".": "dot", "/": "slash", "-": "minus", "=": "equal", "_": "shift-minus"}


class Vm:
    def __init__(self, qemu, img, port, tag, timeout=150):
        self.qemu, self.img, self.port, self.tag = qemu, img, port, tag
        self.timeout = timeout
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_sysstate_")
        self.serial = os.path.join(self.tmp, tag + ".log")
        self.proc = None

    def start(self):
        self.proc = subprocess.Popen([
            self.qemu, "-name", "Vimtu64-" + self.tag,
            "-drive", "format=raw,file=%s" % q(self.img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % self.port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc is not None and self.proc.poll() is not None:
                return False
            time.sleep(0.3)
        return needle in self.log()[since:]

    def wait_ready(self, timeout=None):
        return self.wait_log("[GUI64] ready", timeout or self.timeout)

    def monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return Monitor(self.port)
            except OSError:
                time.sleep(0.25)
        return None

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def stop(self):
        if self.proc is not None and self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def type_line(mon, text, per_key=0.22):
    for ch in text:
        mon.key(KEYMAP.get(ch, ch), wait=per_key)
    mon.key("ret", wait=1.0)


def open_terminal(vm, mon):
    for _ in range(4):
        mon.key("meta_l", wait=1.2)
        if "[UI] menu open" in vm.log():
            mon.key("1", wait=2.2)
            if "[APP] term opened" in vm.log():
                return True
        time.sleep(0.5)
    return False


def run_cmd(vm, mon, cmd, marker, tries=3):
    """敲一条命令并等它的判据出现（判据可能是屏幕行，也可能是串口打点）。"""
    for _ in range(tries):
        before = len(vm.log())
        type_line(mon, cmd)
        if marker in vm.log()[before:]:
            return True
        time.sleep(0.8)
    return marker in vm.log()


# ==================== 鼠标闭环定位（与 tests/ui_extra64_test.py 同款手法）====================
def probe_pos(vm, mon):
    before = len(vm.log())
    mon.button(1, wait=0.45)
    mon.button(0, wait=0.55)
    t0 = time.time()
    while time.time() - t0 < 2.5:
        log = vm.log()[before:]
        m = re.search(r"\[UI\] selbox x0=(\d+) y0=(\d+)", log)
        if m:
            return (int(m.group(1)), int(m.group(2)))
        if "[UI] desktop icon select kind=" in log:
            return None
        time.sleep(0.25)
    return None


def settle(vm, mon, times=3):
    pos = None
    for _ in range(times):
        pos = probe_pos(vm, mon)
        if pos is None:
            return None
    return pos


def _move_axis(mon, axis, d, mag):
    if axis == 0:
        mon.move(mag if d > 0 else -mag, 0)
    else:
        mon.move(0, mag if d > 0 else -mag)


def aim_axis(mon, axis, d):
    if abs(d) < 20:
        return
    if abs(d) < 48:
        _move_axis(mon, axis, d, 14)
        return
    for _ in range((abs(d) + 23) // 24 - 1):
        _move_axis(mon, axis, d, 100)
    _move_axis(mon, axis, d, -20)


def press_on_icon(vm, mon, tx, ty, tries=8):
    for _ in range(tries):
        pos = settle(vm, mon)
        if pos is not None:
            cx, cy = pos
            if abs(cx - tx) > 20 or abs(cy - ty) > 24:
                aim_axis(mon, 0, tx - cx)
                aim_axis(mon, 1, ty - cy)
                continue
        before = len(vm.log())
        mon.button(1, wait=0.5)
        if vm.wait_log("[UI] desktop icon select kind=", 2.5, since=before):
            return True
        mon.button(0, wait=0.6)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5716)
    ap.add_argument("--timeout", type=int, default=150, help="每次启动等待 [GUI64] ready 的最长秒数")
    ap.add_argument("--watchdog-window", type=int, default=60, help="看门狗误触发观察窗口（秒）")
    ap.add_argument("--keep", action="store_true")
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

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 sysstate64/config64/session64/panic64 验收 ===")
    print("[fixture] 测试盘：%s（%d 扇区，主分区 LBA %d 已格式化 VimtuFS2）"
          % (img, TARGET_SECTORS, PART_MAIN_LBA))

    # ==================== 第一遍：状态机 / 模块 / 健康 / ring log / 命令 / 配置写盘 / 图标拖动 ========
    print("--- 1) 第一遍启动：状态机 + 模块注册 + 健康 + 新命令 + cfg 写盘 + 图标拖动 ---")
    vm = Vm(qemu, img, args.port, "boot1", args.timeout)
    vm.start()
    up1 = vm.wait_ready()
    log1 = vm.log()
    check("桌面就绪 [GUI64] ready", up1)
    for needle in MUST1:
        check("必须出现 %s" % needle, needle in log1)

    mods = re.findall(r"\[SYS64\] module (\S+) registered", log1)
    check("模块注册行数量 >= 12", len(mods) >= 12,
          "count=%d names=%s" % (len(mods), ",".join(mods)))
    for want in ("mem64", "x86_64", "ata64", "vfs64", "store64", "task64", "apic64", "smp64",
                 "hwinfo64", "acpi64", "edid64", "net64", "usb64", "syscall64", "usermode64", "gui64"):
        check("模块表里有 %s" % want, want in mods)

    m = re.search(r"\[SYS64\] state=BOOT[\s\S]*?\[SYS64\] state=RUNNING gen=(\d+) modules=(\d+)", log1)
    check("状态机 BOOT -> STARTING -> RUNNING（顺序正确）", bool(m))
    check("RUNNING 行带模块数（>=12）", bool(m) and int(m.group(2)) >= 12)
    mh = re.search(r"\[SYS64\] health ok modules=(\d+) failed=0", log1)
    check("健康报告 [SYS64] health ok modules=N failed=0", bool(mh),
          mh.group(0) if mh else "未出现（vfs64 在无卷镜像上会 FAILED，本夹具应有卷）")
    check("健康报告里有各模块状态（[SYS64] health module ...）",
          len(re.findall(r"\[SYS64\] health module \S+ state=\S+ health=\S+", log1)) >= 12)
    mp = re.search(r"\[CONF64\] init carrier=(\S+) slot=(\S+) keys=(\d+)", log1)
    check("config64 初始化行（载体/槽/键数）", bool(mp), mp.group(0) if mp else "未出现")
    check("config64 载体 = vfs（夹具盘有 VimtuFS2 卷）", bool(mp) and mp.group(1) == "vfs")

    mon = vm.monitor() if up1 else None
    typed = open_terminal(vm, mon) if mon else False
    check("终端已打开（meta_l + 1 注入）", typed)

    if typed:
        print("  --- 新命令（原来都是'尚未支持'桩）---")
        for cmd, marker in CMDS:
            check("命令 %-7s 有真输出（%s）" % (cmd, marker), run_cmd(vm, mon, cmd, marker))
        logc = vm.log()
        for cmd, _ in CMDS:
            check("命令 %-7s 不再打印'尚未支持'" % cmd,
                  ("[TERM] unsupported %s" % cmd) not in logc)
        check("6 条过时桩的 not supported yet 文案全部消失",
              "not supported yet (store/config persistence" not in logc and
              "not supported yet (hwinfo/hardware probe" not in logc and
              "not supported yet (PCI enumeration" not in logc and
              "not supported yet (ring3 user programs" not in logc and
              "not supported yet (panic/bsod" not in logc and
              "not supported yet (sysstate" not in logc and
              "not supported yet (restart state machine" not in logc)
        print("  --- 仍然未移植的两条（update / preload）必须如实报'尚未支持' ---")
        check("update 仍是如实桩（[TERM] unsupported update: + 说明原因）",
              run_cmd(vm, mon, "update", "[TERM] unsupported update:"))
        check("preload 仍是如实桩（[TERM] unsupported preload:）",
              run_cmd(vm, mon, "preload", "[TERM] unsupported preload:"))

        print("  --- config64 写盘：cfg set demo=1 -> store flush -> cfg save ---")
        check("cfg set demo=1（cfg set KEY=VALUE 形态）",
              run_cmd(vm, mon, "cfg set demo=1", "[CONF64] set key=demo value=1 type=int"))
        check("store flush 落盘成功", run_cmd(vm, mon, "store flush", "[STORE64] cmd flush rc=0"))
        check("cfg save（config64 自己的落盘入口）",
              run_cmd(vm, mon, "cfg save", "[CONF64] flush ok via=vfs"))

        print("  --- 先关掉终端（Esc）：图标拖动要在桌面上做（终端窗口盖着图标区）---")
        mon.key("esc", wait=1.2)
        check("Esc 关掉终端（[APP] term closed，桌面露出来）", vm.wait_log("[APP] term closed", 6))
        check("关窗后 VOLATILE 策略清了终端状态（[SESS64] reset app=terminal reason=close）",
              vm.wait_log("[SESS64] reset app=terminal", 8))
        time.sleep(1.0)

        print("  --- 桌面图标拖到新位置（拖动结束写 config64）---")
        tx = (ICON0_HIT[0] + ICON0_HIT[1]) // 2
        ty = (ICON0_HIT[2] + ICON0_HIT[3]) // 2
        pressed = press_on_icon(vm, mon, tx, ty)
        check("光标落到桌面图标 0 上（按下有 select 打点）", pressed)
        dragged = False
        fx = fy = None
        if pressed:
            for _ in range(3):
                mon.move(100, 0, wait=0.3)
            mid = len(vm.log())
            mon.button(0, wait=0.8)
            time.sleep(0.8)
            m2 = re.search(r"\[CONF64\] icon 0 moved to (\d+),(\d+)", vm.log()[mid:])
            if not m2:
                m2 = re.search(r"\[CONF64\] icon 0 moved to (\d+),(\d+)", vm.log())
            if m2:
                dragged = True
                fx, fy = int(m2.group(1)), int(m2.group(2))
        check("拖动结束后写入 config64（[CONF64] icon 0 moved to X,Y）", dragged,
              "x=%s y=%s" % (fx, fy))
        if fx is not None:
            check("新位置确实向右移动了（初始 x=24）", fx > 24, "final x=%d" % fx)
        check("拖动也打了 [UI] icon drag idx=0（原有打点没丢）", "[UI] icon drag idx=0" in vm.log())

        print("  --- 看门狗：%d 秒观察窗口内不许误触发 ---" % args.watchdog_window)
        t0 = time.time()
        while time.time() - t0 < args.watchdog_window:
            time.sleep(2.0)
        logw = vm.log()
        check("观察窗口内没有 [WD64] watchdog fire", "watchdog fire" not in logw)
        beats = re.findall(r"\[WD64\] watchdog beat stale=(\d+)ms", logw)
        check("看门狗在跑（beat 行 >= 3 条，证明任务真被调度）", len(beats) >= 3,
              "beats=%d" % len(beats))
        check("beat 里的 stale 一直远小于超时阈值（< 1000ms）",
              bool(beats) and all(int(x) < 1000 for x in beats),
              "max_stale=%dms" % (max(int(x) for x in beats) if beats else -1))
        check("这 %d 秒里没出现 PANIC" % args.watchdog_window, "PANIC" not in logw)

    log1 = vm.log()
    vm.stop()

    print("  --- 宿主侧解析 raw 镜像：/store.a|b 里真的有 cfg.demo=1 ---")
    files = read_vimtufs2_files(img) or {}
    slot_name, slot = None, None
    for cand in ("store.a", "store.b"):
        if cand in files:
            slot_name = cand
            slot = parse_store_slot(files[cand]["data"])
            break
    check("VimtuFS2 卷里有 /store.a 或 /store.b", slot is not None,
          ("命中 /%s" % slot_name) if slot_name else "没找到槽文件")
    if slot:
        check("槽文件 CRC/结构有效（VSTORE64）", bool(slot["ok"]))
        check("槽 KV 里有 cfg.demo=1（字节级证据）", slot["kv"].get("cfg.demo") == "1",
              "kv_keys=%s" % sorted(slot["kv"].keys())[:6])

    # ==================== 第二遍：冷启动读回配置 + 图标位置 ====================
    print("--- 2) 第二遍冷启动（同一块镜像）：config64 读回 demo=1 + 图标位置 ---")
    vm2 = Vm(qemu, img, args.port + 1, "boot2", args.timeout)
    vm2.start()
    up2 = vm2.wait_ready()
    log2 = vm2.log()
    check("第二遍桌面就绪", up2)
    m2 = re.search(r"\[CONF64\] load demo=1", log2)
    check("跨重启持久化：[CONF64] load demo=1", bool(m2),
          (re.findall(r"\[CONF64\] load \S+", log2)[:4] if not m2 else m2.group(0)))
    m3 = re.search(r"\[CONF64\] init carrier=vfs slot=\S+ keys=(\d+)", log2)
    check("第二遍从 VFS 载体读到键（keys >= 1）", bool(m3) and int(m3.group(1)) >= 1,
          m3.group(0) if m3 else "未出现")
    ma = re.search(r"\[CONF64\] apply lang=\S+ zoom=\d+ mirror=\d+ sens=\d+ icon0=(\d+),(\d+)", log2)
    check("第二遍桌面读到的图标位置 = 第一遍拖动后的位置",
          bool(ma) and fx is not None and int(ma.group(1)) == fx and int(ma.group(2)) == fy,
          (ma.group(0) if ma else "未出现") + (" (期望 icon0=%s,%s)" % (fx, fy)))
    vm2.stop()

    # ==================== 第三遍：受控 BSOD ====================
    print("--- 3) 受控蓝屏：终端 panic demo01 -> [PANIC64] stop=DEMO01 + 蓝底白字像素断言 ---")
    vm3 = Vm(qemu, FIXTURE_IMG, args.port + 2, "boot3", args.timeout)
    vm3.start()
    up3 = vm3.wait_ready()
    check("第三遍桌面就绪（BSOD 场景）", up3)
    mon3 = vm3.monitor() if up3 else None
    typed3 = open_terminal(vm3, mon3) if mon3 else False
    check("BSOD 场景：终端已打开", typed3)
    if typed3:
        run_cmd(vm3, mon3, "panic demo01", "[PANIC64] stop=DEMO01", tries=2)
    log3 = vm3.log()
    check("串口 [PANIC64] stop=DEMO01（停止码真的传到串口）",
          "[PANIC64] stop=DEMO01" in log3,
          " | ".join(x.strip() for x in log3.splitlines() if "[PANIC64] stop=" in x))
    mscr = re.search(r"\[PANIC64\] screen w=(\d+) h=(\d+) bg=0x000078D4 fg=0x00FFFFFF lines=(\d+)", log3)
    check("串口 [PANIC64] screen w=/h=/lines=（像素断言的依据）", bool(mscr),
          mscr.group(0) if mscr else "未出现")
    check("蓝屏正文行数 >= 6", bool(mscr) and int(mscr.group(3)) >= 6)

    ppm = os.path.join(vm3.tmp, "bsod.ppm")
    shot_ok = mon3.shot(ppm) if mon3 else False
    check("拿到 BSOD 截图（screendump）", shot_ok and os.path.exists(ppm))
    if shot_ok and os.path.exists(ppm):
        w, h, px = read_ppm(ppm)
        total = blue = white = other = 0
        for y in range(4, h, 16):
            for x in range(4, w, 16):
                c = sample(px, w, x, y)
                total += 1
                if near(c, BSOD_BG, 24):
                    blue += 1
                elif near(c, BSOD_FG, 40):
                    white += 1
                else:
                    other += 1
        check("整屏以蓝底为主（>= 80%% 采样点）", blue >= total * 0.8,
              "blue=%d/%d (%.0f%%) other=%d white=%d"
              % (blue, total, 100.0 * blue / total, other, white))
        check("屏上有白色文字像素", white > 30, "white=%d" % white)
        body_white = 0
        for y in range(102, min(h, 430)):
            for x in range(48, min(w, 1100)):
                c = sample(px, w, x, y)
                if near(c, BSOD_FG, 40):
                    body_white += 1
        check("正文区（y=102..430）有白色文字", body_white > 200, "body_white=%d" % body_white)
        title_white = 0
        for y in range(50, 95):
            for x in range(48, min(w, 900)):
                c = sample(px, w, x, y)
                if near(c, BSOD_FG, 40):
                    title_white += 1
        check("标题区（y=50..95）有白色文字", title_white > 100, "title_white=%d" % title_white)
        try:
            from PIL import Image
            os.makedirs(os.path.dirname(BSOD_PNG), exist_ok=True)
            Image.open(ppm).save(BSOD_PNG)
            check("BSOD 截图已存 PNG：%s" % os.path.relpath(BSOD_PNG, ROOT),
                  os.path.exists(BSOD_PNG) and os.path.getsize(BSOD_PNG) > 1024)
        except Exception as e:
            check("存 BSOD PNG（需要 Pillow）", False, str(e))

    check("蓝屏后停留并停住：[PANIC64] hold=6000ms", "[PANIC64] hold=6000ms" in vm3.log())
    vm3.wait_log("[PANIC64] halt (power cycle required)", 20)
    check("（约 6 秒后）串口报 halt，且不自动重启",
          "[PANIC64] halt (power cycle required)" in vm3.log())
    check("停住之后 QEMU 仍活着（画面留在屏上，没被复位冲掉）", vm3.alive())
    time.sleep(1.0)
    vm3.stop()

    # ==================== 第四遍：软重启（restart --soft）====================
    print("--- 4) restart --soft：优雅停止（停模块 + 会话/配置落盘）-> 硬复位链 ---")
    vm4 = Vm(qemu, FIXTURE_IMG, args.port + 3, "boot4", args.timeout)
    vm4.start()
    up4 = vm4.wait_ready()
    check("第四遍桌面就绪（软重启场景）", up4)
    mon4 = vm4.monitor() if up4 else None
    typed4 = open_terminal(vm4, mon4) if mon4 else False
    check("软重启场景：终端已打开", typed4)
    if typed4:
        run_cmd(vm4, mon4, "restart --soft", "[SYS64] soft restart requested", tries=2)
    log4 = vm4.log()
    check("串口 [SYS64] soft restart requested", "[SYS64] soft restart requested" in log4)
    check("软重启先走 STOPPING（[SYS64] state=STOPPING reason=soft-restart）",
          "[SYS64] state=STOPPING reason=soft-restart" in log4)
    check("逆序停模块：出现 [SYS64] module <name> stop ok",
          re.search(r"\[SYS64\] module \S+ stop ok", log4) is not None)
    check("优雅停止阶段落盘：[SYS64] persist session open=",
          "[SYS64] persist session open=" in log4)
    check("停止完成：[SYS64] state=STOPPED stopped=", "[SYS64] state=STOPPED stopped=" in log4)
    check("随后进入原有硬复位链（[SYS64] soft restart: entering hard reset chain）",
          "[SYS64] soft restart: entering hard reset chain" in log4)
    check("硬复位链打点照旧（[RESET] sys_reboot64: 8042 pulse）",
          "[RESET] sys_reboot64: 8042 pulse" in log4)
    t0 = time.time()
    early_exit = False
    while time.time() - t0 < 15:
        if not vm4.alive():
            early_exit = True
            break
        time.sleep(0.3)
    check("软重启最终触发真复位（-no-reboot 下 QEMU 退出）", early_exit)
    vm4.stop()

    if args.keep:
        print("[keep] 串口日志目录：%s" % vm.tmp)

    n_pass = sum(1 for _, c in checks if c)
    print("断言：%d/%d PASS" % (n_pass, len(checks)))
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
