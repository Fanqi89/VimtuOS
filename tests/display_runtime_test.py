#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/display_runtime_test.py - 运行期显示层（kernel/display64.cpp）自动验收

验收目标（每一条都绑到可核对的串口证据）：
  1) [DISP64] selftest PASS（常量/来源自洽/±5% 对比逻辑的合成样本自检）；
  2) [DISP64] mode list=<n>：n > 0，且每条 [DISP64] mode list i=.. mode=0x.. WxHxBpp 数值合理
     （引导期 loader64.asm 放在物理 0x7400 的可用模式表是真值，不是硬编码清单）；
  3) 0x3DA 实测：两条路都算通过（**如实降级不是失败**）：
       * 可测：[DISP64] vga measure=<n>Hz samples=<n>，n 必须落在 40..200Hz；
         且必须有对比行 [DISP64] vga measure=<n>Hz edid=<m>Hz match=<0|1>；
       * 不可测：[DISP64] vga measure=n/a samples=<n> edges=<n> reason=..（QEMU 的 0x3DA
         每次读都翻转状态位，本项目实测就是这条）——此时 [DISP64] init 的刷新率必须来自
         EDID（src=edid，@75.0 之类）或如实写 src=none@unknown，绝不编数字；
  4) DDC 运行期再探测**未实现**这件事在日志里如实写着：
     [DISP64] ddc runtime re-probe skipped (i2c bit-bang risk; boot-time EDID only)；
  5) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / [DISP64] selftest FAIL。

用法（必须用 Windows 原生 Python）：py -3 tests\\display_runtime_test.py [--timeout 150] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
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

RX_INIT = re.compile(r"\[DISP64\] init modes=(\d+) cur=(\d+)x(\d+)@(\S+) src=(\S+)")
RX_LIST = re.compile(r"\[DISP64\] mode list=(\d+)")
RX_LIST_I = re.compile(r"\[DISP64\] mode list i=(\d+) mode=0x([0-9A-Fa-f]+) (\d+)x(\d+)x(\d+)")
RX_VGA = re.compile(r"\[DISP64\] vga measure=([0-9]+\.[0-9])Hz samples=(\d+)")
RX_VGA_EDID = re.compile(r"\[DISP64\] vga measure=[0-9.]+Hz edid=([0-9.]+)Hz match=([01])")
RX_VGA_NA = re.compile(r"\[DISP64\] vga measure=n/a samples=(\d+) edges=(\d+)")

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "[DISP64] selftest FAIL",
    "[EDID64] selftest FAIL",
]


def find_qemu(explicit=None):
    for c in QEMU_CANDIDATES:
        if explicit and os.path.sep in explicit:
            return explicit if os.path.exists(explicit) else None
        if os.path.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            import shutil
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=150)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_disp64_")
    serial = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-disp64",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    try:
        # 显示层探测发生在 fb_init 之后、task_start64 之前；等 [GUI64] ready 保证全跑完
        t0 = time.time()
        log = ""
        while time.time() - t0 < args.timeout:
            log = slog()
            if "[GUI64] ready" in log:
                time.sleep(1.0)
                log = slog()
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)

        print("=== 1) 自检 + 模式清单 ===")
        check("[DISP64] selftest PASS 必现", "[DISP64] selftest PASS" in log)
        m_init = RX_INIT.search(log)
        check("[DISP64] init modes=.. cur=WxH@.. src=.. 行", bool(m_init),
              m_init.group(0) if m_init else "（缺 init 行）")
        if m_init:
            modes = int(m_init.group(1))
            w, h = int(m_init.group(2)), int(m_init.group(3))
            hz_s, src = m_init.group(4), m_init.group(5)
            check("可用模式数 > 0（引导期 0x7400 模式表非空）", modes > 0, "modes=%d" % modes)
            check("当前模式分辨率合理", w >= 320 and h >= 200, "%dx%d" % (w, h))
            check("src 是四种如实来源之一", src in ("edid", "vga", "crtc", "none"), "src=%s" % src)
            if src == "none":
                check("src=none 时刷新率如实写 unknown（不编数字）", hz_s == "unknown", "@%s" % hz_s)
            else:
                check("src 非 none 时刷新率是数字", bool(re.fullmatch(r"[0-9]+\.[0-9]", hz_s)), "@%s" % hz_s)

        m_cnt = RX_LIST.search(log)
        items = RX_LIST_I.findall(log)
        check("[DISP64] mode list=<n> 汇总行", bool(m_cnt), m_cnt.group(0) if m_cnt else "（缺汇总行）")
        if m_cnt:
            n = int(m_cnt.group(1))
            check("mode list 条数 > 0", n > 0, "n=%d" % n)
            check("逐条 mode list i=.. 条数与汇总一致", len(items) == n,
                  "逐条=%d 汇总=%d" % (len(items), n))
            sane = all(int(it[2]) >= 320 and int(it[3]) >= 200 and int(it[4]) >= 8 for it in items)
            check("每条模式的 W/H/Bpp 合理（>=320x200x8）", sane)
        else:
            check("mode list 条数 > 0", False)

        print("=== 2) 0x3DA 实测（可测/如实降级两条路）===")
        m_vga = RX_VGA.search(log)
        m_na = RX_VGA_NA.search(log)
        check("有 vga measure 行（数字或 n/a）", bool(m_vga) or bool(m_na),
              (m_vga.group(0) if m_vga else (m_na.group(0) if m_na else "")))
        if m_vga:
            hz = float(m_vga.group(1))
            samples = int(m_vga.group(2))
            check("实测刷新率在 40..200Hz 之间", 40.0 <= hz <= 200.0, "hz=%.1f" % hz)
            check("实测采样数 > 0", samples > 0, "samples=%d" % samples)
            m_cmp = RX_VGA_EDID.search(log)
            check("有与 EDID 的对比行 match=0|1", bool(m_cmp),
                  m_cmp.group(0) if m_cmp else "（缺对比行）")
            if m_cmp:
                check("match 是 0/1", m_cmp.group(2) in ("0", "1"), "match=%s" % m_cmp.group(2))
        elif m_na:
            check("降级行带 samples（实测过但不可信）", int(m_na.group(1)) > 0,
                  "samples=%s edges=%s" % (m_na.group(1), m_na.group(2)))
            # 不可测时：如有 EDID，采用值必须来自 EDID；没有就如实 none@unknown
            if "[EDID64] present=1" in log and m_init:
                check("0x3DA 不可测 + 有 EDID -> src=edid（不编数字）", m_init.group(5) == "edid",
                      "src=%s" % m_init.group(5))
            elif m_init:
                check("0x3DA 不可测 + 无 EDID -> src=none@unknown", m_init.group(5) == "none")
        else:
            check("有 vga measure 行（数字或 n/a）", False)

        print("=== 3) DDC 再探测如实跳过 + 禁止项 ===")
        check("DDC 运行期再探测如实 skipped",
              "[DISP64] ddc runtime re-probe skipped" in log)
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in log)

        print("--- [DISP64] 关键原文 ---")
        for line in log.splitlines():
            if "[DISP64]" in line or "[EDID64] preferred" in line:
                print("   | " + line.strip()[:200])
                if "mode list i=" in line and "i=3" not in line and "i=0" not in line:
                    pass
        if args.keep:
            print("[disp64] 串口日志：%s" % serial)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
