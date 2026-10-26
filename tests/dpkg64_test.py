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
import re
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

        # A0. 先提权：装包要写 /usr/share（root:0755），桌面会话是 uid=1000，
        #     内核的 P4 权限检查会**如实拒绝**非属主建目录 ——
        #     实测现场：[PERM64] deny op=mkdir path=/usr/share/vimtu-dpkg-test uid=1000 ... need=wx owner=0:0
        P.type_line(mon, "sudo -i")
        time.sleep(2.5)
        log = log_of(serial)
        check("A0. sudo -i 提权成功（装包要有 root 权限，这是 POSIX 语义不是缺陷）",
              "[TERM] cmd sudo ok" in log,
              "（找到：%r）" % ([x[:60] for x in log.splitlines() if "[TERM] cmd sudo" in x] or [""]))

        # A. --version
        P.type_line(mon, "run /usr/bin/dpkg --version")
        time.sleep(3.0)
        log = log_of(serial)
        check("A. dpkg --version 起得来", "dpkg 0.7.5 (vimtu-dpkg" in log)

        # B. 安装真 .deb
        #    ★ 判据用**纯 ASCII 的两条硬证据**：① 终端对这次 `run` 判 ok；② dpkg 这个进程 code=0 退出。
        #      （不拿 "已安装 …" 这类中文成功消息当判据：它会被内核日志插花，而且失败时消息完全不同。）
        before_ok = log.count("[TERM] cmd run ok")
        P.type_line(mon, "run /usr/bin/dpkg -i /usr/share/dpkg/test-vimtu-1.0.deb")
        # dpkg -i 实测要 ~25-30 s（本内核 pipe 只有 64 B，tar/gunzip 的 10 KB 填充走 160 次往返），
        # 终端 run 的上限是 120 s —— 所以这里**轮询**等证据行，别用固定 sleep（踩过：等 10 s 就判失败）。
        pid_i = None
        code0 = False
        deadline = time.time() + 150
        while time.time() < deadline:
            log = log_of(serial)
            seg_i = log.find("[TERM] run argv path=/usr/bin/dpkg argc=3")
            pids = re.findall(r"\[PROC64\] create pid=(\d+) name=run", log[seg_i:]) if seg_i >= 0 else []
            pid_i = pid_i or (pids[0] if pids else None)
            code0 = pid_i is not None and ("[PROC64] exit pid=%s code=0" % pid_i) in log
            if code0 and log.count("[TERM] cmd run ok") > before_ok:
                break
            time.sleep(3.0)
        log = log_of(serial)
        after_ok = log.count("[TERM] cmd run ok")
        check("B. dpkg -i 走完并成功退出（run ok + 进程 exit code=0）",
              after_ok > before_ok and code0,
              "run_ok %d->%d  dpkg pid=%s exit0=%s" % (before_ok, after_ok, pid_i, code0))

        # C. payload 真的落地（cat 的内容只进窗口，不进串口；用终端自己打的证据行判定）
        P.type_line(mon, "cat /usr/share/vimtu-dpkg-test/hello.txt")
        mc = None
        deadline = time.time() + 30
        while time.time() < deadline and mc is None:
            mc = re.search(r"\[TERM\] cmd cat path=/usr/share/vimtu-dpkg-test/hello\.txt bytes=(\d+)",
                           log_of(serial))
            if mc is None:
                time.sleep(1.0)
        check("C. .deb 里的文件真的落到文件系统（终端 cat 读到它）",
              mc is not None and int(mc.group(1)) > 0,
              "bytes=%s" % (mc.group(1) if mc else "缺证据行"))

        # D. 状态文件读回
        #    ★ 判据绑在**这次** `dpkg -l` 的切片上：它的输出走 fd 1 = 内核控制台（串口），
        #      所以"切片里出现包名 + 这次进程 exit 0"就是状态文件读回的真证据。
        #      （直接全日志 grep "vimtu-dpkg-test" 会被 cat 的命令行路径撞成假阳性 —— 踩过。）
        P.type_line(mon, "run /usr/bin/dpkg -l")
        dl = None
        deadline = time.time() + 60
        while time.time() < deadline and dl is None:
            lg = log_of(serial)
            seg_l = lg.find("[TERM] run argv path=/usr/bin/dpkg argc=2 a1=-l")
            if seg_l >= 0:
                ps = re.findall(r"\[PROC64\] create pid=(\d+) name=run", lg[seg_l:])
                if ps and ("[PROC64] exit pid=%s code=0" % ps[0]) in lg and \
                   "vimtu-dpkg-test" in lg[seg_l:]:
                    dl = ps[0]
            if dl is None:
                time.sleep(1.5)
        check("D. dpkg -l 从状态文件读回安装记录（切片内出现包名 + exit 0）", dl is not None,
              "pid=%s" % dl)
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
