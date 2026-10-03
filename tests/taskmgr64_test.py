#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/taskmgr64_test.py - 任务管理器"能列出并结束运行中的程序"端到端验收（★ P7b 缺陷修复）

被验的缺陷（用户实测）：打开任务管理器 -> 进程页**看不到正在运行的程序**，也**结束不了**它。
根因（修之前）：`kernel/taskmgr64.cpp` 的 tm_build_rows() **只扫 proc64 进程表**，而桌面/应用都跑在
task64 内核任务里 —— 平常 proc 表是空的，于是行数 0、选中行恒 -1、回车只提示"请先选择"；再加上进程页
只在鼠标/键盘事件后重画，新起的进程也不会出现。

本脚本钉住修好之后的行为（每条都有串口证据）：
  ① 纯桌面（**没有任何用户进程**）打开任务管理器：列表**非空** —— 有 proc64 进程行 + task64 内核任务行
     （`[UI] tmgr proc rows=N total=T procs=P tasks=K`，P>=0、K>=1），且至少一条
     `[UI] tmgr task row id=.. name=.. state=.. ticks=.. switches=.. cpu_permille=.. slot=.. cur=..`；
  ② 列表条目与内核 `ps` **逐项一致**：终端敲 `ps diag` 打出的 `[TASK64] diag slot=.. id=.. name=.. state=..`
     与本脚本从任务管理器行里解析出来的 (slot,id,name,state) 完全一致（tick/switches 允许更大 —— 时间在走）；
  ③ **刷新**：在终端起一个真进程 `proc run spin`，**不碰任务管理器**，它自己会（外壳 tick 驱动的定期重画）
     打出新的 `[UI] tmgr proc rows=.. procs=1` 行 —— 证明表不是死的；
  ④ **结束运行中的程序**：新开一个任务管理器实例 -> down -> 回车 -> `[UI] tmgr kill proc pid=<spin> … sig=9 rc=0`
     + `[PROC64] kill pid=<spin> sig=9` + `[PROC64] exit pid=<spin> code=137 cr3_released=1`（真的没了）；
  ⑤ **重复结束已退出的 pid**：再按一次回车 -> 只允许 `rc=-3`（-ESRCH）并给提示，**不得 PANIC**；
  ⑥ **结束内核任务这条线也接通了**：在纯桌面实例上 down+回车 -> `[UI] tmgr kill task id=0 … rc=-1`
     （task_kill64 的护栏拒绝 idle/桌面），提示"无法结束任务"，不崩；
  ⑦ 原有打点不许丢：[UI] tmgr page proc / [UI] tmgr proc rows= / [TASK] ps rows=；
  ⑧ 全程不得 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

用法：py -3 tests\\taskmgr64_test.py [--timeout 300] [--keep]
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

import proc64_test as p64                  # noqa: E402  （夹具/启动工具复用）
import qemuhelp as qh                      # noqa: E402  （公共登录手势）

RUN_CMD_KEYS = ["p", "r", "o", "c", "spc", "r", "u", "n", "spc", "s", "p", "i", "n", "ret"]
PS_DIAG_KEYS = ["p", "s", "spc", "d", "i", "a", "g", "ret"]
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL"]

ROW_RE = re.compile(r"\[UI\] tmgr task row id=(\d+) name=(\S+) state=(\d+) ticks=(\d+) "
                    r"switches=(\d+) cpu_permille=(\d+) slot=(\d+) cur=(\d+)")
PROC_RE = re.compile(r"\[UI\] tmgr proc row pid=(\d+) ppid=(\d+) name=(\S+) state=(\d+) "
                     r"cr3=0x([0-9A-Fa-f]+) threads=(\d+) cpu_permille=(\d+) pages=(\d+)")
DIAG_RE = re.compile(r"\[TASK64\] diag slot=(\d+) id=(\d+) name=(\S+) state=(\d+) ticks=(\d+) "
                     r"switches=(\d+) proc=0x([0-9A-Fa-f]+) critical=(\d+) slice=(\d+) cpu_permille=(\d+)")
ROWS_RE = re.compile(r"\[UI\] tmgr proc rows=(\d+) total=(\d+) procs=(\d+) tasks=(\d+)")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Monitor:
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

    def keys(self, seq, wait=0.22):
        for k in seq:
            self.key(k, wait=wait)


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_for(path, needle, timeout, proc=None, since=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        s = slog(path)
        if needle in s[since:]:
            return s
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return slog(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
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

    tmp = tempfile.mkdtemp(prefix="vimtu64_tmgr64_")
    serial = os.path.join(tmp, "tmgr64.log")
    port = free_port()
    args_q = [qemu, "-name", "Vimtu64-tmgr64", "-drive", "format=raw,file=%s" % p64.q(img),
              "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
              "-serial", "file:%s" % p64.q(serial),
              "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"]
    proc = subprocess.Popen(args_q, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    try:
        qh.login_desktop(Monitor(port), serial, proc, timeout=180)
        wait_for(serial, "[PRELOAD64] glyphs prewarmed=", 60, proc)      # 预热跑完再动键盘（注入更稳）
        time.sleep(1.5)
        mon = Monitor(port)

        # ---- 第 0 步：先开终端（与 tmgr_proc_test 同一套按键顺序：先终端后任务管理器，注入最稳）----
        print("--- 第 0 步：开终端 + `ps diag` 取内核任务表快照 ---")
        for _attempt in range(4):
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if "[APP] term opened" in slog(serial):
                break
        log = wait_for(serial, "[APP] term opened", 20, proc)
        check("开始菜单 -> 终端（[APP] term opened）", "[APP] term opened" in log)
        mon.keys(PS_DIAG_KEYS)
        log = wait_for(serial, "[TASK64] diag tasks=", 25, proc)
        check("终端 ps diag 打出内核任务表（[TASK64] diag slot=..）",
              "[TASK64] diag tasks=" in log)

        # ---- ① 纯桌面（还没有任何用户进程）打开任务管理器：列表必须非空 ----
        print("--- ① 纯桌面：进程页要列出真实内核任务（修之前这里是空的） ---")
        for _attempt in range(5):
            mon.key("meta_l", wait=1.0)
            mon.key("7", wait=3.0)
            if "[APP] tmgr opened" in slog(serial):
                break
            time.sleep(1.0)
        log = wait_for(serial, "[UI] tmgr proc rows=", 30, proc)
        check("任务管理器打开（[APP] tmgr opened）", "[APP] tmgr opened" in log)
        check("原有打点仍在：[UI] tmgr page proc", "[UI] tmgr page proc" in log)
        # 取"打开之后的第一份清单快照"（= 纯桌面那一拍：终端/用户进程还没起）
        # —— 清单每 2 秒会重打一次，所以只取第一条 rows= 行到第二条之间那一段。
        seg = log[log.find("[APP] tmgr opened"):] if "[APP] tmgr opened" in log else log
        m_all = list(ROWS_RE.finditer(seg))
        if len(m_all) >= 2:
            seg = seg[m_all[0].start():m_all[1].start()]
        mr = ROWS_RE.search(seg)
        check("清单打点 [UI] tmgr proc rows=N total=T procs=P tasks=K", bool(mr),
              mr.group(0) if mr else "")
        rows_n = int(mr.group(1)) if mr else 0
        procs_n = int(mr.group(3)) if mr else -1
        tasks_n = int(mr.group(4)) if mr else -1
        check("★ 纯桌面列表非空（rows>=1）且含内核任务行（tasks>=1）",
              rows_n >= 1 and tasks_n >= 1,
              "rows=%d procs=%d tasks=%d" % (rows_n, procs_n, tasks_n))
        check("纯桌面没有 proc64 进程（procs=0，符合预期）", procs_n == 0, "procs=%d" % procs_n)
        trows = ROW_RE.findall(seg)
        check("有 [UI] tmgr task row id=.. name=.. state=.. ticks=.. switches=.. 行", len(trows) >= 1,
              (" | ".join(" ".join(x) for x in trows[:3])) if trows else "")
        check("task 行数 == 清单里的 tasks=K（一份快照里的行数与总数一致）", len(trows) == tasks_n,
              "行数=%d tasks=%d" % (len(trows), tasks_n))

        # ---- ⑥（穿插）：结束内核任务这条线接通了 —— idle/桌面必须被护栏拒绝 ----
        print("--- ⑥ 结束任务路径：选中 idle/桌面 -> 回车 -> 护栏拒绝（rc=-1，不崩） ---")
        mon.key("down", wait=0.8)
        mon.key("ret", wait=1.2)
        log = wait_for(serial, "[UI] tmgr kill task", 8, proc)
        kt = re.findall(r"\[UI\] tmgr kill task id=(\d+) name=(\S+) rc=(-?\d+)", log)
        check("结束内核任务走 task_kill64 并打点 [UI] tmgr kill task id=.. rc=..", bool(kt),
              (" | ".join(" ".join(x) for x in kt[-2:])) if kt else "")
        check("选中 idle（任务 0）时被护栏拒绝：rc=-1（不是崩，也不是假成功）",
              bool(kt) and kt[-1][2] == "-1", (" | ".join(" ".join(x) for x in kt[-1:])) if kt else "")
        check("拒绝后有明确提示（无 PANIC）", "PANIC" not in slog(serial))

        # ---- ② 与内核 ps 逐项一致（用第 0 步那一份 [TASK64] diag 快照） ----
        print("--- ② 列表条目 == 内核 ps（`ps diag` 的 [TASK64] diag 行） ---")
        diags = DIAG_RE.findall(slog(serial))
        check("终端 ps diag 打出内核任务表（[TASK64] diag slot=..）", len(diags) >= 1,
              "%d 行" % len(diags))
        check("原有打点仍在：[TASK] ps rows=", "[TASK] ps rows=" in slog(serial))
        # 逐项比对：tmgr 的 task 行必须能在 diag 里找到同 (slot,id,name,state) 的一行
        dmap = {(d[0], d[1], d[2], d[3]): d for d in diags}
        matched, unmatched = 0, []
        for r in trows:
            key4 = (r[6], r[0], r[1], r[2])       # (slot, id, name, state)
            if key4 in dmap:
                matched += 1
            else:
                unmatched.append(key4)
        check("★ 任务管理器的 task 行与 [TASK64] diag 逐项一致（slot/id/name/state）",
              matched >= 1 and not unmatched,
              "匹配 %d 行，未匹配 %s" % (matched, unmatched[:3]))
        if trows and dmap:
            r0 = trows[0]
            d0 = dmap.get((r0[6], r0[0], r0[1], r0[2]))
            if d0:
                # diag 快照取在第 0 步、tmgr 快照更晚 -> 累计值只会更大/相等（时间在走）
                check("tick/switches 单调可比（tmgr 快照晚于 ps 快照，累计值 >= 且差值有界）",
                      int(r0[3]) >= int(d0[4]) and int(r0[4]) >= int(d0[5]) and
                      (int(r0[3]) - int(d0[4])) <= 250 * 120 and
                      int(r0[5]) <= 1000 and int(d0[9]) <= 1000,
                      "tmgr tick=%s sw=%s cpu=%s / diag tick=%s sw=%s cpu=%s"
                      % (r0[3], r0[4], r0[5], d0[4], d0[5], d0[9]))

        # ---- ③ 刷新：终端起一个真进程，任务管理器自己会把新行打出来 ----
        print("--- ③ 刷新：终端 proc run spin -> 任务管理器（未交互）出现新进程行 ---")
        mark = len(slog(serial))
        nterm = slog(serial).count("[APP] term opened")
        for _attempt in range(4):                    # 切到终端（已有实例时新开一个，焦点随之转移）
            mon.key("meta_l", wait=1.0)
            mon.key("1", wait=2.0)
            if slog(serial).count("[APP] term opened") > nterm:
                break
        for k in RUN_CMD_KEYS:
            mon.key(k, wait=0.22)
        log = wait_for(serial, "[TERM] proc run path=/spin.elf", 25, proc)
        runs = re.findall(r"\[TERM\] proc run path=/spin\.elf pid=(\d+)", log)
        if not runs:
            mon.keys(RUN_CMD_KEYS)
            log = wait_for(serial, "[TERM] proc run path=/spin.elf", 20, proc)
            runs = re.findall(r"\[TERM\] proc run path=/spin\.elf pid=(\d+)", log)
        check("终端 proc run spin 起了一个真进程（[TERM] proc run path=/spin.elf）",
              "[TERM] proc run path=/spin.elf" in log, "runs=%s" % runs)
        pid = runs[-1] if runs else None
        check("用户程序真的在 ring3 跑（spin64: alive pid=）", "spin64: alive pid=" in log)
        # ★ 关键：不碰任务管理器，等它自己刷新出 procs=1 的行
        log = wait_for(serial, "procs=1", 12, proc, since=mark)
        fresh = [m for m in ROWS_RE.finditer(log[mark:]) if m.group(3) == "1"]
        check("★ 未与任务管理器交互，列表自动刷新出新进程（[UI] tmgr proc rows=.. procs=1）",
              bool(fresh), (fresh[-1].group(0) if fresh else "（12s 内没有刷新出 procs=1）"))
        prows = [p for p in PROC_RE.findall(log[mark:]) if pid is None or p[0] == pid]
        check("刷新后的进程行 = 刚起的 spin（pid 一致、name=spin、ppid=0、CR3 非 0/非 0x40000）",
              bool(prows) and prows[-1][2] == "spin" and prows[-1][1] == "0" and
              int(prows[-1][4], 16) not in (0, 0x40000),
              (" ".join(prows[-1])) if prows else "")

        # ---- ④ 结束运行中的程序（新开一个任务管理器实例，保证键盘焦点在它身上） ----
        print("--- ④ 选中并结束该进程：rc=0 + [PROC64] exit code=137 ---")
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("7", wait=2.5)
            if slog(serial).count("[APP] tmgr opened") >= 2:
                break
        wait_for(serial, "[UI] tmgr proc rows=", 20, proc)
        ok_kills, esrch_kills, others = [], [], []
        for _attempt in range(4):
            mon.key("down", wait=0.8)
            mon.key("ret", wait=1.3)
            log = slog(serial)
            hits = re.findall(r"\[UI\] tmgr kill proc pid=(\d+) name=\S+ sig=9 rc=(-?\d+)", log)
            hits = [h for h in hits if pid is None or h[0] == pid] or hits
            ok_kills = [h for h in hits if h[1] == "0"]
            esrch_kills = [h for h in hits if h[1] == "-3"]
            others = [h for h in hits if h[1] not in ("0", "-3")]
            if ok_kills:
                break
        exit_line = ("[PROC64] exit pid=%s code=137 cr3_released=1" % pid) if pid else None
        exit_ok = bool(exit_line) and (exit_line in slog(serial))
        check("[UI] tmgr kill proc pid=%s … sig=9 rc=0（真杀）" % pid, bool(ok_kills),
              (" | ".join(" ".join(h) for h in ok_kills)) if ok_kills else "（没有 rc=0 的 kill 行）")
        check("[PROC64] kill pid=%s sig=9（真走 proc64 kill 路径）" % pid,
              (not pid) or ("[PROC64] kill pid=%s sig=9" % pid) in slog(serial))
        check("进程真的没了：[PROC64] exit pid=%s code=137 cr3_released=1（SIGKILL）" % pid,
              exit_ok, exit_line or "")
        check("其它错误码一个都没有（只允许 0 / -3）", not others,
              " | ".join(" ".join(h) for h in others) or "（无）")

        # ---- ⑤ 重复结束已退出的 pid：只提示，不崩 ----
        print("--- ⑤ 重复结束已退出的目标：明确提示 + 不 PANIC ---")
        mon.key("ret", wait=1.3)
        log = slog(serial)
        hits = re.findall(r"\[UI\] tmgr kill proc pid=(\d+) name=\S+ sig=9 rc=(-?\d+)", log)
        hits = [h for h in hits if pid is None or h[0] == pid] or hits
        rep = [h for h in hits if h[1] != "0"]
        check("重复结束已退出 pid 时如实回 -ESRCH(-3) 并给提示（不是崩也不是假成功）",
              bool(rep) and all(h[1] == "-3" for h in rep),
              (" | ".join(" ".join(h) for h in rep[-2:])) if rep else "（没有重复命中）")
        for needle in FORBIDDEN:
            check("不得出现 %s" % needle, needle not in slog(serial))

        # ---- ⑦ 结束之后列表也确实刷新了（该 pid 不再有活状态：EXITED 僵尸行是 proc64 的正常语义） ----
        print("--- ⑦ 结束之后列表刷新（该 pid 在清单里不再出现活状态） ---")
        wait_for(serial, "procs=", 8, proc, since=len(slog(serial)) - 2000)
        time.sleep(3.0)                                  # 让清单再重打一拍（2s 节流 + 500ms 重画）
        log = slog(serial)
        last_rows = list(ROWS_RE.finditer(log))[-1].group(0) if ROWS_RE.search(log) else ""
        tail = log[log.rfind("[UI] tmgr kill proc"):] if "[UI] tmgr kill proc" in log else log
        live = [p for p in PROC_RE.findall(tail)
                if (pid is None or p[0] == pid) and 1 <= int(p[3]) <= 3]
        check("★ 被结束的进程不再以活状态出现在清单里（结束之后 state 1..3 的行没了）", not live,
              "活行=%s / 末条清单=%s" % (live[-1] if live else "（无）", last_rows))
        check("清单行本身仍在刷新（末条 procs=.. 是结束后的那一拍）", bool(last_rows), last_rows)

        print("--- serial tail ---")
        for line in [x for x in slog(serial).splitlines() if x.strip()][-12:]:
            print("   | " + line[:170])
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    if args.keep:
        print("[tmgr64] 串口日志：%s" % serial)
    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
