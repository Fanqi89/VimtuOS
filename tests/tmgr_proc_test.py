#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/tmgr_proc_test.py - 任务管理器"进程页 = proc64 真进程"端到端验收

为什么需要这个脚本：proc64 的启动期多进程演示跑完就退出了，桌面起来时进程表是空的——
"进程页显示真进程、并且能 kill"必须有**运行期创建的长命进程**才验证得了。做法：
  1) QEMU monitor 注入键盘：开始菜单 -> 终端 -> 敲 `proc run spin`（内嵌 /spin.elf 的
     proc64_create64+proc64_start_elf64 真路径，进程永不退出）；
  2) 再开任务管理器（开始菜单 -> 任务管理器）：
       断言新增打点 [UI] tmgr proc rows=<n>（n>=1）与进程行内容
       [UI] tmgr proc row pid=.. ppid=.. name=spin state=.. cr3=0x.. threads=.. cpu_permille=.. pages=..
       （cr3 必须不是 0、也不是内核地址空间 0x40000 -> 每进程地址空间是真生效的）
  3) 键盘 down + 回车 -> 进程页的"结束进程"走 proc64_kill64(pid, SIGKILL=9)：
       断言 [UI] tmgr kill proc pid=<pid> ... rc=0
            + [PROC64] kill pid=<pid> sig=9 task_id=..
            + [PROC64] exit pid=<pid> code=137 cr3_released=1
  4) 原有打点 [UI] tmgr page proc / [UI] tmgr rows= 必须仍在（不许因本轮改动丢失）。
  5) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

用法：py -3 tests\\tmgr_proc_test.py [--timeout 180] [--keep]
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

import proc64_test as p64          # noqa: E402  （夹具/启动工具复用）

# 终端里要敲的命令（QEMU sendkey 键名）
RUN_CMD_KEYS = ["p", "r", "o", "c", "spc", "r", "u", "n", "spc", "s", "p", "i", "n", "ret"]

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
    p = s.getsockname()[1]
    s.close()
    return p


class Monitor:
    """QEMU monitor（telnet）客户端：sendkey 注入键盘。"""

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
        # QEMU monitor 是单连接（server,nowait）：连接偶发被拒会静默丢一次按键 —— 重试 3 次。
        for _ in range(3):
            if self.send("sendkey %s" % name, wait=wait):
                return True
            time.sleep(0.2)
        return False


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

    tmp = tempfile.mkdtemp(prefix="vimtu64_tmgr_")
    serial = os.path.join(tmp, "tmgr.log")
    port = free_port()
    args_q = [qemu, "-name", "Vimtu64-tmgr", "-drive", "format=raw,file=%s" % p64.q(img),
              "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
              "-serial", "file:%s" % p64.q(serial),
              "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
              "-no-reboot"]
    proc = subprocess.Popen(args_q, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    try:
        log = wait_for(serial, "[GUI64] ready", args.timeout, proc)
        check("[GUI64] ready（桌面起来）", "[GUI64] ready" in log)
        check("[PRELOAD64] glyphs prewarmed=（预热先跑过）", "[PRELOAD64] glyphs prewarmed=" in log)

        mon = Monitor(port)
        time.sleep(1.0)

        # ---- 1) 开始菜单 -> 终端 -> 敲 `proc run spin` ----
        mon.key("meta_l", wait=0.9)
        mon.key("1", wait=1.8)
        log = wait_for(serial, "[APP] term opened", 15, proc)
        check("开始菜单 -> 终端（[APP] term opened）", "[APP] term opened" in log)

        for k in RUN_CMD_KEYS:
            mon.key(k, wait=0.22)
        log = wait_for(serial, "[TERM] proc run path=/spin.elf", 25, proc)
        # spin 的脏槽位重试发生在内核里；如果这一轮还是没活下来（拿不到 spin64: alive），
        # 就在终端里再敲一次 proc run spin（幂等：/spin.elf 已安装，直接建进程）。
        if "spin64: alive pid=" not in log:
            for k in RUN_CMD_KEYS:
                mon.key(k, wait=0.22)
            log = wait_for(serial, "spin64: alive pid=", 20, proc)
        check("终端 proc run spin -> [TERM] proc run path=/spin.elf",
              "[TERM] proc run path=/spin.elf" in log, )
        runs = re.findall(r"\[TERM\] proc run path=/spin\.elf pid=(\d+)", log)
        check("解析到真进程 pid", bool(runs), ("runs=%s" % runs) if runs else "")
        pid = runs[-1] if runs else None
        check("[PROC64] start loaded entry=（ELF 装载 + 建任务）", "[PROC64] start loaded entry=" in log)
        check("用户程序真的在 ring3 跑（spin64: alive pid=）", "spin64: alive pid=" in log,
              next((ln for ln in log.splitlines() if "spin64: alive" in ln), "")[:140])

        # ---- 2) 开始菜单 -> 任务管理器：进程页显示真进程 ----
        # 键盘注入偶发丢键（monitor 单连接）：没看到 [APP] tmgr opened 就重开一次（最多 3 次，
        # 任务管理器本身最多 4 个实例，够用）。
        for _attempt in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("7", wait=2.5)
            log = wait_for(serial, "[APP] tmgr opened", 10, proc)
            if "[APP] tmgr opened" in log:
                break
        log = wait_for(serial, "[UI] tmgr proc rows=", 25, proc)
        check("任务管理器打开（[APP] tmgr opened）", "[APP] tmgr opened" in log)
        check("原有打点仍在：[UI] tmgr page proc", "[UI] tmgr page proc" in log)
        check("原有打点仍在：[UI] tmgr rows=", "[UI] tmgr rows=" in log)
        mm = re.search(r"\[UI\] tmgr proc rows=(\d+) total=(\d+)", log)
        check("新增打点 [UI] tmgr proc rows=<n>", bool(mm), mm.group(0) if mm else "")
        rows_n = int(mm.group(1)) if mm else 0
        total_n = int(mm.group(2)) if mm else 0
        check("进程页行数 >= 1（真进程表非空）", rows_n >= 1, "rows=%d total=%d pid=%s" % (rows_n, total_n, pid))
        # 行日志每 2 秒会重打；取**最后一条**满足 pid 的活进程行（若 pid 已死则退回最后一条 spin 行），
        # 避免恰好在"进程已退出"的那一拍取到 state=4 的旧快照而假失败。
        row_re = re.compile(r"\[UI\] tmgr proc row pid=(\d+) ppid=(\d+) name=(\S+) state=(\d+) "
                            r"cr3=0x([0-9A-Fa-f]+) threads=(\d+) cpu_permille=(\d+) pages=(\d+)")
        rows_all = row_re.findall(log)
        live_rows = [r for r in rows_all if (pid is None or r[0] == pid) and 1 <= int(r[3]) <= 3]
        pick = live_rows[-1] if live_rows else (rows_all[-1] if rows_all else None)
        mr = pick
        check("新增打点 [UI] tmgr proc row pid/ppid/name/state/cr3/threads/cpu_permille/pages",
              bool(mr), (" ".join(mr)) if mr else "")
        if mr:
            check("行内容 = 刚建的 spin 进程（pid 一致、name=spin、ppid=0）",
                  (pid is None or mr[0] == pid) and mr[2] == "spin" and mr[1] == "0",
                  " ".join(mr))
            check("state 是活进程状态（READY/RUNNING/SLEEP = 1..3）", 1 <= int(mr[3]) <= 3,
                  "state=%s" % mr[3])
            check("CR3 非 0 且不是内核地址空间 0x40000（每进程地址空间生效）",
                  int(mr[4], 16) != 0 and int(mr[4], 16) != 0x40000,
                  "cr3=0x%s" % mr[4])
            check("线程数 >= 1", int(mr[5]) >= 1, "threads=%s" % mr[5])
        else:
            for nm in ("行内容 = 刚建的 spin 进程", "state 是活进程状态", "CR3 非 0 且不是内核地址空间",
                       "线程数 >= 1"):
                check(nm, False)

        # ---- 3) down + 回车 -> 结束进程（proc64_kill64 SIGKILL=9）----
        # 键盘注入偶发丢键（--full 里实测过：down/ret 没到窗口 -> 没有 kill 行），所以重试最多 5 次，
        # 每次 down+ret+ret（重复回车对已被杀的行只会再打一条 rc=0，幂等无害）。
        kill_re = re.compile(r"\[UI\] tmgr kill proc pid=(\d+) name=\S+ sig=9 rc=(-?\d+)")
        mk = None
        for _attempt in range(5):
            mon.key("down", wait=0.8)
            mon.key("ret", wait=1.2)
            mon.key("ret", wait=1.2)
            log = wait_for(serial, "[UI] tmgr kill proc", 6, proc)
            hits = kill_re.findall(log)
            want = [h for h in hits if (pid is None or h[0] == pid)] or hits
            if want:
                mk = want[-1]
                break
        check("新增打点 [UI] tmgr kill proc pid=.. sig=9 rc=0", bool(mk) and mk[1] == "0",
              (" ".join(mk)) if mk else "")
        if mk and pid is not None:
            check("被 kill 的 pid 与建出来的进程一致", mk[0] == pid,
                  "kill=%s run=%s" % (mk[0], pid))
        if pid:
            check("[PROC64] kill pid=%s sig=9 task_id=（真走 proc64 kill 路径）" % pid,
                  ("[PROC64] kill pid=%s sig=9" % pid) in log)
            check("[PROC64] exit pid=%s code=137 cr3_released=1（SIGKILL 退出码）" % pid,
                  ("[PROC64] exit pid=%s code=137 cr3_released=1" % pid) in log)
        else:
            check("[PROC64] kill 行", "[PROC64] kill pid=" in log)

        # ---- 4) 禁止出现 ----
        print("--- 禁止出现 ---")
        for needle in FORBIDDEN:
            check("不得出现 %s" % needle, needle not in log)

        print("--- serial tail ---")
        for line in [x for x in log.splitlines() if x.strip()][-12:]:
            print("   | " + line[:170])
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    if args.keep:
        print("[tmgr] 串口日志：%s" % serial)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
