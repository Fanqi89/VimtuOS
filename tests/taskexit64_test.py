#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/taskexit64_test.py - 任务 exit 不再跳飞（#UD rip=0x47 回归，v0.7.6）

真缺陷（v0.7.1 ~ v0.7.5 一直存在，v0.7.5 的发版检查才发现）：
  sysdisk.img（带系统卷）启动时，proc64 的 fork/execve 自检跑到子进程 exit 就
  `[PANIC] cpu exception 6 err=0 rip=0x000000000047 cs=0x8`。定位（指令级）：
  任务内核栈只有 16KB，SYSCALL 处理器可用深度 = 0x2000-0x1000-0xD0 = 0xF30（≈3.9KB），
  而 execve -> VFS -> 磁盘读 实测压到 stack_top-0x22A0（处理器侧 4.5KB），把
  "从 ring3 回来"的返回地址槽盖成 0x40（gdb 写观察点抓到 dev_read_at 的序言 push
  r15=0x20/r14=0/r13=0x20/r12=0/rbx=0x40 与 ata_wait_* 的 pushf/push rbp 落在该槽）。
  修法：① 任务栈 16KB -> 32KB、入口预留 0x2000 -> 0x4000；
        ② 蹦床改成 jmp 到**静态**保存区里的返回地址（不再从内核栈 ret）；
        ③ 退出前打一行 `[USER64] exit resume rsp=.. ret=.. verdict=ok|bad` 核对。

判据（每条都能独立证伪）：
  A. 启动日志里**没有** `[PANIC] cpu exception`（v0.7.5 有两条 6 号异常）
  B. 几何真的改了：`[TASK64] init ... stack_bytes=32768`
  C. 子进程 exit 干净：出现 `[PROC64] exit pid=4`，且**没有** `[PROC64] wait4 TIMEOUT`
  D. 父进程 wait4 拿回子进程号：`proc64: wait4 pid=4`
     （v0.7.5 实测是 `pid=4294967285` = -11 EAGAIN：子任务被 #UD 打死、父进程等不到）
  E. 蹦床核对全 ok：至少一条 `[USER64] exit resume rsp=0x... ret=0xffffffff...`，
     且**没有** `verdict=bad`
  F. 带卷启动真的能到桌面：`[GUI64] ready`

用法：py -3 tests/taskexit64_test.py
退出码：0 全过 / 1 断言失败 / 2 环境问题
"""
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import install_persist64_test as P  # noqa: E402  (只借它的 find_qemu，避免复制一份)
import qemuhelp as qh              # noqa: E402

QEMU = P.T.find_qemu()
DISK = os.path.join(ROOT, "build64", "sysdisk.img")     # 演示盘（带完整系统卷）
WORK = os.path.join(ROOT, "build64", "taskexit64_test")
ok = True


def q(p):
    return p.replace("\\", "/")


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def log_of(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def main():
    if not QEMU or not os.path.exists(DISK):
        print("跳过：缺 qemu 或 %s" % DISK)
        return 2
    os.makedirs(WORK, exist_ok=True)
    serial = os.path.join(WORK, "run.log")
    if os.path.exists(serial):
        os.remove(serial)
    port = qh.free_port()
    args = [QEMU, "-name", "taskexit64", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot",
            "-drive", "format=raw,file=%s" % q(DISK), "-boot", "order=c"]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = qh.Monitor(port)
    try:
        # ★ 这块演示盘启动要跑完整套自检（proc64 / pipe / evshm 6s hold / musl / dynlink），
        #   实测到锁屏 > 180 s —— 登录手势的超时必须给够，否则会"不按键白等"。
        t0 = time.time()
        desk = qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=300, login_timeout=420)
        print("boot+login waited %.1fs" % (time.time() - t0), flush=True)
        log = log_of(serial)
        print("---- 关键行 ----", flush=True)
        for pat in (r"\[TASK64\] init .*",
                    r"\[PROC64\] (create pid=4|fork parent=3 child=4|exit pid=4|wait4 TIMEOUT pid=4).*",
                    r"proc64: wait4 pid=\S+ status=\S+",
                    r"\[USER64\] exit resume .*",
                    r"\[PANIC\] cpu exception .*"):
            for m in re.findall(pat, log):
                print("   ", m, flush=True)
        print("----------------", flush=True)

        check("A. 没有内核异常（[PANIC] cpu exception）",
              "[PANIC] cpu exception" not in log)
        check("B. 任务内核栈几何 = 32KB",
              re.search(r"\[TASK64\] init .*stack_bytes=32768", log) is not None,
              (re.search(r"\[TASK64\] init [^\n]*", log) or [""])[0][:100]
              if re.search(r"\[TASK64\] init [^\n]*", log) else "")
        check("C. 子进程 exit 干净（无 wait4 TIMEOUT）",
              "[PROC64] exit pid=4" in log and "[PROC64] wait4 TIMEOUT" not in log)
        # ★ 用户可见的那行会被内核/演示程序的日志**行间插花**（write 的每一小段之间都可能插进
        #   一行 `[SYSCALL] …` / `[TASK64] reap …`，实测就是这样），所以不可能连续匹配。
        #   口径：把"以 [ 开头的日志段"整段剥掉（那是内核/演示打的，不是用户输出），
        #   再按空白/换行容忍匹配 —— 只放宽排版，不放宽内容（pid 必须 4、status 必须 0）。
        plain = re.sub(r"\[[^\n]*", "", log)
        by_user = re.search(r"proc64: wait4 pid=\s*4\s*status=\s*0", plain) is not None
        by_kernel = "[PROC64] wait4 pid=4 status=0" in log
        check("D. 父进程 wait4 拿回子进程号 4（status=0）",
              by_user and by_kernel and "[PROC64] wait4 TIMEOUT" not in log,
              "用户可见行=%s 内核行=%s" % (by_user, by_kernel))
        res = re.findall(r"\[USER64\] exit resume rsp=0x([0-9A-F]+) ret=0x([0-9A-F]+) verdict=(\w+)", log)
        good = [r for r in res if r[2] == "ok" and r[1].startswith("FFFFFFFF")]
        check("E. 蹦床核对：返回地址来自静态区且是内核代码地址",
              len(good) >= 1 and len(good) == len(res),
              "命中 %d/%d 条（示例 %s）" % (len(good), len(res), res[:2]))
        check("F. 带卷启动到桌面（[GUI64] ready）", desk)
    finally:
        try:
            mon.send("quit", wait=1.0)
        except Exception:
            pass
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()
    print("===== RESULT: %s =====" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
