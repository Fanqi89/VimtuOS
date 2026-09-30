#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/lua_pack_win.py - ★ A4-4a：把 **Lua**（/bin/lua 驱动 + /lib/lua.bin 解释器 + 演示脚本）
装进**已有的** VimtuFS2 v4 系统卷。

与其它 pack 脚本的关系（不要混淆）：
  * tools/make_shellvol.py 造"A4-1 的带 /bin/shell.bin 的基础卷"；
  * tools/tcc_pack_win.py 造"再带 /bin/tcc + /lib/tcc.bin + /tcc/** 的卷"（本脚本的**输入**）；
  * 本脚本在那块卷**上继续写**（tools/tcc_pack_win.py 的 VolumeEdit：从位图/inode 表继续分配），
    加进去：
        /bin/lua            装载驱动（< 64 KiB，内核主程序装载器直接装）
        /lib/lua.bin        Lua 5.4.7 解释器（静态 musl；由驱动 mmap + 搬段 + jmp 装载）
        /tcc/demo/*.lua     演示脚本（hello.lua / io64.lua / err.lua）
  * tools/gzip_pack_win.py 在本脚本的输出上再加 /bin/gzip 与 1 MiB 试验文件 —— 三步串起来
    才是 build64.sh 里最终的那块系统卷。

用法：
    py -3 tools/lua_pack_win.py --vol-in build64/tccvol.img --vol-out build64/luavol.img \
          --drv build64/lua --bin build64/lua.bin --demo-dir user/lua/demo
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512


def _load_tcc_pack():
    """把 tools/tcc_pack_win.py 当模块加载（复用它的 VolumeEdit/Volume2 与回读校验器）。"""
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()

DEMO_SCRIPTS = ("hello.lua", "io64.lua", "err.lua")


def pack_into(vol, drv, lua_bin, demo_dir, probe=None):
    """把 Lua 三件套（+ 可选 A4-4b 探针）装进一个 VolumeEdit；返回 {卷内路径: 字节}。"""
    expect = {}
    vol.put("/bin/lua", drv, mode=0o755)
    expect["/bin/lua"] = drv
    vol.put("/lib/lua.bin", lua_bin, mode=0o755)
    expect["/lib/lua.bin"] = lua_bin
    for f in DEMO_SCRIPTS:
        p = os.path.join(demo_dir, f)
        if not os.path.exists(p):
            raise SystemExit("找不到演示脚本：%s（--demo-dir 指错了？）" % p)
        b = open(p, "rb").read()
        vol.put("/tcc/demo/" + f, b, mode=0o644)
        expect["/tcc/demo/" + f] = b
    if probe:
        b = open(probe, "rb").read()
        vol.put("/bin/a44probe", b, mode=0o755)
        expect["/bin/a44probe"] = b
    return expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（VimtuFS2 v4，由 tcc_pack_win.py 产出）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像（= 输入 + Lua 三件套）")
    ap.add_argument("--drv", required=True, help="build64/lua（装载驱动）-> /bin/lua")
    ap.add_argument("--bin", required=True, help="build64/lua.bin（解释器）-> /lib/lua.bin")
    ap.add_argument("--demo-dir", default=os.path.join(ROOT, "user", "lua", "demo"),
                    help="演示脚本目录 -> /tcc/demo/*.lua（默认 user/lua/demo）")
    ap.add_argument("--probe", default=None,
                    help="build64/a44probe（A4-4b 两个新系统调用的 ring3 探针）-> /bin/a44probe")
    args = ap.parse_args()

    for p, what in ((args.vol_in, "输入卷"), (args.drv, "装载驱动 build64/lua"),
                    (args.bin, "解释器 build64/lua.bin")):
        if not os.path.exists(p):
            sys.stderr.write("找不到 %s：%s\n" % (what, p))
            return 2
    drv = open(args.drv, "rb").read()
    lua_bin = open(args.bin, "rb").read()
    for nm, b in (("/bin/lua", drv), ("/lib/lua.bin", lua_bin)):
        if b[:4] != b"\x7fELF":
            sys.stderr.write("%s 不是 ELF：%d B\n" % (nm, len(b)))
            return 2

    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = int.from_bytes(img[20:24], "little")
    vol = TP.VolumeEdit(total)
    vol.load(img)
    expect = pack_into(vol, drv, lua_bin, args.demo_dir, args.probe)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    tot = sum(len(v) for v in expect.values())
    print("    Lua 卷自检 OK：%d 个文件、%d B 逐字节回读一致（/bin/lua=%d B、/lib/lua.bin=%d B、"
          "%d 个 .lua）-> %s"
          % (len(expect), tot, len(drv), len(lua_bin), len(DEMO_SCRIPTS), args.vol_out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
