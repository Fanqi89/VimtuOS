#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/peload64_test.py - ★ v0.9.44（W1）：PE 装载器 `/bin/peload` 的端到端验收

**这一条要证明的事**（每条都能独立证伪）：

  ① `run /bin/peload /pexcorpus/tiny.exe` -> `[PELOAD] ok … entry_ret=0`
     —— 无导入、无重定位的 PE 能被装进 ring3 地址空间、跳进入口、**拿回返回值**。
        （tiny 的 `pe_entry` 是 `xor eax,eax; ret`，返回 0。光是"没崩"不算证据。）

  ② `run /bin/peload /pexcorpus/reloc.exe` -> **`entry_ret=42`**
     —— 这是**重定位真的做对了**的自证：`reloc.exe` 的 `.data` 里存着一个绝对指针
        `g_self = &g_self`（对应一条 `IMAGE_REL_BASED_DIR64`）。装载器若没把它 += delta，
        入口会返回 1；做对了才返回 42。

  ③ ② 的 `[PELOAD] map … delta=0x…` 必须 **delta != 0**，且 `[PELOAD] reloc … applied > 0`
     —— 证明"不是碰巧装在同一基址、重定位其实是 no-op"。mingw 的 ImageBase = 0x140000000（5GiB），
        而我们刻意 mmap 在 4GiB 附近，所以 delta 一定非 0。

  ④ ④ `run /bin/peload /pexcorpus/exit.exe` -> `[PELOAD] exit-stub code=7` 且进程以 **7** 退出
     —— 这是"**在 Ring 3 填一个假的 ExitProcess 导入**"的端到端证据：
        PE 里 `ExitProcess(7)` 走的是 `call [__imp_ExitProcess]`（Win64 约定，参数在 rcx），
        我们 IAT 里填的是 `peload_stubs.S` 的跳板（`movq %rcx,%rdi` 再 call C 实现），
        桩真做 exit_group(7)。**如果跳板写错（直接填 C 函数地址），退出码会是随机垃圾** ——
        所以这一条同时也是 Win64->SysV 参数搬运的判据。

  ⑤ `run /bin/peload /pexcorpus/cui.exe` -> **如实失败**：`[PELOAD] import MISSING …`、
     非 0 退出（不给 W1 假装能跑）。W2 就从这些 MISSING 行拿到实测缺口清单。

  ⑥ 反例：`run /bin/peload /bin/vpkg`（ELF）-> `[PELOAD] reject reason=dos` 且非 0 退出。

  ⑦ `.text` 的权限收紧成 `r-x`（W^X）：`[PELOAD] prot … perm=r-x`
     —— 先 mmap 成 RW 写内容、再 mprotect 收紧；不是"干脆 RWX 糊过去"。

  ⑧ 全程不得出现 PANIC / TRIPLE FAULT / `[PELOAD] reject reason=trunc`。

桌面/登录/终端一律复用已验证的公共手段（`qemuhelp.wait_desktop` 带登录、Dock 终端入口），
**不要**自己发明"等 [GUI64] ready"——那块演示盘要跑完整套自检，实测到桌面 ≈ 204 s，
且盘里有 `ui.login.last` 时还要过锁屏。

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
    """从 **Dock 的终端图标** 开终端（照 peinfo64_test 的已验证路径）。"""
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

    exe = {k: os.path.join(CORPUS, "%s.exe" % k)
           for k in ("tiny", "reloc", "exit", "cui", "gui")}
    have = {k: os.path.exists(p) for k, p in exe.items()}

    work = tempfile.mkdtemp(prefix="vimtu64_peload_")
    img = os.path.join(work, "disk.img")
    serial = os.path.join(work, "run.log")
    shutil.copyfile(DISK, img)
    print("=== Vimtu64 W1：PE 装载器 /bin/peload（ring3 装载 + DIR64 重定位 + IAT 桩 + 跳入口）===")
    print("    盘：%s" % img)
    print("    语料：%s" % ", ".join("%s=%s" % (k, "有" if v else "缺") for k, v in have.items()))
    print()

    log_fn = lambda: log_of(serial)                                  # noqa: E731
    proc = None
    try:
        proc, mon = start(qemu, img, serial, PORT)
        # ★ 登录手势的等待给足（与 dpkg64_test / peinfo64_test 一致）：这块演示盘到桌面实测 ≈ 204 s。
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

        # ---------- ⑥ 反例：对 ELF 必须 reject 且非 0 退出 ----------
        seg = run_peload(mon, log_fn, "/bin/vpkg", timeout=150)
        code = m1(EXIT_RE, seg)
        check("⑥ 反例：对 ELF（/bin/vpkg）装载 -> reject reason=dos 且退出码非 0",
              "[PELOAD] reject reason=dos" in seg and code is not None and code != "0",
              "exit=%s" % code)

        # ---------- ① tiny.exe：无导入、无重定位 ----------
        if not have["tiny"]:
            check("① tiny.exe：无导入无重定位仍能跳进入口（entry_ret=0）", False,
                  "语料缺失 build64/pexcorpus/tiny.exe", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/tiny.exe")
            okm = re.search(OK_RE, seg)
            code = m1(EXIT_RE, seg)
            check("① tiny：/bin/peload 跑起来了（[PELOAD] ok 行 + 进程退出 0）",
                  okm is not None and code == "0",
                  "ok=%r exit=%s" % (okm.groups() if okm else None, code))
            check("① tiny：**跳进入口并拿回返回值 entry_ret=0**（pe_entry 是 xor eax,eax; ret）",
                  okm is not None and okm.group(2) == "0",
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
            check("② reloc：**entry_ret=42**（.data 里的绝对指针被 DIR64 重定位修正 = 自证）",
                  okm is not None and okm.group(2) == "42",
                  "entry_ret=%s（1 = 重定位没做）" % (okm.group(2) if okm else "缺"))
            check("③ reloc：**base 不在 ImageBase 上（delta != 0）**",
                  mp is not None and mp.group(4) not in ("0x0", "0"),
                  "delta=%s base=%s imagebase=%s" % ((mp.group(4), mp.group(1), mp.group(3))
                                                     if mp else ("缺", "缺", "缺")))
            check("③ reloc：**applied > 0**（DIR64 项真的被加过 delta，不是 no-op）",
                  rl is not None and int(rl.group(3)) > 0,
                  "blocks=%s entries=%s applied=%s types=%s skipped=%s"
                  % (rl.groups() if rl else ("缺",) * 5))
            check("③ reloc：重定位项类型就是 **0xa (IMAGE_REL_BASED_DIR64)**",
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
            check("④ exit：**IAT 里填进了 ExitProcess**（import fill 行, dll=KERNEL32.dll）",
                  filled is not None and filled.group(2) == "ExitProcess"
                  and filled.group(1).lower() == "kernel32.dll",
                  "fill=%r" % ((filled.groups() if filled else None),))
            check("④ exit：PE 调 ExitProcess(7) 走到我们的桩（[PELOAD] exit-stub code=7）",
                  st == "7", "code=%s" % st)
            check("④ exit：**进程真的以 7 退出**（Win64 rcx -> SysV rdi 的跳板搬对了参数）",
                  code == "7", "exit=%s（非 7 = 参数串位）" % code)
            has_ok = "[PELOAD] ok " in seg
            check("④ exit：不许打 [PELOAD] ok（PE 是 ExitProcess 走的，不是 ret 回来的）",
                  not has_ok, "出现了 ok 行（说明 PE 是 ret 回来的，不是 ExitProcess 走的）"
                  if has_ok else "无 ok 行（正确）")

        # ---------- ⑤ cui.exe：如实失败（不假装能跑） ----------
        if not have["cui"]:
            check("⑤ cui.exe：缺导入必须如实失败", False, "语料缺失", skip_env=True)
        else:
            seg = run_peload(mon, log_fn, "/pexcorpus/cui.exe")
            miss = re.findall(r"\[PELOAD\] import MISSING dll=(\S+) name=(\S+)", seg)
            code = m1(EXIT_RE, seg)
            mm = re.search(r"\[PELOAD\] fail reason=missing-imports count=(\d+)", seg)
            check("⑤ cui：**如实报缺哪些导入**（[PELOAD] import MISSING 行 ≥ 1）",
                  len(miss) >= 1, "MISSING=%d 例：%s" % (len(miss), miss[:3]))
            check("⑤ cui：整体如实失败（fail reason=missing-imports + 退出码非 0）",
                  mm is not None and code is not None and code != "0",
                  "count=%s exit=%s" % (mm.group(1) if mm else "缺", code))
            check("⑤ cui：**不许带着空 IAT 跳进去**（不得出现 [PELOAD] enter / ok）",
                  "[PELOAD] enter " not in seg and "[PELOAD] ok " not in seg,
                  "enter=%s ok=%s" % ("[PELOAD] enter " in seg, "[PELOAD] ok " in seg))
            check("⑤ cui：缺口清单里必须有 msvcrt 的 CRT 函数（W2 的输入）",
                  any(d.lower() == "msvcrt.dll" for d, _n in miss),
                  "dlls=%s" % sorted({d.lower() for d, _n in miss}))

        # ---------- ⑦ W^X：.text 收紧成 r-x ----------
        if have["reloc"]:
            # reloc 已经跑过；重新跑一遍只为拿 prot 行（成本很低，且证据更干净）
            seg = run_peload(mon, log_fn, "/pexcorpus/reloc.exe", timeout=180)
            prots = [(g[1], g[2]) for g in re.findall(PROT_RE, seg)]
            text_perm = [perm for name, perm in prots if name == ".text"]
            check("⑦ W^X：.text 的权限被收成 **r-x**（先 RW 写内容、再 mprotect 收紧）",
                  bool(text_perm) and text_perm[0] == "r-x",
                  "prot=%r" % (prots[:6],))

        # ---------- ⑧ 全程零 PANIC / 零截断 ----------
        lg = log_fn()
        for needle in ("PANIC", "TRIPLE FAULT", "[PELOAD] reject reason=trunc"):
            check("⑧ 全程不得出现 %s" % needle, needle not in lg)
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
