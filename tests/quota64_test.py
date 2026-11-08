"""tests/quota64_test.py - VimtuFS2 v5 **per-uid 块数配额**（quota）验收

判据（三层，互相独立）：
  A) 代码结构（不需要 QEMU）：配额表格式（16 项 {uid,limit,used,flags}）/ 预检 qt_allow64 / 记账 qt_charge64 / 只有 root 能设
  B) 串口真跑：`[VFS64] quota ok uid=1234 limit=4 deny_write=1 no_partial=1 deny_create=1 after_free=1 used=4 step=0`
     —— `step` 是自检里**第一条**失败判据的编号（0 = 全过），排障不用猜；
  C) 总自检与打点：`[QUOTA] deny uid=1234 want=.. used=.. limit=4 reason=over-limit`（拒绝路径真的走了）与 `[VFS64] feature on bit=2 name=quota`

边界（如实）：记账粒度 = 一次写操作的块数增量（不是每分配一块查一次表）；删除/变小永远放行；并发写不精确

用法：py -3 tests\\quota64_test.py [--qemu 路径] [--keep]
退出码：0 = 全过；1 = 有断言失败；2 = 环境问题
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

import proc64_test as p64          # noqa: E402  夹具 / QEMU 路径

VFS_C = os.path.join(ROOT, "kernel", "vfs64.cpp")
VFS_H = os.path.join(ROOT, "kernel", "vfs64.h")

FORBIDDEN = [
    "PANIC",
    "TRIPLE FAULT",
    "[VFS64] selftest FAIL",
    "[MEM64] OOM",
]


def q(path):
    return path.replace("\\", "/")


def slog(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def wait_re(path, pattern, timeout, proc=None):
    deadline = time.time() + timeout
    while time.time() < deadline:
        m = re.search(pattern, slog(path))
        if m:
            return m
        if proc is not None and proc.poll() is not None:
            break
        time.sleep(0.4)
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", default=None)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    checks = []
    ok = True

    def check(name, cond, detail=""):
        nonlocal ok
        ok = ok and bool(cond)
        checks.append((name, bool(cond)))
        print("  [%s] %s%s" % ("PASS" if cond else "FAIL", name, ("  " + detail) if detail else ""))

    print("=== A) 代码结构 ===")
    src = open(VFS_C, "r", encoding="utf-8", errors="replace").read()
    hdr = open(VFS_H, "r", encoding="utf-8", errors="replace").read()
    check("配额表格式（magic JQT1 / 16 项 / 每项 uid+limit+used+flags）",
          "QT_MAGIC" in src and "QT_ENTRIES   = 16u" in src and "QT_ENT_BYTES = 16u" in src, "")
    check("写路径有预检（qt_allow64）与记账（qt_charge64）",
          src.count("qt_allow64(") >= 4 and src.count("qt_charge64(") >= 5,
          "allow=%d charge=%d" % (src.count("qt_allow64("), src.count("qt_charge64(")))
    check("超限在动手之前失败（write_stream / write_at / truncate 三处都先判）",
          src.count("qt_allow64(q_uid") >= 3, "count=%d" % src.count("qt_allow64(q_uid"))
    check("只有 root 能改配额（vfs64_quota_set64 判 euid）",
          "need-root" in src and "int vfs64_quota_set64" in src, "")
    check("特性位与接口在头文件（FEAT_QUOTA / quota_set / quota_get）",
          all(k in hdr for k in ("VFS64_FEAT_QUOTA", "vfs64_quota_set64", "vfs64_quota_get64")), "")
    print("=== B) 真 QEMU ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\\n")
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败\\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_quota_")
    disk = os.path.join(tmp, "quota.img")
    shutil.copyfile(base, disk)                  # ★ 副本：绝不直接引导共享夹具
    serial = os.path.join(tmp, "serial.log")
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-quota", "-drive", "format=raw,file=%s" % q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_re(serial, r"\[VFS64\] quota ok uid=(\d+) limit=(\d+) deny_write=1 no_partial=1 "
                             r"deny_create=1 after_free=1 used=(\d+) step=(\d+)", 300, proc)
        check("[VFS64] quota ok 行出现（自检 bit19 跑完）", m is not None,
              m.group(0) if m else "（没等到 quota ok 行）")
        if m:
            check("限额 = 4 块（uid=1234）", m.group(2) == "4", "limit=%s" % m.group(2))
            check("Q2 删掉并把块还给属主后 used 正好 4（q3 = 2048B = 4 块）", m.group(3) == "4",
                  "used=%s" % m.group(3))
            check("bit19 每一条判据都过（step=0）", m.group(4) == "0", "step=%s" % m.group(4))
        md = wait_re(serial, r"\[QUOTA\] deny uid=1234 want=(\d+) used=(\d+) limit=4 reason=over-limit",
                     300, proc)
        check("拒绝路径真的打点（[QUOTA] deny uid=1234 ... reason=over-limit）", md is not None,
              md.group(0) if md else "（没等到 [QUOTA] deny 行）")
        if md:
            check("拒绝时 used + want > limit（确实是超额才拒）",
                  int(md.group(1)) + int(md.group(2)) > 4,
                  "want=%s used=%s limit=4" % (md.group(1), md.group(2)))
        check("[QUOTA] set ok uid=1234 limit=4（root 设置生效）",
              "[QUOTA] set ok uid=1234 limit=4" in slog(serial), "")
        check("特性层打点：[VFS64] feature on bit=2 name=quota",
              "[VFS64] feature on bit=2 name=quota" in slog(serial), "")
        # ★ 注意：QEMU 的 `-serial file:` 带缓冲，而且这里的正则别再写双反斜杠（会匹配字面反斜杠）。
        #   做法：这里只记标记，最终判定放到进程退出后（finally 里回读一次串口）。
        _selft_ok = ("[VFS64] selftest PASS" in slog(serial))
        txt = slog(serial)
        bad = [w for w in FORBIDDEN if w in txt]
        check("没有禁止行", not bad, "、".join(bad) if bad else "")
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=20)
        except Exception:
            try:
                proc.kill()
            except Exception:
                pass
        _final_log = slog(serial)          # 进程已退出 -> QEMU 把串口缓冲刷盘了
        if args.keep:
            print("日志保留：%s" % tmp)
        else:
            shutil.rmtree(tmp, ignore_errors=True)
    if not _selft_ok:
        _selft_ok = ("[VFS64] selftest PASS" in _final_log)
    check("[VFS64] selftest PASS（总自检没被特性层拉红）", _selft_ok,
          "" if _selft_ok else "串口里没有 selftest PASS（见上面的 FAIL mask）")

    print()
    passed = sum(1 for _n, c in checks if c)
    print("=== %d/%d PASS ===" % (passed, len(checks)))
    return 0 if passed == len(checks) and checks else 1


if __name__ == "__main__":
    sys.exit(main())
