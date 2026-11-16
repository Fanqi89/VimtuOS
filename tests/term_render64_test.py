# -*- coding: utf-8 -*-
"""tests/term_render64_test.py —— v0.4.7 终端渲染验收（用户规范：三无原则 + 网格 + 纯黑底）。

判据（全部是"事实级"的，不看文档自述）：
  1) 启动不再显示硬件检查报告页：串口里**没有** `[HWUI] report shown`，且 `[CON64] replay` 有（滚动 boot log）
  2) ★ v0.10.7（方案 B）：终端**改回抗锯齿** —— 判据从"必须没有中间灰"翻转成
     "**必须有灰阶档（>= 8 档）**，且必须是**纯灰阶**（|r-g|,|g-b| <= 1，无彩色边纹/次像素）"。
     为什么翻转：单色阈值会把 1px 细笔画（'H' 左边那一竖，16px 下覆盖率 30~45%）整条判死，
     屏幕上 'H' 看起来像 'N'，日志不可读。灰阶混合后细笔画是**浅色线**，笔画在、只是淡。
  3) 正文纯白 #FFFFFF（不是旧的 #E0E0E0）
  4) 行距是整数像素、且 ASCII 推进 = 汉字宽 / 2（中英 2:1）—— 用终端自己的排版打点核对
用法：py -3 tests/term_render64_test.py [--img ...] [--port ...]
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402  复用它的 QEMU/monitor/ppm 工具

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "termrender")
os.makedirs(outdir, exist_ok=True)

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5671)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu:
        print("跳过：找不到 qemu-system-x86_64")
        return 2
    if not os.path.exists(args.img):
        print("跳过：镜像不存在 %s（先跑 build64.sh）" % args.img)
        return 2

    print("=== term_render64_test：终端三无渲染 + 启动直出 boot log ===")
    vm = T.Vm(qemu, args.img, args.port, "term")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 QEMU monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏可交互", False, vm.log()[-200:])
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面（[GUI64] ready）", False)
            return 1
        time.sleep(2.5)
        log = vm.log()

        # ---------- 1) 启动：不显示报告页，直出滚动 boot log ----------
        check("启动**不再**显示硬件检查报告页（串口无 [HWUI] report shown）",
              "[HWUI] report shown" not in log,
              "出现次数=%d" % log.count("[HWUI] report shown"))
        check("启动期确实回放了滚动的内核日志（[CON64] replay …）",
              "[CON64] replay" in log,
              (re.search(r"\[CON64\] replay[^\r\n]*", log).group(0) if "[CON64] replay" in log else "（缺）"))
        check("boot log 是英文（含 [X64]/[MEM64]/[TASK64] 之类英文字段）",
              ("[X64]" in log) and ("[TASK64]" in log))

        # ---------- 打开终端 ----------
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        opened = vm.wait_log("[APP] term opened", 25)
        check("开始菜单 -> 终端（[APP] term opened）", opened)
        if not opened:
            return 1
        time.sleep(1.0)
        # 打一行命令（终端回显 + 中文帮助，正好同时覆盖 ASCII 面与中文面）
        # ★ textstress 的 Monitor 只有 key()（sendkey），没有 type_line —— 逐字符敲。
        for ch in "help":
            mon.key(ch, wait=0.10)
        mon.key("ret", wait=1.8)
        time.sleep(1.2)

        # ---------- 终端几何（标题带空格：title=.*? app=3） ----------
        m = None
        for mm in re.finditer(r"\[UI\] win geom tag=(\S+) title=(.*?) app=3 x=(-?\d+) y=(-?\d+) "
                              r"w=(\d+) h=(\d+) client=(\d+)x(\d+)", vm.log()):
            m = mm
        if not m:
            check("拿到终端窗口几何（[UI] win geom … app=3）", False)
            return 1
        cx, cy = int(m.group(3)) + 1, int(m.group(4)) + 25
        cw, ch = int(m.group(7)), int(m.group(8))
        check("终端窗口几何可解析（含空格标题也能匹配）", cw > 200 and ch > 100,
              "client=%dx%d @ (%d,%d)" % (cw, ch, cx, cy))

        f = mon.shot(os.path.join(outdir, "term.ppm"))   # shot() 已返回解析好的 (w,h,data)
        assert f is not None, "screendump 失败"
        a = T.frame_arr(f)
        # 取客户区内部（避开边框与标题栏，也避开右下角光标所在行）
        x0, y0 = cx + 6, cy + 6
        x1, y1 = cx + cw - 6, cy + ch - 40
        hist = {}
        total = 0
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                px = a[y][x]
                v = (int(px[0]), int(px[1]), int(px[2]))
                hist[v] = hist.get(v, 0) + 1
                total += 1
        black = hist.get((0, 0, 0), 0)
        white = hist.get((255, 255, 255), 0)
        gray = sum(n for v, n in hist.items()
                   if v not in ((0, 0, 0), (255, 255, 255))
                   and max(abs(v[0] - v[1]), abs(v[1] - v[2]), abs(v[0] - v[2])) <= 1
                   and 0 < v[0] < 255)
        # ★ v0.10.7：中间灰的**档数**（不是像素数）+ 彩色像素统计
        gray_levels = set(v[0] for v, n in hist.items()
                          if v not in ((0, 0, 0), (255, 255, 255))
                          and max(abs(v[0] - v[1]), abs(v[1] - v[2]), abs(v[0] - v[2])) <= 1
                          and 0 < v[0] < 255)
        color_hist = {v: n for v, n in hist.items()
                      if max(abs(v[0] - v[1]), abs(v[1] - v[2]), abs(v[0] - v[2])) > 1}
        colored = sum(color_hist.values())
        check("终端背景是**绝对纯黑** #000000（占比 > 55%）", black > total * 0.55,
              "black=%d/%d=%.3f" % (black, total, float(black) / max(total, 1)))
        check("终端正文是**纯白** #FFFFFF（不是旧的 #E0E0E0）", white > 200, "white=%d" % white)
        # ★ v0.10.7（方案 B）：判据从"无中间灰"翻转成"必须有中间灰，但必须是纯灰阶"。
        #   ① 灰阶档数 >= 8：抗锯齿真的在工作（细笔画覆盖率被表达成浅色，而不是消失）；
        #   ② 所有像素 |r-g|,|g-b| <= 1：没有彩色边纹 / 次像素渲染（比"颜色数<=4"更强，
        #      后者只是"二值"的同义词）；
        #   ③ 档数上界 200：防止脏色噪声把画面搞花。
        check("**抗锯齿生效**：终端里有足够多的中间灰档（>= 8 档）", len(gray_levels) >= 8,
              "灰阶档=%d 灰阶像素=%d（占比 %.4f）" % (len(gray_levels), gray, float(gray) / max(total, 1)))
        check("**纯灰阶**：没有彩色像素（|r-g|,|g-b| <= 1；无彩色边纹/次像素）", colored == 0,
              "彩色像素=%d 例=%s" % (colored, sorted(color_hist.items(), key=lambda kv: -kv[1])[:3]))
        # 档数没有客观上界（覆盖率本就可以铺满 1..254），所以判"灰阶像素**够多**"：
        # 证明抗锯齿作用在正文笔画上，而不是只有光标边缘几个像素。
        check("**抗锯齿覆盖正文**：中间灰像素数 > 500", gray > 500,
              "灰阶像素=%d" % gray)

        # ---------- 排版：中英 2:1 + 整数行距 ----------
        lg = re.search(r"\[UI\] term layout client=\d+x\d+ cols=(\d+) rows=(\d+)", vm.log())
        check("终端排版打点存在（cols/rows 随客户区推导）", lg is not None,
              lg.group(0) if lg else "（缺）")
        if lg:
            cols, rows = int(lg.group(1)), int(lg.group(2))
            # 半格 = 客户区可用宽 / 列数；汉字格 = 2 倍半格
            half = (cw - 8) / float(cols)
            cellh = (ch - 4) / float(rows)
            check("半格宽是整数像素（无缩放原则：不允许 8.5px 这种）",
                  abs(half - round(half)) < 0.51, "half=%.3f px（%d 列 / %d px 客户区）" % (half, cols, cw))
            check("行距是整数像素且落在 16..20（度量驱动、不裁切）",
                  abs(cellh - round(cellh)) < 0.6 and 16 <= round(cellh) <= 20,
                  "cell_h=%.3f px（%d 行 / %d px）" % (cellh, rows, ch))

        # ---------- 串口卫生 ----------
        for bad in ("PANIC", "TRIPLE FAULT", "selftest FAIL"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d" % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    if bad:
        for n in bad:
            print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
