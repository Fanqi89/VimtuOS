#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/netsurf_gen_expat_config.py - ★ v0.12.5：生成 expat 的 `expat_config.h`（官方由 cmake/configure 产出）

为什么需要：expat 的 `lib/xmlparse.c`、`lib/xmlrole.c`、`lib/xmltok.c` 第一件事就是
`#include "expat_config.h"`，而 git 源码树里**没有这个文件**（它由 cmake/autoconf 生成）。
不提供它，expat 一个 .c 都编不过（实测：`fatal error: 'expat_config.h' file not found`）。

本脚本按"x86_64 Linux + musl + 静态库"这一目标写出等价配置，落在
`build64/netsurf/inc/expat_config.h`（**构建产物，不进仓**）。
其中三条是 expat 自己会做硬性检查（不能少）：
  * `XML_GE` 必须是 1 或 0（xmlparse.c 有 `#error`);
  * `XML_DTD` 与 `XML_GE=0` 不能同时成立（xmlparse.c 有 `#error`）→ 这里 GE=1 且保留 DTD；
  * `BYTEORDER` 决定 xmltok 的字节序分支（x86_64 = 1234 little-endian）。

用法：py -3 tools/netsurf_gen_expat_config.py [--out <path>]
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

TEXT = """/* expat_config.h —— 手写替身（官方由 cmake/configure 生成）
 *
 * 由 tools/netsurf_gen_expat_config.py 生成到 build64/netsurf/inc/（**不进仓**）。
 * 目标：x86_64 Linux（musl）静态库，只给 expat 库本体需要的定义。
 * xmlparse.c 对 XML_GE / XML_DTD 有硬性一致性检查（GE 必须 1 或 0；GE=0 时不许有 DTD），
 * 所以这里 XML_GE=1 且保留 XML_DTD。BYTEORDER 决定 xmltok 的字节序分支（x86_64 = 1234）。
 */
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

#define BYTEORDER 1234

#define HAVE_DLFCN_H 1
#define HAVE_FCNTL_H 1
#define HAVE_GETPAGESIZE 1
#define HAVE_GETRANDOM 1
#define HAVE_INTTYPES_H 1
#define HAVE_MEMMOVE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_UNISTD_H 1

#define XML_GE 1
#define XML_CONTEXT_BYTES 1024

#endif /* EXPAT_CONFIG_H */
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "build64", "netsurf", "inc", "expat_config.h"))
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "wb") as f:
        f.write(TEXT.encode("utf-8"))
    print("[EXPAT-CONFIG] ok=1 bytes=%d -> %s" % (len(TEXT.encode("utf-8")), args.out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
