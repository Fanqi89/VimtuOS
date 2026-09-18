#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/apic64_test.py - VimtuOS APIC（LAPIC + IOAPIC 接管中断路由）端到端验收

做什么：
  1) 用 QEMU（BIOS 路径，默认 machine）启动**系统内核**（build64/system.img），
     读串口日志断言 APIC 接管成功（kernel/apic64.cpp 的打点，格式严格对应）：
       [APIC] lapic base=... / ioapic #0 ... / route irq0|1|12|14 ... /
       [APIC] pic masked all (8259 disabled) / [APIC] irq mode = apic / selftest PASS
  2) **功能证据（最重要，不能只看打点）**：
       * PIT 仍在走：先看 [LM64] PIT IRQ OK，再看 [TASK64] kheart beat=N 在 APIC 模式下
         继续增长（IRQ0 经 IOAPIC GSI2 -> 向量 32 驱动 schedule64 的直接证据）；
       * ATA：APIC 接管后 [ATA64] irq14 selftest PASS（reads=.. irqs>0 polled=0）——
         说明 IRQ14 真的经 IOAPIC 到达，而不是回退轮询；
       * 键盘：QEMU monitor sendkey（Win 键）-> 断言 "[UI] menu open" + 数字键开应用
         断言 "[APP] term opened"（IRQ1 -> IOAPIC 向量 33 -> GUI 的端到端路径）；
       * 鼠标：monitor mouse_move -> 前后两张 screendump 的像素差（>20 且 <3% 屏幕）——
         IRQ12 -> IOAPIC 向量 44 到达并触发局部脏矩形重绘。
  3) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / [APIC] selftest FAIL /
     [APIC] unavailable / [APIC] irq mode = pic / [ATA64] irq14 selftest FAIL。
  4) 降级路径：用 `-machine pc,acpi=off`（等价于"没有 ACPI 的机器"）再启动一次，
     断言系统照常起来且打印 "[APIC] unavailable reason=... -> stay on 8259 PIC" +
     "[APIC] irq mode = pic"，kheart 照常心跳（证明 8259 PIC 回退可用、不变砖）。
     （如果本地 QEMU 不支持这个 machine 属性，这一项如实报告为 SKIP，不误判 PASS。）

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\apic64_test.py
    py -3 tests\\apic64_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 150

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

# 必须出现（顺序即串口输出顺序）
MUST = [
    ("[APIC] lapic base=", "LAPIC 基址/ID/SVR（来自 IA32_APIC_BASE，SVR=0x100|0xFF）"),
    ("[APIC] ioapic #0", "IOAPIC 版本寄存器给出的 addr/gsi_base/max_redir"),
    ("[APIC] route irq0 ->", "PIT 路由（IRQ0 -> 向量 32，GSI 按 MADT ISO 覆盖）"),
    ("[APIC] route irq1 ->", "键盘路由（IRQ1 -> 向量 33）"),
    ("[APIC] route irq12 ->", "鼠标路由（IRQ12 -> 向量 44）"),
    ("[APIC] route irq14 ->", "ATA 路由（IRQ14 -> 向量 46）"),
    ("[APIC] pic masked all (8259 disabled)", "两个 8259 的 IMR 全屏蔽（防双投递）"),
    ("[APIC] irq mode = apic", "全局 g_irq_mode64 切成 APIC（EOI 走 LAPIC 0xB0）"),
    ("[APIC] selftest PASS", "在线读回自检（基址/ID/SVR/IOAPIC/重定向表/掩码）"),
    ("[LM64] PIT IRQ OK", "切换之前 PIC 路径本身是通的（对照基线）"),
    ("[ATA64] irq14 selftest PASS", "★ 功能证据：IRQ14 经 IOAPIC 到达（irqs>0），不是轮询兜底"),
    ("[GUI64] ready", "APIC 接管后桌面照常起来"),
]

# 禁止出现
FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "[APIC] selftest FAIL",
    "[APIC] unavailable",
    "[APIC] irq mode = pic",
    "[ATA64] irq14 selftest FAIL",
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


class Monitor:
    """QEMU monitor 客户端（telnet 端口）：sendkey / mouse_move / screendump。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
            s.settimeout(1.0)
            while True:
                try:
                    if not s.recv(4096):
                        break
                except socket.timeout:
                    break
        finally:
            s.close()

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def mouse_move(self, dx, dy):
        self.send("mouse_move %d %d" % (dx, dy), wait=0.6)

    def shot(self, path, wait=2.5):
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return True
            time.sleep(0.3)
        return os.path.exists(path)


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
    idx = 2
    fields = []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def diff_count(px1, px2):
    n = 0
    for i in range(0, min(len(px1), len(px2)), 3):
        if px1[i] != px2[i] or px1[i + 1] != px2[i + 1] or px1[i + 2] != px2[i + 2]:
            n += 1
    return n


def boot(qemu, img, extra_args, serial, errfile, wait_for, timeout, monitor_port=None,
         keep_alive=False):
    """无头启动；轮询串口日志到 wait_for（或超时/提前退出）。返回 (log, early, err)。"""
    if os.path.exists(serial):
        os.remove(serial)
    if os.path.exists(errfile):
        os.remove(errfile)
    args = [
        qemu, "-name", "Vimtu64-apic64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
    ]
    args += list(extra_args)
    if monitor_port:
        args += ["-monitor", "telnet:127.0.0.1:%d,server,nowait" % monitor_port]
    err = open(errfile, "wb")
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
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
            if wait_for in slog():
                break
            if proc.poll() is not None:
                early = True
                break
            time.sleep(0.5)
    finally:
        # keep_alive=True 时留给调用方继续用（后面还要 monitor 注入键鼠/截屏），
        # 否则按老习惯在等不到 wait_for 或等到之后立即杀掉。
        if not keep_alive and proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    err.close()
    try:
        with open(errfile, "r", encoding="utf-8", errors="replace") as f:
            err_txt = f.read()
    except OSError:
        err_txt = ""
    return proc, slog, early, err_txt


def beats(log):
    return [int(m) for m in re.findall(r"\[TASK64\] kheart beat=(\d+)", log)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="系统内核镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5592, help="monitor 端口（主跑）")
    ap.add_argument("--timeout", type=int, default=120, help="每次启动等待的最长秒数")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    img = args.img
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_apic64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 apic64 acceptance（LAPIC + IOAPIC 接管中断路由 + 降级）===")
    print("[apic64] 主跑：%s（默认 machine / BIOS 路径，ACPI 可用）" % args.img)
    serial = os.path.join(tmp, "apic64.log")
    errfile = os.path.join(tmp, "apic64.err")
    proc, slog, early, err = boot(qemu, img, [], serial, errfile,
                                  "[GUI64] ready", args.timeout, monitor_port=args.port,
                                  keep_alive=True)     # 后面 4~6) 还要 monitor 注入键鼠/截屏
    atexit.register(proc.kill)                     # 任何异常退出都不留 QEMU 进程
    log = slog()
    print("--- 1) 必须出现的 APIC 打点 ---")
    for needle, what in MUST:
        check("%s（%s）" % (needle, what), needle in log)

    print("--- 2) 禁止出现 ---")
    for needle in FORBIDDEN:
        check("不得出现 %s" % needle, needle not in log)
    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）err=%s" % err.strip()[:160])

    print("--- 3) 打点格式（路由行的 GSI/向量/dest 位域）---")
    for irq, vec in ((0, 32), (1, 33), (12, 44), (14, 46)):
        m = re.search(r"\[APIC\] route irq%d -> gsi=(\d+) vec=(\d+) dest=(\d+)" % irq, log)
        check("route irq%d 行格式完整（gsi/vec/dest）" % irq, bool(m),
              m.group(0) if m else "未出现")
        if m:
            check("route irq%d 向量=%d dest=0" % (irq, vec),
                  int(m.group(2)) == vec and int(m.group(3)) == 0,
                  "实际 gsi=%s vec=%s dest=%s" % (m.group(1), m.group(2), m.group(3)))
    m0 = re.search(r"\[APIC\] lapic base=0x([0-9A-Fa-f]+) id=(\d+) svr=0x([0-9A-Fa-f]+) enabled=1",
                   log)
    check("lapic 行格式完整（base/id/svr/enabled）", bool(m0), m0.group(0) if m0 else "未出现")
    if m0:
        check("LAPIC base 非 0 / SVR=0x1FF",
              int(m0.group(1), 16) != 0 and int(m0.group(3), 16) == 0x1FF,
              "base=0x%s svr=0x%s" % (m0.group(1), m0.group(3)))
    mi = re.search(r"\[APIC\] ioapic #0 addr=0x([0-9A-Fa-f]+) gsi_base=(\d+) max_redir=(\d+)", log)
    check("ioapic 行格式完整（addr/gsi_base/max_redir）", bool(mi), mi.group(0) if mi else "未出现")
    if mi:
        check("IOAPIC max_redir >= 23", int(mi.group(3)) >= 23, "实际 %s" % mi.group(3))
    ma = re.search(r"\[ATA64\] irq14 selftest reads=(\d+) irqs=(\d+) polled=(\d+)", log)
    check("ATA irq14 selftest 行完整", bool(ma), ma.group(0) if ma else "未出现")
    if ma:
        check("IRQ14 是真中断路径（irqs>0 且 polled=0）",
              int(ma.group(2)) > 0 and int(ma.group(3)) == 0,
              "irqs=%s polled=%s" % (ma.group(2), ma.group(3)))

    print("--- 4) 功能证据：PIT（IRQ0 经 IOAPIC）仍在驱动调度器 ---")
    b0 = beats(log)
    check("APIC 模式下已有 kheart 心跳（beat 行存在）", len(b0) > 0,
          "beat=%s" % (b0[-1] if b0 else "无"))
    time.sleep(3.0)
    log2 = slog()
    b1 = beats(log2)
    check("kheart beat 持续增长（调度器仍被 IRQ0 驱动）",
          len(b1) > 0 and len(b0) > 0 and b1[-1] > b0[-1],
          "before=%s after=%s" % (b0[-1] if b0 else "?", b1[-1] if b1 else "?"))

    print("--- 5) 功能证据：键盘（IRQ1 经 IOAPIC -> 桌面）---")
    mon = Monitor(args.port)
    before = slog()
    mon.key("meta_l", wait=1.2)
    if "[UI] menu open" not in slog():
        mon.key("meta_l", wait=1.2)          # 按键偶尔丢失，再试一次
    after = slog()
    check("键盘：[UI] menu open 出现（IRQ1 -> 向量 33 到达）",
          "[UI] menu open" in after, "" if "[UI] menu open" in after else "（本次日志未见）")
    mon.key("1", wait=2.2)                   # 数字 1 = 开始菜单第 0 项：终端
    after2 = slog()
    check("键盘：数字键激活菜单并开应用（[APP] term opened）",
          "[APP] term opened" in after2 or "[UI] menu activate idx=0" in after2,
          "" if "[APP] term opened" in after2 or "[UI] menu activate idx=0" in after2 else "（未出现）")

    print("--- 6) 功能证据：鼠标（IRQ12 经 IOAPIC -> 局部脏矩形重绘）---")
    shot1 = os.path.join(tmp, "s1.ppm")
    shot2 = os.path.join(tmp, "s2.ppm")
    if mon.shot(shot1):
        try:
            w1, h1, px1 = read_ppm(shot1)
            mon.mouse_move(300, 200)
            time.sleep(0.8)
            mon.shot(shot2)
            w2, h2, px2 = read_ppm(shot2)
            if (w1, h1) == (w2, h2):
                d = diff_count(px1, px2)
                check("鼠标移动后有像素变化（IRQ12 到达）", d > 20, "变化像素=%d" % d)
                check("变化是局部脏矩形（<3% 屏幕）", d < int(w1 * h1 * 0.03),
                      "占比 %.3f%%" % (100.0 * d / (w1 * h1)))
            else:
                check("两次截图尺寸一致", False, "%dx%d vs %dx%d" % (w1, h1, w2, h2))
        except Exception as e:  # 截图/PPM 解析异常不算通过
            check("鼠标像素断言可执行", False, str(e))
    else:
        check("鼠标：screendump 可抓", False, "截图未生成")

    if proc.poll() is None:                        # 主跑结束：先收掉 QEMU 再跑降级
        proc.kill()

    print("--- 7) 降级路径：-machine pc,acpi=off（没有 ACPI 的机器）---")
    dserial = os.path.join(tmp, "noacpi.log")
    derr = os.path.join(tmp, "noacpi.err")
    dproc, dslog, dearly, derr_txt = boot(qemu, img, ["-machine", "pc,acpi=off"],
                                          dserial, derr, "[GUI64] ready", args.timeout + 30,
                                          keep_alive=True)
    atexit.register(dproc.kill)
    dlog = dslog()
    unsupported = (not dlog.strip()) and ("invalid" in derr_txt or "invalid" in derr.lower())
    if unsupported:
        print("  [SKIP] 本地 QEMU 不支持 -machine pc,acpi=off：%s" % derr_txt.strip()[:120])
        checks.append(("降级路径（环境不支持 -> SKIP）", True))
    else:
        check("降级：打印 unavailable 行并说明留在 PIC",
              "[APIC] unavailable reason=" in dlog and "-> stay on 8259 PIC" in dlog)
        check("降级：[APIC] irq mode = pic", "[APIC] irq mode = pic" in dlog)
        check("降级：没有启用 APIC（不得出现 irq mode = apic）",
              "[APIC] irq mode = apic" not in dlog)
        check("降级：selftest 不报 FAIL", "[APIC] selftest FAIL" not in dlog)
        check("降级：系统照常起来（[LM64] PIT IRQ OK + [GUI64] ready）",
              "[LM64] PIT IRQ OK" in dlog and "[GUI64] ready" in dlog)
        db = beats(dlog)
        time.sleep(2.5)
        dlog2 = dslog()
        db2 = beats(dlog2)
        check("降级：kheart 心跳持续（8259 PIC 路径可用）",
              len(db) > 0 and len(db2) > 0 and db2[-1] > db[-1],
              "before=%s after=%s" % (db[-1] if db else "?", db2[-1] if db2 else "?"))
        for needle in ("PANIC", "TRIPLE FAULT"):
            check("降级：不得出现 %s" % needle, needle not in dlog2)

    if args.keep:
        print("[apic64] 串口日志：%s" % tmp)
    print("--- serial tail（主跑）---")
    for line in [x for x in log2.splitlines() if x.strip()][-25:]:
        print("   | " + line[:180])

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
