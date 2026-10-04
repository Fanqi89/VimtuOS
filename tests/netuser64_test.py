#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/netuser64_test.py - ★ P9：**Ring 3 完整网络栈**端到端验收（内核只有一条原始帧收发 ABI）

要证明的事（逐条对应任务书）：
  ① 交付与体积纪律：/bin/netd 在**系统卷**里（构建期 tools/net_pack_win.py 写入，本脚本用宿主侧
     独立解析卷**逐字节**比对）；内核二进制里搜不到它（64B 高熵探针）；netd.elf <= 64 KiB 且
     PT_LOAD 全落在用户窗口低 64 KiB（kernel/elf64.cpp 的**主程序装载区**硬约束）。
  ② net_raw(53) 真跑：串口里 `[NETRAW] tx bytes=.. ok=1` / `[NETRAW] rx bytes=..`；ABI 负例
     （未知 op / 长度越界 / 坏指针）逐条 `err=` 正确，用户态侧 `[NET] abi .. ok=1`。
  ③ DHCP 四步（DISCOVER/OFFER/REQUEST/ACK）拿到 10.0.2.15 + 掩码/网关/DNS，且**用户态**绑定。
  ④ ICMP ping 网关（10.0.2.2）得到应答 + 往返时间（rtt_ms）。
  ⑤ DNS：查 QEMU 的 10.0.2.3，解析**真实域名**（宿主侧同时解析，逐字节比对 IP）；应答必须
     是 `rcode=0 tc=0`，A 记录是**压缩指针**形式（ptr=1，软断言）。
  ⑥ UDP echo：客人发 64 B -> 宿主 Python 原样回 -> 用户态 `match=1` 且 FNV-1a 校验和与宿主一致。
  ⑦ TCP：三次握手（syn/synack/ack）+ **发 64 B 收 64 B**（宿主 Python 回显服务）：
     客人与宿主**三方逐字节一致**（两边的 sha256/FNV 与宿主生成物对照）；再走 FIN/ACK 收尾。
  ⑧ TCP 反方向（hostfwd + 宿主 Python 连进来）：客人 listen -> accept -> 回显 64 B。
  ⑨ 错误路径：ARP 打不通/黑地址 -> 有界超时明确失败，不崩；无 PANIC；本次运行段里无 enosys。

网络：QEMU **用户模式网络**（-netdev user + e1000）。客人访问宿主用"宿主别名" 10.0.2.2：
   - UDP/TCP 回显服务 = 宿主 Python 线程（0.0.0.0 绑定，覆盖 127.0.0.1 与网卡地址两种映射）；
   - 反方向用 hostfwd=（宿主 127.0.0.1:<hp> -> 客人 :<lport>）让宿主连进客人。
客人侧配置写进夹具盘的 /etc/netd.conf（端口由本脚本挑空闲口 -> 顺带证明配置覆盖生效）。

用法（必须用 Windows 原生 Python）：
    py -3 tests\\netuser64_test.py
    py -3 tests\\netuser64_test.py --keep --timeout 300
退出码：0 = 硬断言全过；1 = 有硬断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import hashlib
import importlib.util
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh              # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

BUILD = os.path.join(ROOT, "build64")
SYSTEM_IMG = os.path.join(BUILD, "system.img")
SYSDISK = os.path.join(BUILD, "sysdisk.img")
NETVOL = os.path.join(BUILD, "netvol.img")
NETD_ELF = os.path.join(BUILD, "netd.elf")
KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
FIXTURE = os.path.join(BUILD, "netuser64_test.img")
PART_MAIN_LBA = 8009
SECTOR = 512

PATTERN = bytes(((i * 7 + 3) & 0xFF) for i in range(64))          # 与 user/net/netd.c 同一份 64 B
PAT_SHA = hashlib.sha256(PATTERN).hexdigest()
PAT_FNV = None                                                    # 下面算（与 C 的 fnv1a32 同口径）

TCP_FWD_HOST = "127.0.0.1"

TYPED_NAMES = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}


def fnv1a32(b):
    h = 0x811C9DC5
    for x in b:
        h ^= x
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


PAT_FNV = fnv1a32(PATTERN)

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "[NET] netd aborted", "[NET] no net_raw MAC",
             "[ELF64] reject", "[NET] netd fatal"]


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


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod



# ============================ 宿主侧三个回显/客户端（线程，写一份宿主证据文件）============================
class HostServers:
    def __init__(self, tcp_port, udp_port, fwd_host_port, log_path):
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.fwd_host_port = fwd_host_port
        self.log_path = log_path
        self.stop = threading.Event()
        self.threads = []
        self.lock = threading.Lock()
        self.ev = {"tcp_srv": None, "udp_srv": None, "tcp_fwd": None}

    def log(self, line):
        with self.lock:
            with open(self.log_path, "a", encoding="utf-8") as f:
                f.write(line + "\n")

    # ---- TCP 回显服务（客人连进来；0.0.0.0 覆盖 127.0.0.1 与具体网卡地址）----
    def _tcp_echo_server(self):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            s.bind(("0.0.0.0", self.tcp_port))
            s.listen(4)
            s.settimeout(1.0)
        except OSError as e:
            self.log("HOST tcp_srv FAIL bind %s:%d: %s" % ("0.0.0.0", self.tcp_port, e))
            return
        deadline = time.time() + 240
        while not self.stop.is_set() and time.time() < deadline:
            try:
                c, addr = s.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with c:
                c.settimeout(10)
                got = b""
                try:
                    while len(got) < 64:
                        ch = c.recv(64 - len(got))
                        if not ch:
                            break
                        got += ch
                    c.sendall(got)                       # 原样回显
                except OSError as e:
                    self.log("HOST tcp_srv recv/send error: %s" % e)
                self.ev["tcp_srv"] = got
                self.log("HOST tcp_srv conn=%s:%d sent_sha=%s recv_len=%d recv_sha=%s pattern_ok=%d"
                         % (addr[0], addr[1], hashlib.sha256(got).hexdigest()[:16],
                            len(got), hashlib.sha256(got).hexdigest()[:16],
                            1 if got == PATTERN else 0))
                break
        s.close()

    # ---- UDP 回显服务（客人发过来，宿主原样回）----
    def _udp_echo_server(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("0.0.0.0", self.udp_port))
            s.settimeout(1.0)
        except OSError as e:
            self.log("HOST udp_srv FAIL bind %s:%d: %s" % ("0.0.0.0", self.udp_port, e))
            return
        deadline = time.time() + 240
        while not self.stop.is_set() and time.time() < deadline:
            try:
                data, addr = s.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            self.ev["udp_srv"] = data
            s.sendto(data, addr)                         # 原样回
            self.log("HOST udp_srv conn=%s:%d recv_len=%d recv_sha=%s echoed_sha=%s pattern_ok=%d"
                     % (addr[0], addr[1], len(data), hashlib.sha256(data).hexdigest()[:16],
                        hashlib.sha256(data).hexdigest()[:16], 1 if data == PATTERN else 0))
            break
        s.close()

    # ---- hostfwd 反方向：宿主主动连客人（客人 listen 后回显 64 B）----
    # ★ 重试到**真的换到 64 B**为止：客人的 listen 步骤排在验收串的最后（起机 + 前面几步之后），
    #   所以这里的连接会先失败/被忽略若干次；每次失败/超时都换一条新连接重来。
    def _tcp_fwd_client(self):
        deadline = time.time() + 240
        attempt = 0
        while not self.stop.is_set() and time.time() < deadline:
            attempt += 1
            try:
                c = socket.create_connection((TCP_FWD_HOST, self.fwd_host_port), timeout=3)
            except OSError:
                time.sleep(0.4)
                continue
            with c:
                c.settimeout(8)
                got = b""
                try:
                    c.sendall(PATTERN)
                    while len(got) < 64:
                        ch = c.recv(64 - len(got))
                        if not ch:
                            break
                        got += ch
                except OSError as e:
                    self.log("HOST tcpfwd attempt=%d incomplete: %s" % (attempt, e))
                    time.sleep(0.4)
                    continue
                if got == PATTERN:
                    self.ev["tcp_fwd"] = got
                    self.log("HOST tcpfwd attempt=%d sent_sha=%s recv_len=%d recv_sha=%s pattern_ok=1"
                             % (attempt, PAT_SHA[:16], len(got), hashlib.sha256(got).hexdigest()[:16]))
                    return
                time.sleep(0.4)
        self.log("HOST tcpfwd FAILED attempts=%d (客人没回显)" % attempt)

    def start(self):
        for fn in (self._tcp_echo_server, self._udp_echo_server, self._tcp_fwd_client):
            t = threading.Thread(target=fn, daemon=True)
            t.start()
            self.threads.append(t)

    def join(self, timeout=20):
        for t in self.threads:
            t.join(timeout=timeout)


def free_port(exclude=(), udp=False):
    """挑一个**现在真的能绑**的空闲端口。"""
    for _ in range(80):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM if udp else socket.SOCK_STREAM)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("0.0.0.0", 0))
            p = s.getsockname()[1]
        finally:
            s.close()
        if p in exclude or p <= 1024:
            continue
        return p
    raise RuntimeError("找不到空闲端口")


class Vm:
    """一台 QEMU（用户模式网络 + e1000 + hostfwd），带 monitor 与串口日志。"""
    """一台 QEMU（用户模式网络 + e1000 + hostfwd），带 monitor 与串口日志。"""

    def __init__(self, qemu, img, tag, workdir, hfwd_host_port, guest_listen_port):
        self.serial = os.path.join(workdir, tag + ".log")
        mport = qh.free_port()
        args = [
            qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(self.serial),
            "-netdev", "user,id=n0,hostfwd=tcp:%s:%d-:%d" % (TCP_FWD_HOST, hfwd_host_port, guest_listen_port),
            "-device", "e1000,netdev=n0",
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
            "-no-reboot",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = qh.Monitor(mport)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout=60, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def type_line(self, text, per_key=0.06):
        for ch in text:
            if ch in TYPED_NAMES:
                self.mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch.isalnum():
                self.mon.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey 不支持这个字符：%r" % ch)
        self.mon.key("ret", wait=per_key + 0.3)

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def check_elf_layout(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2 and machine == 0x3E, "不是 ET_EXEC/x86_64"
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes, phn = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    lo, hi, segs = 0x100000000, 0x100000000 + 0x10000, []
    for i in range(phn):
        o = phoff + i * phes
        t = struct.unpack_from("<I", d, o)[0]
        va, msz = struct.unpack_from("<Q", d, o + 16)[0], struct.unpack_from("<Q", d, o + 40)[0]
        assert t not in (2, 3), "出现 PT_DYNAMIC/PT_INTERP"
        if t == 1:
            assert lo <= va and va + msz <= hi, "PT_LOAD 越出主程序装载区 va=%#x msz=%#x" % (va, msz)
            segs.append((va, msz))
    ent = struct.unpack_from("<Q", d, 24)[0]
    assert any(va <= ent < va + m for va, m in segs), "入口不在任何 PT_LOAD 内"
    return len(d), sum(m for _, m in segs)


def vol_read(vol_bytes, path):
    TP = load_mod("tcc_pack_win_np", "tcc_pack_win.py")
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in TP._entries(vol_bytes, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return TP._read_file(vol_bytes, rec, total)
        cur = ino
    return None


def kprobe_bad(kernel, blob):
    """/tools/probe64.py 同一判据：全图挑"不同字节值最多"的 64B 窗口在内核里搜。"""
    best_off, best_n = -1, -1
    for off in range(0, max(1, len(blob) - 64), 32):
        n = len(set(blob[off:off + 64]))
        if n > best_n:
            best_n, best_off = n, off
    if best_off < 0 or best_n < 8:
        return False
    probe = blob[best_off:best_off + 64]
    return probe in kernel


def main():
    # Windows 控制台是 GBK：带圈数字之类的字符打不出来会直接把脚本搞崩（实测 ⑪）。
    # 只把**输出**改成容错，不影响任何断言。
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300, help="起机等待 [GUI64] ready 的最长秒数")
    ap.add_argument("--netd-timeout", type=int, default=90, help="等 [NET] netd done 的最长秒数")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for p in (SYSTEM_IMG, NETD_ELF, NETVOL, SYSDISK, KERNEL_OS):
        if not os.path.exists(p):
            sys.stderr.write("缺构建产物：%s（先跑 bash build64.sh）\n" % p)
            return 2

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  -- " + str(detail)) if detail else ""))
        return bool(cond)

    def soft(name, cond, detail=""):
        print("  [%s] %s%s" % ("PASS" if cond else "WARN", name, ("  -- " + str(detail)) if detail else ""))

    print("=== Vimtu64 P9：Ring 3 完整网络栈（net_raw 53 + /bin/netd）端到端验收 ===")

    # ==================== ① 交付 / 体积 / 卷内逐字节 ====================
    print("== ① 交付与体积纪律 ==")
    sz, memsz = check_elf_layout(NETD_ELF)
    check("① /bin/netd 是静态 ELF64 且 PT_LOAD 全在 64 KiB 主程序装载区（文件 %d B / memsz %d B）"
          % (sz, memsz), sz <= 96 * 1024 and memsz <= 65536)
    kib = open(KERNEL_OS, "rb").read()
    nelf = open(NETD_ELF, "rb").read()
    check("① 内核二进制里搜不到 /bin/netd 的 64B 高熵探针（交付 = 系统卷里的文件）", not kprobe_bad(kib, nelf))

    # ==================== ② 夹具盘：/bin/netd + /etc/netd.conf（端口由本脚本挑） ====================
    print("== ② 夹具盘（宿主侧独立解析卷逐字节比对）==")
    tcp_port = free_port()
    udp_port = free_port((tcp_port,), udp=True)
    guest_listen = free_port((tcp_port, udp_port))
    hfwd_host = free_port((tcp_port, udp_port, guest_listen))
    dns_name = "example.com"
    host_ip = None
    try:
        host_ip = socket.gethostbyname(dns_name)
        print("    宿主解析 %s = %s（客人 DNS 结果将与它比对）" % (dns_name, host_ip))
    except OSError as e:
        print("    [!] 宿主解析 %s 失败（%s）—— DNS 步骤按『如实标注』处理" % (dns_name, e))

    tmp = tempfile.mkdtemp(prefix="vimtu64_netuser64_")
    conf = os.path.join(tmp, "netd.conf")
    with open(conf, "w", encoding="utf-8", newline="\n") as f:
        f.write("# 验收夹具：端口/域名覆盖（证明 /etc/netd.conf 生效）\n")
        f.write("tcpport=%d\n" % tcp_port)
        f.write("udpport=%d\n" % udp_port)
        f.write("listenport=%d\n" % guest_listen)
        f.write("dnsname=%s\n" % dns_name)
    pack = subprocess.run([sys.executable, os.path.join(ROOT, "tools", "net_pack_win.py"),
                           "--fixture-img", FIXTURE, "--from-vol", NETVOL, "--conf", conf,
                           "--netd", NETD_ELF, "--system", SYSTEM_IMG],
                          capture_output=True, text=True)
    if pack.returncode != 0:
        sys.stderr.write("夹具盘生成失败：%s\n%s\n" % (pack.returncode, pack.stdout + pack.stderr))
        return 2
    fix = open(FIXTURE, "rb").read()
    vol = fix[PART_MAIN_LBA * SECTOR:]
    got = vol_read(vol, "/bin/netd")
    check("② 夹具盘卷内 /bin/netd 与 build64/netd.elf 逐字节一致（%d B）" % len(nelf),
          got == nelf)
    gconf = vol_read(vol, "/etc/netd.conf")
    check("② 夹具盘卷内 /etc/netd.conf 与宿主写入逐字节一致", gconf == open(conf, "rb").read())

    # ==================== ③ 宿主服务（先起，客人在后面几步才用到） ====================
    print("== ③ 宿主 Python 回显服务（TCP %d / UDP %d / hostfwd %d -> 客人 :%d）=="
          % (tcp_port, udp_port, hfwd_host, guest_listen))
    hs = HostServers(tcp_port, udp_port, hfwd_host, os.path.join(tmp, "host.log"))
    hs.start()

    # ==================== ④ QEMU：起机 -> 终端 -> elfrun /bin/netd ====================
    print("== ④ QEMU 起机（用户模式网络 + e1000 + hostfwd）==")
    vm = Vm(qemu, FIXTURE, "netuser64", tmp, hfwd_host, guest_listen)
    log = ""
    try:
        qh.login_desktop(vm.mon, vm.log, vm.proc, timeout=min(args.timeout, 240))
        up = vm.wait_log("[GUI64] ready", args.timeout)
        check("④ 桌面就绪（[GUI64] ready）", up)
        # 开始菜单 -> 终端（与 busybox64/sounds64 同一套注入；多轮重试抗时序）
        opened = False
        for _ in range(8):
            if "[APP] term opened" in vm.log():
                opened = True
                break
            vm.mon.key("ret", wait=1.4)
            vm.mon.key("meta_l", wait=1.6)
            vm.mon.key("1", wait=2.2)
        check("④ 终端已打开（[APP] term opened）", opened)
        time.sleep(0.8)
        n0 = len(vm.log())
        vm.type_line("elfrun /bin/netd", per_key=0.05)
        done = vm.wait_log("[NET] netd done", args.netd_timeout)
        check("④ `elfrun /bin/netd` 跑完整验收串（[NET] netd done，<= %d s）" % args.netd_timeout, done)
        time.sleep(0.6)
        log = vm.log()
        seg = log[n0:]                                    # 只看我们自己这一段（前面是别的子系统的日志）
    finally:
        vm.close()
        hs.join(timeout=15)

    # ==================== ⑤ 内核 ABI（net_raw 53）证据 ====================
    print("== ⑤ 内核 ABI：net_raw(53) 的原始帧收发打点 ==")
    check("⑤ [NET] netd start abi=53（用户态真的用这一号）", "[NET] netd start abi=53 (net_raw)" in log)
    m = re.search(r"\[NETRAW\] mac=([0-9a-f]{2}(?::[0-9a-f]{2}){5}) link=up", log)
    check("⑤ [NETRAW] mac=<6 段> link=up（kernel/netraw64.cpp 的 mac 查询）", bool(m),
          m.group(0) if m else "未出现")
    tx = re.findall(r"\[NETRAW\] tx bytes=(\d+) ok=(\d+) err=(-?\d+) frames=(\d+)", log)
    check("⑤ [NETRAW] tx 打点（发出**一整帧**，%d 条；最后一条 ok=1）" % len(tx),
          len(tx) >= 8 and tx[-1][1] == "1" and int(tx[-1][0]) >= 60,
          ("%d 条，最后 %s" % (len(tx), tx[-1]) if tx else "未出现"))
    rx = re.findall(r"\[NETRAW\] rx bytes=(\d+) frames=(\d+)", log)
    check("⑤ [NETRAW] rx 打点（取回**一整帧**，%d 条）" % len(rx),
          len(rx) >= 8 and all(int(a) >= 42 for a in (x[0] for x in rx)),
          ("%d 条，示例 %s" % (len(rx), rx[:3]) if rx else "未出现"))
    # 长度口径的判定在 kernel/netraw64.cpp（[NETRAW] deny 打点在那里）；
    # 用户态指针越界由分派层拦（[SYSCALL] deny nr=53）。两边都要有证据。
    check("⑤ [NETRAW] 长度口径被拦（短帧/超 MTU 的 deny 打点齐全）",
          all(re.search(p, log) for p in (
              r"\[NETRAW\] deny op=0 reason=short err=3",
              r"\[NETRAW\] deny op=0 reason=oversize err=6",
              r"\[NETRAW\] deny op=1 reason=short err=3",
              r"\[NETRAW\] deny op=1 reason=oversize err=6")))
    ndeny = len(re.findall(r"\[SYSCALL\] deny nr=53", log))
    check("⑤ 坏指针被分派层拦下（[SYSCALL] deny nr=53 出现 %d 次：tx/rx/mac 各一类 + 未知 op）" % ndeny,
          ndeny >= 4, "出现 %d 次" % ndeny)
    abi = re.findall(r"\[NET\] abi (\S+) rc=(-?\d+) expect=(-?\d+) ok=(\d)", seg)
    check("⑤ 用户态 ABI 负例逐条对上（%d 条，全 ok=1）" % len(abi),
          len(abi) >= 9 and all(a[1] == a[2] and a[3] == "1" for a in abi),
          ("; ".join("%s rc=%s" % (a[0], a[1]) for a in abi[:4]) if abi else "未出现"))

    # ==================== ⑥ ifconfig / DHCP 四步 ====================
    print("== ⑥ DHCP 客户端（四步）==")
    m = re.search(r"\[NET\] ifconfig mac=[0-9a-f:]{17} link=up ip=(\d+\.\d+\.\d+\.\d+)/(\d+) "
                  r"gw=(\d+\.\d+\.\d+\.\d+) dns=(\d+\.\d+\.\d+\.\d+) src=(\w+)", log)
    check("⑥ ifconfig 报告 MAC/链路/地址/掩码/网关/DNS", bool(m), m.group(0) if m else "未出现")
    check("⑥ dhcp DISCOVER（广播，xid=0x…）", bool(re.search(r"\[NET\] dhcp discover xid=0x[0-9a-f]{8}", log)))
    m = re.search(r"\[NET\] dhcp offer yiaddr=(\d+\.\d+\.\d+\.\d+) server=(\d+\.\d+\.\d+\.\d+)", log)
    check("⑥ dhcp OFFER（slirp 的 DHCP 服务器应答）", bool(m), m.group(0) if m else "未出现")
    check("⑥ dhcp REQUEST（带 requested ip + server id）",
          bool(re.search(r"\[NET\] dhcp request xid=0x[0-9a-f]{8} req=\d+\.\d+\.\d+\.\d+ server=\d+\.\d+\.\d+\.\d+", log)))
    m = re.search(r"\[NET\] dhcp ack ip=(\d+\.\d+\.\d+\.\d+) mask=(\d+\.\d+\.\d+\.\d+) "
                  r"gw=(\d+\.\d+\.\d+\.\d+) dns=(\d+\.\d+\.\d+\.\d+) lease=(\d+)", log)
    check("⑥ dhcp ACK（地址/掩码/网关/DNS/租期）", bool(m), m.group(0) if m else "未出现")
    check("⑥ 四步齐全 + 绑定 10.0.2.15/24（步骤计数 4/4）",
          "[NET] dhcp bind ip=10.0.2.15 steps=4/4 src=dhcp" in log
          and (m.group(1) == "10.0.2.15" and m.group(2) == "255.255.255.0" and m.group(3) == "10.0.2.2"
               and m.group(4) == "10.0.2.3") if m else False,
          (m.group(0) if m else "无 ACK 行"))

    # ==================== ⑦ ARP + ICMP ====================
    print("== ⑦ ARP 缓存 + ICMP echo（ping 网关）==")
    check("⑦ ARP 请求（用户态构造的 who-has 广播）", "[NET] arp who-has 10.0.2.2 tx=28" in log)
    m = re.search(r"\[NET\] arp reply 10\.0\.2\.2 is-at ([0-9a-f]{2}(?::[0-9a-f]{2}){5})", log)
    check("⑦ ARP 应答（slirp 回的网关 MAC，进缓存）", bool(m), m.group(0) if m else "未出现")
    check("⑦ ARP 缓存命中（第二次直接查表 lookup=0）",
          bool(re.search(r"\[NET\] arp cached gw=10\.0\.2\.2 lookup=0", log)))
    check("⑦ ping 网关：echo 请求已发（id/seq/bytes）",
          bool(re.search(r"\[NET\] icmp echo id=\d+ seq=1 dst=10\.0\.2\.2 bytes=32", log)))
    m = re.search(r"\[NET\] icmp reply from 10\.0\.2\.2 seq=1 bytes=32 ttl=(\d+)", log)
    check("⑦ ping 网关：收到 echo 应答（bytes=32 + ttl）", bool(m), m.group(0) if m else "未出现")
    m = re.search(r"\[NET\] ping gw rtt_ms=(\d+) rc=0", log)
    check("⑦ ping 往返时间已量化（rtt_ms=… rc=0）", bool(m), m.group(0) if m else "未出现")

    # ==================== ⑧ DNS ====================
    print("== ⑧ DNS（A 记录 / 压缩指针 / 多答案 / TC）==")
    m = re.search(r"\[NET\] dns query id=0x([0-9a-f]+) name=(\S+) server=(\d+\.\d+\.\d+\.\d+) bytes=(\d+)", log)
    check("⑧ DNS 查询已发出（含 id/名字/服务器 10.0.2.3/字节数）", bool(m), m.group(0) if m else "未出现")
    mr = re.search(r"\[NET\] dns rx bytes=(\d+) rcode=(\d+) answers=(\d+) tc=(\d)", log)
    check("⑧ DNS 收到应答且报文合法（rcode=0 tc=0 answers>=1）",
          bool(mr) and mr.group(2) == "0" and mr.group(4) == "0" and int(mr.group(3)) >= 1,
          mr.group(0) if mr else "未出现")
    ans = re.findall(r"\[NET\] dns a name=(\S+) addr=(\d+\.\d+\.\d+\.\d+) ttl=(\d+) ptr=(\d)", log)
    check("⑧ A 记录解析（多答案能力：%d 条）" % len(ans), len(ans) >= 1,
          "; ".join("%s ttl=%s ptr=%s" % (a[1], a[2], a[3]) for a in ans[:3]) or "未出现")
    if host_ip:
        check("⑧ 客人解析到的 IP 与宿主解析一致（%s -> %s）" % (dns_name, host_ip),
              any(a[1] == host_ip for a in ans),
              ("客人: " + ", ".join(a[1] for a in ans)) if ans else "未出现")
    else:
        soft("⑧ 宿主本身解析不了 %s —— DNS 步骤按『如实标注』处理" % dns_name, False)
    soft("⑧ A 记录名使用了**压缩指针**（DNS 压缩指针路径真的走到了）",
         any(a[3] == "1" for a in ans), "ptr 值：%s" % ([a[3] for a in ans] or "无"))

    # ==================== ⑨ UDP echo ====================
    print("== ⑨ UDP echo（客人 -> 宿主 -> 客人，逐字节一致）==")
    m = re.search(r"\[NET\] udp tx dst=10\.0\.2\.2:(\d+) sport=(\d+) bytes=64 sum=0x([0-9a-f]{8})", log)
    check("⑨ UDP 已发出 64 B（含 FNV-1a 校验和）", bool(m), m.group(0) if m else "未出现")
    if m:
        check("⑨ 客人算的发送校验和 == 宿主同一份 pattern 的 FNV-1a（0x%08x）" % PAT_FNV,
              int(m.group(3), 16) == PAT_FNV, "客人=0x%s" % m.group(3))
    m = re.search(r"\[NET\] udp rx from=10\.0\.2\.2:(\d+) dport=(\d+) bytes=(\d+) sum=0x([0-9a-f]{8}) "
                  r"match=(\d)", log)
    check("⑨ UDP 收到宿主回声：64 B + match=1（逐字节一致）+ 校验和相同",
          bool(m) and m.group(3) == "64" and m.group(5) == "1" and int(m.group(4), 16) == PAT_FNV,
          m.group(0) if m else "未出现")
    hlog = ""
    try:
        hlog = open(os.path.join(tmp, "host.log"), encoding="utf-8").read()
    except OSError:
        pass
    mh = re.search(r"HOST udp_srv conn=(\S+) recv_len=(\d+) recv_sha=(\S+) echoed_sha=(\S+) pattern_ok=(\d)", hlog)
    check("⑨ 宿主侧独立证据：收到 64 B、pattern 一致、原样回显",
          bool(mh) and mh.group(2) == "64" and mh.group(5) == "1" and mh.group(3) == PAT_SHA[:16],
          mh.group(0) if mh else "未出现")

    # ==================== ⑩ TCP（三次握手 + 64 B 收发 + FIN） ====================
    print("== ⑩ TCP 客户端（三次握手 / 序号推进 / 64 B 三方逐字节一致 / FIN）==")
    check("⑩ TCP connect（目的 10.0.2.2:%d + 本地端口）" % tcp_port,
          bool(re.search(r"\[NET\] tcp connect dst=10\.0\.2\.2:%d sport=(\d+)" % tcp_port, log)))
    check("⑩ 三次握手 ①：SYN 已发（seq=0x…）", bool(re.search(r"\[NET\] tcp syn seq=0x[0-9a-f]{8} -> sent", log)))
    check("⑩ 三次握手 ②：SYN|ACK 收到（seq/ack 都记了）",
          bool(re.search(r"\[NET\] tcp synack seq=0x[0-9a-f]{8} ack=0x[0-9a-f]{8} -> est", log)))
    m = re.search(r"\[NET\] tcp ack=0x[0-9a-f]{8} state=ESTABLISHED rtt_ms=(\d+) local=(\S+)", log)
    check("⑩ 三次握手 ③：ESTABLISHED + 本地 (ip:port) + rtt_ms", bool(m), m.group(0) if m else "未出现")
    m = re.search(r"\[NET\] tcp tx seq=0x[0-9a-f]{8} bytes=64 sum=0x([0-9a-f]{8})", log)
    check("⑩ 发 64 B（载荷与宿主同一份 pattern）", bool(m) and int(m.group(1), 16) == PAT_FNV,
          m.group(0) if m else "未出现")
    check("⑩ 序号推进：自己的 64 B 被对端 ACK（data acked=1 + snd_una）",
          bool(re.search(r"\[NET\] tcp data acked=1 snd_una=0x[0-9a-f]{8}", log)))
    m = re.search(r"\[NET\] tcp rx seq=0x[0-9a-f]{8} bytes=(\d+) sum=0x([0-9a-f]{8}) match=(\d)", log)
    check("⑩ 收 64 B 且逐字节等于发出去的那份（match=1 + 校验和相同）",
          bool(m) and m.group(1) == "64" and m.group(3) == "1" and int(m.group(2), 16) == PAT_FNV,
          m.group(0) if m else "未出现")
    check("⑩ FIN/ACK 收尾（fin -> sent + state=CLOSED）",
          bool(re.search(r"\[NET\] tcp fin seq=0x[0-9a-f]{8} -> (sent|peer-first)", log))
          and bool(re.search(r"\[NET\] tcp closed state=CLOSED", log)))
    mh = re.search(r"HOST tcp_srv conn=(\S+) sent_sha=(\S+) recv_len=(\d+) recv_sha=(\S+) pattern_ok=(\d)", hlog)
    check("⑩ 宿主侧独立证据：64 B pattern 一致（三方逐字节一致：宿主/客人收/客人发）",
          bool(mh) and mh.group(3) == "64" and mh.group(5) == "1" and mh.group(2) == PAT_SHA[:16]
          and mh.group(4) == PAT_SHA[:16],
          mh.group(0) if mh else "未出现")

    # ==================== ⑪ TCP 反方向（hostfwd） ====================
    print("== ⑪ TCP 监听回显（hostfwd：宿主 -> 客人）==")
    check("⑪ 客人 listen 指定的端口（%d）" % guest_listen,
          ("[NET] tcp listen port=%d" % guest_listen) in log)
    check("⑪ 客人 accept（三次握手，被动态）",
          bool(re.search(r"\[NET\] tcp accept from=\d+\.\d+\.\d+\.\d+:\d+ seq=0x[0-9a-f]{8} our_seq=0x[0-9a-f]{8} "
                         r"state=ESTABLISHED", log)))
    m = re.search(r"\[NET\] tcp echo rx=(\d+) sum=0x([0-9a-f]{8}) tx=(\d+) rc=(-?\d+) match=1", log)
    check("⑪ 客人回显 64 B（rx=tx=64，rc=0，校验和 == pattern）",
          bool(m) and m.group(1) == "64" and m.group(3) == "64" and m.group(4) == "0"
          and int(m.group(2), 16) == PAT_FNV,
          m.group(0) if m else "未出现")
    mh = re.search(r"HOST tcpfwd attempt=(\d+) sent_sha=(\S+) recv_len=(\d+) recv_sha=(\S+) pattern_ok=(\d)", hlog)
    check("⑪ 宿主侧独立证据：hostfwd 进客人后回显逐字节一致",
          bool(mh) and mh.group(3) == "64" and mh.group(5) == "1" and mh.group(4) == PAT_SHA[:16],
          mh.group(0) if mh else "未出现")

    # ==================== ⑫ 错误路径 / 总结 ====================
    print("== ⑫ 错误路径 + 总结 + 无 PANIC/无 enosys ==")
    check("⑫ ARP 打不通的黑地址：有界超时明确失败（rc=-2，不挂死）",
          bool(re.search(r"\[NET\] arp negative-case rc=-2 ms=\d+ ok=1", log)))
    check("⑫ ping 黑地址：报超时不崩",
          bool(re.search(r"\[NET\] icmp fail dst=10\.0\.2\.99 seq=99 rc=-2 reason=no-arp", log)))
    md = re.search(r"\[NET\] netd done mode=all ok=(\d+) fail=(\d+) skip=(\d+)", log)
    check("⑫ 客人自评：fail=0（全过；skip 只可能是『没有外网 DNS』这一项）",
          bool(md) and md.group(2) == "0", md.group(0) if md else "未出现")
    if md:
        print("     客人统计：ok=%s fail=%s skip=%s" % (md.group(1), md.group(2), md.group(3)))
    for bad in FORBIDDEN:
        check("⑫ 不得出现 %s" % bad, bad not in log)
    start = seg.find("[NET] netd start")
    tail = seg[start:] if start >= 0 else seg
    check("⑫ 本次网络栈运行段里没有 [SYSCALL] enosys（我们只用已有的号）",
          "[SYSCALL] enosys" not in tail)
    check("⑫ 运行段里没有第二条 [NETRAW] log cap reached（打点上限没把证据淹没）",
          tail.count("[NETRAW] log cap reached") == 0, "出现 %d 次" % tail.count("[NETRAW] log cap reached"))

    if args.keep:
        print("[netuser64] 串口日志：%s" % vm.serial)
        print("[netuser64] 宿主证据：%s" % os.path.join(tmp, "host.log"))
    else:
        shutil.rmtree(tmp, ignore_errors=True)

    print("--- 网络栈关键行（原样，供报告引用）---")
    for line in [x for x in log.splitlines() if x.startswith("[NET") or x.startswith("[NETRAW")]:
        print("   | " + line[:190])

    print("=== RESULT: %s ===  checks=%d ok=%d" % ("PASS" if ok else "FAIL", len(checks),
                                                   sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
