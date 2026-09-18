#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/smp64_test.py - VimtuOS SMP（启动 AP：INIT-SIPI-SIPI + 低端跳板）端到端验收

做什么（QEMU BIOS 路径，默认 machine，启动**系统内核** build64/system.img）：
  1) 主跑 `-smp 2`：断言
       * ACPI 确实看到 2 个 CPU：[ACPI64] madt cpus=2
       * 中断已切到 APIC 模式：[APIC] irq mode = apic（SMP 只在 APIC 模式下做）
       * 准备/自检：[SMP] selftest PASS（自检在**发 SIPI 之前**跑）
       * ★ AP 真的起来了：「[SMP] ap id=1 online (stack=0x... index=1)」——
         这一行**由 AP 自己打印**（跳板 -> 64 位 -> smp64_ap_entry64），
         并且其中的 stack 必须与 BSP 在「[SMP] ap id=1 sipi ... stack=0x...」里
         写进共享块的那个栈顶逐位一致（= AP 读到了 BSP 写的共享块）。
       * 汇总：[SMP] online=2/2 bsp_lapic_id=<n> trampoline@0x<hex>
       * 功能正常：SMP 上网后 [TASK64] kheart beat= 持续增长（调度器仍被 IRQ0 驱动）、
         [GUI64] ready。
       * 禁止：PANIC / TRIPLE FAULT / FAILED mask= / [SMP] selftest FAIL /
         [SMP] ap id=<n> timeout。
  2) 额外跑 `-smp 4`：3 个 AP 都要上线（每核独立栈、互不重叠），online=4/4。
  3) 降级跑 `-smp 1`：断言 [SMP] single cpu (no AP to start)（优雅跳过），系统照常启动、
     桌面照常起来，且**没有任何 AP 上线行**。
  4) 降级跑 `-machine pc,acpi=off`（没有 ACPI 的机器）：断言 SMP 被跳过并说明原因
     [SMP] skipped (irq mode = pic)（kernel64.cpp 在非 APIC 模式下的分支）。
     本地 QEMU 不支持该 machine 属性时如实报 SKIP，不误判 PASS。

用法（必须用 Windows 原生 Python，MSYS2 的 python 会让 QEMU 检测失败）：
    py -3 tests\\smp64_test.py
    py -3 tests\\smp64_test.py --qemu "C:\\Program Files\\qemu\\qemu-system-x86_64.exe" --timeout 120

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import atexit
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

# 禁止出现（主跑与两个降级跑都查）
FORBIDDEN_ALL = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "[SMP] selftest FAIL"]


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


def boot(qemu, img, extra_args, serial, errfile, wait_for, timeout):
    """无头启动；轮询串口日志到 wait_for（或超时/提前退出）。
    返回 (proc, slog, early, err_txt)；调用方负责 kill（进程可能还活着）。"""
    if os.path.exists(serial):
        os.remove(serial)
    if os.path.exists(errfile):
        os.remove(errfile)
    args = [
        qemu, "-name", "Vimtu64-smp64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
        "-monitor", "none",
    ]
    args += list(extra_args)
    err = open(errfile, "wb")
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=err)
    atexit.register(proc.kill)                     # 任何异常退出都不留 QEMU 进程
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
    try:
        with open(errfile, "r", encoding="utf-8", errors="replace") as f:
            err_txt = f.read()
    except OSError:
        err_txt = ""
    return proc, slog, early, err_txt


def kill(proc):
    if proc.poll() is None:
        proc.kill()
        try:
            proc.wait(timeout=10)
        except Exception:
            pass


def beats(log):
    return [int(m) for m in re.findall(r"\[TASK64\] kheart beat=(\d+)", log)]


def first_match(pattern, text, default="未出现"):
    m = re.search(pattern, text)
    return m.group(0) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="系统内核镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=120, help="每次启动等待的最长秒数")
    ap.add_argument("--keep", action="store_true", help="保留串口日志路径（打印出来）")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(args.img):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % args.img)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_smp64_")
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    print("=== Vimtu64 smp64 acceptance（启动 AP + 降级）===")

    # ---------------------------------------------------------------- 1) -smp 2
    print("[smp64] 1) 主跑：-smp 2（%s）" % args.img)
    proc2, slog2, early, err = boot(qemu, args.img, ["-smp", "2"],
                                    os.path.join(tmp, "smp2.log"),
                                    os.path.join(tmp, "smp2.err"),
                                    "[GUI64] ready", args.timeout)
    log = slog2()
    if early:
        print("  [!] QEMU 提前退出（复位/三重故障？）err=%s" % err.strip()[:160])

    check("[ACPI64] madt cpus=2（ACPI 看到 2 个 CPU）",
          "[ACPI64] madt cpus=2" in log,
          first_match(r"\[ACPI64\] madt cpus=\d+[^\r\n]*", log))
    check("[APIC] irq mode = apic（SMP 只在 APIC 模式下做）",
          "[APIC] irq mode = apic" in log)
    check("[SMP] selftest PASS（跳板/共享块/GDT/每核栈，发 SIPI 之前）",
          "[SMP] selftest PASS" in log)
    check("[SMP] 准备行（cpus=2 ap=1 bsp_lapic_id/trampoline/vector）",
          bool(re.search(r"\[SMP\] cpus=2 ap=1 bsp_lapic_id=\d+ lapic_base=0x[0-9A-Fa-f]+ "
                         r"trampoline@0x[0-9A-Fa-f]+ vector=0x8", log)),
          first_match(r"\[SMP\] cpus=\d+[^\r\n]*", log))

    # ★ AP 自己打的那一行（regex 要求整行连续 -> 顺带验证没有和 BSP 的行交错）
    m_ap = re.search(r"\[SMP\] ap id=1 online \(stack=0x([0-9A-Fa-f]+) index=(\d+)\)", log)
    check("★ AP 自己打的在线行：[SMP] ap id=1 online (stack=0x... index=1)",
          bool(m_ap), m_ap.group(0) if m_ap else "未出现")
    if m_ap:
        check("AP 在线行 index=1", m_ap.group(2) == "1", "index=%s" % m_ap.group(2))
    # BSP 在共享块里写给该 AP 的栈顶（SIPI 之前打印）
    m_sipi = re.search(r"\[SMP\] ap id=1 sipi vector=0x8 index=1 stack=0x([0-9A-Fa-f]+)", log)
    check("BSP 侧 intent 行完整（sipi vector/index/stack）", bool(m_sipi),
          m_sipi.group(0) if m_sipi else "未出现")
    if m_ap and m_sipi:
        check("AP 读到的栈顶 == BSP 写进共享块的栈顶（共享块真的被 AP 读到）",
              m_ap.group(1).upper() == m_sipi.group(1).upper(),
              "ap=0x%s bsp=0x%s" % (m_ap.group(1), m_sipi.group(1)))
        check("栈顶 16 字节对齐", int(m_ap.group(1), 16) % 16 == 0,
              "stack=0x%s" % m_ap.group(1))

    m_on = re.search(r"\[SMP\] online=(\d+)/(\d+) bsp_lapic_id=(\d+) "
                     r"trampoline@0x([0-9A-Fa-f]+)", log)
    check("[SMP] online=2/2（含 BSP）",
          bool(m_on) and m_on.group(1) == "2" and m_on.group(2) == "2",
          m_on.group(0) if m_on else "未出现")
    if m_on:
        check("汇总行的 trampoline@ 在低端页（非 0 且 < 1MB）",
              0 < int(m_on.group(4), 16) < 0x100000, "trampoline@0x%s" % m_on.group(4))

    check("只有一个 AP（-smp 2 下不该出现 ap id=2 online）",
          "[SMP] ap id=2 online" not in log)
    check("没有单核跳过行（-smp 2 不该走 single cpu 分支）",
          "[SMP] single cpu" not in log)
    check("没有 AP 超时行", "[SMP] ap id=1 timeout" not in log)
    check("没有 note（计数/标志/在线数三者一致，且没有 AP 缺席）",
          "[SMP] note" not in log)
    for needle in FORBIDDEN_ALL:
        check("不得出现 %s" % needle, needle not in log)

    print("--- 功能证据：SMP 下调度器/桌面照常 ---")
    check("[GUI64] ready（SMP 上网后桌面照常起来）", "[GUI64] ready" in log)
    b0 = beats(log)
    check("已有 kheart 心跳（IRQ0 -> schedule64）", len(b0) > 0,
          "beat=%s" % (b0[-1] if b0 else "无"))
    time.sleep(3.0)
    log2 = slog2()
    b1 = beats(log2)
    check("kheart beat 持续增长（SMP 下调度器仍被 IRQ0 驱动）",
          len(b0) > 0 and len(b1) > 0 and b1[-1] > b0[-1],
          "before=%s after=%s" % (b0[-1] if b0 else "?", b1[-1] if b1 else "?"))
    check("SMP 下不得出现 PANIC/TRIPLE FAULT",
          "PANIC" not in log2 and "TRIPLE FAULT" not in log2)
    kill(proc2)

    # ---------------------------------------------------------------- 2) -smp 4
    print("[smp64] 2) 多 AP：-smp 4（3 个 AP 都要上线 + 每核独立栈）")
    proc4, slog4, early4, err4 = boot(qemu, args.img, ["-smp", "4"],
                                      os.path.join(tmp, "smp4.log"),
                                      os.path.join(tmp, "smp4.err"),
                                      "[GUI64] ready", args.timeout)
    time.sleep(1.5)
    log4 = slog4()
    if early4:
        print("  [!] -smp 4 提前退出 err=%s" % err4.strip()[:160])
    check("-smp 4：[ACPI64] madt cpus=4", "[ACPI64] madt cpus=4" in log4)
    check("-smp 4：[SMP] online=4/4",
          bool(re.search(r"\[SMP\] online=4/4 bsp_lapic_id=\d+", log4)),
          first_match(r"\[SMP\] online=\d+/\d+[^\r\n]*", log4))
    ap_lines = re.findall(r"\[SMP\] ap id=(\d+) online \(stack=0x([0-9A-Fa-f]+) index=(\d+)\)",
                          log4)
    check("-smp 4：3 个 AP 各自的 online 行（ap id=1/2/3）",
          sorted(a for a, _, _ in ap_lines) == ["1", "2", "3"],
          "实际 %s" % ([a for a, _, _ in ap_lines] or "无"))
    check("-smp 4：每核栈互不相同（独立内核栈，16 字节对齐）",
          len(ap_lines) == 3 and len({s.upper() for _, s, _ in ap_lines}) == 3 and
          all(int(s, 16) % 16 == 0 for _, s, _ in ap_lines),
          "stacks=%s" % [s for _, s, _ in ap_lines])
    check("-smp 4：[SMP] selftest PASS", "[SMP] selftest PASS" in log4)
    check("-smp 4：[GUI64] ready", "[GUI64] ready" in log4)
    check("-smp 4：没有 AP 超时行 / note",
          "timeout" not in "\n".join(l for l in log4.splitlines() if l.startswith("[SMP] ap"))
          and "[SMP] note" not in log4)
    for needle in FORBIDDEN_ALL:
        check("-smp 4 不得出现 %s" % needle, needle not in log4)
    kill(proc4)

    # ---------------------------------------------------------------- 3) -smp 1
    print("[smp64] 3) 降级：-smp 1（单核：优雅跳过，不启动任何 AP）")
    proc1, slog1, early1, err1 = boot(qemu, args.img, ["-smp", "1"],
                                      os.path.join(tmp, "smp1.log"),
                                      os.path.join(tmp, "smp1.err"),
                                      "[GUI64] ready", args.timeout)
    sleep_extra = 2.0
    time.sleep(sleep_extra)
    log1 = slog1()
    if early1:
        print("  [!] -smp 1 提前退出 err=%s" % err1.strip()[:160])
    check("-smp 1：[SMP] single cpu (no AP to start)（优雅退出，不是失败）",
          "[SMP] single cpu (no AP to start)" in log1)
    check("-smp 1：[ACPI64] madt cpus=1", "[ACPI64] madt cpus=1" in log1)
    check("-smp 1：没有 AP 上线行（一个 AP 都没启）", "[SMP] ap id=" not in log1)
    check("-smp 1：不打印 selftest（没准备任何东西就早退）",
          "[SMP] selftest" not in log1)
    check("-smp 1：[GUI64] ready（系统照常启动）", "[GUI64] ready" in log1)
    d0 = beats(log1)
    check("-smp 1：kheart 心跳存在且调度器在跑", len(d0) > 0,
          "beat=%s" % (d0[-1] if d0 else "无"))
    for needle in FORBIDDEN_ALL:
        check("-smp 1 不得出现 %s" % needle, needle not in log1)
    kill(proc1)

    # ------------------------------------------- 4) -machine pc,acpi=off（无 ACPI）
    print("[smp64] 4) 降级：-machine pc,acpi=off（没进 APIC 模式 -> SMP 必须跳过）")
    procp, slogp, earlyp, errp = boot(qemu, args.img, ["-machine", "pc,acpi=off"],
                                      os.path.join(tmp, "noacpi.log"),
                                      os.path.join(tmp, "noacpi.err"),
                                      "[GUI64] ready", args.timeout + 30)
    time.sleep(1.5)
    logp = slogp()
    unsupported = (not logp.strip()) and ("invalid" in errp.lower())
    if unsupported:
        print("  [SKIP] 本地 QEMU 不支持 -machine pc,acpi=off：%s" % errp.strip()[:120])
        checks.append(("降级路径（环境不支持 -> SKIP）", True))
    else:
        check("无 ACPI：[APIC] irq mode = pic（回退到 8259）",
              "[APIC] irq mode = pic" in logp)
        check("无 ACPI：[SMP] skipped (irq mode = pic)",
              "[SMP] skipped (irq mode = pic)" in logp)
        check("无 ACPI：没有启动任何 AP（无 ap id= online 行）",
              "[SMP] ap id=" not in logp)
        check("无 ACPI：[GUI64] ready（系统照常起来）", "[GUI64] ready" in logp)
        for needle in FORBIDDEN_ALL:
            check("-machine pc,acpi=off 不得出现 %s" % needle, needle not in logp)
    kill(procp)

    if args.keep:
        print("[smp64] 串口日志：%s" % tmp)
    print("--- serial 摘录（主跑 -smp 2）：[SMP] 全部行 + ACPI/APIC/心跳 ---")
    for line in log2.splitlines():
        if line.startswith("[SMP]") or "[ACPI64] madt cpus=" in line \
           or "[APIC] irq mode" in line or "[GUI64] ready" in line:
            print("   | " + line[:180])
    mb = beats(log2)
    print("   | " + first_match(r"\[TASK64\] kheart beat=%d[^\r\n]*" % mb[-1], log2,
                                "(无 kheart 行)") if mb else "   | (无 kheart 行)")

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
