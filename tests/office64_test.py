# -*- coding: utf-8 -*-
"""tests/office64_test.py —— v0.5.3：/bin/office（ring3 三合一办公套件）端到端验收

判据（串口打点 + 屏幕像素 + **宿主侧独立解析卷**）：
  1) 窗口建成：`[OFFICE] win surf=.. w=896 h=580 shm=.. map=0`
  2) 三个模式都加载了：`[OFFICE] seek mode=doc|sheet|slides …`
  3) 文档打字：键盘输入后 `[OFFICE] key code=.. mode=0 line=.. col=..`，且光标真的动了
  4) 存盘：Ctrl+S -> `[OFFICE] save mode=doc path=/tmp/doc.vwd bytes=.. rc=<>=0>`；
     **宿主侧独立解析卷**读 /tmp/doc.vwd，内容必须包含刚敲进去的标记串（证明真落盘）
  5) 表格：切 F2，输入 `=1+2*3` -> `[OFFICE] sheet set cell=A1 expr=… value=7 err=0`
  6) 屏幕：编辑区有墨迹（不是纯色块）
  7) ESC 退出：`[OFFICE] done frames=.. reason=esc`
  8) 全程无 PANIC / TRIPLE FAULT

为什么走 ring3 shell 的 `run`：office 用 shm/wl_surface（都要**进程上下文**），终端内建 run 没有
（这条已知边界见 README 第二节）。

用法：py -3 tests/office64_test.py [--port 5676]
退出码：0 全过 / 1 有断言失败 / 2 环境问题（QEMU、镜像）。
"""
import argparse
import importlib.util
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import textstress64_test as T                                    # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SYS_DISK = os.path.join(ROOT, "build64", "sysdisk.img")
PART_MAIN_LBA = 8009
SECTOR = 512
outdir = os.path.join(os.environ.get("PI_SCRATCH_DIR", ROOT), "office")
os.makedirs(outdir, exist_ok=True)
checks = []
OW, OH = 896, 580
MARK = "ZZ-OFFICE-MARK"          # 敲进文档的标记串（宿主侧读回来核对）


def check(name, cond, detail=""):
    checks.append((name, bool(cond)))
    print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""), flush=True)


def load_mod(name, fname):
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def guest_file(parts):
    """宿主侧独立解析卷（QEMU 停掉之后读）：证明文件真的落盘了。"""
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(SYS_DISK, "rb").read()
    off = PART_MAIN_LBA * SECTOR
    total = int.from_bytes(d[off + 20:off + 24], "little")
    vol = d[off:off + total * SECTOR]
    inodes = int.from_bytes(vol[40:44], "little")
    cur = 0
    rec = None
    for part in parts:
        hit = None
        for i, nm, r in TP._entries(vol, cur, inodes):
            if nm == part:
                hit = (i, r)
                break
        if not hit:
            return None
        cur, rec = hit
    return TP._read_file(vol, rec, total)


KEYMAP = {" ": "spc", "/": "slash", ".": "dot", "-": "minus", "=": "equal",
          "_": "shift-minus", ">": "shift-dot", "|": "shift-backslash",
          "'": "apostrophe", ":": "shift-semicolon",
          "+": "shift-equal", "*": "shift-8", "(": "shift-9", ")": "shift-0"}


def key_for(ch):
    """QEMU sendkey 的口径：大写字母 = shift-<小写>；符号有的也要 shift。"""
    if ch in KEYMAP:
        return KEYMAP[ch]
    if "A" <= ch <= "Z":
        return "shift-" + ch.lower()
    return ch


def type_line(mon, s, per=0.06):
    for ch in s:
        mon.key(key_for(ch), wait=per)


def dark_pixels(a, x0, y0, x1, y1, step=2):
    dark = 0
    total = 0
    for y in range(y0, y1, step):
        for x in range(x0, x1, step):
            px = a[y][x]
            total += 1
            if int(px[0]) < 120 and int(px[1]) < 120 and int(px[2]) < 120:
                dark += 1
    return dark, total


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--img", default=SYS_DISK)
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--port", type=int, default=5676)
    args = ap.parse_args()

    qemu = T.find_qemu(args.qemu) if hasattr(T, "find_qemu") else args.qemu
    if not qemu or not os.path.exists(args.img):
        print("跳过：qemu 或镜像不可用（%s）" % args.img)
        return 2

    print("=== office64_test：/bin/office 三合一（文档打字+存盘 / 表格公式 / 演示） ===")
    vm = T.Vm(qemu, args.img, args.port, "office")
    try:
        mon = vm.monitor()
        if not mon:
            check("连上 monitor", False)
            return 1
        if not vm.wait_log(T.LOCK_INTERACTIVE, 240):
            check("等锁屏", False)
            return 1
        mon.key("ret", 1.4)
        mon.key("ret", 1.4)
        if not vm.wait_log(T.DESKTOP_READY, 180):
            check("进桌面", False)
            return 1
        time.sleep(2.0)
        mon.key("meta_l", 1.2)
        mon.key("1", 2.0)
        check("开场终端（[APP] term opened）", vm.wait_log("[APP] term opened", 25))
        time.sleep(1.2)

        n0 = len(vm.log())
        type_line(mon, "shell")
        mon.key("ret", wait=2.0)
        got_sh = vm.wait_log("[SH64] launch", 60, since=n0)
        check("起 ring3 shell", got_sh)
        if not got_sh:
            print("\n=== RESULT: UNVERIFIED ===（ring3 shell 起不来）")
            return 2
        time.sleep(2.5)

        n1 = len(vm.log())
        ok_win = False
        for attempt in range(3):
            type_line(mon, "run /bin/office")
            mon.key("ret", wait=2.0)
            ok_win = vm.wait_log("[OFFICE] win", 45, since=n1)
            if ok_win:
                break
            print("  [info] 第 %d 次 run 没被 shell 接住，重发" % (attempt + 1))
            time.sleep(1.5)
        vm.wait_log("[OFFICE] render", 30, since=n1)
        time.sleep(1.0)
        log = vm.log()[n1:]
        m_win = re.search(r"\[OFFICE\] win surf=(-?\d+) w=(\d+) h=(\d+) shm=(-?\d+) map=(-?\d+)", log)
        check("窗口建成（w=896 h=580 map=0）",
              bool(m_win and m_win.group(2) == str(OW) and m_win.group(3) == str(OH)
                   and m_win.group(5) == "0"),
              m_win.group(0) if m_win else "（缺）")
        if not m_win or m_win.group(5) != "0":
            print("\n=== RESULT: FAIL === 窗口没建成")
            return 1
        check("三个模式都加载了（seek mode=doc/sheet/slides）",
              all(("[OFFICE] seek mode=%s" % k) in log for k in ("doc", "sheet", "slides")),
              " | ".join(re.findall(r"\[OFFICE\] seek mode=(\w+)", log)))
        m_doc = re.search(r"\[OFFICE\] doc lines=(\d+) bytes=(\d+)", log)
        check("文档加载（lines>=3 bytes>=50）",
              bool(m_doc and int(m_doc.group(1)) >= 3 and int(m_doc.group(2)) >= 50),
              m_doc.group(0) if m_doc else "（缺）")

        # ---- 文档：敲标记串（会插在光标处，第 1 行行首）----
        n2 = len(vm.log())
        type_line(mon, MARK, per=0.12)
        time.sleep(2.0)
        got_key = vm.wait_log("[OFFICE] key", 15, since=n2)
        m_key = None
        for mm in re.finditer(r"\[OFFICE\] key code=(\d+) mode=0 line=(\d+) col=(\d+)", vm.log()[n2:]):
            m_key = mm
        check("打字有打点且光标真的动了（[OFFICE] key … line/col）",
              bool(got_key and m_key and int(m_key.group(3)) >= len(MARK) - 1),
              m_key.group(0) if m_key else "（缺）")

        # ---- 屏幕：文档编辑区有正文墨迹（此刻还在文档模式）----
        m_pos = None
        for mm in re.finditer(r"\[WL64\] surface create id=(\d+) w=(\d+) h=(\d+)[^\r\n]*pos=(-?\d+),(-?\d+)", vm.log()):
            if mm.group(2) == str(OW) and mm.group(3) == str(OH):
                m_pos = mm
        if m_pos:
            wx, wy = int(m_pos.group(4)), int(m_pos.group(5))
            time.sleep(1.0)
            f = mon.shot(os.path.join(outdir, "office_doc.ppm"))
            a = T.frame_arr(f)
            dark, total = dark_pixels(a, wx + 12, wy + 34, wx + OW - 12, wy + OH - 28)
            print("  [info] 文档区暗像素 %d/%d = %.2f%%" % (dark, total, 100.0 * dark / max(1, total)))
            check("编辑区有正文墨迹（暗像素 >= 0.2%）", dark * 500 >= total, "%d/%d" % (dark, total))
        else:
            check("拿到窗口位置", False)

        # ---- F2 切到表格：切走前自动保存文档（内核只特判 F2=0xF9，所以用 F2 循环切模式）----
        n3 = len(vm.log())
        mon.key("f2", wait=2.0)
        check("F2 切到表格（[OFFICE] mode=1）", vm.wait_log("[OFFICE] mode=1", 20, since=n3))
        m_save = re.search(r"\[OFFICE\] save mode=doc path=(\S+) bytes=(\d+) rc=(-?\d+)", vm.log()[n3:])
        ok_save = bool(m_save and int(m_save.group(3)) >= 0 and int(m_save.group(2)) > 0)
        check("切模式时把文档落盘（[OFFICE] save mode=doc … rc>=0）", ok_save,
              m_save.group(0) if m_save else "（缺）")

        # ---- 表格：清空 A1（默认样例是 10），输入 =1+2*3 ----
        for _ in range(16):
            mon.key("backspace", wait=0.08)
        n5 = len(vm.log())
        type_line(mon, "=1+2*3", per=0.10)
        time.sleep(1.5)
        m_set = None
        for mm in re.finditer(r"\[OFFICE\] sheet set cell=(\w+) expr=(\S*) value=(\S+) err=(-?\d+)", vm.log()[n5:]):
            m_set = mm
        check("表格公式求值（=1+2*3 -> 7、err=0）",
              bool(m_set and m_set.group(3) == "7" and m_set.group(4) == "0"),
              m_set.group(0) if m_set else "（缺）")

        # ---- 表格：环引用必须如实报 #CIRC（A1 引用自己）----
        n6 = len(vm.log())
        for _ in range(16):
            mon.key("backspace", wait=0.08)
        type_line(mon, "=A1+1", per=0.10)
        time.sleep(1.5)
        m_cyc = None
        for mm in re.finditer(r"\[OFFICE\] sheet set cell=A1 expr=(\S+) value=(\S+) err=(-?\d+)", vm.log()[n6:]):
            m_cyc = mm
        check("环引用如实报错（=A1+1 -> err=-2 / #CIRC）",
              bool(m_cyc and m_cyc.group(3) == "-2" and m_cyc.group(2) == "#CIRC"),
              m_cyc.group(0) if m_cyc else "（缺）")

        # ---- ESC 退出 ----
        n7 = len(vm.log())
        mon.key("esc", wait=2.0)
        got_done = vm.wait_log("[OFFICE] done", 20, since=n7)
        check("ESC 退出（[OFFICE] done … reason=esc）",
              bool(got_done and "reason=esc" in vm.log()[n7:]),
              (re.search(r"\[OFFICE\] done[^\r\n]*", vm.log()[n7:]).group(0) if got_done else "（缺）"))

        for bad in ("PANIC", "TRIPLE FAULT"):
            check("全程不得出现 %s" % bad, bad not in vm.log())
    finally:
        try:
            vm.close()
        except Exception:
            pass

    # ---- 宿主侧独立解析卷：/tmp/doc.vwd 必须含标记串（证明真落盘，不是内存里自说自话）----
    blob = guest_file(["tmp", "doc.vwd"])
    ok_disk = bool(blob and MARK.encode() in blob)
    check("宿主侧读卷里的 /tmp/doc.vwd 含标记串（真的落盘了）", ok_disk,
          ("%d B，含 %s" % (len(blob), MARK)) if ok_disk else
          ("读不到文件" if not blob else ("%d B，不含 %s" % (len(blob), MARK))))

    bad = [n for n, ok in checks if not ok]
    print("\n=== RESULT: %s ===  checks=%d ok=%d"
          % ("FAIL" if bad else "PASS", len(checks), len(checks) - len(bad)))
    for n in bad:
        print("  FAIL:", n)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
