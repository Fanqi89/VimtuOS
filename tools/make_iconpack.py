#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_iconpack.py - 构建期把 gui_rs/assets/icons/**.svg 光栅化成"图标包"（外置资源）

产物（都落在 build/，**不进内核**）：
  * build/icons/<system|apps>/<name>@<size>.png   逐尺寸预渲染 PNG（白 + alpha = 可运行时着色）
  * build/icons/manifest.json                     逐条目清单（kind/名字/尺寸/CRC/sha256/路径）
  * build/iconpack.bin                            **图标包**：头 + 32B 条目表 + PNG 块

图标包落盘位置（由 build64.sh 用 dd 写进系统镜像，见 kernel/icons64.h 的 ICON64_PACK_LBA）：
  **内核区（LBA 9..8008）的尾部空闲扇区** —— 内核二进制 3.7MB 之后到 LBA 8009 之间的空档。
  为什么不去 VimtuFS2：本工程 system.img 的 8073 扇区里**没有** VimtuFS2 卷（卷在安装器建的数据
  分区上，起点 LBA 8009 = kernel/memlayout64.h 的 ML64_STORE_LBA，两者重叠是已知约束）；
  装到硬盘上以后，这段内核区字节**原样保留**（loader 就是从 LBA 9 读内核的），所以：
  裸 system.img 启动、安装出来的盘、测试夹具盘（system.img 字节 + MBR + @8009 卷）三种情形
  都能读到同一份图标包。

图标包格式（小端；与 kernel/icons64.h 的 ICON64_PACK_* 一致）：
  0   char[8] magic "VIMTUI01"
  8   u32 version (=1)
  12  u32 entry_count
  16  u32 total_bytes（整包大小，含头与条目表）
  20  u32 reserved (0)
  24  条目表 entry_count * 32B：
        u32 kind / u32 size / u32 off / u32 len / u32 crc32 / u32 flags / u32 w / u32 h
      off 相对包起点；crc32 = zlib.crc32(blob)；flags bit0 = 1 表示"烘焙了颜色"（本批全是 0 = 掩码）
  24 + 32*entry_count 起：各 PNG 块（顺序即条目表顺序）

用法：py -3 tools/make_iconpack.py [--limit-bytes N] [--verify-only]
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import svg2png  # noqa: E402  （同目录的自研光栅器）

ASSETS = os.path.join(ROOT, "gui_rs", "assets", "icons")
OUT = os.path.join(ROOT, "build", "icons")
PACK = os.path.join(ROOT, "build", "iconpack.bin")
KAISI_SRC = os.path.join(ROOT, "logo", "kaisi.png")
KAISI_DST = os.path.join(ASSETS, "apps", "kaisi.png")

PACK_MAGIC = b"VIMTUI01"
PACK_VERSION = 1
ENTRY_BYTES = 32

# kind 编号必须与 kernel/icons64.h 的 ICON64_K_*/ICON64_A_* 一致（新脚本会交叉核对）
KINDS = [
    # (kind, 名字, 目录, 尺寸表, 说明)
    (1,  "globe",       "system", (16, 24, 32, 48), "未联网（地球）"),
    (2,  "ethernet",    "system", (16, 24, 32, 48), "有线网络"),
    (3,  "wifi",        "system", (16, 24, 32, 48), "无线网络"),
    (4,  "volume",      "system", (16, 24, 32, 48), "音量"),
    (5,  "volume-mute", "system", (16, 24, 32, 48), "静音"),
    (6,  "bell",        "system", (16, 24, 32, 48), "通知"),
    (7,  "search",      "system", (16, 24, 32, 48), "搜索"),
    (8,  "close",       "system", (16, 24, 32, 48), "关闭"),
    (9,  "power",       "system", (16, 24, 32, 48), "电源/关机"),
    (10, "gear",        "system", (16, 24, 32, 48), "设置（系统）"),
    (11, "lock",        "system", (16, 24, 32, 48), "锁定"),
    (12, "reboot",      "system", (16, 24, 32, 48), "重启"),
    (13, "chevron-left",  "system", (16, 24, 32, 48), "左箭头"),
    (14, "chevron-right", "system", (16, 24, 32, 48), "右箭头"),
    (15, "plus",        "system", (16, 24, 32, 48), "加号"),
    (16, "person",      "system", (16, 24, 32, 48), "用户"),
    (17, "usb",         "system", (16, 24, 32, 48), "USB 设备"),
    (18, "monitor",     "system", (16, 24, 32, 48), "显示器"),
    (19, "check",       "system", (16, 24, 32, 48), "勾选"),
    (20, "battery",     "system", (16, 24, 32, 48), "电池"),
    (64, "mypc",        "apps",   (24, 32, 48),     "此电脑/文件资源管理器"),
    (65, "recycle",     "apps",   (24, 32, 48),     "回收站"),
    (66, "terminal",    "apps",   (24, 32, 48),     "终端"),
    (67, "settings",    "apps",   (24, 32, 48),     "设置"),
    (68, "tmgr",        "apps",   (24, 32, 48),     "任务管理器"),
    (69, "mines",       "apps",   (24, 32, 48),     "扫雷"),
    (70, "calc",        "apps",   (24, 32, 48),     "计算器"),
    (71, "files",       "apps",   (24, 32, 48),     "文件管理器"),
    (72, "monitor-app", "apps",   (24, 32, 48),     "系统监视器"),
    (73, "about",       "apps",   (24, 32, 48),     "关于"),
]


def sha256(b):
    return hashlib.sha256(b).hexdigest()


def check_kaisi_copy():
    """apps/kaisi.png 必须是 logo/kaisi.png 的**同源同哈希字节副本**（结构要求它在 apps/，
    但仓库里 logo/kaisi.png 是唯一真源；这里做校验，防止两边内容漂移）。"""
    src = open(KAISI_SRC, "rb").read()
    if not os.path.exists(KAISI_DST):
        os.makedirs(os.path.dirname(KAISI_DST), exist_ok=True)
        open(KAISI_DST, "wb").write(src)
        print("  apps/kaisi.png 缺失 -> 按 logo/kaisi.png 字节副本落盘（%d B）" % len(src))
    dst = open(KAISI_DST, "rb").read()
    if sha256(src) != sha256(dst):
        raise SystemExit("ERROR: gui_rs/assets/icons/apps/kaisi.png 与 logo/kaisi.png 不同源"
                         "（sha256 不一致）—— 请删掉副本重新生成，绝不出现第二份不同内容的 kaisi")
    print("  apps/kaisi.png 与 logo/kaisi.png 同源同哈希 sha256=%s..%s（%d B）"
          % (sha256(src)[:12], sha256(src)[-8:], len(src)))


def build(limit_bytes):
    os.makedirs(OUT, exist_ok=True)
    entries = []
    blobs = []
    man = {"pack_magic": PACK_MAGIC.decode(), "pack_version": PACK_VERSION,
           "svg_license": "Bootstrap Icons (MIT) — gui_rs/assets/icons/LICENSE",
           "rasterizer": "tools/svg2png.py（自研仅填充路径光栅器，4x4 超采样）",
           "entries": []}
    for kind, name, sub, sizes, desc in KINDS:
        svg = os.path.join(ASSETS, sub, name + ".svg")
        if not os.path.exists(svg):
            raise SystemExit("ERROR: 缺 SVG %s" % svg)
        feats = None
        for size in sizes:
            cov, feats = svg2png.rasterize(svg, size, 4)
            ink = sum(1 for c in cov if c > 0.02)
            if ink == 0:
                raise SystemExit("ERROR: %s@%d 光栅化后是空白" % (name, size))
            if ink >= size * size:
                raise SystemExit("ERROR: %s@%d 光栅化后是满块（形状/填充规则可疑）" % (name, size))
            png = os.path.join(OUT, sub, "%s@%d.png" % (name, size))
            n = svg2png.write_png(cov, size, png, (255, 255, 255), mask_only=False)
            blob = open(png, "rb").read()
            off = None          # 稍后回填
            entries.append({"kind": kind, "name": name, "size": size, "w": size, "h": size,
                            "off": off, "len": len(blob), "crc32": zlib.crc32(blob) & 0xFFFFFFFF,
                            "flags": 0, "png": os.path.relpath(png, ROOT).replace("\\", "/"),
                            "sha256": sha256(blob), "bytes": n})
            blobs.append(blob)
        print("  %-13s kind=%-3d sizes=%s ink>0  features=%s"
              % (name, kind, ",".join(str(s) for s in sizes), ",".join(sorted(feats))))

    hdr = struct.pack("<8sIIII", PACK_MAGIC, PACK_VERSION, len(entries), 0, 0)
    base = len(hdr) + ENTRY_BYTES * len(entries)
    off = base
    for e in entries:
        e["off"] = off
        off += e["len"]
    total = off
    if total > limit_bytes:
        raise SystemExit("ERROR: 图标包 %d B 超过上限 %d B（内核区尾部空闲不够用）" % (total, limit_bytes))

    with open(PACK, "wb") as f:
        f.write(struct.pack("<8sIIII", PACK_MAGIC, PACK_VERSION, len(entries), total, 0))
        for e in entries:
            f.write(struct.pack("<IIIIIIII", e["kind"], e["size"], e["off"], e["len"],
                                e["crc32"], e["flags"], e["w"], e["h"]))
        for b in blobs:
            f.write(b)
    pack_bytes = open(PACK, "rb").read()
    assert len(pack_bytes) == total, (len(pack_bytes), total)

    man["entries"] = entries
    man["entry_count"] = len(entries)
    man["pack_bytes"] = total
    man["pack_sectors"] = (total + 511) // 512
    man["pack_sha256"] = sha256(pack_bytes)
    man["pack_path"] = os.path.relpath(PACK, ROOT).replace("\\", "/")
    json_path = os.path.join(OUT, "manifest.json")
    with open(json_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(man, f, ensure_ascii=False, indent=1)
    print("==> %s：%d 个图标 / %d 个尺寸条目 / %d B（%d 扇区）/ sha256=%s"
          % (os.path.relpath(PACK, ROOT).replace("\\", "/"), len(KINDS), len(entries), total,
             man["pack_sectors"], man["pack_sha256"][:16]))
    print("==> %s" % os.path.relpath(json_path, ROOT).replace("\\", "/"))
    return total


def main():
    ap = argparse.ArgumentParser()
    # 内核区尾部空闲上限：内核区 4,096,000 B - 现状内核 ~3,697,088 B ≈ 398,912 B，留 ~40KB 余量
    ap.add_argument("--limit-bytes", type=int, default=360 << 10)
    ap.add_argument("--verify-only", action="store_true")
    args = ap.parse_args()
    if args.verify_only:
        if not os.path.exists(PACK):
            raise SystemExit("ERROR: %s 不存在（先跑一次构建）" % PACK)
        b = open(PACK, "rb").read()
        magic, ver, n, total, _ = struct.unpack("<8sIIII", b[:24])
        print("pack %s magic=%s ver=%d entries=%d total=%d ok=%s"
              % (PACK, magic.decode(errors="replace"), ver, n, total, total == len(b)))
        return 0
    print("==> 图标包（tools/svg2png.py 自研光栅器：白 + alpha 掩码，运行期按主题 Token 着色）")
    check_kaisi_copy()
    build(args.limit_bytes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
