#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/micropython_check_elf.py - ★ v0.11.0：MicroPython 两个 ELF 的构建期硬断言

为什么要有它（与 tools/lua_build_win.sh 尾部那段内联 python 同一个目的、同一套判据）：
  VimtuOS 的内核 ELF64 装载器只装"主程序"，而主程序的 PT_LOAD 必须完整落在用户窗口
  **低 64 KiB**（kernel/elf64.cpp 的 e64_parse_ex64 -> reason=outside-user-window）；
  真正的解释器由驱动 user/micropython/mpdrv.c 用 mmap(9) 装到 **4GiB+0x90000**
  （USER64_MMAP_VA64）再 jmp 进去。这两条硬约束**在构建期就该被拒绝**，不要等到真机上
  只看到一句 `[ELF64] exec reject` 或者 ring3 #PF。

用法：
    py -3 tools/micropython_check_elf.py build64/mp build64/micropython.bin
    py -3 tools/micropython_check_elf.py build64/micropython.bin      # 只查解释器
退出码：0 = 全部通过；1 = 有断言失败（逐条打印 [FAIL]）；2 = 参数/文件问题。

判据来源（逐条对应，改一处必须同步另一处）：
  * kernel/usermode64.h  USER64_CODE_VA64 / USER64_STACK_VA64 / USER64_MMAP_VA64 /
                         USER64_WINDOW_BYTES64（窗口顶）
  * kernel/elf64.cpp     e64_parse_ex64 的越界拒绝
  * user/micropython/mpdrv.c（= user/lua/luadrv.c 同构）的装载期检查：
        seg-below-mmap / seg-too-big / mmap-va-mismatch / phdr-va / phdr-not-in-read
"""

import os
import struct
import sys

BASE64 = 0x100000000               # USER64_CODE_VA64：用户代码/入口
STACK64 = 0x100000000 + 0x10000    # USER64_STACK_VA64：用户栈底（= 装载区上界）
MMAP_VA = 0x100000000 + 0x90000    # USER64_MMAP_VA64：mmap bump 起点 = 解释器的链接基址
WINDOW_TOP = 0x100000000 + 16 * 1024 * 1024   # 用户窗口顶（4GiB + 16MiB）
SPAN_MAX = WINDOW_TOP - MMAP_VA    # ~15.4 MiB：窗口里留给 mmap 的那一段
DRV_STACK_BYTES = 512 * 1024       # 驱动给解释器换的栈（mpdrv.c 的 DRV_STACK_BYTES）
DRV_LIMIT = 64 * 1024              # 驱动：内核主程序装载器的窗口（低 64 KiB）
READ_BUF = 96 * 1024               # 内核读盘缓冲（lua 那边的同一条上限）


class Bad(Exception):
    pass


def _phdrs(d):
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phes = struct.unpack_from("<H", d, 54)[0]
    phn = struct.unpack_from("<H", d, 56)[0]
    out = []
    for i in range(phn):
        q = phoff + i * phes
        t = struct.unpack_from("<I", d, q)[0]
        flags = struct.unpack_from("<I", d, q + 4)[0]
        off, va, pa, fsz, msz, al = struct.unpack_from("<QQQQQQ", d, q + 8)
        out.append((t, flags, off, va, fsz, msz, al))
    return phoff, phes, phn, out


def _elf_header(d, what):
    if len(d) < 64:
        raise Bad("%s 只有 %d B，连 ELF 头都不完整" % (what, len(d)))
    if d[:4] != b"\x7fELF":
        raise Bad("%s 不是 ELF（magic=%r）" % (what, d[:4]))
    if d[4] != 2 or d[5] != 1:
        raise Bad("%s 不是 ELF64 小端（EI_CLASS=%d EI_DATA=%d）" % (what, d[4], d[5]))
    etype, machine = struct.unpack_from("<HH", d, 16)
    if etype != 2:
        raise Bad("%s 不是 ET_EXEC（e_type=%d；内核只做静态定址装载）" % (what, etype))
    if machine != 0x3E:
        raise Bad("%s 不是 x86_64（e_machine=%#x）" % (what, machine))
    entry = struct.unpack_from("<Q", d, 24)[0]
    return entry


def check_driver(path):
    """驱动（/bin/mp）：< 64 KiB、PT_LOAD 落在 4GiB..4GiB+64KiB、头表在首段里。"""
    d = open(path, "rb").read()
    what = os.path.basename(path)
    entry = _elf_header(d, what)
    phoff, phes, phn, ph = _phdrs(d)

    if phes != 56:
        raise Bad("%s 的 e_phentsize=%d（必须 56）" % (what, phes))
    if not (0 < phn <= 16):
        raise Bad("%s 的 e_phnum=%d（内核上限 16）" % (what, phn))
    if phoff + phn * phes > len(d):
        raise Bad("%s 的程序头表越出文件（e_phoff=%#x）" % (what, phoff))

    first = None
    for (t, flags, off, va, fsz, msz, al) in ph:
        if t == 3:
            raise Bad("%s 出现 PT_INTERP（内核只做静态装载）" % what)
        if t == 2:
            raise Bad("%s 出现 PT_DYNAMIC（内核不做重定位）" % what)
        if t != 1:
            continue
        if first is None:
            first = (off, fsz)
        if va < BASE64:
            raise Bad("%s 的 PT_LOAD 低于 4GiB（va=%#x）" % (what, va))
        if va + msz > STACK64:
            raise Bad("%s 的 PT_LOAD 越出主程序窗口 4GiB..4GiB+64KiB（va=%#x memsz=%#x）"
                      % (what, va, msz))
    if first is None:
        raise Bad("%s 没有任何 PT_LOAD" % what)
    if first[0] != 0:
        raise Bad("%s 的第一个 PT_LOAD p_offset=%#x（必须 0：内核按它算 AT_PHDR）"
                  % (what, first[0]))
    if phoff + phn * phes > first[1]:
        raise Bad("%s 的程序头表不在第一个 PT_LOAD 的文件范围内（AT_PHDR 会是 0）" % what)
    if entry < BASE64 or entry >= STACK64:
        raise Bad("%s 的 e_entry=%#x 不在主程序窗口里" % (what, entry))
    if len(d) > DRV_LIMIT:
        raise Bad("%s = %d B，超过内核主程序装载窗口 %d B" % (what, len(d), DRV_LIMIT))
    if len(d) > READ_BUF:
        raise Bad("%s = %d B，超过内核读盘缓冲 %d B" % (what, len(d), READ_BUF))

    print("    断言 OK：驱动 %s = %d B，entry=%#x phnum=%d（PT_LOAD 落在 4GiB..4GiB+64KiB）"
          % (what, len(d), entry, phn))
    return len(d)


def check_interp(path, verbose=True):
    """解释器（/lib/micropython.bin）：钉在 4GiB+0x90000，全部段在窗口内。"""
    d = open(path, "rb").read()
    what = os.path.basename(path)
    entry = _elf_header(d, what)
    phoff, phes, phn, ph = _phdrs(d)

    results = []

    def ok(name, cond, detail=""):
        results.append((name, bool(cond), detail))
        if verbose:
            print("      [%s] %-50s %s" % ("OK" if cond else "FAIL", name, detail))

    ok("e_phentsize == 56", phes == 56, "e_phentsize=%d" % phes)
    ok("0 < e_phnum <= 16", 0 < phn <= 16, "e_phnum=%d" % phn)
    ok("e_phoff 在文件内", phoff + phn * phes <= len(d), "e_phoff=%#x" % phoff)

    types = [p[0] for p in ph]
    ok("无 PT_INTERP (3)", 3 not in types, "")
    ok("无 PT_DYNAMIC (2)", 2 not in types, "")

    loads = [p for p in ph if p[0] == 1]
    ok("至少一个 PT_LOAD", len(loads) >= 1, "PT_LOAD 个数=%d" % len(loads))
    if not loads:
        raise Bad("%s 没有任何 PT_LOAD" % what)
    va_lo = min(p[3] for p in loads)
    va_hi = max(p[3] + p[5] for p in loads)
    ok("所有 PT_LOAD p_vaddr >= 4GiB", va_lo >= BASE64, "最低 p_vaddr=%#x" % va_lo)
    ok("所有 PT_LOAD p_vaddr >= 4GiB+0x90000", va_lo >= MMAP_VA, "最低 p_vaddr=%#x" % va_lo)
    ok("所有 PT_LOAD end <= 4GiB+16MiB", va_hi <= WINDOW_TOP, "最高 end=%#x" % va_hi)

    first = loads[0]
    ok("第一个 PT_LOAD p_offset == 0", first[2] == 0, "p_offset=%#x" % first[2])
    ok("第一个 PT_LOAD p_vaddr == 4GiB+0x90000", first[3] == MMAP_VA, "p_vaddr=%#x" % first[3])
    ok("程序头表在第一个 PT_LOAD 文件范围内",
       first[2] <= phoff and phoff + phn * phes <= first[2] + first[4],
       "phoff=%#x filesz=%#x" % (phoff, first[4]))

    span = ((va_hi - MMAP_VA + 0xFFF) & ~0xFFF)
    ok("span <= mmap 区 (%d B)" % SPAN_MAX, span <= SPAN_MAX,
       "span=%d B (%.2f MiB)" % (span, span / 1048576.0))
    ok("span + 512KiB 栈 <= mmap 区", span + DRV_STACK_BYTES <= SPAN_MAX,
       "span+stack=%d B" % (span + DRV_STACK_BYTES))
    ok("e_entry 落在某个 PT_LOAD 内",
       any(p[3] <= entry < p[3] + p[5] for p in loads), "e_entry=%#x" % entry)

    bad = [r for r in results if not r[1]]
    if bad:
        for (name, _, detail) in bad:
            print("    ERROR: %s 断言失败（%s）" % (name, detail))
        raise Bad("%s 有 %d 条断言失败" % (what, len(bad)))

    if verbose:
        print("      ---- 段表 ----")
        for (t, flags, off, va, fsz, msz, al) in ph:
            print("      type=%#-10x flags=%d off=%#010x va=%#012x filesz=%#010x "
                  "memsz=%#010x align=%#x" % (t, flags, off, va, fsz, msz, al))
    print("    断言 OK：解释器 %s = %d B，entry=%#x phnum=%d span=%d B（%.1f KiB；"
          "mmap 区剩 %d B）"
          % (what, len(d), entry, phn, span, span / 1024.0, SPAN_MAX - span))
    return len(d), span


def main(argv):
    if len(argv) < 2 or len(argv) > 3:
        sys.stderr.write("用法：py -3 tools/micropython_check_elf.py <driver> [interp]\n"
                         "      py -3 tools/micropython_check_elf.py <interp>\n")
        return 2
    for p in argv[1:]:
        if not os.path.exists(p):
            sys.stderr.write("找不到文件：%s\n" % p)
            return 2

    if len(argv) == 3:
        drv, interp = argv[1], argv[2]
    else:
        # 只给一个文件：按大小自动判断（< 64 KiB 当驱动，否则当解释器）
        if os.path.getsize(argv[1]) <= DRV_LIMIT:
            drv, interp = argv[1], None
        else:
            drv, interp = None, argv[1]

    try:
        if drv:
            check_driver(drv)
        if interp:
            check_interp(interp)
    except Bad as e:
        sys.stderr.write("ERROR: %s\n" % e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
