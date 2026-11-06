#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/edid64_test.py - v0.9.22：显示器 EDID 读取/解析/上报的端到端验收

要证明的事（每条都有串口原文作证据）：
  ① **BIOS/VBE 路径**：引导层用 INT10 4F15 读到的 EDID 被内核解析出厂商/型号/首选时序
     （[EDID64] present=1 ver=1.x mfg=RHT name=QEMU Monitor）。
  ② **[EDID] 汇总行**：把"厂商 + 型号 + 产品码 + 物理尺寸 + 首选分辨率@刷新率 + 模式数"打到控制台（用户要求）。
  ③ **真实模式列表**：[EDID] mode i=… 逐条列出（首选在 i=0），模式数 >= 2，且不含 0x0 垃圾。
  ④ **DRM/KMS 上报**：`[DRM] getconnector … modes=N mm=WxH edid=1`（N >= 2、mm > 0），
     且 `[DRM] edid modes=N cur=WxH refresh=R matched=1`（当前模式的刷新率来自 EDID，不再是固定 60 估计）。
  ⑤ **显示层顺序修复**：`[DISP64] init … edid=<n>` 不再是 n/a（原来 display64 早于 edid64 初始化）。
  ⑥ **UEFI/GOP 路径**（-drive if=pflash 用 OVMF）：`U:modes=N`（模式表非空）+ `U:edid` 行给出
     ok=1 / bad header / none 三者之一（**如实回答**就算过，不假装拿到 EDID）。
  ⑦ 禁止项：PANIC / TRIPLE FAULT / [EDID64] selftest FAIL。

用法（必须 Windows 原生 Python）：py -3 tests\\edid64_test.py [--timeout 240] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")
ISO = os.path.join(ROOT, "vimtu64-64.iso")
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "[EDID64] selftest FAIL"]


def q(p):
    return '"%s"' % p


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.exists(c) or c == "qemu-system-x86_64":
            try:
                subprocess.run([c, "--version"], capture_output=True, timeout=20)
                return c
            except Exception:                                   # noqa: BLE001
                continue
    return None


def find_ovmf():
    """OVMF（edk2 x86_64）code fd：优先用 proc64_test 的实现（各测试共用一处）。"""
    try:
        import proc64_test as p64
        return p64.find_ovmf(), p64._ovmf_vars()
    except Exception:                                          # noqa: BLE001
        pass
    cands = [
        r"C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        r"C:\Program Files (x86)\qemu\share\edk2-x86_64-code.fd",
    ]
    for c in cands:
        if os.path.exists(c):
            v = os.path.join(os.path.dirname(c), "edk2-i386-vars.fd")
            return c, (v if os.path.exists(v) else None)
    return None, None


def read(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def boot(qemu, args, serial, want, timeout):
    """跑一个 VM 直到串口里出现 want（或超时），然后杀掉。返回日志文本。"""
    cmd = [qemu, "-m", "512", "-display", "none", "-serial", "file:%s" % serial, "-no-reboot"] + args
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    try:
        while time.time() - t0 < timeout:
            if want in read(serial):
                time.sleep(3.0)          # 再等一小会儿，把后面的行也收全
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:                                  # noqa: BLE001
                pass
    return read(serial)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-64s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    for p in (DISK, ISO):
        if not os.path.exists(p):
            print("环境问题：缺少构建产物 %s（先跑 bash build64.sh）" % p)
            return 2

    tmp = tempfile.mkdtemp(prefix="edid64_")
    print("=== Vimtu64 EDID 验收（BIOS/VBE + UEFI/GOP 两条引导路径）===")

    # ---------------- ① BIOS 路径 ----------------
    print("=== 1) BIOS/VBE 路径（QEMU -vga std 的 EDID：RHT / QEMU Monitor / 1280x800）===")
    ser1 = os.path.join(tmp, "bios.log")
    log = boot(qemu, ["-drive", "format=raw,file=%s" % DISK, "-boot", "order=c", "-vga", "std"],
               ser1, "[DRM] getconnector", args.timeout)

    m = re.search(r"\[EDID64\] present=1 ver=(\d+)\.(\d+) mfg=(\S+) name=(.+)", log)
    check("① 引导层 EDID 被解析（[EDID64] present=1 mfg=<厂商> name=<型号>）",
          m is not None, m.group(0) if m else "缺 [EDID64] present=1 行")
    check("① 厂商/型号是真实的（RHT / QEMU Monitor，不是 ??? / unknown）",
          m is not None and m.group(3) == "RHT" and "QEMU Monitor" in m.group(4),
          (m.group(0) if m else ""))

    e = re.search(r"\[EDID\] mfg=(\S+) name=(.+) product=(\d+) serial=(\d+) year=(\d+) "
                  r"cm=(\S+) preferred=(\d+)x(\d+)@([\d.]+)Hz modes=(\d+) src=(\S+)", log)
    check("② 控制台打出 [EDID] 汇总行（厂商+型号+产品码+尺寸+首选+刷新率+模式数）",
          e is not None, e.group(0) if e else "缺 [EDID] 汇总行")
    if e:
        check("② 首选分辨率 = 1280x800（QEMU 面板的原生模式，不是盲猜的 1024x768）",
              (e.group(7), e.group(8)) == ("1280", "800"), "%sx%s" % (e.group(7), e.group(8)))
        check("② 模式数 >= 2（真的解析出了模式列表，不是只有一条）",
              int(e.group(10)) >= 2, "modes=%s" % e.group(10))
        check("② 物理尺寸有值（cm=WxH，不是 unknown）",
              re.match(r"^\d+x\d+$", e.group(6)) is not None, "cm=%s" % e.group(6))
    modes = re.findall(r"\[EDID\] mode i=(\d+) (\d+)x(\d+)@([\d.]+)Hz kind=(\d)", log)
    check("③ 模式列表逐条列出（[EDID] mode i=… 至少 2 条）", len(modes) >= 2,
          "n=%d 前两条=%s" % (len(modes), modes[:2]))
    check("③ 首选模式排在 i=0，且就是上面那条 preferred",
          len(modes) >= 1 and modes[0][0] == "0" and e is not None and
          (modes[0][1], modes[0][2]) == (e.group(7), e.group(8)),
          modes[0] if modes else "无")
    check("③ 模式里没有 0x0 这类垃圾项", all(int(m2[1]) >= 640 and int(m2[2]) >= 400 for m2 in modes),
          str([(m2[1], m2[2]) for m2 in modes]))

    d = re.search(r"\[DISP64\] init modes=(\d+) cur=(\d+)x(\d+)@(\S+) src=(\w+)", log)
    check("⑤ 显示层顺序修复：src 不再是 none（EDID 在 display64 之前解析）",
          d is not None and d.group(5) in ("edid", "vga", "crtc"),
          d.group(0) if d else "缺 [DISP64] init 行")
    check("⑤ [DISP64] 的 edid 字段不再是 n/a",
          re.search(r"\[DISP64\] report .* edid=(?!n/a)([\d.]+)", log) is not None or
          re.search(r"\[DISP64\] init .*src=edid", log) is not None,
          (re.search(r"\[DISP64\] report[^\r\n]*", log).group(0)
           if re.search(r"\[DISP64\] report", log) else "（没打 report 行）"))

    dr = re.search(r"\[DRM\] edid modes=(\d+) cur=(\d+)x(\d+) refresh=(\d+) matched=(\d) "
                   r"conn_modes=(\d+) mm=(\d+)x(\d+)", log)
    check("④ 内核把 EDID 接到 DRM：当前模式的刷新率取自 EDID（matched=1，不再是固定 60 估计）",
          dr is not None and dr.group(5) == "1" and int(dr.group(1)) >= 2,
          dr.group(0) if dr else "缺 [DRM] edid 行")
    check("④ GETCONNECTOR 将上报的真实模式数 >= 3（= 1 个当前 + EDID 的 N 个）",
          dr is not None and int(dr.group(6)) >= 3, ("conn_modes=%s" % dr.group(6)) if dr else "")
    check("④ GETCONNECTOR 将上报的物理尺寸 > 0（来自 EDID 的 cm 字段 ×10）",
          dr is not None and int(dr.group(7)) > 0 and int(dr.group(8)) > 0,
          ("mm=%sx%s" % (dr.group(7), dr.group(8))) if dr else "")

    for bad in FORBIDDEN:
        check("⑦ BIOS 路径不得出现 %s" % bad, bad not in log, "")
    if args.keep:
        print("    串口日志：%s" % ser1)

    # ---------------- ② UEFI 路径 ----------------
    print("=== 2) UEFI/GOP 路径（OVMF；真机走的就是这条）===")
    ovmf, ovmf_vars = find_ovmf()
    if not ovmf:
        check("⑥ UEFI 路径可测（找到 OVMF edk2 code fd）", False, "本机没有 edk2-x86_64-code.fd")
    else:
        ser2 = os.path.join(tmp, "uefi.log")
        varsf = os.path.join(tmp, "vars.fd")
        if ovmf_vars and os.path.exists(ovmf_vars):
            shutil.copyfile(ovmf_vars, varsf)
        else:
            with open(varsf, "wb") as f:
                f.write(b"\x00" * 540672)
        # ★ 改用一个**真有 EFI 启动项**的介质：安装 ISO 有 El Torito UEFI 镜像（/EFI/BOOT/BOOTX64.EFI），
        #   而 sysdisk.img 的 ESP 在 OVMF 下没有启动项、会掉进 UEFI Shell（实测过）。
        uargs = ["-drive", "if=pflash,format=raw,unit=0,readonly=on,file=%s" % ovmf,
                 "-drive", "if=pflash,format=raw,unit=1,file=%s" % varsf,
                 "-drive", "media=cdrom,readonly=on,file=%s" % ISO, "-boot", "order=d"]
        log2 = boot(qemu, uargs, ser2, "U:edid", args.timeout)
        if "U:edid" not in log2:
            # 有些固件把串口初始化得更晚：再等一轮，并接受 "U:modes=" 已出现的情况
            time.sleep(5)
            log2 = read(ser2)
        mv = re.search(r"U:modes=(\d+) max=(\d+)", log2)
        check("⑥ UEFI 路径枚举了 GOP 模式表（U:modes=N，原来恒为 0）—— 这就是「不再盲猜」的底料",
              mv is not None and int(mv.group(1)) >= 2, mv.group(0) if mv else "缺 U:modes= 行")
        ed = re.search(r"U:edid (ok=1 src=(\w+) size=(\d+) preferred=(\d+)x(\d+)|"
                       r"none \([^)]*\)|bad header/checksum[^\r\n]*)", log2)
        check("⑥ UEFI 的 EDID 结论是**如实的**（ok=1 / none / bad header 三者之一）",
              ed is not None, (ed.group(0) if ed else "既没有 U:edid ok=1、也没有 U:edid none —— "
                                                     "说明这条路径没跑到（要查 OVMF 是否把 KERNEL64.BIN 拉起来了）"))
        ed_ok = "U:edid ok=1" in log2
        if ed is not None and ed_ok:
            check("⑥ EDID 拿到后按首选分辨率选模式（U:setmode …）",
                  re.search(r"U:setmode (ok|skipped)", log2) is not None,
                  (re.search(r"U:setmode[^\r\n]*", log2).group(0)
                   if re.search(r"U:setmode", log2) else "缺 U:setmode 行"))
        else:
            print("    [info] 这台固件没提供 EDID 协议（U:edid none）—— 代码路径正确、结论如实；")
            print("           真机上大多数 UEFI 固件会实现 EFI_EDID_DISCOVERED/ACTIVE，届时会走 ok=1 分支。")
        for bad in ("PANIC", "TRIPLE FAULT"):
            check("⑦ UEFI 路径不得出现 %s" % bad, bad not in log2, "")
        if args.keep:
            print("    串口日志：%s" % ser2)

    npass = sum(1 for _n, c, _d in checks if c)
    nfail = len(checks) - npass
    print("\n==== edid64_test：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, nfail))
    if nfail:
        print("失败项：")
        for n, c, d in checks:
            if not c:
                print("  [FAIL] %s   %s" % (n, d))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
