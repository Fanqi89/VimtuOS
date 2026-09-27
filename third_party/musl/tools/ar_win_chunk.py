#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""ar 分块包装（★ A2 的 musl 实验专用，只因为宿主是 Windows）

为什么需要它：musl 的 `make lib/libc.a` 一条命令要归档 **700+ 个目标文件**，展开后的命令行
超过 Windows 的 CreateProcess 上限（约 32KB），GNU ar 直接报
    make: ar: Argument list too long
（在 Linux 宿主上不会有这个问题 —— 这是宿主的限制，不是 musl 或工具链的问题）。
本包装把目标文件按 64 个一批调用真正的 `ar`：第一批用 `rc` 建库，后续用 `r` 追加。

用法（config.mak 里）：AR = py -3 tools/ar_win_chunk.py
    py -3 tools/ar_win_chunk.py rc lib/libc.a obj/....o obj/....o ...
"""
import subprocess
import sys

BATCH = 64


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("用法: ar_win_chunk.py <ar 操作(rc)> <输出库> <目标文件...>\n")
        return 2
    op = sys.argv[1]
    out = sys.argv[2]
    objs = sys.argv[3:]
    if not objs:
        subprocess.check_call(["ar", op, out])
        return 0
    first = True
    for i in range(0, len(objs), BATCH):
        chunk = objs[i:i + BATCH]
        subprocess.check_call(["ar", op if first else "r", out] + chunk)
        first = False
    return 0


if __name__ == "__main__":
    sys.exit(main())
