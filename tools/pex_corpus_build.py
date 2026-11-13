#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/pex_corpus_build.py - ★ v0.9.43（W0）：造**真 PE** 语料（宿主侧，只写 build64/）

为什么要有这个脚本：W0 的体内检器 /bin/peinfo 必须拿**真 exe** 验，不能用我们手搓的字节 ——
手搓的只能证明"解析器认得我自己写的东西"。这里用 MSYS2 的**标准工具链**
（`x86_64-w64-mingw32-gcc`）编出三份 PE，覆盖三种形态：

  ① tiny.exe   PE32+ / **无导入**（`-nostdlib` + 汇编入口）——验"导入表为空"这条分支
  ② cui.exe    PE32+ / **控制台**（CUI=3）+ CRT ——导入 msvcrt/kernel32（printf 走 msvcrt）
  ③ gui.exe    PE32+ / **图形**（GUI=2）+ CRT ——导入 user32/kernel32（MessageBoxA）

产物（**不提交**，build64/ 已 gitignore）：
    build64/pexcorpus/src/*.c|*.S      本次用到的源码（构建可复现）
    build64/pexcorpus/*.exe            三份真 PE
    build64/pexcorpus/PEX.json         清单：路径、size、sha256、期望 subsys、导入 DLL 集合

用法：py -3 tools/pex_corpus_build.py
退出码：0 = 全建成；1 = 有硬失败；2 = 环境问题（缺 mingw 工具链）
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
OUT = os.path.join(ROOT, "build64", "pexcorpus")
SRC = os.path.join(OUT, "src")

GCC = r"C:\msys64\mingw64\bin\x86_64-w64-mingw32-gcc.exe"

# ---- ① 极小 PE：无 CRT、无导入（汇编入口，直接 ret）----
TINY_S = r'''
/* tiny.S - 最小 PE：只有 PE 头 + 一个 .text 节，**没有任何导入**。
 * 目的：让 peinfo 走"导入表为空"的分支（union 必须是空串，不能崩、不能打 ?）。*/
    .text
    .globl pe_entry
pe_entry:
    xorl %eax, %eax
    ret
'''

# ---- ② 控制台 exe：printf 走 msvcrt ----
CUI_C = r'''
/* cui.c - 控制台 PE：靠 CRT 的 printf。
 * 导入面（这才是 W2 要实现的清单）：
 *   msvcrt.dll  : __acrt_iob_func / __stdio_common_vfprintf（UCRT）或 printf（老 msvcrt）
 *   kernel32.dll: 进程启动/退出相关
 * 我们**不运行**它，只用 peinfo 看它导入了什么。*/
#include <stdio.h>

int main(void) {
    printf("hello from a real Windows console exe\n");
    return 0;
}
'''

# ---- ③ 图形 exe：MessageBoxA 走 user32 ----
GUI_C = r'''
/* gui.c - 图形 PE（subsystem=GUI=2）：调 MessageBoxA。
 * 这一份是 W3 的"最小目标"：只要 user32!MessageBoxA + kernel32 里那几个能跑起来，
 * 它就能在我们自己的桌面上弹出一个真窗口。*/
#include <windows.h>

int WINAPI WinMain(HINSTANCE h, HINSTANCE p, LPSTR cmd, int show) {
    (void)h; (void)p; (void)cmd; (void)show;
    MessageBoxA(0, "hello from a real Windows GUI exe", "vimtu-peinfo", MB_OK);
    return 0;
}
'''


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def run(cmd, what):
    print("    $ %s" % " ".join(cmd[1:3] + ["..."] if len(cmd) > 3 else cmd[1:]))
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, errors="replace")
    if r.returncode != 0:
        sys.stderr.write("FAILED (%s) rc=%d\n%s\n%s\n"
                         % (what, r.returncode, r.stdout[-2000:], r.stderr[-2000:]))
        return False
    if r.stderr.strip():
        print("      stderr: " + r.stderr.strip().replace("\n", " | ")[:300])
    return True


def main():
    if not os.path.exists(GCC):
        sys.stderr.write("环境问题：缺 %s（需要 MSYS2 的 mingw-w64 工具链）\n" % GCC)
        return 2
    os.makedirs(SRC, exist_ok=True)

    for name, text in (("tiny.S", TINY_S), ("cui.c", CUI_C), ("gui.c", GUI_C)):
        with open(os.path.join(SRC, name), "w", encoding="utf-8", newline="\n") as f:
            f.write(text.lstrip("\n"))

    print("== pexcorpus: building real PE files with mingw-w64 ==")
    items = []

    # ① tiny.exe：无导入
    tiny = os.path.join(OUT, "tiny.exe")
    ok = run([GCC, "-nostdlib", "-Wl,-e,pe_entry", "-Wl,--subsystem,console",
              "-o", tiny, os.path.join(SRC, "tiny.S")], "tiny.exe")
    if not ok:
        return 1
    items.append(("tiny", tiny, "① minimal PE, no imports", 3, []))

    # ② cui.exe：控制台 + CRT
    cui = os.path.join(OUT, "cui.exe")
    ok = run([GCC, "-O2", "-o", cui, os.path.join(SRC, "cui.c")], "cui.exe")
    if not ok:
        return 1
    items.append(("cui", cui, "② console exe, CRT (printf)", 3, []))

    # ③ gui.exe：图形 + MessageBoxA
    gui = os.path.join(OUT, "gui.exe")
    ok = run([GCC, "-O2", "-mwindows", "-o", gui, os.path.join(SRC, "gui.c")], "gui.exe")
    if not ok:
        return 1
    items.append(("gui", gui, "③ GUI exe, MessageBoxA (subsystem=2)", 2, ["USER32.DLL"]))

    manifest = []
    print()
    print("== corpus ==")
    for key, path, desc, exp_subsys, exp_dlls in items:
        if not os.path.exists(path):
            sys.stderr.write("缺产物 %s\n" % path)
            return 1
        size = os.path.getsize(path)
        rec = {
            "key": key,
            "vol": "/pexcorpus/%s.exe" % key,
            "src": path,
            "size": size,
            "sha256": sha256_of(path),
            "desc": desc,
            "expect_subsys": exp_subsys,
            "expect_dlls_must_include": exp_dlls,
        }
        manifest.append(rec)
        print("   %-10s %-46s %7d B  %s" % (key, rec["vol"], size, desc))

    with open(os.path.join(OUT, "PEX.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2, sort_keys=True)
    print()
    print("== PEX.json written: %d items ==" % len(manifest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
