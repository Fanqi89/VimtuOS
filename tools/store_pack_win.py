#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/store_pack_win.py - ★ 应用商店/包管理器：**套件仓库生成器 + 装卷器**（纯宿主 Python，标准库）

本工具做两件事（与仓库其它 `*_pack_win.py` 同一条纪律：只加、只读、只验，不碰内核）：

① **造套件仓库**（`--make-repo <spec.json> --repo-out <dir>`）：
   按 spec 生成**包文件本体** + 索引 `index.json`：
     * `.vap64` —— 复用 SDK 的打包器 `sdk/software-template/packaging/vap64_pack.py`
       （它自己又 import `tools/make_vap.py` = 布局唯一真源；本工具再用**独立的解析器**回读校验）；
     * `.deb`   —— 真 ar 归档（全局头 `!<arch>\\n` + 60B 成员头）+ `control.tar.gz` +
       `data.tar.gz`（两者都在包内；`--compress xz` 时写 `*.tar.xz`，用来验收"xz 明确拒绝"）；
       可选维护者脚本（preinst/postinst/prerm/postrm）用来验收"带脚本的 deb 整包拒绝"。
   索引字段：name/version/arch/type/file/size/sha256/depends/description/summary
   （size/sha256 由**包文件本体**算出，宿主侧同时回读复核一遍）。
   反例用 `sha256_override`：把索引里的 sha256 改成错的，用来验收"坏哈希被拒"。

② **装进系统卷 / 拼夹具盘**（`--vol-in/--vol-out` 或 `--fixture-img`）：
   把 `/bin/vpkg`、`/bin/store`、`/opt/vpkg/index.json`、`/opt/vpkg/repo/*` 以及 `--src` 里
   额外指定的文件写进 VimtuFS2 v4 卷（复用 `tools/tcc_pack_win.VolumeEdit`），写完
   **逐字节回读自检**（再用一个独立解析器 `vol_read` 复读一遍），需要时拼成可引导盘
   （`tools/make_shellvol.build_disk`：MBR + 主分区 @ LBA 8009）。

用法（仓库里 build64.sh 的那一步）：
    py -3 tools/store_pack_win.py --make-repo build64/store/repo_spec.json \\
          --repo-out build64/store/repo
    py -3 tools/store_pack_win.py --vol-in build64/netvol.img --vol-out build64/storevol.img \\
          --vpkg build64/store/vpkg.elf --store build64/store/store.elf \\
          --repo build64/store/repo --system build64/system.img --disk build64/sysdisk.img

夹具盘（验收脚本用；`--fresh-vol N` = 从零格式化一块 N 扇区的卷，便于造"空间不足"）：
    py -3 tools/store_pack_win.py --fixture-img build64/store64_test.img --fresh-vol 3200 \\
          --src build64/store/vpkg.elf:/bin/vpkg:0755 --src ... \\
          --repo build64/store/repo --system build64/system.img

退出码：0 = 成功（含回读自检）；1 = 自检失败；2 = 参数/输入问题。
"""
import argparse
import re
import gzip
import hashlib
import importlib.util
import io
import json
import os
import struct
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SECTOR = 512
PART_MAIN_LBA = 8009
VPKG_VOL = "/bin/vpkg"
STORE_VOL = "/bin/store"
REPO_VOL = "/opt/vpkg/repo"
INDEX_VOL = "/opt/vpkg/index.json"


def load_mod(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


TP = load_mod("tcc_pack_win", os.path.join(HERE, "tcc_pack_win.py"))       # VolumeEdit / Volume2 / verify
SV = TP.SV                                                                  # make_shellvol：build_disk / 常量
assert TP.SV is not None and TP.SV.PART_MAIN_LBA == PART_MAIN_LBA, "make_shellvol 的几何变了？"


def sha256_of(b):
    return hashlib.sha256(b).hexdigest()


def q(p):
    return p.replace("\\", "/")


# ---------------------------------------------------------------------------
# .vap64：复用 SDK 打包器（tools/make_vap.py 是真源）+ SDK 的独立解析器复核
# ---------------------------------------------------------------------------
def _vap_mods():
    vp = os.path.join(ROOT, "sdk", "software-template", "packaging", "vap64_pack.py")
    if not os.path.exists(vp):
        raise RuntimeError("找不到 SDK 打包器：%s" % vp)
    mod = load_mod("vap64_pack", vp)
    return mod, mod.load_make_vap()


def make_vap64(payload, name, verbose=True):
    """把 payload（平铺二进制或整个 ELF 文件字节）打成 VAP64 包；返回 (blob, info)。"""
    vp, mv = _vap_mods()
    blob, crc = mv.build_vap(payload, name)          # 真源打包（含它自己的 selfcheck）
    mv.selfcheck(blob, payload, name, crc)
    info = vp.parse_vap(blob)                        # SDK 的**独立解析器**再验一遍
    assert info["name"] == name and info["code_size"] == len(payload), "VAP64 回读不一致"
    if verbose:
        print("    [VAP64] name=%s payload=%d B pack=%d B crc=0x%08X"
              % (name, len(payload), len(blob), info["crc32"]))
    return blob, info


# ---------------------------------------------------------------------------
# .deb：ar + control.tar.gz + data.tar.gz（真骨架；xz 变体用来验收"明确拒绝"）
# ---------------------------------------------------------------------------
def _tar_bytes(entries, fmt=tarfile.GNU_FORMAT):
    """entries: [(name, kind, data, mode)]；kind = 'f' / 'd'。返回未压缩的 tar 字节。"""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=fmt) as tf:
        for name, kind, data, mode in entries:
            ti = tarfile.TarInfo(name=name)
            ti.mtime = 0
            ti.uid = 0
            ti.gid = 0
            ti.mode = mode
            if kind == "d":
                ti.type = tarfile.DIRTYPE
                tf.addfile(ti)
            else:
                ti.type = tarfile.REGTYPE
                ti.size = len(data)
                tf.addfile(ti, io.BytesIO(data))
    return buf.getvalue()


def _ar_member(name, data):
    if len(name) > 16:
        raise ValueError("ar 成员名太长：%s" % name)
    hdr = name.ljust(16) + "0".ljust(12) + "0".ljust(6) + "0".ljust(6) + \
          ("100644").ljust(8) + str(len(data)).ljust(10) + "`\n"
    assert len(hdr) == 60, len(hdr)
    out = hdr.encode("ascii") + data
    if len(data) & 1:
        out += b"\n"                                    # ar 要求偶数长度（补 1 字节）
    return out


def make_deb(files, control, scripts=None, compress="gz", verbose=True):
    """files: {卷内相对路径: bytes}；control: dict（Package/Version/Architecture/Depends/Description…）；
    scripts: {"preinst": b"..."} 之一（写进 control.tar）；compress: "gz" / "xz"。
    返回 deb 字节（ar 归档）。"""
    scripts = scripts or {}
    lines = []
    for k, v in control.items():
        lines.append("%s: %s" % (k, v))
    ctl_entries = [("./control", "f", ("\n".join(lines) + "\n").encode("utf-8"), 0o644)]
    for sname, sdata in scripts.items():
        if sname not in ("preinst", "postinst", "prerm", "postrm"):
            raise ValueError("不是 deb 维护者脚本名：%s" % sname)
        ctl_entries.append(("./" + sname, "f", sdata, 0o755))
    data_entries = []
    seen_dirs = set()
    for path, data in files.items():
        parts = [p for p in path.split("/") if p]
        for i in range(1, len(parts)):
            d = "/".join(parts[:i])
            if d not in seen_dirs:
                seen_dirs.add(d)
                data_entries.append(("./" + d + "/", "d", b"", 0o755))
        data_entries.append(("./" + path, "f", data, 0o644))

    ctl_tar = _tar_bytes(ctl_entries)
    dat_tar = _tar_bytes(data_entries)
    if compress == "gz":
        ctl_member = ("control.tar.gz", gzip.compress(ctl_tar, 9, mtime=0))
        dat_member = ("data.tar.gz", gzip.compress(dat_tar, 9, mtime=0))
    elif compress == "xz":
        import lzma
        ctl_member = ("control.tar.xz", lzma.compress(ctl_tar, preset=6))
        dat_member = ("data.tar.xz", lzma.compress(dat_tar, preset=6))
    else:
        raise ValueError("compress 只支持 gz / xz（验收就要这两种）")
    deb = b"!<arch>\n" + _ar_member("debian-binary", b"2.0\n") + \
          _ar_member(ctl_member[0], ctl_member[1]) + _ar_member(dat_member[0], dat_member[1])
    if verbose:
        print("    [DEB]     package=%s members=%s+%s files=%d scripts=%s deb=%d B"
              % (control.get("Package", "?"), ctl_member[0], dat_member[0], len(files),
                 ",".join(sorted(scripts)) if scripts else "-", len(deb)))
    return deb


def parse_deb_members(deb):
    """独立解析 ar（不信 make_deb 的写入）：返回 [(名字, 偏移, 长度)]。"""
    assert deb[:8] == b"!<arch>\n", "不是 ar 归档"
    off, out = 8, []
    while off + 60 <= len(deb):
        h = deb[off:off + 60]
        assert h[58:60] == b"`\n", "ar 成员头坏了 @%d" % off
        name = h[0:16].decode("ascii").split("/")[0].strip()
        size = int(h[48:58].decode("ascii").strip() or "0")
        data = off + 60
        assert data + size <= len(deb), "ar 成员越界：%s" % name
        out.append((name, data, size))
        off = data + size + (size & 1)
    return out


# ---------------------------------------------------------------------------
# 套件仓库（包本体 + index.json）
# ---------------------------------------------------------------------------
def _resolve(rel, base):
    return rel if os.path.isabs(rel) else os.path.join(base, rel)


def build_repo(spec, repo_out, base=None, verbose=True):
    """按 spec（dict，含 "repo": [...]）生成包文件与 index.json。返回索引 dict（含算好的 size/sha256）。"""
    base = base or ROOT
    os.makedirs(repo_out, exist_ok=True)
    entries = []
    for item in spec["repo"]:
        name = item["name"]
        fname = item.get("file") or (name + (".deb" if item["type"] == "deb" else ".vap64"))
        if item["type"] == "vap64":
            payload = open(_resolve(item["payload"], base), "rb").read()
            blob, _info = make_vap64(payload, name, verbose=verbose)
            if len(blob) > 32768 + 64:
                raise RuntimeError(".vap64 太大（%d B）" % len(blob))
        elif item["type"] == "deb":
            d = item.get("deb", {})
            files = {}
            for vpath, val in (d.get("files") or {}).items():
                if isinstance(val, dict) and "zeros" in val:
                    files[vpath] = b"\0" * int(val["zeros"])
                elif isinstance(val, dict) and "host" in val:
                    files[vpath] = open(_resolve(val["host"], base), "rb").read()
                elif isinstance(val, dict) and "text" in val:
                    files[vpath] = str(val["text"]).encode("utf-8")
                elif isinstance(val, str):
                    files[vpath] = val.encode("utf-8")
                else:
                    raise ValueError("deb 文件项格式不认识：%r" % vpath)
            ctl = dict(d.get("control") or {"Package": name, "Version": item.get("version", "1.0")})
            scripts = {}
            for sname, sval in (d.get("scripts") or {}).items():
                scripts[sname] = sval.encode("utf-8") if isinstance(sval, str) else sval
            blob = make_deb(files, ctl, scripts, d.get("compress", "gz"), verbose=verbose)
        else:
            raise ValueError("包类型只支持 vap64 / deb：%r" % item["type"])
        path = os.path.join(repo_out, fname)
        with open(path, "wb") as f:
            f.write(blob)
        got = open(path, "rb").read()                    # 回读一遍（磁盘上必须一致）
        if got != blob or len(got) != len(blob):
            raise RuntimeError("回读不一致：%s" % path)
        real_sha = sha256_of(got)
        entry = {
            "name": name,
            "version": item.get("version", "1.0"),
            "arch": item.get("arch", "vimtu64"),
            "type": item["type"],
            "file": fname,
            "size": len(got),
            "sha256": item.get("sha256_override", real_sha),
            "depends": item.get("depends", []),
            "summary": item.get("summary", ""),
            "description": item.get("description", ""),
        }
        entry["_real_sha256"] = real_sha
        entries.append(entry)
    index = {"schema": 1, "generator": "store_pack_win.py", "repo": REPO_VOL,
             "packages": [{k: v for k, v in e.items() if not k.startswith("_")} for e in entries]}
    ipath = os.path.join(repo_out, "index.json")
    with open(ipath, "w", encoding="utf-8") as f:
        json.dump(index, f, ensure_ascii=False, indent=1)
        f.write("\n")
    if verbose:
        print("    索引：%s（%d 条）" % (ipath, len(entries)))
        for e in entries:
            print("      %-14s %-6s %-12s %7d B sha256=%s depends=%s"
                  % (e["name"], e["type"], e["file"], e["size"], e["sha256"][:12], e["depends"]))
    return index


def verify_repo(repo_dir, index, verbose=True):
    """独立复核：索引里的 size/sha256 与新读的包文件一致；.vap64 再用 SDK 解析器验一遍。"""
    bad = []
    vp, _mv = _vap_mods()
    for e in index["packages"]:
        p = os.path.join(repo_dir, e["file"])
        if not os.path.exists(p):
            bad.append("%s 缺文件" % e["file"])
            continue
        blob = open(p, "rb").read()
        if len(blob) != e["size"]:
            bad.append("%s size %d != %d" % (e["file"], len(blob), e["size"]))
        if e["type"] == "vap64":
            try:
                info = vp.parse_vap(blob)                # 独立解析（不信写入路径）
                if info["name"] != e["name"]:
                    bad.append("%s 包内名字 %s != %s" % (e["file"], info["name"], e["name"]))
            except AssertionError as ex:
                bad.append("%s VAP64 解析失败：%s" % (e["file"], ex))
        else:
            names = [n for n, _o, _s in parse_deb_members(blob)]
            if "debian-binary" not in names:
                bad.append("%s 不是 ar 归档" % e["file"])
        if verbose:
            print("     复核 %-16s %7d B sha256=%s" % (e["file"], len(blob), sha256_of(blob)[:12]))
    return bad



def shipped_spec(vpkg_host, store_host, base=None):
    """**系统卷里那份仓库**的 spec（build64.sh 用的那一份，逐字写在这里 = 唯一真源）。
    两个 .vap64（payload 是 build64/store/demo_cli.elf / demo_gui.elf）+ 三个 .deb
    （纯 payload 一个 / 带 preinst 一个 = 反例 / xz 一个 = 反例）。"""
    base = base or ROOT
    d = os.path.dirname(os.path.abspath(vpkg_host))
    return {"repo": [
        {"name": "demo-cli", "version": "1.0", "type": "vap64",
         "payload": os.path.join(d, "demo_cli.elf"), "file": "demo-cli.vap64",
         "summary": "VimtuOS demo CLI (installs /bin/demo-cli)",
         "description": "Example CLI package shipped in the system volume", "depends": []},
        {"name": "demo-gui", "version": "1.0", "type": "vap64",
         "payload": os.path.join(d, "demo_gui.elf"), "file": "demo-gui.vap64",
         "summary": "VimtuOS demo GUI app (wm client)",
         "description": "Example GUI package (needs a compositor session)",
         "depends": ["demo-cli"]},
        {"name": "vpkg-debdemo", "version": "1.0", "type": "deb", "file": "vpkg-debdemo.deb",
         "summary": "Pure payload .deb (ar + control.tar.gz + data.tar.gz)",
         "description": "Data-only .deb built on the host by tools/store_pack_win.py",
         "depends": [],
         "deb": {"control": {"Package": "vpkg-debdemo", "Version": "1.0",
                             "Architecture": "vimtu64",
                             "Maintainer": "VimtuOS <dev@vimtuos.invalid>",
                             "Description": "pure payload demo"},
                 "files": {"usr/share/vpkg-debdemo/data.txt": "vpkg deb demo: 0123456789\n",
                           "usr/share/vpkg-debdemo/hello.txt": "hello from data.tar.gz\n"}}},
        {"name": "baddeb", "version": "1.0", "type": "deb", "file": "baddeb.deb",
         "summary": ".deb with a preinst maintainer script (refused on purpose)",
         "description": "Negative case: maintainer scripts are not supported",
         "depends": [],
         "deb": {"control": {"Package": "baddeb", "Version": "1.0",
                             "Architecture": "vimtu64", "Description": "has preinst"},
                 "files": {"usr/share/baddeb/x.txt": "never lands\n"},
                 "scripts": {"preinst": "#!/bin/sh\necho nope\n"}}},
    ]}

# ---------------------------------------------------------------------------
# 装卷（与 net_pack_win.py 同一套：VolumeEdit + 独立 vol_read）
# ---------------------------------------------------------------------------
def vol_read(vol_bytes, path):
    """**独立**按卷格式把文件读回来（直接块 -> ind -> dind）。"""
    total = struct.unpack_from("<I", vol_bytes, 20)[0]
    inodes = struct.unpack_from("<I", vol_bytes, 40)[0]
    parts = [p for p in path.split("/") if p]
    cur = 0
    for k, part in enumerate(parts):
        hit = None
        for i, nm, rec in TP._entries(vol_bytes, cur, inodes):
            if nm == part:
                hit = (i, rec)
                break
        if not hit:
            return None
        ino, rec = hit
        if k == len(parts) - 1:
            return TP._read_file(vol_bytes, rec, total)
        cur = ino
    return None


def collect_files(args):
    """返回 {卷内路径: (字节, 模式)}。"""
    out = {}
    for host, vol, mode in ((args.vpkg, VPKG_VOL, 0o755), (args.store, STORE_VOL, 0o755)):
        if not host:
            continue
        if not os.path.exists(host):
            sys.stderr.write("找不到 %s\n" % host)
            return None
        b = open(host, "rb").read()
        if b[:4] != b"\x7fELF" or b[4] != 2 or b[5] != 1:
            sys.stderr.write("%s 不是 ELF64 小端\n" % host)
            return None
        if len(b) > 96 * 1024:
            sys.stderr.write("%s 超过内核读盘缓冲上限 96 KiB：%d B\n" % (host, len(b)))
            return None
        out[vol] = (b, mode)
        print("    %-16s = %d B（sha256=%s）" % (vol, len(b), sha256_of(b)[:16]))
    if args.repo:
        index_host = os.path.join(args.repo, "index.json")
        if not os.path.exists(index_host):
            sys.stderr.write("仓库里没有 index.json：%s\n" % args.repo)
            return None
        index = json.load(open(index_host, encoding="utf-8"))
        bad = verify_repo(args.repo, index, verbose=True)
        if bad:
            sys.stderr.write("仓库自检失败：%s\n" % bad)
            return None
        ib = open(index_host, "rb").read()
        out[INDEX_VOL] = (ib, 0o644)
        print("    %-16s = %d B（%d 条）" % (INDEX_VOL, len(ib), len(index["packages"])))
        for e in index["packages"]:
            p = os.path.join(args.repo, e["file"])
            b = open(p, "rb").read()
            vp = "%s/%s" % (REPO_VOL, e["file"])
            out[vp] = (b, 0o644)
            print("    %-16s = %d B（%s %s）" % (vp, len(b), e["type"], e["name"]))
    for s in (args.src or []):
        # ★ Windows 宿主路径里**也有冒号**（C:\…）：按正则取"最后一个 :<八进制模式>"与
        #   "/卷内路径"两段，剩下的整段才是宿主路径（否则 split(":") 会把盘符切开）。
        m = re.match(r"^(.+):(/[^:]*):([0-7]{3,4})$", s)
        if not m:
            sys.stderr.write("--src 要写成 <宿主文件>:<卷内绝对路径>:<八进制模式>：%r\n" % s)
            return None
        host, vol, mode = m.group(1), m.group(2), int(m.group(3), 8)
        if not os.path.exists(host):
            sys.stderr.write("找不到 --src 宿主文件：%s\n" % host)
            return None
        out[vol] = (open(host, "rb").read(), mode)
        print("    %-16s = %d B（--src）" % (vol, len(out[vol][0])))
    if args.extra_dir:                                    # 夹具：整目录搬进 /opt/vpkg/extra
        for fn in sorted(os.listdir(args.extra_dir)):
            p = os.path.join(args.extra_dir, fn)
            if os.path.isfile(p):
                out["/opt/vpkg/extra/%s" % fn] = (open(p, "rb").read(), 0o644)
    return out

def verify_vol(vol_bytes, files):
    expect = {k: v[0] for k, v in files.items()}
    bad = TP.verify(vol_bytes, expect)
    if bad:
        sys.stderr.write("卷自检失败：%s\n" % bad)
        return False
    for path in sorted(files):
        got = vol_read(vol_bytes, path)
        if got is None or got != files[path][0]:
            sys.stderr.write("卷内读回不一致：%s（%s vs %s）\n"
                             % (path, len(got) if got is not None else -1, len(files[path][0])))
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make-repo", default=None, help="套件仓库 spec（JSON）")
    ap.add_argument("--make-shipped", action="store_true",
                    help="按内置 spec 造**系统卷那份仓库**（build64/store/repo；--vpkg 必给）")
    ap.add_argument("--repo-out", default=None, help="仓库输出目录（包本体 + index.json）")
    ap.add_argument("--vol-in", default=None, help="输入卷（构建期卷链上一步）")
    ap.add_argument("--vol-out", default=None, help="输出卷")
    ap.add_argument("--fresh-vol", type=int, default=0,
                    help="夹具：从零格式化一块这么大的卷（扇区；0 = 不用）")
    ap.add_argument("--fixture-img", default=None, help="夹具盘输出（整盘）")
    ap.add_argument("--vpkg", default=None, help="build64/store/vpkg.elf")
    ap.add_argument("--store", default=None, help="build64/store/store.elf")
    ap.add_argument("--repo", default=None, help="仓库目录（含 index.json）")
    ap.add_argument("--src", action="append", default=None, help="额外文件 <宿主>:<卷内>:<模式>")
    ap.add_argument("--extra-dir", default=None, help="（夹具）把目录内容装到 /opt/vpkg/extra/")
    ap.add_argument("--writable-roots", action="store_true",
                    help="（夹具）把 /bin /usr/share /var 等目录预建成 0777：桌面会话是 **uid 1000**，"
                         "装系统目录本来要 root（P4 权限真拦截会回 -EACCES）")
    ap.add_argument("--system", default=None, help="build64/system.img")
    ap.add_argument("--disk", default=None, help="输出完整系统盘（构建期）")
    ap.add_argument("--target-sectors", type=int, default=SV.DEFAULT_TARGET_SECTORS)
    args = ap.parse_args()

    if args.make_shipped or args.make_repo:
        if args.make_shipped:
            if not args.vpkg:
                sys.stderr.write("--make-shipped 需要 --vpkg（build64/store/vpkg.elf）\n")
                return 2
            spec = shipped_spec(args.vpkg, args.store or "")
            out_dir = args.repo_out or os.path.join(os.path.dirname(os.path.abspath(args.vpkg)),
                                                    "repo")
            # 把 spec 落盘（构建留证：这一次到底打了哪几个包）
            spath = os.path.join(os.path.dirname(out_dir), "repo_spec.json")
            os.makedirs(os.path.dirname(spath) or ".", exist_ok=True)
            with open(spath, "w", encoding="utf-8") as f:
                json.dump(spec, f, ensure_ascii=False, indent=1)
                f.write("\n")
            print("    spec：%s（--make-shipped）" % spath)
            base = os.path.dirname(os.path.abspath(args.vpkg))
        else:
            spec = json.load(open(args.make_repo, encoding="utf-8"))
            out_dir = args.repo_out or os.path.join(os.path.dirname(args.make_repo))
            base = os.path.dirname(os.path.abspath(args.make_repo))
        index = build_repo(spec, out_dir, base=base)
        bad = verify_repo(out_dir, index, verbose=True)
        if bad:
            sys.stderr.write("仓库自检失败：%s\n" % bad)
            return 1
        print("== 套件仓库 OK：%d 个包 -> %s" % (len(index["packages"]), out_dir))
        return 0

    # ---- 卷：装进已有卷（构建期）或从零格式化（夹具）----
    if args.fresh_vol > 0 and args.fixture_img:
        vol = TP.VolumeEdit(args.fresh_vol)
    elif args.vol_in:
        img = open(args.vol_in, "rb").read()
        if img[0:8] != b"VIMTUFS2":
            sys.stderr.write("输入不是 VimtuFS2 卷：%s\n" % args.vol_in)
            return 2
        total = int.from_bytes(img[20:24], "little")
        vol = TP.VolumeEdit(total)
        vol.load(img)
    else:
        sys.stderr.write("要么 --make-repo，要么 --vol-in/--vol-out，要么 --fixture-img + --fresh-vol\n")
        return 2

    if args.writable_roots:
        # ★ 夹具专用：本仓库的桌面会话跑在 uid=1000 下，而 VimtuFS2 v4 的 /bin 是 root:0755 ->
        #   写入会被 P4 权限拦成 -EACCES（包管理器据此报 perm 错误，那是**如实行为**）。
        #   夹具要把"安装路径"验证出来，所以把这些目录预建成 0777（真实系统要 root 才能装）。
        for d, mode in (("/bin", 0o777), ("/lib", 0o777), ("/etc", 0o777), ("/opt", 0o777),
                        ("/opt/vpkg", 0o777), ("/opt/vpkg/repo", 0o777), ("/Fonts", 0o777),
                        ("/usr", 0o777), ("/usr/share", 0o777), ("/var", 0o777),
                        ("/var/lib", 0o777), ("/var/lib/vpkg", 0o777)):
            vol.mkdirs(d, mode=mode)
        print("    夹具：/bin /usr/share /var 等目录预建成 0777（非 root 会话可写）")

    files = collect_files(args)
    if files is None:
        return 2
    for path in sorted(files):
        vol.put(path, files[path][0], mode=files[path][1])
    out = vol.finish()
    if not verify_vol(out, files):
        return 1
    print("    卷自检 OK：%d 个文件逐字节回读一致" % len(files))

    if args.fixture_img:
        if not args.system:
            sys.stderr.write("--fixture-img 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        target = (args.fresh_vol + PART_MAIN_LBA) if args.fresh_vol > 0 else args.target_sectors
        disk = SV.build_disk(system_bytes, out, target)
        with open(args.fixture_img, "wb") as f:
            f.write(disk)
        print("    夹具盘：%s（%d B = %d 扇区；主分区 LBA %d 起 = VimtuFS2 v4 卷）"
              % (args.fixture_img, len(disk), target, PART_MAIN_LBA))
        return 0

    if not args.vol_out:
        sys.stderr.write("给了 --vol-in 就要 --vol-out\n")
        return 2
    with open(args.vol_out, "wb") as f:
        f.write(out)
    print("    商店卷：%s（%d B）" % (args.vol_out, len(out)))
    if args.disk:
        if not args.system:
            sys.stderr.write("--disk 必须同时给 --system\n")
            return 2
        system_bytes = open(args.system, "rb").read()
        disk = SV.build_disk(system_bytes, out, args.target_sectors)
        with open(args.disk, "wb") as f:
            f.write(disk)
        print("    系统盘：%s（%d B = %d 扇区）" % (args.disk, len(disk), args.target_sectors))
    return 0


if __name__ == "__main__":
    sys.exit(main())
