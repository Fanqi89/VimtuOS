#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/make_pack_win.py - ★ B5：把 **GNU make** 与 ring3 的 **/bin/sh** 装进系统卷。

三步走里的"第六步"（前五步见 tools/{tcc,lua,gzip,edit,assets}_pack_win.py）：
    ... assetsvol.img  ->  **本脚本**：/bin/make（装载驱动）+ /lib/make.bin（真 make）+
                            /bin/sh（= shell.bin 的字节；make 的 recipe 就是 `/bin/sh -c …`）+
                            /make-demo/**（一份 3 文件 C 演示工程，见 user/make/demo/）。
写完照例用 tcc_pack_win.verify 逐字节回读比对（直接块/一级/二级间接都走一遍）。
给了 --system/--disk 时再用 make_shellvol.build_disk 拼出完整演示盘（口径与其它 pack 脚本一致）。

用法：
    py -3 tools/make_pack_win.py --vol-in build64/assetsvol.img --vol-out build64/makevol.img \
          --make build64/make --bin build64/make.bin --shell build64/shell.bin \
          [--demo-dir user/make/demo] [--system build64/system.img --disk build64/sysdisk.img]
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
    path = os.path.join(HERE, "tcc_pack_win.py")
    spec = importlib.util.spec_from_file_location("tcc_pack_win", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = _load_tcc_pack()


def pack_into(vol, drv, binb, shell, demo_dir):
    """把 make 一件套 + /bin/sh + 演示工程装进 VolumeEdit；返回 {卷内路径: 字节}。"""
    expect = {}
    vol.put("/bin/make", drv, mode=0o755)
    expect["/bin/make"] = drv
    vol.put("/lib/make.bin", binb, mode=0o755)
    expect["/lib/make.bin"] = binb
    # /bin/sh：make 的 recipe 走 `/bin/sh -c '<recipe>'`，本内核没有别的 sh —— 它就是 ring3 shell
    # 的同一份字节（-c 模式不需要终端邮箱，见 user/shell/main.c 的 B5 段）。
    vol.put("/bin/sh", shell, mode=0o755)
    expect["/bin/sh"] = shell
    if demo_dir and os.path.isdir(demo_dir):
        # ★ 实测修：/make-demo 必须**可写**（0777）。内核里的进程 uid=1000（[PROC64] create … uid=1000），
        #   而默认的 0755/uid0 会让 tcc 建不出 main.o/hello：`tcc: error: could not write 'main.o'`
        #   （实测原文）。mkdirs 对已存在的目录是幂等的（tcc_pack_win.VolumeEdit.mkdirs）。
        vol.mkdirs("/make-demo", mode=0o777)
        for f in sorted(os.listdir(demo_dir)):
            p = os.path.join(demo_dir, f)
            if not os.path.isfile(p):
                continue
            b = open(p, "rb").read()
            vol.put("/make-demo/" + f, b, mode=0o644)
            expect["/make-demo/" + f] = b
    return expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷（assets_pack_win.py 的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷镜像")
    ap.add_argument("--make", required=True, help="build64/make（装载驱动）-> /bin/make")
    ap.add_argument("--bin", required=True, help="build64/make.bin（真 make）-> /lib/make.bin")
    ap.add_argument("--shell", required=True, help="build64/shell.bin -> /bin/sh")
    ap.add_argument("--demo-dir", default=os.path.join(ROOT, "user", "make", "demo"),
                    help="演示工程目录 -> /make-demo/（默认 user/make/demo）")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    for p, what in ((args.make, "/bin/make"), (args.bin, "/lib/make.bin"), (args.shell, "/bin/sh")):
        if not os.path.exists(p):
            sys.stderr.write("找不到 %s：%s\n" % (what, p))
            return 2
    drv = open(args.make, "rb").read()
    binb = open(args.bin, "rb").read()
    shell = open(args.shell, "rb").read()
    for nm, b in (("/bin/make", drv), ("/lib/make.bin", binb), ("/bin/sh", shell)):
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
    expect = pack_into(vol, drv, binb, shell, args.demo_dir)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    open(args.vol_out, "wb").write(out)
    print("    make 卷自检 OK：%d 个文件、%d B 逐字节回读一致（/bin/make=%d B、/lib/make.bin=%d B、"
          "/bin/sh=%d B、/make-demo=%d 个文件）-> %s"
          % (len(expect), sum(len(v) for v in expect.values()), len(drv), len(binb), len(shell),
             len([k for k in expect if k.startswith("/make-demo/")]), args.vol_out))

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
        print("    演示盘：%s（%d B = %d 扇区；主分区 LBA %d 起）"
              % (args.disk, len(disk), args.target_sectors, TP.SV.PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main())
