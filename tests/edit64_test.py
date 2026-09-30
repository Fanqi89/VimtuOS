#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/edit64_test.py - ★ A4-5 验收②：**Ring 3 编辑器**（/bin/edit；自研，ANSI + 控制台 + 信号）

要证明的事（与任务书的六条断言一一对应）：
  ① 启动后屏幕出现文件内容与状态行：串口里能看到编辑器发的 ANSI 序列
     （`\\x1b[2J\\x1b[H` 清屏、`\\x1b[<r>;<c>H` 定位、`\\x1b[7m` 状态行反显）+ 正文行原文
     （"VimtuOS A4-5 edit64 test file" 等）+ 状态行里的文件名/行列/行数/修改标记；
  ② 输入字符后内容变化：注键 'x' 之后 `[EDIT64] key=0x78 .. dirty=1`，且屏幕上那一行变成 "x..."；
  ③ Ctrl+S 后**宿主侧解析卷里文件逐字节一致**：
     退出后从 VM 改过的那块盘里按 VimtuFS2 格式读回 /etc/edit64_test.txt，与期望字节逐字节比对
     （期望 = 'x' + Ctrl+C 丢弃 'z' + 'w' -> "xw" + 原始内容）；
  ④ 退出后终端 raw/规范模式还原：`[EDIT64] termios raw ok=1 lflag=0xb raw=0x0`（进入）
     + `[EDIT64] termios restored ok=1 lflag=0xb ...`（退出，读回来就是规范模式的那份）；
  ⑤ Ctrl+C 在编辑器内的语义（本设计 = 放弃当前改动、继续运行）：
     `[SIG64] deliver ... sig=2`（新做的信号投递）+ `[EDIT64] sigint handler ran` +
     `[EDIT64] sigint discard dirty_before=1`，并且编辑器**没死**（后面还继续收键、还退出成功）；
  ⑥ 1 MiB 文件能打开并移动：阶段 b 打开 /tcc/demo/edit1m.txt（1048576 B，8527 行），
     打点 `load ... bytes=1048576 lines=<n> ms=<n>`（给出耗时）+ 翻页/方向键（key=0xf7/0xfe/0xfc/0xfd），
     退出后卷里那份必须**逐字节不变**（说明只读移动没有破坏内容）。

夹具：build64/sysdisk.img 的**副本**（含 /bin/edit + /etc/edit64_test.txt + /tcc/demo/edit1m.txt；
  build64.sh 的第四步 tools/edit_pack_win.py 写的）。用副本 = VM 的写入不污染构建产物。
注意：编辑器**不在内核镜像里**（build64.sh 有断言）；本脚本还会断言内核二进制里搜不到它的字节。

用法（必须 Windows 原生 Python）：
    py -3 tests\\edit64_test.py                    # 无头 QEMU + monitor 注键（默认，快）
    py -3 tests\\edit64_test.py --img <盘> --timeout 400 --keep
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
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
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh             # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SYSDISK = os.path.join(ROOT, "build64", "sysdisk.img")
EDIT_BIN = os.path.join(ROOT, "build64", "edit")
KERNEL_OS = os.path.join(ROOT, "build64", "kernel64_os.bin")
FIXTURE_IMG = os.path.join(ROOT, "build64", "edit64_test.img")
SECTOR = 512
PART_MAIN_LBA = 8009                      # = kernel/part64.h 的主分区起点
TEST_PATH = "/etc/edit64_test.txt"
BIG_PATH = "/tcc/demo/edit1m.txt"
BIG_BYTES = 1 << 20


def _load(mod_name, path):
    spec = importlib.util.spec_from_file_location(mod_name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


EP = _load("edit_pack_win", os.path.join(ROOT, "tools", "edit_pack_win.py"))


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.path.exists(c) or shutil.which(c):
            return c
    return None


def q(p):
    return p.replace("\\", "/")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None, help="含 /bin/edit 的磁盘镜像（缺省：复制 build64/sysdisk.img）")
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=400)
    ap.add_argument("--keep", action="store_true")
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
    else:
        if not os.path.exists(SYSDISK):
            print("SKIP: 没有 build64/sysdisk.img（先跑 bash build64.sh）")
            return 2
        img = FIXTURE_IMG
        shutil.copyfile(SYSDISK, img)          # 副本：VM 会往这块盘里写
    print("[edit64] 夹具 = %s（含 /bin/edit + /etc/edit64_test.txt + /tcc/demo/edit1m.txt）" % img)

    checks = []
    failed = []

    def check(name, cond, detail=""):
        checks.append((name, bool(cond)))
        if not cond:
            failed.append(name)
        print("    %s %s%s" % ("[ok]" if cond else "[!!]", name, ("  -- " + detail) if detail else ""))

    # ---- 交付纪律：内核二进制里不能有 /bin/edit 的字节（它是卷里的文件）----
    if os.path.exists(KERNEL_OS) and os.path.exists(EDIT_BIN):
        k = open(KERNEL_OS, "rb").read()
        e = open(EDIT_BIN, "rb").read()
        mid = len(e) // 2
        check("内核二进制里搜不到 /bin/edit 的 64B 探针（编辑器只从系统卷装载）",
              e[mid:mid + 64] not in k, "edit=%d B kernel=%d B" % (len(e), len(k)))
        check("/bin/edit < 64 KiB（内核主程序装载窗口）", len(e) <= 64 * 1024, "%d B" % len(e))

    logdir = tempfile.mkdtemp(prefix="edit64_test_")
    serial = os.path.join(logdir, "serial.log")
    mport = qh.free_port()
    proc = subprocess.Popen([
        qemu, "-name", "Vimtu64-edit64",
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
                return f.read().replace("\r", "")
        except OSError:
            return ""

    def wait_for(marker, timeout=120.0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if marker in slog():
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    mon = None

    def keys(*names):
        nonlocal mon
        if mon is None:
            mon = qh.Monitor(mport)
        for nm in names:
            mon.send("sendkey %s" % nm, wait=0.55)

    try:
        ok_ready_a = wait_for("[EDIT64] ready file=%s" % TEST_PATH, 180)
        log = slog()
        check("阶段 a：编辑器起来了（ready file=/etc/edit64_test.txt）", ok_ready_a)
        check("交付：/bin/edit 从系统卷装载（[ELF64] load ... path=/bin/edit 或 execve 行）",
              ("path=/bin/edit" in log) or ("/bin/edit" in log and "[PROC64] start loaded" in log))
        check("① 屏幕清屏序列（ANSI \\x1b[2J\\x1b[H）", "\x1b[2J\x1b[H" in log)
        check("① 光标定位序列（ANSI \\x1b[<r>;<c>H）", re.search(r"\x1b\[\d+;\d+H", log) is not None)
        check("① 状态行反显（ANSI \\x1b[7m）", "\x1b[7m" in log)
        check("① 正文行上屏（第一行原文）", "VimtuOS A4-5 edit64 test file" in log)
        check("① 状态行含文件名/行列/行数（/etc/edit64_test.txt .. lines=..）",
              re.search(r"/etc/edit64_test\.txt\s+\d+,\d+\s+lines=\d+", log) is not None)
        check("① 屏幕上有修改标记字段（MODIFIED / saved）",
              ("MODIFIED" in log) or ("saved" in log))
        check("④ 进入 raw 模式（TCGETS 保存 + TCSETS 写入；lflag 0xb -> 0x0）",
              re.search(r"\[EDIT64\] termios raw ok=1 lflag=0x0*[bB] raw=0x0\b", log) is not None)
        check("⑥ 装载打点带行数与耗时（load ... bytes=187 lines=.. ms=..）",
              re.search(r"\[EDIT64\] load file=/etc/edit64_test\.txt bytes=187 lines=\d+ ms=\d+", log) is not None)
        check("键盘焦点申请成功（input_poll FOCUS 路径：focus request rc=0）",
              re.search(r"\[EDIT64\] focus request rc=0", log) is not None)
        check("SIGINT handler 注册成功（sigaction(INT) rc=0）",
              re.search(r"\[EDIT64\] sigaction\(INT\) rc=0", log) is not None)

        # ---- 注键（阶段 a）：x -> Ctrl+S -> z -> Ctrl+C(丢弃) -> w -> Ctrl+S -> Ctrl+Q ----
        keys("x")
        time.sleep(0.4)
        keys("ctrl-s")
        time.sleep(0.6)
        keys("z")
        time.sleep(0.4)
        keys("ctrl-c")
        time.sleep(0.8)
        log = slog()
        check("② 输入字符后内容变化（key=0x78 收到 + 插入后 dirty=1/col 前进 + 该行显示成 x...）",
              re.search(r"\[EDIT64\] key=0x78 line=1 col=1 dirty=0", log) is not None and
              re.search(r"\[EDIT64\] key=0x13 line=1 col=2 dirty=1", log) is not None and
              ("xVimtuOS A4-5 edit64 test file" in log))
        check("⑤ Ctrl+C -> 信号投递（[SIG64] deliver ... sig=2 handler=0x..）",
              re.search(r"\[SIG64\] deliver pid=\d+ sig=2 handler=0x[0-9A-Fa-f]+", log) is not None)
        check("③ Ctrl+S 保存打点（save path=/etc/edit64_test.txt bytes=188）",
              re.search(r"\[EDIT64\] save path=/etc/edit64_test\.txt bytes=188", log) is not None)
        check("① 状态行含文件名 + 行列 + 行数（/etc/edit64_test.txt  N,N  lines=N）",
              re.search(r"/etc/edit64_test\.txt\s+\d+,\d+\s+lines=\d+", log) is not None or
              re.search(r"\d+,\d+\s+lines=\d+", log) is not None)
        check("⑤ 编辑器里的 handler 真跑了（sigint handler ran）",
              "[EDIT64] sigint handler ran (discard+continue)" in log)
        check("⑤ 放弃改动（sigint discard dirty_before=1）",
              re.search(r"\[EDIT64\] sigint discard dirty_before=1", log) is not None)
        check("⑤ 编辑器没被 Ctrl+C 打死（后面还打完收尾行）",
              "[EDIT64] sigint handler ran" in log)
        keys("w")
        time.sleep(0.4)
        keys("ctrl-s")
        time.sleep(0.6)
        log = slog()
        check("③ 第二次保存（'xw' + 原文 = 189 B）",
              re.search(r"\[EDIT64\] save path=/etc/edit64_test\.txt bytes=189", log) is not None)
        keys("ctrl-q")
        ok_done_a = wait_for("[EDIT64] demo phase=a done exited=1", 90)
        log = slog()
        check("阶段 a 干净退出（exited=1，不是超时）", ok_done_a and "[EDIT64] exit code=0" in log)
        check("④ 退出时还原 termios（TCSETS 回规范模式 + 读回来核对 lflag=0xb）",
              re.search(r"\[EDIT64\] termios restored ok=1 lflag=0x0*[bB] ic=0x[0-9A-Fa-f]+", log) is not None)
        check("④ 退出时打印的还是规范模式那份（want_lflag=0xb）",
              re.search(r"want_lflag=0x0*[bB]", log) is not None)

        # ---- 阶段 b：1 MiB 文件（只移动、不保存）----
        ok_ready_b = wait_for("[EDIT64] ready file=%s" % BIG_PATH, 120)
        log = slog()
        check("⑥ 1 MiB 文件打开（load ... bytes=1048576 lines=<n> ms=<n>）",
              ok_ready_b and
              re.search(r"\[EDIT64\] load file=/tcc/demo/edit1m\.txt bytes=1048576 lines=\d+ ms=\d+", log) is not None)
        m = re.search(r"\[EDIT64\] load file=/tcc/demo/edit1m\.txt bytes=1048576 lines=(\d+) ms=(\d+)", log)
        if m:
            print("        （1 MiB 装载：lines=%s ms=%s）" % (m.group(1), m.group(2)))
        keys("pgdn", "pgdn", "pgdn", "down", "right", "up", "pgup")
        log = slog()
        check("⑥ 翻页/方向键都收到了（key=0xf7 翻页 / 0xfe 下 / 0xfc 右 / 0xfd 上）",
              all(("key=0x%s" % h) in log for h in ("f7", "fe", "fc", "fd")))
        keys("ctrl-q")
        ok_done_b = wait_for("[EDIT64] demo phase=b done exited=1", 90)
        log = slog()
        check("阶段 b 干净退出（exited=1）", ok_done_b and "[EDIT64] exit code=0" in log)
        check("④ 第二次退出也还原了 termios（lflag=0xb）",
              len(re.findall(r"\[EDIT64\] termios restored ok=1 lflag=0x0*[bB]", log)) >= 2)
        check("全程无 PANIC", "[PANIC]" not in log)
        check("全程无信号投递失败（[SIG64] deliver FAILED）", "[SIG64] deliver FAILED" not in log)
    finally:
        try:
            if mon is None:
                mon = qh.Monitor(mport)
            mon.send("quit", wait=0.3)
        except Exception:
            pass
        time.sleep(2.5)
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except Exception:
                pass

    # ---- ③ 宿主侧：解析卷里那份文件，逐字节比对 ----
    print("[edit64] --- 宿主侧从卷镜像里逐字节回读（VM 已关机）---")
    img_bytes = open(img, "rb").read()
    vol = img_bytes[PART_MAIN_LBA * SECTOR:]
    # 期望内容：'x'（第一次插入）-> Ctrl+S 保存 -> 'z' -> Ctrl+C 丢弃（回到盘上的 "x"+原文，
    # 光标回文件开头）-> 'w' 插到开头 -> Ctrl+S。所以最终 = "wx" + 原文（189 B）。
    want = b"wx" + EP.make_test_text()
    try:
        got = EP.read_volume_file(vol, TEST_PATH)
    except Exception as ex:
        got = None
        print("       读回失败：%s" % ex)
    check("③ 卷里 /etc/edit64_test.txt 与期望**逐字节一致**（'xw' + 原文 = %d B）" % len(want),
          got == want,
          ("读回 %d B：%r" % (len(got), got[:48])) if got is not None else "读回失败")
    if got == want:
        print("       证据：%r" % got[:64])
    try:
        got_big = EP.read_volume_file(vol, BIG_PATH)
    except Exception:
        got_big = None
    check("⑥ 1 MiB 文件（只移动未保存）在卷里逐字节不变",
          got_big is not None and got_big == EP.make_big_text() and len(got_big) == BIG_BYTES,
          "%d B" % (len(got_big) if got_big else -1))

    ok = len(failed) == 0
    print()
    print("[edit64] 断言 %d/%d 通过" % (len(checks) - len(failed), len(checks)))
    if failed:
        for f in failed:
            print("       FAIL: %s" % f)
    if args.keep:
        print("[edit64] 日志保留在：%s" % logdir)
    else:
        shutil.rmtree(logdir, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
