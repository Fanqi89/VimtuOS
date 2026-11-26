#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/drv_stack_gate.py - ★ v0.12.4：**用户栈大小**在"内核常量"与"ring3 换栈驱动"之间的一致性门禁

背景（真事故，不是假想）：ring3 的五个装载驱动（bbdrv / tccdrv / luadrv / makedrv / mpdrv）
要把内核摆好的初始栈（argc/argv/envp/auxv + 参数字符串）**搬到自己的大栈**上去，它们的算法是

    len     = STACK_TOP - sp          # sp = 程序入口的 rsp
    new_sp  = my_stack + MY_STACK_BYTES - len

也就是**必须知道内核栈顶**。五个文件里各自写了一份
`const u64 old_top = U64_STACK_VA64 + 0xNNNNULL;`。

v0.12.4 把 `kernel/usermode64.h` 的 USER64_STACK_BYTES64 从 16 KiB 放大到 64 KiB（TLS 握手要栈），
五个文件的 `+ 0x4000` 就成了错的：搬过去的是**错误偏移的一段内存**，于是 busybox/tcc/lua/make/
micropython 一进 ring3 就 #PF（实测：[BBDRV] exec … 之后
`[SIG64] fault pid=… #PF err=0x6 cr2=0x000000010013BE40`，version64_test 的 ring3 cat 直接红）。

这道门禁就是把这条"隐式耦合"变成**构建期失败**：
  [DRVSTACK] ok=1 stack=65536 users=5（bbdrv/tccdrv/luadrv/makedrv/mpdrv 全部一致）
不一致时 exit 1 并逐条打印"哪个文件写了什么、内核是什么"。

用法（build64.sh 里在编 ring3 驱动之前调用）：
  py -3 tools/drv_stack_gate.py [--kernel kernel/usermode64.h] [--user user]
退出码：0 = 一致；1 = 漂移；2 = 找不到文件/解析不出常量。
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

DRIVERS = [
    os.path.join("user", "busybox", "bbdrv.c"),
    os.path.join("user", "apps", "tcc", "tccdrv.c"),
    os.path.join("user", "lua", "luadrv.c"),
    os.path.join("user", "make", "makedrv.c"),
    os.path.join("user", "micropython", "mpdrv.c"),
]

# kernel/usermode64.h: static const uint64_t USER64_STACK_BYTES64  = 64ULL * 1024ULL;
K_RE = re.compile(r"USER64_STACK_BYTES64\s*=\s*(\d+)ULL\s*\*\s*(\d+)ULL")
# 驱动里: const u64 old_top = U64_STACK_VA64 + 0x10000ULL;
D_RE = re.compile(r"old_top\s*=\s*U64_STACK_VA64\s*\+\s*0x([0-9A-Fa-f]+)ULL")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kernel", default=os.path.join(ROOT, "kernel", "usermode64.h"))
    ap.add_argument("--user", default=os.path.join(ROOT, "user"))
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.kernel):
        sys.stderr.write("[DRVSTACK] FAIL 找不到内核头：%s\n" % args.kernel)
        return 2
    with open(args.kernel, "r", encoding="utf-8", errors="replace") as f:
        src = f.read()
    m = K_RE.search(src)
    if not m:
        sys.stderr.write("[DRVSTACK] FAIL 从 %s 解析不出 USER64_STACK_BYTES64\n" % args.kernel)
        return 2
    want = int(m.group(1)) * int(m.group(2))

    bad = []
    used = 0
    for rel in DRIVERS:
        p = rel if os.path.isabs(rel) else os.path.join(ROOT, rel)
        if not os.path.exists(p):
            bad.append((rel, "文件不存在"))
            continue
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            s = f.read()
        hits = D_RE.findall(s)
        if len(hits) != 1:
            bad.append((rel, "找不到唯一一处 `old_top = U64_STACK_VA64 + 0x…ULL`（找到 %d 处）" % len(hits)))
            continue
        got = int(hits[0], 16)
        used += 1
        if got != want:
            bad.append((rel, "写的是 0x%X（%d B），内核是 0x%X（%d B）" % (got, got, want, want)))

    if bad:
        sys.stderr.write("[DRVSTACK] FAIL stack=%d 与 %d 处驱动不一致：\n" % (want, len(bad)))
        for rel, why in bad:
            sys.stderr.write("    %s: %s\n" % (rel, why))
        sys.stderr.write("    修法：把这几个文件里 old_top 的 `+ 0x…ULL` 改成与 kernel/usermode64.h 的\n"
                         "          USER64_STACK_BYTES64 相同（改内核栈 = 必须同时改这几处）。\n")
        return 1

    if not args.quiet:
        print("[DRVSTACK] ok=1 stack=%d users=%d (bbdrv/tccdrv/luadrv/makedrv/mpdrv 一致)" % (want, used))
    return 0


if __name__ == "__main__":
    sys.exit(main())
