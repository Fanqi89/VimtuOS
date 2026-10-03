#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/sounds_gen.py - ★ 系统音效：4 段素材的**可复现合成器**（宿主侧，纯标准库）

输出 4 个 WAV：48 kHz / 16-bit / 立体声 / PCM —— 就是内核 hda64 的**唯一**流格式，
也是音频 ABI（int 0x80 号 49 `audio_play`）唯一接受的 format（0x11）：

    startup  0.700 s  开机 / 登录完成（两声上行短和弦：D5 -> A5）
    notify   0.150 s  通知（单声 C6 ping）
    click    0.030 s  点击（很短的 2.2 kHz tick）
    error    0.250 s  错误（两声下行方波 buzz：B4 -> F#4）

★ 素材来源与许可：**全部由本脚本合成**（正弦/方波 + 音量包络），没有采样任何第三方音频、
  也没有引用任何现成素材库/字体 —— 因此**不存在许可问题**（不需要 CC0/公共领域声明：
  这里没有外部作品；本脚本与它生成的 wav 与内核一起按仓库自身的许可发布）。
★ 可复现：无随机数、无时间戳、参数全部写死在 SOURCES 里；同一版本 Python 生成的字节逐字节相同
  （验收 tests/sounds64_test.py 会用宿主侧重算的 sha256 与**卷内读回**的字节对照）。
★ 立体声两声道同相（L==R）：单声道素材复制到双声道，纯粹为了让格式与驱动一致。

用法：
    py -3 tools/sounds_gen.py [--out-dir build64/sounds] [--quiet]
退出码：0 = 全部生成并自检通过；1 = 自检失败（长度/峰值/格式不符）；2 = 参数/环境问题。
"""
import argparse
import hashlib
import math
import os
import sys
import wave

RATE = 48000
CH = 2
BITS = 16

# ---- 素材参数表（唯一真源；改这里 = 改素材，脚本输出与宿主统计跟着变）-------------------------
#   (起始 ms, 时长 ms, 频率 Hz, 波形, 峰值(0..32767), 起音 ms, 衰减 tau ms)
SOURCES = {
    # 开机/登录完成：D5 起手、A5 落定（两音一小段交叠），像"系统起来了"
    "startup": {
        "peak_target": 14000,
        "segments": [
            (0.0, 330.0, 587.33, "sine", 15000, 10.0, 150.0),
            (320.0, 380.0, 880.00, "sine", 15000, 10.0, 210.0),
        ],
        "total_ms": 700.0,
    },
    # 通知：单声 C6 ping，快起音 + 短衰减
    "notify": {
        "peak_target": 12000,
        "segments": [(0.0, 150.0, 1046.50, "sine", 15000, 4.0, 45.0)],
        "total_ms": 150.0,
    },
    # 点击：30 ms 的极小 tick（峰值刻意压低，避免交互时刺耳）
    "click": {
        "peak_target": 6000,
        "segments": [(0.0, 30.0, 2200.00, "sine", 8000, 1.5, 9.0)],
        "total_ms": 30.0,
    },
    # 错误：两声下行方波（B4 -> F#4），buzz 感
    "error": {
        "peak_target": 22000,
        "segments": [
            (0.0, 135.0, 493.88, "sq", 16000, 5.0, 95.0),
            (150.0, 100.0, 369.99, "sq", 16000, 5.0, 75.0),
        ],
        "total_ms": 250.0,
    },

# ★ 峰值口径：每个素材渲染完（含收尾淡出）后**归一化到 peak_target**，所以"宿主侧量到的峰值"
#   从一开始就是定死的数字（验收直接对照采样级峰值；段里的 peak 只是各段之间的相对权重）。
}

# 正弦叠 3/5 次谐波（听感更像"乐器"而不是纯正弦）；归一化因子 = 1 + 0.35 + 0.12
SINE_HARM = (1.0, 0.0, 0.35, 0.0, 0.12)
SINE_NORM = sum(SINE_HARM)
SQ_NORM = 1.0


def render(name):
    """把一个 SOURCES 条目渲染成 int16 的**单声道**帧列表（确定性；无浮点随机源）。"""
    spec = SOURCES[name]
    n = int(round(RATE * spec["total_ms"] / 1000.0))
    acc = [0.0] * n
    for (t0_ms, dur_ms, freq, kind, peak, atk_ms, tau_ms) in spec["segments"]:
        i0 = int(round(RATE * t0_ms / 1000.0))
        i1 = min(n, i0 + int(round(RATE * dur_ms / 1000.0)))
        atk = max(1, int(round(RATE * atk_ms / 1000.0)))
        tau = max(1.0, RATE * tau_ms / 1000.0)
        norm = SINE_NORM if kind == "sine" else SQ_NORM
        for i in range(i0, i1):
            k = i - i0
            env = min(1.0, k / float(atk)) * math.exp(-k / tau)
            if env <= 0.0:
                continue
            ph = 2.0 * math.pi * freq * (k / float(RATE))
            if kind == "sq":
                v = 1.0 if math.sin(ph) >= 0.0 else -1.0
            else:
                v = (math.sin(ph) + SINE_HARM[2] * math.sin(3.0 * ph)
                     + SINE_HARM[4] * math.sin(5.0 * ph))
            acc[i] += peak * env * v / norm
    # 收尾淡出：保证素材**边界处是静音**（验收按"非静音区段"量时长/峰值，边界干净才好量）
    fade = min(int(round(RATE * 0.020)), n // 4)
    for k in range(fade):
        acc[n - fade + k] *= (fade - k) / float(fade + 1)
    # 归一化到 peak_target（见 SOURCES 上面那段说明）
    mx = 0.0
    for v in acc:
        a = -v if v < 0.0 else v
        if a > mx:
            mx = a
    gain = (spec["peak_target"] / mx) if mx > 0.0 else 1.0
    out = []
    for v in acc:
        iv = int(round(v * gain))
        if iv > 32767:
            iv = 32767
        elif iv < -32768:
            iv = -32768
        out.append(iv)
    return out


def write_wav(path, mono):
    """写 48k/16/2 的 WAV（两声道同相）；返回 (frames, peak)。"""
    peak = 0
    pcm = bytearray()
    for s in mono:
        a = -s if s < 0 else s
        if a > peak:
            peak = a
        b = (s & 0xFFFF).to_bytes(2, "little")
        pcm += b + b                      # L == R
    frames = len(mono)
    with wave.open(path, "wb") as w:
        w.setnchannels(CH)
        w.setsampwidth(BITS // 8)
        w.setframerate(RATE)
        w.writeframes(bytes(pcm))
    return frames, peak


def fnv1a32(b):
    h = 0x811C9DC5
    for x in b:
        h ^= x
        h = (h * 0x01000193) & 0xFFFFFFFF
    return h


def stats(path):
    """独立地**重新解析**宿主侧 wav（不看写入时的状态），返回统计字典。"""
    raw = open(path, "rb").read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("不是 RIFF/WAVE：%s" % path)
    i, fmt, data_off = 12, None, None
    while i + 8 <= len(raw):
        cid = raw[i:i + 4]
        sz = int.from_bytes(raw[i + 4:i + 8], "little")
        if cid == b"fmt ":
            fmt = raw[i + 8:i + 8 + sz]
        elif cid == b"data":
            data_off = i + 8
            data_sz = sz if sz and i + 8 + sz <= len(raw) else len(raw) - i - 8
            break
        i += 8 + sz + (sz & 1)
    ch = int.from_bytes(fmt[2:4], "little")
    rate = int.from_bytes(fmt[4:8], "little")
    bits = int.from_bytes(fmt[14:16], "little")
    pcm = raw[data_off:data_off + data_sz]
    frames = len(pcm) // (2 * ch)
    peak = 0
    for k in range(frames * ch):
        v = int.from_bytes(pcm[2 * k:2 * k + 2], "little", signed=True)
        if abs(v) > peak:
            peak = abs(v)
    return {
        "path": path, "file_bytes": len(raw), "pcm_bytes": len(pcm), "rate": rate,
        "ch": ch, "bits": bits, "frames": frames, "peak": peak,
        "dur_ms": frames * 1000.0 / rate, "fnv1a32": fnv1a32(pcm),
        "sha256": hashlib.sha256(raw).hexdigest(),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=os.path.join("build64", "sounds"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    os.makedirs(args.out_dir, exist_ok=True)
    bad = 0
    for name in ("startup", "notify", "click", "error"):
        if name not in SOURCES:
            sys.stderr.write("缺素材定义：%s\n" % name)
            return 2
        path = os.path.join(args.out_dir, name + ".wav")
        mono = render(name)
        frames, wrote_peak = write_wav(path, mono)
        st = stats(path)
        want_frames = int(round(RATE * SOURCES[name]["total_ms"] / 1000.0))
        ok = (st["rate"] == RATE and st["ch"] == CH and st["bits"] == BITS
              and st["frames"] == want_frames and st["frames"] == frames
              and wrote_peak == st["peak"] and st["peak"] > 0)
        if not ok:
            bad += 1
        if not args.quiet:
            print("  [%s] %-7s %d Hz/%dch/%dbit frames=%d dur=%.1f ms file=%d B pcm=%d B "
                  "peak=%d fnv1a32=0x%08x sha256=%s"
                  % ("OK" if ok else "BAD", name, st["rate"], st["ch"], st["bits"],
                     st["frames"], st["dur_ms"], st["file_bytes"], st["pcm_bytes"],
                     st["peak"], st["fnv1a32"], st["sha256"][:16]))
    if bad:
        sys.stderr.write("素材自检失败 %d 项\n" % bad)
        return 1
    if not args.quiet:
        print("  4 段素材已生成（自合成 / 零第三方素材）：%s" % os.path.abspath(args.out_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
