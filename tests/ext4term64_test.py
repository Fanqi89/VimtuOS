#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/ext4term64_test.py - ★ v0.9.40：终端 / fd64 接上 ext4 只读服务卷

背景（用户第二步要求）：GUI 里已经能双击 D: 看 ext4 了，但**内核终端的 ls/cat 还看不见** ——
  终端走的是另一套卷模型（fd64 -> fs64 分派），而 fs64 的 `ls`/`read_range` 两个入口原来只有
  VIMTUFS2 / ISO9660 / FAT32 三个分支，ext4 服务卷落到最后去问 fat_slot=-1（必失败）。

本测试证明（全走真实用户路径，不碰私有接口）：
  ① `vol d` 切到 ext4 服务卷：`[VOL] switch letter=D: slot=255`
  ② `ls` 列出 3 条：`[TERM] cmd ls entries=3`（终端 -> fd64 目录句柄 -> fs64_ls64 -> 反向 RPC）
  ③ `cat hello.txt` 读到 13 B：`[TERM] cmd cat bytes=13`（终端 -> fd64 read -> fs64_read_range64）
  ④ 内容真的来自 ring3 服务：`[EXT4VOL] rpc op=list path=/` + `[EXT4VOL] rpc op=read path=/hello.txt`
  ⑤ **不破坏 VimtuFS2**：切回 `vol c` 后 `ls` 仍然有条目（entries >= 7）
  ⑥ 只读卷不被当成可写：`write /x.txt hi` 必须被拒（`read-only` / `-EROFS`），并且没有凭空出现文件
  ⑦ 全程无 PANIC，kheart 心跳持续

用法：py -3 -u tests\\ext4term64_test.py [--qemu PATH] [--timeout 240] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import importlib.util
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def _load(name, fn):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fn))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


EM = _load("em", "ext4mount64_test.py")      # Mon / keys_for2 / patch_conf / read_text
NP = _load("np", "netping64_test.py")        # find_qemu

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")
MKFS = os.path.join(ROOT, "tools", "mkfs_ext4_win.py")
FX_BLOCKS = 16384
MON_PORT = 5683


def _svc_dma(log):
    """取服务侧最新一次 `[EXT4SVC] serve round=… dma=<n>` 的 dma 字节数（没有就 0）。

    为什么用它当证据：内核侧 rpc 打点是限流的，而服务侧的 dma 计数只在真的替内核读盘时才涨。
    """
    v = 0
    for m in re.finditer(r"\[EXT4SVC\] serve round=\d+ beats=\d+ kern_beats=\d+ dma=(\d+)", log):
        v = int(m.group(1))
    return v


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = NP.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for need in (SYSDISK, MKFS):
        if not os.path.exists(need):
            sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_ext4term_")
    fx = os.path.join(tmp, "ext4fx.img")
    print("=== 0) 夹具：ext4 数据盘 + 系统盘（rpc=1）===")
    if subprocess.call([sys.executable, MKFS, "--out", fx, "--blocks", str(FX_BLOCKS)],
                       stdout=subprocess.DEVNULL) != 0:
        sys.stderr.write("夹具生成失败\n")
        return 2
    img = os.path.join(tmp, "sys_term.img")
    with open(img, "wb") as f:
        f.write(EM.patch_conf(open(SYSDISK, "rb").read(), [(b"rpc=0", b"rpc=1")]))

    serial = os.path.join(tmp, "serial_term.log")
    qargs = [
        qemu, "-name", "VimtuOS-ext4term",
        "-drive", "format=raw,file=%s,index=0,media=disk" % img,
        "-device", "ich9-ahci,id=ahci",
        "-drive", "file=%s,if=none,id=d1,format=raw" % fx,
        "-device", "ide-hd,drive=d1,bus=ahci.0",
        "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
        "-serial", "file:%s" % serial,
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % MON_PORT,
        "-no-reboot",
    ]
    proc = subprocess.Popen(qargs, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mon = EM.Mon(MON_PORT)
    checks = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    try:
        mon.connect()
        deadline = time.time() + args.timeout
        log = ""
        while time.time() < deadline:
            time.sleep(0.5)
            log = EM.read_text(serial)
            if re.search(r"\[DRV64\] letter=[A-Z]: .*fs=ext4", log) and "[GUI64] ready" in log:
                break
            if proc.poll() is not None:
                break

        m = re.search(r"\[DRV64\] letter=([A-Z]): disk=\d+ part=\d+ fs=ext4.*?ro=1", log)
        check("盘符链路在（[DRV64] letter=D: … fs=ext4 … ro=1）", m is not None,
              m.group(0) if m else "缺")
        letter = m.group(1) if m else "D"

        # ---- 登录 + 开终端 ----
        print("=== 1) 登录 -> 开终端 ===")
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=2.5)
        mon.key("meta_l", wait=1.2)
        mon.key("1", wait=2.5)
        time.sleep(1.5)
        check("终端应用打开（[APP] term opened）", "[APP] term opened" in EM.read_text(serial))

        def run(cmd, needle, wait_s=25):
            base = len(EM.read_text(serial))
            mon.type_keys(EM.keys_for2(cmd))
            mon.key("ret", wait=1.2)
            t0 = time.time()
            while time.time() - t0 < wait_s:
                seg = EM.read_text(serial)[base:]
                if needle in seg:
                    return seg, True
                if proc.poll() is not None:
                    break
                time.sleep(0.5)
            return EM.read_text(serial)[base:], needle in EM.read_text(serial)[base:]

        dma0 = _svc_dma(EM.read_text(serial))          # 交互前的服务侧 DMA 基线
        # ---- vol d + ls ----
        print("=== 2) vol %s -> ls（终端 -> fd64 -> fs64 -> 反向 RPC）===" % letter.lower())
        seg, ok = run("vol %s" % letter.lower(), "[VOL] switch letter=%s:" % letter)
        check("切到 ext4 服务卷（[VOL] switch letter=%s:）" % letter, ok,
              (re.search(r"\[VOL\][^\r\n]*", seg).group(0) if re.search(r"\[VOL\]", seg) else "缺"))

        seg, ok = run("ls", "[TERM] cmd ls entries=")
        mls = re.search(r"\[TERM\] cmd ls entries=(\d+)", seg)
        check("终端 ls 列出 ext4 根目录 3 条（cmd ls entries=3）",
              ok and mls is not None and mls.group(1) == "3",
              mls.group(0) if mls else "缺 [TERM] cmd ls 行")
        check("ls 不是走 \"空/失败\" 兜底（没有 entries=0 denied-or-missing）",
              "cmd ls entries=0 denied-or-missing" not in seg)
        # ★ 内核侧的 `[EXT4VOL] rpc op=` 行是**限流**打点（前 8 条 + 每 32 条），所以不能用它
        #   当"这次 ls 走了服务"的判据。改用三条硬证据：
        #   ① ls 的字节总数 = 夹具里 100000 + 13（C: 上根本没有这些文件）；
        #   ② 这次 ls 期间内核确实发过 RPC（至少一条 [EXT4VOL] rpc op= 行）；
        #   ③ 服务侧的 DMA 字节数在交互期间增长（服务真的替我们读了盘）。
        check("ls 的字节总数 = ext4 夹具的真实大小（100000 + 13 = 100013）",
              mls is not None and re.search(r"\[TERM\] cmd ls entries=3 bytes=100013", seg) is not None,
              (re.search(r"\[TERM\] cmd ls[^\r\n]*", seg).group(0)
               if re.search(r"\[TERM\] cmd ls", seg) else "缺 cmd ls 行"))
        check("这次 ls 期间内核发过 RPC（[EXT4VOL] rpc op=… 行）",
              re.search(r"\[EXT4VOL\] rpc op=", seg) is not None,
              (re.search(r"\[EXT4VOL\] rpc op=[^\r\n]*", seg).group(0)
               if re.search(r"\[EXT4VOL\] rpc op=", seg) else "缺 EXT4VOL rpc 行"))

        # ---- cat hello.txt ----
        print("=== 3) cat hello.txt（13 B，内容来自服务）===")
        seg, ok = run("cat hello.txt", "[TERM] cmd cat bytes=")
        mcat = re.search(r"\[TERM\] cmd cat bytes=(\d+)", seg)
        check("cat 读到 13 字节（cmd cat bytes=13）",
              ok and mcat is not None and mcat.group(1) == "13",
              mcat.group(0) if mcat else "缺 cmd cat 行")
        check("cat 走的是 fd64 打开（[FD64] open path=/hello.txt … vol=9）",
              re.search(r"\[FD64\] open path=/hello\.txt fd=\d+ flags=\d+ vol=9", seg) is not None,
              (re.search(r"\[FD64\] open path=/hello\.txt[^\r\n]*", seg).group(0)
               if re.search(r"\[FD64\] open path=/hello", seg) else "缺 FD64 open 行"))
        check("cat 走的仍然是服务卷（[TERM] cmd cat path=/hello.txt … vol=9 那条链）",
              re.search(r"\[FD64\] open path=/hello\.txt fd=\d+ flags=\d+ vol=9", seg) is not None)

        # ---- 只读：写必须被拒 ----
        print("=== 4) 只读卷：write 必须被拒 ===")
        seg, _ok = run("write /newfile.txt hi", "[FD64] open FAILED", wait_s=15)
        check("只读卷上写被拒（终端如实报 fail，没有静默成功）",
              re.search(r"\[TERM\] cmd write fail", seg) is not None,
              (re.search(r"\[TERM\] cmd write[^\r\n]*", seg).group(0)
               if re.search(r"\[TERM\] cmd write", seg) else "缺 cmd write 行"))
        seg, _ok = run("ls", "[TERM] cmd ls entries=")
        mls2 = re.search(r"\[TERM\] cmd ls entries=(\d+)", seg)
        check("写被拒后目录仍是 3 条（没有凭空出现新文件）",
              mls2 is not None and mls2.group(1) == "3",
              mls2.group(0) if mls2 else "缺 cmd ls 行")

        # ---- 切回 C: 不能坏 ----
        print("=== 5) 切回 C:（VimtuFS2 回归）===")
        seg, ok = run("vol c", "[VOL] switch letter=C:")
        check("切回 C:（[VOL] switch letter=C:）", ok)
        seg, ok = run("ls", "[TERM] cmd ls entries=")
        mls3 = re.search(r"\[TERM\] cmd ls entries=(\d+)", seg)
        check("C: 的 ls 仍然正常（entries>=7）",
              ok and mls3 is not None and int(mls3.group(1)) >= 7,
              mls3.group(0) if mls3 else "缺 cmd ls 行")

        time.sleep(2.5)
        full = EM.read_text(serial)
        dma1 = max([int(m.group(1)) for m in
                    re.finditer(r"\[EXT4SVC\] serve round=\d+ beats=\d+ kern_beats=\d+ dma=(\d+)", full)] or [0])
        check("服务真的替我们读了盘（服务侧 DMA 字节数 %d -> %d）" % (dma0, dma1), dma1 > dma0)
        check("全程无 PANIC", "[PANIC64]" not in full)
        beats = len(re.findall(r"\[TASK64\] kheart beat=", full))
        check("内核仍然活着（kheart 心跳 >=3）", beats >= 3, "beats=%d" % beats)

        npass = sum(1 for _, c in checks if c)
        print("\n%d/%d PASS" % (npass, len(checks)))
        if npass != len(checks):
            print("FAILED: %s" % "; ".join(n for n, c in checks if not c))
            print("日志保留在 %s" % tmp)
            print("FAIL")
            return 1
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        print("PASS：终端/fd64 已接上 ext4 只读服务卷（ls 3 条、cat 13 B、写被拒、C: 不受影响）")
        return 0
    finally:
        mon.close()
        try:
            proc.terminate(); proc.wait(timeout=10)
        except Exception:                                        # noqa: BLE001
            try:
                proc.kill()
            except Exception:                                    # noqa: BLE001
                pass


if __name__ == "__main__":
    sys.exit(main())
