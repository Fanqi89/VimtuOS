"""tests/logfollow64_test.py —— v0.5.9：终端与日志的**自动跟到底部**（auto-follow）。

用户要求：\"添加终端和日志的自动滚动\"。三条真缺陷（前两条在 kernel/console64.cpp，
第三条实测发现于 kernel/terminal64.cpp）：

  ① 回放按 CON64_SCROLL_BATCH(8) 行一批整体上移。最后不足一批的 1..7 行留在 g_scr64.pend_*
     里 —— 循环结束后**没有任何人把它们画上屏**，屏幕停的是\"倒数第 8~2 行\"，
     最新的几行永远不出现。
  ② 实时阶段（batch=1）若 pend_* 里还有行，`row = rows - dy + k`（dy=1）会把它们画到
     rows-1 之外 —— 越界/屏外。修法：实时行之前先收口。
  ③ 终端默认客户区 964x724、窗口 y=16 -> 窗口底 765，而 Dock 顶是 724 且**画在窗口上面**
     —— 最底两行（最新那条日志所在的行）被 Dock 盖住：日志明明滚到底了，用户却看不见。
     修法：新建窗口时按 Dock 顶边收一次行数（`[UI] term fit above dock`）。

修法（console64）：新增 con64_scr_flush64()（把待画行补画到屏底）；回放结束 / 实时 drain
之后 / 退出前都收口，并打出 `[CON64] follow ...` / `[CON64] follow live ...` 证据行。
终端侧：`dmesg` 打印完把\"光标落在哪一行\"固化成 `[TERM64] dmesg follow ...` 证据行。

判据（逐条可证伪；每条都给实测数字）：
  ① `[CON64] follow bottom=1 rows=R drawn=D newest_row=D-1 pending=0 flushed=F replayed=N`
     * D == min(N, R)、newest_row == D-1、pending == 0
     * F == (N - R) % 8（N > R 时）else 0 —— 修前这些行不上屏（F 恒 0、pending>0）
  ② `[CON64] follow live bottom=1 ... pending=0`
  ③ `[TERM64] dmesg follow bottom=1 lines=N rows=R cursor_row=R-1 newest_row=R-2`（N > R）
  ④ 屏幕像素：终端客户区**倒数第二个文本行带**有墨（最新一条日志真画出来了）；
     反向对照：`clear` 之后同一条带墨量为 0（证明指标不是恒真）
  ⑤ 客户区底边留在 Dock 之上（`[UI] term fit above dock ... win_bottom<=dock_top`）

用法：py -3 tests/logfollow64_test.py [--img build64/sysdisk.img] [--port 5696]
退出码：0 全过 / 1 有断言失败 / 2 环境问题（qemu / 镜像 / 没进桌面）。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                     # noqa: E402
import startmenu64_test as SM                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    import numpy as np
except ImportError:                                              # pragma: no cover
    print("需要 numpy（tests/ 其它脚本同样依赖）")
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DEF = os.path.join(ROOT, "build64", "sysdisk.img")
PORT = 5696
BATCH = 8                     # kernel/console64.cpp 的 CON64_SCROLL_BATCH
PAD_Y = 4                     # 终端客户区顶部内边距（TERM_PAD_Y）

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def find_tile(vm, mon, want, tries=4):
    """开始菜单里找一格（与 clicklaunch64_test 同口径的打点解析）。"""
    for _ in range(tries):
        mon.key("meta_l", 1.6)
        time.sleep(1.2)
        for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(-?\d+) name=(\S+) en=(.+?) "
                              r"x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log()):
            if mm.group(3) == want:
                return mm
        mon.key("esc", 0.8)
        time.sleep(1.0)
    return None


def read_frame(mon, path):
    if not mon.shot(path):
        return None
    w, h, data = SM.read_ppm(path)
    return np.frombuffer(data, dtype=np.uint8).reshape(h, w, 3)


def longest_run(flags):
    """最长连续 True 段 -> (start, end)；没有则 None。"""
    best = None
    s = None
    for i, f in enumerate(flags):
        if f and s is None:
            s = i
        elif not f and s is not None:
            if best is None or (i - 1 - s) > (best[1] - best[0]):
                best = (s, i - 1)
            s = None
    if s is not None and (best is None or (len(flags) - 1 - s) > (best[1] - best[0])):
        best = (s, len(flags) - 1)
    return best


def find_client(a):
    """找终端客户区：**纯黑**（sum(rgb) <= 12）矩形。

    为什么用"纯黑"：桌面壁纸是亮色（实测 1100,400 = 237,242,250），Dock 是深色但**不是纯黑**，
    只有终端客户区（TERM_BG = #000000）是纯黑 —— 于是"纯黑占多数的列/行"的最小~最大范围
    就是客户区（实测 x=17..980、y=41..710，高 670 = 布局打点的 client_h，一个像素不差）。
    返回 (x0, y0, x1, y1) 或 None。
    """
    h, w, _ = a.shape
    pure = (a.sum(axis=2) <= 12)
    colcnt = pure.sum(axis=0)
    xs = [x for x in range(w) if colcnt[x] > h * 0.3]
    if not xs:
        return None
    x0, x1 = xs[0], xs[-1]
    sub = pure[:, (x0 + 1):x1]
    width = sub.shape[1]
    if width <= 0:
        return None
    rc = sub.sum(axis=1)
    ys = [y for y in range(h) if rc[y] > width * 0.3]      # 0.3 阈值：正文行有白字（约占 10%~20%）
    if not ys:
        return None
    return (x0, ys[0], x1, ys[-1])


def band_ink(a, x0, y0, x1, idx, ch):
    """终端网格第 idx 行带里的\"亮像素\"数（亮 = 三通道都 > 140，即终端正文纯白）。"""
    ya = y0 + PAD_Y + idx * ch
    yb = ya + ch
    xa, xb = x0 + 2, x1 - 1
    if yb > a.shape[0] or xb <= xa:
        return -1
    reg = a[ya:yb, xa:xb]
    if reg.size == 0:
        return -1
    return int((reg.min(axis=2) > 140).sum())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=IMG_DEF)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu)
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）" % args.img)
        return 2

    tmp = os.path.join(os.environ.get("PI_SCRATCH_DIR") or os.path.dirname(args.img), "logfollow64")
    os.makedirs(tmp, exist_ok=True)

    print("=== logfollow64_test：终端与日志的自动跟到底部（img=%s）===" % args.img)
    vm = T.Vm(qemu, args.img, args.port, "logfollow")
    mon = SM.Monitor(args.port)
    cur = SM.Cursor()
    try:
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            print("跳过：没等到锁屏（环境问题）")
            return 2
        mon.key("ret", 1.4)
        mon.key("ret", 1.6)
        if not vm.wait_log(T.DESKTOP_READY, 200):
            print("跳过：没进桌面（环境问题）")
            return 2
        time.sleep(2.0)
        log = vm.log()

        # ---------------- ① 开机滚屏日志：回放收口 ----------------
        print("\n--- 1) 开机滚屏日志回放：最新的几行必须画到屏底 ---")
        mr = re.search(r"\[CON64\] replay lines=(\d+) dropped=(\d+) ms=(\d+)", log)
        mf = re.search(r"\[CON64\] follow bottom=(\d+) rows=(\d+) drawn=(\d+) newest_row=(\d+) "
                       r"pending=(\d+) flushed=(\d+) replayed=(\d+)", log)
        check("回放确实跑过（[CON64] replay lines=N）", mr is not None,
              (mr.group(0) if mr else "缺失"))
        check("有 auto-follow 证据行（[CON64] follow ...）", mf is not None,
              (mf.group(0) if mf else "缺失（镜像里没有这条打点）"))
        if mr and mf:
            rows = int(mf.group(2))
            drawn, newest = int(mf.group(3)), int(mf.group(4))
            pending, flushed = int(mf.group(5)), int(mf.group(6))
            replayed, n_draw = int(mr.group(1)), int(mf.group(7))
            check("bottom=1（内核自算：pending 清空 + 上屏行数 == min(回放, 行数)）",
                  mf.group(1) == "1", mf.group(0))
            check("回放行数与 replay 打点一致（%d == %d）" % (n_draw, replayed),
                  n_draw == replayed)
            check("上屏行数 == min(回放, 行数)（drawn=%d min=%d）"
                  % (drawn, min(replayed, rows)), drawn == min(replayed, rows))
            check("最新一行的网格行就是最后一行（newest_row=%d drawn-1=%d）"
                  % (newest, drawn - 1), newest == drawn - 1)
            check("待画队列已清空（pending=%d）" % pending, pending == 0)
            want_flushed = ((replayed - rows) % BATCH) if replayed > rows else 0
            check("收口补画的行数 == 修前被丢掉的那几行（flushed=%d 期望=%d）"
                  % (flushed, want_flushed), flushed == want_flushed,
                  "修前这里恒为 0，那几行永远不上屏")
        ml = re.search(r"\[CON64\] follow live bottom=(\d+) rows=(\d+) drawn=(\d+) "
                       r"pending=(\d+) flushed=(\d+)", log)
        check("实时阶段/退出前也收口（[CON64] follow live bottom=1）",
              ml is not None and ml.group(1) == "1",
              (ml.group(0) if ml else "缺失"))

        # ①b 收口自检（确定性覆盖"最后不足一批"这条路径：默认启动余数恰好是 0）
        msf = re.search(r"\[CON64\] flush selftest lines=(\d+) flushed=(\d+) pending=(\d+) "
                        r"last_row_matches=(\d+)", log)
        check("收口自检证据行存在（[CON64] flush selftest）", msf is not None,
              (msf.group(0) if msf else "缺失"))
        if msf:
            st_lines, st_flushed = int(msf.group(1)), int(msf.group(2))
            st_pending, st_match = int(msf.group(3)), int(msf.group(4))
            check("自检把 3 行经 pend->flush 真的补画上屏（lines=%d flushed=%d）"
                  % (st_lines, st_flushed), st_lines == 3 and st_flushed == 3)
            check("收口后待画队列清空（pending=%d）" % st_pending, st_pending == 0)
            check("屏上最后一行 == 最后塞进去的那一行（last_row_matches=%d）" % st_match,
                  st_match == 1, "逐字符核对 g_scr64.last_drawn")

        # ---------------- ③ 终端 dmesg：输出滚到底 ----------------
        print("\n--- 2) 终端 `dmesg`：输出必须滚到底（最新一行在屏底） ---")
        mt = find_tile(vm, mon, "终端")
        check("开始菜单里有\"终端\"格（tile 打点）", mt is not None)
        if not mt:
            return 1
        cx = int(mt.group(5)) + int(mt.group(7)) // 2
        cy = int(mt.group(6)) + int(mt.group(8)) // 2
        cur.goto(mon, cx, cy)
        cur.settle(mon)
        time.sleep(0.3)
        mon.click()
        check("终端窗口打开（[APP] term opened cols=）",
              vm.wait_log("[APP] term opened cols=", 30))

        for ch in "dmesg":
            mon.key(ch, 0.12)
        mon.key("ret", 1.0)
        got = False
        for _ in range(60):                       # dmesg 要打 200+ 行到终端，给足时间
            if "[TERM64] dmesg follow" in vm.log():
                got = True
                break
            time.sleep(0.5)
        log2 = vm.log()
        mterm = re.search(r"\[TERM64\] dmesg follow bottom=(\d+) lines=(\d+) rows=(\d+) "
                          r"cursor_row=(\d+) cursor_col=(\d+) newest_row=(\d+)", log2)
        check("dmesg 打出 auto-follow 证据行", got and mterm is not None,
              (mterm.group(0) if mterm else "缺失"))
        if not mterm:
            return 1
        t_bot = int(mterm.group(1))
        t_lines, t_rows = int(mterm.group(2)), int(mterm.group(3))
        t_cur, t_new = int(mterm.group(4)), int(mterm.group(6))
        check("dmesg 的行数超过一屏（lines=%d rows=%d）" % (t_lines, t_rows), t_lines > t_rows)
        check("bottom=1（真的滚到了底部）", t_bot == 1, mterm.group(0))
        check("光标停在最后一行（cursor_row=%d rows-1=%d）" % (t_cur, t_rows - 1),
              t_cur == t_rows - 1)
        check("最新一行在光标上方一行（newest_row=%d）" % t_new, t_new == t_cur - 1)

        # 客户区尺寸来自内核打点（不是猜的几何）
        mlay = None
        for mm in re.finditer(r"\[UI\] term layout client=(\d+)x(\d+) cols=(\d+) rows=(\d+)", log2):
            mlay = mm
        check("有终端布局打点（[UI] term layout client=WxH cols=C rows=R）", mlay is not None)
        if not mlay:
            return 1
        client_w, client_h, lay_rows = int(mlay.group(1)), int(mlay.group(2)), int(mlay.group(4))
        ch_h = (client_h - PAD_Y) // lay_rows if lay_rows > 0 else 0
        check("行高可由几何反推（client_h=%d rows=%d -> 行高=%d）"
              % (client_h, lay_rows, ch_h), ch_h > 0)
        check("布局行数与 dmesg 打点的 rows 一致（%d == %d）" % (lay_rows, t_rows),
              lay_rows == t_rows)

        # ---------------- ⑤ 客户区留在 Dock 之上 ----------------
        print("\n--- 3) 客户区必须留在 Dock 之上（Dock 画在窗口上面） ---")
        mdock = re.search(r"\[DOCK64\] geom x=(-?\d+) y=(-?\d+)", log2)
        dock_top = int(mdock.group(2)) if mdock else -1
        check("有 Dock 几何打点（[DOCK64] geom y=）", mdock is not None,
              (mdock.group(0) if mdock else "缺失"))
        mfit = re.search(r"\[UI\] term fit above dock client_h=(\d+) rows=(\d+) "
                         r"win_bottom=(\d+) dock_top=(\d+)", log2)
        check("终端按 Dock 顶边收过高度（[UI] term fit above dock）", mfit is not None,
              (mfit.group(0) if mfit else "缺失"))
        if mfit:
            win_bottom, fit_dock = int(mfit.group(3)), int(mfit.group(4))
            check("窗口底边在 Dock 顶边之上（win_bottom=%d dock_top=%d）"
                  % (win_bottom, fit_dock), win_bottom <= fit_dock)
        if dock_top > 0:
            # 窗口 y=16 + 标题栏 25（实测）-> 客户区顶 = 41（下面还会用像素再确认）
            check("客户区底边留在 Dock 之上（16+25+%d-1 < %d）" % (client_h, dock_top),
                  16 + 25 + client_h - 1 < dock_top)

        # ---------------- ④ 屏幕像素交叉验证 + 反向对照 ----------------
        print("\n--- 4) 屏幕像素：最新一行真的画在屏底（含 clear 反向对照） ---")
        time.sleep(1.0)
        a = read_frame(mon, os.path.join(tmp, "dmesg.ppm"))
        check("dmesg 后 screendump 成功", a is not None)
        if a is None:
            return 1
        found = find_client(a)
        check("找到终端客户区（纯黑矩形，见 find_client）", found is not None,
              (str(found) if found else "没找到：壁纸或窗口布局变了"))
        if found:
            x0, y0, x1, y1 = found
            meas_h = y1 - y0 + 1
            print("      客户区 x=%d..%d 顶=%d 实测高=%d 布局高=%d 行高=%d Dock顶=%d"
                  % (x0, x1, y0, meas_h, client_h, ch_h, dock_top), flush=True)
            check("实测客户区高度与布局打点一致（|%d-%d| <= 6）" % (meas_h, client_h),
                  abs(meas_h - client_h) <= 6)
            check("实测客户区底边留在 Dock 之上（%d < %d）" % (y1, dock_top),
                  dock_top < 0 or y1 < dock_top)
            ink_new = band_ink(a, x0, y0, x1, t_rows - 2, ch_h)      # ★ 最新一条日志行
            ink_last = band_ink(a, x0, y0, x1, t_rows - 1, ch_h)     # 光标行（提示符）
            print("      最新日志行亮像素=%d  光标行亮像素=%d" % (ink_new, ink_last), flush=True)
            check("最新一条日志行真的有墨（亮像素 > 0）", ink_new > 0, "ink=%d" % ink_new)

            # 反向对照：clear 之后同一个指标必须是 0（证明指标不是恒真）
            for c in "clear":
                mon.key(c, 0.12)
            mon.key("ret", 1.5)
            time.sleep(1.0)
            b = read_frame(mon, os.path.join(tmp, "clear.ppm"))
            if b is not None:
                ink_new2 = band_ink(b, x0, y0, x1, t_rows - 2, ch_h)
                print("      clear 之后同一行亮像素=%d" % ink_new2, flush=True)
                check("反向对照：clear 之后该行墨量为 0（指标会变）", ink_new2 == 0,
                      "ink=%d" % ink_new2)
            else:
                check("反向对照：clear 后 screendump 成功", False)

        # ---------------- 全程不许蓝屏 ----------------
        check("全程没有 PANIC64（没有看门狗/蓝屏）", "PANIC64] stop" not in vm.log())
    finally:
        vm.close()

    bad = [n for n, ok in checks if not ok]
    print("\n=== 汇总：%d/%d PASS ===" % (len(checks) - len(bad), len(checks)))
    if bad:
        for n in bad:
            print("  FAIL: %s" % n)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
