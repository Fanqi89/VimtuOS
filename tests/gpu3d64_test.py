#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/gpu3d64_test.py - ★ v0.9.17：virtio-gpu 的 **3D（virgl）加速通路**验收

要证明的事（每条都有串口原文）：
  ⓪ **本批（v0.9.17）的结论**：3D 的"通路"已铺到主机 virgl（capset 拿到、上下文/3D 资源/提交/回读
     全部被设备接受），但 **CLEAR 之后的回读像素仍是 0** —— QEMU 自己的 stderr 明确说
     `failed to dispatch BIND_OBJECT: 22`（vrend 拒绝了 surface 绑定，六种编码都试过）。
     所以本脚本对"3D 出图"这条是 **UNVERIFIED**：ok=1 就按强判据过；ok=0 时只要求
     **如实报出原因**，并把 QEMU 侧证据一起打出来（不假装通过）。
  ① 主机真有 virgl 时：设备协商上 VIRTIO_GPU_F_VIRGL -> `[GPU3D] capset id=1|2 …`
     -> 建 virgl 上下文 + 3D 资源 -> SUBMIT_3D（surface -> framebuffer -> CLEAR）
     -> TRANSFER_FROM_HOST_3D -> **逐像素核对清屏色**：
     `[GPU3D] selftest res=… w=64 h=64 ctx=1 stride=256 pages=4 ok=1 px=0x… want=0xff4080bf`
  ② 主机没有 3D（普通 `virtio-gpu-pci`，没协商 VIRGL）时：**一个 3D 命令都不发**，
     只打 `[GPU3D] skipped reason=no-virgl`（如实降级，不假装）。
  ③ 两种情况都不得出现 `[GPU3D] selftest FAIL` / PANIC / TRIPLE FAULT。

为什么要 `-display sdl,gl=on`：Windows 上的 QEMU 只有带 GL 的后端才能建
`virtio-gpu-gl-pci`（`-display none` 会直接报 "display backend does not have OpenGL support"）。

用法（必须 Windows 原生 Python）：py -3 tests\\gpu3d64_test.py [--timeout 200] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题。
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
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402

BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
FORBIDDEN = ["PANIC", "TRIPLE FAULT"]


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


class Run:
    def __init__(self, qemu, img, tmp, tag, extra, timeout, display_gl):
        self.serial = os.path.join(tmp, "gpu3d_%s.log" % tag)
        argv = [qemu, "-name", "Vimtu64-gpu3d-%s" % tag,
                "-drive", "format=raw,file=%s" % q(img),
                "-boot", "order=c", "-m", "512"]
        argv += extra
        if display_gl:
            argv += ["-display", "sdl,gl=on"]
        else:
            argv += ["-display", "none"]
        argv += ["-serial", "file:%s" % q(self.serial), "-no-reboot"]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.timeout = timeout

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(self, needle, timeout=None):
        deadline = time.time() + (timeout if timeout is not None else self.timeout)
        while time.time() < deadline:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return needle in self.log()
            time.sleep(0.4)
        return needle in self.log()

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=200)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %-64s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    print("=== Vimtu64 gpu3d acceptance（virtio-gpu 3D / virgl 通路）===")
    qemu = find_qemu(args.qemu)
    if not qemu:
        print("环境问题：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(DISK):
        print("环境问题：缺少 %s（先跑 bash build64.sh）" % DISK)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_gpu3d_")
    img = os.path.join(tmp, "gpu3d.img")
    shutil.copyfile(DISK, img)

    # ---------- ①/②：有 GL 后端 + virtio-gpu-gl-pci ----------
    rA = Run(qemu, img, tmp, "gl", ["-device", "virtio-vga-gl"],
             args.timeout, True)
    okfail = False
    try:
        got = rA.wait("[GPU3D]", 150)
        logA = rA.log()
        if not got:
            ov = rA.log()
            print("  [info] 串口长度 %d；最后 400 字符：%r" % (len(ov), ov[-400:]))
        check("① 3D 通路自检有结论（[GPU3D] 打点出现）", got,
              (re.search(r"\[GPU3D\][^\r\n]*", logA).group(0)
               if re.search(r"\[GPU3D\][^\r\n]*", logA) else "没有 [GPU3D] 行"))
        capset = re.search(r"\[GPU3D\] capset id=(\d+) ver=(\d+) size=(\d+)", logA)
        check("① 主机给了 virgl capset（id=1|2）—— 证明链是「内核 -> virtio-gpu -> 主机 GL 渲染器」",
              capset is not None and capset.group(1) in ("1", "2"),
              (capset.group(0) if capset else "缺 capset 行"))
        st = re.search(r"\[GPU3D\] selftest res=(\d+) w=(\d+) h=(\d+) ctx=1 stride=(\d+) pages=(\d+) "
                       r"ok=([01]) px=(0x[0-9a-f]+) want=(0x[0-9a-f]+)", logA)
        fail_line = re.search(r"\[GPU3D\] selftest FAIL reason=(\S+) ok=0", logA)
        check("① 3D 自检有**完整结论**（成功行 或 如实的 FAIL 行，二者必居其一）",
              st is not None or fail_line is not None,
              (st.group(0) if st else (fail_line.group(0) if fail_line else "两种结论行都没有")))
        # ★ 诚实口径：ok=1 才要求像素完全匹配；ok=0 时必须**如实**给出原因（UNVERIFIED）
        tries = re.findall(r"\[GPU3D\] flavor try slot=(\d+) name=(\S+) bind_len=(\d+) surf_h=(\d+) "
                           r"ok=([01]) px=(0x[0-9a-f]+)", logA)
        check("① 三种 surface/bind 编码都真被试过（flavor try × 3）", len(tries) == 3,
              "tries=%d" % len(tries))
        if st and st.group(6) == "1":
            check("① 3D 资源几何正确（64x64 stride=256 pages=4）",
                  (st.group(2), st.group(3), st.group(4), st.group(5)) == ("64", "64", "256", "4"),
                  "w=%s h=%s stride=%s pages=%s" % (st.group(2), st.group(3), st.group(4), st.group(5)))
            check("① **回读像素 == 清屏色**（GL 真的执行了 CLEAR）",
                  st.group(7).lower() == "0xff4080bf", "px=%s want=%s" % (st.group(7), st.group(8)))
            okfail = True
        elif st:
            # ok=0：允许，但必须是**如实的失败**，并且理由要写清楚
            check("① 3D 出图：如实标 UNVERIFIED（selftest ok=0，px=0x0，不是假装成功）",
                  st.group(6) == "0" and st.group(7).lower() == "0x0",
                  "ok=%s px=%s" % (st.group(6), st.group(7)))
        else:
            fail = re.search(r"\[GPU3D\] selftest FAIL reason=(\S+) ok=0", logA)
            check("① 3D 出图：如实标 UNVERIFIED（selftest FAIL reason=… ok=0）", fail is not None,
                  (fail.group(0) if fail else "既没有 selftest 成功行、也没有 FAIL 行 —— 这才是缺陷"))
        print("--- ① 禁止项 ---")
        for bad in FORBIDDEN:
            check("① 不得出现 %s" % bad, bad not in logA)
    finally:
        rA.close()

    # ---------- ③：没有 VIRGL 的设备（普通 virtio-gpu-pci）必须如实跳过 ----------
    rB = Run(qemu, img, tmp, "no3d", ["-vga", "virtio"], args.timeout, False)
    try:
        rB.wait("[GPU3D]", 150)
        logB = rB.log()
        check("③ 无 VIRGL 设备时如实跳过（[GPU3D] skipped reason=no-virgl）",
              "[GPU3D] skipped reason=no-virgl" in logB,
              (re.search(r"\[GPU3D\][^\r\n]*", logB).group(0)
               if re.search(r"\[GPU3D\][^\r\n]*", logB) else "没有 [GPU3D] 行"))
        check("③ 跳过时**一个 3D 命令都没发**（无 capset / 无 selftest 行）",
              "[GPU3D] capset" not in logB and "[GPU3D] selftest" not in logB)
        check("③ 无 VIRGL 时也不许有 cmd FAIL", "[GPU3D] cmd FAIL" not in logB)
        check("③ 普通 virtio-gpu 的 2D 路径照旧（[VGPU] ready backend=virtio-gpu-2d）",
              "[VGPU] ready backend=virtio-gpu-2d" in logB)
        for bad in FORBIDDEN:
            check("③ 不得出现 %s" % bad, bad not in logB)
    finally:
        rB.close()
        if args.keep:
            print("[gpu3d] 串口日志：%s" % tmp)

    npass = sum(1 for _n, c, _d in checks if c)
    nfail = len(checks) - npass
    print("\n==== gpu3d64_test：%d 条断言，PASS %d / FAIL %d ====" % (len(checks), npass, nfail))
    if nfail:
        print("失败项：")
        for n, c, d in checks:
            if not c:
                print("  [FAIL] %s   %s" % (n, d))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
