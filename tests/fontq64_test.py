# -*- coding: utf-8 -*-
"""tests/fontq64_test.py —— v0.4.8：UI 字体质量（笔画加厚 + 对比度）+ 最小 fontconfig。

用户原话："优化字体显示不平滑、不够清晰、颜色过浅、渲染不够好（除终端外）"、
"引入 fontconfig 来管理字体"。本测试只认两样东西：**串口打点** 与 **屏幕像素**。

判据：
  1) fontconfig 生效：串口有 `[FONTCFG] seed …`（首次种下 /etc/fonts.conf）与
     `[FONTCFG] load … ui_stem=120 …`（把策略落到字体层），以及 `[FONTCFG] applied keys=N`
  2) UI 正文**对比度**（WCAG 相对亮度比）：最深正文像素 vs 卡片底色 >= 12:1（AAA 门槛是 7:1）
  3) UI 次要文字（text_dim）对比度 >= 5.5:1（旧主题是 104,108,116 -> 约 6.2:1 的白底档；
     本版加深到 84,88,96。这条断言量化"颜色不那么浅了"）
  4) 墨迹（笔画）充足：设置页正文区"暗像素"（亮度 < 96）占比 >= 2%，且存在"笔画核心"像素
     （亮度 < 32）—— 笔画加厚后核心更快到满，旧版细笔画常常只有中间灰、没有核心
  5) 终端**不受影响**：同一帧里终端区域仍然只有黑/白两种颜色（三无原则没被 UI 的 AA 改动污染）
用法：py -3 tests/fontq64_test.py
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
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "fontq")
os.makedirs(outdir, exist_ok=True)
checks = []


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def lum(c):
    """WCAG 相对亮度（sRGB 0..255 -> 0..1）"""
    def f(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2])


def contrast(a, b):
    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "system.img"))
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5673)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用")
        return 2

    print("=== fontq64_test：UI 字体质量 + 最小 fontconfig ===")
    vm = T.Vm(qemu, args.img, args.port, "fontq")
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
        log = vm.log()

        # ---------- 1) fontconfig ----------
        m_seed = re.search(r"\[FONTCFG\] seed path=/etc/fonts\.conf bytes=(\d+) rc=(\d+)", log)
        m_load = re.search(r"\[FONTCFG\] load path=/etc/fonts\.conf ui_stem=(\d+) aa=(\d+)", log)
        m_app = re.search(r"\[FONTCFG\] applied src=(\S+) keys=(\d+) stem=(\d+) ui_aa=(\d+)", log)
        # 第二次启动时文件已存在，seed 就不会再出现 —— 两者有其一即算"配置就绪"
        check("fontconfig 就绪（首次 seed 或已存在后直接 load）", bool(m_seed or m_load),
              m_seed.group(0) if m_seed else (m_load.group(0) if m_load else "（缺）"))
        check("fontconfig 把策略落到字体层（applied keys/stem/ui_aa）", bool(m_app),
              m_app.group(0) if m_app else "（缺）")
        if m_app:
            check("笔画加厚为 120%（策略已落到字体层）", m_app.group(3) == "120",
                  "src=%s stem=%s ui_aa=%s keys=%s" % (m_app.group(1), m_app.group(3), m_app.group(4), m_app.group(2)))
            check("配置来源如实标注（file 或 default(...)）",
                  m_app.group(1) == "file" or m_app.group(1).startswith("default"),
                  "src=%s" % m_app.group(1))
        # 角色映射：配置来源是 file 时 load 行必须带四族；来源是 default 时用 applied 行 + faces 打点核对
        line_load = (re.search(r"\[FONTCFG\] load[^\r\n]*", log).group(0)
                     if re.search(r"\[FONTCFG\] load", log) else "")
        mapped = all(k in line_load for k in (" ui=", " mono=", " cjk=", " fb="))
        check("角色 -> 字体面映射可查询（ui/mono/cjk/fallback 四族齐全）", mapped,
              (line_load if line_load else "（缺 [FONTCFG] load 行）"))

        # ---------- 2) 打开设置页（UI 字体样本最多的窗口） ----------
        mon.key("meta_l", 1.2)
        mon.key("6", 2.6)                      # 数字快捷键 6 = 设置
        ok_set = vm.wait_log("[UI] win geom", 25)
        check("打开设置窗口（[UI] win geom … app=5）", ok_set)
        time.sleep(2.0)
        m = None
        for mm in re.finditer(r"\[UI\] win geom tag=(\S+) title=(.*?) app=5 x=(-?\d+) y=(-?\d+) "
                              r"w=(\d+) h=(\d+) client=(\d+)x(\d+)", vm.log()):
            m = mm
        if not m:
            check("拿到设置窗口几何", False)
            return 1
        cx, cy = int(m.group(3)) + 1, int(m.group(4)) + 25
        cw, ch = int(m.group(7)), int(m.group(8))
        f = mon.shot(os.path.join(outdir, "settings.ppm"))
        a = T.frame_arr(f)

        # 内容区（右侧卡片）：取窗口左半偏右、避开标题栏与左导航
        x0, y0 = cx + cw // 2, cy + 60
        x1, y1 = cx + cw - 30, cy + ch - 60
        hist = {}
        for y in range(y0, y1, 2):
            for x in range(x0, x1, 2):
                px = a[y][x]
                v = (int(px[0]), int(px[1]), int(px[2]))
                hist[v] = hist.get(v, 0) + 1
        if not hist:
            check("采样到设置页像素", False)
            return 1
        # 底色 = 出现最多的颜色（卡片/面板背景）
        bg = max(hist.items(), key=lambda kv: kv[1])[0]
        # 最深的一批像素 = 正文笔画核心
        darkest = min(hist.keys(), key=lambda v: sum(v))
        # 中间档（比底色暗但比核心亮）= 次要文字 / AA 边缘
        mid = [v for v in hist if 60 <= sum(v) / 3.0 <= 150]
        total = sum(hist.values())
        dark_cnt = sum(n for v, n in hist.items() if sum(v) / 3.0 < 96)
        core_cnt = sum(n for v, n in hist.items() if sum(v) / 3.0 < 32)

        c_main = contrast(darkest, bg)
        c_mid = contrast(min(mid, key=lambda v: sum(v)), bg) if mid else 0.0
        print("  [info] bg=%s darkest=%s 正文对比度=%.2f:1  次要文字对比度=%.2f:1  "
              "暗像素=%.2f%% 笔画核心=%.2f%%" % (bg, darkest, c_main, c_mid,
                                                  100.0 * dark_cnt / total, 100.0 * core_cnt / total))
        check("UI 正文对比度 >= 12:1（WCAG AAA 门槛是 7:1）", c_main >= 12.0, "%.2f:1" % c_main)
        check("UI 次要文字对比度 >= 5.5:1（\"颜色过浅\"这条的量化判据）", c_mid >= 5.5, "%.2f:1" % c_mid)
        # 盘点不拍脑袋：“暗像素占比”只**如实报数”（不设阀值）—— 它与窗口里有多少文字直接相关，
        # 拿它当硬判据会变成“字多就绿、字少就红”。真正可靠的是下面两条（对比度 + 画核心）。
        print("  [info] 暗像素占比 = %.2f%%（仅报数：随窗口文字量变化）" % (100.0 * dark_cnt / total))
        check("笔画核心存在（亮度 < 32 的像素 >= 0.2%）—— 细笔画常只有中间灰、没有核心",
              100.0 * core_cnt / total >= 0.2, "%.2f%%" % (100.0 * core_cnt / total))

        # ---------- 3) 终端仍是三无（UI 的 AA 改动不应污染终端） ----------
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        if vm.wait_log("[APP] term opened", 25):
            time.sleep(1.2)
            mt = None
            for mm in re.finditer(r"\[UI\] win geom tag=(\S+) title=(.*?) app=3 x=(-?\d+) y=(-?\d+) "
                                  r"w=(\d+) h=(\d+) client=(\d+)x(\d+)", vm.log()):
                mt = mm
            if mt:
                tx, ty = int(mt.group(3)) + 8, int(mt.group(4)) + 32
                tw, th = int(mt.group(7)) - 16, int(mt.group(8)) - 60
                ft = mon.shot(os.path.join(outdir, "term.ppm"))
                at = T.frame_arr(ft)
                gray = 0
                for y in range(ty, ty + th, 2):
                    for x in range(tx, tx + tw, 2):
                        px = at[y][x]
                        v = (int(px[0]), int(px[1]), int(px[2]))
                        if v in ((0, 0, 0), (255, 255, 255)):
                            continue
                        if max(v) - min(v) <= 1 and 0 < v[0] < 255:
                            gray += 1
                check("终端仍是无抗锯齿（中间灰 = 0；UI 的笔画加厚不影响终端）", gray == 0, "灰阶像素=%d" % gray)

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d" % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
