#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/payload_embed_win.py —— 把数据分区（VimtuFS2 卷）**并入安装载荷** system.img。

修的是这条真缺陷（v0.5.11）：
  build64.sh 头注释写着 system.img = "一份完整的系统磁盘映像（8073 扇区）"，但实现只切了
  前 8073 扇区（MBR+引导+内核+64 扇区裸槽），**卷留在 sysdisk.img 的 8009 起**。
  于是用 ISO / 安装介质装到硬盘后，目标盘 LBA 8009 处是空字节：
      [VFS64] mount FAILED reason=magic -> mount_system FAILED
      [CONF64] WARN NOT-PERSISTENT carrier=raw reason=no-vimtufs2-volume
  之后所有写入（桌面图标位置/壁纸/主题/用户名/密码/新建文件夹）**全部失败**，重启即丢。
  （tests/iconboot64_test.py 里那句"主分区还没有卷"就是本缺陷的旧实测记录。）

本工具做的事：
  1) 读 sysdisk.img 的 MBR，取出主分区（type=0x07）的起点与扇区数（默认要求起点 = --parts-lba）
  2) 把 system.img 扩到 (主分区起点 + 主分区扇区数) * 512 字节
  3) 把 sysdisk.img[主分区起点..] 逐字节拷进 system.img 的同偏移
  4) 自检：回读 system.img 主分区起点，必须是 "VIMTUFS2" magic（不是就打错退出）
  5) 把新的总扇区数写到 --out-sectors 文件（build64.sh 用它更新 SYS_SECTORS / payload_hdr）

用法：
  py -3 tools/payload_embed_win.py --disk build64/sysdisk.img --system build64/system.img \
        --parts-lba 8009 --out-sectors build64/sys_sectors.txt
退出码：0 成功 / 1 失败（MBR/卷/magic 不符）
"""
import argparse
import os
import struct
import sys

MAGIC = b"VIMTUFS2"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", required=True, help="构建好的 sysdisk.img（含 MBR + 主分区卷）")
    ap.add_argument("--system", required=True, help="安装载荷 system.img（本工具会扩容并写入卷）")
    ap.add_argument("--parts-lba", type=int, default=8009, help="主分区起始 LBA（默认 8009）")
    ap.add_argument("--out-sectors", default="", help="把新的载荷总扇区数写到这个文件")
    args = ap.parse_args()

    if not os.path.exists(args.disk) or not os.path.exists(args.system):
        print("[payload] 缺文件：%s / %s" % (args.disk, args.system))
        return 1

    d = open(args.disk, "rb").read()
    if len(d) < 512 or d[510:512] != b"\x55\xAA":
        print("[payload] sysdisk 没有 MBR 签名")
        return 1
    part = None
    for i in range(4):
        e = d[446 + 16 * i: 446 + 16 * (i + 1)]
        if e[4] == 0x07:
            part = (struct.unpack("<I", e[8:12])[0], struct.unpack("<I", e[12:16])[0])
            break
    if not part:
        print("[payload] sysdisk 的 MBR 里没有 type=0x07 主分区")
        return 1
    start, sectors = part
    if start != args.parts_lba:
        print("[payload] 主分区起点 %d != 期望 %d" % (start, args.parts_lba))
        return 1
    if start + sectors > len(d) // 512:
        print("[payload] 主分区 %d+%d 超出 sysdisk 大小 %d 扇区"
              % (start, sectors, len(d) // 512))
        return 1

    vol = d[start * 512: (start + sectors) * 512]
    if not vol.startswith(MAGIC):
        print("[payload] sysdisk 主分区没有 VIMTUFS2 magic（卷没构建好？）")
        return 1

    total_sectors = start + sectors
    total_bytes = total_sectors * 512
    cur = os.path.getsize(args.system)
    with open(args.system, "r+b") as f:
        if cur < total_bytes:
            f.truncate(total_bytes)                 # 扩到完整映像大小（多出来的是零）
        f.seek(start * 512)
        f.write(vol)
    # 自检：回读 magma
    with open(args.system, "rb") as f:
        f.seek(start * 512)
        back = f.read(8)
    ok = back == MAGIC
    print("[payload] 载荷并入数据分区：主分区 %d+%d 扇区（%d B），system.img %d -> %d 扇区，"
          "magic=%s 自检=%s"
          % (start, sectors, len(vol), cur // 512, total_sectors,
             back.decode("latin1"), "OK" if ok else "FAIL"))
    if not ok:
        return 1
    if args.out_sectors:
        with open(args.out_sectors, "w", encoding="ascii") as f:
            f.write("%d\n" % total_sectors)
    return 0


if __name__ == "__main__":
    sys.exit(main())
