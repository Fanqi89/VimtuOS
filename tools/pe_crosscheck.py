#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/pe_crosscheck.py - ★ v0.9.43（W0）：PE 解析的**宿主侧独立实现**（交叉核对器）

用途有两层：

1. **交叉核对**：`tests/peinfo64_test.py` 让内体 `/bin/peinfo` 在 VimtuOS 里看同一个 exe，
   再用本文件在宿主上看一遍，两边**逐字段比对**。两套实现（C 的偏移读 vs Python 的 struct）
   如果对同一个文件得出同一组字段，才说明"我们真的读懂了 PE"，而不是"两处抄了同一个错"。

2. **导入并集清单的直接来源**：`py -3 tools/pe_crosscheck.py --union build64/pexcorpus/*.exe`
   打印所有 exe 的**导入并集** —— 那就是 W2/W3 要实现的 API 清单（路线图里的 W0 产出）。

为什么不复用 tools/pe_info.py：那个脚本是给 **EFI 镜像**调试用的（只看 PE32+ 头、基址与
重定位表是否存在），不解析导入表，也不做节表 RVA 换算。硬塞进去会让两边职责都变糊。

字段口径与 user/win/peinfo.c **逐项对齐**（同一个名字、同一个进制/顺序），这样比对就是
一行行字符串相等，不需要再写映射层。

用法：
    py -3 tools/pe_crosscheck.py FILE...             # 人读的完整报告
    py -3 tools/pe_crosscheck.py --json FILE...      # 输出 JSON（测试用）
    py -3 tools/pe_crosscheck.py --union FILE...     # 只打导入并集（W2/W3 清单）
退出码：0 = 全部解析成功；1 = 有文件解析失败。
"""
import json
import struct
import sys

MAX_SEC = 96
MAX_DLL = 64
MAX_DLL_FUNCS = 4096
MAX_EXPORTS = 512
MAX_RELOC_BLK = 256
MAX_NAME = 64

SUBSYS = {1: "native", 2: "gui", 3: "cui", 9: "wince",
          10: "efi", 11: "efi-bs", 12: "efi-rt", 13: "efi-rom"}


class PeError(Exception):
    pass


def _u16(b, o):
    if o + 2 > len(b):
        raise PeError("trunc@%#x" % o)
    return struct.unpack_from("<H", b, o)[0]


def _u32(b, o):
    if o + 4 > len(b):
        raise PeError("trunc@%#x" % o)
    return struct.unpack_from("<I", b, o)[0]


def _u64(b, o):
    if o + 8 > len(b):
        raise PeError("trunc@%#x" % o)
    return struct.unpack_from("<Q", b, o)[0]


def _cstr(b, off, cap=MAX_NAME):
    """PE 里的 NUL 字符串；越界/0 偏移当"取不到"。与 C 版 copy_str 同口径。"""
    if not off or off >= len(b):
        return ""
    end = off
    while end < len(b) and b[end] and (end - off) < cap:
        end += 1
    return b[off:end].decode("latin-1")


def parse(path):
    """把一份 PE 解析成与 user/win/peinfo.c 同口径的 dict。"""
    with open(path, "rb") as f:
        b = f.read()

    r = {"file": path, "size": len(b), "truncated": 0}
    if len(b) < 64 or b[:2] != b"MZ":
        raise PeError("dos")
    lfanew = _u32(b, 0x3C)
    r["lfanew"] = lfanew
    if lfanew < 64 or lfanew + 24 > len(b) or b[lfanew:lfanew + 4] != b"PE\0\0":
        raise PeError("nt")
    coff = lfanew + 4
    r["machine"] = _u16(b, coff)
    nsec = _u16(b, coff + 2)
    optsz = _u16(b, coff + 16)
    r["nsec"] = nsec
    r["optsz"] = optsz
    r["chars"] = _u16(b, coff + 18)
    if nsec == 0 or nsec > MAX_SEC:
        r["truncated"] = 1

    opt = coff + 20
    magic = _u16(b, opt)
    r["magic"] = magic
    if magic not in (0x10B, 0x20B):
        raise PeError("magic")
    is64 = magic == 0x20B
    r["imagebase"] = _u64(b, opt + 24) if is64 else _u32(b, opt + 28)
    r["entry"] = _u32(b, opt + 16)
    r["sectalign"] = _u32(b, opt + 32)
    r["filealign"] = _u32(b, opt + 36)
    r["sizeimage"] = _u32(b, opt + 56)
    r["sizehdr"] = _u32(b, opt + 60)
    r["subsys"] = _u16(b, opt + 68)
    r["subsys_name"] = SUBSYS.get(r["subsys"], "unknown")
    r["dllchar"] = _u16(b, opt + 70)
    nrva = _u32(b, opt + (108 if is64 else 92))
    r["nrva"] = min(nrva, 16)
    dd = opt + (112 if is64 else 96)

    sec_off = opt + optsz
    if sec_off + nsec * 40 > len(b):
        raise PeError("trunc")
    r["sec_off"] = sec_off
    secs = []
    for i in range(min(nsec, MAX_SEC)):
        so = sec_off + i * 40
        name = b[so:so + 8].rstrip(b"\0 ").decode("latin-1") or "-"
        secs.append({
            "idx": i, "name": name,
            "va": _u32(b, so + 12), "vsz": _u32(b, so + 8),
            "raw": _u32(b, so + 20), "rawsz": _u32(b, so + 16),
            "flags": _u32(b, so + 36),
        })
    r["sections"] = secs

    def rva2off(rva):
        if rva < r["sizehdr"]:
            return rva
        for s in secs:
            span = s["vsz"] or s["rawsz"]
            if s["va"] <= rva < s["va"] + span:
                d = rva - s["va"]
                return (s["raw"] + d) if d < s["rawsz"] else 0
        return 0

    # 数据目录（只记非空）
    dirs = {}
    for i in range(r["nrva"]):
        o = dd + i * 8
        if o + 8 > len(b):
            break
        va, sz = _u32(b, o), _u32(b, o + 4)
        if va or sz:
            dirs[i] = {"vadr": va, "size": sz}
    r["dirs"] = dirs

    # 导入
    imports = []
    n_func = n_ord = 0
    ivadr = dirs.get(1, {}).get("vadr", 0)
    ioff = rva2off(ivadr) if ivadr else 0
    tsz = 8 if is64 else 4
    while ioff and len(imports) < MAX_DLL:
        if ioff + 20 > len(b):
            break
        oft = _u32(b, ioff)
        name_rva = _u32(b, ioff + 12)
        ft = _u32(b, ioff + 16)
        if not oft and not name_rva and not ft:
            break
        dll = _cstr(b, rva2off(name_rva) if name_rva else 0) or "?dll"
        thunk_rva = oft or ft
        t_off = rva2off(thunk_rva) if thunk_rva else 0
        entries = []
        if t_off:
            for k in range(MAX_DLL_FUNCS):
                eo = t_off + k * tsz
                if eo + tsz > len(b):
                    break
                tv = _u64(b, eo) if is64 else _u32(b, eo)
                if not tv:
                    break
                by_ord = bool(tv >> 63) if is64 else bool(tv & 0x80000000)
                if by_ord:
                    entries.append({"ord": tv & 0xFFFF})
                    n_ord += 1
                else:
                    bo = rva2off(tv & 0x7FFFFFFF)
                    fn = _cstr(b, bo + 2) if bo else ""
                    entries.append({"name": fn, "hint": _u16(b, bo) if bo else 0})
                    n_func += 1
        imports.append({"dll": dll, "entries": entries})
        ioff += 20
    r["imports"] = imports
    r["n_dll"] = len(imports)
    r["n_func"] = n_func
    r["n_ord"] = n_ord

    # 导出
    exports = []
    eoff = rva2off(dirs.get(0, {}).get("vadr", 0)) if 0 in dirs else 0
    if eoff and eoff + 40 <= len(b):
        nname = _u32(b, eoff + 24)
        base = _u32(b, eoff + 16)
        r["export_name"] = _cstr(b, rva2off(_u32(b, eoff + 12)))
        r["export_base"] = base
        r["export_funcs"] = _u32(b, eoff + 20)
        r["export_names"] = nname
        no = rva2off(_u32(b, eoff + 32))
        oo = rva2off(_u32(b, eoff + 36))
        fo = rva2off(_u32(b, eoff + 28))
        for i in range(min(nname, MAX_EXPORTS)):
            if not (no and oo and fo):
                break
            if no + i * 4 + 4 > len(b) or oo + i * 2 + 2 > len(b):
                break
            ordi = _u16(b, oo + i * 2)
            if fo + ordi * 4 + 4 > len(b):
                continue
            exports.append({"idx": i, "rva": _u32(b, fo + ordi * 4),
                            "name": _cstr(b, rva2off(_u32(b, no + i * 4)))})
    else:
        r["export_name"] = None
        r["export_base"] = 0
        r["export_funcs"] = 0
        r["export_names"] = 0
    r["exports"] = exports

    # 重定位
    blocks = n_rel = 0
    types = set()
    rv = dirs.get(5, {})
    roff = rva2off(rv.get("vadr", 0)) if rv else 0
    rend = min(roff + rv["size"], len(b)) if roff else 0
    while roff and roff + 8 <= rend and blocks < MAX_RELOC_BLK:
        bsz = _u32(b, roff + 4)
        if bsz < 8 or roff + bsz > rend:
            break
        for k in range((bsz - 8) // 2):
            ty = _u16(b, roff + 8 + k * 2) >> 12
            if ty:
                types.add(ty)
                n_rel += 1
        blocks += 1
        roff += bsz
    r["reloc_blocks"] = blocks
    r["reloc_entries"] = n_rel
    r["reloc_types"] = sorted(types)

    # TLS / 资源 / CLR
    r["tls"] = None
    tls = dirs.get(9, {})
    if tls:
        to = rva2off(tls["vadr"])
        cb_field = to + (24 if is64 else 12)
        ncb = 0
        if to and cb_field + tsz <= len(b):
            cb = _u64(b, cb_field) if is64 else _u32(b, cb_field)
            cbo = rva2off(cb & 0xFFFFFFFF) if cb else 0
            if cbo:
                for k in range(64):
                    if cbo + k * tsz + tsz > len(b):
                        break
                    v = _u64(b, cbo + k * tsz) if is64 else _u32(b, cbo + k * tsz)
                    if not v:
                        break
                    ncb += 1
        r["tls"] = {"vadr": tls["vadr"], "callbacks": ncb}
    r["res"] = dirs.get(2)
    r["clr"] = dirs.get(14)

    # 导入并集（与 C 版 union 行同口径：dll!name / dll!ord#N，用 | 连接）
    parts = []
    for im in imports:
        for e in im["entries"]:
            if "ord" in e:
                parts.append("%s!ord#%d" % (im["dll"], e["ord"]))
            else:
                parts.append("%s!%s" % (im["dll"], e["name"]))
    r["union"] = "|".join(parts)
    return r


def report(r):
    print("[PEINFO] file=%s size=%d read=%d" % (r["file"], r["size"], r["size"]))
    print("[PEINFO] dos mz=1 lfanew=%#x" % r["lfanew"])
    print("[PEINFO] nt machine=%#x nsec=%d optsz=%d chars=%#x"
          % (r["machine"], r["nsec"], r["optsz"], r["chars"]))
    print("[PEINFO] opt magic=%#x" % r["magic"])
    print("[PEINFO] opt subsys=%d(%s) imagebase=%#x entry=%#x"
          % (r["subsys"], r["subsys_name"], r["imagebase"], r["entry"]))
    print("[PEINFO] opt sizeimage=%#x sizehdr=%#x sectalign=%#x filealign=%#x dllchar=%#x nrva=%d"
          % (r["sizeimage"], r["sizehdr"], r["sectalign"], r["filealign"],
             r["dllchar"], r["nrva"]))
    for s in r["sections"]:
        f = s["flags"]
        perm = ("r" if f & 0x40000000 else "-") + ("w" if f & 0x80000000 else "-") + \
               ("x" if f & 0x20000000 else "-")
        print("[PEINFO] section idx=%d name=%s va=%#x vsz=%#x raw=%#x rawsz=%#x flags=%#x perm=%s"
              % (s["idx"], s["name"], s["va"], s["vsz"], s["raw"], s["rawsz"], f, perm))
    for i, d in sorted(r["dirs"].items()):
        print("[PEINFO] dir idx=%d vadr=%#x size=%#x" % (i, d["vadr"], d["size"]))
    for im in r["imports"]:
        nf = sum(1 for e in im["entries"] if "name" in e)
        no = sum(1 for e in im["entries"] if "ord" in e)
        print("[PEINFO] import dll=%s thunks=%d funcs=%d ord=%d"
              % (im["dll"], len(im["entries"]), nf, no))
        for e in im["entries"]:
            if "ord" in e:
                print("[PEINFO] import_o dll=%s ord=%d" % (im["dll"], e["ord"]))
            else:
                print("[PEINFO] import_f dll=%s name=%s hint=%d"
                      % (im["dll"], e["name"], e["hint"]))
    if r["export_name"] is None:
        print("[PEINFO] export name=- base=0 funcs=0 names=0")
    else:
        print("[PEINFO] export name=%s base=%d funcs=%d names=%d"
              % (r["export_name"] or "?", r["export_base"],
                 r["export_funcs"], r["export_names"]))
        for e in r["exports"]:
            print("[PEINFO] export_f idx=%d rva=%#x name=%s"
                  % (e["idx"], e["rva"], e["name"] or "?"))
    print("[PEINFO] reloc blocks=%d entries=%d types=%s"
          % (r["reloc_blocks"], r["reloc_entries"],
             ",".join("%#x" % t for t in r["reloc_types"]) if r["reloc_types"] else "-"))
    if r["tls"]:
        print("[PEINFO] tls vadr=%#x callbacks=%#x" % (r["tls"]["vadr"], r["tls"]["callbacks"]))
    if r["res"]:
        print("[PEINFO] res vadr=%#x size=%#x" % (r["res"]["vadr"], r["res"]["size"]))
    if r["clr"]:
        print("[PEINFO] clr vadr=%#x size=%#x" % (r["clr"]["vadr"], r["clr"]["size"]))
    print("[PEINFO] summary dlls=%d funcs=%d ord=%d exports=%d relocs=%d tls=%d res=%d clr=%d truncated=%d"
          % (r["n_dll"], r["n_func"], r["n_ord"], len(r["exports"]),
             r["reloc_entries"], 1 if r["tls"] else 0, 1 if r["res"] else 0,
             1 if r["clr"] else 0, r["truncated"]))
    print("[PEINFO] union \"%s\"" % r["union"])


def main(argv):
    if len(argv) < 2:
        sys.stderr.write(__doc__)
        return 2
    mode = "report"
    files = []
    for a in argv[1:]:
        if a == "--json":
            mode = "json"
        elif a == "--union":
            mode = "union"
        else:
            files.append(a)
    if not files:
        sys.stderr.write("需要至少一个 PE 文件\n")
        return 2

    rc = 0
    out = []
    for f in files:
        try:
            r = parse(f)
        except (PeError, OSError) as e:
            sys.stderr.write("reject %s reason=%s\n" % (f, e))
            rc = 1
            continue
        out.append(r)
        if mode == "report":
            report(r)
        elif mode == "union":
            print("%-40s subsys=%d(%s) dlls=%d funcs=%d ord=%d"
                  % (f, r["subsys"], r["subsys_name"], r["n_dll"], r["n_func"], r["n_ord"]))
            for im in r["imports"]:
                print("   %-22s %d" % (im["dll"], len(im["entries"])))
            print("   union: %s" % (r["union"] or "(none)"))
    if mode == "json":
        print(json.dumps(out, indent=2, sort_keys=True))
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv))
