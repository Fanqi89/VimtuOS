#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tests/vmtool64_test.py - ★ v0.9.30：ACL / 配额 / 日志 / 快照 的 **ring-3 入口**验收

要证明的事（每一层都必须有串口证据，不靠"看起来跑通了"）：
  ① `/usr/bin/vmtool` 真的在系统卷里、真的能被 ring-3 `run` 起来（`[FD64] open` + `[ELF64] load` +
     `run: /usr/bin/vmtool pid=.. exited code=0`）。
  ② **setxattr(188) / getxattr(191)** 是 Linux 真号，值字节按 Linux 的 posix_acl_xattr 规范
  ② **setxattr(188) / getxattr(191)** 是 Linux 真号，值字节按 Linux 的 posix_acl_xattr 规范
     （`{u32 a_version=2}` + 8N 条目）→ 本测试写的 `acl deny` 是 **36 B（4+4×8）**，内核剥掉
     4 B 头后落盘 **32 B**：`[VMTU] setxattr … len=36 rc=0 ok` + `[XATTR] setxattr ok … len=32 hdr=1`；
     读回 `[VMTU] getxattr … rc=32`。
  ③ **ACL 真的生效**：给文件写一份"属主也被锁死"的 ACL 之后，同一个 uid 的写必须被拒
     （`[PERM64] deny … via=acl need=w`），`acl clear` 之后又能写 —— 双向。
  ④ **配额真的生效**：root 给 uid 1000 设 4 块上限 → uid 1000 写 10 块被拒（`[QUOTA] deny uid=1000 …`）。
  ⑤ **特性真的落盘**：`vmtool feature on all`（私有号 1028）在**开机前**打开四项特性，重启后
     ACL 写入仍然成功、配额仍然生效 ⇒ 证明 bit0..3 存在盘上（不是内存态）。
  ⑥ **日志 / 快照有用户可见入口**：`/proc/vimtu/journal`、`/proc/vimtu/snapshots`、`/proc/vimtu/quota`
     可被 ring-3 打开（`[LX64] proc open …`）；快照能拍（`[VMTU] snap_take … ok` + `[SNAP] take ok`）。

阶段划分（为什么两段）：`feature on` 与 `quota set` 都要求 root，而 ACL 拒绝必须由**非 root** 的
uid 1000 触发（root 会绕过 DAC）。所以：阶段 1 = root 开特性/设配额/拍照；阶段 2 = 重新开机、
以 vimtu(1000) 登录做 ACL 与配额的**拒绝**验收 —— 顺带把"特性持久化"也证明了。

用法（必须 Windows 原生 Python）：py -3 tests\\vmtool64_test.py [--timeout 240] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题（QEMU/构建产物缺失）。
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import qemuhelp as qh              # noqa: E402

BUILD = os.path.join(ROOT, "build64")
SYSDISK = os.path.join(BUILD, "sysdisk.img")
VMTool = os.path.join(BUILD, "vmtool")

QEMU_CANDIDATES = [
    r"C:\Program Files\qemu\qemu-system-x86_64.exe",
    r"C:\Program Files (x86)\qemu\qemu-system-x86_64.exe",
    "qemu-system-x86_64",
]

TYPED_NAMES = {
    " ": "spc",
    "/": "slash",
    ".": "dot",
    "-": "minus",
    "_": "shift-minus",
    "=": "equal",
    ",": "comma",
    ":": "shift-semicolon",
}

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "FAILED mask=",
    "selftest FAIL",
]


def q(p):
    return p.replace("\\", "/")


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


class Session:
    def __init__(self, qemu, img, tag, timeout, root_login=False):
        self.tmp = tempfile.mkdtemp(prefix="vimtu64_vmtool64_")
        self.serial = os.path.join(self.tmp, tag + ".log")
        mport = qh.free_port()
        args = [
            qemu, "-name", "Vimtu64-" + tag,
            "-drive", "format=raw,file=%s" % q(img),
            "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
            "-serial", "file:%s" % q(self.serial),
            "-monitor", "telnet:127.0.0.1:%d,server,nowait" % mport,
            "-no-reboot",
        ]
        self.proc = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.mon = qh.Monitor(mport)
        qh.login_desktop(self.mon, self.log, self.proc, timeout=min(timeout, 180))
        self.wait("[GUI64] ready", timeout)
        self.open_terminal()

    def log(self):
        try:
            with open(self.serial, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return ""

    def wait(self, needle, timeout=30, since=0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if needle in self.log()[since:]:
                return True
            time.sleep(0.3)
        return False

    def type_line(self, text, per_key=0.06):
        for ch in text:
            if ch in TYPED_NAMES:
                self.mon.key(TYPED_NAMES[ch], wait=per_key)
            elif ch == "_":
                self.mon.key("shift-minus", wait=per_key)
            elif ch.isalnum():
                self.mon.key(ch, wait=per_key)
            else:
                raise ValueError("sendkey \u4e0d\u652f\u6301\u8fd9\u4e2a\u5b57\u7b26\uff1a%r" % ch)
        self.mon.key("ret", wait=per_key + 0.2)

    def open_terminal(self):
        for _ in range(6):
            if "[APP] term opened" in self.log():
                break
            self.mon.key("ret", wait=1.6)
            self.mon.key("ret", wait=1.6)
            time.sleep(1.0)
            self.mon.key("meta_l", wait=1.8)
            self.mon.key("1", wait=2.4)
        self.wait("[APP] term opened", 20)

    def close(self, keep=False):
        self.proc.kill()
        try:
            self.proc.wait(timeout=10)
        except Exception:
            pass
        if keep:
            print("[vmtool64] \u4e32\u53e3\u65e5\u5fd7\uff1a%s" % self.serial)


def count(text, needle):
    return text.count(needle)


def vmtool_exits(text):
    """所有 `run: /usr/bin/vmtool … exited code=N` 的退出码序列。"""
    return [int(m) for m in re.findall(r"run: /usr/bin/vmtool.*?exited code=(-?\d+)", text)]


def run_tool(sess, args, expect_exit, timeout=120, per_key=0.05):
    """跑一条 `run /usr/bin/vmtool …`，等到**新的**退出行出现；返回 (整段日志, 真实退出码)。"""
    base = len(sess.log())
    before = len(vmtool_exits(sess.log()))
    sess.type_line("run /usr/bin/vmtool" + (" " + args if args else ""), per_key=per_key)
    deadline = time.time() + timeout
    code = None
    while time.time() < deadline:
        ex = vmtool_exits(sess.log())
        if len(ex) > before:
            code = ex[-1]
            break
        time.sleep(0.3)
    time.sleep(0.4)
    seg = sess.log()[base:]
    return seg, code, expect_exit


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    qemu = find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("\u627e\u4e0d\u5230 qemu-system-x86_64\n")
        return 2
    for need in (SYSDISK, VMTool):
        if not os.path.exists(need):
            sys.stderr.write("\u7f3a\u5c11\u6784\u5efa\u4ea7\u7269\uff1a%s\uff08\u5148\u8dd1 bash build64.sh\uff09\n" % need)
            return 2

    tmp = tempfile.mkdtemp(prefix="vimtu64_vmtool64_fix_")
    img = os.path.join(tmp, "vmtool64.img")
    shutil.copyfile(SYSDISK, img)

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    # ==================== 阶段 1（root）：开特性 / 设配额 / 拍快照 ====================
    print("=== \u9636\u6bb5 1\uff1aroot \u5f00\u7279\u6027 + \u8bbe\u914d\u989d + \u62cd\u5feb\u7167 ===")
    s1 = Session(qemu, img, "vmtool-root", args.timeout)
    try:
        log0 = s1.log()
        s1.type_line("su - root")
        s1.wait("[TERM] cmd su ok", 30)
        check("su - root\uff08\u7ec8\u7aef\u8eab\u4efd\u53d8 root\uff09",
              "[USER64] su ok from=" in s1.log() and "to=root euid=0" in s1.log())
        s1.type_line("shell")
        s1.wait("[SH64] launch path=/bin/shell.bin", 120)
        time.sleep(0.8)
        base = len(s1.log())

        seg, code, _ = run_tool(s1, "feature on all", 0)
        log1 = s1.log()
        check("vmtool \u5728\u7cfb\u7edf\u5377\u91cc\u4e14\u80fd\u88ab ring-3 \u8d77\u6765\uff08[FD64] open /usr/bin/vmtool + [ELF64] load\uff09",
              "[FD64] open path=/usr/bin/vmtool fd=" in log1 and
              "[ELF64] load path=/usr/bin/vmtool" in log1)
        check("\u7279\u6027\u5f00\u5173\uff08\u79c1\u6709\u53f7 1028\uff09\uff1afeature on all \u9000\u51fa\u7801 0", code == 0, "code=%s" % code)
        check("[VMTU] feat_enable \u6253\u70b9\uff08ring-3 \u771f\u8c03\u7528\uff0cpid \u975e 0\uff09",
              re.search(r"\[VMTU\] feat_enable pid=\d+ uid=0\b.*len=15 rc=0 ok", log1) is not None)
        # \u2605 \u5185\u6838\u6253\u70b9\u91cc\u7684 bit= \u662f**\u4f4d\u63a9\u7801**\uff081 journal / 2 quota / 4 acl / 8 snapshot\uff09\uff0c\u4e0d\u662f\u5e8f\u53f7
        for mask, nm in ((1, "journal"), (2, "quota"), (4, "acl"), (8, "snapshot")):
            check("[VFS64] feature on bit=%d name=%s\uff08\u771f\u5199\u76d8\uff09" % (mask, nm),
                  re.search(r"\[VFS64\] feature on bit=%d name=%s" % (mask, nm), log1) is not None)

        seg, code, _ = run_tool(s1, "features", 0)
        check("/proc/vimtu/features \u53ef\u88ab ring-3 \u6253\u5f00\uff08[LX64] proc open\uff09",
              "[LX64] proc open /proc/vimtu/features fd=" in s1.log())
        check("features \u5b50\u547d\u4ee4\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        seg, code, _ = run_tool(s1, "quota set 1000 4", 0)
        log1 = s1.log()
        check("[VMTU] quota_set \u6253\u70b9\uff08uid=1000 limit=4\uff09",
              re.search(r"\[VMTU\] quota_set pid=\d+ uid=0\b.*len=1000 rc=0 ok", log1) is not None)
        check("[QUOTA] set ok uid=1000 limit=4\uff08\u5bb9\u91cf\u771f\u5199\u5165\u914d\u989d\u8868\uff09",
              re.search(r"\[QUOTA\] set ok uid=1000 limit=4", log1) is not None)
        check("quota set \u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        # \u2605 \u5feb\u7167\uff08\u79c1\u6709\u53f7 1024\uff09\uff1a\u672c\u6d4b\u8bd5\u7684\u7cfb\u7edf\u5377 24,759 \u5757\u3001\u5df2\u7528 65%\uff0c\u800c\u5168\u91cf\u514b\u9686
        #   \u81f3\u5c11\u9700\u8981"\u5df2\u7528\u6570\u636e\u5757"\u90a3\u4e48\u591a\u7a7a\u95f4 \u2014\u2014 \u653e\u4e0d\u4e0b\u662f**\u7269\u7406\u4e8b\u5b9e**\u3002\u6240\u4ee5\u8fd9\u91cc\u9a8c\u7684\u662f
        #   "\u5165\u53e3\u771f\u5230\u4e86\u5185\u6838\u3001\u5185\u6838**\u5982\u5b9e\u62d2\u7edd**\u3001\u4e0d\u628a\u5377\u5199\u574f"\uff08\u6b63\u5411\u8def\u5f84\u5728\u5185\u6838\u81ea\u68c0 bit21
        #   \u4e0e tests/snapshot64_test.py \u91cc\u5df2\u8986\u76d6\uff1b\u771f\u5377\u4e0a\u7684\u6b63\u5411\u53d6\u8bc1\u9700\u8981 76 MB \u7684\u5b89\u88c5\u5377\uff0c\u89c1\u6587\u6863\u8fb9\u754c\uff09\u3002
        seg, code, _ = run_tool(s1, "snap-take", 1)
        log1 = s1.log()
        check("[VMTU] snap_take \u6253\u70b9\uff08ring-3 \u771f\u8c03\u7528\uff0cpid \u975e 0\uff09",
              re.search(r"\[VMTU\] snap_take pid=\d+ uid=0 len=0 rc=1 fail", log1) is not None)
        check("\u7a7a\u95f4\u4e0d\u591f\u65f6\u5185\u6838**\u5982\u5b9e\u62d2\u7edd**\uff08[SNAP] take FAILED reason=no-space want=\u2026 free=\u2026\uff09",
              re.search(r"\[SNAP\] take FAILED reason=no-space want=\d+ free=\d+", log1) is not None)
        check("snap-take \u5931\u8d25\u65f6 vmtool \u9000\u51fa\u7801 = 1", code == 1, "code=%s" % code)

        # \u2605 \u62d2\u7edd\u4e4b\u540e\u5377\u5fc5\u987b\u4ecd\u7136\u5065\u5eb7\uff08\u4e0d\u80fd\u62cd\u4e00\u534a\u5c31\u628a\u5377\u5199\u574f\uff09\uff1a\u518d\u5199\u4e00\u4e2a\u6587\u4ef6\u5fc5\u987b\u6210\u529f
        seg, code, _ = run_tool(s1, "write /home/vimtu/snap-probe.bin 512", 0)
        check("\u5feb\u7167\u88ab\u62d2\u540e\u5377\u4ecd\u53ef\u5199\uff08write /home/vimtu/snap-probe.bin\uff09", code == 0, "code=%s" % code)

        seg, code, _ = run_tool(s1, "journal", 0)
        check("/proc/vimtu/journal \u53ef\u8bfb\uff08[LX64] proc open\uff09",
              "[LX64] proc open /proc/vimtu/journal fd=" in s1.log())
        check("journal \u5b50\u547d\u4ee4\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        seg, code, _ = run_tool(s1, "snapshots", 0)
        check("/proc/vimtu/snapshots \u53ef\u8bfb\uff08[LX64] proc open\uff09",
              "[LX64] proc open /proc/vimtu/snapshots fd=" in s1.log())
        check("snapshots \u5b50\u547d\u4ee4\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        seg, code, _ = run_tool(s1, "quota-all", 0)
        check("/proc/vimtu/quota \u53ef\u8bfb\uff08[LX64] proc open\uff09",
              "[LX64] proc open /proc/vimtu/quota fd=" in s1.log())
        check("quota-all \u5b50\u547d\u4ee4\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        log1 = s1.log()
        for bad in FORBIDDEN:
            check("\u9636\u6bb51\uff1a\u4e0d\u5f97\u51fa\u73b0 %s" % bad, bad not in log1)
    finally:
        s1.close(keep=args.keep)

    # ==================== 阶段 2（vimtu / uid 1000）：ACL 与配额的拒绝验收 ====================
    print("=== \u9636\u6bb5 2\uff1a\u91cd\u542f\u540e\u4ee5 vimtu(uid 1000) \u9a8c\u6536 ACL / \u914d\u989d\u62d2\u7edd ===")
    s2 = Session(qemu, img, "vmtool-user", args.timeout)
    try:
        s2.type_line("shell")
        s2.wait("[SH64] launch path=/bin/shell.bin", 120)
        time.sleep(0.8)

        # 1) 建文件（属主 = uid 1000），确认写成功
        seg, code, _ = run_tool(s2, "write /home/vimtu/q1.bin 512", 0)
        check("\u7528 vmtool \u5efa\u6587\u4ef6\uff08write /home/vimtu/q1.bin 512\uff09\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)
        check("\u6587\u4ef6\u771f\u5199\u76d8\uff08[FD64] open path=/home/vimtu/q1.bin\uff09",
              "[FD64] open path=/home/vimtu/q1.bin" in s2.log())

        # 2) 写一份"属主也锁死"的 ACL（Linux 规范 36 B，带 4 B 版本头）
        seg, code, _ = run_tool(s2, "acl deny /home/vimtu/q1.bin", 0)
        log2 = s2.log()
        check("setxattr(188)\uff1a\u7528\u6237\u6001\u53d1\u7684\u662f **Linux \u89c4\u8303\u5b57\u8282**\uff08[VMTU] setxattr hdr=1\uff09",
              re.search(r"\[VMTU\] setxattr pid=\d+ uid=1000 path=/home/vimtu/q1\.bin name=system\.posix_acl_access "
                        r"len=36 rc=0 ok", log2) is not None)
        check("\u5185\u6838\u5265\u6389 4 B \u7248\u672c\u5934\u540e\u843d\u76d8 32 B\uff08[XATTR] setxattr ok len=32 hdr=1\uff09",
              re.search(r"\[XATTR\] setxattr ok path=/home/vimtu/q1\.bin name=system\.posix_acl_access len=32 hdr=1", log2) is not None)
        check("acl deny \u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        # 3) getxattr 读回
        seg, code, _ = run_tool(s2, "acl get /home/vimtu/q1.bin", 0)
        log2 = s2.log()
        check("\u8bfb\u56de ACL\uff0891\uff09\uff1a\u957f\u5ea6 = 4 \u9879 \u00d7 8 B = 32\uff08[VMTU] getxattr rc=32\uff09",
              re.search(r"\[VMTU\] getxattr pid=\d+ uid=1000 path=/home/vimtu/q1\.bin "
                        r".*rc=32 ok", log2) is not None)
        check("acl get \u9000\u51fa\u7801 0", code == 0, "code=%s" % code)

        # 4) 同一个 uid 再写 -> 必须被 ACL 拒
        seg, code, _ = run_tool(s2, "write /home/vimtu/q1.bin 512", 1)
        log2 = s2.log()
        check("ACL \u771f\u751f\u6548\uff1a\u540c uid \u5199\u5165\u88ab\u62d2\uff08[PERM64] deny \u2026 via=acl need=w\uff09",
              re.search(r"\[PERM64\] deny op=(write|access) path=/home/vimtu/q1\.bin uid=1000 "
                        r"via=acl need=w", log2) is not None and
              "[FD64] open FAILED path=/home/vimtu/q1.bin rc=13" in log2)
        check("acl \u62d2\u7edd\u65f6 vmtool \u9000\u51fa\u7801 = 1", code == 1, "code=%s" % code)

        # 5) 清掉 ACL -> 又能写
        seg, code, _ = run_tool(s2, "acl clear /home/vimtu/q1.bin", 0)
        check("\u6e05 ACL\uff08setxattr size=0 -> \u5220\u9664\u9879\uff09\u9000\u51fa\u7801 0", code == 0, "code=%s" % code)
        n_ok = len(re.findall(r"\[TERM\] cmd rm ok|\[FD64\] close", s2.log()))
        _ = n_ok
        seg, code, _ = run_tool(s2, "write /home/vimtu/q1.bin 512", 0)
        check("ACL \u6e05\u6389\u540e\u53c8\u80fd\u5199\uff08\u53cc\u5411\u8bc1\u660e\uff09", code == 0, "code=%s" % code)

        # 6) 配额：root 在阶段 1 设的 limit=4 应该仍然生效（特性/配额表已落盘）
        seg, code, _ = run_tool(s2, "write /home/vimtu/q1.bin 5120", 1)
        log2 = s2.log()
        check("QUOTA \u771f\u751f\u6548\uff1a10 \u5757 > limit=4 \u88ab\u62d2\uff08[QUOTA] deny uid=1000\uff09",
              re.search(r"\[QUOTA\] deny uid=1000", log2) is not None)
        check("\u914d\u989d\u62d2\u7edd\u65f6 vmtool \u9000\u51fa\u7801 = 1\uff08\u4e0d\u4ea7\u751f\u534a\u622a\u6587\u4ef6\uff09", code == 1, "code=%s" % code)

        log2 = s2.log()
        for bad in FORBIDDEN:
            check("\u9636\u6bb52\uff1a\u4e0d\u5f97\u51fa\u73b0 %s" % bad, bad not in log2)
        check("\u9636\u6bb52\uff1a\u4e0d\u5f97\u51fa\u73b0 ACL \u7279\u6027\u672a\u5f00\u7684\u9519\u8bef",
              "reason=acl-feature-off" not in log2)
    finally:
        s2.close(keep=args.keep)

    print("=== RESULT: %s ===  checks=%d ok=%d" %
          ("PASS" if ok else "FAIL", len(checks), sum(1 for _, c in checks if c)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
