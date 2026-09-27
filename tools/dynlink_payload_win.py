#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/dynlink_payload_win.py - ★ A3 下半：把动态链接产物写进 system.img 的**裸扇区载荷**

为什么走"载荷"而不是内嵌进内核（体积纪律，见任务说明）：
  内核区（LBA 9..8008 = 4 MiB）的**尾部 512 扇区**本来是外置图标包的预留区间
  （kernel/icons64.h 的 ICON64_PACK_MAX_SECTORS；实际图标包只有 ~100 扇区）。
  本批把动态链接的 4 份产物写到该区间的**最后 128 扇区**（= 64 KiB，LBA 7881..8008），
  由内核在启动期用 ata64_read 读出来、再幂等装进 VimtuFS2：
      /lib/ldvimtu.so   我们的动态链接器
      /lib/libfoo.so    演示共享库
      /dynhello.elf     动态主程序（PT_INTERP=/lib/ldvimtu.so）
      /xmmsse.elf       FPU/xmm 上下文回归程序（两个进程同时跑）
  好处：内核镜像**一个字节都不用涨**（4 份产物合计 ~17 KB 全在盘上）；
  代价：这些字节进不了"内核二进制"，所以必须靠 system.img（安装介质载荷 / 测试夹具）传递 ——
  测试夹具正是"system.img 的字节 + 标准 MBR + 空 VimtuFS2 卷"，所以这条路对夹具成立。

布局（全部扇区对齐）：
  LBA L（由 build64.sh 给：ML64_KERNEL_LBA + ML64_KERNEL_SECTORS - 128 = 7881）
  L+0  : 头扇区：magic "VIMTUDYN" + version + count + total_sectors + record_bytes
         然后紧跟 count × 记录（name[48] + u32 size + u32 off_sectors）
  数据  : 第 off_sectors 扇区起（相对 L），每份产物单独占整数扇区

用法（build64.sh 调用）：
  py -3 tools/dynlink_payload_win.py build64/system.img 7881 /lib/x.so=build64/x.so ...
"""
import os
import struct
import sys

MAGIC = b"VIMTUDYN"
VERSION = 1
SECTOR = 512
MAX_FILES = 8
NAME_MAX = 48
RECORD = NAME_MAX + 8            # name[48] + size(u32) + off_sectors(u32)
PAYLOAD_SECTORS = 128            # 与 kernel/kernel64.cpp 的 DYNLINK_PAYLOAD_SECTORS64 一致


def main():
    if len(sys.argv) < 5:
        sys.stderr.write(__doc__)
        return 2
    img = sys.argv[1]
    lba = int(sys.argv[2])
    payload_sectors = int(sys.argv[3])
    if payload_sectors != PAYLOAD_SECTORS:
        sys.stderr.write("ERROR: payload_sectors=%d 与内核常量 %d 不一致\n" % (payload_sectors, PAYLOAD_SECTORS))
        return 2

    plans = []
    off = 1                                              # 头占第 0 扇区
    for spec in sys.argv[4:]:
        name, _, path = spec.partition("=")
        if not name or not path or not os.path.exists(path):
            sys.stderr.write("ERROR: 参数必须是 <vfs路径>=<宿主文件>，且文件存在：%r\n" % spec)
            return 2
        nb = name.encode("ascii")
        if len(nb) >= NAME_MAX:
            sys.stderr.write("ERROR: 路径太长（>=%d）：%s\n" % (NAME_MAX, name))
            return 2
        blob = open(path, "rb").read()
        need = (len(blob) + SECTOR - 1) // SECTOR
        rec = bytearray(RECORD)
        rec[0:len(nb)] = nb
        struct.pack_into("<II", rec, NAME_MAX, len(blob), off)
        plans.append((name, blob, off, rec))
        off += need
    if len(plans) == 0 or len(plans) > MAX_FILES:
        sys.stderr.write("ERROR: 文件数必须是 1..%d\n" % MAX_FILES)
        return 2
    if off > payload_sectors:
        sys.stderr.write("ERROR: 载荷需要 %d 扇区 > 预留 %d 扇区\n" % (off, payload_sectors))
        return 2

    hdr = bytearray(SECTOR)
    hdr[0:8] = MAGIC
    struct.pack_into("<IIII", hdr, 8, VERSION, len(plans), payload_sectors, RECORD)
    hdr[32:32 + len(plans) * RECORD] = b"".join(p[3] for p in plans)   # 记录表从 32 字节起

    size = os.path.getsize(img)
    if size % SECTOR:
        sys.stderr.write("ERROR: %s 不是整数扇区大小\n" % img)
        return 2
    if (lba + payload_sectors) * SECTOR > size:
        sys.stderr.write("ERROR: 载荷区间 LBA %d..%d 超出 %s（%d 扇区）\n"
                         % (lba, lba + payload_sectors - 1, img, size // SECTOR))
        return 2

    with open(img, "r+b") as f:
        f.seek(lba * SECTOR)
        f.write(bytes(hdr))
        for (name, blob, off, _rec) in plans:
            f.seek((lba + off) * SECTOR)
            f.write(blob)
    print("    dynlink payload @ LBA %d..%d（%d 扇区 = %d B）：%s"
          % (lba, lba + payload_sectors - 1, payload_sectors, payload_sectors * SECTOR,
             ", ".join("%s(%dB)" % (p[0], len(p[1])) for p in plans)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
