#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/zstd_selftest.py - vs_zstd.c 的宿主交叉验证（用**真 zstd** 造样本）

为什么要有它：解码器"看起来能跑"不算证据。这里的判据是**真 zstd 编出来的字节流
→ 我们的解码器 → 与原文件逐字节一致**，覆盖到 zstd 的每一条分支：

  * 压缩级别（1/3/9/19）：决定 Huffman 表是"预定义/裸 4 位权重/FSE 压缩权重"、
    FSE 序列表是"预定义/RLE/自定义"，以及块是 Raw 还是 Compressed；
  * 数据形态（全零 / 重复文本 / 随机 / 混合）：决定 repeat offset 与匹配长度分布；
  * 尺寸（0.1 KB ~ 3 MB）：决定字面量段是 1 流还是 4 流，以及头是 3/4/5 字节；
  * 内容校验开关（--check / --no-check）：两条都要过。

用法：py -3 -u tools/zstd_selftest.py
退出码：0 全过 / 1 有样本失败 / 2 环境缺失（没有 zstd 或 gcc）
"""
import os
import random
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORK = os.path.join(ROOT, "build64", "zstd_selftest")
EXE = os.path.join(WORK, "vs_zstd_hosttest.exe")
SRC = os.path.join(HERE, "vs_zstd_hosttest.c")


def find_tool(names, extra_dirs=()):
    for n in names:
        p = shutil.which(n)
        if p:
            return p
    for d in extra_dirs:
        for n in names:
            p = os.path.join(d, n)
            if os.path.exists(p):
                return p
    return None


def make_payload(kind, size, seed=1234):
    rnd = random.Random(seed)
    if kind == "zero":
        return b"\x00" * size
    if kind == "text":
        base = (b"Package: vimtu-zstd-selftest\nVersion: 1.0\n"
                b"Architecture: amd64\nDescription: zstd decoder cross-check\n")
        out = bytearray()
        while len(out) < size:
            out += base
            out += (b"line %06d: %s\n" % (len(out) & 0xFFFF,
                                          b"abcdefghijklmnopqrstuvwxyz" * 2))
        return bytes(out[:size])
    if kind == "rand":
        return bytes(rnd.getrandbits(8) for _ in range(size))
    # mixed：30% 随机 + 70% 小偏置（造出"字面量多、匹配短"的真实分布）
    out = bytearray()
    while len(out) < size:
        if rnd.random() < 0.3:
            out += bytes(rnd.getrandbits(8) for _ in range(rnd.randrange(1, 40)))
        else:
            out += bytes((rnd.getrandbits(3) + 0x61) for _ in range(rnd.randrange(1, 80)))
    return bytes(out[:size])


CASES = []
for kind in ("zero", "text", "rand", "mixed"):
    for size in (0, 1, 7, 200, 300, 1023, 1024, 4096, 17000, 300000, 1200000):
        if kind == "rand" and size > 300000:
            continue                      # 随机数据不可压，样本没必要做大
        CASES.append((kind, size))


def build():
    gcc = find_tool(["gcc"], ["C:/msys64/usr/bin", "C:/msys64/mingw64/bin"])
    zstd = find_tool(["zstd"], ["C:/msys64/mingw64/bin", "C:/msys64/usr/bin"])
    return gcc, zstd


def main():
    gcc, zstd = build()
    if not gcc:
        print("SKIP(env): 找不到 gcc")
        return 2
    if not zstd:
        print("SKIP(env): 找不到 zstd（zstd.exe）")
        return 2
    os.makedirs(WORK, exist_ok=True)
    cc = subprocess.run([gcc, "-O2", "-Wall", "-Wextra", "-Werror", "-o", EXE, SRC],
                        capture_output=True, text=True)
    if cc.returncode != 0:
        print("编译失败：")
        print(cc.stdout)
        print(cc.stderr)
        return 1
    print("    宿主自测二进制：%s" % EXE)

    args = []
    ncase = 0
    for kind, size in CASES:
        data = make_payload(kind, size)
        op = os.path.join(WORK, "in_%s_%d.bin" % (kind, size))
        with open(op, "wb") as f:
            f.write(data)
        for level in (1, 3, 9, 19):
            for chk in ("--check", "--no-check"):
                zp = os.path.join(WORK, "z_%s_%d_%d%s.zst" % (kind, size, level,
                                                               "" if chk == "--check" else "_nock"))
                r = subprocess.run([zstd, chk, "-%d" % level, "-q", "-f", "-o", zp, op],
                                   capture_output=True)
                if r.returncode != 0:
                    print("zstd 造样本失败：%s %d %d %s" % (kind, size, level, chk))
                    print(r.stderr.decode("utf-8", "replace"))
                    return 2
                args += [zp, op]
                ncase += 1
    print("    真 zstd 样本：%d 个（%d 组 kind/size × 4 级 × 2 校验）" % (ncase, len(CASES)))

    # 命令行太长会撞 Windows 32 KB 上限 → 分块跑，任何一块失败即整体失败
    bad = 0
    step = 24
    for i in range(0, len(args), step):
        r = subprocess.run([EXE] + args[i:i + step], capture_output=True, text=True)
        out = r.stdout + r.stderr
        sys.stdout.write(out)
        if r.returncode != 0:
            bad = 1
    print("===== RESULT: %s =====" % ("FAIL" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
