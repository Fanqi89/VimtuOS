#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/peload64_test.py - ★ v0.9.44（W1）/ v0.9.45（W2）：PE 装载器 `/bin/peload` 的端到端验收

**这一条要证明的事**（每条都能独立证伪）：

  === W1 部分（装载器本体）===
  ① `run /bin/peload /pexcorpus/tiny.exe` -> `[PELOAD] ok … entry_ret=0`
     —— 无导入、无重定位的 PE 能被装进 ring3 地址空间、跳进入口、**拿回返回值**。
  ② `run /bin/peload /pexcorpus/reloc.exe` -> **`entry_ret=42`**
     —— 重定位真的做对了的自证：`.data` 里存着一个绝对指针 `g_self = &g_self`（一条 DIR64），
        装载器没加 delta 会返回 1，做对了才 42。
  ③ ② 的 `[PELOAD] map … delta=0x…` 必须 **delta != 0**，且 `[PELOAD] reloc … applied > 0`
     —— 证明"不是碰巧装在同一基址、重定位其实是 no-op"。
  ④ `exit.exe`（只导入 `kernel32!ExitProcess`）-> `exit-stub code=7` 且进程以 **7** 退出
     —— **Ring 3 填一个假的 ExitProcess 导入**的端到端证据；同时是 Win64->SysV 跳板的判据
        （跳板写错 → 参数串位 → 退出码随机）。
  ⑤ 反例：`run /bin/peload /bin/vpkg`（ELF）-> `[PELOAD] reject reason=dos` 且非 0 退出。
  ⑥ `.text` 的权限收紧成 `r-x`（W^X）：先 mmap 成 RW 写内容、再 mprotect 收紧。
  ⑦ 全程零 PANIC / 零 TRIPLE FAULT / 零 `reject reason=trunc`。

  === W2 部分（TEB/PEB + GS.base + Win32/msvcrt 桩）===
  ⑧ `run /bin/peload /pexcorpus/gsprobe.exe` -> **`entry_ret=42`**
     —— GS.base / TEB / PEB 的**自证**：该 exe 是纯汇编，读 `%gs:0x30`（TEB 自身）与 `%gs:0x60`（PEB），
        检查 TEB+0x30 指回自己、PEB 非 0、PEB+0x10（ImageBaseAddress）处是 'MZ'、
        且 ImageBaseAddress+0x1000 == 它自己入口的运行时地址。四条全对才返回 42。
        同时 `[PELOAD] teb …` 行必须显示 **gs_set_rc=0 / gs_read_rc=0 / gs_read == base**
        —— 即内核私有 syscall 1037/1038 真的写进了 IA32_GS_BASE 并读得回来。
  ⑨ `run /bin/peload /pexcorpus/cui.exe` -> **CRT 真的跑起来**
     —— 终端里出现 **"hello from a real Windows console exe"**（那是 msvcrt 的 `vfprintf`
        走我们的桩写进 VimtuOS 终端的结果），进程退出 **0**，且 **零 `import MISSING`**
        （28 个导入全部命中）。
        ★ 注意：带 CRT 的 exe **不打 `[PELOAD] ok`** —— CRT 的 main 返回后走 `exit(ret)`，
          也就是我们的 exit 桩，与 `exit.exe` 同一条语义。判据要求"无 ok 行"。
  ⑩ `run /bin/peload /pexcorpus/gui.exe` -> **仍如实失败**：缺 `USER32.dll!MessageBoxA`
     —— W2 的边界要写清：**图形窗口是 W3 的活**。不许为了让测试变绿而假装加载 user32。

语料由 `tools/pex_corpus_build.py` 用 mingw-w64 现场编（不手搓），共 6 份：
tiny / reloc / exit / gsprobe / cui / gui。

桌面/登录/终端一律复用已验证的公共手段（`qemuhelp.wait_desktop` 带登录、Dock 终端入口），
**不要**自己发明"等 [GUI64] ready"——那块演示盘要跑完整套自检，实测到桌面 ≈ 204 s。

用法：py -3 tests/peload64_test.py [--qemu PATH] [--keep]
退出码：0 = 全过（含 SKIP）；1 = 有断言失败；2 = 环境问题（缺盘 / 缺 QEMU）。
"""
import argparse
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
PORT = 5674
HELLO = "hello from a real Windows console exe"

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
    """起 QEMU（与 peinfo64_test 同一条形状：-serial file: + telnet monitor + SM.Monitor(port)）。"""
    if os.path.exists(serial):
        os.remove(serial)
    cmd = [qemu, "-name", "peload64",
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
    """从 Dock 的终端图标开终端（照 peinfo64_test 的已验证路径）。"""
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


# ==================== 串口切片工具 ====================
EXIT_RE = r"\[PROC64\] exit pid=\d+ code=(\d+)"
MAP_RE = r"\[PELOAD\] map base=(\S+) size=(\S+) imagebase=(\S+) delta=(\S+)"
RELOC_RE = (r"\[PELOAD\] reloc blocks=(\d+) entries=(\d+) applied=(\d+) "
            r"types=(\S+) skipped=(\d+)")
OK_RE = (r"\[PELOAD\] ok path=(\S+) entry_ret=(\d+) reloc=(\d+) import=(\d+)")
EXITSTUB_RE = r"\[PELOAD\] exit-stub code=(\d+)"
PROT_RE = r"\[PELOAD\] prot idx=(\d+) name=(\S+) perm=([r-][w-][x-])"
TEB_RE = (r"\[PELOAD\] teb base=(\S+) self=(\S+) peb=(\S+) imagebase=(\S+) "
          r"gs_set_rc=(\d+) gs_read_rc=(\d+) gs_read=(\S+) stubs=(\d+)")
MISSING_RE = r"\[PELOAD\] import MISSING dll=(\S+) (?:name=(\S+)|ord#(\d+))"
SUM_RE = r"\[PELOAD\] import summary filled=(\d+) missing=(\d+)"


def run_peload(mon, log_fn, path, timeout=240):
    """在终端里 `run /bin/peload <path>`，返回这次调用的串口切片（含进程退出行）。"""
    before = len(log_fn())
    P.type_line(mon, "run /bin/peload %s" % path)
    deadline = time.time() + timeout
    while time.time() < deadline:
        seg = log_fn()[before:]
        done = (re.search(OK_RE, seg) or "[PELOAD] reject reason=" in seg
                or "[PELOAD] fail reason=" in seg or re.search(EXITSTUB_RE, seg))
        if done:
            t1 = time.time()
            while time.time() - t1 < 25:
                if re.search(EXIT_RE, log_fn()[before:]):
                    break
                time.sleep(0.5)
            time.sleep(0.5)
            return log_fn()[before:]
        if "[PANIC64] stop=" in seg:
            break
        time.sleep(1.0)
    return log_fn()[before:]


def m1(rx, seg, grp=1):
    m = re.search(rx, seg)
    return m.group(grp) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu or not os.path.exists(DISK):
        sys.stderr.write("环境问题：qemu=%r disk=%r\n" % (qemu, DISK))
        return 2

    keys = ("tiny", "reloc", "exit", "gsprobe", "cui", "gui")
    exe = {k: os.path.join(CORPUS, "%s.exe" % k) for k in keys}
    have = {k: os.path.exists(p) for k, p in exe.items()}

    work = tempfile.mkdtemp(prefix="vimtu64_peload_")
    img = os.path.join(work, "disk.img")
    serial = os.path.join(work, "run.log")
    shutil.copyfile(DISK, img)
    print("=== Vimtu64 W1/W2：PE 装载器 /bin/peload ===")
    print("    盘：%s" % img)
    print("    语料：%s" % ", ".join("%s=%s" % (k, "有" if v else "缺") for k, v in have.items()))
    print()

    log_fn = lambda: log_of(serial)                                  # noqa: E731
    proc = None
    try:
        proc, mon = start(qemu, img, serial, PORT)
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

        # ---------- ⑤ 反例：对 ELF 必须 reject 且非 0 退出 ----------
        seg = run_peload(mon, log_fn, "/bin/vpkg", timeout=150)
        code = m1(EXIT_RE, seg)
        check("⑤ 反例：对 ELF（/bin/vpkg）装载 -> reject reason=dos 且退出码非 0",
              "[PELOAD] reject reason=dos" in seg and code is not None and code != "0",
              "exit=%s" % code)

        # ---------- ① tiny.exe ----------
        if not have["tiny"]:
            check("① tiny.exe：无导入无重定位仍能跳进入口（entry_ret=0）", False,
                  "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/tiny.exe")
            okm = re.search(OK_RE, seg)
            code = m1(EXIT_RE, seg)
            check("① tiny：/bin/peload 跑起来了（[PELOAD] ok + 进程退出 0）",
                  okm is not None and code == "0",
                  "ok=%r exit=%s" % (okm.groups() if okm else None, code))
            check("① tiny：跳进入口并拿回返回值 entry_ret=0", okm is not None and okm.group(2) == "0",
                  "entry_ret=%s" % (okm.group(2) if okm else "缺"))
            check("① tiny：无重定位（reloc=0）且无导入（import=0）",
                  okm is not None and okm.group(3) == "0" and okm.group(4) == "0",
                  "reloc=%s import=%s" % ((okm.group(3), okm.group(4)) if okm else ("缺", "缺")))

        # ---------- ②③ reloc.exe：重定位自证 ----------
        if not have["reloc"]:
            check("② reloc.exe：重定位做对了才返回 42", False, "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/reloc.exe")
            okm = re.search(OK_RE, seg)
            mp = re.search(MAP_RE, seg)
            rl = re.search(RELOC_RE, seg)
            code = m1(EXIT_RE, seg)
            check("② reloc：装起来了（[PELOAD] ok + 进程退出 0）",
                  okm is not None and code == "0",
                  "ok=%r exit=%s" % (okm.groups() if okm else None, code))
            check("② reloc：**entry_ret=42**（.data 的绝对指针被 DIR64 修正 = 自证）",
                  okm is not None and okm.group(2) == "42",
                  "entry_ret=%s（1 = 重定位没做）" % (okm.group(2) if okm else "缺"))
            check("③ reloc：base 不在 ImageBase 上（delta != 0）",
                  mp is not None and mp.group(4) not in ("0x0", "0"),
                  "delta=%s base=%s imagebase=%s" % ((mp.group(4), mp.group(1), mp.group(3))
                                                     if mp else ("缺", "缺", "缺")))
            check("③ reloc：applied > 0（DIR64 项真的被加过 delta，不是 no-op）",
                  rl is not None and int(rl.group(3)) > 0,
                  "blocks=%s entries=%s applied=%s types=%s skipped=%s"
                  % (rl.groups() if rl else ("缺",) * 5))
            check("③ reloc：重定位项类型就是 0xa (IMAGE_REL_BASED_DIR64)",
                  rl is not None and "0xa" in rl.group(4),
                  "types=%s" % (rl.group(4) if rl else "缺"))

        # ---------- ④ exit.exe：IAT 里的假 ExitProcess 桩 ----------
        if not have["exit"]:
            check("④ exit.exe：IAT 假 ExitProcess 桩生效（退出码 = PE 请求的 7）", False,
                  "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/exit.exe")
            st = m1(EXITSTUB_RE, seg)
            code = m1(EXIT_RE, seg)
            filled = re.search(r"\[PELOAD\] import fill dll=(\S+) name=(\S+) at=(\S+)", seg)
            check("④ exit：IAT 里填进了 ExitProcess（import fill 行, dll=KERNEL32.dll）",
                  filled is not None and filled.group(2) == "ExitProcess"
                  and filled.group(1).lower() == "kernel32.dll",
                  "fill=%r" % ((filled.groups() if filled else None),))
            check("④ exit：PE 调 ExitProcess(7) 走到我们的桩（exit-stub code=7）",
                  st == "7", "code=%s" % st)
            check("④ exit：进程真的以 7 退出（Win64 rcx -> SysV rdi 的跳板搬对了参数）",
                  code == "7", "exit=%s（非 7 = 参数串位）" % code)
            has_ok = "[PELOAD] ok " in seg
            check("④ exit：不许打 [PELOAD] ok（PE 是 ExitProcess 走的，不是 ret 回来的）",
                  not has_ok, "出现了 ok 行（说明 PE 是 ret 回来的，不是 ExitProcess 走的）"
                  if has_ok else "无 ok 行（正确）")

        # ---------- ⑧ gsprobe.exe：GS.base / TEB / PEB 自证（★ W2） ----------
        if not have["gsprobe"]:
            check("⑧ gsprobe.exe：GS.base/TEB/PEB 自证（entry_ret=42）", False,
                  "语料缺失（跑 py -3 tools/pex_corpus_build.py）", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/gsprobe.exe")
            okm = re.search(OK_RE, seg)
            tb = re.search(TEB_RE, seg)
            code = m1(EXIT_RE, seg)
            check("⑧ gsprobe：**entry_ret=42**（gs:[0x30]/[0x60] + PEB->ImageBase 四条检查全过）",
                  okm is not None and okm.group(2) == "42",
                  "entry_ret=%s（1=TEB 错 2=PEB 为空 3=ImageBase 对不上）"
                  % (okm.group(2) if okm else "缺"))
            check("⑧ gsprobe：[PELOAD] teb 行就位（TEB/PEB/ImageBase 都打出来了）",
                  tb is not None, "teb=%r" % ((tb.groups() if tb else None),))
            check("⑧ gsprobe：内核私有 syscall 真写进了 IA32_GS_BASE（gs_set_rc=0）",
                  tb is not None and tb.group(5) == "0",
                  "gs_set_rc=%s" % (tb.group(5) if tb else "缺"))
            check("⑧ gsprobe：读回来的 GS.base == 我们建的 TEB（gs_read==base）",
                  tb is not None and tb.group(6) == "0" and tb.group(7) == tb.group(1),
                  "base=%s gs_read=%s gs_read_rc=%s"
                  % ((tb.group(1), tb.group(7), tb.group(6)) if tb else ("缺", "缺", "缺")))
            check("⑧ gsprobe：TEB+0x30 指回 TEB 自己（self==base）",
                  tb is not None and tb.group(2) == tb.group(1),
                  "self=%s base=%s" % ((tb.group(2), tb.group(1)) if tb else ("缺", "缺")))
            check("⑧ gsprobe：进程正常退出（code=0）", code == "0", "exit=%s" % code)

        # ---------- ⑨ cui.exe：CRT 真的跑起来（★ W2 的主判据） ----------
        if not have["cui"]:
            check("⑨ cui.exe：CRT 打印它自己那一行并退出 0", False, "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/cui.exe")
            okm = re.search(OK_RE, seg)
            code = m1(EXIT_RE, seg)
            miss = re.findall(MISSING_RE, seg)
            sm = re.search(SUM_RE, seg)
            has_hello = HELLO in seg
            check("⑨ cui：**终端里出现了 exe 自己 printf 的那一行**（msvcrt vfprintf 走我们的桩）",
                  has_hello, "" if has_hello else "没看到 %r" % HELLO)
            check("⑨ cui：28 个导入全部填上（import summary missing=0）",
                  sm is not None and sm.group(2) == "0" and not miss,
                  "summary=%s MISSING=%d" % ((sm.groups() if sm else None), len(miss)))
            # ★ 与 exit.exe 同一条语义：CRT 的 main 返回后走的是 `exit(ret)`，
            #   也就是**我们的 exit 桩 -> exit_group**，所以这里**不应该**出现 [PELOAD] ok。
            #   第一版把它写成"必须有 ok 行"，是我判据写错了（不是产品坏了）。
            has_ok = "[PELOAD] ok " in seg
            check("⑨ cui：CRT 走 exit() 而不是 ret（因此无 [PELOAD] ok，与 exit.exe 同一语义）",
                  not has_ok, "出现了 ok 行（说明 CRT 是 ret 回来的，与预期不符）"
                  if has_ok else "无 ok 行（正确：exit 桩接管）")
            check("⑨ cui：PE 的 28 个导入全部命中我们的桩（没有一条落到 MISSING）",
                  sm is not None and not miss and int(sm.group(1)) == 28,
                  "filled=%s missing=%s" % ((sm.group(1), sm.group(2)) if sm else ("缺", "缺")))
            check("⑨ cui：进程退出码 0", code == "0", "exit=%s" % code)
            check("⑨ cui：[PELOAD] teb 行显示 GS.base 已生效（gs_set_rc=0）",
                  re.search(TEB_RE, seg) is not None and re.search(TEB_RE, seg).group(5) == "0",
                  "teb=%r" % ((re.search(TEB_RE, seg).groups() if re.search(TEB_RE, seg) else None),))

        # ---------- ⑩ gui.exe：仍如实失败（W2 的边界 = user32 是 W3） ----------
        if not have["gui"]:
            check("⑩ gui.exe：缺 USER32!MessageBoxA 必须如实失败", False, "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/gui.exe")
            miss = re.findall(MISSING_RE, seg)
            code = m1(EXIT_RE, seg)
            mm = re.search(r"\[PELOAD\] fail reason=missing-imports count=(\d+)", seg)
            has_msgbox = any((d.lower() == "user32.dll" and n == "MessageBoxA") for d, n, _o in miss)
            check("⑩ gui：缺口清单里必须有 USER32.dll!MessageBoxA（W3 的目标）",
                  has_msgbox, "dlls=%s" % sorted({(d.lower(), n) for d, n, _o in miss})[:6])
            check("⑩ gui：整体如实失败（fail reason=missing-imports + 退出码非 0）",
                  mm is not None and code is not None and code != "0",
                  "count=%s exit=%s" % (mm.group(1) if mm else "缺", code))
            check("⑩ gui：不许带着空 IAT 跳进去（不得出现 [PELOAD] enter / ok）",
                  "[PELOAD] enter " not in seg and "[PELOAD] ok " not in seg,
                  "enter=%s ok=%s" % ("[PELOAD] enter " in seg, "[PELOAD] ok " in seg))

        # ---------- ⑥ W^X ----------
        if have["reloc"]:
            seg = run_peload(mon, log_fn, "/pexcorpus/reloc.exe", timeout=180)
            prots = [(g[1], g[2]) for g in re.findall(PROT_RE, seg)]
            text_perm = [perm for name, perm in prots if name == ".text"]
            check("⑥ W^X：.text 的权限被收成 r-x（先 RW 写内容、再 mprotect 收紧）",
                  bool(text_perm) and text_perm[0] == "r-x",
                  "prot=%r" % (prots[:6],))

        # ---------- ⑦ 全程零 PANIC / 零截断 ----------
        lg = log_fn()
        for needle in ("PANIC", "TRIPLE FAULT", "[PELOAD] reject reason=trunc"):
            check("⑦ 全程不得出现 %s" % needle, needle not in lg)
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
