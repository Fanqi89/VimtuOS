#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/netping64_test.py - ★ v0.9.39：**busybox `ping -c 3` 真跑通**

这一项之前为什么不可能（三条都有实测证据）：
  ① kernel/sock64.cpp 明确「没有做的：SOCK_RAW」，而 busybox 的 ping 必须
     `socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`（networking/ping.c:225）；
  ② socket handle 不进 fd 表，而 busybox 是 `enum { pingsock = 0 }` + `xmove_fd(sock, 0)`
     —— 它把 raw socket **挪到 fd 0** 再用（实测 `ping: can't duplicate file descriptor`）；
  ③ `alarm(37)` 只记录不投递、`setitimer(38)` 根本没实现 —— 而 ping 的 1 秒节拍正是
     `setitimer(ITIMER_REAL)` + SIGALRM 驱动的（ping.c:535-541）；
     另外 busybox 当时是 `CONFIG_FEATURE_FANCY_PING=n` 的"精简 ping"，**完全不解析选项**，
     于是 `ping -c 3 host` 把 "-c" 当主机名（实测 `ping: bad address '-c'`）。

★ 必须先进 Ring 3 shell：内核终端只有 poweroff/shutdown 这类**内建**命令，
  `ping` 是 busybox 的 applet，路径是 `shell` -> `run /bin/busybox ping -c 3 10.0.2.2`。

判据（只认本次会话的串口现场，不看代码）：
  ① `[SOCK64] create dom=2 type=3 proto=1 h=<n> ok=1`      raw ICMP socket 真建出来了
  ② `[FD64] sock alias fd=0 h=<n>`                          xmove_fd(sock, 0) 真生效
  ③ `[PROC64] setitimer pid=<n> … iv_ms=1000 …`             1 秒节拍真装上了
  ④ `[NET64] icmp reply from 10.0.2.2 seq=… ttl=…`  ≥3 个不同 seq
  ⑤ `[SIG64] send … sig=14 src=itimer` ≥2 次                 多包节拍真是 SIGALRM 驱动的
  ⑥ `3 packets transmitted, 3 packets received` + `0% packet loss`
  ⑦ 三行 `64 bytes from 10.0.2.2: seq=… ttl=… time=… ms`
  ⑧ 无 PANIC

用法：py -3 tests/netping64_test.py [--img build64/system.img] [--port 5674]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
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

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
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


def keys_for(text):
    out = []
    for ch in text:
        if ch == " ":
            out.append("spc")
        elif ch == "-":
            out.append("minus")
        elif ch == ".":
            out.append("dot")
        elif ch == "/":
            out.append("slash")
        else:
            out.append(ch)
    return out


class Monitor(object):
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

    def type_keys(self, names, wait=0.12):
        for k in names:
            self.key(k, wait=wait)


class Vm(object):
    def __init__(self, qemu, img, port, name="vimtu-netping64", workdir=None):
        self.qemu, self.img, self.port = qemu, img, port
        self.tmp = workdir or tempfile.mkdtemp(prefix="vimtu64_ping_")
        self.serial = os.path.join(self.tmp, name + "_serial.log")
        if os.path.exists(self.serial):
            os.remove(self.serial)
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none", "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

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
            if self.proc.poll() is not None:
                return needle in self.log()[since:]
            time.sleep(0.3)
        return False

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


FAILS = []


def check(name, ok, detail=""):
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  |  " + str(detail)) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5674)
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统镜像 %s" % args.img)
        return 2

    print("=== 0) 启动 QEMU（e1000 + slirp：网关 10.0.2.2 一定会回 echo reply）===")
    vm = Vm(qemu, args.img, args.port)
    print("    serial = %s" % vm.serial)
    try:
        mon = vm.wait_monitor()
        if not vm.wait_log("[LOCK64] bg blur ready", 200):
            print("环境问题：等不到锁屏就绪")
            return 2
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=1.5)
        if not vm.wait_log("[GUI64] ready", 150):
            print("环境问题：进不了桌面")
            return 2
        check("内核网络已启动（出现 [NET64] 行）", vm.wait_log("[NET64]", 30))

        print("=== 1) 打开终端 -> 进 Ring 3 shell ===")
        mon.key("meta_l", wait=1.2)
        check("Win 键打开开始菜单", vm.wait_log("[START64] open why=win-key", 10))
        mon.key("1", wait=1.6)
        check("数字快捷键 1 打开终端", vm.wait_log("[APP] term opened", 12))
        base = len(vm.log())
        mon.type_keys(keys_for("shell"))
        mon.key("ret", wait=1.2)
        # ★ 用内核侧确定性证据（不靠提示符字符串：它会被 [TASK64] 日志穿插）
        check("进入 Ring 3 shell（[ELF64] load path=/bin/shell.bin）",
              vm.wait_log("[ELF64] load path=/bin/shell.bin", 30, since=base))

        print("=== 2) run /bin/busybox ping -c 3 10.0.2.2 ===")
        base = len(vm.log())
        mon.type_keys(keys_for("run /bin/busybox ping -c 3 10.0.2.2"))
        mon.key("ret", wait=0.4)
        ok_done = vm.wait_log("packets received", 90, since=base)

        log = vm.log()[base:]      # ★ 只看本次会话段（开机自检也会打一条 seq=1 的 icmp reply）
        print("=== 3) 内核侧证据（raw ICMP socket / fd 别名 / itimer）===")
        m_cre = re.search(r"\[SOCK64\] create dom=2 type=3 proto=1 h=(\d+) ok=1", log)
        check("① 建出 SOCK_RAW/IPPROTO_ICMP 的 socket（create type=3 proto=1）",
              m_cre is not None, m_cre.group(0) if m_cre else "未出现")
        m_ali = re.search(r"\[FD64\] sock alias fd=0 h=(\d+)", log)
        check("② xmove_fd(sock, 0) 生效（[FD64] sock alias fd=0）",
              m_ali is not None, m_ali.group(0) if m_ali else "未出现")
        m_it = re.findall(r"\[PROC64\] setitimer pid=(\d+) first_ms=(\d+) iv_ms=(\d+)", log)
        check("③ setitimer(ITIMER_REAL) 真装上了", len(m_it) >= 1,
              ("%d 次；最后一次=%s" % (len(m_it), m_it[-1])) if m_it else "未出现")
        # ★ busybox 的 sendping_tail 是每包重新武装的**一次性**定时器
        #   （it_value=interval, it_interval=0）—— 所以要看 first_ms=1000，iv_ms 本就是 0。
        check("③ 1 秒节拍（至少一次 first_ms=1000、且共装了 ≥2 次）",
              any(x[1] == "1000" for x in m_it) and len(m_it) >= 2,
              "first_ms 列表=%s" % ([x[1] for x in m_it],))

        print("=== 4) 网络侧证据（网关真回 3 个不同 seq）===")
        seqs = sorted(set(int(x) for x in re.findall(
            r"\[NET64\] icmp reply from 10\.0\.2\.2 seq=(\d+)", log)))
        check("④ 收到 3 个不同的 echo reply seq", len(seqs) >= 3, "seqs=%s" % (seqs[:8],))
        check("④ 回复里带 ttl（真收到了 IPv4 报文）",
              re.search(r"\[NET64\] icmp reply from 10\.0\.2\.2 seq=\d+ bytes=\d+ ttl=\d+",
                        log) is not None)
        n_sig = len(re.findall(r"\[SIG64\] send pid=\d+ sig=14 src=itimer", log))
        check("⑤ SIGALRM（sig=14, src=itimer）至少投递 2 次（多包节拍）",
              n_sig >= 2, "投递次数=%d" % n_sig)

        print("=== 5) 用户可见输出（busybox 自己的统计行与每包一行）===")
        check("⑥ ping 跑完（出现 'packets received'）", ok_done)
        m_stat = re.search(r"(\d+) packets transmitted, (\d+) packets received", log)
        check("⑥ 统计行 3 发 3 收", m_stat is not None and
              m_stat.group(1) == "3" and m_stat.group(2) == "3",
              m_stat.group(0) if m_stat else "未出现")
        m_loss = re.search(r"(\d+)% packet loss", log)
        check("⑥ 丢包率 0%", m_loss is not None and m_loss.group(1) == "0",
              m_loss.group(0) if m_loss else "未出现")
        lines = re.findall(r"64 bytes from 10\.0\.2\.2: seq=\d+ ttl=\d+ time=[\d.]+ ms", log)
        check("⑦ 三行 '64 bytes from 10.0.2.2 … time=… ms'", len(lines) >= 3,
              "行数=%d 首行=%s" % (len(lines), lines[0] if lines else "无"))
        check("⑧ 无 PANIC", "PANIC" not in vm.log())
    finally:
        vm.close()

    print("")
    if FAILS:
        print("FAILED %d 项：%s" % (len(FAILS), "; ".join(FAILS)))
        return 1
    print("PASS：busybox `ping -c 3` 真跑通（raw ICMP + fd 别名 + ITIMER_REAL/SIGALRM + 3 发 3 收 0 丢包）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
