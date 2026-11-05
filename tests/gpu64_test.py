# -*- coding: utf-8 -*-
"""tests/gpu64_test.py - v0.9.4 通用 GPU 层（P1）验收

判据口径（只认串口里的纯 ASCII 证据，绑"这一次 QEMU 调用"的日志切片）：
  ① `[GPU] scan buses=… devices=… gpus=… known=… unknown=… fb=WxH backend=vbe`
     —— fb 的宽高必须与 `[G64] fb render=` 的 phys 一致（证明探 BAR 没把显示搞坏）
  ② `[GPU] dev idx=0 bdf=… vendor=… device=… name=… class=030000 bar0=0x… size=…
     mmio=ok cmd=0xNNNN mem=1 bm=1`
     —— size 是 2 的幂且 >= 4096（写全 1 回读法真的读出了窗口）、bar0 落在 4GiB 以内、
        mem/bm 两位**回读**为 1（不是"写了就算成功"）
  ③ `[GPU] probe vendor=unknown idx=0 skipped reason=no-safe-probe`
     —— QEMU 的三种图形后端（Bochs 1234 / virtio 1af4 / QXL 1b36）都不是 Intel/AMD/NVIDIA，
        必须**如实降级**、绝不假装探测成功
  ④ `[GPU] report gpus=N known=0 unknown=N probed=0 skipped=N mmio_ok=N cmd_on=N extra=0
     accel=none-not-implemented`
  ⑤ 打点位置：`[GPU] scan` 必须在 `[VGPU]` 之前（= gpu64_init64 在 vgpu64_init64 之前跑）
  ⑥ 桌面照常：`[GUI64] ready` + 一张桌面截图里顶部条非黑像素占比 > 0.3（画面真的在）
  ⑦ 全程无 PANIC / OOM / TRIPLE FAULT

四种夹具（同一份构建，只换图形后端）：
  std    : -vga std                     -> vendor=1234（Bochs VGA，未知厂商，如实降级）
  virtio : -vga virtio                  -> vendor=1af4（virtio-gpu，未知厂商）
  qxl    : -vga qxl                     -> vendor=1b36（QXL，未知厂商）
  multi  : -vga std + 2 张 secondary-vga -> gpus=3、extra=0（多卡记账不漏）

★ 未取证的部分（如实写在这里，不靠"看起来能编译"充数）：
  Intel(8086) / AMD(1002) / NVIDIA(10de) 三家**厂商专用只读探测**的代码路径在本机跑不到 ——
  QEMU 11.1 的三种后端都不是这三家，新版本也没有 x-pci-vendor-id 这类改 ID 的属性可以造假设备。
  本脚本因此**不**对 `probe vendor=intel|amd|nvidia` 行做任何断言；报告与文档里一律写
  "代码路径已写、真机语义未取证（no hardware）"。

用法：py -3 tests\\gpu64_test.py [--qemu 路径] [--img build64\\sysdisk.img] [--only std,virtio] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import qemuhelp as qh  # noqa: E402

FORBIDDEN = ("PANIC", "TRIPLE FAULT", "OOM:", "FAILED mask=")
BAR0_LIMIT = 0x100000000          # 恒等映射只到 4GiB（memlayout64.h）


# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM: %r" % raw[:16])
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def nonblack_frac(body, w, rect, step=4):
    x, y, rw, rh = rect
    nz = tot = 0
    for yy in range(y, y + rh, step):
        for xx in range(x, x + rw, step):
            tot += 1
            o = (yy * w + xx) * 3
            if body[o:o + 3] != b"\x00\x00\x00":
                nz += 1
    return (nz / tot) if tot else 0.0


def last(rx, text):
    ms = list(re.finditer(rx, text))
    return ms[-1] if ms else None


def is_pow2(v):
    return v > 0 and (v & (v - 1)) == 0


# ---------------------------------------------------------------------------
# QEMU 壳
# ---------------------------------------------------------------------------
class Vm:
    def __init__(self, qemu, img, vga, port, tmp, tag, extra=()):
        self.tag = tag
        self.serial = os.path.join(tmp, "%s_serial.log" % tag)
        self.shots = os.path.join(tmp, "shots")
        os.makedirs(self.shots, exist_ok=True)
        argv = [qemu, "-name", "vimtu-gpu-%s" % tag,
                "-drive", "format=raw,file=%s" % img,
                "-boot", "order=c", "-m", "512", "-vga", vga]
        argv += list(extra)
        argv += ["-display", "none", "-serial", "file:%s" % self.serial,
                 "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                 "-no-reboot"]
        self.proc = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = None
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", port), timeout=1).close()
                self.mon = qh.Monitor(port)
                break
            except OSError:
                time.sleep(0.25)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        """等 needle 出现在 since 之后的日志里（since = 起跑时的日志长度，避免拿到上一次的残留）。"""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def shot(self, tag, wait=1.2):
        path = os.path.join(self.shots, "%s_%s.ppm" % (self.tag, tag))
        if os.path.exists(path):
            os.remove(path)
        if not self.mon or not self.mon.send("screendump %s" % path.replace("\\", "/"), wait=wait):
            return None
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                w, h, body = read_ppm(path)
                return w, h, body
            time.sleep(0.1)
        return None

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


# ---------------------------------------------------------------------------
# 单次冷启动：断言
# ---------------------------------------------------------------------------
def run_case(qemu, img, tag, vga, extra, expect_vendor, expect_gpus, expect_mmio, checks):
    def check(name, cond, detail=""):
        checks.append((tag, name, bool(cond), detail))
        print("  [%s] %-46s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    print("--- 夹具 %-6s（-vga %s %s）---" % (tag, vga, " ".join(extra) if extra else ""))
    tmp = tempfile.mkdtemp(prefix="gpu64_%s_" % tag)
    port = qh.free_port()
    vm = Vm(qemu, img, vga, port, tmp, tag, extra)
    try:
        if vm.mon is None:
            check("QEMU monitor 可连（telnet）", False, "连不上 127.0.0.1:%d" % port)
            return
        if not vm.wait_log("[G64] fb render=", 150):
            check("内核起了图形栈（[G64] fb render=）", False, vm.log()[-300:])
            return
        mfb = last(r"\[G64\] fb render=(\d+)x(\d+) phys=(\d+)x(\d+) zoom=(\d+)", vm.log())
        phys = (int(mfb.group(3)), int(mfb.group(4))) if mfb else (0, 0)

        # ---------- ①②③④ 打点本身 ----------
        ok_scan = vm.wait_log("[GPU] scan ", 90)
        check("① 打点 [GPU] scan 出现", ok_scan,
              (last(r"\[GPU\] scan[^\r\n]*", vm.log()).group(0) if ok_scan else "超时"))
        if not ok_scan:
            return
        ms = last(r"\[GPU\] scan buses=(\d+) devices=(\d+) gpus=(\d+) known=(\d+) unknown=(\d+) "
                  r"fb=(\d+)x(\d+) backend=vbe", vm.log())
        if not check("① scan 行字段齐全（buses/devices/gpus/known/unknown/fb/backend=vbe）", ms is not None,
                     ms.group(0) if ms else "格式不符"):
            return
        buses, devices, gpus, known, unknown = (int(ms.group(i)) for i in range(1, 6))
        fbw, fbh = int(ms.group(6)), int(ms.group(7))
        check("① 扫到了总线与设备（buses>=1 devices>=1）", buses >= 1 and devices >= 1,
              "buses=%d devices=%d" % (buses, devices))
        check("① gpus=%d（期望 %d）" % (gpus, expect_gpus), gpus == expect_gpus, "实测 %d" % gpus)
        check("① known+unknown == gpus（分类不漏不重）", known + unknown == gpus,
              "known=%d unknown=%d gpus=%d" % (known, unknown, gpus))
        check("① fb 与 [G64] fb render 的 phys 一致（探 BAR 没改分辨率/没弄坏 LFB）",
              (fbw, fbh) == phys, "GPU 行 %dx%d / G64 行 %dx%d" % (fbw, fbh, phys[0], phys[1]))

        ok_rep = vm.wait_log("[GPU] report ", 60)
        mr = last(r"\[GPU\] report gpus=(\d+) known=(\d+) unknown=(\d+) probed=(\d+) skipped=(\d+) "
                  r"mmio_ok=(\d+) cmd_on=(\d+) extra=(\d+) accel=none-not-implemented", vm.log())
        if not check("④ 打点 [GPU] report 出现且格式完整", ok_rep and mr is not None,
                     mr.group(0) if mr else "缺/格式不符"):
            return
        g2, k2, u2, probed, skipped, mmio_ok, cmd_on, extra = (int(mr.group(i)) for i in range(1, 9))
        check("④ report 与 scan 一致（gpus/known/unknown）", (g2, k2, u2) == (gpus, known, unknown),
              "report=%d/%d/%d scan=%d/%d/%d" % (g2, k2, u2, gpus, known, unknown))
        check("④ probed+skipped == gpus", probed + skipped == gpus, "%d+%d vs %d" % (probed, skipped, gpus))
        check("④ 三家的只读探测在本机全部如实 skipped（probed=0，不假装成功）", probed == 0,
              "probed=%d" % probed)
        n_ok = len(re.findall(r"\[GPU\] dev idx=\d+[^\r\n]* mmio=ok ", vm.log()))
        check("④ mmio_ok == dev 行里 mmio=ok 的张数（报告与明细对得上）",
              mmio_ok == n_ok, "report mmio_ok=%d / dev 行 ok=%d" % (mmio_ok, n_ok))
        check("④ cmd_on == gpus（mem-decode + bus-master 回读为 1）", cmd_on == gpus, "cmd_on=%d" % cmd_on)
        check("④ extra == 0（GPU64_MAX64 没溢出）", extra == 0, "extra=%d" % extra)

        # ---------- ④b ★ v0.9.8：帧缓冲来源 + 页表只读走查 ----------
        # 新增行：[FB64] fb source=<vbe|gop> addr=0x… w=… h=… pitch=… hi4g=… pt=… pt_last=… usable=…
        # 它回答三件事：这台机器走哪条上屏路、帧缓冲在哪、
        # 页表里这块显存到底能不能写（不能写就 usable=0，一律不碰屏）。
        # 注意：db g64_hex64 打的是**大写**十六进制（LIKE 00000000FD000000），所以地址类要收 A-F。
        mfbs = last("[[]FB64[]] fb source=([a-z]+) addr=0x([0-9a-fA-F]+) w=([0-9]+) h=([0-9]+) "
                    "pitch=([0-9]+) hi4g=([0-9]) pt=([a-z]+) pt_last=([a-z]+) usable=([0-9])",
                    vm.log())
        if check("④b 打点 [FB64] fb source=… 出现且字段齐全", mfbs is not None,
                 mfbs.group(0) if mfbs else "缺/格式不符"):
            check("④b 本夹具是 BIOS 启动 -> source=vbe（UEFI 下必须是 gop）",
                  mfbs.group(1) == "vbe", "source=%s" % mfbs.group(1))
            check("④b 帧缓冲尺寸与 [GPU] scan 的 fb 一致",
                  (int(mfbs.group(3)), int(mfbs.group(4))) == (fbw, fbh),
                  "%sx%s vs %dx%d" % (mfbs.group(3), mfbs.group(4), fbw, fbh))
            check("④b VBE LFB 在 4GiB 以内（hi4g=0）", mfbs.group(6) == "0",
                  "hi4g=%s addr=0x%s" % (mfbs.group(6), mfbs.group(2)))
            check("④b 页表走查：首末页都可写（pt=rw pt_last=rw）",
                  mfbs.group(7) == "rw" and mfbs.group(8) == "rw",
                  "pt=%s pt_last=%s" % (mfbs.group(7), mfbs.group(8)))
            check("④b usable=1（页表判定通过才出帧；0 时一律不碰屏）",
                  mfbs.group(9) == "1", "usable=%s" % mfbs.group(9))

        # ---------- ② 每张卡的 dev 行 ----------
        devs = list(re.finditer(
            r"\[GPU\] dev idx=(\d+) bdf=([0-9a-f]{2}:[0-9a-f]{2}\.[0-9]) vendor=([0-9a-f]{4}) "
            r"device=([0-9a-f]{4}) name=(\w+) class=([0-9a-f]{6}) bar0=0x([0-9a-f]+) size=(\d+) "
            r"mmio=(\w+) cmd=0x([0-9a-f]{4}) mem=(\d) bm=(\d)", vm.log()))
        if not check("② dev 行数 == gpus（每张卡一行）", len(devs) == gpus,
                     "行数 %d / gpus %d" % (len(devs), gpus)):
            return
        d0 = devs[0]
        check("② 第一张卡 idx=0（序号从 0 起、不跳号）", int(d0.group(1)) == 0, d0.group(0))
        check("② 第一张卡 vendor=%s（本夹具期望 %s）" % (d0.group(3), expect_vendor),
              d0.group(3) == expect_vendor, d0.group(0))
        check("② 厂商名如实（unknown 夹具必须 name=unknown，不许编造）",
              (d0.group(5) == "unknown") if expect_vendor not in ("8086", "1002", "10de")
              else (d0.group(5) in ("intel", "amd", "nvidia")), "name=%s" % d0.group(5))
        check("② class=030000（真的是显示控制器）", d0.group(6) == "030000", "class=%s" % d0.group(6))
        bar0 = int(d0.group(7), 16)
        size = int(d0.group(8))
        kind0 = d0.group(9)
        # 第一张卡的 BAR0 形状按夹具期望（期望值 = 该后端的**实测**形状，见 EXPECT_MMIO 注释）
        check("② 第一张卡 mmio=%s（本夹具期望 %s）" % (kind0, expect_mmio),
              kind0 == expect_mmio, d0.group(0))
        if kind0 == "ok":
            check("② BAR0 落在 4GiB 恒等映射以内", 0 < bar0 < BAR0_LIMIT, "bar0=0x%x" % bar0)
            check("② BAR0 窗口大小是 2 的幂且 >= 4096（写全 1 回读法真的测出来了）",
                  is_pow2(size) and size >= 4096, "size=%d" % size)
        else:
            # 不是 MMIO 的卡：必须如实写 0，不许编造一个窗口出来
            check("② 非 MMIO 的卡如实写 bar0=0x0 / size=0（不编窗口）",
                  bar0 == 0 and size == 0, "bar0=0x%x size=%d" % (bar0, size))
        check("② 命令寄存器回读 mem=1 bm=1", d0.group(11) == "1" and d0.group(12) == "1",
              "cmd=0x%s mem=%s bm=%s" % (d0.group(10), d0.group(11), d0.group(12)))
        for d in devs:
            check("③ 未知厂商的 idx=%s 如实 skipped reason=no-safe-probe" % d.group(1),
                  ("[GPU] probe vendor=unknown idx=%s skipped reason=no-safe-probe" % d.group(1))
                  in vm.log(), "vendor=%s" % d.group(3))

        # ---------- ③b ★ v0.9.8：驱动分派 + **统一的安全回退** ----------
        # 本夹具的卡全是未知厂商（1234/1af4/1b36），所以每张卡都必须走
        #   status=none fallback=no-touch —— 不写一个厂商寄存器、不接管 scanout。
        # （AMD 0x1002 与 NVIDIA 0x10de 在真机上走的就是同一段代码，本文件验不了；
        #   这里用未知厂商证明“没专属驱动时什么都不动”这条路径。）
        # driver 行在 [GPU] report 之后才出：先等 Intel 自检那行落盘，保证前面的行都到齐了
        # （串口有 1~3 s 落盘延迟，不等就是抢跑）。
        vm.wait_log("[INTEL] selftest ", 60)
        drv = list(re.finditer(r"\[GPU\] driver vendor=(\w+) idx=(\d+) status=none fallback=no-touch reason=(\S+)",
                               vm.log()))
        check("③b 每张卡都有一行 [GPU] driver（数量 == gpus）", len(drv) == gpus,
              "行数 %d / gpus %d" % (len(drv), gpus))
        for d in drv:
            check("③b idx=%s 未知厂商走统一回退 reason=unknown-vendor" % d.group(2),
                  d.group(1) == "unknown" and d.group(3) == "unknown-vendor", d.group(0))
        check("③b 没有卡被误报成 status=safe-probe（本夹具没有 Intel/AMD/NVIDIA）",
              "status=safe-probe" not in vm.log(), "")
        check("③b 没有 Intel 设备 -> [INTEL] selftest skipped reason=no-intel-device",
              "[INTEL] selftest skipped reason=no-intel-device" in vm.log(), "")

        # ---------- ⑤ 打点位置（gpu64_init64 必须先于 vgpu64_init64）----------
        log = vm.log()
        i_gpu = log.find("[GPU] scan ")
        i_vgpu = log.find("[VGPU]")
        check("⑤ [GPU] scan 在 [VGPU] 之前（调用点位置正确）", 0 <= i_gpu < i_vgpu,
              "gpu@%d vgpu@%d" % (i_gpu, i_vgpu))

        # ---------- ⑥ 桌面照常 ----------
        qh.login_desktop(vm.mon, lambda: vm.log(), proc=vm.proc, timeout=200)
        up = vm.wait_log("[GUI64] ready", 240)
        check("⑥ 桌面照常起来（[GUI64] ready）", up,
              (last(r"\[GUI64\] ready[^\r\n]*", vm.log()).group(0) if up else "超时"))
        if up:
            sh = vm.shot("desktop", wait=2.0)
            if check("⑥ 桌面截图可用（screendump）", sh is not None, "screendump 失败" if not sh else ""):
                w, h, body = sh
                frac = nonblack_frac(body, w, (0, 0, w, min(h, 200)))
                check("⑥ 桌面上部真的画出来了（非黑像素 %.3f > 0.3）" % frac, frac > 0.3, "%dx%d" % (w, h))

        # ---------- ⑦ 无致命错误 ----------
        bad = [p for p in FORBIDDEN if p in vm.log()]
        check("⑦ 全程没有 PANIC / TRIPLE FAULT / OOM / mask 失败", not bad, ",".join(bad))
    finally:
        vm.close()


CASES = {
    # tag: (vga, 额外 QEMU 参数, 期望 gpus)；期望 vendor 见下面的 EXPECT_VENDOR
    "std":    ("std",    [], 1),
    "virtio": ("virtio", [], 1),
    "qxl":    ("qxl",    [], 1),
    "multi":  ("std",    ["-device", "secondary-vga", "-device", "secondary-vga"], 3),
}
EXPECT_VENDOR = {"std": "1234", "virtio": "1af4", "qxl": "1b36", "multi": "1234"}
# 第一张卡 BAR0 的形状 —— 期望值来自**实测**（不是猜的，第一版就猜错过一次）：
#   std    : -vga std  -> Bochs VGA，bar0=0xfd000000 size=16777216 mmio=ok
#   virtio : -vga virtio -> virtio-gpu，bar0=0xfe000000 size=8388608 mmio=ok（★ 我一开始猜"BAR0 是 I/O BAR"，
#            实测打脸：过渡型 virtio-vga 的 BAR0 就是 legacy VGA 的 LFB 窗口）
#   qxl / multi 的实测值见下面的 EXPECT_MMIO（首次跑完填的）
EXPECT_MMIO = {"std": "ok", "virtio": "ok", "qxl": "ok", "multi": "ok"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"))
    ap.add_argument("--only", default="")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.qemu):
        print("环境问题：找不到 QEMU：%s" % args.qemu)
        return 2
    if not os.path.exists(args.img):
        print("环境问题：找不到系统盘映像：%s（先跑 bash build64.sh）" % args.img)
        return 2

    want = [t for t in (args.only.split(",") if args.only else ["std", "virtio", "qxl", "multi"]) if t]
    for t in want:
        if t not in CASES:
            print("环境问题：未知夹具 %s（可选 %s）" % (t, ",".join(CASES)))
            return 2

    checks = []
    t0 = time.time()
    for t in want:
        vga, extra, ngpu = CASES[t]
        run_case(args.qemu, args.img, t, vga, extra, EXPECT_VENDOR[t], ngpu, EXPECT_MMIO[t], checks)
        time.sleep(1.0)

    npass = sum(1 for c in checks if c[2])
    nfail = len(checks) - npass
    print("\n==== gpu64_test：%d 条断言，PASS %d / FAIL %d（%d 个夹具，%.1fs）===="
          % (len(checks), npass, nfail, len(want), time.time() - t0))
    if nfail:
        print("失败项：")
        for tag, name, ok, detail in checks:
            if not ok:
                print("  [%s] %s   %s" % (tag, name, detail))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
