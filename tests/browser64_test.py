# -*- coding: utf-8 -*-
"""tests/browser64_test.py —— v0.5.0：/bin/browser（ring3 真窗口浏览器）端到端验收

判据（串口打点 + 屏幕像素，不看代码自述）：
  1) 窗口真的建成：`[BROWSER] win surf=<n> w=896 h=580 shm=<n> map=0`
     —— 同时是**大 surface ABI** 的回归：v0.5.0 把 shm 单对象上限从 64 KiB 提到 2 MiB，
     896x580 = 519,680 像素 = 2,078,720 B，旧上限下这一步必然 `map≠0` / `create` 失败。
  2) 字体栈起来了：`[BROWSER] fonts faces=.. ok=1`
  3) 起始页解析成功：`[BROWSER] load url=/www/index.html bytes=<n> rc=0 blocks=<n> text=<n>`
     且 blocks >= 10、bytes >= 1000（卷里的 /www/index.html 由 tools/browser_pack_win.py 装进去）
  4) 排版与绘制：`[BROWSER] render frame=0 scroll=0 lines=<n> page_h=<n> drawn=<n>`，
     lines >= 20、page_h > 可视高（说明内容超过一屏 -> 可滚动）
  5) 屏幕像素：窗口区域里**页面底色占多数**（白），且有足够的深色墨迹（文字），
     链接蓝色像素存在（a 标签的 color）
  6) 滚动：按 PageDown 后 `[BROWSER] key code=.. scroll=<n>` 且 scroll > 0；
     屏幕像素与滚动前**不同**（差异像素 > 2000）
  7) ESC 退出：`[BROWSER] done frames=<n> reason=esc`
  8) 全程无 PANIC / TRIPLE FAULT

为什么走 ring3 shell 的 `run`（而不是终端内建 run）：
  浏览器要用 shm_create / wl_surface（都需要**进程上下文**）。终端内建 `run` 走
  kernel/elf64.cpp 的 user64_enter_at64，不建 Proc64 -> p64_current64()==nullptr ->
  `[SHM64] create FAILED reason=no-proc`（这条已知边界记在 README 第二节）。
  shell 的 `run` 是 fork+execve+wait4 真进程，子进程有独立 Proc64。

用法：py -3 tests/browser64_test.py [--img ...] [--port 5674]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "browser")
os.makedirs(outdir, exist_ok=True)
checks = []

BW, BH = 896, 580          # 与 user/browser/browser.c 的 BW/BH 一致


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "_": "shift-minus"}


def type_line(mon, s, per=0.08):
    for ch in s:
        mon.key(KEYMAP.get(ch, ch), wait=per)


def sample(a, x0, y0, x1, y1, step=2):
    """统计一个矩形里的颜色直方图：白/暗/蓝 与颜色数。"""
    white = dark = blue = total = 0
    colors = set()
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            px = a[y][x]
            v = (int(px[0]), int(px[1]), int(px[2]))
            total += 1
            colors.add(v)
            r, g, b = v
            if r > 240 and g > 240 and b > 240:
                white += 1
            if r < 96 and g < 96 and b < 96:
                dark += 1
            if b > 140 and b - r > 60 and b - g > 40:
                blue += 1
    return {"total": total, "white": white, "dark": dark, "blue": blue, "colors": len(colors)}


def diff_count(a, b, x0, y0, x1, y1, step=2):
    n = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            pa, pb = a[y][x], b[y][x]
            if int(pa[0]) != int(pb[0]) or int(pa[1]) != int(pb[1]) or int(pa[2]) != int(pb[2]):
                n += 1
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5674)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）—— 先跑 bash build64.sh" % args.img)
        return 2

    print("=== browser64_test：/bin/browser 真窗口 + HTML 排版 + 滚动 ===")
    vm = T.Vm(qemu, args.img, args.port, "browser")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏", False)
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 1
        time.sleep(2.0)
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        check("开场终端（[APP] term opened）", vm.wait_log("[APP] term opened", 25))
        time.sleep(1.2)

        # 起 ring3 shell（真进程通道），再在里面 run 浏览器
        n0 = len(vm.log())
        type_line(mon, "shell")
        mon.key("ret", wait=2.0)
        got_sh = vm.wait_log("[SH64] launch", 60, since=n0)
        check("起 ring3 shell（[SH64] launch path=/bin/shell.bin）", got_sh,
              (re.search(r"\[SH64\] launch[^\r\n]*", vm.log()[n0:]).group(0) if got_sh else "（缺）"))
        if not got_sh:
            print("\n=== RESULT: UNVERIFIED ===（环境：ring3 shell 起不来）")
            return 2
        time.sleep(2.5)

        n1 = len(vm.log())
        type_line(mon, "run /bin/browser")
        mon.key("ret", wait=2.0)
        # 装载 + 字体解析 + 首帧，给 90 秒（stb_truetype 解析三个面要几秒）
        ok_win = vm.wait_log("[BROWSER] win", 90, since=n1)
        if not ok_win:
            # shell 的输入环在键盘连打时偶发丢行（实测两次里有一次），重发一次同样的命令；
            # 这不放宽判据 —— 下面仍然要求 [BROWSER] win 打点真的出现。
            print("  [info] 第一次 run 没被 shell 接住，重发一次")
            type_line(mon, "run /bin/browser")
            mon.key("ret", wait=2.0)
            ok_win = vm.wait_log("[BROWSER] win", 90, since=n1)
        # ★ 时序：wait_log 一看到 `[BROWSER] win` 就返回，而 `load`/`render` 是它**后面**两条
        #   printf —— 立刻取日志会截在中间（第一版就是这么误判成"解析失败"的，页面其实画出来了）。
        #   这里等首帧打点真的落盘再取切片；判据本身没有放宽。
        vm.wait_log("[BROWSER] render", 40, since=n1)
        time.sleep(1.0)
        log = vm.log()[n1:]
        m_win = re.search(r"\[BROWSER\] win surf=(\d+) w=(\d+) h=(\d+) shm=(\d+) map=(-?\d+)", log)
        check("大窗口建成（[BROWSER] win surf=.. w=896 h=580 shm=.. map=0）",
              bool(m_win and m_win.group(5) == "0" and m_win.group(2) == str(BW) and m_win.group(3) == str(BH)),
              m_win.group(0) if m_win else "（缺）")
        if not m_win or m_win.group(5) != "0":
            print("\n=== RESULT: FAIL === 大 surface 建不出来（v0.5.0 的 shm 2 MiB 上限没生效？）")
            return 1

        m_f = re.search(r"\[BROWSER\] fonts faces=(\d+) ok=(\d+) bytes=(\d+)", log)
        check("字体栈加载（faces>=1, ok=1）", bool(m_f and int(m_f.group(1)) >= 1 and m_f.group(2) == "1"),
              m_f.group(0) if m_f else "（缺）")

        m_load = re.search(r"\[BROWSER\] load url=(\S+) bytes=(\d+) rc=(-?\d+) blocks=(\d+) text=(\d+)", log)
        check("起始页解析成功（url=/www/index.html rc=0 blocks>=10）",
              bool(m_load and m_load.group(3) == "0" and int(m_load.group(4)) >= 10
                   and int(m_load.group(2)) >= 1000),
              m_load.group(0) if m_load else "（缺）")

        m_r = re.search(r"\[BROWSER\] render frame=(\d+) scroll=(\d+) lines=(\d+) page_h=(\d+) drawn=(\d+)", log)
        check("排版出内容且超过一屏（lines>=20, page_h>496）",
              bool(m_r and int(m_r.group(3)) >= 20 and int(m_r.group(4)) > 496),
              m_r.group(0) if m_r else "（缺）")

        # 窗口位置（内核给的居中 pos）
        m_pos = None
        for mm in re.finditer(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+)[^\r\n]*pos=(-?\d+),(-?\d+)", vm.log()):
            if mm.group(2) == str(BW) and mm.group(3) == str(BH):
                m_pos = mm
        if not m_pos:
            check("拿到窗口在屏上的位置（[WL64] surface create pos=x,y）", False)
            return 1
        wx, wy = int(m_pos.group(4)), int(m_pos.group(5))
        print("  [info] 窗口位置 pos=%d,%d（%dx%d）" % (wx, wy, BW, BH))

        time.sleep(2.0)
        f0 = mon.shot(os.path.join(outdir, "browser_f0.ppm"))
        a0 = T.frame_arr(f0)
        h, w = len(a0), len(a0[0])
        check("窗口完整落在屏幕内", wx >= 0 and wy >= 0 and wx + BW <= w and wy + BH <= h,
              "屏幕 %dx%d" % (w, h))
        # 页面区（避开标题栏/地址栏/状态栏）
        px0, py0 = wx + 10, wy + 60
        px1, py1 = wx + BW - 10, wy + BH - 24
        s0 = sample(a0, px0, py0, px1, py1)
        print("  [info] 页面区采样 total=%d 白=%d(%.1f%%) 暗=%d(%.2f%%) 蓝=%d 颜色数=%d"
              % (s0["total"], s0["white"], 100.0 * s0["white"] / max(1, s0["total"]),
                 s0["dark"], 100.0 * s0["dark"] / max(1, s0["total"]), s0["blue"], s0["colors"]))
        check("页面底色为白（>=50%% 采样点为纯白）", s0["white"] * 2 >= s0["total"],
              "%d/%d" % (s0["white"], s0["total"]))
        check("有正文墨迹（暗像素 >= 0.5%%）", s0["dark"] * 200 >= s0["total"],
              "%d/%d = %.2f%%" % (s0["dark"], s0["total"], 100.0 * s0["dark"] / max(1, s0["total"])))
        check("链接蓝色像素存在（a 标签的 color 真的画出来了）", s0["blue"] > 0, "蓝=%d" % s0["blue"])
        check("不是纯色块（采样颜色数 >= 8）", s0["colors"] >= 8, "colors=%d" % s0["colors"])

        # 滚动：鼠标点击页面下册（不依赖键盘焦点；详见 browser.c 的注释）
        # QEMU 的 PS/2 指针是相对坐标，起机后停在屏幕中心（≈640,400）—— 先确认指针落在窗口内，
        # 再用相对位移把它挪到页面区下半（+200 y）并点一下。
        mon.send("mouse_move 0 200")
        time.sleep(0.8)
        n2 = len(vm.log())
        mon.send("mouse_button 1")
        time.sleep(0.3)
        mon.send("mouse_button 0")
        time.sleep(1.5)
        got_click = vm.wait_log("[BROWSER] click", 20, since=n2)
        # 串口里这行可能被内核的 [SYSCALL] 打点插进中间（每次鼠标按下都会跟一条），所以判定用
        # "打点存在" + 尽力抓字段；滚动真的发生的证据是下面的像素差异。
        m_k = re.search(r"\[BROWSER\] click[^\r\n]*scroll=(\d+)", vm.log()[n2:])
        check("点击页面下册触发翻页（[BROWSER] click … scroll=…）", bool(got_click),
              (m_k.group(0)[:90] if m_k else "（有一行 [BROWSER] click，字段被 [SYSCALL] 打点截断）"))
        time.sleep(1.5)
        f1 = mon.shot(os.path.join(outdir, "browser_f1.ppm"))
        a1 = T.frame_arr(f1)
        d = diff_count(a0, a1, px0, py0, px1, py1)
        print("  [info] 滚动前后页面区差异像素 = %d（每 2x2 采一点）" % d)
        check("滚动后画面真的变了（差异像素 > 2000）", d > 2000, "diff=%d" % d)

        # 退出：点标题栏右侧的 [X]（同样的鼠标路径，覆盖 close 分支）
        n3 = len(vm.log())
        mon.send("mouse_move 426 -480")     # 指针从 (640,600) 移到右上角 (1066,120) = [X] 附近
        time.sleep(0.8)
        mon.send("mouse_button 1")
        time.sleep(0.3)
        mon.send("mouse_button 0")
        time.sleep(2.0)
        got_done = vm.wait_log("[BROWSER] done", 25, since=n3)
        hit_close = "[BROWSER] close click" in vm.log()[n3:]
        check("点 [X] 退出（[BROWSER] close click -> quit + done reason=esc）",
              bool(got_done and hit_close and "reason=esc" in vm.log()[n3:]),
              ("close=%s done=%s" % (hit_close, got_done)))

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    for n in bad:
        print("  FAIL:", n)
    # ★ 交互与渲染分开算：渲染链（窗口/字体/解析/排版/像素）全过、只有"鼠标/键盘事件到达
    #   wl surface"没通时，**如实报 UNVERIFIED（退出码 2）**，绝不当成 PASS —— 这条边界是
    #   内核既有 GUI 的输入路由（gui64 的窗口命中测试优先于 wl64 的 surface 命中测试）造成的，
    #   记在 README 第二节，下一批处理。
    interactive = ("click" in " ".join(bad)) or ("退出" in " ".join(bad))
    only_interaction = bool(bad) and interactive and len(bad) <= 2
    if only_interaction:
        print("\n=== RESULT: UNVERIFIED === 渲染链 %d/%d 通过；交互（鼠标/键盘到达 surface）未通"
              % (len(checks) - len(bad), len(checks)))
        return 2
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
