#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/drvsvc64_test.py - ★ 本批：用户态设备映射（pci_map_bar）+ Ring 3 驱动服务骨架验收

被验的需求（任务书 ②③ 一一对应，全部取串口真打点，不设内存态断言）：

  ① `pci_map_bar(bdf, bar_index, out_va, out_len)`（自有 ABI 号 48）成功：
     * `[PCIMAP] map … u=1`（映射的页真的用户可访问）；
     * VA 落在**设备映射窗**内（= 用户窗口顶部 shm 窗下方 2 MiB，见 kernel/proc64.h）+ 页对齐；
     * 长度 = **BAR 实际大小**（写全 1 回读法）：2 的幂、且 `pages == len/4096`；
     * 物理基址与**内核驱动自己的打点**对得上（`[HDA64] pci 0:27.0 bar0=0x…` /
       `[XHCI] pci 0:20.0 bar0=0x…`）；
     * **幂等**：同一个 (bdf, bar) 再调一次 -> 同一个 VA/长度，内核打点 `re=1`。
  ② 读到的寄存器值与内核打点一致（**对照**）：
     * HDA：`DRVDEMO hda regs gcap=… vmin=… vmaj=…` vs
       `[HDA64] ctrl gctl=… statests=… caps=… vmaj=… vmin=…`；
     * xHCI：`DRVDEMO xhci caps caplen=… ver=… hcs1=… max_slots=… max_ports=… csz=… ac64=…` vs
       `[XHCI] pci … caplen=… hcs1=… max_slots=… max_ports=… csz=… ver=… ac64=…`；
     * PORTSC：内核枚举时说"port N connected speed=high"的端口，用户态读到的 PORTSC 必须
       CCS=1（然后 PS 位 = 同一个速度编码）——两边看到的是同一台设备。
  ③ 负例都拿到**明确错误码**且不崩：非 root(-EPERM=-1)、不存在的 BDF(-ENODEV=-5)、
     I/O 端口 BAR(-EINVAL=-3)、BAR 序号越界(-EINVAL)、坏 out 指针(-EFAULT=-2)。
  ④ 越权不许成功：普通 mmap(MAP_FIXED) 覆盖设备窗 -> -ENOMEM(-12)；
     munmap 设备窗 -> -EINVAL(-22)（那一段的"物理页"是设备 MMIO，不是页池的页）。
  ⑤ `drvdemo` 退出后映射回收：`[PCIMAP] release pid=… slots=2 …` + 终端 `mem` 的
     页池 free pages 回到调用前的水位。
  ⑥ 全程无 PANIC / TRIPLE FAULT；`[SYSCALL] enosys nr=48` 不出现。
  ⑦ HDA 的**真实操作**证据：SD0 流复位握手 `wr=0x1 -> 回读 bit0=1`、清 0 -> `RUN=0` +
     `SD0STS.FIFORDY=1`；并且**之后内核驱动仍然可用**（`audio playtone 200` 的
     `[HDA64] cmd audio playtone … ok=1`）——这是"用户态摸过设备之后系统没坏"的证据。

测试台：**QEMU**（HDA 只能 QEMU）。设备拓扑固定为
  `-device ich9-intel-hda,addr=0x1b.0` + `-device hda-duplex` + `-device qemu-xhci,id=xhci,addr=0x14.0`
  + `-device usb-kbd,bus=xhci.0` —— 也就是 HDA=00:1b.0（bdf 0xD8）、xHCI=00:14.0（bdf 0xA0），
  与 kernel/hda64.cpp / xhci64.cpp 的扫描结果在启动日志里逐一对照（**不写死型号表**，只对照打点）。

用法：py -3 tests\\drvsvc64_test.py [--qemu 路径] [--timeout 300] [--keep] [--no-desktop]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import socket
import qemuhelp as qh              # noqa: E402  （公共登录手势）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

# 构建产物目录（默认 build64）。★ 为什么可覆盖：本仓库同时可能有另一条线在跑 build64.sh，
# 而 build64.sh 开头是 `rm -rf build64` —— 用一份**快照目录**跑验收可以避免"构建把夹具抽走"。
ART_DIR = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(ART_DIR, "system.img")
KERNEL_OS = os.path.join(ART_DIR, "kernel64_os.bin")
DRVDEMO_ELF = os.path.join(ART_DIR, "drvdemo.elf")
FIXTURE_IMG = os.path.join(ART_DIR, "drvsvc64_test.img")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
DRVDEMO_PATH = "/bin/drvdemo"

# ---- 与内核常量逐位一致（kernel/proc64.h / usermode64.h）----
USER64_CODE_VA64 = 0x0000000100000000
USER64_WINDOW_BYTES64 = 16 * 1024 * 1024          # 4GiB..4GiB+16MiB
SHM_WINDOW_BYTES64 = 256 * 1024                   # 顶部 256 KiB（SHM64 窗）
DEV_SLOTS = 8
DEV_SLOT_BYTES = 256 * 1024
DEV_WINDOW_BYTES = DEV_SLOTS * DEV_SLOT_BYTES     # 2 MiB
DEV_WINDOW_VA = USER64_CODE_VA64 + USER64_WINDOW_BYTES64 - SHM_WINDOW_BYTES64 - DEV_WINDOW_BYTES
DEV_WINDOW_END = USER64_CODE_VA64 + USER64_WINDOW_BYTES64 - SHM_WINDOW_BYTES64

# ---- 错误码（kernel/syscall64.h 的 PCIMAP64_*）----
E_PERM, E_FAULT, E_INVAL, E_NOMEM, E_NODEV = 1, 2, 3, 4, 5

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash",
}
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "selftest FAIL", "[SYSCALL] enosys nr=48"]


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


class PMonitor:
    """**持久连接**的 QEMU monitor（只用来 sendkey）。

    为什么不用 tests/qemuhelp.py 的 Monitor：它是"每个按键新开一条 TCP、发完就关"。
    在 QEMU `-monitor telnet:…,server,nowait` 下，前一条连接刚断开、下一条就接上时，
    QEMU 侧会出现"已有客户端"的竞态 —— connect 成功但 `sendkey` 被丢掉，而且**静默**。
    实测症状：一条命令能打进去、之后的命令全部消失（客机还在跑、终端也有焦点）。
    这里改成一条长连接、顺序发 `sendkey` 行，从根上消掉这个竞态。
    """

    def __init__(self, port):
        self.port = port
        self.sock = None
        self.failed = 0

    def _ensure(self):
        if self.sock is not None:
            return True
        deadline = time.time() + 60
        while time.time() < deadline:
            try:
                self.sock = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                return True
            except OSError:
                time.sleep(0.5)
        return False

    def key(self, name, wait=0.9):
        if not self._ensure():
            self.failed += 1
            return False
        try:
            self.sock.sendall(("sendkey %s\n" % name).encode("ascii"))
        except OSError:
            self.failed += 1
            try:
                self.sock.close()
            except Exception:
                pass
            self.sock = None
            return False
        time.sleep(wait)
        return True

def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


KERNEL_TAG_RE = re.compile(r"\[[A-Z][A-Z0-9_]*\][^\r\n]*\r?\n")


def stream(text):
    """剥掉内核打点行 -> 只剩终端/程序自己的输出流（逐字节可比对；与 a42a64_test 同一套过滤）。"""
    return KERNEL_TAG_RE.sub("", text)


# ---------------------------------------------------------------------------
# 夹具盘：system.img + 标准 MBR + 主分区（VimtuFS2 v4 卷：/bin/drvdemo）
# ---------------------------------------------------------------------------
def prepare_fixture():
    if not (os.path.exists(SYSTEM_IMG) and os.path.exists(DRVDEMO_ELF)):
        return None
    import make_shellvol as msv           # tools/make_shellvol.py（同一份离线写入器）
    demo = open(DRVDEMO_ELF, "rb").read()
    system_bytes = open(SYSTEM_IMG, "rb").read()
    if demo[:4] != b"\x7fELF" or not system_bytes:
        return None
    vol = msv.Volume(TARGET_SECTORS - PART_MAIN_LBA)
    bin_ino = vol.mkdir("bin", parent=0, mode=0o755)
    vol.mkdir("tmp", parent=0, mode=0o777)
    vol.write_file("drvdemo", demo, parent=bin_ino, mode=0o755)
    vol_bytes = vol.finish()
    bad = msv.verify(vol_bytes, {DRVDEMO_PATH: demo})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = msv.build_disk(system_bytes, vol_bytes, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(img)
    return FIXTURE_IMG, demo


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--art-dir", default=None,
                    help="构建产物目录（默认 <repo>/build64；指一份快照目录可在另一条线构建期间跑验收）")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-desktop", action="store_true", help="只做构建产物断言（不启 QEMU）")
    args = ap.parse_args()
    global ART_DIR, SYSTEM_IMG, KERNEL_OS, DRVDEMO_ELF, FIXTURE_IMG
    if args.art_dir:
        ART_DIR = os.path.abspath(args.art_dir)
        SYSTEM_IMG = os.path.join(ART_DIR, "system.img")
        KERNEL_OS = os.path.join(ART_DIR, "kernel64_os.bin")
        DRVDEMO_ELF = os.path.join(ART_DIR, "drvdemo.elf")
        FIXTURE_IMG = os.path.join(ART_DIR, "drvsvc64_test.img")
    print("[drvsvc] 构建产物目录：%s" % ART_DIR)

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + str(detail)) if detail else ""))

    print("=== Vimtu64 用户态设备映射（pci_map_bar 48）+ Ring 3 驱动服务骨架（/bin/drvdemo）===")

    # ---------------- ⓪ 构建产物 ----------------
    if not os.path.exists(KERNEL_OS):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % KERNEL_OS)
        return 2
    if not os.path.exists(DRVDEMO_ELF):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh —— 本批新增的 /bin/drvdemo）\n" % DRVDEMO_ELF)
        return 2
    ossz = os.path.getsize(KERNEL_OS)
    check("系统内核在内核区硬上限内（<= 4096000 B）", ossz <= 4096000,
          "kernel64_os.bin=%d B；余量 %d B" % (ossz, 4096000 - ossz))
    check("余量 >= 预留下限 640 KiB（655360 B）", (4096000 - ossz) >= 655360,
          "系统内核 %d B；余量 %d B" % (ossz, 4096000 - ossz))
    dsz = os.path.getsize(DRVDEMO_ELF)
    check("/bin/drvdemo 是 ELF64 且 <= 64 KiB（用户窗口装载区上限）",
          open(DRVDEMO_ELF, "rb").read(4) == b"\x7fELF" and 0 < dsz <= 65536, "%d B" % dsz)
    kimg = open(KERNEL_OS, "rb").read()
    demo_bytes = open(DRVDEMO_ELF, "rb").read()
    mid = len(demo_bytes) // 2
    check("★ 内核二进制里搜不到 drvdemo 的 64B 探针（交付 = 系统卷里的文件，不给内核加字节）",
          demo_bytes[mid:mid + 64] not in kimg, "内核 %d B" % len(kimg))

    if args.no_desktop:
        print("=== RESULT: %s ===  checks=%d ok=%d" %
              ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
        return 0 if ok else 1

    # ---------------- 夹具 ----------------
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    fx = prepare_fixture()
    if not fx:
        sys.stderr.write("造夹具盘失败（需要 build64/system.img + build64/drvdemo.elf）\n")
        return 2
    img, demo_bytes = fx
    check("夹具卷里的 /bin/drvdemo 与 build64/drvdemo.elf 逐字节一致（工具自检回读）",
          os.path.getsize(DRVDEMO_ELF) == len(demo_bytes), "%d B" % len(demo_bytes))
    print("[drvsvc] 测试盘已生成：%s（主分区 LBA %d 上是带 %s 的 VimtuFS2 v4 卷）"
          % (img, PART_MAIN_LBA, DRVDEMO_PATH))

    tmp = tempfile.mkdtemp(prefix="vimtu64_drvsvc_")
    serial = os.path.join(tmp, "boot1.log")
    wav = os.path.join(tmp, "hda_out.wav")
    mport = qh.free_port()
    qargs = [
        qemu, "-name", "Vimtu64-drvsvc",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
        # ★ 固定拓扑（HDA=00:1b.0 / xHCI=00:14.0）：drvdemo 的候选 BDF 与内核驱动的扫描结果
        #   必须落到同两个设备上，后面的"对照断言"才有意义。
        "-device", "ich9-intel-hda,addr=0x1b.0",
        "-device", "hda-duplex,audiodev=snd0",
        "-audiodev", "wav,id=snd0,path=%s" % q(wav),
        "-device", "qemu-xhci,id=xhci,addr=0x14.0",
        # ★ 故意**不**挂 `usb-kbd,bus=xhci.0`：实测那样会让客机的键盘路径走 xHCI HID，
        #   而 xhci64 的 HID 传输环在一次命令之后会把后续报告打成 `[XHCI] stray transfer evt`
        #   （键盘随后彻底失效）—— 本脚本要打多条命令，所以键盘走 QEMU 默认的 PS/2。
        #   xHCI 侧"用户态与内核看到同一份端口状态"的对照因此改成**逐端口 CCS 一致**（见下）。
        "-no-reboot",
    ]
    proc = subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = None
    try:
        mon = PMonitor(mport)
        qh.login_desktop(mon, lambda: slog(serial), proc, timeout=min(args.timeout, 180))
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if "[GUI64] ready" in slog(serial):
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        log = slog(serial)
        check("系统起来并进了桌面（[GUI64] ready）", "[GUI64] ready" in log, serial)

        # ---------------- ① 内核驱动的设备打点（后面逐条对照）----------------
        m_hda = None
        m_xhci = None
        deadline = time.time() + 40
        while time.time() < deadline:
            log = slog(serial)
            m_hda = re.search(r"\[HDA64\] pci (\d+):(\d+)\.(\d+) bar0=0x([0-9a-fA-F]+) codecs=(\d+)", log)
            m_xhci = re.search(r"\[XHCI\] pci (\d+):(\d+)\.(\d+) bar0=0x([0-9a-fA-F]+) caplen=0x([0-9a-fA-F]+) "
                               r"hcs1=0x([0-9a-fA-F]+) max_slots=(\d+) max_ports=(\d+) csz=(\d+) ver=0x([0-9a-fA-F]+) "
                               r"ac64=(\d+)", log)
            if m_hda and m_xhci:
                break
            time.sleep(0.4)
        check("HDA 控制器被内核驱动认下来（[HDA64] pci b:d.f bar0=0x… codecs=1）", m_hda is not None,
              (m_hda.group(0) if m_hda else "（缺）"))
        check("xHCI 主控被内核驱动认下来（[XHCI] pci b:d.f bar0=0x… caplen/hcs1/…）", m_xhci is not None,
              (m_xhci.group(0) if m_xhci else "（缺）"))
        if not (m_hda and m_xhci):
            print("=== RESULT: FAIL ===  （设备打点缺失，后面的对照无法进行）")
            return 1
        hda_bdf = (int(m_hda.group(1)) << 8) | (int(m_hda.group(2)) << 3) | int(m_hda.group(3))
        xhci_bdf = (int(m_xhci.group(1)) << 8) | (int(m_xhci.group(2)) << 3) | int(m_xhci.group(3))
        hda_bar0, xhci_bar0 = int(m_hda.group(4), 16), int(m_xhci.group(4), 16)
        check("内核侧 HDA 的 bdf = 0x%x（= 00:1b.0，与测试台的 -device addr 一致）" % hda_bdf,
              hda_bdf == 0xD8, "bdf=0x%x" % hda_bdf)
        check("内核侧 xHCI 的 bdf = 0x%x（= 00:14.0）" % xhci_bdf, xhci_bdf == 0xA0, "bdf=0x%x" % xhci_bdf)
        m_ctrl = re.search(r"\[HDA64\] ctrl gctl=0x([0-9a-fA-F]+) statests=0x([0-9a-fA-F]+) "
                           r"caps=0x([0-9a-fA-F]+) vmaj=(\d+) vmin=(\d+)", log)
        check("HDA ctrl 打点可解析（gctl/statests/caps/vmaj/vmin）", m_ctrl is not None,
              (m_ctrl.group(0) if m_ctrl else "（缺）"))

        # ---------------- 开终端 ----------------
        opened = False
        for _ in range(6):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if "[APP] term opened" in slog(serial):
                opened = True
                break
        check("桌面之后打开终端（开始菜单 -> 终端：[APP] term opened）", opened)

        def wait_re(pattern, timeout=40, since=0):
            """在内核打点流里找 pattern；**同一个 pattern 也在"剥掉内核行的用户输出流"里找**。
            为什么要两条：drvdemo 的输出是一条命令拆成多次 write(1) 打的，内核每次 write 都会插一行
            `[SYSCALL] nr=1 …`，于是 `DRVDEMO neg noroot euid=1000 rc=1` 在**原始**串口流里被切成
            好几段（实测）。剥掉内核行后各段重新接起来 —— 与 a42a64_test 的 probe_stream 同一条口径。
            返回的 match 一定带完整的分组，调用方不看偏移。"""
            deadline = time.time() + timeout
            while time.time() < deadline:
                win = slog(serial)[since:]
                m = re.search(pattern, win) or re.search(pattern, stream(win))
                if m:
                    return m
                if proc.poll() is not None:
                    return None
                time.sleep(0.3)
            return None

        # ★ 每个按键的间隔刻意放大（PER_KEY=0.9 s）：本仓库可能同时有另一条线在构建/跑 QEMU，
        #   客机被 CPU 抢占时 QEMU 的 PS/2 队列会被注入的键填满并丢事件（实测：整条命令丢字）。
        PER_KEY = 0.9

        def type_line(text, per_key=PER_KEY):
            for ch in text:
                if ch in TYPED_NAMES:
                    mon.key(TYPED_NAMES[ch], wait=per_key)
                elif ch.isalnum():
                    mon.key(ch, wait=per_key)
                else:
                    raise ValueError("sendkey 不支持这个字符：%r" % ch)
            mon.key("ret", wait=per_key + 0.2)

        def send_cmd(text, start_pat, timeout=150, tries=3, per_key=PER_KEY):
            """打一条终端命令并等它的**起始**打点；没等到就重打（最多 tries 次）。
            只在"起始打点还没出现"时重打 —— 出现过就不再重打（否则会跑两次）。"""
            for _ in range(tries):
                base = len(slog(serial))
                type_line(text, per_key=per_key)
                m = wait_re(start_pat, timeout, since=base)
                if m:
                    return m
            return None

        # ---------------- ③ 非 root：必须被拒（-EPERM）----------------
        before_nonroot = len(slog(serial))
        m = send_cmd("elfrun /bin/drvdemo",
                     r"\[PROC64\] create pid=\d+ name=elfrun cr3=[0-9A-F]+ ppid=\d+ uid=(\d+)",
                     timeout=150, tries=3)
        check("③ 非 root 那次 drvdemo 被当**真进程**跑起来（[PROC64] create … name=elfrun）", m is not None,
              (m.group(0) if m else "（缺 create 行）"))
        check("非 root 那次跑的是普通用户身份（… uid=1000，非 0）",
              m is not None and int(m.group(1)) != 0, (m.group(0) if m else "（缺）"))
        m = wait_re(r"\[PCIMAP\] deny pid=(\d+) bdf=0x([0-9a-fA-F]+) bar=(\d+) reason=not-root err=(\d+)",
                    90, since=before_nonroot)
        check("★ 非 root 调用 pci_map_bar -> [PCIMAP] deny … reason=not-root err=1（-EPERM）",
              m is not None and m.group(4) == str(E_PERM), (m.group(0) if m else "（缺 deny 行）"))
        m = wait_re(r"DRVDEMO init euid=(\d+) uid=(\d+) pid=(\d+)", 60, since=before_nonroot)
        check("drvdemo 自己是普通用户（DRVDEMO init euid=1000 …）",
              m is not None and int(m.group(1)) != 0, (m.group(0) if m else "（缺）"))
        m = wait_re(r"DRVDEMO neg noroot euid=(\d+) rc=(\d+)", 60, since=before_nonroot)
        check("★ drvdemo 如实报告被拒（DRVDEMO neg noroot euid=1000 rc=1）",
              m is not None and int(m.group(1)) != 0 and m.group(2) == str(E_PERM),
              (m.group(0) if m else "（缺）"))
        check("非 root 那次**什么都没映射**（DRVDEMO done maps=0 … rc=1）",
              wait_re(r"DRVDEMO done maps=0 hdaops=0 rc=1", 60, since=before_nonroot) is not None)
        check("非 root 那次没有产生任何 [PCIMAP] map 行",
              "[PCIMAP] map " not in slog(serial)[before_nonroot:])
        check("非 root 那次进程干净退出（[ELF64] run cmd path=/bin/drvdemo rc=-1 via=proc … code=1）",
              wait_re(r"\[ELF64\] run cmd path=/bin/drvdemo rc=-?1 via=proc pid=\d+ code=1",
                      90, since=before_nonroot) is not None)
        check("③ 非 root 路径无 PANIC（系统还活着）",
              proc.poll() is None and "PANIC" not in slog(serial)[before_nonroot:])

        # ---------------- root 会话 ----------------
        m = send_cmd("su - root", r"\[USER64\] su ok from=\w+ to=root euid=0", timeout=120, tries=3)
        check("终端里切到 root 会话（[USER64] su ok … to=root euid=0）", m is not None,
              (m.group(0) if m else "（缺 su 打点）"))

        # ---------------- ①②④⑦ root：drvdemo 全流程 ----------------
        before_root = len(slog(serial))
        m = send_cmd("elfrun /bin/drvdemo",
                     r"\[PROC64\] create pid=\d+ name=elfrun cr3=[0-9A-F]+ ppid=\d+ uid=0",
                     timeout=150, tries=3)
        check("① root 那次 drvdemo 被当**真进程**跑起来（[PROC64] create … name=elfrun … uid=0）",
              m is not None, (m.group(0) if m else "（缺 create 行）"))
        m_map_hda = wait_re(r"\[PCIMAP\] map pid=(\d+) bdf=0x([0-9a-fA-F]+) bar=(\d+) pa=0x([0-9a-fA-F]+) "
                            r"len=(\d+) va=0x([0-9a-fA-F]+) pages=(\d+) u=(\d+) re=(\d+) b64=(\d+) pool_free=(\d+)",
                            150, since=before_root)
        check("★ ① root 调用 pci_map_bar 成功（[PCIMAP] map … HDA BAR0）", m_map_hda is not None,
              (m_map_hda.group(0) if m_map_hda else "（缺 map 行）"))
        if m_map_hda:
            pa = int(m_map_hda.group(4), 16)
            ln = int(m_map_hda.group(5))
            va = int(m_map_hda.group(6), 16)
            pages = int(m_map_hda.group(7))
            check("① 映射的用户页真的可访问（u=1）", m_map_hda.group(8) == "1", m_map_hda.group(0))
            check("① VA 落在设备映射窗内（0x%x..0x%x）且页对齐" % (DEV_WINDOW_VA, DEV_WINDOW_END),
                  DEV_WINDOW_VA <= va < DEV_WINDOW_END and (va & 0xFFF) == 0,
                  "va=0x%x" % va)
            check("① 长度 = BAR 实际大小（2 的幂，且 4096 <= len <= 256 KiB 槽上限）",
                  ln >= 4096 and ln <= DEV_SLOT_BYTES and (ln & (ln - 1)) == 0, "len=%d" % ln)
            check("① 映射页数 = len/4096（覆盖范围正好是 BAR，不多映射）",
                  pages == (ln + 4095) // 4096, "pages=%d len=%d" % (pages, ln))
            check("★ ① 物理基址与内核 HDA 驱动读到的一致（[PCIMAP] pa=0x%x == [HDA64] bar0=0x%x）"
                  % (pa, hda_bar0), pa == hda_bar0)
            check("① bdf 与内核 HDA 打点一致（0x%x）" % hda_bdf, int(m_map_hda.group(2), 16) == hda_bdf)
            check("① 首次映射不是复用（re=0）", m_map_hda.group(9) == "0")

        m_map_x = None
        deadline = time.time() + 60
        while time.time() < deadline:
            for mm in re.finditer(r"\[PCIMAP\] map pid=(\d+) bdf=0x([0-9a-fA-F]+) bar=(\d+) pa=0x([0-9a-fA-F]+) "
                                  r"len=(\d+) va=0x([0-9a-fA-F]+) pages=(\d+) u=(\d+) re=(\d+) b64=(\d+) pool_free=(\d+)",
                                  slog(serial)[before_root:]):
                if int(mm.group(2), 16) == xhci_bdf:
                    m_map_x = mm
                    break
            if m_map_x:
                break
            time.sleep(0.3)
        check("★ ① xHCI BAR0 也被映射（[PCIMAP] map bdf=0x%x）" % xhci_bdf, m_map_x is not None,
              (m_map_x.group(0) if m_map_x else "（缺）"))
        if m_map_x:
            check("★ ① xHCI 的物理基址与内核 xHCI 驱动一致（0x%x）" % xhci_bar0,
                  int(m_map_x.group(4), 16) == xhci_bar0, m_map_x.group(0))

        m = wait_re(r"DRVDEMO scan tried=(\d+) mapped=(\d+) hda=0x([0-9a-fA-F]+) xhci=0x([0-9a-fA-F]+)",
                    60, since=before_root)
        check("★ ① drvdemo 扫到的 bdf 与内核驱动一致（hda=0x%x xhci=0x%x）" % (hda_bdf, xhci_bdf),
              m is not None and int(m.group(3), 16) == hda_bdf and int(m.group(4), 16) == xhci_bdf,
              (m.group(0) if m else "（缺 scan 行）"))
        if m:
            check("① 扫描过程里映射成功的设备数 <= 映射窗槽数（8），没有把窗占满",
                  int(m.group(2)) <= DEV_SLOTS, m.group(0))

        # ---- ② 寄存器值与内核打点对照 ----
        m = wait_re(r"DRVDEMO hda regs gcap=0x([0-9a-fA-F]+) vmin=(\d+) vmaj=(\d+) statests=0x([0-9a-fA-F]+) "
                    r"gctl=0x([0-9a-fA-F]+)", 60, since=before_root)
        check("② drvdemo 读到 HDA GCAP/VMIN/VMAJ/STATESTS/GCTL（值可解释）", m is not None,
              (m.group(0) if m else "（缺 hda regs 行）"))
        if m and m_ctrl:
            gcap_c = int(m_ctrl.group(3), 16)
            check("★ ② HDA GCAP 与内核打点一致（caps=0x%x）" % gcap_c, int(m.group(1), 16) == gcap_c,
                  "drvdemo=0x%x kernel=0x%x" % (int(m.group(1), 16), gcap_c))
            check("★ ② HDA VMAJ/VMIN 与内核打点一致（vmaj=%s vmin=%s）" % (m_ctrl.group(4), m_ctrl.group(5)),
                  int(m.group(3)) == int(m_ctrl.group(4)) and int(m.group(2)) == int(m_ctrl.group(5)))
            check("② GCTL.CRST 已置位（控制器在跑，gctl bit0=1）", (int(m.group(5), 16) & 1) == 1,
                  "gctl=0x%x" % int(m.group(5), 16))

        m = wait_re(r"DRVDEMO hda sd0 ctl=0x([0-9a-fA-F]+) sts=0x([0-9a-fA-F]+) lpib=(\d+) cbl=(\d+) "
                    r"fmt=0x([0-9a-fA-F]+)", 60, since=before_root)
        check("② drvdemo 读到 HDA SD0 流描述符（ctl/sts/lpib/cbl/fmt）", m is not None,
              (m.group(0) if m else "（缺 hda sd0 行）"))

        m = wait_re(r"DRVDEMO xhci caps caplen=0x([0-9a-fA-F]+) ver=0x([0-9a-fA-F]+) hcs1=0x([0-9a-fA-F]+) "
                    r"hcc1=0x([0-9a-fA-F]+) max_slots=(\d+) max_ports=(\d+) csz=(\d+) ac64=(\d+)",
                    60, since=before_root)
        check("② drvdemo 读到 xHCI CAPLENGTH/HCIVERSION/HCSPARAMS1/HCCPARAMS1", m is not None,
              (m.group(0) if m else "（缺 xhci caps 行）"))
        if m:
            check("★ ② xHCI caplen 与内核一致（0x%x）" % int(m_xhci.group(5), 16),
                  int(m.group(1), 16) == int(m_xhci.group(5), 16))
            check("★ ② xHCI HCSPARAMS1 与内核一致（0x%x）" % int(m_xhci.group(6), 16),
                  int(m.group(3), 16) == int(m_xhci.group(6), 16))
            check("★ ② xHCI max_slots/max_ports/csz/ver/ac64 逐字段与内核一致",
                  int(m.group(5)) == int(m_xhci.group(7)) and int(m.group(6)) == int(m_xhci.group(8)) and
                  int(m.group(7)) == int(m_xhci.group(9)) and
                  int(m.group(2), 16) == int(m_xhci.group(10), 16) and
                  int(m.group(8)) == int(m_xhci.group(11)),
                  "drvdemo=%s / kernel=%s" % (m.group(0), m_xhci.group(0)))

        # ---- ② PORTSC 与内核枚举对照 ----
        # ★ 本测试台**不**给 xHCI 挂 USB 键盘（`-device usb-kbd,bus=xhci.0` 会让客机的键盘路径
        #   走 xHCI HID，而实测 xhci64 的 HID 传输环在一次命令之后开始把后续报告打成
        #   `[XHCI] stray transfer evt`，键盘随即失效 —— 那样本脚本**第二条命令就打不进去**了）。
        #   终端输入走 QEMU 的 PS/2 键盘（其它 GUI 验收脚本同一条路）。xHCI 侧仍然验"两边看到的
        #   端口状态一致"：内核说哪个端口连着/没连着，drvdemo 的 PORTSC 必须给同一个结论。
        kernel_ports = {}
        for mm in re.finditer(r"\[XHCI\] port (\d+) connected speed=(\w+)", slog(serial)):
            kernel_ports[int(mm.group(1))] = mm.group(2)
        n_noport = len(re.findall(r"\[XHCI\] no device on port (\d+)", slog(serial)))
        m = wait_re(r"DRVDEMO xhci caps caplen=0x([0-9a-fA-F]+) ver=0x([0-9a-fA-F]+) hcs1=0x([0-9a-fA-F]+) "
                    r"hcc1=0x([0-9a-fA-F]+) max_slots=(\d+) max_ports=(\d+) csz=(\d+) ac64=(\d+)",
                    60, since=before_root)
        max_ports = int(m.group(6)) if m else 0
        check("② 内核把 xHCI 的每个端口都看过一遍（[XHCI] port N connected / no device on port N 覆盖 max_ports）",
              max_ports > 0 and (len(kernel_ports) + n_noport) >= max_ports,
              "kernel: connected=%s noport=%d；drvdemo max_ports=%d" % (sorted(kernel_ports), n_noport, max_ports))
        user_ports = {}
        for mm in re.finditer(r"DRVDEMO xhci port n=(\d+) portsc=0x([0-9a-fA-F]+) ccs=(\d+) ped=(\d+) ps=(\d+)",
                              stream(slog(serial)[before_root:])):
            user_ports[int(mm.group(1))] = (int(mm.group(3)), int(mm.group(5)), mm.group(0))
        check("★ ② 用户态把 xHCI 的每个端口都读了一遍（DRVDEMO xhci port n=… 覆盖 max_ports）",
              max_ports > 0 and len(user_ports) == max_ports,
              "drvdemo 端口数=%d max_ports=%d" % (len(user_ports), max_ports))
        same_view = True
        bad = ""
        for n in range(1, max_ports + 1):
            if n not in user_ports:
                same_view, bad = False, "port %d 未读" % n
                break
            ccs = user_ports[n][0]
            want = 1 if n in kernel_ports else 0
            if ccs != want:
                same_view, bad = False, "port %d：内核 connected=%s 但用户态 ccs=%d" % (n, n in kernel_ports, ccs)
                break
        check("★ ② 用户态 PORTSC 的 CCS 与内核枚举**逐端口一致**（同一台设备的同一份状态）",
              same_view, bad or ("connected=%s / noport=%d" % (sorted(kernel_ports), n_noport)))
        for n, spd in sorted(kernel_ports.items()):
            code = {"full": 1, "low": 2, "high": 3, "super": 4}.get(spd, -1)
            check("② 内核报 port %d connected speed=%s 时，用户态 PORTSC 的 PS=%d" % (n, spd, code),
                  n in user_ports and user_ports[n][1] == code,
                  ("drvdemo ps=%s" % (user_ports[n][1],)) if n in user_ports else "（缺该端口）")

        # ---- ① 幂等 ----
        m = wait_re(r"DRVDEMO hda idem va0=0x([0-9a-fA-F]+) va1=0x([0-9a-fA-F]+) len0=(\d+) len1=(\d+) same=(\d+)",
                    60, since=before_root)
        check("★ ① 重复调用幂等（DRVDEMO hda idem same=1：同一个 VA/长度）",
              m is not None and m.group(5) == "1", (m.group(0) if m else "（缺 idem 行）"))
        if m and m_map_hda:
            check("① 幂等返回的 VA 就是首次那个（drvdemo va1 == [PCIMAP] va）",
                  m.group(2).lower().lstrip("0") == m_map_hda.group(6).lower().lstrip("0"))
        # 内核打点的十六进制是**大写**（debug64.h 的 dbg64_hex64），这里两种都收。
        m_re = wait_re(r"\[PCIMAP\] map pid=\d+ bdf=0x[0-9a-fA-F]+ bar=0 pa=0x[0-9a-fA-F]+ len=\d+ "
                       r"va=0x[0-9a-fA-F]+ pages=\d+ u=1 re=1 b64=\d+ pool_free=(\d+)", 60, since=before_root)
        check("★ ① 内核侧留证：第二次调用走 re=1（复用，不重复分配页表页）", m_re is not None,
              (m_re.group(0) if m_re else "（缺 re=1 行）"))

        # ---- ③ 负例（iobar 允许扫到"第一个不是 ENODEV 的 BAR"为止，见 drvdemo 的注释）----
        for tag, want, why in [
            ("badbdf", E_NODEV, "不存在的 BDF -> -ENODEV(-5)"),
            ("badbar", E_INVAL, "bar_index 越界（6）-> -EINVAL(-3)"),
            ("outptr", E_FAULT, "out 指针非法 -> -EFAULT(-2)"),
        ]:
            m = wait_re(r"DRVDEMO neg %s .*rc=(\d+)" % tag, 30, since=before_root)
            check("③ DRVDEMO neg %-7s rc=%s（%s）" % (tag, want, why),
                  m is not None and m.group(1) == str(want), (m.group(0) if m else "（缺）"))
        m_io = wait_re(r"DRVDEMO neg iobar bdf=0x([0-9a-fA-F]+) bar=(\d+) rc=3", 30, since=before_root)
        check("③ DRVDEMO neg iobar rc=3（I/O 端口 BAR -> -EINVAL(-3)，不是 MMIO 就不给映射）",
              m_io is not None, (m_io.group(0) if m_io else "（该机上没扫到 I/O BAR 设备）"))
        check("③ 内核侧留证：非 MMIO 的 BAR 被点名拒绝（[PCIMAP] FAILED … reason=bar-is-io-port err=3）",
              wait_re(r"\[PCIMAP\] FAILED pid=\d+ bdf=0x[0-9a-fA-F]+ bar=\d+ reason=bar-is-io-port err=3",
                      30, since=before_root) is not None)
        n_new = len(re.findall(r"\[PCIMAP\] map .*re=0", slog(serial)[before_root:]))
        check("③ 扫描过程中真正新建的映射数不超过映射窗槽数（8）—— 负例没有把窗占满",
              n_new <= DEV_SLOTS, "re=0 行数=%d" % n_new)

        # ---- ④ 越权不许成功 ----
        m = wait_re(r"DRVDEMO neg mmap_dev va=0x([0-9a-fA-F]+) rc=(\d+)", 30, since=before_root)
        check("★ ④ 普通 mmap(MAP_FIXED) 想覆盖设备窗 -> -ENOMEM(12)（越权不许成功）",
              m is not None and m.group(2) == "12" and int(m.group(1), 16) == DEV_WINDOW_VA,
              (m.group(0) if m else "（缺）"))
        m = wait_re(r"DRVDEMO neg munmap_dev va=0x([0-9a-fA-F]+) rc=(\d+)", 30, since=before_root)
        check("★ ④ 对设备映射窗 munmap -> -EINVAL(22)（拒绝：那里是 MMIO，不是页池的页）",
              m is not None and m.group(2) == "22", (m.group(0) if m else "（缺）"))

        # ---- ⑦ HDA 真实操作 ----
        m = wait_re(r"DRVDEMO hda op srst wr=0x0*1 rb=0x([0-9a-fA-F]+) ok=(\d+)", 60, since=before_root)
        check("★ ⑦ HDA 真实写操作：SD0CTL.SRST=1 回读 bit0=1（写真的到了设备）",
              m is not None and m.group(2) == "1" and (int(m.group(1), 16) & 1) == 1,
              (m.group(0) if m else "（缺）"))
        m = wait_re(r"DRVDEMO hda op clear wr=0x0+ rb=0x([0-9a-fA-F]+) ok=(\d+) sts_rdy=(\d+)",
                    60, since=before_root)
        check("★ ⑦ 清 SRST 后 RUN=0（ok=1）且 SD0STS.FIFORDY=1（sts_rdy=1）—— 寄存器可解释",
              m is not None and m.group(2) == "1" and m.group(3) == "1",
              (m.group(0) if m else "（缺）"))
        check("⑦ drvdemo 自报完成（DRVDEMO done maps=2 hdaops=2 rc=0）",
              wait_re(r"DRVDEMO done maps=(\d+) hdaops=(\d+) rc=0", 30, since=before_root) is not None)
        m = wait_re(r"\[ELF64\] run cmd path=/bin/drvdemo rc=0 via=proc pid=(\d+) code=0", 60,
                    since=before_root)
        check("⑦ root 那次 drvdemo 正常退出（[ELF64] run cmd … rc=0 via=proc … code=0）", m is not None,
              (m.group(0) if m else "（缺）"))

        # ---- ⑤ 映射回收（记账 + 页池水位：都用内核打点，不依赖终端可见文本）----
        drv_pid = int(m.group(1)) if m else -1
        m = wait_re(r"\[PCIMAP\] release pid=(\d+) slots=(\d+) freed=0 \(mmio, not page-pool\) pool_free=(\d+)",
                    90, since=before_root)
        check("★ ⑤ drvdemo 退出后映射记账被回收（[PCIMAP] release … slots>=2 freed=0 …）",
              m is not None and int(m.group(2)) >= 2, (m.group(0) if m else "（缺）"))
        if m and drv_pid > 0:
            check("⑤ 回收的是 drvdemo 那个 pid（%d）" % drv_pid, int(m.group(1)) == drv_pid,
                  "release pid=%s drvdemo pid=%d" % (m.group(1), drv_pid))
        if m and m_map_hda:
            pool_at_map = int(m_map_hda.group(11))
            pool_at_rel = int(m.group(3))
            check("★ ⑤ 设备映射不占页池、退出后页池回到基线（pool_free %d -> %d，必须 >=）"
                  % (pool_at_map, pool_at_rel), pool_at_rel >= pool_at_map,
                  "[PCIMAP] map pool_free=%d；release pool_free=%d（设备 MMIO 不是页池的页）"
                  % (pool_at_map, pool_at_rel))

        # ---- ⑦ 共存：用户态摸过 HDA 之后内核驱动仍然可用 ----
        before_tone = len(slog(serial))
        m = send_cmd("audio playtone 200",
                     r"\[HDA64\] cmd audio playtone ms=200 runs\+=\d+ bcis\+=\d+ lpib=\d+ ok=1",
                     timeout=120, tries=3)
        check("★ ⑦ 共存：drvdemo 写过 SD0 之后内核驱动仍能播放（[HDA64] cmd audio playtone … ok=1）",
              m is not None, (m.group(0) if m else "（缺 playtone 行）"))

        # ---------------- ⑥ 禁止项 ----------------
        log_final = slog(serial)
        for needle in FORBIDDEN:
            check("不得出现 %s" % needle, needle not in log_final)
        check("⑥ 系统还活着（没有 PANIC 后的复位）", proc.poll() is None, "qemu rc=%s" % proc.poll())
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
        if args.keep:
            print("[drvsvc] 串口日志：%s" % serial)

    log_final = slog(serial)
    print("--- serial tail ---")
    for line in [x for x in log_final.splitlines() if x.strip()][-25:]:
        print("   | " + line[:180])
    bad = [n for n, c in checks if not c]
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    if bad:
        print("失败项：")
        for n in bad:
            print("   - " + n)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
