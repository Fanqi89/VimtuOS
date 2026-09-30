#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/edit_pack_win.py - ★ A4-5：把 **Ring 3 编辑器**装进系统卷，并可顺手重拼演示盘。

三步走里的**最后一步**（见 tools/lua_pack_win.py / tools/gzip_pack_win.py 的说明）：
  基础卷（shell + tcc） -> lua_pack 加 Lua -> gzip_pack 加 gzip + 1 MiB 文本
  -> **本脚本**加：
     /bin/edit              自研极简文本编辑器（build64/edit；静态 ELF64，< 64 KiB）
     /etc/edit64_test.txt   验收夹具文本（tests/edit64_test.py 与启动期演示都用它；
                            内容由本脚本的 make_test_text() 确定性生成，测试可以重新生成同一份）
     /tcc/demo/edit1m.txt   **正好 1 MiB** 的文本（1 MiB 大文件验收：打开 + 翻页 + 不保存；
                            内容复用 tools/gzip_pack_win.py 的 make_big_text()，与 gzip 验收同一份字节）
  给了 --system/--disk 时，再用 make_shellvol.build_disk 拼出完整演示盘（与上一步同口径）。

为什么编辑器**不进内核镜像**（交付纪律，与 shell/tcc/lua/gzip 一致）：
  内核区只剩 ~450 KB；编辑器是卷里的文件，内核只按路径去卷里找（kernel64.cpp 的 edit64_demo64）；
  build64.sh 里有一条"内核二进制里搜不到 /bin/edit 的字节"的断言钉住这件事。

本模块还被 tests/edit64_test.py 复用（read_volume_file / make_test_text / make_big_text），
所以这里把"按路径从卷镜像里逐字节读回"做成一个可直接调用的函数。

用法：
    py -3 tools/edit_pack_win.py --vol-in build64/sysvol.img --vol-out build64/editvol.img \\
          --edit build64/edit [--system build64/system.img --disk build64/sysdisk.img]
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512
BIG_BYTES = 1 << 20            # 正好 1 MiB

TEST_TEXT_PATH = "/etc/edit64_test.txt"
BIG_TEXT_PATH = "/tcc/demo/edit1m.txt"
EDIT_PATH = "/bin/edit"


def _load(mod_name, path):
    spec = importlib.util.spec_from_file_location(mod_name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load("tcc_pack_win", os.path.join(HERE, "tcc_pack_win.py"))
GP = _load("gzip_pack_win", os.path.join(HERE, "gzip_pack_win.py"))


def make_test_text():
    """验收夹具文本（**确定性**：测试用同一个函数重新生成，逐字节比对）。

    设计成"多行 + 每行可识别"，这样测试能断言：屏幕上出现了正文与状态行、改字符后内容变了、
    Ctrl+C 放弃改动回到盘上内容、Ctrl+S 后宿主侧读回的字节与期望**逐字节一致**。
    """
    lines = [
        "VimtuOS A4-5 edit64 test file",
        "line 2: second line for the editor test",
        "line 3: 0123456789 abcdefghijklmnopqrstuvwxyz",
        "line 4: the quick brown fox jumps over the lazy dog",
        "line 5: end of fixture\n",
    ]
    return "".join(lines).encode("ascii")


def make_big_text(n=BIG_BYTES):
    """1 MiB 文本：沿用 gzip 验收那一份（同一个生成器 -> 同一份字节）。"""
    return GP.make_big_text(n)


def pack_into(vol, edit_bytes, test_text=None, big_text=None):
    """把编辑器三件套装进一个 VolumeEdit；返回 {卷内路径: 字节}。"""
    expect = {}
    vol.put(EDIT_PATH, edit_bytes, mode=0o755)
    expect[EDIT_PATH] = edit_bytes
    t = test_text if test_text is not None else make_test_text()
    vol.put(TEST_TEXT_PATH, t, mode=0o644)
    expect[TEST_TEXT_PATH] = t
    b = big_text if big_text is not None else make_big_text()
    vol.put(BIG_TEXT_PATH, b, mode=0o644)
    expect[BIG_TEXT_PATH] = b
    return expect


# ---------------------------------------------------------------------------
# 按路径从**卷镜像字节**里逐字节读回（tests/edit64_test.py 用它做宿主侧比对）
# ---------------------------------------------------------------------------
def read_volume_file(img, path):
    """从 VimtuFS2 卷镜像里按绝对路径读一个文件；找不到抛 KeyError。"""
    if img[0:8] != b"VIMTUFS2":
        raise ValueError("不是 VimtuFS2 卷")
    total = struct.unpack_from("<I", img, 20)[0]
    inodes = struct.unpack_from("<I", img, 40)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in TP._entries(img, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            raise KeyError("卷里找不到 /" + "/".join(parts[:k + 1]))
        ino, rec = hit
        if k == len(parts) - 1:
            return TP._read_file(img, rec, total)
        if rec[0] != 2:
            raise KeyError("/%s 不是目录" % "/".join(parts[:k + 1]))
        cur = ino
    raise KeyError("空路径")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（VimtuFS2 v4，gzip_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--edit", required=True, help="build64/edit -> /bin/edit")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if not os.path.exists(args.edit):
        sys.stderr.write("找不到 build64/edit：%s（先跑 user/apps/edit/build_edit.sh）\n" % args.edit)
        return 2
    ed = open(args.edit, "rb").read()
    if ed[:4] != b"\x7fELF":
        sys.stderr.write("/bin/edit 不是 ELF：%d B\n" % len(ed))
        return 2
    if len(ed) > 64 * 1024:
        sys.stderr.write("/bin/edit 超过 64 KiB（主程序装载窗口）：%d B\n" % len(ed))
        return 2

    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = pack_into(vol, ed)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    # 再用本模块的读取器独立回读一次（测试用的就是它；两条路都过才算数）
    for p, want in expect.items():
        if read_volume_file(out, p) != want:
            sys.stderr.write("回读不一致：%s\n" % p)
            return 1
    open(args.vol_out, "wb").write(out)
    print("    edit 卷自检 OK：%d 个文件、%d B 逐字节回读一致（/bin/edit=%d B、%s=%d B、%s=%d B）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()), len(ed),
             TEST_TEXT_PATH, len(expect[TEST_TEXT_PATH]), BIG_TEXT_PATH, BIG_BYTES, args.vol_out))

    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system（system.img）\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        if len(system_bytes) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = TP.SV.build_disk(system_bytes, out, args.target_sectors)
        open(args.disk, "wb").write(disk)
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起，含 /bin/edit）"
              % (args.disk, len(disk), args.target_sectors, TP.SV.PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
