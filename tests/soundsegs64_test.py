#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""③ 音效取证：从 QEMU `-audiodev wav` 的录音里切出**非静音区段**，给出峰值/RMS/出现时刻。

为什么单独一个脚本：`tests/sounds64_test.py` 的 ⑧ 段只断言"整窗峰值 >= 阈值"，
报告要的是**每一段**的 peak / RMS / 出现时刻（秒，相对录音起点）。这个脚本只读录音、
只做度量，不碰任何被测代码 —— 因此它既是证据工具，也不会放宽任何既有断言。

用法：
  py -3 tests/soundsegs64_test.py                    # 自动取最新一个 out_snd.wav
  py -3 tests/soundsegs64_test.py --wav <path>
  py -3 tests/soundsegs64_test.py --wav <path> --min-gap-ms 120 --thr 300
  py -3 tests/soundsegs64_test.py --expect 30,150,250 # 只标注与这些素材时长（ms）匹配的段

退出码：0 = 至少找到 1 个非静音区段；1 = 整段录音全静音（或文件不可读）。
"""
import argparse
import glob
import os
import struct
import sys
import tempfile


def wav_read(path):
    """极简 RIFF/PCM 读取（QEMU 的 wav audiodev 输出：PCM s16le）。返回 (rate, ch, frames)。"""
    with open(path, "rb") as f:
        raw = f.read()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError("不是 RIFF/WAVE：%r" % raw[:16])
    pos, rate, ch, bits, data = 12, None, None, None, None
    while pos + 8 <= len(raw):
        cid = raw[pos:pos + 4]
        sz = struct.unpack_from("<I", raw, pos + 4)[0]
        body = raw[pos + 8:pos + 8 + sz]
        if cid == b"fmt ":
            afmt, ch, rate, _br, _ba, bits = struct.unpack_from("<HHIIHH", body, 0)
            if afmt != 1:
                raise ValueError("非 PCM（fmt=%d）" % afmt)
        elif cid == b"data":
            # ★ QEMU 的 wav audiodev 是**流式写**：RIFF 头里的 chunk 长度字段全是 0（它不回头 seek）。
            #   所以长度 0 或越界时，按"到文件尾"取数据（实测 out_snd.wav 的 data size 字段就是 0）。
            data = body if (sz > 0 and pos + 8 + sz <= len(raw)) else raw[pos + 8:]
        pos += 8 + sz + (sz & 1)
    if data is None or not ch or not rate:
        raise ValueError("缺 data/fmt 块")
    assert bits == 16, "本脚本只处理 16 bit（实测 QEMU 输出 %d bit）" % bits
    n = len(data) // (2 * ch)
    frames = struct.unpack_from("<%dh" % (n * ch), data, 0)
    return rate, ch, frames


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wav", default=None)
    ap.add_argument("--thr", type=int, default=300, help="帧幅度阈值（|sample|，16 bit 满量程 32767）")
    ap.add_argument("--min-gap-ms", type=int, default=120, help="小于这个间隔的静音不切段")
    ap.add_argument("--min-len-ms", type=int, default=8, help="短于这个的区段当噪声丢掉")
    ap.add_argument("--expect", default=None, help="逗号分隔的素材时长（ms），用于标注匹配")
    ap.add_argument("--marks", default=None,
                    help="sounds64_test 写出的 seg_marks.txt（每行 name off_bytes end_bytes）："
                         "按触发器给该区间的 peak/RMS/时长")
    args = ap.parse_args()

    wav = args.wav
    if not wav:
        cands = glob.glob(os.path.join(tempfile.gettempdir(), "vimtu64_snd_*", "out_snd.wav"))
        cands += glob.glob(os.path.join(tempfile.gettempdir(), "**", "out_snd.wav"), )
        if not cands:
            sys.stderr.write("找不到 out_snd.wav（跑 py -3 tests/sounds64_test.py 生成）\n")
            return 2
        wav = max(cands, key=os.path.getmtime)
    if not os.path.exists(wav):
        sys.stderr.write("文件不存在：%s\n" % wav)
        return 2

    rate, ch, s = wav_read(wav)
    nfr = len(s) // ch
    print("录音：%s" % wav)
    print("  格式：%d Hz x %d ch x 16 bit；帧数 %d（%.3f s）" % (rate, ch, nfr, nfr / float(rate)))

    # 每帧取各通道最大绝对值
    amp = [0] * nfr
    for i in range(nfr):
        o = i * ch
        m = 0
        for c in range(ch):
            v = s[o + c]
            if v < 0:
                v = -v
            if v > m:
                m = v
        amp[i] = m

    gap_fr = int(rate * args.min_gap_ms / 1000.0)
    min_fr = int(rate * args.min_len_ms / 1000.0)
    segs = []
    i = 0
    while i < nfr:
        if amp[i] <= args.thr:
            i += 1
            continue
        j = i
        last_loud = i
        while j < nfr:
            if amp[j] > args.thr:
                last_loud = j
            elif j - last_loud > gap_fr:
                break
            j += 1
        if last_loud - i + 1 >= min_fr:
            segs.append((i, last_loud))
        i = last_loud + 1

    exp = [float(x) for x in args.expect.split(",")] if args.expect else []
    print("\n%-4s %9s %9s %9s %8s %8s %s" % ("#", "start(s)", "end(s)", "dur(ms)", "peak", "rms", "匹配素材"))
    allrms = 0.0
    for k, (a, b) in enumerate(segs):
        pk = max(amp[a:b + 1])
        acc = 0
        for t in range(a, b + 1):
            acc += amp[t] * amp[t]
        rms = (acc / float(b - a + 1)) ** 0.5
        allrms += rms
        dur = (b - a + 1) * 1000.0 / rate
        tag = ""
        for e in exp:
            if abs(dur - e) <= max(6.0, e * 0.25):
                tag = "%g ms 素材" % e
                break
        print("%-4d %9.3f %9.3f %9.1f %8d %8.1f %s"
              % (k + 1, a / float(rate), (b + 1) / float(rate), dur, pk, rms, tag))

    # ---- 按"触发器标记"给区间（sounds64_test 的 ⑧ 段写出的字节区间）----
    if args.marks and os.path.exists(args.marks):
        print("\n--- 按触发器（⑧ 段标记的录音字节区间；出现时刻 = 该区间起点在**录音时间轴**上的秒数）---")
        print("%-10s %9s %9s %9s %9s %10s %10s %10s"
              % ("trigger", "off(B)", "len(B)", "len(ms)", "start(s)", "peak", "rms", "素材时长"))
        assets = {"click": 30, "notify": 150, "error": 250, "startup": 1130}
        for line in open(args.marks, encoding="utf-8"):
            parts = line.split()
            if len(parts) != 3:
                continue
            nm, a_b, b_b = parts[0], int(parts[1]), int(parts[2])
            a, b = a_b // (2 * ch), b_b // (2 * ch)
            a = max(0, min(a, nfr - 1))
            b = max(a + 1, min(b, nfr))
            w = amp[a:b]
            pk = max(w)
            rms = (sum(v * v for v in w) / float(len(w))) ** 0.5
            print("%-10s %9d %9d %8.1f %9.3f %10d %10.1f %10s"
                  % (nm, a_b, b_b - a_b, (b - a) * 1000.0 / rate, a / float(rate), pk, rms,
                     ("%g ms" % assets[nm]) if nm in assets else "?"))
        print("（口径：整段区间 = [off, end)，含静音；因此 len(ms) 明显大于素材时长 —— 素材时长见最后一列）")
    print("\n区段数=%d；非静音总时长=%.3f s（占录音 %.1f%%）；阈值 |sample| > %d"
          % (len(segs), sum((b - a + 1) for a, b in segs) / float(rate),
             100.0 * sum((b - a + 1) for a, b in segs) / max(1, nfr), args.thr))
    if not segs:
        print("结论：整段录音**全静音**（没有任何区段超过阈值）")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
