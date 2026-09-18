#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/sched_stress_test.py - 调度器"创建 → 运行 → 退出 → 回收"压力验收

为什么要这个测试（它抓的是什么真缺陷）：
  调度器（kernel/task64.cpp）用中断帧当任务上下文，"任务自杀 / 别的任务回收它的栈 / 调度器
  切走"这三件事是**三个执行流交错**的地方。上游在一次 apic64 验收里偶发过一次：
      [TASK64] exit name=ksum id=3 ticks=1
      [PANIC] cpu exception 13 err=0000000000009834 rip=FFFFFFFF80168A72 ...
  0xFFFFFFFF80168A72 正是 kernel/task_switch_iret64 里那条 iretq —— 也就是"iretq 从一个已经
  不属于该任务（或被别的执行流覆盖）的帧里弹 cs/ss"。这种缺陷靠"偶尔跑一次启动"很难复现
  （上游重跑 3 次都没复现），所以内核里加了**默认启动就跑**的退出压力路径（内置线程 kstress，
  见 kernel/task64.cpp：反复创建/退出 200 个短命任务），本脚本读它的串口证据判定。

真根因（本测试就是围着它设计的）：回收路径 task_drain_reap 的临界区原来是可被 PIT 打断的
  （IF=1）。回收者可能在"判完 DEAD 状态"之后被切走；等它回来时那个槽**已经**被别的回收者处理
  掉、甚至**已被新任务复用** —— 过期的那半截于是会 kfree 掉**新任务正在使用的栈**
  （use-after-free）、重复 g_task_count-- / 把活任务的槽写成 FREE。栈被复用后，下一次切入
  那个任务的 iretq 就会弹到垃圾 cs/ss → 正是上面那条 #GP。

断言（全部基于串口日志原文 + 内核自己算的不变量）：
  1) 汇总行必须是 PASS 且自洽：
       [TASK64] stress spawn=<n> done=<n> reap=<n> live=<n> count=<n> fail=<mask> PASS
     n ≥ 100、fail=0、spawn == done == reap（创建 = 跑到退出 = 被回收）；
  2) 任务表不变量：live == count —— "任务表里被占用的槽位数"必须等于调度器自己的计数
     g_task_count。回收路径被抢占过（重复回收/双减计数/状态错写）就会不一致（fail bit4）；
  3) 不得出现 [TASK64] frameprobe FAIL / PANIC / TRIPLE FAULT / FAILED mask= /
     bad/double free / kfree: bad / 蹦床坏状态 —— 这些是帧错位与野指针的硬证据；
  4) 压力跑完系统仍然健康：[GUI64] ready + [TASK64] kheart beat= 持续增长（调度器没死）；
  5) 压力日志要"安静"：整份日志里 [TASK64] reap 行不超过 4 行。
     为什么查这个：压力子任务曾经每个都打 create/exit/reap 三行（200 个 = 600 行、约 200 行/秒），
     会把别的模块**不持行锁**的长行（例如 [STORE64] init 行）从中间插断，害得靠整行匹配的
     验收脚本（store64_test）假失败。安静 + 用计数不变量判定才是能长期留在回归里的做法。

用法（必须用 Windows 原生 Python；MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\sched_stress_test.py
    py -3 tests\\sched_stress_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 240

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import shutil
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

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")

STRESS_RX = re.compile(
    r"\[TASK64\] stress spawn=(\d+) done=(\d+) reap=(\d+) live=(\d+) count=(\d+) fail=(\d+) (PASS|FAIL)")
BEAT_RX = re.compile(r"\[TASK64\] kheart beat=(\d+)")
REAP_ANY_RX = re.compile(r"\[TASK64\] reap ")

FORBIDDEN = [
    ("[TASK64] frameprobe FAIL", "帧校验触发：待切换的目标帧不自洽（iretq 会弹垃圾 cs/ss）"),
    ("PANIC", "内核异常（cpu exception）"),
    ("TRIPLE FAULT", "三重故障"),
    ("FAILED mask=", "某项自检 FAIL"),
    ("bad/double free", "内核堆：野指针/二次释放"),
    ("kfree: bad", "内核堆：坏指针 kfree"),
    ("[TASK64] trampoline: bad task table state", "蹦床读到的任务表状态不对"),
]

MIN_SPAWN = 100          # 压力轮次的硬下限（内核默认 200）
MAX_REAP_LINES = 4       # 压力必须安静：正常情况下只有 ksum/kstress 的几行 reap


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


def q(p):
    return p.replace("\\", "/")


def boot_and_wait(qemu, img, serial, errfile, timeout):
    """无头启动系统内核；轮询串口日志到 [GUI64] ready（或超时/提前停住）。返回 (proc, slog, early)"""
    for p in (serial, errfile):
        if os.path.exists(p):
            os.remove(p)
    args = [
        qemu, "-name", "Vimtu64-sched-stress",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
    ]
    err = open(errfile, "wb")
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
    early = False
    last_len, last_change = 0, time.time()

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    deadline = time.time() + timeout
    while time.time() < deadline:
        txt = slog()
        if len(txt) != last_len:
            last_len, last_change = len(txt), time.time()
        # 必须两个都等到：桌面起来了 **并且** 压力轮次已经打完汇总行
        # （长命子任务会让压力比桌面启动更晚结束）
        if "[GUI64] ready" in txt and "[TASK64] stress" in txt:
            break
        if proc.poll() is not None:
            early = True
            break
        # 串口不再增长（halt / 挂死）：别白等到超时
        if txt and time.time() - last_change > 25.0:
            break
        time.sleep(0.25)

    return proc, slog, early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240, help="等待 [GUI64] ready 的最长秒数")
    ap.add_argument("--min-spawn", type=int, default=MIN_SPAWN)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_sched_stress_")
    serial = os.path.join(tmp, "stress.log")
    errfile = os.path.join(tmp, "stress.err")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 调度器压力验收（创建→运行→退出→回收）===")
    print("[sched_stress] 启动：%s" % args.img)
    proc, slog, early = boot_and_wait(qemu, args.img, serial, errfile, args.timeout)
    try:
        print("--- 1) 压力轮次完成（汇总行自洽）---")
        log = slog()
        m = STRESS_RX.search(log)
        check("[TASK64] stress 汇总行存在（压力路径真的跑了）", bool(m),
              m.group(0) if m else "没等到（压力路径没跑 / 启动挂了）")
        spawn = done = reap = live = count = failn = 0
        if m:
            spawn, done, reap, live, count, failn = (int(m.group(i)) for i in range(1, 7))
            check("spawn >= %d（压力够大）" % args.min_spawn, spawn >= args.min_spawn, "spawn=%d" % spawn)
            check("fail=0（无帧校验/无回收落后/无子任务丢失）", failn == 0, "fail=%d" % failn)
            check("spawn == done（每个子任务都走到了 task_exit64）", spawn == done,
                  "spawn=%d done=%d" % (spawn, done))
            check("spawn == reap（每个子任务的栈都被回收）", spawn == reap,
                  "spawn=%d reap=%d" % (spawn, reap))

        print("--- 2) 任务表不变量：占用槽位数 == g_task_count ---")
        check("live == count（回收路径被抢占过就会不一致）",
              spawn > 0 and live == count, "live=%d count=%d" % (live, count))

        print("--- 3) 禁止出现的硬错误 ---")
        for needle, what in FORBIDDEN:
            check("不得出现 %s（%s）" % (needle, what), needle not in log)
        if early:
            print("  [!] QEMU 提前退出（复位/三重故障？）")

        print("--- 4) 压力日志安静（不干扰别的模块不持锁的日志行）---")
        nreap = len(REAP_ANY_RX.findall(log))
        check("[TASK64] reap 行 <= %d（压力子任务必须安静）" % MAX_REAP_LINES,
              nreap <= MAX_REAP_LINES, "reap_lines=%d" % nreap)
        check("ksum 的一次性退出/回收仍照常打（对照基线）",
              "[TASK64] reap name=ksum" in log)

        print("--- 5) 压力跑完系统仍健康（桌面起来 + kheart 心跳在涨）---")
        b0 = [int(x) for x in BEAT_RX.findall(log)]
        time.sleep(4.0)
        log2 = slog()
        b1 = [int(x) for x in BEAT_RX.findall(log2)]
        check("[GUI64] ready（压力之后桌面仍能起来）", "[GUI64] ready" in log2)
        check("kheart beat 持续增长（调度器没被压力搞死）",
              bool(b0) and bool(b1) and b1[-1] > b0[-1],
              "before=%s after=%s" % (b0[-1] if b0 else "?", b1[-1] if b1 else "?"))
        check("压力跑完之后没有 PANIC（再等 4 秒复查）",
              "PANIC" not in log2 and "TRIPLE FAULT" not in log2)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("--- 串口证据（压力汇总行 + reap 行数）---")
    for ln in slog().splitlines():
        if "[TASK64] stress" in ln or "[TASK64] reap " in ln:
            print("   | " + ln.strip()[:150])

    if args.keep:
        print("[sched_stress] 串口日志：%s" % serial)
    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
