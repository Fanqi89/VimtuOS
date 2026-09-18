#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/user64_test.py - 用户态（ring3）+ int 0x80 系统调用自动验收

做什么：无头启动**已安装系统**镜像（build64/system.img），读串口日志，断言
        "内核进 ring3 跑真实用户程序 -> 用户程序用 int 0x80 打日志/取 pid/取 ticks/exit
         -> 回到 ring0 后继续启动桌面" 这条链路真的走通了。

断言（与 kernel/usermode64.cpp、kernel/syscall64.cpp 的打点严格对应，改格式必须同步）：
  [USER64] map code=... stack=... code_pages=.. stack_pages=..   用户页已映射
  [USER64] enter ring3 entry=... rsp=...                         真的 iretq 进 ring3
  hello from ring3 (VimtuOS user mode)                           用户程序 write(1,...) 的输出
  [SYSCALL] nr=1 ...                                              write 系统调用被分发
  [SYSCALL] nr=2 ...                                              exit 系统调用被分发
  [USER64] back to kernel (ring0)                                 exit 后回到内核
  [USER64] selftest PASS / [SYSCALL] selftest PASS                两侧自检
  [GUI64] ready                                                  回到 ring0 后桌面照常起来
禁止出现：PANIC / TRIPLE FAULT / selftest FAIL / FAILED mask= / [SYSCALL] deny

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\user64_test.py
    py -3 tests\\user64_test.py --img build64/system.img --timeout 90

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/镜像缺失）
"""
import argparse
import os
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

# (断言文本, 说明)；顺序即输出顺序
MUST = [
    ("[USER64] map code=",                          "用户页映射完成"),
    ("[USER64] enter ring3 entry=",                 "进入 ring3（iretq）"),
    ("hello from ring3 (VimtuOS user mode)",         "用户程序 write(1, ...) 的原文"),
    ("[SYSCALL] nr=1",                              "write 系统调用被分发"),
    ("pid=",                                        "getpid() 打点"),
    ("ticks=",                                      "ticks() 打点"),
    ("[SYSCALL] nr=2",                              "exit 系统调用被分发"),
    ("[USER64] back to kernel (ring0)",             "exit 后回到 ring0"),
    ("[USER64] selftest PASS",                      "用户态侧自检"),
    ("[SYSCALL] selftest PASS",                     "系统调用侧自检"),
    ("[GUI64] ready",                               "回到 ring0 后桌面照常起来"),
]

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "selftest FAIL",
    "FAILED mask=",
    "[SYSCALL] deny",
    "[USER64] run FAILED",
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"),
                    help="已安装系统镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=120, help="最长等待秒数")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s\n（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_user64_")
    serial = os.path.join(tmp, "serial.log")

    qemu_args = [
        qemu, "-name", "Vimtu64-user64",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
    ]
    print("=== Vimtu64 user64 acceptance ===")
    print("[user64] 引导 %s" % args.img)
    proc = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    early = False
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            if "[GUI64] ready" in slog():
                break
            if proc.poll() is not None:
                early = True
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    log = slog()
    checks = []
    ok = True

    def check(name, cond):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s" % ("PASS" if cond else "FAIL", name))

    print("--- 必须出现 ---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log)
    print("--- 禁止出现 ---")
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log)

    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    if args.keep:
        print("[user64] 串口日志：%s" % serial)

    print("--- serial tail（%d 条断言，失败 %d 条）---" % (len(checks), sum(1 for _, c in checks if not c)))
    for line in [l for l in log.splitlines() if l.strip()][-14:]:
        print("   | " + line[:180])
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
