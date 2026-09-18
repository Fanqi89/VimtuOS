#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/pe_info.py - 检查 PE32+（EFI 镜像）头部，排查"固件为什么加载不了它"

为什么需要：EDK2 的 PE 加载器对镜像有一堆隐含要求（首选基址必须在可用内存里、
必须带重定位表或基址为 0、子系统/对齐/段数…）。出问题时固件只给一句
"Not Found" 或 "is not an image"，靠肉眼看十六进制很难定位。本工具把关键字段
和人话解释一起打出来。

用法：python tools/pe_info.py build64/BOOTX64.EFI [更多文件…]
"""
import struct
import sys


def u16(b, o):
    return struct.unpack_from("<H", b, o)[0]


def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]


def u64(b, o):
    return struct.unpack_from("<Q", b, o)[0]


SUBSYS = {0: "unknown", 1: "native", 2: "windows_gui", 3: "windows_cui",
          10: "efi_application", 11: "efi_boot_service_driver", 12: "efi_runtime_driver",
          13: "efi_rom", 14: "xbox"}


def check(path):
    with open(path, "rb") as f:
        b = f.read()
    print("=" * 74)
    print("%s  (%d 字节)" % (path, len(b)))
    if b[:2] != b"MZ":
        print("  ✗ 不是 MZ/PE 文件")
        return
    e_lfanew = u32(b, 0x3C)
    if b[e_lfanew:e_lfanew + 4] != b"PE\0\0":
        print("  ✗ 缺少 PE 签名（e_lfanew=0x%X）" % e_lfanew)
        return
    coff = e_lfanew + 4
    machine = u16(b, coff)
    nsec = u16(b, coff + 2)
    chars = u16(b, coff + 18)
    opt_size = u16(b, coff + 16)
    opt = coff + 20
    magic = u16(b, opt)
    print("  机器=0x%04X（0x8664=x64）  段数=%d  可选头=%d 字节  特征=0x%04X"
          % (machine, nsec, opt_size, chars))
    if magic != 0x20B:
        print("  ✗ 不是 PE32+（magic=0x%X；0x20B 才是 64 位）" % magic)
    base = u64(b, opt + 24)
    sect_align = u32(b, opt + 32)
    file_align = u32(b, opt + 36)
    size_image = u32(b, opt + 56)
    size_hdr = u32(b, opt + 60)
    subsys = u16(b, opt + 68)
    nrv = u32(b, opt + 108)
    print("  首选基址 ImageBase  = 0x%X  (%s)" % (
        base, "0 → 固件可放在任意可用内存（推荐）" if base == 0 else
        "★ 非 0：固件会先尝试按这个地址分配，失败就放弃加载"))
    print("  SizeOfImage        = 0x%X (%d 字节)" % (size_image, size_image))
    print("  段对齐/文件对齐     = 0x%X / 0x%X" % (sect_align, file_align))
    print("  子系统             = %d (%s)" % (subsys, SUBSYS.get(subsys, "?")))
    print("  数据目录项数        = %d" % nrv)
    # 数据目录：重定位表在第 5 项（索引 5）
    dd = opt + 112
    reloc_rva, reloc_size = u32(b, dd + 5 * 8), u32(b, dd + 5 * 8 + 4)
    print("  重定位表            = RVA 0x%X 大小 0x%X  (%s)"
          % (reloc_rva, reloc_size, "有" if reloc_size else "★ 无 → 固件无法重定位"))
    strip = bool(chars & 0x0001)
    print("  RELOCS_STRIPPED 标志 = %s" % ("★ 是 → 只能在首选基址加载" if strip else "否"))
    print("  --- 段 ---")
    sh = opt + opt_size
    has_reloc = False
    for i in range(nsec):
        o = sh + 40 * i
        name = b[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize = u32(b, o + 8)
        va = u32(b, o + 12)
        raw = u32(b, o + 20)
        flags = u32(b, o + 36)
        if name == ".reloc":
            has_reloc = True
        print("    %-8s VA=0x%06X vsize=0x%06X raw=0x%06X flags=0x%08X" % (name, va, vsize, raw, flags))
    verdict = []
    if base != 0 and not has_reloc:
        verdict.append("★ 致命：非 0 基址 + 无重定位 → 固件只能按 0x%X 分配，物理内存不够就加载失败" % base)
    if strip:
        verdict.append("★ 致命：RELOCS_STRIPPED")
    if not has_reloc and base == 0:
        verdict.append("注意：无 .reloc，但基址为 0，固件可任意放置（不可重定位也无妨）")
    if subsys != 10 and subsys != 11 and subsys != 12 and subsys != 13:
        verdict.append("注意：子系统是 %s，EFI 应用应为 10" % SUBSYS.get(subsys))
    if file_align > 0x200:
        verdict.append("注意：FileAlignment=0x%X 偏大（EFI 镜像常见 0x200）" % file_align)
    print("  --- 结论 ---")
    print("    %s" % ("\n    ".join(verdict) if verdict else "无明显问题"))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("用法：python tools/pe_info.py <pe文件> [更多…]")
    for p in sys.argv[1:]:
        check(p)
