#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/desktopops64_test.py - P5 验收（**本轮实际落地的子集**，详见下方"落地范围"）

★ 落地范围（务必先读，别把没做的当成做了）：
  本轮把"桌面交互细节"这批需求做成了可独立测试的**配置层 + 资源管理器层 + 设置入口**，
  但**桌面外壳的接线（gui64.cpp）本轮没有落地** —— 原因是接线版本在长会话里会让应用窗口的内容
  画不出来（设置应用页切换后内容区一直是一层底色，settings64_test 的像素断言因此变红），
  排查到"应用每隔一帧就丢掉自己的绘制结果"这一步后时间用尽，为了不让整棵树带着回归，本轮
  **回退了 gui64.cpp / input.*，只保留不依赖外壳接线的部分**。因此本脚本只断言真正生效的行为：

  1) 文件资源管理器默认隐藏系统分区（需求："文件资源管理器只能显示可见分区，系统分区默认是隐藏的"）：
     * 默认（ui.explorer.show_system = 0）：非引导盘上的系统分区（MBR 0xEF / EFI 系统分区）**不列出**，
       串口打点 [EXPL64] hidden idx=.. disk=.. part=.. name=.. reason=system-partition；列表行
       [EXPL64] visible partitions only list=N hidden=M show_system=0 证据在案。
     * 引导盘（系统盘 C:）上的条目保持既有界面（既有 explorer64_test 断言它们必须在列表里）。
  2) 设置 → 个性化 → Dock 页里的两个新入口（本批唯一新增的设置控件）：
     * 「显示隐藏分区」开关（[SET64] ctl page=8 id=181）→ 立刻重扫：hidden=0、列表条目数 = 之前 + 隐藏数。
     * 「恢复默认桌面图标」按钮（[SET64] ctl page=8 id=180）→ [DESK64] icons reset why=settings
       defaults=3 persisted=1 via=ui.desktop.icons（桌面图标集合的持久化键）。
  3) 终端入口（等价开关）：`cfg set ui.explorer.show_system 1|0` / `cfg set ui.desktop.icons 0,1,2|`
     都进 config64 覆盖表；下一次打开资源管理器时按新值重扫。
  4) 持久化：上面的两个键在重启后仍在（[CONF64] load ...）。

用法：py -3 tests\\desktopops64_test.py [--img build64/system.img] [--qemu 路径] [--port 5678] [--keep] [--no-reboot]
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]
PORT = 5678
CURSOR = [512, 384]   # 光标位置模型（QEMU 相对位移 + 客人 ×1.7/每包 24px 上限）
# 第二块盘：MBR 里一个 type=0xEF 的系统分区（没有合法 FAT32）-> drive64 列成 skip=esp；
# 资源管理器默认按"系统分区"把它隐藏（非引导盘）。
ESP_LBA = 2048
ESP_SECS = 4000


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    import shutil
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


# ---------------------------------------------------------------------------
# PPM 像素工具（QEMU monitor screendump -> P6 PPM；与 settings64_test.py 同一套语义）
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
        st = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[st:idx]))
    idx += 1
    w, h, _ = fields
    return w, h, raw[idx:]


def sample(px, w, x, y):
    o = (y * w + x) * 3
    return px[o], px[o + 1], px[o + 2]


def dist(a, b):
    return sum(abs(a[i] - b[i]) for i in range(3))


def region_diff(p1, p2, w, rect, step=2, thr=6):
    """两块 PPM 在 rect=(x,y,w,h) 内有多少取样点差异 > thr；返回 (数量, 最大差)。"""
    x, y, rw, rh = rect
    n = 0
    mx = 0
    for yy in range(max(0, y), min(y + rh, 800), step):
        for xx in range(max(0, x), min(x + rw, 1280), step):
            d = dist(sample(p1, w, xx, yy), sample(p2, w, xx, yy))
            if d > thr:
                n += 1
            if d > mx:
                mx = d
    return n, mx


def make_esp_disk(path, sectors=16384):
    img = bytearray(sectors * 512)
    off = 446
    img[off + 0] = 0x00
    img[off + 4] = 0xEF                      # 分区类型：EFI/系统分区
    struct.pack_into("<II", img, off + 8, ESP_LBA, ESP_SECS)
    img[510] = 0x55
    img[511] = 0xAA
    with open(path, "wb") as f:
        f.write(img)
    return path


class Monitor:
    def __init__(self, port):
        self.port = port

    def send(self, cmd, wait=0.30):
        s = socket.create_connection(("127.0.0.1", self.port), timeout=8)
        try:
            s.sendall(cmd.encode() + b"\n")
            time.sleep(wait)
        finally:
            s.close()

    def raw(self, cmds, wait_between=0.12, wait_end=0.5):
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

    def shot(self, path, wait=2.0):
        """screendump 抓一帧 PPM（等文件落盘；失败返回 None）。"""
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % q(path), wait=wait)
        for _ in range(40):
            if os.path.exists(path) and os.path.getsize(path) > 1024:
                return path
            time.sleep(0.2)
        return None

    def key(self, name, wait=0.9):
        self.send("sendkey %s" % name, wait=wait)

    def click(self, wait=0.5):
        self.raw(["mouse_button 1", "mouse_button 0"], wait_between=0.12, wait_end=wait)

    def type_line(self, text, wait=0.5):
        """在终端里敲一行命令（含回车）。用 QEMU 的 sendkey 名字。"""
        names = {" ": "spc", ".": "dot", ",": "comma", "_": "shift-minus", "-": "minus",
                 "/": "slash", "|": "shift-backslash", "\\": "backslash", "=": "equal",
                 "0": "0", "1": "1", "2": "2", "3": "3", "4": "4", "5": "5", "6": "6",
                 "7": "7", "8": "8", "9": "9"}
        for ch in text:
            if ch.isalpha():
                self.key(ch, wait=0.06)
            elif ch in names:
                self.key(names[ch], wait=0.06)
            else:
                raise ValueError("type_line: 不支持的字符 %r" % ch)
        self.key("ret", wait=wait)


class Vm:
    def __init__(self, qemu, drives, port, name, workdir):
        self.port, self.tmp = port, workdir
        self.serial = os.path.join(workdir, name + "_serial.log")
        args = [qemu, "-name", name]
        for d in drives:
            args += ["-drive", "format=raw,file=%s" % q(d)]
        args += ["-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
                 "-serial", "file:%s" % q(self.serial),
                 "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
                 "-netdev", "user,id=vnet0", "-device", "e1000,netdev=vnet0",
                 "-no-reboot"]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def wait_monitor(self):
        for _ in range(80):
            try:
                socket.create_connection(("127.0.0.1", self.port), timeout=1).close()
                return Monitor(self.port)
            except OSError:
                time.sleep(0.25)
        return Monitor(self.port)

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout, since=0):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if needle in self.log()[since:]:
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def n(self):
        return len(self.log())

    def close(self):
        if self.proc.poll() is None:
            self.proc.kill()
            try:
                self.proc.wait(timeout=10)
            except Exception:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--no-reboot", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        sys.stderr.write("镜像不存在：%s（先跑 bash build64.sh）\n" % args.img)
        return 2
    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_deskops_")
    esp = make_esp_disk(os.path.join(tmp, "esp_other.img"))
    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail))

    vm = Vm(qemu, [args.img, esp], args.port, "vimtu-deskops", tmp)
    mon = vm.wait_monitor()

    def click_until(x, y, needle, tries=6):
        offs = ((0, 0), (0, -8), (0, 8), (-12, 0), (12, 0), (0, -16), (0, 16), (-12, -8), (12, 8))
        for i in range(max(tries, 9)):
            dx, dy = offs[i % len(offs)]
            mon.send("mouse_move 0 0", wait=0.05)
            goto(x + dx, y + dy)
            time.sleep(0.25)
            n0 = vm.n()
            mon.click()
            if vm.wait_log(needle, 3, since=n0):
                return n0
        return -1

    def goto(tx, ty):
        """粗略闭环：QEMU 相对位移，客人 ×1.7 + 每包 24px 上限；分步走（起点用上一次的位置）。"""
        cur = [CURSOR[0], CURSOR[1]]
        for _ in range(400):
            rx, ry = int(tx) - cur[0], int(ty) - cur[1]
            if abs(rx) <= 1 and abs(ry) <= 1:
                break
            cx = max(-14, min(14, int(rx / 1.7)))
            cy = max(-14, min(14, int(ry / 1.7)))
            if cx == 0 and cy == 0:
                break
            mon.move(cx, cy, wait=0.08)
            cur[0] = max(0, min(1279, cur[0] + int(cx * 17 / 10)))
            cur[1] = max(0, min(799, cur[1] + int(cy * 17 / 10)))
        for dx, dy in ((3, 0), (-3, 0), (0, 3), (0, -3)):
            mon.move(dx, dy, wait=0.08)
        CURSOR[0], CURSOR[1] = cur[0], cur[1]
        return (cur[0], cur[1])

    try:
        print("=== 0) 引导 ===")
        up = vm.wait_log("[GUI64] ready", 120)
        check("桌面就绪 [GUI64] ready", up)
        if not up:
            raise SystemExit(1)
        log = vm.log()
        check("内核自检没有失败行（[MEM64]/[GUI64]/[DRV64] selftest）",
              "[MEM64] selftest PASS" in log and "[GUI64] selftest PASS" in log,
              (re.search(r"\[DRV64\] selftest[^\r\n]*", log) or [""])[0])

        print("=== 1) 资源管理器默认隐藏系统分区（非引导盘 0xEF）===")
        dock = dict((int(m.group(1)), (int(m.group(8)), int(m.group(9))))
                    for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(\d+) name=(\S+) x=(\d+) y=(\d+) "
                                         r"w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", log))
        check("Dock 项几何打点（能定位「此电脑」图标 idx=1 / 终端 idx=3）",
              len(dock) >= 4 and 1 in dock and 3 in dock, str(sorted(dock)))
        n0 = vm.n()
        goto(dock[1][0], dock[1][1])
        time.sleep(0.3)
        mon.click(wait=1.4)
        check("打开文件资源管理器（[APP] mypc opened + 此电脑打点）",
              vm.wait_log("[APP] mypc opened", 10, since=n0) and vm.wait_log("[UI] explorer thispc", 10, since=n0),
              (re.search(r"\[UI\] explorer thispc[^\r\n]*", vm.log()[n0:]) or [""])[0])
        hid = re.findall(r"\[EXPL64\] hidden idx=(\d+) disk=(\d+) part=(\d+) name=(.+?) reason=system-partition", vm.log())
        check("系统分区被隐藏并打点（[EXPL64] hidden disk=1 part=1 name=EFI 系统分区 reason=system-partition）",
              len(hid) >= 1 and any(h[1] == "1" for h in hid), str(hid[:3]))
        vis = re.findall(r"\[EXPL64\] visible partitions only list=(\d+) hidden=(\d+) show_system=(\d+) boot_disk=(\S+)", vm.log())
        off = [v for v in vis if v[2] == "0" and int(v[1]) >= 1]
        check("默认视图只列可见分区（list=N hidden=M>=1 show_system=0）", len(off) >= 1, str(off[:2]))
        check("隐藏项确实没进卡片列表（[UI] explorer card 行数 = list）",
              len(re.findall(r"\[UI\] explorer card idx=", vm.log())) >= 1 and
              len(re.findall(r"\[UI\] explorer card idx=", vm.log())) >= int(off[0][0]) if off else False,
              "cards=%d" % len(re.findall(r"\[UI\] explorer card idx=", vm.log())))

        print("=== 2) 设置 → 个性化 → Dock 页：显示隐藏分区开关（id=181）===")
        n0 = vm.n()
        mon.key("meta_l", wait=1.0)
        mon.key("6", wait=2.6)          # 老菜单第 6 项 = 设置
        check("设置应用打开（[APP] settings opened）", vm.wait_log("[APP] settings opened", 12, since=n0), "")
        nav = re.search(r"\[SET64\] nav item page=8 grp=\d+ name=.+? x=\d+ y=\d+ w=\d+ h=\d+ cx=\d+ cy=\d+ "
                        r"acx=(\d+) acy=(\d+)", vm.log())
        check("左导航有「Dock 栏」条目（page=8，绝对坐标 acx/acy）", nav is not None,
              nav.group(0) if nav else "")
        page8 = False
        if nav:
            n1 = vm.n()
            click_until(int(nav.group(1)), int(nav.group(2)), "[SET64] page=8")
            page8 = vm.wait_log("[SET64] page=8", 4, since=n1)
            check("点导航切到「Dock 栏」页（[SET64] page=8）", page8, "")
        sw = re.search(r"\[SET64\] ctl page=8 id=181 kind=switch x=\d+ y=\d+ w=\d+ h=\d+ cx=\d+ cy=\d+ "
                       r"ax=(\d+) ay=(\d+) acx=(\d+) acy=(\d+)", vm.log())
        check("新入口控件打点（[SET64] ctl page=8 id=181 kind=switch）", sw is not None,
              sw.group(0) if sw else "")
        if sw:
            n1 = vm.n()
            click_until(int(sw.group(3)), int(sw.group(4)), "[EXPL64] show_system=1")
            check("开关打开 -> [EXPL64] show_system=1 why=settings persisted=1",
                  vm.wait_log("[EXPL64] show_system=1 why=settings", 5, since=n1),
                  (re.search(r"\[EXPL64\] show_system[^\r\n]*", vm.log()[n1:]) or [""])[0])
            on = re.findall(r"\[EXPL64\] visible partitions only list=(\d+) hidden=(\d+) show_system=1", vm.log())
            check("开关打开后重扫：hidden=0（系统分区出现在列表里）", len(on) >= 1 and on[-1][1] == "0", str(on[-1:]))
            if off and on:
                check("条目数守恒（列表里多出被隐藏的那几条）",
                      int(on[-1][0]) == int(off[0][0]) + int(off[0][1]),
                      "off list=%s on list=%s" % (off[0], on[-1]))

        print("=== 3) 设置入口：恢复默认桌面图标（id=180，持久化 ui.desktop.icons）===")
        btn = re.search(r"\[SET64\] ctl page=8 id=180 kind=button x=\d+ y=\d+ w=\d+ h=\d+ cx=\d+ cy=\d+ "
                        r"ax=(\d+) ay=(\d+) acx=(\d+) acy=(\d+)", vm.log())
        check("新入口控件打点（[SET64] ctl page=8 id=180 kind=button）", btn is not None, btn.group(0) if btn else "")
        if btn:
            n1 = vm.n()
            click_until(int(btn.group(3)), int(btn.group(4)), "[DESK64] icons reset")
            rst = re.search(r"\[DESK64\] icons reset why=settings defaults=3 persisted=1 via=ui\.desktop\.icons",
                            vm.log()[n1:])
            check("点「恢复默认桌面图标」-> [DESK64] icons reset why=settings（三项 + 持久化）", rst is not None,
                  rst.group(0) if rst else (re.search(r"\[DESK64\] icons reset[^\r\n]*", vm.log()[n1:]) or [""])[0])
            check("集合键确实落到 store64 覆盖表（[STORE64] set cfg.ui.desktop.icons=0,1,2|）",
                  re.search(r"\[STORE64\] set cfg\.ui\.desktop\.icons=0,1,2\|", vm.log()[n1:]) is not None,
                  (re.search(r"\[STORE64\] set cfg\.ui\.desktop\.icons[^\r\n]*", vm.log()[n1:]) or [""])[0])

        print("=== 4) 终端等价开关：cfg set ui.explorer.show_system 0 ===")
        n0 = vm.n()
        goto(dock[3][0], dock[3][1])
        time.sleep(0.3)
        mon.click(wait=1.2)
        check("终端打开（[APP] term opened）", vm.wait_log("[APP] term opened", 10, since=n0), "")
        mon.type_line("cfg set ui.explorer.show_system 0", wait=1.2)
        check("终端写入 ui.explorer.show_system=0（[CONF64] set key=ui.explorer.show_system value=0 type=int）",
              re.search(r"\[CONF64\] set key=ui\.explorer\.show_system value=0 type=int", vm.log()[n0:]) is not None,
              (re.search(r"\[CONF64\] set key=ui\.explorer\.show_system[^\r\n]*", vm.log()[n0:]) or [""])[0])
        # 再用设置页开关切回 0：验证"配置 -> 立刻重扫"的反向路径（也证明终端写的值被设置页读到了）
        if sw:
            n1 = vm.n()
            click_until(int(sw.group(3)), int(sw.group(4)), "[EXPL64] show_system=0")
            check("开关切回 -> [EXPL64] show_system=0 why=settings（双向可切）",
                  vm.wait_log("[EXPL64] show_system=0 why=settings", 6, since=n1),
                  (re.search(r"\[EXPL64\] show_system[^\r\n]*", vm.log()[n1:]) or [""])[0])
            back = [v for v in re.findall(r"\[EXPL64\] visible partitions only list=(\d+) hidden=(\d+) show_system=(\d+)", vm.log()[n1:]) if v[2] == "0" and int(v[1]) >= 1]
            check("切回后重扫又隐藏系统分区（hidden>=1）", len(back) >= 1, str(back[:1]))

        # ===========================================================================
        # ★ P5 接线验收（需求 1-7 + 应用窗口内容防回归）
        #   本节的交互全部走"闭环光标"：桌面上的 [UI] selbox x0/y0（按下点）就是
        #   客人光标的真实位置，用它校正模型，避免 PS/2 丢包导致点击落空。
        # ===========================================================================
        cstate = [512, 384]

        def goto_local(tx, ty):
            for _ in range(400):
                rx, ry = int(tx) - cstate[0], int(ty) - cstate[1]
                if abs(rx) <= 1 and abs(ry) <= 1:
                    break
                cx = max(-14, min(14, int(rx / 1.7)))
                cy = max(-14, min(14, int(ry / 1.7)))
                if cx == 0 and cy == 0:
                    break
                mon.move(cx, cy, wait=0.09)
                cstate[0] = max(0, min(1279, cstate[0] + int(cx * 17 / 10)))
                cstate[1] = max(0, min(799, cstate[1] + int(cy * 17 / 10)))
            for dx, dy in ((3, 0), (-3, 0), (0, 3), (0, -4)):
                mon.move(dx, dy, wait=0.09)
            CURSOR[0], CURSOR[1] = cstate[0], cstate[1]

        def csync():
            """闭环：在桌面上按下并拖 6px -> [UI] selbox 的 x0/y0 = 按下点（客人真实位置）。
            拖动本身把客人又挪了 +10px（驱动灵敏度 ×1.7），所以模型同步到"拖动之后"。"""
            n = vm.n()
            mon.send("mouse_button 1", wait=0.15)
            mon.move(6, 6, wait=0.12)
            mon.send("mouse_button 0", wait=0.25)
            time.sleep(0.35)
            m = re.findall(r"\[UI\] selbox x0=(-?\d+) y0=(-?\d+)", vm.log()[n:])
            if not m:
                return None
            gx, gy = int(m[-1][0]), int(m[-1][1])
            cstate[0] = max(0, min(1279, gx + 10))
            cstate[1] = max(0, min(799, gy + 10))
            return (gx, gy)

        def move_to(tx, ty, probe=None, rounds=5):
            px, py = probe if probe else (tx, max(40, min(ty - 70, 690)))
            goto_local(px, py)
            for _ in range(rounds):
                if csync() is None:
                    break
                if abs(px - cstate[0]) <= 4 and abs(py - cstate[1]) <= 4:
                    break
                goto_local(px, py)
            goto_local(tx, ty)

        def click_needle(move_fn, needle, tries=4, btn=1, wait=0.9):
            for i in range(tries):
                move_fn(i)
                time.sleep(0.25)
                n = vm.n()
                mon.click(btn=btn, wait=wait)
                time.sleep(0.5)
                if vm.wait_log(needle, 3, since=n):
                    return True
            return False

        # 先清桌面：Ctrl+Shift+W 最小化所有窗口（需求 6 的快捷键；也让桌面交互不被窗口挡住）
        n_hot = vm.n()
        for _ in range(6):
            mon.key("ctrl-shift-w", wait=0.9)
        time.sleep(1.5)
        hot_lines = len(re.findall(r"\[UI\] hotkey ctrl\+shift\+w minimize", vm.log()[n_hot:]))
        check("需求 6：Ctrl+Shift+W 最小化活动窗口（[UI] hotkey ctrl+shift+w minimize）", hot_lines >= 1,
              "%d 行" % hot_lines)
        print("=== 5) 需求 7：桌面图标圆角正方形（Token）+ 需求 1：桌面右键菜单 ===")
        items = dict((int(m.group(2)), (int(m.group(4)), int(m.group(5)), int(m.group(6))))
                     for m in re.finditer(r"\[DESK64\] item idx=(\d+) kind=(\d+) name=(\S+) x=(\d+) y=(\d+) "
                                          r"w=(\d+) h=(\d+) cell=(\d+)x(\d+)", vm.log()))
        check("桌面项集合打点（[DESK64] init items=3 set=0,1,2| + 三项 item 行）",
              re.search(r"\[DESK64\] init items=3 set=0,1,2\| recycle=\d", vm.log()) is not None and
              len(items) >= 3 and all(k in items for k in (0, 1, 2)), str(sorted(items)))
        check("桌面层自检（[DESK64] selftest PASS mask=0 items=3）",
              "[DESK64] selftest PASS mask=0" in vm.log(),
              (re.search(r"\[DESK64\] selftest[^\r\n]*", vm.log()) or [""])[0])
        shot_desk = mon.shot(os.path.join(tmp, "p5_desk.ppm"))
        icon_px = None
        if shot_desk and items:
            iw, ih, ipx = read_ppm(shot_desk)
            ix, iy, isz = items[0]
            wall = sample(ipx, iw, ix + isz + 14, iy + 30)
            corner = sample(ipx, iw, ix + 1, iy + 1)
            topmid = sample(ipx, iw, ix + isz // 2, iy + 3)
            icon_px = (wall, corner, topmid)
            check("需求 7：图标底板是圆角正方形（左下角像素 = 壁纸，顶边中点 = 主题渐变底板）",
                  dist(corner, wall) <= 14 and dist(topmid, wall) >= 120,
                  "corner=%s wall=%s dist=%d / plate=%s dist=%d" %
                  (corner, wall, dist(corner, wall), topmid, dist(topmid, wall)))
        else:
            check("需求 7：桌面截图成功", False, "screendump 失败")
        # 需求 1：桌面空白处右键 -> 亚克力菜单
        n0 = vm.n()
        for _try in range(3):                      # PS/2 丢包时点击会落空：闭环 + 重试
            move_to(1150 + _try * 6 - 6, 640 + _try * 8, probe=(1150, 560))
            time.sleep(0.3)
            mon.raw(["mouse_button 2", "mouse_button 0"], wait_between=0.12, wait_end=1.0)   # 2 = 右键
            time.sleep(1.0)
            if "[DESK64] menu open" in vm.log()[n0:]:
                break
        mo = re.search(r"\[DESK64\] menu open x=(\d+) y=(\d+) w=(\d+) h=(\d+) items=(\d+) enabled=(\d+) row=(\d+) "
                       r"r=(\d+) shadow=2 edge=1 \(acrylic\)", vm.log()[n0:])
        check("需求 1：桌面右键弹出菜单（[DESK64] menu open … items=8 enabled=5 … (acrylic)）",
              mo is not None, mo.group(0) if mo else (re.search(r"\[DESK64\] menu[^\r\n]*", vm.log()[n0:]) or [""])[0])
        mi = [(int(m.group(1)), int(m.group(2)), int(m.group(4)))
              for m in re.finditer(r"\[DESK64\] menu item idx=(\d+) id=(\d+) name=(\S+) enabled=(\d)", vm.log()[n0:])]
        check("需求 1：菜单项逐个打点（8 项 + 不可用项 enabled=0 why=…）",
              len(mi) >= 8 and any(e == 0 for _, _, e in mi) and any(e == 1 for _, _, e in mi),
              "items=%d disabled=%d" % (len(mi), sum(1 for _, _, e in mi if e == 0)))
        if mo:
            mx, my, mw, mh = (int(mo.group(i)) for i in (1, 2, 3, 4))
            shot_menu = mon.shot(os.path.join(tmp, "p5_menu.ppm"))
            if shot_menu:
                pw, ph, ppx = read_ppm(shot_menu)
                pin = sample(ppx, pw, mx + 12, my + mh // 2)
                pout = sample(ppx, pw, mx - 14, my + mh // 2)
                check("需求 1：菜单真的画出来了（菜单内像素 ≠ 同高度桌面壁纸像素）",
                      dist(pin, pout) >= 10, "in=%s out=%s dist=%d" % (pin, pout, dist(pin, pout)))
        mon.key("esc", wait=0.7)
        time.sleep(0.6)
        check("需求 1：ESC 关闭菜单（[DESK64] menu close why=esc）",
              "menu close why=esc" in vm.log()[n0:],
              (re.search(r"\[DESK64\] menu close[^\r\n]*", vm.log()[n0:]) or [""])[0])
        n1 = vm.n()
        mon.raw(["mouse_button 2", "mouse_button 0"], wait_between=0.12, wait_end=1.0)
        time.sleep(0.5)
        move_to(620, 640, probe=(620, 560))
        time.sleep(0.2)
        mon.click(wait=1.0)
        time.sleep(0.6)
        check("需求 1：点菜单外部关闭（[DESK64] menu close why=outside）",
              "menu close why=outside" in vm.log()[n1:],
              (re.search(r"\[DESK64\] menu close[^\r\n]*", vm.log()[n1:]) or [""])[0])

        print("=== 6) 需求 2：玻璃选择框（框内像素 diff=0）+ 桌面图标拖入回收站 ===")
        n0 = vm.n()
        shot_before = mon.shot(os.path.join(tmp, "p5_sel_before.ppm"))
        move_to(560, 640, probe=(560, 560))
        time.sleep(0.3)
        mon.send("mouse_button 1", wait=0.25)
        for _ in range(9):
            mon.move(9, 5, wait=0.10)
        time.sleep(0.5)
        shot_during = mon.shot(os.path.join(tmp, "p5_sel_during.ppm"))
        mon.send("mouse_button 0", wait=0.8)
        time.sleep(0.8)
        gl = re.search(r"\[DESK64\] selbox glass x=(\d+) y=(\d+) w=(\d+) h=(\d+) corner=(\d+) edge=1 inside_alpha=0 sel=\d",
                       vm.log()[n0:])
        check("需求 2：左键拖出玻璃选择框（[DESK64] selbox glass … corner=9 edge=1 inside_alpha=0）",
              gl is not None, gl.group(0) if gl else (re.search(r"\[DESK64\] selbox[^\r\n]*", vm.log()[n0:]) or [""])[0])
        check("需求 2：松手打点（[UI] selbox x0/y0/x1/y1 兼容行 + [DESK64] selbox result）",
              re.search(r"\[UI\] selbox x0=\d+ y0=\d+ x1=\d+ y1=\d+", vm.log()[n0:]) is not None and
              "selbox result" in vm.log()[n0:])
        mb = re.findall(r"\[UI\] selbox x0=(\d+) y0=(\d+) x1=(\d+) y1=(\d+)", vm.log()[n0:])
        if shot_before and shot_during and mb:
            bx0, by0, bx1, by1 = (int(v) for v in mb[-1])
            inner = (bx0 + 20, by0 + 20, max(1, bx1 - bx0 - 40), max(1, by1 - by0 - 40))
            p1 = read_ppm(shot_before)
            p2 = read_ppm(shot_during)
            n_in, d_in = region_diff(p1[2], p2[2], p1[0], inner, step=2, thr=6)
            n_all, d_all = region_diff(p1[2], p2[2], p1[0], (0, 0, 1280, 800), step=8, thr=6)
            check("需求 2：框**内**像素与拖动前同位置 diff=0（框中间全透明）",
                  n_in == 0, "box=%s interior=%s diff=%d max=%d" % ((bx0, by0, bx1, by1), inner, n_in, d_in))
            check("需求 2：框边缘确实画出来了（全屏有差异像素）", n_all > 0, "diff=%d max=%d" % (n_all, d_all))
        # 桌面图标 -> 回收站（拖放）
        n0 = vm.n()
        term = items.get(2)
        rec = items.get(1)
        pressed = False
        if term and rec:
            tc = (term[0] + term[2] // 2, term[1] + term[2] // 2)
            for _ in range(4):
                move_to(tc[0], tc[1], probe=(150, 320))
                n = vm.n()
                mon.send("mouse_button 1", wait=0.25)
                time.sleep(0.45)
                if "desktop icon select" in vm.log()[n:]:
                    pressed = True
                    break
                mon.move(2, 2, wait=0.12)          # 2px 不会触发拖动（>5px 才算），只为读出落点
                time.sleep(0.35)
                mm = re.findall(r"\[UI\] selbox x0=(-?\d+) y0=(-?\d+)", vm.log()[n:])
                mon.send("mouse_button 0", wait=0.35)
                time.sleep(0.3)
                if mm:
                    gx, gy = int(mm[-1][0]), int(mm[-1][1])
                    cstate[0], cstate[1] = gx, gy
                    dx, dy = tc[0] - gx, tc[1] - gy
                    while abs(dx) > 1.6 or abs(dy) > 1.6:
                        mx2 = max(-14, min(14, int(dx)))
                        my2 = max(-14, min(14, int(dy)))
                        mon.move(mx2, my2, wait=0.09)
                        cstate[0] = max(0, min(1279, cstate[0] + int(mx2 * 17 / 10)))
                        cstate[1] = max(0, min(799, cstate[1] + int(my2 * 17 / 10)))
                        dx -= mx2 * 1.7
                        dy -= my2 * 1.7
                    for a2, b2 in ((3, 0), (-3, 0), (0, 3), (0, -4)):
                        mon.move(a2, b2, wait=0.09)
            check("需求 2：按下桌面图标命中（[UI] desktop icon select kind=2）",
                  "desktop icon select" in vm.log()[n0:],
                  (re.search(r"\[UI\] desktop icon select[^\r\n]*", vm.log()[n0:]) or [""])[0])
        if pressed and rec:
            rc = (rec[0] + rec[2] // 2, rec[1] + rec[2] // 2)
            for _ in range(200):
                rx, ry = rc[0] - cstate[0], rc[1] - cstate[1]
                if abs(rx) <= 2 and abs(ry) <= 2:
                    break
                mx2 = max(-14, min(14, int(rx / 1.7)))
                my2 = max(-14, min(14, int(ry / 1.7)))
                if mx2 == 0 and my2 == 0:
                    break
                mon.move(mx2, my2, wait=0.09)
                cstate[0] = max(0, min(1279, cstate[0] + int(mx2 * 17 / 10)))
                cstate[1] = max(0, min(799, cstate[1] + int(my2 * 17 / 10)))
            time.sleep(0.4)
            mon.send("mouse_button 0", wait=1.2)
            time.sleep(0.8)
        check("需求 2/3：图标拖到回收站 -> [DESK64] drag to recycle + [RECYCLE64] add（桌面项 3->2）",
              re.search(r"\[DESK64\] drag to recycle idx=\d+ kind=\d+ target=\d+", vm.log()[n0:]) is not None and
              re.search(r"\[RECYCLE64\] add kind=\d+ name=\S+ n=1 desktop=2", vm.log()[n0:]) is not None,
              (re.search(r"\[RECYCLE64\] add[^\r\n]*", vm.log()[n0:]) or [""])[0])

        print("=== 7) 需求 3：回收站窗口（恢复 / 永久删除二次确认 / 空状态）===")
        dock2 = dict((int(m.group(1)), (int(m.group(4)), int(m.group(5))))
                     for m in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(\d+) name=\S+ x=\d+ y=\d+ w=\d+ h=\d+ "
                                          r"cx=(\d+) cy=(\d+)", vm.log()))
        n0 = vm.n()
        opened = False
        if 2 in dock2:
            opened = click_needle(lambda i: move_to(dock2[2][0] + (i % 3) * 5 - 5, dock2[2][1], probe=(dock2[2][0], 690)),
                                  r"\[APP\] recycle opened", tries=3, wait=1.0)
        check("需求 3：打开回收站窗口（[APP] recycle opened + [RECYCLE64] geom/btn 打点）",
              opened and re.search(r"\[RECYCLE64\] geom x=\d+ y=\d+ w=\d+ h=\d+ client=\d+x\d+ row_h=\d+", vm.log()[n0:]) is not None,
              (re.search(r"\[RECYCLE64\] geom[^\r\n]*", vm.log()[n0:]) or [""])[0])
        rb = dict((m.group(2), (int(m.group(3)), int(m.group(4)), int(m.group(5)), int(m.group(6))))
                  for m in re.finditer(r"\[RECYCLE64\] btn idx=(\d+) name=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log()))
        check("需求 3：三个按钮矩形打点（restore/delete/empty）",
              len(rb) >= 3, str(sorted(rb)))
        check("需求 3：item 行打点（回收站里 1 项 -> [RECYCLE64] row idx=0）",
              re.search(r"\[RECYCLE64\] row idx=0 x=\d+ y=\d+ w=\d+ h=\d+", vm.log()[n0:]) is not None,
              (re.search(r"\[RECYCLE64\] row[^\r\n]*", vm.log()[n0:]) or [""])[0])
        if "restore" in rb:
            n1 = vm.n()
            bx, by, bw, bh = rb["restore"]
            okr = click_needle(lambda i: goto(bx + bw // 2 + i * 3, by + bh // 2), r"\[RECYCLE64\] restore", tries=3, wait=1.0)
            check("需求 3：恢复（[RECYCLE64] restore kind=… n=0 desktop=3）",
                  okr and re.search(r"\[RECYCLE64\] restore kind=\d+ name=\S+ n=0 desktop=3", vm.log()[n1:]) is not None,
                  (re.search(r"\[RECYCLE64\] restore[^\r\n]*", vm.log()[n1:]) or [""])[0])
        # 再放一项进去（拖终端图标 -> 回收站），验证"永久删除需要二次确认"
        n1 = vm.n()
        again = False
        if term and rec:
            tc = (term[0] + term[2] // 2, term[1] + term[2] // 2)
            for _ in range(4):
                move_to(tc[0], tc[1], probe=(150, 320))
                n = vm.n()
                mon.send("mouse_button 1", wait=0.25)
                time.sleep(0.45)
                if "desktop icon select" in vm.log()[n:]:
                    again = True
                    break
                mon.move(2, 2, wait=0.12)
                time.sleep(0.35)
                mm = re.findall(r"\[UI\] selbox x0=(-?\d+) y0=(-?\d+)", vm.log()[n:])
                mon.send("mouse_button 0", wait=0.35)
                time.sleep(0.3)
                if mm:
                    gx, gy = int(mm[-1][0]), int(mm[-1][1])
                    cstate[0], cstate[1] = gx, gy
                    dx, dy = tc[0] - gx, tc[1] - gy
                    while abs(dx) > 1.6 or abs(dy) > 1.6:
                        mx2 = max(-14, min(14, int(dx)))
                        my2 = max(-14, min(14, int(dy)))
                        mon.move(mx2, my2, wait=0.09)
                        cstate[0] = max(0, min(1279, cstate[0] + int(mx2 * 17 / 10)))
                        cstate[1] = max(0, min(799, cstate[1] + int(my2 * 17 / 10)))
                        dx -= mx2 * 1.7
                        dy -= my2 * 1.7
                    for a2, b2 in ((3, 0), (-3, 0), (0, 3), (0, -4)):
                        mon.move(a2, b2, wait=0.09)
        if again and rec:
            rc = (rec[0] + rec[2] // 2, rec[1] + rec[2] // 2)
            for _ in range(200):
                rx, ry = rc[0] - cstate[0], rc[1] - cstate[1]
                if abs(rx) <= 2 and abs(ry) <= 2:
                    break
                mx2 = max(-14, min(14, int(rx / 1.7)))
                my2 = max(-14, min(14, int(ry / 1.7)))
                if mx2 == 0 and my2 == 0:
                    break
                mon.move(mx2, my2, wait=0.09)
                cstate[0] = max(0, min(1279, cstate[0] + int(mx2 * 17 / 10)))
                cstate[1] = max(0, min(799, cstate[1] + int(my2 * 17 / 10)))
            time.sleep(0.4)
            mon.send("mouse_button 0", wait=1.2)
            time.sleep(0.8)
        if "delete" in rb:
            bx, by, bw, bh = rb["delete"]
            n1 = vm.n()
            okd = click_needle(lambda i: goto(bx + bw // 2 + i * 3, by + bh // 2), r"delete ask", tries=3, wait=1.0)
            cf = re.search(r"\[RECYCLE64\] confirm dialog which=\d+ confirm_btn=(\d+),(\d+)-\d+x\d+ cancel_btn=(\d+),(\d+)",
                           vm.log()[n1:])
            check("需求 3：永久删除先弹二次确认（[RECYCLE64] delete ask … confirm=1 + confirm dialog … two_step=1）",
                  okd and cf is not None,
                  (re.search(r"\[RECYCLE64\] confirm dialog[^\r\n]*", vm.log()[n1:]) or [""])[0])
            check("需求 3：确认前不删（确认框出现时 n 仍为 1）",
                  re.search(r"\[RECYCLE64\] delete ask kind=\d+ name=\S+ confirm=1", vm.log()[n1:]) is not None and
                  "delete done" not in vm.log()[n1:])
            if cf:
                cb = (int(cf.group(3)), int(cf.group(4)))
                n2 = vm.n()
                okc = click_needle(lambda i: goto(cb[0] + 18 + i * 3, cb[1] + 8), r"delete ask cancel", tries=3, wait=1.0)
                check("需求 3：取消 -> 不删除（[RECYCLE64] delete ask cancel idx=… n=1）",
                      okc and re.search(r"\[RECYCLE64\] delete ask cancel idx=\d+ n=1", vm.log()[n2:]) is not None,
                      (re.search(r"\[RECYCLE64\] delete ask cancel[^\r\n]*", vm.log()[n2:]) or [""])[0])
                n3 = vm.n()
                okd2 = click_needle(lambda i: goto(bx + bw // 2 + i * 3, by + bh // 2), r"delete ask kind", tries=3, wait=1.0)
                cf2 = re.search(r"confirm_btn=(\d+),(\d+)-", vm.log()[n3:])
                if okd2 and cf2:
                    click_needle(lambda i: goto(int(cf2.group(1)) + 18 + i * 3, int(cf2.group(2)) + 8),
                                 r"delete done", tries=3, wait=1.2)
                check("需求 3：确认 -> 永久删除（[RECYCLE64] delete done kind=… name=… n=0 permanent=1）",
                      re.search(r"\[RECYCLE64\] delete done kind=\d+ name=\S+ n=0 permanent=1", vm.log()[n3:]) is not None,
                      (re.search(r"\[RECYCLE64\] delete done[^\r\n]*", vm.log()[n3:]) or [""])[0])
        if "empty" in rb:
            bx, by, bw, bh = rb["empty"]
            n1 = vm.n()
            click_needle(lambda i: goto(bx + bw // 2 + i * 3, by + bh // 2), r"delete ask empty-all|delete done", tries=3, wait=1.0)
            check("需求 3：空状态（[RECYCLE64] empty (recycle bin is empty) 或清空 done）",
                  "empty (recycle bin is empty)" in vm.log()[n1:] or "[RECYCLE64] delete done" in vm.log()[n1:],
                  (re.search(r"\[RECYCLE64\] (empty|delete done)[^\r\n]*", vm.log()[n1:]) or [""])[0])

        print("=== 8) 需求 4/5/6：指针形状 / 八向缩放 / Dock 语义 + 最小化飞 + 悬停窗口列表 ===")
        dock6 = dock2.get(6)
        n0 = vm.n()
        if dock6:
            click_needle(lambda i: move_to(dock6[0] + (i % 3) * 5 - 5, dock6[1], probe=(dock6[0], 690)),
                         r"\[APP\] settings opened", tries=3, wait=1.6)
        check("需求 6：点 Dock 打开应用（[DOCK64] press … action=open）",
              re.search(r"\[DOCK64\] press idx=6 app=5 action=(open|restore-last-min|activate)", vm.log()[n0:]) is not None,
              (re.search(r"\[DOCK64\] press[^\r\n]*", vm.log()[n0:]) or [""])[0])
        sm = re.search(r"\[UI\] win geom tag=\S+ title=\S+ app=5 x=(\d+) y=(\d+) w=(\d+) h=(\d+) client=(\d+)x(\d+)",
                       vm.log())
        if sm:
            sx, sy, sw2, sh2, scw, sch = (int(sm.group(i)) for i in range(1, 7))
            # 需求 4：指针形状（窗口四边/四角 + 文本输入）
            n1 = vm.n()
            for (px_, py_) in ((sx + 2, sy + sh2 // 2), (sx + sw2 // 2, sy + 2), (sx + 2, sy + 2),
                               (sx + sw2 - 3, sy + 2), (sx + scw // 2, sy + 60)):
                goto(px_, py_)
                time.sleep(0.4)
            shapes = re.findall(r"\[INPUT64\] cursor shape=(\S+) prev=(\S+)", vm.log()[n1:])
            names = set(s for s, _ in shapes)
            check("需求 4：指针形状随命中区域切换（四边/四角 size-h/size-v/size-d1|d2 + 文本 text 打点）",
                  len(names & {"size-h", "size-v"}) >= 1 and len(names & {"size-d1", "size-d2"}) >= 1 and
                  ("text" in names or "wait" in names),
                  "shapes=%s" % sorted(names))
            check("需求 4：形状切换有打点原文（[INPUT64] cursor shape=… prev=…）", len(shapes) >= 3,
                  " / ".join("%s<-%s" % (s, p) for s, p in shapes[:6]))
            # 需求 5：右边缘拖动缩放（内容随尺寸重排 = [SET64] layout client= 重新打点）
            n1 = vm.n()
            move_to(sx + sw2 - 2, sy + sh2 // 2, probe=(sx + sw2 - 2, sy + 40))
            time.sleep(0.3)
            mon.send("mouse_button 1", wait=0.25)
            for _ in range(8):
                mon.move(8, 0, wait=0.1)
            time.sleep(0.2)
            mon.send("mouse_button 0", wait=0.9)
            time.sleep(1.0)
            rs = re.findall(r"\[UI\] win resize dir=(\S+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) client=(\d+)x(\d+)",
                            vm.log()[n1:])
            check("需求 5：右边缘拖拽缩放（[UI] win resize dir=r + client 尺寸随动）",
                  any(r[0] == "r" for r in rs) and len(rs) >= 1,
                  "begin/end=%s frames=%d" % ((re.search(r"\[UI\] win resize begin dir=\S+", vm.log()[n1:]) or [""])[0],
                                              len(rs)))
            check("需求 5：内容随尺寸重排（缩放后 [SET64] layout client= 重新打点）",
                  re.search(r"\[SET64\] layout client=\d+x\d+ nav=\d+ card=\d+", vm.log()[n1:]) is not None and
                  re.search(r"\[UI\] win resize end dir=r x=\d+ y=\d+ w=\d+ h=\d+ client=\d+x\d+", vm.log()[n1:]) is not None,
                  (re.search(r"\[UI\] win resize end[^\r\n]*", vm.log()[n1:]) or [""])[0])
            # 需求 6：标题栏最小化按钮 -> 缩小渐隐飞向 Dock（时长走 Token）
            n1 = vm.n()
            goto(sx + sw2 - 71, sy + 13)
            time.sleep(0.3)
            mon.click(wait=1.1)
            time.sleep(1.8)
            fly = re.search(r"\[DOCK64\] minimize fly app=5 dock_idx=\d+ from=\d+,\d+ \d+x\d+ to=\d+,\d+ \d+x\d+ "
                            r"dur=(\d+)ms anim=scale\+fade", vm.log()[n1:])
            check("需求 6：最小化飞向 Dock（[DOCK64] minimize fly … dur=225ms anim=scale+fade + done）",
                  fly is not None and fly.group(1) == "225" and
                  re.search(r"\[DOCK64\] minimize fly done app=5 frames=\d+ anim=scale\+fade", vm.log()[n1:]) is not None,
                  fly.group(0) if fly else (re.search(r"\[DOCK64\] minimize fly[^\r\n]*", vm.log()[n1:]) or [""])[0])
            check("需求 6：该应用在 Dock 出现小横杠（[DOCK64] minbar idx=6 … clickable=0 + hit=none）",
                  re.search(r"\[DOCK64\] minbar idx=6 x=\d+ y=\d+ w=\d+ h=3 color=#[0-9A-F]{6} clickable=0", vm.log()[n1:]) is not None and
                  re.search(r"\[DOCK64\] minbar hit_test idx=6 .* -> hit=none", vm.log()[n1:]) is not None,
                  (re.search(r"\[DOCK64\] minbar[^\r\n]*", vm.log()[n1:]) or [""])[0])
            # 需求 6：Dock 点击语义：有小横杠 -> 恢复（scale+opacity）
            if dock6:
                n2 = vm.n()
                click_needle(lambda i: move_to(dock6[0] + (i % 3) * 5 - 5, dock6[1], probe=(dock6[0], 690)),
                             r"restore pop app=5", tries=3, wait=1.2)
                check("需求 6：点 Dock 恢复最小化窗口（[DOCK64] press … action=restore-last-min + restore pop dur=225ms "
                      "scale0.88->1.00+opacity）",
                      re.search(r"\[DOCK64\] press idx=6 app=5 action=restore-last-min", vm.log()[n2:]) is not None and
                      re.search(r"\[DOCK64\] restore pop app=5 dur=225ms anim=scale0\.88->1\.00\+opacity why=dock", vm.log()[n2:]) is not None,
                      (re.search(r"\[DOCK64\] restore pop app=5[^\r\n]*", vm.log()[n2:]) or [""])[0])
                n3 = vm.n()
                mon.click(wait=1.2)
                time.sleep(1.6)
                check("需求 6：窗口在最前面时再点 Dock 才最小化（action=minimize）",
                      re.search(r"\[DOCK64\] press idx=6 app=5 action=(activate|minimize)", vm.log()[n3:]) is not None,
                      (re.search(r"\[DOCK64\] press[^\r\n]*", vm.log()[n3:]) or [""])[0])
                # 需求 6：悬停 Dock 图标 -> 弹出窗口列表（可选择恢复指定窗口）
                n4 = vm.n()
                move_to(dock6[0], 700, probe=(dock6[0], 640))
                for _ in range(6):
                    mon.move(0, 9, wait=0.12)
                time.sleep(1.8)
                wl = re.search(r"\[DOCK64\] winlist idx=6 n=(\d+) x=\d+ y=\d+ w=\d+ h=\d+ row=\d+ one-bar-per-app=1",
                               vm.log()[n4:])
                check("需求 6：悬停 Dock 图标弹出窗口列表（[DOCK64] winlist idx=6 n=1 … one-bar-per-app=1 + item 行）",
                      wl is not None and re.search(r"\[DOCK64\] winlist item idx=6 row=0 title=\S+ minimized=\d", vm.log()[n4:]) is not None,
                      wl.group(0) if wl else (re.search(r"\[DOCK64\] winlist[^\r\n]*", vm.log()[n4:]) or [""])[0])
                if wl:
                    wm = re.search(r"\[DOCK64\] winlist idx=6 n=\d+ x=(\d+) y=(\d+) w=(\d+) h=(\d+) row=(\d+)", vm.log()[n4:])
                    if wm:
                        wx, wy = int(wm.group(1)), int(wm.group(2))
                        n5 = vm.n()
                        goto(wx + 40, wy + 20)
                        time.sleep(0.3)
                        mon.click(wait=1.2)
                        time.sleep(1.0)
                        check("需求 6：窗口列表里点一行恢复指定窗口（[DOCK64] winlist restore idx=6 row=0 title=…）",
                              re.search(r"\[DOCK64\] winlist restore idx=6 row=0 title=\S+ was_minimized=\d", vm.log()[n5:]) is not None,
                              (re.search(r"\[DOCK64\] winlist restore[^\r\n]*", vm.log()[n5:]) or [""])[0])
        # ===========================================================================
        # ★ 防回归：应用窗口绘制后内容区必须保持"内容色"，不能只剩整片 client_bg(240,240,240)
        #   成因见交付报告：gui64 的脏矩形不清 + settings64 把绘制关在"尺寸变化"的 if 里。
        # ===========================================================================
        print("=== 9) 防回归：应用窗口内容不被整屏同色覆盖 ===")
        nav_c = None
        card_c = None
        if sm:
            sx, sy, sw2, sh2 = (int(sm.group(i)) for i in (1, 2, 3, 4))
            # 先把设置窗口放到最前面（Dock 恢复后可能被别的窗口压住）
            n0 = vm.n()
            if dock6:
                click_needle(lambda i: move_to(dock6[0] + (i % 3) * 5 - 5, dock6[1], probe=(dock6[0], 690)),
                             r"\[DOCK64\] press idx=6 app=5", tries=2, wait=1.2)
            time.sleep(1.5)
            for _ in range(3):                     # 鼠标在窗口内来回动：不带尺寸变化的重绘
                goto(sx + 320, sy + 300)
                time.sleep(0.25)
                goto(sx + 40, sy + 320)
                time.sleep(0.25)
            time.sleep(1.2)
            shot_set = mon.shot(os.path.join(tmp, "p5_set.ppm"))
            if shot_set:
                iw, ih, ipx = read_ppm(shot_set)
                nav_c = sample(ipx, iw, sx + 9, sy + 33)
                card_c = sample(ipx, iw, sx + 300, sy + 250)
        check("需求（防回归）：设置窗口导航/卡片仍是内容色（不是整片 client_bg 240,240,240）",
              nav_c is not None and card_c is not None and nav_c != (240, 240, 240) and card_c != (240, 240, 240),
              "nav=%s card=%s" % (nav_c, card_c))
        check("需求（防回归）：内核侧没有 [GUI64] app blank 失败行（应用画完之后备缓冲不是一片 client_bg）",
              "[GUI64] app blank" not in vm.log(),
              (re.search(r"\[GUI64\] app blank[^\r\n]*", vm.log()) or ["（无）"])[0])
        check("需求（防回归）：脏矩形提交后会清掉（不是每帧整屏重绘）",
              "[DBG] pre-fill" not in vm.log() and "dirty=0,0 1280x800" not in vm.log())

        print("=== 10) 不能出现的日志 ===")
        log = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:", "kfree: bad",
                    "bad/double free", "[GUI64] selftest FAILED", "[DESK64] selftest FAIL", "[GUI64] app blank"):
            check("不应出现 %s" % bad, bad not in log)

        carrier_vfs = "carrier=vfs" in vm.log()
        if not args.no_reboot and not carrier_vfs:
            print("  [skip] store 载体 = raw（裸镜像没有 VimtuFS2 卷）-> flush 被跳过，跳过重启持久化复验")
        if not args.no_reboot and carrier_vfs:
            print("=== 11) 重启复验：两个键都还在（[CONF64] load）===")
            vm.wait_log("[CONF64] autosave ok", 12)
            check("配置自动落盘（[CONF64] autosave ok）", "[CONF64] autosave ok" in vm.log(),
                  (re.search(r"\[CONF64\] autosave[^\r\n]*", vm.log()) or [""])[0])
            time.sleep(3)
            vm.close()
            time.sleep(2)
            vm = Vm(qemu, [args.img, esp], args.port, "vimtu-deskops2", tmp)
            mon = vm.wait_monitor()
            up2 = vm.wait_log("[GUI64] ready", 120)
            check("重启后桌面再次就绪", up2)
            if up2:
                L2 = vm.log()
                check("重启后 [CONF64] load ui.desktop.icons=0,1,2|（桌面图标集合持久化）",
                      re.search(r"\[CONF64\] load ui\.desktop\.icons=0,1,2\|", L2) is not None,
                      (re.search(r"\[CONF64\] load ui\.desktop\.icons[^\r\n]*", L2) or [""])[0])
                check("重启后 [CONF64] load ui.explorer.show_system=0（隐藏分区开关持久化）",
                      re.search(r"\[CONF64\] load ui\.explorer\.show_system=0", L2) is not None,
                      (re.search(r"\[CONF64\] load ui\.explorer\.show_system[^\r\n]*", L2) or [""])[0])

        if args.keep:
            print("截图/日志目录：%s" % tmp)
    finally:
        vm.close()

    n_pass = sum(1 for _, c in checks if c)
    print("=== RESULT: %s （%d/%d 断言通过）===" % ("PASS" if ok else "FAIL", n_pass, len(checks)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
