#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/textvnc.py - P7a 复现工具：**VMware VNC 真屏（读客人真实像素）+ 真鼠标**。

为什么要有这份东西（上一批"没复现"的原因）：
  QEMU 路径只有 monitor 的 `sendkey`，**没有绝对鼠标**，注入速率也不够做"连续拖拽"
  （拖边框缩放/拖标题栏移动都是一串连续位移）。所以上一批只能看串口打点，判不出
  "窗口内容有没有跟着走"。
  VMware 的 VNC 服务器给三样东西：
    * FramebufferUpdate  -> **客人整屏真实像素**（判定口径用像素，不靠肉眼/不靠断言字串）；
    * PointerEvent       -> 绝对坐标鼠标（按下-移动-松开 = 真鼠标拖拽）；
    * KeyEvent           -> 敲键（登录手势两次回车 / Win 键 / Ctrl+Shift+Esc）。
  并且这份脚本**闭环**控鼠标：先用像素模板匹配把客人的光标找出来，再按误差迭代逼近，
  不依赖 VMware 的加速曲线（加速只会改变步长，不会让闭环发散）。

对外接口（两个 driver 脚本用）：
    VMSession(...)                 -> with 块里 start/登录/等桌面，退出时 **stop hard**
        s.serial_after(mark)       -> 只要 mark 之后的串口文本（避免读到上一轮残留）
        s.wait(mark, timeout)      -> 等串口出现标记
        s.dock_geom()/s.dock_item(i)/s.win_geom(title)
        s.cap()                    -> Frame（整屏像素）
        s.move_to(x, y)            -> 闭环把客人光标移到 (x,y)，返回 (ok, 轨迹)
        s.click(x, y) / s.drag(x0, y0, x1, y1, steps)
        s.key(keysym, down=True/False)
        s.save(frame.rgb, "a.png")

自检：python tests/textvnc.py --selftest   （起 VM -> 登录 -> 抓屏 -> 定位光标）
"""
import os
import re
import socket
import struct
import subprocess
import sys
import time

import numpy as np

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

VMRUN = r"C:\Program Files (x86)\VMware\VMware Workstation\vmrun.exe"
ROOT = r"C:\Users\fanqi\Desktop\VimtuOS\Vimtu64"
VMTEST = r"C:\Users\fanqi\Desktop\新建文件夹\v64-install-test"
VMX_BOOT = os.path.join(VMTEST, "vimtu64-installed-boot.vmx")
SERIAL_BOOT = os.path.join(VMTEST, "serial-installed-boot.log")
VNC_BOOT_PORT = 5904
SCRATCH = os.environ.get("PI_SCRATCH_DIR", os.path.join(ROOT, "build64"))
EVID = os.path.join(SCRATCH, "textrepro")

LOCK_INTERACTIVE = "[LOCK64] bg blur ready"
DESKTOP_READY = "[GUI64] ready"

KEYSYM = {
    "ret": 0xFF0D, "esc": 0xFF1B, "tab": 0xFF09, "space": 0x20,
    "super": 0xFFEB, "ctrl": 0xFFE3, "shift": 0xFFE1, "alt": 0xFFE9,
}


# ------------------------------------------------------------------ vmrun
def vmrun(*args, timeout=180):
    r = subprocess.run([VMRUN, "-T", "ws"] + list(args), capture_output=True, timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace")


def vm_running():
    _rc, out = vmrun("list")
    return [l.strip() for l in out.splitlines() if l.strip().lower().endswith(".vmx")]


def vm_stop_ours():
    """只停**本目录**的 VM（别去动另一条线的 VM）。"""
    for vmx in vm_running():
        if os.path.dirname(vmx).lower() == VMTEST.lower():
            vmrun("stop", vmx, "hard")
    for _ in range(40):
        if not [v for v in vm_running() if os.path.dirname(v).lower() == VMTEST.lower()]:
            return True
        time.sleep(1)
    return False


def read_text(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_mark(path, needle, timeout, proc=None, step=0.4, since=0):
    t0 = time.time()
    while True:
        txt = read_text(path)
        if needle in txt[since:]:
            return True
        if proc is not None and proc.poll() is not None:
            return False
        if time.time() - t0 > timeout:
            return False
        time.sleep(step)

def _same_vmx(a, b):
    return os.path.basename(a).lower() == os.path.basename(b).lower()


def vm_is_running(vmx):
    return any(_same_vmx(v, vmx) for v in vm_running())


def vm_start_async(vmx, mode="nogui"):
    """★ 关键：`vmrun start` 在本机**可能不返回**（它在等一个它等不到的确认）。
    所以一律 fire-and-forget（Popen + DEVNULL），然后用串口/`vmrun list` 判进度。"""
    return subprocess.Popen([VMRUN, "-T", "ws", "start", vmx, mode],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            stdin=subprocess.DEVNULL)


def vm_stop(vmx, timeout=90):
    """先 vmrun stop hard（同样 fire-and-forget），超时再按命令行精确杀 vmware-vmx。"""
    subprocess.Popen([VMRUN, "-T", "ws", "stop", vmx, "hard"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     stdin=subprocess.DEVNULL)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not vm_is_running(vmx):
            return True
        time.sleep(2)
    ps = ("Get-CimInstance Win32_Process -Filter \"Name='vmware-vmx.exe'\" | "
          "Where-Object { $_.CommandLine -like '*%s*' } | "
          "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % vmx.replace("'", "''"))
    subprocess.run(["powershell", "-NoProfile", "-Command", ps],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    return not vm_is_running(vmx)


def vm_stop_ours():
    """只停**本目录**的 VM（别去动另一条线的 VM）。"""
    for vmx in vm_running():
        if os.path.dirname(vmx).lower() == VMTEST.lower():
            vm_stop(vmx)
    return not [v for v in vm_running() if os.path.dirname(v).lower() == VMTEST.lower()]


# ------------------------------------------------------------------ 帧（**按原始字节**判定）
# ★ 为什么不在解码后的 RGB 上判定：
#   本机 VMware Workstation 的 VNC 服务器**一收到 SetPixelFormat 就断连**（实测 6 种格式全断，
#   证据见 $PI_SCRATCH_DIR/vnc_dbg2.log、vnc_calib.log），所以只能用它的默认 **16bpp** 帧；
#   而 16bpp 的通道顺序**没法用协议问出来**（SetPixelFormat 是唯一途径，一问就断）。于是：
#     * 所有**判定**（逐像素同/不同、uniform 占比、区域差异）都跑在**原始字节**上 —— 与通道顺序
#       无关，而且比解码后更严格（少一次量化）；
#     * `rgb` 只用于**人看的 PNG 证据**，映射用"灰阶一致性"自动挑，报告里如实标注是推断映射。
FMT_DESC = "16bpp 565（服务器默认；本机不能协商 32bpp）"


def decode_565(raw, swap=False):
    """(h,w,2) 原始字节 -> (h,w,3) 近似 RGB（565；swap=True 时 R/B 互换）。"""
    v = raw[:, :, 0].astype(np.uint16) | (raw[:, :, 1].astype(np.uint16) << 8)
    r = ((v >> 11) & 31).astype(np.uint16) * 255 // 31
    g = ((v >> 5) & 63).astype(np.uint16) * 255 // 63
    b = (v & 31).astype(np.uint16) * 255 // 31
    out = np.stack([r, g, b], 2).astype(np.uint8)
    return out[:, :, [2, 1, 0]] if swap else out


def gray_frac(rgb):
    """灰阶像素占比（|R-G| 与 |G-B| 都小）—— 用来挑"最像真实界面"的解码映射。"""
    d1 = np.abs(rgb[:, :, 0].astype(np.int16) - rgb[:, :, 1].astype(np.int16))
    d2 = np.abs(rgb[:, :, 1].astype(np.int16) - rgb[:, :, 2].astype(np.int16))
    return float(((d1 < 24) & (d2 < 24)).mean())


def raw_pixel_candidates(px_bytes):
    """一个原始像素（2/3/4 字节元组）-> 候选 RGB 解读列表（含 R/B 互换 / 565 解）。
    用来做"自我标定"：外壳客户区底色 theme64.cpp 的 client_bg = rgb(240,240,240)，
    哪条候选解读正好给出 (240,240,240)，就用哪条 —— 顺便证明"这块像素就是外壳底色"。"""
    cands = []
    if len(px_bytes) >= 3:
        r, g, b = px_bytes[0], px_bytes[1], px_bytes[2]
        cands += [(r, g, b), (b, g, r)]
    if len(px_bytes) == 2:
        v = px_bytes[0] | (px_bytes[1] << 8)
        r = ((v >> 11) & 31) * 255 // 31
        g = ((v >> 5) & 63) * 255 // 63
        b = (v & 31) * 255 // 31
        cands += [(r, g, b), (b, g, r)]
    return cands


def decode_raw(raw):
    """(h,w,px) 原始字节 -> (h,w,3) 近似 RGB + 一句格式描述（只给人看图用）。"""
    px = raw.shape[2]
    if px == 2:
        a = decode_565(raw, swap=False)
        b = decode_565(raw, swap=True)
        return (a if gray_frac(a) >= gray_frac(b) else b), "16bpp565(%s)" % \
            ("RGB" if gray_frac(a) >= gray_frac(b) else "BGR")
    # 3/4 字节：默认小端 BGR(A)（VMware/SVGA 的常见默认）；用灰阶一致性在 BGR/RGB 间挑
    a = raw[:, :, [2, 1, 0]]
    b = raw[:, :, [0, 1, 2]]
    if gray_frac(a) >= gray_frac(b):
        return a, "%dbpp(BGR%s)" % (px * 8, "X" if px == 4 else "")
    return b, "%dbpp(RGB%s)" % (px * 8, "X" if px == 4 else "")


class Frame:
    """一帧：`raw` = 原始字节（判定用），`rgb` = 近似解码（只看图用）。"""
    __slots__ = ("w", "h", "raw", "rgb", "fmt")

    def __init__(self, raw):
        self.h, self.w = raw.shape[0], raw.shape[1]
        self.raw = raw
        self.rgb, self.fmt = decode_raw(raw)

    def px(self, x, y):
        return tuple(int(v) for v in self.rgb[y, x])

    def raw_region(self, x, y, w, h):
        return self.raw[max(0, y):y + h, max(0, x):x + w].astype(np.int16)

    def region(self, x, y, w, h):
        return self.rgb[max(0, y):y + h, max(0, x):x + w].astype(np.int16)

    def mean_abs_diff(self, other, x, y, w, h, ox=0, oy=0):
        """平均绝对差：**在原始字节上**算（逐字节比较，与像素格式/通道顺序无关）。"""
        a = self.raw_region(x, y, w, h)
        b = other.raw_region(x + ox, y + oy, w, h)
        n = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        a, b = a[:n[0], :n[1]], b[:n[0], :n[1]]
        if a.size == 0:
            return 999.0
        return float(np.abs(a - b).mean())

    def identical_frac(self, other, x, y, w, h, ox=0, oy=0, tol=0):
        """与另一帧在 (x+ox,y+oy) 处**逐像素完全相同**（原始字节差 <= tol）的占比。"""
        a = self.raw_region(x, y, w, h)
        b = other.raw_region(x + ox, y + oy, w, h)
        n = min(a.shape[0], b.shape[0]), min(a.shape[1], b.shape[1])
        a, b = a[:n[0], :n[1]], b[:n[0], :n[1]]
        if a.size == 0:
            return -1.0
        return float((np.abs(a - b).max(axis=2) <= tol).mean())

    def dominant_raw(self, x, y, w, h):
        """区域里出现最多的**原始像素**（字节元组）及其占比（判定"一片纯色"用）。"""
        a = self.raw[max(0, y):y + h, max(0, x):x + w]
        px = a.shape[2]
        flat = a.reshape(-1, px)
        if flat.shape[0] == 0:
            return (), 0.0
        v = flat[:, 0].astype(np.uint32)
        for i in range(1, px):
            v = v | (flat[:, i].astype(np.uint32) << (8 * i))
        vals, cnts = np.unique(v, return_counts=True)
        i = int(np.argmax(cnts))
        big = int(vals[i])
        return tuple((big >> (8 * k)) & 0xFF for k in range(px)), float(cnts[i]) / float(v.size)


class TextVNC:
    """RFB 3.3/3.8 最小客户端：只请求 Raw 编码（不依赖 hextile 实现）。"""

    def __init__(self, port=VNC_BOOT_PORT, host="127.0.0.1", timeout=15):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        ver = self._recv(12).split(b"\n")[0].strip()
        if not ver.startswith(b"RFB "):
            raise OSError("不是 RFB 服务器：%r" % ver)
        self.sock.sendall(ver + b"\n")
        major, minor = int(ver[4:7]), int(ver[8:11])
        if (major, minor) >= (3, 7):
            n = self._recv(1)[0]
            types = self._recv(n)
            if n == 0:                       # n=0 => 后面跟失败原因（通常是"已有客户端"）
                ln = struct.unpack(">I", self._recv(4))[0]
                why = self._recv(ln).decode("utf-8", "replace") if 0 < ln < 4096 else ""
                raise OSError("VNC 服务器拒绝：%s" % why)
            if 1 not in types:
                raise OSError("VNC 服务器不支持无认证（types=%r）" % types)
            self.sock.sendall(bytes([1]))    # ★ 只发 1 字节安全类型（多发会把 ClientInit 顶错位 -> 服务器断连）
            if (major, minor) >= (3, 8):
                if self._recv(4) != b"\x00\x00\x00\x00":
                    raise OSError("VNC 认证失败")
        else:
            self._recv(4)                        # RFB 3.3：服务器直接给 4 字节安全类型（None=0）
        self.sock.sendall(b"\x01")                  # ClientInit(shared)
        si = self._recv(24)
        self.w, self.h = struct.unpack(">HH", si[0:4])
        # ★ ServerInit 的 24 字节里**最后 4 字节就是 desktop-name 的长度**，后面紧跟名字本体；
        #   名字必须读掉，否则它会顶在流里，后面每条消息都错位（实测：未知消息类型 = 名字里的字符）。
        nl = struct.unpack(">I", si[20:24])[0]
        if 0 < nl < 4096:
            self._recv(nl)
        # ★ 像素格式/编码：**两个都不发**，原因都是实测出来的（本机 VMware Workstation 2026-10-03）：
        #   * SetPixelFormat —— 服务器收到就**直接断连**（试过 6 种格式：32/24、24/24、大端、
        #     16/16 565、8/8 索引、R/B 互换，全部断；日志见 $PI_SCRATCH_DIR/vnc_dbg2.log）；
        #   * SetEncodings   —— 只列 1 种（Raw）也会断；列 4 种能活但服务器可能改用 hextile。
        #   => 保持沉默：服务器默认就是 Raw(0)，像素格式是它自己的默认。
        #      默认到底是几 bpp 不靠猜：下面用一个"整屏探测"把每像素字节数量出来，
        #      同时**把这一帧存进缓冲区**（服务器是"只在变化时发"的，第一帧尤其宝贵）。
        self.ptr_x, self.ptr_y = -1, -1
        self.bpp = 0
        self.buf = None
        self._first_frame(timeout=20.0)

    def _read_until_silence(self, silence=1.0, max_wait=25.0):
        """读到"静默"为止（Raw 长度未知时用它判边界）。"""
        old = self.sock.gettimeout()
        self.sock.settimeout(silence)
        data = bytearray()
        t0 = time.time()
        try:
            while time.time() - t0 < max_wait:
                try:
                    ch = self.sock.recv(262144)
                except socket.timeout:
                    break
                if not ch:
                    break
                data += ch
                t0 = time.time()
        finally:
            self.sock.settimeout(old)
        return bytes(data)

    def _parse_blob(self, blob, nrects, px):
        """按已知的每像素字节数把一段 Raw 数据放进缓冲区；返回用掉的字节数。"""
        off = 0
        for _ in range(nrects):
            x, y, w, h = struct.unpack_from(">HHHH", blob, off)
            off += 8
            enc = struct.unpack_from(">i", blob, off)[0]
            off += 4
            if enc != 0:
                raise OSError("探测/解析时遇到非 Raw 编码 %d" % enc)
            n = w * h * px
            if off + n > len(blob):
                raise OSError("Raw 数据不够（rect %dx%d 需要 %d 字节，只剩 %d）"
                              % (w, h, n, len(blob) - off))
            arr = np.frombuffer(blob[off:off + n], dtype=np.uint8).reshape(h, w, px)
            off += n
            self.buf[y:y + h, x:x + w] = arr
        return off

    def _first_frame(self, timeout=20.0):
        """请求整屏 -> 整段读下来 -> 反推每像素字节数 -> 存进缓冲区。"""
        self._req(0, 0, self.w, self.h, incremental=0)
        old = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            t = self._recv(1)[0]
            if t != 0:
                raise OSError("首帧不是 FramebufferUpdate（type=%d）" % t)
            self._recv(1)
            nrects = struct.unpack(">H", self._recv(2))[0]
        finally:
            self.sock.settimeout(old)
        if nrects < 1:
            raise OSError("首帧 nrects=0")
        blob = self._read_until_silence()
        for px in (4, 3, 2):
            self.bpp = px * 8
            self.buf = np.zeros((self.h, self.w, px), dtype=np.uint8)
            try:
                used = self._parse_blob(blob, nrects, px)
            except OSError:
                continue
            if used <= len(blob):                     # 解析得通：就是它
                self.fmt_desc = "%dbpp（服务器默认，探测得出）" % self.bpp
                return
        raise OSError("反推每像素字节数失败（%d 字节 / %d rect / %dx%d）"
                      % (len(blob), nrects, self.w, self.h))

    # ---------------- 低层 ----------------
    def _recv(self, n):
        out = b""
        while len(out) < n:
            ch = self.sock.recv(n - len(out))
            if not ch:
                raise OSError("VNC 连接被关闭（%d/%d 字节）" % (len(out), n))
            out += ch
        return out

    def _req(self, x, y, w, h, incremental=1):
        self.sock.sendall(struct.pack(">BBHHHH", 3, incremental, x, y, w, h))

    def _msg(self, cover):
        """读一条服务器消息；cover 是 (h,w) bool 掩码，Raw/CopyRect 落进去。返回覆盖像素数。"""
        t = self._recv(1)[0]
        if t == 0:
            self._recv(1)
            nrects = struct.unpack(">H", self._recv(2))[0]
            got = 0
            for _ in range(nrects):
                x, y, w, h = struct.unpack(">HHHH", self._recv(8))
                enc = struct.unpack(">i", self._recv(4))[0]
                if enc == 0:
                    px = self.bpp // 8
                    data = np.frombuffer(self._recv(w * h * px), dtype=np.uint8).reshape(h, w, px)
                    self.buf[y:y + h, x:x + w] = data
                elif enc == 1:
                    sx, sy = struct.unpack(">HH", self._recv(4))
                    self.buf[y:y + h, x:x + w] = self.buf[sy:sy + h, sx:sx + w].copy()
                else:
                    raise OSError("服务器用了没实现的编码 %d（只请求了 Raw）" % enc)
                if cover is not None:
                    cover[y:y + h, x:x + w] = True
                    got += w * h
            return got
        if t == 1:
            _pad, _first, n = struct.unpack(">BHH", self._recv(5))
            self._recv(n * 6)
            return 0
        if t == 2:
            return 0
        if t == 3:
            _pad, n = struct.unpack(">BI", self._recv(7))
            self._recv(n)
            return 0
        raise OSError("未知 RFB 消息类型 %d" % t)

    def cap(self, timeout=0.7):
        """返回"当前真屏"。模型：**缓冲区是累积的** —— __init__ 里已经拿过一整帧，
        之后只要把服务器推来的更新落进缓冲区即可。

        而 VMware 的服务器**屏幕没变就一个字节都不发**，所以：
          * 要一次增量更新（incremental=1）把"刚发生的动作"（真鼠标移动 -> 客人重画光标）
            带来的变化收进来；
          * 若 timeout 内什么都没来 = 屏幕确实没变 => **当前缓冲区就是真屏**，直接返回。
        （早期版本每次都要整屏 + 收不满重试 4 次 = 每次 30s 起，拖拽测试直接跑不完。）"""
        self._req(0, 0, self.w, self.h, incremental=1)
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self._msg(None)
            except socket.timeout:
                break
            except TimeoutError:
                break
        return Frame(self.buf.copy())

    def full_refresh(self, timeout=20.0):
        """要一次**整屏**更新并落进缓冲区（画面刚发生大变化时用；静态屏上可能什么也不回）。"""
        self._req(0, 0, self.w, self.h, incremental=0)
        t0 = time.time()
        while time.time() - t0 < timeout:
            try:
                self._msg(None)
            except socket.timeout:
                break
            except TimeoutError:
                break
        return Frame(self.buf.copy())

    # ---------------- 输入 ----------------
    def pointer(self, x, y, mask=0):
        x = max(0, min(self.w - 1, int(x)))
        y = max(0, min(self.h - 1, int(y)))
        self.ptr_x, self.ptr_y = x, y

    def key(self, keysym, down=True):
        if isinstance(keysym, str):
            keysym = KEYSYM.get(keysym, ord(keysym[0]))
        # KeyEvent = type(1)+down(1)+padding(2)+keysym(4) = 8 字节（漏掉 2 字节 padding 会让
        # 服务器协议错位并直接断连 —— 这是本项目踩过的坑，别"优化"掉）。
        self.sock.sendall(struct.pack(">BBHI", 4, 1 if down else 0, 0, int(keysym)))

    def tap(self, keysym, hold=0.08, gap=0.35):
        self.key(keysym, True)
        time.sleep(hold)
        self.key(keysym, False)
        time.sleep(gap)

    def combo(self, names, gap=0.08):
        for n in names:
            self.key(n, True)
            time.sleep(gap)
        time.sleep(0.1)
        for n in reversed(names):
            self.key(n, False)
            time.sleep(gap)

    # ---------------- 光标闭环 ----------------
    def find_cursor(self, frame=None, hint=None):
        """用 gui64 draw_cursor 的箭头形状（19 行、黑底白描边）做模板匹配，返回左上角坐标。

        检查前 5 行的固定图案（够独特；箭头在客人里是**软件绘制**的，一定在 VNC 帧里）：
            row0 'X' / row1 'XX' / row2 'X.X' / row3 'X..X' / row4 'X...X'
        """
        f = frame if frame is not None else self.cap()
        a = f.rgb.astype(np.int16)
        black = (a.max(axis=2) < 60)
        white = (a.min(axis=2) > 170)
        h, w = black.shape
        if h < 24 or w < 24:
            return None
        # 所有条件都用同一形状的窗口 (h-5, w-6)，锚点为候选光标左上角
        B, Wh = black, white
        cand = (B[0:h - 5, 0:w - 6] & B[1:h - 4, 0:w - 6] & B[1:h - 4, 1:w - 5]
                & B[2:h - 3, 0:w - 6] & B[2:h - 3, 2:w - 4] & Wh[2:h - 3, 1:w - 5]
                & B[3:h - 2, 0:w - 6] & B[3:h - 2, 3:w - 3]
                & B[4:h - 1, 0:w - 6] & B[4:h - 1, 4:w - 2])
        ys, xs = np.nonzero(cand)
        if len(ys) == 0 and hint is not None:
            # ★ 兜底：光标压到深色/花哨背景（Dock 图标、窗口标题栏）时，前 5 行的严格图案
            #   可能有一条不成立。退一步只在 hint 附近 ±140px 里找"前 3 行"的弱图案（够定位）。
            x0, y0 = max(0, hint[0] - 140), max(0, hint[1] - 140)
            x1, y1 = min(w - 3, hint[0] + 140), min(h - 3, hint[1] + 140)
            sub = B[y0:y1, x0:x1]
            if sub.shape[0] > 6 and sub.shape[1] > 6:
                hh, ww = sub.shape
                weak = (sub[0:hh - 3, 0:ww - 3] & sub[1:hh - 2, 0:ww - 3]
                        & sub[1:hh - 2, 1:ww - 2] & sub[2:hh - 1, 0:ww - 3]
                        & sub[2:hh - 1, 2:ww - 1])
                wy, wx = np.nonzero(weak)
                if len(wy):
                    d = (wx + x0 - hint[0]) ** 2 + (wy + y0 - hint[1]) ** 2
                    k = int(np.argmin(d))
                    return (int(wx[k] + x0), int(wy[k] + y0))
            return None
        if len(ys) == 0:
            return None
        if hint is not None and len(ys) > 1:
            d = (xs - hint[0]) ** 2 + (ys - hint[1]) ** 2
            order = np.argsort(d)
            ys, xs = ys[order], xs[order]
        best = None
        for i in range(min(len(ys), 32)):
            x, y = int(xs[i]), int(ys[i])
            if not (y + 19 < h and x + 10 < w):
                continue                      # 箭头是 19 行 11 列：越界的候选不算
            score = 9
            # 第 10 行 'X.....XXXXX'：箭头横档（再加一条强判据，把"恰好前 5 行长一样"的误配滤掉）
            if B[y + 10, x + 6] and B[y + 10, x + 7] and B[y + 10, x + 9] and B[y + 10, x + 10]:
                score += 4
            # 第 18 行 'X........X '：箭杆底端
            if B[y + 18, x]:
                score += 1
            if best is None or score > best[2]:
                best = (x, y, score)
            if best[2] >= 14:
                break
        return best[:2] if best else None

    def cursor_pos(self, hint=None):
        return self.find_cursor(hint=hint)


    def type_text(self, text, gap=0.06):
        """按 ASCII 文本敲键（VNC KeyEvent；终端里输入命令用）。回车用 '\\r' -> 0xFF0D。"""
        for ch in text:
            if ch in ("\r", "\n"):
                ks = KEYSYM["ret"]
            elif ch == " ":
                ks = 0x20
            elif ch == "-":
                ks = 0x2D
            elif ch == ".":
                ks = 0x2E
            elif ch == "/":
                ks = 0x2F
            elif ch == "_":
                ks = 0x5F
            else:
                ks = ord(ch)
            self.tap(ks, hold=0.03, gap=gap)
    def move_to(self, x, y, tol=2, max_iter=10, verbose=True):
        """闭环把客人光标挪到 (x,y)。返回 (ok, 轨迹)。加速曲线只影响步长，不影响收敛。"""
        traj = []
        cur = None
        for i in range(max_iter):
            f = self.cap()
            pos = self.find_cursor(f, hint=cur)
            if pos is None:
                return False, traj + [("nocursor", None, None)]
            cur = pos
            dx, dy = x - pos[0], y - pos[1]
            traj.append((i, pos, (dx, dy)))
            if abs(dx) <= tol and abs(dy) <= tol:
                return True, traj
            fx = 0.65 if abs(dx) > 40 else 1.0
            fy = 0.65 if abs(dy) > 40 else 1.0
            sx = max(-160, min(160, int(dx * fx)))
            sy = max(-160, min(160, int(dy * fy)))
            self.pointer(pos[0] + sx, pos[1] + sy, 0)
            time.sleep(0.05)
        return False, traj

    def click(self, x, y, settle=0.6, tol=3):
        ok, tr = self.move_to(x, y, tol=tol)
        if not ok:
            return False, tr
        self.pointer(x, y, 1)
        time.sleep(0.12)
        self.pointer(x, y, 0)
        time.sleep(settle)
        return True, tr

    def drag(self, x0, y0, x1, y1, steps=14, settle=0.8, tol=3):
        """真鼠标拖拽：先闭环到起点 -> 按下 -> 分步移到终点 -> 松开。"""
        ok, tr = self.move_to(x0, y0, tol=tol)
        if not ok:
            return False, tr
        # 起点稳定后精确重发一次（避免 move_to 最后一次的落点偏差）
        self.pointer(x0, y0, 0)
        time.sleep(0.15)
        self.pointer(x0, y0, 1)
        time.sleep(0.15)
        for i in range(1, steps + 1):
            px = x0 + (x1 - x0) * i // steps
            py = y0 + (y1 - y0) * i // steps
            self.pointer(px, py, 1)
            time.sleep(0.03)
        self.pointer(x1, y1, 1)
        time.sleep(0.25)                 # 让外壳把这帧画完再松手
        self.pointer(x1, y1, 0)
        time.sleep(settle)
        return True, tr

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


def save_png(rgb, name):
    os.makedirs(EVID, exist_ok=True)
    path = os.path.join(EVID, name)
    try:
        from PIL import Image
        Image.fromarray(rgb).save(path)
    except Exception:
        path = path[:-4] + ".ppm"
        with open(path, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (rgb.shape[1], rgb.shape[0]))
            f.write(rgb.tobytes())
    return path




# ------------------------------------------------------------------ 真鼠标（宿主注入）
import ctypes                                                   # noqa: E402
import ctypes.wintypes as _wt                                   # noqa: E402

_user32 = ctypes.windll.user32


def find_window(part):
    """按标题找可见窗口（VMware Workstation 的控制台窗口）。"""
    res = []

    @ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
    def cb(hwnd, lparam):
        n = ctypes.create_unicode_buffer(512)
        _user32.GetWindowTextW(hwnd, n, 512)
        if part.lower() in n.value.lower() and _user32.IsWindowVisible(hwnd):
            res.append((hwnd, n.value))
        return True

    _user32.EnumWindows(cb, 0)
    return res[0] if res else (None, None)


class HostMouse:
    """★ 真鼠标：VMware 的 VNC 服务器**不转发 PointerEvent**（实测：客人光标一点不动，
    见 $PI_SCRATCH_DIR/mouse_path.log），所以点击/拖拽一律走**宿主真鼠标**：
      1) 找到 VMware 窗口、置前、在窗口中心点一下 —— 这一步让 VMware 抓鼠标（客人 PS/2 相对模式）；
      2) 之后用 `mouse_event(MOUSEEVENTF_MOVE, dx, dy)` 做**相对**位移；
      3) 用 VNC 抓帧 + 光标像素模板做**闭环**收敛（VMware 有鼠标加速，实测本机约 1.67 倍，
         所以步长按 0.55 折算，然后按误差迭代 —— 不靠"猜加速曲线"）。"""

    def __init__(self, title_part="VMware Workstation", k=0.55, verbose=False):
        self.title_part, self.k, self.verbose = title_part, k, verbose
        self.title_part, self.k, self.verbose = title_part, k, verbose
        self.k_est = 0.5          # 客人/宿主位移比的自适应估计（跨调用保留）
        self.grabbed = False
        self.center = (0, 0)
        self.traj_log = []

    def grab(self, tries=3):
        for _ in range(tries):
            hwnd, _title = find_window(self.title_part)
            if not hwnd:
                time.sleep(1.0)
                continue
            self.hwnd = hwnd
            _user32.SetForegroundWindow(hwnd)
            time.sleep(0.5)
            r = _wt.RECT()
            _user32.GetWindowRect(hwnd, ctypes.byref(r))
            cx, cy = (r.left + r.right) // 2, (r.top + r.bottom) // 2
            self.center = (cx, cy)
            _user32.SetCursorPos(cx, cy)
            time.sleep(0.3)
            _user32.mouse_event(0x0002, 0, 0, 0, 0)     # 左键按下
            time.sleep(0.08)
            _user32.mouse_event(0x0004, 0, 0, 0, 0)     # 抬起（= 客人收到一次点击 + 抓鼠标）
            time.sleep(0.5)
            self.grabbed = True
            return True
        return False

    def rel(self, dx, dy, gap=0.012):
        _user32.mouse_event(0x0001, int(dx), int(dy), 0, 0)
        if gap:
            time.sleep(gap)

    def down(self):
        _user32.mouse_event(0x0002, 0, 0, 0, 0)

    def up(self):
        _user32.mouse_event(0x0004, 0, 0, 0, 0)

    def move_to(self, vnc, x, y, tol=2, max_iter=14, budget=20.0):
        """闭环把客人光标挪到 (x,y)（客人坐标）。返回 (ok, 轨迹)。
        有**总时间预算**：cap() 要 ms 级，一旦哪里卡住也不会把整个测试拖死。"""
        traj = []
        pos = vnc.cursor_pos()
        if pos is None:
            return False, [("nocursor", None, None)]
        t_end = time.time() + budget
        # ★ 客人/宿主位移比**自适应**：实测本机 VMware 的加速曲线不是线性的
        #   （单事件 -120 宿主 px 只走 24 客人 px；小步长 (6,3) 却放大到 (10,5)）。
        #   所以每轮按"上一轮实测比例"定步长，并用 EMA 修，不去猜曲线。
        ratio = max(0.05, min(4.0, self.k_est))
        for i in range(max_iter):
            dx, dy = x - pos[0], y - pos[1]
            traj.append((i, pos, (dx, dy), round(ratio, 3)))
            if abs(dx) <= tol and abs(dy) <= tol:
                self.traj_log += traj
                return True, traj
            if time.time() > t_end:
                self.traj_log += traj
                traj.append(("budget", pos, (dx, dy), round(ratio, 3)))
                return False, traj
            # 一轮发 n 个**小**相对步（每步 ≤20 宿主 px：大步会被单事件上限削掉）
            n = max(1, min(12, int(max(abs(dx), abs(dy)) / 12)))
            sx = max(-20, min(20, int(round(dx / ratio / n))))
            sy = max(-20, min(20, int(round(dy / ratio / n))))
            for _ in range(n):
                self.rel(sx, sy, gap=0.012)
            time.sleep(0.10)
            npos = vnc.cursor_pos(hint=pos)
            if npos is None:
                return False, traj + [("nocursor", None, None)]
            hx, hy = sx * n, sy * n
            gxs, gys = npos[0] - pos[0], npos[1] - pos[1]
            if abs(hx) > 4:
                ratio = 0.5 * ratio + 0.5 * (abs(gxs) / float(abs(hx)))
            elif abs(hy) > 4:
                ratio = 0.5 * ratio + 0.5 * (abs(gys) / float(abs(hy)))
            ratio = max(0.05, min(4.0, ratio))
            self.k_est = ratio               # 跨调用保留（下一个目标点直接从这儿起步）
            pos = npos
        self.traj_log += traj
        return False, traj

    def click_at(self, vnc, x, y, settle=0.6, tol=3):
        ok, tr = self.move_to(vnc, x, y, tol=tol)
        if not ok:
            return False, tr
        self.rel(x - vnc.cursor_pos()[0] if vnc.cursor_pos() else 0, 0, gap=0)
        self.down()
        time.sleep(0.12)
        self.up()
        time.sleep(settle)
        return True, tr

    def drag(self, vnc, x0, y0, x1, y1, steps=16, settle=0.9, tol=3):
        """真鼠标拖拽：闭环到起点 -> 按下 -> 分步相对推到终点（每步闭环纠偏）-> 松开。"""
        ok, tr = self.move_to(vnc, x0, y0, tol=tol)
        if not ok:
            return False, tr
        self.down()
        time.sleep(0.15)
        for i in range(1, steps + 1):
            tx = x0 + (x1 - x0) * i // steps
            ty = y0 + (y1 - y0) * i // steps
            pos = vnc.cursor_pos()
            if pos is None:
                self.up()
                return False, tr + [("nocursor-mid-drag", None, None)]
            dx, dy = tx - pos[0], ty - pos[1]
            self.rel(int(dx * self.k), int(dy * self.k))
            time.sleep(0.02)
        # 最后再闭环到终点（拖动时窗口跟着走，末端容易差几像素）
        self.down()
        for _ in range(6):
            pos = vnc.cursor_pos()
            if pos is None:
                break
            dx, dy = x1 - pos[0], y1 - pos[1]
            if abs(dx) <= tol and abs(dy) <= tol:
                break
            self.rel(int(dx * self.k), int(dy * self.k))
            time.sleep(0.02)
        time.sleep(0.25)
        self.up()
        time.sleep(settle)
        return True, tr
# ------------------------------------------------------------------ 会话
class VMSession:
    """起来 -> 等锁屏 -> VNC 登录 -> 等桌面；退出一定 `vmrun stop hard`。"""

    def __init__(self, vmx=VMX_BOOT, serial=SERIAL_BOOT, port=VNC_BOOT_PORT,
                 fresh_serial=True, boot_timeout=240, keep=False, mode="gui"):
        self.vmx, self.serial, self.port = vmx, serial, port
        self.fresh_serial, self.boot_timeout, self.keep, self.mode = \
            fresh_serial, boot_timeout, keep, mode
        self.vnc = None
        self.mouse = None
        self.t0 = 0
        self.boot_secs = 0.0

    def __enter__(self):
        vm_stop_ours()
        if self.fresh_serial and os.path.exists(self.serial):
            for _ in range(10):
                try:
                    os.remove(self.serial)
                    break
                except OSError:
                    time.sleep(1)
        self.t0 = time.time()
        vm_start_async(self.vmx, self.mode)          # ★ 不等它返回（见 vm_start_async 注释）
        if not wait_mark(self.serial, LOCK_INTERACTIVE, self.boot_timeout):
            raise OSError("等不到锁屏（%s）：串口尾部=%r"
                          % (LOCK_INTERACTIVE, read_text(self.serial)[-400:]))
        self.boot_secs = time.time() - self.t0
        self.vnc = TextVNC(self.port)
        # ★ 登录手势：锁屏 -> 回车进登录界面 -> 再回车按登录按钮（ui.login.auto 默认 0）
        self.vnc.tap("ret", gap=1.2)
        self.vnc.tap("ret", gap=1.2)
        if not wait_mark(self.serial, DESKTOP_READY, 180):
            raise OSError("登录后等不到桌面：%r" % read_text(self.serial)[-400:])
        self.t_desktop = time.time() - self.t0
        time.sleep(2.0)                              # 首帧动画走完
        self.grab_mouse()                            # ★ 抓真鼠标（之后 click/drag 都走它）
        return self

    def __exit__(self, *exc):
        try:
            if self.vnc:
                self.vnc.close()
        finally:
            if not self.keep:
                vm_stop(self.vmx)
                vm_stop_ours()
        return False

    # ---- 真鼠标（宿主注入；VNC PointerEvent 在本机 VMware 上不生效，见 HostMouse 注释）----
    def grab_mouse(self):
        """聚焦 VMware 窗口 + 点一下抓鼠标；登录完（桌面就绪）后调一次。"""
        if self.mouse is None:
            self.mouse = HostMouse()
        return self.mouse.grab()

    def click(self, x, y, settle=0.6, tol=3):
        return self.mouse.click_at(self.vnc, x, y, settle=settle, tol=tol)

    def drag(self, x0, y0, x1, y1, steps=16, settle=0.9, tol=3):
        return self.mouse.drag(self.vnc, x0, y0, x1, y1, steps=steps, settle=settle, tol=tol)

    def move_to(self, x, y, tol=2, max_iter=14):
        return self.mouse.move_to(self.vnc, x, y, tol=tol, max_iter=max_iter)

    # ---- 串口 ----
    def log(self):
        return read_text(self.serial)

    def log_since(self, mark):
        txt = read_text(self.serial)
        i = txt.find(mark)
        return txt[i:] if i >= 0 else ""

    def wait(self, needle, timeout=30, since=0):
        return wait_mark(self.serial, needle, timeout, since=since)

    def lines(self, pattern, since=0):
        txt = read_text(self.serial)[since:]
        return [l for l in txt.splitlines() if re.search(pattern, l)]

    # ---- 几何（都从串口真打点解析，不用猜）----
    def dock_geom(self):
        m = re.search(r"\[DOCK64\] geom x=(\d+) y=(\d+) w=(\d+) h=(\d+) r=(\d+) icon=(\d+) gap=(\d+)",
                      self.log())
        if not m:
            return None
        k = [int(v) for v in m.groups()]
        return {"x": k[0], "y": k[1], "w": k[2], "h": k[3], "r": k[4], "icon": k[5], "gap": k[6]}

    def dock_item(self, idx):
        m = re.search(r"\[DOCK64\] item idx=%d app=(\d+) name=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) "
                      r"cx=(\d+) cy=(\d+)" % idx, self.log())
        if not m:
            return None
        g = m.groups()
        return {"idx": idx, "app": int(g[0]), "name": g[1], "x": int(g[2]), "y": int(g[3]),
                "w": int(g[4]), "h": int(g[5]), "cx": int(g[6]), "cy": int(g[7])}

    def win_geom(self, title_part, tag=None):
        """最近一条 [UI] win geom 打点（tag=new/set/fit）。"""
        pat = r"\[UI\] win geom tag=(\S+) title=(\S+) app=(\d+) x=(-?\d+) y=(-?\d+) " \
              r"w=(\d+) h=(\d+) client=(\d+)x(\d+)"
        last = None
        for m in re.finditer(pat, self.log()):
            if title_part.lower() not in m.group(2).lower():
                continue
            if tag and m.group(1) != tag:
                continue
            g = m.groups()
            last = {"tag": g[0], "title": g[1], "app": int(g[2]), "x": int(g[3]), "y": int(g[4]),
                    "w": int(g[5]), "h": int(g[6]), "client_w": int(g[7]), "client_h": int(g[8])}
        return last

    def win_client_box(self, title_part):
        w = self.win_geom(title_part)
        if not w:
            return None
        # DECO_H = 24+1 = 25（标题栏 24 + 上边框 1）、DECO_W = 2（左右各 1）
        return {"x": w["x"] + 1, "y": w["y"] + 25, "w": w["client_w"], "h": w["client_h"],
                "win": w}

    def power_menu(self):
        m = re.search(r"\[START64\] power menu open x=(\d+) y=(\d+) w=(\d+) h=(\d+) rows=3", self.log())
        if not m:
            return None
        k = [int(v) for v in m.groups()]
        return {"x": k[0], "y": k[1], "w": k[2], "h": k[3]}

    # ---- 抓帧 / 动作（driver 直接复用）----
    def mark(self):
        return len(self.log())

    def cap(self, name=None):
        f = self.vnc.cap()
        if name:
            save_png(f.rgb, name)
        return f

    def open_dock_app(self, idx, title_part, timeout=25, since=None):
        """点 Dock 第 idx 项开应用（**真鼠标**），等它的 [UI] win geom 出现。返回窗口几何 dict。"""
        it = self.dock_item(idx)
        if not it:
            return None
        before = len(re.findall(r"\[UI\] win geom", self.log()))
        ok, tr = self.click(it["cx"], it["cy"])
        if not ok:
            return None
        t0 = time.time()
        while time.time() - t0 < timeout:
            if len(re.findall(r"\[UI\] win geom", self.log())) > before:
                g = self.win_geom(title_part)
                if g:
                    return g
            time.sleep(0.4)
        return None

    def close_all_windows(self, tries=3):
        """Alt-F4 之类没实现：用 Dock 右键也没有 —— 这里靠 gui64 的 close 热键没有，
        改用「点标题栏最右的关闭按钮」不可靠。驱动器改为不关窗（每次开窗都会激活）。"""
        return False


def close_button(win):
    """标题栏关闭按钮中心（BTN_W=30，右上角）：x = win.x + win.w - 1 - 15, y = win.y + 12。"""
    return (win["x"] + win["w"] - 16, win["y"] + 12)


def title_drag_point(win):
    """标题栏中部（避开三按钮与圆角）：(x_center, y_center)。"""
    return (win["x"] + win["w"] // 2, win["y"] + 12)


def resize_grip_point(win, grip=6):
    """右下角缩放抓取点：在 grip 带里取 (w-3, h-3) 相对窗口左上角。"""
    return (win["x"] + win["w"] - 3, win["y"] + win["h"] - 3)


def selftest():
    with VMSession() as s:
        f = s.cap()
        print("[selftest] 屏 %dx%d，串口桌面就绪于 %.1fs" % (f.w, f.h, time.time() - s.t0))
        pos = s.vnc.find_cursor(f, hint=None)
        print("[selftest] 像素格式=%s 光标模板匹配=%s" % (f.fmt, pos))
        dg = s.dock_geom()
        print("[selftest] dock geom = %s" % dg)
        print("[selftest] 截图 %s" % save_png(f.rgb, "selftest_desktop.png"))
        if pos and dg:
            # 闭环校验：把光标挪到 Dock 第 3 项（终端）中心，看客人自己打的 hover 打点是不是 idx=3
            it = s.dock_item(3)
            ok, traj = s.vnc.move_to(it["cx"], it["cy"])
            time.sleep(0.8)
            hov = [l for l in s.log().splitlines() if "[DOCK64] hover idx=" in l]
            print("[selftest] 闭环到 dock 项 3：ok=%s 轨迹末=%s 串口 hover=%s"
                  % (ok, traj[-1] if traj else None, hov[-1][-30:] if hov else "无"))
            return 0 if ok else 1
    return 1
    return 1


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    ap.print_help()
    return 0


if __name__ == "__main__":
    sys.exit(main())
