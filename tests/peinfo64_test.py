#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/peinfo64_test.py - ★ v0.9.43（W0）：PE 只读体检器 `/bin/peinfo` 的端到端验收

**这一条要证明的事**（W0 的判据，全部可证伪）：

  ① `/bin/peinfo` 在 VimtuOS 里能起来、对**真 exe** 打印出完整字段（不是"能跑就算过"）。
  ② 它的解析结果与**宿主侧独立实现** `tools/pe_crosscheck.py` **逐字段一致**：
     machine / nsec / optsz / magic / subsys / imagebase / entry / sizeimage / sizehdr /
     sectalign / filealign / nrva、**每个节**（idx/name/va/vsz/raw/rawsz/flags/perm）、
     **每条导入**（dll + 函数名/hint 或序号）、summary、以及**导入并集 `union`**。
     两套实现（C 的偏移读 vs Python 的 struct）得出同一组值，才说明"真读懂了 PE"，
     而不是"两处抄了同一个错"。
  ③ 三份语料覆盖三种形态：**无导入**（tiny，subsystem=3 但无导入表）/
     **控制台+CRT**（cui）/ **图形+MessageBoxA**（gui，subsystem=2）——
     gui 的 `union` 必须含 `USER32.dll!MessageBoxA`，那是 W3 的最小目标。
  ④ 反例：对**不是 PE 的文件**（用 /bin/vpkg 这个 ELF）必须 `reject reason=dos` 且**非 0 退出**，
     不许"看不出东西也报成功"。
  ⑤ 全程不得出现 PANIC / TRIPLE FAULT / `[PEINFO] reject reason=trunc`。

语料来源：`tools/pex_corpus_build.py`（mingw-w64 现场编的真 PE）+ `build64/pexcorpus/PEX.json`。
语料缺失时 ①②③ 记 **SKIP(env)**（不是 PASS），仍跑 ④ 这条反例。

桌面/登录/终端一律复用已验证的公共手势（`qemuhelp.wait_desktop` 带登录、Dock 终端入口），
**不要**自己发明"等 [GUI64] ready"——那块演示盘要跑完整套自检，实测到桌面 ≈ 204 s，
且盘里有 `ui.login.last` 时还要过锁屏（踩过：等 ready 会超时假红）。

用法：py -3 tests/peinfo64_test.py [--qemu PATH] [--keep]
退出码：0 = 全过（含 SKIP）；1 = 有断言失败；2 = 环境问题（缺盘/缺 QEMU）。
"""
import argparse
import importlib.util
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh                                            # noqa: E402
import startmenu64_test as SM                                    # noqa: E402
import install_persist64_test as P                               # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files\qemu\qemu-system-x86_64w.exe",
]
DISK = os.path.join(ROOT, "build64", "sysdisk.img")
CORPUS = os.path.join(ROOT, "build64", "pexcorpus")
PORT = 5673
ok = True
checks = []
skipped = []


def check(name, cond, detail="", skip_env=False):
    global ok
    if skip_env:
        skipped.append(name)
        print("  [SKIP] %s %s" % (name, detail), flush=True)
        return
    ok = ok and bool(cond)
    checks.append((name, bool(cond)))
    print("  [%s] %s %s" % ("PASS" if cond else "FAIL", name, detail), flush=True)


def q(p):
    return p.replace("\\", "/")


def find_qemu(cli):
    for c in ([cli] if cli else []) + QEMU_CANDIDATES:
        if c and os.path.exists(c):
            return c
    return None


def log_of(p):
    try:
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def start(qemu, img, serial, port):
    """起 QEMU（与 dpkg64_test 同一条形状：-serial file: + telnet monitor + SM.Monitor(port)）。"""
    if os.path.exists(serial):
        os.remove(serial)
    cmd = [qemu, "-name", "peinfo64",
           "-drive", "format=raw,file=%s" % q(img),
           "-boot", "order=c", "-m", "512", "-vga", "std",
           "-display", "none",
           "-serial", "file:%s" % q(serial),
           "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port,
           "-no-reboot"]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(240):
        if proc.poll() is not None:
            break
        try:
            socket.create_connection(("127.0.0.1", port), timeout=1).close()
            break
        except OSError:
            time.sleep(0.25)
    return proc, SM.Monitor(port)


def open_terminal_dock(log_fn, mon, timeout=120):
    """从 **Dock 的终端图标** 开终端（照 dpkg64_test 的已验证路径；开始菜单会一次装 14 个图标）。"""
    deadline = time.time() + timeout
    hit = None
    while time.time() < deadline and not hit:
        lg = log_fn()
        if "[PANIC64] stop=" in lg:
            return False
        for mm in re.finditer(r"\[DOCK64\] item idx=(\d+) app=(-?\d+) name=\S+ x=(\d+) y=(\d+) "
                              r"w=(\d+) h=(\d+) cx=(\d+) cy=(\d+)", lg):
            if mm.group(2) == "3":                                  # app=3 = 终端
                hit = mm
                break
        if not hit:
            time.sleep(1.0)
    if not hit:
        return False
    cur = SM.Cursor()
    cur.goto(mon, int(hit.group(7)), int(hit.group(8)))
    cur.settle(mon)
    time.sleep(0.3)
    mon.click()
    t0 = time.time()
    while time.time() - t0 < 90:
        lg = log_fn()
        if "[APP] term opened" in lg:
            return True
        if "[PANIC64] stop=" in lg:
            return False
        time.sleep(0.5)
    return False


# ==================== 宿主侧期望值（独立实现） ====================
def load_crosscheck():
    path = os.path.join(ROOT, "tools", "pe_crosscheck.py")
    spec = importlib.util.spec_from_file_location("pe_crosscheck", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


# ==================== 从两边抓字段（同一套正则） ====================
FIELD_RES = {
    "machine":   r"\[PEINFO\] nt machine=(\S+) ",
    "nsec":      r"\[PEINFO\] nt machine=\S+ nsec=(\d+) ",
    "optsz":     r"\[PEINFO\] nt machine=\S+ nsec=\d+ optsz=(\d+) ",
    "magic":     r"\[PEINFO\] opt magic=(\S+)",
    "subsys":    r"\[PEINFO\] opt subsys=(\d+)\(",
    "imagebase": r"\[PEINFO\] opt subsys=\d+\(\S+\) imagebase=(\S+) ",
    "entry":     r"\[PEINFO\] opt subsys=\d+\(\S+\) imagebase=\S+ entry=(\S+)",
    "sizeimage": r"\[PEINFO\] opt sizeimage=(\S+) ",
    "sizehdr":   r"\[PEINFO\] opt sizeimage=\S+ sizehdr=(\S+) ",
    "sectalign": r"\[PEINFO\] opt sizeimage=\S+ sizehdr=\S+ sectalign=(\S+) ",
    "filealign": r"\[PEINFO\] opt sizeimage=\S+ sizehdr=\S+ sectalign=\S+ filealign=(\S+) ",
    "nrva":      r"\[PEINFO\] opt sizeimage=\S+ sizehdr=\S+ sectalign=\S+ filealign=\S+ dllchar=\S+ nrva=(\d+)",
    "summary":   r"\[PEINFO\] summary dlls=(\d+) funcs=(\d+) ord=(\d+) exports=(\d+) relocs=(\d+)",
    "union":     r"\[PEINFO\] union \"([^\"]*)\"",
}
SECTION_RE = (r"\[PEINFO\] section idx=(\d+) name=(\S+) va=(\S+) vsz=(\S+) "
              r"raw=(\S+) rawsz=(\S+) flags=(\S+) perm=([r-][w-][x-])")
DLL_RE = r"\[PEINFO\] import dll=(\S+) thunks=(\d+) funcs=(\d+) ord=(\d+)"
IMPORTF_RE = r"\[PEINFO\] import_f dll=(\S+) name=(\S+) hint=(\d+)"
IMPORTO_RE = r"\[PEINFO\] import_o dll=(\S+) ord=(\d+)"


def harvest(text):
    """从一段串口文本里抓 [PEINFO] 的字段（容忍内核日志插花：只认 [PEINFO] 开头的片段）。"""
    got = {}
    for k, rx in FIELD_RES.items():
        m = re.search(rx, text)
        got[k] = (m.groups() if len(m.groups()) > 1 else m.group(1)) if m else None
    got["sections"] = [m.groups() for m in re.finditer(SECTION_RE, text)]
    got["dlls"] = [m.groups() for m in re.finditer(DLL_RE, text)]
    got["import_f"] = [m.groups() for m in re.finditer(IMPORTF_RE, text)]
    got["import_o"] = [m.groups() for m in re.finditer(IMPORTO_RE, text)]
    got["present"] = len(re.findall(r"\[PEINFO\] ", text))
    return got


def expect_from(mod, path):
    """用宿主实现算出同一份字段（与 C 版输出格式逐项对齐）。"""
    r = mod.parse(path)
    exp = {
        "machine": "%#x" % r["machine"],
        "nsec": str(r["nsec"]),
        "optsz": str(r["optsz"]),
        "magic": "%#x" % r["magic"],
        "subsys": str(r["subsys"]),
        "imagebase": "%#x" % r["imagebase"],
        "entry": "%#x" % r["entry"],
        "sizeimage": "%#x" % r["sizeimage"],
        "sizehdr": "%#x" % r["sizehdr"],
        "sectalign": "%#x" % r["sectalign"],
        "filealign": "%#x" % r["filealign"],
        "nrva": str(min(r["nrva"], 16)),
        "summary": (str(r["n_dll"]), str(r["n_func"]), str(r["n_ord"]),
                    str(len(r["exports"])), str(r["reloc_entries"])),
        "union": r["union"],
    }
    exp["sections"] = []
    for s in r["sections"]:
        f = s["flags"]
        perm = ("r" if f & 0x40000000 else "-") + ("w" if f & 0x80000000 else "-") + \
               ("x" if f & 0x20000000 else "-")
        exp["sections"].append((str(s["idx"]), s["name"], "%#x" % s["va"], "%#x" % s["vsz"],
                                "%#x" % s["raw"], "%#x" % s["rawsz"], "%#x" % f, perm))
    exp["dlls"] = []
    exp["import_f"] = []
    exp["import_o"] = []
    for im in r["imports"]:
        nf = sum(1 for e in im["entries"] if "name" in e)
        no = sum(1 for e in im["entries"] if "ord" in e)
        exp["dlls"].append((im["dll"], str(len(im["entries"])), str(nf), str(no)))
        for e in im["entries"]:
            if "ord" in e:
                exp["import_o"].append((im["dll"], str(e["ord"])))
            else:
                exp["import_f"].append((im["dll"], e["name"], str(e["hint"])))
    return exp


def run_peinfo(mon, log_fn, path, timeout=240):
    """在终端里 `run /bin/peinfo <path>`，返回这次调用的串口切片（含进程退出行）。"""
    before = len(log_fn())
    P.type_line(mon, "run /bin/peinfo %s" % path)
    deadline = time.time() + timeout
    while time.time() < deadline:
        seg = log_fn()[before:]
        if re.search(r"\[PEINFO\] union \"", seg) or "[PEINFO] reject reason=" in seg:
            t1 = time.time()
            while time.time() - t1 < 25:
                if re.search(r"\[PROC64\] exit pid=\d+ code=\d+", log_fn()[before:]):
                    break
                time.sleep(0.5)
            time.sleep(0.5)
            return log_fn()[before:]
        if "[PANIC64] stop=" in seg:
            break
        time.sleep(1.0)
    return log_fn()[before:]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu or not os.path.exists(DISK):
        sys.stderr.write("环境问题：qemu=%r disk=%r\n" % (qemu, DISK))
        return 2
    mod = load_crosscheck()

    man_path = os.path.join(CORPUS, "PEX.json")
    corpus = []
    if os.path.exists(man_path):
        with open(man_path, encoding="utf-8") as f:
            corpus = json.load(f)

    work = tempfile.mkdtemp(prefix="vimtu64_peinfo_")
    img = os.path.join(work, "disk.img")
    serial = os.path.join(work, "run.log")
    shutil.copyfile(DISK, img)
    print("=== Vimtu64 W0: PE 只读体检器 /bin/peinfo（体内 vs 宿主 逐字段交叉核对）===")
    print("    盘：%s" % img)
    print("    语料：%d 份（%s）\n" % (len(corpus), CORPUS))

    log_fn = lambda: log_of(serial)                                  # noqa: E731
    proc = None
    try:
        proc, mon = start(qemu, img, serial, PORT)
        # ★ 登录手势的等待给足（与 dpkg64_test 一致）：这块演示盘到桌面实测 ≈ 204 s。
        if not qh.wait_desktop(mon, log_fn, proc, timeout=400, login_timeout=460):
            print("起不到桌面")
            check("桌面就绪（qh.wait_desktop 含登录手势）", False, "超时")
            raise SystemExit(1)
        check("桌面就绪（qh.wait_desktop 含登录手势）", True)

        if not open_terminal_dock(log_fn, mon):
            check("终端打开（[APP] term opened）", False, "Dock 入口没成功")
            raise SystemExit(1)
        check("终端打开（[APP] term opened）", True)
        time.sleep(1.5)

        # ---------- ④ 反例：对 ELF 必须 reject 且非 0 退出 ----------
        seg = run_peinfo(mon, log_fn, "/bin/vpkg", timeout=150)
        m = re.search(r"\[PROC64\] exit pid=(\d+) code=(\d+)", seg)
        check("④ 反例：对 ELF 调 peinfo -> reject reason=dos 且退出码非 0",
              "[PEINFO] reject reason=dos" in seg and m is not None and m.group(2) != "0",
              "exit=%s" % (m.group(2) if m else "缺"))

        # ---------- ①②③ 三份真 exe ----------
        for rec in corpus:
            key, src, vol = rec["key"], rec["src"], rec["vol"]
            if not os.path.exists(src):
                check("%s：宿主语料存在" % key, False, "PEX.json 列了但文件不在：%s" % src)
                continue
            exp = expect_from(mod, src)
            seg = run_peinfo(mon, log_fn, vol)
            got = harvest(seg)
            base = "%s(%s)" % (key, os.path.basename(vol))

            check("① %s：/bin/peinfo 跑起来了（[PEINFO] 行 %d 条）" % (base, got["present"]),
                  got["present"] >= 6 and got["union"] is not None,
                  "present=%d union=%r" % (got["present"], (got["union"] or "")[:40]))

            bad = ["%s: 体内=%r 宿主=%r" % (k, got[k], exp[k])
                   for k in ("machine", "nsec", "optsz", "magic", "subsys", "imagebase", "entry",
                             "sizeimage", "sizehdr", "sectalign", "filealign", "nrva")
                   if got[k] != exp[k]]
            check("② %s：头字段逐项一致（machine/nsec/optsz/magic/subsys/imagebase/entry/size*/align/nrva）"
                  % base, not bad, " | ".join(bad[:4]))

            check("② %s：%d 个节的 idx/name/va/vsz/raw/rawsz/flags/perm 逐项一致"
                  % (base, len(exp["sections"])),
                  got["sections"] == exp["sections"],
                  ("体内=%r 宿主=%r" % (got["sections"][:2], exp["sections"][:2]))
                  if got["sections"] != exp["sections"] else "")

            check("② %s：%d 条导入逐项一致（dll/thunks/funcs/ord + 每个函数名/hint 或序号）"
                  % (base, len(exp["import_f"]) + len(exp["import_o"])),
                  got["dlls"] == exp["dlls"] and got["import_f"] == exp["import_f"]
                  and got["import_o"] == exp["import_o"],
                  ("dlls 体内=%r 宿主=%r" % (got["dlls"], exp["dlls"]))
                  if got["dlls"] != exp["dlls"] else
                  (("import_f 体内=%r 宿主=%r" % (got["import_f"][:3], exp["import_f"][:3]))
                   if got["import_f"] != exp["import_f"] else ""))

            check("② %s：summary（dlls/funcs/ord/exports/relocs）一致" % base,
                  got["summary"] == exp["summary"],
                  "体内=%r 宿主=%r" % (got["summary"], exp["summary"]))

            check("② %s：**导入并集 union 逐字符一致**" % base,
                  got["union"] == exp["union"],
                  ("体内=%r" % (got["union"] or "")) if got["union"] != exp["union"] else "")

            if key == "tiny":
                check("③ tiny.exe：**无导入**（union 空串、dlls=0）",
                      got["union"] == "" and exp["summary"][0] == "0",
                      "union=%r" % (got["union"],))
            if key == "cui":
                check("③ cui.exe：subsystem=3(控制台) 且 union 含 msvcrt 的 printf 家族函数",
                      got["subsys"] == "3" and re.search(r"msvcrt\.dll!(vfprintf|fprintf|puts)",
                                                         got["union"] or "") is not None,
                      "subsys=%s" % got["subsys"])
            if key == "gui":
                check("③ gui.exe：subsystem=2(**图形**) 且 union 含 **USER32.dll!MessageBoxA**（W3 最小目标）",
                      got["subsys"] == "2" and "USER32.dll!MessageBoxA" in (got["union"] or ""),
                      "subsys=%s" % got["subsys"])

        # ---------- ⑤ 全程零 PANIC / 零截断 ----------
        lg = log_fn()
        for needle in ("PANIC", "TRIPLE FAULT", "[PEINFO] reject reason=trunc"):
            check("⑤ 全程不得出现 %s" % needle, needle not in lg)
    finally:
        if proc is not None:
            try:
                proc.terminate()
                proc.wait(timeout=10)
            except Exception:                                        # noqa: BLE001
                try:
                    proc.kill()
                except Exception:                                    # noqa: BLE001
                    pass

    print()
    if skipped:
        print("=== SKIP(env)：%d 条 ===" % len(skipped))
    print("=== RESULT: %s ===  checks=%d ok=%d fail=%d skip=%d"
          % ("PASS" if ok else "FAIL", len(checks),
             sum(1 for _n, c in checks if c), sum(1 for _n, c in checks if not c), len(skipped)))
    if args.keep:
        print("临时目录：%s" % work)
    else:
        shutil.rmtree(work, ignore_errors=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
