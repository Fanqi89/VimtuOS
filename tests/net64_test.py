#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/net64_test.py - VimtuOS 网络端到端验收（e1000 驱动 + ARP/ICMP）

做什么：
  1) 用 QEMU **用户模式网络 + e1000 网卡**启动系统内核（build64/system.img）：
       -netdev user,id=n0 -device e1000,netdev=n0
     其余参数与既有测试一致（无头、-serial file:、-no-reboot）。
  2) 读串口日志断言（与 kernel/e1000_64.cpp、kernel/net64.cpp 的打点严格对应）：
     [E1000] pci ... / mac=... link=up / init ok tx_desc=/ rx_desc=
     [E1000] selftest PASS
     [NET64] config ip=10.0.2.15/24 ...
     [NET64] arp reply 10.0.2.2 is-at <mac>        <- 真发 ARP 并等到 slirp 的回复
     [NET64] icmp reply from 10.0.2.2 seq=1 ...    <- 真发 ICMP echo 并等到回复
     [NET64] selftest PASS
  3) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / [E1000] not found / [NET64] no link
     等失败标记（net64 的离线自检失败会打 [NET64] selftest FAIL mask=）。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\net64_test.py
    py -3 tests\\net64_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 150

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
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

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")

# 必须出现（顺序即串口输出顺序）
MUST = [
    ("[E1000] pci ", "PCI 上找到 e1000（bus:dev.fn + bar0 + mmio=ok）"),
    ("[E1000] mac=", "读到 MAC"),
    ("[E1000] init ok tx_desc=", "描述符环就绪（TX/RX 描述符数）"),
    ("[E1000] selftest PASS", "e1000 寄存器/环地址自检全过"),
    ("[NET64] config ip=10.0.2.15/24 gw=10.0.2.2 dns=10.0.2.3", "静态配置（QEMU 用户网络固定值）"),
    ("[NET64] arp reply 10.0.2.2 is-at", "ARP 请求/回复真路径（slirp 回了网关 MAC）"),
    ("[NET64] icmp reply from 10.0.2.2", "ICMP echo 真路径（ping 网关收到回复）"),
    ("[NET64] selftest PASS", "协议栈离线自检（校验和/IP/ARP/最小帧补零）全过"),
    ("[GUI64] ready", "探测完成后桌面照常起来（联网流程不阻塞启动）"),
]

# 禁止出现
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "[E1000] selftest FAIL",
    "[NET64] selftest FAIL",
    "[E1000] not found",        # 本测试显式挂了 -device e1000，找不到就是回归
    "[NET64] no link",          # 链路/探测降级行（成功路径上不该出现）
]


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


def boot(qemu, img, logdir, timeout):
    """无头启动（用户模式网络 + e1000），轮询串口日志到 [GUI64] ready（或超时/提前退出）。"""
    serial = os.path.join(logdir, "net64.log")
    if os.path.exists(serial):
        os.remove(serial)
    netid = "n0"
    args = [
        qemu, "-name", "Vimtu64-net64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-netdev", "user,id=%s" % netid,
        "-device", "e1000,netdev=%s" % netid,
        "-no-reboot",
    ]
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    early = False

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    try:
        deadline = time.time() + timeout
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
    return slog(), early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="系统内核镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150, help="启动等待的最长秒数")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_net64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 net64 acceptance（e1000 + 用户模式网络 + ARP/ICMP）===")
    print("[net64] 启动：%s（-netdev user,id=n0 -device e1000,netdev=n0）" % args.img)
    log, early = boot(qemu, args.img, tmp, args.timeout)

    print("--- 必须出现 ---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log)

    print("--- 禁止出现 ---")
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log)
    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）")

    # 额外：ARP reply 行里应带六段 MAC；icmp reply 行应带 seq=1
    import re
    m = re.search(r"\[NET64\] arp reply 10\.0\.2\.2 is-at ([0-9a-f]{2}(?::[0-9a-f]{2}){5})", log)
    check("arp reply 行含完整 MAC（aa:bb:...）", bool(m), m.group(1) if m else "未出现")
    m2 = re.search(r"\[NET64\] icmp reply from 10\.0\.2\.2 seq=1 bytes=\d+ ttl=\d+ \(tx=\d+ rx=\d+\)", log)
    check("icmp reply 行格式完整（seq=1 bytes/ttl/tx/rx）", bool(m2), m2.group(0) if m2 else "未出现")

    if args.keep:
        print("[net64] 串口日志：%s" % os.path.join(tmp, "net64.log"))
    print("--- serial tail ---")
    for line in [x for x in log.splitlines() if x.strip()][-30:]:
        print("   | " + line[:180])

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
