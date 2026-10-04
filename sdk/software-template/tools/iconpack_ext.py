#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sdk/software-template/tools/iconpack_ext.py - 把 SVG 图标**外置进系统卷**（/icons/**），不进内核

它做两步（两步都留证据）：
  ① 光栅化：用仓库自研光栅器 `tools/svg2png.py`（只填路径、4x4 超采样）把
     `gui_rs/assets/icons/{system,apps}/*.svg` 渲染成 16/24/32/48 px 的 PNG（白 + alpha 掩码）；
  ② 装卷：按内核运行期**真实的查找路径**写进 VimtuFS2 系统卷：
        /icons/<system|apps>/<名字>@<尺寸>.png
     并把逐文件 sha256 写进 `/etc/icons_ext.json`（卷内清单，便于第三方核对）。
     内核侧行为（真源 kernel/icons64.cpp:463-475）：卷里存在 `/icons/…` 时**优先于图标包**，
     打点变成 `[ICON64] load kind=<名字> path=/icons/<sub>/<名字>@<尺寸>.png src=vfs ok=1`。

装卷复用 `packaging/vol_install.py`（同一套 VolumeEdit + 逐字节回读自检），不重写。

用法：
    # 全部图标（system + apps），写进卷并拼一块可引导盘
    py -3 sdk/software-template/tools/iconpack_ext.py \\
          --vol-in build64/demovol.img --vol-out build64/sdk/iconvol.img \\
          --system build64/system.img --disk build64/sdk/icondisk.img

    # 只装某几个（例如本轮新增的）
    py -3 sdk/software-template/tools/iconpack_ext.py --only folder,file,document \\
          --vol-in build64/demovol.img --vol-out build64/sdk/iconvol.img

    # 只光栅化，不写卷（看像素证据）
    py -3 sdk/software-template/tools/iconpack_ext.py --only folder --no-vol

★ 如实边界：**新名字**的图标内核不会主动请求 —— 内核只认 kernel/icons64.cpp 的 kIcon64Names
  那 30 个 kind（系统 20 + 应用 10）。所以：
    * 名字在内核表里的（例如 bell / folder 与内核 kind 同名的情况看表）-> 卷里放上就被加载（src=vfs）；
    * 表外的新名字（folder / document / copy / paste 等）**先把文件装进卷并留档**，
      等内核侧登记了对应 kind 才会被请求（本模板不改 kernel/**，所以这一步如实标"待内核登记"）。
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SDK = os.path.dirname(HERE)                       # sdk/software-template
ROOT = os.path.dirname(os.path.dirname(SDK))      # 仓库根
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(SDK, "packaging"))

ASSETS = os.path.join(ROOT, "gui_rs", "assets", "icons")
SVG2PNG = os.path.join(ROOT, "tools", "svg2png.py")
OUT = os.path.join(ROOT, "build", "sdk", "icons_ext")

SIZES = (16, 24, 32, 48)


def sha256_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 16), b""):
            h.update(b)
    return h.hexdigest()


def collect(only):
    out = []
    for sub in ("system", "apps"):
        d = os.path.join(ASSETS, sub)
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if not fn.endswith(".svg"):
                continue
            name = fn[:-4]
            if only and name not in only:
                continue
            out.append((sub, name, os.path.join(d, fn)))
    return out


def rasterize(items, verbose=True):
    os.makedirs(OUT, exist_ok=True)
    made = []
    for sub, name, svg in items:
        for size in SIZES:
            dst = os.path.join(OUT, sub, "%s@%d.png" % (name, size))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            r = subprocess.run([sys.executable, SVG2PNG, svg, dst, "--size", str(size)],
                               capture_output=True)
            if r.returncode != 0:
                sys.stderr.write("光栅化失败 %s @%d：%s\n"
                                 % (svg, size, r.stderr.decode("utf-8", "replace")[:200]))
                return None
            made.append((sub, name, size, dst))
        if verbose:
            print("   光栅化 %-16s %s" % (name, " ".join("%dpx" % s for s in SIZES)))
    return made


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", default="", help="只处理这些名字（逗号分隔）")
    ap.add_argument("--vol-in")
    ap.add_argument("--vol-out")
    ap.add_argument("--system")
    ap.add_argument("--disk")
    ap.add_argument("--target-sectors", type=int, default=32768)
    ap.add_argument("--no-vol", action="store_true", help="只光栅化，不写卷")
    args = ap.parse_args(argv[1:])

    only = set(x.strip() for x in args.only.split(",") if x.strip())
    items = collect(only)
    if not items:
        sys.stderr.write("没有匹配的 SVG（--only=%r）\n" % args.only)
        return 2
    print("== ① 光栅化 %d 个 SVG（%d 个尺寸）-> %s" % (len(items), len(SIZES), OUT))
    made = rasterize(items)
    if made is None:
        return 1

    manifest = {"generator": "sdk/software-template/tools/iconpack_ext.py",
                "rasterizer": "tools/svg2png.py（自研仅填充路径光栅器，4x4 超采样）",
                "source_dir": "gui_rs/assets/icons（Bootstrap Icons, MIT）",
                "vfs_path_template": "/icons/<system|apps>/<name>@<size>.png",
                "entries": []}
    for sub, name, size, png in made:
        manifest["entries"].append({"sub": sub, "name": name, "size": size,
                                    "bytes": os.path.getsize(png),
                                    "sha256": sha256_file(png)})
    print("   像素证据（前 8 条）：")
    for e in manifest["entries"][:8]:
        print("     %-9s %-12s @%-3d %5d B sha256=%s"
              % (e["sub"], e["name"], e["size"], e["bytes"], e["sha256"][:16]))

    if args.no_vol:
        print("== ② 跳过装卷（--no-vol）")
        return 0
    if not (args.vol_in and args.vol_out):
        sys.stderr.write("要给 --vol-in/--vol-out 才会装卷（或用 --no-vol 只光栅化）\n")
        return 2

    man_path = os.path.join(OUT, "manifest.json")
    with open(man_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=1)
    print("== ② 装卷（VimtuFS2）：%d 个 PNG + /etc/icons_ext.json" % len(made))
    import vol_install as vi
    argv2 = ["vol_install", "--vol-in", args.vol_in, "--vol-out", args.vol_out,
             "--target-sectors", str(args.target_sectors),
             "--src", "%s:/etc/icons_ext.json:0644" % man_path]
    for sub, name, size, png in made:
        argv2 += ["--src", "%s:/icons/%s/%s@%d.png:0644" % (png, sub, name, size)]
    if args.disk:
        argv2 += ["--disk", args.disk]
        if args.system:
            argv2 += ["--system", args.system]
    return vi.main(argv2)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
