#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/tools/elf64_check.py - 构建期自检：你的产物**真的**能被 VimtuOS 装载吗

判据与内核一致（真源：kernel/elf64.cpp + kernel/usermode64.h + kernel/app64.h），逐条：
  ELF64：
    ① e_ident：ELF / 64 位 / 小端；e_type = ET_EXEC(2)；e_machine = x86_64(0x3E)；
    ② e_phentsize = 56、0 < e_phnum <= 16（ELF64_MAX_PHDR64）；
    ③ 每个 PT_LOAD 落在 **4GiB .. 4GiB+64KiB**（USER64_CODE_VA64 .. USER64_STACK_VA64 之前），
       且页对齐后不压到用户栈区；
    ④ **没有** PT_INTERP / PT_DYNAMIC（内核只做静态装载，没有动态链接器）；
    ⑤ 入口落在某个 PT_LOAD 内；程序头表在**第一个** PT_LOAD 内（否则 auxv 的 AT_PHDR 是 0）；
    ⑥ 文件 <= 96 KiB（内核读盘缓冲上限）。
  blob（objcopy -O binary 的平铺二进制）：
    ⑦ 非空且 <= 32768 B（8 页）—— kernel/usermode64.cpp 的 blob 装载器上限。

用法：py -3 sdk/software-template/tools/elf64_check.py <file.elf> [file.bin]
退出码：0 = 全过；1 = 有断言失败；2 = 文件不存在。
"""
import os
import struct
import sys

LO = 0x100000000                     # 4 GiB = USER64_CODE_VA64
HI = 0x100000000 + 0x10000           # 4 GiB + 64 KiB = USER64_STACK_VA64 之前
ELF_MAX_BYTES = 96 * 1024
BLOB_MAX_BYTES = 8 * 4096


def check_elf(path):
    d = open(path, "rb").read()
    assert d[:4] == b"\x7fELF", "不是 ELF"
    assert d[4] == 2 and d[5] == 1, "不是 ELF64 小端"
    etype, machine = struct.unpack_from("<HH", d, 16)
    assert etype == 2, "e_type 必须是 ET_EXEC(2)（内核不做动态链接）"
    assert machine == 0x3E, "e_machine 必须是 x86_64(0x3E)"
    entry = struct.unpack_from("<Q", d, 24)[0]
    phoff = struct.unpack_from("<Q", d, 32)[0]
    phentsize, phnum = struct.unpack_from("<H", d, 54)[0], struct.unpack_from("<H", d, 56)[0]
    assert phentsize == 56, "e_phentsize 必须是 56"
    assert 0 < phnum <= 16, "e_phnum 必须 0 < n <= 16"
    segs, nload, first_off, first_filesz = [], 0, None, None
    for i in range(phnum):
        p = phoff + i * phentsize
        ptype = struct.unpack_from("<I", d, p)[0]
        poff, pva, _ppa, pfsz, pmsz, _al = struct.unpack_from("<QQQQQQ", d, p + 8)
        if ptype == 3:
            raise AssertionError("出现 PT_INTERP（内核只做静态装载）")
        if ptype == 2:
            raise AssertionError("出现 PT_DYNAMIC（静态链接不该有）")
        if ptype != 1:
            continue
        nload += 1
        if first_off is None:
            first_off, first_filesz = poff, pfsz
        assert pmsz >= pfsz, "p_memsz < p_filesz"
        assert poff + pfsz <= len(d), "段文件范围越界"
        assert pva >= LO and pva + pmsz <= HI, "PT_LOAD 越出装载区（va=0x%x memsz=0x%x）" % (pva, pmsz)
        assert (pva + pmsz + 0xFFF) & ~0xFFF <= HI, "PT_LOAD 页对齐后压到用户栈区"
        segs.append((pva, pmsz))
    assert nload > 0, "没有 PT_LOAD"
    assert any(va <= entry < va + msz for va, msz in segs), "入口不在任何 PT_LOAD 内"
    assert first_off == 0 and phoff + phnum * phentsize <= first_filesz, \
        "程序头表不在第一个 PT_LOAD 内（auxv AT_PHDR 会是 0）"
    assert len(d) <= ELF_MAX_BYTES, "文件超过内核读盘缓冲 96 KiB"
    print("    ELF 自检 OK：entry=0x%x phnum=%d PT_LOAD=%d size=%d B span=0x%x..0x%x"
          % (entry, phnum, nload, len(d), segs[0][0], segs[-1][0] + segs[-1][1]))


def check_blob(path):
    n = os.path.getsize(path)
    assert 0 < n <= BLOB_MAX_BYTES, "blob 大小 %d 不在 1..%d（8 页）" % (n, BLOB_MAX_BYTES)
    print("    blob 自检 OK：size=%d B（上限 %d B = 8 页）" % (n, BLOB_MAX_BYTES))


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    for path in argv[1:]:
        if not os.path.isfile(path):
            sys.stderr.write("找不到 %s\n" % path)
            return 2
        print("  -> %s" % path)
        try:
            if path.endswith(".bin"):
                check_blob(path)
            else:
                check_elf(path)
        except AssertionError as e:
            sys.stderr.write("  [FAIL] %s：%s\n" % (path, e))
            return 1
    print("  自检结论：全部通过（%d 个文件）" % (len(argv) - 1))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
