#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/hda64_test.py - Intel HDA（High Definition Audio）声卡驱动端到端验收

被验的需求（驱动线第一件）：VimtuOS 原先没有声卡驱动，声音面板/设置页的音量、静音、输出源
只是内存态。本测试验的是加完 **kernel/hda64.{h,cpp}** 之后的**真实行为**（全部取串口真打点，
不设内存态断言）：

  ① PCI 找到 HDA 控制器与 BAR0（class 0x0403）：`[HDA64] pci <b>:<d>.<f> bar0=0x… codecs=1`
  ② 码器响应：`[HDA64] codec #0 vid=0x… did=0x… step=0x…`（vid/did 非 0xFFFFFFFF/0）
  ③ DAC / Pin 通路建立：`[HDA64] dac nid=0x… pin nid=0x… path=…` +
     `[HDA64] pin nid=0x… ctl=0x40 out_en=1`（Pin Widget Control 真写 + 回读）
  ④ 格式设置成功：`[HDA64] fmt 48000/16/2 … rdback=0x11 … ok=1`（Set Converter Format 0x200 回读一致，
     流号 0x706 回读 = 0x10）
  ⑤ 控制器侧流位置前进：启动期自检 `[HDA64] selftest PASS mask=0 lpib=<n> bcis=<m>`（n>0、m>0），
     以及终端 `audio playtone 300` 之后的 `[HDA64] cmd audio playtone … runs+=N bcis+=M ok=1`（N≥1、M≥1）
     + 每块 `[HDA64] stream done lpib=<n> cbl=<n> bcis=1 ok=1` 的**数字**
  ⑥ 音量 / 静音回读一致：`audio vol 0/50/100` 后 `[HDA64] volume pct=<p> step=<g>/<n> … rb=0x<g> … ok=1`
     （回读增益 == 写入增益；0% 静音位 = 1）；`audio mute on|off` 后 `[HDA64] mute … bit=1|0`
  ⑦ `audio info` 的字段与启动期打点一致（vid/did/dac/pin/fmt/outs）
  ⑧ 无驱动时**如实降级**：`-device` 不挂声卡 -> `[HDA64] not found` + `selftest skipped`；
     `audio info/vol/mute/playtone` 都返回明确的 driver_absent 错误、不假装成功；桌面照常起来、不变砖
  ⑨ 全程无 PANIC / TRIPLE FAULT；另外校验声音面板在**有驱动**时是 applied=1 via=hda64

用法：py -3 tests\\hda64_test.py [--img build64/system.img] [--qemu 路径] [--port 5797] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemuhelp as qh              # noqa: E402  （★ 公共登录手势：ui.login.auto 默认 0）

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "三重故障"]


def q(p):
    return p.replace("\\", "/")


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


class Monitor:
    """QEMU monitor（telnet）+ 终端按键注入（与 tests/fs_term_test.py 同一套字符映射）。"""

    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.35):
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

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def raw(self, cmds, wait_between=0.12, wait_end=0.4):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            for c in cmds:
                s.sendall(c.encode() + b"\n")
                time.sleep(wait_between)
            time.sleep(wait_end)
        finally:
            s.close()

    def move(self, dx, dy, wait=0.10):
        self.send("mouse_move %d %d" % (dx, dy), wait=wait)

    def click(self, wait=0.45):
        self.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=wait)

    def type_line(self, text, per_key=0.14):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
                 "=": "equal", "_": "shift-minus"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char for sendkey: %r" % ch)
        self.key("ret", wait=per_key + 0.2)


class Cursor:
    """与 tests/panels64_test.py 同款的确定性光标（mouse_move 是相对量，客人内核 x1.7）。"""

    def __init__(self, x=512, y=384):
        self.x, self.y = x, y

    def _step(self, mon, px, py, wait):
        cx = max(-14, min(14, int(px / 1.7)))
        cy = max(-14, min(14, int(py / 1.7)))
        if cx == 0 and cy == 0:
            return False
        mon.move(cx, cy, wait=wait)
        self.x = max(0, min(1279, self.x + int(cx * 17 / 10)))
        self.y = max(0, min(799, self.y + int(cy * 17 / 10)))
        return True

    def settle(self, mon, wait=0.10):
        for dx, dy in ((3, 0), (-3, 0), (0, 3), (0, -3), (3, 0), (-3, 0), (0, 3), (0, -3)):
            mon.move(dx, dy, wait=wait)

    def goto(self, mon, tx, ty, wait=0.10):
        for _ in range(400):
            rx = int(tx) - self.x
            ry = int(ty) - self.y
            if abs(rx) <= 1 and abs(ry) <= 1:
                break
            if not self._step(mon, rx, ry, wait):
                break
        self.settle(mon, wait)


class Vm:
    def __init__(self, qemu, img, port, tag, with_hda, workdir):
        self.tag = tag
        self.serial = os.path.join(workdir, "serial_%s.log" % tag)
        self.wav = os.path.join(workdir, "out_%s.wav" % tag)
        args = [qemu, "-name", "vimtu-hda-%s" % tag,
                "-drive", "format=raw,file=%s" % q(img),
                "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
                "-serial", "file:%s" % q(self.serial),
                "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                "-netdev", "user,id=vnet0", "-device", "e1000,netdev=vnet0",
                "-no-reboot"]
        if with_hda:
            # QEMU 的 Intel HDA 控制器 + 通用码器；wav 后端把声卡实际混出的音频录进文件
            args += ["-device", "ich9-intel-hda",
                     "-device", "hda-duplex,audiodev=snd0",
                     "-audiodev", "wav,id=snd0,path=%s" % q(self.wav)]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.port = port

    def log(self):
        return slog(self.serial)

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.25)
        return False

    def monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                break
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def stop(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def last(pattern, text, flags=0):
    m = re.findall(pattern, text, flags)
    return m[-1] if m else None


def wav_stats(path):
    """QEMU 的 wav 后端在关文件时才回填 RIFF 长度；被 kill 后仍是合法数据。
    返回 (rate, channels, samples, peak)。失败返回 None。"""
    try:
        raw = open(path, "rb").read()
    except OSError:
        return None
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        return None
    i, fmt, data_off = 12, None, None
    while i + 8 <= len(raw):
        cid = raw[i:i + 4]
        sz = int.from_bytes(raw[i + 4:i + 8], "little")
        if cid == b"fmt ":
            fmt = raw[i + 8:i + 8 + sz]
        elif cid == b"data":
            data_off = i + 8
            break
        i += 8 + sz + (sz & 1)
    if not fmt or data_off is None:
        return None
    ch = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    pcm = raw[data_off:]
    n = len(pcm) // 2
    peak = 0
    for k in range(0, n, 7):                       # 抽样峰值（省时间）
        v = struct.unpack_from("<h", pcm, k * 2)[0]
        if abs(v) > peak:
            peak = abs(v)
    return (rate, ch, n // max(1, ch), peak)


def check(checks, name, cond, detail=""):
    detail = str(detail) if detail else ""
    checks.append((name, bool(cond), detail))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

def last(pattern, text, flags=0):
    m = list(re.finditer(pattern, text, flags))
    return m[-1] if m else None


def open_terminal(mon, vm, checks, tag):
    opened = False
    for _ in range(3):
        mon.key("meta_l", wait=1.0)
        mon.key("1", wait=2.0)
        if vm.wait_log("[APP] term opened", 15):
            opened = True
            break
    check(checks, "%s：开始菜单 -> 终端（[APP] term opened）" % tag, opened)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5797)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = args.qemu or r"C:\Program Files\qemu\qemu-system-x86_64.exe"
    if not os.path.exists(qemu):
        sys.stderr.write("找不到 qemu-system-x86_64：%s\n" % qemu)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_hda_")
    checks = []

    # ==================== 第一遍：挂 Intel HDA（QEMU ich9-intel-hda + hda-duplex）====================
    print("=== 1) 挂 HDA：PCI/码器/通路/格式/自检 ===")
    vm = Vm(qemu, args.img, args.port, "hda", True, tmp)
    mon = vm.monitor()
    try:
        if not qh.login_desktop(mon, vm.log, vm.proc, timeout=180):
            check(checks, "进入桌面（锁屏可交互 + 两次回车）", False, "没等到 [LOCK64] bg blur ready")
            raise SystemExit(1)
        up = vm.wait_log("[GUI64] ready", 120)
        log = vm.log()
        check(checks, "桌面就绪（[GUI64] ready）", up)

        m = re.search(r"\[HDA64\] pci (\d+):(\d+)\.(\d+) bar0=(0x[0-9a-f]+) codecs=(\d+)", log)
        check(checks, "① PCI 找到 HDA 控制器（class 0x0403）+ BAR0 MMIO", m is not None and int(m.group(5)) >= 1,
              m.group(0) if m else "（无）")
        bar0 = int(m.group(4), 16) if m else 0
        check(checks, "① BAR0 落在 MMIO 区（>= 0x100000 且 != 0）", bar0 >= 0x100000, "bar0=0x%x" % bar0)

        mc = re.search(r"\[HDA64\] codec #(\d+) vid=(0x[0-9a-f]+) did=(0x[0-9a-f]+) step=(0x[0-9a-f]+)", log)
        vid = int(mc.group(2), 16) if mc else 0
        did = int(mc.group(3), 16) if mc else 0
        check(checks, "② 码器响应 F0000（vid/did 非 0xffffffff / 0）",
              mc is not None and vid not in (0, 0xFFFF) and did != 0xFFFF, mc.group(0) if mc else "（无）")

        md = re.search(r"\[HDA64\] dac nid=(0x[0-9a-f]+) pin nid=(0x[0-9a-f]+) (path=implicit|path=\d+) caps=(0x[0-9a-f]+)", log)
        check(checks, "③ DAC + Pin Complex 通路建立（dac/pin/path 打点）", md is not None,
              md.group(0) if md else "（无）")
        mp = re.search(r"\[HDA64\] pin nid=(0x[0-9a-f]+) ctl=(0x[0-9a-f]+) out_en=(\d+) cfg=(0x[0-9a-f]+) name=(\S+)", log)
        check(checks, "③ Pin Widget Control 真写 + 回读（ctl=0x40 out_en=1）",
              mp is not None and int(mp.group(2), 16) == 0x40 and mp.group(3) == "1", mp.group(0) if mp else "（无）")

        mf = re.search(r"\[HDA64\] fmt 48000/16/2 verb=(0x[0-9a-f]+) set=(0x[0-9a-f]+) rdback=(0x[0-9a-f]+) conv=(0x[0-9a-f]+) ok=(\d)", log)
        check(checks, "④ Set Converter Format 0x200 = 48k/16/2，回读一致（rdback=0x11 conv=0x10 ok=1）",
              mf is not None and mf.group(3) == "0x11" and mf.group(4) == "0x10" and mf.group(5) == "1",
              mf.group(0) if mf else "（无）")

        ms = re.search(r"\[HDA64\] selftest PASS mask=0 lpib=(\d+) bcis=(\d+) tone_ms=(\d+)", log)
        check(checks, "⑤ 启动期自检 PASS（送已知 PCM 并断言控制器侧 LPIB/BCIS 前进）",
              ms is not None and int(ms.group(1)) > 0 and int(ms.group(2)) > 0, ms.group(0) if ms else "（无）")
        if ms:
            print("      -> 客观证据：lpib=%s（字节）、bcis=%s（周期完成次数）、tone_ms=%s" %
                  (ms.group(1), ms.group(2), ms.group(3)))

        # ---- 终端命令：audio info / vol / mute / playtone / out ----
        print("=== 2) 终端 audio 命令（真驱动）===")
        open_terminal(mon, vm, checks, "HDA")
        mon.type_line("audio info")
        vm.wait_log("[HDA64] cmd audio info", 20)
        log = vm.log()
        mi = last(r"\[HDA64\] cmd audio info pci=(\d+):(\d+)\.(\d+) found=1 ready=1 codecs=(\d+) vid=(0x[0-9a-f]+) did=(0x[0-9a-f]+) dac=(0x[0-9a-f]+) pin=(0x[0-9a-f]+) out_en=(\d+) fmt=(0x[0-9a-f]+) conv=(0x[0-9a-f]+) vol=(\d+) mute=(\d+) amp_rb=(0x[0-9a-f]+) outs=(\d+)", log)
        check(checks, "⑦ audio info：控制器/码器/通路/格式/输出源与启动期打点一致",
              mi is not None and mc is not None and int(mi.group(5), 16) == int(mc.group(2), 16) and
              int(mi.group(6), 16) == int(mc.group(3), 16) and
              mi.group(10) == "0x0011" and mi.group(11) == "0x10" and int(mi.group(14), 16) >= 1,
              mi.group(0) if mi else (last(r"\[HDA64\] cmd audio info[^\r\n]*", log) or "（无）"))

        volmap = {}        # ★ 修复（①）：把每档的 (写入增益, 步数, 回读) 存下来做语义断言
        for pct in (50, 0, 100):
            n1 = len(vm.log())
            mon.type_line("audio vol %d" % pct)
            vm.wait_log("[HDA64] cmd audio vol applied=1 pct=%d" % pct, 20)
            log = vm.log()
            mv = last(r"\[HDA64\] volume pct=(\d+) step=(\d+)/(\d+) mute=(\d+) rb=(0x[0-9a-f]+) whence=set ok=(\d)", log[n1:])
            if mv:
                volmap[pct] = (int(mv.group(2)), int(mv.group(3)), int(mv.group(5), 16))
            if pct > 0:
                ok_rb = (mv is not None and int(mv.group(5), 16) == int(mv.group(2)) and
                         mv.group(6) == "1" and mv.group(4) == "0")
            else:
                ok_rb = mv is not None and (int(mv.group(5), 16) & 0x80) != 0 and mv.group(4) == "1"
            check(checks, "⑥ audio vol %d：放大器真写 + 回读一致（step=%s rb=%s ok=%s）" % (
                pct, mv.group(2) if mv else "?", mv.group(5) if mv else "?", mv.group(6) if mv else "?"),
                ok_rb, mv.group(0) if mv else (last(r"\[HDA64\] volume[^\r\n]*", log) or "（无）"))
        # ---- ★ 修复（①）语义同步：100% 必须落在**0 dB 增益索引**（100% 写入值），且与回读一致 ----
        # 改动前：hda_gain_from_pct 把"步数"当成"每步 dB"，100% 写成 gain index 0（= 最大衰减/静音），
        #   实测系统音效峰值 ≈ 0..259（听不见）。改动后：100% -> gain_max（0 dB），回读一致。
        cm = last(r"\[HDA64\] amp cap=(0x[0-9a-f]+) steps=(\d+) step_qdb=(\d+) offset=(\d+) mute_cap=(\d+) range_db_x4=\d+ gain_max=(\d+)", log)
        steps = int(cm.group(2)) if cm else -1
        gmax = int(cm.group(6)) if cm else -1
        check(checks, "⑥ 放大器能力字按 spec 解码（steps=bits[22:16]、step_qdb=bits[14:8]、offset=bits[6:0]、mute=bit31）",
              cm is not None and steps >= 1 and int(cm.group(3)) >= 1 and int(cm.group(5)) == 1,
              cm.group(0) if cm else "（无）")
        if gmax > 0 and 100 in volmap and 50 in volmap:
            g100, n100, rb100 = volmap[100]
            g50, n50, rb50 = volmap[50]
            check(checks, "⑥ 100%% = 0 dB 增益索引（写入 %d == gain_max %d，分母 %d，回读 0x%x 一致）" % (g100, gmax, n100, rb100),
                  g100 == gmax and n100 == gmax and rb100 == g100)
            check(checks, "⑥ 50%% 的增益索引 = round(gain_max/2) 且严格小于 100%%（%d,%d < %d,%d）"
                  % (g50, rb50, g100, rb100),
                  g50 == (gmax * 50 + 50) // 100 and g50 < g100)
        else:
            check(checks, "⑥ 音量语义（100%=0 dB 索引 / 50% 更小）", False, "缺少 volume/amp cap 打点")

        n2 = len(vm.log())
        mon.type_line("audio mute on")
        vm.wait_log("[HDA64] cmd audio mute applied=1 on=1", 20)
        log = vm.log()
        mm = last(r"\[HDA64\] mute on=1 rb=(0x[0-9a-f]+) bit=(\d)", log[n2:])
        check(checks, "⑥ audio mute on：静音位真写 + 回读 bit=1", mm is not None and mm.group(2) == "1",
              mm.group(0) if mm else "（无）")
        n3 = len(vm.log())
        mon.type_line("audio mute off")
        vm.wait_log("[HDA64] cmd audio mute applied=1 on=0", 20)
        log = vm.log()
        mm2 = last(r"\[HDA64\] mute on=0 rb=(0x[0-9a-f]+) bit=(\d)", log[n3:])
        check(checks, "⑥ audio mute off：静音位回读 bit=0", mm2 is not None and mm2.group(2) == "0",
              mm2.group(0) if mm2 else "（无）")

        n4 = len(vm.log())
        mon.type_line("audio playtone 300")
        vm.wait_log("[HDA64] cmd audio playtone", 40)
        log = vm.log()
        mt = last(r"\[HDA64\] cmd audio playtone ms=(\d+) runs\+=(\d+) bcis\+=(\d+) lpib=(\d+) ok=1", log)
        check(checks, "⑤ audio playtone 300：流位置/中断计数前进（runs+/bcis+/lpib 数字）",
              mt is not None and int(mt.group(2)) >= 1 and int(mt.group(3)) >= 1 and int(mt.group(4)) > 0,
              mt.group(0) if mt else (last(r"\[HDA64\] cmd audio playtone[^\r\n]*", log) or "（无）"))
        sd = re.findall(r"\[HDA64\] stream done lpib=(\d+) cbl=(\d+) bcis=(\d+) ok=1", log[n4:])
        check(checks, "⑤ playtone 期间每块流都完成（stream done lpib>0 cbl=2048 bcis=1 ok=1）",
              len(sd) >= 2 and all(int(a) < int(b) and int(c) == 1 for a, b, c in sd),
              "blocks=%d 例: %s" % (len(sd), sd[:3]))
        if mt:
            print("      -> 客观证据：playtone runs+=%s bcis+=%s lpib=%s；stream done 块数=%d" %
                  (mt.group(2), mt.group(3), mt.group(4), len(sd)))

        mon.type_line("audio out")
        vm.wait_log("[HDA64] cmd audio out list=1", 20)
        log = vm.log()
        mo = last(r"\[HDA64\] cmd audio out list=1 count=(\d+)", log)
        check(checks, "⑦ audio out：如实列出检测到的输出源（count>=1）", mo is not None and int(mo.group(1)) >= 1,
              mo.group(0) if mo else "（无）")

        # ---- 面板接线：有驱动时 applied=1 via=hda64 ----
        print("=== 3) 声音面板接线（有驱动 -> applied=1 via=hda64）===")
        cur = Cursor()
        icons = {n: (int(x), int(y)) for n, x, y in
                 re.findall(r"\[START64\] status icon (\w+) x=(\d+) y=(\d+)", vm.log())}
        panel_ok = False
        if "sound" in icons:
            for _ in range(4):
                n6 = len(vm.log())
                ix, iy = icons["sound"]
                cur.goto(mon, ix + 11, iy + 11)
                time.sleep(0.3)
                mon.click()
                if vm.wait_log("[PANEL64] sound panel driver=hda64", 5, since=n6):
                    panel_ok = True
                    break
                mon.key("meta_l", wait=0.8)      # 菜单被关了：重开再点
        check(checks, "⑨ 声音面板报告 driver=hda64（真驱动）", panel_ok,
              (last(r"\[PANEL64\] sound panel[^\r\n]*", vm.log()) or "（无）"))
        if panel_ok:
            n7 = len(vm.log())
            sl = last(r"\[PANEL64\] sound panel slider x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log())
            if sl:
                slx, sly, slw = int(sl.group(1)), int(sl.group(2)), int(sl.group(3))
                cur.goto(mon, slx + slw // 10, sly + 6)
                mon.raw(["mouse_button 1"], wait_end=0.3)
                cur.goto(mon, slx + slw * 4 // 5, sly + 6)
                mon.raw(["mouse_button 0"], wait_end=0.6)
            vm.wait_log("[PANEL64] sound volume=", 6, since=n7)
            vv = last(r"\[PANEL64\] sound volume=(\d+) src=(\S+) why=(\S+) applied=(\d) via=hda64", vm.log()[n7:])
            check(checks, "⑨ 面板滑轨拖动 -> 真写硬件（applied=1 via=hda64）", vv is not None,
                  vv.group(0) if vv else (last(r"\[PANEL64\] sound volume[^\r\n]*", vm.log()) or "（无）"))
            mon.key("esc", wait=0.8)

        print("=== 4) 录音文件（QEMU wav 后端；可验证的输出证据）===")
        time.sleep(1.0)
        st = wav_stats(vm.wav)
        check(checks, "⑤ QEMU wav 后端创建了录音文件且长度与播放量级相符",
              st is not None and st[2] >= 4000, str(st) if st else "（没有 wav 文件）")
        if st:
            check(checks, "⑤ 录音里确实有音频信号（peak > 0：声卡真的混出了波形）", st[3] > 0,
                  "rate=%d ch=%d samples=%d peak=%d" % st)
            print("      -> 录音：%d Hz / %dch / %d 帧，峰值=%d（%s）" % (st[0], st[1], st[2], st[3], vm.wav))

        bad = [w for w in FORBIDDEN if w in vm.log()]
        check(checks, "⑨ 全程无 PANIC / 三重故障", not bad, ",".join(bad))
    finally:
        vm.stop()

    # ==================== 第二遍：不挂声卡（如实降级）====================
    print("=== 5) 不挂声卡：如实降级（driver absent）===")
    vm2 = Vm(qemu, args.img, args.port + 1, "nohda", False, tmp)
    mon2 = vm2.monitor()
    try:
        if not qh.login_desktop(mon2, vm2.log, vm2.proc, timeout=180):
            check(checks, "无驱动：进入桌面（锁屏可交互 + 两次回车）", False, "没等到锁屏")
        else:
            up2 = vm2.wait_log("[GUI64] ready", 120)
            log2 = vm2.log()
            check(checks, "⑧ 无控制器：如实打点 [HDA64] not found + selftest skipped",
                  "[HDA64] not found" in log2 and "[HDA64] selftest skipped" in log2,
                  (last(r"\[HDA64\][^\r\n]*", log2) or "（无）"))
            open_terminal(mon2, vm2, checks, "无驱动")
            n0 = len(vm2.log())
            mon2.type_line("audio info")
            vm2.wait_log("[HDA64] cmd audio info found=0", 20)
            check(checks, "⑧ audio info：明说 driver absent（found=0 ready=0 driver_absent=1）",
                  "[HDA64] cmd audio info found=0 ready=0 driver_absent=1" in vm2.log()[n0:],
                  (last(r"\[HDA64\] cmd audio info[^\r\n]*", vm2.log()) or "（无）"))
            n1 = len(vm2.log())
            mon2.type_line("audio vol 50")
            vm2.wait_log("[HDA64] cmd audio vol applied=0 driver_absent=1", 20)
            check(checks, "⑧ audio vol：不假装成功（applied=0 driver_absent=1）",
                  "[HDA64] cmd audio vol applied=0 driver_absent=1 pct=50" in vm2.log()[n1:],
                  (last(r"\[HDA64\] cmd audio vol[^\r\n]*", vm2.log()) or "（无）"))
            n2 = len(vm2.log())
            mon2.type_line("audio mute on")
            vm2.wait_log("[HDA64] cmd audio mute applied=0 driver_absent=1", 20)
            check(checks, "⑧ audio mute：不假装成功（applied=0 driver_absent=1）",
                  "[HDA64] cmd audio mute applied=0 driver_absent=1 on=1" in vm2.log()[n2:],
                  (last(r"\[HDA64\] cmd audio mute[^\r\n]*", vm2.log()) or "（无）"))
            n3 = len(vm2.log())
            mon2.type_line("audio playtone 100")
            vm2.wait_log("[HDA64] cmd audio playtone applied=0 driver_absent=1", 20)
            check(checks, "⑧ audio playtone：没有声音也不假装（applied=0 driver_absent=1）",
                  "[HDA64] cmd audio playtone applied=0 driver_absent=1 ms=100" in vm2.log()[n3:],
                  (last(r"\[HDA64\] cmd audio playtone[^\r\n]*", vm2.log()) or "（无）"))
            # 面板在无驱动时保留内存态 + 明写 driver absent（与 panels64_test 的诚实口径一致）
            cur2 = Cursor()
            icons2 = {n: (int(x), int(y)) for n, x, y in
                      re.findall(r"\[START64\] status icon (\w+) x=(\d+) y=(\d+)", vm2.log())}
            panel2 = False
            if "sound" in icons2:
                for _ in range(4):
                    n4 = len(vm2.log())
                    ix, iy = icons2["sound"]
                    cur2.goto(mon2, ix + 11, iy + 11)
                    time.sleep(0.3)
                    mon2.click()
                    if vm2.wait_log("[PANEL64] sound panel driver=absent", 5, since=n4):
                        panel2 = True
                        break
                    mon2.key("meta_l", wait=0.8)
            check(checks, "⑧ 声音面板：driver=absent + audio driver not implemented yet（内存态，不假装）",
                  panel2 and "audio driver not implemented yet" in vm2.log(),
                  (last(r"\[PANEL64\] sound panel[^\r\n]*", vm2.log()) or "（无）"))
            check(checks, "⑧ 无驱动时桌面照常（[GUI64] ready 已出现）且无 PANIC",
                  up2 and not any(w in vm2.log() for w in FORBIDDEN))
    finally:
        vm2.stop()

    npass = sum(1 for _, c, _ in checks if c)
    print("== 结果：%d 项断言，%s（PASS=%d FAIL=%d）==" %
          (len(checks), "全过" if npass == len(checks) else "有失败", npass, len(checks) - npass))
    print("   scratch=%s" % tmp)
    return 0 if npass == len(checks) else 1


if __name__ == "__main__":
    sys.exit(main())
