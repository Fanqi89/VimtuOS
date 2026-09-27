#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/dynlink_check_win.py - ★ A3 下半：动态链接三件产物的**构建期自检**（纯 Python，不依赖 binutils）

为什么需要它（与 tools/musl_build_win.sh 里那段内联 python 自检同一个理由）：
  内核装载解释器/主程序时有一串硬约束（段必须落在用户窗口、ELF 头 + 程序头表必须在第一个
  PT_LOAD 里、可写数据段与只读代码段必须分开、解释器 span 要放得进 [USER64_MMAP_VA64, 窗口顶]）。
  这些约束一旦被链接脚本破坏，只能等 QEMU 里才报 `[ELF64] reject …`；这里在构建期就拦住，
  并把"三类重定位都在"（RELATIVE / GLOB_DAT / JUMP_SLOT / 64）一起断言掉。

用法（tools/dynlink_build_win.sh 会自己带参数调用）：
    py -3 tools/dynlink_check_win.py ldso      build64/ldvimtu.so
    py -3 tools/dynlink_check_win.py libfoo    build64/libfoo.so
    py -3 tools/dynlink_check_win.py dynhello  build64/dynhello.elf
    py -3 tools/dynlink_check_win.py interp    build64/dynhello.elf   # 只打印 PT_INTERP 字符串
退出码：0 = 全过；2 = 断言失败（stderr 打 reason）。
"""
import struct
import sys

PHDR_SIZE = 56

PT_LOAD = 1
PT_DYNAMIC = 2
PT_INTERP = 3

DT_NEEDED = 1
DT_PLTRELSZ = 2
DT_RELA = 7
DT_RELASZ = 8
DT_INIT = 12
DT_SONAME = 14
DT_JMPREL = 23
DT_INIT_ARRAY = 25
DT_INIT_ARRAYSZ = 27

R_X86_64_64 = 1
R_X86_64_GLOB_DAT = 6
R_X86_64_JUMP_SLOT = 7
R_X86_64_RELATIVE = 8

USER64_CODE_VA = 0x0000000100000000          # 4GiB（kernel/usermode64.h 的 USER64_CODE_VA64）
USER64_LOAD_HI = USER64_CODE_VA + 0x10000    # 装载区上界（USER64_STACK_VA64）
USER64_MMAP_VA = USER64_CODE_VA + 0x90000    # mmap 区起点（解释器只能落在它之上）
INTERP_PATH = "/lib/ldvimtu.so"


def need(cond, why):
    if not cond:
        sys.stderr.write("ERROR: " + why + "\n")
        raise SystemExit(2)


class Elf:
    def __init__(self, path):
        self.path = path
        self.data = open(path, "rb").read()
        d = self.data
        need(d[:4] == b"\x7fELF" and d[4] == 2 and d[5] == 1, "%s 不是 ELF64 小端" % path)
        self.etype, self.machine = struct.unpack_from("<HH", d, 16)
        self.entry = struct.unpack_from("<Q", d, 24)[0]
        self.phoff = struct.unpack_from("<Q", d, 32)[0]
        self.phentsize, self.phnum = struct.unpack_from("<HH", d, 54)
        need(self.machine == 0x3E, "%s: machine 不是 x86_64" % path)
        need(self.phentsize == PHDR_SIZE, "%s: e_phentsize != 56" % path)
        need(0 < self.phnum <= 16, "%s: e_phnum 越界（内核上限 16）" % path)
        self.phdrs = []
        for i in range(self.phnum):
            p = self.phoff + i * self.phentsize
            t, fl, off, va, pa, fsz, msz, al = struct.unpack_from("<IIQQQQQQ", d, p)
            self.phdrs.append(dict(type=t, flags=fl, off=off, va=va, filesz=fsz, memsz=msz, align=al))

    def phdr(self, t):
        for p in self.phdrs:
            if p["type"] == t:
                return p
        return None

    def phdrs_of(self, t):
        return [p for p in self.phdrs if p["type"] == t]

    def foff(self, va):
        for p in self.phdrs_of(PT_LOAD):
            if p["va"] <= va < p["va"] + p["filesz"]:
                return p["off"] + (va - p["va"])
        raise SystemExit("ERROR: %s VA 0x%x 不在任何 PT_LOAD 的文件范围里" % (self.path, va))

    def dyn_entries(self):
        p = self.phdr(PT_DYNAMIC)
        if not p:
            return []
        out = []
        for i in range(p["filesz"] // 16):
            tag, val = struct.unpack_from("<qQ", self.data, p["off"] + i * 16)
            if tag == 0:
                break
            out.append((tag, val))
        return out

    def reloc_types(self, tab_va, sz):
        types = set()
        if not tab_va or not sz:
            return types
        for i in range(sz // 24):
            _, info, _ = struct.unpack_from("<QQq", self.data, self.foff(tab_va + i * 24))
            types.add(info & 0xFFFFFFFF)
        return types

    def all_reloc_types(self):
        d = dict(self.dyn_entries())
        types = self.reloc_types(d.get(DT_RELA, 0), d.get(DT_RELASZ, 0))
        types |= self.reloc_types(d.get(DT_JMPREL, 0), d.get(DT_PLTRELSZ, 0))
        return types

    def interp_path(self):
        ip = self.phdr(PT_INTERP)
        if not ip:
            return None
        return self.data[ip["off"]:ip["off"] + ip["filesz"]].split(b"\0")[0].decode("ascii", "replace")

def check_loads(e, base_va, hi):
    """段约束（与 kernel/elf64.cpp 的装载检查一致）：
       * 至少 2 个 PT_LOAD，且有 R|X 段与有 R|W 段（可写数据段与只读代码段必须分开）；
       * 第一个 PT_LOAD 从文件偏移 0 / VA 0 起（ELF 头 + 程序头表都在里面：AT_BASE/AT_PHDR 才有值）；
       * 段页对齐、p_memsz >= p_filesz、全部落在 [0, hi] 之内。"""
    loads = e.phdrs_of(PT_LOAD)
    need(len(loads) >= 2, "%s: PT_LOAD 至少要有 2 段（R|X + R|W），实际 %d" % (e.path, len(loads)))
    fl = [p["flags"] for p in loads]
    need(any((f & 7) == 5 for f in fl), "%s: 没有 R|X 段（代码段必须可执行）" % e.path)
    need(any((f & 7) == 6 for f in fl), "%s: 没有 R|W 段（数据段必须可写）" % e.path)
    need(loads[0]["off"] == 0 and loads[0]["va"] == base_va,
         "%s: 第一个 PT_LOAD 必须从文件偏移 0 / VA 0x%x 起（含 ELF 头与程序头表）" % (e.path, base_va))
    need(loads[0]["filesz"] >= e.phoff + e.phnum * PHDR_SIZE,
         "%s: 程序头表必须落在第一个 PT_LOAD 里（AT_BASE/AT_PHDR 才有值）" % e.path)
    for p in loads:
        need(p["memsz"] >= p["filesz"], "%s: p_memsz < p_filesz" % e.path)
        need(p["va"] % 0x1000 == p["off"] % 0x1000,
             "%s: 段必须满足 p_offset ≡ p_vaddr (mod 4096)" % e.path)
        need(p["va"] + p["memsz"] <= hi,
             "%s: 段越出允许区间（va=0x%x memsz=0x%x 上界=0x%x）" % (e.path, p["va"], p["memsz"], hi))
    need(e.phdr(PT_DYNAMIC) is not None, "%s: 没有 PT_DYNAMIC" % e.path)


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("用法: py -3 tools/dynlink_check_win.py <ldso|libfoo|dynhello|interp> <file>\n")
        return 2
    mode, path = sys.argv[1], sys.argv[2]
    e = Elf(path)

    if mode == "ldso":
        need(e.etype == 3, "ldso: e_type 必须是 ET_DYN(3)（内核按 AT_BASE 映射）")
        check_loads(e, 0, USER64_CODE_VA + 0x100000)
        span = max(p["va"] + p["memsz"] for p in e.phdrs_of(PT_LOAD))
        need(span <= 0x70000, "ldso: span=0x%x 太大（放不进 [USER64_MMAP_VA64, 窗口顶] = 448KiB）" % span)
        types = e.all_reloc_types()
        need(R_X86_64_RELATIVE in types, "ldso: 至少要有 1 条 R_X86_64_RELATIVE（自定位证据）")
        print("    ldso OK: ET_DYN span=0x%x reloc_types=%s" % (span, sorted(types)))

    elif mode == "libfoo":
        need(e.etype == 3, "libfoo: e_type 必须是 ET_DYN(3)")
        check_loads(e, 0, USER64_CODE_VA + 0x100000)
        types = e.all_reloc_types()
        need(R_X86_64_RELATIVE in types, "libfoo: 至少要有 1 条 R_X86_64_RELATIVE（.so 自己的指针）")
        d = dict(e.dyn_entries())
        need(DT_INIT_ARRAY in d and d[DT_INIT_ARRAY] != 0 and d.get(DT_INIT_ARRAYSZ, 0) > 0,
             "libfoo: 必须有 DT_INIT_ARRAY（构造函数）")
        need(d.get(DT_INIT, 0) != 0, "libfoo: 必须有 DT_INIT（链接参数 -init,foo_init）")
        need(d.get(DT_SONAME, 0) != 0, "libfoo: 必须有 DT_SONAME（依赖去重要用）")
        print("    libfoo OK: ET_DYN reloc_types=%s init=0x%x init_array_sz=%d"
              % (sorted(types), d[DT_INIT], d[DT_INIT_ARRAYSZ]))

    elif mode == "dynhello":
        need(e.etype == 3, "dynhello: e_type 必须是 ET_DYN(3)（PIE，用 RELATIVE 自定位）")
        check_loads(e, USER64_CODE_VA, USER64_LOAD_HI)
        need(e.interp_path() == INTERP_PATH,
             "dynhello: PT_INTERP 必须是 %r，实际 %r" % (INTERP_PATH, e.interp_path()))
        d = dict(e.dyn_entries())
        need(d.get(DT_NEEDED, 0) != 0, "dynhello: 必须有 DT_NEEDED（指向 libfoo.so）")
        types = e.all_reloc_types()
        for t, name in ((R_X86_64_RELATIVE, "RELATIVE"), (R_X86_64_GLOB_DAT, "GLOB_DAT"),
                        (R_X86_64_JUMP_SLOT, "JUMP_SLOT"), (R_X86_64_64, "64")):
            need(t in types, "dynhello: 缺少 %s 重定位（这几条路径都要被验证）" % name)
        need(DT_INIT in d, "dynhello: 必须有 DT_INIT（-init dh_init；主程序这条路径不用 .init_array）")
        print("    dynhello OK: ET_DYN interp=%s reloc_types=%s entry=0x%x"
              % (INTERP_PATH, sorted(types), e.entry))

    elif mode == "interp":
        p = e.interp_path()
        need(p is not None, "%s: 没有 PT_INTERP" % path)
        print(p)

    else:
        raise SystemExit("ERROR: 未知模式 %s" % mode)

    return 0


if __name__ == "__main__":
    sys.exit(main())
