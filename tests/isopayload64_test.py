#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/isopayload64_test.py - ★ v0.5.18：ISO 载荷扇区数必须等于 SYSTEM.IMG 的真实大小（秒级，不跑 QEMU）

用户报的真缺陷（v0.5.17 及更早）：
  "预设壁纸（/Wallpapers，0 张）"、"预设头像（0 张）"，而且文件资源管理器里
  **C: 根目录根本没有 Wallpapers / Avatars 两个文件夹**。

真因（实测复现，不是猜测）：
  tools/make_iso64.py 里 `PAYLOAD_SECTORS = 8073` 是 **v0.5.11 之前** system.img 的大小，
  v0.5.11 把数据分区并入安装载荷后 system.img 长到 32768 扇区（16 MiB），但：
    * make_iso64.py 回填引导桩 `VMSS` 用的仍是这个硬编码常量；
    * loader64.asm 按 [0x0F10]（=VMSS）决定从光盘读多少字节到 0x04000000；
    * 安装器按 MediumDesc.payload_secs（=VMSS）决定往目标盘拷多少扇区。
  结果 ISO 装盘只写了 4.13 MB，而系统卷从 LBA 8009 开始 -> 只有前 64 扇区有效：
  卷超级块/位图/inode 表前段在，**inode 号靠后的条目全丢** —— 链尾打包的 /Wallpapers、
  /Avatars（以及 inode 8 之后的绝大多数文件）就都不见了。
  实测对照（同一台 QEMU、同一份 system.img）：
    修前 ISO 装盘：[INSTALL] 完成：已写 8073 扇区；卷 used=223；根下一级 = bin etc lib tcc tmp
    修后 ISO 装盘：[INSTALL] 完成：已写 32768 扇区；卷 used=404；根下一级含 Wallpapers/Avatars
  为什么长期没被发现：hybrid 硬盘介质（vimtu64-64.img）走 payload_embed_win.py 写的
  VIMTUPAY 头（扇区数正确），只有 ISO 光盘引导这条路读 VMSS。

判据（每条都能独立证伪）：
  ① ISO 里 /SYSTEM/SYSTEM.IMG 的字节数是 512 的整数倍，且 = VMSS 回填值 × 512；
  ② VMYB（载荷字节数）= SYSTEM.IMG 字节数；
  ③ ISO 里 SYSTEM.IMG 的 VimtuFS2 卷根目录下同时有 Wallpapers 与 Avatars，
     且 /Wallpapers 至少有 1 张图、/Avatars 至少 1 张图；
  ④ ISO 里的 payload 大小与 hybrid 介质（vimtu64-64.img）里 VIMTUPAY 头写的扇区数一致
     （两条安装路径必须给出同一个答案）。

用法：py -3 tests/isopayload64_test.py
退出码：0 全过 / 1 有断言失败 / 2 环境问题（缺产物）
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from volreloc64_test import Vol  # noqa: E402

ISO = os.path.join(ROOT, "vimtu64-64.iso")
IMG = os.path.join(ROOT, "vimtu64-64.img")
MAIN_LBA = 8009
SECT = 2048
IMG_EXTS = (".png", ".jpg", ".jpeg", ".bmp")

ok = True


def check(name, cond, detail=""):
    global ok
    ok = ok and bool(cond)
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def read_lba(f, lba, n=1):
    f.seek(lba * SECT)
    return f.read(SECT * n)


def walk_dir(f, lba, size):
    out = {}
    buf = read_lba(f, lba, (size + SECT - 1) // SECT)
    off = 0
    while off < len(buf):
        ln = buf[off]
        if ln == 0:
            off = (off // SECT + 1) * SECT
            continue
        rec = buf[off:off + ln]
        ext = struct.unpack_from("<I", rec, 2)[0]
        dlen = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        nlen = rec[32]
        name = rec[33:33 + nlen].decode("latin-1")
        if name not in ("\x00", "\x01"):
            out[name.split(";")[0].upper()] = (ext, dlen, bool(flags & 0x02))
        off += ln
    return out


def iso_file(f, path):
    """按 ISO9660 目录记录取文件 (lba, size)；只处理单 extent 文件。"""
    pvd = read_lba(f, 16)
    lba = struct.unpack_from("<I", pvd, 156 + 2)[0]
    size = struct.unpack_from("<I", pvd, 156 + 10)[0]
    for part in path.split("/"):
        d = walk_dir(f, lba, size)
        key = part.upper()
        if key not in d:
            return None, None
        lba, size, _ = d[key]
    return lba, size


def patched(blob, magic):
    """取 "VMxx" 魔数之后那个 u32（构建期回填值）；返回 (值列表, 命中次数)。"""
    out = []
    i = 0
    while True:
        i = blob.find(magic, i)
        if i < 0:
            break
        out.append(struct.unpack_from("<I", blob, i + 4)[0])
        i += 4
    return out, len(out)


def vol_children(blob, dirname):
    """解出 SYSTEM.IMG 里的 VimtuFS2 卷，返回某目录下的图片文件名列表（None=目录不存在）。"""
    v = Vol(blob, MAIN_LBA)
    items = []
    for i in range(v.inon):
        r = v.inode(i)
        if r[0] == 0:
            continue
        nl = r[1]
        if not (0 < nl <= 31):
            continue
        par = struct.unpack_from("<I", r, 28)[0]
        items.append((r[40:40 + nl].decode("latin1"), r[0], par, i))
    v.close()
    hit = [it for it in items if it[0] == dirname and it[1] == 2]
    if not hit:
        return None
    return sorted(it[0] for it in items if it[2] == hit[0][3])


def main():
    if not os.path.exists(ISO):
        print("跳过：缺 %s（先跑 build64.sh）" % ISO)
        return 2
    with open(ISO, "rb") as f:
        cdiso_lba, cdiso_len = iso_file(f, "BOOT/CDISO.BIN")
        ld_lba, ld_len = iso_file(f, "BOOT/LOADER64.BIN")
        sys_lba, sys_len = iso_file(f, "SYSTEM/SYSTEM.IMG")
        if sys_lba is None:
            print("跳过：ISO 里没有 /SYSTEM/SYSTEM.IMG")
            return 2
        f.seek(sys_lba * SECT)
        blob = f.read(sys_len)
        f.seek(cdiso_lba * SECT)
        cdiso = f.read(cdiso_len)
        f.seek(ld_lba * SECT)
        loader = f.read(ld_len)

    print("ISO: SYSTEM.IMG=%d B（%d 扇区），CDISO=%d B，LOADER64=%d B"
          % (sys_len, sys_len // 512, cdiso_len, ld_len))

    # ① VMSS × 512 == SYSTEM.IMG 字节数
    vmss, n1 = patched(cdiso, b"VMSS")
    check("引导桩里 VMSS 回填点存在（且只有一个值）", len(vmss) == 1 and n1 == 1, "命中=%d 值=%s" % (n1, vmss))
    if vmss:
        check("VMSS × 512 == SYSTEM.IMG 字节数（这条就是本批真缺陷的判据）",
              vmss[0] * 512 == sys_len,
              "VMSS=%d -> %d B，实际 %d B" % (vmss[0], vmss[0] * 512, sys_len))
        check("VMSS 覆盖整份载荷（不少于 16384 扇区 = 8MB）", vmss[0] >= 16384, "VMSS=%d" % vmss[0])

    # ② VMYB == 字节数（loader64 按它从光盘读）
    vmyb, n2 = patched(cdiso, b"VMYB")
    check("VMYB（载荷字节数）= SYSTEM.IMG 字节数", bool(vmyb) and vmyb[0] == sys_len,
          "VMYB=%s，实际 %d" % (vmyb, sys_len))

    # ③ 卷里两个预设目录都在，且都有图
    # ★ v0.6.0：预设搬到了 /System/themes/{wallpapers,avatars}
    for d in ("wallpapers", "avatars"):
        ch = vol_children(blob, d)
        imgs = [n for n in (ch or []) if n.lower().endswith(IMG_EXTS)]
        check("ISO 载荷卷里有 /System/themes/%s 且至少 1 张图" % d, bool(imgs),
              "%s（共 %d 项）" % (imgs, len(ch or [])))

    # ④ 与 hybrid 硬盘介质（VIMTUPAY 头）一致
    if os.path.exists(IMG):
        # 载荷头在 build64.sh 的 PAYLOAD_LBA=8192（单位 512B 扇区）处写死。
        # 不能在文件里盲搜 "VIMTUPAY"：内核/其它 blob 里也有这个字符串，会命中错的地方。
        with open(IMG, "rb") as g:
            g.seek(8192 * 512)
            head = g.read(512)
        if head[:8] == b"VIMTUPAY":
            secs = struct.unpack_from("<I", head, 8)[0]
            check("hybrid 介质 VIMTUPAY.sectors 与 ISO VMSS 一致", bool(vmss) and secs == vmss[0],
                  "img=%d，iso=%s" % (secs, vmss))
        else:
            print("  （LBA 8192 处没有 VIMTUPAY 头：magic=%r，跳过对照）" % head[:8])

    print("=== RESULT: %s ===" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
