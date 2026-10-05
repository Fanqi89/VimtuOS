#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/kernel_size_report.py - 内核体积构成统计（逐目标文件 / 逐节 / 分类小计）

用途（本批 ① 项的交付）：回答"内核余量该往哪腾"——把链接产物拆成
    ① 逐节（.text/.rodata/.data/.bss + 文件字节数）
    ② 逐目标文件的 .text/.rodata/.data 字节（Top N 调用者）
    ③ 四类小计与占比：GUI 专用 / 驱动 / 核心 / 其它（含内嵌资源与外壳应用）
并支持 `--before` 对比出**逐节增量**（本批 ⑦ 项的"前后字节数"口径）。

为什么用**纯 Python 解析 ELF**（不 shell 调 llvm-size / objdump）：
  * 本机的 clang 是 mingw64 版（没有随附 llvm-size/llvm-objdump），msys2 的 objdump 在
    Windows 原生 python 下不一定在 PATH 里；
  * 验收脚本必须是 `py -3` 能跑的（与 tests/*.py 同一条纪律），不能依赖 msys2 环境；
  * 只读节头表 + .shstrtab，几十行就够，且**不依赖被统计的对象是否可反汇编**。

口径（与 build64.sh 的链接行一致；差量说明写在这里，不藏）：
  * 对象清单 = build64/os/*.o + 仓库根的四个汇编对象（entry/isr_stubs/switch/syscall_entry，
    它们在链接行里用的是 $BUILD/<name>.o 而不是 os/ 下那份）+ gui_rs/gui_rs.o
    + build64/font_*_z.o（链接行里 $BUILD/font_*_z.o 与 os/font_*_z.o 是同一批，只算一次）。
  * VIMTU_USER_FBDEMO=asm 之外（默认 c）**不链** build64/os/user_fbdemo64.o
    （build64.sh 的 ASM_FBDEMO_OBJ=""），这里也把它排除并在输出里注明。
  * 逐对象之和 != ELF 节大小（差 = 链接器的对齐/填充 + 节名前缀归并），
    脚本会把两者都打出来并给出差值 —— 那部分就是"对齐税"。

分类（任务书 ① 项点名的三类 + 其余）：
  * GUI 专用：gui64/panels64/startmenu64/locklogin64/explorer64/theme64/gfx64/img64
              -> 未来搬 Ring 3 的目标
  * 驱动：ata64/ahci64/nvme64/usb64/xhci64/ehci64/hda64/e1000_64/virtio_gpu64
              -> 未来"模块化 / 用户态驱动"的目标
  * 核心：mem64/task64/proc64/vfs64/fs64/fd64/elf64/syscall64
              -> 应保留
  * 其它：剩下的全部（内嵌资源、桌面外壳应用、启动/平台层…），并按字节数逐个列出

用法：
    py -3 tools/kernel_size_report.py                          # 默认看 build64/kernel64_os.elf
    py -3 tools/kernel_size_report.py --top 30
    py -3 tools/kernel_size_report.py --save baseline.json     # 存一份机读快照
    py -3 tools/kernel_size_report.py --before baseline.json   # 与快照比逐节增量
    py -3 tools/kernel_size_report.py --elf build64/kernel64.elf   # 安装介质内核
退出码：0 = 成功；2 = 输入缺失 / 不是 ELF。
"""
import argparse
import glob
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# 内核预算三层（与 tests/a42a64_test.py 的三个常量**同一口径**；这里只做展示，
# 真正的断言在测试脚本里 —— 统计脚本不改任何阈值）
LIMIT_BYTES = 8000 * 512            # 内核区硬上限 LBA 9..8008 = 4,096,000 B
RESERVE_BYTES = 640 * 1024          # 预留下限 R = 640 KiB
BATCH_BASELINE_BYTES = 3418352      # 本批基线（写死的回归锚点）

COUNTED = (".text", ".rodata", ".data")     # 计入"内核体积"的节（.bss 单独列，不占文件字节）

GUI_OBJS = {"gui64", "panels64", "startmenu64", "locklogin64",
            "explorer64", "theme64", "gfx64", "img64"}
DRIVER_OBJS = {"ata64", "ahci64", "nvme64", "usb64", "xhci64", "ehci64", "hda64",
               "e1000_64", "virtio_gpu64"}
CORE_OBJS = {"mem64", "task64", "proc64", "vfs64", "fs64", "fd64", "elf64", "syscall64"}
# 其余"桌面外壳应用"（也属于"将来可搬"的候选，但不在任务书点名的 GUI 八个里）
SHELL_APPS = {"icons64", "desktopops64", "calc64", "mines64", "terminal64",
              "settings64", "taskmgr64", "userdb64", "locklogin64", "session64",
              "preload64", "update64", "config64", "sysstate64"}
# 内嵌资源对象（objcopy -I binary 造出来的：blob / 字体 / 图标包 / 图片）
BLOB_OBJS = {"font_bahnschrift_z", "font_fallback_z", "font_mono_z", "font_simhei_z",
             "icon_start_mini", "iconpack_bin", "kaisi_png", "musl_hello_elf",
             "ldvimtu_so", "libfoo_so", "dynhello_elf", "xmmsse_elf", "hello_elf64_elf",
             "proc64_elf", "spin64_elf", "filedemo64_elf", "pipe64_elf", "evshm_elf",
             "sig64_elf", "hello_vap64", "user_demo64", "user_fbdemo64",
             "user_hello_cblob", "user_libctest_cblob", "user_fbdemo_cblob",
             "ap_trampoline64", "bootx64_efi", "uefi64_bin"}


# ---------------------------------------------------------------------------
# 极简 ELF64 读取（只需要节头表 + .shstrtab）
# ---------------------------------------------------------------------------
def elf_sections(path):
    d = open(path, "rb").read()
    if d[:4] != b"\x7fELF" or d[4] != 2:
        raise ValueError("不是 ELF64：%s" % path)
    shoff = struct.unpack_from("<Q", d, 0x28)[0]
    shentsize = struct.unpack_from("<H", d, 0x3A)[0]
    shnum = struct.unpack_from("<H", d, 0x3C)[0]
    shstrndx = struct.unpack_from("<H", d, 0x3E)[0]
    if shoff == 0 or shnum == 0:
        return {}
    raw = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, typ, flags, addr, offset, size = struct.unpack_from("<IIQQQQ", d, off)
        raw.append({"name": name, "type": typ, "flags": flags,
                    "addr": addr, "offset": offset, "size": size})
    tab = raw[shstrndx]
    strs = d[tab["offset"]:tab["offset"] + tab["size"]]
    out = {}
    for s in raw:
        end = strs.find(b"\0", s["name"])
        nm = strs[s["name"]:end].decode("ascii", "replace") if s["name"] else ""
        out[nm] = s
    return out


def obj_size(path):
    """返回 {'.text':n, '.rodata':n, '.data':n, '.bss':n, 'total':n}（按节名前缀归并）。"""
    acc = {k: 0 for k in COUNTED}
    acc[".bss"] = 0
    try:
        secs = elf_sections(path)
    except ValueError:
        return None
    for nm, s in secs.items():
        if not (s["flags"] & 0x2):          # SHF_ALLOC：只算真正进内存映像的节
            continue
        if (s["flags"] & 0x4) and s["type"] == 8:   # SHT_NOBITS（.bss 家族）
            for k in (".bss",):
                acc[k] += s["size"]
            continue
        base = nm.split(".", 2)
        base = "." + base[1] if len(base) > 1 and base[1] else nm
        if base in COUNTED:
            acc[base] += s["size"]
        elif base == ".bss":
            acc[".bss"] += s["size"]
    acc["total"] = acc[".text"] + acc[".rodata"] + acc[".data"]
    return acc


def obj_inventory(objdir, elf):
    """按 build64.sh 的系统内核链接行复现对象清单（返回 [(name, path)]）。"""
    items = []
    seen = set()
    for p in sorted(glob.glob(os.path.join(objdir, "*.o"))):
        nm = os.path.basename(p)[:-2]
        if nm == "user_fbdemo64":            # 默认配置（VIMTU_USER_FBDEMO=c）不链它
            continue
        items.append((nm, p))
        seen.add(nm)
    # 链接行里来自仓库根 build64/ 的汇编对象（与 os/ 下那份是同一批，只取一次）
    b = os.path.dirname(objdir.rstrip("/\\"))
    for nm in ("entry64", "isr_stubs64", "switch64", "syscall_entry64"):
        p = os.path.join(b, nm + ".o")
        if os.path.exists(p) and nm not in seen:
            items.append((nm, p))
            seen.add(nm)
    # 字体压缩对象（链接行里 $BUILD/font_*_z.o；与 os/ 下同一批，已被 seen 挡掉）
    for p in sorted(glob.glob(os.path.join(b, "font_*_z.o"))):
        nm = os.path.basename(p)[:-2]
        if nm not in seen:
            items.append((nm, p))
            seen.add(nm)
    g = os.path.join(ROOT, "gui_rs", "gui_rs.o")
    if os.path.exists(g) and "gui_rs" not in seen:
        items.append(("gui_rs", g))
    return items


def classify(nm):
    if nm in GUI_OBJS:
        return "GUI 专用（→ 搬 Ring 3）"
    if nm in DRIVER_OBJS:
        return "驱动（→ 模块化/用户态驱动）"
    if nm in CORE_OBJS:
        return "核心（保留）"
    if nm in BLOB_OBJS:
        return "内嵌资源 blob"
    if nm in SHELL_APPS:
        return "桌面外壳应用/服务"
    return "启动·平台·其它"


def snapshot(elf, objdir):
    secs = elf_sections(elf)
    per = {}
    for k in COUNTED:
        per[k] = sum(s["size"] for nm, s in secs.items()
                     if (s["flags"] & 0x2) and s["type"] != 8 and
                     (nm == k or nm.startswith(k + ".")))
    bss = sum(s["size"] for nm, s in secs.items()
              if (s["flags"] & 0x2) and s["type"] == 8)
    objs = {}
    for nm, p in obj_inventory(objdir, elf):
        o = obj_size(p)
        if o:
            objs[nm] = o
    return {"elf": elf, "sections": per, "bss": bss, "objs": objs}


def fmt(n):
    return "%9d" % n


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=os.path.join(ROOT, "build64", "kernel64_os.elf"))
    ap.add_argument("--bin", default=None, help="对应的平铺二进制（默认同名 .bin）")
    ap.add_argument("--objdir", default=None, help="目标文件目录（默认与 elf 同级的 os/）")
    ap.add_argument("--top", type=int, default=20)
    ap.add_argument("--save", default=None, help="把这次的口径存成 JSON 快照")
    ap.add_argument("--before", default=None, help="与一份 JSON 快照比逐节增量")
    ap.add_argument("--quiet-objs", action="store_true", help="只打 Top N，不打完整分类清单")
    args = ap.parse_args()

    elf = args.elf if os.path.isabs(args.elf) else os.path.join(ROOT, args.elf)
    if not os.path.exists(elf):
        sys.stderr.write("找不到 ELF：%s\n" % elf)
        return 2
    bindef = args.bin or (elf[:-4] + ".bin" if elf.endswith(".elf") else None)
    objdir = args.objdir or os.path.join(os.path.dirname(elf), "os")
    if not os.path.isdir(objdir):
        sys.stderr.write("找不到目标文件目录：%s\n" % objdir)
        return 2

    snap = snapshot(elf, objdir)
    secs = snap["sections"]
    objs = snap["objs"]

    print("=== VimtuOS 内核体积构成（tools/kernel_size_report.py）===")
    print("ELF：%s" % os.path.relpath(elf, ROOT).replace("\\", "/"))
    print()
    print("---- ① 逐节（SHF_ALLOC；.bss 不占文件字节）----")
    for k in COUNTED:
        print("   %-9s %s B" % (k, fmt(secs[k])))
    print("   %-9s %s B（NOBITS，不占内核区）" % (".bss", fmt(snap["bss"])))
    sec_total = sum(secs.values())
    print("   %-9s %s B（.text+.rodata+.data）" % ("节合计", fmt(sec_total)))
    if bindef and os.path.exists(bindef):
        bsz = os.path.getsize(bindef)
        print("   %-9s %s B（%s）" % ("平铺二进制", fmt(bsz),
                                     os.path.relpath(bindef, ROOT).replace("\\", "/")))
        print("     内核区上限 %d B -> 余量 %d B；预留下限 R=%d B -> 扣预留后剩 %d B"
              % (LIMIT_BYTES, LIMIT_BYTES - bsz, RESERVE_BYTES, LIMIT_BYTES - bsz - RESERVE_BYTES))
        print("     相对本批基线 %d B：%+d B（允许增长 %d B）"
              % (BATCH_BASELINE_BYTES, bsz - BATCH_BASELINE_BYTES,
                 LIMIT_BYTES - BATCH_BASELINE_BYTES - RESERVE_BYTES))
    print()

    # ---- ② 逐目标文件 ----
    rows = sorted(objs.items(), key=lambda kv: -kv[1]["total"])
    print("---- ② 逐目标文件 Top %d（按 .text+.rodata+.data）----" % args.top)
    print("   %-22s %9s %9s %9s %10s %11s" % ("对象", ".text", ".rodata", ".data", "合计", ".bss"))
    for nm, o in rows[:args.top]:
        print("   %-22s %s %s %s %s %s"
              % (nm, fmt(o[".text"]), fmt(o[".rodata"]), fmt(o[".data"]),
                 fmt(o["total"]), fmt(o[".bss"])))
    # ---- ③ 分类小计 ----
    obj_total = sum(o["total"] for _, o in rows)
    print("   %-22s %s（逐对象合计；与节合计差 %+d B = 链接器对齐填充 - .rodata 常量池合并去重，"
          "唯一对得上的是**平铺二进制**）"
          % ("[合计 %d 个对象]" % len(rows), fmt(obj_total), sec_total - obj_total))
    print()
    buckets = {}
    for nm, o in rows:
        buckets.setdefault(classify(nm), {"n": 0, "s": 0, "list": []})
        buckets[classify(nm)]["n"] += 1
        buckets[classify(nm)]["s"] += o["total"]
        buckets[classify(nm)]["list"].append((nm, o))
    print("---- ③ 分类小计（占比分母 = 逐对象合计 %d B）----" % obj_total)
    order = ["核心（保留）", "驱动（→ 模块化/用户态驱动）", "GUI 专用（→ 搬 Ring 3）",
             "桌面外壳应用/服务", "内嵌资源 blob", "启动·平台·其它"]
    for k in order:
        if k not in buckets:
            continue
        v = buckets[k]
        print("   %-30s %2d 个  %s B  %5.1f%%" % (k, v["n"], fmt(v["s"]), 100.0 * v["s"] / obj_total))
    # 桌面外壳全家桶 = 任务书点名的 8 个 GUI 文件 + 其它外壳应用/服务
    gui_all = buckets.get("GUI 专用（→ 搬 Ring 3）", {"s": 0})["s"] + \
        buckets.get("桌面外壳应用/服务", {"s": 0})["s"]
    print("   %-30s %s B  %5.1f%%（= GUI 八个 + 外壳应用/服务；\"整体搬桌面\"的量级）"
          % ("[参考] 桌面全家桶", fmt(gui_all), 100.0 * gui_all / obj_total))
    print()
    if not args.quiet_objs:
        print("---- ③b 分类明细（每类内部按字节数降序）----")
        for k in order:
            if k not in buckets:
                continue
            print("   [%s]" % k)
            for nm, o in sorted(buckets[k]["list"], key=lambda kv: -kv[1]["total"]):
                print("      %-24s %s B  (text=%d rodata=%d data=%d)"
                      % (nm, fmt(o["total"]), o[".text"], o[".rodata"], o[".data"]))
        print()

    # ---- ④ 与快照对比（逐节增量）----
    if args.before:
        bpath = args.before if os.path.isabs(args.before) else os.path.join(ROOT, args.before)
        old = json.load(open(bpath, "r", encoding="utf-8"))
        print("---- ④ 逐节增量 vs %s ----" % os.path.relpath(bpath, ROOT).replace("\\", "/"))
        for k in COUNTED:
            a, b = old["sections"].get(k, 0), secs[k]
            print("   %-9s %s -> %s  (%+d B)" % (k, fmt(a), fmt(b), b - a))
        a, b = old.get("bss", 0), snap["bss"]
        print("   %-9s %s -> %s  (%+d B)" % (".bss", fmt(a), fmt(b), b - a))
        print("   节合计    %s -> %s  (%+d B)"
              % (fmt(sum(old["sections"].values())), fmt(sec_total),
                 sec_total - sum(old["sections"].values())))
        allk = sorted(set(list(old["objs"].keys()) + list(objs.keys())))
        deltas = [(nm, objs.get(nm, {}).get("total", 0) - old["objs"].get(nm, {}).get("total", 0))
                  for nm in allk]
        deltas = [d for d in deltas if d[1] != 0]
        for nm, d in sorted(deltas, key=lambda kv: -abs(kv[1]))[:args.top]:
            print("   %-24s %+d B  (%d -> %d)"
                  % (nm, d, old["objs"].get(nm, {}).get("total", 0), objs.get(nm, {}).get("total", 0)))
        if not deltas:
            print("   （逐对象无变化）")
        print()

    if args.save:
        spath = args.save if os.path.isabs(args.save) else os.path.join(ROOT, args.save)
        json.dump(snap, open(spath, "w", encoding="utf-8"), indent=1, sort_keys=True)
        print("快照已存：%s" % os.path.relpath(spath, ROOT).replace("\\", "/"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
