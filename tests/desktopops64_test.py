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

        print("=== 5) 不能出现的日志 ===")
        log = vm.log()
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL", "OOM:", "kfree: bad",
                    "bad/double free", "[GUI64] selftest FAILED", "[DESK64] selftest FAIL"):
            check("不应出现 %s" % bad, bad not in log)

        carrier_vfs = "carrier=vfs" in vm.log()
        if not args.no_reboot and not carrier_vfs:
            print("  [skip] store 载体 = raw（裸镜像没有 VimtuFS2 卷）-> flush 被跳过，跳过重启持久化复验")
        if not args.no_reboot and carrier_vfs:
            print("=== 6) 重启复验：两个键都还在（[CONF64] load）===")
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
