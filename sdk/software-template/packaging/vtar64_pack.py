#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/packaging/vtar64_pack.py - "类 deb/tar" 的**简单包骨架**（VimtuOS 侧自定义）

★ 先把话说清楚（**如实**）：VimtuOS **没有** dpkg/apt/仓库/依赖解析，也**没有**"应用商店"。
  这个格式只是一个**自证的归档骨架**：把"程序 + 图标 + 元数据 + 校验和"打成**一个文件**，
  便于分发/校验/解包；安装仍然是"把它里面的文件写进系统卷"（见 packaging/vol_install.py）。
  与真 deb 的差距：没有 control Depends/Conflicts、没有 preinst/postinst、没有签名、没有版本升级策略。

容器布局（小端，全部定长头 + 连续载荷，便于用几十行解析）：
  0    8B   magic  "VTAR64\\1\\0"        （\\0 结尾；1 = 版本）
  8    4B   entry_count
  12   4B   total_bytes（整包长度，含头、条目表、载荷与尾校验）
  16   16B  保留（0）
  32   条目表 entry_count * 64B：
          u32 name_len   （<= 63，不含 NUL）
          u32 data_off   （相对包起点的绝对偏移）
          u32 data_len
          u32 mode       （VimtuFS2 模式：0755 / 0644）
          u32 crc32      （zlib.crc32(数据)）
          u32 reserved   （0）
          char name[40]  （NUL 结尾；用 '/' 分目录，如 bin/hello-cli.elf）
  32 + 64*n 起：各条目数据（顺序 = 条目表顺序，允许 16 字节对齐填充）
  末尾： 8B "VTAREND\\1" + 32B sha256(上面全部字节)

用法：
    py -3 sdk/software-template/packaging/vtar64_pack.py -o build64/sdk/hello-cli.vtar64 \\
          --add 0755:bin/hello-cli.elf=build64/sdk/hello-cli.elf \\
          --add 0644:icons/apps/editor.svg=gui_rs/assets/icons/apps/editor.svg
    py -3 sdk/software-template/packaging/vtar64_pack.py --list build64/sdk/hello-cli.vtar64
    py -3 sdk/software-template/packaging/vtar64_pack.py --verify build64/sdk/hello-cli.vtar64
    py -3 sdk/software-template/packaging/vtar64_pack.py --extract build64/sdk/hello-cli.vtar64 --dest build64/sdk/unpacked

退出码：0 = 成功；1 = 校验失败；2 = 参数/文件问题。
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

MAGIC = b"VTAR64\1\0"
TAIL_MAGIC = b"VTAREND\1"
ENTRY = 64
NAME_FIELD = 40


def build(entries, out_path):
    """entries: [(name, data, mode)] -> 写文件，返回总字节数。"""
    n = len(entries)
    hdr_size = 32 + ENTRY * n
    off = (hdr_size + 15) & ~15
    blobs = []
    table = []
    for name, data, mode in entries:
        nb = name.encode("utf-8")
        assert 0 < len(nb) <= NAME_FIELD - 1, "条目名过长/为空：%s" % name
        table.append((len(nb), off, len(data), mode, zlib.crc32(data) & 0xFFFFFFFF))
        blobs.append((name, data))
        off = (off + len(data) + 15) & ~15
    total = off + 8 + 32
    buf = bytearray(total)
    buf[0:8] = MAGIC
    struct.pack_into("<III", buf, 8, n, total, 0)
    for i, (nl, doff, dlen, mode, crc) in enumerate(table):
        p = 32 + ENTRY * i
        struct.pack_into("<6I", buf, p, nl, doff, dlen, mode, crc, 0)
        buf[p + 24:p + 24 + nl] = blobs[i][0].encode("utf-8")
    for i, (_name, data) in enumerate(blobs):
        doff = table[i][1]
        buf[doff:doff + len(data)] = data
    buf[off:off + 8] = TAIL_MAGIC
    buf[off + 8:off + 8 + 32] = hashlib.sha256(bytes(buf[:off + 8])).digest()
    with open(out_path, "wb") as f:
        f.write(bytes(buf))
    return total


def parse(path):
    """独立解析 + 逐条校验（CRC + 尾 sha256）。返回 (entries, warnings)。"""
    d = open(path, "rb").read()
    warn = []
    assert len(d) >= 32 + 8 + 32, "文件太短"
    assert d[0:8] == MAGIC, "magic 不是 VTAR64"
    n, total, _rsv = struct.unpack_from("<III", d, 8)
    assert total == len(d), "total_bytes(%d) != 实际长度(%d)" % (total, len(d))
    assert n >= 0 and 32 + ENTRY * n + 8 + 32 <= len(d), "entry_count 与长度不符"
    entries = []
    for i in range(n):
        p = 32 + ENTRY * i
        nl, doff, dlen, mode, crc, _r = struct.unpack_from("<6I", d, p)
        assert 0 < nl <= NAME_FIELD - 1, "条目 %d 名字长度非法" % i
        name_b = d[p + 24:p + 24 + nl]
        assert d[p + 24 + nl] == 0, "条目 %d 名字没有 NUL 结尾" % i
        name = name_b.decode("utf-8")
        assert doff + dlen <= len(d), "条目 %s 数据越界" % name
        data = d[doff:doff + dlen]
        got = zlib.crc32(data) & 0xFFFFFFFF
        assert got == crc, "条目 %s CRC32 不一致（0x%08X != 0x%08X）" % (name, got, crc)
        entries.append({"name": name, "off": doff, "len": dlen, "mode": mode, "data": data,
                        "sha256": hashlib.sha256(data).hexdigest()})
    # 尾部：magic + 整包 sha256（覆盖 magic 之前的所有字节）
    assert d[-40:-32] == TAIL_MAGIC, "尾部 magic 不对"
    body = d[:len(d) - 32]
    sha = hashlib.sha256(body).digest()
    assert sha == d[-32:], "整包 sha256 不一致（文件被改过）"
    for e in entries:
        if not e["name"].endswith(".sha256sum"):
            warn.append(e["name"])
    return entries, warn


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", help="输出 .vtar64")
    ap.add_argument("--add", action="append", default=[],
                    help="<模式>:<包内路径>=<宿主文件>（可重复，例：--add 0755:bin/app.elf=build64/app.elf）")
    ap.add_argument("--list", metavar="FILE")
    ap.add_argument("--verify", metavar="FILE")
    ap.add_argument("--extract", metavar="FILE")
    ap.add_argument("--dest", default="build64/sdk/unpacked")
    args = ap.parse_args(argv[1:])

    if args.list or args.verify:
        path = args.list or args.verify
        entries, _ = parse(path)
        print("[VTAR64] %s：%d 个条目" % (path, len(entries)))
        for e in entries:
            print("   %-34s %8d B mode=0%o sha256=%s" % (e["name"], e["len"], e["mode"], e["sha256"][:16]))
        print("[VTAR64] verify OK（逐条 CRC32 + 整包 sha256 都通过）")
        return 0

    if args.extract:
        entries, _ = parse(args.extract)
        for e in entries:
            dst = os.path.join(args.dest, e["name"].replace("/", os.sep))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as f:
                f.write(e["data"])
            print("   解开 %-34s %8d B -> %s" % (e["name"], e["len"], dst))
        return 0

    if not args.out or not args.add:
        sys.stderr.write(__doc__)
        return 2
    entries = []
    for spec in args.add:
        # 语法： <八进制模式>:<包内路径>=<宿主文件>     例： 0755:bin/app.elf=build64/app.elf
        if ":" not in spec or "=" not in spec:
            sys.stderr.write("--add 用法：--add 0755:bin/x=宿主路径（收到 %r）\n" % spec)
            return 2
        mode_s, kv = spec.split(":", 1)
        name, host = kv.split("=", 1)
        if not os.path.isfile(host):
            sys.stderr.write("找不到 %s\n" % host)
            return 2
        entries.append((name, open(host, "rb").read(), int(mode_s, 8)))
    # 惯例（类 deb/tar 的 SHA256SUMS）：把逐文件校验和也放进包里，便于第三方独立核对
    sums = "".join("%s  %s\n" % (hashlib.sha256(d).hexdigest(), nm) for nm, d, _m in entries).encode()
    entries.append(("SHA256SUMS", sums, 0o644))
    total = build(entries, args.out)
    entries2, _ = parse(args.out)                       # 写完立刻独立回读校验
    print("[VTAR64] wrote %s entries=%d bytes=%d（独立回读：CRC32 + 整包 sha256 通过）"
          % (args.out, len(entries2), total))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
