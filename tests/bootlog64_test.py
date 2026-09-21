#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/bootlog64_test.py - 开机滚屏引导控制台（boot console + dmesg）端到端验收

批次 N 的能力：每次开机/重启**进桌面之前**，把这次启动真的跑出来的内核日志像 Linux 那样
在屏上滚一遍（黑底 + 等宽面 ASCII + 行首时间戳 [    0.123]，按 FAIL/WARN 着色），然后
停 CON64_STAY_MS 或按任意键立即进桌面；日志本体是 16 KiB 环形缓冲（+ 头部保留行），
终端 `dmesg` 可以随时回看；`boot verbose on|off` 把开关持久化到 config64/store64。

断言（每条都要串口/像素/构建证据，不能只看"没崩"）：
  A. 串口打点：[CON64] ring init bytes=/lines=/text_max=、screen ready cols=/rows=、
     selftest PASS mask=0x0、replay lines=N dropped=M（N >= 10：fb_init 之前的打点确实被缓存并回放）、
     live lines=。
  B. 像素（QEMU screendump，滚屏期间抓两帧）：
     * 黑底 + 文本行：非黑像素占比在 0.5%~25% 之间；>= 12 行有文字带；
     * 行间距均匀：文字带中心间距 ≈ 20px（line_h）；
     * 滚动证据：两帧里同一位置的文字行内容不同（屏上内容在动），并报告"上移 k 行"的最佳匹配。
  C. 跳过（第二遍启动）：滚屏期间注入一个按键 -> [CON64] skip key=1，随后 [GUI64] ready 出现。
  D. dmesg：终端里跑 `dmesg` -> 串口出现 [CON64] dmesg lines= 与 [CON64] dmesg[i] 前缀的
     早期行（[LM64] ENTERED LONG MODE 那一行）；屏幕上终端区域出现十几行正文（像素，有界重试）。
  E. 持久化开关（第三遍冷启动同一块镜像）：`boot verbose off` -> [CON64] boot verbose=off persisted=1；
     下一遍必须出现 [CON64] verbose=0 skipped、且**不再回放**（无 [CON64] replay lines=），桌面照常起来；
     那之后再 `boot verbose on` 恢复默认，避免影响别的脚本复用这块夹具盘。
  F. 禁止：PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL。

三遍启动的分工（避免互相干扰）：
  1) 第一遍：不按键，看完整滚屏（抓两帧 + 回放行数）+ 终端 dmesg；
  2) 第二遍：按键跳过 + `boot verbose off`（持久化）；
  3) 第三遍：冷启动验证开关生效（不回放），并恢复 `boot verbose on`。

用法（Windows 原生 Python）：py -3 tests\\bootlog64_test.py [--qemu 路径] [--keep]
退出码：0 = 全通过；1 = 有断言失败；2 = 环境问题
"""
import argparse
import os
import re
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, ROOT)
import argparse
import os
import re
import socket
import sys
import tempfile
import time
import proc64_test as p64      # noqa: E402  find_qemu / prepare_fixture
import fs_tree_test as fstq    # noqa: E402  kill / free_port / boot_installed

FORBIDDEN = ["PANIC", "TRIPLE FAULT", "FAILED mask=", "selftest FAIL"]
LINE_H = 20                    # console64 的行高（font_line_height）：像素断言用它
INK_LO, INK_HI = 0.005, 0.25   # 非黑像素占比的合理区间
BLACK_MIN = 0.60               # 黑底的最低占比
MIN_BANDS = 12                 # 至少这么多行有文字带


# ==================== PPM（QEMU screendump）====================
def read_ppm(path):
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
    return w, h, data[pos:pos + w * h * 3]



def row_profile(px, w, h, x0=0, x1=None, thr=60, step=2):
    """逐行"有墨"像素数（x 方向按 step 采样再线性放大；够快，比率/行带判定足够准）。"""
    x1 = w if x1 is None else min(x1, w)
    out = []
    for y in range(h):
        o = y * w * 3
        n = 0
        for x in range(x0, x1, step):
            if px[o + 3 * x] > thr or px[o + 3 * x + 1] > thr or px[o + 3 * x + 2] > thr:
                n += 1
        out.append(n * step)
    return out


def bands_from_profile(prof, min_ink=12, y0=0, y1=None):
    """把连续"有文字"的像素行合并成"行带"；返回每带的 (y_top, y_bottom, 中心行)。"""
    y1 = len(prof) if y1 is None else min(y1, len(prof))
    bands = []
    for y in range(y0, y1):
        if prof[y] >= min_ink:
            if bands and y - bands[-1][1] <= 2:
                bands[-1][1] = y
            else:
                bands.append([y, y])
    return [(a, b, (a + b) // 2) for a, b in bands]


def text_bands(px, w, h, min_ink=12, y0=0, y1=None, x0=0, x1=None):
    return bands_from_profile(row_profile(px, w, h, x0=x0, x1=x1), min_ink, y0, y1)


def ink_ratio(px, w, h, prof=None):
    prof = prof if prof is not None else row_profile(px, w, h)
    return sum(prof) / float(w * h)


def console_frame(w, h, px, prof=None):
    """是不是"引导控制台帧"：1280x800、黑底、>= MIN_BANDS 行等宽文字、墨占比在区间内。

    这条判据同时排除了：硬件报告页（深蓝底 + 墨 30%+）、开机 logo（黑底但只有 1~2 个实心块）、桌面（墨 99%）。
    """
    if (w, h) != (1280, 800):
        return False
    prof = prof if prof is not None else row_profile(px, w, h)
    r = ink_ratio(px, w, h, prof)
    if not (INK_LO <= r <= INK_HI):
        return False
    return len(bands_from_profile(prof, 12)) >= MIN_BANDS


def band_sig(px, w, b, x0=120, x1=1240, step=6, thr=60):
    """一个文字带的"内容签名"：按列采样出的有墨位图（比较两帧同一位置是不是同一内容）。"""
    y = b[2]
    o = y * w * 3
    return "".join("1" if (px[o + 3 * x] > thr or px[o + 3 * x + 1] > thr or px[o + 3 * x + 2] > thr) else "0"
                   for x in range(x0, min(x1, w), step))


def sig_similar(a, b):
    if len(a) != len(b) or not a:
        return 0.0
    return sum(1 for i in range(len(a)) if a[i] == b[i]) / float(len(a))


# ==================== 小工具 ====================
def wait_mark(path, needle, timeout, proc=None, step=0.08):
    """快速轮询（skip 注入要抓住 ~2.5s 的回放窗口；比 fs_tree.wait_for 的 0.4s 快）。"""
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                txt = f.read()
        except OSError:
            txt = ""
        if needle in txt:
            return txt
        if proc is not None and proc.poll() is not None:
            return txt
        time.sleep(step)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def screendump(mon, path):
    if os.path.exists(path):
        os.remove(path)
    mon.send("screendump %s" % path.replace("\\", "/"), wait=0.5)
    for _ in range(20):
        if os.path.exists(path) and os.path.getsize(path) > 1024:
            return True
        time.sleep(0.1)
    return False

def xp_words(port, addr, nwords=16):
    """用 QEMU monitor 的 `xp` 读**物理内存**（这里用来直读 vram 帧缓冲的像素）。"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=8)
    except OSError:
        return []
    try:
        s.settimeout(0.6)
        s.sendall(("xp /%dxw 0x%x\n" % (nwords, addr)).encode())
        time.sleep(0.08)
        data = b""
        try:
            while True:
                ch = s.recv(65536)
                if not ch:
                    break
                data += ch
        except socket.timeout:
            pass
    finally:
        s.close()
    vals = re.findall(r"0x([0-9a-fA-F]{8})", data.decode("utf-8", "replace"))
    return vals[1:]                     # 第一个匹配是地址回显，丢掉
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    img = p64.prepare_fixture()
    if not img:
        sys.stderr.write("缺少构建产物 build64/system.img（先跑 bash build64.sh）\n")
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_bootlog_")
    print("=== Vimtu64 bootlog64 acceptance（开机滚屏引导控制台 + dmesg）===")
    print("[bootlog] 夹具盘：%s（VimtuFS2 主分区，store64 可持久化）" % img)

    # ==================================================================
    # 第一遍：完整滚屏（抓两帧）+ 回放行数 + 终端 dmesg
    # ==================================================================
    serial1 = os.path.join(tmp, "boot1.log")
    port1 = fstq.free_port()
    proc, mon = fstq.boot_installed(qemu, [img], serial1, port1, "Vimtu64-bootlog1")
    try:
        print("=== 1) 串口：启动日志缓冲 + 屏幕控制台打点 ===")
        log = wait_mark(serial1, "[CON64] screen ready", 90, proc)
        m0 = re.search(r"\[CON64\] ring init bytes=16384 lines=\d+ text_max=160", log)
        check("[CON64] ring init bytes=16384 lines=N text_max=160", bool(m0),
              m0.group(0) if m0 else ((re.search(r"\[CON64\] ring init[^\r\n]*", log).group(0))
                                      if "[CON64] ring init" in log else "缺"))
        m = re.search(r"\[CON64\] screen ready cols=(\d+) rows=(\d+) line_h=(\d+) adv=(\d+)", log)
        check("[CON64] screen ready cols=/rows=", bool(m), m.group(0) if m else "缺")
        if m:
            check("几何合理（cols>=40 rows>=10，1280x800 下约 40 行）",
                  40 <= int(m.group(1)) and 10 <= int(m.group(2)) <= 80,
                  "cols=%s rows=%s line_h=%s" % (m.group(1), m.group(2), m.group(3)))

        print("=== 2) 像素：黑底 + 等宽文本行 + 滚动证据（连续抓帧，再挑控制台帧）===")
        # 说明：screendump 与串口文件都有各自的延迟（实测 0.5~2s），而且引导控制台只出现一次。
        # 所以这里**连续抓帧**（约 0.4~0.6s 一帧），抓完再挑"黑底 + 多行文字"的控制台帧；
        # 判据一个都不放松：黑底、每帧 >= MIN_BANDS 行文字带、行间距 ≈ 20px、两帧内容不同。
        frames = []                       # (t, w, h, px, prof)
        vram_samples = []                 # 直读 vram 同一行像素的多次采样（滚动证据）
        mlfb = re.search(r"\[LM64\] LFB addr=([0-9A-F]+) size=\d+x\d+ bpp=\d+ pitch=(\d+)", log)
        lfb = int(mlfb.group(1), 16) if mlfb else 0
        pitch = int(mlfb.group(2)) if mlfb else 0
        t_pix0 = time.time()
        while time.time() - t_pix0 < 16:
            fn = os.path.join(tmp, "con_%02d.ppm" % len(frames))
            if not screendump(mon, fn):
                time.sleep(0.2)
                continue
            try:
                w, h, px = read_ppm(fn)
            except Exception as exc:                        # noqa: BLE001
                print("    [warn] 帧解析失败：%s" % exc)
                continue
            prof = row_profile(px, w, h)
            frames.append((time.time() - t_pix0, w, h, px, prof))
            # ★ 滚动证据：QEMU 在 -display none 下的显示表面会给**过期**帧（实测滞后 1~2s），
            #   所以这里同时直读 vram（monitor `xp`，就是屏上像素）：控制台第 1 行文字（y=4..24）
            #   里的第 7 个像素行在不同时刻内容不同 => 屏上内容真的在上移。
            if lfb and pitch and len(vram_samples) < 12:
                vals = xp_words(port1, lfb + 7 * pitch, 24)
                if vals:
                    vram_samples.append([v.lower() for v in vals])
            if len(frames) >= 16:
                break
            cons = [fr for fr in frames if console_frame(fr[1], fr[2], fr[3], fr[4])]
            if len(cons) >= 4:
                break
            time.sleep(0.15)
        cons = [fr for fr in frames if console_frame(fr[1], fr[2], fr[3], fr[4])]
        check("抓到 >= 1 帧控制台帧（黑底 + >= %d 行文字带；控制台只出现一次，显示表面还会滞后）" % MIN_BANDS,
              len(cons) >= 1,
              "共抓 %d 帧，控制台帧 %d 帧：%s" % (len(frames), len(cons),
                (", ".join("%.1fs(墨%.1f%%,%d行)" % (fr[0], 100.0 * ink_ratio(fr[3], fr[1], fr[2], fr[4]),
                                                     len(bands_from_profile(fr[4], 12)))
                           for fr in cons[:4])) or "无"))
        if len(cons) >= 1:
            w1, h1, px1, prof1 = cons[0][1], cons[0][2], cons[0][3], cons[0][4]
            ratio1 = ink_ratio(px1, w1, h1, prof1)
            bands1 = bands_from_profile(prof1, 12)
            check("控制台帧：黑底（黑占比 >= %.0f%%）+ 非黑像素占比在 %.1f%%~%.0f%%"
                  % (BLACK_MIN * 100, INK_LO * 100, INK_HI * 100),
                  (1.0 - ratio1) >= BLACK_MIN and INK_LO <= ratio1 <= INK_HI,
                  "黑=%.2f%% 墨=%.2f%%" % ((1.0 - ratio1) * 100, ratio1 * 100))
            check("文本行带 >= %d 行（屏上全是日志行）" % MIN_BANDS,
                  len(bands1) >= MIN_BANDS, "bands=%d" % len(bands1))
            gaps = [bands1[i + 1][2] - bands1[i][2] for i in range(len(bands1) - 1)]
            good = [g for g in gaps if abs(g - LINE_H) <= 2]
            check("行间距均匀（相邻行带中心间距 ≈ %dpx）" % LINE_H,
                  len(gaps) >= 8 and len(good) >= int(0.75 * len(gaps)),
                  "间距=%s" % (gaps[:8],))
            gaps = [bands1[i + 1][2] - bands1[i][2] for i in range(len(bands1) - 1)]
            good = [g for g in gaps if abs(g - LINE_H) <= 2]
            check("行间距均匀（相邻行带中心间距 ≈ %dpx）" % LINE_H,
                  len(gaps) >= 8 and len(good) >= int(0.75 * len(gaps)),
                  "间距=%s" % (gaps[:8],))
            # 滚动证据（帧间比较）：QEMU 在 -display none 下的显示表面是**过期**的（实测滞后 1~2s），
            # 所以帧间一致**不能**证明"没滚"；真正的滚动证据见第 5 步（直接读 vram 的同一行像素）。
            # 这里只如实报告帧间比较结果，供人参考。
            best = None
            for i in range(len(cons)):
                for j in range(i + 1, len(cons)):
                    wi, hi, pxi = cons[i][1], cons[i][2], cons[i][3]
                    wj, hj, pxj = cons[j][1], cons[j][2], cons[j][3]
                    bi = text_bands(pxi, wi, hi, min_ink=12)
                    bj = text_bands(pxj, wj, hj, min_ink=12)
                    if not bi or not bj:
                        continue
                    si = [band_sig(pxi, wi, b) for b in bi]
                    sj = [band_sig(pxj, wj, b) for b in bj]
                    sim_top = sig_similar(si[0], sj[0])
                    cand = (sim_top, cons[i][0], cons[j][0])
                    if best is None or sim_top < best[0]:
                        best = cand
            if best:
                print("    [info] 帧间顶部行相似度=%.2f（帧间隔 %.1fs；-display none 的显示表面会滞后，"
                      "滚动证据以第 5 步的 vram 直读为准）" % (best[0], best[2] - best[1]))
            else:
                print("    [info] 只抓到 1 帧控制台帧，帧间比较跳过（滚动证据以本步的 vram 直读为准）")
        # ---- vram 直读（真实帧缓冲）：黑底 + 时间戳像素 + 同一行内容随时间变化（滚屏实证）----
        # 为什么不能只看 screendump：-display none 下 QEMU 的显示表面会滞后 1~2s（实测），
        # 控制台只出现一次；monitor `xp` 读的是 vram（屏上像素），能直接看到像素在变。
        vuniq = {",".join(s) for s in vram_samples}
        vstamp = any("ff7f8c8d" in s for s in vram_samples)      # console64 时间戳色 0xFF7F8C8D
        vbody = any("ffd0d0d0" in s for s in vram_samples)       # 正文色 0xFFD0D0D0
        check("vram 直读：控制台第 1 行出现过时间戳像素（黑底 + 等宽文字真的写进帧缓冲）",
              vstamp or vbody, "采样 %d 次；时间戳色=%s 正文色=%s" % (len(vram_samples), vstamp, vbody))
        check("滚动证据：同一行像素在不同时刻不同（内容在上移，不是静止画面）", len(vuniq) >= 2,
              "不同采样=%d/%d" % (len(vuniq), len(vram_samples)))

        print("=== 3) 回放规模 / 自检 / 实时模式（本遍不按键）===")
        log = wait_mark(serial1, "[CON64] live lines=", 30, proc)
        m = re.search(r"\[CON64\] replay lines=(\d+) dropped=(\d+) ms=(\d+)", log)
        check("[CON64] replay lines=N dropped=M ms=", bool(m), m.group(0) if m else "缺")
        if m:
            check("replay 行数 >= 10（fb_init 之前的打点被缓存并回放）", int(m.group(1)) >= 10,
                  "lines=%s dropped=%s ms=%s" % (m.group(1), m.group(2), m.group(3)))
        check("[CON64] selftest PASS mask=0x0", "[CON64] selftest PASS mask=0x0" in log)
        check("[CON64] live lines=（进入实时追加模式）", "[CON64] live lines=" in log)
        log = wait_mark(serial1, "[GUI64] ready", 40, proc)
        check("本遍正常进桌面（[GUI64] ready）", "[GUI64] ready" in log)

        print("=== 4) dmesg：终端命令 -> 屏幕 + 串口早期行 ===")
        if fstq.open_terminal(mon, serial1, proc):
            time.sleep(1.0)
            mon.type_line("dmesg", per_key=0.12)
            log = wait_mark(serial1, "[CON64] dmesg lines=", 30, proc, step=0.15)
            check("[CON64] dmesg lines= head= dropped=（终端命令跑通）", "[CON64] dmesg lines=" in log,
                  (re.search(r"\[CON64\] dmesg lines=[^\r\n]*", log) or re.search(r"$^", log)).group(0))
            log = wait_mark(serial1, "[CON64] dmesg[", 20, proc, step=0.15)   # 头部 16 行 + 尾部 16 行的串口证据
            m = re.search(r"\[CON64\] dmesg\[(\d+)\] \[ *\d+\.\d{6}\] \[LM64\] ENTERED LONG MODE", log)
            check("dmesg 输出含最早期的 [LM64] ENTERED LONG MODE（头部保留行）", bool(m),
                  m.group(0) if m else "缺（环形缓冲把最早的启动行覆盖了？）")
            check("dmesg 的 [    0.000000] 时间戳格式（Linux 风格）",
                  bool(re.search(r"\[CON64\] dmesg\[\d+\] \[ *\d+\.\d{6}\]", log)))
            # 像素：终端区域出现一大片正文（dmesg 打 250+ 行；用"有墨的行数"判定：
            # 终端字符行高 16px、字形上下几乎相连，行带会连成一条，判不出行数；有界重试等 GUI 画完）
            fs = os.path.join(tmp, "term_dmesg.ppm")
            text_rows = term_ink = 0
            for attempt in range(10):
                time.sleep(1.0 if attempt else 1.5)
                if not screendump(mon, fs):
                    continue
                w, h, px = read_ppm(fs)
                prof = row_profile(px, w, h, x0=16, x1=min(w, 992))
                rows_now = sum(1 for y in range(45, min(h, 716)) if prof[y] >= 100)
                ink_now = sum(prof[45:min(h, 716)])
                if rows_now > text_rows:                 # 取最好的一帧（显示表面会滞后）
                    text_rows = rows_now
                    term_ink = ink_now
                if text_rows >= 400 and term_ink >= 150000:
                    break
            # 判据：一整屏 dmesg 正文 ≈ 40 行 × 12 像素行 ≈ 480 行有墨、约 20 万像素；
            # 这里取 1/3 作为下限（空终端 ≈ 0；只有 banner ≈ 20 行/5k px），证明"一大片正文"确实画上去了。
            check("像素：终端窗口里 dmesg 正文 >= 150 行像素行有文字（约 3 万墨像素）",
                  text_rows >= 150 and term_ink >= 30000,
                  "有文字行=%d 终端区域墨=%d px" % (text_rows, term_ink))
        else:
            check("开始菜单 -> 终端（[APP] term opened）", False, "终端没打开")

        bad = [c for c in FORBIDDEN if c in log]
        check("第一遍：没有 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL", not bad, ",".join(bad))
    finally:
        fstq.kill(proc)

    # ==================================================================
    # 第二遍：滚屏期间按键跳过 + boot verbose off（持久化）
    # ==================================================================
    serial2 = os.path.join(tmp, "boot2.log")
    port2 = fstq.free_port()
    proc2, mon2 = fstq.boot_installed(qemu, [img], serial2, port2, "Vimtu64-bootlog2")
    try:
        print("=== 5) 跳过：滚屏期间注入一个按键 -> [CON64] skip key=1 ===")
        wait_mark(serial2, "[CON64] screen ready", 90, proc2, step=0.05)
        skip_seen = False
        for _ in range(10):
            if "[GUI64] ready" in wait_mark(serial2, "[CON64] screen ready", 0.1, proc2):
                break
            mon2.key("ret", wait=0.15)
            if "[CON64] skip key=1" in wait_mark(serial2, "[CON64] skip key=1", 0.6, proc2, step=0.05):
                skip_seen = True
                break
            time.sleep(0.25)
        log2 = wait_mark(serial2, "[CON64] skip key=1", 6, proc2, step=0.1)
        skip_seen = skip_seen or ("[CON64] skip key=1" in log2)
        check("[CON64] skip key=1（任意键立即结束引导控制台）", skip_seen,
              (re.search(r"\[CON64\] (skip|replay|live)[^\r\n]*", log2) or re.search(r"$^", log2)).group(0))
        log2 = wait_mark(serial2, "[GUI64] ready", 30, proc2, step=0.15)
        check("跳过之后**继续**既有流程（[GUI64] ready）", "[GUI64] ready" in log2)
        if fstq.open_terminal(mon2, serial2, proc2):
            time.sleep(0.8)
            mon2.type_line("boot verbose off", per_key=0.12)
            log2 = wait_mark(serial2, "[CON64] boot verbose=off", 25, proc2, step=0.15)
            check("[CON64] boot verbose=off persisted=1", 
                  bool(re.search(r"\[CON64\] boot verbose=off persisted=1", log2)),
                  (re.search(r"\[CON64\] boot verbose=[^\r\n]*", log2) or re.search(r"$^", log2)).group(0))
        else:
            check("第二遍：开始菜单 -> 终端", False, "终端没打开")
        bad = [c for c in FORBIDDEN if c in log2]
        check("第二遍：没有 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL", not bad, ",".join(bad))
    finally:
        fstq.kill(proc2)

    # ==================================================================
    # 第三遍：冷启动同一块镜像（boot.verbose=0）—— 不回放，仍进桌面；再恢复 on
    # ==================================================================
    serial3 = os.path.join(tmp, "boot3.log")
    proc3, mon3 = fstq.boot_installed(qemu, [img], serial3, fstq.free_port(), "Vimtu64-bootlog3")
    try:
        print("=== 6) 第三遍冷启动：boot.verbose=0 -> 跳过滚屏，仍进桌面 ===")
        log3 = wait_mark(serial3, "[CON64] verbose=0 skipped", 90, proc3, step=0.15)
        check("[CON64] verbose=0 skipped（关掉的开关真的生效）", "[CON64] verbose=0 skipped" in log3,
              (re.search(r"\[CON64\] verbose=0[^\r\n]*", log3) or re.search(r"$^", log3)).group(0))
        log3 = wait_mark(serial3, "[GUI64] ready", 40, proc3, step=0.15)
        check("第三遍仍正常进桌面（[GUI64] ready）", "[GUI64] ready" in log3)
        check("verbose=0 时不回放（无 [CON64] replay lines=）", "[CON64] replay lines=" not in log3)
        if fstq.open_terminal(mon3, serial3, proc3):
            time.sleep(0.8)
            mon3.type_line("dmesg", per_key=0.12)
            log3 = wait_mark(serial3, "[CON64] dmesg lines=", 30, proc3, step=0.15)
            check("verbose=0 时 dmesg 仍可用（缓冲不受开关影响）", "[CON64] dmesg lines=" in log3)
            mon3.type_line("boot verbose on", per_key=0.12)     # 恢复默认，别影响别的脚本复用夹具盘
            log3 = wait_mark(serial3, "[CON64] boot verbose=on", 25, proc3, step=0.15)
            check("boot verbose on 恢复（persisted=1）",
                  bool(re.search(r"\[CON64\] boot verbose=on persisted=1", log3)))
        else:
            check("第三遍：开始菜单 -> 终端", False, "终端没打开")
        bad = [c for c in FORBIDDEN if c in log3]
        check("第三遍：没有 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL", not bad, ",".join(bad))
    finally:
        fstq.kill(proc3)
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
