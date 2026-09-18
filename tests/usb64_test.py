#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/usb64_test.py - VimtuOS USB 主机（UHCI + HID 引导键盘）端到端验收

做什么（QEMU BIOS 路径，启动**系统内核** build64/system.img）：
  1) 主跑 `-usb -device usb-kbd`（QEMU 的 piix3-usb-uhci + 全速 HID 引导键盘）：
       * PCI 找到主控：[USB64] uhci pci <bus>:<dev>.<fn> io=<hex> ports=2
       * 帧列表：[USB64] frame list @<hex> (1024 entries)
       * 端口复位：[USB64] port <n> connected speed=full reset ok
       * 设备描述符：[USB64] device addr=0 mps=8 vendor=0627 product=0001（QEMU usb-kbd）
       * 配置/引导协议：[USB64] config set value=1 ifaces=1 hid=1 ep_in=81 mps=8
                        [USB64] hid boot protocol set (8-byte reports)
       * 自检：[USB64] selftest PASS
       * 桌面照常：[GUI64] ready，且 kusb 线程不饿着桌面（[TASK64] kheart beat= 持续增长）
  2) ★ 端到端证据（最重要的那一条）：用 QEMU monitor 的 sendkey 按键，断言
       * [USB64] hid report key=E3 down=1（USB 中断端点真的收到了 HID 报告；
         只有插了 USB 键盘、且 UHCI 枚举/中断传输都通了才会有这一行）
       * 同一次按键之后桌面外壳真的响应：[UI] menu open（Win 键 = 开始菜单）
       * 继续按 '1'：key=1E -> [UI] menu activate idx=0 -> [APP] term opened（终端被打开）
       * 边沿检测：同一个键只产生一次 down=1 / down=0（不是每个报告都刷屏）
     ※ 实测结论（脚本里也断言了）：QEMU 11 的 monitor `sendkey` **会同时分发给 PS/2 键盘**
       （`help sendkey` 只有 keys + hold_ms，没有设备选择参数），所以 [UI] 那一行本身
       不能区分是 PS/2 还是 USB —— USB 的专属证据是 [USB64] hid report 行 + 同一轮里桌面
       的响应；降级跑（第 3 步）反过来证明没有 USB 键盘时**不会**出现这些行。
  3) 降级跑 `-usb`（有主控、没插键盘）：断言 [USB64] no device on port + selftest skipped，
     没有任何 [USB64] hid report 行；系统照常 [GUI64] ready，PS/2 键盘仍能打开开始菜单。
  4) 降级跑**不加 -usb**（机器上根本没有 UHCI 主控）：断言 [USB64] not found + skipped，
     系统照常启动、桌面照常起来（优雅降级，自检不算失败）。
  5) 每次启动都禁止：PANIC / TRIPLE FAULT / FAILED mask= / frameprobe FAIL / [USB64] selftest FAIL。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\usb64_test.py
    py -3 tests\\usb64_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 150

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import atexit
import os
import re
import shutil
import socket
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

FORBIDDEN_ALL = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "frameprobe FAIL",
                 "[USB64] selftest FAIL", "[USB64] enum FAILED"]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


def pick_port(start=5592, tries=12):
    """找一个空闲 TCP 端口（monitor 用 telnet 监听）。"""
    for p in range(start, start + tries):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            s.close()
    return start


class Monitor:
    """QEMU monitor 客户端（telnet 端口）：只用来发 sendkey。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)


def boot(qemu, img, extra_args, serial, errfile, port, wait_for, timeout):
    """无头启动；轮询串口日志到 wait_for（或超时/提前退出）。
    返回 (proc, slog, early)；调用方负责 kill（进程可能还活着）。"""
    if os.path.exists(serial):
        os.remove(serial)
    if os.path.exists(errfile):
        os.remove(errfile)
    args = [
        qemu, "-name", "Vimtu64-usb64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
        "-no-reboot",
    ]
    args += list(extra_args)
    err = open(errfile, "wb")
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
    atexit.register(proc.kill)
    early = False

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    deadline = time.time() + timeout
    while time.time() < deadline:
        if wait_for in slog():
            break
        if proc.poll() is not None:
            early = True
            break
        time.sleep(0.5)
    err.close()
    return proc, slog, early


def kill(proc):
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


def first_match(pattern, text, default="未出现"):
    m = re.search(pattern, text)
    return m.group(0) if m else default


def beats(log):
    return [int(m) for m in re.findall(r"\[TASK64\] kheart beat=(\d+)", log)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="系统内核镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150, help="每次启动等待的最长秒数")
    ap.add_argument("--keep", action="store_true", help="打印串口日志目录")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_usb64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 USB 主机（UHCI + HID 引导键盘）验收 ===")

    # ============================================================ 1) -usb -device usb-kbd
    print("[usb64] 1) 主跑：-usb -device usb-kbd（%s）" % args.img)
    port = pick_port()
    mon = Monitor(port)
    proc, slog, early = boot(qemu, args.img, ["-usb", "-device", "usb-kbd"],
                             os.path.join(tmp, "usb_kbd.log"),
                             os.path.join(tmp, "usb_kbd.err"),
                             port, "[GUI64] ready", args.timeout)
    log = slog()
    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）")

    check("[USB64] uhci pci <bus>:<dev>.<fn> io=<hex> ports=<n>",
          bool(re.search(r"\[USB64\] uhci pci \d+:\d+\.\d+ io=[0-9A-F]+ ports=\d+", log)),
          first_match(r"\[USB64\] uhci pci[^\r\n]*", log))
    check("[USB64] frame list @<hex> (1024 entries)",
          bool(re.search(r"\[USB64\] frame list @[0-9A-F]{8} \(1024 entries\)", log)),
          first_match(r"\[USB64\] frame list[^\r\n]*", log))
    check("[USB64] port <n> connected speed=full reset ok",
          bool(re.search(r"\[USB64\] port \d+ connected speed=(full|low) reset ok", log)),
          first_match(r"\[USB64\] port \d+ connected[^\r\n]*", log))
    m_dev = re.search(r"\[USB64\] device addr=0 mps=(\d+) vendor=([0-9A-Fa-f]{4}) "
                      r"product=([0-9A-Fa-f]{4})", log)
    check("[USB64] device addr=0 mps=8 vendor=0627 product=0001（QEMU usb-kbd）",
          bool(m_dev), m_dev.group(0) if m_dev else "未出现")
    if m_dev:
        check("设备描述符里 mps=8（EP0 最大包）", m_dev.group(1) == "8",
              "mps=%s" % m_dev.group(1))
        check("vendor/product = 0627:0001（QEMU 的 usb-kbd）",
              m_dev.group(2).upper() == "0627" and m_dev.group(3).upper() == "0001",
              "%s:%s" % (m_dev.group(2), m_dev.group(3)))
    m_cfg = re.search(r"\[USB64\] config set value=1 ifaces=(\d+) hid=1 ep_in=([0-9A-Fa-f]{2}) "
                      r"mps=(\d+)", log)
    check("[USB64] config set value=1 ifaces=1 hid=1 ep_in=<hex> mps=8",
          bool(m_cfg) and m_cfg.group(2).upper() == "81" and m_cfg.group(3) == "8",
          m_cfg.group(0) if m_cfg else "未出现")
    check("[USB64] hid boot protocol set (8-byte reports)",
          "[USB64] hid boot protocol set (8-byte reports)" in log)
    check("[USB64] selftest PASS（主控/帧列表/端口/设备/端点全过）",
          "[USB64] selftest PASS" in log)
    check("[GUI64] ready（USB 初始化后桌面照常起来）", "[GUI64] ready" in log)
    check("kusb 内核线程被创建（task64 的 create 行里有 name=kusb）",
          "name=kusb" in log,
          first_match(r"\[TASK64\] create[^\r\n]*name=kusb[^\r\n]*", log))
    for needle in FORBIDDEN_ALL:
        check("不得出现 %s" % needle, needle not in log)

    # ---- ★ 端到端：monitor sendkey -> USB HID 键盘 -> 桌面外壳 ----
    print("--- 端到端证据：sendkey 注入按键（USB 中断端点 -> 桌面响应）---")
    print("    ※ 实测：QEMU 的 sendkey 会同时分发给 PS/2（help sendkey 无设备参数），")
    print("       所以 USB 的专属证据是 [USB64] hid report 行 + 同一轮里桌面的响应。")
    mon.key("meta_l", wait=1.2)                      # Win 键 -> 开始菜单
    log2 = slog()
    check("★ [USB64] hid report key=E3 down=1（USB 键盘的报告真的到了驱动）",
          "hid report key=E3 down=1" in log2,
          first_match(r"\[USB64\] hid report key=E3 down=1[^\r\n]*", log2))
    check("★ 桌面外壳响应了这次按键：[UI] menu open（开始菜单被打开）",
          "[UI] menu open" in log2)
    # 边沿检测：`sendkey meta_l` 期间设备可能发多个报告，但只有"新按下/释放"各一次
    n_mod_down = len(re.findall(r"\[USB64\] hid report key=E3 down=1", log2))
    n_mod_up = len(re.findall(r"\[USB64\] hid report key=E3 down=0", log2))
    check("修饰键边沿只打一次按下（不是每个报告都刷屏）", n_mod_down == 1,
          "down=1 行数=%d" % n_mod_down)
    check("修饰键释放边沿也能收到（down=0）", n_mod_up >= 1, "down=0 行数=%d" % n_mod_up)

    mon.key("1", wait=1.4)                           # 开始菜单第 1 项 = 终端
    mon.key("ret", wait=1.0)
    log3 = slog()
    check("★ key=1E（数字 1）的 USB 报告行出现",
          "hid report key=1E down=1" in log3,
          first_match(r"\[USB64\] hid report key=1E[^\r\n]*", log3))
    check("★ 桌面按这个键真的开了终端：[APP] term opened", "[APP] term opened" in log3)
    check("菜单项被激活（[UI] menu activate idx=0）", "[UI] menu activate idx=0" in log3)
    check("按下的键都产生了释放边沿（队列不会卡住）",
          len(re.findall(r"\[USB64\] hid report key=\w{2} down=0", log3)) >= 3,
          "down=0 行数=%d" % len(re.findall(r"\[USB64\] hid report key=\w{2} down=0", log3)))

    print("--- kusb 不饿着桌面：kheart 心跳持续增长 ---")
    b0 = beats(log3)
    time.sleep(4.0)
    log4 = slog()
    b1 = beats(log4)
    check("kusb 在线时 kheart 心跳仍在增长（桌面/调度器没被 USB 轮询饿死）",
          len(b0) > 0 and len(b1) > 0 and b1[-1] > b0[-1],
          "before=%s after=%s" % (b0[-1] if b0 else "?", b1[-1] if b1 else "?"))
    for needle in ["PANIC", "TRIPLE FAULT", "frameprobe FAIL"]:
        check("按键之后仍不得出现 %s" % needle, needle not in log4)
    kill(proc)

    # ============================================================ 2) -usb（无键盘）
    print("[usb64] 2) 降级：-usb（有主控、没插设备）")
    port2 = pick_port(port + 1)
    mon2 = Monitor(port2)
    proc2, slog2, early2 = boot(qemu, args.img, ["-usb"],
                                os.path.join(tmp, "usb_empty.log"),
                                os.path.join(tmp, "usb_empty.err"),
                                port2, "[GUI64] ready", args.timeout)
    time.sleep(1.0)
    logn = slog2()
    if early2:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    check("[USB64] no device on port <n>（没插设备 -> 优雅降级）",
          bool(re.search(r"\[USB64\] no device on port \d+", logn)),
          first_match(r"\[USB64\] no device on port \d+", logn))
    check("[USB64] not found 不得出现（主控是有的）", "[USB64] not found" not in logn)
    check("[USB64] selftest 打 skipped（不算失败）",
          "selftest skipped" in logn,
          first_match(r"\[USB64\] selftest[^\r\n]*", logn))
    check("没有任何 [USB64] hid report 行（没键盘就不该有报告）",
          "[USB64] hid report" not in logn)
    check("[GUI64] ready（系统照常启动）", "[GUI64] ready" in logn)
    for needle in FORBIDDEN_ALL:
        check("-usb 不得出现 %s" % needle, needle not in logn)
    # PS/2 键盘仍可用（顺带实测：QEMU 的 sendkey 也走 PS/2）
    mon2.key("meta_l", wait=1.2)
    logn2 = slog2()
    check("PS/2 键盘仍能打开开始菜单（没有 USB 键盘时系统一切照旧）",
          "[UI] menu open" in logn2)
    check("这次仍然没有任何 [USB64] hid report 行（= sendkey 走的是 PS/2）",
          "[USB64] hid report" not in logn2)
    kill(proc2)

    # ============================================================ 3) 没有 -usb（没主控）
    print("[usb64] 3) 降级：不加 -usb（机器上没有 UHCI 主控）")
    proc3, slog3, early3 = boot(qemu, args.img, [],
                                os.path.join(tmp, "usb_none.log"),
                                os.path.join(tmp, "usb_none.err"),
                                pick_port(port2 + 1), "[GUI64] ready", args.timeout)
    logz = slog3()
    if early3:
        print("  [!] QEMU 提前退出（复位/三重故障？）")
    check("[USB64] not found（没有主控 -> 优雅降级）", "[USB64] not found" in logz,
          first_match(r"\[USB64\] not found[^\r\n]*", logz))
    check("没有主控时不打 hid report / config set",
          "[USB64] hid report" not in logz and "[USB64] config set" not in logz)
    check("[GUI64] ready（系统照常启动）", "[GUI64] ready" in logz)
    for needle in FORBIDDEN_ALL:
        check("无主控时不得出现 %s" % needle, needle not in logz)
    kill(proc3)

    if args.keep:
        print("[usb64] 串口日志目录：%s" % tmp)

    print("--- serial 摘录（主跑：全部 [USB64] 行 + 桌面响应）---")
    for line in slog().splitlines():
        if line.startswith("[USB64]") or "[UI] menu" in line or "[APP] term opened" in line \
           or "[GUI64] ready" in line:
            print("   | " + line.strip()[:170])

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
