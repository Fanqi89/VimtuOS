#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/userlib64_test.py - ★ A2 验收：**用 C 写**的用户程序（自研最小 libc + 自有 ABI 包装）

被测对象（都在 build64/system.img 的启动路径上跑，见 kernel/kernel64.cpp 的 os_boot_path）：
  * user/apps/hello.c      —— crt0 -> main(argc,argv) -> puts/printf -> exit（自有 ABI exit(2)）
  * user/apps/libctest.c   —— printf 子集（%s %c %d %u %x %X %% + 宽度）、malloc/free/calloc/realloc
                              压力 + 堆哨兵自检、POSIX 契约（getcwd 走 Linux 兼容路径、chdir 如实 ENOSYS）
  * user/apps/fbdemo.c     —— A1 汇编演示的 **C 重写**（fb_map -> 画渐变带/移动矩形 -> fb_flip 局部提交
                              -> 越界被拒 / 部分越界被夹取 -> fb_present -> exit）

断言（每条都对应一段可复现的对口证据，不猜、不放宽）：
  1) 串口逐字节证据：hello 的一行 + printf 子集的**精确串**（含宽度对齐）+ malloc 压力行 + POSIX 行；
  2) 运行路径证据：[USER64] capp map writable=1（C 程序走的是可写代码页那条路）、
     每个程序各自的 `[USER64] demo done name=<n> rc=0`、Linux 兼容路径的 `[SYSCALL] insn nr=79`；
  3) fbdemo.c 的**像素证据与 A1 判据完全一致**：连续 3 帧两两差异 > 阈值、变化区域落在
     fb_flip 请求的带内、带外几乎不变、越界 flip 被拒（clip=reject + [FBDEMO] oob ret=1）、
     部分越界被夹取（clip=clamped + [FBDEMO] clamp ret=0）、[FB64] present 打过点；
  4) 收尾：演示结束后锁屏 -> 显式登录（回车两次）-> [GUI64] ready，且桌面帧与演示帧不同；
  5) 全程禁止：PANIC / TRIPLE FAULT / selftest FAIL / FAILED mask= / [USER64] run FAILED /
     [SYSCALL] deny（自己的运行时不许拿非法参数去打内核的脸）。

用法（必须用 Windows 原生 Python）：py -3 tests\\userlib64_test.py [--img build64/system.img] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/镜像缺失）
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
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402  （★ 公共登录手势：ui.login.auto 默认 0）

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

SHOTS_MAX = 9                        # 最多抓几帧（够找"连续三帧"）
INSIDE_MIN_FRAC = 0.05               # 与 tests/fbmap64_test.py 同一判据：带内变化 > 带面积 5%

# ---- 期望的**精确输出串**（逐字节；来自 user/apps/*.c 里的 printf 格式串）----
HELLO_LINE = "hello from Vimtu64 C userland"
LIBC_PRINTF_LINE = ("[LIBC] printf s=[vimtu64] d=[-42] u=[4294967295] x=[deadbeef] "
                    "X=[DEADBEEF] c=[Z] pct=[%]")
LIBC_WIDTH_LINE = "[LIBC] width [    7][7    ][00007][      ab][ab      |]"
LIBC_POSIX_LINE = "[LIBC] posix getcwd=[/] chdir=-1 errno=38"
LIBC_AFTER_LINE = "[LIBC] printf after stress ok"

FORBIDDEN = [
    "PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=",
    "[USER64] run FAILED", "[USER64] enter FAILED",
    "[FBDEMO] fb_map FAILED", "[FB64] map FAILED",
    "[SYSCALL] deny",                # 自己的运行时不许打非法参数（见 user/apps/libctest.c 顶部的说明）
]


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            found = shutil.which(c)
            if found:
                return found
    return None


def q(p):
    return p.replace("\\", "/")


class Vm:
    def __init__(self, qemu, img, port, serial, name="vimtu-userlib64"):
        self.serial = serial
        self.port = port
        self.proc = subprocess.Popen([
            qemu, "-name", name,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(160):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.15):
        try:
            s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        except OSError:
            return False
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()
        return True

    def key(self, k, wait=1.0):
        return self.send("sendkey %s" % k, wait=wait)

    def shot(self, ppm, timeout=8.0):
        if os.path.exists(ppm):
            os.remove(ppm)
        self.send("screendump %s" % q(ppm), wait=0.05)
        t0 = time.time()
        while time.time() - t0 < timeout:
            if os.path.exists(ppm) and os.path.getsize(ppm) > 1024:
                n1 = os.path.getsize(ppm)
                time.sleep(0.15)
                if os.path.getsize(ppm) == n1:
                    return True
            time.sleep(0.05)
        return False


def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%s" % path)
    idx = 2
    vals = []
    while len(vals) < 3:
        while raw[idx:idx + 1].isspace():
            idx += 1
        s = idx
        while not raw[idx:idx + 1].isspace():
            idx += 1
        vals.append(int(raw[s:idx]))
    idx += 1
    w, h, _ = vals
    return w, h, raw[idx:]


def diff_region(pa, pb, w, h, x0, y0, x1, y1, step=1, thresh=24):
    """矩形 [x0,x1) × [y0,y1) 内变化的像素数 + 变化像素的 bbox（与 fbmap64_test.py 同一判据）。"""
    n = 0
    bx0 = by0 = 10 ** 9
    bx1 = by1 = -1
    for y in range(y0, y1):
        o = y * w * 3
        for x in range(x0, x1, step):
            oo = o + x * 3
            d = max(abs(pa[oo] - pb[oo]), abs(pa[oo + 1] - pb[oo + 1]), abs(pa[oo + 2] - pb[oo + 2]))
            if d > thresh:
                n += 1
                if x < bx0:
                    bx0 = x
                if x > bx1:
                    bx1 = x
                if y < by0:
                    by0 = y
                if y > by1:
                    by1 = y
    return n, (bx0, by0, bx1, by1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--keep", action="store_true", help="保留临时目录（打印路径）")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_userlib_")
    serial = os.path.join(tmp, "serial.log")
    port = args.port or qh.free_port()
    vm = Vm(qemu, args.img, port, serial)
    mon = Monitor(port)

    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    shots = []          # [(frame_no, ppm_path)]
    band = None         # fb_flip 请求的区域（clip=ok）
    fbinfo = {}
    dims = None
    # 等串口日志里出现某几条正则（串口是**边写边读**的文件：不等就可能在半行处读断 —— 实测踩过，
    # 症状是"[FB64] map pid="能等到而整行正则匹配不上、malloc 压力行还没落盘）
    def wait_re(pats, timeout):
        t0w = time.time()
        while time.time() - t0w < timeout:
            lgw = vm.log()
            if all(re.search(pp, lgw) for pp in pats):
                return lgw
            if vm.proc.poll() is not None:
                break
            time.sleep(0.2)
        return vm.log()

    try:
        # ---------- 1) C 程序在 ring3 跑起来了（可写代码页那条路）----------
        print("=== 1) C 用户程序进 ring3（[USER64] capp map writable=1）===")
        got_capp = vm.wait_log("[USER64] capp map writable=1", 180)
        check("C 程序走了 user64_run_capp64（[USER64] capp map writable=1）", got_capp)
        # 等到 libctest 全部跑完（它最后一行 = "[LIBC] printf after stress ok" + 退出打点）再读日志：
        # 这样 hello/libctest 两段的所有串口证据都已经落盘（不留半行竞态）
        log = wait_re([r"\[USER64\] capp map writable=1 size=\d+ name=hello_c",
                       r"\[USER64\] capp map writable=1 size=\d+ name=libctest_c",
                       r"\[LIBC\] printf after stress ok",
                       r"\[USER64\] demo done name=libctest_c rc=0"], 180)
        for tag, what in (("hello_c", "hello.c"), ("libctest_c", "libctest.c")):
            m = re.search(r"\[USER64\] capp map writable=1 size=(\d+) name=%s" % tag, log)
            check("blob 尺寸与名字都对上（%s：%s）" % (what, tag), m is not None,
                  ("size=%s" % m.group(1)) if m else "")

        # ---------- 2) hello.c 的串口逐字节证据 ----------
        print("=== 2) hello.c：puts/printf 的原文 + 正常退出 ===")
        check("hello.c 打印了那一行（逐字节：%s）" % HELLO_LINE, HELLO_LINE in log)
        mh = re.search(r"\[HELLO\] argc=1 argv0=user64 pid=(-?\d+) ticks=(\d+)", log)
        check("crt0 合成的 argc/argv 正确（[HELLO] argc=1 argv0=user64 pid=.. ticks=..）", mh is not None,
              mh.group(0) if mh else "")
        check("getpid/ticks 走自有 ABI 3/4（pid/ticks 都是数字）", mh is not None)
        check("hello.c 正常退出（[USER64] demo done name=hello_c rc=0）",
              "[USER64] demo done name=hello_c rc=0" in log)

        # ---------- 3) printf 子集（精确串）----------
        print("=== 3) printf 子集：%s %c %d %u %x %X %% 与宽度 ===")
        check("printf 子集行逐字节一致", LIBC_PRINTF_LINE in log, LIBC_PRINTF_LINE)
        check("printf 宽度行逐字节一致（%%5d / %%-5d / %%05d / %%8s / %%-8s）", LIBC_WIDTH_LINE in log,
              LIBC_WIDTH_LINE)
        check("`printf` 的返回值/长度语义没把换行弄丢（上一行以换行结尾）",
              (LIBC_PRINTF_LINE + "\n") in log)

        # ---------- 4) malloc 压力 + 堆哨兵 ----------
        print("=== 4) malloc/free/calloc/realloc 压力 + 哨兵 ===")
        mm = re.search(r"\[LIBC\] malloc stress blocks=(\d+) bad=0x([0-9a-f]+) heap_check=(\d+) "
                       r"used=(\d+) total=(\d+) live=(\d+)", log)
        check("压力行出现（[LIBC] malloc stress …）", mm is not None,
              mm.group(0) if mm else "")
        if mm:
            check("24 次分配/交错释放/复用空闲链表 + calloc/realloc 全部无错（bad=0x0）",
                  mm.group(2) == "0", "bad=0x%s" % mm.group(2))
            check("堆自检通过（heap_check=0：块头哨兵 + 竞技场尾哨兵 + 空闲链表升序都完好）",
                  mm.group(3) == "0", "heap_check=%s" % mm.group(3))
            check("竞技场大小 = V64_HEAP_BYTES（total=4096）", mm.group(5) == "4096", "total=%s" % mm.group(5))
            check("压力结束后所有块都归还（live=0）", mm.group(6) == "0", "live=%s" % mm.group(6))
        check("压力之后 printf 仍然正常（[LIBC] printf after stress ok）", LIBC_AFTER_LINE in log)

        # ---------- 5) POSIX 契约（两条路径）----------
        print("=== 5) POSIX 契约：getcwd（Linux 兼容路径）/ chdir（如实 ENOSYS）===")
        check("getcwd 走 syscall 指令路径（[SYSCALL] insn nr=79）", "[SYSCALL] insn nr=79" in log)
        check("POSIX 行逐字节一致（getcwd=[/] chdir=-1 errno=38）", LIBC_POSIX_LINE in log, LIBC_POSIX_LINE)
        check("libctest.c 正常退出（[USER64] demo done name=libctest_c rc=0）",
              "[USER64] demo done name=libctest_c rc=0" in log)

        # ---------- 6) fbdemo.c：与 A1 同一套串口打点 ----------
        print("=== 6) fbdemo.c（C 版 A1 演示）：fb_map / 帧 / 越界 / 夹取 ===")
        check("fb_map 打点（[FB64] map pid=）", vm.wait_log("[FB64] map pid=", 180))
        # ★ 抓屏要**尽早**开始：用户侧 map 打点一出现说明演示马上画第一帧（每帧只停 700ms）。
        #   所以这里先等用户侧那行，然后立刻进抓屏循环；两边的 map 行等抓完再解析（那时必然都落盘了）。
        vm.wait_log("[FBDEMO] map va=0x", 60)
        last_frame = -1
        t0 = time.time()
        while len(shots) < SHOTS_MAX and time.time() - t0 < 150:
            lg = vm.log()
            mf = re.findall(r"\[FBDEMO\] frame=(\d+) flip=(\d+)", lg)
            if mf and int(mf[-1][0]) > last_frame:
                last_frame = int(mf[-1][0])
                time.sleep(0.1)
                ppm = os.path.join(tmp, "cdemo_f%02d.ppm" % last_frame)
                if mon.shot(ppm, timeout=4.0):
                    shots.append((last_frame, ppm))
            if ("[FB64] user-draw demo done" in lg) or ("[FBDEMO] done" in lg and len(shots) >= 3):
                break
            time.sleep(0.05)
        check("抓到了 >= 3 帧 C 演示画面", len(shots) >= 3,
              "shots=%d frames=%s" % (len(shots), [s[0] for s in shots]))

        # 两边的 map 行都解析一遍（十六进制**大小写都收**：内核 dbg64_hex64 打大写，
        # 实测 pa=0x000000000049A000 —— 只写 [0-9a-f] 会漏，本批次踩过）
        log = wait_re([r"\[FB64\] map pid=-?\d+ va=0x[0-9A-Fa-f]+ pa=0x[0-9A-Fa-f]+ w=\d+ h=\d+ "
                       r"pitch=\d+ fmt=\d+ pages=\d+ re=\d+ u=\d+",
                       r"\[FBDEMO\] map va=0x[0-9A-Fa-f]+ w=\d+ h=\d+ pitch=\d+ fmt=\d+"], 30)
        m = re.search(r"\[FB64\] map pid=(-?\d+) va=0x([0-9A-Fa-f]+) pa=0x([0-9A-Fa-f]+) "
                      r"w=(\d+) h=(\d+) pitch=(\d+) fmt=(\d+) pages=(\d+) re=(\d+) u=(\d+)", log)
        check("map 打点格式完整（va/pa/w/h/pitch/fmt/pages/re/u）", m is not None)
        if m:
            fbinfo = dict(pid=int(m.group(1)), va=int(m.group(2), 16), w=int(m.group(4)), h=int(m.group(5)),
                          pitch=int(m.group(6)), fmt=int(m.group(7)), u=int(m.group(10)))
            check("内核页表里该页 U/S=1（打点 u=1）", fbinfo["u"] == 1, "u=%d" % fbinfo["u"])
            check("几何自洽（pitch == width*4，fmt=0，va 在用户半区）",
                  fbinfo["pitch"] == fbinfo["w"] * 4 and fbinfo["fmt"] == 0 and
                  fbinfo["va"] >= 0x100000000, "w=%d pitch=%d fmt=%d" % (fbinfo["w"], fbinfo["pitch"],
                                                                        fbinfo["fmt"]))

        mfl = re.search(r"\[FB64\] flip pid=-?\d+ x=(-?\d+) y=(-?\d+) w=(\d+) h=(\d+) clip=ok", vm.log())
        if mfl:
            band = (int(mfl.group(1)), int(mfl.group(2)), int(mfl.group(3)), int(mfl.group(4)))
        check("fb_flip(10) 打点（clip=ok，区域可解析）", band is not None, "band=%s" % (band,))

        triple = None
        pair_stats = []
        if len(shots) >= 3 and band is not None:
            for i in range(len(shots) - 2):
                wa, ha, pa = read_ppm(shots[i][1])
                wb, hb, pb = read_ppm(shots[i + 1][1])
                wc, hc, pc = read_ppm(shots[i + 2][1])
                if (wa, ha) != (wb, hb) or (wa, ha) != (wc, hc):
                    break
                dims = (wa, ha)
                gx = max(0, band[0])
                gy = max(0, band[1])
                gw = min(band[2], wa - gx)
                gh = min(band[3], ha - gy)
                d01, bbox01 = diff_region(pa, pb, wa, ha, gx, gy, gx + gw, gy + gh)
                d12, _ = diff_region(pb, pc, wa, ha, gx, gy, gx + gw, gy + gh)
                need = max(2000, int(gw * gh * INSIDE_MIN_FRAC))
                pair_stats.append((shots[i][0], shots[i + 1][0], shots[i + 2][0], d01, d12, need, bbox01))
                if d01 >= need and d12 >= need:
                    triple = (i, d01, bbox01)
                    break
        for a, b, c, d0, d1, need, _ in pair_stats:
            print("     帧 %d->%d 带内变化=%d ；%d->%d 带内变化=%d（阈值 %d）" % (a, b, d0, b, c, d1, need))
        check("连续 3 帧两两差异 > 阈值（带内变化像素）", triple is not None)
        if triple is not None:
            i, inside, bbox = triple
            wa, ha, pa = read_ppm(shots[i][1])
            wb, hb, pb = read_ppm(shots[i + 1][1])
            gx = max(0, band[0])
            gy = max(0, band[1])
            gw = min(band[2], wa - gx)
            gh = min(band[3], ha - gy)
            bx0, by0, bx1, by1 = bbox
            check("变化区域在 fb_flip 请求的带内（bbox 未越出）",
                  (bx0 >= gx - 2) and (bx1 < gx + gw + 2) and (by0 >= gy - 2) and (by1 < gy + gh + 2),
                  "bbox=(%d,%d)-(%d,%d) band=(%d,%d,%d,%d)" % (bx0, by0, bx1, by1, gx, gy, gw, gh))
            outside, _ = diff_region(pa, pb, wa, ha, 0, 0, wa, ha, step=4)
            outside_est = outside - inside // 4
            check("带外变化远小于带内（内核侧没有画别处）", outside_est <= max(400, inside // 4),
                  "outside_sampled~%d inside=%d" % (outside_est, inside))

        # ---------- 7) 收尾与证据链 ----------
        print("=== 7) 收尾：[FBDEMO] done / clip=reject / clamp / present / 回内核 ===")
        wait_done = vm.wait_log("[FBDEMO] done frames=", 240)
        # 等到"关内核绘制 -> 跑 -> 恢复"这条链和退出打点都落盘再断言（半行竞态）
        log = wait_re([r"\[FBDEMO\] done frames=5",
                       r"\[FB64\] user-draw demo start: kernel paint off",
                       r"\[FB64\] user-draw demo done rc=0 kernel paint on",
                       r"\[USER64\] demo done name=fbdemo_c rc=0"], 240)
        check("C 版演示跑完（[FBDEMO] done frames=5）", wait_done and "[FBDEMO] done frames=5" in log)
        check("演示期间内核侧绘制关闭（[FB64] user-draw demo start … kernel paint off）",
              "[FB64] user-draw demo start" in log and "kernel paint off" in log)
        check("演示结束恢复内核绘制（[FB64] user-draw demo done … kernel paint on）",
              "[FB64] user-draw demo done" in log and "kernel paint on" in log)
        nframe = len(re.findall(r"\[FBDEMO\] frame=\d+ flip=\d+", log))
        check("每帧都有 [FBDEMO] frame=.. flip=.. 打点（>= 4 条）", nframe >= 4, "nframe=%d" % nframe)
        check("越界 flip 被拒绝（[FB64] flip … clip=reject）",
              re.search(r"\[FB64\] flip pid=-?\d+ x=\d+ y=\d+ w=\d+ h=\d+ clip=reject", log) is not None)
        check("用户程序看到越界被拒（[FBDEMO] oob ret=1）",
              re.search(r"\[FBDEMO\] oob ret=1\b", log) is not None)
        check("部分越界被夹取后提交（[FB64] flip … clip=clamped 且 [FBDEMO] clamp ret=0）",
              ("clip=clamped" in log) and re.search(r"\[FBDEMO\] clamp ret=0\b", log) is not None)
        rej_at = log.find("clip=reject")
        check("越界拒绝之后程序仍在跑（reject 行之后还有 frame 打点）",
              (rej_at >= 0) and (log.find("[FBDEMO] frame=", rej_at + 1) > rej_at))
        check("整屏提交调用过（[FB64] present pid=）", "[FB64] present pid=" in log)
        muser = re.search(r"\[FBDEMO\] map va=0x([0-9A-Fa-f]+) w=(\d+) h=(\d+) pitch=(\d+) fmt=(\d+)", log)
        check("用户侧 map 打点（[FBDEMO] map va=0x.. w=.. h=.. pitch=.. fmt=..）", muser is not None)
        if muser and fbinfo:
            uva = int(muser.group(1), 16)
            check("用户看到的 VA 与内核映射的 VA 一致（同一块后备缓冲）", uva == fbinfo["va"],
                  "user=0x%x kernel=0x%x" % (uva, fbinfo["va"]))
            check("用户看到的几何与内核一致",
                  (int(muser.group(2)), int(muser.group(3)), int(muser.group(4)))
                  == (fbinfo["w"], fbinfo["h"], fbinfo["pitch"]))
        check("C 版演示确实在 ring3 里（[USER64] enter ring3 在 [FBDEMO] 之前）",
              ("[USER64] enter ring3" in log) and log.index("[USER64] enter ring3") < log.index("[FBDEMO]"))
        check("演示退出后回到内核（[USER64] back to kernel 在 [FBDEMO] 之后）",
              log.rindex("[USER64] back to kernel") > log.index("[FBDEMO]"))
        check("C 版演示正常退出（[USER64] demo done name=fbdemo_c rc=0）",
              "[USER64] demo done name=fbdemo_c rc=0" in log)

        # ---------- 8) 禁止出现 ----------
        print("=== 8) 禁止出现（自检失败/崩溃/非法参数）===")
        for needle in FORBIDDEN:
            check("不得出现 %s" % needle, needle not in log)

        # ---------- 9) 演示之后系统仍正常（锁屏 -> 显式登录 -> 桌面）----------
        print("=== 9) 三个 C 程序跑完之后系统照常进桌面 ===")
        qh.login_desktop(mon, vm.log, proc=vm.proc, timeout=240)
        up = vm.wait_log("[GUI64] ready", 240)
        check("显式登录后桌面就绪（[GUI64] ready）", up)
        log = vm.log()
        check("桌面 Dock 几何打点还在（[DOCK64] geom）", "[DOCK64] geom" in log)
        check("启动流程照常推进（[APP64]/[STORE64]/[CONF64] 都在）",
              ("[APP64]" in log) and ("[STORE64]" in log) and ("[CONF64]" in log))
        if shots:
            desktop = os.path.join(tmp, "desktop.ppm")
            got = mon.shot(desktop)
            check("桌面帧抓取成功", got)
            if got:
                wd, hd, pd = read_ppm(desktop)
                if dims and (wd, hd) == dims:
                    _, _, plast = read_ppm(shots[-1][1])
                    changed, _ = diff_region(plast, pd, wd, hd, 0, 0, wd, hd, step=4)
                    check("桌面把屏幕接管回去了（整屏与 C 演示帧不同）", changed > 20000,
                          "changed_sampled=%d" % changed)
                else:
                    check("桌面帧分辨率与演示帧一致", False, "%s vs %s" % ((wd, hd), dims))
    finally:
        log = vm.log()
        vm.close()

    print("--- serial tail（A2 证据原文）---")
    for line in [x for x in log.splitlines()
                 if ("[FBDEMO]" in x or "[LIBC]" in x or "[HELLO]" in x or "[USER64] capp" in x
                     or "hello from Vimtu64 C userland" in x)][-16:]:
        print("   | " + line[:180])
    if args.keep:
        print("[userlib64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
