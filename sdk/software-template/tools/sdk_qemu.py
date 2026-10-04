#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/tools/sdk_qemu.py - 模板自带的 QEMU 夹具与串口/截屏工具（**只给测试用**）

为什么模板里要有这个：
  本仓库的 App 验收一律是"**真引导 + 真串口 + 真像素**"（不是单元测试打桩）。
  这套工具把三件事收在一处：
    ① 造夹具盘：system.img 字节 + VimtuFS2 卷（把你的程序/图标/字库/配置写进卷）+ MBR（LBA 8009）；
    ② 起 QEMU（-serial file:… + telnet monitor）与"登录进桌面 + 开终端"的手势；
    ③ 打字（monitor sendkey）/ 截屏（screendump → PPM）。

★ 纪律（写验收脚本时照着做）：
  * **同一时刻只跑一个 QEMU 重活**：本工具不加锁，靠调用方自觉（本仓库的约定）；
  * 断言"不许削弱"：失败就非 0 退出，不做"大概齐"；
  * **别把 VM 挂着**：QemuVm.close() 一定要在 finally 里调。
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(SDK))
sys.path.insert(0, os.path.join(ROOT, "tests"))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(SDK, "packaging"))

SECTOR = 512
PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768                     # 16 MB：与仓库其它 GUI 夹具同一几何
SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
BASE_VOL = os.path.join(ROOT, "build64", "shellvol.img")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

TYPED_NAMES = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
               "_": "shift-minus", "=": "equal"}

PANIC_MARKERS = ("PANIC", "TRIPLE FAULT")


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


# --------------------------------------------------------------------------- 夹具盘
def build_fixture(out_disk, extra_srcs, verbose=True):
    """把 extra_srcs 写进一块 VimtuFS2 卷，再拼成可引导盘 build64 同款。

    extra_srcs: [(宿主文件, 卷内绝对路径, 八进制模式)]
    返回 (disk_path, {卷内路径: 字节数})
    """
    import importlib.util
    import struct

    def mod_from(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return m

    tp = mod_from("tcc_pack_win", os.path.join(ROOT, "tools", "tcc_pack_win.py"))
    sv = mod_from("make_shellvol", os.path.join(ROOT, "tools", "make_shellvol.py"))
    for p in (SYSTEM_IMG, BASE_VOL):
        if not os.path.exists(p):
            raise RuntimeError("缺少 %s（先跑 bash build64.sh）" % p)

    img = open(BASE_VOL, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        raise RuntimeError("%s 不是 VimtuFS2 卷" % BASE_VOL)
    total = struct.unpack_from("<I", img, 20)[0]
    vol = tp.VolumeEdit(total)
    vol.load(img)
    sizes = {}
    for host, vpath, mode in extra_srcs:
        if not os.path.isfile(host):
            raise RuntimeError("缺少 %s" % host)
        data = open(host, "rb").read()
        vol.put(vpath, data, mode=mode)
        sizes[vpath] = len(data)
    out = vol.finish()
    bad = tp.verify(out, {vp: open(h, "rb").read() for h, vp, _m in extra_srcs})
    if bad:
        raise RuntimeError("夹具卷自检失败：%s" % bad)
    disk = sv.build_disk(open(SYSTEM_IMG, "rb").read(), out, TARGET_SECTORS)
    with open(out_disk, "wb") as f:
        f.write(disk)
    if verbose:
        print("   夹具盘：%s（%d B = %d 扇区；卷内 %d 个文件）"
              % (out_disk, len(disk), TARGET_SECTORS, len(sizes)))
        for vp in sorted(sizes):
            print("     %-42s %8d B" % (vp, sizes[vp]))
    return out_disk, sizes


def icon_srcs(only=None, verbose=True, sizes=None):
    """用 tools/iconpack_ext.py 光栅化 gui_rs/assets/icons 下的 SVG，返回可装卷的 --src 列表。

    sizes: 只挑这些档（例如 (24, 48)）。★ 为什么要有这个参数：内核运行期只请求
    系统图标 @24 / 应用图标 @48（`kernel/icons64.cpp:410` 的 want），把 4 档全装进夹具
    会让启动期多做几百次 PNG 解码 —— 在 QEMU + 并发构建的机器上足以把内核看门狗
    （`[PANIC64] stop=WATCHDOG_TIMEOUT`）踩响。**全尺寸装卷**用 `iconpack_ext.py`（默认 4 档）。
    """
    ext = os.path.join(HERE, "iconpack_ext.py")
    out = os.path.join(ROOT, "build", "sdk", "icons_ext")
    args = [sys.executable, ext, "--no-vol"]
    if only:
        args += ["--only", only]
    r = subprocess.run(args, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError("光栅化失败：%s" % r.stderr.decode("utf-8", "replace")[:300])
    srcs = []
    for sub in ("system", "apps"):
        d = os.path.join(out, sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".png"):
                continue
            if sizes is not None:
                m = re.search(r"@(\d+)\.png$", fn)
                if not m or int(m.group(1)) not in sizes:
                    continue
            srcs.append((os.path.join(d, fn), "/icons/%s/%s" % (sub, fn), 0o644))
    if verbose:
        print("   外置图标：%d 个 PNG（-> /icons/<system|apps>/<名字>@<尺寸>.png；sizes=%s）"
              % (len(srcs), sizes if sizes else "全部"))
    return srcs


# --------------------------------------------------------------------------- QEMU
class QemuVm:
    def __init__(self, qemu, img, port, serial, name="vimtu-sdk"):
        self.serial = serial
        self.proc = subprocess.Popen([
            qemu, "-name", name,
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

    def wait_re(self, rx, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            m = re.search(rx, self.log())
            if m:
                return m
            if self.proc.poll() is not None:
                return None
            time.sleep(0.15)
        return None

    def wait(self, needle, timeout):
        return self.wait_re(re.escape(needle), timeout) is not None

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class Monitor:
    """最小 QEMU monitor 壳（telnet）：**每条命令开一个新连接**（与仓库 tests/qemuhelp.py 同款）。

    为什么不用一条长连接：QEMU 的 telnet monitor 是行协议、还会回显 READLINE 提示，
    长连接在"发命令 -> 读回显"之间容易卡住；新连接一次性发完就关最省事也最稳。
    """

    def __init__(self, port, timeout=8):
        self.port = port
        self.timeout = timeout

    def send(self, cmd, wait=0.35):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=self.timeout)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    # sendkey(name, wait)：给"打字"用（与仓库 tests/font64user_test.py 的 Monitor 同名同义）
    def sendkey(self, name, wait=0.12):
        return self.send("sendkey %s" % name, wait=wait)

    def key(self, name, wait=1.2):
        self.send("sendkey %s" % name, wait=wait)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n1 = os.path.getsize(ppm)
                time.sleep(0.12)
                if os.path.getsize(ppm) == n1:
                    return True
            time.sleep(0.05)
        return False


def login_desktop(mon, vm, timeout=240):
    """登录手势 —— **复用仓库的那一份**（tests/qemuhelp.py），别自己拍一套。

    为什么不自己发两次回车：登录界面必须**显式输入**才进桌面（`ui.login.auto` 默认 0），
    而手势的第一步是"等锁屏真正可交互"（串口出现 `[LOCK64] bg blur ready`）；不等就打回车会打空。
    """
    try:
        import qemuhelp as qh
        qh.login_desktop(mon, lambda: vm.log(), proc=vm.proc, timeout=timeout)
        return qh.wait_mark(lambda: vm.log(), qh.DESKTOP_READY, timeout, proc=vm.proc)
    except ImportError:
        # 兜底：没有 tests/qemuhelp.py 时用简化手势（等锁屏 -> 两次回车）
        t0 = time.time()
        while time.time() - t0 < timeout and "[LOCK64] bg blur ready" not in vm.log():
            time.sleep(0.4)
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=1.5)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if "[GUI64] ready" in vm.log():
                return True
            time.sleep(0.4)
        return False


def open_terminal(mon, vm, tries=3):
    """开始菜单（meta_l）-> 第一项（终端）。"""
    for _ in range(tries):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if vm.wait("[APP] term opened", 12):
            return True
    return False


def type_line(mon, text, per_key=0.11):
    for ch in text:
        if ch in TYPED_NAMES:
            mon.sendkey(TYPED_NAMES[ch], wait=per_key)
        elif ch.isalnum():
            mon.sendkey(ch, wait=per_key)
        else:
            raise ValueError("sendkey 不支持这个字符：%r" % ch)
    mon.sendkey("ret", wait=per_key + 0.3)


def read_ppm(path):
    """极简 PPM(P6) 读取 -> (w, h, bytes)。"""
    d = open(path, "rb").read()
    parts = []
    i = 0
    while len(parts) < 4:
        while i < len(d) and d[i:i + 1].isspace():
            i += 1
        if d[i:i + 1] == b"#":
            while i < len(d) and d[i:i + 1] != b"\n":
                i += 1
            continue
        j = i
        while j < len(d) and not d[j:j + 1].isspace():
            j += 1
        parts.append(d[i:j])
        i = j
    w, h = int(parts[1]), int(parts[2])
    px = d[i + 1:i + 1 + w * h * 3]
    return w, h, px


def count_color(px, w, h, want, tol=16):
    """统计画面里与 want=(r,g,b) 相差在 tol 内的像素数（给"上屏像素"证据用）。"""
    n = 0
    for k in range(0, min(len(px), w * h * 3), 3):
        if (abs(px[k] - want[0]) <= tol and abs(px[k + 1] - want[1]) <= tol
                and abs(px[k + 2] - want[2]) <= tol):
            n += 1
    return n


def temp_serial(prefix="vimtu_sdk_"):
    tmp = tempfile.mkdtemp(prefix=prefix)
    return tmp, os.path.join(tmp, "serial.log")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p
