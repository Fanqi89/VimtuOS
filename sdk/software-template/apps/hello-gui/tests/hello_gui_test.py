#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/apps/hello-gui/tests/hello_gui_test.py - hello-gui 的端到端验收（模板）

跑法（Windows 原生 Python；**同一时刻只跑一个 QEMU 重活**）：
    py -3 sdk/software-template/apps/hello-gui/tests/hello_gui_test.py [--img 现成夹具盘] [--keep]

断言（**一条都不许削弱**）：
  ① 卷里有 /bin/hello-gui.elf + /lib/wm.elf（Ring 3 合成器）+ 四份字库 + 外置图标；
  ② 登录进桌面 -> 开终端 -> `elfrun /bin/hello-gui.elf`；
  ③ 字库**从系统卷读**（[HELLO-GUI] font … ok=1 bytes>0 upem>0）；
  ④ surface + shm 真建起来了（[HELLO-GUI] surf id>0 … va!=0；内核 [WL64] surface create 有对应行）；
  ⑤ 帧循环提交成功（frame f=0/1/2 rc=0）且画布有墨（ink>0）；
  ⑥ **上屏像素证据**：抓屏里"纯红标记块"（0xFFFF0000，24x24=576 px）按容差计数 >= 200；
  ⑦ 外置图标被内核加载：串口里 `[ICON64] load … src=vfs ok=1` 至少 5 条（本轮新增图标装进系统卷的机制证据）；
  ⑧ 收尾：destroy rc=0 + done；
  ⑨ 全程无 PANIC / TRIPLE FAULT。

退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题。
"""
import argparse
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
ROOT = os.path.dirname(os.path.dirname(SDK))
sys.path.insert(0, os.path.join(SDK, "tools"))
import sdk_qemu as sq                                            # noqa: E402
ELF = os.path.join(ROOT, "build64", "sdk", "hello-gui.elf")
WM_ELF = os.path.join(ROOT, "build64", "wm.elf")
FIXTURE = os.path.join(ROOT, "build64", "sdk", "hello_gui_test.img")
FONTS = (
    ("/Fonts/NotoSans-Regular.ttf", os.path.join(ROOT, "build", "font_bahnschrift.ttf")),
    ("/Fonts-open/NotoSansSC-Regular.ttf", os.path.join(ROOT, "build", "font_simhei.ttf")),
    ("/Fonts-open/SarasaMonoSC-Regular.ttf", os.path.join(ROOT, "build", "font_mono.ttf")),
    ("/Fonts-open/unifont-14.0.01.ttf", os.path.join(ROOT, "build", "font_fallback.ttf")),
)

FORBIDDEN = ("PANIC", "TRIPLE FAULT")
MARKER = (0xFF, 0x00, 0x00)


def ensure_artifacts():
    """缺产物就自己造（验收脚本要能**独立**跑起来）：hello-gui.elf 走 build.sh。"""
    import subprocess
    if not os.path.exists(ELF):
        print("   缺 %s -> 先跑 build.sh" % ELF)
        r = subprocess.run(["bash", os.path.join(os.path.dirname(HERE), "build.sh")],
                           cwd=ROOT, capture_output=True)
        if r.returncode != 0 or not os.path.exists(ELF):
            raise RuntimeError("构建失败：%s" % r.stderr.decode("utf-8", "replace")[-300:])


def prepare(verbose=True):
    ensure_artifacts()
    srcs = [(ELF, "/bin/hello-gui.elf", 0o755), (WM_ELF, "/lib/wm.elf", 0o755)]
    for vpath, host in FONTS:
        srcs.append((host, vpath, 0o644))
    # ★ 只装内核**实际请求**的两档（system@24 / apps@48）：把启动期 PNG 解码量压到最小，
    #   避免 QEMU + 并发负载把内核看门狗踩响（[PANIC64] stop=WATCHDOG_TIMEOUT）。
    #   全尺寸（16/24/32/48）装卷的证据走 tools/iconpack_ext.py 那一步（默认 4 档）。
    srcs += sq.icon_srcs(verbose=verbose, sizes=(24, 48))
    return sq.build_fixture(FIXTURE, srcs, verbose=verbose)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=None)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))
        return bool(cond)

    print("=== 0) 夹具盘 ===")
    for p in (WM_ELF, os.path.join(ROOT, "build64", "system.img")):
        if not os.path.exists(p):
            sys.stderr.write("缺少 %s（先跑 bash build64.sh）\n" % p)
            return 2
    img = args.img
    if not img:
        try:
            img = prepare()                   # 内部会在缺 hello-gui.elf 时自动 build.sh
        except Exception as e:
            sys.stderr.write("夹具盘准备失败：%s\n" % e)
            return 2
    check("夹具盘就绪", os.path.exists(img), img)

    qemu = sq.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    port = args.port or sq.free_port()
    tmp, serial = sq.temp_serial("vimtu_sdk_gui_")
    vm = None
    try:
        vm = sq.QemuVm(qemu, img, port, serial, name="vimtu-sdk-gui")
        mon = sq.Monitor(port)
        print("=== 1) 引导 + 登录 + 开终端 ===")
        check("内核起来（[GUI64] ready）", sq.login_desktop(mon, vm))
        check("开始菜单 -> 终端（[APP] term opened）", sq.open_terminal(mon, vm))

        print("=== 2) 跑 /bin/hello-gui.elf（合成器 + shm surface + 画字）===")
        sq.type_line(mon, "elfrun /bin/hello-gui.elf", per_key=0.11)
        m = vm.wait_re(r"\[HELLO-GUI\] ver=1 pid=(\d+)", 60)
        check("[HELLO-GUI] 起来了（真进程）", m is not None, m.group(0) if m else "没等到")
        pid = int(m.group(1)) if m else 0

        mf = vm.wait_re(r"\[HELLO-GUI\] font path=(\S+) ok=(\d) bytes=(\d+) upem=(\d+) nglyph=(\d+)", 60)
        check("字库从系统卷读成功（ok=1 bytes>0）",
              mf is not None and mf.group(2) == "1" and int(mf.group(3)) > 0,
              mf.group(0) if mf else "没等到")
        ms = vm.wait_re(r"\[HELLO-GUI\] surf id=(\d+) w=(\d+) h=(\d+) shm=(\d+) va=0x([0-9a-f]+)", 30)
        check("surface + shm 建好（id>0 且 va!=0）",
              ms is not None and int(ms.group(1)) > 0 and int(ms.group(5), 16) != 0,
              ms.group(0) if ms else "没等到")
        mt = vm.wait_re(r"\[HELLO-GUI\] text size=18 glyphs=(\d+) width=(\d+) lh=(\d+) ink=(\d+)", 30)
        check("画字有效（glyphs>0 width>0 ink>0）",
              mt is not None and int(mt.group(1)) > 0 and int(mt.group(2)) > 0 and int(mt.group(4)) > 0,
              mt.group(0) if mt else "没等到")
        m0 = vm.wait_re(r"\[HELLO-GUI\] frame f=0 rc=(\d+) ink=(\d+)", 30)
        check("第 0 帧提交成功（rc=0 且 ink>0）",
              m0 is not None and m0.group(1) == "0" and int(m0.group(2)) > 0,
              m0.group(0) if m0 else "没等到")
        check("内核侧有对应的 surface 行（[WL64] surface create）",
              vm.wait_re(r"\[WL64\] surface create id=\d+ w=256 h=56", 20) is not None)

        print("=== 3) 上屏像素（抓屏找纯红标记块）===")
        # 合成器从 fork 到注册实测 1.5~2 s，且表面只在 commit 后的一段窗口里在屏上（程序跑完会 destroy），
        # 所以这里**连拍 5 张**（间隔 0.5 s）取最大计数 —— 判据不降级（仍然要求标记块真的出现在屏上），
        # 只是不受"抓屏恰好落在两帧之间"的影响。每一张的实测值都打出来。
        ppm = os.path.join(tmp, "gui.ppm")
        got = False
        red = -1
        shots = []
        for k in range(5):
            if mon.shot(ppm):
                got = True
                w, h, px = sq.read_ppm(ppm)
                n = sq.count_color(px, w, h, MARKER, tol=16)
                shots.append(n)
                red = max(red, n)
            if red >= 200:
                break
            time.sleep(0.5)
        print("   抓屏 %d 张：纯红像素（容差 16）= %s（取最大 %d）" % (len(shots), shots, red))
        check("抓屏成功", got)
        check("画面上有标记块像素（>=200，说明 surface 真的合成上屏）", red >= 200, "red=%d" % red)

        check("进程收尾（destroy + done）", vm.wait("[HELLO-GUI] done", 60))
        log = vm.log()
        mr = re.findall(r"\[HELLO-GUI\] destroy rc=(\d+)", log)
        check("surface 销毁成功（rc=0）", bool(mr) and mr[-1] == "0", "rc=%s" % (mr[-1] if mr else "?"))
        mf2 = re.findall(r"\[HELLO-GUI\] frame f=(\d+) rc=(\d+) ink=(\d+)", log)
        check("三帧都提交成功（rc 全 0）",
              len(mf2) >= 3 and all(x[1] == "0" for x in mf2[:3]),
              "帧行 %d 条" % len(mf2))
        # 注意路径与 src= 之间还有 size=<n>：`[ICON64] load kind=x path=/icons/…@24.png size=24 src=vfs ok=1`
        ic = re.findall(r"\[ICON64\] load kind=(\S+) path=(/icons/\S+?) size=\d+ src=vfs ok=1", log)
        check("外置图标被内核加载（[ICON64] load … src=vfs ok=1 至少 5 条）", len(ic) >= 5,
              "src=vfs 加载 %d 条，例如 %s" % (len(ic), ic[0][0] + " -> " + ic[0][1] if ic else "无"))
        check("全程无 PANIC / TRIPLE FAULT", not any(f in log for f in FORBIDDEN))
    finally:
        if vm:
            vm.close()
        print("   串口日志：%s" % serial)
        print("=== 结论：%s（%d/%d）==="
              % ("PASS" if ok else "FAIL", sum(1 for _n, c in checks if c), len(checks)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
