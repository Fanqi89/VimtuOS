#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fs_term_test.py - 终端接**真文件系统**（kernel/fd64.cpp -> vfs64 -> VimtuFS2）端到端验收

做什么（两遍冷启动，**同一块盘**）：
  第一遍：
    1) 开始菜单 -> 终端，敲 `write /t.txt hello`
       断言 [FD64] open path=/t.txt fd=.. / [FD64] write fd=.. n=5 / [FD64] close fd=..
            + [TERM] cmd write ok（fd 层是真路径，不再是 16x512B 的 RAM-only ramfs）；
    2) 敲 `ls`：断言 [TERM] cmd ls entries=<n> bytes=<b>（n >= 1；n 会与第二遍对照）；
    3) 敲 `df`：断言 [TERM] cmd df blocks=<总块> free=<空闲块>，且 free < blocks（数字合理）；
    4) 敲 `run filedemo.elf`：ring3 程序走 syscall open(2)/read(0)/write(1)/close(3) 打开 /t.txt：
       断言第二组 [FD64] open path=/t.txt、[FD64] read fd=.. n=5 + 程序输出片段
       （filedemo: open /t.txt fd= / filedemo: read= / hello / filedemo: done）。
  第二遍（**冷启动同一镜像**，VimtuFS2 的持久化证据）：
    5) 敲 `cat /t.txt`：断言 [TERM] cmd cat bytes=5 —— 文件跨重启还在、内容就是第一遍写的 5 字节；
    6) 敲 `ls`（删除前）记录 entries=B；
    7) 敲 `rm /t.txt`：断言 [TERM] cmd rm ok；
    8) 再敲 `ls`：entries=C，必须 C == B-1（**rm 之后 ls 不再列出**）。
  两遍都禁止 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [TERM] unsupported。

边界（如实写）：单层路径 "/name"、单文件 <= 67584 B、无子目录树、无权限；rm 不能删目录。
用法（必须用 Windows 原生 Python）：py -3 tests\\fs_term_test.py [--timeout 180] [--keep]
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
    "[TERM] unsupported",
]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Monitor:
    """QEMU monitor（telnet）客户端：sendkey 注入键盘（tmgr_proc_test 同款，含单连接重试）。"""

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
        """把一行 ASCII 文本敲进当前窗口（只支持本测试用到的字符集）。"""
        names = {
            " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
            "=": "equal", "_": "shift-minus",
        }
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
    args_q = [qemu, "-name", "Vimtu64-fs", "-drive", "format=raw,file=%s" % p64.q(img),
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
    img = p64.prepare_fixture()          # ★ 只造一次：两遍冷启动共用同一块盘
    if not img:
        sys.stderr.write("造测试盘失败\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_fs_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def open_terminal(mon, serial, proc, tag):
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if "[APP] term opened" in wait_for(serial, "[APP] term opened", 12, proc):
                opened = True
                break
        check("%s：开始菜单 -> 终端（[APP] term opened）" % tag,
              opened and "[APP] term opened" in slog(serial))
        # 开终端会幂等安装 /spin.elf 与 /filedemo.elf：
        #   第一遍：必须看到安装行；第二遍（同一块盘冷启动）：必须**没有**安装行（幂等跳过 = 持久化）。
        if tag == "第一遍":
            check("%s：开终端幂等安装内嵌程序（[TERM] install /filedemo.elf）" % tag,
                  "[TERM] install /filedemo.elf" in slog(serial))
        else:
            check("%s：filedemo.elf 仍在盘上（安装幂等跳过）" % tag,
                  "[TERM] install /filedemo.elf" not in slog(serial))

    # ==================== 第一遍：写 + ls + df + ring3 读 ====================
    print("=== 第一遍：write/ls/df/run filedemo.elf ===")
    serial1 = os.path.join(tmp, "boot1.log")
    port1 = free_port()
    proc, log = boot(qemu, img, serial1, port1)
    mon = Monitor(port1)
    open_terminal(mon, serial1, proc, "第一遍")

    mon.type_line("write /t.txt hello")
    log = wait_for(serial1, "[TERM] cmd write", 15, proc)
    check("write：命令 ok（真写盘）", "[TERM] cmd write ok" in log)
    check("write：走 FD 层打点 [FD64] open path=/t.txt", "[FD64] open path=/t.txt fd=" in log)
    mw = re.search(r"\[FD64\] write fd=(\d+) n=(\d+) total=(\d+)", log)
    check("write：[FD64] write n=5（真写入 5 字节）", bool(mw) and mw.group(2) == "5",
          mw.group(0) if mw else "（缺 [FD64] write 行）")
    check("write：[FD64] close fd=..", "[FD64] close fd=" in log)

    mon.type_line("ls")
    log = wait_for(serial1, "[TERM] cmd ls entries=", 20, proc)
    ml1 = re.findall(r"\[TERM\] cmd ls entries=(\d+) bytes=(\d+)", log)
    check("ls：[TERM] cmd ls entries=<n> bytes=<b>", bool(ml1), str(ml1[-1]) if ml1 else "")
    entries1 = int(ml1[-1][0]) if ml1 else 0
    check("ls：条目数 >= 1（/t.txt 已被列出）", entries1 >= 1, "entries=%d" % entries1)

    mon.type_line("df")
    log = wait_for(serial1, "[TERM] cmd df blocks=", 20, proc)
    md = re.search(r"\[TERM\] cmd df blocks=(\d+) free=(\d+) files=(\d+)", log)
    check("df：[TERM] cmd df blocks=/free=/files=", bool(md), md.group(0) if md else "")
    if md:
        blocks, free = int(md.group(1)), int(md.group(2))
        check("df：总块 > 0 且 空闲块 < 总块（数字合理）", blocks > 0 and free < blocks,
              "blocks=%d free=%d" % (blocks, free))

    n_open_before = log.count("[FD64] open path=/t.txt")
    mon.type_line("run filedemo.elf")
    log = wait_for(serial1, "filedemo: done", 30, proc)
    check("run filedemo.elf：程序跑完（filedemo: done (exit 0)）", "filedemo: done (exit 0)" in log)
    check("ring3 文件访问走 FD 层（出现第二组 [FD64] open path=/t.txt）",
          log.count("[FD64] open path=/t.txt") > n_open_before,
          "opens=%d" % log.count("[FD64] open path=/t.txt"))
    check("ring3：filedemo: open /t.txt fd=（用户程序自己的输出）",
          "filedemo: open /t.txt fd=" in log)
    mr1 = re.findall(r"\[FD64\] read fd=(\d+) n=(\d+)", log)
    check("ring3：读回 5 字节（[FD64] read n=5）", any(r[1] == "5" for r in mr1),
          str(mr1[-3:]) if mr1 else "（无 read 行）")
    check("ring3：打印出文件内容（hello 片段）", "hello" in log)

    print("--- 第一遍禁止项 ---")
    for bad in FORBIDDEN:
        check("第一遍不得出现 %s" % bad, bad not in log)
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass

    # ==================== 第二遍：冷启动同一块盘 -> cat 读回 + rm ====================
    print("=== 第二遍（冷启动同一镜像）：cat 跨重启读回 + rm ===")
    serial2 = os.path.join(tmp, "boot2.log")
    port2 = free_port()
    proc2, log2 = boot(qemu, img, serial2, port2)
    mon2 = Monitor(port2)
    open_terminal(mon2, serial2, proc2, "第二遍")

    mon2.type_line("cat /t.txt")
    log2 = wait_for(serial2, "[TERM] cmd cat bytes=", 20, proc2)
    mc = re.search(r"\[TERM\] cmd cat bytes=(\d+)", log2)
    check("第二遍 cat：命令 ok 且读到 5 字节（跨重启持久化）",
          "[TERM] cmd cat ok" in log2 and bool(mc) and mc.group(1) == "5",
          mc.group(0) if mc else "（缺 [TERM] cmd cat 行）")
    check("第二遍 cat：文件路径经过 FD 层（[FD64] open path=/t.txt）",
          "[FD64] open path=/t.txt fd=" in log2)

    mon2.type_line("ls")
    log2 = wait_for(serial2, "[TERM] cmd ls entries=", 20, proc2)
    mls = re.findall(r"\[TERM\] cmd ls entries=(\d+) bytes=(\d+)", log2)
    entries_before = int(mls[-1][0]) if mls else 0
    check("第二遍 ls（删除前）：entries >= 1", entries_before >= 1, "entries=%d" % entries_before)

    mon2.type_line("rm /t.txt")
    log2 = wait_for(serial2, "[TERM] cmd rm", 20, proc2)
    check("rm：命令 ok（vfs64_unlink 真删）", "[TERM] cmd rm ok" in log2)

    if entries_before > 0:
        mon2.type_line("ls")
        log2 = wait_for(serial2, "[TERM] cmd ls entries=%d" % (entries_before - 1), 20, proc2)
    mls2 = re.findall(r"\[TERM\] cmd ls entries=(\d+) bytes=(\d+)", log2)
    entries_after = int(mls2[-1][0]) if mls2 else -1
    check("rm 之后 ls 不再列出（entries 恰好 -1）", entries_after == entries_before - 1,
          "before=%d after=%d" % (entries_before, entries_after))

    print("--- 第二遍禁止项 ---")
    for bad in FORBIDDEN:
        check("第二遍不得出现 %s" % bad, bad not in log2)
    if proc2.poll() is None:
        proc2.kill()
        try:
            proc2.wait(timeout=10)
        except Exception:
            pass

    if args.keep:
        print("[fs_term] 串口日志：%s / %s" % (serial1, serial2))
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
