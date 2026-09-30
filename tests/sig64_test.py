#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/sig64_test.py - ★ A4-5 验收①：**信号投递**（ring3 真收到信号 + 用户栈信号帧 + rt_sigreturn）

要证明的事（与任务书的七条断言一一对应；证据 = 内核 [SIG64] 打点 + 用户程序 sig64: 输出）：
  ① handler 投递与 rt_sigreturn 后继续执行：
       `[SIG64] deliver pid=.. sig=.. handler=0x.. frame=0x.. where=..` + `[SIG64] sigreturn pid=.. sig=..`
       + 用户程序 handler 里的 `sig64: handler sig=.. count=..` 与返回后的主流程继续输出；
  ② 默认动作终止 + wait4 能看到 128+sig：
       `[SIG64] default action pid=.. sig=15 exit=143` + 用户侧 `WEXITSTATUS=143`；
  ③ SIGKILL 不可捕获：`sigaction(SIGKILL)` -> EINVAL（用户侧 rc=-1）+ 真 SIGKILL 的子进程退出码 137；
  ④ rt_sigprocmask 真生效：阻塞期间 `pending=0x1000`（只挂未决）-> 解除后 `deliver ... where=syscall`；
     用户侧时序证据 `blocked: seen=0 (expect 0)` / `unblocked: seen=1 (expect 1)`；
  ⑤ 用户态错误地址访问**只杀该进程**、内核继续跑：
       `[SIG64] fault pid=.. no=14 #PF .. -> sig=11` + `default action pid=.. sig=11 exit=139`，
       而且 `[PANIC] cpu exception` **一条都不能有**（以前这就是整机 PANIC）；
       顺带 #GP（wrmsr）-> 139、#DE（除零）-> 136；
  ⑥ kill(-pgid) 广播：两个子进程 + 父进程都注册了同号 handler，`send-group pgrp=.. n=3`，
     三边都打出 `handler sig=10`；
  ⑦ 全程无内核 PANIC（`[PANIC]` / TRIPLE FAULT 都不许出现），且演示收尾行齐全。

夹具：build64/system.img（系统内核）+ 标准 MBR + **空** VimtuFS2 主分区（与 proc64_test 同一套）。
  为什么用空卷：/sig64.elf 是**内嵌 blob**、启动期由内核自己装进卷 —— 不需要任何卷内容，
  而且空卷上没有 /bin/edit，编辑器演示会如实打一行 skipped（本脚本不测它，见 tests/edit64_test.py）。

用法（必须 Windows 原生 Python）：
    py -3 tests\\sig64_test.py                 # 默认无头 QEMU（日志断言，轻活）
    py -3 tests\\sig64_test.py --img <已装好的盘> --timeout 300 --keep
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh             # noqa: E402（公共工具：空闲端口 / monitor 客户端）

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSTEM_IMG = os.path.join(ROOT, "build64", "system.img")
FIXTURE_IMG = os.path.join(ROOT, "build64", "sig64_test.img")
SECTOR = 512
PART_BOOT_LBA = 9
PART_BOOT_SECS = 8000
PART_MAIN_LBA = PART_BOOT_LBA + PART_BOOT_SECS        # 8009（与 kernel/part64.h 一致）
TARGET_SECTORS = 32768                                # 16 MB

FORBIDDEN = [
    "[PANIC]",
    "TRIPLE FAULT",
    "[SIG64] selftest FAIL",
    "[SIG64] demo skipped",
    "[SIG64] demo TIMEOUT",
    "[SIG64] deliver FAILED",
    "[SIG64] exit FAILED",
    "[SIG64] send-group pgrp=19 sig=10 n=0",
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.exists(c) or shutil.which(c):
            return c
    return None


def q(p):
    return p.replace("\\", "/")


def _crc32(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def _vimtufs2_format(buf, start_lba, total_sectors):
    """与 tests/proc64_test.py 同一套（空 VimtuFS2 卷；规格 = kernel/vfs64.cpp）。"""
    bmn = (total_sectors + 4095) // 4096
    inodes = max(16, min(256, total_sectors // 64))
    ino_blocks = (inodes + 7) // 8
    bm_start = 1
    ino_start = bm_start + bmn
    data_start = ino_start + ino_blocks
    data_blocks = total_sectors - data_start
    if data_blocks <= 0:
        raise RuntimeError("fixture volume too small")

    sb = bytearray(SECTOR)
    sb[0:8] = b"VIMTUFS2"
    struct.pack_into("<I", sb, 8, 2)
    struct.pack_into("<I", sb, 12, SECTOR)
    struct.pack_into("<I", sb, 16, SECTOR)
    struct.pack_into("<I", sb, 20, total_sectors)
    struct.pack_into("<I", sb, 24, 0)
    struct.pack_into("<I", sb, 28, bm_start)
    struct.pack_into("<I", sb, 32, bmn)
    struct.pack_into("<I", sb, 36, ino_start)
    struct.pack_into("<I", sb, 40, inodes)
    struct.pack_into("<I", sb, 44, 64)
    struct.pack_into("<I", sb, 48, data_start)
    struct.pack_into("<I", sb, 52, data_blocks)
    struct.pack_into("<I", sb, 56, 0)
    struct.pack_into("<I", sb, 60, _crc32(sb[:60]))
    sb[510], sb[511] = 0x55, 0xAA

    base = start_lba * SECTOR
    buf[base:base + SECTOR] = sb
    for m in range(bmn):
        bm = bytearray(SECTOR)
        for k in range(4096):
            blk = m * 4096 + k
            if blk >= total_sectors or blk < data_start:
                bm[k >> 3] |= 1 << (k & 7)
        off = base + (bm_start + m) * SECTOR
        buf[off:off + SECTOR] = bm
    for i in range(ino_blocks):
        off = base + (ino_start + i) * SECTOR
        buf[off:off + SECTOR] = b"\0" * SECTOR
    root = bytearray(64)
    root[0] = 2
    struct.pack_into("<I", root, 28, 0)
    struct.pack_into("<I", root, 60, _crc32(root[:60]))
    off = base + ino_start * SECTOR
    buf[off:off + 64] = root


def _mbr_entry(bootable, ptype, start, sectors):
    e = bytearray(16)
    e[0] = 0x80 if bootable else 0x00
    e[1:4] = b"\xFE\xFF\xFF"
    e[4] = ptype
    e[5:8] = b"\xFE\xFF\xFF"
    struct.pack_into("<I", e, 8, start)
    struct.pack_into("<I", e, 12, sectors)
    return e


def prepare_fixture():
    if not os.path.exists(SYSTEM_IMG):
        return None
    with open(SYSTEM_IMG, "rb") as f:
        sys_bytes = f.read()
    if len(sys_bytes) == 0 or len(sys_bytes) % SECTOR or len(sys_bytes) > TARGET_SECTORS * SECTOR:
        return None
    buf = bytearray(TARGET_SECTORS * SECTOR)
    buf[0:len(sys_bytes)] = sys_bytes
    buf[446:462] = _mbr_entry(True, 0xEF, PART_BOOT_LBA, PART_BOOT_SECS)
    buf[462:478] = _mbr_entry(False, 0x07, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    buf[478:510] = b"\0" * 32
    buf[510], buf[511] = 0x55, 0xAA
    _vimtufs2_format(buf, PART_MAIN_LBA, TARGET_SECTORS - PART_MAIN_LBA)
    with open(FIXTURE_IMG, "wb") as f:
        f.write(buf)
    return FIXTURE_IMG


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None, help="已装好的磁盘镜像（缺省：用 system.img 造夹具）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="保留日志目录")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        print("SKIP: 找不到 qemu-system-x86_64（环境问题）")
        return 2
    if args.img:
        img = args.img
        if not os.path.exists(img):
            print("SKIP: 镜像不存在：%s" % img)
            return 2
        print("[sig64] 直接用给定镜像：%s" % img)
    else:
        img = prepare_fixture()
        if not img:
            print("SKIP: 没有 build64/system.img（先跑 bash build64.sh）")
            return 2
        print("[sig64] 夹具 = %s（system.img + MBR + 空 VimtuFS2 主分区 LBA %d）" % (img, PART_MAIN_LBA))

    logdir = tempfile.mkdtemp(prefix="sig64_test_")
    serial = os.path.join(logdir, "serial.log")
    mport = qh.free_port()
    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-sig64",
        "-drive", "format=raw,file=%s" % q(img),
        "-boot", "order=c", "-m", "512", "-vga", "std",
        "-display", "none",
        "-serial", "file:%s" % q(serial),
        "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
        "-no-reboot",
    ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def slog():
        try:
            with open(serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    checks = []
    failed = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        if not cond:
            failed.append(name)
        print("    %s %s%s" % ("[ok]" if cond else "[!!]", name, ("  -- " + detail) if detail else ""))

    log = ""
    try:
        deadline = time.time() + args.timeout
        while time.time() < deadline:
            log = slog()
            if "[SIG64] demo done" in log:
                time.sleep(1.5)                      # 让后续行（桌面/后续演示）落盘
                log = slog()
                break
            if proc.poll() is not None:
                break
            time.sleep(0.5)
    finally:
        try:
            mon = qh.Monitor(mport)
            mon.send("quit", wait=0.2)
        except Exception:
            pass
        time.sleep(2.0)
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass
    # 串口行的结尾是 CRLF（dbg64_nl 写 "\r\n"）：统一去掉 \r，否则正则里的行尾匹配会落空。
    log = (slog() or log).replace("\r", "")
    if not log:
        print("SKIP: 串口日志为空（QEMU 起不来？）")
        return 2

    print("[sig64] --- ① handler 投递 + rt_sigreturn ---")
    check("用户在 handler 里打了点（sig64: handler sig=.. count=..）",
          re.search(r"sig64: handler sig=\d+ count=\d+", log) is not None)
    check("[SIG64] selftest PASS", "[SIG64] selftest PASS" in log)
    check("演示装卷 + 启动：install ok path=/sig64.elf", "[SIG64] install" in log and "path=/sig64.elf" in log)
    check("真进程演示起来了（demo start pid=..）",
          re.search(r"\[SIG64\] demo start pid=\d+ path=/sig64\.elf", log) is not None)
    check("用户程序自己确认 handler 装好（sig64: handlers installed）",
          "sig64: handlers installed (usr1/usr2/int)" in log)
    m = re.search(r"\[SIG64\] deliver pid=(\d+) sig=(\d+) handler=0x([0-9A-Fa-f]+) frame=0x([0-9A-Fa-f]+) where=(\w+)", log)
    check("内核真投递（deliver ... handler=0x.. frame=0x.. where=..）", m is not None,
          m.group(0) if m else "")
    check("rt_sigreturn 真恢复现场（sigreturn pid=.. sig=.. rip=0x..）",
          re.search(r"\[SIG64\] sigreturn pid=\d+ sig=\d+ rip=0x[0-9A-Fa-f]+ mask=0x", log) is not None)
    check("handler 返回后主流程继续（demo done 行齐全）",
          re.search(r"sig64: demo done usr1=\d+ usr2=\d+ int=\d+ term=\d+", log) is not None)

    print("[sig64] --- ② 默认动作终止（128+sig）+ wait4 ---")
    check("[SIG64] default action sig=15 exit=143",
          re.search(r"\[SIG64\] default action pid=\d+ sig=15 exit=143", log) is not None)
    check("内核记录退出：exit pid=.. sig=15 code=143 by_signal=1",
          re.search(r"\[SIG64\] exit pid=\d+ sig=15 code=143 by_signal=1", log) is not None)
    check("父进程 wait4 看到 143（WEXITSTATUS）= 128+15",
          re.search(r"sig64: default-action exit=143 \(expect 143\)", log) is not None)
    check("wait4 系统调用对得上（status=36608 = 143<<8）",
          re.search(r"sig64: wait4 child\(SIGTERM\) pid=\d+ ret=\d+ status=36608 WEXITSTATUS=143", log) is not None)

    print("[sig64] --- ③ SIGKILL 不可捕获 ---")
    check("用户侧注册 SIGKILL 被拒（rc=-1，EINVAL）",
          re.search(r"sig64: sigaction\(SIGKILL\) rc=-1", log) is not None)
    check("内核侧如实打 deny（reason=uncatchable）",
          re.search(r"\[SIG64\] action deny pid=\d+ sig=9 reason=uncatchable", log) is not None)
    check("真 SIGKILL：默认动作 exit=137",
          re.search(r"\[SIG64\] default action pid=\d+ sig=9 exit=137", log) is not None)
    check("被 SIGKILL 的两个子进程退出码都是 137",
          re.search(r"sig64: children exit codes=137,137 \(expect 137,137\)", log) is not None)

    print("[sig64] --- ④ rt_sigprocmask 阻塞/未决/解除后投递（时序证据）---")
    check("阻塞期间只挂未决（blocked=0x1000 pending=0x1000）",
          re.search(r"\[SIG64\] pending/procmask pid=\d+ blocked=0x0000000000001000 "
                    r"pending=0x0000000000001000 \(delivered when unblocked\)", log) is not None)
    check("用户侧在阻塞期间看到 handler 计数仍为 0",
          "sig64: blocked: seen=0 (expect 0)" in log)
    check("解除阻塞后**在系统调用出口**投递（where=syscall）",
          re.search(r"\[SIG64\] deliver pid=\d+ sig=12 handler=0x[0-9A-Fa-f]+ frame=0x[0-9A-Fa-f]+ where=syscall\b",
                    log) is not None)
    check("解除后 handler 计数变 1（用户侧时序证据）",
          "sig64: unblocked: seen=1 (expect 1)" in log)
    check("rt_sigprocmask 真改了屏蔽字（mask=0x..0 表示已解除）",
          re.search(r"\[SIG64\] pending/procmask pid=\d+ how=1 mask=0x0000000000000000", log) is not None)

    print("[sig64] --- ⑤ 用户态异常只杀该进程（#PF / #GP / #DE）---")
    check("用户态 #PF 被转成信号（fault ... no=14 #PF ... -> sig=11）",
          re.search(r"\[SIG64\] fault pid=\d+ no=14 #PF err=0x[0-9A-Fa-f]+ cr2=0x0000000100800000", log) is not None)
    check("#PF 默认动作：只杀该进程 exit=139",
          re.search(r"\[SIG64\] default action pid=\d+ sig=11 exit=139 fault=#PF", log) is not None)
    check("父进程 wait4 看到 #PF 子进程 139",
          re.search(r"sig64: bad-pointer exit=139 \(expect 139\)", log) is not None)
    check("用户态 #GP（wrmsr）也被转成信号（no=13 #GP）",
          re.search(r"\[SIG64\] fault pid=\d+ no=13 #GP", log) is not None)
    check("#GP 子进程退出码 139",
          re.search(r"sig64: bad-insn exit=139 \(expect 139\)", log) is not None)
    check("除零 #DE -> SIGFPE（exit=136 = 128+8）",
          re.search(r"\[SIG64\] fault pid=\d+ no=0 #DE", log) is not None and
          re.search(r"sig64: div-zero exit=136 \(expect 136\)", log) is not None)
    check("三个异常子进程的内核退出记录都带 by_signal=1",
          len(re.findall(r"\[SIG64\] exit pid=\d+ sig=(11|8) code=(139|136) by_signal=1", log)) >= 3)

    print("[sig64] --- ⑥ kill(-pgid) 广播 ---")
    check("父进程自立进程组（setpgid(0,0) -> tty fg pgrp=..）",
          re.search(r"\[SIG64\] tty fg pgrp=\d+ prev=\d+", log) is not None)
    check("广播覆盖 3 个进程（父 + 两子）",
          re.search(r"\[SIG64\] send-group pgrp=\d+ sig=10 n=3 src=kill-group", log) is not None)
    check("两个子进程各自收到（deliver pid=.. sig=10）",
          len(re.findall(r"\[SIG64\] deliver pid=\d+ sig=10 handler=0x", log)) >= 3)
    check("父进程也收到（用户侧 parent_seen=1）",
          re.search(r"sig64: kill rc=0 parent_seen=1 \(expect 1\)", log) is not None)

    print("[sig64] --- ⑦ 终端 Ctrl+C（前台进程组 -> SIGINT）---")
    check("终端 Ctrl+C 走前台进程组（tty-int send pgrp=.. sig=2）",
          re.search(r"\[SIG64\] tty-int send pgrp=\d+ sig=2 rc=0", log) is not None)
    check("SIGINT 投递给编辑器/演示进程的 handler",
          re.search(r"\[SIG64\] deliver pid=\d+ sig=2 handler=0x[0-9A-Fa-f]+", log) is not None)
    check("用户侧 handler 真跑了（int count=1）",
          re.search(r"sig64: handler sig=2 count=1", log) is not None)
    check("演示收尾：tty_int=1 且已退出",
          re.search(r"\[SIG64\] demo done pid=\d+ exited=1 tty_int=1", log) is not None)

    print("[sig64] --- 无 PANIC / 无其它禁止行 ---")
    for pat in FORBIDDEN:
        if pat == "[SIG64] demo skipped":
            # 空卷上没有 /bin/edit -> 编辑器演示**应当**如实跳过（这不是失败，见 tests/edit64_test.py）
            continue
        check("不得出现 %s" % pat, pat not in log)
    check("[SIG64] log_suppressed=0（打点没被预算节流掉）",
          re.search(r"\[SIG64\] demo done pid=\d+ exited=1 tty_int=1 ticks=\d+ log_suppressed=0", log) is not None)

    ok = len(failed) == 0
    print()
    print("[sig64] 断言 %d/%d 通过" % (len(checks) - len(failed), len(checks)))
    if failed:
        for f in failed:
            print("       FAIL: %s" % f)
    if args.keep:
        print("[sig64] 日志保留在：%s" % logdir)
    else:
        shutil.rmtree(logdir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
