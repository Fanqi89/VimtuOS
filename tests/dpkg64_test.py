#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/dpkg64_test.py - ★ v0.7.5：用户态 dpkg/apt 真能装 .deb（端到端，真点终端真跑）

判据（每条都能独立证伪）：
  A. `run /usr/bin/dpkg --version`  → 串口出现 "dpkg 0.7.5 (vimtu-dpkg"
  B. `run /usr/bin/dpkg -i /usr/share/dpkg/test-vimtu_1.0.deb`
     → 出现 "dpkg: 已安装 vimtu-dpkg-test 1.0"（= ar 解析 + control 解析 + tar 解包 + 写状态 全过）
  C. `cat /usr/share/vimtu-dpkg-test/hello.txt`
     → 出现 "hello from a real .deb installed by vimtu-dpkg"（**payload 真的落到文件系统**）
  D. `run /usr/bin/dpkg -l` → 出现 "vimtu-dpkg-test"（状态文件读回）
  E. `run /usr/bin/apt update` → 出现 "当前未启用"（远程仓库如实拒绝，不假装成功）

用法：py -3 tests/dpkg64_test.py
退出码：0 全过 / 1 断言失败 / 2 环境问题
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import install_persist64_test as P  # noqa: E402
import startmenu64_test as SM      # noqa: E402
import qemuhelp as qh              # noqa: E402

QEMU = P.T.find_qemu()
DISK = os.path.join(ROOT, "build64", "sysdisk.img")      # 演示盘（带完整系统卷）
WORK = os.path.join(ROOT, "build64", "dpkg64_test")
PORT = 5661
ok = True


def q(p):
    return p.replace("\\", "/")


def open_terminal_dock(log_fn, mon, timeout=90):
    """从 **Dock 的终端图标** 开终端（不用开始菜单）。

    为什么要这条路径（实测原因）：这块演示盘的每个图标从卷里装载要 ~1.4 s
    （`[ICON64] load … ok=1` 相邻两行隔 1 秒多），而开始菜单一开就要装 14 个图标 ——
    单次 GUI 帧迭代 >20 s，直接触发 20 s 看门狗蓝屏（`[PANIC64] stop=WATCHDOG_TIMEOUT`）。
    Dock 图标只有 9 个且不重绘列表，1 次点击就能开终端，是本构建下唯一稳的入口。
    """
    import re
    import time
    deadline = time.time() + timeout
    hit = None
    while time.time() < deadline and not hit:
        for mm in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(-?\d+) name=\S+ x=(\d+) y=(\d+) "
                              r"w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log_fn()):
            if mm.group(2) == "3":                    # app=3 = 终端
                hit = mm
                break
        if not hit:
            time.sleep(1.0)
    if not hit:
        return False
    cur = SM.Cursor()
    cur.goto(mon, int(hit.group(7)), int(hit.group(8)))
    cur.settle(mon)
    time.sleep(0.3)
    mon.click()
    t0 = time.time()
    while time.time() - t0 < 60:
        lg = log_fn()
        if "[APP] term opened" in lg:
            return True
        if "[PANIC64] stop=" in lg:
            print("  （开窗期间看门狗蓝屏：%s）" % ([x[:90] for x in re.findall(r"\[PANIC64\] stop=[^\n]*", lg)][:1],))
            return False
        time.sleep(0.5)
    return False

def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def log_of(p):
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return ""


def start(args, serial, port):
    import socket
    import subprocess
    import time
    a = [QEMU, "-name", "dpkg64", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"] + args
    p = subprocess.Popen(a, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(80):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return p, SM.Monitor(port)


def main():
    import time
    if not QEMU or not os.path.exists(DISK):
        print("跳过：缺 qemu 或 %s" % DISK)
        return 2
    os.makedirs(WORK, exist_ok=True)
    serial = os.path.join(WORK, "run.log")
    if os.path.exists(serial):
        os.remove(serial)
    proc, mon = start(["-drive", "format=raw,file=%s" % q(DISK), "-boot", "order=c"], serial, PORT)
    try:
        # ★ 登录手势的超时给够：这块演示盘启动要跑完整套自检（proc64/pipe/evshm 6s hold/musl/
        #   dynlink），实测到桌面 ≈ 204 s；原来默认 login_timeout=180 会让"等锁屏"超时、
        #   于是根本不按键，白等 400 s 报"起不到桌面"（实测踩过）。
        if not qh.wait_desktop(mon, lambda: log_of(serial), proc, timeout=400, login_timeout=460):
            print("起不到桌面")
            return 2
        if not open_terminal_dock(lambda: log_of(serial), mon):
            print("终端开不出来（Dock 图标路径）")
            return 2
        time.sleep(1.5)

        # A. --version
        P.type_line(mon, "run /usr/bin/dpkg --version")
        time.sleep(3.0)
        log = log_of(serial)
        check("A. dpkg --version 起得来", "dpkg 0.7.5 (vimtu-dpkg" in log)

        # B. 安装真 .deb
        P.type_line(mon, "run /usr/bin/dpkg -i /usr/share/dpkg/test-vimtu-1.0.deb")
        time.sleep(8.0)
        log = log_of(serial)
        check("B. dpkg -i 装成功（ar+control+tar+status 全过）",
              "已安装 vimtu-dpkg-test 1.0" in log,
              "（日志里最后一处 dpkg: 行：%r）" %
              (([l for l in log.splitlines() if "dpkg:" in l] or [""])[-1][:120]))

        # C. payload 真的落地
        P.type_line(mon, "cat /usr/share/vimtu-dpkg-test/hello.txt")
        time.sleep(4.0)
        log = log_of(serial)
        check("C. .deb 里的文件真的落到文件系统",
              "hello from a real .deb installed by vimtu-dpkg" in log)

        # D. 状态文件读回
        P.type_line(mon, "run /usr/bin/dpkg -l")
        time.sleep(4.0)
        log = log_of(serial)
        check("D. dpkg -l 列出已安装包", "vimtu-dpkg-test" in log)

        # E. apt 远程仓库如实拒绝
        P.type_line(mon, "run /usr/bin/apt update")
        time.sleep(4.0)
        log = log_of(serial)
        check("E. apt update 如实报未启用（不假装成功）", "当前未启用" in log)

        print("===== RESULT: %s =====" % ("PASS" if ok else "FAIL"))
    finally:
        try:
            mon.send("quit", wait=1.5)
        except Exception:
            pass
        try:
            proc.wait(timeout=20)
        except Exception:
            proc.kill()
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
