# -*- coding: utf-8 -*-
"""tests/sigpipe64_test.py —— v0.4.9：管道断开时**真的投递 SIGPIPE**（端到端，不再 UNVERIFIED）。

v0.4.8 的教训（写在这里免得后人再踩）：
  旧版本想用 `busybox yes | head -1` 造"读者提前退出"，但夹具用的是 `build64/system.img`
  —— 那是**没有 VimtuFS2 卷**的裸载荷（卷是安装器建在数据分区上的），所以盘上没有
  `/bin/shell.bin`、`sh` 拉不起 ring3 shell，`yes|head` 这条管线根本跑不出来，只能报
  UNVERIFIED。其实"读者提前退出"这件事**单进程就能做**：自己关掉自己的读端再写。
  v0.4.9 因此带了一个专门的 ring3 程序 `/epipe.elf`（user/apps/epipe.c）。

本版夹具 = `build64/sysdisk.img`（system.img + VimtuFS2 主分区卷）的副本，终端里
`run /epipe.elf`（`run` 是终端内建命令，不需要 shell）。

判据（端到端，不看代码自述）：
  1) 程序真跑起来并建成管道：`epipe: pipe rc=0 rfd=<a> wfd=<b>`
  2) **基线对照**：读者在场时同一份写路径不投递信号 —— `epipe: baseline write=2 read=2 data=ok`
  3) SIG_IGN 时只回错误不杀进程：`epipe: ign close=0 write=-32 (expect -32) alive=1`
  4) 内核在读端全关的写路径上置了未决位：`[SIG64] raise pid=<n> sig=13 src=pipe-epipe`
  5) 走的是**默认动作**：`[SIG64] default action pid=<n> sig=13 exit=141`（128+13），且两个
     pid 必须是同一个（是写者进程自己被杀，不是别的东西）
  6) **反相对照**（让判据有牙齿）：`epipe: SURVIVED` 一次都不许出现 —— 出现了就说明第 5 条
     无论如何都会"通过"，那它就不是判据
  7) 顺带定性 v0.4.8 遗留的疑点：**带卷的盘上** `shell` 能拉起 ring3 shell
     （`[SH64] launch path=/bin/shell.bin size=…`）—— 之前"TTY 里敲 sh 没反应"是夹具没卷，不是内核缺功能
  8) 全程无 PANIC / TRIPLE FAULT
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


# QEMU sendkey 的名字不是字符本身：space -> spc、'-' -> minus、'|' -> shift-backslash
KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
          "|": "shift-backslash", "'": "apostrophe", "=": "equal"}


def type_line(mon, s, per=0.08):
    for ch in s:
        mon.key(KEYMAP.get(ch, ch), wait=per)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK, help="带 VimtuFS2 卷的盘（默认 build64/sysdisk.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5672)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）—— 先跑 bash build64.sh" % args.img)
        return 2

    print("=== sigpipe64_test：管道断开 -> SIGPIPE 真投递（单进程自证 + ring3 shell 定性） ===")
    vm = T.Vm(qemu, args.img, args.port, "sigpipe")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏", False)
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 1
        time.sleep(2.0)
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        check("开场终端（[APP] term opened）", vm.wait_log("[APP] term opened", 25))
        time.sleep(1.2)

        # ---------- 0) 起 ring3 shell（外部程序的**真进程**通道：fork+execve+wait4） ----------
        # 为什么必须走 shell 的 run：终端**内建** `run` 走 kernel/elf64.cpp 的 user64_enter_at64 ——
        #   那是"当前任务直接进 ring3"的同步模型，**不建 Proc64**，于是 p64_current64()==nullptr：
        #   fd 表退回内核表（pipe 能用），但**进程根本没有信号状态**（sig64 的 pending 位无处可置）。
        #   实测（v0.4.9 诊断，串口片段 epipe_serial.txt）：终端里 `run /epipe.elf` 打出 SURVIVED，
        #   整个会话一条 [SIG64] raise / [SIG64] action 都没有。
        #   shell 的 `run PATH` 是 fork(57)+execve(59)+wait4(61) 真进程（user/shell/main.c），
        #   子进程有独立 Proc64 -> 信号状态存在 -> SIGPIPE 才有地方落下。
        #   （终端内建 run 缺进程上下文这条作为**已知边界**记进 README，不在本测试里假装通过。）
        n_shell = len(vm.log())
        type_line(mon, "shell")
        mon.key("ret", wait=2.0)
        got_sh = vm.wait_log("[SH64] launch", 60, since=n_shell)
        check("起 ring3 shell（[SH64] launch path=/bin/shell.bin）", got_sh,
              (re.search(r"\[SH64\] launch[^\r\n]*", vm.log()[n_shell:]).group(0) if got_sh else "（缺）"))
        if not got_sh:
            print("\n=== RESULT: UNVERIFIED ===（环境：ring3 shell 起不来，无法用真进程造 EPIPE 现场）")
            return 2
        time.sleep(2.5)

        # ---------- 1) 在 shell 里跑 /epipe.elf：单进程造 EPIPE 现场 ----------
        n0 = len(vm.log())
        type_line(mon, "run /epipe.elf")
        mon.key("ret", wait=2.0)
        # epipe 装载 + 三段输出 + 被 SIGPIPE 杀掉，给它 60 秒（ELF64 装载在本内核要数秒）
        got_raise = got_default = False
        pid_raise = pid_default = None
        t0 = time.time()
        while time.time() - t0 < 60:
            log = vm.log()[n0:]
            m1 = re.search(r"\[SIG64\] raise pid=(\d+) sig=13 src=pipe-epipe", log)
            if m1:
                got_raise = True
                pid_raise = m1.group(1)
            m2 = re.search(r"\[SIG64\] default action pid=(\d+) sig=13 exit=141", log)
            if m2:
                got_default = True
                pid_default = m2.group(1)
            if got_raise and got_default:
                break
            time.sleep(0.5)
        log = vm.log()[n0:]

        m_pipe = re.search(r"epipe: pipe rc=0 rfd=(\d+) wfd=(\d+)", log)
        check("ring3 程序真跑起来并建成管道（epipe: pipe rc=0 rfd=… wfd=…）", bool(m_pipe),
              m_pipe.group(0) if m_pipe else "（缺：/epipe.elf 没跑起来或 pipe(22) 失败）")
        check("基线对照：读者在场时写/读都成功（同一写路径不投递信号）",
              bool(re.search(r"epipe: baseline write=2 read=2 data=ok", log)),
              (re.search(r"epipe: baseline[^\r\n]*", log).group(0) if "epipe: baseline" in log else "（缺）"))
        check("SIG_IGN：读端全关后写返回 -EPIPE(32) 且进程存活",
              bool(re.search(r"epipe: ign close=0 write=-32 \(expect -32\) alive=1", log)),
              (re.search(r"epipe: ign[^\r\n]*", log).group(0) if "epipe: ign" in log else "（缺）"))
        check("内核在读端全关的写路径上置了 SIGPIPE 未决位（[SIG64] raise … src=pipe-epipe）",
              got_raise, (re.search(r"\[SIG64\] raise[^\r\n]*", log).group(0) if got_raise else "（缺）"))
        check("按**默认动作**终止（[SIG64] default action … sig=13 exit=141 = 128+13）",
              got_default, (re.search(r"\[SIG64\] default action[^\r\n]*", log).group(0) if got_default else "（缺）"))
        check("raise 与 default action 是**同一个** pid（被杀的就是写者自己）",
              bool(pid_raise and pid_default and pid_raise == pid_default),
              "raise=%s default=%s" % (pid_raise, pid_default))
        # ★ 反相对照：这条一旦为真，上面"按默认动作终止"就失去意义
        check("反相对照：epipe: SURVIVED **不出现**（出现即说明 SIGPIPE 没有被投递）",
              "epipe: SURVIVED" not in vm.log(),
              "SURVIVED 命中 0 次" if "epipe: SURVIVED" not in vm.log() else "命中！")

        # ---------- 3) 旁证：shell 自己没被误杀（被杀的是 epipe 那个子进程） ----------
        n2 = len(vm.log())
        type_line(mon, "echo still-alive")
        mon.key("ret", wait=1.5)
        check("shell 自身未被误杀（后续命令仍执行）", vm.wait_log("still-alive", 20, since=n2),
              (re.search(r"still-alive[^\r\n]*", vm.log()[n2:]).group(0)
               if "still-alive" in vm.log()[n2:] else "（缺）"))

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d" % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
