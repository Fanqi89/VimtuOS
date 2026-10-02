#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/busybox_pack_win.py - ★ 本批：把 **busybox（静态 musl）+ 它的装载驱动 + applet
包装程序 + 用户态目录索引 + 验收夹具** 装进系统卷（VimtuFS2 v4），并逐字节回读自检。

交付物（卷内路径）：
  /lib/busybox.bin     busybox 本体（build64/busybox.bin，~500 KB，静态 ELF64，钉在 4GiB+0x90000）
  /bin/busybox         装载驱动（build64/busybox）；按 argv[0] 的 basename 选 applet
  /bin/<applet>        applet 包装程序（build64/bbwrap 的字节，每个名字一份；卷里没有 symlink）
  /etc/vimtu.dirs      用户态目录索引（见 user/busybox/vimtu_dirent.c 的说明）
  /tmp/bb/*            验收夹具（组合管道脚本、ed 编辑脚本、素材文件）

关于"多入口"的取舍（任务里点名的两种做法）：
  * 我们**没有**走"把驱动本体拷 60 份"（每份 13 KB）那条路，而是走**小包装程序**：
    每份 4.7 KB 的 /bin/<applet> 只做一件事 —— execve("/bin/busybox", argv, envp)，
    argv[0] 原样保留（= 用户敲的名字），busybox 自己按 basename 选 applet。
  * busybox 自带的 "busybox APPLET ARGS" 形式也永远可用（`run /bin/busybox ls -l /`）。
  * **不与已有用户态程序抢名字**：卷里已经有 /bin/gzip、/bin/gunzip、/bin/tar、/bin/edit、
    /bin/make、/bin/tcc、/bin/lua 等（前面几批交付的真程序），本脚本**跳过**这些名字，
    不覆盖它们 —— busybox 的那一份仍可用 `busybox gzip` / `busybox tar` 访问。

用法：
    py -3 tools/busybox_pack_win.py --vol-in build64/demovol.img --vol-out build64/busyboxvol.img \
          --busybox-bin build64/busybox.bin --busybox-drv build64/busybox --bbwrap build64/bbwrap \
          --system build64/system.img --disk build64/sysdisk.img
退出码：0 = 成功（含逐字节回读自检）；2 = 参数/输入问题；1 = 自检失败。
"""
import argparse
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

SECTOR = 512
PART_MAIN_LBA = 8009


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(HERE, fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
CFG = load_mod("bb_config", os.path.join(ROOT, "user", "busybox", "bb_config.py"))

# ---------------------------------------------------------------------------
# 验收夹具（脚本放卷里，测试只需敲很短的命令，避免终端 sendkey 的引号限制）
# ---------------------------------------------------------------------------
BB_LINES = b"alpha\nbeta\ngamma\nhello world\ndelta\n"
BB_EDIT_IN = b"one\ntwo\nthree\n"
BB_EDIT_OUT = b"one\nTWO-EDITED\nthree\n"     # 宿主侧逐字节比对的期望值

PIPE_SH = b"""# /tmp/bb/pipe.sh - the real combination chains (checked by tests/busybox64_test.py)
echo '[PIPE] ls -l /tcc/demo | grep hello | wc -l'
ls -l /tcc/demo | grep hello | wc -l
echo '[PIPE] cat | sed > b ; cat b'
cat /tmp/bb/lines.txt | sed s/beta/BETA/ > /tmp/bb/sedout.txt
cat /tmp/bb/sedout.txt
echo '[PIPE] sort | uniq -c'
sort /tmp/bb/lines.txt | uniq -c
echo '[PIPE] find / -name *.c | head'
find / -name *.c | head -3
"""

EDIT_SH = b"""# /tmp/bb/edit.sh - edit one line with ed and wq (host-side byte check)
ed -s /tmp/bb/edit_me.txt < /tmp/bb/ed.cmds
echo '[EDIT] ed done'
cat /tmp/bb/edit_me.txt
"""

ED_CMDS = b"2s/two/TWO-EDITED/\nw\nq\n"

AWK_SH = b"""# /tmp/bb/awk.sh - awk expressions (the terminal cannot type quotes)
awk '{print $1}' /tmp/bb/lines.txt
awk 'BEGIN{print 2+3*4}'
echo AWKOK
"""

UNIQ_SH = b"""# /tmp/bb/uniq.sh - sort + uniq over real pipes
sort /tmp/bb/lines.txt > /tmp/bb/sorted.txt
uniq /tmp/bb/sorted.txt
uniq -c /tmp/bb/sorted.txt | head -8
echo UNIQOK
"""


APPLETS_SH = b"""# /tmp/bb/applets.sh - every applet under test, with @@MARKERs.
echo '@@LS'
ls /tcc
echo '@@LS_L'
ls -l /etc
echo '@@CAT'
cat /tmp/bb/lines.txt
echo '@@HEAD'
head -n 2 /tmp/bb/lines.txt
echo '@@TAIL'
tail -n 1 /tmp/bb/lines.txt
echo '@@WC'
wc -l /tmp/bb/lines.txt
echo '@@GREP'
grep hello /tmp/bb/lines.txt
echo '@@SED'
sed s/delta/DELTA/ /tmp/bb/lines.txt
echo '@@AWK'
awk '{print $1}' /tmp/bb/lines.txt
awk 'BEGIN{print 2+3*4}'
echo '@@SORT'
sort /tmp/bb/lines.txt
echo '@@UNIQ'
sort /tmp/bb/lines.txt | uniq -c
echo '@@OD'
od -c /tmp/bb/lines.txt
echo '@@HEXDUMP'
hexdump -C /tmp/bb/lines.txt
echo '@@FIND'
find /tcc -name *.c
echo '@@FILES'
cp /tmp/bb/lines.txt /tmp/bb/copy.txt
chmod 600 /tmp/bb/copy.txt
ls -l /tmp/bb/copy.txt
cat /tmp/bb/copy.txt
mv /tmp/bb/copy.txt /tmp/bb/moved.txt
cat /tmp/bb/moved.txt
cat /tmp/bb/copy.txt
rm /tmp/bb/moved.txt
cat /tmp/bb/moved.txt
mkdir /tmp/bb/sub
cp /tmp/bb/lines.txt /tmp/bb/sub/inner.txt
cat /tmp/bb/sub/inner.txt
echo '@@UNAME'
uname -a
echo '@@ID'
id
whoami
echo '@@KILL'
kill -0 1
echo '@@PS'
ps
echo '@@DF'
df
echo '@@FREE'
free
echo '@@UPTIME'
uptime
echo '@@TAR'
tar -cf /tmp/bb/a.tar /tmp/bb/lines.txt
tar -tf /tmp/bb/a.tar
echo '@@GZIP'
gzip -k /tmp/bb/lines.txt
ls -l /tmp/bb/lines.txt.gz
echo '@@DONE'
"""

NET_SH = b"""# /tmp/bb/net.sh - wget / ping must FAIL clearly (no userland TCP/IP yet)
echo '@@WGET'
wget http://10.0.2.2/index.html
echo '@@WGET_RC' $?
echo '@@PING'
ping -c 1 127.0.0.1
echo '@@PING_RC' $?
"""


def fixture_files():
    return {
        "/tmp/bb/lines.txt": BB_LINES,
        "/tmp/bb/edit_me.txt": BB_EDIT_IN,
        "/tmp/bb/ed.cmds": ED_CMDS,
        "/tmp/bb/pipe.sh": PIPE_SH,
        "/tmp/bb/edit.sh": EDIT_SH,
        "/tmp/bb/awk.sh": AWK_SH,
        "/tmp/bb/uniq.sh": UNIQ_SH,
        "/tmp/bb/applets.sh": APPLETS_SH,
        "/tmp/bb/net.sh": NET_SH,
    }


# ---------------------------------------------------------------------------
# 目录索引（\u7528\u6237\u6001\u76ee\u5f55\u5217\u4e3e\u7684\u6570\u636e\u6e90\uff1b\u683c\u5f0f = "<\u76ee\u5f55\u7edd\u5bf9\u8def\u5f84>\\t<\u540d\u5b57>"）
# ---------------------------------------------------------------------------
def walk(vol_bytes, parent=0, prefix="/", out=None, used=None):
    if out is None:
        out = []
    total = int.from_bytes(vol_bytes[20:24], "little")
    inodes = int.from_bytes(vol_bytes[40:44], "little")
    for i, nm, rec in TP._entries(vol_bytes, parent, inodes, used):
        typ = rec[0]
        out.append("%s\t%s" % (prefix, nm))
        if typ == 2:
            sub = (prefix.rstrip("/") + "/" + nm) if prefix != "/" else "/" + nm
            walk(vol_bytes, i, sub, out, used)
    _ = total
    return out


def make_index(vol_bytes, extra=None):
    """卷的目录树 -> /etc/vimtu.dirs 的字节。extra = [(目录, 名字)] 是"还没写进去、但马上要写"
    的条目（索引文件自己），这样索引对 /etc 的列举是自洽的。"""
    used = TP.used_blocks(vol_bytes)
    lines = walk(vol_bytes, 0, "/", None, used)
    for d, nm in (extra or []):
        lines.append("%s\t%s" % (d, nm))
    lines = sorted(set(lines))
    body = "# VimtuOS64 dirindex v1: <dir>\t<name>  (build-time snapshot; see user/busybox/vimtu_dirent.c)\n"
    return (body + "".join(l + "\n" for l in lines)).encode("utf-8")


def pack_into(vol, busybox_bin, drv, wrap, skipped):
    """把 busybox 那一套装进 VolumeEdit；返回 {卷内路径: 字节}（给 verify 用）。
    skipped = 卷里已经存在、因此**不覆盖**的包装程序名字（返回给调用方打日志）。"""
    expect = {}
    vol.put("/lib/busybox.bin", busybox_bin, mode=0o755)
    expect["/lib/busybox.bin"] = busybox_bin
    vol.put("/bin/busybox", drv, mode=0o755)
    expect["/bin/busybox"] = drv

    # 卷里已有哪些名字？（不覆盖前面几批交付的真程序）
    snap = vol.finish()
    inodes = int.from_bytes(snap[40:44], "little")
    have = set(nm for _, nm, _ in TP._entries(snap, vol.find_dir("/bin"), inodes,
                                              TP.used_blocks(snap)))
    for name in CFG.WRAPPERS:
        if name in have:
            skipped.append(name)
            continue
        vol.put("/bin/" + name, wrap, mode=0o755)
        expect["/bin/" + name] = wrap

    # /tmp/bb 必须**世界可写**：终端会话是 uid=1000，而卷里的文件默认属主 root；
    # /tmp 本身是 0777（make_shellvol 的约定）。夹具里的可写文件也给 0666
    # （ed 要往回写 edit_me.txt、gzip -k / cp / mv / rm 要建/删文件）。
    vol.mkdirs("/tmp/bb", mode=0o777)
    for path, data in fixture_files().items():
        vol.put(path, data, mode=0o666)
        expect[path] = data

    idx = make_index(vol.finish(), extra=[("/etc", "vimtu.dirs")])
    vol.put("/etc/vimtu.dirs", idx, mode=0o644)
    expect["/etc/vimtu.dirs"] = idx
    return expect


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vol-in", required=True, help="输入卷（VimtuFS2 v2，卷链上一步的产物）")
    ap.add_argument("--vol-out", required=True, help="输出卷")
    ap.add_argument("--busybox-bin", required=True, help="build64/busybox.bin -> /lib/busybox.bin")
    ap.add_argument("--busybox-drv", required=True, help="build64/busybox -> /bin/busybox")
    ap.add_argument("--bbwrap", required=True, help="build64/bbwrap -> /bin/<applet>")
    ap.add_argument("--system", default=None, help="build64/system.img（做演示盘时给）")
    ap.add_argument("--disk", default=None, help="输出完整演示盘（system.img + MBR + 主分区卷）")
    ap.add_argument("--target-sectors", type=int, default=TP.SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    for p, what in ((args.busybox_bin, "busybox.bin"), (args.busybox_drv, "/bin/busybox"),
                    (args.bbwrap, "bbwrap")):
        if not os.path.exists(p):
            sys.stderr.write("找不到 %s：%s（先跑 tools/busybox_build_win.sh）\n" % (what, p))
            return 2
    bb = open(args.busybox_bin, "rb").read()
    drv = open(args.busybox_drv, "rb").read()
    wrap = open(args.bbwrap, "rb").read()
    for nm, b in (("busybox.bin", bb), ("busybox", drv), ("bbwrap", wrap)):
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
    skipped = []
    expect = pack_into(vol, bb, drv, wrap, skipped)
    out = vol.finish()

    bad = TP.verify(out, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return 1
    nwrap = len(expect) - 3 - len(fixture_files())
    print("    busybox 卷自检 OK：%d 个文件、%d B 逐字节回读一致"
          % (len(expect), sum(len(v) for v in expect.values())))
    print("    /lib/busybox.bin=%d B  /bin/busybox=%d B  /bin/<applet>=%d 份×%d B  "
          "/etc/vimtu.dirs=%d B（%d 条目录项）"
          % (len(bb), len(drv), nwrap, len(wrap), len(expect["/etc/vimtu.dirs"]),
             expect["/etc/vimtu.dirs"].count(b"\n") - 1))
    if skipped:
        print("    跳过（卷里已有真程序，不覆盖）：%s" % " ".join(sorted(skipped)))

    open(args.vol_out, "wb").write(out)
    print("    卷镜像：%s（%d B）" % (args.vol_out, len(out)))

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
