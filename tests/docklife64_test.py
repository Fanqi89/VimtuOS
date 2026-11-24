#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/docklife64_test.py - ★ v0.11.3：Dock × 窗口生命周期（ring3 wl_surface 窗口）

这一条钉的是"任务栏真的活了"，不是像素好看：

  A. **运行项登记**：查看器用 28 号声明成窗口 -> 内核给它建 Dock 运行项
     `[DOCK64] ext-add surf=1 win=10001 title=...`，Dock 列表里出现
     `[DOCK64] item idx=<n> ... kind=running win=10001 cx=.. cy=..`（这条给坐标，测试据此点击）
  B. **活动高亮**：活动窗口（内核判：最高 id 且未最小化）在 Dock 上画**加宽胶囊**
     （`[DOCK64] extactive idx=<n> surf=1 x=.. y=.. w=.. h=4 color=#0078D7`）+ 像素判据。
  C. **最小化收容**：点窗口的最小按钮 -> `[WM] btn ... action=minimize`
     + `[WL64] ctl ... action=minimize ... minimized=1` + `[DOCK64] ext-state surf=1 minimized=1`
     + `[DOCK64] minbar idx=<n>`；像素：窗口矩形里**一个图片像素都不剩**（红/蓝/标题带全 0）。
  D. **点 Dock 恢复**：点 Dock 项 -> `[DOCK64] press ... action=ext-restore`
     + `[WL64] ctl ... action=restore`；像素：窗口矩形与最小化前**逐字节相同**（指针已挪开）。
  E. **关窗退出运行态**：点窗口的关闭按钮 -> `[WL64] ctl ... action=close`
     + `[DOCK64] ext-del surf=1`；`items=` 数下降，且窗口矩形回到桌面（0 图片像素）。

用法：py -3 tests\\docklife64_test.py [--qemu 路径] [--keep]
退出码：0 = 全过，1 = 有断言失败，2 = 环境问题。
"""
import argparse
import os
import re
import shutil
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import viewer64_test as V                                          # noqa: E402（复用 QEMU/VNC 夹具与读 PPM）
import qemuhelp as qh                                              # noqa: E402

# 合成器标题带的强调色（主题 light：accent = dock_dot = rgb(0,120,215)）
ACCENT = (0x00, 0x78, 0xD7)
TITLE_BG = (0xF2, 0xF4, 0xF8)


def near(c, t, tol=2):
    return abs(c[0] - t[0]) <= tol and abs(c[1] - t[1]) <= tol and abs(c[2] - t[2]) <= tol


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5641)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(errors="replace")
        except Exception:                                      # noqa: BLE001
            pass

    qemu = V.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    for p in (V.DISK_SRC, V.VIEWER_ELF):
        if not os.path.exists(p):
            sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % p)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_docklife_")
    disk = os.path.join(tmp, "disk.img")
    shutil.copyfile(V.DISK_SRC, disk)
    print("[docklife64] 夹具盘：%s（%d B）" % (disk, os.path.getsize(disk)))

    ok = True
    checks = []

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    def shot(name):
        p = os.path.join(tmp, name)
        if os.path.exists(p):
            os.remove(p)
        mon.shot(p)
        return V.read_ppm(p)

    def park_and_shot(name):
        """指针顶到 (0,0) 再截屏：否则指针像素会让"逐字节相同"这条判据假红。"""
        V.park_cursor(mon)
        time.sleep(0.5)
        return shot(name)

    vm = None
    mon = None
    try:
        vm = V.Vm(qemu, disk, args.port, os.path.join(tmp, "serial.log"), "vimtu-docklife", args.timeout)
        mon = V.Monitor(args.port)
        qh.login_desktop(mon, vm.log, vm.proc, timeout=min(args.timeout, 200))
        check("桌面就绪 [GUI64] ready", vm.wait_re(r"\[GUI64\] ready", args.timeout) is not None)

        # ---- 打开查看器（它自己 fork /lib/wm.elf 当合成器）----
        mon.key("meta_l", wait=1.0)
        mon.key("1", wait=2.0)
        check("终端打开", vm.wait_re(r"\[APP\] term opened", 15) is not None)
        n0 = vm.n()
        V.type_line(mon, "shell", per_key=0.12)
        check("ring3 外壳", vm.wait_re(r"\[SH64\] launch path=/bin/shell\.bin", 40, since=n0) is not None)

        n1 = vm.n()
        V.type_line(mon, "/bin/viewer /Pictures/redblue.png", per_key=0.09)
        mv = vm.wait_re(r"\[VIEWER\] surf id=(\d+) w=(\d+) h=(\d+)", 90, since=n1)
        check("查看器 surface 建好", mv is not None, mv.group(0) if mv else "")
        if not mv:
            raise RuntimeError("查看器没起来，后面的判据无从谈起")
        sid = int(mv.group(1))
        ww, wh = int(mv.group(2)), int(mv.group(3))

        # ---- A. Dock 运行项登记 ----
        madd = vm.wait_re(r"\[DOCK64\] ext-add surf=(\d+) win=(\d+) title=(\S+)", 30, since=n1)
        check("A: Dock 登记运行项（[DOCK64] ext-add）",
              madd is not None and int(madd.group(1)) == sid, madd.group(0) if madd else "")
        win_id = int(madd.group(2)) if madd else -1
        check("A: 外部窗口 win_id = 10000 + surf", win_id == 10000 + sid, "win=%d" % win_id)

        mitem = vm.wait_re(r"\[DOCK64\] item idx=(\d+)[^\n]*win=%d\b" % win_id, 20, since=n1)
        check("A: Dock 列表出现该运行项（item kind=running）", mitem is not None,
              mitem.group(0) if mitem else "")
        item_idx = int(mitem.group(1)) if mitem else -1
        # 从 item 行里取点击坐标（cx/cy）
        mcx = vm.wait_re(r"\[DOCK64\] item idx=%d[^\n]*cx=(\d+) cy=(\d+)" % item_idx, 5, since=n1) if item_idx >= 0 else None
        check("A: item 行给了点击坐标（cx/cy）", mcx is not None, mcx.group(0) if mcx else "")
        icx, icy = (int(mcx.group(1)), int(mcx.group(2))) if mcx else (-1, -1)

        # ---- A2. 窗口矩形（合成器自己打的）----
        mrect = vm.wait_re(r"\[WM\] surf id=%d [^\n]*rect=(-?\d+),(-?\d+),(\d+),(\d+)" % sid, 20, since=n1)
        check("A: 合成器报了窗口矩形（[WM] surf rect=）", mrect is not None, mrect.group(0) if mrect else "")
        wx, wy, fw, fh = (int(mrect.group(1)), int(mrect.group(2)),
                          int(mrect.group(3)), int(mrect.group(4))) if mrect else (0, 0, 0, 0)
        print("    窗口矩形 = %d,%d %dx%d（客户区 %dx%d）" % (wx, wy, fw, fh, ww, wh))

        time.sleep(2.0)
        w, h, BEFORE = park_and_shot("before.ppm")

        # ---- B. 活动高亮（胶囊 + 像素）----
        mact = vm.wait_re(r"\[DOCK64\] extactive idx=(\d+) surf=(\d+) x=(\d+) y=(\d+) w=(\d+) h=(\d+) color=#(\w+)",
                          20, since=n1)
        check("B: 活动窗口高亮（[DOCK64] extactive）",
              mact is not None and int(mact.group(2)) == sid, mact.group(0) if mact else "")
        if mact:
            ax, ay, aw, ah = int(mact.group(3)), int(mact.group(4)), int(mact.group(5)), int(mact.group(6))
            want = (int(mact.group(7)[0:2], 16), int(mact.group(7)[2:4], 16), int(mact.group(7)[4:6], 16))
            got = V.count(BEFORE, w, h, (ax, ay, ax + aw, ay + ah), lambda r, g, b: near((r, g, b), want))
            check("B: 胶囊像素真的是强调色（w=%d h=%d）" % (aw, ah), got >= aw * ah // 2,
                  "accent=%d/%d" % (got, aw * ah))
            check("B: 胶囊比『运行小圆点』宽（>=3x 图标宽 40%%）", aw >= 24, "w=%d" % aw)

        # ---- B2. ★ v0.11.5 补：Dock 运行项画的是**应用真图标**（brand #8FD6FF），不是默认箭头 ----
        # 为什么补这一条：v0.11.4 给外部窗口加了应用身份（32 号 set_app -> kind 74）与 Dock 图标通道
        #   （dock64_ext_icon64），但验收只钉了"运行项存在/胶囊/最小化/恢复"，**图标本身没有像素判据**——
        #   "图标全换了吗"这个问题，只有这一条能回答。
        if icx >= 0 and icy >= 0:
            bp = V.count(BEFORE, w, h, (icx - 23, icy - 23, icx + 23, icy + 23),
                         lambda r, g, b: (b - r) >= 40 and b >= 0xE0)          # 品牌蓝：b-r=0x70（Dock 底 b-r≈6）
            check("B2: Dock 运行项画的是应用真图标（品牌蓝像素 >= 40）", bp >= 40,
                  "brand=%d（图标中心 %d,%d）" % (bp, icx, icy))
            mdicon = vm.wait_re(r"\[DOCK64\] ext-icon surf=\d+ kind=74 name=image-viewer", 5, since=n1)
            check("B2: 外部窗口图标登记（[DOCK64] ext-icon … kind=74 name=image-viewer）",
                  mdicon is not None, mdicon.group(0) if mdicon else "（缺行）")

        # ---- C. 最小化（点最小按钮 = 三槽里的第一槽）----
        nmin = vm.n()
        min_cx, min_cy = wx + fw - 1 - 30 * 3 + 15, wy + 12
        V.park_cursor(mon)
        V.move_px(mon, 0, +1, min_cx)
        V.move_px(mon, 1, +1, min_cy)
        time.sleep(0.4)
        V.single_click(mon)
        mbtn = vm.wait_re(r"\[WM\] btn id=%d which=min action=minimize rc=0" % sid, 15, since=nmin)
        check("C: 点最小按钮 -> 合成器 minimize", mbtn is not None, mbtn.group(0) if mbtn else "")
        mctl = vm.wait_re(r"\[WL64\] ctl id=%d [^\n]*action=minimize[^\n]*minimized=1" % sid, 15, since=nmin)
        check("C: 30 号 ctl action=minimize（内核 surface 表是唯一真源）", mctl is not None,
              mctl.group(0) if mctl else "")
        mst = vm.wait_re(r"\[DOCK64\] ext-state surf=%d minimized=1" % sid, 10, since=nmin)
        check("C: Dock 收到 minimized=1", mst is not None, mst.group(0) if mst else "")
        mbar = vm.wait_re(r"\[DOCK64\] minbar idx=(\d+)[^\n]*clickable=0", 10, since=nmin)
        check("C: Dock 该项画最小化横杠（minbar）", mbar is not None, mbar.group(0) if mbar else "")

        time.sleep(1.2)
        w, h, MIN = park_and_shot("minimized.ppm")
        # 窗口矩形内：图片红/蓝、标题带、旧自绘色 都必须 0
        red = V.count(MIN, w, h, (wx, wy, wx + fw, wy + fh), V.is_red)
        blue = V.count(MIN, w, h, (wx, wy, wx + fw, wy + fh), V.is_blue)
        band = V.count(MIN, w, h, (wx, wy, wx + fw, wy + 25),
                       lambda r, g, b: near((r, g, b), TITLE_BG))
        check("C: 窗口矩形里一个图片像素都不剩（红/蓝/标题带 = 0）",
              red == 0 and blue == 0 and band == 0, "red=%d blue=%d band=%d" % (red, blue, band))

        # ---- D. 点 Dock 项恢复（几何逐像素还原）----
        nres = vm.n()
        V.park_cursor(mon)
        V.move_px(mon, 0, +1, icx)
        V.move_px(mon, 1, +1, icy)
        time.sleep(0.4)
        V.single_click(mon)
        mpr = vm.wait_re(r"\[DOCK64\] press idx=(\d+)[^\n]*action=ext-restore[^\n]*surf=%d" % sid, 15, since=nres)
        check("D: 点 Dock 项 -> action=ext-restore", mpr is not None, mpr.group(0) if mpr else "")
        mrctl = vm.wait_re(r"\[WL64\] ctl id=%d [^\n]*action=restore[^\n]*minimized=0" % sid, 15, since=nres)
        check("D: 30 号 ctl action=restore（minimized=0）", mrctl is not None, mrctl.group(0) if mrctl else "")

        time.sleep(1.5)
        w2, h2, AFTER = park_and_shot("restored.ppm")
        rect = (wx, wy, wx + fw, wy + fh)
        diff = 0
        for y in range(rect[1], rect[3]):
            for x in range(rect[0], rect[2]):
                o = (y * w + x) * 3
                if BEFORE[o:o + 3] != AFTER[o:o + 3]:
                    diff += 1
        check("D: 恢复后窗口矩形与最小化前**逐字节相同**", diff == 0,
              "diff=%d / %d px" % (diff, fw * fh))

        # ---- E. 关窗（点关闭按钮）-> Dock 项退出运行态 ----
        nclose = vm.n()
        close_cx, close_cy = wx + fw - 1 - 30 + 15, wy + 12
        V.park_cursor(mon)
        V.move_px(mon, 0, +1, close_cx)
        V.move_px(mon, 1, +1, close_cy)
        time.sleep(0.4)
        V.single_click(mon)
        mcl = vm.wait_re(r"\[WL64\] ctl id=%d [^\n]*action=close[^\n]*notify=1[^\n]*destroyed=1" % sid, 15, since=nclose)
        check("E: 30 号 ctl action=close（先投事件再销毁）", mcl is not None, mcl.group(0) if mcl else "")
        mdel = vm.wait_re(r"\[DOCK64\] ext-del surf=%d" % sid, 15, since=nclose)
        check("E: Dock 运行项退出运行态（[DOCK64] ext-del）", mdel is not None, mdel.group(0) if mdel else "")
        mdone = vm.wait_re(r"\[VIEWER\] done frames=\d+ reason=close", 20, since=nclose)
        check("E: 客户端收到 CLOSE 并退出（reason=close）", mdone is not None, mdone.group(0) if mdone else "")

        time.sleep(1.2)
        w3, h3, GONE = park_and_shot("closed.ppm")
        red2 = V.count(GONE, w3, h3, (wx, wy, wx + fw, wy + fh), V.is_red)
        blue2 = V.count(GONE, w3, h3, (wx, wy, wx + fw, wy + fh), V.is_blue)
        check("E: 关窗后窗口矩形回到桌面（红/蓝 = 0）", red2 == 0 and blue2 == 0,
              "red=%d blue=%d" % (red2, blue2))

        print("=== 串口尾 20 行 ===")
        print("\n".join(vm.log().splitlines()[-20:]))
    except Exception as e:                                      # noqa: BLE001
        print("  [FAIL] 夹具异常：%r" % (e,))
        ok = False
    finally:
        if vm:
            vm.stop()
            if not args.keep:
                shutil.rmtree(tmp, ignore_errors=True)

    n_ok = sum(1 for _, c in checks if c)
    print("=== RESULT: %d/%d PASS ===" % (n_ok, len(checks)))
    return 0 if (ok and checks) else 1


if __name__ == "__main__":
    sys.exit(main())
