"""tests/acl64_test.py - VimtuFS2 v5 **POSIX ACL 子集**（xattr: system.posix_acl_access）验收

判据（三层，互相独立）：
  A) 代码结构（不需要 QEMU）：xattr 块格式（XAT1 + 链式项）/ ACL 二进制数组解析（含 MASK）/ 权限判定接入 perm_check_ino64
  B) 串口真跑：`[VFS64] acl ok acl_bytes=40 named_uid=1234 r_ok=1 w_denied=1 other_by_mode=1 cleared_back_to_mode=1 step=0`
     —— `step` 是自检里**第一条**失败判据的编号（0 = 全过），排障不用猜；
  C) 总自检与打点：`[PERM64] deny ... via=acl need=w`（拒绝走的是 ACL 分支，不是 mode）与 `[XATTR] setxattr ok path=/a1.txt name=system.posix_acl_access`

边界（如实）：ACL 只做 access ACL 的三段 + 命名用户/组 + MASK（不做 default ACL、不做继承、无附加组）

用法：py -3 tests\\acl64_test.py [--qemu 路径] [--keep]
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
    check("xattr 块格式（magic XAT1 + count + [nlen][name][vlen][val] 链）",
          "XA_MAGIC" in src and "XA_HDR" in src and "xattr_put64" in src, "")
    check("ACL 按 Linux 二进制 posix_acl 数组解析（USER_OBJ/USER/GROUP_OBJ/GROUP/MASK/OTHER）",
          all(k in src for k in ("XA_TAG_USER_OBJ", "XA_TAG_USER", "XA_TAG_GROUP_OBJ",
                                 "XA_TAG_GROUP", "XA_TAG_MASK", "XA_TAG_OTHER")), "")
    check("有 ACL 就完全按 ACL 判（perm_check_ino64 里 acl_perm64 先于 mode）",
          "acl_perm64(ino, n3)" in src and "via=acl" in src, "")
    check("xattr 块号落在 v5 inode 的 81（VFS_I5_XATTR）+ 特性位 ACL",
          "VFS_I5_XATTR" in src and "VFS64_FEAT_ACL" in hdr, "")
    check("setxattr/getxattr 接口在头文件（含 system.posix_acl_access 常量）",
          all(k in hdr for k in ("vfs64_setxattr64", "vfs64_getxattr64",
                                 "VFS64_XATTR_ACL_ACCESS")), "")
    print("=== B) 真 QEMU ===")
    qemu = p64.find_qemu(args.qemu)
    if not qemu:
        sys.stderr.write("找不到 qemu-system-x86_64\\n")
        return 2
    base = p64.prepare_fixture()
    if not base or not os.path.exists(base):
        sys.stderr.write("缺少构建产物或造夹具失败\\n")
        return 2
    tmp = tempfile.mkdtemp(prefix="vimtu64_acl_")
    disk = os.path.join(tmp, "acl.img")
    shutil.copyfile(base, disk)                  # ★ 副本：绝不直接引导共享夹具
    serial = os.path.join(tmp, "serial.log")
    sock = socket.socket()
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.close()
    proc = subprocess.Popen(
        [qemu, "-name", "Vimtu64-acl", "-drive", "format=raw,file=%s" % q(disk),
         "-boot", "order=c", "-m", "512", "-vga", "std", "-display", "none",
         "-serial", "file:%s" % q(serial),
         "-monitor", "telnet:127.0.0.1:%d,server,nowait" % port, "-no-reboot"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        m = wait_re(serial, r"\[VFS64\] acl ok acl_bytes=(\d+) named_uid=1234 r_ok=1 w_denied=1 "
                             r"other_by_mode=1 cleared_back_to_mode=1 step=(\d+)", 300, proc)
        check("[VFS64] acl ok 行出现（自检 bit20 跑完）", m is not None,
              m.group(0) if m else "（没等到 acl ok 行）")
        if m:
            check("ACL 值 = 5 项 × 8B = 40 B（Linux 二进制 posix_acl 数组）", m.group(1) == "40",
                  "acl_bytes=%s" % m.group(1))
            check("bit20 每一条判据都过（step=0）", m.group(2) == "0", "step=%s" % m.group(2))
        md = wait_re(serial, r"\[PERM64\] deny op=\S+ path=/a1.txt uid=\d+ via=acl need=w", 300, proc)
        check("拒绝走 ACL 分支（[PERM64] deny ... via=acl need=w）", md is not None,
              md.group(0) if md else "（没等到 via=acl 的 deny 行）")
        check("[XATTR] setxattr ok ... name=system.posix_acl_access（xattr 真的落盘）",
              "[XATTR] setxattr ok path=/a1.txt name=system.posix_acl_access" in slog(serial), "")
        check("特性层打点：[VFS64] feature on bit=4 name=acl",
              "[VFS64] feature on bit=4 name=acl" in slog(serial), "")
        # ★ 注意：QEMU 的 `-serial file:` 带缓冲，而且这里的正则别再写双反斜杠（会匹配字面反斜杠）。
        #   做法：这里只记标记，最终判定放到进程退出后（finally 里回读一次串口）。
        # ★ v0.9.30：**先等总自检收尾**（PASS 或 FAIL）再判。实测踩到：批量连跑时进程被过早杀掉，
        #   后面几段（bit21 快照拷贝最慢）还没跑完，串口里就永远没有 "selftest PASS" → 误判红。
        #   判据不变（仍然要求 PASS，且不能有 FAIL mask），只是把"等到"这件事做对。
        wait_re(serial, r"\[VFS64\] selftest (PASS|FAIL)", 180, proc)
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
