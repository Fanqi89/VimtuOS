#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fbmap64_test.py - A1 验收：**ring3 用户程序自己把画面画到屏幕上**（内核只映射显存 + 提交区域）

覆盖（每条都要 串口打点 + 像素证据）：
  1) fb_map(9)：用户态拿到后备缓冲的映射
       [FB64] map pid=.. va=0x.. pa=0x.. w=.. h=.. pitch=.. fmt=.. pages=.. re=.. u=1
     * va 必须在**用户半区**（>= 4GiB 且在 < 0x0000_8000_0000_0000 之内；不是内核高半区 0xFFFFFFFF8…）；
     * u=1（内核页表里那一页确实映射成了用户可访问页：user64_page_is_user_ok64 的实测值）；
     * 用户侧 [FBDEMO] map 打点里的 va 必须与内核的 [FB64] map va 一致（同一映射）。
  2) fb_flip(10)：用户提交的区域上屏 + **边界夹取**（clip=ok / clip=clamped / clip=reject 三种都打点）
     * [FBDEMO] oob ret=1：完全越界的 flip 被内核拒绝（clip=reject），用户程序**继续跑**；
     * [FBDEMO] clamp ret=0：部分越界的 flip 被夹取后提交（clip=clamped sub=..）。
  3) 像素：按 [FBDEMO] frame= 同步抓帧，连续 3 帧两两差异都超过阈值，且**变化区域落在
     fb_flip 请求的那条带内**（带外几乎不变 —— 演示期间内核侧绘制被关掉，见 [FB64] user-draw demo）。
  4) 演示结束后系统仍正常：锁屏 -> 显式登录（回车两次）-> [GUI64] ready + [DOCK64] geom，
     并且桌面那一帧与演示帧不同（桌面把屏幕接管回去了）。

用法（必须用 Windows 原生 Python）：py -3 tests\\fbmap64_test.py [--img build64/system.img] [--keep]
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

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

KERNEL_HIGH = 0xFFFFFFFF80000000     # 内核高半区起点（用户 VA 绝不能落在这里）
USER_HALF_END = 0x0000800000000000   # 规范地址的用户半区上界

SHOTS_MAX = 9                        # 最多抓几帧（够找"连续三帧"）
INSIDE_MIN_FRAC = 0.05               # 带内变化像素 > 带面积的 5% 才算"这帧真的换了"


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
    def __init__(self, qemu, img, port, serial, name="vimtu-fbmap64"):
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
        """抓一帧到 ppm；返回 True = 文件已就绪（大小已稳定）。"""
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
    """矩形 [x0,x1) × [y0,y1) 内变化的像素数 + 变化像素的 bbox（step = 采样步长）。"""
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
    ap.add_argument("--port", type=int, default=5664)
    ap.add_argument("--keep", action="store_true", help="保留临时目录（打印路径）")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_fbmap_")
    serial = os.path.join(tmp, "serial.log")
    vm = Vm(qemu, args.img, args.port, serial)
    mon = Monitor(args.port)

    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    shots = []          # [(frame_no, ppm_path)]
    band = None         # (x, y, w, h) —— fb_flip 请求的区域（clip=ok 那条）
    fbinfo = {}         # [FB64] map 的解析结果
    dims = None         # (w, h) 屏幕尺寸
    try:
        # ---------- 1) fb_map：内核把后备缓冲映射给 ring3 ----------
        print("=== 1) fb_map：内核把后备缓冲映射给 ring3 ===")
        got_map = vm.wait_log("[FB64] map pid=", 180)
        check("用户程序调到了 fb_map(9)（[FB64] map pid=）", got_map)
        log = vm.log()
        m = re.search(r"\[FB64\] map pid=(-?\d+) va=0x([0-9a-f]+) pa=0x([0-9a-f]+) "
                      r"w=(\d+) h=(\d+) pitch=(\d+) fmt=(\d+) pages=(\d+) re=(\d+) u=(\d+)", log)
        check("map 打点格式完整（va/pa/w/h/pitch/fmt/pages/re/u）", m is not None)
        if m:
            fbinfo = dict(pid=int(m.group(1)), va=int(m.group(2), 16), pa=int(m.group(3), 16),
                          w=int(m.group(4)), h=int(m.group(5)), pitch=int(m.group(6)),
                          fmt=int(m.group(7)), pages=int(m.group(8)), re=int(m.group(9)), u=int(m.group(10)))
            va = fbinfo["va"]
            check("fb_map 返回的用户 VA 在用户半区（4GiB..0x8000_0000_0000）",
                  (va >= 0x100000000) and (va < USER_HALF_END), "va=0x%x" % va)
            check("fb_map 的 VA 不是内核高半区地址", va < KERNEL_HIGH, "va=0x%x" % va)
            check("内核页表里该页 U/S=1（打点 u=1）", fbinfo["u"] == 1, "u=%d" % fbinfo["u"])
            check("几何自洽（pitch == width*4，fmt=0 XRGB8888，pages 覆盖整块）",
                  fbinfo["pitch"] == fbinfo["w"] * 4 and fbinfo["fmt"] == 0 and
                  fbinfo["pages"] >= (fbinfo["h"] * fbinfo["pitch"] + 4095) // 4096,
                  "w=%d h=%d pitch=%d pages=%d" % (fbinfo["w"], fbinfo["h"], fbinfo["pitch"], fbinfo["pages"]))

        # ---------- 2) 像素：按帧同步抓屏 ----------
        print("=== 2) 像素：连续帧差异 + 变化区域 ===")
        last_frame = -1
        t0 = time.time()
        while len(shots) < SHOTS_MAX and time.time() - t0 < 150:
            lg = vm.log()
            mf = re.findall(r"\[FBDEMO\] frame=(\d+) flip=(\d+)", lg)
            if mf and int(mf[-1][0]) > last_frame:
                last_frame = int(mf[-1][0])
                time.sleep(0.2)                     # 让这一帧的 flip 真的落到显示表面
                ppm = os.path.join(tmp, "demo_f%02d.ppm" % last_frame)
                if mon.shot(ppm):
                    shots.append((last_frame, ppm))
            if "[FB64] user-draw demo done" in lg or ("[FBDEMO] done" in lg and len(shots) >= 3):
                break
            time.sleep(0.1)
        check("抓到了 >= 3 帧演示画面", len(shots) >= 3,
              "shots=%d frames=%s" % (len(shots), [s[0] for s in shots]))

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
            outside_est = outside - inside // 4          # 带内贡献按采样比（1:4）扣掉
            check("带外变化远小于带内（内核侧没有画别处）", outside_est <= max(400, inside // 4),
                  "outside_sampled~%d inside=%d" % (outside_est, inside))

        # ---------- 3) 串口证据链 ----------
        print("=== 3) 串口证据链（FB64 / FBDEMO / 越界拒绝）===")
        wait_done = vm.wait_log("[FBDEMO] done frames=", 240)
        check("演示跑完（[FBDEMO] done frames=）", wait_done)
        log = vm.log()
        check("演示期间内核侧绘制关闭（[FB64] user-draw demo start … kernel paint off）",
              "[FB64] user-draw demo start" in log and "kernel paint off" in log)
        check("演示结束恢复内核绘制（[FB64] user-draw demo done … kernel paint on）",
              "[FB64] user-draw demo done" in log and "kernel paint on" in log)
        nframe = len(re.findall(r"\[FBDEMO\] frame=\d+ flip=\d+", log))
        check("每帧都有 [FBDEMO] frame=.. flip=.. 打点（>= 4 条）", nframe >= 4, "nframe=%d" % nframe)
        rej = re.search(r"\[FB64\] flip pid=-?\d+ x=\d+ y=\d+ w=\d+ h=\d+ clip=reject", log)
        check("越界 flip 被拒绝（[FB64] flip … clip=reject）", rej is not None)
        check("用户程序看到越界被拒（[FBDEMO] oob ret=1）",
              re.search(r"\[FBDEMO\] oob ret=1\b", log) is not None)
        check("部分越界被夹取后提交（[FB64] flip … clip=clamped 且 [FBDEMO] clamp ret=0）",
              ("clip=clamped" in log) and re.search(r"\[FBDEMO\] clamp ret=0\b", log) is not None)
        rej_at = log.find("clip=reject")
        check("越界拒绝之后程序仍在跑（reject 行之后还有 frame 打点）",
              (rej_at >= 0) and (log.find("[FBDEMO] frame=", rej_at + 1) > rej_at), "reject_at=%d" % rej_at)
        check("整屏提交调用过（[FB64] present）", "[FB64] present pid=" in log)
        muser = re.search(r"\[FBDEMO\] map va=0x([0-9a-f]+) w=(\d+) h=(\d+) pitch=(\d+) fmt=(\d+)", log)
        check("用户侧 map 打点（[FBDEMO] map va=0x.. w=.. h=.. pitch=.. fmt=..）", muser is not None)
        if muser and fbinfo:
            uva = int(muser.group(1), 16)
            check("用户看到的 VA 与内核映射的 VA 一致（同一块后备缓冲）", uva == fbinfo["va"],
                  "user=0x%x kernel=0x%x" % (uva, fbinfo["va"]))
            check("用户看到的几何与内核一致",
                  (int(muser.group(2)), int(muser.group(3)), int(muser.group(4)))
                  == (fbinfo["w"], fbinfo["h"], fbinfo["pitch"]))
        check("演示确实在 ring3 里（[USER64] enter ring3 在 [FBDEMO] 之前）",
              ("[USER64] enter ring3" in log) and
              log.index("[USER64] enter ring3") < log.index("[FBDEMO]"))
        check("演示退出后回到内核（[USER64] back to kernel 在 [FBDEMO] 之后）",
              log.rindex("[USER64] back to kernel") > log.index("[FBDEMO]"))
        for needle in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "FAILED mask=", "[USER64] run FAILED",
                       "[FB64] map FAILED", "[FBDEMO] fb_map FAILED"):
            check("不得出现 %s" % needle, needle not in log)
        check("恢复内核绘制后启动照常（[CON64] 引导控制台照旧渲染）", "[CON64]" in log)
        print("=== 4) 演示结束后系统仍正常（锁屏 -> 登录 -> 桌面）===")
        lock = vm.wait_log("[LOCK64] bg blur ready", 240)
        check("演示之后锁屏照常出现（[LOCK64] bg blur ready）", lock)
        mon.key("ret", wait=1.0)
        mon.key("ret", wait=1.5)
        up = vm.wait_log("[GUI64] ready", 240)
        check("显式登录后桌面就绪（[GUI64] ready）", up)
        log = vm.log()
        check("桌面 Dock 几何打点还在（[DOCK64] geom）", "[DOCK64] geom" in log)
        check("演示之后启动流程照常推进（[APP64]/[STORE64]/[CONF64] 都还在，没卡在用户绘图上）",
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
                    check("桌面把屏幕接管回去了（整屏与演示帧不同）", changed > 20000,
                          "changed_sampled=%d" % changed)
                else:
                    check("桌面帧分辨率与演示帧一致", False, "%s vs %s" % ((wd, hd), dims))
    finally:
        log = vm.log()
        vm.close()

    print("--- serial tail（FB64/FBDEMO 证据原文）---")
    for line in [l for l in log.splitlines() if "[FB64]" in l or "[FBDEMO]" in l][-14:]:
        print("   | " + line[:180])
    if args.keep:
        print("[fbmap64] 临时目录：%s" % tmp)
    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
