#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_vap.py - VAP64 应用包打包器（纯标准库，只用 zlib.crc32）

VAP64 = Vimtu64 自有"可安装应用"格式。布局与 kernel/app64.h 逐字段一致：

  偏移 0    8B   magic        = "VAP64\\0\\0\\0"
  偏移 8    4B   version      = 1
  偏移 12   4B   header_size  = 32
  偏移 16   4B   entry_offset = 代码入口在**文件内**的绝对偏移 = header_size + name_len
                               （名字紧跟头后，所以 hello 这种 6B 名字是 38，不是 32）
  偏移 20   4B   code_size    = 代码段字节数（1..32768，user64_run_blob64 的 8 页上限）
  偏移 24   4B   code_crc32   = zlib.crc32(代码段)，与内核侧标准 CRC-32 同口径
  偏移 28   4B   name_len     = 名字字节数（含结尾 NUL，1..24）
  偏移 32   name_len 字节     : 名字（NUL 结尾，紧跟头后，≤24B）
  随后      code_size 字节    : 代码段（平铺二进制，入口在段首）

  文件总长 = 32 + name_len + code_size（loader 用这条等式做长度一致性校验）

用法：
    python tools/make_vap.py <in.bin> <out.vap> <name>

输出（成功时恰好一行，自动验收 grep 用）：
    [VAP] wrote <out> name=<n> code=<bytes> crc=0x<hex>
"""
import os
import struct
import sys
import zlib

MAGIC = b"VAP64\0\0\0"
VERSION = 1
HEADER_SIZE = 32
NAME_MAX = 24          # 含结尾 NUL
CODE_MAX = 32768       # 与 kernel/usermode64.cpp 的"代码最多 8 页"对齐


def _fail(msg):
    sys.stderr.write("[VAP] ERROR: %s\n" % msg)
    raise SystemExit(1)


def build_vap(code, name):
    """把代码段 + 名字打成 VAP64 字节串；打包前做自检。返回 (blob, crc)。"""
    if not code:
        _fail("input code is empty")
    if len(code) > CODE_MAX:
        _fail("code too large: %d > %d" % (len(code), CODE_MAX))
    try:
        name_b = name.encode("ascii")
    except UnicodeEncodeError:
        _fail("name must be ASCII")
    if not (1 <= len(name_b) < NAME_MAX):
        _fail("name length (without NUL) must be 1..%d, got %d" % (NAME_MAX - 1, len(name_b)))
    for ch in name_b:
        if ch < 0x21 or ch > 0x7E or ch in (0x2F, 0x5C):   # 可见 ASCII，且不含 '/' '\\'
            _fail("name has illegal byte 0x%02X (printable ASCII, no '/', '\\')" % ch)
    name_len = len(name_b) + 1                              # 含结尾 NUL
    entry_offset = HEADER_SIZE + name_len
    crc = zlib.crc32(code) & 0xFFFFFFFF

    hdr = MAGIC + struct.pack("<6I", VERSION, HEADER_SIZE, entry_offset,
                              len(code), crc, name_len)
    if len(hdr) != HEADER_SIZE:
        _fail("internal: header size mismatch")
    blob = hdr + name_b + b"\0" + code
    if len(blob) != HEADER_SIZE + name_len + len(code):
        _fail("internal: total length mismatch")
    return blob, crc


def selfcheck(blob, code, name, crc):
    """打包后回读一遍：magic / 版本 / 头大小 / 长度一致性 / CRC 必须全部对上。"""
    if len(blob) < HEADER_SIZE:
        _fail("selfcheck: blob shorter than header")
    if blob[0:8] != MAGIC:
        _fail("selfcheck: magic mismatch")
    ver, hs, eo, cs, c32, nl = struct.unpack_from("<6I", blob, 8)
    if ver != VERSION or hs != HEADER_SIZE:
        _fail("selfcheck: version/header_size mismatch")
    if eo != HEADER_SIZE + nl:
        _fail("selfcheck: entry_offset != header_size + name_len")
    if len(blob) != HEADER_SIZE + nl + cs:
        _fail("selfcheck: total length != 32 + name_len + code_size")
    if c32 != zlib.crc32(blob[eo:eo + cs]) & 0xFFFFFFFF or c32 != crc:
        _fail("selfcheck: crc mismatch")
    if blob[HEADER_SIZE + nl - 1] != 0:
        _fail("selfcheck: name not NUL-terminated")
    if nl != len(name.encode("ascii")) + 1 or cs != len(code):
        _fail("selfcheck: name_len/code_size mismatch")


def main(argv):
    if len(argv) != 4:
        sys.stderr.write(__doc__)
        return 2
    in_path, out_path, name = argv[1], argv[2], argv[3]
    if not os.path.isfile(in_path):
        _fail("input file not found: %s" % in_path)
    with open(in_path, "rb") as f:
        code = f.read()

    blob, crc = build_vap(code, name)
    selfcheck(blob, code, name, crc)

    tmp = out_path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(blob)
    os.replace(tmp, out_path)

    print("[VAP] wrote %s name=%s code=%d crc=0x%08X" % (out_path, name, len(code), crc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
