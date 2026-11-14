#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/flush64_test.py —— v0.10.1「写盘落盘」端到端验收（FLUSH CACHE）

为什么要有这个测试：到 v0.10.0 为止，三个块设备驱动**都没有 flush 语义** ——
WRITE DMA 返回成功只代表"数据进了设备（或它的缓存）"。安装收尾刚写下的卷超级块
与 GPT 备份头如果还在设备缓存里，掉电 / 宿主强杀 VM 之后就是"装完却挂不上卷"。
本批补上 PATA/AHCI/NVMe 三条 FLUSH 路径，并在两个真实调用点接上（安装收尾、关机/重启）。

判据（全部要"设备确认"级证据，不接受"我们发了命令"）：
  A) 安装收尾刷盘（安装机 = 默认 i440fx -> PATA 目标盘）
       [ATA64] flush drive=1 ok path=pata
       [INSTALL] flush drive=1 ok (cache synced to media)
  B) 负向对照：驱动器号 4..7 是保留空洞（无设备），flush 必须**如实失败**
       [ATA64] flush drive=4 failed path=none reason=nodrive
     —— 没有这一条，"ok" 就有可能是无条件打印的
  C) 关机刷全盘（同一个装好的盘放到 q35 的 ich9-ahci 端口 0 -> 驱动号 8）
       [AHCI64] flush dev=0 port=<n> ok (cache synced)
       [ATA64] flush drive=8 ok path=ahci
       [ATA64] flush all why=shutdown drives=1 ok=1 fail=0

用法：py -3 -u tests/flush64_test.py [--keep]
退出码：0 全绿 / 1 有断言失败 / 2 环境不具备（qemu 或镜像缺失 / 装不起来）
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                     # noqa: E402  （find_qemu）
import startmenu64_test as SM                     # noqa: E402  （Monitor + Cursor/click）
import qemuhelp as qh                             # noqa: E402  （登录手势/wait_desktop）

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MEDIUM = os.path.join(ROOT, "vimtu64-64.img")
OS_KERNEL = os.path.join(ROOT, "build64", "kernel64_os.bin")
WORK = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "flush64")
os.makedirs(WORK, exist_ok=True)
TARGET = os.path.join(WORK, "flush_target.img")
PORT = 5783
# 128 MiB：够主分区 + ESP，安装复制量和"扩容搬卷"都小（本测试关心的是 flush，不是容量）
TARGET_SECTORS = 262144

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                          ("  -- " + detail) if detail else ""), flush=True)


def q(p):
    return p.replace("\\", "/")


def log_of(path):
    try:
        return open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def wait_in(path, needle, timeout, proc=None):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if needle in log_of(path):
            return True
        if proc is not None and proc.poll() is not None:
            return needle in log_of(path)
        time.sleep(0.4)
    return False


def start(qemu, args, serial, port, name):
    if os.path.exists(serial):
        os.remove(serial)
    proc = subprocess.Popen([qemu, "-name", name, "-m", "512", "-vga", "std",
                             "-display", "none", "-serial", "file:%s" % q(serial),
                             "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                             "-no-reboot"] + args,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(160):
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return proc


def stop(proc, mon=None, clean=True):
    """clean=True 走 monitor quit（QEMU 正常退出，宿主侧写缓存也落盘）；否则 kill。"""
    if proc.poll() is not None:
        return
    if clean and mon is not None:
        mon.send("quit", wait=1.5)
        try:
            proc.wait(timeout=30)
            return
        except Exception:
            pass
    proc.kill()
    try:
        proc.wait(timeout=10)
    except Exception:
        pass


def type_line(mon, text, per=0.13):
    km = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}
    for ch in text:
        mon.key(km.get(ch, ch), wait=per)
    mon.key("ret", wait=1.0)


def open_terminal(serial, mon):
    """与 install_persist64_test.open_terminal 同一套手势（开始菜单 -> 终端磁贴 -> 点击）。"""
    hit = None
    for _ in range(5):
        mon.key("meta_l", 1.6)
        time.sleep(1.2)
        for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(-?\d+) name=(\S+) en=(.+?) "
                              r"x=(\d+) y=(\d+) w=(\d+) h=(\d+)", log_of(serial)):
            if mm.group(3) == "终端":
                hit = mm
                break
        if hit:
            break
        mon.key("esc", 0.8)
        time.sleep(1.0)
    if not hit:
        return False
    cur = SM.Cursor()
    cur.goto(mon, int(hit.group(5)) + int(hit.group(7)) // 2,
             int(hit.group(6)) + int(hit.group(8)) // 2)
    cur.settle(mon)
    time.sleep(0.3)
    mon.click()
    return wait_in(serial, "[APP] term opened", 30)


def first_match(pattern, text, default="（缺失）"):
    m = re.search(pattern, text)
    return m.group(0) if m else default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true", help="跑完不删目标盘（排障用）")
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu)
    if not qemu:
        print("跳过：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(MEDIUM) or not os.path.exists(OS_KERNEL):
        print("跳过：安装介质或系统内核缺失（%s / %s）" % (MEDIUM, OS_KERNEL))
        return 2

    print("=== flush64_test：安装收尾刷盘 + 负向对照 + 关机刷全盘（AHCI）===")
    if os.path.exists(TARGET):
        os.remove(TARGET)
    with open(TARGET, "wb") as f:
        f.truncate(TARGET_SECTORS * 512)
    print("  目标盘 %d 扇区（%.0f MiB）" % (TARGET_SECTORS, TARGET_SECTORS / 2048.0))

    # ---------------------------------------------------------------- A) 安装 + 收尾刷盘
    serial_i = os.path.join(WORK, "install.log")
    proc = start(qemu, ["-drive", "format=raw,file=%s,index=0,media=disk" % q(MEDIUM),
                        "-drive", "format=raw,file=%s,index=1,media=disk" % q(TARGET),
                        "-boot", "order=c"], serial_i, args.port, "vimtu-flush-install")
    mon = SM.Monitor(args.port)
    if not wait_in(serial_i, "磁盘枚举完成", 120):
        print("  安装向导没就绪")
        stop(proc, mon, clean=False)
        return 2
    for key in ("ret", "ret", "ret", "ret", "n", "ret"):
        mon.key(key, wait=2.0)
    done = wait_in(serial_i, "[SETUP] 安装完成", 900, proc)
    log_i = log_of(serial_i)
    check("安装走完（[SETUP] 安装完成）", done,
          first_match(r"\[SETUP\] 安装完成[^\r\n]*", log_i))
    check("A1 安装收尾真的发了一次 FLUSH（[ATA64] flush drive=1 ok path=pata）",
          "[ATA64] flush drive=1 ok path=pata" in log_i,
          first_match(r"\[ATA64\] flush drive=[^\r\n]*", log_i))
    check("A2 安装器确认缓存已刷到介质（[INSTALL] flush drive=1 ok）",
          "[INSTALL] flush drive=1 ok (cache synced to media)" in log_i,
          first_match(r"\[INSTALL\] flush drive=[^\r\n]*", log_i))
    check("B  负向对照：保留空洞 drive=4 的 flush 必须失败（ok 不是无条件的）",
          "[ATA64] flush drive=4 failed path=none reason=nodrive" in log_i,
          first_match(r"\[ATA64\] flush drive=4[^\r\n]*", log_i))
    stop(proc, mon, clean=True)

    # ---------------------------------------------------------------- C) 关机刷全盘（AHCI）
    # q35 + if=ide,index=0 -> 落到 ich9-ahci 端口 0 -> 统一驱动号 8（ATA64_AHCI_BASE + 0）
    serial_s = os.path.join(WORK, "shutdown.log")
    proc = start(qemu, ["-machine", "q35",
                        "-drive", "format=raw,file=%s,if=ide,index=0,media=disk" % q(TARGET),
                        "-boot", "order=c"], serial_s, args.port, "vimtu-flush-shutdown")
    mon = SM.Monitor(args.port)
    if not qh.wait_desktop(mon, lambda: log_of(serial_s), proc, timeout=420, login_timeout=460):
        print("  装好的盘在 AHCI 上没进桌面")
        stop(proc, mon, clean=False)
        log_s = log_of(serial_s)
        check("C0 AHCI 盘挂上系统卷（[VFS64] mount_system slot=）",
              "[VFS64] mount_system slot=" in log_s,
              first_match(r"\[VFS64\] mount_system[^\r\n]*", log_s))
        summary()
        return 1
    log_s = log_of(serial_s)
    check("C0 AHCI 盘挂上系统卷（[VFS64] mount_system slot=）",
          "[VFS64] mount_system slot=" in log_s,
          first_match(r"\[VFS64\] mount_system slot=[^\r\n]{0,90}", log_s))
    if not open_terminal(serial_s, mon):
        print("  打不开终端")
        stop(proc, mon, clean=False)
        summary()
        return 1
    print("  终端已开，输入 shutdown ...")
    type_line(mon, "shutdown")
    # 关机链路：ACPI S5 断电 -> QEMU 退出（-no-reboot 不影响 S5）
    qh.wait_mark(lambda: log_of(serial_s), "[SHUTDOWN] acpi s5", 90, proc)
    for _ in range(120):
        if proc.poll() is not None:
            break
        time.sleep(0.5)
    stop(proc, mon, clean=False)                    # 已经退出就是空操作
    log_s = log_of(serial_s)

    check("C1b 卡死 PxCI 被完整端口重建救回（[AHCI64] port recover … start=ok）",
          re.search(r"\[AHCI64\] port recover port=\d+ ci0=0x[0-9a-f]+ ci_now=0x0+ link=up start=ok",
                    log_s) is not None,
          first_match(r"\[AHCI64\] port recover[^\r\n]*", log_s))
    log_s = log_of(serial_s)

    check("C1 AHCI 驱动真的发出 FLUSH CACHE EXT 并收到设备确认（[AHCI64] flush dev=0 ok）",
          re.search(r"\[AHCI64\] flush dev=0 port=\d+ ok \(cache synced\)", log_s) is not None,
          first_match(r"\[AHCI64\] flush dev=[^\r\n]*", log_s))
    check("C2 统一驱动号 8（AHCI）走到 flush（[ATA64] flush drive=8 ok path=ahci）",
          "[ATA64] flush drive=8 ok path=ahci" in log_s,
          first_match(r"\[ATA64\] flush drive=8[^\r\n]*", log_s))
    check("C3 关机汇总：每条真盘都刷成功（drives=1 ok=1 fail=0）",
          "[ATA64] flush all why=shutdown drives=1 ok=1 fail=0" in log_s,
          first_match(r"\[ATA64\] flush all[^\r\n]*", log_s))
    check("C4 整链没有 PANIC", "PANIC" not in log_s,
          first_match(r"PANIC[^\r\n]*", log_s, "（无）"))

    if not args.keep:
        try:
            os.remove(TARGET)
        except OSError:
            pass
    return summary()


def summary():
    ok = sum(1 for _, c in checks if c)
    bad = [n for n, c in checks if not c]
    print("\n=== 汇总：%d/%d PASS ===" % (ok, len(checks)))
    if bad:
        for n in bad:
            print("  FAIL: %s" % n)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
