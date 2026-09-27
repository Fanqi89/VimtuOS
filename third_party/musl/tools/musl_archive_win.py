#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""musl 静态库归档助手（★ A2 的 musl 实验专用；只因为宿主是 Windows）

为什么需要它：musl 的 `make lib/libc.a` 会把 700+ 个目标文件名拼进**一条命令行**，
Windows 的 CreateProcess 命令行上限（约 32KB）直接把它打断：
    make: ar: Argument list too long      （直接 AR=ar 时）
    make: py: Argument list too long      （把 AR 换成包装脚本后，限制在 make 调用包装时就已经触发）
所以最终归档这一步**不经过 make**：本脚本自己枚举目标文件（规则与 musl Makefile 的
LIBC_OBJS 一致：obj/src/** 与 obj/compat/**，**不含** obj/crt 与 obj/ldso），
再按批（每批 64 个）调用 ar。Linux 宿主上不需要它：直接 `make lib/libc.a` 就完事。

用法（在 musl 源码根目录）：
    py -3 tools/musl_archive_win.py
产出：lib/libc.a（+ ranlib 建索引）
"""
import os
import subprocess
import sys

BATCH = 64


def collect():
    objs = []
    for top in ("obj/src", "obj/compat"):
        for root, _dirs, files in os.walk(top):
            for f in files:
                if f.endswith(".o"):
                    objs.append(os.path.join(root, f).replace("\\", "/"))
    objs.sort()
    return objs


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)
    objs = collect()
    if not objs:
        sys.stderr.write("没有找到 obj/**/*.o —— 先跑 `make -j lib/libc.a` 把目标文件编出来\n")
        return 2
    out = "lib/libc.a"
    if os.path.exists(out):
        os.remove(out)
    # ★ 为什么要带 P（成员名存**完整路径**）：Windows 宿主的 GNU ar 在 r（替换）模式下
    #   按**大小写不敏感**匹配成员名 —— `obj/src/exit/_Exit.o` 会顶掉先加进去的
    #   `obj/src/unistd/_exit.o`（实测：归档里少了 `_Exit` 符号，链接时报 undefined symbol: _Exit）。
    #   带 P 之后成员名互不相同，1345 个成员一个不少。
    for i in range(0, len(objs), BATCH):
        chunk = objs[i:i + BATCH]
        cmd = ["ar", "rcP" if i == 0 else "rP", out] + chunk
        subprocess.check_call(cmd)
    subprocess.check_call(["ranlib", out])
    print("lib/libc.a 归档完成：%d 个目标文件，%d 字节" % (len(objs), os.path.getsize(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
