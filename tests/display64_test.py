#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/display64_test.py - 运行期显示层（EDID / 刷新率 / 时序）自动验收

做什么：
  1) 无头启动 build64/system.img（= 已装好的系统内核），QEMU 用 -vga std：
     这台"显示器"有自己的内置 EDID（厂商 RHT / 名字 QEMU Monitor），引导层
     （boot/loader64.asm 的 read_edid）会把它读到物理 0x7600，BootInfo.edid_ok = 1；
  2) 断言内核运行期真的解析了它（kernel/edid64.cpp 的 [EDID64] 打点），
     并把解析出的首选时序与 framebuffer 的实际物理分辨率对齐（这是"解析对不对"的硬证据：
     两条路径来源不同 —— EDID 描述符位域 vs 引导层选中的 VBE 模式）；
  3) 用 [EDID64] detail 行里的 htotal/vtotal 复算一次刷新率，校验算术自洽
     （四舍五入到 0.1Hz，必须与 preferred 行的 refresh= 完全一致）；
  4) 打开"设置"应用（Win 键 + 数字 6），断言"显示"页真的画出了 EDID 的刷新率
     （[UI] settings display hz=<x.x>Hz source=edid preferred-timing），而不是旧的"不支持"；
  5) 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / [EDID64] bad header/checksum /
     [EDID64] selftest FAIL。

边界（测试覆盖不到的，如实写）：
  * 只覆盖 QEMU -vga std 这一台"显示器"（VMware / 真机 / 无 EDID 的固件没验）；
  * 只断言第一块 128B EDID 的解析；扩展块（CEA-861）不解析、也不断言；
  * 任务管理器"性能"页的刷新率行是屏幕内容，本脚本只覆盖设置页那一条串口打点。

用法（必须用 Windows 原生 Python）：
    py -3 tests\\display64_test.py
    py -3 tests\\display64_test.py --img <已装好的磁盘镜像> --timeout 150 --keep

退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
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

# 首选时序行的口径（与 kernel/edid64.cpp 的打点逐字对应；改格式必须同步这里）
RX_PRESENT = re.compile(r"\[EDID64\] present=(\d+) ver=(\d+)\.(\d+) mfg=(\S+) name=(.+?) size=(\S+)")
RX_PREFERRED = re.compile(
    r"\[EDID64\] preferred pclk=(\d+) Vactive=(\d+) Hactive=(\d+) refresh=(\d+)\.(\d)Hz")
RX_DETAIL = re.compile(
    r"\[EDID64\] detail prod=(\S+) serial=(\d+) week=(\d+) year=(\d+) ext=(\d+) "
    r"htotal=(\d+) vtotal=(\d+) hfront=(\d+) hsync=(\d+) vfront=(\d+) vsync=(\d+) "
    r"range_v=(\d+)\.\.(\d+) range_h=(\d+)\.\.(\d+)")
RX_FB_PHYS = re.compile(r"\[G64\] fb render=(\d+)x(\d+) phys=(\d+)x(\d+) zoom=(\d+)")
RX_SET_HZ = re.compile(r"\[UI\] settings display hz=([0-9.]+)Hz source=(\S+)")

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "[EDID64] selftest FAIL",
    "[EDID64] bad header/checksum",
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
    """QEMU monitor 客户端（telnet 端口）—— 只用来发键，打开设置窗口。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.5):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return b""
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
            s.settimeout(1.0)
            data = b""
            while True:
                try:
                    ch = s.recv(4096)
                    if not ch:
                        break
                    data += ch
                except socket.timeout:
                    break
        finally:
            s.close()
        return data

    def key(self, name, wait=1.0):
        self.send("sendkey %s" % name, wait=wait)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYSTEM_IMG, help="已装好的系统盘镜像（默认 build64/system.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5599, help="QEMU monitor telnet 端口")
    ap.add_argument("--timeout", type=int, default=150, help="序列等待上限秒数")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s\n（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_display_")
    serial = os.path.join(tmp, "serial.log")
    qemu_args = [
        qemu, "-name", "Vimtu64-display",
        "-drive", "format=raw,file=%s" % q(args.img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % args.port,
        "-no-reboot",
    ]
    print("[display64] 引导 %s（-vga std：QEMU 内置 EDID）" % args.img)
    proc = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                               ("  " + detail) if detail else ""))

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_for(needle, timeout, what):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in slog():
                print("  [ok] %s（%.1fs）" % (what, time.time() - t0))
                return True
            if proc.poll() is not None:
                print("  [!] QEMU 提前退出（复位/三重故障？）")
                return False
            time.sleep(0.5)
        print("  [!] 等 %r 超时 %.0fs" % (needle, timeout))
        return False

    try:
        print("=== 1) 等桌面起来 ===")
        up = wait_for("[GUI64] ready", args.timeout, "桌面就绪 [GUI64] ready")
        log = slog()
        check("系统内核启动到桌面", up)
        check("内存/外壳自检通过",
              "[MEM64] selftest PASS" in log and "[GUI64] selftest PASS" in log)

        print("=== 2) EDID 运行期解析（kernel/edid64.cpp）===")
        check("[EDID64] selftest PASS 必现", "[EDID64] selftest PASS" in log)
        m_pres = RX_PRESENT.search(log)
        if not m_pres:
            check("有 [EDID64] present=... 行", False, "（EDID 打点缺失）")
        else:
            present = int(m_pres.group(1))
            check("[EDID64] present=1（QEMU -vga std 实测提供内置 EDID）", present == 1,
                  "实际 present=%d，mfg=%s name=%s size=%s"
                  % (present, m_pres.group(4), m_pres.group(5).strip(), m_pres.group(6)))
            check("厂商 ID 是 EDID 3x5bit 解码出的 3 个字母",
                  bool(re.fullmatch(r"[A-Z]{3}", m_pres.group(4))), "mfg=" + m_pres.group(4))
            check("显示器名字非空且不是占位串",
                  m_pres.group(5).strip() not in ("", "unknown"), "name=%s" % m_pres.group(5).strip())
            check("EDID 版本已解析（1.3/1.4 口径）",
                  (int(m_pres.group(2)), int(m_pres.group(3))) >= (1, 3),
                  "ver=%s.%s" % (m_pres.group(2), m_pres.group(3)))
        check("没有走优雅降级路径（present=0）",
              "[EDID64] present=0" not in log)

        print("=== 3) 首选时序：pclk / Vactive / Hactive / 刷新率 ===")
        m_pref = RX_PREFERRED.search(log)
        if not m_pref:
            check("有 [EDID64] preferred ... refresh=<数字>Hz 行", False, "（首选时序打点缺失）")
        else:
            pclk = int(m_pref.group(1))
            vact = int(m_pref.group(2))
            hact = int(m_pref.group(3))
            ref_x10 = int(m_pref.group(4)) * 10 + int(m_pref.group(5))
            check("首选时序数值合理（pclk > 0、分辨率 >= 640x480）",
                  pclk > 0 and hact >= 640 and vact >= 480,
                  "pclk=%d Vactive=%d Hactive=%d refresh=%d.%dHz"
                  % (pclk, vact, hact, ref_x10 // 10, ref_x10 % 10))

            m_fb = RX_FB_PHYS.search(log)
            if not m_fb:
                check("有 [G64] fb ... phys=AxB 行", False)
            else:
                phys_w, phys_h = int(m_fb.group(3)), int(m_fb.group(4))
                # 硬证据：EDID 首选时序的 Hactive/Vactive 必须等于引导层实际设成 VBE 模式的
                # 物理分辨率。两条路径互不依赖（描述符位域解析 vs 引导层模式自适应）。
                check("EDID 首选 Hactive/Vactive == [G64] fb phys",
                      (hact, vact) == (phys_w, phys_h),
                      "EDID %dx%d vs fb phys %dx%d" % (hact, vact, phys_w, phys_h))

            m_det = RX_DETAIL.search(log)
            if not m_det:
                check("有 [EDID64] detail ... htotal/vtotal 行", False)
            else:
                htotal, vtotal = int(m_det.group(6)), int(m_det.group(7))
                # 复算：refresh_x10 = round(pclk_kHz * 10000 / (htotal * vtotal))
                den = htotal * vtotal
                want = (pclk * 10000 + den // 2) // den if den else 0
                check("refresh 与 pclk/(Htotal*Vtotal) 算术自洽",
                      want == ref_x10 and den > 0,
                      "htotal=%d vtotal=%d -> %.1fHz，打点 %.1fHz"
                      % (htotal, vtotal, want / 10.0, ref_x10 / 10.0))
                check("首选时序与消隐自洽（Hactive+Hblank 口径 > Hactive、Vtotal > Vactive）",
                      htotal > hact and vtotal > vact,
                      "htotal=%d > hact=%d，vtotal=%d > vact=%d" % (htotal, hact, vtotal, vact))

        print("=== 4) 设置-显示页真的显示 EDID 刷新率（打开设置应用）===")
        for _ in range(60):
            try:
                socket.create_connection(("127.0.0.1", args.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        mon = Monitor(args.port)
        opened = False
        for attempt in range(3):
            mon.key("meta_l", wait=1.2)
            time.sleep(0.4)
            mon.key("6", wait=2.5)          # 开始菜单第 6 项 = 设置
            if wait_for("[APP] settings opened", 12, "设置窗口打开（第 %d 次）" % (attempt + 1)):
                opened = True
                break
        check("设置应用能打开", opened)
        if opened:
            got = wait_for("[UI] settings display hz=", 15, "显示页画出刷新率来源")
            log = slog()
            m_hz = RX_SET_HZ.search(log)
            check("显示页真的打出了刷新率来源行（首帧一次）", got)
            check("设置-显示页刷新率来自 EDID（不是 unknown/不支持）",
                  bool(m_hz) and m_hz.group(2) == "edid",
                  ("hz=%s source=%s" % (m_hz.group(1), m_hz.group(2))) if m_hz else "（无该行）")
            if m_hz and m_pref:
                check("设置页显示的刷新率 == [EDID64] preferred 的刷新率",
                      abs(float(m_hz.group(1)) - ref_x10 / 10.0) < 0.05,
                      "%s vs %.1f" % (m_hz.group(1), ref_x10 / 10.0))
            check("显示页没有走\"未知（无 EDID）\"分支",
                  "hz=unknown source=none" not in log)

        print("=== 5) 禁止出现的失败标记 ===")
        log = slog()
        for bad in FORBIDDEN:
            check("不得出现 %s" % bad, bad not in log)

        print("--- 关键串口原文 ---")
        for pat in (r"\[EDID64\] present=", r"\[EDID64\] preferred", r"\[EDID64\] detail",
                    r"\[EDID64\] selftest", r"\[G64\] fb render=", r"\[UI\] settings display hz="):
            for line in log.splitlines():
                if re.search(pat, line):
                    print("   | " + line.strip()[:200])

        if args.keep:
            print("[display64] 串口日志：%s" % serial)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    print("=== RESULT: %s ===  checks=%d ok=%d"
          % ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
