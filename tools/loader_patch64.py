#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/loader_patch64.py - 把"内核真实长度"回填进 loader64.bin（'KSNT' 回填槽）

为什么要这个工具（v0.9.31，内核容量 4MB -> 15MiB）：
  内核**容量**是 15MiB，但系统内核实际只有 ~3MB。旧 loader 的写法是"启动时恒定读满
  KERNEL_SECTORS 个扇区"（那时容量 = 长度，都是 8000）。容量涨到 30,720 之后，如果继续
  照旧读满，每次开机都要白读 ~12MB —— QEMU 上约 0.5~1.5 秒，真机 BIOS/AHCI 同样要等。
  所以改成：构建期把**真实字节数折成扇区数**写进 loader 里的回填槽，loader 只读那么多。

回填槽（见 boot/loader64.asm 数据区的 kern_sect_sig / kern_read_secs / kern_read_atapi）：
    'K', 'S', 'N', 'T', <u16 LE 512B 扇区数>, <u16 LE 2048B 光盘扇区数>
  * 两个值的默认值 = 容量（30,720 / 7,680），所以"没回填"= 读满容量（老行为，只是慢）；
  * loader 里**没有**第二道钳位（Stage 2 容量 32 KB，运行期装不下第二道检查；见 boot/boot.asm），
    越界值必须由本工具拦下 —— 所以这里的范围检查是唯一的防线，不许放宽。

用法：
    py -3 tools/loader_patch64.py build64/loader64.bin 6131

退出码：0 = 成功；2 = 参数/文件问题（找不到签名、签名不唯一、数值越界）。"""
import os
import struct
import sys

MAGIC = b"KSNT"
# 必须与 kernel/memlayout64.h 的 ML64_KERNEL_SECTORS / boot/loader64.asm 的 KERNEL_SECTORS 一致
KERNEL_SECTORS = 30720
LOADER_MAX_BYTES = 32768          # Stage 2 容量（64 扇区 = 32 KB）


def fail(msg):
    print("loader_patch64: 错误：%s" % msg, file=sys.stderr)
    raise SystemExit(2)


def main(argv):
    if len(argv) != 3:
        print("用法：py -3 tools/loader_patch64.py <loader64.bin> <扇区数>", file=sys.stderr)
        return 2
    path = argv[1]
    try:
        sectors = int(argv[2], 0)
    except ValueError:
        fail("扇区数不是整数：%r" % (argv[2],))
    if not os.path.isfile(path):
        fail("文件不存在：%s" % path)
    size = os.path.getsize(path)
    if size > LOADER_MAX_BYTES:
        fail("loader 大小 %d B 超过 %d B 上限" % (size, LOADER_MAX_BYTES))
    if sectors <= 0 or sectors > KERNEL_SECTORS:
        fail("扇区数 %d 不在 1..%d 之间（容量闸门）" % (sectors, KERNEL_SECTORS))

    data = bytearray(open(path, "rb").read())
    hits = []
    pos = data.find(MAGIC)
    while pos >= 0:
        hits.append(pos)
        pos = data.find(MAGIC, pos + 1)
    if len(hits) != 1:
        fail("签名 'KSNT' 出现 %d 次（必须恰好 1 次；是不是 boot/loader64.asm 里被复制了？）" % len(hits))
    at = hits[0] + len(MAGIC)
    if at + 4 > len(data):
        fail("签名在文件末尾，没有 4 字节回填位（512B 扇区数 + 2048B 光盘扇区数）")
    atapi = (sectors + 3) // 4          # 512B -> 2048B，向上取整（同一份内核在光盘上按 2048B 读）
    old_secs, old_atapi = struct.unpack_from("<HH", data, at)
    struct.pack_into("<HH", data, at, sectors, atapi)
    open(path, "wb").write(bytes(data))
    print("    loader 回填：%s 偏移 %d，读盘扇区数 %d -> %d（%d B / 容量 %d 扇区），"
          "光盘扇区数 %d -> %d"
          % (path, at, old_secs, sectors, sectors * 512, KERNEL_SECTORS, old_atapi, atapi))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
