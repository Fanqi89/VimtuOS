#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/boot64_assert.py - Vimtu64 自动启动验收（无需人看屏幕）

原理：用 QEMU 无头启动镜像，把两条调试通道分别写到文件，再对日志做阶段断言。
      这是 64 位移植过程中唯一可靠的回归手段（本机 VT-x 未启用，只能用 TCG 软件模拟）。

两条通道有什么区别（这是脚本里 must_serial / must_debugcon 分开的原因）：
  * 串口 COM1(0x3F8)：干净通道。BIOS 不写它，只有 loader 与内核写 -> 断言内核阶段用它。
  * debugcon(0x402) ：**QEMU 的 SeaBIOS 自己也在往这里写**（"SeaBIOS (version ...)"），
                      所以它混了固件日志。断言 BIOS/引导扇区/loader 阶段用它。

用法：
    python tests/boot64_assert.py
    python tests/boot64_assert.py --stage m0 --timeout 40 --keep-log /tmp/serial.log
    python tests/boot64_assert.py --img vimtu64-64.img

退出码：0 = 全部断言通过；1 = 有断言失败；2 = 环境问题（QEMU/镜像缺失）
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

STAGES = {
    # M0：BIOS -> loader64 -> x86_64 长模式 -> 64 位 C++ 代码 + BootInfo/E820/LFB 可读
    "m0": {
        "must_serial": [
            "[LM64] ENTERED LONG MODE",     # 已进入 x86_64 长模式（EFER.LMA=1）
            "[LM64] BSS CLEARED",           # 64 位 C++ 代码已执行
            "[LM64] BOOTINFO OK",           # loader 写的 BootInfo(magic AUR1) 读到了
            "[LM64] LFB addr=",             # VBE 图形模式参数可读
            "[LM64] E820",                  # E820 内存地图可读
            "[LM64] M0 PASS",               # 本阶段终点
        ],
        # 注意：BIOS 阶段的横幅（MBR 的 "Aurora32 boot"、loader 的 "Vimtu32/64 Loader"）
        # 走 INT 10h 直接写显存，既不进串口也不进 debugcon，因此这里不断言它们；
        # 后续这些 dbg 标记足以证明引导链每一段都真的执行过。
        "must_debugcon": [
            "L:edid ok",                    # 实模式阶段：VBE + EDID 探测成功
            "L:bootinfo",                   # loader 写好 BootInfo（boot.bin 已执行）
            "L:ata",                        # 读内核阶段结束（读盘已由 BIOS INT 13h / ATAPI 完成）
            # ★ 新增：磁盘引导（裸盘/硬盘）路径必须由 BIOS INT 13h 扩展读内核
            #   （旧版是自写 PATA PIO —— 在 SATA=AHCI 的机器上读不到盘，装完系统起不来）
            "[LM] disk boot via INT 13h dl=0x",
            "L:lm64",                       # 准备切长模式
        ],
        # "HALT:" 不算失败：正常跑完会停在 HALT 提示上（见 must_serial 的 M0 PASS）
        "forbidden": ["PANIC", "TRIPLE FAULT", "not in long mode", "bad bootinfo magic",
                      "KERNEEL", "disk read error"],
    },

    # M1：在 M0 基础上要求"64 位中断真的在跑"（PIC/IDT/TSS/PIT + 中断桩 + 帧布局）
    "m1": {
        "must_serial": [
            "[LM64] ENTERED LONG MODE",
            "[LM64] BOOTINFO OK",
            "[X64] PIC remapped, IDT/TSS loaded, PIT 250Hz",   # 平台层初始化完成
            "[LM64] PIT IRQ OK",        # 等到 12 个 PIT tick -> 中断投递 + iretq 返回都正确
            "irq_total=",               # 中断计数非零：PIT 确实在投递并被处理
            "[LM64] M0 PASS",
        ],
        "must_debugcon": ["L:lm64", "L:ata"],
        # 这些出现即说明中断路径有问题
        "forbidden": ["PANIC", "TRIPLE FAULT", "[EXC]", "cpu exception",
                      "PIT IRQ TIMEOUT", "unhandled", "no hardware IRQ"],
    },
}


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


def run_qemu(qemu, img, seconds):
    tmpdir = tempfile.mkdtemp(prefix="vimtu64_assert_")
    serial_path = os.path.join(tmpdir, "serial.log")
    dbgcon_path = os.path.join(tmpdir, "debugcon.log")

    def q(p):
        return p.replace("\\", "/")

    args = [
        qemu,
        "-name", "Vimtu64-assert",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c",
        "-m", "512",
        "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial_path),
        "-debugcon", "file:%s" % q(dbgcon_path),
        "-global", "isa-debugcon.iobase=0x402",
        "-no-reboot",
    ]
    sys.stderr.write("[assert] %s\n" % " ".join(args))
    proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    deadline = time.time() + seconds
    early = False
    try:
        while time.time() < deadline:
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

    def read(path):
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except FileNotFoundError:
            return ""

    return read(serial_path), read(dbgcon_path), early


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--stage", default="m0", choices=sorted(STAGES.keys()))
    ap.add_argument("--img", default=os.path.join(ROOT, "vimtu64-64.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=40)
    ap.add_argument("--keep-log", default=None)
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("image not found: %s (run build64.sh first)\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("qemu-system-x86_64 not found\n")
        return 2

    serial, dbgcon, early = run_qemu(qemu, args.img, args.timeout)
    if args.keep_log:
        with open(args.keep_log, "w", encoding="utf-8") as f:
            f.write("### SERIAL (COM1)\n" + serial + "\n### DEBUGCON (0x402)\n" + dbgcon)

    spec = STAGES[args.stage]
    combined = serial + "\n" + dbgcon
    ok = True

    print("=== Vimtu64 boot acceptance (stage=%s, %ss) ===" % (args.stage, args.timeout))

    def check(needle, text, label):
        nonlocal ok
        hit = needle in text
        ok = ok and hit
        print("  [%s] (%s) %s" % ("PASS" if hit else "FAIL", label, needle))

    for needle in spec["must_serial"]:
        check(needle, serial, "serial")
    for needle in spec["must_debugcon"]:
        check(needle, dbgcon, "debugcon")
    for needle in spec["forbidden"]:
        hit = needle in combined
        ok = ok and not hit
        print("  [%s] must NOT appear: %s" % ("FAIL" if hit else "PASS", needle))

    print("--- serial tail ---")
    for line in [l for l in serial.splitlines() if l.strip()][-16:]:
        print("   | " + line[:200])
    if early:
        print("note: QEMU exited before timeout (possible triple fault / reset)")
    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
