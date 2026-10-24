"""tests/termcjk64_test.py —— 终端汉字"缺字/缺笔画"验收（v0.5.12）。

用户报告："修复终端界面文字渲染缺字的问题"（截图里终端 banner 的中文笔画残缺）。

真因（本批定位，全部有实测数字）：
  终端字符格的非 ASCII 走了 face 1「中文面」= Noto Sans SC（**比例字体、笔画很细**），
  而终端是"三无"渲染（单色掩码、非黑即白、阈值 128 = 覆盖率 >= 50% 才画）。
  16px 汉字里 1px 宽的横/竖/撇捺覆盖率常只有 30~45% —— **整条笔画被判死**，
  屏幕上看就是"缺字/缺笔画"。
  修前实测（banner 第二行 6 个汉字：输入查看命令）：
      墨点合计 312，4-连通分量 80（'入' 一个撇捺断成 12 段、'输' 断成 23 段）
  修法：终端按码点切换单色阈值 —— ASCII 保持 128，**汉字用 96（37.5%）**，
       仍然是纯黑/纯白二值（不产生中间灰），符合终端"三无"规范。
  修后实测：墨点 312 -> 423（+36%），连通分量 80 -> 53（-34%）
      '查' 65 -> 109、'看' 62 -> 101、'入' 12 -> 6 段、'命' 50 -> 54、'令' 38 -> 45

判据（逐条可证伪）：
  ① `[FONT64] mono ascii=8 cjk=16 ratio=2`：中英 1:2 未被破坏
  ② `[FONT64] selftest PASS`：字体查询链自检通过（阈值改动不得影响字体链路）
  ③ banner 6 个汉字：**每个都有墨迹**（无空白字）
  ④ 6 个汉字**墨点合计 >= 350**（修前 312 / 修后 423；阈值留双侧余量）
  ⑤ 6 个汉字**4-连通分量合计 <= 65**（修前 80 / 修后 53；分量 = 笔画断裂程度）
  ⑦ 全程无 PANIC / selftest FAIL

用法：py -3 tests/termcjk64_test.py [--img build64/sysdisk.img] [--port 5795]
退出码：0 全过 / 1 有断言失败 / 2 环境问题。
"""
import argparse
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402
import startmenu64_test as SM                                    # noqa: E402
import qemuhelp as qh                                            # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

try:
    import numpy as np
except ImportError:                                              # pragma: no cover
    print("需要 numpy")
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IMG_DEF = os.path.join(ROOT, "build64", "sysdisk.img")
PORT = 5795
PAD_X, PAD_Y, LINE_H = 4, 4, 18
CJK_TEXT = "输入 help 查看命令"   # 完整 banner 第二行：中间有 6 个 ASCII 字符（48px），切格必须算上
MIN_INK = 350            # 修前 312 / 修后 423
MAX_COMP = 65            # 修前 80 / 修后 53

checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name,
                           ("  -- " + detail) if detail else ""), flush=True)


def comps(mask):
    """4-连通分量数。"""
    h, w = mask.shape
    seen = np.zeros_like(mask, dtype=bool)
    n = 0
    for y in range(h):
        for x in range(w):
            if mask[y, x] and not seen[y, x]:
                n += 1
                stack = [(y, x)]
                seen[y, x] = True
                while stack:
                    cy, cx = stack.pop()
                    for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                        ny, nx = cy + dy, cx + dx
                        if 0 <= ny < h and 0 <= nx < w and mask[ny, nx] and not seen[ny, nx]:
                            seen[ny, nx] = True
                            stack.append((ny, nx))
    return n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=IMG_DEF)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=PORT)
    args = ap.parse_args()
    qemu = T.find_qemu(args.qemu)
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用")
        return 2
    tmp = os.path.join(os.environ.get("PI_SCRATCH_DIR") or ROOT, "termcjk64")
    os.makedirs(tmp, exist_ok=True)

    print("=== termcjk64_test：终端汉字笔画完整性（img=%s）===" % args.img)
    vm = T.Vm(qemu, args.img, args.port, "termcjk")
    mon = SM.Monitor(args.port)
    try:
        if not qh.wait_desktop(mon, lambda: vm.log(), vm.proc, timeout=300):
            print("跳过：没进桌面")
            return 2
        hit = None
        for _ in range(5):
            mon.key("meta_l", 1.6)
            time.sleep(1.2)
            for mm in re.finditer(r"\[START64\] tile idx=(\d+) app=(-?\d+) name=(\S+) en=(.+?) "
                                  r"x=(\d+) y=(\d+) w=(\d+) h=(\d+)", vm.log()):
                if mm.group(3) == "终端":
                    hit = mm
                    break
            if hit:
                break
            mon.key("esc", 0.8)
            time.sleep(1.0)
        check("开始菜单里有终端", hit is not None)
        if not hit:
            return 1
        cur = SM.Cursor()
        cur.goto(mon, int(hit.group(5)) + int(hit.group(7)) // 2,
                 int(hit.group(6)) + int(hit.group(8)) // 2)
        cur.settle(mon)
        time.sleep(0.3)
        mon.click()
        up = vm.wait_log("[APP] term opened cols=", 30)
        check("终端打开（[APP] term opened）", up)
        time.sleep(2.5)

        log = vm.log()
        check("中英 1:2 未被破坏（[FONT64] mono ascii=8 cjk=16 ratio=2）",
              "[FONT64] mono ascii=8 cjk=16 ratio=2" in log)
        check("字体查询链自检通过（[FONT64] selftest PASS）",
              "[FONT64] selftest PASS" in log,
              (re.search(r"\[FONT64\] selftest [^\r\n]*", log) or ["缺失"])[0])

        # ★ 抓帧必须等画面稳定：终端窗口有打开动画（缩放/淡入），动画帧上字形会被插值糊掉，
        #   实测抓到动画帧时 '查' 只剩 10 个墨点、出现 144 个中间灰像素（假缺字）。
        #   判据：连续两帧差异 <= 400 像素才算稳定（光标闪烁占 ~144px）（最多 8 次）。
        shot = os.path.join(tmp, "term.ppm")
        prev = None
        a = None
        for attempt in range(8):
            if not mon.shot(shot):
                time.sleep(0.6)
                continue
            w, h, data = SM.read_ppm(shot)
            cur = np.frombuffer(data, dtype=np.uint8).reshape(h, w, 3)
            if prev is not None:
                diff = int((cur != prev).any(axis=2).sum())
                if diff <= 400:
                    a = cur
                    print("      画面稳定：第 %d 次抓帧，帧间差异 %d px" % (attempt + 1, diff), flush=True)
                    break
            prev = cur
            time.sleep(0.7)
        check("抓帧成功且画面稳定（连续两帧差异 <= 400 px，光标闪烁约 144 px）", a is not None)
        if a is None:
            return 1
        pure = (a.sum(axis=2) <= 12)
        colcnt = pure.sum(axis=0)
        xs = [x for x in range(w) if colcnt[x] > h * 0.3]
        check("找到终端客户区（纯黑底）", bool(xs))
        if not xs:
            return 1
        x0 = xs[0]
        sub = pure[:, x0 + 1:xs[-1]]
        rc = sub.sum(axis=1)
        ys = [y for y in range(h) if rc[y] > sub.shape[1] * 0.3]
        cy0 = ys[0]
        bright = (a.max(axis=2) > 40)
        # ★ 切格基准 = **第一行（ASCII banner）第一个墨迹列**，而不是"客户区左 + PAD_X"。
        #   为什么：终端窗口位置随级联摆放变化（off = g_count*24），窗口边框/圆角也会干扰
        #   "纯黑第一列"的检测 —— 实测按客户区左边切格会整体偏 2 个汉字（'查'的墨点算到 '令' 头上）。
        #   第一行是 "VimtuOS Terminal …"，第一个字符 'V' 的左界就是文本起点（±1px）。
        row0 = bright[cy0 + PAD_Y:cy0 + PAD_Y + LINE_H, x0:x0 + 400]
        cols_ink = np.where(row0.any(axis=0))[0]
        check("第一行有墨迹（能定位文本起点）", len(cols_ink) > 0)
        if len(cols_ink) == 0:
            return 1
        bx = x0 + int(cols_ink[0])
        print("      文本起点 x=%d（客户区左边 x=%d）" % (bx, x0), flush=True)
        ty = cy0 + PAD_Y + LINE_H          # banner 第二行（中文）

        tot_ink = tot_comp = 0
        per = []
        cx = 0
        for ch in CJK_TEXT:
            adv = 8 if ord(ch) < 0x80 else 16
            if ord(ch) >= 0x80:
                cell = bright[ty:ty + LINE_H, bx + cx: bx + cx + adv]
                ink = int(cell.sum())
                nc = comps(cell)
                per.append("%s: 墨点=%d 分量=%d" % (ch, ink, nc))
                tot_ink += ink
                tot_comp += nc
                check("汉字 %r 有墨迹（非空白/非缺字占位）" % ch, ink >= 20, "墨点=%d" % ink)
            cx += adv
        print("      " + "；".join(per), flush=True)
        check("6 个汉字墨点合计 >= %d（修前 312 / 修后 423）" % MIN_INK, tot_ink >= MIN_INK,
              "墨点合计=%d" % tot_ink)
        check("6 个汉字笔画断裂（4-连通分量）<= %d（修前 80 / 修后 53）" % MAX_COMP,
              tot_comp <= MAX_COMP, "分量合计=%d" % tot_comp)
        check("全程无 PANIC", "PANIC64] stop" not in vm.log())
        check("全程无 selftest FAIL", "[FONT64] selftest FAIL" not in vm.log())
    finally:
        vm.close()

    bad = [n for n, ok in checks if not ok]
    print("\n=== 汇总：%d/%d PASS ===" % (len(checks) - len(bad), len(checks)))
    for n in bad:
        print("  FAIL: %s" % n)
    return 0 if not bad else 1


if __name__ == "__main__":
    sys.exit(main())
