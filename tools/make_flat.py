#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_flat.py - 把 PE32+ 镜像"摊平"成裸二进制（用于 UEFI 平铺长模式引导器）

为什么需要它：我们的 UEFI 引导逻辑要避开 EDK2 的 PE 加载器（实测它拒绝我们那个 ~8KB 的 PE），
所以把它编译成 Windows 目标（用 ms_abi，能和 EFI 服务正确对接）并链接到固定物理地址，
再摊平成裸二进制，由极小的 PE 桩读进内存直接跳过去执行。

用法：python tools/make_flat.py <in.efi> <out.bin>
"""
import struct
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    src, dst = sys.argv[1], sys.argv[2]
    d = open(src, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    if d[pe:pe + 4] != b"PE\0\0":
        print("不是 PE 文件")
        return 1
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    optsz = struct.unpack_from("<H", d, pe + 20)[0]
    opt = pe + 24
    image_base = struct.unpack_from("<Q", d, opt + 24)[0]
    size_img = struct.unpack_from("<I", d, opt + 56)[0]
    entry = struct.unpack_from("<I", d, opt + 16)[0]
    print("PE: ImageBase=0x%X SizeOfImage=0x%X EntryPoint=0x%X 节数=%d"
          % (image_base, size_img, entry, nsec))
    if image_base % 0x1000:
        print("警告：ImageBase 未按 4KB 对齐")
    buf = bytearray(size_img)
    so = opt + optsz
    for i in range(nsec):
        b = so + i * 40
        name = d[b:b + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", d, b + 8)
        copy = min(rawsize, max(0, size_img - vaddr))
        if copy > 0:
            buf[vaddr:vaddr + copy] = d[rawptr:rawptr + copy]
        print("  节 %-8s VA=0x%05X VSize=0x%-5X 拷入 %d 字节" % (name, vaddr, vsize, copy))
    open(dst, "wb").write(bytes(buf))
    print("平铺二进制：%s = %d 字节（加载地址 0x%X）" % (dst, len(buf), image_base))
    return 0


if __name__ == "__main__":
    sys.exit(main())
