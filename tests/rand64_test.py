#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/rand64_test.py - ★ v0.12.3：密码学级随机数（CSPRNG）验收

为什么这项要单独一个脚本：TLS 的所有秘密（会话密钥 / client random / ECDHE 私钥 / DRBG 种子）
都从 `getrandom(318)` 来。旧实现是 "TSC + ticks + xorshift"，自述"非密码学安全" —— 那种东西
做 TLS 等于没加密。本脚本验的是**两条路径都真的在工作**：

  ① 默认 CPU（QEMU `qemu64`，**没有** RDRAND）：走 SHA-256 计数器 DRBG，种子来自 TSC 抖动
     -> 判据 `[RAND64] selftest PASS … rdrand=0 … seed_src=tsc-jitter seed_bytes=32`
  ② `-cpu max`（有 RDRAND/RDSEED）：走硬件熵
     -> 判据 `… rdrand=1 … seed_src=rdseed|rdrand`

另加分布类判据（防止"退化成一个常数"这个最常见故障）：
  · `hottest=<n>` ≤ 12（1024 字节落进 256 个桶，期望 4，上限 3 倍）
  · `sample=` 不是 00000000

用法：py -3 tests/rand64_test.py [--img build64/system.img] [--port 5691]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
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


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


FAILS = []


def check(name, ok, detail=""):
    print("[%s] %s%s" % ("PASS" if ok else "FAIL", name,
                         ("  |  " + str(detail)) if detail else ""))
    if not ok:
        FAILS.append(name)
    return ok


def boot_and_grab(qemu, img, cpu, wait_s=300, name="vimtu-rand64"):
    """起一次 QEMU，等到 [RAND64] selftest 一行出现（或超时），返回（串口全文, 是否出现）"""
    tmp = tempfile.mkdtemp(prefix="vimtu64_rand_")
    serial = os.path.join(tmp, name + "_serial.log")
    cmd = [qemu, "-name", name, "-drive", "format=raw,file=%s" % q(img),
           "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
           "-serial", "file:%s" % q(serial), "-no-reboot"]
    if cpu:
        cmd += ["-cpu", cpu]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    seen = False
    try:
        while time.time() - t0 < wait_s:
            try:
                with open(serial, "r", encoding="utf-8", errors="replace") as f:
                    data = f.read()
            except OSError:
                data = ""
            if "[RAND64] selftest" in data:
                seen = True
                time.sleep(1.0)          # 让它把这一行写完
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                data = f.read()
        except OSError:
            data = ""
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    return data, seen


def parse(line):
    m = re.search(r"\[RAND64\] selftest (\w+).*?rdrand=(\d) rdseed=(\d) seed_src=(\S+) "
                  r"seed_bytes=(\d+) hottest=(\d+) sample=([0-9A-Fa-f]+)", line)
    if not m:
        return None
    return {"verdict": m.group(1), "rdrand": int(m.group(2)), "rdseed": int(m.group(3)),
            "seed_src": m.group(4), "seed_bytes": int(m.group(5)),
            "hottest": int(m.group(6)), "sample": m.group(7)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统镜像 %s（先跑 bash build64.sh）" % args.img)
        return 2

    print("=== ① 默认 CPU（qemu64：没有 RDRAND）→ 必须走 SHA-256 DRBG + TSC 抖动播种 ===")
    log1, seen1 = boot_and_grab(qemu, args.img, None)
    lines1 = [l for l in log1.splitlines() if "[RAND64]" in l]
    if not check("出现 [RAND64] selftest 打点", seen1, lines1[0].strip() if lines1 else "未出现"):
        return 1
    r1 = parse(lines1[0])
    if not check("打点格式可解析（verdict/rdrand/rdseed/seed_src/seed_bytes/hottest/sample）",
                 r1 is not None, lines1[0].strip()[:160]):
        return 1
    check("① 默认 CPU 自检 PASS", r1["verdict"] == "PASS", r1["verdict"])
    check("① 默认 CPU 没有硬件熵（rdrand=0）", r1["rdrand"] == 0, "rdrand=%d" % r1["rdrand"])
    check("① 降级路径写明种子来源（seed_src=tsc-jitter）", r1["seed_src"] == "tsc-jitter",
          "seed_src=%s seed_bytes=%d" % (r1["seed_src"], r1["seed_bytes"]))
    check("① 播了足够熵（seed_bytes >= 32）", r1["seed_bytes"] >= 32, str(r1["seed_bytes"]))
    check("① 字节分布不退化（hottest <= 12，期望 4）", r1["hottest"] <= 12, "hottest=%d" % r1["hottest"])
    check("① 输出不是常数（sample != 00000000）", r1["sample"].lower() != "00000000", r1["sample"])
    check("① 全程无 PANIC", "PANIC" not in log1)

    print("=== ② -cpu max（有 RDRAND/RDSEED）→ 必须用硬件熵 ===")
    log2, seen2 = boot_and_grab(qemu, args.img, "max")
    lines2 = [l for l in log2.splitlines() if "[RAND64]" in l]
    if not check("出现 [RAND64] selftest 打点（-cpu max）", seen2,
                 lines2[0].strip() if lines2 else "未出现"):
        return 1
    r2 = parse(lines2[0])
    check("-cpu max 自检 PASS", r2 is not None and r2["verdict"] == "PASS",
          lines2[0].strip()[:160])
    if r2:
        check("② CPU 报出 RDRAND（rdrand=1）", r2["rdrand"] == 1, "rdrand=%d" % r2["rdrand"])
        check("② 种子来自硬件（seed_src=rdseed 或 rdrand）",
              r2["seed_src"] in ("rdseed", "rdrand"), r2["seed_src"])
        check("② 分布不退化（hottest <= 12）", r2["hottest"] <= 12, "hottest=%d" % r2["hottest"])
        check("② 输出不是常数（sample != 00000000）", r2["sample"].lower() != "00000000", r2["sample"])
    check("② 全程无 PANIC", "PANIC" not in log2)
    if r1 and r2:
        check("两条路径的输出不同（seed_src 不同 = 真降级过）",
              r1["seed_src"] != r2["seed_src"],
              "%s vs %s" % (r1["seed_src"], r2["seed_src"]))

    print("")
    if FAILS:
        print("RESULT: FAIL —— %d 项：%s" % (len(FAILS), "; ".join(FAILS)))
        return 1
    print("RESULT: PASS —— CSPRNG 两条路径都验过（无硬件熵时 SHA-256 DRBG，有 RDRAND 时走硬件）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
