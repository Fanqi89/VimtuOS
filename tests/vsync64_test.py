#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/vsync64_test.py - ★ 用户计划 ⑭ 验收：VSync + 交换链 + 混合渲染 + 防崩溃降级

覆盖（每条都用**串口打点原文 + 宿主抓屏像素**做证据，不靠"没崩"）：
  ① 撕裂硬证据：帧引擎连着播放"每帧 8 条色带整体变化"的序列（kernel/fb.cpp 的 fb64_draw_bands，
     第 f 帧第 i 条带颜色 = PAL[(i+f)%8]），宿主侧用 QEMU monitor `screendump` **高频**抓 ≥30 张，
     每张**要么整帧是第 N 帧、要么整帧是第 N+1 帧**（8 条带颜色必须能反推出**同一个**帧号）；
     出现两个不同帧的带序 = 撕裂 = FAIL。设备路径（-vga virtio + SET_SCANOUT 整帧翻页）必须 0 撕裂。
  ② 帧节拍：`[VSYNC] stats` 的帧间隔中位数 vs 目标刷新率的偏差、p95、抖动；双缓冲/三缓冲各测一次并报差异。
  ③ GPU vs CPU：`[VSYNC] bench present ... gpu_med_us=.. soft_med_us=..`（同一帧内容，设备整帧提交
     vs 软件整帧写 LFB）+ `gpu=off` 强制软件路径跑**同一套功能断言**。
  ④ 降级：注入（fw_cfg inject=timeout / illegal / gone）之后必须出现 `[VGPU] INJECT` +
     `[VGPU] disable reason=`（或 `[FB64] present dev=0 -> soft-lfb`）+ `[VSYNC] degrade`，
     随后**继续出帧** ≥ N 帧、屏幕继续推进、全程没有 PANIC。

配置通道：内核侧整块功能由 QEMU fw_cfg 的文件 `opt/vimtu/vsync` 开启（kernel/fb.cpp 的 fb64_cfg_load64），
   命令行写法：-fw_cfg name=opt/vimtu/vsync,string=demo=1;bufs=2;gpu=auto;frames=1500;fps=30
   （用 ';' 分隔，**值里不能有逗号** —— QEMU 的 -fw_cfg 按逗号切属性）。没有该文件时内核一行不打、
   既有启动路径零影响（回归脚本因此完全不受影响）。

用法：
  py -3 tests\\vsync64_test.py                 # 全跑（撕裂 bufs=2/bufs=3 + gpu=off + 降级注入）
  py -3 tests\\vsync64_test.py --only tear     # 只跑撕裂（设备路径 bufs=2）
  py -3 tests\\vsync64_test.py --regress       # 追加跑既有回归脚本并汇总 RESULT 行
退出码：0 = 全部通过；1 = 有断言失败；2 = 环境问题（QEMU/镜像/监控口等）。
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

FORBIDDEN = ("PANIC", "TRIPLE FAULT", "FAILED mask=", "OOM:")

# 与 kernel/fb.cpp 的 g_vs_pal[8] **逐值一致**（顺序不能改，验收靠它反推帧号）
BAND_PAL = [(0xFF, 0x00, 0x00), (0x00, 0xFF, 0x00), (0x00, 0x00, 0xFF), (0xFF, 0xFF, 0x00),
            (0xFF, 0x00, 0xFF), (0x00, 0xFF, 0xFF), (0xFF, 0x80, 0x00), (0x80, 0x00, 0xFF)]
TOL = 24                      # 单通道容差（宿主/设备色彩管线可能有 ±1 抖动）


# --------------------------------------------------------------------------- PPM / 色带工具
def read_ppm(path):
    with open(path, "rb") as f:
        raw = f.read()
    if not raw.startswith(b"P6"):
        raise ValueError("not P6 PPM: %r" % raw[:16])
    idx = 2
    fields = []
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
    w, h, _ = fields
    return w, h, raw[idx:]


def px(body, w, x, y):
    o = (y * w + x) * 3
    return body[o], body[o + 1], body[o + 2]


def band_color(body, w, h, band, xs=None):
    """第 band 条带（0..7）的**众数颜色**：在带中心行上多点取样，取出现最多的那一色。
    返回 (color, ok, detail)：ok=False 表示这一带内部颜色不单一（同一带里两种颜色 = 采样行跨界或撕裂）。"""
    y0 = int(h * band / 8)
    y1 = int(h * (band + 1) / 8)
    ys = [y0 + max(1, (y1 - y0) // 4), y0 + max(1, (y1 - y0) // 2), y0 + max(1, (y1 - y0) * 3 // 4)]
    if xs is None:
        xs = [w // 8, w // 4, w // 2, w * 3 // 4, w * 7 // 8]
    cnt = {}
    for y in ys:
        for x in xs:
            c = px(body, w, x, y)
            cnt[c] = cnt.get(c, 0) + 1
    best = max(cnt.items(), key=lambda kv: kv[1])
    return best[0], len(cnt) == 1, cnt


def pal_index(c):
    """把观测颜色映射到调色板下标；不匹配任何调色板 -> -1。"""
    for i, p in enumerate(BAND_PAL):
        if all(abs(c[k] - p[k]) <= TOL for k in range(3)):
            return i
    return -1


def frame_of_shot(body, w, h):
    """反推这张截图的帧号：8 条带的调色板下标必须是 PAL 的一个**整体移位**。
    返回 (frame, detail)：frame >= 0 = 整帧一致；frame = -1 = 混合（撕裂）；frame = -2 = 不是色带画面。
    第 f 帧的第 i 条带 = PAL[(i+f)%8] -> 由第 0 条带的下标 pi0 得 f = (pi0 - 0) % 8 再看其余是否吻合。"""
    idxs = []
    for b in range(8):
        c, ok, cnt = band_color(body, w, h, b)
        pi = pal_index(c)
        if pi < 0:
            return -2, "band%d=%s 不是调色板色 (%s)" % (b, c, cnt)
        idxs.append(pi)
    if len(set(idxs)) != 8:
        return -2, "带色重复 idxs=%s" % (idxs,)
    f = idxs[0]
    for b in range(8):
        if idxs[b] != (b + f) % 8:
            return -1, "MIXED idxs=%s (应为 f=%d 的 %s)" % (idxs, f, [(i + f) % 8 for i in range(8)])
    return f, "idxs=%s" % (idxs,)


def percentile(vals, p):
    if not vals:
        return 0
    s = sorted(vals)
    i = int(len(s) * p / 100.0)
    if i >= len(s):
        i = len(s) - 1
    return s[i]


# ★ 修（实测缺陷）：screendump 的**文件名必须 ASCII** —— QEMU 是原生 Windows 程序，按 ANSI(GBK)
#   落盘；Python 拿 Unicode 路径去 stat 会永远找不到文件（实测：中文 tag 的用例 300 s 抓 0 张，
#   纯 ASCII tag 的用例 32 张/1 s）。中文只留在断言文案里，文件名走 file_tag()。
def file_tag(tag):
    import re as _re
    t = _re.sub(r"[^0-9A-Za-z._=+-]", "_", tag)
    return t or "shot"

# --------------------------------------------------------------------------- QEMU 壳
class Vm:
    def __init__(self, qemu, img, vga, port, tmp, cfg):
        self.vga = vga
        self.cfg = cfg
        self.serial = os.path.join(tmp, "%s_serial.log" % vga)
        self.shots = os.path.join(tmp, "shots_%s" % vga)
        os.makedirs(self.shots, exist_ok=True)
        args = [qemu, "-name", "vimtu-vsync-%s" % vga,
                "-drive", "format=raw,file=%s" % img,
                "-boot", "order=c", "-m", "512", "-vga", vga,
                "-display", "none", "-serial", "file:%s" % self.serial,
                "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port]
        if cfg:
            args += ["-fw_cfg", "name=opt/vimtu/vsync,string=%s" % cfg]
        self.cmdline = " ".join(args)
        with open(self.serial, "w", encoding="utf-8") as f:
            f.write("")
        self.proc = subprocess.Popen(args)
        self.port = port
        self.sock = None
        self._mon_connect()

    # ---- monitor（一条长连接，反复 screendump —— "高频抓屏"就这么来的）----
    def _mon_connect(self, timeout=15):
        deadline = time.time() + timeout
        while time.time() < deadline:
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=5)
                s.settimeout(0.2)
                s.sendall(b"\n")
                self.sock = s
                return
            except OSError:
                time.sleep(0.25)
        raise RuntimeError("QEMU monitor 连不上 127.0.0.1:%d" % self.port)

    def mon_cmd(self, line):
        if self.sock is None:
            self._mon_connect()
        self.sock.sendall(line.encode() + b"\n")

    def shot(self, tag, max_wait=4.0):
        """发一条 screendump 并等文件写稳（大小两次一致）。返回 (path, w, h, body) 或 None。"""
        path = os.path.join(self.shots, "%s.ppm" % tag)
        if os.path.exists(path):
            os.remove(path)
        try:
            self.mon_cmd("screendump %s" % path.replace("\\", "/"))
        except OSError:
            self._mon_connect()
            self.mon_cmd("screendump %s" % path.replace("\\", "/"))
        deadline = time.time() + max_wait
        last = -1
        while time.time() < deadline:
            if os.path.exists(path):
                try:
                    sz = os.path.getsize(path)
                except OSError:
                    sz = -1
                if sz > 0 and sz == last:
                    try:
                        w, h, body = read_ppm(path)
                        if len(body) >= w * h * 3:
                            return path, w, h, body
                    except (ValueError, OSError):
                        pass
                last = sz
            time.sleep(0.01)
        return None

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait_log(self, needle, timeout):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log():
                return True
            if self.proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def close(self):
        try:
            if self.sock:
                self.sock.close()
        except OSError:
            pass
        try:
            self.proc.terminate()
            self.proc.wait(timeout=10)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass


# --------------------------------------------------------------------------- 断言收集
class Checks:
    def __init__(self):
        self.items = []

    def ch(self, name, ok, detail=""):
        self.items.append((name, bool(ok), detail))
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail else ""))
        return bool(ok)

    def info(self, name, detail):
        self.items.append((name, True, detail))
        print("  [INFO] %s -- %s" % (name, detail))

    def fails(self):
        return [x for x in self.items if not x[1]]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


# --------------------------------------------------------------------------- ① 撕裂 + ② 节拍
def run_tear(qemu, img, tmp, cfg_str, vga, expect_gpu, n_shots, ck, tag):
    """抓 n_shots 张，逐张判"整帧一致"；同时从 stats 行取节拍统计。返回 (shots, mixed, frames_seen, stats)。"""
    port = free_port()
    vm = Vm(qemu, img, vga, port, tmp, cfg_str)
    try:
        if not vm.wait_log("[VSYNC] demo start", 300):
            ck.ch("%s 帧引擎起来了（[VSYNC] demo start）" % tag, False, "300s 内没看到（串口尾：%s）" % vm.log()[-400:])
            return None
        start = vm.log()
        m = re.search(r"\[VSYNC\] demo start mode=(\S+) bufs=(\d+) gpu=(\S+) backend=(\S+) dev_frames=(\d+) "
                      r"target_us=(\d+) frames=(\d+) vblank_edges=(\d+) vblank_ms=(\d+) in_place=(\d)", start)
        ck.ch("%s 帧引擎打点可直接解析（mode/bufs/gpu/backend/target_us/frames）" % tag, m is not None,
              (m.group(0) if m else "regex 未命中"))
        if m:
            mode, bufs, gpu_s, backend, dev_frames, target_us, frames, vb_edges, vb_ms, in_place = m.groups()
            ck.info("%s 帧节拍来源 mode=%s（vblank_edges=%s / vblank_ms=%s ms）" % (tag, mode, vb_edges, vb_ms),
                    "有硬件回扫 -> hw；没有 -> pace（按刷新率估计 + 睡眠/自旋混合）")
            ck.ch("%s 交换链块数 = 请求值（%s）" % (tag, cfg_bufs(cfg_str)), int(bufs) == int(cfg_bufs(cfg_str)),
                  "bufs=%s" % bufs)
            if expect_gpu:
                ck.ch("%s 设备路径真的建起了交换链（dev_frames>=2 backend=%s）" % (tag, backend),
                      int(dev_frames) >= 2, "dev_frames=%s backend=%s" % (dev_frames, backend))
            else:
                ck.ch("%s gpu=off 强制软件路径（in_place=1）" % tag, in_place == "1" and gpu_s == "off",
                      "gpu=%s in_place=%s" % (gpu_s, in_place))
        shots = []
        mixed = []
        frames_seen = []
        max_span = 0.0
        t0 = time.time()
        i = 0
        # ★ 修：①文件名走 ASCII（见 file_tag）；②截图之间留 ~0.12 s —— screendump 会短暂暂停
        #   客户机，贴着连拍时帧号几乎不动，"≥3 个不同帧号"会变成假失败；③循环条件带上
        #   "≥3 个不同帧号"，否则 32 张连拍可能只覆盖 1~2 帧。
        while (len(shots) < n_shots or len(set(frames_seen)) < 3) and time.time() - t0 < 300:
            if "[VSYNC] demo done" in vm.log():
                break
            sh = vm.shot("%s_%03d" % (file_tag(tag), i))
            i += 1
            if sh is None:
                continue
            path, w, h, body = sh
            shots.append((path, w, h, body))
            f, det = frame_of_shot(body, w, h)
            if f == -1:
                mixed.append((path, det))
            elif f >= 0:
                frames_seen.append(f)
            dt = time.time() - t0
            if len(shots) % 8 == 0:
                print("    ... 抓了 %d 张（%.1fs，最新 frame=%s，不同帧号=%d）"
                      % (len(shots), dt, f, len(set(frames_seen))))
            time.sleep(0.12)
        max_span = time.time() - t0
        # ★ 修：`[VSYNC] stats` / `[VSYNC] bench` 只在引擎**跑满 frames 帧之后**才打；实测引擎
        #   真实速率远低于 30 fps 的名义节拍（screendump 还会暂停客户机），截图循环结束时通常
        #   还没跑完 -> 这里按有界上限等它收尾，否则 stats/bench 必然"未命中"（假失败）。
        if "[VSYNC] stats" not in vm.log():
            vm.wait_log("[VSYNC] stats", 420)
        log = vm.log()
        ck.ch("%s 演示窗口内抓到 >= %d 张（实际 %d 张，%.1fs）" % (tag, n_shots, len(shots), max_span),
              len(shots) >= n_shots, "shots=%d" % len(shots))
        if expect_gpu:
            ck.ch("%s **撕裂硬证据**：0 张混合帧（每张整帧只属于一个帧号）" % tag, len(mixed) == 0,
                  "mixed=%d %s" % (len(mixed), mixed[:2]))
        else:
            ck.info("%s 软件路径撕裂计数（原地写 LFB，Bochs VBE 无页翻转原语）" % tag,
                    "mixed=%d / %d 张（如实记录，不作硬断言：见报告边界）" % (len(mixed), len(shots)))
        dist = sorted(set(frames_seen))
        ck.ch("%s 屏幕帧号确实在推进（>=3 个不同帧号）" % tag, len(dist) >= 3,
              "帧号集合（前 12 个）=%s" % (dist[:12],))
        st = re.search(r"\[VSYNC\] stats frames=(\d+) bufs=(\d+) mode=(\S+) target_us=(\d+) med_us=(\d+) "
                       r"p95_us=(\d+) jitter_us=(\d+) samples=(\d+) gpu_frames=(\d+) cpu_frames=(\d+)", log)
        ck.ch("%s [VSYNC] stats 打点（帧间隔统计）" % tag, st is not None, (st.group(0) if st else "未命中"))
        bench = re.search(r"\[VSYNC\] bench present_n=(\d+) px=(\d+) gpu_med_us=(\d+) soft_med_us=(\d+) "
                          r"ratio_x100=(\d+)", log)
        ck.ch("%s [VSYNC] bench（设备 vs 软件整帧提交耗时）" % tag, bench is not None,
              (bench.group(0) if bench else "未命中"))
        ck.ch("%s 全程无 PANIC/三连异常" % tag, not any(x in log for x in FORBIDDEN),
              "命中=%s" % [x for x in FORBIDDEN if x in log])
        stats = {}
        if st:
            stats = {
                "frames": int(st.group(1)), "bufs": int(st.group(2)), "mode": st.group(3),
                "target_us": int(st.group(4)), "med_us": int(st.group(5)), "p95_us": int(st.group(6)),
                "jitter_us": int(st.group(7)), "samples": int(st.group(8)),
                "gpu_frames": int(st.group(9)), "cpu_frames": int(st.group(10)),
            }
        if bench:
            stats["gpu_med_us"] = int(bench.group(3))
            stats["soft_med_us"] = int(bench.group(4))
            stats["ratio_x100"] = int(bench.group(5))
        return {"shots": len(shots), "mixed": mixed, "frames": frames_seen, "stats": stats, "log": log}
    finally:
        vm.close()


def cfg_bufs(cfg):
    m = re.search(r"bufs=(\d+)", cfg)
    return m.group(1) if m else "2"


def check_pace(ck, tag, r, tol_med=0.15, p95_extra=8000):
    """② 帧节拍：中位数 vs 目标、p95、抖动（阈值依据写在这里，别只报数字）。"""
    s = r["stats"]
    tgt = s.get("target_us", 0)
    med = s.get("med_us", 0)
    p95 = s.get("p95_us", 0)
    jit = s.get("jitter_us", 0)
    if not tgt or not med:
        ck.ch("%s 节拍统计可用（target/med 非 0）" % tag, False, "stats=%s" % s)
        return
    dev = abs(med - tgt) / float(tgt)
    ck.ch("%s 帧间隔中位数 vs 目标刷新率（偏差 <= %.0f%%）" % (tag, tol_med * 100), dev <= tol_med,
          "target=%dus med=%dus dev=%.1f%%（依据：节拍器 1 ms 细等 + TCG 每帧工作量）" % (tgt, med, dev * 100))
    ck.ch("%s p95 <= 目标 + %dus" % (tag, p95_extra), p95 <= tgt + p95_extra,
          "p95=%dus（target=%dus，抖动上限按 1.5 帧 + 8ms 给）" % (p95, tgt))
    ck.ch("%s 抖动（相对中位数平均绝对偏差）<= 目标 35%%" % tag, jit <= tgt * 0.35,
          "jitter=%dus target=%dus samples=%d frames=%d" % (jit, tgt, s.get("samples", 0), s.get("frames", 0)))
    ck.info("%s 节拍原文" % tag, "mode=%s target_us=%d med_us=%d p95_us=%d jitter_us=%d gpu_frames=%d cpu_frames=%d" %
            (s.get("mode"), tgt, med, p95, jit, s.get("gpu_frames", 0), s.get("cpu_frames", 0)))


# --------------------------------------------------------------------------- ④ 降级注入
def run_degrade(qemu, img, tmp, frame_str, vga, inj, inj_at, ck, min_after=60):
    cfg = frame_str % {"inject": "inject=%s;" % inj, "inj_at": "inj_at=%d;" % inj_at}
    tag = "降级(%s)" % inj
    port = free_port()
    vm = Vm(qemu, img, vga, port, tmp, cfg)
    try:
        if not vm.wait_log("[VSYNC] demo done", 600):
            ck.ch("%s 帧引擎整段跑完（[VSYNC] demo done）" % tag, False, "串口尾：%s" % vm.log()[-400:])
            return
        log = vm.log()
        ck.ch("%s 注入真的命中了（[VGPU] INJECT kind=..）" % tag, "[VGPU] INJECT" in log,
              _grab_line(log, "[VGPU] INJECT"))
        ck.ch("%s 注入后驱动降级（[VGPU] disable reason=.. 或 [FB64] present dev=0）" % tag,
              ("[VGPU] disable reason=" in log) or ("[FB64] present dev=0" in log),
              _grab_line(log, "[VGPU] disable reason=") or _grab_line(log, "[FB64] present dev=0"))
        m = re.search(r"\[VSYNC\] degrade frame=(\d+) reason=(\S+) -> path=(\S+) backend=(\S+)", log)
        ck.ch("%s 帧引擎如实报出降级（[VSYNC] degrade frame=.. reason=.. -> path=soft）" % tag, m is not None,
              (m.group(0) if m else "未命中"))
        dfl = int(m.group(1)) if m else -1
        st = re.search(r"\[VSYNC\] stats frames=(\d+) bufs=\d+ mode=\S+ target_us=\d+ med_us=(\d+) p95_us=(\d+) "
                       r"jitter_us=(\d+) samples=\d+ gpu_frames=(\d+) cpu_frames=(\d+)", log)
        frames = int(st.group(1)) if st else 0
        cpu_frames = int(st.group(6)) if st else 0
        ck.ch("%s 降级后**继续出帧** >= %d 帧（降级帧=%d 总帧=%d，cpu_frames=%d）" % (tag, min_after, dfl, frames, cpu_frames),
              frames - dfl >= min_after and dfl >= 0, "frames=%d degrade_at=%d" % (frames, dfl))
        ck.ch("%s 降级后确实全走 CPU 路径（cpu_frames >= %d）" % (tag, min_after), cpu_frames >= min_after,
              "cpu_frames=%d gpu_frames=%s" % (cpu_frames, (st.group(5) if st else "?")))
        ck.ch("%s 降级后无 PANIC、系统继续可用（帧引擎跑完 + 后续启动继续）" % tag,
              not any(x in log for x in FORBIDDEN), "命中=%s" % [x for x in FORBIDDEN if x in log])
        ck.info("%s 降级打点原文" % tag, (m.group(0) if m else "-"))
    finally:
        vm.close()


def _grab_line(log, needle):
    for ln in log.splitlines():
        if needle in ln:
            return ln.strip()
    return ""


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    _qg = next((_p for _p in (r"C:\Program Files\qemu\qemu-system-x86_64.exe",
                              r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe") if os.path.exists(_p)),
               "qemu-system-x86_64")
    ap.add_argument("--qemu", default=os.environ.get("VIMTU_QEMU", _qg))
    ap.add_argument("--img", default=os.path.join(ROOT, "build64", "sysdisk.img"))
    ap.add_argument("--only", default="all", choices=["all", "tear", "gpuoff", "degrade", "regress"])
    ap.add_argument("--shots", type=int, default=32, help="撕裂用例最少抓多少张（规范要求 >=30）")
    ap.add_argument("--frames", type=int, default=600,
                    help="帧引擎跑多少帧（演示窗口长度）。实测引擎真实速率 ~3~6 帧/秒（30 fps 只是"
                         "节拍目标；TCG 下每帧整帧提交本身就是毫秒级），1500 帧要 5~8 分钟，"
                         "默认降到 600（约 2~3 分钟/用例，仍远超 ④ 要求的降级后 >=60 帧）")
    ap.add_argument("--fps", type=int, default=30, help="目标刷新率（TCG 下 30 更现实；如实报告）")
    ap.add_argument("--inj-at", type=int, default=150, help="第几条 virtio-gpu 命令后注入")
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--regress", action="store_true", help="追加跑既有回归脚本")
    args = ap.parse_args()

    if not os.path.exists(args.img):
        print("环境问题：找不到镜像 %s（先 bash build64.sh）" % args.img)
        return 2
    if not _qemu_ok(args.qemu):
        print("环境问题：QEMU 不可用 %s" % args.qemu)
        return 2

    ck = Checks()
    tmp = tempfile.mkdtemp(prefix="vsync64_")
    print("== vsync64_test: img=%s fps=%d frames=%d shots>=%d tmp=%s" % (args.img, args.fps, args.frames, args.shots, tmp))

    tear_cfg = "demo=1;bufs=%%(bufs)s;gpu=auto;frames=%d;fps=%d;bench=1" % (args.frames, args.fps)
    gpuoff_cfg = "demo=1;bufs=2;gpu=off;frames=%d;fps=%d;bench=1" % (args.frames, args.fps)
    frame_str = "demo=1;bufs=2;gpu=auto;frames=%d;fps=%d;bench=1;%%(inject)s%%(inj_at)s" % (args.frames, args.fps)

    results = {}
    try:
        if args.only in ("all", "tear"):
            for bufs in (2, 3):
                tag = "撕裂bufs=%d" % bufs
                print("\n---- ① 撕裂/② 节拍/③ GPU-vs-CPU：设备路径 bufs=%d（-vga virtio）----" % bufs)
                r = run_tear(args.qemu, args.img, tmp, tear_cfg % {"bufs": bufs}, "virtio", True, args.shots, ck, tag)
                if r:
                    results[bufs] = r
                    check_pace(ck, tag, r)
                    if r["stats"].get("gpu_med_us") is not None:
                        ck.ch("%s 设备整帧提交比软件整帧写 LFB 快（ratio_x100<100）" % tag,
                              r["stats"]["ratio_x100"] < 100,
                              "gpu_med_us=%d soft_med_us=%d ratio=%d%%" %
                              (r["stats"]["gpu_med_us"], r["stats"]["soft_med_us"], r["stats"]["ratio_x100"]))
            if 2 in results and 3 in results:
                a, b = results[2]["stats"], results[3]["stats"]
                ck.info("双缓冲 vs 三缓冲差异",
                        "bufs=2 med_us=%d p95=%d jitter=%d / bufs=3 med_us=%d p95=%d jitter=%d；撕裂 2:%d 3:%d" %
                        (a.get("med_us", 0), a.get("p95_us", 0), a.get("jitter_us", 0),
                         b.get("med_us", 0), b.get("p95_us", 0), b.get("jitter_us", 0),
                         len(results[2]["mixed"]), len(results[3]["mixed"])))

        if args.only in ("all", "gpuoff"):
            print("\n---- ③ gpu=off 强制软件路径（-vga std，同一套功能断言）----")
            r = run_tear(args.qemu, args.img, tmp, gpuoff_cfg, "std", False, args.shots, ck, "gpu=off")
            if r:
                s = r["stats"]
                ck.ch("gpu=off 跑满全部帧（cpu_frames == frames）",
                      s.get("frames", 0) > 0 and s.get("cpu_frames", 0) == s.get("frames", 0),
                      "frames=%s cpu_frames=%s" % (s.get("frames"), s.get("cpu_frames")))
                ck.ch("gpu=off 也有帧间隔统计（med/p95/jitter 非 0）",
                      s.get("med_us", 0) > 0 and s.get("p95_us", 0) > 0 and s.get("jitter_us", 0) >= 0,
                      "med=%s p95=%s jitter=%s" % (s.get("med_us"), s.get("p95_us"), s.get("jitter_us")))
                check_pace(ck, "gpu=off", r)

        if args.only in ("all", "degrade"):
            for inj in ("timeout", "illegal", "gone"):
                print("\n---- ④ 降级注入 inject=%s（设备路径 -> 自动降级 CPU，继续出帧）----" % inj)
                run_degrade(args.qemu, args.img, tmp, frame_str, "virtio", inj, args.inj_at, ck)

        if args.only == "regress" or args.regress:
            print("\n---- 回归：既有验收脚本 RESULT 行 ----")
            for script, needle in (("a42a64_test.py", "89/89"), ("textwm64_test.py", "RESULT"),
                                   ("virtiogpu64_test.py", "RESULT"), ("gui_modern64_test.py", "RESULT"),
                                   ("iconboot64_test.py", "RESULT")):
                path = os.path.join(HERE, script)
                if not os.path.exists(path):
                    ck.ch("回归 %s 存在" % script, False, "缺文件")
                    continue
                t0 = time.time()
                p = subprocess.run([sys.executable, path], cwd=ROOT, capture_output=True, text=True, timeout=5400)
                lines = [ln for ln in (p.stdout or "").splitlines() if "RESULT" in ln]
                last = lines[-1].strip() if lines else ""
                ok = (p.returncode == 0) and (("失败" not in last) if last else True)
                ck.ch("回归 %s 退出码 0 且 RESULT 全通过（%.0fs）" % (script, time.time() - t0), ok,
                      "rc=%d RESULT=%s" % (p.returncode, last or "(无 RESULT 行)"))
    finally:
        if not args.keep:
            try:
                import shutil
                shutil.rmtree(tmp, ignore_errors=True)
            except Exception:
                pass

    f = ck.fails()
    print("\n== RESULT: %d 项断言，失败 %d；%s" %
          (len(ck.items), len(f), "PASS" if not f else "FAIL"))
    for name, _, det in f:
        print("   FAIL: %s -- %s" % (name, det))
    return 0 if not f else 1


def _qemu_ok(qemu):
    try:
        p = subprocess.run([qemu, "--version"], capture_output=True, text=True, timeout=30)
        return p.returncode == 0
    except Exception:
        return False


if __name__ == "__main__":
    sys.exit(main())
