#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/packaging/vap64_pack.py - 把**平铺二进制**打成 VAP64 应用包

VAP64 是 VimtuOS 自有的"可安装应用"格式，逐字段定义点 = `kernel/app64.h`（布局见下方），
仓库里已有的打包器 = `tools/make_vap.py`（**同一个真源**：本脚本直接 import 它来打包，
另用一个**独立解析器**做回读校验，所以"打包器自检"和"独立校验"是两条路）。

布局（小端；本脚本只做转发 + 独立校验，字段含义见 tools/make_vap.py 头部）：
  0   8B  magic "VAP64\\0\\0\\0"        | 8   4B  version = 1
  12  4B  header_size = 32              | 16  4B  entry_offset = 32 + name_len
  20  4B  code_size（1..32768）         | 24  4B  code_crc32 = zlib.crc32(code)
  28  4B  name_len（含 NUL，1..24）     | 32  name_len 字节名字（NUL 结尾）
  随后 code_size 字节代码段（入口在段首）

用法：
    # 打包（输入必须是 objcopy -O binary 出来的**平铺** blob，不是 ELF）
    py -3 sdk/software-template/packaging/vap64_pack.py build64/sdk/hello-cli.bin \\
          build64/sdk/hello-cli.vap hello-cli

    # 只校验一个已有 .vap（独立解析，不信打包器自检）
    py -3 sdk/software-template/packaging/vap64_pack.py --verify build64/sdk/hello-cli.vap

退出码：0 = 成功；1 = 校验失败；2 = 参数/文件问题。
"""
import argparse
import importlib.util
import os
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))     # sdk/software-template -> 仓库根

MAGIC = b"VAP64\0\0\0"
HEADER = 32
CODE_MAX = 32768
NAME_MAX = 24


def load_make_vap():
    """复用 tools/make_vap.py（唯一真源：布局/上限/自检都与构建期的产物同一条路径）。"""
    path = os.path.join(ROOT, "tools", "make_vap.py")
    spec = importlib.util.spec_from_file_location("make_vap", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_vap(blob):
    """★ 独立解析（这里**不**复用 tools/make_vap.py 的 selfcheck）：返回 dict 或抛 AssertionError。"""
    assert len(blob) >= HEADER, "文件比 32B 头还短"
    assert blob[0:8] == MAGIC, "magic 不是 VAP64\\0\\0\\0"
    ver, hs, eo, cs, crc, nl = struct.unpack_from("<6I", blob, 8)
    assert ver == 1 and hs == HEADER, "version/header_size 不是 1/32"
    assert 1 <= nl < NAME_MAX, "name_len 越界"
    assert eo == HEADER + nl, "entry_offset != 32 + name_len"
    assert len(blob) == HEADER + nl + cs, "总长 != 32 + name_len + code_size"
    assert 1 <= cs <= CODE_MAX, "code_size 越界（上限 %d）" % CODE_MAX
    name_b = blob[HEADER:HEADER + nl]
    assert name_b[-1] == 0, "名字没有 NUL 结尾"
    name = name_b[:-1].decode("ascii")
    code_crc = zlib.crc32(blob[eo:eo + cs]) & 0xFFFFFFFF
    assert code_crc == crc, "CRC32 不一致（0x%08X != 0x%08X）" % (code_crc, crc)
    return {"name": name, "code_size": cs, "entry_offset": eo, "crc32": crc, "bytes": len(blob)}


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("in_bin", nargs="?", help="输入平铺二进制（objcopy -O binary 的产物）")
    ap.add_argument("out_vap", nargs="?", help="输出 .vap")
    ap.add_argument("name", nargs="?", help="应用名（可见 ASCII，不含 / 与 \\\\，<=23 字节）")
    ap.add_argument("--verify", metavar="FILE", help="只校验一个已有 .vap（独立解析）")
    args = ap.parse_args(argv[1:])

    if args.verify:
        blob = open(args.verify, "rb").read()
        info = parse_vap(blob)
        print("[VAP64] verify OK file=%s name=%s code=%d crc=0x%08X bytes=%d"
              % (args.verify, info["name"], info["code_size"], info["crc32"], info["bytes"]))
        return 0

    if not (args.in_bin and args.out_vap and args.name):
        sys.stderr.write(__doc__)
        return 2
    if not os.path.isfile(args.in_bin):
        sys.stderr.write("找不到输入：%s\n" % args.in_bin)
        return 2

    mv = load_make_vap()
    code = open(args.in_bin, "rb").read()
    blob, crc = mv.build_vap(code, args.name)          # 真源打包（含它自己的 selfcheck）
    mv.selfcheck(blob, code, args.name, crc)           # 打包器自检
    info = parse_vap(blob)                             # 本脚本的独立解析器再验一遍
    with open(args.out_vap, "wb") as f:
        f.write(blob)
    print("[VAP64] wrote %s name=%s code=%d crc=0x%08X bytes=%d"
          % (args.out_vap, info["name"], info["code_size"], info["crc32"], info["bytes"]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
