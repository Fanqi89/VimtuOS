#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/qemuhelp.py - 验收脚本的**公共登录手势**（新增 GUI 脚本直接复用这一份）

为什么需要它：登录界面现在**必须显式输入**才进桌面 —— kernel/locklogin64.cpp 里
`ui.login.auto` 默认 0（见 status_report.py 的「★ P6 真机缺陷修复」第 4 条）。
任何"等 [GUI64] ready"的脚本，如果不先做手势，就会一直停在锁屏/登录界面直到超时。

手势（与 tests/gui_modern64_test.py / tests/icons64_test.py / tests/settings64_test.py /
tests/desktopops64_test.py / tests/ui_extra64_test.py 里的写法**完全同一套**）：
    1) 等锁屏真正可交互：串口出现 "[LOCK64] bg blur ready"
       （kernel/locklogin64.cpp:1395 —— 它正好在进入输入循环之前打）
    2) sendkey ret  -> 锁屏 -> 登录界面（[LOGIN64] login screen shown）
    3) 再 sendkey ret -> 按下登录按钮（无口令用户直接进桌面；有口令的用户脚本再自己输口令）

用法（脚本里两行）：
    import qemuhelp as qh
    qh.login_desktop(mon, lambda: vm.log(), proc=vm.proc)     # 不做手势就永远等不到 [GUI64] ready
    up = vm.wait_log("[GUI64] ready", 180)

不改动脚本的对外行为：只多做"等锁屏 + 两次回车"，不删改任何断言、不放宽任何阈值。
"""
import socket
import time

LOCK_INTERACTIVE = "[LOCK64] bg blur ready"      # 锁屏可交互（kernel/locklogin64.cpp:1395）
DESKTOP_READY = "[GUI64] ready"                  # 桌面就绪


def free_port():
    """找一个空闲 TCP 端口（给 `-monitor telnet:127.0.0.1:<port>,server,nowait` 用）。"""
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Monitor:
    """最小 QEMU monitor（telnet）：只发 sendkey。
    与 tests/fs_tree_test.py 的 Monitor 同款，给**没有自己 monitor 壳**的脚本复用。"""

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

    def key(self, name, wait=1.3):
        for _ in range(3):
            if self.send("sendkey %s" % name, wait=wait):
                return True
            time.sleep(0.2)
        return False


def read_log(src):
    """src = 返回日志文本的可调用对象，或串口日志文件路径。"""
    if callable(src):
        try:
            return src() or ""
        except Exception:
            return ""
    try:
        with open(src, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_mark(src, needle, timeout, proc=None, step=0.3):
    """轮询等串口里出现 needle（proc 可给：进程退出就立刻结束，不白等）。"""
    deadline = time.time() + timeout
    while True:
        if needle in read_log(src):
            return True
        if proc is not None and proc.poll() is not None:
            return False
        if time.time() >= deadline:
            return False
        time.sleep(step)


def press(mon, name, wait=1.0):
    """注入一个按键。mon 可以是任意 monitor 壳：有 key(name, wait=) 就用它；
    只有 send(cmd, wait=) 的（例如 tests/gfx64_test.py 的 Monitor）就走 sendkey 命令。"""
    k = getattr(mon, "key", None)
    if callable(k):
        k(name, wait=wait)
    else:
        mon.send("sendkey %s" % name, wait=wait)


def login_desktop(mon, log, proc=None, timeout=180, first_wait=1.0, second_wait=1.5):
    """★ 显式登录手势（默认不再自动登录）。

    返回 True = 锁屏曾经可交互、两次回车已注入；False = 在 timeout 内没等到锁屏
    （例如脚本本来就停在安装向导：那种脚本**不该**调用本函数）。
    """
    if not wait_mark(log, LOCK_INTERACTIVE, timeout, proc):
        return False
    press(mon, "ret", wait=first_wait)     # 锁屏 -> 登录界面
    press(mon, "ret", wait=second_wait)    # 登录按钮（无口令用户）
    return True


def wait_desktop(mon, log, proc=None, timeout=240, login_timeout=180):
    """新脚本推荐入口：先做登录手势，再等桌面。返回 True = 出现 [GUI64] ready。"""
    login_desktop(mon, log, proc, login_timeout)
    return wait_mark(log, DESKTOP_READY, timeout, proc)


# ---------------------------------------------------------------- VMware 路径（没有 QEMU monitor）
def _recv_exact(s, n):
    buf = b""
    while len(buf) < n:
        ch = s.recv(n - len(buf))
        if not ch:
            raise OSError("VNC 连接被关闭（收到 %d/%d 字节）" % (len(buf), n))
        buf += ch
    return buf


KEY_RETURN = 0xFF0D                       # X11/RFB keysym：Return


def vnc_press(port, keysym=KEY_RETURN, count=1, host="127.0.0.1", gap=0.8, timeout=8):
    """在 VNC 服务器上敲键（RFB KeyEvent）——给**没有 QEMU monitor**的 VMware 路径做登录手势。

    返回 True = 已把 count 次"按下+抬起"发出去。协议：3.3 与 3.7/3.8（安全类型 None）。
    """
    s = socket.create_connection((host, port), timeout=timeout)
    try:
        s.settimeout(timeout)
        banner = _recv_exact(s, 12).split(b"\n")[0].strip()      # b"RFB 003.008"
        ver = banner if banner.startswith(b"RFB ") else b"RFB 003.003"
        s.sendall(ver + b"\n")
        try:
            major, minor = int(ver[4:7]), int(ver[8:11])
        except ValueError:
            major, minor = 3, 3
        if (major, minor) >= (3, 7):
            n = _recv_exact(s, 1)[0]
            types = _recv_exact(s, n) if n else b""
            s.sendall(bytes([1 if 1 in types else (types[0] if types else 1)]))
            if (major, minor) >= (3, 8):
                if _recv_exact(s, 4) != b"\x00\x00\x00\x00":
                    return False                                 # 认证失败（VMware 设了 VNC 密码）
        else:
            _recv_exact(s, 4)                                    # 3.3：直接回安全类型
        s.sendall(b"\x01")                                       # ClientInit(shared)
        _recv_exact(s, 24)                                       # ServerInit（宽高+像素格式+名长）
        # 桌面名是"服务端主动发"的，读不读都不影响后面发键 —— 尽力读一下，失败就跳过
        # （VMware 的实现里这一步会卡住，所以这里必须容错，不能让它挡住登录手势）。
        try:
            s.settimeout(1.5)
            nl = int.from_bytes(_recv_exact(s, 4), "big")
            if 0 < nl <= 4096:
                _recv_exact(s, nl)
        except OSError:
            pass
        s.settimeout(timeout)
        ks = int(keysym).to_bytes(4, "big")
        for _ in range(max(1, int(count))):
            s.sendall(bytes([4, 1, 0, 0]) + ks)                   # KeyEvent down
            s.sendall(bytes([4, 0, 0, 0]) + ks)                   # KeyEvent up
            time.sleep(gap)
        return True
    finally:
        try:
            s.close()
        except OSError:
            pass


def vnc_login_desktop(log, port, proc=None, timeout=180, count=2, gap=0.8):
    """VMware 路径的登录手势：等锁屏可交互 -> 用 VNC 敲 count 次回车进桌面。"""
    if not wait_mark(log, LOCK_INTERACTIVE, timeout, proc):
        return False
    try:
        return vnc_press(port, count=count, gap=gap)
    except OSError:
        return False
