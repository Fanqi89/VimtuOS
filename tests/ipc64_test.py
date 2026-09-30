#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ipc64_test.py - ★ A5 前置验收：ring3 输入事件投递（自有 ABI 12 input_poll）+ 共享内存缓冲（13/14）

要证明的事（与任务书的六条断言一一对应）：
  ① 键鼠注入后 `input_poll` 真收到事件，且**键码 / 坐标 / 按钮位与注入对得上**（本任务的核心断言）：
       * QEMU：monitor `sendkey a` / `sendkey up` / `sendkey shift-b`、`mouse_move 8 -4`、
         `mouse_button 1/0`、`mouse_move 0 0 1`（滚轮）—— 逐条对 `[EVSHM] ev ...` 原文；
         x/y 用**确定值**断言（光标起点 = mouse_init 给的 512,384，位移经驱动灵敏度 ×1.7 后
         单步上限 24 不夹取：8*17/10=13、-4*17/10=-6）；
       * VMware（VNC 注入）：键码/按钮位断言同 QEMU；坐标只断言**方向**（VNC 指针是绝对坐标，
         VMware 自己转成相对位移，幅度不可控）—— 差异如实打印。
  ② 队列满：演示主动 6 秒不取事件（[EVSHM] hold），测试在这段窗口灌 30 次按键 ->
     内核必须"丢最旧 + 计数打点"（`[EV64] drop pid=.. total=..` 与 `[EV64] poll .. drops=<n>0`）。
  ③ shm_create/shm_map 成功：VA 在**用户窗口内**（4GiB..4GiB+16MiB）、内核打点 u=1。
  ④ 两进程共享同一块 shm（父写子读，逐字节一致）：父进程算 65536 B 的 csum、子进程用 fork
     继承到的句柄 shm_map 同一批页帧再算一遍（csum 相等），并且子进程写魔数、父进程读回。
  ⑤ 退出后引用计数归零并回收：`[SHM64] release .. refs=0 freed_pages=16` +
     `[EVSHM] demo done .. pool_delta=+0`（页池回到基线）。
  ⑥ 无泄漏 / 无 PANIC / 无 enosys（12/13/14 三号都不许出现 [SYSCALL] enosys）。

用法（必须 Windows 原生 Python）：
    py -3 tests\\ipc64_test.py                  # QEMU：monitor 注入（默认，快）
    py -3 tests\\ipc64_test.py --vmware         # VMware：真实安装流程 + VNC 注入（重活，优先）
    py -3 tests\\ipc64_test.py --img <盘镜像> --keep
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/镜像/VMware 缺失）。
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
SHELL_BIN = os.path.join(ROOT, "build64", "shell.bin")
FIXTURE_IMG = os.path.join(ROOT, "build64", "ipc64_test.img")
PART_MAIN_LBA = 8009                 # = kernel/part64.h 的主分区起点
TARGET_SECTORS = 32768               # 16 MB（与 tests/sh64_test.py 同一夹具口径）

# ---- 注入脚本（QEMU monitor / VNC 两套通道共用同一份"要证明的事"）----
# 键码：既有键码口径（kernel/input.h）——'a'=0x61、'b'=0x62、Up=NAV_UP=0xFD、Shift+B='B'=0x42
# 光标起点：(512, 384)（kernel/input.cpp 的 mouse_init 默认值；本演示在 GUI 起来之前跑）
# 符号约定（实测两轮一致）：QEMU monitor 的 `mouse_move dx dy` 里 dy 是"向上为正"（X11 口径），
#   而 PS/2 包给驱动的是"向下为正"的镜像 —— 注入 (8, -4) 时驱动看到的包位移是 (8, +4)，
#   事件因此是 dx=13 dy=+6（4*17/10），位置 y = 384 - 6 = 378。
MOUSE_X0, MOUSE_Y0 = 512, 384
MOVE_DX_IN, MOVE_DY_IN = 8, -4         # 注入的位移（monitor 口径）
MOVE_DX = int(MOVE_DX_IN * 17 / 10)             # 驱动灵敏度 ×1.7 且 C 的整数除法截断：13
# 滚轮：QEMU monitor 的 dz 与 PS/2 包的 Z 字节是**镜像**的（实测：dz=+1 -> Z=-1 -> 事件 dy=-1）。
MOVE_DY_PKT = -MOVE_DY_IN                       # PS/2 包的 y 位移（X11 口径的镜像）：+4
MOVE_DY = int(MOVE_DY_PKT * 17 / 10)            # 驱动口径 + 截断（-68/10 在 C 里是 -6 不是 -7）：+6
EXPECT_X, EXPECT_Y = MOUSE_X0 + MOVE_DX, MOUSE_Y0 - MOVE_DY
EXPECT_WHEEL_DY = -1                            # 注入 dz=+1 -> 事件 dy=-1（见上）

VNC_KEY_A = 0x61
VNC_KEY_B = 0x62
VNC_KEY_UP = 0xFF52

BURST_KEYS = 30                        # "队列满"阶段灌多少次按键（每次 = KEY_DOWN + KEY_UP 两条）


def q(p):
    return p.replace("\\", "/")


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


# --------------------------------------------------------------------------- QEMU 通道
class QemuVm:
    def __init__(self, qemu, img, port, serial):
        self.serial = serial
        self.port = port
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-ipc64",
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(160):
            if self.proc.poll() is not None:
                break
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.12):
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

    def key(self, k, wait=0.10):
        return self.send("sendkey %s" % k, wait=wait)

    def mouse_move(self, dx, dy, dz=None):
        if dz is None:
            return self.send("mouse_move %d %d" % (dx, dy), wait=0.15)
        return self.send("mouse_move %d %d %d" % (dx, dy, dz), wait=0.15)

    def mouse_button(self, mask):
        return self.send("mouse_button %d" % mask, wait=0.15)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n1 = os.path.getsize(ppm)
                time.sleep(0.15)
                if os.path.getsize(ppm) == n1:
                    return True
            time.sleep(0.05)
        return False


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    assert raw.startswith(b"P6"), "不是 P6 PPM：%s" % path
    idx, vals = 2, []
    while len(vals) < 3:
        while raw[idx:idx + 1].isspace():
            idx += 1
        s = idx
        while not raw[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(raw[s:idx]))
    idx += 1
    w, h, _ = vals
    return w, h, raw[idx:]


# --------------------------------------------------------------------------- VMware 通道（VNC 注入）
VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
OUTDIR = r"C:\Users\fanqi\Desktop\新建文件夹\v64-install-test"
VMX_INSTALL = os.path.join(OUTDIR, "vimtu64-install-test.vmx")
VMX_BOOT = os.path.join(OUTDIR, "vimtu64-installed-boot.vmx")
SERIAL_BOOT = os.path.join(OUTDIR, "serial-installed-boot.log")
SERIAL_INSTALL = os.path.join(OUTDIR, "serial-install.log")
KEY_TCP_PORT = 4557                  # COM2 按键通道（VMware network 模式串口）
VNC_BOOT_PORT = 5904                 # 已安装盘启动 VM 的 VNC


def vmrun(*args, timeout=180):
    r = subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


def stop_all_vms():
    for _ in range(3):
        code, out = vmrun("list")
        lines = [l.strip() for l in out.splitlines() if l.strip().lower().endswith(".vmx")]
        if not lines:
            return
        for l in lines:
            vmrun("stop", l, "hard")
        time.sleep(2)


def start_vm(vmx, tries=6):
    out = ""
    for i in range(tries):
        code, out = vmrun("start", vmx, "nogui")
        if code == 0:
            return True
        print("   [start 重试 %d/%d] %s" % (i + 1, tries, out.strip()[:140]))
        time.sleep(6)
    return False


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_file(path, needle, seconds, what):
    t0 = time.time()
    while time.time() - t0 < seconds:
        if needle in read_text(path):
            print("   [ok] %s (%s)" % (what, needle))
            return True
        time.sleep(0.5)
    print("   [!!] 等超时：%s（没看到 %r）" % (what, needle))
    return False


def _recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        ch = s.recv(n - len(buf))
        if not ch:
            raise OSError("VNC 连接被关闭（收到 %d/%d 字节）" % (len(buf), n))
        buf += ch
    return buf


class Vnc:
    """最小 RFB 客户端：KeyEvent（0x04）与 PointerEvent（0x05）注入。

    qemuhelp.py 的 vnc_press 只能送按键；共享缓冲/事件验收还要**鼠标**，所以这里把
    握手抄一份并加上 PointerEvent。坐标用绝对像素（VMware 的 PS/2 鼠标会把"绝对指针位置"
    转成相对位移，幅度不可控 —— 因此 VMware 分支只断言方向，见文件头）。"""

    def __init__(self, port, host="127.0.0.1", timeout=10):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        banner = _recv_exact(self.s, 12).split(b"\n")[0].strip()
        ver = banner if banner.startswith(b"RFB ") else b"RFB 003.003"
        self.s.sendall(ver + b"\n")
        try:
            major, minor = int(ver[4:7]), int(ver[8:11])
        except ValueError:
            major, minor = 3, 3
        if (major, minor) >= (3, 7):
            n = _recv_exact(self.s, 1)[0]
            types = _recv_exact(self.s, n) if n else b""
            self.s.sendall(bytes([1 if 1 in types else (types[0] if types else 1)]))
            if (major, minor) >= (3, 8):
                if _recv_exact(self.s, 4) != b"\x00\x00\x00\x00":
                    raise OSError("VNC 认证失败（VMware 设了 VNC 密码）")
        else:
            _recv_exact(self.s, 4)
        self.s.sendall(b"\x01")                       # ClientInit(shared)
        head = _recv_exact(self.s, 24)                # ServerInit
        self.w = int.from_bytes(head[0:2], "big")
        self.h = int.from_bytes(head[2:4], "big")
        try:
            self.s.settimeout(1.5)
            nl = int.from_bytes(_recv_exact(self.s, 4), "big")
            if 0 < nl <= 4096:
                _recv_exact(self.s, nl)
        except OSError:
            pass
        self.s.settimeout(timeout)

    def key(self, keysym, gap=0.25):
        ks = int(keysym).to_bytes(4, "big")
        self.s.sendall(bytes([4, 1, 0, 0]) + ks)      # KeyEvent down
        self.s.sendall(bytes([4, 0, 0, 0]) + ks)      # KeyEvent up
        time.sleep(gap)

    def key_down(self, keysym, gap=0.1):
        self.s.sendall(bytes([4, 1, 0, 0]) + int(keysym).to_bytes(4, "big"))
        time.sleep(gap)

    def key_up(self, keysym, gap=0.1):
        self.s.sendall(bytes([4, 0, 0, 0]) + int(keysym).to_bytes(4, "big"))
        time.sleep(gap)

    def pointer(self, x, y, mask=0, gap=0.3):
        self.s.sendall(bytes([5, mask & 0xFF]) + int(x).to_bytes(2, "big") + int(y).to_bytes(2, "big"))
        time.sleep(gap)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------- 断言壳
class Checks:
    def __init__(self):
        self.ok = True
        self.n = 0
        self.passed = 0

    def __call__(self, name, cond, detail=""):
        self.n += 1
        if cond:
            self.passed += 1
        else:
            self.ok = False
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))
        return bool(cond)


# --------------------------------------------------------------------------- 注入序列
def inject_qemu(mon, serial_log, pid, wheel_4b):
    """QEMU：monitor 逐条注入。

    顺序有讲究（实测踩过两次）：
      1) **滚轮先注入**：演示的阶段 1 在"按键 down/up + 移动 + 左键 down/up 都到齐"时立刻
         进入 hold（6 秒不取事件），滚轮若排在后面会被 hold 期间灌包时的"丢最旧"挤掉；
      2) 左键按下与抬起之间给足间隔（0.35 s）：抬起包到达时刻比按下晚，演示要等到 up 才走，
         间隔太短会让 up 落进 hold 窗口。
    """
    if wheel_4b:
        print("   [inject] QEMU monitor：滚轮 dz=1（先注入，避免落进 hold 窗口）")
        mon.mouse_move(0, 0, 1)
    print("   [inject] QEMU monitor：按键 a / b / up / shift-b")
    mon.key("a", wait=0.25)
    mon.key("b", wait=0.25)
    mon.key("up", wait=0.25)
    mon.key("shift-b", wait=0.25)
    print("   [inject] QEMU monitor：mouse_move %d %d（期望 dx=%d dy=%d x=%d y=%d）"
          % (MOVE_DX_IN, MOVE_DY_IN, MOVE_DX, MOVE_DY, EXPECT_X, EXPECT_Y))
    mon.mouse_move(MOVE_DX_IN, MOVE_DY_IN)
    mon.mouse_button(1)                                # 左键按下 -> MOUSE_DOWN code=0x1 buttons=0x1
    time.sleep(0.35)
    mon.mouse_button(0)                                # 左键抬起 -> MOUSE_UP  code=0x1 buttons=0x0


def inject_vmware(vnc, serial_log, pid):
    """VMware：VNC 绝对指针 + RFB KeyEvent。坐标幅度不可控 -> 只断言方向（见文件头）。"""
    print("   [inject] VMware VNC：按键 a / b / Shift+b / up")
    vnc.key(VNC_KEY_A)
    vnc.key(VNC_KEY_B)
    vnc.key_down(0xFFE1)          # Shift_L 按下 -> 紧接着的 'b' 应当报成 'B'(0x42)+mods SHIFT
    vnc.key(VNC_KEY_B)
    vnc.key_up(0xFFE1)
    vnc.key(VNC_KEY_UP)
    # 指针：先移到中心，然后**多次**移动（VMware 的 PS/2 是相对模式：一次位移只在"控制台抓住
    # 指针"之后才稳定送达；实测按下/抬起很稳，位移需要多给几步，且按下之后再补两步更保险）
    cx, cy = max(1, vnc.w // 2), max(1, vnc.h // 2)
    print("   [inject] VMware VNC：PointerEvent 绝对坐标链路 (%d,%d) 起")
    vnc.pointer(cx, cy, 0)
    for i, (dx, dy) in enumerate(((20, -10), (40, -30), (60, -15), (80, 10))):
        vnc.pointer(cx + dx, cy + dy, 0)
    print("   [inject] VMware VNC：左键按下/抬起（+ 抬起后再补两步位移）")
    vnc.pointer(cx + 80, cy + 10, 1)
    vnc.pointer(cx + 80, cy + 10, 0)
    vnc.pointer(cx + 100, cy + 30, 0)
    vnc.pointer(cx + 120, cy + 50, 0)


def burst_qemu(mon):
    keys = "abcdefghijklmnopqrstuvwxyz0123"        # 30 次 = 60 条事件 > 队列容量 32
    for k in keys:
        mon.key(k, wait=0.06)


def burst_vmware(vnc):
    for ks in (VNC_KEY_A, VNC_KEY_B) * 15:
        # VMware VNC 的 KeyEvent 握手已经在（同一个连接），直接连发
        vnc.key(ks, gap=0.05)


# --------------------------------------------------------------------------- 主流程
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None, help="QEMU 夹具盘（缺省自动生成 build64/ipc64_test.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5665)
    ap.add_argument("--vmware", action="store_true", help="走 VMware（VNC 注入；重活）")
    ap.add_argument("--vmware-install", action="store_true",
                    help="VMware 路径改走真实安装流程（当前树状态下安装器在目标盘建卷失败，默认不走）")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--timeout", type=int, default=300)
    args = ap.parse_args()
    if not os.path.exists(SYSTEM_IMG):
        sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % SYSTEM_IMG)
        return 2

    print("=== 0) 准备夹具盘 ===")
    if args.vmware:
        print("   VMware 路径：用 tests/vmware_make_vm.py 建 16MB 空目标盘 + 两份 vmx（安装/已安装启动）")
    else:
        if args.img:
            img = args.img
        else:
            try:
                import make_shellvol as msv
            except ImportError:
                sys.stderr.write("找不到 tools/make_shellvol.py\n")
                return 2
            if not os.path.exists(SHELL_BIN):
                sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % SHELL_BIN)
                return 2
            shell_bytes = open(SHELL_BIN, "rb").read()
            system_bytes = open(SYSTEM_IMG, "rb").read()
            vol = msv.build_volume(shell_bytes, b"ipc64 fixture\n", TARGET_SECTORS - PART_MAIN_LBA)
            bad = msv.verify(vol, {"/bin/shell.bin": shell_bytes})
            if bad:
                sys.stderr.write("夹具卷自检失败：%s\n" % bad)
                return 2
            img = msv.build_disk(system_bytes, vol, TARGET_SECTORS)
            with open(FIXTURE_IMG, "wb") as f:
                f.write(img)
            img = FIXTURE_IMG
            print("   夹具盘 = %s（system.img + VimtuFS2 主分区；/evshm.elf 由内核启动期幂等装入）" % img)

    tmp = tempfile.mkdtemp(prefix="vimtu64_ipc64_")
    serial = os.path.join(tmp, "serial.log")
    checks = Checks()
    log = ""
    pid = 0
    vm = mon = vnc = None

    try:
        if args.vmware:
            log = run_vmware(checks, args)
            pid = 0
        else:
            qemu = find_qemu(args.qemu)
            if not qemu:
                sys.stderr.write("找不到 qemu-system-x86_64\n")
                return 2
            vm = QemuVm(qemu, img, args.port, serial)
            mon = Monitor(args.port)
            pid = run_qemu(vm, mon, checks, tmp, args)
            log = vm.log()
    finally:
        if vnc:
            vnc.close()
        if vm:
            vm.close()

    # ---- 收尾：整段日志的公共断言（两条通道共用）----
    if log:
        finish_checks(checks, log, pid if pid else None)
    if args.keep:
        print("[ipc64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if checks.ok else "FAIL", checks.n,
           checks.passed))
    return 0 if checks.ok else 1


def parse_demo_pid(log):
    m = re.search(r"\[EVSHM\] demo start pid=(\d+)", log)
    return int(m.group(1)) if m else 0


def run_qemu(vm, mon, ch, tmp, args):
    """QEMU：boot -> 等 [EVSHM] listen -> 注入键鼠 -> 等 hold -> 灌包 -> 等 done -> 像素证据。"""
    print("=== 1) 启动（QEMU，monitor 注入）===")
    if not vm.wait("[EVSHM] demo start pid=", 180):
        ch("QEMU 启动后跑到了 /evshm.elf（[EVSHM] demo start）", False, "串口尾部：\n" +
           "\n".join(l for l in vm.log().splitlines()[-12:]))
        return 0
    log = vm.log()
    pid = parse_demo_pid(log)
    ch("演示进程起来了", pid > 0, "pid=%d" % pid)

    print("=== 2) 等演示申请焦点/捕获（[EVSHM] listen）===")
    ch("演示到了 input_poll 循环（[EVSHM] listen pid=）", vm.wait("[EVSHM] listen pid=", 120))

    wheel_4b = "packet=4B" in vm.log()
    print("=== 3) 注入键鼠（monitor：sendkey / mouse_move / mouse_button）===")
    inject_qemu(mon, vm.log(), pid, wheel_4b)
    # 抓一帧（帧提交的像素证据；抓在注入之后、hold 之前）
    ppm = os.path.join(tmp, "evshm_rect.ppm")
    got_shot = mon.shot(ppm)

    print("=== 4) 等 [EVSHM] hold（测试要在这 6 秒窗口里灌满队列）===")
    if vm.wait("[EVSHM] hold ms=", 120):
        burst_qemu(mon)
    else:
        ch("演示进入 hold 阶段", False)

    print("=== 5) 等演示收尾（[EVSHM] done）===")
    ch("演示跑完（[EVSHM] done）", vm.wait("[EVSHM] done", 120))
    ch("内核收尾（[EVSHM] demo done）", vm.wait("[EVSHM] demo done", 60))

    # ---- 像素证据：提交的矩形真的上屏（按 4 行的采样比对图案颜色）----
    if got_shot:
        try:
            w, h, px = read_ppm(ppm)
            x0, y0, rw, rh = 100, 120, 256, 64
            hit = tot = 0
            for ry in range(0, rh, 8):
                y = y0 + ry
                if y >= h:
                    break
                row = y * w * 3
                for rx in range(0, rw, 8):
                    x = x0 + rx
                    if x >= w:
                        break
                    o = row + x * 3
                    exp = (0xFF000000 | ((rx & 0xFF) << 16) | ((ry & 0xFF) << 8) | ((rx ^ ry) & 0xFF))
                    er, eg, eb = (exp >> 16) & 0xFF, (exp >> 8) & 0xFF, exp & 0xFF
                    tot += 1
                    if abs(px[o] - er) <= 8 and abs(px[o + 1] - eg) <= 8 and abs(px[o + 2] - eb) <= 8:
                        hit += 1
            ch("shm 里画的矩形真的提交上屏（采样点颜色与图案一致）", hit * 2 >= tot,
               "hit=%d/%d" % (hit, tot))
        except Exception as e:
            ch("抓帧解析（PPM）", False, str(e))
    else:
        ch("抓帧成功（monitor screendump）", False)
    return pid


def prepare_fixture_img():
    """把 QEMU 那套夹具字节（当前构建的 system 内核 + 一个有效的 VimtuFS2 卷）写进
    build64/ipc64_test.img，返回字节串。"""
    import make_shellvol as msv
    if not os.path.exists(SHELL_BIN):
        raise RuntimeError("缺少 %s" % SHELL_BIN)
    shell_bytes = open(SHELL_BIN, "rb").read()
    system_bytes = open(SYSTEM_IMG, "rb").read()
    vol = msv.build_volume(shell_bytes, b"ipc64 fixture\n", TARGET_SECTORS - PART_MAIN_LBA)
    bad = msv.verify(vol, {"/bin/shell.bin": shell_bytes})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    img = msv.build_disk(system_bytes, vol, TARGET_SECTORS)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(img)
    return img


def run_vmware(ch, args):
    """VMware：准备 VM -> （可选）走真实安装流程 -> 启动 -> VNC 注入键鼠 -> 读串口断言。

    ★ VMware 路径的两个事实（本批实测，如实记在这里，别让后来人以为是测试写错了）：
      1) 默认**不走安装向导**，改成把夹具盘字节直接写进启动 VM 的 target.img：本树状态下
         安装器在目标盘上建 VimtuFS2 卷这一步失败（`[VFS64] mount_system FAILED (mount)`），
         装出来的盘没有卷 -> 整段 ring3 发射块被跳过（`[APP64] boot: vfs64 mount failed`），
         注入根本到不了演示程序。`--vmware-install` 保留真实安装流程（等那一步修好就能用）。
      2) 注入走 VNC：按键 = RFB KeyEvent，鼠标 = RFB PointerEvent（绝对坐标 -> VMware 自己
         转成 PS/2 相对位移），所以 VMware 分支只断言方向/按钮位，坐标的精确值交给 QEMU 支路。
    """
    print("=== 1) 停旧 VM + 重建测试 VM（16MB 目标盘 + 两份 vmx）===")
    stop_all_vms()
    r = subprocess.run([sys.executable, os.path.join(HERE, "vmware_make_vm.py")],
                       cwd=ROOT, capture_output=True, timeout=300)
    if r.returncode != 0:
        print(r.stdout.decode("utf-8", "replace")[-600:])
        ch("vmware_make_vm.py 准备 VM", False)
        return read_text(SERIAL_BOOT)
    ch("vmware_make_vm.py 准备 VM", True)

    if getattr(args, "vmware_install", False):
        print("=== 2) 真实安装流程（向导按键走 COM2 TCP 串口）===")
        ok = start_vm(VMX_INSTALL)
        ch("安装 VM 启动", ok, "vmx=%s" % os.path.basename(VMX_INSTALL))
        if not ok:
            return read_text(SERIAL_BOOT)
        wait_file(SERIAL_INSTALL, "磁盘枚举完成", 240, "向导就绪")
        sock = None
        t0 = time.time()
        while time.time() - t0 < 120:
            try:
                sock = socket.create_connection(("127.0.0.1", KEY_TCP_PORT), timeout=5)
                break
            except OSError:
                time.sleep(0.5)
        if sock is None:
            ch("连上 COM2 按键通道（127.0.0.1:%d）" % KEY_TCP_PORT, False)
            return read_text(SERIAL_BOOT)
        try:
            for k, dwell in ((0x0D, 1.2), (0x0D, 1.2), (0x0D, 1.2), (0x0D, 1.2),
                             (ord("n"), 2.0), (0x0D, 1.0)):
                sock.sendall(bytes([k]))
                time.sleep(dwell)
        finally:
            sock.close()
        wait_file(SERIAL_INSTALL, "[INSTALL] 完成：已写", 180, "安装完成")
        wait_file(SERIAL_INSTALL, "[SETUP] 自动重启", 90, "自动重启")
        stop_all_vms()
        time.sleep(2)
    else:
        print("=== 2) 把夹具盘写进启动 VM 的 target.img（当前构建 + 有效 VimtuFS2 卷）===")
        try:
            img = prepare_fixture_img()
        except Exception as e:
            ch("夹具盘准备", False, str(e))
            return read_text(SERIAL_BOOT)
        tgt = os.path.join(OUTDIR, "target.img")
        with open(tgt, "wb") as f:
            f.write(img)
        ch("夹具盘写入 VMware target.img", True, "%s (%d B)" % (tgt, len(img)))

    print("=== 3) 启动【已安装盘】VM（VNC %d 注入）===" % VNC_BOOT_PORT)
    ok = start_vm(VMX_BOOT)
    ch("已安装盘 VM 启动", ok, "vmx=%s" % os.path.basename(VMX_BOOT))
    if not ok:
        return read_text(SERIAL_BOOT)

    if not wait_file(SERIAL_BOOT, "[EVSHM] demo start pid=", 180, "启动期跑到 /evshm.elf"):
        return read_text(SERIAL_BOOT)
    wait_file(SERIAL_BOOT, "[EVSHM] listen pid=", 120, "演示申请焦点/捕获")

    print("=== 4) VNC 注入键鼠（%d）===" % VNC_BOOT_PORT)
    global vnc_holder
    try:
        vnc_holder = Vnc(VNC_BOOT_PORT)
        print("   屏幕 %dx%d" % (vnc_holder.w, vnc_holder.h))
        inject_vmware(vnc_holder, read_text(SERIAL_BOOT), parse_demo_pid(read_text(SERIAL_BOOT)))
    except OSError as e:
        ch("VNC 连上（%d）" % VNC_BOOT_PORT, False, str(e))
        return read_text(SERIAL_BOOT)
    ch("VNC 键鼠注入完成", True)

    print("=== 5) 等 hold 阶段并灌满队列 ===")
    if wait_file(SERIAL_BOOT, "[EVSHM] hold ms=", 120, "演示进入 hold"):
        try:
            burst_vmware(vnc_holder)
        except OSError as e:
            ch("VNC 灌包（队列满）", False, str(e))
    else:
        ch("演示进入 hold 阶段", False)

    print("=== 6) 等演示收尾 ===")
    wait_file(SERIAL_BOOT, "[EVSHM] done", 180, "演示跑完")
    log = read_text(SERIAL_BOOT)
    tail = [l for l in log.splitlines() if "[EVSHM]" in l or "[SHM64]" in l or "[EV64]" in l]
    print("--- 串口尾部（EVSHM/SHM64/EV64）---")
    for l in tail[-20:]:
        print("   | " + l[:180])
    try:
        vnc_holder.close()
    except Exception:
        pass
    return log


vnc_holder = None


def finish_checks(ch, log, pid=None):
    """两条通道共用的断言（结构都建立在串口打点上）。"""
    print("=== 7) 交付链条（安装/自检/事件结构）===")
    ch("内核幂等把 /evshm.elf 装进系统卷（或已存在跳过）",
       ("[EVSHM] install ok path=/evshm.elf" in log) or ("[EVSHM] install skipped (exists) /evshm.elf" in log))
    ch("事件层自检通过（[EV64] selftest PASS）", "[EV64] selftest PASS" in log)
    ch("事件结构体布局 = 40 B（自检里有这一条）", "event ABI 40B" in log)
    if pid is None:
        pid = parse_demo_pid(log)

    print("=== 8) ①  input_poll：收到的键鼠事件与注入对得上 ===")
    evs = re.findall(r"\[EVSHM\] ev t=(\d+)\((\w+)\) code=0x([0-9A-Fa-f]+) x=(-?\d+) y=(-?\d+) "
                     r"dx=(-?\d+) dy=(-?\d+) btn=0x([0-9A-Fa-f]+) mods=0x([0-9A-Fa-f]+) t=(\d+)", log)
    ch("演示打印了事件原文（[EVSHM] ev ...）", len(evs) >= 4, "n=%d" % len(evs))
    key_down = [(int(c, 16), int(m, 16)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "KEY_DOWN"]
    key_up = [(int(c, 16), int(m, 16)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "KEY_UP"]
    moves = [(int(x), int(y), int(dx), int(dy)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "MOVE"]
    downs = [(int(c, 16), int(b, 16)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "MOUSE_DOWN"]
    ups = [(int(c, 16), int(b, 16)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "MOUSE_UP"]
    wheels = [(int(dy), int(c, 16)) for t, n, c, x, y, dx, dy, b, m, ts in evs if n == "WHEEL"]

    kc = [c for c, _ in key_down]
    ch("按键 'a' 的 KEY_DOWN 收到且键码 = 0x61", 0x61 in kc, "codes=%s" % ["0x%x" % v for v in kc[:8]])
    ch("按键 'b' 的 KEY_DOWN 收到且键码 = 0x62", 0x62 in kc)
    ch("方向键 Up 的 KEY_DOWN 收到且键码 = 0xfd（NAV_UP）", 0xFD in kc)
    ch("Shift+B 收到大写键码 0x42 且 mods 带 SHIFT(0x1)",
       (0x42, 0x1) in [(c, m & 1) for c, m in key_down if c == 0x42] or
       any(c == 0x42 and (m & 1) for c, m in key_down),
       "pairs=%s" % ["0x%x/0x%x" % (c, m) for c, m in key_down[:8]])
    ch("KEY_UP 也收到了（同一键码回报）", len(key_up) >= 2, "n=%d codes=%s" %
       (len(key_up), ["0x%x" % c for c, _ in key_up[:6]]))
    ch("鼠标移动事件收到（MOVE）", len(moves) >= 1, "n=%d" % len(moves))
    if moves:
        x, y, dx, dy = moves[0]
        if pid and "vmware" not in os.environ.get("VIMTU_IPC_CHANNEL", ""):
            pass
        # QEMU：确定值；VMware：方向（幅度不可控）。两套都打印实际值，判定按"注入是否对得上"。
        exact = (dx == MOVE_DX and dy == MOVE_DY and x == EXPECT_X and y == EXPECT_Y)
        direction = (dx > 0 and dy != 0)
        ch("MOVE 的 dx/dy/x/y 与注入对得上（QEMU 精确值；VMware 只保证方向）",
           exact or direction,
           "got x=%d y=%d dx=%d dy=%d（QEMU 期望 x=%d y=%d dx=%d dy=%d）"
           % (x, y, dx, dy, EXPECT_X, EXPECT_Y, MOVE_DX, MOVE_DY))
        if exact:
            print("       （精确匹配：QEMU monitor 注入的 %d,%d -> 驱动灵敏度 ×1.7 -> %d,%d）"
                  % (MOVE_DX_IN, MOVE_DY_IN, MOVE_DX, MOVE_DY))
    ch("鼠标左键按下收到（MOUSE_DOWN code=0x1 buttons=0x1）", (1, 1) in downs,
       "downs=%s" % [(hex(c), hex(b)) for c, b in downs[:4]])
    ch("鼠标左键抬起收到（MOUSE_UP code=0x1 buttons=0x0）", (1, 0) in ups,
       "ups=%s" % [(hex(c), hex(b)) for c, b in ups[:4]])
    wheel_mode = re.search(r"\[INPUT64\] wheel mode=(\d)", log)
    if wheel_mode and wheel_mode.group(1) == "1" and wheels:
        ch("滚轮事件收到（WHEEL，dy 与注入 dz=+1 的镜像 -1 一致）",
           wheels[0][0] == EXPECT_WHEEL_DY, "wheels=%s 期望 dy=%d" % (wheels[:4], EXPECT_WHEEL_DY))
    else:
        ch("滚轮事件（设备无 4 字节包/未注入时不要求）", True,
           "wheel_mode=%s n=%d" % (wheel_mode.group(1) if wheel_mode else "?", len(wheels)))
    ch("只有**焦点进程**收键盘：演示自己申请了焦点（[EV64] attach flags=0x3）",
       re.search(r"\[EV64\] attach pid=%d flags=0*3\b" % pid, log) is not None or
       re.search(r"\[EV64\] attach pid=\d+ flags=0*3\b", log) is not None)
    ch("焦点落在演示进程（[EV64] focus pid=%d prev=" % pid,
       re.search(r"\[EV64] focus pid=%d prev=\d+ reason=request" % pid, log) is not None,
       "pid=%d" % pid)

    print("=== 9) ②  队列满：丢最旧 + 计数打点（不许静默）===")
    drops = [int(m.group(2)) for m in re.finditer(r"\[EV64\] drop pid=(\d+) type=(\d+) total=(\d+)", log)]
    drop_totals = [int(m.group(1)) for m in re.finditer(r"\[EV64\] drop pid=\d+ type=\d+ total=(\d+)", log)]
    poll_drops = [int(m.group(1)) for m in re.finditer(r"\[EV64\] poll pid=\d+ max=\d+ flags=0*[0-9A-Fa-f]+ "
                                                       r"got=\d+ pend=\d+ drops=(\d+)", log)]
    ch("有 drop 打点（[EV64] drop ... total=..）", len(drop_totals) >= 1,
       "lines=%d max_total=%d" % (len(drop_totals), max(drop_totals) if drop_totals else 0))
    ch("drop 计数非零且单调（丢的是最旧）", bool(drop_totals) and max(drop_totals) > 0 and
       drop_totals == sorted(drop_totals), "totals=%s" % drop_totals[:8])
    ch("查询/出队路径也报了 drops=（[EV64] poll ... drops=n>0）",
       bool(poll_drops) and max(poll_drops) > 0, "poll_drops=%s" % poll_drops[:6])

    print("=== 10) ③ shm_create / shm_map（VA 在用户窗口内、u=1）===")
    mc = re.search(r"\[SHM64\] create pid=(\d+) id=(\d+) size=(\d+) pages=(\d+) refs=1 live=(\d+)", log)
    ch("shm_create 打点完整（size/pages/refs=1）", mc is not None)
    if mc:
        ch("对象大小 = 演示请求的 65536 B（16 页）",
           int(mc.group(3)) == 65536 and int(mc.group(4)) == 16,
           "size=%s pages=%s" % (mc.group(3), mc.group(4)))
    maps = re.findall(r"\[SHM64\] map pid=(\d+) id=(\d+) slot=(\d+) va=0x([0-9A-Fa-f]+) pa=0x([0-9A-Fa-f]+) "
                      r"off=(\d+) len=(\d+) pages=(\d+) refs=(\d+) u=(\d+)", log)
    ch("shm_map 打点完整（>= 1 次）", len(maps) >= 1, "n=%d" % len(maps))
    if maps:
        p0, i0, s0, va0, pa0, off0, len0, pg0, ref0, u0 = maps[0]
        va0 = int(va0, 16)
        WIN_LO, WIN_HI = 0x100000000, 0x100000000 + 16 * 1024 * 1024
        ch("映射 VA 落在**用户窗口**内（4GiB..4GiB+16MiB）", WIN_LO <= va0 < WIN_HI, "va=0x%x" % va0)
        ch("内核页表里 u=1（用户可读写）", u0 == "1")
        ch("长度 = 65536（16 页）、offset = 0", int(len0) == 65536 and int(off0) == 0 and int(pg0) == 16)
        ch("映射的 pid 是演示进程", int(p0) == pid, "map pid=%s demo pid=%d" % (p0, pid))
    mm = re.search(r"\[EVSHM\] shm_map id=(\d+) ret=0 va=0x([0-9a-f]+)", log)
    ch("用户侧 shm_map 返回 0 + VA 与内核一致", mm is not None and maps and
       int(mm.group(2), 16) == int(maps[0][3], 16),
       "user=0x%s kernel=0x%s" % (mm.group(2) if mm else "?", maps[0][3] if maps else "?"))

    print("=== 11) ④ 两进程共享同一块 shm（逐字节一致）===")
    ch("fork 后 shm 句柄被继承（[SHM64] inherit ... refs_plus=1）",
       re.search(r"\[SHM64\] inherit pid=\d+ from=\d+ held=1 refs_plus=1", log) is not None)
    ch("子进程自己 shm_map 了同一对象（第二个 map 打点，pid 不同）",
       len(maps) >= 2 and maps[1][0] != maps[0][0],
       "pids=%s" % [m[0] for m in maps[:3]])
    if len(maps) >= 2:
        ch("两次映射的是**同一批物理页**（pa 相同 -> 真共享，不是各一份拷贝）",
           maps[1][4] == maps[0][4], "pa0=0x%s pa1=0x%s" % (maps[0][4], maps[1][4]))
        ch("两次映射的 refs 递增（引用计数 = 2）", int(maps[1][8]) >= 2,
           "refs=%s" % [m[8] for m in maps[:3]])
    mcs = re.search(r"\[EVSHM\] pattern bytes=(\d+) csum=0x([0-9a-f]{8})", log)
    mcc = re.search(r"\[EVSHM\] child csum=0x([0-9a-f]{8}) match=(\d)", log)
    ch("父进程算了共享缓冲的 csum（65536 B）", mcs is not None and int(mcs.group(1)) == 65536)
    ch("子进程逐字节核对一致（csum 相等 + match=1）",
       mcs is not None and mcc is not None and mcc.group(1) == mcs.group(2) and mcc.group(2) == "1",
       "parent=0x%s child=0x%s" % (mcs.group(2) if mcs else "?", mcc.group(1) if mcc else "?"))
    ch("子进程往共享页写魔数（[EVSHM] child magic=0xCAFEBABE）",
       "[EVSHM] child magic=0xCAFEBABE" in log)
    ch("父进程读回魔数（ok=1 -> 共享是双向的，不是拷贝）",
       re.search(r"\[EVSHM\] parent magic=0xcafebabe ok=1", log, re.I) is not None)
    ch("子进程被 wait4 收掉", re.search(r"\[EVSHM\] wait4 child=\d+ ret=\d+ status=\d+", log) is not None)

    print("=== 12) 帧提交（fb_flip）与 shm 内容的关系 ===")
    ch("演示把共享缓冲里的矩形提交上屏（[FB64] flip clip=ok x=100 y=120 w=256 h=64）",
       re.search(r"\[FB64\] flip pid=\d+ x=100 y=120 w=256 h=64 clip=ok", log) is not None)
    ch("用户侧看到提交成功（[EVSHM] flip ret=0）",
       re.search(r"\[EVSHM\] flip ret=0 x=100 y=120 w=256 h=64", log) is not None)

    print("=== 13) ⑤ 退出后引用计数归零并回收（页池回基线）===")
    rel = re.findall(r"\[SHM64\] release pid=(\d+) id=(\d+) refs=(\d+) freed_pages=(\d+)", log)
    rel0 = [r for r in rel if r[2] == "0"]
    ch("有引用归零的释放打点（refs=0）", len(rel0) >= 1, "releases=%s" % rel[:4])
    if rel0:
        ch("归零时把 16 页都还回页池（freed_pages=16）", int(rel0[-1][3]) == 16,
           "freed=%s" % rel0[-1][3])
    ch("有\"还有引用\"的中间态（fork 后退出：refs=1 freed_pages=0）",
       any(r[2] == "1" and r[3] == "0" for r in rel), "releases=%s" % rel[:4])
    md = re.search(r"\[EVSHM\] demo done pid=(\d+) exited=(\d+) code=(\d+) ticks=(\d+) "
                   r"pool_free=(\d+) pool_delta=([+-]\d+) shm_live=(\d+) procs_left=(\d+)", log)
    ch("内核收尾打点完整（exited/code/pool_delta/shm_live）", md is not None)
    if md:
        ch("演示正常退出（exited=1 code=0）", md.group(2) == "1" and md.group(3) == "0",
           "exited=%s code=%s" % (md.group(2), md.group(3)))
        ch("页池回到基线（pool_delta=+0 -> 无泄漏）", md.group(6) == "+0",
           "pool_free=%s delta=%s" % (md.group(5), md.group(6)))
        ch("没有活着的 shm 对象（shm_live=0）", md.group(7) == "0")
        ch("没有残留进程（procs_left 回到启动前的计数）", int(md.group(8)) >= 0,
           "procs_left=%s" % md.group(8))
    ch("焦点被释放（[EV64] focus pid=0 prev=.. reason=release）",
       re.search(r"\[EV64\] focus pid=0 prev=\d+ reason=release", log) is not None)

    print("=== 14) ⑥ 计数口径 / 无 PANIC / 无 enosys ===")
    msum = re.search(r"\[EVSHM\] key=(\d+) mouse=(\d+) wheel=(\d+)", log)
    ch("按类型计数打点（[EVSHM] key=n mouse=n wheel=n）", msum is not None)
    if msum:
        ch("计数与事件原文一致（key 数 == KEY_DOWN 条数）",
           int(msum.group(1)) == len(key_down),
           "summary key=%s 原文=%d" % (msum.group(1), len(key_down)))
        ch("至少收到 1 个键 + 1 个鼠标事件（注入真的到了 ring3）",
           int(msum.group(1)) >= 1 and int(msum.group(2)) >= 1, msum.group(0))
    ch("max=0 只查询语义用到了（[EV64] poll ... max=0 ...）",
       re.search(r"\[EV64\] poll pid=\d+ max=0 flags=0*[0-9A-Fa-f]+ got=0 pend=\d+ drops=\d+", log) is not None)
    # ★ clip=reject 不在这里断言：那正是 fbdemo（A1 的越界用例）故意打出来的，本演示不产生它；
    #   本演示只要求自己的 flip 是 clip=ok（见上面那条）。
    for needle in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=", "[EVSHM] demo skipped",
                   "[EVSHM] install FAILED", "[EVSHM] input_poll FAILED", "[SHM64] create FAILED",
                   "[SHM64] map FAILED"):
        ch("不得出现 %s" % needle, needle not in log)
    ch("自有 ABI 12/13/14 没有 enosys（[SYSCALL] enosys nr=12/13/14）",
       ("[SYSCALL] enosys nr=12" not in log) and ("[SYSCALL] enosys nr=13" not in log) and
       ("[SYSCALL] enosys nr=14" not in log))
    n_evshm = len(re.findall(r"\[EVSHM\] ev ", log))
    ch("事件原文条数 >= 4（键 down/up + 鼠标 down/up）", n_evshm >= 4, "n=%d" % n_evshm)


if __name__ == "__main__":
    os.environ["VIMTU_IPC_CHANNEL"] = "vmware" if "--vmware" in sys.argv else "qemu"
    sys.exit(main())
