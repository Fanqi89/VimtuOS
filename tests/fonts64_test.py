#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/fonts64_test.py - 四个字体面的端到端验收（[FONT64] 打点 + 构建产物度量 + 像素证据）

背景（本轮的字体架构）：
  face 0 西文 UI（Noto Sans 子集）  face 1 中文（Noto Sans SC 子集）
  face 2 终端等宽（Sarasa Mono SC 子集，ASCII 宽 = 汉字宽/2）
  face 3 缺字兜底（Unifont 子集：只收"前三个面都没有、但界面/终端字面量里出现"的码点）
  查询链：当前面 -> 中文面 -> 兜底面 -> 缺字占位（+ [FONT64] glyph miss 计数/打点）

断言（每条都要打点/像素/度量证据，不能只看"没崩"）：
  A. 构建产物（Python 侧，fontTools）：四个 .ttf 都在且是 TrueType(glyf+loca)；
     面 0/1/2 覆盖 ASCII 32..126；等宽面 'A' 推进 *2 == 中文面 U+4E00 推进（8px vs 16px）；
     兜底面非空 + 与前三面**无交集** + 码点确实来自 kernel/*.cpp、kernel/*.h、user/*.asm 的字符串字面量；
     需求表（西文+中文+等宽+兜底）里每个码点四个面至少有一个能画（链路完整性 = 不会出缺字占位）。
  B. 运行期（QEMU 串口 + 像素）：
     [FONT64] faces=4 ascii=1 cjk=1 mono=1 fallback=1 / mono ascii=8 cjk=16 ratio=2 /
     fallback hit cp=0x.. face=3 / selftest PASS mask=.. / glyph miss 计数 = 0；
     终端窗口里 ASCII 行的墨水宽度 ≈ 字符数 × 8px（旧 16px 位图格会翻倍）且最大墨水游程 <= 9px
     （半宽字形）；若 banner 第二行是中文，最大墨水游程 >= 12px（汉字整格）。
  C. 禁止出现 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

用法（必须用 Windows 原生 Python）：py -3 tests\\fonts64_test.py [--qemu 路径] [--timeout 180] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)

import proc64_test as p64        # noqa: E402  find_qemu / prepare_fixture / SYSTEM_IMG
import fs_term_test as fst       # noqa: E402  Monitor（sendkey）/ boot 同款
import fs_tree_test as fstq      # noqa: E402  kill（结束 QEMU）

BUILD_FONTS = {
    0: "font_bahnschrift.ttf",   # 西文 UI
    1: "font_simhei.ttf",        # 中文
    2: "font_mono.ttf",          # 终端等宽
    3: "font_fallback.ttf",      # 缺字兜底
}
ASCII = "".join(chr(c) for c in range(32, 127))
FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL"]


# ==================== PPM（QEMU screendump）====================
def read_ppm(path):
    """读 P6 PPM，返回 (w, h, px)，px[(y*w+x)] = (r,g,b)。"""
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%s" % path)
    pos, vals = 2, []
    while len(vals) < 3:
        while pos < len(data) and data[pos:pos + 1].isspace():
            pos += 1
        if data[pos:pos + 1] == b"#":
            while pos < len(data) and data[pos:pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos:pos + 1].isspace():
            pos += 1
        vals.append(int(data[start:pos]))
    pos += 1
    w, h = vals[0], vals[1]
    px = []
    for y in range(h):
        base = pos + y * w * 3
        row = data[base:base + w * 3]
        for x in range(w):
            px.append((row[3 * x], row[3 * x + 1], row[3 * x + 2]))
    return w, h, px


def row_ink_span(px, w, y, x0, x1, bg_max=60):
    """一行里"非背景"像素的列范围 (first, last)；没有墨水返回 None。"""
    first = last = None
    for x in range(x0, min(x1, w)):
        r, g, b = px[y * w + x]
        if r > bg_max or g > bg_max or b > bg_max:
            if first is None:
                first = x
            last = x
    return None if first is None else (first, last)


def row_max_ink_run(px, w, y, x0, x1, bg_max=60):
    """一行里最长的连续"非背景"列游程（像素）—— 半宽 ASCII <= 8px，汉字 >= 12px。"""
    best = cur = 0
    for x in range(x0, min(x1, w)):
        r, g, b = px[y * w + x]
        if r > bg_max or g > bg_max or b > bg_max:
            cur += 1
            best = max(best, cur)
        else:
            cur = 0
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    # ==================== A. 构建产物（Python 侧度量）====================
    print("=== A. 四个面的构建产物（fontTools 度量）===")
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        sys.stderr.write("缺少 fontTools：py -3 -m pip install fonttools\n")
        return 2

    arts = {}
    for face, name in BUILD_FONTS.items():
        p = os.path.join(ROOT, "build", name)
        arts[face] = p
        check("产物存在：build/%s" % name, os.path.exists(p))

    cmaps, tables_ok = {}, True
    for face, p in arts.items():
        if not os.path.exists(p):
            tables_ok = False
            continue
        f = TTFont(p, lazy=True)
        tab = set(f.keys())
        if "glyf" not in tab or "loca" not in tab or "CFF " in tab or "CFF2" in tab:
            tables_ok = False
        cmap = f.getBestCmap()
        cmaps[face] = set(cp for cp in cmap if cp <= 0xFFFF)
        f.close()
    check("四个产物都是 TrueType(glyf+loca，无 CFF) —— 内核 face_init 的硬要求", tables_ok)

    if len(cmaps) == 4:
        ascii_ok = all(all(ord(c) in cmaps[f] for c in ASCII) for f in (0, 1, 2))
        check("面 0/1/2 覆盖 ASCII 32..126（硬要求）", ascii_ok,
              "面0=%d 面1=%d 面2=%d 个 ASCII" % tuple(len([c for c in ASCII if ord(c) in cmaps[f]]) for f in (0, 1, 2)))

        def px_adv(face, cp):
            f = TTFont(arts[face], lazy=True)
            g = f.getBestCmap().get(cp)
            v = None if not g else round(f["hmtx"][g][0] * 16 / f["head"].unitsPerEm)
            f.close()
            return v

        a2, c2 = px_adv(2, 0x41), px_adv(1, 0x4E00)
        check("等宽面 ASCII 宽 ×2 == 中文面汉字宽（中英 1:2）",
              a2 is not None and c2 is not None and a2 * 2 == c2,
              "mono ASCII=%spx, CJK=%spx" % (a2, c2))

        fb = cmaps[3]
        other = cmaps[0] | cmaps[1] | cmaps[2]
        check("兜底面非空（有码点真的只能靠 Unifont）", len(fb - {0}) > 0,
              "兜底码点=%s" % " ".join("U+%04X" % c for c in sorted(fb)[:12]))
        check("兜底面与前三面**无交集**（只收前三个面没有的码点）", not (fb & other),
              "交集=%s" % " ".join("U+%04X" % c for c in sorted(fb & other)[:8]))
        try:
            import _subset_fonts as sf                      # 仓库根的构建脚本（需求表 + 字面量扫描）
            lits = set(sf.scan_source_codepoints().keys())
            check("兜底面的每个码点都出现在源码字符串字面量里（kernel/*.cpp、kernel/*.h、user/*.asm）",
                  fb <= lits, "不在字面量里的=%s" % " ".join("U+%04X" % c for c in sorted(fb - lits)[:8]))
            need = set(map(ord, sf.CHARS_UI)) | set(map(ord, sf.cjk_chars())) | set(map(ord, sf.CHARS_MONO)) | fb
            uncovered = [c for c in sorted(need) if not any(c in cmaps[f] for f in range(4))]
            check("链路完整性：需求表 %d 个码点，四个面至少一个能画（0 个缺字占位）" % len(need),
                  not uncovered, "无人可画=%s" % " ".join("U+%04X" % c for c in uncovered[:8]))
        except ImportError as e:
            check("能 import 仓库根的 _subset_fonts（需求表来源）", False, str(e))

    # ==================== B. 运行期：QEMU 串口 + 像素 ====================
    print("=== B. 运行期：[FONT64] 打点 + 终端像素 ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    if not os.path.exists(p64.SYSTEM_IMG):
        sys.stderr.write("缺少构建产物：%s（先跑 bash build64.sh）\n" % p64.SYSTEM_IMG)
        return 2
    img = p64.prepare_fixture()
    if not img:
        sys.stderr.write("造测试盘失败\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_fonts_")
    serial = os.path.join(tmp, "boot.log")
    port = fst.free_port()
    proc, log = fst.boot(qemu, img, serial, port)
    mon = fst.Monitor(port)
    try:
        # ---- 串口打点 ----
        log = fst.wait_for(serial, "[FONT64] selftest", 60, proc)
        check("[FONT64] faces=4 ascii=1 cjk=1 mono=1 fallback=1",
              "[FONT64] faces=4 ascii=1 cjk=1 mono=1 fallback=1" in log)
        check("[FONT64] selftest PASS mask=", "[FONT64] selftest PASS mask=" in log)
        check("[FONT64] mono ascii=8 cjk=16 ratio=2",
              "[FONT64] mono ascii=8 cjk=16 ratio=2" in log)
        hits = [ln for ln in log.splitlines() if "[FONT64] fallback hit cp=0x" in ln]
        check("兜底命中（查询链落到 face 3，而不是缺字占位）",
              any(ln.rstrip().endswith("face=3") for ln in hits),
              hits[0].strip() if hits else "（缺 [FONT64] fallback hit 行）")
        misses = [ln for ln in log.splitlines() if "[FONT64] glyph miss cp=0x" in ln]
        check("缺字计数 = 0（四个面都没有的码点）", not misses,
              "缺字=" + "; ".join(m.strip() for m in misses[:4]) if misses else "glyph miss = 0")
        bad = [c for c in FORBIDDEN if c in log]
        check("没有 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL", not bad, ",".join(bad))

        # ---- 打开终端（等宽面渲染）----
        opened = False
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if "[APP] term opened" in fst.wait_for(serial, "[APP] term opened", 12, proc):
                opened = True
                break
        check("开始菜单 -> 终端（[APP] term opened）", opened)
        time.sleep(1.5)

        # ---- 像素：终端第一行 ASCII 的墨水宽度 = 字符数 × 8px（老 16px 位图格会翻倍）----
        shot = os.path.join(tmp, "term.ppm")
        mon.send("screendump %s" % shot.replace("\\", "/"), wait=1.5)
        w = h = None
        if os.path.exists(shot):
            w, h, px = read_ppm(shot)
        if w and h:
            # 终端默认窗口：客户区 (17,41) 起 964x644，PAD 4 -> 第 0 行 y=45，第 1 行 y=61
            cx, cy = 17, 41
            # banner 的版本号唯一真源 = build64.sh 的 VIMTUOS_VERSION（编译期宏 VIMTUOS_VERSION_STR，
            # kernel/terminal64.cpp 的 shell_banner 用的就是它）。这里按真源算期望字符串，
            # 不再写死旧原文 —— 版本号升级时这条断言**不需要**再跟着改（对齐真源，不是放宽）。
            banner = "VimtuOS Terminal v0.3.2-beta14 (64-bit long mode)"
            try:
                with open(os.path.join(ROOT, "build64.sh"), "r", encoding="utf-8", errors="replace") as _bf:
                    _vm = re.search(r'VIMTUOS_VERSION="([^"]+)"', _bf.read())
                if _vm:
                    banner = "VimtuOS Terminal v%s (64-bit long mode)" % _vm.group(1)
            except OSError:
                pass
            span0 = row_ink_span(px, w, cy + 4 + 8, cx + 4, cx + 4 + 900)
            check("像素：终端 ASCII 行墨水宽度 ≈ 字符数 × 8px（半格）",
                  span0 is not None and (len(banner) * 7) <= (span0[1] - span0[0]) <= (len(banner) * 8 + 24),
                  "span=%s, 期望≈%dpx（banner=%d 字符 × 8px）" % (span0, len(banner) * 8, len(banner)) if span0 else "该行没有墨水")
            run0 = row_max_ink_run(px, w, cy + 4 + 8, cx + 4, cx + 4 + 900)
            check("像素：ASCII 字形最大墨水游程 <= 9px（半宽字形，不是 16px 位图块）", run0 <= 9,
                  "max_run=%dpx" % run0)
            zh_run = row_max_ink_run(px, w, cy + 4 + 16 + 8, cx + 4, cx + 4 + 900)
            span1 = row_ink_span(px, w, cy + 4 + 16 + 8, cx + 4, cx + 4 + 900)
            if zh_run >= 12:
                check("像素：banner 第二行是中文，汉字整格（最大墨水游程 >= 12px）", True,
                      "max_run=%dpx span=%s" % (zh_run, span1))
            else:
                check("像素：banner 第二行（英文界面时为 ASCII，如实报告跳过汉字游程断言）",
                      zh_run <= 9, "max_run=%dpx（<12 = 该行是 ASCII）" % zh_run)
        else:
            check("QEMU screendump 抓帧", False, "截图未生成")
    finally:
        fstq.kill(proc)
        if not args.keep:
            try:
                for f in os.listdir(tmp):
                    os.remove(os.path.join(tmp, f))
                os.rmdir(tmp)
            except OSError:
                pass

    total = len(checks)
    good = sum(1 for _, c in checks if c)
    print("=== RESULT: %s ===  checks=%d ok=%d" % ("PASS" if ok else "FAIL", total, good))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
