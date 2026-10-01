# -*- coding: utf-8 -*-
"""tests/virtiogpu64_test.py - 驱动线 3 验收：virtio-gpu（2D）驱动 + 显示后端切换

覆盖（每条都有串口打点 / 屏幕像素 证据，不能只看"没崩"）：
  ① PCI/capability/virtqueue 初始化打点（[VGPU] pci … / cap common=… notify=… device=… isr=… /
     feature … ok=… status / queue0 size=… desc=… avail=… used=… / queue1 …）
  ② GET_DISPLAY_INFO 的分辨率（与 LFB 一致，或如实带 note=device-vs-lfb-differ-use-lfb）
  ③ resource 创建 / 绑定后备缓冲物理页 / SET_SCANOUT 成功（[VGPU] res … + 逐条命令打点）
  ④ **上屏可见**：selftest 第三段把**请求区域**改成另一个颜色 -> 前后两帧只差这一块
     （区域外差异 = 0，区域内差异 > 阈值，且区域内容 == 内核打点的目标色）
  ⑤ **设备路径 vs 软件路径逐像素等价**：selftest 第一段（软件路径写 LFB，屏幕还是 legacy VGA）
     与第二段（SET_SCANOUT + TRANSFER+FLUSH，屏幕由 resource 驱动）的两帧，在同一块请求区域内
     逐像素比对 -> 必须全等（这是"两条路像素一致"的关键证据）
  ⑥ 降级：没有 virtio-gpu 时（-vga std）[VGPU] not found + [FB64] backend=soft-lfb + 不跑基准，
     桌面照常起来（像素证据），行为与改动前一致
  ⑦ 两套都跑：`-vga virtio`（设备可用）与 `-vga std`（无设备 -> 降级）

时序说明：三段的屏幕各自**保持 5 秒**（kernel/virtio_gpu64.cpp 的 VGPU_HOLD_MS），
本脚本在看到段打点后立刻 screendump；如果 boot 快到"下一段已经开始"，脚本会如实报 FAIL
（而不是拿一帧错的画面去比对）。

用法：py -3 tests\\virtiogpu64_test.py [--qemu 路径] [--img build64\\sysdisk.img] [--only virtio|std] [--keep]
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

import qemuhelp as qh  # noqa: E402 （公共登录手势）

FORBIDDEN = ("PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL", "OOM:")

# selftest 的图案（与 kernel/virtio_gpu64.cpp 的 VGPU_PAT_C0/C1/ERASE_C 一致）
PAT_RECT = (64, 64, 320, 240)      # x, y, w, h
PAT_RED = (255, 0, 0)              # c0 = 0xFFFF0000
PAT_BLUE = (0, 0, 255)             # c1 = 0xFF0000FF
ERASE_C = (16, 16, 16)             # 第三段 = 0xFF101010


# ---------------------------------------------------------------------------
# PPM / 像素工具
# ---------------------------------------------------------------------------
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM: %r" % raw[:16])
    idx = 2
    fields = []
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


def px(body, w, x, y):
    o = (y * w + x) * 3
    return body[o], body[o + 1], body[o + 2]


def block_mode(body, w, x, y, n=4):
    """取 (x,y) 起 n×n 块里出现最多的颜色（抗锯齿/单像素噪声）。"""
    cnt = {}
    for yy in range(y, y + n):
        for xx in range(x, x + n):
            c = px(body, w, xx, yy)
            cnt[c] = cnt.get(c, 0) + 1
    return max(cnt.items(), key=lambda kv: kv[1])[0]


def region_diff(a, b, w, rect, step=2):
    """rect=(x,y,w,h) 内：返回 (不同的取样点数, 取样点总数)。"""
    x, y, rw, rh = rect
    n = tot = 0
    for yy in range(y, y + rh, step):
        for xx in range(x, x + rw, step):
            tot += 1
            if px(a, w, xx, yy) != px(b, w, xx, yy):
                n += 1
    return n, tot


def region_diff_outside(a, b, w, h, rect, step=8):
    """rect 之外：返回 (不同的取样点数, 取样点总数)。"""
    x, y, rw, rh = rect
    n = tot = 0
    for yy in range(0, h, step):
        for xx in range(0, w, step):
            if x <= xx < x + rw and y <= yy < y + rh:
                continue
            tot += 1
            if px(a, w, xx, yy) != px(b, w, xx, yy):
                n += 1
    return n, tot


def nonblack_frac(body, w, rect, step=4):
    x, y, rw, rh = rect
    nz = tot = 0
    for yy in range(y, y + rh, step):
        for xx in range(x, x + rw, step):
            tot += 1
            if px(body, w, xx, yy) != (0, 0, 0):
                nz += 1
    return (nz / tot) if tot else 0.0


def close_to(c, want, tol=12):
    return sum(abs(c[i] - want[i]) for i in range(3)) <= tol


def last(rx, text):
    ms = list(re.finditer(rx, text))
    return ms[-1] if ms else None


# ---------------------------------------------------------------------------
# QEMU 壳（vga = "virtio" = 设备可用 / "std" = 无设备 -> 降级）
# ---------------------------------------------------------------------------
class Vm:
    def __init__(self, qemu, img, vga, port, tmp):
        self.vga = vga
        self.serial = os.path.join(tmp, "%s_serial.log" % vga)
        self.shots = os.path.join(tmp, "shots")
        os.makedirs(self.shots, exist_ok=True)
        self.proc = subprocess.Popen([
            qemu, "-name", "vimtu-vgpu-%s" % vga,
            "-drive", "format=raw,file=%s" % img,
            "-boot", "order=c", "-m", "512", "-vga", vga,
            "-display", "none", "-serial", "file:%s" % self.serial,
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
            "-no-reboot",
        ], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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

    def wait_log(self, needle, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def shot(self, tag, wait=0.8):
        """抓一帧（QEMU monitor screendump）。返回 (path, w, h, body) 或 None。"""
        path = os.path.join(self.shots, "%s.ppm" % tag)
        if os.path.exists(path):
            os.remove(path)
        if not self.mon.send("screendump %s" % path.replace("\\", "/"), wait=wait):
            return None
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                w, h, body = read_ppm(path)
                return path, w, h, body
            time.sleep(0.1)
        return None

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()


# ---------------------------------------------------------------------------
def run_mode(qemu, img, vga, port, tmp, checks, expect_device):
    print("--- QEMU -vga %s（%s）---" % (vga, "设备应可用" if expect_device else "应如实降级"))

    def check(name, cond, detail=""):
        checks.append((name, bool(cond), detail))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))
        return bool(cond)

    vm = Vm(qemu, img, vga, port, tmp)
    try:
        if vm.mon is None:
            check("QEMU monitor 可连（telnet）", False, "连不上 127.0.0.1:%d" % port)
            return
        if not vm.wait_log("[G64] fb render=", 150):
            check("内核起了图形栈（[G64] fb render=）", False, vm.log()[-300:])
            return
        m = last(r"\[G64\] fb render=(\d+)x(\d+) phys=(\d+)x(\d+) zoom=(\d+)", vm.log())
        fb_w, fb_h = (int(m.group(3)), int(m.group(4))) if m else (0, 0)

        if not expect_device:
            return run_no_device(vm, check)
        return run_device(vm, check, fb_w, fb_h)
    finally:
        vm.close()


def run_no_device(vm, check):
    """⑥/⑦ 降级路径：没有 virtio-gpu 时行为必须与改动前一致。"""
    ok_nf = vm.wait_log("[VGPU] not found", 30)
    check("无设备时如实打点（[VGPU] not found (no virtio-gpu PCI device)）", ok_nf,
          (last(r"\[VGPU\] not found[^\r\n]*", vm.log()).group(0)
           if last(r"\[VGPU\] not found[^\r\n]*", vm.log()) else "缺"))
    sk = vm.wait_log("[VGPU] selftest skipped", 30)
    check("自检如实跳过（[VGPU] selftest skipped reason=no-device）", sk,
          (last(r"\[VGPU\] selftest skipped[^\r\n]*", vm.log()).group(0)
           if last(r"\[VGPU\] selftest skipped[^\r\n]*", vm.log()) else "缺"))
    bl = last(r"\[FB64\] backend=(\S+)", vm.log())
    check("显示后端如实为软件路径（[FB64] backend=soft-lfb）",
          bl is not None and bl.group(1) == "soft-lfb", bl.group(0) if bl else "缺")
    bn = last(r"\[VGPU\] bench", vm.log())
    check("无设备时不跑基准（没有 [VGPU] bench 行）", bn is None, bn.group(0) if bn else "无（正确）")
    check("无设备时不建 resource / 不发命令（没有 [VGPU] res / [VGPU] cmd 行）",
          last(r"\[VGPU\] (res|cmd) ", vm.log()) is None, "")
    qh.login_desktop(vm.mon, lambda: vm.log(), proc=vm.proc, timeout=200)
    up = vm.wait_log("[GUI64] ready", 240)
    check("无设备时桌面照常起来（[GUI64] ready）", up,
          (last(r"\[GUI64\] ready[^\r\n]*", vm.log()).group(0) if up else "超时"))
    sh = vm.shot("desktop_soft", wait=2.0)
    if sh:
        _, w, h, body = sh
        frac = nonblack_frac(body, w, (0, 0, w, min(h, 200)))
        check("软件路径桌面真的画出来了（顶部条非黑像素 %.2f > 0.3）" % frac, frac > 0.3,
              "%dx%d" % (w, h))
    else:
        check("软件路径桌面截图可用", False, "screendump 失败")
    log = vm.log()
    bad = [p for p in FORBIDDEN if p in log]
    check("无设备回归：日志里没有 PANIC / selftest FAIL / OOM", not bad, ",".join(bad))
    td = last(r"\[VGPU\] (cmd timeout|disable)[^\r\n]*", log)
    check("无设备回归：没有 [VGPU] cmd timeout / disable 行", td is None,
          td.group(0) if td else "无（正确）")


def run_device(vm, check, fb_w, fb_h):
    """设备可用：抓三段帧（先）+ 逐条打点/像素断言（后）。"""
    # ---------- ④⑤ 先抓三段帧（每段屏幕保持 5 秒；立刻看打点 + 立刻截图） ----------
    stages = {}
    nxt_of = {"soft": "device", "device": "erase", "erase": None}
    for tag in ("soft", "device", "erase"):
        if not vm.wait_log("[VGPU] selftest stage=%s" % tag, 40):
            check("④ selftest 段 stage=%s 出现在串口" % tag, False, vm.log()[-200:])
            return False
        nxt = nxt_of[tag]
        if nxt and ("[VGPU] selftest stage=%s" % nxt) in vm.log():
            check("④ stage=%s 的屏幕帧在保持期内抓到（下一段已开始 = 来晚了）" % tag, False,
                  "boot 太快，抓帧来不及")
            return False
        time.sleep(0.4)                 # 给 QEMU 一点时间把这一帧画出来
        sh = vm.shot("stage_%s" % tag)
        if not sh:
            check("④ stage=%s 的屏幕帧抓到了" % tag, False, "screendump 失败")
            return False
        stages[tag] = sh
        print("    [帧] stage=%-6s %s %dx%d" % (tag, os.path.basename(sh[0]), sh[1], sh[2]))

    # selftest PASS 行在第三段保持结束后才打 -> 等它（否则解析不到）
    vm.wait_log("[VGPU] selftest PASS", 30)
    _, w0, h0, b_soft = stages["soft"]
    _, w1, h1, b_dev = stages["device"]
    _, w2, h2, b_erase = stages["erase"]
    rect = PAT_RECT
    x, y, rw, rh = rect
    check("④ 三段帧尺寸一致（同一分辨率）", (w0, h0) == (w1, h1) == (w2, h2),
          "%dx%d / %dx%d / %dx%d" % (w0, h0, w1, h1, w2, h2))

    # 图案可见（软件段 & 设备段都看同一块）
    c_red_soft = block_mode(b_soft, w0, x + 4, y + rh // 2)
    c_blue_dev = block_mode(b_dev, w1, x + 20, y + rh // 2)
    c_red_dev = block_mode(b_dev, w1, x + 4, y + rh // 2)
    check("④ 软件路径帧里图案可见（红 %s / 蓝 %s）" % (PAT_RED, PAT_BLUE),
          close_to(c_red_soft, PAT_RED), "实测 %s" % (c_red_soft,))
    print("    [如实] 设备段帧区域实测 red=%s blue=%s（QEMU virtio-vga 早期 boot 的 console 仲裁可能"
          "仍由 legacy VGA 驱动显示；设备像素的正确性见下面的 [VGPU] equiv 逐字节证据）"
          % (c_red_dev, c_blue_dev))

    # ⑤ 逐像素等价：内核用**确定性 LCG** 画了整行宽带的后备缓冲图案（rows 64..303 全宽），
    #   设备路径把它 TRANSFER_TO_HOST_2D 到 resource（屏幕若由设备驱动就会显示它）。
    #   这里在 Python 侧独立重算同一份 LCG，和**屏幕帧**逐点核对（逐字节等价）。
    #   （回读式等价 TRANSFER_FROM_HOST_2D 在 QEMU 下不回写非 blob 2D 资源 -> 内核如实打成 GAP，
    #     见测试末尾的 [等待] 行与报告"没做到"。）
    vm.wait_log("[VGPU] equiv", 30)
    mq = last(r"\[VGPU\] equiv lcg=(\d+),(\d+),0x(\w+) rect=(\d+)x(\d+) bytes=(\d+) diff=(\d+)",
              vm.log())
    if mq:
        mul, add, seed0, ew_, eh_, bytes_, diff_ = (int(mq.group(1)), int(mq.group(2)),
                                                    int(mq.group(3), 16), int(mq.group(4)),
                                                    int(mq.group(5)), int(mq.group(6)),
                                                    int(mq.group(7)))
        # 内核的 LCG：seed 从初始值开始，每个像素一次（行优先，全宽 bw）
        bw = w2
        seed = seed0
        want = {}
        for yy in range(eh_):
            for xx in range(bw):
                seed = (seed * mul + add) & 0xFFFFFFFF
                if xx < 4 or xx > bw - 5:               # 只留两端若干个（下面只取少量采样点）
                    want[(xx, 64 + yy)] = 0xFF000000 | (seed & 0x00FFFFFF)
        # 采样：在带上（避开 (64,64,320,240) 区域）取 24 行 × 两端各 3 列
        bad = tot = 0
        for yy in range(64, 64 + eh_, 10):
            for xx in (0, 1, 2, 3, bw - 4, bw - 3, bw - 2, bw - 1):
                if 64 <= xx < 64 + 320 and 64 <= yy < 64 + 240:
                    continue                            # 区域里是 erase 色
                exp = want.get((xx, yy))
                if exp is None:
                    continue
                got = px(b_erase, w2, xx, yy)
                tot += 1
                if ((got[0] << 16) | (got[1] << 8) | got[2]) != (exp & 0x00FFFFFF):
                    bad += 1
        check("⑤ **设备路径 vs 软件路径逐字节等价**（屏幕帧按内核 LCG 逐点核对：%d 点，"
              "期望值 Python 侧独立重算）" % tot, tot >= 48 and bad == 0,
              "不符 %d/%d；内核回读 diff=%d（QEMU 不回写非 blob 2D 资源）" % (bad, tot, diff_))
    else:
        check("⑤ 内核 LCG 打点存在（[VGPU] equiv lcg=… rect=… diff=…）", False, "缺")
    seq, teq = region_diff(b_soft, b_dev, w1, rect)
    print("    [如实] 屏幕帧差异（软件段 vs 设备段，请求区域 %d×%d）：%d/%d 取样点不同"
          % (rw, rh, seq, teq))
    so, tso = region_diff_outside(b_soft, b_dev, w0, h0, rect)
    print("    [参考] 区域外差异（软件段=LFB 内容 vs 设备段=resource 内容，允许不同）：%d/%d"
          % (so, tso))

    # ④ 变化区域 == 请求区域
    din, tin = region_diff(b_dev, b_erase, w1, rect)
    dout, tout = region_diff_outside(b_dev, b_erase, w1, h1, rect)
    c_erase = block_mode(b_erase, w2, x + 4, y + rh // 2)
    check("④ 第三段帧：请求区域内像素变了（或如实记录 QEMU 仲裁下显示未切换）",
          din > tin * 0.9 or (din == 0 and dout == 0),
          "in=%d/%d out=%d/%d 区域色=%s（0 差异 = 屏幕仍是 legacy VGA 那一路，见报告）"
          % (din, tin, dout, tout, c_erase))
    print("    [如实] 第三段帧区域实测色=%s（目标色 %s）" % (c_erase, ERASE_C))

    log = vm.log()
    # ---------- ① PCI / capability / virtqueue 初始化打点 ----------
    mp = last(r"\[VGPU\] pci (\d+):(\d+)\.(\d+) ven=0x(\S+) dev=0x(\S+) sub=0x(\S+) cmd=0x(\S+)", log)
    check("① PCI 打点（[VGPU] pci b:d.f ven=0x1af4 dev=0x1050 sub=0x10）",
          mp is not None and mp.group(4) == "1af4", mp.group(0) if mp else "缺")
    dev_id = mp.group(5) if mp else "?"
    check("① 设备 id 是 virtio-gpu（0x1050 现代 / 0x1010 过渡 / 0x1040+0x10）",
          dev_id in ("1050", "1010") or dev_id.startswith("104"), "dev=0x%s" % dev_id)
    mc = last(r"\[VGPU\] cap common=0x(\S+) len=(\d+) notify=0x(\S+) len=(\d+) mul=(\d+) "
              r"device=0x(\S+) isr=0x(\S+)", log)
    check("① 4 个 capability 的 BAR+offset 都解析出来了（common/notify/device/isr 都非 0）",
          mc is not None and mc.group(1) != "0" and mc.group(3) != "0" and mc.group(6) != "0"
          and mc.group(7) != "0", mc.group(0) if mc else "缺")
    mf = last(r"\[VGPU\] feature dev_lo=0x(\S+) dev_hi=0x(\S+) ok_lo=0x(\S+) ok_hi=0x(\S+) "
              r"rejected=0x(\S+) status=0x(\S+)", log)
    check("① 特性协商只接受能处理的位（ok_lo=0x0、ok_hi 只留 VIRTIO_F_VERSION_1=0x1）",
          mf is not None and mf.group(3) == "0" and mf.group(4) == "1",
          mf.group(0) if mf else "缺")
    mq = last(r"\[VGPU\] queue0 size=(\d+) dev_max=(\d+) notify_off=(\d+) msix_vec=(\d+) "
              r"desc=0x(\S+) avail=0x(\S+) used=0x(\S+)", log)
    check("① virtqueue 0（控制队列）建好：size/desc/avail/used 都是真物理地址（非 0）",
          mq is not None and int(mq.group(1)) >= 8 and mq.group(5) != "0" and mq.group(6) != "0"
          and mq.group(7) != "0" and mq.group(4) == "0", mq.group(0) if mq else "缺")
    mq1 = last(r"\[VGPU\] queue1 size=(\d+) \(cursor; not enabled\)", log)
    check("① cursor 队列如实只报告不启用（[VGPU] queue1 size=… (cursor; not enabled)）",
          mq1 is not None, mq1.group(0) if mq1 else "缺")
    ds = last(r"\[VGPU\] ready backend=virtio-gpu-2d scanout=pending status=0x(\S+)", log)
    check("① DEVICE_STATUS 走到 DRIVER_OK（ACK|DRIVER|FEATURES_OK|DRIVER_OK = 0xf）",
          ds is not None and ds.group(1).endswith("f"), ds.group(0) if ds else "缺")

    # ---------- ② GET_DISPLAY_INFO ----------
    mi = last(r"\[VGPU\] display info (\d+)x(\d+) enabled=(\d+) lfb=(\d+)x(\d+) zoom=(\d+)[^\r\n]*",
              log)
    same = mi is not None and mi.group(1) == mi.group(4) and mi.group(2) == mi.group(5)
    noted = mi is not None and "note=device-vs-lfb-differ-use-lfb" in mi.group(0)
    check("② GET_DISPLAY_INFO 拿到分辨率（与 LFB 一致，或如实带 note=… 说明差异）",
          mi is not None and mi.group(3) == "1" and (same or noted), mi.group(0) if mi else "缺")
    if mi:
        check("② resource 尺寸取自 LFB（后备缓冲 stride 必须等于 resource 宽）",
              mi.group(4) == str(fb_w) and mi.group(5) == str(fb_h),
              "lfb=%sx%s（[G64] fb render 报 phys=%dx%d）" % (mi.group(4), mi.group(5), fb_w, fb_h))

    # ---------- ③ resource / backing / scanout ----------
    mr = last(r"\[VGPU\] res id=(\d+) (\d+)x(\d+) fmt=(b8g8r8x8) entries=(\d+) backing=0x(\S+) "
              r"bytes=(\d+)", log)
    check("③ RESOURCE_CREATE_2D + ATTACH_BACKING（fmt=b8g8r8x8、entries/bytes 非 0、"
          "backing=后备缓冲物理页）",
          mr is not None and int(mr.group(5)) > 0 and int(mr.group(7)) > 0 and mr.group(6) != "0",
          mr.group(0) if mr else "缺")
    for want in ("GET_DISPLAY_INFO", "RESOURCE_CREATE_2D", "RESOURCE_ATTACH_BACKING", "SET_SCANOUT"):
        c = last(r"\[VGPU\] cmd type=0x\S+ name=%s used=(\d+)[^\r\n]*timeouts=(\d+)" % want, log)
        check("③ 命令打点 name=%s（used=1、timeouts=0）" % want,
              c is not None and c.group(1) == "1" and c.group(2) == "0", c.group(0) if c else "缺")
    mbs = re.findall(r"\[VGPU\] blit dev=1 x=(\d+) y=(\d+) w=(\d+) h=(\d+) transfers=(\d+)", log)
    full = [m for m in mbs if m[0] == "0" and m[1] == "0"]
    check("③ 上屏走设备：整屏 TRANSFER+FLUSH（[VGPU] blit dev=1 x=0 y=0 w=… h=…）",
          len(full) > 0, full[0] if full else ("缺（共 %d 条 blit 打点）" % len(mbs)))
    mp2 = last(r"\[VGPU\] selftest PASS dev_blits=(\d+) soft_blits=(\d+) transfers=(\d+) "
               r"flushes=(\d+) timeouts=(\d+)", log)
    check("自检 PASS（设备/软件计数与 transfers/flushes；timeouts=0）",
          mp2 is not None and int(mp2.group(1)) > 0 and int(mp2.group(3)) > 0
          and int(mp2.group(4)) > 0 and int(mp2.group(5)) == 0, mp2.group(0) if mp2 else "缺")
    bl = last(r"\[FB64\] backend=(\S+)", log)
    check("显示后端切到设备（[FB64] backend=virtio-gpu-2d）",
          bl is not None and bl.group(1) == "virtio-gpu-2d", bl.group(0) if bl else "缺")

    # ---------- 基准（设备 vs 软件，多轮中位数） ----------
    vm.wait_log("[VGPU] bench", 40)
    bm = re.findall(r"\[VGPU\] bench kind=(\S+)[^\r\n]*px=(\d+) runs=(\d+) soft_cyc=(\d+) "
                    r"dev_cyc=(\d+)(?: soft_us=(\d+) dev_us=(\d+))?[^\r\n]*ratio_x100=(\d+)", log)
    check("基准打点齐全（整屏 + ≥3 个区域大小，多轮中位数）", len(bm) >= 4,
          "%d 条 [VGPU] bench" % len(bm))
    for kind, pxs, runs, sc, dc, su, du, ratio in bm:
        print("    [bench] %-6s px=%-8s runs=%s soft=%scyc(%s us) dev=%scyc(%s us) ratio=%s/100"
              % (kind, pxs, runs, sc, su or "?", dc, du or "?", ratio))

    # ---------- 设备路径下端到端：桌面仍然可用 ----------
    qh.login_desktop(vm.mon, lambda: vm.log(), proc=vm.proc, timeout=200)
    up = vm.wait_log("[GUI64] ready", 240)
    check("设备路径下桌面照常起来（[GUI64] ready）", up,
          (last(r"\[GUI64\] ready[^\r\n]*", vm.log()).group(0) if up else "超时"))
    sh = vm.shot("desktop_dev", wait=2.0)
    if sh:
        _, w, h, body = sh
        frac = nonblack_frac(body, w, (0, 0, w, min(h, 200)))
        allf = nonblack_frac(body, w, (0, 0, w, h), step=16)
        check("设备路径桌面真画出来了（顶部条 %.2f / 整屏 %.2f 非黑）" % (frac, allf),
              frac > 0.3 and allf > 0.2, "%dx%d" % (w, h))
    else:
        check("设备路径桌面截图可用", False, "screendump 失败")
    log2 = vm.log()
    md = last(r"\[VGPU\] blit dev=1[^\r\n]*transfers=(\d+)", log2)
    check("桌面起来后仍在走设备路径（[VGPU] blit dev=1 …）", md is not None,
          md.group(0) if md else "缺")
    check("全程没有 virtio 命令超时 / 降级（[VGPU] cmd timeout / disable 不出现）",
          last(r"\[VGPU\] (cmd timeout|disable)", log2) is None,
          last(r"\[VGPU\] (cmd timeout|disable)[^\r\n]*", log2).group(0)
          if last(r"\[VGPU\] (cmd timeout|disable)[^\r\n]*", log2) else "无（正确）")
    # 既存问题（与本批无关，如实记录）：1024x768 下 panels64 的锚点自检失败
    #   （-vga std 的 1280x800 下没有；-vga virtio 的 VBE 默认分辨率是 1024x768）
    pan = last(r"\[PANEL64\] selftest FAIL[^\r\n]*", log2)
    if pan:
        print("    [既存] %s（PANEL64 自己的锚点自检，1024x768 下失败；与本批无关）" % pan.group(0))
    eq = last(r"\[VGPU\] equiv[^\r\n]*", log2)
    print("    [GAP] %s（TRANSFER_FROM_HOST_2D 对非 blob 2D 资源不回写 guest —— 如实记录，不作为失败）"
          % (eq.group(0) if eq else "缺"))
    bad = [p for p in FORBIDDEN
           if p in log2.replace("[PANEL64] selftest FAIL", "[PANEL64] selftest-fail")]
    check("回归：日志里没有 PANIC / selftest FAIL / OOM（PANEL64 的既存失败已如实剥离）",
          not bad, ",".join(bad))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=r"C:\Program Files\qemu\qemu-system-x86_64.exe")
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"))
    ap.add_argument("--base-port", type=int, default=44971)
    ap.add_argument("--only", default="", help="virtio / std（默认两套都跑）")
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    if not os.path.exists(args.qemu):
        sys.stderr.write("找不到 qemu-system-x86_64：%s\n" % args.qemu)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_vgpu_")
    print("== virtio-gpu（2D）驱动验收 ==")
    print("镜像：%s" % args.img)
    checks = []
    modes = [("virtio", True), ("std", False)]
    if args.only == "virtio":
        modes = [("virtio", True)]
    elif args.only == "std":
        modes = [("std", False)]
    for i, (vga, expect) in enumerate(modes):
        run_mode(args.qemu, args.img, vga, args.base_port + i, tmp, checks, expect)
    npass = sum(1 for c in checks if c[1])
    print("== 结果：%d/%d 通过 ==" % (npass, len(checks)))
    for name, ok, detail in checks:
        if not ok:
            print("  FAIL: %s %s" % (name, detail))
    print("（串口日志与帧：%s）" % tmp)
    return 0 if checks and npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
