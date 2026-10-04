#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/dir64_test.py - ★ 本批：Ring 3 **实时目录枚举 ABI**（自有 ABI int 0x80 号 50/51/52）端到端验收

要证明的事（逐条对应任务书）：
  ① dir_open / dir_read / dir_close 枚举出来的东西与 shell 的 `ls`（终端服务的"代列协议"，
     内核 fd64 目录原语）**逐项一致**：名字集合、子目录 kind=dir、文件大小、mtime 合理。
  ② 同一个会话里**新建**文件后，紧接着的 ls / find / cat / du **立刻能看到**（创建前/后两次输出对比）。
  ③ **删除**之后不再出现。
  ④ 无权限目录 -> **明确错误码**（-EACCES = -13）、busybox 明确报错、**不崩**。
  ⑤ 句柄回收：句柄是**每进程 fd 表**里的槽；进程退出随 fd 表回收（不同进程/不同次调用拿到**同一个**
     slot），从不超过 fd 表上限（没有 EMFILE 累积 = 无泄漏）。
  ⑥ 兜底路径仍可用：人为关掉新 ABI（/tmp/.dir64-off）-> 用户态打点 + 退回构建期快照
     （/etc/vimtu.dirs）；快照里**没有**本次开机新建的文件（这正是对照）。
  ⑦ 全程无 PANIC / 无 triple fault；50/51/52 **不在** enosys 清单里；三类 [DIR64] 打点齐全。
  ⑧ 构建期证据：内核镜像里有三条打点格式串；系统卷里有 /tmp/bb/dirs.sh 与 /tmp/bb/dirfall.sh。

用法：  py -3 tests\\dir64_test.py
退出码：0 = 硬断言全过；1 = 有硬断言失败；2 = 环境问题。
"""
import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import qemuhelp as qh              # noqa: E402

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

BUILD = os.path.join(ROOT, "build64")
DISK = os.path.join(BUILD, "sysdisk.img")
KERNEL_OS = os.path.join(BUILD, "kernel64_os.bin")
BB_BIN = os.path.join(BUILD, "busybox.bin")

PART_MAIN_LBA = 8009
TARGET_SECTORS = 32768
SECTOR = 512

FORBIDDEN = ["PANIC", "TRIPLE FAULT"]

TYPED_NAMES = {
    " ": "spc", "/": "slash", ".": "dot", "-": "minus", ">": "shift-dot",
    "=": "equal", "_": "shift-minus", ":": "shift-semicolon",
    "<": "shift-comma", "|": "shift-backslash", "(": "shift-9", ")": "shift-0",
    "+": "shift-equal", "*": "shift-8", ",": "comma", ";": "semicolon",
}


def load_mod(name, fname):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(ROOT, "tools", fname))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read_guest_file(img_path, parts):
    TP = load_mod("tcc_pack_win", "tcc_pack_win.py")
    d = open(img_path, "rb").read()
    vol = d[PART_MAIN_LBA * SECTOR:TARGET_SECTORS * SECTOR]
    if vol[0:8] != b"VIMTUFS2":
        return None
    total = struct.unpack_from("<I", vol, 20)[0]
    inodes = struct.unpack_from("<I", vol, 40)[0]
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
        _, rec = hit
        cur = hit[0]
    return TP._read_file(vol, rec, total)


def find_qemu(explicit=None):
    if explicit:
        return explicit if os.path.exists(explicit) else None
    for c in QEMU_CANDIDATES:
        if os.sep in c or "/" in c:
            if os.path.exists(c):
                return c
        else:
            f = shutil.which(c)
            if f:
                return f
    return None


def q(p):
    return p.replace("\\", "/")


# ===========================================================================
# 会话（与 tests/busybox64_test.py 同一套注入时序；见那里的长注释）
# ===========================================================================
class Session:
    def __init__(self, qemu, img, tag, timeout):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_dir64_")
        self.serial = os.path.join(self.tmp, tag + ".log")
        mport = qh.free_port()
        args = [
            qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std",
            "-display", "none",
            "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
            "-no-reboot",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = qh.Monitor(mport)
        qh.login_desktop(self.mon, self.log, self.proc, timeout=min(timeout, 180))
        self.wait_raw("[GUI64] ready", timeout)
        self.open_terminal()

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def term_open(self):
        return "[APP] term opened" in self.log()

    def open_terminal(self):
        for _ in range(6):
            if self.term_open():
                break
            self.mon.key("ret", wait=1.6)
            self.mon.key("ret", wait=1.6)
            time.sleep(1.0)
            self.mon.key("meta_l", wait=1.8)
            self.mon.key("1", wait=2.4)
        self.wait_raw("[APP] term opened", 20)
        self.type_line("shell", per_key=0.2)
        self.wait_raw("[SH64] launch path=/bin/shell.bin", 90)
        time.sleep(1.0)

    def wait_raw(self, needle, timeout=30, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            time.sleep(0.3)
        return False

    def type_line(self, text, per_key=0.08):
        for ch in text:
            if ch in TYPED_NAMES:
                self.mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch.isalnum():
                self.mon.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey 不支持这个字符：%r（加进 TYPED_NAMES）" % ch)
        self.mon.key("ret", wait=per_key + 0.2)

    def run_cmd(self, cmd, wait=None, timeout=180, per_key=0.07):
        base = len(self.log())
        self.type_line(cmd, per_key=per_key)
        if wait:
            self.wait_raw(wait, timeout=timeout, since=base)
            time.sleep(0.5)
        return base

    def text_since(self, base):
        return self.log()[base:]

    def close(self, keep=False):
        self.proc.kill()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass
        if not keep:
            shutil.rmtree(self.tmp, ignore_errors=True)


# ===========================================================================
# 解析小工具
# ===========================================================================
def marked(text, name):
    """取 '@@<name>' 与下一个 '@@' 之间的原文。"""
    k = text.find("@@%s" % name)
    if k < 0:
        return None
    k = text.find("\n", k)
    n = text.find("@@", k + 1)
    return text[k + 1:n if n > 0 else len(text)]


def payload_lines(section):
    """去掉内核/用户态打点行（以 '[' 开头）后的正文行。"""
    if section is None:
        return []
    out = []
    for ln in section.splitlines():
        s = ln.rstrip("\r")
        if not s.strip():
            continue
        if s.lstrip().startswith("["):
            continue
        out.append(s)
    return out


def bb_ls_names(section):
    """busybox `ls` 的名字集合。

    ★ 实测：本内核给 ring3 的 fd 1 认 TIOCGWINSZ（25x80）-> musl 的 isatty(1) 为真 ->
      busybox `ls` 走**多列**输出。所以要按空白切成 token，而不是"一行一个名字"。
    """
    names = set()
    for s in payload_lines(section):
        names.update(s.split())
    return names


def shell_ls_entries(section):
    """shell 内建 `ls` 的输出：目录 '  name/'、文件 '  name  N bytes'。"""
    dirs, files = set(), {}
    for s in payload_lines(section):
        m = re.match(r"^\s+(\S+)/$", s)
        if m:
            dirs.add(m.group(1))
            continue
        m = re.match(r"^\s+(\S+)\s+(\d+)\s+bytes$", s)
        if m:
            files[m.group(1)] = int(m.group(2))
    return dirs, files


def dir64_ents(raw):
    """从串口里抓 [DIR64] ent（用户态 dump：内核给的**原始记录字段**）-> {name: (kind, size, mtime)}。"""
    out = {}
    for m in re.finditer(r"\[DIR64\] ent name=(\S+) kind=(\d+) size=(\d+) mtime=(\d+)", raw):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)), int(m.group(4)))
    return out


def mtime_ok(packed):
    """vfs64 打包时间戳（kernel/vfs64.cpp：year-2000<<26 | month<<22 | day<<17 | h<<12 | m<<6 | s）。
    0 = 未知（可接受）；非 0 必须解出一个合理的日历值。"""
    if packed == 0:
        return True
    year = 2000 + ((packed >> 26) & 0x3F)
    month = (packed >> 22) & 0xF
    day = (packed >> 17) & 0x1F
    hour = (packed >> 12) & 0x1F
    minute = (packed >> 6) & 0x3F
    second = packed & 0x3F
    return (2000 <= year <= 2063) and (1 <= month <= 12) and (1 <= day <= 31) and \
           (hour <= 23) and (minute <= 59) and (second <= 59)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--img", default=None)
    ap.add_argument("--timeout", type=int, default=480)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    hard = []
    soft = []

    def H(name, ok, detail=""):
        if not ok:
            hard.append(name)
        print("  [%s] %s%s" % ("PASS" if ok else "FAIL", name, ("  -- " + detail) if detail else ""))
        return ok

    def S(name, ok, detail=""):
        print("  [%s] %s%s" % ("PASS" if ok else "WARN", name, ("  -- " + detail) if detail else ""))
        if not ok:
            soft.append(name)

    # ==================== ⑧ 构建期证据（宿主侧）====================
    print("== ⑧ 构建期证据（内核打点格式串 + 卷里的夹具）==")
    for p in (DISK, KERNEL_OS, BB_BIN):
        if not os.path.exists(p):
            sys.stderr.write("缺文件：%s（先跑 bash build64.sh）\n" % p)
            return 2
    k = open(KERNEL_OS, "rb").read()
    for tag in (b"[DIR64] open path=", b"[DIR64] open FAILED path=", b"[DIR64] read slot=",
                b"[DIR64] close slot="):
        H("内核镜像里有打点格式串 %r" % tag.decode(), tag in k)
    S("内核镜像体积 = %d B" % len(k), True)
    d = read_guest_file(args.img or DISK, ["tmp", "bb", "dirs.sh"])
    H("系统卷里有 /tmp/bb/dirs.sh（%s B）" % (len(d) if d else None), bool(d) and b"@@DUMP_TCC" in d)
    d = read_guest_file(args.img or DISK, ["tmp", "bb", "dirfall.sh"])
    H("系统卷里有 /tmp/bb/dirfall.sh（%s B）" % (len(d) if d else None), bool(d) and b".dir64-off" in d)

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64（--qemu 指定）\n")
        return 2

    img = args.img or DISK
    sess = Session(qemu, img, "dir64", args.timeout)
    raw = ""
    try:
        # ---- 顺带留证：本会话的身份（权限用例需要非 root）----
        b = sess.run_cmd("run /bin/busybox id", wait="uid=", timeout=90)
        ident = sess.text_since(b)
        S("会话身份（权限用例的依据）：%s" % " / ".join(x.strip() for x in ident.splitlines()
                                                   if x.strip() and "uid=" in x)[:120], True)

        # ---- 主脚本：live 目录枚举 ----
        base = len(sess.log())
        sess.type_line("run /bin/busybox sh /tmp/bb/dirs.sh", per_key=0.06)
        sess.wait_raw("@@DONE", timeout=260, since=base)
        time.sleep(2.5)
        ap_out = sess.text_since(base)

        sec = {}
        for n in ("LS_BEFORE", "FIND_BEFORE", "LS_AFTER", "LS_L_AFTER", "FIND_AFTER", "CAT_AFTER",
                  "DU_AFTER", "LS_DIR_AFTER", "FIND_TYPE", "LS_AFTER_RM", "FIND_AFTER_RM",
                  "NOPERM_LS", "NOPERM_RC", "DUMP_TCC", "DUMP_TCC_L", "DUMP_DEMO", "DUMP_DEMO_L"):
            sec[n] = marked(ap_out, n)
        H("dirs.sh 跑到 @@DONE（所有分段都抓到了）",
          all(sec[n] is not None for n in sec))

        # ---- ② 新建文件前后对比（核心断言）----
        print("== ② 同一会话内新建文件 -> 立刻可见（创建前/后对比）==")
        before = bb_ls_names(sec["LS_BEFORE"])
        after = bb_ls_names(sec["LS_AFTER"])
        H("ls /tmp/bb（创建前）不含 live.txt：%s" % sorted(before)[:4], "live.txt" not in before)
        H("ls /tmp/bb（创建后）**含** live.txt：%s" % sorted(after)[:4], "live.txt" in after)
        H("find /tmp/bb -name live.txt（创建前）无输出", not payload_lines(sec["FIND_BEFORE"]))
        H("find /tmp/bb -name live.txt（创建后）给出 /tmp/bb/live.txt",
          any("live.txt" in x for x in payload_lines(sec["FIND_AFTER"])))
        H("cat /tmp/bb/live.txt 拿到刚写的内容",
          any("live-created-this-boot" in x for x in payload_lines(sec["CAT_AFTER"])))
        H("du /tmp/bb/live.txt 有输出（走的也是 readdir+stat）",
          any("live.txt" in x for x in payload_lines(sec["DU_AFTER"])))
        S("ls -l /tmp/bb/live.txt 原文：%s" % " | ".join(payload_lines(sec["LS_L_AFTER"]))[:160], True)

        # ---- ②b mkdir 出来的子目录也要看得见（kind=dir）----
        H("mkdir 出来的 livedir 出现在 ls 里：%s" % sorted(bb_ls_names(sec["LS_DIR_AFTER"]))[:4],
          "livedir" in bb_ls_names(sec["LS_DIR_AFTER"]))
        H("find /tmp/bb -name livedir -type d 命中（类型判定正确）",
          any("livedir" in x for x in payload_lines(sec["FIND_TYPE"])))

        # ---- ③ 删除后不再出现 ----
        print("== ③ 删除后不再出现 ==")
        after_rm = bb_ls_names(sec["LS_AFTER_RM"])
        H("rm 之后 ls /tmp/bb 不含 live.txt：%s" % sorted(after_rm)[:4], "live.txt" not in after_rm)
        H("rm 之后 find 也没有 live.txt", not any("live.txt" in x for x in payload_lines(sec["FIND_AFTER_RM"])))

        # ---- ① ABI 记录 vs shell `ls` 逐项一致 ----
        print("== ① dir_open/read/close 与 shell 的 ls 逐项一致 ==")
        ents = dir64_ents(ap_out)
        H("[DIR64] ent 记录出现了（内核把 name/kind/size/mtime 交到了用户态）", len(ents) >= 8,
          "拿到 %d 条：%s" % (len(ents), sorted(ents)[:6]))
        H("/tcc 的三个子目录 kind=2(dir)、libtcc1.a kind=1(file)",
          ents.get("demo", (0,))[0] == 2 and ents.get("include", (0,))[0] == 2 and
          ents.get("lib", (0,))[0] == 2 and ents.get("libtcc1.a", (0,))[0] == 1,
          "demo=%s include=%s lib=%s libtcc1.a=%s" % (ents.get("demo"), ents.get("include"),
                                                      ents.get("lib"), ents.get("libtcc1.a")))
        H("目录项的 size=0、文件的 size>0（libtcc1.a=%s）" % (ents.get("libtcc1.a", (0, 0))[1],),
          ents.get("libtcc1.a", (0, 0))[1] > 0 and ents.get("demo", (0, 1))[1] == 0)
        bad_mt = [n for n, (_k, _s, mt) in ents.items() if not mtime_ok(mt)]
        H("mtime 都合理（0=未知 或 解得出 2000..2063 / 月日时分秒合法），异常 %s" % bad_mt, not bad_mt)
        S("mtime 实测样本：%s" % ", ".join("%s=%d" % (n, ents[n][2]) for n in sorted(ents)[:5]), True)

        # shell 的 ls（终端服务的"代列协议"，内核 fd64 目录原语）——同一目录
        bs = sess.run_cmd("ls /tcc/demo", wait="/tcc/demo:", timeout=60)
        sh_txt = sess.text_since(bs)
        sh_dirs, sh_files = shell_ls_entries(sh_txt)
        print("      shell `ls /tcc/demo`：dirs=%s files=%s" % (sorted(sh_dirs), sorted(sh_files)))
        H("shell ls 列到了 /tcc/demo（非空）", bool(sh_dirs or sh_files))
        bb_demo = bb_ls_names(sec["DUMP_DEMO"])
        H("busybox `ls /tcc/demo` 的名字集合 == shell `ls /tcc/demo` 的名字集合",
          bb_demo == (sh_dirs | set(sh_files)),
          "busybox=%s shell=%s" % (sorted(bb_demo), sorted(sh_dirs | set(sh_files))))
        size_mismatch = [n for n, sz in sh_files.items() if ents.get(n, (0, -1))[1] != sz]
        H("每个文件的大小：ABI 记录 == shell ls 报的字节数（不一致：%s）" % size_mismatch, not size_mismatch,
          "样本 %s" % {n: (ents.get(n, (0, 0))[1], sh_files[n]) for n in list(sh_files)[:3]})
        kind_mismatch = [n for n in sh_dirs if ents.get(n, (1,))[0] != 2]
        H("shell 报成目录的条目，ABI 记录的 kind 也是 dir（不一致：%s）" % kind_mismatch, not kind_mismatch)

        # ---- ④ 无权限目录 -> 明确错误码、不崩 ----
        print("== ④ 无权限目录 -> 明确错误码、不崩 ==")
        np_ls = payload_lines(sec["NOPERM_LS"])
        np_rc = " ".join(payload_lines(sec["NOPERM_RC"]))
        H("busybox `ls /tmp/bb/noperm`（chmod 000）明确报权限错：%s" % (np_ls[:1] or ""),
          any(("Permission denied" in x) or ("denied" in x) for x in np_ls))
        H("busybox `ls` 的退出码非 0（原文：%s）" % (np_rc.strip() or "<$? 未展开，见下>"),
          re.search(r"@@NOPERM_RC\s+[1-9]", ap_out) is not None)
        # ★ 如实标注：本移植的 ash 在脚本里 `echo $?` 实测展开成空（与既有 net.sh 的
        #   `@@WGET_RC` 同一现象）——所以\"退出码\"只作为可选证据；**明确错误码**由 busybox
        #   自己的报错文本 + 内核的 [DIR64] open FAILED err=13 两条硬断言负责。
        H("内核侧明确错误码 [DIR64] open FAILED path=/tmp/bb/noperm err=13（EACCES）",
          re.search(r"\[DIR64\] open FAILED path=/tmp/bb/noperm err=13", ap_out) is not None)
        H("无权限目录之后脚本继续跑完（@@DONE 之后还有 DUMP_CLEAN）", "DUMP_CLEAN" in ap_out)

        # ---- ⑤ 句柄回收（每进程 fd 表 -> 退出即回收；无泄漏）----
        print("== ⑤ 句柄回收（不同进程/不同次拿到同一个 slot；从不撞 fd 表上限）==")
        opens = re.findall(r"\[DIR64\] open path=(\S+) slot=(\d+)", ap_out)
        closes = re.findall(r"\[DIR64\] close slot=(\d+)", ap_out)
        H("打点齐全：open=%d 条 / close=%d 条" % (len(opens), len(closes)),
          len(opens) >= 3 and len(closes) >= 3)
        slots = [int(s) for (p, s) in opens if p == "/tcc/demo"]
        H("同一目录 /tcc/demo 在**不同进程**里都拿到同一个 slot=%s（上一个进程退出后句柄已回收）"
          % (sorted(set(slots)) or None,),
          len(slots) >= 2 and len(set(slots)) == 1 and slots[0] < 32)
        maxslot = max([int(s) for (_p, s) in opens] or [0])
        H("所有 slot 都远小于 fd 表上限 32（max=%d；没有越分配越高的泄漏）" % maxslot, maxslot < 32)
        H("没有 EMFILE（[DIR64] open FAILED ... err=24）", "err=24" not in ap_out)

        # ---- ⑥ 兜底：构建期快照仍可用 ----
        print("== ⑥ 兜底路径（人为关掉新 ABI -> 退回 /etc/vimtu.dirs 快照）==")
        base2 = len(sess.log())
        sess.type_line("run /bin/busybox sh /tmp/bb/dirfall.sh", per_key=0.06)
        sess.wait_raw("@@DONE", timeout=180, since=base2)
        time.sleep(2.0)
        f_out = sess.text_since(base2)
        H("用户态打点 [DIR64] fallback snapshot reason=disabled(/tmp/.dir64-off) 出现",
          "[DIR64] fallback snapshot reason=disabled" in f_out)
        tcc_fb = bb_ls_names(marked(f_out, "FALL_LS_TCC"))
        H("兜底（快照）仍能列 /tcc：%s" % sorted(tcc_fb), "demo" in tcc_fb and "libtcc1.a" in tcc_fb)
        tmp_fb = bb_ls_names(marked(f_out, "FALL_LS_TMP"))
        H("兜底列的是**构建期快照**：本次新建的 fallnew.txt **看不到**（快照里没有它；%s）"
          % sorted(tmp_fb)[:4], "fallnew.txt" not in tmp_fb)
        H("兜底的 find 也给结果（find /tcc -name *.c -> hello.c）",
          any("hello.c" in x for x in payload_lines(marked(f_out, "FALL_FIND"))))
        H("关掉新 ABI 期间没有 [DIR64] open FAILED path=/tcc",
          "[DIR64] open FAILED path=/tcc" not in f_out)

        raw = sess.log()
    finally:
        sess.close(keep=args.keep)

    # ==================== ⑦ 全局纪律 ====================
    print("== ⑦ 全局纪律（无 PANIC / 无 enosys / 无 deny 泛滥）==")
    for f in FORBIDDEN:
        H("串口里没有 %r" % f, f not in raw)
    enosys = sorted(set(int(m) for m in re.findall(r"\[SYSCALL\][^\n]*enosys nr=(\d+)", raw)))
    H("50/51/52 不在 enosys 清单里（清单=%s）" % (enosys or "无"),
      not (set(enosys) & {50, 51, 52}))
    H("[DIR64] read slot= 打点出现（dir_read 真的被调用）", "[DIR64] read slot=" in raw)
    H("[DIR64] close slot= 打点出现（dir_close 真的被调用）", "[DIR64] close slot=" in raw)

    print()
    print("汇总：硬断言失败 %d 条%s；如实标注的 GAP/WARN %d 条"
          % (len(hard), ("（%s）" % ", ".join(hard[:6])) if hard else "", len(soft)))
    if hard:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
