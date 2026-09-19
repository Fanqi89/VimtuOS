#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fd64_test.py - 批次 D：**每进程 fd 表 + fd 继承 + O_APPEND + pipe** 端到端验收

做什么（一遍冷启动，BIOS/SeaBIOS 路径 —— 每进程 CR3 生效，fork 才可用）：
  A) 启动期（内核自己跑的 ring3 pipe 演示，见 kernel/proc64.cpp 的 proc64_pipe_demo64）：
     1) [PROC64] install ok path=/pipe64.elf              —— 幂等装进 VimtuFS2
     2) [PROC64] fork ... fds=2 fds_shared=1              —— **fork 继承整张 fd 表**（逐槽共享对象）
     3) [FD64] pipe r=.. w=.. bytes=64                    —— pipe(22) 真实现（64B 环形缓冲）
     4) [FD64] pipe read n=18 data=PIPE-OK-FROM-CHILD     —— 子写父读的**环回数据**
     5) pipe64: child wrote n=.. / pipe64: parent read n=.. / PIPE-OK-FROM-CHILD
        （用户程序的输出会被每个 write 的 [SYSCALL] insn 日志打断，所以按片段断言）
     6) [PROC64] pipe-demo done ... exited=1 code=0       —— 整个演示以 exit(0) 收尾
  B) 终端 `fdtest`（内核表 = 桌面/终端进程那张表；实现在 kernel/fd64.cpp 的 fd64_demo64）：
     7) [FD64] demo indep fd_a.. fd_b.. a1=0123 b1=0123 a2=4567 b2=4567 same_object=0
        —— 同进程两次 open 同一个文件 = **两个独立游标**（读一半再读另一半能接上）
     8) [FD64] demo dup fd_a.. fd_dup.. same_object=1 off_a=<n> off_dup=<n>（两个偏移相等）
        —— dup(32) 后两个 fd **共享同一个 OpenFile64**（一个读、另一个的偏移跟着走）
     9) [FD64] demo append lseek=0 w=1 off_after=11 bytes=11 content=0123456789!
        —— O_APPEND：先 lseek 到 0 再写，内容仍然追加到末尾（写后偏移 = 末尾）
    10) [FD64] demo pipe n=9 data=PIPE-RING ok=1 + [FD64] pipe read n=9 data=PIPE-RING
        —— pipe 环回（内核侧）
    11) [FD64] demo pipe shortwrite=64/100 (ring=64)     —— 写满 = 短写（不阻塞）
    12) [FD64] demo pipe empty=1011 (1000+EAGAIN) eof=0  —— 读空 = -EAGAIN；写端全关 = 0（EOF）
    13) [FD64] demo forkinherit ... same_object=1 refs_plus1=1 survived_child_exit=1
        —— fork 的 fd 语义：克隆整张表 = 逐槽共享同一个对象 + 引用计数 +1；"子进程退出"后原对象还在
    14) [FD64] close fd=.. refs=1 / refs=0 (object freed) —— close 只是引用计数 -1（归零才释放）
    15) [FD64] demo PASS ok=1
  C) 禁止项：PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [FD64] demo FAIL /
     [FD64] demo skipped（没卷 = 环境坏了，不该当通过）。

边界（如实写，细节见 kernel/fd64.h 与 docs/应用层与系统调用说明.md）：
  * pipe 容量固定 64 B、没有阻塞/select/poll（写满短写、读空 -EAGAIN）；
  * 没有文件权限、没有 O_CLOEXEC（execve 默认保留 fd）、fd 表容量 32/进程、对象池 64；
  * vfs64 没有 read-at-offset：读写都要把整个文件过一遍内存。

用法（必须用 Windows 原生 Python）：py -3 tests\\fd64_test.py [--timeout 180] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  （夹具/启动工具复用：prepare_fixture/find_qemu）

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[FD64] demo FAIL",
    "[FD64] demo skipped",
]

# 启动期 ring3 pipe 演示（片段匹配：用户程序输出会被 [SYSCALL] insn 行打断）
BOOT_MUST = [
    ("[FD64] selftest PASS", "FD 层自检（路径规范化 + 表 + dup 共享对象 + 目录句柄）"),
    ("[PROC64] install ok path=/pipe64.elf", "幂等装进 VimtuFS2 的 ring3 pipe 程序"),
    ("[PROC64] fork parent=", "fork 发生（每进程 CR3 生效）"),
    ("fds_shared=1", "fork 逐槽共享 fd 打点"),
    ("[FD64] pipe r=", "pipe(22) 创建成功"),
    ("[FD64] pipe read n=18 data=PIPE-OK-FROM-CHILD", "★ 子进程写、父进程读的环回数据"),
    ("pipe64: pipe r=", "用户程序自己的 pipe() 打印"),
    ("pipe64: child wrote n=", "子进程打印写入字节数"),
    ("pipe64: parent read n=", "父进程打印读到的字节数"),
    ("PIPE-OK-FROM-CHILD", "管道里传过去的原文（也出现在 [FD64] pipe read 行）"),
    ("pipe64: demo done exit(0)", "ring3 演示以 exit(0) 收尾"),
    ("[PROC64] pipe-demo done", "内核侧 pipe 演示收尾"),
]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Monitor:
    """QEMU monitor（telnet）客户端：sendkey 注入键盘（fs_term_test 同款，含单连接重试）。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
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

    def key(self, name, wait=0.35):
        for _ in range(3):
            if self.send("sendkey %s" % name, wait=wait):
                return True
            time.sleep(0.2)
        return False

    def type_line(self, text, per_key=0.14):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "=": "equal", "_": "shift-minus"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char for sendkey: %r" % ch)
        self.key("ret", wait=per_key + 0.15)


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(path, needle, timeout, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = slog(path)
        if needle in s:
            return s
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return slog(path)


def boot(qemu, img, serial, port):
    args_q = [qemu, "-name", "Vimtu64-fd64", "-drive", "format=raw,file=%s" % p64.q(img),
              "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
              "-serial", "file:%s" % p64.q(serial),
              "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
              "-no-reboot"]
    proc = subprocess.Popen(args_q, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log = wait_for(serial, "[GUI64] ready", 90, proc)
    return proc, log


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(p64.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % p64.SYSTEM_IMG)
        return 2
    img = p64.prepare_fixture()
    if not img:
        sys.stderr.write("造测试盘失败\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_fd64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== Vimtu64 fd64 acceptance（每进程 fd 表 / dup / fork 继承 / O_APPEND / pipe）===")
    serial = os.path.join(tmp, "boot.log")
    port = free_port()
    proc, log = boot(qemu, img, serial, port)
    mon = Monitor(port)

    # ==================== A) 启动期 ring3 pipe 演示 ====================
    print("--- A) 启动期：ring3 pipe 演示（fork 后子写父读）---")
    for needle, what in BOOT_MUST:
        check("%s（%s）" % (needle, what), needle in log)

    forks = re.findall(r"\[PROC64\] fork parent=(\d+) child=(\d+) cr3=([0-9A-Fa-f]{16}) pages=(\d+) "
                       r"fds=(\d+) fds_shared=1", log)
    check("fork 行都带 fds=/fds_shared=1（fd 表继承打点齐全）", len(forks) >= 4, "forks=%d" % len(forks))
    check("pipe 演示那次 fork：fds=2（读端 + 写端都被继承）", any(f[4] == "2" for f in forks),
          str(forks[:4]))
    check("pipe-demo 以 code=0 退出（exited=1 code=0）",
          re.search(r"\[PROC64\] pipe-demo done pid=\d+ exited=1 code=0", log) is not None)

    # ==================== B) 终端 fdtest ====================
    print("--- B) 终端 fdtest：独立游标 / dup 共享游标 / O_APPEND / pipe / fork 继承 ---")
    opened = False
    for _ in range(3):
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        if "[APP] term opened" in wait_for(serial, "[APP] term opened", 12, proc):
            opened = True
            break
    check("开始菜单 -> 终端（[APP] term opened）", opened)

    mon.type_line("fdtest")
    log = wait_for(serial, "[TERM] cmd fdtest", 25, proc)
    check("fdtest：[TERM] cmd fdtest ok", "[TERM] cmd fdtest ok" in log)

    mi = re.search(r"\[FD64\] demo indep fd_a=(\d+) fd_b=(\d+) a1=(\S+) b1=(\S+) a2=(\S+) b2=(\S+) "
                   r"same_object=(\d+)", log)
    check("独立游标：两次 open 读到同一段（a1=b1=0123、a2=b2=4567）且对象不同",
          bool(mi) and mi.group(3) == "0123" and mi.group(4) == "0123" and
          mi.group(5) == "4567" and mi.group(6) == "4567" and mi.group(7) == "0",
          mi.group(0) if mi else "（缺 indep 行）")

    md = re.search(r"\[FD64\] demo dup fd_a=(\d+) fd_dup=(\d+) same_object=(\d+) off_a=(\d+) "
                   r"off_dup=(\d+)", log)
    dup_ok = bool(md) and md.group(3) == "1" and md.group(4) == md.group(5) and int(md.group(4)) > 0
    check("dup 共享游标：same_object=1 且 off_a == off_dup > 0", dup_ok,
          md.group(0) if md else "（缺 dup 行）")

    ma = re.search(r"\[FD64\] demo append lseek=0 w=1 off_after=11 bytes=11 content=(\S+)", log)
    check("O_APPEND：lseek 到 0 再写 -> 仍然追加到末尾（off_after=11 / content=0123456789!）",
          bool(ma) and ma.group(1) == "0123456789!", ma.group(0) if ma else "（缺 append 行）")

    check("pipe 环回（内核侧）：[FD64] pipe read n=9 data=PIPE-RING",
          "[FD64] pipe read n=9 data=PIPE-RING" in log)
    check("pipe 环回（demo 行）：[FD64] demo pipe n=9 data=PIPE-RING ok=1",
          "[FD64] demo pipe n=9 data=PIPE-RING ok=1" in log)
    check("pipe 短写（满 = 64 B）：[FD64] demo pipe shortwrite=64/100 (ring=64)",
          "[FD64] demo pipe shortwrite=64/100 (ring=64)" in log)
    check("pipe 空读（写端还开着）= -EAGAIN、写端全关后读 = 0（EOF）",
          "[FD64] demo pipe empty=1011 (1000+EAGAIN) eof=0" in log)

    mfh = re.search(r"\[FD64\] demo forkinherit fd=(\d+) same_object=1 refs_plus1=1 "
                    r"survived_child_exit=1 off_after=(\d+) ok=1", log)
    check("fork 的 fd 语义：表克隆逐槽共享对象 + 引用计数 +1 + 原对象活过\"子进程退出\"",
          bool(mfh) and mfh.group(2) == "7", mfh.group(0) if mfh else "（缺 forkinherit 行）")

    check("close 是引用计数 -1：[FD64] close fd=.. refs=1",
          re.search(r"\[FD64\] close fd=\d+ refs=1", log) is not None)
    check("引用归零才真释放：[FD64] close .. refs=0 (object freed)",
          re.search(r"\[FD64\] close fd=\d+ refs=0 \(object freed\)", log) is not None)
    check("fdtest 收尾：[FD64] demo PASS ok=1", "[FD64] demo PASS ok=1" in log)

    print("--- 禁止项 ---")
    for bad in FORBIDDEN:
        check("不得出现 %s" % bad, bad not in log)
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass

    if args.keep:
        print("[fd64] 串口日志：%s" % serial)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
