# -*- coding: utf-8 -*-
"""tests/wheel64_test.py - 滚轮端到端验收（终端历史缓冲 / 设置内容滚动 / 文本预览行切分）

背景（用户实测）：滚轮在"开始菜单 -> 全部应用"里能用，但在**终端**和**设置**里完全没反应，
"文本查看器"（文件管理器的只读文本预览）也一样。根因：内核的滚轮事件只喂给了弹窗与开始菜单
（gui64.cpp 的 handle_mouse -> panels64_wheel64 / startmenu64_wheel64），**窗口层根本没有滚轮回调**。

本批修复（v0.9.25）：
  * kernel/gui64.{h,cpp}：窗口契约新增 on_wheel + gui64_set_wheel()；滚轮分发 = 弹窗 -> 开始菜单
    -> **光标下最前的窗口**（win_at）。
  * kernel/terminal64.cpp：真正的 scrollback（128 行环形历史缓冲）+ 视口偏移；看历史时不画光标；
    来新输出自动回底部（Windows Terminal 口径）。
  * kernel/settings64.cpp：内容区滚动偏移（画时 g_po_y 位移 + 仍被 fb_set_clip 裁在内容区）、
    边画边量本页内容高度（g_trk/trk）算真实滚动上限、右侧 4px 滚动条、命中把偏移加回去。
  * kernel/explorer64.cpp：文本预览读入上限 1 KiB -> 16 KiB、按 UI 字体实测列宽换行、
    行切分抽成唯一实现 prev_line_step()（绘制/计数/自检共用）+ 视口夹取复用 clamp_scroll_pure()。

本脚本覆盖（真 QEMU + 真 PS/2 滚轮包 + 像素）：
  A) 终端（**先开、无遮挡**）：开始菜单 -> 终端 -> help 灌满历史 -> 滚轮上滚
     断言 [TERM] scroll off=<n> hist=<h> rows=<r> dz=.. (up)（n > 0、h >= n），再下滚回 0
     + 前后两张 screendump 的差异像素 > 阈值（真的重画了，不是只打日志）
  B) 设置（在终端之上）：开始菜单 -> 设置 -> 点左导航"关于"页 -> 内容超出一屏 -> 滚轮下滚
     断言 [SET64] scroll off=<n> max=<m> ch=<h> dz=..（m > 0、n 递增），再上滚回 0
     + 前后两张 screendump 的差异像素 > 阈值
  C) 禁止行：PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [TERM] unsupported / [SET64] selftest FAIL

边界（如实写）：文本预览窗口**没有**在本脚本里用鼠标点开（打开它要"双击图标网格里的条目"，
那套闭环定位在 tests/explorer64_test.py 里）；它的行切分/夹取由内核自检
`[EXPL] selftest PASS`（第 9 段：prev_line_step + clamp_scroll_pure）覆盖，分发路径与终端/设置同一段代码。

用法（Windows 原生 Python）：py -3 tests\\wheel64_test.py [--qemu 路径] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import proc64_test as p64          # noqa: E402  夹具/端口/QEMU 路径
import qemuhelp as qh              # noqa: E402  登录手势（锁屏 -> 登录 -> 桌面）

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
    "[TERM] unsupported",
    "[MEM64] OOM",              # 终端历史缓冲多一次 96 KiB/实例的分配：堆不够会在这里现形
    "[TERM] open failed",
]

SENS = 1.7            # kernel/input.cpp：dx * 17 / 10
STEP = 12             # 一个包的宿主像素（客人侧 = STEP*1.7 ≈ 20 < MOUSE_STEP_MAX 24）
CUR0 = (512, 384)     # kernel/input.cpp mouse_init()


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_re(path, pattern, timeout, proc=None, since=0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, slog(path)[since:])
        if m:
            return m
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return None


def last_re(path, pattern, timeout, proc=None, since=0, settle=1.4):
    """等到出现匹配后**再等 settle 秒**取最后一条 —— 滚轮连发会打多行（每包/每帧一行），
    判据要的是"最终停在哪"，不是第一条。"""
    deadline = time.time() + timeout
    seen = []
    while time.time() < deadline:
        seen = list(re.finditer(pattern, slog(path)[since:]))
        if seen:
            break
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.3)
    time.sleep(settle)
    seen = list(re.finditer(pattern, slog(path)[since:]))
    return seen[-1] if seen else None



def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("不是 P6 PPM：%r" % raw[:16])
    idx, fields = 2, []
    while len(fields) < 3:
        while idx < len(raw) and raw[idx:idx + 1].isspace():
            idx += 1
        if raw[idx:idx + 1] == b"#":
            while idx < len(raw) and raw[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while idx < len(raw) and not raw[idx:idx + 1].isspace():
            idx += 1
        fields.append(int(raw[start:idx]))
    idx += 1
    w, h, _mx = fields
    return w, h, raw[idx:idx + w * h * 3]


def diff_pixels(ppm_a, ppm_b, x0, y0, x1, y1):
    """两张同尺寸 PPM 在矩形内的差异像素数（每像素任一通道差 > 24 记 1）。"""
    wa, ha, pa = read_ppm(ppm_a)
    wb, hb, pb = read_ppm(ppm_b)
    if (wa, ha) != (wb, hb):
        return -1
    x0 = max(0, x0)
    y0 = max(0, y0)
    x1 = min(wa, x1)
    y1 = min(ha, y1)
    n = 0
    for y in range(y0, y1):
        base = (y * wa) * 3
        for x in range(x0, x1):
            o = base + x * 3
            if (abs(pa[o] - pb[o]) > 24 or abs(pa[o + 1] - pb[o + 1]) > 24
                    or abs(pa[o + 2] - pb[o + 2]) > 24):
                n += 1
    return n


class Mon:
    """QEMU monitor：sendkey 注入键盘 / mouse_move 注入相对鼠标（含滚轮 dz）。"""

    def __init__(self, port):
        self.port = port
        self.dev = qh.Monitor(port)
        self.cursor = list(CUR0)
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_wheel_")

    def send(self, cmd, wait=0.25):
        return self.dev.send(cmd, wait=wait)

    def key(self, name, wait=0.3):
        return self.dev.key(name, wait=wait)

    def mouse(self, dx, dy, dz=None):
        if dz is None:
            return self.send("mouse_move %d %d" % (dx, dy), wait=0.16)
        return self.send("mouse_move %d %d %d" % (dx, dy, dz), wait=0.16)

    def wheel(self, dz, notches=1):
        """真滚轮包。dz 是**客人内核口径**（> 0 = 上滚 / < 0 = 下滚）。
        ★ QEMU monitor 的 dz 与 PS/2 包的 Z 字节是**镜像**的（实测：注入 dz=+1 -> Z 字节 -1，
          见 tests/ipc64_test.py 的 EXPECT_WHEEL_DY = -1），所以这里发 -dz。"""
        for _ in range(notches):
            self.mouse(0, 0, -dz)
            time.sleep(0.18)

    def move_to(self, x, y, guard=60):
        tx, ty = x - self.cursor[0], y - self.cursor[1]
        while (abs(tx) > 3 or abs(ty) > 3) and guard > 0:
            guard -= 1
            sx = int(max(-STEP, min(STEP, round(tx / SENS))))
            sy = int(max(-STEP, min(STEP, round(ty / SENS))))
            if sx == 0 and sy == 0:
                break
            self.mouse(sx, sy)
            tx -= int(sx * SENS)
            ty -= int(sy * SENS)
        self.cursor = [x - tx, y - ty]
        return self.cursor

    def click(self, wait=0.35):
        self.send("mouse_button 1", wait=0.15)
        self.send("mouse_button 0", wait=wait)

    def type_line(self, text, per_key=0.12):
        names = {" ": "spc", "/": "slash", ".": "dot", "-": "minus"}
        for ch in text:
            if ch in names:
                self.key(names[ch], wait=per_key)
            elif ch.isalnum():
                self.key(ch, wait=per_key)
            else:
                raise ValueError("unsupported char: %r" % ch)
        self.key("ret", wait=per_key + 0.2)

    def shot(self, tag, wait=1.6):
        path = os.path.join(self.tmp, tag + ".ppm")
        if os.path.exists(path):
            os.remove(path)
        self.send("screendump %s" % path.replace("\\", "/"), wait=wait)
        for _ in range(20):
            if os.path.exists(path) and os.path.getsize(path) > 2048:
                return path
            time.sleep(0.3)
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\n")
        return 2
    base = p64.prepare_fixture()          # 每次重建一块**空 v3 卷**夹具（不碰 build64 产物）
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败：%s（先跑 bash build64.sh）\n" % p64.SYSTEM_IMG)
        return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_wheel64_")
    disk = os.path.join(tmp, "wheel.img")     # ★ 副本：绝不直接引导共享夹具（会被写脏）
    shutil.copyfile(base, disk)
    serial = os.path.join(tmp, "serial.log")
    port = p64_free()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-wheel", "-drive", "format=raw,file=%s" % p64.q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % p64.q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    try:
        mon = Mon(port)
        qh.login_desktop(mon.dev, serial, proc, timeout=200)
        if not wait_re(serial, r"\[GUI64\] ready", 150, proc):
            sys.stderr.write("没进桌面（[GUI64] ready 未出现）\n")
            return 2
        # 滚轮必须真的是 4 字节包（3 字节包没有 Z 轴，谁都没法滚）
        mw = wait_re(serial, r"\[INPUT64\] wheel mode=(\d) id=(\d+) packet=(\d+B)", 30, proc)
        check("PS/2 鼠标是 4 字节包（滚轮可用）",
              mw is not None and mw.group(3) == "4B",
              mw.group(0) if mw else "（无 [INPUT64] wheel 行）")

        # ==================== A) 终端：历史缓冲滚动（此时只有终端一个窗口，无遮挡）====================
        print("=== A) 终端：help 灌满历史 -> 滚轮上滚/下滚 ===")
        since = len(slog(serial))
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("1", wait=1.8)
            if "[APP] term opened" in slog(serial)[since:]:
                break
        check("开始菜单 -> 终端（[APP] term opened）", "[APP] term opened" in slog(serial)[since:])
        mon.type_line("help")
        hc = wait_re(serial, r"\[TERM\] cmd help ok", 25, proc, since=since)
        check("终端执行 help（[TERM] cmd help ok）", hc is not None, hc.group(0) if hc else "（无）")
        time.sleep(1.0)
        # 光标在 (512,384)：默认终端窗口 (16,16) 起、约 974x720，一定盖住这个点
        mon.move_to(512, 384)
        time.sleep(0.5)
        shot_c = mon.shot("term_before")
        since_t = len(slog(serial))
        mon.wheel(1, notches=2)                          # 上滚 2 格（dz > 0 = 看更早）
        ts = wait_re(serial, r"\[TERM\] scroll off=(\d+) hist=(\d+) rows=(\d+) dz=(\d+) \(up\)",
                     20, proc, since=since_t)
        check("终端收到滚轮并滚历史（[TERM] scroll off=n hist=h rows=r dz=.. (up)）",
              ts is not None, ts.group(0) if ts else "（无：滚轮没到终端窗口）")
        if ts:
            off1, hist, rows = int(ts.group(1)), int(ts.group(2)), int(ts.group(3))
            check("历史行数 >= 视口偏移 > 0（help 的输出真的进了 scrollback）",
                  off1 > 0 and hist >= off1, "off=%d hist=%d rows=%d" % (off1, hist, rows))
            time.sleep(1.5)                  # 等这一帧真的落到帧缓冲（screendump 读的是 VRAM，
                                             #   不 sleep 会读到滚动前那一帧：实测 diff=0 的假红）
            shot_d = mon.shot("term_after")
            if shot_c and shot_d:
                d = diff_pixels(shot_c, shot_d, 20, 50, 980, 700)
                check("滚动后终端客户区**像素真的变了**（差异像素 > 2000）", d > 2000, "diff=%d" % d)
            else:
                check("滚动前后 screendump 可读", False, "screendump 失败")
            since_t = len(slog(serial))
            mon.wheel(-1, notches=2)                     # 下滚 2 格 = 回最新
            ts2 = last_re(serial, r"\[TERM\] scroll off=(\d+) hist=\d+ rows=\d+ dz=\d+ \(down\)",
                          20, proc, since=since_t)
            check("下滚回最新（最后一条 (down) 行 off 归 0）",
                  ts2 is not None and int(ts2.group(1)) == 0,
                  ts2.group(0) if ts2 else "（无）")

        # ==================== B) 设置：内容区滚动（终端开着、设置在它上面）====================
        print("=== B) 设置：打开 -> 关于页（内容超一屏）-> 滚轮下滚/上滚 ===")
        since = len(slog(serial))
        for _ in range(3):
            mon.key("meta_l", wait=0.9)
            mon.key("6", wait=1.6)       # 开始菜单数字快捷键：1=终端 2=我的电脑 3=监视器 4=计算器
                                         # 5=扫雷 6=设置 7=任务管理器 8=关于（见 startmenu64.cpp kDigitApp）
            if "[APP] settings opened" in slog(serial)[since:]:
                break
        check("开始菜单 -> 设置（[APP] settings opened）",
              "[APP] settings opened" in slog(serial)[since:])
        nav = wait_re(serial, r"\[SET64\] nav item page=15 .*acx=(\d+) acy=(\d+)", 40, proc)
        check("设置左导航打出「关于」页的绝对坐标（[SET64] nav item page=15 … acx/acy）",
              nav is not None, nav.group(0)[:90] if nav else "（无）")
        if nav:
            ax, ay = int(nav.group(1)), int(nav.group(2))
            print("      点 About 导航项 (%d,%d)" % (ax, ay))
            mon.move_to(ax, ay)
            mon.click(wait=0.6)
            pg = wait_re(serial, r"\[SET64\] page=15 name=about", 20, proc, since=since)
            check("点导航切到关于页（[SET64] page=15 name=about）", pg is not None,
                  pg.group(0) if pg else "（无）")
            # 停车点用**内容区矩形**（[SET64] view page=15 name=about x= y= w= h= 是绝对坐标）：
            #   关于页最下面那个按钮已经被裁到窗口外（这正是"内容被截断"本身），不能拿它当鼠标落点。
            vw = wait_re(serial, r"\[SET64\] view page=15 name=about x=(\d+) y=(\d+) w=(\d+) h=(\d+)",
                         30, proc)
            cx, cy = (int(vw.group(1)) + int(vw.group(3)) // 2, int(vw.group(2)) + 40) \
                if vw else (ax + 260, ay)
            check("关于页给出内容区绝对矩形（[SET64] view page=15 … x/y/w/h）", vw is not None,
                  vw.group(0)[:90] if vw else "（无）")
            ctl = wait_re(serial, r"\[SET64\] ctl page=15 .*acx=(\d+) acy=(\d+)", 30, proc)
            check("关于页登记控件并给出绝对坐标（[SET64] ctl page=15 … acx/acy）", ctl is not None,
                  ctl.group(0)[:90] if ctl else "（无）")
            mon.move_to(cx, cy)
            time.sleep(0.6)
            shot_a = mon.shot("set_before")
            since_s = len(slog(serial))
            mon.wheel(-1, notches=2)                     # 下滚 2 格 = 内容向上走
            sc = wait_re(serial, r"\[SET64\] scroll off=(\d+) max=(\d+) ch=(\d+)", 20, proc, since=since_s)
            check("设置收到滚轮并滚动内容（[SET64] scroll off=n max=m ch=h）", sc is not None,
                  sc.group(0) if sc else "（无：滚轮没到设置窗口）")
            if sc:
                off1, mx1 = int(sc.group(1)), int(sc.group(2))
                check("滚动上限 max > 0（关于页内容确实超过一屏）", mx1 > 0, "max=%d" % mx1)
                check("下滚后偏移 off > 0 且不超上限", 0 < off1 <= mx1,
                      "off=%d max=%d" % (off1, mx1))
                since_s = len(slog(serial))
                mon.wheel(-1, notches=1)
                sc2 = last_re(serial, r"\[SET64\] scroll off=(\d+) max=", 15, proc, since=since_s)
                check("再下滚一格偏移继续增大",
                      sc2 is not None and int(sc2.group(1)) > off1,
                      (sc2.group(0) + " (前 off=%d)" % off1) if sc2 else "（无第二条 scroll 行）")
                shot_b = mon.shot("set_after")
                if shot_a and shot_b:
                    if vw:
                        rx0, ry0 = int(vw.group(1)), int(vw.group(2))
                        rx1, ry1 = rx0 + int(vw.group(3)) - 6, ry0 + int(vw.group(4))
                    else:
                        rx0, ry0, rx1, ry1 = 190, 66, 1090, 686
                    d = diff_pixels(shot_a, shot_b, rx0 + 2, ry0 + 2, rx1, ry1)
                    check("滚动后设置内容区**像素真的变了**（差异像素 > 2000）", d > 2000,
                          "diff=%d 区域=(%d,%d)-(%d,%d)" % (d, rx0, ry0, rx1, ry1))
                else:
                    check("滚动前后 screendump 可读", False, "screendump 失败")
                # 上滚回顶（三格足够把 2~3 格推回去）
                since_s = len(slog(serial))
                mon.wheel(1, notches=3)
                sc3 = last_re(serial, r"\[SET64\] scroll off=(\d+) max=", 15, proc, since=since_s)
                check("上滚回顶部（最后一条 scroll off=0）",
                      sc3 is not None and int(sc3.group(1)) == 0,
                      sc3.group(0) if sc3 else "（无）")

        # ==================== C) 内核自检 + 禁止行 ====================
        log = slog(serial)
        check("内核纯逻辑自检通过（[EXPL] selftest PASS：含预览行切分/夹取第 9 段）",
              "[EXPL] selftest PASS" in log)
        bad = [k for k in FORBIDDEN if k in log]
        check("无 PANIC / TRIPLE FAULT / FAILED mask= / selftest FAIL / [TERM] unsupported",
              not bad, "命中：%s" % bad if bad else "")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=8)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        if args.keep:
            print("保留临时目录：%s" % tmp)

    npass = sum(1 for _n, c in checks if c)
    print("\n=== 汇总：%d/%d 通过 ===" % (npass, len(checks)))
    for n, c in checks:
        if not c:
            print("  FAIL: %s" % n)
    return 0 if ok else 1


def p64_free():
    import socket
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


if __name__ == "__main__":
    sys.exit(main())
