#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/packaging/vol_install.py - 把文件装进 **VimtuFS2 系统卷**（纯宿主侧，逐字节回读自检）

这是"安装"这一步在 VimtuOS 上的**真实做法**（本仓库构建期就是这么干的）：
  用 VimtuFS2 的卷编辑器把文件写进卷镜像 → 再把卷镜像拼成完整磁盘（MBR + 主分区 @ LBA 8009）。
  ★ **没有包管理器 / 没有应用商店**：这里只有"写进卷 + 回读校验"两件事（见下方"如实边界"）。

卷编辑器与校验器**复用仓库既有实现**（不重写、不复制）：
  `tools/tcc_pack_win.py` 的 `VolumeEdit`（支持直接块 + 一级/二级间接）与 `verify()`；
  `tools/make_shellvol.py` 的 `build_disk()`（拼盘：system.img 字节 + MBR + 主分区偏移）。

用法：
    # ① 只写卷（输入卷来自构建期卷链，例如 build64/soundvol.img）
    py -3 sdk/software-template/packaging/vol_install.py \\
          --vol-in build64/soundvol.img --vol-out build64/sdk/sdkvol.img \\
          --src build64/sdk/hello-cli.elf:/bin/hello-cli.elf:0755 \\
          --src build64/sdk/hello-cli.vap:/apps/hello-cli/hello-cli.vap:0755

    # ② 再拼一块可引导的演示盘（QEMU 直接跑）
    py -3 sdk/software-template/packaging/vol_install.py \\
          --vol-in build64/demovol.img --vol-out build64/sdk/sdkvol.img \\
          --src ... --system build64/system.img --disk build64/sdk/sdkdisk.img

    # ③ 只回读校验一个卷里的文件（独立按卷格式解析，不信写入侧）
    py -3 sdk/software-template/packaging/vol_install.py --check build64/sdk/sdkvol.img \\
          --expect /bin/hello-cli.elf=build64/sdk/hello-cli.elf

--src 语法： <宿主文件>:<卷内绝对路径>:<八进制模式>     （模式可省，默认 0755）
           卷内父目录不存在会自动建（mkdirs）。

如实边界（**别把没做的说成做了**）：
  * 没有依赖解析 / 冲突检查 / 卸载记录 / 脚本钩子（preinst、postinst）；
  * 不校验签名（VimtuFS2 没这个字段），只做 CRC/sha256 级完整性校验；
  * "安装位置"是**约定**：程序 `/bin/<name>.elf`（或 `/apps/<name>/`），图标 `/icons/{system,apps}/`，
    配置 `/etc/`，库 `/lib/`；权限位由你给（0755 可执行 / 0644 数据）—— 与仓库既有交付纪律一致。

退出码：0 = 成功；1 = 自检失败；2 = 参数/文件问题。
"""
import argparse
import hashlib
import importlib.util
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

SECTOR = 512
PART_MAIN_LBA = 8009


def load_tools():
    """载入仓库既有的卷编辑/拼盘实现（同一真源）。"""
    def mod_from(name, path):
        spec = importlib.util.spec_from_file_location(name, path)
        m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(m)
        return m

    tp = mod_from("tcc_pack_win", os.path.join(ROOT, "tools", "tcc_pack_win.py"))
    sv = mod_from("make_shellvol", os.path.join(ROOT, "tools", "make_shellvol.py"))
    return tp, sv


def sha(b):
    return hashlib.sha256(b).hexdigest()


def parse_src(spec):
    parts = spec.rsplit(":", 2)
    if len(parts) == 3 and parts[2].isdigit():
        host, vpath, mode = parts[0], parts[1], int(parts[2], 8)
    elif len(parts) == 2 and not parts[1].isdigit():
        host, vpath, mode = parts[0], parts[1], 0o755
    else:
        host, vpath, mode = parts[0], parts[1], 0o755
    if not vpath.startswith("/"):
        raise SystemExit("卷内路径必须是绝对路径：%s" % vpath)
    if not os.path.isfile(host):
        raise SystemExit("找不到宿主文件：%s" % host)
    return host, vpath, mode


def vol_read_all(vol_bytes, vpath):
    """独立按卷格式把文件读回来（直接块 -> ind -> dind 三级都走）。"""
    tp, _sv = load_tools()
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    parts = [p for p in vpath.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in tp._entries(vol_bytes, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return tp._read_file(vol_bytes, rec, total)
        cur = ino
    return None


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in")
    ap.add_argument("--vol-out")
    ap.add_argument("--src", action="append", default=[], help="<宿主文件>:<卷内路径>[:<八进制模式>]")
    ap.add_argument("--system", help="build64/system.img（要拼盘时给）")
    ap.add_argument("--disk", help="输出可引导演示盘")
    ap.add_argument("--target-sectors", type=int, default=32768)
    ap.add_argument("--check", metavar="VOLIMG", help="只回读校验一个卷镜像")
    ap.add_argument("--expect", action="append", default=[],
                    help="--check 时的期望：<卷内路径>=<宿主文件>")
    args = ap.parse_args(argv[1:])

    tp, sv = load_tools()

    if args.check:
        img = open(args.check, "rb").read()
        if img[0:8] != b"VIMTUFS2":
            sys.stderr.write("不是 VimtuFS2 卷：%s\n" % args.check)
            return 2
        bad = 0
        for spec in args.expect:
            vpath, host = spec.split("=", 1)
            want = open(host, "rb").read()
            got = vol_read_all(img, vpath)
            if got is None or got != want:
                print("  [FAIL] %-34s 卷内读回不一致（%s vs %d）"
                      % (vpath, len(got) if got is not None else "缺失", len(want)))
                bad += 1
            else:
                print("  [PASS] %-34s %8d B sha256=%s" % (vpath, len(want), sha(want)[:16]))
        print("[VOL64] check %s：%d 个期望，%d 个失败" % (args.check, len(args.expect), bad))
        return 1 if bad else 0

    if not (args.vol_in and args.vol_out):
        sys.stderr.write(__doc__)
        return 2
    img = open(args.vol_in, "rb").read()
    if img[0:8] != b"VIMTUFS2":
        sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
        return 2
    total = struct.unpack_from("<I", img, 20)[0]
    vol = tp.VolumeEdit(total)
    vol.load(img)

    expect = {}
    for spec in args.src:
        host, vpath, mode = parse_src(spec)
        data = open(host, "rb").read()
        vol.put(vpath, data, mode=mode)
        expect[vpath] = data
        print("   装入 %-38s <- %-46s %8d B mode=0%o" % (vpath, host, len(data), mode))
    out = vol.finish()
    bad = tp.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    for vpath, want in sorted(expect.items()):
        got = vol_read_all(out, vpath)
        if got != want:
            sys.stderr.write("回读不一致：%s\n" % vpath)
            return 1
        print("   回读 %-38s %8d B sha256=%s" % (vpath, len(want), sha(want)[:12]))
    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("[VOL64] 装卷自检 OK：%d 个文件逐字节回读一致 -> %s（卷 %d 扇区）"
          % (len(expect), args.vol_out, total))

    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system\n")
            return 2
        sysb = open(args.system, "rb").read()
        if len(sysb) > args.target_sectors * SECTOR:
            sys.stderr.write("system.img 比整块盘还大\n")
            return 2
        disk = sv.build_disk(sysb, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("[VOL64] 演示盘：%s（%d B = %d 扇区；主分区 LBA %d）"
              % (args.disk, len(disk), args.target_sectors, PART_MAIN_LBA))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
