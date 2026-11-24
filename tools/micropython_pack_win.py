#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/micropython_pack_win.py - ★ v0.11.0：把 **MicroPython** 装进**已有的** VimtuFS2 v4 卷

与 tools/lua_pack_win.py 同一个套路（那个脚本的注释里写了整条卷链的来历）：
  * 本脚本在**输入卷上继续写**（复用 tools/tcc_pack_win.py 的 VolumeEdit：从位图/inode 表
    继续分配块），加进去两个文件：
        /bin/mp                 装载驱动（< 64 KiB；内核主程序装载器直接装，与 /bin/lua 同构）
        /lib/micropython.bin    MicroPython 解释器（静态 musl；由驱动 mmap + 搬段 + jmp 装载）
  * 写完**逐字节回读自检**（TP.verify），不一致就返回 1，绝不写出一块坏卷。

为什么不像 lua 那样再带演示脚本：MicroPython 的交付形态就是"终端里敲 micropython 就进 REPL"
（内核命令 micropython / python / mp -> proc64_start_elf64("/bin/mp")），没有要额外装的
.py 演示文件；真要装脚本，属于后面单独的一步（与 /tcc/demo 同级的东西），不混在这一步里。

用法：
    py -3 tools/micropython_pack_win.py --vol-in build64/sysvol.img --vol-out build64/mpvol.img \\
          --drv build64/mp --bin build64/micropython.bin
    # 也可以原地更新（脚本先把整块卷读进内存、自检通过后才落盘，失败不会破坏输入卷）：
    py -3 tools/micropython_pack_win.py --vol-in build64/sysvol.img --vol-out build64/sysvol.img \\
          --drv build64/mp --bin build64/micropython.bin
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

SECTOR = 512
DRV_LIMIT = 64 * 1024      # 内核主程序装载器的用户窗口（低 64 KiB）


def _load_tcc_pack():
    """把 tools/tcc_pack_win.py 当模块加载（复用它的 VolumeEdit/Volume2 与回读校验器）。"""
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def pack_scripts(vol, script_dir):
    """★ v0.11.6：把 <script_dir>/*.py 装进 /mpy/<名字>；返回 {卷内路径: 字节}。

    为什么单独一层：MicroPython 的交付形态原来只有 REPL（"敲 mp 就进交互"），而
    "用 Python 写一个能在系统里直接跑的小程序"要求**脚本本身也在卷里** ——
    内核命令 `mp /mpy/xxx.py` 由 /bin/mp 交给端口的 pyexec_file() 执行。
    """
    expect = {}
    if not script_dir:
        return expect
    if not os.path.isdir(script_dir):
        raise SystemExit("找不到脚本目录：%s" % script_dir)
    names = sorted(n for n in os.listdir(script_dir) if n.endswith(".py"))
    if not names:
        raise SystemExit("脚本目录里没有 .py：%s" % script_dir)
    for n in names:
        data = open(os.path.join(script_dir, n), "rb").read()
        vol.put("/mpy/" + n, data, mode=0o644)
        expect["/mpy/" + n] = data
    return expect


def pack_into(vol, drv, mp_bin):
    """把 MicroPython 两件套装进一个 VolumeEdit；返回 {卷内路径: 字节}（回读校验的期望值）。"""
    if len(drv) > DRV_LIMIT:
        raise SystemExit("/bin/mp 驱动 = %d B，超过内核主程序装载窗口 64 KiB" % len(drv))
    expect = {}
    vol.put("/bin/mp", drv, mode=0o755)
    expect["/bin/mp"] = drv
    vol.put("/lib/micropython.bin", mp_bin, mode=0o755)
    expect["/lib/micropython.bin"] = mp_bin
    return expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷镜像（VimtuFS2 v4，例如 build64/sysvol.img）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像（= 输入 + /bin/mp + /lib/micropython.bin）")
    ap.add_argument("--drv", required=True, help="build64/mp（装载驱动）-> /bin/mp")
    ap.add_argument("--bin", required=True, help="build64/micropython.bin（解释器）-> /lib/micropython.bin")
    ap.add_argument("--script-dir", default=None,
                    help="★ v0.11.6：user/micropython/demo（*.py 装进 /mpy/，供 `mp /mpy/x.py` 运行）")
    args = ap.parse_args()

    for p, what in ((args.vol_in, "输入卷"), (args.drv, "装载驱动 build64/mp"),
                    (args.bin, "解释器 build64/micropython.bin")):
        if not os.path.exists(p):
            sys.stderr.write("找不到 %s：%s\n" % (what, p))
            return 2
    drv = open(args.drv, "rb").read()
    mp_bin = open(args.bin, "rb").read()
    for nm, b in (("/bin/mp", drv), ("/lib/micropython.bin", mp_bin)):
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
    expect = pack_into(vol, drv, mp_bin)
    expect.update(pack_scripts(vol, args.script_dir))
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    tot = sum(len(v) for v in expect.values())
    print("    MicroPython 卷自检 OK：%d 个文件、%d B 逐字节回读一致"
          "（/bin/mp=%d B、/lib/micropython.bin=%d B）-> %s"
          % (len(expect), tot, len(drv), len(mp_bin), args.vol_out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
