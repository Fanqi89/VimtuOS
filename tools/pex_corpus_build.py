#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/pex_corpus_build.py - ★ v0.9.43（W0）/ v0.9.44（W1）：造**真 PE** 语料（宿主侧，只写 build64/）

为什么要有这个脚本：体内工具（W0 的 /bin/peinfo、W1 的 /bin/peload）必须拿**真 exe** 验，
不能用我们手搓的字节 —— 手搓的只能证明"解析器/装载器认得我自己写的东西"。
这里用 MSYS2 的**标准工具链**（`x86_64-w64-mingw32-gcc`）编出五份 PE，覆盖五种形态：

  ① tiny.exe   PE32+ / **无导入、无重定位**（`-nostdlib` + 汇编入口 `xor eax,eax; ret`）
               —— W0 验"导入表为空"这条分支；W1 验"无重定位也能跳进入口并拿回返回值"
  ② cui.exe    PE32+ / **控制台**（CUI=3）+ CRT —— 导入 msvcrt/kernel32（printf 走 msvcrt）
  ③ gui.exe    PE32+ / **图形**（GUI=2）+ CRT —— 导入 user32/kernel32（MessageBoxA）
  ④ reloc.exe  PE32+ / **无导入、有 DIR64 重定位**（★ W1 新增：重定位自证器）
               入口自证：`g_self` 是一个**存在 .data 里的绝对指针**（= &g_self），装载器若把
               `IMAGE_REL_BASED_DIR64` 加对了 delta，那么 `g_self == &g_self` 成立 -> 返回 42；
               否则返回 1。**光看"程序没崩"验不出重定位，必须看返回值。**
  ⑤ exit.exe   PE32+ / **只导入 kernel32!ExitProcess**（★ W1 新增：IAT 桩自证器）
               入口调 `ExitProcess(7)`。W1 的装载器会给这个导入填一个**自己的桩**
               （真做 exit_group(7)），于是进程以 7 退出 —— 这就是"Ring 3 填了一个假的
               ExitProcess 导入"的端到端证据。

为什么 ④ 必须新造：`tiny.exe` 的 `summary` 是 `relocs=0` —— **它验不了重定位**。
而 `cui/gui` 有 reloc 但有导入，W1 不填 IAT 就跑不了。

产物（**不提交**，build64/ 已 gitignore）：
    build64/pexcorpus/src/*.c|*.S      本次用到的源码（构建可复现）
    build64/pexcorpus/*.exe            五份真 PE
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
 * 目的：让 peinfo 走"导入表为空"的分支（union 必须是空串，不能崩、不能打 ?）。
 *      让 peload 走"无重定位 + 无导入"的最短路径（entry_ret=0）。*/
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
 * 我们**不运行**它，只用 peinfo 看它导入了什么；W1 的 peload 必须**如实报缺哪些导入**。*/
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

# ---- ④ 重定位自证器（★ W1）：无导入 + 有 DIR64 ----
RELOC_C = r'''
/* reloc.c - ★ W1 的重定位自证器。
 *
 * 原理：`g_self` 是一个**存在 .data 里的绝对指针**，初值 = &g_self。
 * 链接器会为它生成一条 IMAGE_REL_BASED_DIR64 重定位项（HIGHLOW 在 64 位下不合法）。
 *
 *   * 装载器**没做**重定位（delta != 0 但没加）：g_self 里仍是链接期的地址 &g_self@ImageBase，
 *     跟运行时算出来的 &g_self 不等 -> 入口返回 1。
 *   * 装载器**做对了**：g_self += delta 之后恰好等于运行时的 &g_self -> 入口返回 42。
 *
 * 所以"返回值"本身就是重定位正确性的判据 —— 不能只看"程序没崩"。
 * volatile 是必须的：不加的话编译器可以在编译期把 `g_self == &g_self` 折成常量 1，
 * 那样这个语料就什么都验不出来了。 */
void * volatile g_self = (void *)&g_self;

long pe_entry(void) {
    void *p = g_self;
    return (p == (void *)&g_self) ? 42 : 1;
}
'''

# ---- ⑤ ExitProcess IAT 桩自证器（★ W1）----
EXIT_C = r'''
/* exit.c - ★ W1 的 IAT 桩自证器：**只导入一个函数** kernel32!ExitProcess。
 *
 * W1 的装载器把自己的一张极小桩表填进 IAT（ExitProcess 真做 exit_group）。
 * 于是 `ExitProcess(7)` 会让**装载器进程本身**以 7 退出 —— 这就是
 * "Ring 3 填了一个假的 ExitProcess 导入"的端到端证据（而不是假装成功）。
 *
 * 为什么不用 CRT：CRT 会带进 27 个别的导入（见 cui.exe 的 union 行），
 * 那样就分不清"ExitProcess 桩生效了"还是"CRT 先崩了"。这里只留一条。 */
__declspec(dllimport) void ExitProcess(unsigned int code);

long pe_entry(void) {
    ExitProcess(7u);
    return 99;              /* 走不到：ExitProcess 桩直接 exit_group */
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

    for name, text in (("tiny.S", TINY_S), ("cui.c", CUI_C), ("gui.c", GUI_C),
                       ("reloc.c", RELOC_C), ("exit.c", EXIT_C)):
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
    items.append(("tiny", tiny, "(1) minimal PE, no imports, no relocs", 3, []))

    # ② cui.exe：控制台 + CRT
    cui = os.path.join(OUT, "cui.exe")
    ok = run([GCC, "-O2", "-o", cui, os.path.join(SRC, "cui.c")], "cui.exe")
    if not ok:
        return 1
    items.append(("cui", cui, "(2) console exe, CRT (printf)", 3, []))

    # ③ gui.exe：图形 + MessageBoxA
    gui = os.path.join(OUT, "gui.exe")
    ok = run([GCC, "-O2", "-mwindows", "-o", gui, os.path.join(SRC, "gui.c")], "gui.exe")
    if not ok:
        return 1
    items.append(("gui", gui, "(3) GUI exe, MessageBoxA (subsystem=2)", 2, ["USER32.DLL"]))

    # ④ reloc.exe：无导入 + 有 DIR64 重定位（★ W1 重定位自证器）
    #    强制 -mno-sse*：本语料的目的是验重定位，不想因为编译器拿 SSE 清 .data
    #    而把失败原因搅在一起（虽然内核已开 CR4.OSFXSR，能跑 SSE）。
    reloc = os.path.join(OUT, "reloc.exe")
    ok = run([GCC, "-nostdlib", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
              "-Wl,-e,pe_entry", "-Wl,--dynamicbase", "-Wl,--subsystem,console",
              "-o", reloc, os.path.join(SRC, "reloc.c")], "reloc.exe")
    if not ok:
        return 1
    items.append(("reloc", reloc, "(4) no imports, WITH DIR64 relocs (entry returns 42 iff applied)",
                  3, []))

    # ⑤ exit.exe：只导入 kernel32!ExitProcess（★ W1 IAT 桩自证器）
    exe_exit = os.path.join(OUT, "exit.exe")
    ok = run([GCC, "-nostdlib", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-avx",
              "-Wl,-e,pe_entry", "-Wl,--subsystem,console",
              "-o", exe_exit, os.path.join(SRC, "exit.c"), "-lkernel32"], "exit.exe")
    if not ok:
        return 1
    items.append(("exit", exe_exit, "(5) imports ONLY kernel32!ExitProcess (IAT stub proof)",
                  3, ["KERNEL32.DLL"]))

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
